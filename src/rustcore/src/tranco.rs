//! TRUST04: the Tranco top-site rank table — a vendored snapshot of
//! the Tranco list (https://tranco-list.eu) that the site-trust
//! verdicts (TRUST01) and the homograph detector (TRUST05) consult
//! for "is this host a popular domain".  A lookup never leaves the
//! machine: the rank table is shipped data, not a remote service.
//!
//! Data sources, same shape as blocklist.rs:
//!   * vendored `data/tranco-top100k.csv` — ranks 1..100000 of the
//!     Tranco daily top-1M, compiled in so ranking works with no data
//!     dir and no network at all;
//!   * `<data dir>/tranco-list.dat` — the download-on-demand full
//!     top-1M the Qt-side TrancoList updater drops in.  The updater
//!     writes the response body verbatim (top-1m.csv.zip); the
//!     loader sniffs the PK magic and pulls the `*.csv` member out
//!     through the same central-directory walk extverify uses, so a
//!     plaintext `rank,domain` file works too (fixtures, hand-drops).
//!
//! Merge is poison-safe like blocklist.rs: the update can only ADD
//! entries — a domain already seeded keeps its vendored rank, and no
//! update can quietly un-rank (or re-rank) a seed domain.  The
//! trade-off is deliberate: a stale seed rank is a cosmetic signal
//! issue, a poisoned rank is a trust-verdict issue; the seed is
//! refreshed with the crate.
//!
//! Row grammar is deliberately tolerant so upstream drops in
//! unchanged:
//!   * `<rank>,<domain>`   — the Tranco CSV shape
//!   * `<domain>`          — bare domain, ranked at its line ordinal
//!   * `#`, `!`, `;`, `//` lines, blanks — comments/junk
//! Domains must pass the same DNS-name gate the blocklist applies
//! (a dot, sane labels) and IP literals are never entries — Tranco
//! ranks domains only.

use std::collections::HashMap;
use std::io::ErrorKind;
use std::net::IpAddr;
use std::path::PathBuf;
use std::sync::{Arc, RwLock};

use crate::error::{fail, Fail, RcResult, RcStatus};

/// Updated-list drop point inside the data dir — the Qt-side
/// TrancoList updater writes this file and calls rc_tranco_reload().
const OVERRIDE_FILE: &str = "tranco-list.dat";

/// Vendored seed, compiled in so ranking works with no data dir and
/// no network at all.
const BUILTIN_LIST: &str = include_str!("../data/tranco-top100k.csv");

/// Bound on parsed entries — the full top-1M plus headroom; a file
/// growing past this is almost certainly hostile or corrupt.  Bound
/// on a single line and on the inflated `*.csv` member too.
const MAX_ENTRIES: usize = 2_000_000;
const MAX_LINE: usize = 512;
const MAX_MEMBER: usize = 64 * 1024 * 1024;

/// The loaded table.  `dir` is the data dir the entries were resolved
/// against — the lazy loader rebuilds when the effective dir changes,
/// which is how a store that appears after first use
/// (rc_set_data_dir arriving late) still picks up its override file.
#[derive(Debug)]
pub struct TrancoList {
    ranks: HashMap<String, u32>,
    dir: Option<PathBuf>,
}

/// The active table — Arc'd so a rank lookup in flight survives a
/// reload swapping the map under it.
static LIST: RwLock<Option<Arc<TrancoList>>> = RwLock::new(None);

fn list_slot() -> std::sync::RwLockReadGuard<'static, Option<Arc<TrancoList>>> {
    LIST.read().unwrap_or_else(|e| e.into_inner())
}

fn list_slot_mut() -> std::sync::RwLockWriteGuard<'static, Option<Arc<TrancoList>>> {
    LIST.write().unwrap_or_else(|e| e.into_inner())
}

fn current_dir() -> Option<PathBuf> {
    crate::store::lock().dir().ok().map(|d| d.to_path_buf())
}

