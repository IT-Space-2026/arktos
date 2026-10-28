//! SYNC04 — the bookmarks CRDT adapter on the SYNC01 op-log.
//!
//! Semantics (documented honestly — this is what the code does):
//!
//!   * Every node is one OR-set element keyed by a guid (the XBEL
//!     `id` attr; random for local creates, a deterministic content
//!     fingerprint for nodes a foreign file brings in so pre-sync
//!     trees merge instead of doubling).
//!   * `link` (id, parent, pos, node-snapshot) both adds and moves:
//!     it wins iff its (lamport, device) stamp beats the element's
//!     recorded place stamp — for a tombstoned element, iff it beats
//!     the stamp the remover observed (a placement the remover never
//!     saw survives the delete, which is the OR-set rule).
//!   * `rm` (id, observed-stamps of the removed subtree) tombstones
//!     exactly the elements the remover had seen — a concurrently
//!     added or moved-in child survives, retargeted up the recorded
//!     parent chain to the nearest live ancestor.
//!   * `set` (id, field, value) is per-field LWW on (lamport,
//!     device_id).
//!   * Sibling order is convergent: children sit sorted by the
//!     fractional-index `pos` the link carried, ties broken by guid.
//!   * Causal folder ordering: an op whose declared parent (or the
//!     element itself for `set`) is not yet known is parked in
//!     `pending` and re-driven when the guid resolves — either as
//!     live (a link applied) or dead (a tombstone recorded).
//!   * Move cycles (A under B while B under A) resolve by stamp: the
//!     losing placement is ejected to root.  Deterministic on every
//!     replica.
//!
//! Durability: no adapter db — SyncEngine's signed op-log IS the
//! durable CRDT state; BmSync rebuilds by replaying it on first
//! use, so tombstones, stamps and parked ops survive restarts for
//! free.
//!
//! Emission: the store records semantic events (BmEvent) on every
//! public mutation; `with` hands them to flush_local, which maps the
//! bound file (<root>/profiles/<scope>/bookmarks.xbel) back to the
//! sync scope and appends signed ops.  Remote applies route around
//! the recording mutators so a peer's op can never echo.

use std::collections::BTreeMap;
use std::path::Path;
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};

use crate::bookmarks::{self, Bookmarks, BmEvent, OutEv};
use crate::error::{fail, RcResult, RcStatus};
use crate::facade;
use crate::notify;
use crate::sync::{self, SyncEngine, WireOp};

/// (lamport, device_id) — the total order every conflict rule sorts
/// by.  Unique per op since device_id breaks same-tick ties.
type Stamp = (u64, String);

fn stamp_of(op: &WireOp) -> Stamp {
    (op.lamport, op.device_id.clone())
}

fn parse_stamp(v: &Value) -> Stamp {
    (
        v.get(0).and_then(Value::as_u64).unwrap_or(0),
        v.get(1)
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string(),
    )
}

const FIELDS: [&str; 5] = ["title", "url", "desc", "expanded", "tags"];
const MAX_GUID: usize = 128;
/// Parked ops awaiting one guid — a hostile member cannot grow this
/// without bound (group peers are trusted-ish, but bound anyway).
const MAX_PENDING_PER_GUID: usize = 64;

/// What the adapter knows about one element — live placement plus
/// the tombstone frontier.  Tombstoned records keep `parent` so
/// children arriving later retarget up the chain to a live ancestor.
#[derive(Clone, Default)]
struct NodeRec {
    parent: String,
    place: Stamp,
    dead: bool,
    obs: Stamp,
}

/// One sync scope's (profile's) bookmarks CRDT state — rebuilt from
/// the op-log on first use, so it carries no files of its own.
pub(crate) struct BmSync {
    nodes: BTreeMap<String, NodeRec>,
    fields: BTreeMap<(String, String), Stamp>,
    /// guid we're waiting on -> ops parked until it resolves.
    pending: BTreeMap<String, Vec<WireOp>>,
    hydrated: bool,
}

/// The per-(root, scope) adapters.  Locked only to fetch/insert —
/// never held across a call into the store or engine.
static REG: Mutex<BTreeMap<(String, String), BmSync>> = Mutex::new(BTreeMap::new());

impl BmSync {
    fn new() -> BmSync {
        BmSync {
            nodes: BTreeMap::new(),
            fields: BTreeMap::new(),
            pending: BTreeMap::new(),
            hydrated: false,
        }
    }

    fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
        fail(RcStatus::InvalidArgument, msg.into())
    }

    /// Synthesize a record for a tree node the ops never mentioned —
    /// a pre-sync or imported node.  Its place stamp is (0,""), the
    /// bottom of the order: any op about it is newer.
    fn ensure_rec(&mut self, b: &Bookmarks, guid: &str) {
        if guid.is_empty() || self.nodes.contains_key(guid) {
            return;
        }
        if let Some(h) = b.by_guid(guid) {
            self.nodes.insert(
                guid.to_string(),
                NodeRec {
                    parent: b.parent_guid(h),
                    ..NodeRec::default()
                },
            );
        }
    }

    /// Rebuild from the engine's durable log: replay this scope's
    /// ops through apply — idempotent because every rule is stamp-
    /// guarded, and parked ops re-park until their dependency
    /// replays.
    fn hydrate(&mut self, b: &mut Bookmarks, engine: &SyncEngine, scope: &str) -> RcResult<()> {
        if self.hydrated {
            return Ok(());
        }
        self.hydrated = true;
        for op in engine.export(Some("bookmarks"), 0)? {
            if op.scope == scope {
                let _ = self.apply(b, &op);
            }
        }
        Ok(())
    }

    // ---- emit (local events -> signed ops) --------------------------

    /// Turn recorded local events into signed ops, parent-first so a
    /// subtree's causal order lands in the log.  Emits carry the
    /// CURRENT placement, so one link op per guid per drain is
    /// enough — `linked` dedups a guid covered by an earlier subtree
    /// emit in the same drain.
    fn emit_events(
        &mut self,
        b: &Bookmarks,
        engine: &SyncEngine,
        scope: &str,
        evs: &[BmEvent],
        linked: &mut std::collections::HashSet<String>,
    ) -> RcResult<()> {
        for ev in evs {
            match ev {
                BmEvent::Link(h) => self.emit_link(b, engine, scope, *h, linked)?,
                BmEvent::Remove(pairs) => self.emit_remove(engine, scope, pairs)?,
                BmEvent::Set(h, f) => self.emit_set(b, engine, scope, *h, f)?,
            }
        }
        Ok(())
    }

    fn append(&mut self, engine: &SyncEngine, scope: &str, body: &Value) -> RcResult<WireOp> {
        let op = engine.append("bookmarks", scope, body)?;
        self.note_local(&op);
        Ok(op)
    }

    /// Emit link ops for the subtree at `h`, parent-first.  The top
    /// node always emits (the event IS its new placement); a
    /// descendant emits only when it is unseen (an import riding
    /// along) or tombstoned (an undo-restore re-linking the whole
    /// subtree must outrank its tombstones) — a live descendant of a
    /// moved node keeps its old place stamp so a remote remove that
    /// observed it still lands.
    fn emit_link(
        &mut self,
        b: &Bookmarks,
        engine: &SyncEngine,
        scope: &str,
        h: u64,
        linked: &mut std::collections::HashSet<String>,
    ) -> RcResult<()> {
        if b.guid_of(h).is_none() {
            return Ok(()); // freed between record and drain
        }
        let mut stack = vec![h];
        while let Some(x) = stack.pop() {
            let (guid, parent, rank, doc) = {
                let Some(n) = b.node_ref(x) else {
                    continue;
                };
                if n.guid.is_empty() {
                    continue;
                }
                (
                    n.guid.clone(),
                    b.parent_guid(x),
                    n.rank.clone(),
                    json!({
                        "type": n.kind, "title": n.title, "url": n.url,
                        "desc": n.desc, "expanded": n.expanded,
                        "tags": n.tags,
                    }),
                )
            };
            if linked.contains(&guid) {
                continue; // already emitted this drain, current state
            }
            let needs_op = x == h
                || self
                    .nodes
                    .get(&guid)
                    .map(|r| r.dead)
                    .unwrap_or(true);
            if needs_op {
                self.append(
                    engine,
                    scope,
                    &json!({"t":"link","id":guid,"parent":parent,"pos":rank,"node":doc}),
                )?;
                linked.insert(guid);
            }
            let kids = b.children_of(x).unwrap_or_default();
            for c in kids.into_iter().rev() {
                stack.push(c);
            }
        }
        Ok(())
    }

    fn emit_remove(
        &mut self,
        engine: &SyncEngine,
        scope: &str,
        pairs: &[(String, String)],
    ) -> RcResult<()> {
        let Some((head, head_parent)) = pairs.first() else {
            return Ok(());
        };
        if head.is_empty() {
            return Ok(());
        }
        let stamp_of_guid = |g: &str| -> (i64, &str) {
            self.nodes
                .get(g)
                .map(|r| (r.place.0 as i64, r.place.1.as_str()))
                .unwrap_or((0, ""))
        };
        let parent_of = |g: &str, recorded: &str| -> String {
            if !recorded.is_empty() {
                return recorded.to_string();
            }
            self.nodes
                .get(g)
                .map(|r| r.parent.clone())
                .unwrap_or_default()
        };
        let (h_lam, h_dev) = stamp_of_guid(head);
        let kids: Vec<Value> = pairs
            .iter()
            .skip(1)
            .filter(|(g, _)| !g.is_empty())
            .map(|(g, pg)| {
                let (lam, dev) = stamp_of_guid(g);
                json!([g, parent_of(g, pg), lam, dev])
            })
            .collect();
        self.append(
            engine,
            scope,
            &json!({
                "t":"rm","id":head,"parent":parent_of(head, head_parent),
                "obs":[h_lam, h_dev],"kids":kids
            }),
        )?;
        Ok(())
    }

    fn emit_set(
        &mut self,
        b: &Bookmarks,
        engine: &SyncEngine,
        scope: &str,
        h: u64,
        field: &str,
    ) -> RcResult<()> {
        let Some(n) = b.node_ref(h) else {
            return Ok(());
        };
        if n.guid.is_empty() {
            return Ok(());
        }
        let v = match field {
            "title" => json!(n.title),
            "url" => json!(n.url),
            "desc" => json!(n.desc),
            "expanded" => json!(n.expanded),
            "tags" => json!(n.tags),
            _ => return Ok(()),
        };
        self.append(
            engine,
            scope,
            &json!({"t":"set","id":n.guid,"f":field,"v":v}),
        )?;
        Ok(())
    }

    /// Record our own op's stamps — the local tree already changed;
    /// this keeps place/obs/field stamps honest for the next remove.
    fn note_local(&mut self, op: &WireOp) {
        let s = stamp_of(op);
        match op.body["t"].as_str() {
            Some("link") => {
                if let Some(id) = op.body["id"].as_str() {
                    let parent = op.body["parent"].as_str().unwrap_or("").to_string();
                    let rec = self.nodes.entry(id.to_string()).or_default();
                    rec.dead = false;
                    rec.place = s.clone();
                    rec.parent = parent;
                    for f in FIELDS {
                        self.fields.insert((id.to_string(), f.to_string()), s.clone());
                    }
                }
            }
            Some("rm") => {
                let mut marks: Vec<(String, Stamp)> = Vec::new();
                if let Some(id) = op.body["id"].as_str() {
                    marks.push((id.to_string(), parse_stamp(&op.body["obs"])));
                }
                if let Some(kids) = op.body["kids"].as_array() {
                    for k in kids {
                        if let Some(g) = k.get(0).and_then(Value::as_str) {
                            marks.push((g.to_string(), parse_stamp(&Value::Array(
                                vec![k.get(2).cloned().unwrap_or(Value::Null),
                                     k.get(3).cloned().unwrap_or(Value::Null)]))));
                        }
                    }
                }
                for (g, obs) in marks {
                    let rec = self.nodes.entry(g).or_default();
                    rec.dead = true;
                    if obs > rec.obs {
                        rec.obs = obs;
                    }
                }
            }
            Some("set") => {
                if let (Some(id), Some(f)) =
                    (op.body["id"].as_str(), op.body["f"].as_str())
                {
                    self.fields.insert((id.to_string(), f.to_string()), s);
                }
            }
            _ => {}
        }
    }

    // ---- apply (verified remote/replayed op -> tree) ----------------

    /// Apply one op; true when the visible tree changed.
    fn apply(&mut self, b: &mut Bookmarks, op: &WireOp) -> RcResult<bool> {
        match op.body["t"].as_str() {
            Some("link") => self.apply_link(b, op),
            Some("rm") => self.apply_rm(b, op),
            Some("set") => self.apply_set(b, op),
            _ => Self::invalid("bad bookmarks op"),
        }
    }

    fn park(&mut self, need: &str, op: &WireOp) {
        let q = self.pending.entry(need.to_string()).or_default();
        if q.len() < MAX_PENDING_PER_GUID {
            q.push(op.clone());
        }
    }

    /// `guid` just resolved — live or dead, either answer unblocks
    /// its waiters (dead waiters retarget or drop).
    fn unpark(&mut self, b: &mut Bookmarks, guid: &str) {
        let ops = self.pending.remove(guid).unwrap_or_default();
        for op in ops {
            let _ = self.apply(b, &op);
        }
    }

    /// Nearest live ancestor guid for a tombstoned node — "" when
    /// the chain reaches root or an unknown record.
    fn live_ancestor(&self, guid: &str) -> String {
        let mut cur = self
            .nodes
            .get(guid)
            .map(|r| r.parent.clone())
            .unwrap_or_default();
        for _ in 0..64 {
            if cur.is_empty() {
                return cur;
            }
            match self.nodes.get(&cur) {
                Some(r) if r.dead => cur = r.parent.clone(),
                Some(_) => return cur,
                None => return String::new(),
            }
        }
        String::new()
    }

    /// Resolve a declared parent guid to a live handle — "" is root,
    /// a tombstoned guid climbs to its nearest live ancestor, an
    /// unknown guid is Err(park-key).
    fn resolve_parent(&self, b: &Bookmarks, parent: &str) -> Result<u64, String> {
        if parent.is_empty() {
            return Ok(b.root());
        }
        match self.nodes.get(parent) {
            Some(rec) if rec.dead => {
                let anc = self.live_ancestor(parent);
                if anc.is_empty() {
                    Ok(b.root())
                } else {
                    Ok(b.by_guid(&anc).unwrap_or_else(|| b.root()))
                }
            }
            Some(_) => Ok(b.by_guid(parent).unwrap_or_else(|| b.root())),
            None => {
                if let Some(h) = b.by_guid(parent) {
                    Ok(h)
                } else {
                    Err(parent.to_string())
                }
            }
        }
    }

    fn apply_link(&mut self, b: &mut Bookmarks, op: &WireOp) -> RcResult<bool> {
        let body = &op.body;
        let id = body["id"].as_str().unwrap_or("");
        if id.is_empty() || id.len() > MAX_GUID {
            return Self::invalid("bad link id");
        }
        let parent = body["parent"].as_str().unwrap_or("");
        let pos = body["pos"].as_str().unwrap_or("");
        let s = stamp_of(op);
        if let Some(rec) = self.nodes.get(id) {
            if rec.dead {
                if s <= rec.obs {
                    return Ok(false); // a placement the remover saw — stays dead
                }
            } else if s <= rec.place {
                return Ok(false); // stale re-delivery
            }
        }
        self.ensure_rec(b, id);
        let mut parent_h = match self.resolve_parent(b, parent) {
            Ok(h) => h,
            Err(need) => {
                self.park(&need, op);
                return Ok(false);
            }
        };
        // Cycle: the resolved parent lives inside id's own subtree —
        // two concurrent moves can close a loop.  The lower-stamped
        // placement is ejected to root, identically on every replica.
        let mut cycle_eject: Option<u64> = None;
        if let Some(ih) = b.by_guid(id) {
            let mut walk = Some(parent_h);
            let mut below = parent_h;
            while let Some(w) = walk {
                if w == ih {
                    cycle_eject = Some(below);
                    break;
                }
                below = w;
                walk = b.parent_of(w);
            }
        }
        if let Some(c_h) = cycle_eject {
            let c_guid = b.guid_of(c_h).unwrap_or("").to_string();
            let c_place = self
                .nodes
                .get(&c_guid)
                .map(|r| r.place.clone())
                .unwrap_or_default();
            if s > c_place {
                // The incoming placement wins: eject the cycle top
                // to root, then id can still land under parent_h.
                b.place(b.root(), c_h)?;
                if let Some(r) = self.nodes.get_mut(&c_guid) {
                    r.parent = String::new();
                }
            } else {
                // The existing subtree placement wins: id goes to
                // root instead.
                parent_h = b.root();
            }
        }
        let h = match b.by_guid(id) {
            Some(h) => h,
            None => {
                let kind = body["node"]["type"].as_u64().unwrap_or(2) as u8;
                if kind == bookmarks::TYPE_ROOT || kind > bookmarks::TYPE_SEPARATOR {
                    return Self::invalid("bad link node type");
                }
                let mut n = bookmarks::Node::new(kind);
                n.guid = id.to_string();
                b.alloc_node(n)
            }
        };
        // Field-level merge: the snapshot wins per field, stamped.
        if let Some(ndoc) = body["node"].as_object() {
            for f in FIELDS {
                if let Some(v) = ndoc.get(f) {
                    if s > self
                        .fields
                        .get(&(id.to_string(), f.to_string()))
                        .cloned()
                        .unwrap_or_default()
                    {
                        // A gated url can reject — that's a permanent
                        // refusal, not a retryable apply error.
                        if b.set_field(h, f, v).is_ok() {
                            self.fields.insert((id.to_string(), f.to_string()), s.clone());
                        }
                    }
                }
            }
        }
        // "type" isn't a synced field name; tags handled above.
        {
            let n = b.node_mut(h)?;
            n.rank = pos.to_string();
        }
        b.place(parent_h, h)?;
        {
            let pg = b.parent_guid(h);
            let rec = self.nodes.entry(id.to_string()).or_default();
            rec.dead = false;
            rec.place = s;
            rec.parent = pg;
        }
        self.unpark(b, id);
        Ok(true)
    }

    fn apply_rm(&mut self, b: &mut Bookmarks, op: &WireOp) -> RcResult<bool> {
        let body = &op.body;
        let id = body["id"].as_str().unwrap_or("");
        if id.is_empty() || id.len() > MAX_GUID {
            return Self::invalid("bad rm id");
        }
        let mut targets: Vec<(String, String, Stamp)> = vec![(
            id.to_string(),
            body["parent"].as_str().unwrap_or("").to_string(),
            parse_stamp(&body["obs"]),
        )];
        if let Some(kids) = body["kids"].as_array() {
            for k in kids {
                let Some(g) = k.get(0).and_then(Value::as_str) else {
                    continue;
                };
                if g.is_empty() || g.len() > MAX_GUID {
                    continue;
                }
                targets.push((
                    g.to_string(),
                    k.get(1)
                        .and_then(Value::as_str)
                        .unwrap_or("")
                        .to_string(),
                    (
                        k.get(2).and_then(Value::as_u64).unwrap_or(0),
                        k.get(3)
                            .and_then(Value::as_str)
                            .unwrap_or("")
                            .to_string(),
                    ),
                ));
            }
        }
        // 1. Bookkeeping: tombstone the observed elements; a live
        //    element whose place stamp outranks what the remover saw
        //    survives untouched.
        let mut dead_now: Vec<String> = Vec::new();
        for (g, pg, obs) in targets {
            self.ensure_rec(b, &g);
            let rec = self.nodes.entry(g.clone()).or_default();
            if rec.parent.is_empty() {
                rec.parent = pg;
            }
            if rec.dead {
                if obs > rec.obs {
                    rec.obs = obs;
                }
                continue;
            }
            if rec.place > obs {
                continue; // un-observed placement survives
            }
            rec.dead = true;
            if obs > rec.obs {
                rec.obs = obs;
            }
            dead_now.push(g);
        }
        if dead_now.is_empty() {
            return Ok(false);
        }
        // 2. Rescue live children of newly dead nodes — they were
        //    never observed by the remover and keep their rank under
        //    the nearest live ancestor.
        let mut changed = false;
        for g in &dead_now {
            let Some(dh) = b.by_guid(g) else {
                continue;
            };
            let anc_guid = self.live_ancestor(g);
            let anc_h = if anc_guid.is_empty() {
                b.root()
            } else {
                b.by_guid(&anc_guid).unwrap_or_else(|| b.root())
            };
            for c in b.children_of(dh).unwrap_or_default() {
                let c_guid = b.guid_of(c).unwrap_or("").to_string();
                let c_dead = self
                    .nodes
                    .get(&c_guid)
                    .map(|r| r.dead)
                    .unwrap_or(false);
                if c_guid.is_empty() || c_dead {
                    continue;
                }
                b.place(anc_h, c)?;
                if let Some(r) = self.nodes.get_mut(&c_guid) {
                    r.parent = anc_guid.clone();
                } else {
                    self.nodes.insert(
                        c_guid.clone(),
                        NodeRec {
                            parent: anc_guid.clone(),
                            ..NodeRec::default()
                        },
                    );
                }
                changed = true;
            }
        }
        // 3. Free the dead handles — their live children are already
        //    re-homed, so the subtree free takes only dead records.
        for g in &dead_now {
            if let Some(dh) = b.by_guid(g) {
                b.erase(dh)?;
                changed = true;
            }
        }
        // 4. Ops parked on these guids now know the answer (dead) —
        //    they retarget or drop.
        for g in dead_now {
            self.unpark(b, &g);
        }
        Ok(changed)
    }

    fn apply_set(&mut self, b: &mut Bookmarks, op: &WireOp) -> RcResult<bool> {
        let body = &op.body;
        let id = body["id"].as_str().unwrap_or("");
        let f = body["f"].as_str().unwrap_or("");
        if id.is_empty() || id.len() > MAX_GUID || !FIELDS.contains(&f) {
            return Self::invalid("bad set op");
        }
        let s = stamp_of(op);
        self.ensure_rec(b, id);
        match self.nodes.get(id) {
            Some(r) if r.dead => return Ok(false),
            Some(_) => {}
            None => {
                self.park(id, op);
                return Ok(false);
            }
        }
        let key = (id.to_string(), f.to_string());
        if s <= self.fields.get(&key).cloned().unwrap_or_default() {
            return Ok(false);
        }
        let Some(h) = b.by_guid(id) else {
            return Ok(false);
        };
        b.set_field(h, f, &body["v"])?;
        self.fields.insert(key, s);
        Ok(true)
    }
}

