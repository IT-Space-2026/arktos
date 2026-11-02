// SYNC11: the scheduled "sync round" — the mw `sync_now` op's core —
// plus the always-on inbound half that makes rounds able to land.
//
// Serverless sync has no relay: a peer can only reach this device
// while OUR onion service is published and someone drains the
// transport event queue.  The first cut stopped the transport at the
// end of every round, which made reachability a coincidence — two
// independently scheduled ~30s windows almost never overlap, so
// every dial burned the whole connect deadline ("connect deadline:
// <onion>") and inbound frames that did land went unanswered until
// the receiver's own round ("exchange timed out").  The model is now
// "listen while the app runs": once a round or a pair dialog brings
// the `auto` transport up it stays up, and a per-data-root serve
// thread drains the queue between rounds — importing inbound
// sync_ops frames and answering each with our export — so a peer's
// dial gets a reply whenever this process is alive.
//
// Wire doc on a link (one frame each way):
//   {"t":"sync_ops","scope":"<profile>","ops":[WireOp json…]}
// The receiver imports the ops and replies with its own export on
// the same link.  Anything that isn't a sync_ops doc is ignored —
// pair-session frames belong to their own protocol (the serve thread
// never polls while a pair session is live; pair_poll owns the queue
// then).

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, LazyLock, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use serde_json::{json, Value};

use crate::error::{RcResult, RcStatus};
use crate::sync::SyncEngine;
use crate::synctransport::{self, SyncTransport, TransportEvent};

/// Bound on one member dial — `connect()` itself retries for up to
/// CONNECT_DEADLINE inside the transport, so it runs on a worker
/// thread the round can abandon: a powered-off peer costs this, not
/// two minutes.  A warm onion connect lands in seconds; 30s covers a
/// cold descriptor fetch without dragging the round out.
const DIAL_BUDGET: Duration = Duration::from_secs(30);
/// Per-member cap on the post-connect reply wait.
const MEMBER_BUDGET: Duration = Duration::from_secs(20);
/// The short inbound-serve sweep at round start and end.
const SERVE_BUDGET: Duration = Duration::from_millis(1500);
const POLL_STEP: Duration = Duration::from_millis(80);
/// The background serve loop's cadence between drains — replies must
/// land well inside the dialer's reply budget.
const SERVE_STEP: Duration = Duration::from_millis(200);

fn fail(msg: impl Into<String>) -> crate::error::Fail {
    crate::error::Fail {
        status: RcStatus::Io,
        msg: msg.into(),
    }
}

fn ops_frame(engine: &SyncEngine, scope: &str) -> RcResult<Vec<u8>> {
    let ops: Vec<Value> = engine
        .export(None, 0)?
        .iter()
        .map(|o| o.to_json())
        .collect();
    Ok(json!({"t": "sync_ops", "scope": scope, "ops": ops})
        .to_string()
        .into_bytes())
}

// ---- inbound serve thread ----------------------------------------------------
//
// One long-lived thread per data root owns queue service between
// rounds: while a transport is registered it drains poll() on a
// cadence and runs each frame through serve_event.  Two exclusions
// keep queue ownership unambiguous:
//
//   * `drain` — a mutex a round holds for its entire poll window, so
//     a reply frame for an in-flight exchange can never be stolen by
//     the background loop mid-round (a stolen reply still imports —
//     but the round would then report a phantom timeout).
//   * live_pairing_for — pair_poll owns the queue while a ceremony
//     is open; draining there would eat Noise handshake frames.
//
// The thread survives transport restarts transparently (running()
// simply returns the new instance) and exits when the transport is
// gone for good — the serve entry is then reusable.

struct ServeControl {
    /// True while a serve thread is alive — ensure_serving claims the
    /// false->true edge, so exactly one thread owns a root.
    active: AtomicBool,
    /// Held by run_once across its drain window, and taken briefly by
    /// pair_poll — one queue owner at a time.  Leaked per root so the
    /// guard can outlive the registry borrow.
    drain: &'static Mutex<()>,
}

static SERVERS: LazyLock<Mutex<HashMap<PathBuf, Arc<ServeControl>>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

