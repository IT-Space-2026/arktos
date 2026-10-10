//! CLR01: the clear-private-data decision engine.
//!
//! "Which stores get wiped for a clear request" was the body of
//! ClearPrivateData::accept() (the seven-checkbox dialog) and
//! BrowserApplication::clearPrivateDataOnExit() (the opt-in exit
//! wipe) — two hand-maintained copies of the same store fan-out.
//! The stores themselves are increasingly Rust-owned (history.db,
//! sessions, per-site decisions), so the decision moves here as a
//! pure function; the Qt shell marshals (period, categories, scope)
//! in and executes the ordered operation list it gets back.  The
//! core owns no data for this feature — every op is an instruction
//! to the adapters that do the wiping.
//!
//! Two scope rules the plan bakes in:
//!   * "persistent" — the normal profile plus every materialized
//!     container profile and the shell's persistent stores.  The
//!     off-the-record profile is deliberately absent: a persistent
//!     clear must not reach into a live private session.
//!   * "otr" — the live off-the-record context only: its in-memory
//!     visited links, cookie store, DOM storage and http cache, plus
//!     the private rows of the download list.  The persistent stores
//!     an OTR session never wrote — history, icons, search history,
//!     sessions, container trees, the disk cache — get NO ops, so a
//!     private-window clear can never touch the profile it shares
//!     the process with.
//!
//! Periods: "all" clears whole stores; "hour"/"day"/"week" clear a
//! recent window.  Ops carry "since_secs" when a window applies and
//! "ranged" only where the backing store supports a time-scoped
//! delete — today that is the history visit log alone (the engine's
//! cookie/cache/storage APIs are all-or-nothing), so ranged-unable
//! ops are additionally marked "degraded" and the executor wipes
//! them whole.  Over-clearing is the honest failure direction for a
//! privacy wipe; the UI exposes no period picker yet — the window
//! vocabulary is wired so the decision is ready when one lands.

use serde_json::{json, Value};

use crate::error::{Fail, RcResult, RcStatus};

// Category bits — identical values to rustcore.h's RC_CLEAR_* and the
// Qt adapter's ClearPrivateData::ClearCategory enum (pinned by a
// static_assert on the C++ side and by the parity corpus here).
pub const CAT_HISTORY: u32 = 0x01;
pub const CAT_DOWNLOADS: u32 = 0x02;
pub const CAT_SEARCH: u32 = 0x04;
pub const CAT_COOKIES: u32 = 0x08;
pub const CAT_SITEDATA: u32 = 0x10;
pub const CAT_CACHE: u32 = 0x20;
pub const CAT_ICONS: u32 = 0x40;
pub const CAT_SESSION: u32 = 0x80;
/// Deep storage wipe — the exit-time "clear everything" scheduling
/// (deferred storage-tree removal, unmaterialized-container tree
/// removal).  No dialog checkbox maps to it; the on-exit path passes
/// it explicitly.
pub const CAT_FULL_WIPE: u32 = 0x100;

/// The period vocabulary: normalized name + window length in seconds
/// (0 = the whole store).  None on an unrecognized name — the FFI
/// maps it to RC_INVALID_ARGUMENT rather than guessing at a window.
fn parse_period(period: &str) -> Option<(&'static str, i64)> {
    match period.trim().to_ascii_lowercase().as_str() {
        "" | "all" | "everything" => Some(("all", 0)),
        "hour" | "last-hour" => Some(("hour", 3_600)),
        "day" | "today" | "last-day" => Some(("day", 86_400)),
        "week" | "last-week" => Some(("week", 604_800)),
        _ => None,
    }
}

/// One operation row.  `scope` marks ops that run against the
/// profiles of one browsing context ("persistent" = normal +
/// materialized containers, "otr" = the live off-the-record
/// profile); ops without it are shell stores the executor reaches
/// directly.  `since_secs` is the requested window (0 = whole
/// store); `ranged` is true only where the backing store can honor
/// it — a ranged request on a ranged:false store is flagged
/// "degraded" and executes as a full wipe.
fn op(name: &str, scope: Option<&str>, since_secs: i64, ranged: bool) -> Value {
    let mut o = json!({"op": name, "since_secs": since_secs, "ranged": ranged});
    if let Some(s) = scope {
        o["scope"] = json!(s);
    }
    if since_secs > 0 && !ranged {
        o["degraded"] = json!(true);
    }
    o
}

/// The off-the-record plan: only stores the private context actually
/// owns.  Everything persistent is absent by construction — that is
/// the de-scoping guarantee, not an oversight.
fn otr_ops(categories: u32, since_secs: i64) -> Vec<Value> {
    let mut ops = Vec::new();
    if categories & CAT_HISTORY != 0 {
        // The OTR profile never writes the app history store; what
        // exists is Chromium's in-memory visited-link set.
        ops.push(op("visited_links.clear", Some("otr"), since_secs, false));
    }
    if categories & CAT_DOWNLOADS != 0 {
        // Only the private rows — the shared download list keeps the
        // persistent session's entries.
        ops.push(op("downloads.clear_otr", None, since_secs, false));
    }
    if categories & CAT_COOKIES != 0 {
        ops.push(op("cookies.clear", Some("otr"), since_secs, false));
    }
    if categories & CAT_SITEDATA != 0 {
        // In-memory DOM storage: a live-page sweep, nothing to defer
        // (an unnamed profile persists no trees).
        ops.push(op("sitedata.page_sweep", Some("otr"), since_secs, false));
    }
    if categories & CAT_CACHE != 0 {
        ops.push(op("cache.clear", Some("otr"), since_secs, false));
    }
    // CAT_SEARCH, CAT_ICONS, CAT_SESSION, CAT_FULL_WIPE: persistent
    // stores — no OTR op exists for them.
    ops
}

