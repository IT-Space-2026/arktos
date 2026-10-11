//! TRUST05: IDN/homograph lookalike detector.
//!
//! A Unicode confusable-skeleton check over the Tranco popularity
//! table (TRUST04).  The rule is the locked TRUST01 decision: warn
//! ONLY when a host's visual skeleton coincides with a Tranco-popular
//! domain it is not — a benign IDN that resembles no popular domain is
//! clean, so the warning stays rare and meaningful.
//!
//! How it works:
//!
//! 1. The host is canonicalized: xn-- A-labels are punycode-decoded
//!    (RFC 3492, bounded) and every character is case-folded — DNS
//!    is case-insensitive, so folding first is the correct compare
//!    form and also makes the self-match robust to SHOUTED-URL
//!    queries.  A label that fails to decode is kept literally.
//! 2. skeleton() maps each character through the UTS #39
//!    confusables table (data/confusables.txt, generated
//!    confusables_table.rs): Cyrillic а→a, р→p, palochka ӏ→l, digit
//!    1→l, m→"rn" — so "аррӏе" and "paypa1" both skeleton to a
//!    popular ASCII name.  Unmapped characters pass through
//!    unchanged; a skeleton that stays non-ASCII simply never
//!    matches the ASCII-only Tranco index.
//! 3. The Tranco table carries a parallel skeleton index (built
//!    during its parse pass, same rank-ordered first-wins +
//!    poison-safe merge discipline as the rank table): skeleton of
//!    the decoded entry -> raw listed name.  Suffixes of the query
//!    skeleton are probed most-specific-first — the same
//!    registrable-domain walk rank_of performs.
//! 4. On a hit the RAW listed name is compared against the decoded
//!    query suffix at the same label count: equal means the query
//!    *is* that popular domain (clean), different means it is a
//!    visual twin (LOOKALIKE, lookalike_of = the listed name).
//!
//! Honest limits, documented so nobody "fixes" them blindly later:
//!
//! * The skeleton table keeps only mappings that fully resolve to
//!   domain-legal ASCII — a Cyrillic char that resembles a non-ASCII
//!   Latin letter (в→ʙ) can never converge on an ASCII Tranco key
//!   and is dropped at generation time.  Full UTS #39 NFD
//!   normalization is NOT applied; precomposed accented forms whose
//!   confusable target is a base+combining sequence do not reduce
//!   (ü stays ü, not u).  Both shrink recall in exchange for a
//!   zero-dependency table — the high-volume spoof scripts
//!   (Cyrillic, Greek, math-alphanumerics, digit/letter leet) are
//!   covered.
//! * The check is a popularity-weighted WARNING signal for the
//!   TRUST01 composite, not a blocklist — it never vetoes
//!   navigation, and it answers "clean" on any input it cannot
//!   evaluate rather than manufacturing suspicion.
//!
//! Hot-path discipline: the probe runs entirely in fixed stack
//! buffers (no heap allocation); the only allocations are the
//! skeleton index build at list-load time and the verdict String on
//! an actual flag.

use crate::confusables_table::CONFUSABLES;
use std::net::IpAddr;

/// Input/output bounds — a DNS name is <=253 bytes; decoded Unicode
/// and expanded skeletons get proportional headroom and anything past
/// the bound simply declines to evaluate (clean verdict).
const MAX_HOST: usize = 512;
const MAX_DECODED: usize = 512;
const MAX_SKEL: usize = 1024;
const MAX_LABEL_CHARS: usize = 64;

pub enum Verdict {
    /// Nothing to warn about — unranked lookalike shape, the real
    /// popular domain itself, or input the probe could not evaluate.
    Clean,
    /// The host's skeleton coincides with a different Tranco-listed
    /// domain — a homograph/homophone warning candidate.
    Lookalike { lookalike_of: String },
}

/// The verdict plus the side signals TRUST01 reports: whether any
/// confusable/decoded characters were seen at all and whether the
/// decoded name mixes scripts (a cheap corroborating signal).
pub struct Probe {
    pub verdict: Verdict,
    pub mixed_script: bool,
    /// The computed skeleton when it differs from the decoded host —
    /// empty when the name had no confusable/decodable characters.
    pub skeleton: String,
}