fn serve_ctl(root: &Path) -> Arc<ServeControl> {
    SERVERS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .entry(root.to_path_buf())
        .or_insert_with(|| {
            Arc::new(ServeControl {
                active: AtomicBool::new(false),
                drain: Box::leak(Box::new(Mutex::new(()))),
            })
        })
        .clone()
}

fn serve_loop(root: PathBuf, scope: String, ctl: Arc<ServeControl>) {
    struct DoneGuard(Arc<ServeControl>);
    impl Drop for DoneGuard {
        fn drop(&mut self) {
            self.0.active.store(false, Ordering::SeqCst);
        }
    }
    let _done = DoneGuard(ctl.clone());
    let engine = match SyncEngine::open(&root.to_string_lossy()) {
        Ok(e) => e,
        Err(_) => return,
    };
    loop {
        let Some(t) = synctransport::running(&root) else {
            return;
        };
        {
            let _g = ctl.drain.lock().unwrap_or_else(|e| e.into_inner());
            if !crate::syncpair::live_pairing_for(&root) {
                let mut served = 0u32;
                for ev in t.poll() {
                    serve_event(&engine, &t, &ev, &scope, &mut served);
                }
            }
        }
        thread::sleep(SERVE_STEP);
    }
}

/// Start the inbound serve thread for `data_root` if none is alive.
/// Returns the control block — callers (a round) may hold `drain` to
/// own the queue exclusively for a stretch.
fn ensure_serving(data_root: &Path, scope: &str) -> Arc<ServeControl> {
    let ctl = serve_ctl(data_root);
    if !ctl.active.swap(true, Ordering::SeqCst) {
        let thread_ctl = ctl.clone();
        let root = data_root.to_path_buf();
        let scope = scope.to_string();
        thread::spawn(move || serve_loop(root, scope, thread_ctl));
    }
    ctl
}

/// The queue-ownership guard pair_poll takes before draining: Some
/// while no round holds the window, None while one does — a skipped
/// poll tick loses nothing (the events stay queued) and keeps a
/// mid-round ceremony's Noise frames out of the round's drains.
pub(crate) fn try_drain(
    data_root: &Path,
) -> Option<std::sync::MutexGuard<'static, ()>> {
    serve_ctl(data_root).drain.try_lock().ok()
}

/// One inbound event: accepted links count as serving contacts;
/// a sync_ops frame imports + answers on the same link.
fn serve_event(
    engine: &SyncEngine,
    t: &Arc<dyn SyncTransport>,
    ev: &TransportEvent,
    scope: &str,
    served: &mut u32,
) {
    let TransportEvent::Frame { link, payload } = ev else {
        return;
    };
    let Ok(doc) = serde_json::from_slice::<Value>(payload) else {
        return;
    };
    // Typed {"t":"sync_ops",...} is canonical; the untyped
    // {"ops":[...]} form (older mobile builds) imports the same.
    if doc.get("ops").is_none() {
        return;
    }
    let ops = doc.get("ops").cloned().unwrap_or(Value::Array(vec![]));
    if let Some(arr) = ops.as_array() {
        let _ = engine.import(arr);
    }
    if let Ok(reply) = ops_frame(engine, scope) {
        let _ = t.send(*link, &reply);
    }
    *served += 1;
}

/// Drain events, serving sync frames; returns once `until` passes.
fn serve_window(
    engine: &SyncEngine,
    t: &Arc<dyn SyncTransport>,
    scope: &str,
    until: Instant,
    served: &mut u32,
) {
    while Instant::now() < until {
        let events = t.poll();
        for ev in &events {
            serve_event(engine, t, ev, scope, served);
        }
        thread::sleep(POLL_STEP);
    }
}

/// `t.connect` retries internally for the full CONNECT_DEADLINE —
/// far beyond what a round can afford per member.  Run it on a
/// throwaway thread and wait `budget`: a dead peer surfaces at the
/// budget instead of two minutes in.  A connect that lands after the
/// caller gave up is closed by the worker itself, so no half-owned
/// link lingers.
fn connect_bounded(
    t: &Arc<dyn SyncTransport>,
    endpoint: &str,
    nickname: &str,
    budget: Duration,
) -> RcResult<u64> {
    let (tx, rx) = std::sync::mpsc::channel();
    let worker_t = Arc::clone(t);
    let ep = endpoint.to_string();
    let nick = nickname.to_string();
    thread::spawn(move || match worker_t.connect(&ep, &nick) {
        Ok(link) => {
            if tx.send(Ok(link)).is_err() {
                let _ = worker_t.close_link(link);
            }
        }
        Err(e) => {
            let _ = tx.send(Err(e));
        }
    });
    rx.recv_timeout(budget)
        .unwrap_or_else(|_| Err(fail(format!("dial timed out: {endpoint}"))))
}

