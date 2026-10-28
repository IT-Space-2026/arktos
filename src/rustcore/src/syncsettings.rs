//! SYNC07 — the settings CRDT adapter on the SYNC01 op-log.
//!
//! Semantics (documented honestly — this is what the code does):
//!
//!   * A last-writer-wins map over synced preference keys.  Two op
//!     types carry the whole mutation set:
//!       {"t":"set","key":K,"val":<typed-value-doc>}  — upsert
//!       {"t":"del","key":K}                        — remove
//!     <typed-value-doc> is the settings store's own {"t","v"}
//!     shape (value_to_json), so the wire never invents a second
//!     value encoding.  Resolution is per-key LWW on the op's
//!     (lamport, device_id) stamp: a strictly newer op moves the
//!     key — a peer's del removes our value, a peer's set
//!     overwrites it — and an older or equal op drops silently.
//!     Concurrent sets converge to the higher stamp's value.
//!   * Device-local exclusion list: key-PREFIX rules (raw
//!     starts_with, so "downloadmanager/download" covers the
//!     download_N_* records) that never emit AND never apply —
//!     paths, window geometry, proxy, and per-machine prefs are
//!     device state, not group state.  The effective list is the
//!     built-in defaults plus the profile's own "sync/localExclude"
//!     string-list value — which sits under the excluded "sync"
//!     prefix itself, so the list can never sync into changing
//!     itself.  An excluded remote op is consumed-and-dropped: it
//!     counts as applied (it was honestly processed), writes
//!     nothing, and leaves no frontier entry — lifting the rule
//!     later lets the NEXT op through, never resurrects the ones
//!     skipped while excluded.
//!   * Emission rides RcSettings::set/remove — the funnel every
//!     mutation lane (flat rc_*, mw ops, in-process callers)
//!     already shares, so there is no outbox: the write persists
//!     first, then the op appends best-effort (a failed append
//!     leaves a durable local change the next advertise re-offers —
//!     the honest degraded case).  Apply mutates through
//!     set_applied/remove_applied — no emit — so a merged op can
//!     never echo back onto the log.
//!   * First-touch advertise per (root, scope): the first emit or
//!     apply for a scope re-issues every present non-excluded key
//!     whose frontier is still empty — pre-sync prefs flow to the
//!     group once, like SYNC05's pre-sync rows.  ini imports never
//!     emit per-key (merge_ini is a bulk absent-keys fill); the
//!     advertise pass is how imported values reach the group.
//!   * Global-scope settings NEVER sync — the sync group is a
//!     profile (SYNC03); the app-global scope is device state.
//!
//! Durability: no adapter db — the op-log replay rebuilds the
//! per-key stamp frontier on first use.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

use crate::error::{fail, RcResult, RcStatus};
use crate::facade;
use crate::notify;
use crate::settings::{self, RcSettings};
use crate::sync::{self, SyncEngine, WireOp};

const STORE: &str = "settings";

/// (lamport, device_id) — the total order the per-key LWW sorts by.
type Stamp = (u64, String);

fn stamp_of(op: &WireOp) -> Stamp {
    (op.lamport, op.device_id.clone())
}

fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::InvalidArgument, msg.into())
}

/// The settings key the device-local half of the exclusion list
/// lives under — a string-list of additional prefixes.  It sits
/// under the built-in "sync" rule, so the list itself can never
/// sync into changing itself.
pub(crate) const EXCLUDE_KEY: &str = "sync/localExclude";

/// The built-in half of the exclusion list — the per-machine
/// preference subtrees every device keeps to itself.  Raw key
/// prefixes (starts_with): "sync" also gates the localExclude key;
/// "sessions"/"BrowserMainWindow" carry window geometry and tab-set
/// blobs; "proxy" is machine network config; the downloadmanager
/// leaves are filesystem paths and the download_N_* in-flight
/// records are per-device bookkeeping.
pub(crate) const DEFAULT_EXCLUDES: &[&str] = &[
    "sync",
    "sessions",
    "BrowserMainWindow",
    "proxy",
    "downloadmanager/downloadDirectory",
    "downloadmanager/externalPath",
    "downloadmanager/download",
];

