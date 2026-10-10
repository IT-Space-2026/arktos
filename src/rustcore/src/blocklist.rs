//! SEC18: local anti-phishing/malware domain blocklist — the
//! poor-man's Safe Browsing: every answer comes from a shipped,
//! updatable list, so no URL or hash ever leaves the machine for a
//! lookup.
//!
//! The list is a plain-text file of domains.  Parsing is deliberately
//! tolerant so the upstream feeds drop in unchanged:
//!   * `<domain>`                        — plain domain per line
//!   * `<ip> <domain>`                   — hostfile rows (URLhaus)
//!   * `scheme://<domain>[/path...]`     — URL rows (OpenPhish feed)
//!   * `||<domain>^` / `*.<domain>`      — adblock-style entries
//!   * `#`, `!`, `;`, `//`, `[` lines    — comments/section junk
//!
//! Matching is exact-host + suffix: a listed `evil.example` blocks
//! `evil.example` and every subdomain; it cannot be dodged by
//! `not` + `evil.example` and never reaches up (a listed
//! `www.bad.example` does not block `bad.example`).  The set is a
//! HashSet probed once per label — O(host labels), no trie needed.
//!
//! Data sources (the updater merges both into
//! `<data dir>/blocklist-domains.txt`):
//!   * https://urlhaus.abuse.ch/downloads/hostfile/   (malware)
//!   * https://openphish.com/feed.txt                  (phishing)
//! The vendored data/blocklist-domains.txt is the offline seed —
//! updates merge INTO the builtin set rather than replacing it, so a
//! partial or poisoned update can only add entries, never quietly
//! unblock a seed domain.

use std::collections::HashSet;
use std::io::ErrorKind;
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::{Arc, RwLock};

use crate::error::{fail, Fail, RcResult, RcStatus};

/// Updated-list drop point inside the data dir — the Qt-side
/// DomainBlocklist updater writes this file and calls
/// rc_blocklist_reload().
const OVERRIDE_FILE: &str = "blocklist-domains.txt";

/// Vendored seed, compiled in so blocking works with no data dir and
/// no network at all.
const BUILTIN_LIST: &str = include_str!("../data/blocklist-domains.txt");

/// Bound on parsed entries (URLhaus alone is ~thousands; feeds growing
/// past this are almost certainly hostile or corrupt) and on a single
/// line.
const MAX_ENTRIES: usize = 1_000_000;
const MAX_LINE: usize = 8192;

/// The loaded list.  `dir` is the data dir the entries were resolved
/// against — the lazy loader rebuilds when the effective dir changes,
/// which is how a store that appears after first use (rc_set_data_dir
/// arriving late) still picks up its override file.
#[derive(Debug)]
pub struct BlockList {
    domains: HashSet<String>,
    dir: Option<PathBuf>,
}

/// The active list — Arc'd so a check in flight on the IO thread
/// survives a reload swapping the set under it.
static LIST: RwLock<Option<Arc<BlockList>>> = RwLock::new(None);

fn list_slot() -> std::sync::RwLockReadGuard<'static, Option<Arc<BlockList>>> {
    LIST.read().unwrap_or_else(|e| e.into_inner())
}

fn list_slot_mut() -> std::sync::RwLockWriteGuard<'static, Option<Arc<BlockList>>> {
    LIST.write().unwrap_or_else(|e| e.into_inner())
}

fn current_dir() -> Option<PathBuf> {
    crate::store::lock().dir().ok().map(|d| d.to_path_buf())
}

/// One domain entry out of one line, or None for comments/junk.
/// Handles every upstream row shape listed in the module docs.
fn entry_from_line(line: &str) -> Option<String> {
    let line = line.trim();
    if line.len() > MAX_LINE || line.is_empty() {
        return None;
    }
    match line.as_bytes()[0] {
        b'#' | b'!' | b';' | b'[' | b'/' => return None,
        _ => {}
    }
    let mut fields = line.split_whitespace();
    let first = fields.next()?;
    // Hostfile rows lead with an IP literal — the domain is field two.
    // (URLhaus ships "127.0.0.1\t<host>"; loopback/broadcast entries
    // like "localhost" simply fail validation below.)  A lone IP on
    // its own line is itself the entry — feeds do list bare
    // malicious hosts.
    let token = if first.parse::<IpAddr>().is_ok() {
        match fields.next() {
            Some(second) => second,
            None => first,
        }
    } else {
        first
    };
    domain_from_token(token)
}

