//! SYNC05 — the history CRDT adapter on the SYNC01 op-log.
//!
//! Semantics (documented honestly — this is what the code does):
//!
//!   * One union-set element per visit, keyed by a visit-id that is
//!     deterministic over (url, visited_at) — device-independent, so
//!     identical pre-sync visits on two devices name the same
//!     element and dedup to one row instead of doubling.  Two
//!     same-url same-millisecond visits are indistinguishable to the
//!     deduped listing anyway, so collapsing them is free.
//!   * `visit` (id, url, title, ts) is the only op type — append-
//!     only union.  On apply: an unknown element inserts; a known
//!     element merges the title last-writer-wins by (lamport,
//!     device_id) stamp, an empty remote title never erasing a
//!     recorded one.  `update_title` rides the same op — it resolves
//!     the row's (url, ts) at mutation time and re-emits.
//!   * Dedup by visit-id is two-layered: the `seen` map replayed
//!     from the op-log knows every op'd element, and a row probe on
//!     (url, ts) catches elements the local table holds pre-sync
//!     (never advertised — e.g. a remote op landing before the first
//!     local flush).
//!   * Deletes stay local BY CONTRACT: remove/clear/clear_since/
//!     expire record no events and emit no ops — a peer's delete
//!     never moves this device's copy, and ours never moves theirs.
//!     The honest edge: a remote op for an element this device
//!     deleted before ever syncing it will insert it again (it was
//!     never seen); the device can delete it again, and the
//!     post-merge retention pass re-prunes it anyway.
//!   * Per-device retention applied locally after merge:
//!     expire_older_than records the widest cutoff in the store's
//!     meta table; apply_remote re-runs it after every merged op so
//!     a peer's longer history cannot resurrect rows this device
//!     already expired.  The prune itself emits nothing.
//!
//! Durability: no adapter db — SyncEngine's signed op-log IS the
//! durable CRDT state; `seen` rebuilds by replaying it on first
//! use.
//!
//! Emission: the store records semantic events (HistEvent) on every
//! union-growing mutation; `history::with` hands them to
//! flush_local, which maps the bound file (<root>/profiles/<scope>/
//! history.db) back to the sync scope and appends signed ops.  The
//! first flush after hydrate also advertises every local row whose
//! visit-id is still unknown — the pre-sync history flows to the
//! group once.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::{Arc, Mutex};

use serde_json::json;
use sha2::{Digest, Sha256};

use crate::error::{fail, RcResult, RcStatus};
use crate::facade;
use crate::history::{self, HistEvent, HistOutEv, HistoryStore};
use crate::notify;
use crate::sync::{self, SyncEngine, WireOp};

/// (lamport, device_id) — the total order the title merge sorts by.
type Stamp = (u64, String);

fn stamp_of(op: &WireOp) -> Stamp {
    (op.lamport, op.device_id.clone())
}

const MAX_VID: usize = 128;
const MAX_URL: usize = 256 * 1024;

