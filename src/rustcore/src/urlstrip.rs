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
//!   "scoped": [                            // POL04: params that are
//!     {"hosts": ["amazon.*"],              //   trackers only on the
//!      "params": ["ref_*", "tag"]}         //   listed domains —
//!   ],                                     //   "ref" is functional
//!                                          //   everywhere else
//!   "unwrap": [                            // POL04: redirector rules —
//!     {"hosts": ["google.*"],              //   the named param's value
//!      "path": "/url", "param": "q"}       //   decodes to the real
//!   ],                                     //   destination
//!   "exceptions": [
//!     {"host": "ex.com"},                  // no cleaning on ex.com or *.ex.com
//!     {"host": "a.com", "keep": ["si"]}    // all params stripped except "si"
//!   ]
//! }
//! ```
//! Exceptions exist for sites whose auth/flow breaks when a listed
//! name is removed — respect them, never strip past them.  A full
//! (keep-less) exception also disables unwrapping FOR that host and
//! unwrapping TO it; a keep list governs parameter stripping only.

use serde_json::Value;
use std::io::ErrorKind;
use std::path::Path;
use std::sync::Arc;

use crate::error::{Fail, RcResult, RcStatus};
use crate::store::{data_dir, DirCache};

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

/// Host matcher for scoped/unwrap rules (POL04).  "example.com"
/// matches the host itself or any subdomain; a trailing ".*" is a
/// TLD wildcard — "amazon.*" matches wherever the base appears as a
/// whole label run followed by a dot (amazon.com, www.amazon.co.uk).
/// Deliberately heuristic: no public-suffix list, so "amazon.*" also
/// catches lookalike hosts like amazon.com.evil.example — stripping a
/// known tracker name there is harmless.
#[derive(Debug)]
enum HostPattern {
    Suffix(String),
    TldWildcard(String),
}

impl HostPattern {
    fn compile(rule: &str) -> Option<HostPattern> {
        let rule = rule.trim().to_lowercase();
        if rule.is_empty() {
            return None;
        }
        if let Some(base) = rule.strip_suffix(".*") {
            if base.is_empty() || base.contains('*') {
                return None;
            }
            return Some(HostPattern::TldWildcard(base.to_owned()));
        }
        Some(HostPattern::Suffix(rule))
    }

    fn matches(&self, host: &str) -> bool {
        match self {
            HostPattern::Suffix(s) => {
                host == s.as_str() || host.ends_with(&format!(".{s}"))
            }
            HostPattern::TldWildcard(base) => {
                host.starts_with(&format!("{base}."))
                    || host.contains(&format!(".{base}."))
            }
        }
    }
}

/// POL04: params that are trackers only on the listed domains —
/// "ref" is an Amazon affiliate id but a functional parameter on
/// countless other sites, so it only strips where scoped.
#[derive(Debug)]
struct ScopedParams {
    hosts: Vec<HostPattern>,
    params: Vec<Pattern>,
}

/// POL04: a redirector rule — on a matching host+path the named
/// query parameter's percent-decoded value is the real destination.
/// The whole original URL is replaced by that target (the wrapper's
/// own tracking params — sa, ved, h, s — die with it).
#[derive(Debug)]
struct UnwrapRule {
    hosts: Vec<HostPattern>,
    path: String,
    param: Pattern,
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
    scoped: Vec<ScopedParams>,
    unwrap: Vec<UnwrapRule>,
    exceptions: Vec<Exception>,
}

