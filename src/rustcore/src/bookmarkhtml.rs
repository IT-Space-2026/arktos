//! BKIMP01 — Netscape bookmark-file import/export at the store
//! boundary.
//!
//! The importer is a memory-safe transliteration of
//! tools/htmlToXBel/converter.cpp's forgiving tag tokenizer (the
//! Netscape format is HTML, not XML: unclosed <DT>/<HR>/<DD> elements
//! and valueless attributes rule out a strict parser).  Its inserts
//! go straight into the canonical tree — there is no XBEL detour —
//! and every inserted bookmark passes the store's url-scheme policy:
//!
//!   * http/https/ftp/mailto and schemeless/relative urls pass;
//!   * javascript: passes only behind the caller's
//!     "import_bookmarklets" flag — the single consent point for
//!     scriptable bookmarks;
//!   * every other scheme (data:, vbscript:, file:, the arora-*
//!     internals, ...) refuses — the store never accepts a url that
//!     WebView::loadUrl would execute or expose.
//!
//! The gate lives here AND in Bookmarks::create/set_str so it applies
//! to every insert, not just this path: a hostile .xbel import or a
//! dropped link hits the same wall.
//!
//! Re-imports are idempotent: a folder merges into a same-title
//! folder under the same parent, and a bookmark whose url already
//! sits directly under its container is counted as a duplicate.
//! That is also what makes the Qt adapter's flag-on second pass work
//! — only what the policy skipped lands on the re-run.
//!
//! The exporter emits the standard NETSCAPE-Bookmark-file-1 shape and
//! installs it atomically with mode 0600 — exported files can carry
//! token-bearing urls.

use std::collections::BTreeMap;
use std::fmt::Write as _;
use std::path::Path;

use serde_json::{json, Value};

use crate::bookmarks::{Bookmarks, Node, TYPE_BOOKMARK, TYPE_FOLDER, TYPE_SEPARATOR};
use crate::error::{Fail, RcResult, RcStatus};
use crate::store;

/// rc_bm_import_html flag bits (declared in include/rustcore.h).
pub const IMPORT_BOOKMARKLETS: i32 = 0x1;
pub const IMPORT_DRY_RUN: i32 = 0x2;

/// Same bound the Qt caller enforces on the file before handing it
/// over; defense in depth for direct FFI users.
const MAX_HTML: usize = 64 * 1024 * 1024;
/// converter.cpp's bounded closing-tag scan window — element bodies
/// are titles, so a hostile file of unclosed tags cannot turn the
/// scan quadratic.
const CLOSE_SCAN_WINDOW: usize = 256 * 1024;
/// Mirrors XBEL_MAX_DEPTH — deeper <DL> nesting is skipped, not
/// trusted (converter.cpp had no bound at all).
const MAX_FOLDER_DEPTH: usize = 256;
/// Total item bound: a 64 MB file can mint ~10M tiny tags; the store
/// does not grow without limit on attacker say-so.
const MAX_ITEMS: u64 = 512 * 1024;

fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
    Err(Fail {
        status: RcStatus::InvalidArgument,
        msg: msg.into(),
    })
}

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    Err(Fail {
        status: RcStatus::Corrupt,
        msg: msg.into(),
    })
}

// ---- url scheme policy ---------------------------------------------

/// Extracts the url's scheme the way a browser would resolve it:
/// ASCII whitespace and control bytes are stripped first (browsers
/// ignore "java\tscript:"'s embedded tab), then the token before the
/// first ':' must be a valid scheme name.  None means relative or
/// schemeless — harmless to hand to load().
fn url_scheme(url: &str) -> Option<String> {
    if url.is_empty() {
        return None;
    }
    let probe: String = url
        .chars()
        .filter(|c| !c.is_control() && !c.is_whitespace())
        .collect();
    let colon = probe.find(':')?;
    let scheme = &probe[..colon];
    if scheme.is_empty()
        || !scheme.chars().next()?.is_ascii_alphabetic()
        || !scheme
            .chars()
            .all(|c| c.is_ascii_alphanumeric() || matches!(c, '+' | '-' | '.'))
    {
        return None;
    }
    Some(scheme.to_ascii_lowercase())
}

/// The insert-time scheme gate.  `allow_javascript` is the
/// caller-passed import_bookmarklets flag — everywhere it is not
/// explicitly granted, scriptable bookmarks refuse.
pub fn check_bookmark_url(url: &str, allow_javascript: bool) -> RcResult<()> {
    let Some(scheme) = url_scheme(url) else {
        return Ok(());
    };
    let ok = match scheme.as_str() {
        "http" | "https" | "ftp" | "mailto" => true,
        "javascript" => allow_javascript,
        _ => false,
    };
    if ok {
        Ok(())
    } else {
        invalid(format!("bookmark url scheme refused: {scheme}"))
    }
}