/// Dial one member and pull its ops: send ours first, then wait for
/// the reply frame on this link (deadline-bounded).
fn exchange_with(
    engine: &SyncEngine,
    t: &Arc<dyn SyncTransport>,
    scope: &str,
    onion: &str,
    nickname: &str,
    served: &mut u32,
) -> RcResult<u32> {
    let link = connect_bounded(t, onion, nickname, DIAL_BUDGET)?;
    let our = ops_frame(engine, scope)?;
    t.send(link, &our)?;
    let deadline = Instant::now() + MEMBER_BUDGET;
    let mut applied = 0u32;
    while Instant::now() < deadline {
        for ev in t.poll() {
            match &ev {
                TransportEvent::Frame { link: l, payload } if *l == link => {
                    if let Ok(doc) = serde_json::from_slice::<Value>(payload) {
                        if let Some(arr) =
                            doc.get("ops").and_then(Value::as_array)
                        {
                            applied = engine
                                .import(arr)
                                .map(|r| r.applied)
                                .unwrap_or(0);
                        }
                    }
                    let _ = t.close_link(link);
                    return Ok(applied);
                }
                TransportEvent::LinkClosed { link: l, .. } if *l == link => {
                    let _ = t.close_link(link);
                    return Err(fail(format!("peer closed link: {onion}")));
                }
                other => serve_event(engine, t, other, scope, served),
            }
        }
        thread::sleep(POLL_STEP);
    }
    let _ = t.close_link(link);
    Err(fail(format!("exchange timed out: {onion}")))
}

/// One round: dial every member of `scope`, exchange ops both ways,
/// serve inbound sync frames seen on the way.  The transport is NOT
/// stopped afterwards — a peer can only ever reach us while our
/// listener is published, so once a round (or a pair dialog) brought
/// it up it stays up for the life of the process and the serve
/// thread answers inbound sync frames between rounds.
pub fn run_once(data_root: &Path, scope: &str) -> RcResult<Value> {
    // A live pair session owns the transport's event queue — a round
    // now would drain handshake frames it must not see.  Skip and
    // report honestly rather than racing the dialog.
    if crate::syncpair::live_pairing_for(data_root) {
        return Ok(json!({
            "ok": true,
            "skipped": "pairing in progress",
            "synced": 0,
        }));
    }
    let engine = SyncEngine::open(&data_root.to_string_lossy())?;
    let members = engine.members_for_scope(scope)?;
    let self_id = engine.device_id();
    let dial: Vec<_> = members
        .iter()
        .filter(|m| m.device_id != self_id && !m.onion_addr.is_empty())
        .collect();
    if dial.is_empty() {
        // No members on this scope — but when a transport is already
        // up (a pair dialog or another scope's round started it)
        // inbound service still has to run or peers' frames sit
        // unanswered.  Never START a transport for a memberless
        // round, though: unpaired devices keep their zero footprint.
        if synctransport::running(data_root).is_some() {
            let _ = ensure_serving(data_root, scope);
        }
        return Ok(json!({"ok": true, "synced": 0, "members": 0}));
    }

    if synctransport::running(data_root).is_none() {
        synctransport::start(data_root, "auto", "")?;
    }
    let t = synctransport::running(data_root)
        .ok_or_else(|| fail("transport did not come up"))?;

    // Inbound service runs continuously between rounds; the round
    // takes the drain lock so its exchange replies can't be stolen.
    let ctl = ensure_serving(data_root, scope);
    let _drain = ctl.drain.lock().unwrap_or_else(|e| e.into_inner());

    let mut served = 0u32;
    // Brief sweep so frames that queued while nobody polled get
    // answered before we go dialing.
    serve_window(
        &engine,
        &t,
        scope,
        Instant::now() + SERVE_BUDGET,
        &mut served,
    );

    let mut synced = 0u32;
    let mut errors: Vec<String> = Vec::new();
    // The nickname the peer authorized for us at pairing time —
    // member-<our id16> — is what name-gated backends check.  Tor
    // ignores it (restricted discovery keys the client auth); the
    // LAN leg's own member-cert pin supersedes it.
    let self_nick =
        format!("member-{}", &self_id[..16.min(self_id.len())]);
    for m in &dial {
        match exchange_with(
            &engine, &t, scope, &m.onion_addr, &self_nick, &mut served,
        ) {
            Ok(_) => {
                synced += 1;
                let _ = engine.member_seen(&m.device_id, None);
            }
            Err(e) => errors.push(format!("{}: {}", m.device_id, e.msg)),
        }
    }
    // Answer anything that arrived while we were out dialing.
    serve_window(
        &engine,
        &t,
        scope,
        Instant::now() + SERVE_BUDGET,
        &mut served,
    );

    Ok(json!({
        "ok": true,
        "synced": synced,
        "members": dial.len(),
        "served": served,
        "errors": errors,
    }))
}