/// Raw-prefix match — one rule kills a whole subtree (or a key
/// family sharing a stem, e.g. download_N_*).
fn excluded(rules: &[String], key: &str) -> bool {
    DEFAULT_EXCLUDES.iter().any(|r| key.starts_with(*r))
        || rules
            .iter()
            .any(|r| !r.is_empty() && key.starts_with(r.as_str()))
}

/// The device-local half of the exclusion list — the scope's own
/// sync/localExclude string-list (absent/wrong type → no additions).
fn local_rules(h: &RcSettings) -> Vec<String> {
    match h.get(EXCLUDE_KEY) {
        Ok(settings::Value::List(l)) => l,
        _ => Vec::new(),
    }
}

/// One scope's LWW frontier — rebuilt from the op-log on first
/// use, so it carries no files of its own.
#[derive(Default)]
struct SetSync {
    /// key -> stamp of the newest op the log holds for it — the
    /// LWW frontier apply and advertise share.
    frontier: BTreeMap<String, Stamp>,
    hydrated: bool,
    /// First-touch advertise runs once per adapter lifetime — after
    /// it, keys only ever reach the log through mutations.
    advertised: bool,
}

/// The per-(root, scope) adapters.  Locked only to fetch/insert —
/// never held across a call outside the adapter (same discipline
/// as synchistory, which does hold it across store calls — the
/// apply lane keeps that pattern since handle opens are cheap).
static REG: Mutex<BTreeMap<(String, String), SetSync>> = Mutex::new(BTreeMap::new());

impl SetSync {
    /// Rebuild from the engine's durable log: replay this scope's
    /// set/del ops into the frontier — the newest stamp per key
    /// wins, which is exactly the LWW state.
    fn hydrate(&mut self, engine: &SyncEngine, scope: &str) -> RcResult<()> {
        if self.hydrated {
            return Ok(());
        }
        self.hydrated = true;
        for op in engine.export(Some(STORE), 0)? {
            if op.scope != scope {
                continue;
            }
            let t = op.body["t"].as_str();
            if t != Some("set") && t != Some("del") {
                continue;
            }
            let Some(key) = op.body["key"].as_str() else {
                continue;
            };
            let s = stamp_of(&op);
            let e = self
                .frontier
                .entry(key.to_string())
                .or_insert_with(|| s.clone());
            if s > *e {
                *e = s;
            }
        }
        Ok(())
    }

    /// Emit every present non-excluded key the frontier doesn't
    /// know — runs once per adapter lifetime, so pre-sync prefs
    /// flow to the group once.  `h` is the caller's already-open
    /// scope handle.
    fn advertise(
        &mut self,
        h: &RcSettings,
        engine: &SyncEngine,
        scope: &str,
        rules: &[String],
    ) -> RcResult<()> {
        for key in h.list(None)? {
            if excluded(rules, &key) || self.frontier.contains_key(&key) {
                continue;
            }
            let Ok(v) = h.get(&key) else {
                continue;
            };
            let op = engine.append(
                STORE,
                scope,
                &json!({"t": "set", "key": key, "val": settings::value_to_json(&v)}),
            )?;
            self.frontier.insert(key, stamp_of(&op));
        }
        Ok(())
    }
}

/// <root>/profiles/<scope>/settings.json → (root, scope) — the
/// binding every emit path funnels through; the global scope's
/// flat path (<root>/settings.json) is NOT a synced store and
/// returns None, so global-scope mutations never emit.
fn binding_scope(path: &Path) -> Option<(String, String)> {
    if path.file_name()?.to_str()? != "settings.json" {
        return None;
    }
    let dir = path.parent()?;
    let scope = dir.file_name()?.to_str()?.to_string();
    if dir.parent()?.file_name()?.to_str()? != "profiles" {
        return None;
    }
    let root = dir.parent()?.parent()?.to_string_lossy().into_owned();
    Some((root, scope))
}