// ---- confusable skeleton --------------------------------------------

fn confusable(cp: u32) -> Option<&'static [u8]> {
    CONFUSABLES
        .binary_search_by_key(&cp, |e| e.0)
        .ok()
        .map(|i| CONFUSABLES[i].1)
}

/// The case-folded, punycode-decoded form of `host` written into
/// `out` (UTF-8).  Returns bytes written, or None when the result
/// overflows.  Labels starting xn-- decode through RFC 3492; a label
/// that fails to decode is copied literally so one bad label can't
/// sink the whole name.
fn decoded_host_into(host: &str, out: &mut [u8]) -> Option<usize> {
    let mut n = 0usize;
    let mut first = true;
    for label in host.split('.') {
        if !first {
            if n >= out.len() {
                return None;
            }
            out[n] = b'.';
            n += 1;
        }
        first = false;
        let mut chars = [0u32; MAX_LABEL_CHARS];
        let lb = label.as_bytes();
        let decoded: Option<usize> = if lb.len() > 4
            && lb[..4].eq_ignore_ascii_case(b"xn--")
        {
            punycode_decode(&lb[4..], &mut chars)
        } else {
            None
        };
        match decoded {
            Some(count) => {
                for &cp in &chars[..count] {
                    let c = char::from_u32(cp)?;
                    n = push_folded(c, out, n)?;
                }
            }
            None => {
                for c in label.chars() {
                    n = push_folded(c, out, n)?;
                }
            }
        }
    }
    Some(n)
}

/// Case-folded UTF-8 emission — char::to_lowercase may yield more
/// than one char (İ -> i̇); each is appended in order.
fn push_folded(c: char, out: &mut [u8], mut n: usize) -> Option<usize> {
    let mut buf = [0u8; 4];
    for lc in c.to_lowercase() {
        let b = lc.encode_utf8(&mut buf).as_bytes();
        if n + b.len() > out.len() {
            return None;
        }
        out[n..n + b.len()].copy_from_slice(b);
        n += b.len();
    }
    Some(n)
}

/// The confusable skeleton of an already case-folded host written
/// into `out`.  Each char maps through CONFUSABLES or passes through
/// unchanged; dots survive untouched so label boundaries in the
/// skeleton line up with label boundaries in the decoded host.
/// Returns bytes written or None on overflow.
pub(crate) fn skeleton_into(decoded: &str, out: &mut [u8]) -> Option<usize> {
    let mut n = 0usize;
    for c in decoded.chars() {
        match confusable(c as u32) {
            Some(t) => {
                if n + t.len() > out.len() {
                    return None;
                }
                out[n..n + t.len()].copy_from_slice(t);
                n += t.len();
            }
            None => {
                let mut buf = [0u8; 4];
                let b = c.encode_utf8(&mut buf).as_bytes();
                if n + b.len() > out.len() {
                    return None;
                }
                out[n..n + b.len()].copy_from_slice(b);
                n += b.len();
            }
        }
    }
    Some(n)
}

// ---- RFC 3492 punycode decode ----------------------------------------

const PUNY_BASE: u32 = 36;
const PUNY_TMIN: u32 = 1;
const PUNY_TMAX: u32 = 26;
const PUNY_SKEW: u32 = 38;
const PUNY_DAMP: u32 = 700;
const PUNY_INITIAL_BIAS: u32 = 72;
const PUNY_INITIAL_N: u32 = 0x80;

fn puny_adapt(mut delta: u32, numpoints: u32, first: bool) -> u32 {
    delta /= if first { PUNY_DAMP } else { 2 };
    delta += delta / numpoints;
    let mut k = 0;
    while delta > (PUNY_BASE - PUNY_TMIN) * PUNY_TMAX / 2 {
        delta /= PUNY_BASE - PUNY_TMIN;
        k += PUNY_BASE;
    }
    k + (PUNY_BASE - PUNY_TMIN + 1) * delta / (delta + PUNY_SKEW)
}

