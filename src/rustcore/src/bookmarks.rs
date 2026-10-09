//! RCORE02a: the canonical bookmark store — XBEL on disk, a handle-
//! addressed tree in memory, and CRUD + lazy per-node queries over
//! the C ABI.
//!
//! The Qt side keeps BookmarkNode proxies as its view; this store is
//! the single source of truth.  Models never marshal the tree across
//! the FFI — they ask for children of node X by handle, one node per
//! call, which is exactly Qt's lazy model contract.
//!
//! On-disk format stays XBEL 1.0 so files flow both ways with every
//! previous Arora build and foreign tools.  The reader is a quick-xml
//! transliteration of XbelReader's grammar (folder folded=, bookmark
//! href=, title/desc elements, unknown-element skipping, 256-deep and
//! 64 MB bounds, &nbsp; pre-expansion).  The writer emits the same
//! shape XbelWriter produced, atomically via write-temp + rename.
//!
//! Handles: root is 1; 0 is the null handle.  detach() unlinks a
//! subtree but keeps it alive for undo; destroy() frees a detached
//! subtree for good.  Bookmarks/folders may carry a comma-joined
//! `tags` attribute — foreign XBEL readers ignore it.

use std::collections::{HashMap, HashSet};
use std::fmt::Write as _;
use std::fs;
use std::io::Write;
use std::path::Path;
use std::sync::Mutex;

use serde_json::{json, Value};

use crate::error::{Fail, RcResult, RcStatus};
use crate::parsers::{
    attr, attr_or_empty, bom_transcode, local, read_element_text, Ev, Scan,
};

/// Node kinds — the same ordinals as BookmarkNode::Type so the Qt
/// adapter can cast without a translation table.
pub const TYPE_ROOT: u8 = 0;
pub const TYPE_FOLDER: u8 = 1;
pub const TYPE_BOOKMARK: u8 = 2;
pub const TYPE_SEPARATOR: u8 = 3;

/// Mirrors XbelReader's MaximumInputSize / MaximumNestingDepth.
const MAX_XBEL: usize = 64 * 1024 * 1024;
const XBEL_MAX_DEPTH: usize = 256;

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    Err(Fail {
        status: RcStatus::Corrupt,
        msg: msg.into(),
    })
}

#[derive(Clone)]
pub struct Node {
    pub parent: Option<u64>,
    pub children: Vec<u64>,
    pub kind: u8,
    pub title: String,
    pub url: String,
    pub desc: String,
    pub expanded: bool,
    pub tags: Vec<String>,
}

impl Node {
    fn new(kind: u8) -> Node {
        Node {
            parent: None,
            children: Vec::new(),
            kind,
            title: String::new(),
            url: String::new(),
            desc: String::new(),
            expanded: false,
            tags: Vec::new(),
        }
    }

    fn to_json(&self) -> Value {
        json!({
            "type": self.kind,
            "title": self.title,
            "url": self.url,
            "desc": self.desc,
            "expanded": self.expanded,
            "tags": self.tags,
        })
    }
}

pub struct Bookmarks {
    nodes: HashMap<u64, Node>,
    next: u64,
    /// Subtrees unlinked by detach() — they stay addressable until
    /// attach() or destroy() so undo can resurrect them intact.
    detached: HashSet<u64>,
    root: u64,
}

static BM: Mutex<Option<Bookmarks>> = Mutex::new(None);

/// Runs `f` against the shared store (initialized to a lone empty
/// root on first use), surviving a poisoned mutex.
pub fn with<R>(f: impl FnOnce(&mut Bookmarks) -> R) -> R {
    let mut guard = BM.lock().unwrap_or_else(|e| e.into_inner());
    f(guard.get_or_insert_with(Bookmarks::new))
}

impl Bookmarks {
    fn new() -> Bookmarks {
        let mut nodes = HashMap::new();
        nodes.insert(1, Node::new(TYPE_ROOT));
        Bookmarks {
            nodes,
            next: 2,
            detached: HashSet::new(),
            root: 1,
        }
    }

    fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
        Err(Fail {
            status: RcStatus::InvalidArgument,
            msg: msg.into(),
        })
    }

    fn node(&self, h: u64) -> RcResult<&Node> {
        self.nodes
            .get(&h)
            .ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "bad node handle".into(),
            })
    }

    fn node_mut(&mut self, h: u64) -> RcResult<&mut Node> {
        self.nodes
            .get_mut(&h)
            .ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "bad node handle".into(),
            })
    }

    pub fn root(&self) -> u64 {
        self.root
    }

    fn alloc(&mut self, node: Node) -> u64 {
        let h = self.next;
        self.next += 1;
        self.nodes.insert(h, node);
        h
    }

    // ---- queries ---------------------------------------------------

    pub fn child_count(&self, h: u64) -> Option<usize> {
        self.nodes.get(&h).map(|n| n.children.len())
    }

    pub fn child_at(&self, h: u64, row: i64) -> Option<u64> {
        let n = self.nodes.get(&h)?;
        if row < 0 || row as usize >= n.children.len() {
            return None;
        }
        Some(n.children[row as usize])
    }

    pub fn parent_of(&self, h: u64) -> Option<u64> {
        self.nodes.get(&h).and_then(|n| n.parent)
    }

    pub fn get_json(&self, h: u64) -> Option<String> {
        self.nodes
            .get(&h)
            .map(|n| serde_json::to_string(&n.to_json()).unwrap_or_default())
    }

    /// Depth-first first bookmark node whose url matches — the
    /// "is this page bookmarked" dedup query.
    pub fn find_url(&self, url: &str) -> Option<u64> {
        let mut stack = vec![self.root];
        while let Some(h) = stack.pop() {
            let n = self.nodes.get(&h)?;
            if n.kind == TYPE_BOOKMARK && n.url == url {
                return Some(h);
            }
            for c in n.children.iter().rev() {
                stack.push(*c);
            }
        }
        None
    }

    // ---- mutations ---------------------------------------------------

    /// Creates a node from its JSON description and links it under
    /// `parent` at `row` (row < 0 or past the end appends).
    pub fn create(&mut self, parent: u64, row: i64, json: &Value) -> RcResult<u64> {
        let kind = json["type"].as_u64().unwrap_or(TYPE_BOOKMARK as u64) as u8;
        if kind == TYPE_ROOT || kind > TYPE_SEPARATOR {
            return Self::invalid("bad node type");
        }
        let mut node = Node::new(kind);
        if let Some(t) = json["title"].as_str() {
            node.title = t.into();
        }
        if let Some(u) = json["url"].as_str() {
            node.url = u.into();
        }
        if let Some(d) = json["desc"].as_str() {
            node.desc = d.into();
        }
        node.expanded = json["expanded"].as_bool().unwrap_or(false);
        if let Some(tags) = json["tags"].as_array() {
            node.tags = tags
                .iter()
                .filter_map(|t| t.as_str().map(str::to_owned))
                .collect();
        }
        let h = self.alloc(node);
        self.attach(parent, row, h)?;
        Ok(h)
    }

    /// Links an existing (detached or freshly allocated) node under
    /// `parent` at `row`.  The node must not already be attached —
    /// callers that move nodes detach() first, matching the
    /// BookmarkNode::add reparent semantics on the Qt side.
    pub fn attach(&mut self, parent: u64, row: i64, h: u64) -> RcResult<()> {
        if h == self.root {
            return Self::invalid("cannot attach the root");
        }
        let pkind = self.node(parent)?.kind;
        if pkind == TYPE_BOOKMARK || pkind == TYPE_SEPARATOR {
            return Self::invalid("cannot parent under a leaf");
        }
        {
            let node = self.node(h)?;
            if node.parent.is_some() {
                return Self::invalid("node is already attached");
            }
            // Refuse cycles: parent must not live inside h's subtree.
            let mut walk = Some(parent);
            while let Some(w) = walk {
                if w == h {
                    return Self::invalid("attach would create a cycle");
                }
                walk = self.nodes.get(&w).and_then(|n| n.parent);
            }
        }
        let p = self.node_mut(parent)?;
        let at = if row < 0 || row as usize > p.children.len() {
            p.children.len()
        } else {
            row as usize
        };
        p.children.insert(at, h);
        self.detached.remove(&h);
        self.node_mut(h)?.parent = Some(parent);
        Ok(())
    }

    /// Unlinks `h` from its parent without freeing the subtree —
    /// undo re-links it via attach().
    pub fn detach(&mut self, h: u64) -> RcResult<()> {
        if h == self.root {
            return Self::invalid("cannot detach the root");
        }
        let parent = self.node(h)?.parent;
        if let Some(p) = parent {
            let pl = self.node_mut(p)?;
            pl.children.retain(|c| *c != h);
        }
        self.node_mut(h)?.parent = None;
        self.detached.insert(h);
        Ok(())
    }

    /// Frees a detached subtree (the undo command's terminal delete).
    pub fn destroy(&mut self, h: u64) -> RcResult<()> {
        if h == self.root {
            return Self::invalid("cannot destroy the root");
        }
        if self.node(h)?.parent.is_some() {
            return Self::invalid("detach before destroy");
        }
        let mut stack = vec![h];
        while let Some(x) = stack.pop() {
            let children = match self.nodes.remove(&x) {
                Some(n) => n.children,
                None => continue,
            };
            stack.extend(children);
            self.detached.remove(&x);
        }
        Ok(())
    }

    pub fn set_str(&mut self, h: u64, field: &str, value: &str) -> RcResult<()> {
        let n = self.node_mut(h)?;
        match field {
            "title" => n.title = value.into(),
            "url" => n.url = value.into(),
            "desc" => n.desc = value.into(),
            _ => return Self::invalid("unknown string field"),
        }
        Ok(())
    }

    pub fn set_expanded(&mut self, h: u64, expanded: bool) -> RcResult<()> {
        self.node_mut(h)?.expanded = expanded;
        Ok(())
    }

    pub fn set_tags(&mut self, h: u64, tags: Vec<String>) -> RcResult<()> {
        self.node_mut(h)?.tags = tags;
        Ok(())
    }

    // ---- XBEL --------------------------------------------------------

    /// Replaces the tree with the parsed document.  Mirrors
    /// XbelReader's grammar one-for-one; on failure the old tree
    /// stays untouched.
    pub fn load_bytes(&mut self, data: &[u8]) -> RcResult<()> {
        if data.len() > MAX_XBEL {
            return corrupt("the XBEL document is too large");
        }
        // The old reader pre-expanded the &nbsp; entity before Qt's
        // resolver-less stream saw it — same byte-level fix here.
        let expanded_data;
        let data = if data.windows(6).any(|w| w == b"&nbsp;") {
            let text = String::from_utf8_lossy(data).replace("&nbsp;", " ");
            expanded_data = text.into_bytes();
            &expanded_data
        } else {
            data
        };
        let data = bom_transcode(data)?;
        let mut scan = Scan::new(&data);

        let mut fresh = Bookmarks::new();
        let mut rooted = false;
        loop {
            match scan.next()? {
                Ev::Eof => break,
                Ev::Start { name, attrs } => {
                    let version = attr_or_empty(&attrs, "version");
                    if local(&name) != "xbel" || !(version.is_empty() || version == "1.0")
                    {
                        return corrupt("the file is not an XBEL version 1.0 file");
                    }
                    rooted = true;
                    let root = fresh.root;
                    read_container(&mut scan, &mut fresh, root, 0, false)?;
                }
                _ => {}
            }
        }
        if !rooted {
            return corrupt("the file is not an XBEL version 1.0 file");
        }
        *self = fresh;
        Ok(())
    }

    /// Loads from a file; a missing file is an empty store, not an
    /// error (first run), matching XbelReader::read(fileName).
    pub fn load_path(&mut self, path: &Path) -> RcResult<()> {
        match fs::read(path) {
            Ok(data) => self.load_bytes(&data),
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                *self = Bookmarks::new();
                Ok(())
            }
            Err(e) => Err(Fail {
                status: RcStatus::Io,
                msg: format!("cannot read {}: {e}", path.display()),
            }),
        }
    }

    /// Serializes the root's children and atomically installs the
    /// result: temp file + fsync + rename, so a crash mid-save never
    /// truncates the user's bookmarks.
    pub fn save_path(&self, path: &Path) -> RcResult<()> {
        let xml = self.to_xbel();
        let tmp = path.with_extension("xbel.rc-tmp");
        {
            let mut f = fs::File::create(&tmp).map_err(|e| Fail {
                status: RcStatus::Io,
                msg: format!("cannot create {}: {e}", tmp.display()),
            })?;
            f.write_all(xml.as_bytes()).and_then(|_| f.sync_all())
                .map_err(|e| Fail {
                    status: RcStatus::Io,
                    msg: format!("cannot write {}: {e}", tmp.display()),
                })?;
        }
        fs::rename(&tmp, path).map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot install {}: {e}", path.display()),
        })
    }

    fn to_xbel(&self) -> String {
        let mut out = String::with_capacity(4096);
        out.push_str("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE xbel>\n<xbel version=\"1.0\">\n");
        let children = self
            .nodes
            .get(&self.root)
            .map(|n| n.children.clone())
            .unwrap_or_default();
        for h in children {
            self.write_item(&mut out, h, 1);
        }
        out.push_str("</xbel>\n");
        out
    }

    fn write_item(&self, out: &mut String, h: u64, depth: usize) {
        let Some(n) = self.nodes.get(&h) else {
            return;
        };
        let pad = "    ".repeat(depth);
        match n.kind {
            TYPE_FOLDER => {
                let mut open = format!("{pad}<folder folded=\"{}\"", if n.expanded { "no" } else { "yes" });
                if !n.tags.is_empty() {
                    let _ = write!(open, " tags=\"{}\"", escape_attr(&n.tags.join(",")));
                }
                out.push_str(&open);
                out.push_str(">\n");
                write_text_element(out, &pad, "title", &n.title);
                for c in &n.children {
                    self.write_item(out, *c, depth + 1);
                }
                out.push_str(&pad);
                out.push_str("</folder>\n");
            }
            TYPE_BOOKMARK => {
                out.push_str(&pad);
                out.push_str("<bookmark");
                if !n.url.is_empty() {
                    let _ = write!(out, " href=\"{}\"", escape_attr(&n.url));
                }
                if !n.desc.is_empty() {
                    // XbelWriter emits desc as an attribute (nonstandard
                    // but the on-disk convention this file format has).
                    let _ = write!(out, " desc=\"{}\"", escape_attr(&n.desc));
                }
                if !n.tags.is_empty() {
                    let _ = write!(out, " tags=\"{}\"", escape_attr(&n.tags.join(",")));
                }
                out.push_str(">\n");
                write_text_element(out, &pad, "title", &n.title);
                out.push_str(&pad);
                out.push_str("</bookmark>\n");
            }
            TYPE_SEPARATOR => {
                out.push_str(&pad);
                out.push_str("<separator/>\n");
            }
            _ => {}
        }
    }
}