// ---- forgiving tag tokenizer (converter.cpp transliteration) --------

struct HtmlTag {
    name: String,                  // lowercased
    attrs: Vec<(String, String)>,  // lowercased names, entity-decoded values
    closing: bool,
}

fn attr<'a>(tag: &'a HtmlTag, name: &str) -> Option<&'a str> {
    // Last occurrence wins — the converter's QHash overwrote dupes.
    tag.attrs
        .iter()
        .rev()
        .find(|(k, _)| k == name)
        .map(|(_, v)| v.as_str())
}

fn is_name_byte(b: u8) -> bool {
    b.is_ascii_alphanumeric() || b == b'-' || b == b':'
}

/// Parses a start or end tag starting at bytes[pos] == '<'.
/// On success *pos is left just past '>'; None is a hard stop —
/// converter.cpp's loop breaks on the first unparseable tag too.
fn parse_tag(bytes: &[u8], pos: &mut usize) -> Option<HtmlTag> {
    let mut i = *pos + 1;
    let mut closing = false;
    if i < bytes.len() && bytes[i] == b'/' {
        closing = true;
        i += 1;
    }
    let name_start = i;
    while i < bytes.len() && is_name_byte(bytes[i]) {
        i += 1;
    }
    // A name with a non-ASCII byte can never be dl/h3/a/hr, and
    // letting it degrade into "<a> + attr" would parse looser than
    // the C++ original — bail on the whole tag instead.
    if i == name_start || !bytes[name_start..i].is_ascii() {
        return None;
    }
    let name = std::str::from_utf8(&bytes[name_start..i])
        .ok()?
        .to_ascii_lowercase();
    let mut tag = HtmlTag {
        name,
        attrs: Vec::new(),
        closing,
    };
    while i < bytes.len() {
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        if i >= bytes.len() {
            break;
        }
        if bytes[i] == b'>' {
            i += 1;
            break;
        }
        if bytes[i] == b'/' {
            i += 1;
            continue;
        }
        let attr_start = i;
        while i < bytes.len()
            && !bytes[i].is_ascii_whitespace()
            && !matches!(bytes[i], b'=' | b'>' | b'/')
        {
            i += 1;
        }
        if !bytes[attr_start..i].is_ascii() {
            return None;
        }
        let attr_name = std::str::from_utf8(&bytes[attr_start..i])
            .ok()?
            .to_ascii_lowercase();
        while i < bytes.len() && bytes[i].is_ascii_whitespace() {
            i += 1;
        }
        let mut value = String::new();
        if i < bytes.len() && bytes[i] == b'=' {
            i += 1;
            while i < bytes.len() && bytes[i].is_ascii_whitespace() {
                i += 1;
            }
            if i < bytes.len() && matches!(bytes[i], b'"' | b'\'') {
                let quote = bytes[i];
                i += 1;
                let vs = i;
                while i < bytes.len() && bytes[i] != quote {
                    i += 1;
                }
                value = decode_entities(&bytes[vs..i]);
                if i < bytes.len() {
                    i += 1;
                }
            } else {
                let vs = i;
                while i < bytes.len()
                    && !bytes[i].is_ascii_whitespace()
                    && bytes[i] != b'>'
                {
                    i += 1;
                }
                value = decode_entities(&bytes[vs..i]);
            }
        }
        if !attr_name.is_empty() {
            tag.attrs.push((attr_name, value));
        }
    }
    *pos = i;
    Some(tag)
}

fn find_case_insensitive(haystack: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() || haystack.len() < needle.len() {
        return None;
    }
    (0..=haystack.len() - needle.len())
        .find(|&i| haystack[i..i + needle.len()].eq_ignore_ascii_case(needle))
}

/// Offset of the next "</name" sequence terminated by '>', whitespace
/// or '/'.  Matches converter.cpp: the index itself may sit anywhere,
/// but only matches fully inside the bounded scan window count.
fn find_closing_tag(bytes: &[u8], pos: usize, name: &str) -> Option<usize> {
    let end = bytes.len().min(pos + CLOSE_SCAN_WINDOW);
    let needle = format!("</{name}");
    let nb = needle.as_bytes();
    let mut p = pos;
    loop {
        let idx = find_case_insensitive(&bytes[p..], nb).map(|o| p + o)?;
        if idx + nb.len() > end {
            return None;
        }
        let after = idx + nb.len();
        if after >= bytes.len()
            || bytes[after] == b'>'
            || bytes[after].is_ascii_whitespace()
            || bytes[after] == b'/'
        {
            return Some(idx);
        }
        p = idx + 1;
    }
}