/// Normalizes one candidate token to a lowercase bare domain.
/// Returns None when the token cannot be a DNS hostname (paths,
/// emails, wildcards mid-string, IP literals, single labels).
fn domain_from_token(token: &str) -> Option<String> {
    let mut t = token;
    // adblock-ish decorations: "||dom^" and "*.dom".
    if let Some(rest) = t.strip_prefix("||") {
        t = rest;
    }
    if let Some(rest) = t.strip_prefix("*.") {
        t = rest;
    }
    if let Some(rest) = t.strip_suffix('^') {
        t = rest;
    }
    // URL rows (OpenPhish): keep only the authority's host.
    if let Some(pos) = t.find("://") {
        let after = &t[pos + 3..];
        let end = after
            .find(['/', '?', '#'])
            .unwrap_or(after.len());
        t = &after[..end];
        // userinfo may precede the host.
        t = t.rsplit('@').next().unwrap_or("");
    }
    // Strip an optional :port (an IPv6 literal keeps its brackets).
    if t.starts_with('[') {
        t = t
            .find(']')
            .map(|i| &t[1..i])
            .unwrap_or(t);
    } else {
        t = t.split(':').next().unwrap_or("");
    }
    let host = t.trim_end_matches('.').to_lowercase();
    validate_domain(&host).then(|| host)
}

/// Conservative DNS-name gate: one dot minimum (no TLD or localhost
/// entries), labels of [a-z0-9_-] only, sane lengths.  IP literals
/// pass the shape test and are stored — they only ever exact-match an
/// IP-literal request host, so a listed "1.2.3.4" cannot catch
/// "x.1.2.3.4" (see contains()).
fn validate_domain(host: &str) -> bool {
    if host.len() > 253 || !host.contains('.') {
        return false;
    }
    host.split('.').all(|label| {
        !label.is_empty()
            && label.len() <= 63
            && label
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    })
}

fn parse_entries(text: &str) -> HashSet<String> {
    let mut domains = HashSet::new();
    for line in text.lines() {
        if domains.len() >= MAX_ENTRIES {
            break;
        }
        if let Some(entry) = entry_from_line(line) {
            domains.insert(entry);
        }
    }
    domains
}

impl BlockList {
    /// Exact + suffix membership: `host` is blocked when it equals a
    /// listed entry or sits beneath one.  Probing walks the label
    /// boundary left-to-right, so a listed domain can never match a
    /// merely-overlapping name ("notevil.example" is safe beside
    /// "evil.example").
    pub fn contains(&self, host: &str) -> bool {
        let mut h = host.trim().trim_end_matches('.').to_lowercase();
        // QUrl::host() reports IPv6 literals bracketed.
        if h.starts_with('[') && h.ends_with(']') {
            h = h[1..h.len() - 1].to_owned();
        }
        // An IP-literal query can only exact-match — suffix-walking
        // it would probe junk like "0.113.66".
        if h.parse::<IpAddr>().is_ok() {
            return self.domains.contains(h.as_str());
        }
        let mut candidate = h.as_str();
        loop {
            // Suffix candidates that parse as IP literals are skipped:
            // an IP entry can only ever equal a full request host,
            // so "www.203.0.113.66" must not trip "203.0.113.66".
            if candidate.parse::<IpAddr>().is_err()
                && self.domains.contains(candidate)
            {
                return true;
            }
            match candidate.find('.') {
                Some(i) => candidate = &candidate[i + 1..],
                None => return false,
            }
        }
    }

    pub fn len(&self) -> usize {
        self.domains.len()
    }
}

/// Builtin ∪ override-if-present.  The union is deliberate: an update
/// adds coverage on top of the seed, so a truncated or malicious
/// update can only ever block MORE, never unblock.
fn load_from_disk() -> BlockList {
    let dir = current_dir();
    let mut domains = parse_entries(BUILTIN_LIST);
    if let Some(dir) = &dir {
        match std::fs::read_to_string(dir.join(OVERRIDE_FILE)) {
            Ok(text) => domains.extend(parse_entries(&text)),
            Err(e) if e.kind() == ErrorKind::NotFound => {}
            Err(_) => {} // unreadable override — the seed still blocks
        }
    }
    BlockList { domains, dir }
}

/// The loaded list, lazily populated.  A cached set resolved before
/// the data dir existed is rebuilt once the dir appears — the store
/// can arrive after first use (rc_set_data_dir runs whenever the
/// SecureStore shim or the updater first touches the core).
fn active_list() -> Arc<BlockList> {
    let dir = current_dir();
    {
        let slot = list_slot();
        if let Some(list) = slot.as_ref() {
            if list.dir == dir {
                return list.clone();
            }
        }
    }
    let list = Arc::new(load_from_disk());
    *list_slot_mut() = Some(list.clone());
    list
}

/// rc_blocklist_load: swap in a caller-supplied list body
/// (update/test seam).  A body yielding no usable entries is
/// RC_CORRUPT and the previous list stays active.
pub fn load(text: &str) -> RcResult<()> {
    let domains = parse_entries(text);
    if domains.is_empty() {
        return fail(
            RcStatus::Corrupt,
            "blocklist: no usable domain entries",
        );
    }
    *list_slot_mut() = Some(Arc::new(BlockList {
        domains,
        dir: current_dir(),
    }));
    Ok(())
}