fn puny_digit(b: u8) -> Option<u32> {
    match b {
        b'a'..=b'z' => Some((b - b'a') as u32),
        b'A'..=b'Z' => Some((b - b'A') as u32),
        b'0'..=b'9' => Some((b - b'0' + 26) as u32),
        _ => None,
    }
}

/// Decode the punycode body of one xn-- label into `chars`.
/// Returns the codepoint count, or None on any malformed/overflowing
/// input — the caller then treats the label literally.
fn punycode_decode(body: &[u8], chars: &mut [u32]) -> Option<usize> {
    let mut len = 0usize;
    let mut rest = body;
    // The basic codepoints sit before the last delimiter.
    if let Some(pos) = body.iter().rposition(|&b| b == b'-') {
        // The basic segment is plain ASCII — hyphens inside it are
        // literal domain characters, not delimiters.
        for &b in &body[..pos] {
            if !b.is_ascii() || len >= chars.len() {
                return None;
            }
            chars[len] = b as u32;
            len += 1;
        }
        rest = &body[pos + 1..];
    }
    let (mut n, mut i, mut bias) = (PUNY_INITIAL_N, 0u32, PUNY_INITIAL_BIAS);
    let mut idx = 0usize;
    while idx < rest.len() {
        let oldi = i;
        let (mut k, mut w) = (PUNY_BASE, 1u32);
        loop {
            if idx >= rest.len() {
                return None;
            }
            let digit = puny_digit(rest[idx])?;
            idx += 1;
            i = i.checked_add(digit.checked_mul(w)?)?;
            let t = if k <= bias {
                PUNY_TMIN
            } else if k >= bias + PUNY_TMAX {
                PUNY_TMAX
            } else {
                k - bias
            };
            if digit < t {
                break;
            }
            w = w.checked_mul(PUNY_BASE - t)?;
            k += PUNY_BASE;
        }
        let total = len as u32 + 1;
        bias = puny_adapt(i.checked_sub(oldi)?, total, oldi == 0);
        n = n.checked_add(i / total)?;
        i %= total;
        if n > 0x10FFFF || len >= chars.len() {
            return None;
        }
        // Insert at position i like the spec — char::from_u32
        // additionally rejects surrogates.
        char::from_u32(n)?;
        chars.copy_within((i as usize)..len, i as usize + 1);
        chars[i as usize] = n;
        len += 1;
        i += 1;
    }
    Some(len)
}

// ---- script mix -------------------------------------------------------

/// Coarse script bucket for the mixed-script side signal — digits,
/// separators and punctuation are neutral.
fn script_of(c: char) -> u8 {
    match c as u32 {
        0x0061..=0x007A | 0x00C0..=0x024F | 0x1E00..=0x1EFF => 1, // Latin
        0x0400..=0x052F | 0x2DE0..=0x2DFF | 0xA640..=0xA69F => 2, // Cyrillic
        0x0370..=0x03FF | 0x1F00..=0x1FFF => 3,                  // Greek
        0x0530..=0x058F => 4,                                   // Armenian
        _ => 0,
    }
}

fn mixed_script(decoded: &str) -> bool {
    // Per-label, not per-name — a single-script IDN always ends in a
    // Latin TLD, so only label-INTERNAL script mixing is the spoof
    // signal (aррӏе with a Latin a is the classic shape).
    decoded.split('.').any(|label| {
        let mut mask = 0u32;
        for c in label.chars() {
            let s = script_of(c);
            if s != 0 {
                mask |= 1 << s;
            }
        }
        mask.count_ones() > 1
    })
}

