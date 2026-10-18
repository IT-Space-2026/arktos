//! TRUST04/SEC19: the site-rank table — a vendored snapshot of the
//! Tranco list (https://tranco-list.eu) plus two consent-gated
//! downloads that the site-trust verdicts (TRUST01) and the
//! homograph detector (TRUST05) consult for "is this host a popular
//! domain".  A lookup never leaves the machine: the rank table is
//! shipped data, not a remote service.
//!
//! Data sources, same shape as blocklist.rs:
//!   * vendored `data/tranco-top100k.csv` — ranks 1..100000 of the
//!     Tranco daily top-1M, compiled in so ranking works with no data
//!     dir and no network at all;
//!   * `<data dir>/tranco-list.dat` — the download-on-demand full
//!     Tranco top-1M the Qt-side TrancoList updater drops in.  The
//!     updater writes the response body verbatim (top-1m.csv.zip);
//!     the loader sniffs the PK magic and pulls the `*.csv` member
//!     out through the same central-directory walk extverify uses,
//!     so a plaintext `rank,domain` file works too (fixtures,
//!     hand-drops);
//!   * `<data dir>/majestic-million.csv` — the Majestic Million
//!     (https://majestic.com/reports/majestic-million, CC BY 3.0)
//!     second rank source: a plain multi-column CSV
//!     (`GlobalRank,TldRank,Domain,...`) — no zip wrapper, the parser
//!     reads the GlobalRank and Domain columns directly.
//!
//! Rank merge (SEC19): the effective rank is the BEST (lowest) rank
//! across all sources — Tranco's aggregate and Majestic's
//! backlink-based ranking measure different signals, and membership
//! in either counts.  An update can still never un-rank a domain
//! (min can only improve a rank) and a source that stops listing a
//! domain simply stops contributing — the vendored seed alone keeps
//! the feature alive.
//!
//! Row grammars are deliberately tolerant so upstream drops in
//! unchanged:
//!   * `<rank>,<domain>`   — the Tranco CSV shape
//!   * `<rank>,<tldrank>,<domain>,...` — the Majestic CSV shape
//!     (GlobalRank/TldRank/Domain/TLD columns; the header row fails
//!     the rank parse and drops out)
//!   * `<domain>`          — bare domain, ranked at its line ordinal
//!   * `#`, `!`, `;`, `//` lines, blanks — comments/junk
//!
//! Domains must pass the same DNS-name gate the blocklist applies
//! (a dot, sane labels) and IP literals are never entries — the
//! lists rank domains only.

use std::collections::HashMap;
use std::net::IpAddr;
use std::path::Path;
use std::sync::Arc;

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::store::{data_dir, DirCache};

/// Updated-list drop points inside the data dir — the Qt-side
/// TrancoList updater writes these files and calls
/// rc_tranco_reload().
const OVERRIDE_FILE: &str = "tranco-list.dat";
const MAJESTIC_FILE: &str = "majestic-million.csv";

/// Vendored seed, compiled in so ranking works with no data dir and
/// no network at all.
const BUILTIN_LIST: &str = include_str!("../data/tranco-top100k.csv");

/// Bound on parsed entries — the full top-1M plus headroom; a file
/// growing past this is almost certainly hostile or corrupt.  Bound
/// on a single line, on the inflated `*.csv` zip member and on a
/// whole override file (majestic_million.csv is ~80 MB today).
const MAX_ENTRIES: usize = 2_000_000;
const MAX_LINE: usize = 512;
const MAX_MEMBER: usize = 64 * 1024 * 1024;
const MAX_FILE: u64 = 128 * 1024 * 1024;