// ---- entities + text bodies -----------------------------------------

fn named_entity(name: &str) -> Option<char> {
    // The HTML4 set that shows up in real bookmark files; unknown
    // names pass through verbatim rather than being eaten.
    Some(match name {
        "amp" => '&',
        "lt" => '<',
        "gt" => '>',
        "quot" => '"',
        "apos" => '\'',
        "nbsp" => '\u{00A0}',
        "copy" => '©',
        "reg" => '®',
        "trade" => '™',
        "hellip" => '…',
        "mdash" => '—',
        "ndash" => '–',
        "lsquo" => '‘',
        "rsquo" => '’',
        "ldquo" => '“',
        "rdquo" => '”',
        "laquo" => '«',
        "raquo" => '»',
        "euro" => '€',
        "pound" => '£',
        "yen" => '¥',
        "cent" => '¢',
        "sect" => '§',
        "para" => '¶',
        "middot" => '·',
        "deg" => '°',
        "plusmn" => '±',
        "times" => '×',
        "divide" => '÷',
        "frac12" => '½',
        "frac14" => '¼',
        "frac34" => '¾',
        "iexcl" => '¡',
        "iquest" => '¿',
        "bull" => '•',
        "dagger" => '†',
        "Dagger" => '‡',
        "permil" => '‰',
        "larr" => '←',
        "uarr" => '↑',
        "rarr" => '→',
        "darr" => '↓',
        "harr" => '↔',
        // Structural characters — a hostile href can hide the scheme
        // behind entity encoding ("javascript&colon;..."), so the
        // decode has to happen before the scheme gate sees it.
        "colon" => ':',
        "sol" => '/',
        "bsol" => '\\',
        "lpar" => '(',
        "rpar" => ')',
        "Tab" => '\t',
        "NewLine" => '\n',
        // Latin-1 set that appears in real bookmark titles.
        "eacute" => 'é',
        "aacute" => 'á',
        "oacute" => 'ó',
        "iacute" => 'í',
        "uacute" => 'ú',
        "egrave" => 'è',
        "agrave" => 'à',
        "euml" => 'ë',
        "ouml" => 'ö',
        "auml" => 'ä',
        "uuml" => 'ü',
        "ccedil" => 'ç',
        "ntilde" => 'ñ',
        "szlig" => 'ß',
        _ => return None,
    })
}

/// Entity-decodes UTF-8 bytes: numeric refs plus the named set above;
/// an unrecognized "&foo" is copied through literally.
fn decode_entities(bytes: &[u8]) -> String {
    let text = String::from_utf8_lossy(bytes);
    if !text.contains('&') {
        return text.into_owned();
    }
    let mut out = String::with_capacity(text.len());
    let mut rest = text.as_ref();
    while let Some(amp) = rest.find('&') {
        out.push_str(&rest[..amp]);
        rest = &rest[amp..];
        let semi = rest[1..].find(';').map(|o| o + 1);
        let decoded = semi.and_then(|s| {
            let body = &rest[1..s];
            if s - 1 > 32 {
                return None;
            }
            if let Some(num) = body.strip_prefix('#') {
                let (digits, radix) = match num.strip_prefix('x').or_else(|| num.strip_prefix('X'))
                {
                    Some(h) => (h, 16),
                    None => (num, 10),
                };
                if digits.is_empty() || !digits.chars().all(|c| c.is_digit(radix)) {
                    return None;
                }
                u32::from_str_radix(digits, radix)
                    .ok()
                    .and_then(char::from_u32)
            } else if body.chars().all(|c| c.is_ascii_alphanumeric()) {
                named_entity(body)
            } else {
                None
            }
        });
        match decoded {
            Some(c) => {
                out.push(c);
                rest = &rest[semi.unwrap() + 1..];
            }
            None => {
                out.push('&');
                rest = &rest[1..];
            }
        }
    }
    out.push_str(rest);
    out
}

/// htmlToText: strips markup inside an element body and entity-
/// decodes what remains (the converter's QTextDocumentFragment step).
fn html_to_text(bytes: &[u8]) -> String {
    let mut out = String::new();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'<' {
            while i < bytes.len() && bytes[i] != b'>' {
                i += 1;
            }
            if i < bytes.len() {
                i += 1;
            }
        } else {
            let start = i;
            while i < bytes.len() && bytes[i] != b'<' {
                i += 1;
            }
            out.push_str(&decode_entities(&bytes[start..i]));
        }
    }
    out
}

// ---- import -----------------------------------------------------------

#[derive(Default)]
struct Summary {
    imported: u64,
    folders: u64,
    separators: u64,
    duplicates: u64,
    skipped: u64,
    skipped_by_scheme: BTreeMap<String, u64>,
    truncated: bool,
}