/// The post-mutation hook RcSettings::set/remove call: sign the
/// change onto the scope's op-log.  Best-effort BY DESIGN — the
/// write is already durable, so a failed append drops silently;
/// the key's still-empty frontier means the next advertise
/// re-offers it.  `local` is the handle's own sync/localExclude
/// list, already read under its lock by the caller.
pub(crate) fn emit_local(
    file: &Path,
    key: &str,
    local: &[String],
    value: Option<&settings::Value>,
) {
    let Some((root, scope)) = binding_scope(file) else {
        return;
    };
    if excluded(local, key) {
        return;
    }
    let Ok(engine) = SyncEngine::open(&root) else {
        return;
    };
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let ss = reg
        .entry((root.clone(), scope.clone()))
        .or_insert_with(SetSync::default);
    if !ss.hydrated && ss.hydrate(&engine, &scope).is_err() {
        return;
    }
    let body = match value {
        Some(v) => json!({"t": "set", "key": key, "val": settings::value_to_json(v)}),
        None => json!({"t": "del", "key": key}),
    };
    if let Ok(op) = engine.append(STORE, &scope, &body) {
        ss.frontier.insert(key.to_string(), stamp_of(&op));
    }
    // First-touch merge, after the op so a fresh key doesn't get a
    // second emission: advertise every local key the frontier
    // still doesn't know — pre-sync prefs flow to the group once.
    if !ss.advertised {
        ss.advertised = true;
        if let Ok(h) = RcSettings::open(&root, Some(&scope)) {
            let _ = ss.advertise(&h, &engine, &scope, local);
        }
    }
}

/// Register the apply lane for `root`'s sync engine — called from
/// every sync-domain MW op so the seam always routes to the core
/// the call is bound to.
pub(crate) fn install(root: &str) {
    let root = root.to_string();
    sync::register_applier(
        STORE,
        Arc::new(move |op: &WireOp| apply_remote(&root, op)),
    );
}

fn apply_remote(root: &str, op: &WireOp) -> RcResult<()> {
    let scope = facade::effective_profile(&op.scope).to_string();
    let t = op.body["t"].as_str().unwrap_or("");
    let key = op.body["key"].as_str().unwrap_or("");
    if key.is_empty() || (t != "set" && t != "del") {
        return invalid("bad settings op");
    }
    let key = settings::check_key(key)?;
    if t == "set" && settings::value_from_json(&op.body["val"]).is_none() {
        return invalid("bad set op value");
    }
    let h = RcSettings::open(root, Some(&scope))?;
    let local = local_rules(&h);
    let engine = SyncEngine::open(root)?;
    let s = stamp_of(op);
    let changed = {
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        let ss = reg
            .entry((root.to_string(), scope.clone()))
            .or_insert_with(SetSync::default);
        if !ss.hydrated {
            ss.hydrate(&engine, &scope)?;
        }
        let changed = if excluded(&local, &key) {
            // Device-local keys never apply — consumed-and-dropped.
            false
        } else {
            let prev = ss.frontier.get(&key).cloned().unwrap_or_default();
            // STRICTLY-older is stale.  An equal stamp can only be
            // this very op — (lamport, device) is unique per op, and
            // the log already holds it when hydrate replays — so
            // equality is the first apply (or a drain's idempotent
            // re-delivery), never a conflict to resolve.
            if s < prev {
                false
            } else {
                let moved = match t {
                    "set" => {
                        let v = settings::value_from_json(&op.body["val"]).unwrap();
                        h.set_applied(&key, v)?
                    }
                    _ => h.remove_applied(&key)?,
                };
                if s > prev {
                    ss.frontier.insert(key, s);
                }
                moved
            }
        };
        // A peer's first op for this scope is also our cue to
        // advertise the pre-sync keys the frontier doesn't know.
        if !ss.advertised {
            ss.advertised = true;
            let _ = ss.advertise(&h, &engine, &scope, &local);
        }
        changed
    };
    if changed {
        let _hint = notify::profile_hint(&scope);
        notify::emit("settings");
        notify::emit_event("settings/changed", json!({ "op": t }));
    }
    Ok(())
}

