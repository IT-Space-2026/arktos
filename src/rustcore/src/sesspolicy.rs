//! MD08: sesspolicy — the session-domain snapshot/restore policy.
//!
//! The session pipeline used to hard-code every "when do we snapshot
//! and what do we restore?" decision in Qt: the AutoSaver debounce +
//! ceiling lived as file-local #defines, every change notification
//! counted as dirty unconditionally, a saved session never aged out
//! and the crash-during-restore path always asked.  Those are pure
//! policy — they compute over the bound profile's settings — so
//! they live here behind the same JSON manifest in / verdict out
//! contract the other MD domains use:
//!
//!   manifest in -> {"op":"defaults"}                 (NULL store ok)
//!                | {"op":"snapshot"}                 -> {"interval_ms","maxwait_ms"}
//!                | {"op":"dirty","kind":<kind>}      -> {"dirty","kind"}
//!                | {"op":"retention"}                -> {"max_age_days","max_windows","max_tabs_per_window"}
//!                | {"op":"stale"}                    -> {"stale","age_ms","max_age_days"}
//!                | {"op":"restore"}                  -> {"startup_default","crash_restore"}
//!                | {"op":"lifecycle"}                -> {"suspend_idle_ms","memory_purge",
//!                                                     "lazy_restore","lazy_restore_pinned"}
//!                | {"op":"policy"}                   -> the whole resolved set
//!
//! Store keys (leaves under the bound scope's "session/" prefix —
//! the Qt writers keep owning them via ProfileSettings; this module
//! only reads, so a mid-process settings-dialog save is honored by
//! the per-op open):
//!   snapshotIntervalMs   int   debounce before a dirty mark saves
//!   snapshotMaxWaitMs    int   the most a dirty mark may age before
//!                              an immediate save
//!   dirtyOnTabs          bool  tab-strip changes mark the session
//!   dirtyOnChrome        bool  menubar/toolbar/statusbar/bookmarksbar
//!                              /tabbar visibility toggles mark it
//!   dirtyOnSidebar       bool  sidebar dock visibility marks it
//!   dirtyOnLifecycle     bool  window teardown marks it
//!   maxAgeDays           int   a session.dat older than this is not
//!                              restored (0 = never stale)
//!   maxWindows           int   cap on windows per snapshot
//!                              (0 = unlimited)
//!   maxTabsPerWindow     int   cap on tabs per window record
//!                              (0 = unlimited)
//!   crashRestore         str   "ask" (the restore-loop prompt),
//!                              "always" (restore silently) or
//!                              "never" (drop the crashed snapshot)
//!   startupModeDefault   int   the BrowserApplication::StartupMode
//!                              ordinal startupMode() falls back to
//!                              when no preference was ever stored
//!
//! The "lifecycle" op reads OUTSIDE the session/ prefix — it resolves
//! the Qt-owned tab-lifecycle rows the Settings dialog writes under
//! their own groups, so the dialog and the policy answer can never
//! diverge (verbatim parity with TabWidget::loadSettings):
//!   tabs/suspendTabs            bool  idle-suspend master switch (off)
//!   tabs/suspendTabsMinutes     int   idle minutes before suspend (30)
//!   tabs/memoryPurge            bool  the background freeze-pulse (on)
//!   performance/lazyRestoreTabs bool  restored tabs defer first load
//!                                     until activation (off)
//!   performance/alwaysLoadPinnedTabs bool  pinned tabs exempt from the
//!                                     lazy defer (on)
//! suspend_idle_ms is suspendTabs ? suspendTabsMinutes * 60000 : 0 —
//! the same arithmetic the C++ ran, including an out-of-band minutes
//! value passing through unsanitized (a negative reads as "never
//! idle" exactly like it did before).  The energy-saver's 10-minute
//! pin is live state, not stored policy — it stays Qt-side.
//!
//! Mirroring rules (kept 1:1 with the pre-refactor C++):
//!   * every missing or mistyped row resolves to the C++ constant —
//!     3000ms/15000ms timing, every dirty class on, no age or count
//!     caps, "ask" on the crash-restore, StartPageStartup (2) as the
//!     startup fallback — a policy read can never silently change
//!     behavior;
//!   * an unknown dirty "kind" still answers dirty=true — the old
//!     code marked every changeOccurred, so a caller naming a class
//!     this module does not know errs toward saving, never toward
//!     losing the window state;
//!   * the effective maxwait floors at the effective interval — a
//!     ceiling below the debounce would fire immediately and starve
//!     the coalescing the interval exists for;
//!   * "stale" reads the bound scope's session.dat mtime — a missing
//!     file is never stale (there is nothing to skip), and a
//!     maxAgeDays of 0 disables the check outright;
//!   * caps are non-negative; 0 means "no limit" everywhere, so the
//!     numbers Qt-side truncators can apply them blind.
//!
//! Read-only: there are no mutation ops and nothing is emitted —
//! the session.state lifecycle file, the restore guard and the
//! Qt timers all stay caller-side; this surface only answers
//! questions.