// ---- tests -------------------------------------------------------------------
//
// Loopback transports prove the new contract without real Tor: the
// serve thread answers dials with no round on the receiver, and a
// round leaves its transport published instead of tearing it down.

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sync::SyncEngine;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-syncnow-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    /// B runs no round at all — only the serve thread spawned by
    /// ensure_serving — and still answers A's dial+exchange.  A's
    /// op must land in B's log, and A's round must leave its own
    /// transport running (persistent listener).
    #[test]
    fn serve_thread_answers_dial_and_round_keeps_transport() {
        let (ra, rb) = (tmpdir("a"), tmpdir("b"));
        let scope = "default";
        let ea = SyncEngine::open(ra.to_str().unwrap()).unwrap();
        let eb = SyncEngine::open(rb.to_str().unwrap()).unwrap();
        ea.member_add(&eb.public_key(), "mem:syncnow-b", scope)
            .unwrap();
        eb.member_add(&ea.public_key(), "mem:syncnow-a", scope)
            .unwrap();
        ea.append("bookmarks", scope, &json!({"v": "x"})).unwrap();
        let ta = synctransport::start(&ra, "mem", "syncnow-a").unwrap();
        let tb = synctransport::start(&rb, "mem", "syncnow-b").unwrap();
        // Pairing grants the peer's member-<id16> nick — the mem gate
        // enforces the same contract the tor layer would.
        let a_nick = format!(
            "member-{}",
            &ea.device_id()[..16]
        );
        tb.authorize(&a_nick, "").unwrap();
        ta.authorize(
            &format!("member-{}", &eb.device_id()[..16]),
            "",
        )
        .unwrap();
        let _ctl = ensure_serving(&rb, scope);

        let r = run_once(&ra, scope).unwrap();
        assert_eq!(r["synced"].as_u64(), Some(1), "round: {r}");
        assert!(synctransport::running(&ra).is_some());

        // The exchange is real: A's op reached B's log via the reply.
        for _ in 0..50 {
            if eb.export(None, 0).unwrap().iter().any(|o| o.store == "bookmarks") {
                break;
            }
            thread::sleep(Duration::from_millis(40));
        }
        assert!(
            eb.export(None, 0).unwrap().iter().any(|o| o.store == "bookmarks"),
            "B never imported A's op"
        );

        synctransport::stop(&ra);
        synctransport::stop(&rb);
    }

    /// A dead peer (member recorded, no loop registered under that
    /// name) must fail inside DIAL_BUDGET, not the transport's full
    /// connect deadline.
    #[test]
    fn dead_peer_fails_inside_dial_budget() {
        let ra = tmpdir("dead");
        let scope = "default";
        let ea = SyncEngine::open(ra.to_str().unwrap()).unwrap();
        ea.member_add(
            &[7u8; 32],
            "mem:syncnow-ghost",
            scope,
        )
        .unwrap();
        synctransport::start(&ra, "mem", "syncnow-dead").unwrap();
        let started = Instant::now();
        let r = run_once(&ra, scope).unwrap();
        let errs = r["errors"].as_array().unwrap();
        assert_eq!(errs.len(), 1, "expected one member error: {r}");
        assert!(
            started.elapsed() < DIAL_BUDGET + Duration::from_secs(5),
            "dial overran its budget"
        );
        synctransport::stop(&ra);
    }
}