/// The persistent plan.  Op order mirrors the legacy accept() +
/// clearPrivateDataOnExit() fan-out: unmaterialized containers first
/// (the legacy call ran before every per-category block), then the
/// per-category stores in dialog order, with the exit-wipe's deep
/// storage ops last.
fn persistent_ops(categories: u32, since_secs: i64) -> Vec<Value> {
    let mut ops = Vec::new();

    // CONT05: registered containers whose profile was never
    // materialized have no live profile to sweep — their on-disk
    // trees are wiped directly, gated by the same category flags.
    if categories & (CAT_COOKIES | CAT_SITEDATA | CAT_CACHE | CAT_HISTORY) != 0 {
        let mut c = op("containers.unmaterialized", None, since_secs, false);
        c["cookies"] = json!(categories & CAT_COOKIES != 0);
        c["sitedata"] = json!(categories & CAT_SITEDATA != 0);
        c["cache"] = json!(categories & CAT_CACHE != 0);
        c["visited_links"] = json!(categories & CAT_HISTORY != 0);
        ops.push(c);
    }
    if categories & CAT_HISTORY != 0 {
        // The visit log is the one ranged-capable store.
        ops.push(op("history.clear", None, since_secs, true));
        // Chromium's visited-link database — all-or-nothing.
        ops.push(op("visited_links.clear", Some("persistent"), since_secs, false));
    }
    if categories & CAT_DOWNLOADS != 0 {
        ops.push(op("downloads.clear", None, since_secs, false));
    }
    if categories & CAT_SEARCH != 0 {
        ops.push(op("search.clear", None, since_secs, false));
    }
    if categories & CAT_COOKIES != 0 {
        ops.push(op("cookies.clear", Some("persistent"), since_secs, false));
    }
    if categories & CAT_SITEDATA != 0 {
        // Live origins must be emptied before the deferred disk
        // wipe, or in-memory storage re-flushes over the clear —
        // order is load-bearing.
        ops.push(op("sitedata.page_sweep", Some("persistent"), since_secs, false));
        ops.push(op("sitedata.defer_wipe", Some("persistent"), since_secs, false));
    }
    if categories & CAT_CACHE != 0 {
        ops.push(op("cache.clear", Some("persistent"), since_secs, false));
        // The app-side fetch cache (opensearch, adblock lists).
        ops.push(op("nam_cache.clear", None, since_secs, false));
    }
    if categories & CAT_ICONS != 0 {
        ops.push(op("icons.clear", None, since_secs, false));
    }
    if categories & CAT_SESSION != 0 {
        // The saved session blob is itself browsing history.
        ops.push(op("session.clear", None, since_secs, false));
    }
    if categories & CAT_FULL_WIPE != 0 {
        // PRIV01: the exit wipe additionally schedules the Network/
        // tree + cache dirs for removal at next start (async deletes
        // may not flush before exit), and removes whole trees of
        // unmaterialized containers.
        ops.push(op("storage.defer_full_wipe", Some("persistent"), since_secs, false));
        ops.push(op("containers.wipe_unmaterialized", None, since_secs, false));
    }
    ops
}