/// The active ruleset — Arc'd so a strip in flight on the IO thread
/// survives a reload swapping the set under it.
static RULES: DirCache<RuleSet> = DirCache::new();

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
    let parse_hosts = |v: Option<&Value>| -> Vec<HostPattern> {
        v.and_then(Value::as_array)
            .map(|a| {
                a.iter()
                    .filter_map(Value::as_str)
                    .filter_map(HostPattern::compile)
                    .collect()
            })
            .unwrap_or_default()
    };
    let mut scoped = Vec::new();
    if let Some(list) = doc.get("scoped").and_then(Value::as_array) {
        for entry in list {
            let hosts = parse_hosts(entry.get("hosts"));
            let params = parse_patterns(entry.get("params").unwrap_or(&Value::Null));
            if hosts.is_empty() || params.is_empty() {
                continue;
            }
            scoped.push(ScopedParams { hosts, params });
        }
    }
    let mut unwrap = Vec::new();
    if let Some(list) = doc.get("unwrap").and_then(Value::as_array) {
        for entry in list {
            // "hosts" is a list; "host" is a single-value shorthand.
            let mut hosts = parse_hosts(entry.get("hosts"));
            if let Some(single) = entry.get("host").and_then(Value::as_str) {
                hosts.extend(HostPattern::compile(single));
            }
            let param = entry
                .get("param")
                .and_then(Value::as_str)
                .and_then(Pattern::compile);
            let (Some(param), false) = (param, hosts.is_empty()) else {
                continue;
            };
            let path = entry
                .get("path")
                .and_then(Value::as_str)
                .map(|p| p.trim().to_lowercase())
                .unwrap_or_default();
            unwrap.push(UnwrapRule { hosts, path, param });
        }
    }
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
    Ok(RuleSet {
        params,
        scoped,
        unwrap,
        exceptions,
    })
}

/// The loaded ruleset, lazily populated: `<data dir>`/OVERRIDE_FILE
/// wins when present and parses, otherwise the vendored default.  An
/// unparsable override falls back to the builtin so a bad update can
/// never disarm the strip.  The DirCache rebuilds it once when the
/// data dir arrives late (rc_set_data_dir after first use) — the
/// same staleness rule blocklist/tranco apply.
fn active_rules() -> Arc<RuleSet> {
    RULES.get_or_load(|dir| {
        load_from_disk(dir).unwrap_or_else(|_| {
            parse_rules(BUILTIN_RULES)
                .expect("vendored urlstrip rules must parse")
        })
    })
}

fn load_from_disk(dir: Option<&Path>) -> RcResult<RuleSet> {
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
    RULES.replace(parse_rules(json)?);
    // PERF01: the request-policy strip stage reads this ruleset —
    // cached verdicts computed against the old one must die.
    crate::policy::bump_eval_generation();
    Ok(())
}

/// rc_urlstrip_reload: re-run the override-or-builtin selection, e.g.
/// after a rules update lands in the data dir.
pub fn reload() -> RcResult<()> {
    RULES.replace(load_from_disk(data_dir().as_deref())?);
    crate::policy::bump_eval_generation();
    Ok(())
}

/// What the pipeline needs beyond the rewritten URL: `removed`
/// counts the tracker units a cleaning dropped — one per stripped
/// query parameter plus one per adopted unwrap hop — so the shield
/// panel can show an honest "N trackers removed" rather than a bare
/// cleaned/not-cleaned bit.  `unwrapped` marks outcomes where a
/// redirector rule fired (the caller re-checks the new host's
/// policies — the blocklist must still get its say on the target).
#[derive(Debug, PartialEq, Eq)]
pub struct CleanOutcome {
    pub url: String,
    pub removed: usize,
    pub unwrapped: bool,
}

/// The FFI-facing entry: the cleaned URL, or the input verbatim when
/// no rule fired (or the URL is not http/https).  Never fails on
/// content — a malformed URL simply returns unchanged.  Unwrapping is
/// enabled and no dynamic exemptions apply — this is the "what would
/// the cleaned form be" seam, not the request path.
pub fn strip(url: &str) -> String {
    clean_url(&active_rules(), url, true, &|_| false)
        .map(|o| o.url)
        .unwrap_or_else(|| url.to_owned())
}