use serde_json::{json, Value};

use crate::error::{fail, RcResult, RcStatus};
use crate::session;
use crate::settings::RcSettings;

/// The settings-group prefix every leaf lives under.
const PREFIX: &str = "session/";

// The compiled-in constants this module defaults to — the values
// the C++ code hard-coded before the move (autosaver.cpp's
// AUTOSAVE_IN / MAXWAIT, the every-class-dirty behavior, the
// restoring-flag prompt, StartPageStartup).  Defaults exist so a
// missing or mistyped row can never change behavior.
const DEFAULT_INTERVAL_MS: i64 = 3_000;
const DEFAULT_MAXWAIT_MS: i64 = 15_000;
const DEFAULT_STARTUP_MODE: i64 = 2; // BrowserApplication::StartPageStartup

fn key(leaf: &str) -> String {
    format!("{PREFIX}{leaf}")
}

fn flag(s: &RcSettings, leaf: &str) -> bool {
    s.get_bool(&key(leaf)).unwrap_or(true)
}

fn int_at(s: &RcSettings, leaf: &str, default: i64, lo: i64, hi: i64) -> i64 {
    s.get_int(&key(leaf)).unwrap_or(default).clamp(lo, hi)
}

/// The resolved timing pair — interval clamped to a usable debounce
/// band, maxwait clamped and floored at the interval.
fn snapshot(s: Option<&RcSettings>) -> (i64, i64) {
    let interval = s
        .map(|s| int_at(s, "snapshotIntervalMs", DEFAULT_INTERVAL_MS, 250, 300_000))
        .unwrap_or(DEFAULT_INTERVAL_MS);
    let maxwait = s
        .map(|s| int_at(s, "snapshotMaxWaitMs", DEFAULT_MAXWAIT_MS, 1_000, 600_000))
        .unwrap_or(DEFAULT_MAXWAIT_MS)
        .max(interval);
    (interval, maxwait)
}

fn retention(s: Option<&RcSettings>) -> (i64, i64, i64) {
    let f = |leaf: &str, hi: i64| {
        s.map(|s| int_at(s, leaf, 0, 0, hi)).unwrap_or(0)
    };
    (f("maxAgeDays", 3_650), f("maxWindows", 1_000), f("maxTabsPerWindow", 10_000))
}

fn crash_restore(s: Option<&RcSettings>) -> &'static str {
    match s
        .and_then(|s| s.get_string(&key("crashRestore")).ok())
        .as_deref()
    {
        Some("always") => "always",
        Some("never") => "never",
        _ => "ask",
    }
}

fn startup_default(s: Option<&RcSettings>) -> i64 {
    s.map(|s| int_at(s, "startupModeDefault", DEFAULT_STARTUP_MODE, 0, 3))
        .unwrap_or(DEFAULT_STARTUP_MODE)
}

/// The tab-lifecycle read — the Settings dialog's own rows under
/// "tabs/" and "performance/", resolved with the same defaults and
/// arithmetic TabWidget::loadSettings ran inline.  A missing or
/// mistyped row answers the C++ default verbatim.
fn lifecycle(s: Option<&RcSettings>) -> Value {
    let flag_at = |key: &str, default: bool| {
        s.and_then(|s| s.get_bool(key).ok()).unwrap_or(default)
    };
    let suspend = flag_at("tabs/suspendTabs", false);
    let minutes = s
        .and_then(|s| s.get_int("tabs/suspendTabsMinutes").ok())
        .unwrap_or(30);
    json!({
        "suspend_idle_ms": if suspend { minutes.saturating_mul(60_000) } else { 0 },
        "memory_purge": flag_at("tabs/memoryPurge", true),
        "lazy_restore": flag_at("performance/lazyRestoreTabs", false),
        "lazy_restore_pinned": flag_at("performance/alwaysLoadPinnedTabs", true),
    })
}