/// Conservative DNS-name gate, same rules the blocklist applies: one
/// dot minimum, [a-z0-9_-] labels of sane length, no IP literals —
/// Tranco ranks domains only, so an IP-shaped entry is junk or
/// poison.
fn validate_domain(host: &str) -> bool {
    if host.len() > 253 || !host.contains('.') || host.parse::<IpAddr>().is_ok()
    {
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

/// One (domain, rank) entry out of one line, or None for
/// comments/junk.  `ordinal` is the line's 1-based position and
/// becomes the rank of a bare-domain row.
fn entry_from_line(line: &str, ordinal: u32) -> Option<(String, u32)> {
    let line = line.trim();
    if line.len() > MAX_LINE || line.is_empty() {
        return None;
    }
    match line.as_bytes()[0] {
        b'#' | b'!' | b';' | b'[' | b'/' => return None,
        _ => {}
    }
    let (rank_tok, domain_tok) = match line.split_once(',') {
        Some((r, d)) => (r.trim(), d.trim()),
        None => ("", line),
    };
    let rank = if rank_tok.is_empty() {
        ordinal
    } else {
        // Rank 0 collides with the "unranked" FFI sentinel and is no
        // Tranco rank anyway.
        match rank_tok.parse::<u32>() {
            Ok(r) if r > 0 => r,
            _ => return None,
        }
    };
    // A second comma means the row is not the rank,domain shape —
    // CSV quoting games are junk here, not a domain.
    if domain_tok.contains(',') {
        return None;
    }
    let host = domain_tok.trim_end_matches('.').to_lowercase();
    validate_domain(&host).then(|| (host, rank))
}

fn parse_entries(text: &str) -> HashMap<String, u32> {
    let mut ranks = HashMap::new();
    for (i, line) in text.lines().enumerate() {
        if ranks.len() >= MAX_ENTRIES {
            break;
        }
        // First occurrence wins — Tranco files are rank-ordered, so
        // a duplicated domain keeps its best (earliest) rank.
        if let Some((host, rank)) = entry_from_line(line, i as u32 + 1) {
            ranks.entry(host).or_insert(rank);
        }
    }
    ranks
}

/// The updatable body out of an override blob: the Tranco download is
/// a zip container (`top-1m.csv.zip`); a plaintext `rank,domain`
/// body is accepted as-is.
fn entries_from_blob(blob: &[u8]) -> HashMap<String, u32> {
    let text;
    let body = if blob.starts_with(b"PK\x03\x04") {
        match crate::extverify::zip_member_by_suffix(blob, ".csv", MAX_MEMBER)
        {
            Ok(member) => {
                text = String::from_utf8_lossy(&member).into_owned();
                text.as_str()
            }
            Err(_) => return HashMap::new(),
        }
    } else {
        match std::str::from_utf8(blob) {
            Ok(s) => s,
            // Bad bytes can't forge a valid domain (the gate is
            // strict-ASCII) — lossy keeps the good rows.
            Err(_) => {
                text = String::from_utf8_lossy(blob).into_owned();
                text.as_str()
            }
        }
    };
    parse_entries(body)
}

impl TrancoList {
    /// The host's Tranco rank, or None when unlisted.  The probe
    /// walks the hostname's label boundary left-to-right: the exact
    /// host first, then each parent domain — the most specific
    /// listed ancestor wins.  Without a public-suffix list in-crate
    /// that walk *is* the registrable-domain probe: Tranco entries
    /// are pay-level domains, so the first hit going up is the
    /// registrable name (and a `co.uk`-style suffix is never an
    /// entry anyway — the parse gate refuses rank-less shapes only
    /// Tranco's own rows can produce).
    ///
    /// Hot-path discipline: the host normalizes into a fixed stack
    /// buffer and every probe is a `&str` borrow into the map —
    /// zero heap allocation per lookup.
    pub fn rank_of(&self, host: &str) -> Option<u32> {
        let h = host.trim();
        let mut buf = [0u8; 253];
        let mut n = 0usize;
        for &b in h.as_bytes() {
            if n >= buf.len() {
                return None;
            }
            buf[n] = b.to_ascii_lowercase();
            n += 1;
        }
        while n > 0 && buf[n - 1] == b'.' {
            n -= 1;
        }
        if n == 0 {
            return None;
        }
        let mut end = n;
        // QUrl::host() reports IPv6 literals bracketed.
        if buf[0] == b'[' && buf[end - 1] == b']' {
            buf.copy_within(1..end, 0);
            end -= 2;
        }
        let host = std::str::from_utf8(&buf[..end]).ok()?;
        // IP literals and dot-less names are never ranked.
        if host.parse::<IpAddr>().is_ok() || !host.contains('.') {
            return None;
        }
        let mut candidate = host;
        loop {
            // A suffix that re-parses as an IP literal is skipped the
            // same way the blocklist skips it — an IP entry can only
            // ever equal a whole request host (and IPs are never
            // entries here regardless).
            if candidate.parse::<IpAddr>().is_err() {
                if let Some(&rank) = self.ranks.get(candidate) {
                    return Some(rank);
                }
            }
            match candidate.find('.') {
                Some(i) => candidate = &candidate[i + 1..],
                None => return None,
            }
            if !candidate.contains('.') {
                return None;
            }
        }
    }

    pub fn len(&self) -> usize {
        self.ranks.len()
    }
}

/// Seed ∪ override, seed rank winning every shared key.  The union
/// is deliberate: an update adds coverage on top of the seed, so a
/// truncated or malicious update can only ever rank MORE domains,
/// never un-rank or re-rank a seeded one.
fn load_from_disk() -> TrancoList {
    let dir = current_dir();
    let mut ranks = parse_entries(BUILTIN_LIST);
    if let Some(dir) = &dir {
        match std::fs::read(dir.join(OVERRIDE_FILE)) {
            Ok(blob) => {
                for (host, rank) in entries_from_blob(&blob) {
                    ranks.entry(host).or_insert(rank);
                }
            }
            Err(e) if e.kind() == ErrorKind::NotFound => {}
            Err(_) => {} // unreadable override — the seed still ranks
        }
    }
    TrancoList { ranks, dir }
}

/// The loaded table, lazily populated.  A cached table resolved
/// before the data dir existed is rebuilt once the dir appears — the
/// store can arrive after first use (rc_set_data_dir runs whenever
/// the SecureStore shim or the updater first touches the core).
fn active_list() -> Arc<TrancoList> {
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

/// rc_tranco_load: swap in a caller-supplied `rank,domain` body
/// (update/test seam).  A body yielding no usable entries is
/// RC_CORRUPT and the previous table stays active.
pub fn load(text: &str) -> RcResult<()> {
    let ranks = parse_entries(text);
    if ranks.is_empty() {
        return fail(RcStatus::Corrupt, "tranco: no usable entries");
    }
    *list_slot_mut() = Some(Arc::new(TrancoList {
        ranks,
        dir: current_dir(),
    }));
    Ok(())
}

/// rc_tranco_reload: re-run the seed ∪ `<data dir>/tranco-list.dat`
/// merge, e.g. after a list update lands in the data dir.
/// RC_CORRUPT when an override exists but yields zero entries — the
/// previous table stays active.
pub fn reload() -> RcResult<()> {
    if let Some(dir) = current_dir() {
        if let Ok(blob) = std::fs::read(dir.join(OVERRIDE_FILE)) {
            if entries_from_blob(&blob).is_empty() {
                return Err(Fail {
                    status: RcStatus::Corrupt,
                    msg: "tranco: override yields no entries".into(),
                });
            }
        }
    }
    *list_slot_mut() = Some(Arc::new(load_from_disk()));
    Ok(())
}

/// rc_tranco_rank: FFI-facing lookup — 0 when unranked (Tranco ranks
/// start at 1).  Never fails on content — a malformed host simply
/// matches nothing.
pub fn rank(host: &str) -> u32 {
    active_list().rank_of(host).unwrap_or(0)
}

/// Entry count of the active table — diagnostics/tests.
pub fn count() -> usize {
    active_list().len()
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::MutexGuard;

    // The active table is process-global — serialize on the shared
    // store lock (sitedecisions showed private per-module locks let a
    // neighbour's set_data_dir move the store mid-test).
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn table(text: &str) -> TrancoList {
        TrancoList {
            ranks: parse_entries(text),
            dir: None,
        }
    }

    /// Minimal zip container holding one stored-or-deflated member —
    /// enough for the update-path tests without a zip crate.
    fn test_zip(name: &str, body: &[u8], deflate: bool) -> Vec<u8> {
        let (method, stored): (u16, Vec<u8>) = if deflate {
            (8, miniz_oxide::deflate::compress_to_vec(body, 6))
        } else {
            (0, body.to_vec())
        };
        let uncomp = body.len() as u32;
        let comp = stored.len() as u32;
        let name_b = name.as_bytes();
        let mut zip = Vec::new();
        // Local file header
        zip.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        zip.extend_from_slice(&20u16.to_le_bytes()); // version
        zip.extend_from_slice(&0u16.to_le_bytes()); // flags
        zip.extend_from_slice(&method.to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes()); // mod time
        zip.extend_from_slice(&0u16.to_le_bytes()); // mod date
        zip.extend_from_slice(&0u32.to_le_bytes()); // crc32 (unchecked)
        zip.extend_from_slice(&comp.to_le_bytes());
        zip.extend_from_slice(&uncomp.to_le_bytes());
        zip.extend_from_slice(&(name_b.len() as u16).to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes()); // extra len
        zip.extend_from_slice(name_b);
        zip.extend_from_slice(&stored);
        // Central directory
        let cd_off = zip.len() as u32;
        zip.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        zip.extend_from_slice(&20u16.to_le_bytes()); // version made by
        zip.extend_from_slice(&20u16.to_le_bytes()); // version needed
        zip.extend_from_slice(&0u16.to_le_bytes()); // flags
        zip.extend_from_slice(&method.to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes());
        zip.extend_from_slice(&0u32.to_le_bytes()); // crc32
        zip.extend_from_slice(&comp.to_le_bytes());
        zip.extend_from_slice(&uncomp.to_le_bytes());
        zip.extend_from_slice(&(name_b.len() as u16).to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes()); // extra
        zip.extend_from_slice(&0u16.to_le_bytes()); // comment
        zip.extend_from_slice(&0u16.to_le_bytes()); // disk
        zip.extend_from_slice(&0u16.to_le_bytes()); // internal attrs
        zip.extend_from_slice(&0u32.to_le_bytes()); // external attrs
        zip.extend_from_slice(&0u32.to_le_bytes()); // local header off
        zip.extend_from_slice(name_b);
        let cd_size = zip.len() as u32 - cd_off;
        // EOCD
        zip.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes()); // disk
        zip.extend_from_slice(&0u16.to_le_bytes()); // cd disk
        zip.extend_from_slice(&1u16.to_le_bytes()); // entries this disk
        zip.extend_from_slice(&1u16.to_le_bytes()); // entries total
        zip.extend_from_slice(&cd_size.to_le_bytes());
        zip.extend_from_slice(&cd_off.to_le_bytes());
        zip.extend_from_slice(&0u16.to_le_bytes()); // comment len
        zip
    }

    #[test]
    fn parses_every_row_shape() {
        let t = table(
            "# comment\n\
             ! comment\n\
             ; comment\n\
             // comment\n\
             [section]\n\
             \n\
             1,google.com\n\
             2, example.org \n\
             bare.example.net\n\
             0,zerorank.example\n\
             notarank,junk.example\n\
             4,still-ok.example\n\
             5,1.2.3.4\n\
             6,localhost\n\
             7,sub,comma.example\n",
        );
        assert_eq!(t.rank_of("google.com"), Some(1));
        assert_eq!(t.rank_of("example.org"), Some(2));
        assert_eq!(t.rank_of("bare.example.net"), Some(9));
        assert_eq!(t.rank_of("still-ok.example"), Some(4));
        // rank-0 / non-numeric-rank / IP / single-label / extra-comma
        // rows all dropped.
        assert_eq!(t.len(), 4);
    }

    #[test]
    fn rank_probes_exact_then_registrable() {
        let t = table("10,google.com\n20,sub.shop.example\n");
        assert_eq!(t.rank_of("google.com"), Some(10));
        // Unlisted subdomain walks up to the listed parent.
        assert_eq!(t.rank_of("www.google.com"), Some(10));
        assert_eq!(t.rank_of("a.b.c.google.com"), Some(10));
        // A listed subdomain's own rank wins over the parent's.
        assert_eq!(t.rank_of("sub.shop.example"), Some(20));
        assert_eq!(t.rank_of("deep.sub.shop.example"), Some(20));
        // The parent alone is not listed — a listed subdomain never
        // reaches up.
        assert_eq!(t.rank_of("shop.example"), None);
        // Label-boundary only.
        assert_eq!(t.rank_of("notgoogle.com"), None);
        assert_eq!(t.rank_of("google.com.evil.example"), None);
        // Normalization: case, trailing dot, brackets/IPs, junk.
        assert_eq!(t.rank_of("WWW.GOOGLE.COM."), Some(10));
        assert_eq!(t.rank_of("1.2.3.4"), None);
        assert_eq!(t.rank_of("[2001:db8::1]"), None);
        assert_eq!(t.rank_of("localhost"), None);
        assert_eq!(t.rank_of(""), None);
        assert_eq!(t.rank_of(&"a".repeat(300)), None);
    }

    #[test]
    fn duplicated_domain_keeps_first_rank() {
        // Tranco files are rank-ordered — first occurrence is the
        // best rank and wins.
        let t = table("3,dup.example\n9,dup.example\n");
        assert_eq!(t.rank_of("dup.example"), Some(3));
    }

    #[test]
    fn builtin_seed_parses_and_ranks() {
        let t = table(BUILTIN_LIST);
        assert!(t.len() >= 100_000);
        assert_eq!(t.rank_of("google.com"), Some(1));
        assert!(t.rank_of("facebook.com").is_some());
        assert!(t.rank_of("www.google.com") == Some(1));
        assert_eq!(t.rank_of("definitely-not-ranked.invalid"), None);
    }

    #[test]
    fn update_blob_accepts_plaintext_and_zip() {
        let csv = b"7,updated.example\n";
        assert_eq!(entries_from_blob(csv).len(), 1);
        // Stored member.
        let zip_store = test_zip("top-1m.csv", csv, false);
        let e = entries_from_blob(&zip_store);
        assert_eq!(e.get("updated.example"), Some(&7));
        // Deflated member — the real top-1m.csv.zip shape.
        let zip_defl = test_zip("top-1m.csv", csv, true);
        let e = entries_from_blob(&zip_defl);
        assert_eq!(e.get("updated.example"), Some(&7));
        // A zip with no .csv member and a garbage blob both parse
        // empty — the caller treats that as "no usable update".
        let zip_other = test_zip("readme.txt", csv, false);
        assert!(entries_from_blob(&zip_other).is_empty());
        assert!(entries_from_blob(b"\x00\x01\x02garbage").is_empty());
        assert!(entries_from_blob(b"PK\x03\x04truncated").is_empty());
    }

    #[test]
    fn merge_is_poison_safe() {
        // Build the merge the way load_from_disk does: seed first,
        // update rows only fill gaps.
        let mut ranks = parse_entries("5,seed.example\n9,also-seed.example\n");
        let seed_len = ranks.len();
        for (host, rank) in entries_from_blob(
            b"1,seed.example\n2,new.example\n", // re-rank + drop attempts + add
        ) {
            ranks.entry(host).or_insert(rank);
        }
        let t = TrancoList { ranks, dir: None };
        // The update cannot re-rank a seed entry…
        assert_eq!(t.rank_of("seed.example"), Some(5));
        // …nor un-rank one it simply omits…
        assert_eq!(t.rank_of("also-seed.example"), Some(9));
        // …only add coverage.
        assert_eq!(t.rank_of("new.example"), Some(2));
        assert_eq!(t.len(), seed_len + 1);
    }

    #[test]
    fn ffi_level_load_rank_reload() {
        let _g = guard();
        load("11,ffi-ranked.example\n").unwrap();
        assert_eq!(rank("ffi-ranked.example"), 11);
        assert_eq!(rank("www.ffi-ranked.example"), 11);
        assert_eq!(rank("unlisted.invalid"), 0);
        // A junk load keeps the previous table.
        assert!(load("### no entries ###").is_err());
        assert_eq!(rank("ffi-ranked.example"), 11);
        // Reload re-merges the seed (+ any override), restoring the
        // vendored coverage on top of whatever was loaded.
        reload().unwrap();
        assert_eq!(rank("google.com"), 1);
        assert!(count() >= 100_000);
    }

    #[test]
    fn reload_picks_up_datadir_override() {
        let _g = guard();
        let dir = std::env::temp_dir().join(format!(
            "rustcore-tranco-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let override_path = dir.join(OVERRIDE_FILE);
        // A plaintext override adds ranks on top of the seed.
        std::fs::write(&override_path, "42,added-by-update.example\n").unwrap();
        crate::store::lock().set_data_dir(dir.to_str().unwrap()).unwrap();
        reload().unwrap();
        assert_eq!(rank("added-by-update.example"), 42);
        assert_eq!(rank("google.com"), 1); // seed coverage intact
        // A poisoned override trying to drop seed coverage can't.
        std::fs::write(&override_path, "999999,google.com\n").unwrap();
        reload().unwrap();
        assert_eq!(rank("google.com"), 1); // seed rank wins
        // An empty override is RC_CORRUPT — previous table stays.
        std::fs::write(&override_path, "# nothing usable\n").unwrap();
        assert!(reload().is_err());
        assert_eq!(rank("google.com"), 1);
        std::fs::remove_file(&override_path).unwrap();
        reload().unwrap();
        assert_eq!(rank("added-by-update.example"), 0);
        std::fs::remove_dir_all(&dir).ok();
    }
}