/// POL04: the request-pipeline entry.  `navigation` gates redirect
/// unwrapping to top-level navigations — a subresource hop may depend
/// on the redirector's own response, while a link click never does.
/// `is_exempt` carries the per-site opt-outs the shield panel learns
/// (an exempt host's URLs pass through untouched, and an exempt
/// unwrap target is never adopted — the redirector hop stands).
pub fn strip_for_request(
    url: &str,
    navigation: bool,
    is_exempt: &dyn Fn(&str) -> bool,
) -> Option<CleanOutcome> {
    clean_url(&active_rules(), url, navigation, is_exempt)
}

/// Bound on chained unwrapping — a real chain like l.facebook.com ->
/// t.co still resolves while a self-referential rule pair cannot
/// loop.
const MAX_UNWRAP_HOPS: usize = 3;

fn clean_url(
    rules: &RuleSet,
    url: &str,
    allow_unwrap: bool,
    is_exempt: &dyn Fn(&str) -> bool,
) -> Option<CleanOutcome> {
    let sep = url.find("://")?;
    let scheme = &url[..sep];
    if !scheme.eq_ignore_ascii_case("http") && !scheme.eq_ignore_ascii_case("https") {
        return None;
    }

    // The fragment swallows everything after '#', so a '?' behind it
    // is not a query — and without a query nothing here can fire.
    let frag = url.find('#');
    let qpos = url.find('?')?;
    if frag.is_some_and(|f| qpos > f) {
        return None;
    }

    let host = url_host(url);
    if is_exempt(&host) {
        return None;
    }
    if let Some(ex) = exception_for(rules, &host) {
        if ex.keep.is_empty() {
            return None; // fully exempt — cleaning breaks this site
        }
    }

    let mut removed = 0usize;
    let mut current = url.to_string();
    if allow_unwrap {
        for _ in 0..MAX_UNWRAP_HOPS {
            let Some(target) = try_unwrap(rules, &current) else {
                break;
            };
            // A per-site-exempt or fully-excepted destination keeps
            // the redirector hop — the user asked for that site's
            // links to be left alone.
            let target_host = url_host(&target);
            if is_exempt(&target_host)
                || exception_for(rules, &target_host)
                    .is_some_and(|ex| ex.keep.is_empty())
            {
                break;
            }
            current = target;
            removed += 1;
        }
    }
    let unwrapped = current != url;

    // Parameter stripping runs under the FINAL host's rules — an
    // unwrapped destination's own keep-list and scoped rules apply.
    let host = url_host(&current);
    let keep: &[Pattern] = exception_for(rules, &host)
        .map(|ex| ex.keep.as_slice())
        .unwrap_or(&[]);
    let cfrag = current.find('#');
    let cqpos = match current.find('?') {
        Some(q) if cfrag.is_none_or(|f| q < f) => q,
        _ => {
            return if !unwrapped {
                None
            } else {
                Some(CleanOutcome {
                    url: current,
                    removed,
                    unwrapped,
                })
            };
        }
    };
    let (stripped, params_removed) =
        strip_query(rules, &current, &host, cqpos, cfrag, keep)
            .map(|(s, n)| (s, n))
            .unwrap_or((current, 0));
    if stripped == url {
        None
    } else {
        Some(CleanOutcome {
            url: stripped,
            removed: removed + params_removed,
            unwrapped,
        })
    }
}

/// The bare host of an http(s) URL: authority minus userinfo/port,
/// IPv6 brackets dropped, lowercased.
fn url_host(url: &str) -> String {
    let auth_start = match url.find("://") {
        Some(i) => i + 3,
        None => return String::new(),
    };
    let auth_end = url[auth_start..]
        .find(['/', '?', '#'])
        .map(|i| auth_start + i)
        .unwrap_or(url.len());
    let authority = &url[auth_start..auth_end];
    let hostport = authority.rsplit('@').next().unwrap_or("");
    if hostport.starts_with('[') {
        hostport
            .find(']')
            .map(|i| hostport[1..i].to_lowercase())
            .unwrap_or_else(|| hostport.to_lowercase())
    } else {
        hostport
            .split(':')
            .next()
            .unwrap_or("")
            .to_lowercase()
    }
}