/// The dirty-on-what verdict for one change class.  Unknown kinds
/// answer true — the pre-refactor code marked every change, so an
/// unrecognized class errs toward saving.
fn dirty(s: Option<&RcSettings>, kind: &str) -> bool {
    match s {
        None => true,
        Some(s) => match kind {
            "tabs" => flag(s, "dirtyOnTabs"),
            "chrome" => flag(s, "dirtyOnChrome"),
            "sidebar" => flag(s, "dirtyOnSidebar"),
            "lifecycle" => flag(s, "dirtyOnLifecycle"),
            _ => true,
        },
    }
}

/// The whole resolved policy in one object — the "policy" op and
/// the shape "defaults" answers on a NULL store.
fn resolved(s: Option<&RcSettings>) -> Value {
    let (interval, maxwait) = snapshot(s);
    let (age, windows, tabs) = retention(s);
    json!({
        "interval_ms": interval,
        "maxwait_ms": maxwait,
        "dirty_on": {
            "tabs": s.map(|s| flag(s, "dirtyOnTabs")).unwrap_or(true),
            "chrome": s.map(|s| flag(s, "dirtyOnChrome")).unwrap_or(true),
            "sidebar": s.map(|s| flag(s, "dirtyOnSidebar")).unwrap_or(true),
            "lifecycle": s.map(|s| flag(s, "dirtyOnLifecycle")).unwrap_or(true),
        },
        "max_age_days": age,
        "max_windows": windows,
        "max_tabs_per_window": tabs,
        "crash_restore": crash_restore(s),
        "startup_default": startup_default(s),
        "lifecycle": lifecycle(s),
    })
}

/// session.dat's age in milliseconds under the bound scope — None
/// when no file exists (nothing to skip) or the mtime is unreadable.
fn session_age_ms(s: &RcSettings) -> Option<u64> {
    let path = s.scope_dir().join(session::FILE_NAME);
    let modified = std::fs::metadata(&path).and_then(|m| m.modified()).ok()?;
    modified
        .elapsed()
        .ok()
        .map(|d| u64::try_from(d.as_millis()).unwrap_or(u64::MAX))
}