/// rc_blocklist_reload: re-run the builtin∪override merge, e.g. after
/// a list update lands in the data dir.  RC_CORRUPT when an override
/// file exists but yields zero entries — the previous list stays
/// active.
pub fn reload() -> RcResult<()> {
    if let Some(dir) = current_dir() {
        if let Ok(text) = std::fs::read_to_string(dir.join(OVERRIDE_FILE)) {
            if parse_entries(&text).is_empty() {
                return Err(Fail {
                    status: RcStatus::Corrupt,
                    msg: "blocklist: override yields no entries".into(),
                });
            }
        }
    }
    *list_slot_mut() = Some(Arc::new(load_from_disk()));
    Ok(())
}

/// rc_blocklist_check: FFI-facing membership test.  Never fails on
/// content — a malformed host simply matches nothing.
pub fn check(host: &str) -> bool {
    active_list().contains(host)
}

/// Entry count of the active list — diagnostics/tests.
pub fn count() -> usize {
    active_list().len()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::MutexGuard;

    // The active list is process-global — serialize on the shared
    // store lock (sitedecisions showed private per-module locks let a
    // neighbour's set_data_dir move the store mid-test).
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn list(text: &str) -> BlockList {
        BlockList {
            domains: parse_entries(text),
            dir: None,
        }
    }

    #[test]
    fn parses_every_row_shape() {
        let l = list(
            "# comment\n\
             ! comment\n\
             ; comment\n\
             // comment\n\
             [section]\n\
             \n\
             plain.example\n\
             127.0.0.1\thostfile.example\n\
             0.0.0.0 hostfile2.example\n\
             https://urlrow.example/some/path?x=1#f\n\
             http://user:pw@authurl.example:8080/p\n\
             ||adblock.example^\n\
             *.wild.example\n\
             127.0.0.1\tlocalhost\n\
             garbage !@#$%\n\
             not-a-domain\n\
             a.b.c.d.e.multi.example\n",
        );
        assert_eq!(l.len(), 8);
        for host in [
            "plain.example",
            "hostfile.example",
            "hostfile2.example",
            "urlrow.example",
            "authurl.example",
            "adblock.example",
            "wild.example",
        ] {
            assert!(l.contains(host), "{host}");
        }
        assert!(l.contains("a.b.c.d.e.multi.example"));
    }

    #[test]
    fn exact_and_suffix_matching() {
        let l = list("evil.example\nsub.deep.example\n");
        assert!(l.contains("evil.example"));
        assert!(l.contains("www.evil.example"));
        assert!(l.contains("a.b.c.evil.example"));
        assert!(l.contains("sub.deep.example"));
        assert!(l.contains("x.sub.deep.example"));
        // A listed subdomain never reaches up to its parent.
        assert!(!l.contains("deep.example"));
        assert!(!l.contains("www.deep.example"));
        // Label-boundary match only — no substring dodges either way.
        assert!(!l.contains("notevil.example"));
        // A name that merely embeds the listed labels mid-string is
        // not a suffix match.
        assert!(!l.contains("evil.example.evil2.example"));
        assert!(!l.contains("unrelated.example"));
        assert!(!l.contains("evil.examplex"));
        // Case + trailing-dot + brackets are normalized on the query.
        assert!(l.contains("EVIL.example."));
        assert!(l.contains("Www.Evil.Example"));
    }

    #[test]
    fn ip_literal_entries_only_exact_match() {
        let l = list("203.0.113.66\n");
        assert!(l.contains("203.0.113.66"));
        assert!(!l.contains("www.203.0.113.66"));
        assert!(!l.contains("203.0.113.67"));
    }

    #[test]
    fn validation_rejects_junk() {
        assert!(domain_from_token("ok.example").is_some());
        assert!(domain_from_token("localhost").is_none());
        assert!(domain_from_token("com").is_none());
        assert!(domain_from_token("a b.example").is_none());
        assert!(domain_from_token("evil.example/path").is_none());
        assert!(domain_from_token("evil.example%2f").is_none());
        assert!(domain_from_token("evil..example").is_none());
        assert!(domain_from_token("user@mail.example").is_none());
        assert!(domain_from_token("").is_none());
    }

    #[test]
    fn builtin_seed_parses_and_blocks() {
        let l = list(BUILTIN_LIST);
        assert!(l.len() >= 10);
        assert!(l.contains("testsafebrowsing.appspot.com"));
        assert!(!l.contains("example.com"));
        assert!(!l.contains("arora-browser.org"));
    }

    #[test]
    fn ffi_level_load_check_reload() {
        let _g = guard();
        load("ffi-bad.example\n").unwrap();
        assert!(check("ffi-bad.example"));
        assert!(check("www.ffi-bad.example"));
        assert!(!check("clean.example"));
        // A junk load keeps the previous list.
        assert!(load("### no entries here ###").is_err());
        assert!(check("ffi-bad.example"));
        // Reload re-merges the seed (+ any override), restoring the
        // builtin coverage on top of whatever was loaded.
        reload().unwrap();
        assert!(check("testsafebrowsing.appspot.com"));
        assert!(count() >= 10);
    }
}
