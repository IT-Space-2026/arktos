// SYNC11: the scheduled "sync round" — the mw `sync_now` op's core.
// The desktop keeps no always-on sync session (battery): each round
// starts the `auto` transport when none is running, exchanges journal
// ops with every member it can dial (their recorded onion — the auto
// leg resolves the LAN fast path when co-located), serves inbound
// sync frames while up, then stops the transport again iff this round
// started it.  A peer is therefore reachable whenever EITHER side's
// round window overlaps or a pair dialog is open — eventual exchange
// without a 24/7 Arti/mDNS footprint.
//
// Wire doc on a link (one frame each way):
//   {"t":"sync_ops","scope":"<profile>","ops":[WireOp json…]}
// The receiver imports the ops and replies with its own export on
// the same link.  Anything that isn't a sync_ops doc is ignored —
// pair-session frames belong to their own protocol.

use std::path::Path;
use std::thread;
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use std::sync::Arc;

use crate::error::{RcResult, RcStatus};
use crate::sync::SyncEngine;
use crate::synctransport::{self, SyncTransport, TransportEvent};

/// Per-member dial+exchange cap — a tor onion dial is the slow half.
const MEMBER_BUDGET: Duration = Duration::from_secs(20);
/// The short inbound-serve sweep at round start.
const SERVE_BUDGET: Duration = Duration::from_millis(1500);
const POLL_STEP: Duration = Duration::from_millis(80);

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
    if doc.get("t").and_then(Value::as_str) != Some("sync_ops") {
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
    let link = t.connect(onion, nickname)?;
    let our = ops_frame(engine, scope)?;
    t.send(link, &our)?;
    let deadline = Instant::now() + MEMBER_BUDGET;
    let mut applied = 0u32;
    while Instant::now() < deadline {
        for ev in t.poll() {
            match &ev {
                TransportEvent::Frame { link: l, payload } if *l == link => {
                    if let Ok(doc) = serde_json::from_slice::<Value>(payload) {
                        if doc.get("t").and_then(Value::as_str) == Some("sync_ops") {
                            if let Some(arr) =
                                doc.get("ops").and_then(Value::as_array)
                            {
                                applied = engine
                                    .import(arr)
                                    .map(|r| r.applied)
                                    .unwrap_or(0);
                            }
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
/// serve inbound sync frames seen on the way.  The transport is
/// stopped afterwards only when this round started it.
pub fn run_once(data_root: &Path, scope: &str) -> RcResult<Value> {
    // A live pair session owns the transport's event queue — a round
    // now would drain handshake frames it must not see.  Skip and
    // report honestly rather than racing the dialog.
    if crate::syncpair::live_pairing() {
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
        return Ok(json!({"ok": true, "synced": 0, "members": 0}));
    }

    let started_here = synctransport::running(data_root).is_none();
    if started_here {
        synctransport::start(data_root, "auto", "")?;
    }
    let t = synctransport::running(data_root)
        .ok_or_else(|| fail("transport did not come up"))?;

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
    for m in &dial {
        match exchange_with(
            &engine, &t, scope, &m.onion_addr, &m.device_id, &mut served,
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

    if started_here && !crate::syncpair::live_pairing() {
        let _ = synctransport::stop(data_root);
    }
    Ok(json!({
        "ok": true,
        "synced": synced,
        "members": dial.len(),
        "served": served,
        "errors": errors,
    }))
}
