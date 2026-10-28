//! SYNC06 — open-tabs presence on the SYNC01 op-log.
//!
//! Semantics (documented honestly — this is what the code does):
//!
//!   * One op type, "presence": the body's "windows" array is a
//!     WHOLE-DOC snapshot of the device's open tab set for the
//!     scope — {t:"presence", v:1, windows:[{current, tabs:[{url,
//!     title?, pinned?, container?}]}]}.  Every publish re-issues
//!     the complete set, so a close propagates by absence — the
//!     next doc simply lacks the closed tab.  No per-tab ops, no
//!     tombstones; an empty windows array is a real doc (the
//!     "everything closed" wipe).
//!   * Presence is EPHEMERAL, not a durable CRDT: the newest doc
//!     per device wins outright — LWW on the op's (lamport,
//!     device_id) stamp, never a merge — and the apply lane holds
//!     it in memory only.  It writes to NO store: a peer's open
//!     tabs are a read-only view, so receiving one can never mint
//!     a history row (or any durable record) on this device.
//!     Opening a listed tab is the user's own navigation — its
//!     history entry is local and ordinary, exactly like typing
//!     the url.
//!   * What durability exists is the op-log's, not the adapter's:
//!     a restart replays the newest per-device docs off the
//!     durable log, so the view shows last-known state — honest
//!     but stale-capable (no TTL yet; SYNC11 records the limit).
//!   * The read side reports every OTHER device's latest doc for
//!     the scope — "tabs on my other devices" is exactly the
//!     non-self set.

use std::collections::BTreeMap;
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

use crate::error::{fail, RcResult, RcStatus};
use crate::notify;
use crate::sync::{self, SyncEngine, WireOp};

/// (lamport, device_id) — the total order the per-device LWW sorts
/// by.  Unique per op since device_id breaks same-tick ties.
type Stamp = (u64, String);

fn stamp_of(op: &WireOp) -> Stamp {
    (op.lamport, op.device_id.clone())
}

const MAX_WINDOWS: usize = 32;
const MAX_TABS_PER_WINDOW: usize = 256;
const MAX_URL: usize = 64 * 1024;
const MAX_TITLE: usize = 4 * 1024;
const MAX_LABEL: usize = 128;

fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::InvalidArgument, msg.into())
}

/// Validate + rebuild a windows array into the canonical doc shape —
/// publish and apply share it, so the wire never carries fields the
/// view does not read and an over-cap doc is refused rather than
/// silently truncated (a clipped set would lie about what is open).
fn normalize_windows(v: &Value) -> RcResult<Value> {
    let Some(wins) = v.as_array() else {
        return invalid("\"windows\" must be an array");
    };
    if wins.len() > MAX_WINDOWS {
        return invalid(format!("\"windows\" exceeds {MAX_WINDOWS} entries"));
    }
    let mut out = Vec::with_capacity(wins.len());
    for w in wins {
        let Some(wo) = w.as_object() else {
            return invalid("window entry must be an object");
        };
        let current = match wo.get("current") {
            None | Some(Value::Null) => -1,
            Some(v) => v
                .as_i64()
                .ok_or_else(|| crate::error::Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "\"current\" must be an integer".into(),
                })?,
        };
        let Some(tabs) = wo.get("tabs").and_then(Value::as_array) else {
            return invalid("window entry is missing a \"tabs\" array");
        };
        if tabs.len() > MAX_TABS_PER_WINDOW {
            return invalid(format!("\"tabs\" exceeds {MAX_TABS_PER_WINDOW} entries"));
        }
        let mut wtabs = Vec::with_capacity(tabs.len());
        for t in tabs {
            let Some(to) = t.as_object() else {
                return invalid("tab entry must be an object");
            };
            let url = to
                .get("url")
                .and_then(Value::as_str)
                .unwrap_or("");
            if url.is_empty() || url.len() > MAX_URL {
                return invalid("tab entry has a missing or oversized \"url\"");
            }
            let mut tab = serde_json::Map::new();
            tab.insert("url".into(), json!(url));
            if let Some(title) = to.get("title").and_then(Value::as_str) {
                if title.len() > MAX_TITLE {
                    return invalid("tab \"title\" too large");
                }
                if !title.is_empty() && title != url {
                    tab.insert("title".into(), json!(title));
                }
            }
            if to.get("pinned").and_then(Value::as_bool) == Some(true) {
                tab.insert("pinned".into(), json!(true));
            }
            if let Some(container) = to.get("container").and_then(Value::as_str) {
                if container.len() > MAX_LABEL {
                    return invalid("tab \"container\" too large");
                }
                if !container.is_empty() {
                    tab.insert("container".into(), json!(container));
                }
            }
            wtabs.push(Value::Object(tab));
        }
        out.push(json!({"current": current, "tabs": wtabs}));
    }
    Ok(Value::Array(out))
}