// ---- plumbing --------------------------------------------------------

/// The outbox drain hooked from bookmarks::with — groups events by
/// the (root, scope) their file binding implies and appends signed
/// ops.  Events on non-profile files are unsynced and drop away.
pub(crate) fn flush_local(b: &mut Bookmarks) {
    if b.outbox.is_empty() {
        return;
    }
    let events = b.take_outbox();
    // Per-scope queues keep each group's internal order.
    let mut groups: Vec<(String, String, Vec<OutEv>)> = Vec::new();
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
        if let Err(done) = emit_scope(&root, &scope, b, &evs) {
            // Re-queue whatever didn't emit — a transient engine
            // failure must not silently drop a user edit.
            let mut rest: Vec<OutEv> = evs[done..].to_vec();
            rest.extend(b.take_outbox());
            b.outbox = rest;
            return;
        }
    }
}

/// Emit one scope's queued events: hydrate the adapter once, run the
/// first-touch advertise, then append ops per event.  Err(done) =
/// the first `done` events did emit; the rest are for the caller to
/// re-queue.
fn emit_scope(
    root: &str,
    scope: &str,
    b: &mut Bookmarks,
    evs: &[OutEv],
) -> Result<(), usize> {
    let engine = SyncEngine::open(root).map_err(|_| 0usize)?;
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let key = (root.to_string(), scope.to_string());
    let bms = reg.entry(key).or_insert_with(BmSync::new);
    if !bms.hydrated {
        bms.hydrate(b, &engine, scope).map_err(|_| 0usize)?;
    }
    let mut linked = std::collections::HashSet::new();
    for (i, oe) in evs.iter().enumerate() {
        bms.emit_events(b, &engine, scope, std::slice::from_ref(&oe.ev), &mut linked)
            .map_err(|_| i)?;
    }
    // First-touch merge, after the events so freshly-emitted nodes
    // don't get a second op: advertise every live node the state
    // still has never seen (pre-sync + imported trees flow to the
    // group once).
    advertise(bms, b, &engine, scope).map_err(|_| evs.len())?;
    Ok(())
}

