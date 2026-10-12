//! TRUST01: the composite site-trust verdict — a WOT-style trust
//! signal consumed by the lock-icon badge and popover (TRUST08/09
//! on the Qt side).  It NEVER blocks: every output is advisory.
//!
//! Inputs, all already rustcore-resident:
//!   * blocklist membership      — blocklist::check(host)
//!   * Tranco rank               — tranco::rank(host)
//!   * homograph lookalike probe — lookalike::probe(host)
//!   * https state               — the caller passes it in
//!   * first-seen                — the history store's host rollup
//!
//! Locked level mapping:
//!   threat-list hit or a confusable∩popular twin  -> "red"
//!   plaintext http or unknown Tranco rank         -> "amber"
//!   Tranco-known + https                          -> "green"
//!
//! "first_seen" is reported in signals[] truthfully but never
//! decides the level alone — the locked "feeds the score QUIETLY"
//! decision: a Tranco-known https site stays green on the very
//! first visit, and an unrated site is already amber on the rank
//! signal whether it was visited or not.  The FFI signature
//! (host, is_https) cannot see "credential-bearing page", so
//! plaintext http conservatively ambers every host — a caller
//! that knows a page is trivial may soften the display.
//!
//! Output JSON:
//!   {"level":"green|amber|red","signals":[...],"rank":N,
//!    "lookalike_of":"..."}
//! signals[] reports each input that evaluated true — "threat",
//! "lookalike", "rank" (rank>0), "first_seen" (no prior visit).
//! "lookalike_of" is present only when the lookalike probe fired.
//!
//! Hot-path discipline: the blocklist/rank/lookalike probes are the
//! crate's alloc-free hot paths; the first-seen check is one
//! indexed SQLite query per candidate suffix against the hosts
//! rollup — never a visit-log scan.  The only allocation is the
//! verdict string itself.

use crate::error::RcResult;
use crate::{blocklist, history, lookalike, tranco, util};

/// Milliseconds since the Unix epoch — the probe's notion of "now".
fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

/// Probe-selection bits for `lookup_opts` — TRUST03's per-signal
/// Preferences switches.  A cleared bit skips that probe ENTIRELY:
/// it never runs, so it cannot fire and cannot appear in signals[].
/// The rank and first-seen inputs are ungated — they carry no
/// per-signal switch.
pub const PROBE_THREATLIST: u32 = 0x1;
pub const PROBE_LOOKALIKE: u32 = 0x2;
/// Every probe on — the plain rc_sitetrust_lookup request.
pub const PROBE_ALL: u32 = PROBE_THREATLIST | PROBE_LOOKALIKE;

/// rc_sitetrust_lookup: the verdict as a JSON string the FFI
/// marshals verbatim.  Never fails on content — an empty or
/// un-evaluable host naturally produces {"level":"amber",
/// "signals":[]}: nothing listed it, nothing ranked it.
pub fn lookup(host: &str, is_https: bool) -> String {
    lookup_opts(host, is_https, PROBE_ALL)
}

/// rc_sitetrust_lookup_opts: like lookup() but `probes` selects
/// which advisory inputs run (PROBE_* bits) — the threat-list and
/// lookalike checks each answer their own Preferences switch.
pub fn lookup_opts(host: &str, is_https: bool, probes: u32) -> String {
    lookup_at(host, is_https, now_ms(), probes)
}

fn lookup_at(host: &str, is_https: bool, now: i64, probes: u32) -> String {
    let threat =
        probes & PROBE_THREATLIST != 0 && blocklist::check(host);
    let rank = tranco::rank(host);
    // A skipped probe evaluates clean — it must not synthesize a
    // signal the switch was meant to suppress.
    let probe = if probes & PROBE_LOOKALIKE != 0 {
        lookalike::probe(host)
    } else {
        lookalike::Probe {
            verdict: lookalike::Verdict::Clean,
            mixed_script: false,
            skeleton: String::new(),
        }
    };
    // An unopened/erroring history store reads as "seen" — a signal
    // the store cannot evaluate must not be claimed (the popover
    // would show "first visit" on every page while the DB is down).
    let seen =
        history::with(|s| s.host_seen(host, now).unwrap_or(true));

    let lookalike_of = match probe.verdict {
        lookalike::Verdict::Lookalike { lookalike_of } => Some(lookalike_of),
        lookalike::Verdict::Clean => None,
    };

    let level = if threat || lookalike_of.is_some() {
        "red"
    } else if !is_https || rank == 0 {
        "amber"
    } else {
        "green"
    };

    let mut j = String::with_capacity(160);
    j.push_str("{\"level\":\"");
    j.push_str(level);
    j.push_str("\",\"signals\":[");
    let mut first = true;
    let mut signal = |j: &mut String, name: &str| {
        if !first {
            j.push(',');
        }
        first = false;
        util::json_push_str(j, name);
    };
    if threat {
        signal(&mut j, "threat");
    }
    if lookalike_of.is_some() {
        signal(&mut j, "lookalike");
    }
    if rank > 0 {
        signal(&mut j, "rank");
    }
    if !seen {
        signal(&mut j, "first_seen");
    }
    j.push_str("],\"rank\":");
    j.push_str(&rank.to_string());
    if let Some(twin) = &lookalike_of {
        j.push_str(",\"lookalike_of\":");
        util::json_push_str(&mut j, twin);
    }
    j.push('}');
    j
}