/// The loaded table.  The DirCache slot stamps the data dir the
/// entries were resolved against — a store that appears after first
/// use (rc_set_data_dir arriving late) still picks up its override
/// file on the next probe.
///
/// `skeletons` is the TRUST05 homograph index: the UTS #39
/// confusable skeleton of each entry's *decoded* name -> the raw
/// listed name.  Only entries whose skeleton differs from their
/// listed spelling live here — an identity-skeleton entry is already
/// answered by `ranks` directly, which keeps the index roughly the
/// size of the confusable+xn-- subset instead of a second full copy
/// of the table.  It is built in the same parse pass and under the
/// same poison-safe merge as `ranks`: line order decides collisions
/// (best rank first), and an update can only add skeleton keys, never
/// re-point a seeded one at a different name.
/// One merged entry: `best` is the effective (lowest) rank any list
/// assigns, and each list's OWN rank is kept alongside — 0 means the
/// list does not carry the host.  Keeping per-source ranks lets the
/// verdict name every list that knows the domain (Tranco rank 50,
/// Majestic rank 12), not just the one that won.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct RankEntry {
    best: u32,
    tranco: u32,
    majestic: u32,
}

#[derive(Debug, Clone)]
pub struct TrancoList {
    /// domain -> RankEntry (SRC_TRANCO for the seed + tranco-list.dat,
    /// SRC_MAJESTIC for majestic-million.csv).
    ranks: HashMap<String, RankEntry>,
    skeletons: HashMap<String, String>,
}

/// Source bit for the vendored seed and `tranco-list.dat` (both
/// Tranco data).
const SRC_TRANCO: u8 = 1;
/// Source bit for `majestic-million.csv`.
const SRC_MAJESTIC: u8 = 2;

/// Grammar each override file parses under — keyed by the file name
/// the Qt updater drops it under.
fn grammar_for(name: &str) -> bool {
    name == MAJESTIC_FILE
}

/// Source bitmask each override file merges under.
fn src_for(name: &str) -> u8 {
    if name == MAJESTIC_FILE { SRC_MAJESTIC } else { SRC_TRANCO }
}

/// The active table — Arc'd so a rank lookup in flight survives a
/// reload swapping the map under it.
static LIST: DirCache<TrancoList> = DirCache::new();

/// Conservative DNS-name gate: util::valid_dns_shape, same rules the
/// blocklist applies, plus no IP literals — Tranco ranks domains
/// only, so an IP-shaped entry is junk or poison.
fn validate_domain(host: &str) -> bool {
    crate::util::valid_dns_shape(host) && host.parse::<IpAddr>().is_err()
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
    validate_domain(&host).then_some((host, rank))
}

/// One (domain, rank) entry out of one Majestic Million CSV row, or
/// None for the header/comments/junk.  The upstream shape is
/// `GlobalRank,TldRank,Domain,TLD,...` — rank is field 0, the domain
/// field 2.  The header row's "GlobalRank" is no u32 and drops out;
/// a row without a Domain column is junk.
fn majestic_entry_from_line(line: &str) -> Option<(String, u32)> {
    let line = line.trim();
    if line.len() > MAX_LINE || line.is_empty() {
        return None;
    }
    match line.as_bytes()[0] {
        b'#' | b'!' | b';' | b'[' | b'/' => return None,
        _ => {}
    }
    let mut fields = line.split(',');
    let rank: u32 = fields.next()?.trim().parse().ok()?;
    if rank == 0 {
        return None;
    }
    fields.next()?; // TldRank
    let host = fields.next()?.trim().trim_end_matches('.').to_lowercase();
    validate_domain(&host).then_some((host, rank))
}

/// Both indexes out of one ordered pass — (ranks, skeletons).  The
/// skeleton of each entry's decoded name is computed once, here, so
/// the lookalike probe itself stays allocation-free; entries whose
/// skeleton equals their listed name skip the map entirely (rank_of
/// already answers those).
fn parse_entries(text: &str) -> (HashMap<String, u32>, HashMap<String, String>) {
    let mut ranks = HashMap::new();
    let mut skeletons = HashMap::new();
    for (i, line) in text.lines().enumerate() {
        if ranks.len() >= MAX_ENTRIES {
            break;
        }
        // First occurrence wins — Tranco files are rank-ordered, so
        // a duplicated domain keeps its best (earliest) rank.  The
        // same rule feeds the skeleton index: the most popular entry
        // a skeleton can resolve to wins the key.
        if let Some((host, rank)) = entry_from_line(line, i as u32 + 1) {
            if ranks.contains_key(&host) {
                continue;
            }
            if let Some(skel) = crate::lookalike::entry_skeleton(&host) {
                if skel != host {
                    skeletons.entry(skel).or_insert_with(|| host.clone());
                }
            }
            ranks.insert(host, rank);
        }
    }
    (ranks, skeletons)
}

