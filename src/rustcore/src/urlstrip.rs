//! SEC17: ClearURLs-style tracking-parameter stripping.
//!
//! A query-parameter ruleset (vendored JSON, overridable by
//! `<data dir>/urlstrip-rules.json` like a filter list) drives a pure
//! string-surgery strip: kept segments pass through byte-for-byte, so
//! percent-encoding and duplicate ordering are preserved and only the
//! matched `name=value` pairs leave the URL.  All matching is
//! memory-safe Rust over attacker-controlled URLs — nothing here can
//! reach Qt's parser with a malformed descriptor.
//!
//! Ruleset format (data/urlstrip-rules.json):
//! ```json
//! {
//!   "version": 1,
//!   "params": ["utm_*", "gclid"],          // trailing '*' = prefix
//!   "exceptions": [
//!     {"host": "ex.com"},                  // no stripping on ex.com or *.ex.com
//!     {"host": "a.com", "keep": ["si"]}    // all params stripped except "si"
//!   ]
//! }
//! ```
//! Exceptions exist for sites whose auth/flow breaks when a listed
//! name is removed — respect them, never strip past them.

use serde_json::Value;
use std::io::ErrorKind;
use std::sync::{Arc, RwLock};

use crate::error::{Fail, RcResult, RcStatus};

/// Updated-rules drop point inside the data dir — a list refresh
/// writes this file and calls rc_urlstrip_reload().
const OVERRIDE_FILE: &str = "urlstrip-rules.json";

/// Vendored default, compiled in so the feature works with no data
/// dir or network at all.  Edits here ride the same review as code.
const BUILTIN_RULES: &str = include_str!("../data/urlstrip-rules.json");

/// Query-parameter name matcher.  A trailing `*` in the rule text is
/// a prefix match ("utm_*" catches utm_source, utm_anything); anything
/// else is an exact name so "si" cannot eat "simple".
#[derive(Debug)]
enum Pattern {
    Exact(String),
    Prefix(String),
}

impl Pattern {
    fn compile(rule: &str) -> Option<Pattern> {
        let rule = rule.trim().to_lowercase();
        if rule.is_empty() {
            return None;
        }
        Some(match rule.strip_suffix('*') {
            Some(prefix) => Pattern::Prefix(prefix.to_owned()),
            None => Pattern::Exact(rule),
        })
    }

    fn matches(&self, name: &str) -> bool {
        match self {
            Pattern::Exact(n) => name == n,
            Pattern::Prefix(p) => name.starts_with(p.as_str()),
        }
    }
}

/// Per-site carve-out: an empty keep list exempts the host from
/// stripping entirely; a non-empty list names params that survive
/// while the rest still strip.
#[derive(Debug)]
struct Exception {
    host: String,
    keep: Vec<Pattern>,
}

#[derive(Debug)]
pub struct RuleSet {
    params: Vec<Pattern>,
    exceptions: Vec<Exception>,
}

/// The active ruleset — Arc'd so a strip in flight on the IO thread
/// survives a reload swapping the set under it.
static RULES: RwLock<Option<Arc<RuleSet>>> = RwLock::new(None);

fn rules_slot() -> std::sync::RwLockReadGuard<'static, Option<Arc<RuleSet>>> {
    RULES.read().unwrap_or_else(|e| e.into_inner())
}

fn rules_slot_mut() -> std::sync::RwLockWriteGuard<'static, Option<Arc<RuleSet>>> {
    RULES.write().unwrap_or_else(|e| e.into_inner())
}

fn parse_patterns(v: &Value) -> Vec<Pattern> {
    v.as_array()
        .map(|a| a.iter().filter_map(Value::as_str).filter_map(Pattern::compile).collect())
        .unwrap_or_default()
}

fn parse_rules(text: &str) -> RcResult<RuleSet> {
    let bad = |msg: String| Fail {
        status: RcStatus::Corrupt,
        msg,
    };
    let doc: Value = serde_json::from_str(text)
        .map_err(|e| bad(format!("urlstrip rules: {e}")))?;
    if let Some(version) = doc.get("version") {
        // A newer schema must fail loudly rather than be half-read —
        // the updater keeps the old file until the core understands it.
        if version.as_i64() != Some(1) {
            return Err(bad(format!(
                "urlstrip rules: unsupported version {version}"
            )));
        }
    }
    let params = parse_patterns(doc.get("params").unwrap_or(&Value::Null));
    let mut exceptions = Vec::new();
    if let Some(list) = doc.get("exceptions").and_then(Value::as_array) {
        for ex in list {
            let host = ex
                .get("host")
                .and_then(Value::as_str)
                .map(|h| h.trim().to_lowercase())
                .unwrap_or_default();
            if host.is_empty() {
                continue;
            }
            let keep = ex
                .get("keep")
                .map(parse_patterns)
                .unwrap_or_default();
            exceptions.push(Exception { host, keep });
        }
    }
    Ok(RuleSet { params, exceptions })
}