/// The emit half: snapshot → signed "tabs"-store op for `scope`.
/// `windows` is the caller's whole-doc view; an empty array is a
/// legal publish — the wipe is how "everything closed" propagates.
pub(crate) fn publish(root: &str, scope: &str, windows: &Value) -> RcResult<WireOp> {
    let windows = normalize_windows(windows)?;
    SyncEngine::open(root)?.append(
        "tabs",
        scope,
        &json!({"t": "presence", "v": 1, "windows": windows}),
    )
}

/// One device's newest published doc — the whole-record LWW unit.
struct DeviceDoc {
    stamp: Stamp,
    wall_ms: i64,
    windows: Value,
}

/// One scope's presence view — device_id → that device's newest
/// doc.  In-memory only BY DESIGN: presence is ephemeral, not
/// synced state; a restart rebuilds by replaying the log.
#[derive(Default)]
struct ScopePresence {
    docs: BTreeMap<String, DeviceDoc>,
    hydrated: bool,
}

/// The per-(root, scope) views.  Locked only to fetch/insert —
/// never held across a call into the engine.
static REG: Mutex<BTreeMap<(String, String), ScopePresence>> = Mutex::new(BTreeMap::new());

/// The stamp-ordered LWW swap — shared by hydrate replay and live
/// apply.  Returns true when the doc actually moved forward.
fn offer(p: &mut ScopePresence, op: &WireOp, windows: Value) -> bool {
    let s = stamp_of(op);
    if let Some(d) = p.docs.get(&op.device_id) {
        if s <= d.stamp {
            return false;
        }
    }
    p.docs.insert(
        op.device_id.clone(),
        DeviceDoc {
            stamp: s,
            wall_ms: op.wall_ms,
            windows,
        },
    );
    true
}

/// Rebuild one scope's view off the durable log: replay this
/// scope's presence ops newest-wins.  Malformed logged bodies are
/// skipped — a peer's bad op already counted once on import; the
/// replay only reconstructs what applied.
fn hydrate(p: &mut ScopePresence, engine: &SyncEngine, scope: &str) -> RcResult<()> {
    if p.hydrated {
        return Ok(());
    }
    p.hydrated = true;
    for op in engine.export(Some("tabs"), 0)? {
        if op.scope != scope || op.body["t"].as_str() != Some("presence") {
            continue;
        }
        if let Ok(windows) = normalize_windows(&op.body["windows"]) {
            offer(p, &op, windows);
        }
    }
    Ok(())
}

/// Register the apply lane for `root`'s sync engine — called from
/// every sync-domain MW op so the seam always routes to the core
/// the call is bound to.
pub(crate) fn install(root: &str) {
    let root = root.to_string();
    sync::register_applier("tabs", Arc::new(move |op: &WireOp| apply_remote(&root, op)));
}

fn apply_remote(root: &str, op: &WireOp) -> RcResult<()> {
    if op.body["t"].as_str() != Some("presence") {
        return invalid("bad tabs op");
    }
    let windows = normalize_windows(&op.body["windows"])?;
    let scope = op.scope.clone();
    let engine = SyncEngine::open(root)?;
    let updated = {
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        let sp = reg
            .entry((root.to_string(), scope.clone()))
            .or_default();
        if !sp.hydrated {
            hydrate(sp, &engine, &scope)?;
        }
        offer(sp, op, windows)
    };
    if updated {
        notify::emit("synctabs");
        notify::emit_event("synctabs/changed", json!({ "op": "presence" }));
    }
    Ok(())
}