/// The rc_clearplan body: (period, category bits, scope) -> the
/// ordered operation list as JSON text.
pub fn plan_json(period: &str, categories: u32, otr: bool) -> RcResult<String> {
    let (name, secs) = parse_period(period).ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("clearplan: unknown period '{period}'"),
    })?;
    let ops = if otr {
        otr_ops(categories, secs)
    } else {
        persistent_ops(categories, secs)
    };
    Ok(json!({
        "scope": if otr { "otr" } else { "persistent" },
        "period": name,
        "since_secs": secs,
        "ops": ops,
    })
    .to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn plan(period: &str, cats: u32, otr: bool) -> Value {
        serde_json::from_str(&plan_json(period, cats, otr).unwrap()).unwrap()
    }

    fn op_names(p: &Value) -> Vec<String> {
        p["ops"]
            .as_array()
            .unwrap()
            .iter()
            .map(|o| o["op"].as_str().unwrap().to_string())
            .collect()
    }

    const ALL_DIALOG: u32 =
        CAT_HISTORY | CAT_DOWNLOADS | CAT_SEARCH | CAT_COOKIES | CAT_SITEDATA | CAT_CACHE | CAT_ICONS;

    #[test]
    fn full_persistent_plan_order() {
        let p = plan("all", ALL_DIALOG | CAT_SESSION | CAT_FULL_WIPE, false);
        assert_eq!(p["scope"], "persistent");
        assert_eq!(p["period"], "all");
        assert_eq!(p["since_secs"], 0);
        assert_eq!(
            op_names(&p),
            vec![
                "containers.unmaterialized",
                "history.clear",
                "visited_links.clear",
                "downloads.clear",
                "search.clear",
                "cookies.clear",
                "sitedata.page_sweep",
                "sitedata.defer_wipe",
                "cache.clear",
                "nam_cache.clear",
                "icons.clear",
                "session.clear",
                "storage.defer_full_wipe",
                "containers.wipe_unmaterialized",
            ]
        );
        // The container op carries the per-category flags.
        assert_eq!(p["ops"][0]["cookies"], true);
        assert_eq!(p["ops"][0]["visited_links"], true);
        // Scoped ops address the persistent profile set.
        assert_eq!(p["ops"][2]["scope"], "persistent");
        assert_eq!(p["ops"][5]["scope"], "persistent");
    }

    #[test]
    fn single_categories() {
        // Every checkbox alone produces exactly its own store ops.
        assert_eq!(
            op_names(&plan("all", CAT_HISTORY, false)),
            vec!["containers.unmaterialized", "history.clear", "visited_links.clear"]
        );
        assert_eq!(
            op_names(&plan("all", CAT_DOWNLOADS, false)),
            vec!["downloads.clear"]
        );
        assert_eq!(
            op_names(&plan("all", CAT_SEARCH, false)),
            vec!["search.clear"]
        );
        assert_eq!(
            op_names(&plan("all", CAT_COOKIES, false)),
            vec!["containers.unmaterialized", "cookies.clear"]
        );
        assert_eq!(
            op_names(&plan("all", CAT_SITEDATA, false)),
            vec![
                "containers.unmaterialized",
                "sitedata.page_sweep",
                "sitedata.defer_wipe"
            ]
        );
        assert_eq!(
            op_names(&plan("all", CAT_CACHE, false)),
            vec!["containers.unmaterialized", "cache.clear", "nam_cache.clear"]
        );
        assert_eq!(
            op_names(&plan("all", CAT_ICONS, false)),
            vec!["icons.clear"]
        );
        // No categories -> no work (and no container sweep).
        assert_eq!(op_names(&plan("all", 0, false)), Vec::<String>::new());
    }

    #[test]
    fn otr_plan_is_scoped_away_from_persistent() {
        let p = plan("all", ALL_DIALOG | CAT_SESSION | CAT_FULL_WIPE, true);
        assert_eq!(p["scope"], "otr");
        assert_eq!(
            op_names(&p),
            vec![
                "visited_links.clear",
                "downloads.clear_otr",
                "cookies.clear",
                "sitedata.page_sweep",
                "cache.clear",
            ]
        );
        // Every op is either OTR-scoped or the OTR download rows;
        // no persistent store name may appear.
        for o in p["ops"].as_array().unwrap() {
            let name = o["op"].as_str().unwrap();
            assert!(
                o["scope"] == "otr" || name == "downloads.clear_otr",
                "op {name} escapes the OTR scope"
            );
            assert!(!name.contains("history.clear")
                && !name.contains("session")
                && !name.contains("icons")
                && !name.contains("search.clear")
                && !name.contains("containers")
                && !name.contains("defer"));
        }
    }

    #[test]
    fn otr_single_categories() {
        assert_eq!(
            op_names(&plan("all", CAT_COOKIES, true)),
            vec!["cookies.clear"]
        );
        // Persistent-only categories produce nothing in OTR scope.
        assert_eq!(
            op_names(&plan("all", CAT_SEARCH | CAT_ICONS | CAT_SESSION, true)),
            Vec::<String>::new()
        );
    }

    #[test]
    fn ranged_period_marks_capability() {
        let p = plan("hour", ALL_DIALOG, false);
        assert_eq!(p["period"], "hour");
        assert_eq!(p["since_secs"], 3600);
        for o in p["ops"].as_array().unwrap() {
            assert_eq!(o["since_secs"], 3600);
            if o["op"] == "history.clear" {
                assert_eq!(o["ranged"], true);
                assert!(o.get("degraded").is_none());
            } else {
                assert_eq!(o["ranged"], false);
                assert_eq!(o["degraded"], true);
            }
        }
        // Week/day resolve too; unknown names refuse.
        assert_eq!(plan("day", CAT_HISTORY, false)["since_secs"], 86_400);
        assert_eq!(plan("last-week", CAT_HISTORY, false)["since_secs"], 604_800);
        assert_eq!(plan("everything", CAT_HISTORY, false)["since_secs"], 0);
        assert!(plan_json("fortnight", CAT_HISTORY, false).is_err());
        assert!(plan_json("hour;drop", CAT_HISTORY, false).is_err());
    }

    #[test]
    fn case_and_whitespace_tolerant() {
        assert_eq!(plan("  ALL ", CAT_ICONS, false)["period"], "all");
        assert_eq!(plan("Hour", CAT_ICONS, false)["period"], "hour");
    }
}