/// Same pass over the Majestic Million multi-column shape.
fn parse_majestic_entries(
    text: &str,
) -> (HashMap<String, u32>, HashMap<String, String>) {
    let mut ranks = HashMap::new();
    let mut skeletons = HashMap::new();
    for line in text.lines() {
        if ranks.len() >= MAX_ENTRIES {
            break;
        }
        if let Some((host, rank)) = majestic_entry_from_line(line) {
            if ranks.contains_key(&host) {
                continue;
            }
            if let Some(skel) = crate::lookalike::entry_skeleton(&host) {
                if skel != host {
                    skeletons.entry(skel).or_insert_with(|| host.clone());
                }
            }
            ranks.insert(host, rank);
        }
    }
    (ranks, skeletons)
}

/// The updatable body out of an override blob: the Tranco download is
/// a zip container (`top-1m.csv.zip`); plaintext bodies are accepted
/// as-is, in the `rank,domain` grammar or the Majestic Million
/// columnar grammar (`majestic` selects it — the Qt updater drops
/// each source under its own file name).
fn entries_from_blob(
    blob: &[u8],
    majestic: bool,
) -> (HashMap<String, u32>, HashMap<String, String>) {
    let text;
    let body = if !majestic && blob.starts_with(b"PK\x03\x04") {
        match crate::extverify::zip_member_by_suffix(blob, ".csv", MAX_MEMBER)
        {
            Ok(member) => {
                text = String::from_utf8_lossy(&member).into_owned();
                text.as_str()
            }
            Err(_) => return (HashMap::new(), HashMap::new()),
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
    if majestic {
        parse_majestic_entries(body)
    } else {
        parse_entries(body)
    }
}

/// Fold one parsed source into the merged table (SEC19): a domain's
/// effective rank is the best (lowest) rank any list gives it — an
/// update can improve a seed or sibling-list rank but never un-rank
/// anything.  Skeleton collisions resolve to the better-ranked raw
/// name so the lookalike probe keeps answering with the most popular
/// twin.
fn merge_into(
    ranks: &mut HashMap<String, RankEntry>,
    skeletons: &mut HashMap<String, String>,
    eranks: HashMap<String, u32>,
    eskels: HashMap<String, String>,
    src: u8,
) {
    for (host, rank) in eranks {
        ranks
            .entry(host)
            .and_modify(|r| {
                // Every list keeps its own rank — the panel shows
                // each source's number, not only the winner's.  Two
                // merges of the same source (seed + downloaded list)
                // fold to that source's best rank.
                let field = if src == SRC_MAJESTIC {
                    &mut r.majestic
                } else {
                    &mut r.tranco
                };
                *field = if *field == 0 { rank } else { (*field).min(rank) };
                r.best = r.best.min(rank);
            })
            .or_insert(match src {
                SRC_MAJESTIC => RankEntry { best: rank, tranco: 0, majestic: rank },
                _ => RankEntry { best: rank, tranco: rank, majestic: 0 },
            });
    }
    for (skel, raw) in eskels {
        skeletons
            .entry(skel)
            .and_modify(|cur| {
                let cur_rank = ranks.get(cur.as_str()).map(|e| e.best).unwrap_or(u32::MAX);
                let new_rank = ranks.get(raw.as_str()).map(|e| e.best).unwrap_or(u32::MAX);
                if new_rank < cur_rank {
                    *cur = raw.clone();
                }
            })
            .or_insert(raw);
    }
}

/// Read a data-dir override file under the size bound — an oversized
/// drop is an attack or junk, and an unreadable/absent file simply
/// contributes nothing.
fn read_override(dir: &Path, name: &str) -> Option<Vec<u8>> {
    let path = dir.join(name);
    let meta = std::fs::metadata(&path).ok()?;
    if !meta.is_file() || meta.len() > MAX_FILE {
        return None;
    }
    std::fs::read(&path).ok()
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
    /// Hot-path discipline: the host normalizes into util::HostNorm's
    /// fixed stack buffer and every probe is a `&str` borrow into
    /// the map — zero heap allocation per lookup.  (RAUDIT01: shared
    /// with blocklist::contains; longer-than-DNS input probes its
    /// rightmost 253 bytes — every entry is shorter, so a matchable
    /// suffix always lives in the tail.)
    pub fn rank_of(&self, host: &str) -> Option<u32> {
        self.rank_detail_of(host).map(|e| e.best)
    }

    /// rank_of plus the per-source ranks — which top-site lists
    /// carry the matched name and at what rank each lists it.
    pub fn rank_detail_of(&self, host: &str) -> Option<RankEntry> {
        let h = crate::util::HostNorm::new(host)?;
        let host = h.as_str();
        // IP literals and dot-less names are never ranked.
        if host.parse::<IpAddr>().is_ok() || !host.contains('.') {
            return None;
        }
        for candidate in h.suffixes() {
            // Tranco entries all contain a dot — the TLD alone is
            // never an answer.
            if !candidate.contains('.') {
                return None;
            }
            // A suffix that re-parses as an IP literal is skipped the
            // same way the blocklist skips it — an IP entry can only
            // ever equal a whole request host (and IPs are never
            // entries here regardless).
            if candidate.parse::<IpAddr>().is_err() {
                if let Some(&entry) = self.ranks.get(candidate) {
                    return Some(entry);
                }
            }
        }
        None
    }

    pub fn len(&self) -> usize {
        self.ranks.len()
    }

    /// TRUST05 skeleton probe: the query side passes a confusable
    /// skeleton suffix and gets back the raw listed name that
    /// skeleton resolves to.  Two maps can answer a probe — an
    /// entry listed under its own name (identity skeleton, answered
    /// by `ranks`) and an entry whose skeleton differs from its
    /// spelling (confusable twins and xn-- A-labels, answered by
    /// `skeletons`).  When both answer, the better-ranked raw wins —
    /// a domain listed under its own name outranks a lookalike twin
    /// that happens to be listed too.
    pub(crate) fn skeleton_match<'a>(
        &'a self,
        skel_suffix: &'a str,
    ) -> Option<&'a str> {
        match (
            self.skeletons.get(skel_suffix),
            self.ranks.get(skel_suffix),
        ) {
            (Some(twin), Some(&own)) => {
                let twin_rank =
                    self.ranks.get(twin.as_str()).map(|e| e.best).unwrap_or(u32::MAX);
                Some(if own.best <= twin_rank {
                    skel_suffix
                } else {
                    twin.as_str()
                })
            }
            (Some(twin), None) => Some(twin.as_str()),
            (None, Some(_)) => Some(skel_suffix),
            (None, None) => None,
        }
    }
}