/// The exception row governing a host, if any (exact or parent
/// suffix match).
fn exception_for<'a>(rules: &'a RuleSet, host: &str) -> Option<&'a Exception> {
    rules
        .exceptions
        .iter()
        .find(|ex| host == ex.host || host.ends_with(&format!(".{}", ex.host)))
}

/// The path portion of an http(s) URL — between the authority and the
/// '?'/'#' boundary — for unwrap-rule matching.
fn url_path(url: &str) -> &str {
    let auth_start = match url.find("://") {
        Some(i) => i + 3,
        None => return "",
    };
    let rest = &url[auth_start..];
    let start = match rest.find('/') {
        Some(i) => auth_start + i,
        None => return "",
    };
    let end = url[start..]
        .find(['?', '#'])
        .map(|i| start + i)
        .unwrap_or(url.len());
    &url[start..end]
}

/// Rule path match: empty or "/" constrains nothing; "/x" matches
/// "/x" and "/x/..."; a trailing-slash rule is a strict prefix.
fn path_matches(path: &str, rule_path: &str) -> bool {
    let prefix = rule_path.trim_end_matches('/');
    if prefix.is_empty() {
        return true;
    }
    path == prefix
        || (rule_path.ends_with('/') && path.starts_with(rule_path))
        || (path.starts_with(prefix)
            && path.as_bytes().get(prefix.len()) == Some(&b'/'))
}

/// Absolute http(s) URL without whitespace/control bytes — anything
/// else is not a safe redirect target.
fn is_web_target(s: &str) -> bool {
    (s.get(..7).is_some_and(|p| p.eq_ignore_ascii_case("http://"))
        || s.get(..8).is_some_and(|p| p.eq_ignore_ascii_case("https://")))
        && !s.bytes().any(|b| b <= b' ')
}

/// POL04: redirect unwrapping — when a rule's host+path match and the
/// named param percent-decodes (once, or twice for double-encoded
/// wrappers) to an absolute http(s) URL, that URL replaces the whole
/// request.  The first matching rule wins.
fn try_unwrap(rules: &RuleSet, url: &str) -> Option<String> {
    if rules.unwrap.is_empty() {
        return None;
    }
    let host = url_host(url);
    if host.is_empty() {
        return None;
    }
    let path = url_path(url);
    let frag = url.find('#');
    let qpos = url.find('?')?;
    if frag.is_some_and(|f| qpos > f) {
        return None;
    }
    let query = &url[qpos + 1..frag.unwrap_or(url.len())];
    for rule in &rules.unwrap {
        if !rule.hosts.iter().any(|h| h.matches(&host)) {
            continue;
        }
        if !path_matches(path, &rule.path) {
            continue;
        }
        for segment in query.split('&') {
            let Some((raw_name, raw_value)) = segment.split_once('=') else {
                continue;
            };
            if !rule.param.matches(&decode_name(raw_name).to_lowercase()) {
                continue;
            }
            let decoded = decode_value(raw_value);
            if is_web_target(&decoded) {
                return Some(decoded);
            }
            // Double-encoded wrappers (safelinks & co. nest a second
            // percent pass) — one more decode, still http(s) only.
            let twice = decode_value(&decoded);
            if twice != decoded && is_web_target(&twice) {
                return Some(twice);
            }
        }
    }
    None
}