/// The skeleton index key for a listed domain — decoded, case-folded
/// and skeletonized, the same canonicalization the query side
/// applies.  Load-path only (it allocates); the probe itself stays
/// alloc-free.  None when the name overflows the canonical buffers.
pub(crate) fn entry_skeleton(host: &str) -> Option<String> {
    let mut dbuf = [0u8; MAX_DECODED];
    let dn = decoded_host_into(host, &mut dbuf)?;
    let decoded = std::str::from_utf8(&dbuf[..dn]).ok()?;
    let mut sbuf = [0u8; MAX_SKEL];
    let sn = skeleton_into(decoded, &mut sbuf)?;
    std::str::from_utf8(&sbuf[..sn])
        .ok()
        .map(|s| s.to_string())
}

/// The suffix of `decoded` spanning its last `labels` dot-separated
/// labels — the registrable-candidate slice the self-match compares.
fn last_labels(decoded: &str, labels: usize) -> &str {
    let mut count = 1usize;
    for (i, b) in decoded.bytes().enumerate().rev() {
        if b == b'.' {
            count += 1;
            if count > labels {
                return &decoded[i + 1..];
            }
        }
    }
    decoded
}

// ---- the probe --------------------------------------------------------

/// The alloc-free hot path.  Canonicalize -> skeleton -> walk the
/// skeleton's label boundaries most-specific-first against the Tranco
/// skeleton index; a hit either self-matches (the query IS that
/// listed domain -> clean) or names the popular domain it resembles.
pub fn probe(host: &str) -> Probe {
    let h = host.trim().trim_end_matches('.');
    let clean = |mixed_script, skeleton: String| Probe {
        verdict: Verdict::Clean,
        mixed_script,
        skeleton,
    };
    if h.is_empty() || h.len() > MAX_HOST || h.parse::<IpAddr>().is_ok() {
        return clean(false, String::new());
    }
    let mut dbuf = [0u8; MAX_DECODED];
    let dn = match decoded_host_into(h, &mut dbuf) {
        Some(n) => n,
        None => return clean(false, String::new()),
    };
    let decoded = match std::str::from_utf8(&dbuf[..dn]) {
        Ok(s) => s,
        Err(_) => return clean(false, String::new()),
    };
    // Tranco ranks domains only — a dotless name has no registrable
    // candidate to compare.
    if !decoded.contains('.') {
        return clean(mixed_script(decoded), String::new());
    }
    let mixed = mixed_script(decoded);
    let mut sbuf = [0u8; MAX_SKEL];
    let sn = match skeleton_into(decoded, &mut sbuf) {
        Some(n) => n,
        None => return clean(mixed, String::new()),
    };
    let skel = match std::str::from_utf8(&sbuf[..sn]) {
        Ok(s) => s,
        Err(_) => return clean(mixed, String::new()),
    };
    let differs = skel != decoded;
    let list = crate::tranco::active_list();
    let mut candidate = skel;
    loop {
        // Exact then each parent — the same registrable-domain walk
        // rank_of performs; single-label suffixes are never entries.
        if let Some(raw) = list.skeleton_match(candidate) {
            let labels = candidate.bytes().filter(|&b| b == b'.').count() + 1;
            let mut rbuf = [0u8; MAX_DECODED];
            let self_match = decoded_host_into(raw, &mut rbuf)
                .and_then(|rn| std::str::from_utf8(&rbuf[..rn]).ok())
                .map(|rd| rd == last_labels(decoded, labels))
                .unwrap_or(false);
            if self_match {
                return clean(mixed, String::new());
            }
            return Probe {
                verdict: Verdict::Lookalike {
                    lookalike_of: raw.to_string(),
                },
                mixed_script: mixed,
                skeleton: if differs { skel.to_string() } else { String::new() },
            };
        }
        match candidate.find('.') {
            Some(i) => candidate = &candidate[i + 1..],
            None => break,
        }
        if !candidate.contains('.') {
            break;
        }
    }
    clean(
        mixed,
        if differs {
            skel.to_string()
        } else {
            String::new()
        },
    )
}