/// The effective exclusion rule lists for one scope — the
/// sync.settings_excludes op's read model.
pub(crate) fn excludes(root: &str, scope: &str) -> RcResult<Value> {
    let h = RcSettings::open(root, Some(scope))?;
    Ok(json!({
        "defaults": DEFAULT_EXCLUDES,
        "local": local_rules(&h),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::Value as SValue;
    use std::path::Path;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-ssync-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.to_string_lossy().into_owned()
    }

    /// A "device": a data root whose profile-scope settings store
    /// and sync engine live side by side — the production shape.
    struct Dev {
        root: String,
    }

    impl Dev {
        fn new(tag: &str) -> Dev {
            Dev { root: tmpdir(tag) }
        }
        fn engine(&self) -> SyncEngine {
            SyncEngine::open(&self.root).unwrap()
        }
        fn settings(&self, scope: &str) -> RcSettings {
            RcSettings::open(&self.root, Some(scope)).unwrap()
        }
        fn set(&self, scope: &str, key: &str, v: SValue) {
            self.settings(scope).set(key, v).unwrap();
        }
        fn get(&self, scope: &str, key: &str) -> Option<SValue> {
            self.settings(scope).get(key).ok()
        }
        fn pair(&self, peer: &Dev, scope: &str) {
            let pe = peer.engine();
            self.engine()
                .member_add(&pe.public_key(), "peer.onion", scope)
                .unwrap();
        }
        fn wire(&self) -> Vec<Value> {
            self.engine()
                .export(Some(STORE), 0)
                .unwrap()
                .iter()
                .map(|o| o.to_json())
                .collect()
        }
        fn import(&self, docs: &[Value]) -> sync::ImportReport {
            install(&self.root);
            self.engine().import(docs).unwrap()
        }
    }

    #[test]
    fn sets_propagate_and_lww_orders() {
        let _g = guard();
        let a = Dev::new("lwA");
        let b = Dev::new("lwB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        a.set("default", "browser/foo", SValue::Str("one".into()));
        let first = a.wire();
        assert_eq!(first.len(), 1, "{first:?}");
        assert_eq!(first[0]["body"]["t"], "set");
        a.set("default", "browser/foo", SValue::Str("two".into()));
        let all = a.wire();
        assert_eq!(all.len(), 2);

        // Deliver newest first, then the stale doc — the LWW swap
        // must refuse to move backwards.
        let rep = b.import(&all[1..]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert_eq!(b.get("default", "browser/foo"), Some(SValue::Str("two".into())));
        let rep = b.import(&first[..1]);
        assert_eq!(rep.applied, 1);
        assert_eq!(
            b.get("default", "browser/foo"),
            Some(SValue::Str("two".into())),
            "a stale set must never overwrite a newer value"
        );
    }

    #[test]
    fn deletes_propagate_lww() {
        let _g = guard();
        let a = Dev::new("dlA");
        let b = Dev::new("dlB");
        let c = Dev::new("dlC");
        a.pair(&b, "default");
        b.pair(&a, "default");
        a.pair(&c, "default");
        c.pair(&a, "default");

        a.set("default", "browser/foo", SValue::Str("v".into()));
        let set_op = a.wire();
        a.settings("default").remove("browser/foo").unwrap();
        let all = a.wire();
        let del_op = &all[1..];
        assert_eq!(del_op[0]["body"]["t"], "del");

        // In-order delivery: the set lands, the del removes it.
        let rep = b.import(&set_op);
        assert_eq!(rep.applied, 1);
        assert_eq!(b.get("default", "browser/foo"), Some(SValue::Str("v".into())));
        let rep = b.import(del_op);
        assert_eq!(rep.applied, 1);
        assert_eq!(b.get("default", "browser/foo"), None);

        // Out-of-order on a second peer: the del first, then the
        // older set — the set must not resurrect the key.
        let rep = c.import(del_op);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        let rep = c.import(&set_op);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert_eq!(
            c.get("default", "browser/foo"),
            None,
            "a set older than the del must not resurrect the key"
        );
    }

    #[test]
    fn concurrent_writes_converge() {
        let _g = guard();
        let a = Dev::new("ccA");
        let b = Dev::new("ccB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        // Both devices write the same key before either sees the
        // other's op — concurrent LWW.
        a.set("default", "browser/foo", SValue::Str("from-a".into()));
        b.set("default", "browser/foo", SValue::Str("from-b".into()));
        let wa = a.wire();
        let wb = b.wire();
        b.import(&wa);
        a.import(&wb);
        let va = a.get("default", "browser/foo");
        let vb = b.get("default", "browser/foo");
        assert_eq!(va, vb, "devices diverged: {va:?} vs {vb:?}");
        assert!(matches!(
            va,
            Some(SValue::Str(ref s)) if s == "from-a" || s == "from-b"
        ));
    }

    #[test]
    fn excluded_keys_never_move() {
        let _g = guard();
        let a = Dev::new("exA");
        let b = Dev::new("exB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        // Built-in rules: the proxy group, window geometry, and a
        // filesystem path emit no ops at all.
        a.set("default", "proxy/enabled", SValue::Bool(true));
        a.set("default", "sessions/lastSession", SValue::Bytes(b"blob".to_vec()));
        a.set("default", "BrowserMainWindow/defaultState", SValue::Bytes(b"g".to_vec()));
        a.set(
            "default",
            "downloadmanager/downloadDirectory",
            SValue::Str("/home/a/dl".into()),
        );
        a.set("default", "downloadmanager/download_0_url", SValue::Str("u".into()));
        assert!(a.wire().is_empty(), "excluded keys emitted: {:?}", a.wire());

        // A peer's op for an excluded key is honestly consumed —
        // counts applied, writes nothing, and B's proxy stays
        // whatever B has locally.
        let op = a
            .engine()
            .append(
                STORE,
                "default",
                &json!({"t": "set", "key": "proxy/enabled",
                        "val": settings::value_to_json(&SValue::Bool(true))}),
            )
            .unwrap();
        let rep = b.import(&[op.to_json()]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert_eq!(b.get("default", "proxy/enabled"), None);

        // The device-local half: a device adds its own rule through
        // the excluded list key — which itself never syncs (it sits
        // under the built-in "sync" prefix).  The crafted proxy op
        // above is the only op A's log holds.
        a.set("default", EXCLUDE_KEY, SValue::List(vec!["myprefix".into()]));
        a.set("default", "myprefix/secret", SValue::Str("s".into()));
        let wire = a.wire();
        let keys: Vec<&str> = wire
            .iter()
            .filter_map(|o| o["body"]["key"].as_str())
            .collect();
        assert!(!keys.contains(&EXCLUDE_KEY), "list synced: {keys:?}");
        assert!(
            !keys.contains(&"myprefix/secret"),
            "user-excluded key emitted: {keys:?}"
        );

        // But a NON-excluded sibling still moves.
        a.set("default", "browser/bar", SValue::Str("x".into()));
        let wire = a.wire();
        assert_eq!(wire.len(), 2, "{wire:?}");
        assert_eq!(wire[1]["body"]["key"], "browser/bar");
        let rep = b.import(&wire);
        // The already-imported crafted op dedups; browser/bar applies.
        assert_eq!(rep.duplicates, 1);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert_eq!(b.get("default", "browser/bar"), Some(SValue::Str("x".into())));
        // And the excluded key still never materialized.
        assert_eq!(b.get("default", "proxy/enabled"), None);
        assert_eq!(b.get("default", EXCLUDE_KEY), None);
    }

    #[test]
    fn pre_sync_keys_advertise_once() {
        let _g = guard();
        let a = Dev::new("adA");
        let b = Dev::new("adB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        // B holds prefs the log has never seen — the no-emit lane
        // stands in for every pre-sync write (ini merge, pre-pair
        // state).  The first op B applies is the cue to advertise
        // them.
        {
            let h = b.settings("default");
            h.set_applied("browser/pre", SValue::Str("old".into())).unwrap();
            h.set_applied("browser/imported", SValue::Str("ini".into()))
                .unwrap();
        }
        assert!(b.wire().is_empty());

        a.set("default", "browser/foo", SValue::Str("x".into()));
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1);

        // B's advertise emitted both unknown keys — B's wire now
        // carries A's merged op plus the two advertised ones.
        let wire = b.wire();
        assert_eq!(wire.len(), 3, "{wire:?}");
        let keys: Vec<&str> = wire
            .iter()
            .filter_map(|o| o["body"]["key"].as_str())
            .collect();
        assert!(keys.contains(&"browser/imported"), "{keys:?}");
        assert!(keys.contains(&"browser/pre"), "{keys:?}");

        // And they flow back to A on the next exchange.
        let rep = a.import(&b.wire());
        assert_eq!(
            a.get("default", "browser/imported"),
            Some(SValue::Str("ini".into())),
            "{:?}",
            rep.apply_errors
        );
        assert_eq!(a.get("default", "browser/pre"), Some(SValue::Str("old".into())));
    }

    #[test]
    fn ini_imported_values_reach_the_group_via_advertise() {
        let _g = guard();
        let a = Dev::new("iiA");
        let b = Dev::new("iiB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        // A legacy settings.ini in A's profile dir — the import
        // fills the store without a single emit.
        let prof = Path::new(&a.root).join("profiles").join("default");
        std::fs::create_dir_all(&prof).unwrap();
        std::fs::write(
            prof.join("settings.ini"),
            "[browser]\nfoo=imported\n[proxy]\nenabled=true\n",
        )
        .unwrap();
        // First open merges it; the first emit then advertises the
        // non-excluded imported keys.
        a.set("default", "browser/trigger", SValue::Int(1));

        let wire = a.wire();
        let keys: Vec<&str> = wire
            .iter()
            .filter_map(|o| o["body"]["key"].as_str())
            .collect();
        assert!(keys.contains(&"browser/trigger"), "{keys:?}");
        assert!(keys.contains(&"browser/foo"), "import not advertised: {keys:?}");
        assert!(!keys.contains(&"proxy/enabled"), "excluded key advertised");

        let rep = b.import(&wire);
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        assert_eq!(
            b.get("default", "browser/foo"),
            Some(SValue::Str("imported".into()))
        );
        assert_eq!(b.get("default", "proxy/enabled"), None);
    }

    #[test]
    fn global_scope_never_emits() {
        let _g = guard();
        let a = Dev::new("glA");
        let h = RcSettings::open(&a.root, None).unwrap();
        h.set("browser/foo", SValue::Str("g".into())).unwrap();
        h.remove("browser/foo").unwrap();
        // No engine ops — and no profile-scope binding exists for
        // the flat path.
        assert!(a.engine().export(Some(STORE), 0).unwrap().is_empty());
    }

    #[test]
    fn malformed_ops_are_refused_not_applied() {
        let _g = guard();
        let a = Dev::new("bdA");
        let b = Dev::new("bdB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        // Hand-built bad ops still verify + import (the sig is
        // honest) but the apply lane refuses them.
        for body in [
            json!({"t": "set", "key": "browser/foo"}),          // no val
            json!({"t": "set", "key": "browser/foo", "val": {"t": "zz", "v": 1}}),
            json!({"t": "wat", "key": "browser/foo"}),
            json!({"t": "del"}),                                 // no key
        ] {
            let op = a.engine().append(STORE, "default", &body).unwrap();
            let rep = b.import(&[op.to_json()]);
            assert_eq!(rep.accepted, 1, "{body}");
            assert_eq!(rep.applied, 0, "{body}");
            assert_eq!(rep.apply_errors.len(), 1, "{body}");
        }
        assert!(b.settings("default").list(None).unwrap().is_empty());
    }
}