/// The loaded ruleset, lazily populated: `<data dir>`/OVERRIDE_FILE
/// wins when present and parses, otherwise the vendored default.  An
/// unparsable override falls back to the builtin so a bad update can
/// never disarm the strip.
fn active_rules() -> Arc<RuleSet> {
    if let Some(rules) = rules_slot().as_ref() {
        return rules.clone();
    }
    let rules = Arc::new(load_from_disk().unwrap_or_else(|_| {
        parse_rules(BUILTIN_RULES).expect("vendored urlstrip rules must parse")
    }));
    *rules_slot_mut() = Some(rules.clone());
    rules
}

fn load_from_disk() -> RcResult<RuleSet> {
    let dir = crate::store::lock().dir().ok().map(|d| d.to_path_buf());
    if let Some(dir) = dir {
        match std::fs::read_to_string(dir.join(OVERRIDE_FILE)) {
            Ok(text) => return parse_rules(&text),
            Err(e) if e.kind() == ErrorKind::NotFound => {}
            Err(_) => {} // unreadable override — the builtin still strips
        }
    }
    parse_rules(BUILTIN_RULES)
}

/// rc_urlstrip_load_rules: swap in a caller-supplied JSON ruleset.
/// A malformed document keeps the previous set (RC_CORRUPT).
pub fn load_rules(json: &str) -> RcResult<()> {
    let rules = Arc::new(parse_rules(json)?);
    *rules_slot_mut() = Some(rules);
    Ok(())
}

/// rc_urlstrip_reload: re-run the override-or-builtin selection, e.g.
/// after a rules update lands in the data dir.
pub fn reload() -> RcResult<()> {
    let rules = Arc::new(load_from_disk()?);
    *rules_slot_mut() = Some(rules);
    Ok(())
}

/// The FFI-facing entry: the cleaned URL, or the input verbatim when
/// no rule fired (or the URL is not http/https).  Never fails on
/// content — a malformed URL simply returns unchanged.
pub fn strip(url: &str) -> String {
    strip_url(&active_rules(), url).unwrap_or_else(|| url.to_owned())
}

/// Returns Some(cleaned) when at least one query parameter matched
/// the ruleset, None when the URL is untouched.
fn strip_url(rules: &RuleSet, url: &str) -> Option<String> {
    let sep = url.find("://")?;
    let scheme = &url[..sep];
    if !scheme.eq_ignore_ascii_case("http") && !scheme.eq_ignore_ascii_case("https") {
        return None;
    }

    // The fragment swallows everything after '#', so a '?' behind it
    // is not a query.
    let frag = url.find('#');
    let qpos = url.find('?')?;
    if frag.is_some_and(|f| qpos > f) {
        return None;
    }

    // Exceptions key on the bare host: authority minus userinfo/port.
    let auth_start = sep + 3;
    let auth_end = url[auth_start..]
        .find(['/', '?', '#'])
        .map(|i| auth_start + i)
        .unwrap_or(url.len());
    let authority = &url[auth_start..auth_end];
    let hostport = authority.rsplit('@').next().unwrap_or("");
    let host = if hostport.starts_with('[') {
        // IPv6 literal — keep the brackets out of the comparison.
        hostport
            .find(']')
            .map(|i| &hostport[1..i])
            .unwrap_or(hostport)
    } else {
        hostport.split(':').next().unwrap_or("")
    }
    .to_lowercase();

    if let Some(ex) = rules
        .exceptions
        .iter()
        .find(|ex| host == ex.host || host.ends_with(&format!(".{}", ex.host)))
    {
        if ex.keep.is_empty() {
            return None; // fully exempt — stripping breaks this site
        }
        return strip_with_keep(rules, url, qpos, frag, &ex.keep);
    }
    strip_with_keep(rules, url, qpos, frag, &[])
}

fn strip_with_keep(
    rules: &RuleSet,
    url: &str,
    qpos: usize,
    frag: Option<usize>,
    keep: &[Pattern],
) -> Option<String> {
    let qend = frag.unwrap_or(url.len());
    let query = &url[qpos + 1..qend];
    let mut kept: Vec<&str> = Vec::with_capacity(query.len() / 8 + 1);
    let mut removed = false;
    for segment in query.split('&') {
        let raw_name = segment.split('=').next().unwrap_or("");
        let name = decode_name(raw_name).to_lowercase();
        let strippable = rules.params.iter().any(|p| p.matches(&name))
            && !keep.iter().any(|p| p.matches(&name));
        if strippable {
            removed = true;
        } else {
            kept.push(segment);
        }
    }
    if !removed {
        return None;
    }
    let tail = frag.map(|f| &url[f..]).unwrap_or("");
    let mut out = String::with_capacity(url.len());
    out.push_str(&url[..qpos]);
    if !kept.is_empty() {
        out.push('?');
        out.push_str(&kept.join("&"));
    }
    out.push_str(tail);
    Some(out)
}

