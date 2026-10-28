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
//! previous Arktos build and foreign tools.  The reader is a quick-xml
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
use std::path::{Path, PathBuf};
use std::sync::Mutex;

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

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
    /// SYNC04: the CRDT element id — persisted as the XBEL `id`
    /// attribute, minted randomly for locally created nodes and by a
    /// deterministic content fingerprint for nodes a foreign file
    /// brings in (so two devices' identical pre-sync trees dedup
    /// instead of doubling).  "" while a freshly loaded node still
    /// waits for the assign_meta pass.
    pub guid: String,
    /// SYNC04: the fractional-index position inside the parent —
    /// persisted as `pos`, drives the convergent sibling order.
    pub rank: String,
}

/// A mutation worth a sync op, recorded by the public mutators and
/// drained by the bookmarks adapter (syncbookmarks) after the call
/// commits.  Remote applies deliberately route around the recording
/// mutators so a peer's op never echoes back as a local one.
#[derive(Clone, Debug)]
pub(crate) enum BmEvent {
    /// Node (re)linked under a parent — create, move, undo-restore,
    /// import insert.  Emitted as a `link` op per subtree node.
    Link(u64),
    /// A detached subtree was destroyed for good — (guid, parent_guid)
    /// pairs captured before the nodes were freed, head first.
    Remove(Vec<(String, String)>),
    /// One field changed on a live node.
    Set(u64, &'static str),
}

/// An outbox entry: the event plus the file binding it was recorded
/// under — the sync adapter maps that path back to the sync scope
/// (profile) it belongs to and drops events for non-profile files.
#[derive(Clone, Debug)]
pub(crate) struct OutEv {
    pub path: Option<PathBuf>,
    pub ev: BmEvent,
}

impl Node {
    pub(crate) fn new(kind: u8) -> Node {
        Node {
            parent: None,
            children: Vec::new(),
            kind,
            title: String::new(),
            url: String::new(),
            desc: String::new(),
            expanded: false,
            tags: Vec::new(),
            guid: String::new(),
            rank: String::new(),
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
    /// The file the tree was last loaded from — None for a fresh or
    /// memory-loaded tree.  The facade's bound handle (RC07) reads it
    /// with `epoch` to re-assert its file binding without reparsing
    /// on every call.
    loaded_path: Option<PathBuf>,
    /// Monotonic counter bumped by every successful (re)load — it
    /// distinguishes "same file, newer tree" so a bound handle can
    /// adopt another writer's reload of its own file instead of
    /// reloading again.
    epoch: u64,
    /// Set by every tree mutation, cleared by load/save — lets
    /// ensure_path flush a displaced tree's pending edits to the
    /// file it was bound to (the store-level twin of the Qt layer's
    /// saveIfNeccessary before re-binding) instead of dropping them.
    dirty: bool,
    /// SYNC04 outbox: mutations record events here; `with` hands them
    /// to the sync adapter after the call commits.  Kept in-memory —
    /// an op that never got emitted is at worst a missed local edit,
    /// never a torn remote state.
    pub(crate) outbox: Vec<OutEv>,
    /// True while load_bytes builds the replacement tree — suppresses
    /// guid/rank minting and event recording inside attach (the parse
    /// is not a user mutation; assign_meta runs once after it).
    loading: bool,
}

static BM: Mutex<Option<Bookmarks>> = Mutex::new(None);

/// Runs `f` against the shared store (initialized to a lone empty
/// root on first use), surviving a poisoned mutex.  After the call
/// commits, recorded mutation events flush to the sync adapter —
/// a no-op when nothing changed or the tree isn't a synced profile
/// file, and never echo: remote applies don't record events.
pub fn with<R>(f: impl FnOnce(&mut Bookmarks) -> R) -> R {
    let mut guard = BM.lock().unwrap_or_else(|e| e.into_inner());
    let b = guard.get_or_insert_with(Bookmarks::new);
    let r = f(b);
    crate::syncbookmarks::flush_local(b);
    r
}

impl Bookmarks {
    pub(crate) fn new() -> Bookmarks {
        let mut nodes = HashMap::new();
        nodes.insert(1, Node::new(TYPE_ROOT));
        Bookmarks {
            nodes,
            next: 2,
            detached: HashSet::new(),
            root: 1,
            loaded_path: None,
            epoch: 0,
            dirty: false,
            outbox: Vec::new(),
            loading: false,
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

    pub(crate) fn node_mut(&mut self, h: u64) -> RcResult<&mut Node> {
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

    /// Read-only node access for sibling modules (the HTML importer
    /// walks children to merge/dedup, the exporter reads the tree).
    pub(crate) fn node_ref(&self, h: u64) -> Option<&Node> {
        self.nodes.get(&h)
    }

    /// Raw allocation for the bulk importer — pair with attach().
    pub(crate) fn alloc_node(&mut self, node: Node) -> u64 {
        self.alloc(node)
    }

    pub(crate) fn children_of(&self, h: u64) -> Option<Vec<u64>> {
        self.nodes.get(&h).map(|n| n.children.clone())
    }

    /// First direct folder child of `parent` titled `title` — the
    /// HTML importer's merge rule (re-imports fold, not duplicate).
    pub(crate) fn child_folder_named(&self, parent: u64, title: &str) -> Option<u64> {
        let n = self.nodes.get(&parent)?;
        n.children.iter().copied().find(|&c| {
            self.nodes
                .get(&c)
                .map(|m| m.kind == TYPE_FOLDER && m.title == title)
                .unwrap_or(false)
        })
    }

    /// Whether a direct bookmark child of `parent` already carries
    /// `url` — the HTML importer's dedup rule.
    pub(crate) fn child_has_bookmark_url(&self, parent: u64, url: &str) -> bool {
        self.nodes
            .get(&parent)
            .map(|n| {
                n.children.iter().any(|&c| {
                    self.nodes
                        .get(&c)
                        .map(|m| m.kind == TYPE_BOOKMARK && m.url == url)
                        .unwrap_or(false)
                })
            })
            .unwrap_or(false)
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
        if kind == TYPE_BOOKMARK {
            // BKIMP01: the url-scheme gate runs at EVERY insert — a
            // hostile XBEL import, a dropped link or a pasted url hits
            // the same wall as the HTML importer.  "allow_javascript"
            // in the spec is the caller's import_bookmarklets consent
            // (default refuse), so the store can never accept a url
            // WebView::loadUrl would execute or expose.
            crate::bookmarkhtml::check_bookmark_url(
                &node.url,
                json["allow_javascript"].as_bool().unwrap_or(false),
            )?;
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
        self.dirty = true;
        if !self.loading {
            self.mint_meta(parent, at, h)?;
            self.record(BmEvent::Link(h));
        }
        Ok(())
    }

    /// SYNC04: a runtime attach mints what the CRDT layer needs on the
    /// node — a random guid when the loader didn't supply one (`id`
    /// attr or the assign_meta fingerprint) and a fractional rank for
    /// the slot it was inserted into.  Parse-time attaches skip this;
    /// assign_meta fills them after the document is in.
    fn mint_meta(&mut self, parent: u64, at: usize, h: u64) -> RcResult<()> {
        let (prev, next) = {
            let p = self.node(parent)?;
            let prev = at
                .checked_sub(1)
                .and_then(|i| p.children.get(i))
                .and_then(|&c| self.nodes.get(&c))
                .map(|n| n.rank.clone())
                .unwrap_or_default();
            let next = p
                .children
                .get(at + 1)
                .and_then(|&c| self.nodes.get(&c))
                .map(|n| n.rank.clone())
                .unwrap_or_default();
            (prev, next)
        };
        let n = self.node_mut(h)?;
        if n.guid.is_empty() {
            n.guid = fresh_guid();
        }
        n.rank = rank_between(&prev, &next);
        Ok(())
    }

    fn record(&mut self, ev: BmEvent) {
        self.outbox.push(OutEv {
            path: self.loaded_path.clone(),
            ev,
        });
    }

    /// The events recorded since the last drain — the sync adapter
    /// (syncbookmarks) turns them into signed ops for the bound
    /// profile's scope.
    pub(crate) fn take_outbox(&mut self) -> Vec<OutEv> {
        std::mem::take(&mut self.outbox)
    }

    /// The node carrying `guid`, if it is in the tree.  O(n) — the
    /// sync apply lane calls it per op, which is fine at bookmark
    /// scale.
    pub(crate) fn by_guid(&self, guid: &str) -> Option<u64> {
        if guid.is_empty() {
            return None;
        }
        self.nodes
            .iter()
            .find(|(_, n)| n.guid == guid)
            .map(|(&h, _)| h)
    }

    pub(crate) fn guid_of(&self, h: u64) -> Option<&str> {
        self.nodes.get(&h).map(|n| n.guid.as_str())
    }

    /// The guid of `h`'s parent — "" when it hangs directly under
    /// root or has no parent.
    pub(crate) fn parent_guid(&self, h: u64) -> String {
        self.nodes
            .get(&h)
            .and_then(|n| n.parent)
            .and_then(|p| self.nodes.get(&p))
            .map(|pn| {
                if pn.kind == TYPE_ROOT {
                    String::new()
                } else {
                    pn.guid.clone()
                }
            })
            .unwrap_or_default()
    }

    /// SYNC04 apply lane: link `h` under `parent` ordered by its
    /// (rank, guid) — the order every replica converges to.  Detaches
    /// a currently attached `h` first.  No guid/rank mint, no event:
    /// the op carries both and a remote op must never echo.  Cycle
    /// resolution is the caller's job.
    pub(crate) fn place(&mut self, parent: u64, h: u64) -> RcResult<()> {
        if h == self.root {
            return Self::invalid("cannot place the root");
        }
        let pkind = self.node(parent)?.kind;
        if pkind == TYPE_BOOKMARK || pkind == TYPE_SEPARATOR {
            return Self::invalid("cannot parent under a leaf");
        }
        if self.node(h)?.parent.is_some() {
            self.detach(h)?;
        }
        let (rank, guid) = {
            let n = self.node(h)?;
            (n.rank.clone(), n.guid.clone())
        };
        let at = {
            let p = self.node(parent)?;
            p.children
                .iter()
                .position(|&c| {
                    self.nodes
                        .get(&c)
                        .map(|cn| {
                            (cn.rank.as_str(), cn.guid.as_str())
                                > (rank.as_str(), guid.as_str())
                        })
                        .unwrap_or(true)
                })
                .unwrap_or_else(|| self.node(parent).map(|p| p.children.len()).unwrap_or(0))
        };
        let p = self.node_mut(parent)?;
        p.children.insert(at, h);
        self.detached.remove(&h);
        self.node_mut(h)?.parent = Some(parent);
        self.dirty = true;
        Ok(())
    }

    /// SYNC04 apply lane: unlink + free a subtree silently — a remote
    /// remove consumes the element; it must not leave an event that
    /// would re-emit the delete as a local one.
    pub(crate) fn erase(&mut self, h: u64) -> RcResult<()> {
        if self.node(h)?.parent.is_some() {
            self.detach(h)?;
        }
        if self.node(h)?.parent.is_some() || h == self.root {
            return Self::invalid("cannot erase");
        }
        self.free_subtree(h)
    }

    /// SYNC04 apply lane: write one synced field without recording.
    /// `url` passes the same scheme gate as a local edit.
    pub(crate) fn set_field(&mut self, h: u64, field: &str, v: &Value) -> RcResult<()> {
        match field {
            "title" | "url" | "desc" => {
                let s = v.as_str().unwrap_or("");
                if field == "url" {
                    crate::bookmarkhtml::check_bookmark_url(s, true)?;
                }
                let n = self.node_mut(h)?;
                match field {
                    "title" => n.title = s.into(),
                    "url" => n.url = s.into(),
                    _ => n.desc = s.into(),
                }
            }
            "expanded" => self.node_mut(h)?.expanded = v.as_bool().unwrap_or(false),
            "tags" => {
                self.node_mut(h)?.tags = v
                    .as_array()
                    .map(|a| {
                        a.iter()
                            .filter_map(|t| t.as_str().map(str::to_owned))
                            .collect()
                    })
                    .unwrap_or_default()
            }
            _ => return Self::invalid("unknown synced field"),
        }
        self.dirty = true;
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
        self.dirty = true;
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
        // SYNC04: capture (guid, parent_guid) per node before the free
        // — the emitted `rm` op lists them so the peer tombstones
        // exactly what this device observed.  The head is detached so
        // its parent_guid is "" here; the adapter substitutes the
        // last declared placement from its own state.
        let gone = if self.loading {
            Vec::new()
        } else {
            let mut gone = Vec::new();
            let mut stack = vec![h];
            while let Some(x) = stack.pop() {
                let (guid, pgu) = match self.nodes.get(&x) {
                    Some(n) => (
                        n.guid.clone(),
                        n.parent
                            .and_then(|p| self.nodes.get(&p))
                            .map(|pn| pn.guid.clone())
                            .unwrap_or_default(),
                    ),
                    None => continue,
                };
                gone.push((guid, pgu));
                stack.extend(self.nodes.get(&x).map(|n| n.children.clone()).unwrap_or_default());
            }
            gone
        };
        self.free_subtree(h)?;
        if !gone.is_empty() {
            self.record(BmEvent::Remove(gone));
        }
        Ok(())
    }

    /// The shared subtree free behind destroy() and the sync lane's
    /// erase() — no guards, no events.
    fn free_subtree(&mut self, h: u64) -> RcResult<()> {
        let mut stack = vec![h];
        while let Some(x) = stack.pop() {
            let children = match self.nodes.remove(&x) {
                Some(n) => n.children,
                None => continue,
            };
            stack.extend(children);
            self.detached.remove(&x);
        }
        self.dirty = true;
        Ok(())
    }

    pub fn set_str(&mut self, h: u64, field: &str, value: &str) -> RcResult<()> {
        {
            let n = self.node_mut(h)?;
            match field {
                "title" => n.title = value.into(),
                "url" => {
                    // BKIMP01: same insert gate — a single deliberate url
                    // edit is the human equivalent of the import flag, so
                    // javascript: passes here; every other non-allowlist
                    // scheme (data:, file:, arktos-*) still refuses.
                    crate::bookmarkhtml::check_bookmark_url(value, true)?;
                    n.url = value.into();
                }
                "desc" => n.desc = value.into(),
                _ => return Self::invalid("unknown string field"),
            }
        }
        self.dirty = true;
        if !self.loading {
            match field {
                "title" | "url" | "desc" => {
                    let f: &'static str = match field {
                        "title" => "title",
                        "url" => "url",
                        _ => "desc",
                    };
                    self.record(BmEvent::Set(h, f));
                }
                _ => {}
            }
        }
        Ok(())
    }

    pub fn set_expanded(&mut self, h: u64, expanded: bool) -> RcResult<()> {
        self.node_mut(h)?.expanded = expanded;
        self.dirty = true;
        if !self.loading {
            self.record(BmEvent::Set(h, "expanded"));
        }
        Ok(())
    }

    pub fn set_tags(&mut self, h: u64, tags: Vec<String>) -> RcResult<()> {
        self.node_mut(h)?.tags = tags;
        self.dirty = true;
        if !self.loading {
            self.record(BmEvent::Set(h, "tags"));
        }
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
        fresh.loading = true;
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
        fresh.loading = false;
        fresh.assign_meta();
        let epoch = self.epoch;
        *self = fresh;
        self.epoch = epoch + 1;
        Ok(())
    }

    /// SYNC04 post-load pass: every node without an `id` gets a
    /// deterministic fingerprint guid — same document, same ids on
    /// every device, so pre-sync trees merge instead of doubling —
    /// and every node without `pos` gets a sequential fractional
    /// rank in file order.
    fn assign_meta(&mut self) {
        let root = self.root;
        self.assign_meta_walk(root, "");
    }

    fn assign_meta_walk(&mut self, h: u64, parent_guid: &str) {
        let children = self
            .nodes
            .get(&h)
            .map(|n| n.children.clone())
            .unwrap_or_default();
        let mut prev = String::new();
        for (i, c) in children.iter().enumerate() {
            let guid = {
                let Some(n) = self.nodes.get_mut(c) else {
                    continue;
                };
                if n.guid.is_empty() {
                    n.guid = fingerprint_guid(parent_guid, i, n);
                }
                if n.rank.is_empty() {
                    n.rank = rank_between(&prev, "");
                }
                prev = n.rank.clone();
                n.guid.clone()
            };
            self.assign_meta_walk(*c, &guid);
        }
    }

    /// Loads from a file; a missing file is an empty store, not an
    /// error (first run), matching XbelReader::read(fileName).
    pub fn load_path(&mut self, path: &Path) -> RcResult<()> {
        match fs::read(path) {
            Ok(data) => {
                self.load_bytes(&data)?;
                self.loaded_path = Some(path.to_path_buf());
                Ok(())
            }
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                let epoch = self.epoch;
                *self = Bookmarks::new();
                self.epoch = epoch + 1;
                self.loaded_path = Some(path.to_path_buf());
                Ok(())
            }
            Err(e) => Err(Fail {
                status: RcStatus::Io,
                msg: format!("cannot read {}: {e}", path.display()),
            }),
        }
    }

    /// The file this tree was loaded from — None for a fresh or
    /// memory-loaded tree (the facade's bound-handle stamp, RC07).
    pub fn loaded_path(&self) -> Option<&Path> {
        self.loaded_path.as_deref()
    }

    /// The reload counter — bumped by every successful
    /// load_path/load_bytes/fresh-reset.
    pub fn epoch(&self) -> u64 {
        self.epoch
    }

    /// Facade bind (RC07): no-op when this tree already belongs to
    /// `path`; otherwise loads it.  Reports whether a load ran so the
    /// caller can mirror rc_bm_load's "bookmarks" emit.  A displaced
    /// tree with pending edits is flushed to the file it was bound
    /// to first — the in-memory tree may move between profiles, its
    /// edits must not.
    pub fn ensure_path(&mut self, path: &Path) -> RcResult<bool> {
        if self.loaded_path.as_deref() == Some(path) {
            return Ok(false);
        }
        if self.dirty {
            if let Some(old) = self.loaded_path.clone() {
                // Flush only while the edits still have a home — a
                // deleted profile directory leaves nowhere to save
                // to, and blocking the re-anchor strands every
                // other profile's bookmarks.
                if old.parent().map(|p| p.exists()).unwrap_or(false) {
                    self.save_path(&old)?;
                }
            }
        }
        self.load_path(path).map(|_| true)
    }

    /// Serializes the root's children and atomically installs the
    /// result: temp file + fsync + rename, so a crash mid-save never
    /// truncates the user's bookmarks, and a concurrent save from a
    /// second process renames only its own complete bytes (CONC01 —
    /// the old fixed `xbel.rc-tmp` name let two writers interleave a
    /// torn temp file).  Cross-writer resolution stays documented
    /// last-writer-wins.
    pub fn save_path(&mut self, path: &Path) -> RcResult<()> {
        let xml = self.to_xbel();
        // Best-effort retire of the pre-CONC01 fixed-name staging file.
        let _ = fs::remove_file(path.with_extension("xbel.rc-tmp"));
        crate::store::atomic_write(path, xml.as_bytes())?;
        // Binding bookkeeping: writing the bound file clears the
        // pending-edits flag; an anonymous (load_mem'd) tree adopts
        // the file it was written to; a third path is export-as and
        // leaves both alone.
        match self.loaded_path.as_deref() {
            Some(p) if p == path => self.dirty = false,
            None => {
                self.loaded_path = Some(path.to_path_buf());
                self.dirty = false;
            }
            _ => {}
        }
        Ok(())
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
                if !n.guid.is_empty() {
                    let _ = write!(open, " id=\"{}\"", escape_attr(&n.guid));
                }
                if !n.rank.is_empty() {
                    let _ = write!(open, " pos=\"{}\"", escape_attr(&n.rank));
                }
                if !n.tags.is_empty() {
                    let _ = write!(open, " tags=\"{}\"", escape_attr(&n.tags.join(",")));
                }
                out.push_str(&open);
                out.push_str(">\n");
                write_text_element(out, &pad, "title", &n.title);
                // XbelWriter's order: title, desc, then children —
                // <desc> is a child element, and folders carry it too.
                if !n.desc.is_empty() {
                    write_text_element(out, &pad, "desc", &n.desc);
                }
                for c in &n.children {
                    self.write_item(out, *c, depth + 1);
                }
                out.push_str(&pad);
                out.push_str("</folder>\n");
            }
            TYPE_BOOKMARK => {
                out.push_str(&pad);
                out.push_str("<bookmark");
                if !n.guid.is_empty() {
                    let _ = write!(out, " id=\"{}\"", escape_attr(&n.guid));
                }
                if !n.rank.is_empty() {
                    let _ = write!(out, " pos=\"{}\"", escape_attr(&n.rank));
                }
                if !n.url.is_empty() {
                    let _ = write!(out, " href=\"{}\"", escape_attr(&n.url));
                }
                if !n.tags.is_empty() {
                    let _ = write!(out, " tags=\"{}\"", escape_attr(&n.tags.join(",")));
                }
                out.push_str(">\n");
                write_text_element(out, &pad, "title", &n.title);
                // <desc> is an ELEMENT in XBEL and the only form the
                // C++ XbelReader binds — the attribute shape an earlier
                // revision of this writer emitted stays readable (the
                // parser tolerates it) but is never produced.
                if !n.desc.is_empty() {
                    write_text_element(out, &pad, "desc", &n.desc);
                }
                out.push_str(&pad);
                out.push_str("</bookmark>\n");
            }
            TYPE_SEPARATOR => {
                out.push_str(&pad);
                out.push_str("<separator");
                if !n.guid.is_empty() {
                    let _ = write!(out, " id=\"{}\"", escape_attr(&n.guid));
                }
                if !n.rank.is_empty() {
                    let _ = write!(out, " pos=\"{}\"", escape_attr(&n.rank));
                }
                out.push_str("/>\n");
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

// ---- SYNC04 identity + ordering -------------------------------------

/// Reads the sync attributes the writer emits (`id` = CRDT guid,
/// `pos` = fractional rank) onto a node being parsed — bounded so a
/// hostile document cannot inject oversized keys.
fn sync_attrs(node: &mut Node, attrs: &[(String, String)]) {
    if let Some(id) = attr(attrs, "id") {
        if id.len() <= 128 {
            node.guid = id.into();
        }
    }
    if let Some(pos) = attr(attrs, "pos") {
        if pos.len() <= 64 {
            node.rank = pos.into();
        }
    }
}

fn hex_str(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for b in data {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

/// A fresh random element id for a locally created node — 16 bytes
/// so collisions are a non-event at bookmark scale.
fn fresh_guid() -> String {
    match crate::noisexx::rand_bytes::<16>() {
        Ok(b) => hex_str(&b),
        Err(_) => {
            // RNG failure is not fatal for an id; time+counter keeps
            // the id unique enough for a single device.
            static CTR: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
            let nanos = std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_nanos() as u64)
                .unwrap_or(0);
            format!(
                "t{:016x}{:08x}",
                nanos,
                CTR.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
            )
        }
    }
}

/// The deterministic element id for a loaded node that carries no
/// `id` — sha256 over (parent guid, slot index, kind, fields), so
/// the same document mints the same ids on every device and two
/// pre-sync trees dedup instead of doubling.
fn fingerprint_guid(parent_guid: &str, index: usize, n: &Node) -> String {
    let mut d = Sha256::new();
    d.update(b"ARBMG1");
    let mut field = |bytes: &[u8]| {
        d.update((bytes.len() as u32).to_le_bytes());
        d.update(bytes);
    };
    field(parent_guid.as_bytes());
    field(index.to_string().as_bytes());
    field(&[n.kind]);
    field(n.title.as_bytes());
    field(n.url.as_bytes());
    field(n.desc.as_bytes());
    field(if n.expanded { b"1" } else { b"0" });
    for t in &n.tags {
        field(t.as_bytes());
    }
    hex_str(&d.finalize()[..16])
}

/// Fractional-index alphabet — ASCII order is the rank order.
const RANK_ABC: &[u8; 62] =
    b"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

fn rank_digit(c: u8) -> u8 {
    RANK_ABC.iter().position(|&x| x == c).unwrap_or(0) as u8
}

/// The smallest-ish rank string strictly between `prev` and `next`
/// ("" = open end) under plain string ordering — the convergent
/// sibling order primitive: peers insert by the op's `pos` and only
/// ever break ties on guid, so every replica orders identically.
pub(crate) fn rank_between(prev: &str, next: &str) -> String {
    let lo: Vec<u8> = prev.bytes().map(rank_digit).collect();
    let hi: Vec<u8> = next.bytes().map(rank_digit).collect();
    let mut open_hi = next.is_empty();
    let mut out = Vec::new();
    for i in 0..256 {
        let l = lo.get(i).copied().unwrap_or(0);
        let h = if open_hi {
            61
        } else {
            hi.get(i).copied().unwrap_or(0)
        };
        if h > l + 1 {
            out.push(RANK_ABC[(l + (h - l) / 2) as usize]);
            return String::from_utf8(out).unwrap_or_default();
        }
        out.push(RANK_ABC[l as usize]);
        if h == l + 1 {
            // Only room below `next` is above prev's remaining digits.
            open_hi = true;
        }
        if h < l {
            // Caller invariant (prev < next) broken — still emit a
            // string; sorting stays defined.
            break;
        }
    }
    out.push(RANK_ABC[30]);
    String::from_utf8(out).unwrap_or_default()
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
                    let mut n = Node::new(TYPE_SEPARATOR);
                    sync_attrs(&mut n, &attrs);
                    let h = store.alloc(n);
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
    sync_attrs(&mut node, attrs);
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
    sync_attrs(&mut node, attrs);
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
        assert!(!s.nodes.contains_key(&b));
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
    fn xbel_writer_emits_desc_elements_like_xbelwriter() {
        // LEG01 shape: XbelWriter emits <desc> as a child element on
        // folders AND bookmarks — the C++ reader binds no other form,
        // so the Rust writer must not emit the desc= attribute it once
        // did, and must not drop folder descriptions.
        let mut s = store();
        let f = s
            .create(
                s.root(),
                -1,
                &json!({"type":1,"title":"F","desc":"folder & <desc>"}),
            )
            .unwrap();
        s.create(
            f,
            -1,
            &json!({"type":2,"title":"B","url":"http://b/","desc":"bm desc"}),
        )
        .unwrap();
        let xml = s.to_xbel();
        assert!(xml.contains("<desc>folder &amp; &lt;desc&gt;</desc>"), "{xml}");
        assert!(xml.contains("<desc>bm desc</desc>"), "{xml}");
        assert!(!xml.contains("desc=\""), "{xml}");

        let mut s2 = store();
        s2.load_bytes(xml.as_bytes()).unwrap();
        let f2 = s2.child_at(s2.root(), 0).unwrap();
        assert_eq!(s2.nodes[&f2].desc, "folder & <desc>");
        let b2 = s2.child_at(f2, 0).unwrap();
        assert_eq!(s2.nodes[&b2].desc, "bm desc");
    }

    #[test]
    fn xbel_reader_tolerates_legacy_desc_attributes() {
        // Files written while this crate emitted desc= attributes must
        // still read — the attribute is a pre-expansion, and a later
        // <desc> element wins when both exist.
        let s = parse(
            "<xbel version='1.0'>\
               <folder desc='attr'><title>F</title><desc>elem</desc></folder>\
               <bookmark href='http://b/' desc='battr'><title>B</title></bookmark>\
               <bookmark href='http://c/' desc='conly'><title>C</title></bookmark>\
             </xbel>",
        );
        let f = s.child_at(s.root(), 0).unwrap();
        assert_eq!(s.nodes[&f].desc, "elem");
        let b = s.child_at(s.root(), 1).unwrap();
        assert_eq!(s.nodes[&b].desc, "battr");
        let c = s.child_at(s.root(), 2).unwrap();
        assert_eq!(s.nodes[&c].desc, "conly");
    }

    #[test]
    fn file_round_trip() {
        let dir = std::env::temp_dir().join(format!(
            "arktos-bm-{}-{}",
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

    #[test]
    fn concurrent_saves_leave_a_parseable_file() {
        // CONC01: two writers on one bookmarks.xbel (standalone
        // instances share the profile dir) must never produce a torn
        // document — each install is a private temp file + rename and
        // the resolution rule is last-writer-wins.
        let dir = std::env::temp_dir().join(format!(
            "arktos-bm-conc-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("bookmarks.xbel");
        let build = |tag: &str| {
            let mut s = store();
            let f = s
                .create(
                    s.root(),
                    -1,
                    &json!({"type":1,"title":format!("F-{tag}")}),
                )
                .unwrap();
            s.create(
                f,
                -1,
                &json!({"type":2,"title":"b","url":format!("http://{tag}/")}),
            )
            .unwrap();
            s
        };
        let pa = path.clone();
        let ta = std::thread::spawn(move || {
            let mut s = build("a");
            for _ in 0..60 {
                s.save_path(&pa).unwrap();
            }
        });
        {
            let mut s = build("b");
            for _ in 0..60 {
                s.save_path(&path).unwrap();
            }
        }
        ta.join().unwrap();
        // Whatever the last writer was, the file must be one complete
        // document — never an interleave of the two.
        let mut s2 = store();
        s2.load_path(&path).unwrap();
        let f2 = s2.child_at(s2.root(), 0).unwrap();
        let title = s2.nodes[&f2].title.clone();
        assert!(title == "F-a" || title == "F-b", "torn save: {title}");
        let b = s2.child_at(f2, 0).unwrap();
        assert_eq!(
            s2.nodes[&b].url,
            format!("http://{}/", &title[2..]),
        );
        std::fs::remove_dir_all(&dir).ok();
    }
}