impl Summary {
    fn items(&self) -> u64 {
        self.imported + self.folders + self.separators + self.duplicates + self.skipped
    }

    fn to_json(&self) -> String {
        let schemes: Value = self
            .skipped_by_scheme
            .iter()
            .map(|(k, v)| (k.clone(), json!(v)))
            .collect::<serde_json::Map<String, Value>>()
            .into();
        serde_json::to_string(&json!({
            "imported": self.imported,
            "folders": self.folders,
            "separators": self.separators,
            "duplicates": self.duplicates,
            "skipped": self.skipped,
            "skipped_by_scheme": schemes,
            "truncated": self.truncated,
        }))
        .unwrap_or_else(|_| "{}".into())
    }
}

/// Per-level bookkeeping mirroring converter.cpp's FolderState: the
/// <H3> seen since the last <DL> on this level names the next folder.
struct Level {
    pending_title: String,
    pending_folded: bool,
    handle: Option<u64>, // None under a dry run or beyond the depth cap
}

/// Parses a Netscape bookmark document and inserts it under `parent`,
/// returning (summary JSON, whether anything was created).
///
/// With `dry_run` the full scan + scheme accounting runs but nothing
/// is inserted — the Qt adapter's consent probe.  Counts are an
/// estimate then: dedup only sees the parent's real children, so a
/// would-be insert counts as imported rather than a duplicate.
pub fn import_html(
    bm: &mut Bookmarks,
    parent: u64,
    html: &[u8],
    allow_bookmarklets: bool,
    dry_run: bool,
) -> RcResult<(String, bool)> {
    if html.len() > MAX_HTML {
        return corrupt("the HTML document is too large");
    }
    match bm.node_ref(parent).map(|n| n.kind) {
        Some(TYPE_FOLDER) | Some(crate::bookmarks::TYPE_ROOT) => {}
        Some(_) => return invalid("cannot import under a leaf node"),
        None => return invalid("bad parent handle"),
    }

    let text = String::from_utf8_lossy(html);
    let bytes = text.as_bytes();

    let mut sum = Summary::default();
    let mut levels: Vec<Level> = vec![Level {
        pending_title: String::new(),
        pending_folded: false,
        handle: Some(parent),
    }];
    let mut overflow_depth = 0usize; // <DL>s skipped beyond the cap

    let mut pos = 0usize;
    while pos < bytes.len() {
        if bytes[pos] != b'<' {
            match bytes[pos..].iter().position(|&b| b == b'<') {
                Some(off) => pos += off,
                None => break,
            }
            continue;
        }
        if bytes[pos..].starts_with(b"<!--") {
            pos = find_case_insensitive(&bytes[pos + 4..], b"-->")
                .map(|o| pos + 4 + o + 3)
                .unwrap_or(bytes.len());
            continue;
        }
        if pos + 1 < bytes.len() && matches!(bytes[pos + 1], b'!' | b'?') {
            // <!DOCTYPE ...>, <![CDATA[...]>, <?pi?>
            pos = bytes[pos + 2..]
                .iter()
                .position(|&b| b == b'>')
                .map(|o| pos + 2 + o + 1)
                .unwrap_or(bytes.len());
            continue;
        }
        let mut tp = pos;
        let Some(tag) = parse_tag(bytes, &mut tp) else {
            break;
        };
        pos = tp;

        if tag.closing {
            if tag.name == "dl" {
                if overflow_depth > 0 {
                    overflow_depth -= 1;
                } else if levels.len() > 1 {
                    levels.pop();
                }
            }
            continue;
        }

        match tag.name.as_str() {
            "dl" => {
                if levels.len() - 1 + overflow_depth >= MAX_FOLDER_DEPTH {
                    overflow_depth += 1;
                    continue;
                }
                let (title, folded) = {
                    let top = levels.last().unwrap();
                    (top.pending_title.clone(), top.pending_folded)
                };
                let container = levels.last().unwrap().handle;
                let h = match container {
                    Some(c) => insert_folder(bm, c, &title, !folded, dry_run, &mut sum)?,
                    None => None, // inside a phantom level — count nothing
                };
                // The stack slot is pushed either way so this level's
                // </DL> pairs correctly.
                levels.push(Level {
                    pending_title: String::new(),
                    pending_folded: false,
                    handle: h,
                });
            }
            "hr" => {
                if let Some(c) = levels.last().unwrap().handle {
                    if !dry_run {
                        let h = bm.alloc_node(Node::new(TYPE_SEPARATOR));
                        bm.attach(c, -1, h)?;
                    }
                    sum.separators += 1;
                }
            }
            "h3" => {
                let end = find_closing_tag(bytes, pos, "h3");
                let title = end
                    .map(|e| html_to_text(&bytes[pos..e]))
                    .unwrap_or_default();
                let top = levels.last_mut().unwrap();
                top.pending_title = title;
                top.pending_folded = attr(&tag, "folded").is_some();
                if let Some(e) = end {
                    pos = e;
                }
            }
            "a" => {
                let Some(href) = attr(&tag, "href") else {
                    continue;
                };
                let end = find_closing_tag(bytes, pos, "a");
                let title = end
                    .map(|e| html_to_text(&bytes[pos..e]))
                    .unwrap_or_default();
                if let Some(c) = levels.last().unwrap().handle {
                    insert_bookmark(bm, c, href, &title, allow_bookmarklets, dry_run, &mut sum)?;
                }
                if let Some(e) = end {
                    pos = e;
                }
            }
            _ => {}
        }
        if sum.items() >= MAX_ITEMS {
            sum.truncated = true;
            break;
        }
    }

    let created = !dry_run && (sum.imported + sum.folders + sum.separators) > 0;
    Ok((sum.to_json(), created))
}

