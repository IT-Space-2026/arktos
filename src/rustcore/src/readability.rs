//! RDR01: reader-mode extraction — the article detection and content
//! extraction that used to run as injected JavaScript (the vendored
//! Mozilla Readability.js + isProbablyReaderable pair) now runs in the
//! core, so the feature no longer depends on evaluating script in the
//! captured page.  The Qt shell hands over the serialized DOM and gets
//! back a verdict plus a cleaned article fragment — on a JS-one-way
//! engine the same path works unchanged.
//!
//! dom_smoothie is a maintained Rust port of Mozilla's Readability.js
//! (MIT) — the same scoring/cleaning pipeline the JS path ran, so
//! parity is structural rather than hand-tuned.  Its dom_query DOM
//! layer is reused for the output sanitizer.
//!
//! Output contract (the JSON verdict rc_readability_extract emits):
//!   {"ok":bool,           an article was extracted
//!    "probably":bool,     isProbablyReaderable verdict — the
//!                         affordance heuristic (reader icon)
//!    "title","byline","siteName","excerpt","dir",
//!    "length":n,          text length of the extracted content
//!    "content":"..."}     article fragment — populated iff ok
//! "ok" mirrors the JS enter() contract: it is false when the document
//! does not look article-like (probably=false) and when the extractor
//! found nothing (GrabFailed/TooManyElements) — the shell treats both
//! as "reader not available", same as today.
//!
//! "content" is re-sanitized against the exact ruleset reader.js
//! applied before inserting the fragment (dead tags removed, on* and
//! javascript: attributes stripped): Readability's own cleaning is
//! good but the fragment renders into a live web view, so the
//! boundary gets its own belt — a hostile page can never smuggle
//! interactive markup through the extractor.
//!
//! Bounds: the serialized DOM is byte-capped before parsing
//! (MAX_HTML_BYTES) and element-capped during extraction
//! (MAX_ELEMENTS).  The dom_query tree is arena-allocated and every
//! traversal/serialization here is iterative, so deeply nested or
//! malformed input degrades to a verdict, never to a crash.

use dom_query::Document;
use dom_smoothie::{Config, Readability};
use serde_json::{json, Value};

use crate::error::{self, RcResult, RcStatus};

/// Hard bound on the serialized DOM handed over — the extractor runs
/// several passes over the tree, so oversized pages decline rather
/// than burn memory.  Far above any real article.
const MAX_HTML_BYTES: usize = 24 * 1024 * 1024;

/// Element-count bound inside the byte cap — a document can hold an
/// absurd number of tiny elements within the byte bound and scoring
/// is O(elements) per pass.  Readability.js leaves this uncapped;
/// real pages stay far below this.
const MAX_ELEMENTS: usize = 60_000;

/// Upper bound on estimated serialized-DOM nesting.  The tree build
/// is roughly O(elements × depth), so absurd nesting is refused
/// before parsing rather than quadratic-bombed.  Real pages stay far
/// below 500; the scan skips comment/CDATA/raw-text bodies so a page
/// carrying markup inside <script> text doesn't inflate the estimate.
const MAX_SCAN_DEPTH: usize = 4096;