/// Seed ∪ both override files, best-rank-wins across every source
/// (SEC19).  The merge is still poison-safe where it matters: an
/// update can only ever rank MORE domains or improve a rank, never
/// un-rank anything, so a truncated or malicious download degrades
/// to "no new coverage" — the vendored seed alone keeps ranking.
fn load_from_disk(dir: Option<&Path>) -> TrancoList {
    let (seed_ranks, mut skeletons) = parse_entries(BUILTIN_LIST);
    let mut ranks = HashMap::new();
    // The vendored seed is Tranco data — merges under its bit like
    // the downloaded override does.
    merge_into(&mut ranks, &mut skeletons, seed_ranks, HashMap::new(), SRC_TRANCO);
    if let Some(dir) = dir {
        for name in [OVERRIDE_FILE, MAJESTIC_FILE] {
            if let Some(blob) = read_override(dir, name) {
                let (eranks, eskels) =
                    entries_from_blob(&blob, grammar_for(name));
                merge_into(&mut ranks, &mut skeletons, eranks, eskels,
                           src_for(name));
            }
        }
    }
    TrancoList { ranks, skeletons }
}

/// The loaded table, lazily populated — the DirCache rebuilds it once
/// when the data dir arrives late (rc_set_data_dir runs whenever the
/// SecureStore shim or the updater first touches the core).
pub(crate) fn active_list() -> Arc<TrancoList> {
    LIST.get_or_load(load_from_disk)
}