/// A <DL> opens a folder under the current container titled by the
/// last <H3> — but merges into a same-title sibling instead of
/// duplicating it, so a re-run folds back into itself.
fn insert_folder(
    bm: &mut Bookmarks,
    parent: u64,
    title: &str,
    expanded: bool,
    dry_run: bool,
    sum: &mut Summary,
) -> RcResult<Option<u64>> {
    if dry_run {
        sum.folders += 1;
        return Ok(Some(0)); // phantom handle — children count but don't store
    }
    if let Some(h) = bm.child_folder_named(parent, title) {
        return Ok(Some(h));
    }
    let mut node = Node::new(TYPE_FOLDER);
    node.title = title.into();
    node.expanded = expanded;
    let h = bm.alloc_node(node);
    bm.attach(parent, -1, h)?;
    sum.folders += 1;
    Ok(Some(h))
}

fn insert_bookmark(
    bm: &mut Bookmarks,
    parent: u64,
    url: &str,
    title: &str,
    allow_bookmarklets: bool,
    dry_run: bool,
    sum: &mut Summary,
) -> RcResult<()> {
    if check_bookmark_url(url, allow_bookmarklets).is_err() {
        sum.skipped += 1;
        *sum
            .skipped_by_scheme
            .entry(url_scheme(url).unwrap_or_else(|| "other".into()))
            .or_insert(0) += 1;
        return Ok(());
    }
    // Dry-run dedup only sees what the parent really holds; phantom
    // parents (handle 0) get no store check at all.
    if !dry_run && !url.is_empty() && bm.child_has_bookmark_url(parent, url) {
        sum.duplicates += 1;
        return Ok(());
    }
    if !dry_run {
        let mut node = Node::new(TYPE_BOOKMARK);
        node.url = url.into();
        node.title = if title.is_empty() {
            "Unknown title".into()
        } else {
            title.into()
        };
        let h = bm.alloc_node(node);
        bm.attach(parent, -1, h)?;
    }
    sum.imported += 1;
    Ok(())
}

// ---- export -----------------------------------------------------------

/// The standard NETSCAPE-Bookmark-file-1 document — readable by every
/// browser and by the importer above.
pub fn to_html(bm: &Bookmarks) -> String {
    let mut out = String::with_capacity(8 * 1024);
    out.push_str("<!DOCTYPE NETSCAPE-Bookmark-file-1>\n");
    out.push_str(
        "<!-- This is an automatically generated file.\n     \
         It will be read and overwritten.\n     DO NOT EDIT! -->\n",
    );
    out.push_str(
        "<META HTTP-EQUIV=\"Content-Type\" CONTENT=\"text/html; charset=UTF-8\">\n",
    );
    out.push_str("<TITLE>Bookmarks</TITLE>\n<H1>Bookmarks</H1>\n<DL><p>\n");
    if let Some(children) = bm.children_of(bm.root()) {
        for h in children {
            write_html_item(&mut out, bm, h, 1);
        }
    }
    out.push_str("</DL><p>\n");
    out
}