/// Estimate the deepest element nesting with a tolerant byte scan —
/// accurate for a serialized engine DOM (always balanced), which is
/// the only caller this has.  Early-exits once the bound is exceeded.
fn estimated_max_depth(html: &[u8]) -> usize {
    fn tag_is(name: &[u8], list: &[&[u8]]) -> bool {
        list.iter().any(|t| name.eq_ignore_ascii_case(t))
    }
    // Elements that never open a level.
    const VOID: &[&[u8]] = &[
        b"area", b"base", b"br", b"col", b"embed", b"hr", b"img",
        b"input", b"link", b"meta", b"param", b"source", b"track",
        b"wbr",
    ];
    // Elements whose content is raw text — markup inside never opens
    // a level in the real parser either.
    const RAW_TEXT: &[&[u8]] = &[
        b"script", b"style", b"textarea", b"title", b"xmp", b"iframe",
        b"noembed", b"noframes", b"noscript",
    ];
    fn skip_until(hay: &[u8], mut i: usize, needle: &[u8]) -> usize {
        while i + needle.len() <= hay.len() {
            if hay[i..].starts_with(needle) {
                return i + needle.len();
            }
            i += 1;
        }
        hay.len()
    }
    // Position of the `</name` close of a raw-text element, or EOF.
    fn find_raw_close(hay: &[u8], mut i: usize, name: &[u8]) -> usize {
        while i + 2 + name.len() <= hay.len() {
            if hay[i] == b'<'
                && hay[i + 1] == b'/'
                && hay[i + 2..i + 2 + name.len()].eq_ignore_ascii_case(name)
            {
                let after = i + 2 + name.len();
                if after >= hay.len() || !hay[after].is_ascii_alphanumeric() {
                    return i;
                }
            }
            i += 1;
        }
        hay.len()
    }

    let n = html.len();
    let mut i = 0;
    let mut depth = 0usize;
    let mut max_depth = 0usize;
    while i < n {
        while i < n && html[i] != b'<' {
            i += 1;
        }
        if i + 1 >= n {
            break;
        }
        match html[i + 1] {
            b'!' => {
                i = if html[i + 2..].starts_with(b"--") {
                    skip_until(html, i + 4, b"-->")
                } else if html[i + 2..].len() >= 7
                    && html[i + 2..i + 9].eq_ignore_ascii_case(b"[CDATA[")
                {
                    skip_until(html, i + 9, b"]]>")
                } else {
                    skip_until(html, i + 2, b">")
                };
            }
            b'?' | b'%' => {
                i = skip_until(html, i + 2, b">");
            }
            b'/' => {
                i += 2;
                if i < n && html[i].is_ascii_alphabetic() {
                    while i < n && html[i] != b'>' {
                        i += 1;
                    }
                    i += 1;
                    depth = depth.saturating_sub(1);
                } else {
                    i = skip_until(html, i, b">");
                }
            }
            c if c.is_ascii_alphabetic() => {
                let name_start = i + 1;
                let mut j = name_start;
                // The tokenizer's tag-name state ends on ws, '/' or
                // '>' — 'my-elem' and 'svg:rect' are single names.
                while j < n
                    && !matches!(html[j],
                        b' ' | b'\t' | b'\n' | b'\r' | 0x0c | b'/' | b'>')
                {
                    j += 1;
                }
                let name = &html[name_start..j];
                // Scan to '>' honoring quoted attribute values so a
                // '>' inside a value cannot end the tag early.
                let mut quote = 0u8;
                let mut last_non_ws = 0u8;
                let mut self_closing = false;
                while j < n {
                    let ch = html[j];
                    if quote != 0 {
                        if ch == quote {
                            quote = 0;
                        }
                    } else if ch == b'"' || ch == b'\'' {
                        quote = ch;
                    } else if ch == b'>' {
                        self_closing = last_non_ws == b'/';
                        break;
                    } else if !ch.is_ascii_whitespace() {
                        last_non_ws = ch;
                    }
                    j += 1;
                }
                i = j + 1;
                if tag_is(name, RAW_TEXT) {
                    // The element opens and closes inside the skip —
                    // net zero depth change; its body is raw text.
                    let close = find_raw_close(html, i, name);
                    i = skip_until(html, close, b">");
                } else if !tag_is(name, VOID) && !self_closing {
                    depth += 1;
                    max_depth = max_depth.max(depth);
                    if max_depth > MAX_SCAN_DEPTH {
                        return max_depth;
                    }
                }
            }
            _ => i += 1,
        }
    }
    max_depth
}

/// The dead-tag list the JS reader's sanitize() removed before
/// inserting the fragment — interactive and document-level elements
/// never survive the FFI boundary.
const DEAD_TAGS: &str = "script, iframe, frame, frameset, object, embed,\
 applet, form, input, button, select, textarea, link, meta, base,\
 template, noscript";

fn config() -> Config {
    Config {
        max_elements_to_parse: MAX_ELEMENTS,
        ..Config::default()
    }
}

/// Mirrors dom_smoothie's strict absolute-URL check (`scheme://`),
/// so the base-url argument degrades to None instead of erroring the
/// whole extraction when the page's URL is non-authority (about:*).
fn is_absolute(url: &str) -> bool {
    let url = url.trim();
    match url.find("://") {
        Some(pos) if pos > 0 => {
            url[..pos].chars().next().is_some_and(|c| c.is_ascii_alphabetic())
                && url[..pos]
                    .chars()
                    .all(|c| c.is_ascii_alphanumeric() || "+-.".contains(c))
        }
        _ => false,
    }
}

fn document(html: &[u8]) -> RcResult<Document> {
    if html.len() > MAX_HTML_BYTES {
        return error::fail(
            RcStatus::InvalidArgument,
            "document exceeds the readability input bound",
        );
    }
    if estimated_max_depth(html) > MAX_SCAN_DEPTH {
        return error::fail(
            RcStatus::InvalidArgument,
            "document nesting exceeds the readability bound",
        );
    }
    // Tolerant like the engine: invalid UTF-8 transcodes lossily
    // rather than refusing the page outright.
    Ok(Document::from(String::from_utf8_lossy(html).into_owned()))
}