/// Percent-decode for an embedded URL VALUE: '%XX' only — '+' is a
/// literal plus inside a nested URL, not a space.
fn decode_value(raw: &str) -> String {
    let bytes = raw.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    while i < bytes.len() {
        match bytes[i] {
            b'%' if i + 2 < bytes.len() => {
                match u8::from_str_radix(&raw[i + 1..i + 3], 16) {
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
            b => {
                out.push(b);
                i += 1;
            }
        }
    }
    String::from_utf8_lossy(&out).into_owned()
}

fn strip_query(
    rules: &RuleSet,
    url: &str,
    host: &str,
    qpos: usize,
    frag: Option<usize>,
    keep: &[Pattern],
) -> Option<(String, usize)> {
    let qend = frag.unwrap_or(url.len());
    let query = &url[qpos + 1..qend];
    let mut kept: Vec<&str> = Vec::with_capacity(query.len() / 8 + 1);
    let mut dropped = 0usize;
    for segment in query.split('&') {
        let raw_name = segment.split('=').next().unwrap_or("");
        let name = decode_name(raw_name).to_lowercase();
        // Global params strip everywhere; scoped params only on the
        // domains where they are trackers; a keep list wins over both.
        let strippable = (rules.params.iter().any(|p| p.matches(&name))
            || rules.scoped.iter().any(|s| {
                s.hosts.iter().any(|h| h.matches(host))
                    && s.params.iter().any(|p| p.matches(&name))
            }))
            && !keep.iter().any(|p| p.matches(&name));
        if strippable {
            dropped += 1;
        } else {
            kept.push(segment);
        }
    }
    if dropped == 0 {
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
    Some((out, dropped))
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
    use std::sync::MutexGuard;

    // The active ruleset is process-global — serialize on the shared
    // store lock (sitedecisions showed private per-module locks let a
    // neighbour's set_data_dir move the store mid-test).
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn rules() -> RuleSet {
        parse_rules(
            r#"{"version":1,"params":["utm_*","gclid","si"],
               "scoped":[{"hosts":["mega.*"],"params":["ref_*","tag"]}],
               "unwrap":[{"hosts":["redir.example"],"path":"/go","param":"u"},
                         {"host":"hop.example","path":"/","param":"url"}],
               "exceptions":[{"host":"exempt.example"},
                             {"host":"keep.example","keep":["si"]}]}"#,
        )
        .unwrap()
    }

    /// The strip helper older tests were written against — cleans
    /// with unwrapping on and no dynamic exemptions.
    fn strip_url(r: &RuleSet, url: &str) -> Option<String> {
        clean_url(r, url, true, &|_| false).map(|o| o.url)
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
    fn scoped_params_strip_only_where_listed() {
        let r = rules();
        // On a scoped domain the param is a tracker.
        assert_eq!(
            strip_url(&r, "https://mega.com/p?ref_=aff&utm_source=x&id=9")
                .as_deref(),
            Some("https://mega.com/p?id=9")
        );
        // TLD wildcard reaches www + multi-level suffixes.
        assert_eq!(
            strip_url(&r, "https://www.mega.co.uk/p?tag=t&id=9").as_deref(),
            Some("https://www.mega.co.uk/p?id=9")
        );
        // Everywhere else the same name is functional — untouched.
        assert_eq!(strip_url(&r, "https://plain.example/p?ref_=aff&id=9"), None);
        // Lookalike hosts must not inherit the scope.
        assert_eq!(strip_url(&r, "https://notmega.com/p?tag=t"), None);
    }

    #[test]
    fn unwrap_rules_resolve_redirectors() {
        let r = rules();
        let out = clean_url(
            &r,
            "https://redir.example/go?u=https%3A%2F%2Freal.example%2Fp%3Fx%3D1&sa=t",
            true,
            &|_| false,
        )
        .unwrap();
        assert_eq!(out.url, "https://real.example/p?x=1");
        assert!(out.unwrapped);
        assert_eq!(out.removed, 1); // one hop adopted

        // The destination's own trackers still strip after the hop.
        let out = clean_url(
            &r,
            "https://redir.example/go?u=https%3A%2F%2Freal.example%2Fp%3Futm_source%3Dn%26x%3D1",
            true,
            &|_| false,
        )
        .unwrap();
        assert_eq!(out.url, "https://real.example/p?x=1");
        assert_eq!(out.removed, 2); // hop + stripped param

        // Double-encoded wrappers resolve through a second pass.
        let out = clean_url(
            &r,
            "https://redir.example/go?u=https%253A%252F%252Freal.example%252F",
            true,
            &|_| false,
        )
        .unwrap();
        assert_eq!(out.url, "https://real.example/");

        // A non-URL value and a non-web scheme are never adopted.
        assert_eq!(strip_url(&r, "https://redir.example/go?u=notaurl"), None);
        assert_eq!(
            strip_url(&r, "https://redir.example/go?u=javascript%3Aalert(1)"),
            None
        );

        // Path mismatch or wrong host -> the rule does not fire.
        assert_eq!(
            strip_url(&r, "https://redir.example/other?u=https%3A%2F%2Fx.example%2F"),
            None
        );
        assert_eq!(
            strip_url(&r, "https://evil.example/go?u=https%3A%2F%2Fx.example%2F"),
            None
        );
    }

    #[test]
    fn unwrap_is_navigation_only() {
        let r = rules();
        // A subresource request to a redirector keeps its wrapper —
        // the hop's response may carry content the page needs.
        assert_eq!(
            clean_url(
                &r,
                "https://redir.example/go?u=https%3A%2F%2Freal.example%2F",
                false,
                &|_| false,
            ),
            None
        );
        assert!(clean_url(
            &r,
            "https://redir.example/go?u=https%3A%2F%2Freal.example%2F",
            true,
            &|_| false,
        )
        .is_some());
    }

    #[test]
    fn exemptions_and_per_site_optouts() {
        let r = rules();
        // Ruleset exception: the redirector is fully exempt -> stays.
        let r2 = parse_rules(
            r#"{"version":1,"unwrap":[{"hosts":["redir.example"],"path":"/go","param":"u"}],
                "exceptions":[{"host":"redir.example"}]}"#,
        )
        .unwrap();
        assert_eq!(
            strip_url(&r2, "https://redir.example/go?u=https%3A%2F%2Fx.example%2F"),
            None
        );

        // Per-site opt-out on the request host leaves it dirty.
        assert_eq!(
            clean_url(
                &r,
                "https://dirty.example/?utm_source=x",
                true,
                &|h| h == "dirty.example",
            ),
            None
        );

        // A per-site-exempt unwrap TARGET keeps the redirector hop.
        assert_eq!(
            clean_url(
                &r,
                "https://redir.example/go?u=https%3A%2F%2Fexempt-target.example%2F",
                true,
                &|h| h == "exempt-target.example",
            ),
            None
        );

        // A chained redirector resolves hop by hop (bound-tested by
        // the self-loop rule never regressing below).
        let r3 = parse_rules(
            r#"{"version":1,"unwrap":[
                 {"hosts":["a.example"],"path":"/r","param":"u"},
                 {"hosts":["b.example"],"path":"/r","param":"u"}]}"#,
        )
        .unwrap();
        let out = clean_url(
            &r3,
            "https://a.example/r?u=https%3A%2F%2Fb.example%2Fr%3Fu%3Dhttps%253A%252F%252Fend.example%252F",
            true,
            &|_| false,
        )
        .unwrap();
        assert_eq!(out.url, "https://end.example/");
        assert_eq!(out.removed, 2);

        // An adversarially deep nest cannot loop forever — the hop
        // bound caps the chain and returns the deepest target found.
        let r4 = parse_rules(
            r#"{"version":1,"unwrap":[
                 {"hosts":["loop.example"],"path":"/r","param":"u"}]}"#,
        )
        .unwrap();
        // Percent-encode every non-unreserved byte — one nesting level
        // per iteration.
        let enc = |s: &str| -> String {
            s.bytes()
                .map(|b| {
                    if b.is_ascii_alphanumeric()
                        || matches!(b, b'.' | b'-' | b'_' | b'~')
                    {
                        (b as char).to_string()
                    } else {
                        format!("%{b:02X}")
                    }
                })
                .collect()
        };
        let mut deep = String::from("https://end.example/");
        for _ in 0..10 {
            deep = format!("https://loop.example/r?u={}", enc(&deep));
        }
        let out = clean_url(&r4, &deep, true, &|_| false).unwrap();
        assert_eq!(out.removed, MAX_UNWRAP_HOPS);
        assert!(out.unwrapped);
        assert!(out.url.starts_with("https://loop.example/r"));
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