/// JSON verdict for the FFI boundary — the shape TRUST01's composite
/// consumes.
pub fn verdict_json(host: &str) -> String {
    let p = probe(host);
    let mut j = String::with_capacity(128);
    j.push_str("{\"verdict\":\"");
    match &p.verdict {
        Verdict::Clean => j.push_str("clean\""),
        Verdict::Lookalike { lookalike_of } => {
            j.push_str("lookalike\",\"lookalike_of\":\"");
            for c in lookalike_of.chars() {
                match c {
                    '"' | '\\' => {
                        j.push('\\');
                        j.push(c);
                    }
                    c if (c as u32) < 0x20 => {}
                    c => j.push(c),
                }
            }
            j.push('"');
        }
    }
    j.push_str(",\"mixed_script\":");
    j.push_str(if p.mixed_script { "true" } else { "false" });
    j.push_str(",\"skeleton\":\"");
    for c in p.skeleton.chars() {
        match c {
            '"' | '\\' => {
                j.push('\\');
                j.push(c);
            }
            c if (c as u32) < 0x20 => {}
            c => j.push(c),
        }
    }
    j.push_str("\"}");
    j
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::MutexGuard;

    // The active Tranco table is process-global — serialize on the
    // shared store lock like the other module tests do.
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn decoded(host: &str) -> String {
        let mut buf = [0u8; MAX_DECODED];
        let n = decoded_host_into(host, &mut buf).unwrap();
        String::from_utf8_lossy(&buf[..n]).into_owned()
    }

    fn skeleton(host: &str) -> String {
        let mut dbuf = [0u8; MAX_DECODED];
        let dn = decoded_host_into(host, &mut dbuf).unwrap();
        let mut sbuf = [0u8; MAX_SKEL];
        let sn = skeleton_into(
            std::str::from_utf8(&dbuf[..dn]).unwrap(),
            &mut sbuf,
        )
        .unwrap();
        String::from_utf8_lossy(&sbuf[..sn]).into_owned()
    }

    fn lookalike_of(host: &str) -> Option<String> {
        match probe(host).verdict {
            Verdict::Lookalike { lookalike_of } => Some(lookalike_of),
            Verdict::Clean => None,
        }
    }

    #[test]
    fn punycode_decodes_rfc3492_vectors() {
        assert_eq!(decoded("xn--bcher-kva.de"), "bücher.de");
        assert_eq!(decoded("xn--mnchen-3ya.de"), "münchen.de");
        assert_eq!(decoded("xn--80ak6aa92e.com"), "аррӏе.com");
        // Case-insensitive A-label; basic segment lowercases too.
        assert_eq!(decoded("XN--BCHER-KVA.de"), "bücher.de");
        // Not an A-label / malformed bodies stay literal.
        assert_eq!(decoded("xn--.de"), "xn--.de");
        assert_eq!(decoded("xn--!.de"), "xn--!.de");
        assert_eq!(decoded("plain.de"), "plain.de");
    }

    #[test]
    fn skeleton_maps_confusables_and_passes_plain() {
        // The table's canonical targets apply everywhere — including
        // m -> "rn", so every *.com skeletons to *.corn.  Both sides
        // of the comparison transform identically, which is what
        // makes the index work.
        assert_eq!(skeleton("apple.com"), "apple.corn");
        assert_eq!(skeleton("аррӏе.com"), "apple.corn"); // Cyrillic a,p,p,l,e
        assert_eq!(skeleton("paypa1.com"), "paypal.corn"); // digit 1 -> l
        assert_eq!(skeleton("paypaI.com"), "paypai.corn"); // folded i, not l
        assert_eq!(skeleton("mozilla.com"), "rnozilla.corn");
        assert_eq!(skeleton("rnozilla.com"), "rnozilla.corn"); // converges
        assert_eq!(skeleton("g00gle.com"), "google.corn"); // 0 -> o
        assert_eq!(skeleton("WWW.APPLE.COM"), "www.apple.corn");
        // Unmapped non-ASCII stays — it can never match an ASCII key.
        assert!(skeleton("bücher.de").contains("ü"));
    }

    #[test]
    fn mixed_script_flags_cross_script_labels() {
        // Label-internal mixing only — a pure-Cyrillic name under a
        // Latin TLD is not itself a mixed-script signal.
        assert!(!mixed_script("аррӏе.com"));
        assert!(!mixed_script("аррӏе.сом"));
        // Latin 'a' hiding inside a Cyrillic label is the classic mix.
        assert!(mixed_script("aррӏе.com"));
        assert!(!mixed_script("apple.com"));
    }

    #[test]
    fn probe_flags_only_popular_lookalikes() {
        let _g = guard();
        crate::tranco::load(
            "1,apple.com\n\
             2,paypal.com\n\
             3,mozilla.com\n\
             4,123.com\n\
             5,xn--mnchen-3ya.de\n\
             6,evil.com\n",
        )
        .unwrap();

        // Unicode and A-label forms of the same homograph both flag.
        assert_eq!(lookalike_of("аррӏе.com").as_deref(), Some("apple.com"));
        assert_eq!(
            lookalike_of("xn--80ak6aa92e.com").as_deref(),
            Some("apple.com")
        );
        // Subdomain of a lookalike still flags.
        assert_eq!(
            lookalike_of("www.аррӏе.com").as_deref(),
            Some("apple.com")
        );
        // ASCII leetspeak twins of popular domains.
        assert_eq!(lookalike_of("paypa1.com").as_deref(), Some("paypal.com"));
        assert_eq!(lookalike_of("rnozilla.com").as_deref(), Some("mozilla.com"));
        assert_eq!(lookalike_of("l23.com").as_deref(), Some("123.com"));

        // The real domains — including under subdomains and caps —
        // self-match and stay clean.
        for h in [
            "apple.com",
            "www.apple.com",
            "APPLE.COM",
            "paypal.com",
            "mozilla.com",
        ] {
            assert_eq!(lookalike_of(h), None, "{}", h);
        }
        // The real IDN entry self-matches in either spelling.
        assert_eq!(lookalike_of("münchen.de"), None);
        assert_eq!(lookalike_of("xn--mnchen-3ya.de"), None);
        // A subdomain trick resolves to its own registrable domain.
        assert_eq!(lookalike_of("apple.com.evil.com"), None);
        // Benign IDN resembling no popular domain -> clean.
        assert_eq!(lookalike_of("bücher.de"), None);
        assert_eq!(lookalike_of("xn--bcher-kva.de"), None);
        // Unranked ASCII near-misses -> clean.
        assert_eq!(lookalike_of("appie.com"), None); // skeleton unranked
        assert_eq!(lookalike_of("paypai.com"), None); // i is not confusable
    }

    #[test]
    fn probe_degrades_on_malformed_input() {
        let _g = guard();
        crate::tranco::load("1,apple.com\n").unwrap();
        for h in ["", "   ", "localhost", "1.2.3.4", "[::1]"] {
            assert!(matches!(probe(h).verdict, Verdict::Clean), "{}", h);
        }
        // Oversized input declines to evaluate.
        assert!(matches!(
            probe(&"a".repeat(600)).verdict,
            Verdict::Clean
        ));
    }

    #[test]
    fn vendored_seed_flags_real_homographs() {
        // End-to-end against the shipped top-100k seed: a Cyrillic
        // homograph of a genuinely popular domain must flag through
        // the real index build, and the real domain must stay clean.
        let _g = guard();
        crate::tranco::reload().unwrap();
        assert_eq!(lookalike_of("аррӏе.com").as_deref(), Some("apple.com"));
        assert_eq!(lookalike_of("apple.com"), None);
        assert_eq!(lookalike_of("google.com"), None);
    }

    #[test]
    fn verdict_json_reports_signals() {
        let _g = guard();
        crate::tranco::load("1,apple.com\n").unwrap();
        let j = verdict_json("аррӏе.com");
        assert!(j.contains("\"verdict\":\"lookalike\""));
        assert!(j.contains("\"lookalike_of\":\"apple.com\""));
        let j = verdict_json("apple.com");
        assert!(j.contains("\"verdict\":\"clean\""));
        assert!(j.contains("\"mixed_script\":false"));
    }
}