fn write_html_item(out: &mut String, bm: &Bookmarks, h: u64, depth: usize) {
    let Some(n) = bm.node_ref(h) else {
        return;
    };
    let pad = "    ".repeat(depth);
    match n.kind {
        TYPE_FOLDER => {
            let _ = writeln!(out, "{pad}<DT><H3>{}</H3>", escape_text(&n.title));
            let _ = writeln!(out, "{pad}<DL><p>");
            for c in &n.children {
                write_html_item(out, bm, *c, depth + 1);
            }
            let _ = writeln!(out, "{pad}</DL><p>");
        }
        TYPE_BOOKMARK => {
            if n.url.is_empty() {
                let _ = writeln!(out, "{pad}<DT><A>{}</A>", escape_text(&n.title));
            } else {
                let _ = writeln!(
                    out,
                    "{pad}<DT><A HREF=\"{}\">{}</A>",
                    escape_attr(&n.url),
                    escape_text(&n.title)
                );
            }
        }
        TYPE_SEPARATOR => {
            let _ = writeln!(out, "{pad}<HR>");
        }
        _ => {}
    }
}

/// STRICT escaping — a crafted title or url containing tag soup,
/// quote-breakers or entity tricks must produce harmless well-formed
/// output, because the file is reparsed by browsers on import.
fn escape_text(s: &str) -> String {
    s.replace('&', "&amp;")
        .replace('<', "&lt;")
        .replace('>', "&gt;")
}

fn escape_attr(s: &str) -> String {
    escape_text(s)
        .replace('"', "&quot;")
        .replace('\'', "&#39;")
}