/// The granular endpoint — JSON manifest in, verdict object out.
/// Store-bound ops need `s`; "defaults" is the only NULL-legal op.
pub fn call_json(s: Option<&RcSettings>, manifest: &Value) -> RcResult<Value> {
    let op = manifest
        .get("op")
        .and_then(Value::as_str)
        .unwrap_or("");
    match op {
        "defaults" => Ok(resolved(None)),
        "policy" => Ok(resolved(s)),
        "snapshot" => {
            let (interval, maxwait) = snapshot(s);
            Ok(json!({"interval_ms": interval, "maxwait_ms": maxwait}))
        }
        "dirty" => {
            let kind = manifest
                .get("kind")
                .and_then(Value::as_str)
                .unwrap_or("");
            if kind.is_empty() {
                return fail(RcStatus::InvalidArgument, "dirty call needs a kind");
            }
            Ok(json!({"dirty": dirty(s, kind), "kind": kind}))
        }
        "retention" => {
            let (age, windows, tabs) = retention(s);
            Ok(json!({
                "max_age_days": age,
                "max_windows": windows,
                "max_tabs_per_window": tabs,
            }))
        }
        "stale" => {
            let s = s.ok_or_else(|| crate::error::Fail {
                status: RcStatus::InvalidArgument,
                msg: "stale call needs a bound profile store".into(),
            })?;
            let (age_days, _, _) = retention(Some(s));
            let age_ms = session_age_ms(s);
            let stale = match (age_days, age_ms) {
                (0, _) => false,
                (limit, Some(ms)) => ms > u64::try_from(limit).unwrap_or(u64::MAX) * 86_400_000,
                (_, None) => false,
            };
            Ok(json!({
                "stale": stale,
                "age_ms": age_ms,
                "max_age_days": age_days,
            }))
        }
        "restore" => Ok(json!({
            "startup_default": startup_default(s),
            "crash_restore": crash_restore(s),
        })),
        "lifecycle" => Ok(lifecycle(s)),
        _ => fail(RcStatus::InvalidArgument, "unknown sesspolicy op"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings;

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "arktos-sesspolicy-{}-{}",
            tag,
            std::process::id()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.to_string_lossy().into_owned()
    }

    fn store(root: &str, profile: Option<&str>) -> RcSettings {
        settings::RcSettings::open(root, profile).unwrap()
    }

    #[test]
    fn defaults_match_cpp_constants() {
        // A NULL store answers the compiled-in set — the parity
        // contract every fallback leans on.
        let d = call_json(None, &json!({"op": "defaults"})).unwrap();
        assert_eq!(d["interval_ms"], 3_000);
        assert_eq!(d["maxwait_ms"], 15_000);
        assert_eq!(d["dirty_on"]["tabs"], true);
        assert_eq!(d["dirty_on"]["chrome"], true);
        assert_eq!(d["dirty_on"]["sidebar"], true);
        assert_eq!(d["dirty_on"]["lifecycle"], true);
        assert_eq!(d["max_age_days"], 0);
        assert_eq!(d["max_windows"], 0);
        assert_eq!(d["max_tabs_per_window"], 0);
        assert_eq!(d["crash_restore"], "ask");
        assert_eq!(d["startup_default"], 2);
        assert_eq!(d["lifecycle"]["suspend_idle_ms"], 0);
        assert_eq!(d["lifecycle"]["memory_purge"], true);
        assert_eq!(d["lifecycle"]["lazy_restore"], false);
        assert_eq!(d["lifecycle"]["lazy_restore_pinned"], true);

        // Bound ops refuse a NULL store, never guess.
        for op in [
            "snapshot",
            "retention",
            "stale",
            "restore",
            "policy",
            "lifecycle",
        ] {
            // snapshot/retention/restore/policy degrade to defaults —
            // they compose over an absent store the same way the C++
            // constants did; only "stale" genuinely needs the dir.
            let r = call_json(None, &json!({"op": op}));
            if op == "stale" {
                assert!(r.is_err());
            } else {
                assert!(r.is_ok(), "{op}");
            }
        }
        assert!(call_json(None, &json!({"op": "bogus"})).is_err());
        assert!(call_json(None, &json!({"op": "dirty"})).is_err());
    }

    #[test]
    fn dirty_kinds_and_unknown() {
        let root = tmpdir("dirty");
        let s = store(&root, Some("work"));
        // Pristine: every class dirty.
        for kind in ["tabs", "chrome", "sidebar", "lifecycle"] {
            let v = call_json(
                Some(&s),
                &json!({"op": "dirty", "kind": kind}),
            )
            .unwrap();
            assert_eq!(v["dirty"], true, "{kind}");
        }
        // A flip lands — one class off, the rest still dirty.
        s.set(
            &key("dirtyOnChrome"),
            settings::Value::Bool(false),
        )
        .unwrap();
        let v = call_json(
            Some(&s),
            &json!({"op": "dirty", "kind": "chrome"}),
        )
        .unwrap();
        assert_eq!(v["dirty"], false);
        let v = call_json(
            Some(&s),
            &json!({"op": "dirty", "kind": "tabs"}),
        )
        .unwrap();
        assert_eq!(v["dirty"], true);
        // Unknown kind errs toward saving.
        let v = call_json(
            Some(&s),
            &json!({"op": "dirty", "kind": "futuristic"}),
        )
        .unwrap();
        assert_eq!(v["dirty"], true);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn timing_clamps_and_floor() {
        let root = tmpdir("timing");
        let s = store(&root, Some("work"));
        s.set(&key("snapshotIntervalMs"), settings::Value::Int(10))
            .unwrap();
        s.set(&key("snapshotMaxWaitMs"), settings::Value::Int(5))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "snapshot"})).unwrap();
        // Interval clamps up to 250; maxwait clamps up to its own
        // 1000ms floor (already above the interval).
        assert_eq!(v["interval_ms"], 250);
        assert_eq!(v["maxwait_ms"], 1_000);
        // A maxwait legitimately under the interval floors up to it.
        s.set(&key("snapshotIntervalMs"), settings::Value::Int(120_000))
            .unwrap();
        s.set(&key("snapshotMaxWaitMs"), settings::Value::Int(30_000))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "snapshot"})).unwrap();
        assert_eq!(v["interval_ms"], 120_000);
        assert_eq!(v["maxwait_ms"], 120_000);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn restore_and_retention() {
        let root = tmpdir("restore");
        let s = store(&root, Some("work"));
        s.set(&key("crashRestore"), settings::Value::Str("never".into()))
            .unwrap();
        s.set(&key("startupModeDefault"), settings::Value::Int(0))
            .unwrap();
        s.set(&key("maxAgeDays"), settings::Value::Int(30))
            .unwrap();
        s.set(&key("maxWindows"), settings::Value::Int(4))
            .unwrap();
        s.set(&key("maxTabsPerWindow"), settings::Value::Int(50))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "restore"})).unwrap();
        assert_eq!(v["crash_restore"], "never");
        assert_eq!(v["startup_default"], 0);
        let v = call_json(Some(&s), &json!({"op": "retention"})).unwrap();
        assert_eq!(v["max_age_days"], 30);
        assert_eq!(v["max_windows"], 4);
        assert_eq!(v["max_tabs_per_window"], 50);
        // A mistyped crashRestore falls back to "ask".
        s.set(&key("crashRestore"), settings::Value::Int(9))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "restore"})).unwrap();
        assert_eq!(v["crash_restore"], "ask");
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn lifecycle_parity() {
        let root = tmpdir("lifecycle");
        let s = store(&root, Some("work"));
        // Pristine: the C++ defaults verbatim — suspend off, purge
        // on, lazy restore off with the pinned exemption on.
        let v = call_json(Some(&s), &json!({"op": "lifecycle"})).unwrap();
        assert_eq!(v["suspend_idle_ms"], 0);
        assert_eq!(v["memory_purge"], true);
        assert_eq!(v["lazy_restore"], false);
        assert_eq!(v["lazy_restore_pinned"], true);
        // The dialog's own rows resolve — suspend on at 10 minutes,
        // purge off, lazy on with the pinned exemption dropped.
        s.set("tabs/suspendTabs", settings::Value::Bool(true))
            .unwrap();
        s.set("tabs/suspendTabsMinutes", settings::Value::Int(10))
            .unwrap();
        s.set("tabs/memoryPurge", settings::Value::Bool(false))
            .unwrap();
        s.set(
            "performance/lazyRestoreTabs",
            settings::Value::Bool(true),
        )
        .unwrap();
        s.set(
            "performance/alwaysLoadPinnedTabs",
            settings::Value::Bool(false),
        )
        .unwrap();
        let v = call_json(Some(&s), &json!({"op": "lifecycle"})).unwrap();
        assert_eq!(v["suspend_idle_ms"], 600_000);
        assert_eq!(v["memory_purge"], false);
        assert_eq!(v["lazy_restore"], true);
        assert_eq!(v["lazy_restore_pinned"], false);
        // The resolved "policy" set carries the same object.
        let v = call_json(Some(&s), &json!({"op": "policy"})).unwrap();
        assert_eq!(v["lifecycle"]["suspend_idle_ms"], 600_000);
        // A mistyped row answers what QVariant::toInt() did — 0 on
        // an unparseable string (the default only covers an absent
        // key) — never a panic.
        s.set(
            "tabs/suspendTabsMinutes",
            settings::Value::Str("soon".into()),
        )
        .unwrap();
        let v = call_json(Some(&s), &json!({"op": "lifecycle"})).unwrap();
        assert_eq!(v["suspend_idle_ms"], 0);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn staleness() {
        let root = tmpdir("stale");
        let s = store(&root, Some("work"));
        // No session.dat -> never stale, age null.
        let v = call_json(Some(&s), &json!({"op": "stale"})).unwrap();
        assert_eq!(v["stale"], false);
        assert!(v["age_ms"].is_null());
        // A fresh file is never stale even with a tiny limit.
        std::fs::create_dir_all(s.scope_dir()).unwrap();
        let dat = s.scope_dir().join(session::FILE_NAME);
        std::fs::write(&dat, b"x").unwrap();
        s.set(&key("maxAgeDays"), settings::Value::Int(30))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "stale"})).unwrap();
        assert_eq!(v["stale"], false);
        assert!(v["age_ms"].is_number());
        // A backdated mtime crosses the limit.
        let old = std::time::SystemTime::now()
            - std::time::Duration::from_secs(31 * 86_400);
        let f = std::fs::File::options().write(true).open(&dat).unwrap();
        f.set_modified(old).unwrap();
        drop(f);
        let v = call_json(Some(&s), &json!({"op": "stale"})).unwrap();
        assert_eq!(v["stale"], true);
        // maxAgeDays=0 disables the check outright.
        s.set(&key("maxAgeDays"), settings::Value::Int(0))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "stale"})).unwrap();
        assert_eq!(v["stale"], false);
        let _ = std::fs::remove_dir_all(&root);
    }
}