/// The read half: the newest doc every OTHER device published for
/// `scope`, in device_id order — the "tabs on my other devices"
/// listing.  This device's own doc is never reported: its live
/// tabs are already on screen.
pub(crate) fn peers(root: &str, scope: &str) -> RcResult<Vec<Value>> {
    let engine = SyncEngine::open(root)?;
    let own = engine.device_id();
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let sp = reg.entry((root.to_string(), scope.to_string())).or_default();
    if !sp.hydrated {
        hydrate(sp, &engine, scope)?;
    }
    Ok(sp
        .docs
        .iter()
        .filter(|(id, _)| **id != own)
        .map(|(id, d)| {
            json!({
                "device_id": id,
                "updated_ms": d.wall_ms,
                "windows": d.windows,
            })
        })
        .collect())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::history::{self, HistoryStore};
    use std::path::Path;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-tsync-{}-{}-{}",
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

    /// A "device": a data root whose sync engine signs + imports.
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
        fn pair(&self, peer: &Dev, scope: &str) {
            let pe = peer.engine();
            self.engine()
                .member_add(&pe.public_key(), "peer.onion", scope)
                .unwrap();
        }
        fn publish(&self, scope: &str, windows: Value) {
            publish(&self.root, scope, &windows).unwrap();
        }
        fn wire(&self) -> Vec<Value> {
            self.engine()
                .export(Some("tabs"), 0)
                .unwrap()
                .iter()
                .map(|o| o.to_json())
                .collect()
        }
        fn import(&self, docs: &[Value]) -> sync::ImportReport {
            install(&self.root);
            self.engine().import(docs).unwrap()
        }
        fn peers(&self, scope: &str) -> Vec<Value> {
            peers(&self.root, scope).unwrap()
        }
    }

    fn doc(urls: &[&str]) -> Value {
        json!([{
            "current": 0,
            "tabs": urls.iter().map(|u| json!({"url": u})).collect::<Vec<_>>(),
        }])
    }

    #[test]
    fn publish_propagates_and_closes_shrink_the_doc() {
        let _g = guard();
        let a = Dev::new("prA");
        let b = Dev::new("prB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        a.publish("default", doc(&["http://one/", "http://two/"]));
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);

        let devices = b.peers("default");
        assert_eq!(devices.len(), 1, "{devices:?}");
        assert_eq!(devices[0]["device_id"], a.engine().device_id());
        let tabs = devices[0]["windows"][0]["tabs"].as_array().unwrap();
        assert_eq!(tabs.len(), 2);
        assert_eq!(tabs[1]["url"], "http://two/");

        // A closes a tab and republishes — the new whole doc
        // replaces the old one outright, so the close propagates
        // by absence.
        a.publish("default", doc(&["http://one/"]));
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        let devices = b.peers("default");
        let tabs = devices[0]["windows"][0]["tabs"].as_array().unwrap();
        assert_eq!(tabs.len(), 1, "{devices:?}");
        assert_eq!(tabs[0]["url"], "http://one/");

        // The wipe: an empty windows array is a real publish, not
        // a deleted device.
        a.publish("default", json!([]));
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1);
        let devices = b.peers("default");
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0]["windows"].as_array().unwrap().len(), 0);
    }

    #[test]
    fn peers_never_lists_the_local_device() {
        let _g = guard();
        let a = Dev::new("sfA");
        let b = Dev::new("sfB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        a.publish("default", doc(&["http://a/"]));
        b.publish("default", doc(&["http://b/"]));
        let wa = a.wire();
        let wb = b.wire();
        b.import(&wa);
        a.import(&wb);

        // B sees only A's doc — its own live tabs are not "other".
        let devices = b.peers("default");
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0]["device_id"], a.engine().device_id());
        let devices = a.peers("default");
        assert_eq!(devices.len(), 1);
        assert_eq!(devices[0]["device_id"], b.engine().device_id());
        // And a different scope sees nothing — docs are per-group.
        assert!(b.peers("work").is_empty());
    }

    #[test]
    fn newer_doc_wins_regardless_of_arrival() {
        let _g = guard();
        let a = Dev::new("lwA");
        let b = Dev::new("lwB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        a.publish("default", doc(&["http://old/"]));
        let first = a.wire();
        a.publish("default", doc(&["http://new/"]));
        let all = a.wire();

        // Deliver newest first, then the stale doc — the LWW swap
        // must refuse to move backwards.
        b.import(&all[1..]);
        b.import(&first[..1]);
        let devices = b.peers("default");
        assert_eq!(
            devices[0]["windows"][0]["tabs"][0]["url"],
            "http://new/",
            "a stale presence doc must never overwrite a newer one"
        );
    }

    #[test]
    fn malformed_docs_are_refused_not_applied() {
        let _g = guard();
        let a = Dev::new("bdA");
        let b = Dev::new("bdB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        // Publish-time validation.
        assert!(publish(&a.root, "default", &json!({"not": "array"})).is_err());
        assert!(publish(&a.root, "default", &json!([{"tabs": [{"url": ""}]}])).is_err());
        assert!(publish(&a.root, "default", &json!([{"tabs": [{"nourl": 1}]}])).is_err());

        // A hand-built bad op still verifies + imports (the sig is
        // honest) but the apply lane refuses it.
        let op = a
            .engine()
            .append("tabs", "default", &json!({"t": "presence", "v": 1, "windows": "junk"}))
            .unwrap();
        let rep = b.import(&[op.to_json()]);
        assert_eq!(rep.accepted, 1);
        assert_eq!(rep.applied, 0);
        assert_eq!(rep.apply_errors.len(), 1);
        assert!(b.peers("default").is_empty());
    }

    /// The spec's hard line: applying presence writes to NO store —
    /// a peer's open tabs are a view, never a visit.  B's history
    /// db for the scope must still not exist after the merge.
    #[test]
    fn applying_presence_writes_nothing_to_any_store() {
        let _g = guard();
        let a = Dev::new("nwA");
        let b = Dev::new("nwB");
        a.pair(&b, "default");
        b.pair(&a, "default");

        a.publish("default", doc(&["http://one/", "http://two/"]));
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1);
        assert_eq!(b.peers("default").len(), 1);

        let hist_db = Path::new(&b.root)
            .join("profiles")
            .join("default")
            .join("history.db");
        history::with(|h: &mut HistoryStore| {
            h.open(Some(hist_db.to_str().unwrap())).unwrap();
            let rows: Vec<Value> = serde_json::from_str(&h.query("{}").unwrap()).unwrap();
            assert!(rows.is_empty(), "presence apply minted history: {rows:?}");
        });
    }
}