/// Emits + installs the export atomically (temp + fsync + rename,
/// mode 0600 on unix — exports can carry token-bearing urls).
pub fn save_html_path(bm: &Bookmarks, path: &Path) -> RcResult<()> {
    store::atomic_write(path, to_html(bm).as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::bookmarks::{TYPE_FOLDER, TYPE_SEPARATOR};

    fn store() -> Bookmarks {
        Bookmarks::new()
    }

    /// Walks (kind, title, url) tuples depth-first — the assertion
    /// shape for tree-equality checks.
    fn shape(bm: &Bookmarks) -> Vec<(u8, String, String)> {
        fn walk(bm: &Bookmarks, h: u64, out: &mut Vec<(u8, String, String)>) {
            let Some(n) = bm.node_ref(h) else { return };
            out.push((n.kind, n.title.clone(), n.url.clone()));
            for c in &n.children {
                walk(bm, *c, out);
            }
        }
        let mut v = Vec::new();
        walk(bm, bm.root(), &mut v);
        v
    }

    fn summary_json(s: &str) -> Value {
        serde_json::from_str(s).unwrap()
    }

    const DOC: &str = "\
<!DOCTYPE NETSCAPE-Bookmark-file-1>
<TITLE>Bookmarks</TITLE><H1>Bookmarks</H1>
<DL><p>
    <DT><H3>Folder &amp; One</H3>
    <DL><p>
        <DT><A HREF=\"https://a.example/\">Alpha</A>
        <DT><A HREF=\"https://b.example/?x=1&amp;y=2\">B&eacute;ta</A>
        <HR>
        <DT><H3 folded>Sub &#233;</H3>
        <DL><p>
            <DT><A HREF=\"https://s.example/\">Sub item</A>
        </DL><p>
    </DL><p>
    <DT><A HREF=\"https://top.example/\">Top</A>
</DL><p>";

    #[test]
    fn import_basic_tree() {
        let mut bm = store();
        let root = bm.root();
        let (json, created) = import_html(&mut bm, root, DOC.as_bytes(), false, false).unwrap();
        assert!(created);
        let s = summary_json(&json);
        assert_eq!(s["imported"], 4);
        assert_eq!(s["folders"], 3); // outer <DL> + Folder One + Sub
        assert_eq!(s["separators"], 1);
        assert_eq!(s["skipped"], 0);

        let outer = bm.child_at(root, 0).unwrap();
        // The top <DL> becomes the empty-title wrapper converter.cpp
        // produced inside the "Imported <date>" folder.
        assert_eq!(bm.node_ref(outer).unwrap().kind, TYPE_FOLDER);
        let f1 = bm.child_folder_named(outer, "Folder & One").unwrap();
        assert_eq!(bm.node_ref(f1).unwrap().expanded, true); // no FOLDED
        let sub = bm.child_folder_named(f1, "Sub é").unwrap();
        assert_eq!(bm.node_ref(sub).unwrap().expanded, false); // FOLDED
        let a = bm.child_at(f1, 0).unwrap();
        assert_eq!(bm.node_ref(a).unwrap().url, "https://a.example/");
        assert_eq!(bm.node_ref(a).unwrap().title, "Alpha");
        let b = bm.child_at(f1, 1).unwrap();
        assert_eq!(
            bm.node_ref(b).unwrap().url,
            "https://b.example/?x=1&y=2"
        );
        assert_eq!(bm.node_ref(b).unwrap().title, "Béta");
        assert_eq!(
            bm.node_ref(bm.child_at(f1, 2).unwrap()).unwrap().kind,
            TYPE_SEPARATOR
        );
        // <DT>/<p>/<DD>/<TITLE>/<H1>/DOCTYPE all ignored silently.
        let top = bm.child_at(outer, 1).unwrap();
        assert_eq!(bm.node_ref(top).unwrap().title, "Top");
    }

    #[test]
    fn import_scheme_gate() {
        const HOSTILE: &str = "\
<DL><p>
<DT><A HREF=\"javascript:alert(1)\">js</A>
<DT><A HREF=\" JaVa\tScRiPt:alert(2)\">js-ws</A>
<DT><A HREF=\"javascript&colon;alert(3)\">js-ent</A>
<DT><A HREF=\"data:text/html,<script>x</script>\">data</A>
<DT><A HREF=\"vbscript:x\">vbs</A>
<DT><A HREF=\"file:///etc/passwd\">file</A>
<DT><A HREF=\"arora-file:///\">internal</A>
<DT><A HREF=\"about:blank\">about</A>
<DT><A HREF=\"https://ok.example/\">ok</A>
<DT><A HREF=\"ftp://f.example/\">ftp</A>
<DT><A HREF=\"mailto:a@b.c\">mail</A>
<DT><A HREF=\"relative/path\">rel</A>
</DL><p>";
        let mut bm = store();
        let root = bm.root();
        let (json, _) = import_html(&mut bm, root, HOSTILE.as_bytes(), false, false).unwrap();
        let s = summary_json(&json);
        assert_eq!(s["imported"], 4); // ok + ftp + mailto + relative
        assert_eq!(s["skipped"], 8);
        let schemes = &s["skipped_by_scheme"];
        assert_eq!(schemes["javascript"], 3);
        assert_eq!(schemes["data"], 1);
        assert_eq!(schemes["vbscript"], 1);
        assert_eq!(schemes["file"], 1);
        assert_eq!(schemes["arora-file"], 1);
        assert_eq!(schemes["about"], 1);
        // Nothing refused reached the store.
        assert_eq!(bm.find_url("javascript:alert(1)"), None);
        assert_eq!(bm.find_url("file:///etc/passwd"), None);

        // With the consent flag only javascript: lands — the rest of
        // the refuse list is unconditional.
        let mut bm2 = store();
        let root2 = bm2.root();
        let (json2, _) =
            import_html(&mut bm2, root2, HOSTILE.as_bytes(), true, false).unwrap();
        let s2 = summary_json(&json2);
        assert_eq!(s2["imported"], 7); // +3 javascript urls
        assert_eq!(s2["skipped"], 5);
        assert!(bm2.find_url("javascript:alert(1)").is_some());
        assert!(bm2.find_url("file:///etc/passwd").is_none());
    }

    #[test]
    fn import_dedup_and_merge() {
        let mut bm = store();
        let root = bm.root();
        import_html(&mut bm, root, DOC.as_bytes(), false, false).unwrap();
        let nodes_before = shape(&bm).len();
        // Re-running the same document folds into itself: every
        // folder merges by title, every bookmark dedups by url —
        // only the <HR> is positional and appends again at its
        // folder's end.
        let (json, _) = import_html(&mut bm, root, DOC.as_bytes(), false, false).unwrap();
        let s = summary_json(&json);
        assert_eq!(s["imported"], 0);
        assert_eq!(s["folders"], 0);
        assert_eq!(s["duplicates"], 4);
        assert_eq!(s["separators"], 1);
        assert_eq!(shape(&bm).len(), nodes_before + 1);
    }

    #[test]
    fn import_dry_run_inserts_nothing() {
        let mut bm = store();
        let root = bm.root();
        let (json, created) = import_html(&mut bm, root, DOC.as_bytes(), false, true).unwrap();
        assert!(!created);
        let s = summary_json(&json);
        assert_eq!(s["imported"], 4);
        assert!(bm.child_count(root) == Some(0));
    }

    #[test]
    fn import_bad_parent_and_hostile_input() {
        let mut bm = store();
        let leaf = bm
            .create(
                bm.root(),
                -1,
                &serde_json::json!({"type":2,"url":"https://x/","title":"x"}),
            )
            .unwrap();
        assert!(import_html(&mut bm, 0xDEAD, b"<DL>", false, false).is_err());
        assert!(import_html(&mut bm, leaf, b"<DL>", false, false).is_err());

        // Unclosed everything, deep nesting, one huge line, bogus
        // tags — bounded, no hang, no panic.
        let mut hostile = String::from("<DL><p><DT><H3>deep</H3>");
        for _ in 0..500 {
            hostile.push_str("<DL><p>");
        }
        hostile.push_str(&"<DT><A HREF=\"https://x/\">".repeat(800));
        hostile.push_str(&"x".repeat(512 * 1024));
        hostile.push_str("<a <a <a garbage <DT><A HREF=");
        let mut bm2 = store();
        let root2 = bm2.root();
        let before = std::time::Instant::now();
        let (json, _) =
            import_html(&mut bm2, root2, hostile.as_bytes(), false, false).unwrap();
        assert!(before.elapsed().as_secs() < 10);
        let s = summary_json(&json);
        // Folders capped at MAX_FOLDER_DEPTH instead of 500.
        assert!(s["folders"].as_u64().unwrap() <= 1 + 256);
    }

    #[test]
    fn export_round_trip() {
        let mut bm = store();
        let root = bm.root();
        let f = bm
            .create(
                root,
                -1,
                &serde_json::json!({"type":1,"title":"F & <B> \"q\"","expanded":true}),
            )
            .unwrap();
        bm.create(
            f,
            -1,
            &serde_json::json!({"type":2,"title":"b &amp; é","url":"https://x/?a=1&b=\"2\""}),
        )
        .unwrap();
        bm.create(f, -1, &serde_json::json!({"type":3})).unwrap();
        bm.create(
            root,
            -1,
            &serde_json::json!({"type":2,"title":"</A><script>alert(1)</script>é漢字","url":"https://evil/'><img>"}),
        )
        .unwrap();

        let html = to_html(&bm);
        // Strict escaping: no raw payload survives.
        assert!(!html.contains("</A><script>"));
        assert!(html.contains("&lt;/A&gt;&lt;script&gt;"));
        assert!(html.contains("HREF=\"https://evil/&#39;&gt;&lt;img&gt;\""));
        assert!(html.contains("HREF=\"https://x/?a=1&amp;b=&quot;2&quot;\""));

        // Re-import preserves structure + the hostile strings as text.
        let mut bm2 = store();
        let root2 = bm2.root();
        import_html(&mut bm2, root2, html.as_bytes(), false, false).unwrap();
        let outer = bm2.child_at(root2, 0).unwrap();
        let f2 = bm2.child_folder_named(outer, "F & <B> \"q\"").unwrap();
        let b2 = bm2.child_at(f2, 0).unwrap();
        assert_eq!(bm2.node_ref(b2).unwrap().url, "https://x/?a=1&b=\"2\"");
        let evil = bm2.child_at(outer, 1).unwrap();
        assert_eq!(
            bm2.node_ref(evil).unwrap().title,
            "</A><script>alert(1)</script>é漢字"
        );
        assert_eq!(bm2.node_ref(evil).unwrap().url, "https://evil/'><img>");
    }

    #[test]
    fn export_file_perms_and_atomic() {
        let dir = std::env::temp_dir().join(format!(
            "arora-bmhtml-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("export.html");
        let mut bm = store();
        bm.create(
            bm.root(),
            -1,
            &serde_json::json!({"type":2,"title":"t","url":"https://x/"}),
        )
        .unwrap();
        save_html_path(&bm, &path).unwrap();
        let text = std::fs::read_to_string(&path).unwrap();
        assert!(text.contains("NETSCAPE-Bookmark-file-1"));
        assert!(text.contains("HREF=\"https://x/\""));
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(&path).unwrap().permissions().mode() & 0o777, 0o600);
        }
        // No temp residue.
        assert_eq!(std::fs::read_dir(&dir).unwrap().count(), 1);
        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn create_gate_covers_plain_inserts() {
        let mut bm = store();
        let root = bm.root();
        // The gate lives in create() — the import path is not the
        // only way a bad scheme can arrive (drops, XBEL imports).
        assert!(bm
            .create(root, -1, &serde_json::json!({"type":2,"url":"javascript:x","title":"j"}))
            .is_err());
        assert!(bm
            .create(root, -1, &serde_json::json!({"type":2,"url":"data:,x","title":"d"}))
            .is_err());
        assert!(bm
            .create(
                root,
                -1,
                &serde_json::json!({"type":2,"url":"javascript:x","title":"j","allow_javascript":true})
            )
            .is_ok());
        // Deliberate single-node edits admit javascript:; data: still
        // refuses.
        let h = bm
            .create(root, -1, &serde_json::json!({"type":2,"url":"https://x/","title":"x"}))
            .unwrap();
        assert!(bm.set_str(h, "url", "javascript:alert(1)").is_ok());
        assert!(bm.set_str(h, "url", "file:///etc/passwd").is_err());
        // schemeless stays fine everywhere.
        assert!(bm
            .create(root, -1, &serde_json::json!({"type":2,"url":"relative/path","title":"r"}))
            .is_ok());
    }
}