fn write_text_element(out: &mut String, pad: &str, name: &str, text: &str) {
    out.push_str(pad);
    out.push_str("    <");
    out.push_str(name);
    out.push('>');
    out.push_str(&escape_text(text));
    out.push_str("</");
    out.push_str(name);
    out.push_str(">\n");
}

fn escape_text(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

fn escape_attr(s: &str) -> String {
    escape_text(s).replace('"', "&quot;")
}

/// Reads one level of <folder>/<bookmark>/<separator> children into
/// `parent`, returning on the container's End — the transliteration
/// of XbelReader::readXBEL / readFolder (they share the same child
/// grammar; `in_folder` is what lets <title>/<desc> bind to `parent`,
/// which only readFolder does).
/// `depth` counts folder nesting only, same as the Qt reader's m_depth.
fn read_container(
    scan: &mut Scan,
    store: &mut Bookmarks,
    parent: u64,
    depth: usize,
    in_folder: bool,
) -> RcResult<()> {
    loop {
        match scan.next()? {
            Ev::End { .. } | Ev::Eof => return Ok(()),
            Ev::Start { name, attrs } => match local(&name) {
                "folder" => read_folder(scan, store, parent, &attrs, depth)?,
                "bookmark" => read_bookmark(scan, store, parent, &attrs)?,
                "separator" => {
                    let h = store.alloc(Node::new(TYPE_SEPARATOR));
                    store.attach(parent, -1, h)?;
                    scan.next()?; // empty elements arrive as Start+End
                }
                "title" | "desc" if in_folder => {
                    let text = read_element_text(scan)?;
                    let n = store.node_mut(parent)?;
                    if local(&name) == "title" {
                        n.title = text;
                    } else {
                        n.desc = text;
                    }
                }
                _ => skip_unknown(scan)?,
            },
            _ => {}
        }
    }
}

fn read_folder(
    scan: &mut Scan,
    store: &mut Bookmarks,
    parent: u64,
    attrs: &[(String, String)],
    depth: usize,
) -> RcResult<()> {
    if depth >= XBEL_MAX_DEPTH {
        return corrupt("the XBEL document is nested too deeply");
    }
    let mut node = Node::new(TYPE_FOLDER);
    node.expanded = attr_or_empty(attrs, "folded") == "no";
    if let Some(tags) = attr(attrs, "tags") {
        node.tags = tags
            .split(',')
            .map(str::trim)
            .filter(|t| !t.is_empty())
            .map(str::to_owned)
            .collect();
    }
    // Tolerate the attribute form this crate emits; the standard
    // <desc> element below overrides it when both exist.
    if let Some(d) = attr(attrs, "desc") {
        node.desc = d.into();
    }
    let h = store.alloc(node);
    store.attach(parent, -1, h)?;
    read_container(scan, store, h, depth + 1, true)
}

fn read_bookmark(
    scan: &mut Scan,
    store: &mut Bookmarks,
    parent: u64,
    attrs: &[(String, String)],
) -> RcResult<()> {
    let mut node = Node::new(TYPE_BOOKMARK);
    if let Some(u) = attr(attrs, "href") {
        node.url = u.into();
    }
    if let Some(d) = attr(attrs, "desc") {
        node.desc = d.into();
    }
    if let Some(tags) = attr(attrs, "tags") {
        node.tags = tags.split(',').map(str::trim).filter(|t| !t.is_empty()).map(str::to_owned).collect();
    }
    loop {
        match scan.next()? {
            Ev::End { .. } | Ev::Eof => break,
            Ev::Start { name, .. } => match local(&name) {
                "title" => node.title = read_element_text(scan)?,
                "desc" => node.desc = read_element_text(scan)?,
                _ => skip_unknown(scan)?,
            },
            _ => {}
        }
    }
    if node.title.is_empty() {
        node.title = "Unknown title".into();
    }
    let h = store.alloc(node);
    store.attach(parent, -1, h)?;
    Ok(())
}

fn skip_unknown(scan: &mut Scan) -> RcResult<()> {
    let mut depth = 1usize;
    while depth > 0 {
        match scan.next()? {
            Ev::Start { .. } => depth += 1,
            Ev::End { .. } | Ev::Eof => depth -= 1,
            _ => {}
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn store() -> Bookmarks {
        Bookmarks::new()
    }

    fn parse(doc: &str) -> Bookmarks {
        let mut s = store();
        s.load_bytes(doc.as_bytes()).unwrap();
        s
    }

    #[test]
    fn parse_basic_tree() {
        let s = parse(
            "<?xml version='1.0'?><!DOCTYPE xbel>\
             <xbel version='1.0'>\
               <folder folded='no'><title>Bar</title>\
                 <bookmark href='http://a/'><title>A</title></bookmark>\
                 <separator/>\
                 <folder><title>Sub</title>\
                   <bookmark href='http://b/'><title>B</title><desc>d</desc></bookmark>\
                 </folder>\
               </folder>\
             </xbel>",
        );
        let root = s.root();
        assert_eq!(s.child_count(root), Some(1));
        let bar = s.child_at(root, 0).unwrap();
        let n = &s.nodes[&bar];
        assert_eq!(n.kind, TYPE_FOLDER);
        assert_eq!(n.title, "Bar");
        assert!(n.expanded);
        assert_eq!(s.child_count(bar), Some(3));
        let a = s.child_at(bar, 0).unwrap();
        assert_eq!(s.nodes[&a].url, "http://a/");
        let sep = s.child_at(bar, 1).unwrap();
        assert_eq!(s.nodes[&sep].kind, TYPE_SEPARATOR);
        let sub = s.child_at(bar, 2).unwrap();
        let b = s.child_at(sub, 0).unwrap();
        assert_eq!(s.nodes[&b].desc, "d");
        assert_eq!(s.parent_of(b), Some(sub));
    }

    #[test]
    fn parse_rejects_garbage() {
        for doc in [
            "",
            "<html/>",
            "<xbel version='2.0'/>",
            "<xbel><folder></xbel>",
        ] {
            let mut s = store();
            assert!(s.load_bytes(doc.as_bytes()).is_err(), "{doc}");
        }
        // Deep nesting bound.
        let mut d = "<xbel>".to_string();
        for _ in 0..300 {
            d.push_str("<folder>");
        }
        let mut s = store();
        assert!(s.load_bytes(d.as_bytes()).is_err());
    }

    #[test]
    fn nbsp_and_default_title() {
        let s = parse(
            "<xbel><bookmark href='h'><title>a&nbsp;b</title></bookmark>\
             <bookmark href='h2'/></xbel>",
        );
        let a = s.child_at(s.root(), 0).unwrap();
        assert_eq!(s.nodes[&a].title, "a b");
        let b = s.child_at(s.root(), 1).unwrap();
        assert_eq!(s.nodes[&b].title, "Unknown title");
    }

    #[test]
    fn crud_round_trip() {
        let mut s = store();
        let root = s.root();
        let f = s
            .create(root, -1, &json!({"type":1,"title":"F"}))
            .unwrap();
        let b = s
            .create(f, -1, &json!({"type":2,"title":"T","url":"http://x/"}))
            .unwrap();
        assert_eq!(s.child_count(f), Some(1));
        assert_eq!(s.find_url("http://x/"), Some(b));
        s.set_str(b, "title", "T2").unwrap();
        assert_eq!(s.nodes[&b].title, "T2");
        s.set_expanded(f, true).unwrap();
        s.detach(b).unwrap();
        assert_eq!(s.child_count(f), Some(0));
        s.attach(f, 0, b).unwrap();
        assert_eq!(s.child_at(f, 0), Some(b));
        s.detach(b).unwrap();
        s.destroy(b).unwrap();
        assert!(s.child_at(f, 0).is_none());
        assert!(s.nodes.get(&b).is_none());
    }

    #[test]
    fn attach_guards() {
        let mut s = store();
        let root = s.root();
        let f = s.create(root, -1, &json!({"type":1})).unwrap();
        let g = s.create(f, -1, &json!({"type":1})).unwrap();
        // Cycle refused: f cannot live inside its own child.
        s.detach(g).unwrap();
        assert!(s.attach(g, -1, f).is_err());
        // Root cannot be moved or destroyed.
        assert!(s.detach(root).is_err());
        assert!(s.destroy(root).is_err());
    }

    #[test]
    fn xbel_write_parse_roundtrip() {
        let mut s = store();
        let root = s.root();
        let f = s
            .create(root, -1, &json!({"type":1,"title":"A & B","expanded":true,"tags":["t1","t2"]}))
            .unwrap();
        s.create(f, -1, &json!({"type":2,"title":"<x>","url":"http://q/?a=1&b=2","desc":"d"})).unwrap();
        s.create(f, -1, &json!({"type":3})).unwrap();
        let xml = s.to_xbel();
        assert!(xml.contains("href=\"http://q/?a=1&amp;b=2\""));
        assert!(xml.contains("folded=\"no\""));

        let mut s2 = store();
        s2.load_bytes(xml.as_bytes()).unwrap();
        let f2 = s2.child_at(s2.root(), 0).unwrap();
        assert_eq!(s2.nodes[&f2].title, "A & B");
        assert_eq!(s2.nodes[&f2].tags, vec!["t1".to_string(), "t2".to_string()]);
        let b2 = s2.child_at(f2, 0).unwrap();
        assert_eq!(s2.nodes[&b2].title, "<x>");
        assert_eq!(s2.nodes[&b2].url, "http://q/?a=1&b=2");
        assert_eq!(s2.nodes[&b2].desc, "d");
        assert_eq!(s2.nodes[&s2.child_at(f2, 1).unwrap()].kind, TYPE_SEPARATOR);
    }

    #[test]
    fn file_round_trip() {
        let dir = std::env::temp_dir().join(format!(
            "arora-bm-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("bookmarks.xbel");
        // Missing file loads as an empty store.
        let mut s = store();
        s.load_path(&path).unwrap();
        assert_eq!(s.child_count(s.root()), Some(0));
        let f = s.create(s.root(), -1, &json!({"type":1,"title":"F"})).unwrap();
        s.create(f, -1, &json!({"type":2,"title":"b","url":"u"})).unwrap();
        s.save_path(&path).unwrap();
        let mut s2 = store();
        s2.load_path(&path).unwrap();
        let f2 = s2.child_at(s2.root(), 0).unwrap();
        assert_eq!(s2.nodes[&f2].title, "F");
        assert_eq!(s2.child_count(f2), Some(1));
        std::fs::remove_dir_all(&dir).ok();
    }
}