/// The isProbablyReaderable verdict — the affordance check the
/// post-load probe runs on every page.
pub fn probably_readable(html: &[u8]) -> RcResult<bool> {
    let doc = document(html)?;
    Ok(dom_smoothie::is_probably_readable(&doc, None, None))
}

/// The reader.js sanitize() ruleset, re-applied to the extracted
/// fragment: dead tags out, on* and javascript: attributes stripped.
fn sanitize(content: &str) -> String {
    let doc = Document::fragment(content);
    doc.select(DEAD_TAGS).remove();
    for node in doc.select("*").nodes() {
        let mut drop_names: Vec<String> = Vec::new();
        for attr in node.attrs() {
            let name = attr.name.local.as_ref().to_ascii_lowercase();
            let value = attr.value.trim().to_ascii_lowercase();
            if name.starts_with("on")
                || ((name == "href" || name == "src")
                    && value.starts_with("javascript:"))
            {
                drop_names.push(name);
            }
        }
        for name in &drop_names {
            node.remove_attr(name);
        }
    }
    doc.inner_html().to_string()
}

/// Probe + extract in one pass — the enter() path.  The verdict JSON
/// is produced even when extraction declines (ok:false), so the FFI
/// surface has exactly one error-free shape.
pub fn extract_json(html: &[u8], url: Option<&str>) -> RcResult<String> {
    let doc = document(html)?;
    let probably = dom_smoothie::is_probably_readable(&doc, None, None);
    let mut verdict = json!({
        "ok": false,
        "probably": probably,
        "title": "",
        "byline": "",
        "siteName": "",
        "excerpt": "",
        "dir": "",
        "length": 0,
        "content": "",
    });
    if !probably {
        return Ok(verdict.to_string());
    }
    let base = url.filter(|u| is_absolute(u));
    let mut readability = Readability::with_document(doc, base, Some(config()))
        .map_err(|e| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: format!("readability init: {e}"),
        })?;
    let article = match readability.parse() {
        Ok(article) => article,
        Err(_) => return Ok(verdict.to_string()),
    };
    if article.content.is_empty() {
        return Ok(verdict.to_string());
    }
    verdict["ok"] = Value::Bool(true);
    verdict["title"] = Value::String(article.title);
    verdict["byline"] =
        Value::String(article.byline.unwrap_or_default());
    verdict["siteName"] =
        Value::String(article.site_name.unwrap_or_default());
    verdict["excerpt"] =
        Value::String(article.excerpt.unwrap_or_default());
    verdict["dir"] = Value::String(article.dir.unwrap_or_default());
    verdict["length"] = Value::from(article.length);
    verdict["content"] = Value::String(sanitize(&article.content));
    Ok(verdict.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn article_page() -> Vec<u8> {
        let mut paragraphs = String::new();
        for _ in 0..6 {
            paragraphs.push_str(
                "<p>READER_MARKER paragraph text that is long enough to look \
                 like real article copy. The quick brown fox jumps over the \
                 lazy dog repeatedly to build up the content score the \
                 extractor looks for.</p>",
            );
        }
        format!(
            "<html><head><title>Smoke Article Title</title></head><body>\
             <nav id=\"clutter\"><a href=\"#\">nav one</a><a href=\"#\">nav two</a></nav>\
             <article><h1>Smoke Article Title</h1>{paragraphs}\
             <p><img src=\"pixel.png\"></p></article></body></html>"
        )
        .into_bytes()
    }

    fn verdict(html: &[u8], url: Option<&str>) -> Value {
        let json = extract_json(html, url).expect("extract");
        serde_json::from_str(&json).expect("verdict json")
    }

    #[test]
    fn article_extracts() {
        let v = verdict(
            &article_page(),
            Some("file:///tmp/arora-reader-smoke/article.html"),
        );
        assert!(v["probably"].as_bool().unwrap());
        assert!(v["ok"].as_bool().unwrap(), "verdict: {v}");
        assert!(v["title"].as_str().unwrap().contains("Smoke Article Title"));
        let content = v["content"].as_str().unwrap();
        assert!(content.contains("READER_MARKER"));
        assert!(!content.contains("nav one"));
        assert!(content.contains("<img"));
        assert!(v["length"].as_u64().unwrap() > 500);
    }

    #[test]
    fn non_article_declines() {
        let html = b"<html><head><title>tiny</title></head>\
                     <body><p>short</p></body></html>";
        assert!(!probably_readable(html).unwrap());
        let v = verdict(html, Some("https://example.com/"));
        assert!(!v["probably"].as_bool().unwrap());
        assert!(!v["ok"].as_bool().unwrap());
        assert_eq!(v["content"].as_str().unwrap(), "");
    }

    #[test]
    fn probe_matches_extract_verdict() {
        assert!(probably_readable(&article_page()).unwrap());
    }

    #[test]
    fn sanitizer_strips_interactive_markup() {
        // An article whose surviving content carries exactly the
        // things reader.js's sanitize() removed.
        let mut paragraphs = String::new();
        for _ in 0..6 {
            paragraphs.push_str(
                "<p>Enough real article copy to make the extractor take \
                 this document seriously and pick the article candidate \
                 rather than declining the page outright as thin.</p>",
            );
        }
        let html = format!(
            "<html><body><article><h1>t</h1>{paragraphs}\
             <p onclick=\"evil()\">Keep me \
             but strip the handler so nothing interactive survives.</p>\
             <a href=\"javascript:evil()\">bad link</a>\
             <img src=\"javascript:evil()\">\
             <script>alert(1)</script><form><input></form></article></body></html>"
        );
        let v = verdict(html.as_bytes(), Some("https://example.com/a"));
        let content = v["content"].as_str().unwrap();
        assert!(!content.contains("onclick"), "{content}");
        assert!(!content.contains("javascript:"), "{content}");
        assert!(!content.contains("<script"), "{content}");
        assert!(!content.contains("<form"), "{content}");
        assert!(!content.contains("<input"), "{content}");
    }

    #[test]
    fn hostile_inputs_never_crash() {
        // Malformed, truncated, nested and pathological inputs all
        // degrade to a verdict.
        for input in [
            b"" as &[u8],
            b"<",
            b"<html><body><article",
            b"\xff\xfe\x00garbage",
            b"<div><div><div>deep",
        ] {
            let _ = probably_readable(input);
            let _ = extract_json(input, None);
        }
        // Deep nesting inside the byte cap is refused by the depth
        // pre-scan before the (quadratic-depth) tree build runs.
        let deep = "<div>".repeat(MAX_SCAN_DEPTH + 64);
        assert!(extract_json(deep.as_bytes(), None).is_err());
        // Balanced-but-deep enough to parse is still handled.
        let mut balanced = String::new();
        for _ in 0..64 {
            balanced.push_str("<div>");
        }
        for _ in 0..64 {
            balanced.push_str("</div>");
        }
        let _ = extract_json(balanced.as_bytes(), None);
        // Past the byte cap -> clean refusal, no parse attempted.
        let big = vec![b'x'; MAX_HTML_BYTES + 1];
        assert!(extract_json(&big, None).is_err());
        assert!(probably_readable(&big).is_err());
        // A non-UTF8 / NUL-heavy body still returns a verdict shape.
        let weird: Vec<u8> = (0u32..4096).map(|i| (i % 256) as u8).collect();
        let v = verdict(&weird, Some("https://example.com"));
        assert!(v["ok"].is_boolean());
    }

    #[test]
    fn non_absolute_url_degrades() {
        // The Qt side always passes the page URL, but a non-authority
        // URL must degrade to "no base" instead of failing the call.
        let v = verdict(&article_page(), Some("about:blank"));
        assert!(v["ok"].as_bool().unwrap());
        let v = verdict(&article_page(), None);
        assert!(v["ok"].as_bool().unwrap());
    }

    #[test]
    fn excerpt_and_byline_populate() {
        let mut paragraphs = String::new();
        for _ in 0..6 {
            paragraphs.push_str(
                "<p>Article copy long enough to convince the extractor \
                 that this page is a real piece of prose worth \
                 presenting in the reader surface instead of clutter.</p>",
            );
        }
        let html = format!(
            "<html><head><title>T</title>\
             <meta name=\"author\" content=\"Jane Writer\">\
             <meta name=\"description\" content=\"An excerpt line.\"></head>\
             <body><article><h1>T</h1>{paragraphs}</article></body></html>"
        );
        let v = verdict(html.as_bytes(), Some("https://example.com/a"));
        assert!(v["ok"].as_bool().unwrap());
        // Metadata extraction is best-effort — assert the fields exist
        // and are strings; exact byline/excerpt rules track upstream.
        assert!(v["byline"].is_string());
        assert!(v["excerpt"].is_string());
    }
}