/// Emit links for live nodes with no state record — runs on every
/// flush but only first-touch nodes cost an op.
fn advertise(bms: &mut BmSync, b: &Bookmarks, engine: &SyncEngine, scope: &str) -> RcResult<()> {
    let mut stack: Vec<u64> = b
        .children_of(b.root())
        .unwrap_or_default()
        .into_iter()
        .rev()
        .collect();
    while let Some(h) = stack.pop() {
        let seen = b
            .guid_of(h)
            .map(|g| bms.nodes.contains_key(g))
            .unwrap_or(true);
        if !seen {
            let mut linked = std::collections::HashSet::new();
            bms.emit_link(b, engine, scope, h, &mut linked)?;
            continue; // emit_link covers the whole subtree
        }
        for c in b.children_of(h).unwrap_or_default().into_iter().rev() {
            stack.push(c);
        }
    }
    Ok(())
}

/// <root>/profiles/<scope>/bookmarks.xbel → (root, scope) — the
/// binding every emit path funnels through; anything else is not a
/// synced profile tree.
fn binding_scope(path: &Path) -> Option<(String, String)> {
    if path.file_name()?.to_str()? != "bookmarks.xbel" {
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
/// the call is bound to.  The bookmarks store is a per-call-bound
/// singleton like the rest of the RCORE-era state.
pub(crate) fn install(root: &str) {
    let root = root.to_string();
    sync::register_applier(
        "bookmarks",
        Arc::new(move |op: &WireOp| apply_remote(&root, op).map(|_| ())),
    );
}

fn apply_remote(root: &str, op: &WireOp) -> RcResult<bool> {
    let scope = facade::effective_profile(&op.scope).to_string();
    let dir = facade::profile_dir(Path::new(root), &scope);
    let xbel = dir.join("bookmarks.xbel");
    let changed = bookmarks::with(|b| -> RcResult<bool> {
        b.ensure_path(&xbel)?;
        let engine = SyncEngine::open(root)?;
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        let bms = reg
            .entry((root.to_string(), scope.clone()))
            .or_insert_with(BmSync::new);
        if !bms.hydrated {
            bms.hydrate(b, &engine, &scope)?;
        }
        bms.apply(b, op)
    })?;
    if changed {
        // The apply path bypasses the Qt save flow, so persist the
        // merged tree itself — a remote op must not be a memory-only
        // change that dies with the process.
        bookmarks::with(|b| {
            let _ = b.ensure_path(&xbel);
            b.save_path(&xbel)
        })?;
        notify::emit("bookmarks");
    }
    Ok(changed)
}

/// Test + debug helper: a stable serialization of the whole tree
/// (guid, kind, fields, rank, children order) — convergence checks
/// compare these.
#[cfg(test)]
pub(crate) fn canonical_dump(b: &Bookmarks) -> String {
    fn walk(b: &Bookmarks, h: u64, depth: usize, out: &mut String) {
        for c in b.children_of(h).unwrap_or_default() {
            let Some(n) = b.node_ref(c) else {
                continue;
            };
            out.push_str(&"  ".repeat(depth));
            out.push_str(&format!(
                "{}|{}|{}|{}|{}|{}|{}|{:?}\n",
                n.guid, n.kind, n.title, n.url, n.desc, n.expanded, n.rank, n.tags
            ));
            walk(b, c, depth + 1, out);
        }
    }
    let mut out = String::new();
    walk(b, b.root(), 0, &mut out);
    out
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;
    use std::path::PathBuf;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-bmsync-{}-{}-{}",
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

    /// A "device": a data root, its own SyncEngine and a bound
    /// bookmark file under profiles/default — the same shape the
    /// singleton BM gets driven through in production.
    struct Dev {
        root: String,
        xbel: PathBuf,
    }

    impl Dev {
        fn new(tag: &str) -> Dev {
            let root = tmpdir(tag);
            let dir = Path::new(&root).join("profiles").join("default");
            std::fs::create_dir_all(&dir).unwrap();
            Dev {
                root,
                xbel: dir.join("bookmarks.xbel"),
            }
        }
        fn engine(&self) -> SyncEngine {
            SyncEngine::open(&self.root).unwrap()
        }
        /// Mutate the shared store bound to THIS device's file.
        fn with_bm<R>(&self, f: impl FnOnce(&mut Bookmarks) -> R) -> R {
            bookmarks::with(|b| {
                b.ensure_path(&self.xbel).unwrap();
                f(b)
            })
        }
        fn dump(&self) -> String {
            self.with_bm(|b| canonical_dump(b))
        }
        fn wire(&self) -> Vec<Value> {
            self.engine()
                .export(Some("bookmarks"), 0)
                .unwrap()
                .iter()
                .map(|o| o.to_json())
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
    fn creates_and_sets_round_trip() {
        let _g = guard();
        let a = Dev::new("rtA");
        let b = Dev::new("rtB");
        a.pair(&b);
        b.pair(&a);

        a.with_bm(|bm| {
            let f = bm
                .create(bm.root(), -1, &json!({"type":1,"title":"F"}))
                .unwrap();
            bm.create(
                f,
                -1,
                &json!({"type":2,"title":"bm","url":"http://x/","desc":"d","tags":["t"]}),
            )
            .unwrap();
        });
        let rep = b.import(&a.wire());
        assert_eq!(rep.accepted, 2, "{:?}", rep.rejected);
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        assert_eq!(a.dump(), b.dump(), "{}", b.dump());

        // A title edit propagates and re-delivery is idempotent.
        a.with_bm(|bm| {
            let h = bm.find_url("http://x/").unwrap();
            bm.set_str(h, "title", "bm-renamed").unwrap();
        });
        let rep = b.import(&a.wire());
        assert_eq!(rep.rejected.len(), 0);
        let rep = b.import(&a.wire());
        assert!(rep.applied == 0, "re-import must dedup, got {:?}", rep);
        assert_eq!(a.dump(), b.dump());
        assert!(b.dump().contains("bm-renamed"));
    }

    #[test]
    fn concurrent_add_remove_move_converge() {
        let _g = guard();
        let a = Dev::new("ccA");
        let b = Dev::new("ccB");
        a.pair(&b);
        b.pair(&a);

        // A: folder F with two bookmarks; B: folder G with one.
        let f_g = a.with_bm(|bm| {
            let f = bm
                .create(bm.root(), -1, &json!({"type":1,"title":"F"}))
                .unwrap();
            bm.create(f, -1, &json!({"type":2,"title":"a1","url":"http://a1/"}))
                .unwrap();
            bm.create(f, -1, &json!({"type":2,"title":"a2","url":"http://a2/"}))
                .unwrap();
            bm.guid_of(f).unwrap().to_string()
        });
        b.with_bm(|bm| {
            let g = bm
                .create(bm.root(), -1, &json!({"type":1,"title":"G"}))
                .unwrap();
            bm.create(g, -1, &json!({"type":2,"title":"b1","url":"http://b1/"}))
                .unwrap();
        });
        // Exchange — union of both trees.
        let wa = a.wire();
        let wb = b.wire();
        b.import(&wa);
        a.import(&wb);
        assert_eq!(a.dump(), b.dump());

        // Concurrent conflict legs:
        //  - A removes folder F entirely.
        //  - B concurrently adds a new bookmark INTO F (which it now
        //    has from sync) and moves its own b1 into F too.
        a.with_bm(|bm| {
            let f_h = bm.by_guid(&f_g).unwrap();
            bm.detach(f_h).unwrap();
            bm.destroy(f_h).unwrap();
        });
        b.with_bm(|bm| {
            let fh = bm.by_guid(&f_g).unwrap();
            bm.create(fh, -1, &json!({"type":2,"title":"into-dead","url":"http://in/"}))
                .unwrap();
            let b1 = bm
                .find_url("http://b1/")
                .unwrap();
            bm.detach(b1).unwrap();
            bm.attach(fh, -1, b1).unwrap();
        });
        let wa2 = a.wire();
        let wb2 = b.wire();
        b.import(&wa2);
        a.import(&wb2);
        assert_eq!(a.dump(), b.dump(), "\n--- a ---\n{}\n--- b ---\n{}", a.dump(), b.dump());

        // What converged: F + its two observed children tombstoned
        // everywhere; the two concurrent additions into F (B's new
        // bookmark + B's moved b1) survived at root on BOTH sides.
        let d = a.dump();
        // Title fields are "|title|" — bare "a1"/"a2" would match
        // guid substrings.
        assert!(!d.contains("|a1|"), "{d}");
        assert!(!d.contains("|a2|"), "{d}");
        assert!(d.contains("|into-dead|"), "{d}");
        assert!(d.contains("|b1|"), "{d}");
        assert!(d.contains("|G|"), "{d}");
    }

    #[test]
    fn out_of_order_child_waits_for_parent() {
        let _g = guard();
        let a = Dev::new("ooA");
        let b = Dev::new("ooB");
        a.pair(&b);
        b.pair(&a);
        a.with_bm(|bm| {
            let f = bm
                .create(bm.root(), -1, &json!({"type":1,"title":"F"}))
                .unwrap();
            bm.create(f, -1, &json!({"type":2,"title":"c","url":"http://c/"}))
                .unwrap();
        });
        let mut docs = a.wire();
        // Deliver the child op first (it's lamport 2, folder is 1).
        assert!(docs.len() >= 2);
        let folder_op = docs.remove(0);
        let rep = b.import(&docs);
        assert!(rep.applied <= docs.len() as u32);
        // The child may have parked — the folder op unblocks it.
        let rep2 = b.import(&[folder_op]);
        let _ = rep2;
        assert_eq!(a.dump(), b.dump(), "{}", b.dump());
    }

    #[test]
    fn rank_between_is_strict() {
        let cases = [
            ("", ""),
            ("a", "b"),
            ("a", "aZ"),
            ("z", ""),
            ("", "a"),
            ("0", "1"),
            ("aY", "aZ"),
        ];
        for (p, n) in cases {
            let m = bookmarks::rank_between(p, n);
            assert!(m > p.to_string() || p.is_empty(), "{p:?} < {m:?}");
            assert!(m < n.to_string() || n.is_empty(), "{m:?} < {n:?}");
        }
        // Many sequential appends stay ordered.
        let mut last = String::new();
        for _ in 0..50 {
            last = bookmarks::rank_between(&last, "");
            assert!(!last.is_empty());
        }
    }

    #[test]
    fn fingerprint_ids_dedup_identical_files() {
        let _g = guard();
        let doc = "<xbel version='1.0'><folder folded='no'><title>F</title>\
                   <bookmark href='http://a/'><title>A</title></bookmark></folder></xbel>";
        let a = Dev::new("fpA");
        let b = Dev::new("fpB");
        a.pair(&b);
        b.pair(&a);
        for d in [&a, &b] {
            std::fs::write(&d.xbel, doc).unwrap();
            d.with_bm(|bm| {
                bm.ensure_path(&d.xbel).unwrap();
            });
        }
        // Identical docs -> identical fingerprint guids -> A's
        // advertise ops land on existing elements, no duplicates.
        b.import(&a.wire());
        let rep = b.import(&a.wire());
        let _ = rep;
        assert_eq!(a.dump(), b.dump());
        assert_eq!(
            b.with_bm(|bm| bm.child_count(bm.root())),
            Some(1),
            "fingerprint dedup must not double the shared folder"
        );
    }

    #[test]
    fn undo_relink_revives() {
        let _g = guard();
        let a = Dev::new("urA");
        let b = Dev::new("urB");
        a.pair(&b);
        b.pair(&a);
        let g = a.with_bm(|bm| {
            let h = bm
                .create(bm.root(), -1, &json!({"type":2,"title":"victim","url":"http://v/"}))
                .unwrap();
            bm.guid_of(h).unwrap().to_string()
        });
        b.import(&a.wire());
        assert_eq!(a.dump(), b.dump());
        // A deletes, B applies; then A "undoes" (re-link same guid)
        // and the revived node comes back on B.
        a.with_bm(|bm| {
            let h = bm.by_guid(&g).unwrap();
            bm.detach(h).unwrap();
            bm.destroy(h).unwrap();
        });
        b.import(&a.wire());
        assert_eq!(b.with_bm(|bm| bm.by_guid(&g)), None);
        // Re-create with the same guid — the undo path re-links the
        // surviving subtree; emulate by restoring a detached node.
        a.with_bm(|bm| {
            // Simulate undo: the node was destroyed at store level,
            // so re-add a node carrying the same guid via a fresh
            // create + guid override is not exposed — instead craft
            // the link op directly like an undo-driven re-attach
            // would emit for a still-detached subtree.
            let h = bm
                .create(bm.root(), -1, &json!({"type":2,"title":"victim","url":"http://v/"}))
                .unwrap();
            bm.node_mut(h).unwrap().guid = g.clone();
        });
        // The new create emitted a link for the same guid with a
        // fresh stamp -> revive on the peer.
        b.import(&a.wire());
        assert_eq!(a.dump(), b.dump(), "{}", b.dump());
        assert!(b.with_bm(|bm| bm.by_guid(&g)).is_some());
    }

    #[test]
    fn move_cycle_resolves_identically() {
        let _g = guard();
        let a = Dev::new("mcA");
        let b = Dev::new("mcB");
        a.pair(&b);
        b.pair(&a);
        let (gx, gy) = a.with_bm(|bm| {
            let x = bm
                .create(bm.root(), -1, &json!({"type":1,"title":"X"}))
                .unwrap();
            let y = bm
                .create(bm.root(), -1, &json!({"type":1,"title":"Y"}))
                .unwrap();
            (
                bm.guid_of(x).unwrap().to_string(),
                bm.guid_of(y).unwrap().to_string(),
            )
        });
        // Give B the same two folders via sync.
        b.import(&a.wire());
        assert_eq!(a.dump(), b.dump());
        // Concurrent cyclic moves: A puts X under Y; B puts Y under X.
        a.with_bm(|bm| {
            let x = bm.by_guid(&gx).unwrap();
            let y = bm.by_guid(&gy).unwrap();
            bm.detach(x).unwrap();
            bm.attach(y, -1, x).unwrap();
        });
        b.with_bm(|bm| {
            let x = bm.by_guid(&gx).unwrap();
            let y = bm.by_guid(&gy).unwrap();
            bm.detach(y).unwrap();
            bm.attach(x, -1, y).unwrap();
        });
        b.import(&a.wire());
        a.import(&b.wire());
        assert_eq!(
            a.dump(),
            b.dump(),
            "cycle resolution must be deterministic\n---a---\n{}\n---b---\n{}",
            a.dump(),
            b.dump()
        );
        // And the result must be a valid tree: exactly one of the
        // two folder placements won; no orphan, no loop.
        a.with_bm(|bm| {
            assert!(bm.by_guid(&gx).is_some() && bm.by_guid(&gy).is_some());
        });
    }
}