/// rc_sitetrust_reload: re-runs both input-table merges — the list
/// updaters each drop a file before calling.  A failure in one must
/// not leave the other stale, so both reloads run and the first
/// error is reported.
pub fn reload() -> RcResult<()> {
    let r1 = blocklist::reload();
    let r2 = tranco::reload();
    r1.and(r2)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::MutexGuard;

    // Blocklist/Tranco/history are all process-global — serialize on
    // the shared store lock like the other module tests do.
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn temp_db(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "arora-sitetrust-{}-{}-{}",
            tag,
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        dir.join("history.db").to_string_lossy().into_owned()
    }

    /// Swaps in fixture lists and an open history store; returns the
    /// db path so the test can seed visits through the FFI-shaped
    /// HistoryStore calls.
    fn fixture(tag: &str, tranco_rows: &str, blocklist_rows: &str) -> String {
        crate::tranco::load(tranco_rows).unwrap();
        crate::blocklist::load(blocklist_rows).unwrap();
        let path = temp_db(tag);
        history::with(|s| s.open(Some(&path))).unwrap();
        path
    }

    fn visit(url: &str, ts: i64) {
        history::with(|s| s.add(url, "", ts)).unwrap();
    }

    #[test]
    fn level_mapping_matrix() {
        let _g = guard();
        let db = fixture(
            "matrix",
            "3,popular.example\n",
            "threat.example\n",
        );
        let now = now_ms();

        // Threat-list hit -> red, "threat" fired (first_seen too —
        // the host was never visited).
        let j: serde_json::Value =
            serde_json::from_str(&lookup_at("threat.example", true, now, PROBE_ALL))
                .unwrap();
        assert_eq!(j["level"], "red");
        assert_eq!(
            j["signals"],
            serde_json::json!(["threat", "first_seen"])
        );
        assert_eq!(j["rank"], 0);

        // Tranco-known + https -> green; never visited -> first_seen
        // rides along quietly without demoting the level.
        let j: serde_json::Value =
            serde_json::from_str(&lookup_at("popular.example", true, now, PROBE_ALL))
                .unwrap();
        assert_eq!(j["level"], "green");
        assert_eq!(
            j["signals"],
            serde_json::json!(["rank", "first_seen"])
        );
        assert_eq!(j["rank"], 3);

        // Same site over plaintext http -> amber.
        let j: serde_json::Value =
            serde_json::from_str(&lookup_at("popular.example", false, now, PROBE_ALL))
                .unwrap();
        assert_eq!(j["level"], "amber");
        assert!(j["signals"]
            .as_array()
            .unwrap()
            .contains(&serde_json::json!("rank")));

        // Unknown rank -> amber even on https.
        let j: serde_json::Value =
            serde_json::from_str(&lookup_at("obscure.example", true, now, PROBE_ALL))
                .unwrap();
        assert_eq!(j["level"], "amber");
        assert_eq!(
            j["signals"],
            serde_json::json!(["first_seen"])
        );

        std::fs::remove_dir_all(
            std::path::Path::new(&db).parent().unwrap(),
        )
        .ok();
    }

    #[test]
    fn lookalike_of_popular_domain_is_red() {
        let _g = guard();
        let db = fixture("twin", "1,paypal.com\n", "other.invalid\n");
        let now = now_ms();

        let j: serde_json::Value =
            serde_json::from_str(&lookup_at("paypa1.com", true, now, PROBE_ALL))
                .unwrap();
        assert_eq!(j["level"], "red");
        assert_eq!(
            j["signals"],
            serde_json::json!(["lookalike", "first_seen"])
        );
        assert_eq!(j["lookalike_of"], "paypal.com");

        // The real domain stays green.
        let j: serde_json::Value =
            serde_json::from_str(&lookup_at("paypal.com", true, now, PROBE_ALL))
                .unwrap();
        assert_eq!(j["level"], "green");
        assert!(j.get("lookalike_of").is_none());

        std::fs::remove_dir_all(
            std::path::Path::new(&db).parent().unwrap(),
        )
        .ok();
    }

    #[test]
    fn first_seen_signal_tracks_prior_visits() {
        let _g = guard();
        let db = fixture("seen", "5,site.example\n", "other.invalid\n");
        let now = now_ms();

        let signals = |host: &str| -> Vec<String> {
            let j: serde_json::Value =
                serde_json::from_str(&lookup_at(host, true, now, PROBE_ALL)).unwrap();
            j["signals"]
                .as_array()
                .unwrap()
                .iter()
                .map(|v| v.as_str().unwrap().to_string())
                .collect()
        };

        assert!(signals("site.example").contains(&"first_seen".to_string()));

        // A lone just-committed visit does not count as prior
        // knowledge — the panel opened on a first visit still
        // reports first_seen.
        visit("https://site.example/", now - 1000);
        assert!(signals("site.example").contains(&"first_seen".to_string()));

        // A visit older than the grace window is prior knowledge.
        visit("https://older.example/", now - 60 * 60_000);
        assert!(!signals("older.example").contains(&"first_seen".to_string()));

        // A second visit counts on its own — returning within the
        // grace window is still familiarity.
        visit("https://repeat.example/", now - 1000);
        visit("https://repeat.example/", now - 500);
        assert!(!signals("repeat.example").contains(&"first_seen".to_string()));

        // Visiting the parent domain covers the subdomain probe —
        // the same registrable walk rank_of performs.
        visit("https://parent.example/", now - 60 * 60_000);
        assert!(!signals("www.parent.example")
            .contains(&"first_seen".to_string()));

        std::fs::remove_dir_all(
            std::path::Path::new(&db).parent().unwrap(),
        )
        .ok();
    }

    #[test]
    fn reload_runs_both_tables() {
        let _g = guard();
        let db = fixture("reload", "9,ranked.example\n", "baddy.example\n");
        // Both reload paths keep working through the composite — the
        // vendored seeds re-merge on top of the fixture loads.
        reload().unwrap();
        assert!(tranco::rank("google.com") > 0);
        assert!(blocklist::count() > 0);
        std::fs::remove_dir_all(
            std::path::Path::new(&db).parent().unwrap(),
        )
        .ok();
    }

    // TRUST03: a cleared probe bit must suppress the signal outright
    // — the Preferences switches behind rc_sitetrust_lookup_opts.
    #[test]
    fn probe_flags_gate_their_signals() {
        let _g = guard();
        let db = fixture(
            "flags",
            "1,paypal.com\n",
            "threat-flag.example\n",
        );
        let now = now_ms();

        // Threat-list probe off -> the listed host cannot flag; the
        // level drops to what the remaining inputs say (unranked ->
        // amber) and "threat" never appears.
        let j: serde_json::Value = serde_json::from_str(&lookup_at(
            "threat-flag.example",
            true,
            now,
            PROBE_ALL & !PROBE_THREATLIST,
        ))
        .unwrap();
        assert_eq!(j["level"], "amber");
        assert_eq!(j["signals"], serde_json::json!(["first_seen"]));

        // Lookalike probe off -> the confusable twin cannot flag
        // either: no "lookalike" signal, no lookalike_of, amber.
        let j: serde_json::Value = serde_json::from_str(&lookup_at(
            "paypa1.com",
            true,
            now,
            PROBE_ALL & !PROBE_LOOKALIKE,
        ))
        .unwrap();
        assert_eq!(j["level"], "amber");
        assert_eq!(j["signals"], serde_json::json!(["first_seen"]));
        assert!(j.get("lookalike_of").is_none());

        // Both off -> only the ungated inputs (rank, first_seen)
        // remain; the same host over the full mask stays red as the
        // all-on control.
        let j: serde_json::Value = serde_json::from_str(&lookup_at(
            "threat-flag.example",
            true,
            now,
            0,
        ))
        .unwrap();
        assert_eq!(j["level"], "amber");
        let j: serde_json::Value = serde_json::from_str(&lookup_at(
            "threat-flag.example",
            true,
            now,
            PROBE_ALL,
        ))
        .unwrap();
        assert_eq!(j["level"], "red");
        let j: serde_json::Value = serde_json::from_str(&lookup_at(
            "paypa1.com", true, now, PROBE_ALL,
        ))
        .unwrap();
        assert_eq!(j["level"], "red");

        std::fs::remove_dir_all(
            std::path::Path::new(&db).parent().unwrap(),
        )
        .ok();
    }

    #[test]
    fn unopened_history_never_claims_first_seen() {
        let _g = guard();
        crate::tranco::load("1,ranked.example\n").unwrap();
        crate::blocklist::load("other.invalid\n").unwrap();
        // Stand the shared store in the "never opened" state — a
        // signal it cannot evaluate must not be claimed.
        history::with(|s| s.close_for_test());
        let j: serde_json::Value = serde_json::from_str(&lookup_at(
            "never-db.example",
            true,
            now_ms(),
            PROBE_ALL,
        ))
        .unwrap();
        assert_eq!(j["level"], "amber");
        // first_seen is absent — the store could not evaluate it.
        assert_eq!(j["signals"], serde_json::json!([]));
    }
}