/// rc_tranco_load: swap in a caller-supplied `rank,domain` body
/// (update/test seam).  A body yielding no usable entries is
/// RC_CORRUPT and the previous table stays active.
pub fn load(text: &str) -> RcResult<()> {
    let (eranks, eskels) = parse_entries(text);
    if eranks.is_empty() {
        return fail(RcStatus::Corrupt, "tranco: no usable entries");
    }
    let mut ranks = HashMap::new();
    let mut skeletons = HashMap::new();
    merge_into(&mut ranks, &mut skeletons, eranks, eskels, SRC_TRANCO);
    LIST.replace(TrancoList { ranks, skeletons });
    Ok(())
}

/// rc_tranco_reload: re-run the seed ∪ `<data dir>/{tranco-list.dat,
/// majestic-million.csv}` merge, e.g. after a list update lands in
/// the data dir.  RC_CORRUPT when an override exists but yields zero
/// usable entries — the previous table stays active.
pub fn reload() -> RcResult<()> {
    let dir = data_dir();
    if let Some(dir) = &dir {
        for name in [OVERRIDE_FILE, MAJESTIC_FILE] {
            if dir.join(name).exists() {
                let empty = read_override(dir, name)
                    .map(|blob| {
                        entries_from_blob(&blob, grammar_for(name))
                            .0
                            .is_empty()
                    })
                    .unwrap_or(true);
                if empty {
                    return Err(Fail {
                        status: RcStatus::Corrupt,
                        msg: format!(
                            "tranco: {name} yields no entries"
                        ),
                    });
                }
            }
        }
    }
    LIST.replace(load_from_disk(dir.as_deref()));
    Ok(())
}

/// rc_tranco_file_entries: usable-entry count of one data-dir
/// override file, parsed under the grammar its name selects — the
/// Qt updater's per-file validation probe (a body that yields zero
/// entries is junk and must not be committed on top of a good copy).
/// 0 when the data dir is unset, the file is absent, unreadable,
/// oversized or yields nothing.
pub fn file_entries(name: &str) -> usize {
    let Some(dir) = data_dir() else { return 0 };
    read_override(&dir, name)
        .map(|blob| entries_from_blob(&blob, grammar_for(name)).0.len())
        .unwrap_or(0)
}

/// rc_tranco_rank: FFI-facing lookup — 0 when unranked (Tranco ranks
/// start at 1).  Never fails on content — a malformed host simply
/// matches nothing.
pub fn rank(host: &str) -> u32 {
    active_list().rank_of(host).unwrap_or(0)
}

/// rank + per-source ranks for the verdict — (best, tranco,
/// majestic), all zero when unranked.  The sources hold each list's
/// own number so the UI can name them all.
pub(crate) fn rank_detail(host: &str) -> (u32, u32, u32) {
    active_list()
        .rank_detail_of(host)
        .map(|e| (e.best, e.tranco, e.majestic))
        .unwrap_or((0, 0, 0))
}

/// Source-bit constants re-exported for the test merge seam —
/// sitetrust tests merge fixture rows under the Majestic grammar.
#[cfg(test)]
pub(crate) const SRCS_TRANCO: u8 = SRC_TRANCO;
#[cfg(test)]
pub(crate) const SRCS_MAJESTIC: u8 = SRC_MAJESTIC;