fn hex(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for b in data {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

/// The element id — hex-SHA-256 over a namespaced (url, ts) frame.
fn visit_id(url: &str, visited_at: i64) -> String {
    let mut d = Sha256::new();
    d.update(b"ARHV1\0");
    d.update(url.as_bytes());
    d.update(b"\0");
    d.update(visited_at.to_le_bytes());
    hex(&d.finalize())
}

/// One sync scope's (profile's) history union state — rebuilt from
/// the op-log on first use, so it carries no files of its own.
pub(crate) struct HistSync {
    /// visit-id -> stamp of the last op applied/emitted for it —
    /// the union-set membership set AND the title-LWW bookkeeping.
    seen: BTreeMap<String, Stamp>,
    hydrated: bool,
    /// First-touch advertise runs once per adapter lifetime — after
    /// it, local rows only ever reach the log through events.
    advertised: bool,
}

/// The per-(root, scope) adapters.  Locked only to fetch/insert —
/// never held across a call into the store or engine.
static REG: Mutex<BTreeMap<(String, String), HistSync>> = Mutex::new(BTreeMap::new());

impl HistSync {
    fn new() -> HistSync {
        HistSync {
            seen: BTreeMap::new(),
            hydrated: false,
            advertised: false,
        }
    }

    fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
        fail(RcStatus::InvalidArgument, msg.into())
    }

    /// Rebuild from the engine's durable log: replay this scope's
    /// visit ops into the seen map — the newest stamp wins, which is
    /// exactly the title-LWW frontier.
    fn hydrate(&mut self, engine: &SyncEngine, scope: &str) -> RcResult<()> {
        if self.hydrated {
            return Ok(());
        }
        self.hydrated = true;
        for op in engine.export(Some("history"), 0)? {
            if op.scope != scope || op.body["t"].as_str() != Some("visit") {
                continue;
            }
            if let Some(id) = op.body["id"].as_str() {
                let s = stamp_of(&op);
                let e = self.seen.entry(id.to_string()).or_insert_with(|| s.clone());
                if s > *e {
                    *e = s;
                }
            }
        }
        Ok(())
    }

    // ---- emit (local events -> signed ops) --------------------------

    fn append_visit(
        &mut self,
        engine: &SyncEngine,
        scope: &str,
        url: &str,
        title: &str,
        ts: i64,
    ) -> RcResult<()> {
        let vid = visit_id(url, ts);
        let op = engine.append(
            "history",
            scope,
            &json!({"t":"visit","id":vid,"url":url,"title":title,"ts":ts}),
        )?;
        self.seen.insert(vid, stamp_of(&op));
        Ok(())
    }

    fn emit_event(
        &mut self,
        engine: &SyncEngine,
        scope: &str,
        ev: &HistEvent,
    ) -> RcResult<()> {
        match ev {
            // Emit-dedup: the element is already in the union (its op
            // is logged or applied), so a raw duplicate row is free.
            HistEvent::Visited { url, title, ts } => {
                if self.seen.contains_key(&visit_id(url, *ts)) {
                    return Ok(());
                }
                self.append_visit(engine, scope, url, title, *ts)
            }
            // Always emit — the title merge on the peer needs it.
            HistEvent::Titled { url, title, ts } => {
                self.append_visit(engine, scope, url, title, *ts)
            }
        }
    }

    // ---- apply (verified remote/replayed op -> store) ---------------

    /// Apply one op; true when the visible listing changed.
    fn apply(&mut self, h: &mut HistoryStore, op: &WireOp) -> RcResult<bool> {
        let body = &op.body;
        if body["t"].as_str() != Some("visit") {
            return Self::invalid("bad history op");
        }
        let id = body["id"].as_str().unwrap_or("");
        let url = body["url"].as_str().unwrap_or("");
        let title = body["title"].as_str().unwrap_or("");
        let Some(ts) = body["ts"].as_i64() else {
            return Self::invalid("bad visit op");
        };
        if id.is_empty()
            || id.len() > MAX_VID
            || url.is_empty()
            || url.len() > MAX_URL
            || id != visit_id(url, ts)
        {
            // The id IS the dedup key — a mismatched one would still
            // apply consistently everywhere, but refusing keeps a
            // buggy member from minting colliding element ids.
            return Self::invalid("bad visit op");
        }
        let s = stamp_of(op);
        // `seen` is the stamp frontier (hydrate seeds it from the
        // op-log — which already holds this very op on a lazy first
        // apply), so element dedup must ask the store itself: a row
        // for (url, ts) means the element already materialized,
        // however its op got here.
        let prev = self.seen.get(id).cloned().unwrap_or_default();
        let mut changed = false;
        if h.visit_exists(url, ts)? {
            // Merge, never double: the title moves iff the op is
            // strictly newer than the recorded frontier (an
            // un-stamped pre-sync row loses to any real op), and an
            // empty remote title never erases.
            if s > prev && !title.is_empty() {
                changed = h.apply_title(url, title, ts)?;
            }
        } else {
            h.apply_visit(url, title, ts)?;
            changed = true;
        }
        if s > prev {
            self.seen.insert(id.to_string(), s);
        }
        Ok(changed)
    }
}

// ---- plumbing --------------------------------------------------------

/// The outbox drain hooked from history::with — groups events by
/// the (root, scope) their file binding implies and appends signed
/// ops.  Events on non-profile files are unsynced and drop away.
pub(crate) fn flush_local(h: &mut HistoryStore) {
    if h.outbox.is_empty() {
        return;
    }
    let events = h.take_outbox();
    // Per-scope queues keep each group's internal order.
    let mut groups: Vec<(String, String, Vec<HistOutEv>)> = Vec::new();
    for ev in events {
        let Some((root, scope)) = ev.path.as_deref().and_then(binding_scope) else {
            continue;
        };
        match groups
            .iter_mut()
            .find(|(r, s, _)| *r == root && *s == scope)
        {
            Some((_, _, q)) => q.push(ev),
            None => groups.push((root, scope, vec![ev])),
        }
    }
    for (root, scope, evs) in groups {
        if let Err(done) = emit_scope(&root, &scope, h, &evs) {
            // Re-queue whatever didn't emit — a transient engine
            // failure must not silently drop a user edit.
            let mut rest: Vec<HistOutEv> = evs[done..].to_vec();
            rest.extend(h.take_outbox());
            h.outbox = rest;
            return;
        }
    }
}

/// Emit one scope's queued events: hydrate the adapter once, append
/// ops per event, then the one-time first-touch advertise.  Err(done)
/// = the first `done` events did emit; the rest are for the caller
/// to re-queue.
fn emit_scope(
    root: &str,
    scope: &str,
    h: &mut HistoryStore,
    evs: &[HistOutEv],
) -> Result<(), usize> {
    let engine = SyncEngine::open(root).map_err(|_| 0usize)?;
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let key = (root.to_string(), scope.to_string());
    let hs = reg.entry(key).or_insert_with(HistSync::new);
    if !hs.hydrated {
        hs.hydrate(&engine, scope).map_err(|_| 0usize)?;
    }
    for (i, oe) in evs.iter().enumerate() {
        hs.emit_event(&engine, scope, &oe.ev).map_err(|_| i)?;
    }
    // First-touch merge, after the events so freshly-emitted visits
    // don't get a second op: advertise every local row the seen-set
    // still doesn't know (pre-sync history flows to the group once).
    if !hs.advertised {
        hs.advertised = true;
        advertise(hs, h, &engine, scope).map_err(|_| evs.len())?;
    }
    Ok(())
}

/// Emit visits for rows with no seen-set entry — runs once per
/// adapter lifetime.
fn advertise(
    hs: &mut HistSync,
    h: &HistoryStore,
    engine: &SyncEngine,
    scope: &str,
) -> RcResult<()> {
    for (url, title, ts) in h.all_visits()? {
        if !hs.seen.contains_key(&visit_id(&url, ts)) {
            hs.append_visit(engine, scope, &url, &title, ts)?;
        }
    }
    Ok(())
}

/// <root>/profiles/<scope>/history.db → (root, scope) — the binding
/// every emit path funnels through; anything else is not a synced
/// profile store.
fn binding_scope(path: &Path) -> Option<(String, String)> {
    if path.file_name()?.to_str()? != "history.db" {
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

/// Register the apply lane for `root`'s sync engine — called from
/// every sync-domain MW op so the seam always routes to the core
/// the call is bound to.
pub(crate) fn install(root: &str) {
    let root = root.to_string();
    sync::register_applier(
        "history",
        Arc::new(move |op: &WireOp| apply_remote(&root, op).map(|_| ())),
    );
}

fn apply_remote(root: &str, op: &WireOp) -> RcResult<bool> {
    let scope = facade::effective_profile(&op.scope).to_string();
    let dir = facade::profile_dir(Path::new(root), &scope);
    let db = dir.join("history.db").to_string_lossy().into_owned();
    let changed = history::with(|h| -> RcResult<bool> {
        h.open(Some(db.as_str()))?;
        let engine = SyncEngine::open(root)?;
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        let hs = reg
            .entry((root.to_string(), scope.clone()))
            .or_insert_with(HistSync::new);
        if !hs.hydrated {
            hs.hydrate(&engine, &scope)?;
        }
        let changed = hs.apply(h, op)?;
        // Per-device retention, re-run after the merge: the boundary
        // this device last applied prunes any resurrected rows —
        // deletes stay local, so the prune emits nothing.
        if let Some(cut) = h.retention_cutoff()? {
            h.expire_older_than(cut)?;
        }
        Ok(changed)
    })?;
    if changed {
        let _hint = notify::profile_hint(&scope);
        notify::emit("history");
        notify::emit_event("history/changed", json!({ "op": "visit" }));
    }
    Ok(changed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::Value;
    use std::path::PathBuf;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-hsync-{}-{}-{}",
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

    /// A "device": a data root plus its profile history.db — the
    /// same shape the singleton store gets driven through in
    /// production.
    struct Dev {
        root: String,
        db: PathBuf,
    }

    impl Dev {
        fn new(tag: &str) -> Dev {
            let root = tmpdir(tag);
            let dir = Path::new(&root).join("profiles").join("default");
            std::fs::create_dir_all(&dir).unwrap();
            Dev {
                root,
                db: dir.join("history.db"),
            }
        }
        fn engine(&self) -> SyncEngine {
            SyncEngine::open(&self.root).unwrap()
        }
        /// Mutate the shared store bound to THIS device's db.
        fn with_h<R>(&self, f: impl FnOnce(&mut HistoryStore) -> R) -> R {
            history::with(|h| {
                h.open(Some(self.db.to_str().unwrap())).unwrap();
                f(h)
            })
        }
        /// The deduped listing as a comparable serialization.
        fn dump(&self) -> Vec<String> {
            self.with_h(|h| {
                let q: Vec<Value> =
                    serde_json::from_str(&h.query("{}").unwrap()).unwrap();
                q.iter().map(|r| r.to_string()).collect()
            })
        }
        fn wire(&self) -> Vec<Value> {
            self.engine()
                .export(Some("history"), 0)
                .unwrap()
                .iter()
                .map(|o| o.to_json())
                .collect()
        }
        /// Only the ops THIS device authored — an export carries the
        /// remote ops the log merged too.
        fn own_ops(&self) -> Vec<Value> {
            let id = self.engine().device_id();
            self.wire()
                .into_iter()
                .filter(|o| o["device_id"].as_str() == Some(id.as_str()))
                .collect()
        }
        fn pair(&self, peer: &Dev) {
            let pe = peer.engine();
            self.engine()
                .member_add(&pe.public_key(), "peer.onion", "default")
                .unwrap();
        }
        fn import(&self, docs: &[Value]) -> sync::ImportReport {
            install(&self.root);
            self.engine().import(docs).unwrap()
        }
    }

    #[test]
    fn visits_round_trip_and_dedup() {
        let _g = guard();
        let a = Dev::new("rtA");
        let b = Dev::new("rtB");
        a.pair(&b);
        b.pair(&a);

        a.with_h(|h| {
            h.add("http://a/", "A", 1000).unwrap();
            h.add("http://b/", "B", 2000).unwrap();
            // A raw duplicate row costs no second op — the visit-id
            // already sits in the seen set.
            h.add("http://b/", "B", 2000).unwrap();
        });
        let wire = a.wire();
        assert_eq!(wire.len(), 2, "{wire:?}");

        let rep = b.import(&wire);
        assert_eq!(rep.accepted, 2, "{:?}", rep.rejected);
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        let rows = b.dump();
        assert_eq!(rows.len(), 2, "{rows:?}");
        assert!(rows[0].contains("http://b/"));
        assert!(rows[1].contains("http://a/"));

        // Re-delivery is idempotent at both layers.
        let rep = b.import(&wire);
        assert_eq!(rep.duplicates, 2);
        assert_eq!(rep.applied, 0);
        assert_eq!(b.dump().len(), 2);
    }

    #[test]
    fn pre_sync_histories_union_without_doubling() {
        let _g = guard();
        let a = Dev::new("unA");
        let b = Dev::new("unB");
        a.pair(&b);
        b.pair(&a);

        // Both devices hold the SAME visit before any sync — the
        // deterministic visit-id must merge it, not double it.
        for d in [&a, &b] {
            d.with_h(|h| {
                h.add("http://same/", "Same", 1000).unwrap();
            });
        }
        a.with_h(|h| h.add("http://a-only/", "A", 2000).unwrap());
        b.with_h(|h| h.add("http://b-only/", "B", 3000).unwrap());

        let wa = a.wire();
        let wb = b.wire();
        // A advertised 2 elements, B advertised 2 (the shared one
        // names the same visit-id on both wires but is a distinct
        // op — dedup is element-level, not op-level).
        b.import(&wa);
        a.import(&wb);
        let da = a.dump();
        let db_ = b.dump();
        assert_eq!(da, db_, "\n--- a ---\n{da:?}\n--- b ---\n{db_:?}");
        assert_eq!(da.len(), 3, "{da:?}");
        let hits = da.iter().filter(|r| r.contains("http://same/")).count();
        assert_eq!(hits, 1, "shared visit must dedup: {da:?}");
    }

    #[test]
    fn deletes_and_clear_stay_local() {
        let _g = guard();
        let a = Dev::new("dlA");
        let b = Dev::new("dlB");
        a.pair(&b);
        b.pair(&a);

        a.with_h(|h| {
            h.add("http://one/", "1", 1000).unwrap();
            h.add("http://two/", "2", 2000).unwrap();
        });
        b.import(&a.wire());
        assert_eq!(b.dump().len(), 2);

        // B deletes one row and later clears entirely — neither
        // produces an op for A to consume (the log still carries
        // A's imported remote ops; only B-authored ops count).
        b.with_h(|h| {
            h.remove("http://one/", "1", 1000).unwrap();
        });
        assert!(b.own_ops().is_empty(), "remove must not emit ops");
        b.with_h(|h| {
            h.clear().unwrap();
        });
        assert!(b.own_ops().is_empty(), "clear must not emit ops");
        assert!(b.dump().is_empty());

        // A's copy is untouched, and re-importing A's wire on B is a
        // dedup no-op — the delete does not resurrect via the log.
        a.with_h(|h| h.add("http://three/", "3", 3000).unwrap());
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        // B now holds only the new visit — its deletes stayed local.
        let rows = b.dump();
        assert_eq!(rows.len(), 1, "{rows:?}");
        assert!(rows[0].contains("http://three/"));
        assert_eq!(a.dump().len(), 3);
    }

    #[test]
    fn retention_reapplies_after_merge() {
        let _g = guard();
        let a = Dev::new("exA");
        let b = Dev::new("exB");
        a.pair(&b);
        b.pair(&a);

        // B's device-wide retention boundary: everything at or
        // before 1500 is expired — recorded in B's own store.
        b.with_h(|h| {
            h.add("http://old-local/", "L", 1000).unwrap();
            assert_eq!(h.expire_older_than(1500).unwrap(), 1);
        });
        assert!(b.dump().is_empty());

        // A holds an ancient visit plus a fresh one; the union
        // reaches B — the ancient row is pruned by B's own policy,
        // the fresh one survives.
        a.with_h(|h| {
            h.add("http://ancient/", "old", 1000).unwrap();
            h.add("http://fresh/", "new", 5000).unwrap();
        });
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        let rows = b.dump();
        assert_eq!(rows.len(), 1, "{rows:?}");
        assert!(rows[0].contains("http://fresh/"));

        // And A is untouched — retention never left B's device.
        assert_eq!(a.dump().len(), 2);
    }

    #[test]
    fn title_updates_merge_lww() {
        let _g = guard();
        let a = Dev::new("ttA");
        let b = Dev::new("ttB");
        a.pair(&b);
        b.pair(&a);

        a.with_h(|h| h.add("http://x/", "", 1000).unwrap());
        b.import(&a.wire());
        assert!(b.dump()[0].contains("http://x/"));

        // The title lands on the same element — B rewrites it.
        b.with_h(|h| h.update_title("http://x/", "Titled").unwrap());
        let rep = a.import(&b.wire());
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert!(a.dump()[0].contains("Titled"), "{:?}", a.dump());

        // A stale re-delivery of the original empty-title op cannot
        // clobber the newer title.
        a.with_h(|h| h.update_title("http://x/", "Newer").unwrap());
        let rep = b.import(&a.wire());
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert!(b.dump()[0].contains("Newer"), "{:?}", b.dump());
    }
}