/// Percent-decode a raw query name for matching (`+` folds to space
/// like form decoding).  Undecodable sequences pass through — they
/// simply match nothing.
fn decode_name(raw: &str) -> String {
    let bytes = raw.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' if i + 2 < bytes.len() => {
                let hex = &raw[i + 1..i + 3];
                match u8::from_str_radix(hex, 16) {
                    Ok(v) => {
                        out.push(v);
                        i += 3;
                    }
                    Err(_) => {
                        out.push(b'%');
                        i += 1;
                    }
                }
            }
            b'+' => {
                out.push(b' ');
                i += 1;
            }
            b => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Mutex, MutexGuard};

    // The active ruleset is process-global — serialize like the store
    // tests do.
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn guard() -> MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn rules() -> RuleSet {
        parse_rules(
            r#"{"version":1,"params":["utm_*","gclid","si"],
               "exceptions":[{"host":"exempt.example"},
                             {"host":"keep.example","keep":["si"]}]}"#,
        )
        .unwrap()
    }

    #[test]
    fn strips_and_preserves() {
        let r = rules();
        assert_eq!(
            strip_url(&r, "https://a.com/?utm_source=x&real=1").as_deref(),
            Some("https://a.com/?real=1")
        );
        assert_eq!(
            strip_url(&r, "https://a.com/p?utm_source=x&utm_medium=y").as_deref(),
            Some("https://a.com/p")
        );
        assert_eq!(
            strip_url(&r, "https://a.com/?b=1&utm_source=x&a=2#f").as_deref(),
            Some("https://a.com/?b=1&a=2#f")
        );
        // '?' inside the fragment is not a query.
        assert_eq!(strip_url(&r, "https://a.com/#frag?utm_source=x"), None);
        assert_eq!(strip_url(&r, "https://a.com/?real=1"), None);
        assert_eq!(strip_url(&r, "https://a.com/"), None);
        // non-http schemes pass through
        assert_eq!(strip_url(&r, "ftp://a.com/f?utm_source=x"), None);
        assert_eq!(strip_url(&r, "data:text/plain,utm_source=x"), None);
        // case-insensitive name match; prefix does not overreach
        assert_eq!(
            strip_url(&r, "https://a.com/?UTM_SOURCE=x&utmx=y&si=1&simple=2")
                .as_deref(),
            Some("https://a.com/?utmx=y&simple=2")
        );
        // encoded name still matches; value encoding preserved
        assert_eq!(
            strip_url(&r, "https://a.com/?utm%5Fsource=x&q=a%20b").as_deref(),
            Some("https://a.com/?q=a%20b")
        );
        // port + userinfo preserved
        assert_eq!(
            strip_url(&r, "https://u:p@a.com:8443/?gclid=1&ok=1").as_deref(),
            Some("https://u:p@a.com:8443/?ok=1")
        );
    }

    #[test]
    fn exceptions() {
        let r = rules();
        // exact + subdomain exemption
        assert_eq!(strip_url(&r, "https://exempt.example/?utm_source=x"), None);
        assert_eq!(strip_url(&r, "https://a.exempt.example/?utm_source=x"), None);
        // suffix inside another host name must not match
        assert_eq!(
            strip_url(&r, "https://notexempt.example/?utm_source=x").as_deref(),
            Some("https://notexempt.example/")
        );
        // keep-list: named params survive, others strip
        assert_eq!(
            strip_url(&r, "https://keep.example/?si=1&utm_source=x").as_deref(),
            Some("https://keep.example/?si=1")
        );
    }

    #[test]
    fn malformed_rules_rejected() {
        assert!(parse_rules("not json").is_err());
        assert!(parse_rules("{}").is_ok()); // empty ruleset strips nothing
        assert!(parse_rules(r#"{"version":2,"params":["x"]}"#).is_err());
    }

    #[test]
    fn builtin_vendored_rules_parse() {
        let r = parse_rules(BUILTIN_RULES).unwrap();
        assert!(!r.params.is_empty());
        assert_eq!(
            strip_url(&r, "https://e.com/?fbclid=f&utm_source=x&real=1")
                .as_deref(),
            Some("https://e.com/?real=1")
        );
    }

    #[test]
    fn ffi_level_strip_and_load() {
        let _g = guard();
        load_rules(r#"{"version":1,"params":["test_*"]}"#).unwrap();
        assert_eq!(
            strip("https://a.com/?test_x=1&keep=2"),
            "https://a.com/?keep=2"
        );
        assert_eq!(strip("https://a.com/?keep=2"), "https://a.com/?keep=2");
        // bad load keeps the previous ruleset
        assert!(load_rules("garbage").is_err());
        assert_eq!(
            strip("https://a.com/?test_x=1"),
            "https://a.com/"
        );
        // reload with no data dir -> vendored builtin
        reload().unwrap();
        assert_eq!(
            strip("https://a.com/?test_x=1&utm_source=u"),
            "https://a.com/?test_x=1"
        );
    }
}