/// Test seam: merge a list body into the active table under an
/// explicit source bit, so verdict tests can build "both"/
/// "majestic-only" provenance without writing override files.
#[cfg(test)]
pub(crate) fn merge_text_as(text: &str, src: u8, majestic_grammar: bool) {
    let (er, es) = entries_from_blob(text.as_bytes(), majestic_grammar);
    let mut l = (*active_list()).clone();
    merge_into(&mut l.ranks, &mut l.skeletons, er, es, src);
    LIST.replace(l);
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
        let (eranks, eskels) = parse_entries(text);
        let mut ranks = HashMap::new();
        let mut skeletons = HashMap::new();
        merge_into(&mut ranks, &mut skeletons, eranks, eskels, SRC_TRANCO);
        TrancoList { ranks, skeletons }
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
        assert_eq!(entries_from_blob(csv, false).0.len(), 1);
        // Stored member.
        let zip_store = test_zip("top-1m.csv", csv, false);
        let e = entries_from_blob(&zip_store, false);
        assert_eq!(e.0.get("updated.example"), Some(&7));
        // Deflated member — the real top-1m.csv.zip shape.
        let zip_defl = test_zip("top-1m.csv", csv, true);
        let e = entries_from_blob(&zip_defl, false);
        assert_eq!(e.0.get("updated.example"), Some(&7));
        // A zip with no .csv member and a garbage blob both parse
        // empty — the caller treats that as "no usable update".
        let zip_other = test_zip("readme.txt", csv, false);
        assert!(entries_from_blob(&zip_other, false).0.is_empty());
        assert!(entries_from_blob(b"\x00\x01\x02garbage", false).0.is_empty());
        assert!(entries_from_blob(b"PK\x03\x04truncated", false).0.is_empty());
    }

    #[test]
    fn majestic_blob_parses_columns() {
        // The real majestic_million.csv shape: header row, then
        // GlobalRank,TldRank,Domain,TLD,... — fields 0 and 2 carry
        // the entry.
        let (ranks, _) = entries_from_blob(
            b"GlobalRank,TldRank,Domain,TLD,RefSubNets,RefIPs,IDN_Domain,IDN_TLD,PrevGlobalRank,PrevTldRank,PrevRefSubNets,PrevRefIPs\n\
              1,1,google.com,com,508408,2224152,google.com,com,1,1,508504,2225468\n\
              42,9,mj.example,example,10,20,mj.example,example,40,8,11,22\n\
              # comment\n\
              \n\
              7,junk.example,example\n\
              0,1,zerorank.example,example\n\
              9,1,1.2.3.4,com\n\
              xx,1,notarank.example,com\n",
            true,
        );
        assert_eq!(ranks.get("google.com"), Some(&1));
        assert_eq!(ranks.get("mj.example"), Some(&42));
        // Short row (no Domain field), rank-0, IP literal and
        // non-numeric-rank rows all drop out.
        assert_eq!(ranks.len(), 2);
        // The majestic grammar never sniffs zip containers.
        let zip = test_zip("majestic_million.csv", b"5,z.example\n", false);
        assert!(entries_from_blob(&zip, true).0.is_empty());
    }

    #[test]
    fn merge_takes_best_rank_across_lists() {
        // SEC19: the effective rank is the best (lowest) rank any
        // source assigns — the seed's stale rank can improve, nothing
        // can be un-ranked, and coverage only grows.
        let mut ranks = HashMap::new();
        let mut skeletons = HashMap::new();
        let (sranks, sskels) =
            parse_entries("5,seed.example\n9,also-seed.example\n");
        merge_into(&mut ranks, &mut skeletons, sranks, sskels, SRC_TRANCO);
        let seed_len = ranks.len();
        let (eranks, eskels) = entries_from_blob(
            b"1,seed.example\n2,new.example\n3,tie.example\n",
            false,
        );
        merge_into(&mut ranks, &mut skeletons, eranks, eskels, SRC_TRANCO);
        // A Majestic body listing the same domain lower still wins.
        let (mranks, mskels) = entries_from_blob(
            b"GlobalRank,TldRank,Domain,TLD\n7,3,seed.example,example\n8,4,mj-only.example,example\n3,5,tie.example,example\n",
            true,
        );
        merge_into(&mut ranks, &mut skeletons, mranks, mskels, SRC_MAJESTIC);
        let t = TrancoList { ranks, skeletons };
        assert_eq!(t.rank_of("seed.example"), Some(1)); // tranco update bested seed+majestic
        assert_eq!(t.rank_of("also-seed.example"), Some(9)); // no source can un-rank
        assert_eq!(t.rank_of("new.example"), Some(2));
        assert_eq!(t.rank_of("mj-only.example"), Some(8)); // membership in either counts
        assert_eq!(t.rank_of("never-listed.example"), None);
        assert_eq!(t.len(), seed_len + 3);
        // Per-source ranks are kept verbatim — the verdict names
        // every list carrying the domain with its own number.
        assert_eq!(t.rank_detail_of("seed.example"),
                   Some(RankEntry { best: 1, tranco: 1, majestic: 7 }));
        assert_eq!(t.rank_detail_of("mj-only.example"),
                   Some(RankEntry { best: 8, tranco: 0, majestic: 8 }));
        assert_eq!(t.rank_detail_of("new.example"),
                   Some(RankEntry { best: 2, tranco: 2, majestic: 0 }));
        assert_eq!(t.rank_detail_of("tie.example"),
                   Some(RankEntry { best: 3, tranco: 3, majestic: 3 }));
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

    #[test]
    fn majestic_file_merges_on_reload() {
        let _g = guard();
        let dir = std::env::temp_dir().join(format!(
            "rustcore-majestic-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let tranco_path = dir.join(OVERRIDE_FILE);
        let majestic_path = dir.join(MAJESTIC_FILE);
        std::fs::write(&tranco_path, "50,both-lists.example\n20,tied.example\n").unwrap();
        std::fs::write(
            &majestic_path,
            "GlobalRank,TldRank,Domain,TLD\n\
             12,4,both-lists.example,example\n\
             77,9,mj-only.example,example\n\
             20,6,tied.example,example\n",
        )
        .unwrap();
        crate::store::lock().set_data_dir(dir.to_str().unwrap()).unwrap();
        // Per-file probe: usable-entry counts under each grammar.
        assert_eq!(file_entries(OVERRIDE_FILE), 2);
        assert_eq!(file_entries(MAJESTIC_FILE), 3);
        assert_eq!(file_entries("missing-file.csv"), 0);
        reload().unwrap();
        // Best rank across the lists wins; either-source membership
        // counts; an unlisted domain stays unranked.
        assert_eq!(rank("both-lists.example"), 12);
        assert_eq!(rank("mj-only.example"), 77);
        assert_eq!(rank("unlisted-still.example"), 0);
        assert_eq!(rank("google.com"), 1); // seed coverage intact
        // Provenance: (best, tranco, majestic) — both-lists keeps
        // each list's own number, the seed is Tranco-only.
        assert_eq!(rank_detail("both-lists.example"), (12, 50, 12));
        assert_eq!(rank_detail("mj-only.example"), (77, 0, 77));
        assert_eq!(rank_detail("tied.example"), (20, 20, 20));
        assert_eq!(rank_detail("google.com"), (1, 1, 0));
        // A corrupt sibling override fails the reload — the Qt side
        // sweeps it before retrying.
        std::fs::write(&majestic_path, "<html>404</html>\n").unwrap();
        assert!(reload().is_err());
        assert_eq!(rank("mj-only.example"), 77); // previous table stays
        std::fs::remove_file(&majestic_path).unwrap();
        std::fs::remove_file(&tranco_path).unwrap();
        reload().unwrap();
        assert_eq!(rank("mj-only.example"), 0);
        std::fs::remove_dir_all(&dir).ok();
    }
}
