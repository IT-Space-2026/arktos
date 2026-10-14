//! DEVT03 — engine-neutral devtools client ("BiDi front, CDP back").
//!
//! The panel (and any future engine adapter) speaks a WebDriver-BiDi-
//! shaped command/event protocol to this module.  Qt's bundled
//! Chromium does NOT ship the BiDi mapper (session.new on the browser
//! websocket answers -32601 — verified empirically on 6.12.0), so the
//! WebEngine backend here translates the BiDi surface onto CDP over the
//! remote-debugging websocket.  When a Servo backend lands the same
//! rc_bidi_* API can point at a native BiDi socket instead — callers
//! never see CDP.
//!
//! Implemented surface (v1):
//!   session.new / session.end
//!   session.subscribe {events:[browsingContext,log,network]}
//!   browsingContext.getTree / .navigate / .close (events:
//!     contextCreated, contextDestroyed, navigationStarted)
//!   script.evaluate {expression, target:{context}, awaitPromise}
//!   storage.getCookies
//!   network.setCacheBehavior {cacheBehavior:"default"|"bypass",
//!     contexts?:[...]} — no contexts = global toggle remembered for
//!     late attaches (maps to CDP Network.setCacheDisabled)
//!   Events out: browsingContext.*, log.entryAdded (console+log),
//!     network.beforeRequestSent / responseStarted / responseCompleted
//!     / fetchError / requestServedFromCache — the network events carry
//!     the CDP extras the panel's Chrome-style view needs: timestamp
//!     (ms), request.type, params.initiator, params.redirectResponse,
//!     response.mimeType / statusText / encodedDataLength (DEVT06),
//!     and — DEVT07 — response.fromDiskCache / fromServiceWorker /
//!     fromPrefetchCache + response.timing (raw, the waterfall reads
//!     receiveHeadersEnd), responseCompleted.decodedDataLength
//!     (uncompressed body bytes summed from Network.dataReceived chunks
//!     bridge-side — the per-chunk events never reach the panel), plus
//!     browsingContext.domContentLoaded / .load translated from
//!     Page.domContentEventFired / loadEventFired so the summary footer
//!     can show Chrome's DOMContentLoaded/Load milestone times
//!     (Page.enable rides the browsingContext subscription).
//!
//! Transport: RFC 6455 client over a blocking TcpStream — Chromium's
//! devtools endpoint is loopback-only and gated by a per-session
//! Origin token (the panel supplies it); frames are masked per spec.
//! The wire framing lives in `ws` below; the CDP<->BiDi mapping lives
//! in `Core`.

use std::collections::{HashMap, HashSet};
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{channel, Sender};
use std::sync::{Arc, Mutex};
use std::thread;

use serde_json::{json, Value};

use crate::error::{Fail, RcStatus};

/* ------------------------------------------------------------------
 * ws — minimal RFC 6455 client (text frames only on the wire).
 * ---------------------------------------------------------------- */

mod ws {
    use super::*;

    fn b64(data: &[u8]) -> String {
        const T: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
        for chunk in data.chunks(3) {
            let n = ((chunk[0] as u32) << 16)
                | ((chunk.get(1).copied().unwrap_or(0) as u32) << 8)
                | (chunk.get(2).copied().unwrap_or(0) as u32);
            out.push(T[(n >> 18) as usize & 63] as char);
            out.push(T[(n >> 12) as usize & 63] as char);
            out.push(if chunk.len() > 1 { T[(n >> 6) as usize & 63] as char } else { '=' });
            out.push(if chunk.len() > 2 { T[n as usize & 63] as char } else { '=' });
        }
        out
    }

    /// Ask the loopback endpoint for the browser target's ws path.
    /// /json/version is readable without the Origin token (Chromium
    /// treats header-less GETs as CLI tooling) — the token only gates
    /// the ws upgrade below.
    ///
    /// This MUST run on its own short-lived socket: the devtools HTTP
    /// server serializes connections, so holding the upgrade socket
    /// open while discovering deadlocks the GET.  It also keep-alives
    /// the response connection, so we stop at Content-Length rather
    /// than reading to EOF.
    pub fn discover_path(host: &str, port: u16) -> Result<String, Fail> {
        let mut stream = TcpStream::connect((host, port))?;
        stream.set_nodelay(true).ok();
        let req = format!(
            "GET /json/version HTTP/1.1\r\nHost: {host}:{port}\r\nConnection: close\r\n\r\n"
        );
        stream.write_all(req.as_bytes())?;
        stream
            .set_read_timeout(Some(std::time::Duration::from_secs(5)))
            .ok();
        let mut body = Vec::new();
        let mut tmp = [0u8; 8192];
        let wanted = |buf: &[u8]| -> Option<usize> {
            // Full response size once headers + Content-Length are in.
            let hdr_end = buf
                .windows(4)
                .position(|w| w == b"\r\n\r\n")
                .map(|i| i + 4)?;
            let head = String::from_utf8_lossy(&buf[..hdr_end]);
            let clen = head
                .lines()
                .find_map(|l| {
                    let (k, v) = l.split_once(':')?;
                    k.trim().eq_ignore_ascii_case("content-length")
                        .then(|| v.trim().parse::<usize>().ok())
                        .flatten()
                })
                .unwrap_or(0);
            Some(hdr_end + clen)
        };
        loop {
            if let Some(need) = wanted(&body) {
                if body.len() >= need {
                    break;
                }
            }
            match stream.read(&mut tmp) {
                Ok(0) => break,
                Ok(n) => body.extend_from_slice(&tmp[..n]),
                Err(e) if e.kind() == std::io::ErrorKind::WouldBlock
                    || e.kind() == std::io::ErrorKind::TimedOut => break,
                Err(e) => return Err(e.into()),
            }
        }
        let hdr_end = body
            .windows(4)
            .position(|w| w == b"\r\n\r\n")
            .map(|i| i + 4)
            .ok_or_else(|| Fail {
                status: RcStatus::Io,
                msg: "devtools /json/version empty".into(),
            })?;
        let text = String::from_utf8_lossy(&body[hdr_end..]);
        let v: Value = serde_json::from_str(text.trim()).map_err(|e| Fail {
            status: RcStatus::Corrupt,
            msg: format!("devtools /json/version: {e}"),
        })?;
        let url = v
            .get("webSocketDebuggerUrl")
            .and_then(|u| u.as_str())
            .ok_or_else(|| Fail {
                status: RcStatus::Io,
                msg: "no webSocketDebuggerUrl".into(),
            })?;
        // ws://host:port/devtools/browser/<guid> -> path part
        let path = url
            .find("://")
            .and_then(|i| url[i + 3..].find('/').map(|j| url[i + 3 + j..].to_string()))
            .unwrap_or_else(|| "/devtools/browser".to_string());
        Ok(path)
    }

    /// Perform the HTTP upgrade; on Ok the stream is a live websocket.
    /// The caller must pass a discovered `path` — see discover_path
    /// for why a second connection must not be held during it.
    pub fn handshake(
        stream: &mut TcpStream,
        host: &str,
        port: u16,
        path: &str,
        origin: &str,
    ) -> Result<(), Fail> {
        let mut key_raw = [0u8; 16];
        getrandom::getrandom(&mut key_raw).map_err(|e| Fail {
            status: RcStatus::Unavailable,
            msg: format!("rng: {e}"),
        })?;
        let key = b64(&key_raw);
        let req = format!(
            "GET {path} HTTP/1.1\r\nHost: {host}:{port}\r\nUpgrade: websocket\r\n\
             Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n\
             Sec-WebSocket-Version: 13\r\nOrigin: {origin}\r\n\r\n"
        );
        stream.write_all(req.as_bytes())?;
        // Read until end of header block.
        let mut buf = Vec::with_capacity(1024);
        let mut byte = [0u8; 1];
        let deadline = std::time::Instant::now() + std::time::Duration::from_secs(10);
        while !buf.ends_with(b"\r\n\r\n") {
            if std::time::Instant::now() > deadline || buf.len() > 16384 {
                return Err(Fail {
                    status: RcStatus::Io,
                    msg: "websocket handshake timed out".into(),
                });
            }
            stream
                .set_read_timeout(Some(std::time::Duration::from_secs(5)))
                .ok();
            match stream.read(&mut byte) {
                Ok(1) => buf.push(byte[0]),
                _ => {
                    return Err(Fail {
                        status: RcStatus::Io,
                        msg: "websocket handshake EOF".into(),
                    })
                }
            }
        }
        stream.set_read_timeout(None).ok();
        let head = String::from_utf8_lossy(&buf);
        if !head.starts_with("HTTP/1.1 101") {
            return Err(Fail {
                status: RcStatus::Io,
                msg: format!(
                    "websocket upgrade rejected: {}",
                    head.lines().next().unwrap_or("?")
                ),
            });
        }
        Ok(())
    }

    /// Write one masked text frame.
    pub fn send_text(stream: &mut TcpStream, payload: &[u8]) -> Result<(), Fail> {
        let mut frame = Vec::with_capacity(payload.len() + 14);
        frame.push(0x81u8); // FIN | text
        let n = payload.len() as u64;
        if n < 126 {
            frame.push(0x80 | n as u8);
        } else if n < 65536 {
            frame.push(0x80 | 126);
            frame.extend_from_slice(&(n as u16).to_be_bytes());
        } else {
            frame.push(0x80 | 127);
            frame.extend_from_slice(&n.to_be_bytes());
        }
        let mut mask = [0u8; 4];
        getrandom::getrandom(&mut mask).unwrap_or_default();
        frame.extend_from_slice(&mask);
        for (i, b) in payload.iter().enumerate() {
            frame.push(b ^ mask[i % 4]);
        }
        stream.write_all(&frame)?;
        Ok(())
    }

    fn send_pong(stream: &mut TcpStream, payload: &[u8]) -> Result<(), Fail> {
        let mut frame = Vec::with_capacity(payload.len() + 6);
        frame.push(0x8Au8);
        frame.push(0x80 | payload.len() as u8);
        let mut mask = [0u8; 4];
        getrandom::getrandom(&mut mask).unwrap_or_default();
        frame.extend_from_slice(&mask);
        for (i, b) in payload.iter().enumerate() {
            frame.push(b ^ mask[i % 4]);
        }
        stream.write_all(&frame)?;
        Ok(())
    }

    pub enum Incoming {
        Text(String),
        Closed,
    }

    /// Blocking read of the next complete text message.  `data` is a
    /// persistent buffer — bytes past the current frame (coalesced
    /// frames) must survive between calls.  Ping is answered inline;
    /// close/EOF/error -> Closed.
    pub fn recv_text(
        reader: &mut TcpStream,
        writer: &Mutex<TcpStream>,
        data: &mut Vec<u8>,
    ) -> Incoming {
        let mut text: Vec<u8> = Vec::new();
        fn fill(reader: &mut TcpStream, data: &mut Vec<u8>, want: usize) -> bool {
            while data.len() < want {
                let mut tmp = [0u8; 16384];
                match reader.read(&mut tmp) {
                    Ok(0) | Err(_) => return false,
                    Ok(n) => data.extend_from_slice(&tmp[..n]),
                }
            }
            true
        }
        loop {
            if !fill(reader, data, 2) {
                return Incoming::Closed;
            }
            let opcode = data[0] & 0x0f;
            let mut len = (data[1] & 0x7f) as u64;
            let mut hdr = 2usize;
            if len == 126 {
                if !fill(reader, data, 4) {
                    return Incoming::Closed;
                }
                len = u16::from_be_bytes([data[2], data[3]]) as u64;
                hdr = 4;
            } else if len == 127 {
                if !fill(reader, data, 10) {
                    return Incoming::Closed;
                }
                len = u64::from_be_bytes(data[2..10].try_into().unwrap());
                hdr = 10;
            }
            let need = hdr + len as usize;
            if !fill(reader, data, need) {
                return Incoming::Closed;
            }
            let payload: Vec<u8> = data[hdr..need].to_vec();
            data.drain(0..need);
            match opcode {
                1 | 2 => {
                    text.extend_from_slice(&payload);
                    // treat each frame as a full message (devtools sends
                    // FIN-set singles); continuation frames land in op 0.
                    return match String::from_utf8(text) {
                        Ok(s) => Incoming::Text(s),
                        Err(_) => Incoming::Closed,
                    };
                }
                0 => {
                    text.extend_from_slice(&payload);
                    if data.first().map(|b| b & 0x80).unwrap_or(0) != 0
                        || true // devtools never sends fragmented JSON; treat op0 as message end
                    {
                        return match String::from_utf8(std::mem::take(&mut text)) {
                            Ok(s) => Incoming::Text(s),
                            Err(_) => Incoming::Closed,
                        };
                    }
                }
                9 => {
                    let mut w = match writer.lock() {
                        Ok(w) => w,
                        Err(e) => e.into_inner(),
                    };
                    let _ = send_pong(&mut w, &payload);
                }
                8 => return Incoming::Closed,
                _ => {}
            }
        }
    }
}

/* ------------------------------------------------------------------
 * Core — shared connection state.
 * ---------------------------------------------------------------- */

type EventSink = Box<dyn Fn(&str, u64, &str) + Send>;

struct PendingCdp {
    /// What to do once the CDP response arrives.
    action: CdpAction,
}

enum CdpAction {
    /// attachToTarget for a context we need a session on; queued
    /// follow-ups live in State::attaching and run on completion.
    Attach { context: String },
    /// A domain-enable step that precedes the same follow-up.
    Step { follow: FollowUp },
    /// Terminal step — translate the CDP result and answer `bidi_id`.
    Answer { bidi_id: u64, kind: AnswerKind },
}

#[derive(Clone, Copy)]
enum AnswerKind {
    Raw,
    Eval,
    Tree,
    Cookies,
}

enum FollowUp {
    /// Once a session exists for context, issue this BiDi command.
    Command {
        bidi_id: u64,
        method: String,
        params: Value,
    },
    /// Subscribe-domains enablement: no BiDi answer needed.
    Quiet,
}

pub struct Core {
    writer: Mutex<TcpStream>,
    next_bidi_id: Mutex<u64>,
    next_cdp_id: Mutex<u64>,
    sink: Mutex<Option<EventSink>>,
    state: Mutex<State>,
    alive: Arc<AtomicBool>,
}

#[derive(Default)]
struct State {
    cdp_pending: HashMap<u64, PendingCdp>,
    subscriptions: HashSet<String>,
    /// BiDi context id == CDP targetId.
    sessions: HashMap<String, String>, // context -> CDP sessionId
    contexts: HashMap<String, (String, String)>, // context -> (url, title)
    /// Deferred follow-ups while a target attach is in flight.
    attaching: HashMap<String, Vec<FollowUp>>,
    /// Global "Disable cache" — re-applied to every session on attach.
    cache_disabled: bool,
    /// requestId -> decoded body bytes seen so far (DEVT07).  The sum
    /// rides loadingFinished as decodedDataLength; per-chunk
    /// Network.dataReceived events never reach the panel.
    decoded_bytes: HashMap<String, i64>,
}

impl Core {
    fn emit(&self, kind: &str, id: u64, payload: &str) {
        let cb = self.sink.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(f) = cb.as_ref() {
            f(kind, id, payload);
        }
    }

    fn answer(&self, bidi_id: u64, result: Value) {
        self.emit(
            "response",
            bidi_id,
            &json!({ "result": result }).to_string(),
        );
    }

    fn answer_err(&self, bidi_id: u64, msg: &str) {
        self.emit(
            "response",
            bidi_id,
            &json!({ "error": { "message": msg } }).to_string(),
        );
    }

    fn event(&self, method: &str, params: Value) {
        self.emit(
            "event",
            0,
            &json!({ "method": method, "params": params }).to_string(),
        );
    }

    fn alloc_cdp(&self) -> u64 {
        let mut g = self.next_cdp_id.lock().unwrap_or_else(|e| e.into_inner());
        *g += 1;
        *g
    }

    fn send_cdp(&self, cdp_id: u64, session: Option<&str>, method: &str, params: Value) -> Result<(), Fail> {
        let mut msg = json!({ "id": cdp_id, "method": method, "params": params });
        if let Some(s) = session {
            msg["sessionId"] = json!(s);
        }
        let bytes = msg.to_string();
        let mut w = self.writer.lock().unwrap_or_else(|e| e.into_inner());
        ws::send_text(&mut w, bytes.as_bytes())
    }

    /// Queue a CDP call whose response runs `action`.
    fn cdp_call(
        &self,
        session: Option<&str>,
        method: &str,
        params: Value,
        action: CdpAction,
    ) -> Result<(), Fail> {
        let id = self.alloc_cdp();
        self.state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .cdp_pending
            .insert(id, PendingCdp { action });
        self.send_cdp(id, session, method, params)
    }

    /// Ensure a CDP session exists for `context`; run `follow` now or
    /// after the attach lands.
    fn with_session(&self, context: &str, follow: FollowUp) -> Result<(), Fail> {
        let session = {
            let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
            st.sessions.get(context).cloned()
        };
        if let Some(sid) = session {
            return self.run_follow(context, &sid, follow);
        }
        let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(q) = st.attaching.get_mut(context) {
            q.push(follow);
            return Ok(());
        }
        st.attaching.insert(context.to_string(), Vec::new());
        drop(st);
        self.state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .attaching
            .get_mut(context)
            .map(|q| q.push(follow));
        self.cdp_call(
            None,
            "Target.attachToTarget",
            json!({ "targetId": context, "flatten": true }),
            CdpAction::Attach {
                context: context.to_string(),
            },
        )
    }

    fn run_follow(&self, context: &str, session: &str, follow: FollowUp) -> Result<(), Fail> {
        match follow {
            FollowUp::Quiet => Ok(()),
            FollowUp::Command {
                bidi_id,
                method,
                params,
            } => self.dispatch_with_session(bidi_id, &method, params, context, session),
        }
    }

    /// Domains the subscriptions require enabled on every session.
    fn wanted_domains(&self) -> Vec<&'static str> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut v = Vec::new();
        if st.subscriptions.contains("network") {
            v.push("Network.enable");
        }
        if st.subscriptions.contains("log") {
            v.push("Log.enable");
            v.push("Runtime.enable");
        }
        if st.subscriptions.contains("browsingContext") {
            // DEVT07: Page.enable so frame lifecycle milestones
            // (domContentEventFired / loadEventFired) reach the
            // session — they map to browsingContext.domContentLoaded /
            // .load for the Network tab's summary footer.  If the
            // engine's CDP lacks the Page domain the enable just
            // errors quietly and the footer omits the milestones.
            v.push("Page.enable");
        }
        v
    }

    fn enable_domains(&self, _context: &str, session: &str) -> Result<(), Fail> {
        for dom in self.wanted_domains() {
            // Domains enabled silently — their empty responses are
            // answered to no BiDi caller.
            self.cdp_call(Some(session), dom, json!({}), CdpAction::Step {
                follow: FollowUp::Quiet,
            })?;
        }
        // A global "Disable cache" outlives the toggle command — new
        // sessions inherit it (Chrome keeps it browser-wide).
        if self
            .state
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .cache_disabled
        {
            self.cdp_call(
                Some(session),
                "Network.setCacheDisabled",
                json!({ "cacheDisabled": true }),
                CdpAction::Step {
                    follow: FollowUp::Quiet,
                },
            )?;
        }
        Ok(())
    }

    fn dispatch(&self, bidi_id: u64, method: &str, params: Value) -> Result<(), Fail> {
        match method {
            "session.new" => {
                self.answer(bidi_id, json!({ "sessionId": "arora-bidi-1", "capabilities": {} }));
                Ok(())
            }
            "session.subscribe" => {
                let events = params
                    .get("events")
                    .and_then(|e| e.as_array())
                    .cloned()
                    .unwrap_or_default();
                {
                    let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
                    for ev in events {
                        if let Some(s) = ev.as_str() {
                            // accept both "network" and "network.beforeRequestSent" shapes
                            let group = s.split('.').next().unwrap_or(s).to_string();
                            st.subscriptions.insert(group);
                        }
                    }
                }
                // Start watching targets so context events + session
                // attach keep up with new tabs.
                self.cdp_call(
                    None,
                    "Target.setDiscoverTargets",
                    json!({ "discover": true }),
                    CdpAction::Step { follow: FollowUp::Quiet },
                )?;
                // Enable domains on every already-attached session.
                let attached: Vec<(String, String)> = {
                    self.state
                        .lock()
                        .unwrap_or_else(|e| e.into_inner())
                        .sessions
                        .iter()
                        .map(|(c, s)| (c.clone(), s.clone()))
                        .collect()
                };
                for (ctx, sid) in attached {
                    self.enable_domains(&ctx, &sid)?;
                }
                self.answer(bidi_id, json!({}));
                Ok(())
            }
            "browsingContext.getTree" => {
                self.cdp_call(
                    None,
                    "Target.getTargets",
                    json!({}),
                    CdpAction::Answer {
                        bidi_id,
                        kind: AnswerKind::Tree,
                    },
                )?;
                Ok(())
            }
            "browsingContext.navigate" => {
                let context = params
                    .get("context")
                    .and_then(|c| c.as_str())
                    .unwrap_or("")
                    .to_string();
                let url = params.get("url").and_then(|u| u.as_str()).unwrap_or("");
                self.with_session(
                    &context,
                    FollowUp::Command {
                        bidi_id,
                        method: method.to_string(),
                        params: params.clone(),
                    },
                )?;
                let _ = url;
                Ok(())
            }
            "script.evaluate" => {
                let context = params
                    .get("target")
                    .and_then(|t| t.get("context"))
                    .and_then(|c| c.as_str())
                    .or_else(|| params.get("context").and_then(|c| c.as_str()))
                    .unwrap_or("")
                    .to_string();
                self.with_session(
                    &context,
                    FollowUp::Command {
                        bidi_id,
                        method: method.to_string(),
                        params,
                    },
                )?;
                Ok(())
            }
            "storage.getCookies" => {
                // Session-scoped read — the browser-level cookie
                // methods (Storage.getCookies, Network.getAllCookies)
                // are not implemented in QtWebEngine's CDP.
                let context = params
                    .get("context")
                    .and_then(|c| c.as_str())
                    .map(str::to_string)
                    .or_else(|| {
                        self.state
                            .lock()
                            .unwrap_or_else(|e| e.into_inner())
                            .contexts
                            .keys()
                            .next()
                            .cloned()
                    })
                    .unwrap_or_default();
                if context.is_empty() {
                    self.answer_err(bidi_id, "storage.getCookies: no browsing context");
                    return Ok(());
                }
                self.with_session(
                    &context,
                    FollowUp::Command {
                        bidi_id,
                        method: method.to_string(),
                        params,
                    },
                )?;
                Ok(())
            }
            "browsingContext.close" => {
                let context = params
                    .get("context")
                    .and_then(|c| c.as_str())
                    .unwrap_or("")
                    .to_string();
                self.cdp_call(
                    None,
                    "Target.closeTarget",
                    json!({ "targetId": context }),
                    CdpAction::Answer {
                        bidi_id,
                        kind: AnswerKind::Raw,
                    },
                )?;
                Ok(())
            }
            "network.setCacheBehavior" => {
                // BiDi shape: {contexts?: [ctx,...], cacheBehavior:
                // "default"|"bypass"}.  An empty/missing contexts list
                // is a GLOBAL toggle — remembered in State so late
                // attaches inherit it.  Named contexts map to their
                // current session only (no lazy attach for a setting).
                let disabled = params
                    .get("cacheBehavior")
                    .and_then(|c| c.as_str())
                    .map(|c| c == "bypass")
                    .unwrap_or(false);
                let named: Vec<String> = params
                    .get("contexts")
                    .and_then(|c| c.as_array())
                    .map(|a| {
                        a.iter()
                            .filter_map(|v| v.as_str().map(str::to_string))
                            .collect()
                    })
                    .unwrap_or_default();
                let sessions: Vec<String> = {
                    let mut st =
                        self.state.lock().unwrap_or_else(|e| e.into_inner());
                    if named.is_empty() {
                        st.cache_disabled = disabled;
                        st.sessions.values().cloned().collect()
                    } else {
                        named
                            .iter()
                            .filter_map(|c| st.sessions.get(c).cloned())
                            .collect()
                    }
                };
                for sid in sessions {
                    self.cdp_call(
                        Some(&sid),
                        "Network.setCacheDisabled",
                        json!({ "cacheDisabled": disabled }),
                        CdpAction::Step {
                            follow: FollowUp::Quiet,
                        },
                    )?;
                }
                self.answer(bidi_id, json!({}));
                Ok(())
            }
            "session.end" => {
                self.alive.store(false, Ordering::SeqCst);
                self.answer(bidi_id, json!({}));
                Ok(())
            }
            _ => Err(Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("bidi: unsupported method {method}"),
            }),
        }
    }

    /// Second stage for commands that needed a session first.
    fn dispatch_with_session(
        &self,
        bidi_id: u64,
        method: &str,
        params: Value,
        _context: &str,
        session: &str,
    ) -> Result<(), Fail> {
        match method {
            "script.evaluate" => {
                let expr = params
                    .get("expression")
                    .and_then(|e| e.as_str())
                    .unwrap_or("")
                    .to_string();
                let await_promise = params
                    .get("awaitPromise")
                    .and_then(|a| a.as_bool())
                    .unwrap_or(false);
                self.cdp_call(
                    Some(session),
                    "Runtime.evaluate",
                    json!({
                        "expression": expr,
                        "awaitPromise": await_promise,
                        "returnByValue": true,
                        "userGesture": true,
                    }),
                    CdpAction::Answer {
                        bidi_id,
                        kind: AnswerKind::Eval,
                    },
                )
            }
            "browsingContext.navigate" => {
                let url = params.get("url").and_then(|u| u.as_str()).unwrap_or("");
                self.cdp_call(
                    Some(session),
                    "Page.navigate",
                    json!({ "url": url }),
                    CdpAction::Answer {
                        bidi_id,
                        kind: AnswerKind::Raw,
                    },
                )
            }
            "storage.getCookies" => {
                // Scope to the context's current URL when known.
                let mut p = json!({});
                if let Some(u) = params.get("urls").cloned().or_else(|| {
                    self.state
                        .lock()
                        .unwrap_or_else(|e| e.into_inner())
                        .contexts
                        .get(_context)
                        .map(|(u, _)| json!([u]))
                }) {
                    p["urls"] = u;
                }
                self.cdp_call(
                    Some(session),
                    "Network.getCookies",
                    p,
                    CdpAction::Answer {
                        bidi_id,
                        kind: AnswerKind::Cookies,
                    },
                )
            }
            _ => Err(Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("bidi: unsupported session method {method}"),
            }),
        }
        .map(|_| ())
    }

    /* -------- inbound side (reader thread) -------- */

    fn on_message(&self, msg: &Value) {
        if let Some(id) = msg.get("id").and_then(|i| i.as_u64()) {
            let pending = self
                .state
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .cdp_pending
                .remove(&id);
            let Some(p) = pending else { return };
            if let Some(err) = msg.get("error") {
                let text = err.get("message").and_then(|m| m.as_str()).unwrap_or("cdp error");
                match p.action {
                    CdpAction::Answer { bidi_id, .. } => {
                        self.answer_err(bidi_id, text);
                        return;
                    }
                    CdpAction::Attach { context, .. } => {
                        // take queued follow-ups out of the attaching map
                        let q = self
                            .state
                            .lock()
                            .unwrap_or_else(|e| e.into_inner())
                            .attaching
                            .remove(&context)
                            .unwrap_or_default();
                        for f in q {
                            if let FollowUp::Command { bidi_id, .. } = f {
                                self.answer_err(bidi_id, text);
                            }
                        }
                        return;
                    }
                    CdpAction::Step { .. } => return,
                }
            }
            let result = msg.get("result").cloned().unwrap_or(json!({}));
            self.complete(p.action, result);
            return;
        }
        // event
        let name = msg.get("method").and_then(|m| m.as_str()).unwrap_or("");
        let params = msg.get("params").cloned().unwrap_or(json!({}));
        let session = msg.get("sessionId").and_then(|s| s.as_str()).unwrap_or("");
        self.on_event(name, params, session);
    }

    fn complete(&self, action: CdpAction, result: Value) {
        match action {
            CdpAction::Attach { context, .. } => {
                let sid = result
                    .get("sessionId")
                    .and_then(|s| s.as_str())
                    .unwrap_or("")
                    .to_string();
                if sid.is_empty() {
                    return;
                }
                let queued = {
                    let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
                    st.sessions.insert(context.clone(), sid.clone());
                    st.attaching.remove(&context).unwrap_or_default()
                };
                let _ = self.enable_domains(&context, &sid);
                for f in queued {
                    let _ = self.run_follow(&context, &sid, f);
                }
            }
            CdpAction::Step { follow } => {
                // quiet bookkeeping step; nothing to answer.
                let _ = follow;
            }
            CdpAction::Answer { bidi_id, kind } => match kind {
                AnswerKind::Raw => self.answer(bidi_id, result),
                AnswerKind::Eval => self.answer(bidi_id, translate_eval(&result)),
                AnswerKind::Tree => self.answer(bidi_id, translate_tree(&result)),
                AnswerKind::Cookies => self.answer(bidi_id, translate_cookies(&result)),
            },
        }
    }

    fn on_event(&self, name: &str, params: Value, session: &str) {
        let subscribed = |group: &str| {
            self.state
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .subscriptions
                .contains(group)
        };
        match name {
            "Target.targetCreated" => {
                let info = &params["targetInfo"];
                if info.get("type").and_then(|t| t.as_str()) != Some("page") {
                    return;
                }
                if !subscribed("browsingContext") {
                    return;
                }
                let ctx = info.get("targetId").and_then(|t| t.as_str()).unwrap_or("");
                let url = info.get("url").and_then(|u| u.as_str()).unwrap_or("");
                let title = info.get("title").and_then(|t| t.as_str()).unwrap_or("");
                self.state
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .contexts
                    .insert(ctx.to_string(), (url.to_string(), title.to_string()));
                self.event(
                    "browsingContext.contextCreated",
                    json!({ "context": ctx, "url": url, "title": title }),
                );
            }
            "Target.targetDestroyed" => {
                if !subscribed("browsingContext") {
                    return;
                }
                let ctx = params.get("targetId").and_then(|t| t.as_str()).unwrap_or("");
                self.state
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .contexts
                    .remove(ctx);
                self.event(
                    "browsingContext.contextDestroyed",
                    json!({ "context": ctx }),
                );
            }
            "Target.targetInfoChanged" => {
                if !subscribed("browsingContext") {
                    return;
                }
                let info = &params["targetInfo"];
                if info.get("type").and_then(|t| t.as_str()) != Some("page") {
                    return;
                }
                let ctx = info.get("targetId").and_then(|t| t.as_str()).unwrap_or("");
                let url = info.get("url").and_then(|u| u.as_str()).unwrap_or("");
                let mut st = self.state.lock().unwrap_or_else(|e| e.into_inner());
                let entry = st.contexts.entry(ctx.to_string()).or_default();
                if entry.0 != url && !url.is_empty() {
                    entry.0 = url.to_string();
                    drop(st);
                    self.event(
                        "browsingContext.navigationStarted",
                        json!({ "context": ctx, "url": url }),
                    );
                }
            }
            "Network.requestWillBeSent" => {
                if !subscribed("network") {
                    return;
                }
                let req = &params["request"];
                self.event(
                    "network.beforeRequestSent",
                    json!({
                        "context": self.context_for_session(session),
                        "timestamp": millis(&params["timestamp"]),
                        "initiator": params["initiator"],
                        // Set on the redirect hop's requestWillBeSent —
                        // the panel closes out the previous row with it.
                        "redirectResponse": params["redirectResponse"],
                        "request": {
                            "request": params["requestId"],
                            "url": req["url"],
                            "method": req["method"],
                            "type": params["type"],
                        },
                    }),
                );
            }
            "Network.responseReceived" => {
                if !subscribed("network") {
                    return;
                }
                let resp = &params["response"];
                self.event(
                    "network.responseStarted",
                    json!({
                        "context": self.context_for_session(session),
                        "timestamp": millis(&params["timestamp"]),
                        "type": params["type"],
                        "request": { "request": params["requestId"], "url": resp["url"] },
                        "response": {
                            "url": resp["url"],
                            "status": resp["status"],
                            "statusText": resp["statusText"],
                            "mimeType": resp["mimeType"],
                            "encodedDataLength": resp["encodedDataLength"],
                            // DEVT07: cache provenance (the panel's
                            // Size column shows "(memory cache)" etc.
                            // instead of a bogus byte count) and the
                            // raw resource-timing block — the waterfall
                            // splits its bar at receiveHeadersEnd.
                            "fromDiskCache": resp["fromDiskCache"],
                            "fromServiceWorker": resp["fromServiceWorker"],
                            "fromPrefetchCache": resp["fromPrefetchCache"],
                            "timing": resp["timing"],
                        },
                    }),
                );
            }
            "Network.dataReceived" => {
                // Per-chunk events stay bridge-side — the panel only
                // needs the decoded-byte total once, attached to the
                // completion (DEVT07's "resources" footer figure).
                if !subscribed("network") {
                    return;
                }
                if let Some(id) = params["requestId"].as_str() {
                    let n = params["dataLength"].as_i64().unwrap_or(0);
                    if n > 0 {
                        let mut st =
                            self.state.lock().unwrap_or_else(|e| e.into_inner());
                        if st.decoded_bytes.len() < 8192
                            || st.decoded_bytes.contains_key(id)
                        {
                            *st.decoded_bytes.entry(id.to_string()).or_insert(0) +=
                                n;
                        }
                    }
                }
            }
            "Network.loadingFinished" => {
                if !subscribed("network") {
                    return;
                }
                let decoded = params["requestId"].as_str().and_then(|id| {
                    self.state
                        .lock()
                        .unwrap_or_else(|e| e.into_inner())
                        .decoded_bytes
                        .remove(id)
                });
                self.event(
                    "network.responseCompleted",
                    json!({
                        "context": self.context_for_session(session),
                        "timestamp": millis(&params["timestamp"]),
                        "request": { "request": params["requestId"] },
                        "response": {
                            "encodedDataLength": params["encodedDataLength"],
                            "decodedDataLength": decoded,
                        },
                    }),
                );
            }
            "Network.requestServedFromCache" => {
                // DEVT07: memory-cache hits report through this event
                // rather than a response flag — the panel marks the
                // row "(memory cache)" like Chrome.
                if !subscribed("network") {
                    return;
                }
                self.event(
                    "network.requestServedFromCache",
                    json!({
                        "context": self.context_for_session(session),
                        "request": { "request": params["requestId"] },
                    }),
                );
            }
            "Network.loadingFailed" => {
                if !subscribed("network") {
                    return;
                }
                if let Some(id) = params["requestId"].as_str() {
                    self.state
                        .lock()
                        .unwrap_or_else(|e| e.into_inner())
                        .decoded_bytes
                        .remove(id);
                }
                self.event(
                    "network.fetchError",
                    json!({
                        "context": self.context_for_session(session),
                        "timestamp": millis(&params["timestamp"]),
                        "type": params["type"],
                        "request": { "request": params["requestId"] },
                        "errorText": params["errorText"],
                    }),
                );
            }
            "Page.domContentEventFired" | "Page.loadEventFired" => {
                // DEVT07: BiDi carries these as browsingContext
                // milestones — the panel's summary footer shows
                // Chrome's DOMContentLoaded / Load times.  Same CDP
                // MonotonicTime clock as the network events, so they
                // subtract cleanly off the waterfall's window start.
                if !subscribed("browsingContext") {
                    return;
                }
                self.event(
                    if name == "Page.domContentEventFired" {
                        "browsingContext.domContentLoaded"
                    } else {
                        "browsingContext.load"
                    },
                    json!({
                        "context": self.context_for_session(session),
                        "timestamp": millis(&params["timestamp"]),
                    }),
                );
            }
            "Runtime.consoleAPICalled" => {
                if !subscribed("log") {
                    return;
                }
                let level = params.get("type").and_then(|t| t.as_str()).unwrap_or("log");
                let text = params
                    .get("args")
                    .and_then(|a| a.as_array())
                    .map(|args| {
                        args.iter()
                            .map(|a| {
                                a.get("value")
                                    .map(|v| match v {
                                        Value::String(s) => s.clone(),
                                        other => other.to_string(),
                                    })
                                    .or_else(|| {
                                        a.get("description").and_then(|d| d.as_str()).map(String::from)
                                    })
                                    .unwrap_or_else(|| a.get("type").and_then(|t| t.as_str()).unwrap_or("?").to_string())
                            })
                            .collect::<Vec<_>>()
                            .join(" ")
                    })
                    .unwrap_or_default();
                self.event(
                    "log.entryAdded",
                    json!({
                        "type": "console",
                        "level": level,
                        "text": text,
                        "context": self.context_for_session(session),
                    }),
                );
            }
            "Log.entryAdded" | "Runtime.exceptionThrown" => {
                if !subscribed("log") {
                    return;
                }
                let (level, text) = if name == "Log.entryAdded" {
                    let e = &params["entry"];
                    (
                        e.get("level").and_then(|l| l.as_str()).unwrap_or("info").to_string(),
                        e.get("text").and_then(|t| t.as_str()).unwrap_or("").to_string(),
                    )
                } else {
                    let d = &params["exceptionDetails"];
                    (
                        "error".to_string(),
                        d.get("text")
                            .and_then(|t| t.as_str())
                            .or_else(|| d["exception"].get("description").and_then(|t| t.as_str()))
                            .unwrap_or("exception")
                            .to_string(),
                    )
                };
                self.event(
                    "log.entryAdded",
                    json!({
                        "type": "javascript",
                        "level": level,
                        "text": text,
                        "context": self.context_for_session(session),
                    }),
                );
            }
            _ => {}
        }
    }

    fn context_for_session(&self, session: &str) -> Value {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        for (ctx, sid) in st.sessions.iter() {
            if sid == session {
                return json!(ctx);
            }
        }
        Value::Null
    }
}

/// CDP MonotonicTime is seconds; the panel works in milliseconds.
/// Anything non-numeric serializes as null rather than a guess.
fn millis(v: &Value) -> Value {
    v.as_f64().map(|s| json!(s * 1000.0)).unwrap_or(Value::Null)
}

/* -------- CDP -> BiDi translations -------- */

fn translate_eval(result: &Value) -> Value {
    let r = result.get("result").unwrap_or(result);
    let ex = result.get("exceptionDetails");
    if let Some(e) = ex {
        let text = e
            .get("text")
            .and_then(|t| t.as_str())
            .or_else(|| e["exception"].get("description").and_then(|t| t.as_str()))
            .unwrap_or("evaluation failed");
        return json!({
            "type": "exception",
            "exceptionDetails": { "text": text },
        });
    }
    let ty = r.get("type").and_then(|t| t.as_str()).unwrap_or("undefined");
    let subtype = r.get("subtype").and_then(|t| t.as_str());
    let mut out = json!({ "type": ty });
    match ty {
        "undefined" => {}
        "object" if subtype == Some("null") => {
            out["type"] = json!("null");
        }
        "number" => {
            if let Some(v) = r.get("value") {
                out["value"] = v.clone();
            } else if let Some(u) = r.get("unserializableValue") {
                out["value"] = json!({ "type": "special", "value": u });
            }
        }
        _ => {
            if let Some(v) = r.get("value") {
                out["value"] = v.clone();
            } else if let Some(d) = r.get("description").and_then(|d| d.as_str()) {
                out["value"] = json!(d);
            }
        }
    }
    json!({ "type": "success", "result": out })
}

fn translate_tree(result: &Value) -> Value {
    let mut contexts = Vec::new();
    if let Some(infos) = result.get("targetInfos").and_then(|t| t.as_array()) {
        for t in infos {
            if t.get("type").and_then(|x| x.as_str()) != Some("page") {
                continue;
            }
            contexts.push(json!({
                "context": t["targetId"],
                "url": t["url"],
                "title": t["title"],
                "children": [],
            }));
        }
    }
    json!({ "contexts": contexts })
}

fn translate_cookies(result: &Value) -> Value {
    let mut cookies = Vec::new();
    if let Some(list) = result.get("cookies").and_then(|c| c.as_array()) {
        for c in list {
            cookies.push(json!({
                "name": c["name"],
                "value": c["value"],
                "domain": c["domain"],
                "path": c["path"],
                "secure": c["secure"],
                "httpOnly": c["httpOnly"],
                "expiry": c["expires"],
            }));
        }
    }
    json!({ "cookies": cookies })
}

/* ------------------------------------------------------------------
 * Public handle + FFI.
 * ---------------------------------------------------------------- */

enum Outbound {
    Command(u64, String, Value),
    Shutdown,
}

/// One BiDi-front/CDP-back connection.  Not Sync-visible details —
/// everything funnels through the mutex-guarded Core.
pub struct Client {
    core: Arc<Core>,
    outbox: Mutex<Option<Sender<Outbound>>>,
    alive: Arc<AtomicBool>,
    reader: Mutex<Option<thread::JoinHandle<()>>>,
    dispatcher: Mutex<Option<thread::JoinHandle<()>>>,
}

impl Client {
    /// Connect to a Chromium devtools websocket endpoint.
    /// `ws_url` like "127.0.0.1:PORT/devtools/browser/GUID"; `origin`
    /// is the per-session allow-origins token.
    pub fn connect(host: &str, port: u16, path: &str, origin: &str) -> Result<Client, Fail> {
        // Discover on a dedicated socket first — the devtools HTTP
        // server won't service a second connection while one is held.
        let owned;
        let path = if path.is_empty() {
            owned = ws::discover_path(host, port)?;
            owned.as_str()
        } else {
            path
        };
        let mut stream = TcpStream::connect((host, port))?;
        stream.set_nodelay(true).ok();
        ws::handshake(&mut stream, host, port, path, origin)?;
        let reader_stream = stream.try_clone()?;
        let alive = Arc::new(AtomicBool::new(true));
        let (tx, rx) = channel::<Outbound>();

        let core = Arc::new(Core {
            writer: Mutex::new(stream),
            next_bidi_id: Mutex::new(0),
            next_cdp_id: Mutex::new(0),
            sink: Mutex::new(None),
            state: Mutex::new(State::default()),
            alive: alive.clone(),
        });

        // Reader thread: inbound frames -> dispatch.
        let rcore = core.clone();
        let ralive = alive.clone();
        let reader = thread::spawn(move || {
            let mut reader = reader_stream;
            let mut data: Vec<u8> = Vec::new();
            while ralive.load(Ordering::SeqCst) {
                match ws::recv_text(&mut reader, &rcore.writer, &mut data) {
                    ws::Incoming::Text(s) => {
                        if let Ok(v) = serde_json::from_str::<Value>(&s) {
                            rcore.on_message(&v);
                        }
                    }
                    ws::Incoming::Closed => break,
                }
            }
            ralive.store(false, Ordering::SeqCst);
            rcore.emit("state", 0, "{\"state\":\"closed\"}");
        });

        // Dispatcher thread: outbound commands -> translation -> wire.
        let dcore = core.clone();
        let dispatcher = thread::spawn(move || {
            while let Ok(msg) = rx.recv() {
                match msg {
                    Outbound::Command(id, method, params) => {
                        if let Err(e) = dcore.dispatch(id, &method, params) {
                            dcore.answer_err(id, &e.msg);
                        }
                    }
                    Outbound::Shutdown => break,
                }
            }
        });

        Ok(Client {
            core,
            outbox: Mutex::new(Some(tx)),
            alive,
            reader: Mutex::new(Some(reader)),
            dispatcher: Mutex::new(Some(dispatcher)),
        })
    }

    /// Queue a BiDi-shaped command; returns its correlation id.  The
    /// response lands on the registered callback as ("response", id).
    pub fn command(&self, method: &str, params_json: &str) -> u64 {
        let id = {
            let mut g = self.core.next_bidi_id.lock().unwrap_or_else(|e| e.into_inner());
            *g += 1;
            *g
        };
        let params = serde_json::from_str(params_json).unwrap_or(json!({}));
        let tx = self.outbox.lock().unwrap_or_else(|e| e.into_inner());
        match tx.as_ref() {
            Some(t) if t.send(Outbound::Command(id, method.to_string(), params)).is_ok() => id,
            _ => {
                self.core.answer_err(id, "bidi: channel closed");
                id
            }
        }
    }

    pub fn set_sink(&self, f: EventSink) {
        *self.core.sink.lock().unwrap_or_else(|e| e.into_inner()) = Some(f);
    }

    pub fn is_alive(&self) -> bool {
        self.alive.load(Ordering::SeqCst)
    }
}

impl Drop for Client {
    fn drop(&mut self) {
        self.alive.store(false, Ordering::SeqCst);
        if let Some(tx) = self.outbox.lock().unwrap_or_else(|e| e.into_inner()).take() {
            let _ = tx.send(Outbound::Shutdown);
        }
        // Nudge the reader out of a blocking recv.
        if let Ok(w) = self.core.writer.lock() {
            let _ = w.shutdown(std::net::Shutdown::Both);
        }
        for slot in [&mut self.reader, &mut self.dispatcher] {
            if let Some(h) = slot.lock().unwrap_or_else(|e| e.into_inner()).take() {
                let _ = h.join();
            }
        }
    }
}

/* -------- FFI -------- */

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};

pub type RcBidiCallback =
    unsafe extern "C" fn(userdata: *mut c_void, kind: *const c_char, id: u64, json: *const c_char);

unsafe fn cstr(p: *const c_char) -> Result<String, Fail> {
    if p.is_null() {
        return Err(Fail {
            status: RcStatus::InvalidArgument,
            msg: "null string".into(),
        });
    }
    Ok(CStr::from_ptr(p).to_string_lossy().into_owned())
}

/// Connect: host/port/ws-path/origin.  Returns an opaque handle or
/// NULL (rc_last_error_message() has the reason).
#[no_mangle]
pub unsafe extern "C" fn rc_bidi_connect(
    host: *const c_char,
    port: u16,
    path: *const c_char,
    origin: *const c_char,
    cb: RcBidiCallback,
    userdata: *mut c_void,
) -> *mut Client {
    let res = catch_unwind(AssertUnwindSafe(|| -> Result<Client, Fail> {
        let h = cstr(host)?;
        let p = cstr(path)?;
        let o = cstr(origin)?;
        let client = Client::connect(&h, port, &p, &o)?;
        let ud = userdata as usize;
        client.set_sink(Box::new(move |kind, id, payload| {
            let k = CString::new(kind).unwrap_or_default();
            let j = CString::new(payload).unwrap_or_default();
            unsafe { cb(ud as *mut c_void, k.as_ptr(), id, j.as_ptr()) }
        }));
        Ok(client)
    }));
    match res {
        Ok(Ok(c)) => Box::into_raw(Box::new(c)),
        Ok(Err(e)) => {
            crate::error::set_error(&e.msg);
            std::ptr::null_mut()
        }
        Err(_) => std::ptr::null_mut(),
    }
}

/// Queue a BiDi-shaped command; returns the correlation id (0 on
/// error).  Response arrives via the connect callback ("response").
#[no_mangle]
pub unsafe extern "C" fn rc_bidi_command(
    client: *mut Client,
    method: *const c_char,
    params_json: *const c_char,
) -> u64 {
    match catch_unwind(AssertUnwindSafe(|| -> Result<u64, Fail> {
        if client.is_null() {
            return Err(Fail {
                status: RcStatus::InvalidArgument,
                msg: "bidi: null client".into(),
            });
        }
        let c = &*client;
        let m = cstr(method)?;
        let p = if params_json.is_null() {
            "{}".to_string()
        } else {
            cstr(params_json)?
        };
        Ok(c.command(&m, &p))
    })) {
        Ok(Ok(id)) => id,
        Ok(Err(e)) => {
            crate::error::set_error(&e.msg);
            0
        }
        Err(_) => 0,
    }
}

#[no_mangle]
pub unsafe extern "C" fn rc_bidi_is_alive(client: *const Client) -> i32 {
    if client.is_null() {
        return 0;
    }
    (*client).is_alive() as i32
}

/// Close + free the handle.  NULL is a no-op.
#[no_mangle]
pub unsafe extern "C" fn rc_bidi_free(client: *mut Client) {
    if client.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        drop(Box::from_raw(client));
    }));
}

/* -------- tests -------- */

#[cfg(test)]
mod tests {
    use super::*;
    use std::net::TcpListener;
    use std::sync::atomic::AtomicUsize;

    /// Fake devtools ws server: accepts, answers the upgrade, then
    /// replies to each JSON-RPC message with a canned result and emits
    /// canned events, all scripted.
    struct FakeServer {
        port: u16,
        stop: Arc<AtomicBool>,
        handle: Option<thread::JoinHandle<()>>,
    }

    impl FakeServer {
        fn start(scripted: Arc<Mutex<Vec<String>>>) -> FakeServer {
            let listener = TcpListener::bind("127.0.0.1:0").unwrap();
            let port = listener.local_addr().unwrap().port();
            let stop = Arc::new(AtomicBool::new(false));
            let stop2 = stop.clone();
            let scripted2 = scripted.clone();
            let handle = thread::spawn(move || {
                let (mut s, _) = listener.accept().unwrap();
                // read HTTP headers
                let mut buf = Vec::new();
                let mut byte = [0u8; 1];
                while !buf.ends_with(b"\r\n\r\n") {
                    if s.read(&mut byte).unwrap_or(0) == 0 {
                        return;
                    }
                    buf.push(byte[0]);
                }
                let head = String::from_utf8_lossy(&buf);
                if !head.contains("bidi.test.invalid") {
                    return; // origin gate check: refuse wrong origin
                }
                s.write_all(
                    b"HTTP/1.1 101 WebSocket Protocol Handshake\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: x\r\n\r\n",
                )
                .unwrap();
                loop {
                    if stop2.load(Ordering::SeqCst) {
                        return;
                    }
                    // read a frame
                    let mut hdr = [0u8; 2];
                    if s.read_exact(&mut hdr).is_err() {
                        return;
                    }
                    let mut len = (hdr[1] & 0x7f) as usize;
                    if len == 126 {
                        let mut e = [0u8; 2];
                        if s.read_exact(&mut e).is_err() {
                            return;
                        }
                        len = u16::from_be_bytes(e) as usize;
                    }
                    let mut mask = [0u8; 4];
                    let mut payload = vec![0u8; len];
                    if s.read_exact(&mut mask).is_err() || s.read_exact(&mut payload).is_err() {
                        return;
                    }
                    for i in 0..len {
                        payload[i] ^= mask[i % 4];
                    }
                    let text = String::from_utf8_lossy(&payload).to_string();
                    scripted2.lock().unwrap().push(text.clone());
                    let v: Value = serde_json::from_str(&text).unwrap();
                    let id = v["id"].as_u64().unwrap_or(0);
                    let method = v["method"].as_str().unwrap_or("").to_string();
                    let resp = match method.as_str() {
                        "Target.getTargets" => json!({"id":id,"result":{"targetInfos":[
                            {"targetId":"AAA","type":"page","url":"http://a/","title":"A"},
                            {"targetId":"BBB","type":"page","url":"http://b/","title":"B"},
                            {"targetId":"TTT","type":"service_worker","url":"ws://w/"}
                        ]}}),
                        "Target.attachToTarget" => json!({"id":id,"result":{"sessionId":"S1"}}),
                        "Runtime.evaluate" => json!({"id":id,"result":{"result":{"type":"number","value":7}}}),
                        "Network.getCookies" => json!({"id":id,"result":{"cookies":[
                            {"name":"n","value":"v","domain":"d","path":"/","secure":true,"httpOnly":false,"expires":1}
                        ]}}),
                        _ => json!({"id":id,"result":{}}),
                    };
                    // unmasked server->client frame
                    let body = resp.to_string();
                    let n = body.len();
                    let mut frame = vec![0x81u8];
                    if n < 126 {
                        frame.push(n as u8);
                    } else {
                        frame.push(126);
                        frame.extend_from_slice(&(n as u16).to_be_bytes());
                    }
                    frame.extend_from_slice(body.as_bytes());
                    if s.write_all(&frame).is_err() {
                        return;
                    }
                }
            });
            FakeServer {
                port,
                stop,
                handle: Some(handle),
            }
        }
    }

    impl Drop for FakeServer {
        fn drop(&mut self) {
            self.stop.store(true, Ordering::SeqCst);
            if let Some(h) = self.handle.take() {
                let _ = h.join();
            }
        }
    }

    fn collect(
        got: &Arc<Mutex<Vec<(String, u64, String)>>>,
        want: &str,
        timeout_ms: u64,
    ) -> Option<String> {
        let t0 = std::time::Instant::now();
        while t0.elapsed() < std::time::Duration::from_millis(timeout_ms) {
            {
                let g = got.lock().unwrap();
                if let Some((_, _, payload)) = g.iter().find(|(k, _, _)| k == want) {
                    return Some(payload.clone());
                }
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        None
    }

    static SEEN_COUNTER: AtomicUsize = AtomicUsize::new(0);

    #[test]
    fn end_to_end() {
        let scripted = Arc::new(Mutex::new(Vec::<String>::new()));
        let server = FakeServer::start(scripted.clone());
        let got: Arc<Mutex<Vec<(String, u64, String)>>> = Arc::new(Mutex::new(Vec::new()));
        let got2 = got.clone();
        let client = Client::connect(
            "127.0.0.1",
            server.port,
            "/devtools/browser/fake",
            "http://bidi.test.invalid",
        )
        .expect("connect");
        client.set_sink(Box::new(move |k, id, p| {
            got2.lock().unwrap().push((k.to_string(), id, p.to_string()));
        }));

        let _id = client.command("session.new", "{}");
        let resp = collect(&got, "response", 3000).expect("session.new response");
        assert!(resp.contains("arora-bidi-1"), "got {resp}");

        let _ = client.command("session.subscribe", r#"{"events":["network","log","browsingContext"]}"#);
        collect(&got, "response", 3000).unwrap();

        let _ = client.command("browsingContext.getTree", "{}");
        let mut tree = None;
        let t0 = std::time::Instant::now();
        while t0.elapsed() < std::time::Duration::from_secs(3) {
            let g = got.lock().unwrap();
            for (k, _, p) in g.iter() {
                if k == "response" && p.contains("contexts") {
                    tree = Some(p.clone());
                }
            }
            drop(g);
            if tree.is_some() {
                break;
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        let tree = tree.expect("tree");
        assert!(tree.contains("AAA"), "tree {tree}");
        assert!(!tree.contains("TTT"), "worker filtered {tree}");

        // evaluate rides attach -> session command chaining
        let _ = client.command("script.evaluate", r#"{"expression":"1+2","target":{"context":"AAA"},"awaitPromise":true}"#);
        let mut evalr = None;
        let t0 = std::time::Instant::now();
        while t0.elapsed() < std::time::Duration::from_secs(3) {
            let g = got.lock().unwrap();
            for (k, _, p) in g.iter() {
                if k == "response" && p.contains("\"value\":7") {
                    evalr = Some(p.clone());
                }
            }
            drop(g);
            if evalr.is_some() {
                break;
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        let evalr = match evalr {
            Some(e) => e,
            None => panic!("eval missing; wire={:?} got={:?}", *scripted.lock().unwrap(), *got.lock().unwrap()),
        };
        assert!(evalr.contains("\"type\":\"number\""), "eval {evalr}");

        let _ = client.command("storage.getCookies", r#"{"context":"AAA"}"#);
        let mut ck = None;
        let t0 = std::time::Instant::now();
        while t0.elapsed() < std::time::Duration::from_secs(3) {
            let g = got.lock().unwrap();
            for (k, _, p) in g.iter() {
                if k == "response" && p.contains("cookies") {
                    ck = Some(p.clone());
                }
            }
            drop(g);
            if ck.is_some() {
                break;
            }
            thread::sleep(std::time::Duration::from_millis(10));
        }
        assert!(ck.expect("cookies").contains("\"name\":\"n\""));

        // confirm the wire saw attach before evaluate
        let wire = scripted.lock().unwrap().clone();
        let attach_pos = wire.iter().position(|m| m.contains("attachToTarget"));
        let eval_pos = wire.iter().position(|m| m.contains("Runtime.evaluate"));
        assert!(attach_pos.is_some() && eval_pos.is_some() && attach_pos < eval_pos);
        let _ = SEEN_COUNTER.fetch_add(1, Ordering::SeqCst);
        drop(client);
        drop(server);
    }

    #[test]
    fn origin_rejected() {
        let scripted = Arc::new(Mutex::new(Vec::new()));
        let server = FakeServer::start(scripted);
        let r = Client::connect(
            "127.0.0.1",
            server.port,
            "/devtools/browser/fake",
            "http://wrong.origin",
        );
        assert!(r.is_err());
        drop(server);
    }

    /* -------- COV07: pure translators + Core over a socketpair -------- */

    use std::ptr;

    /// Two ends of a connected loopback socket.
    fn sockpair() -> (TcpStream, TcpStream) {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let a = TcpStream::connect(l.local_addr().unwrap()).unwrap();
        let (b, _) = l.accept().unwrap();
        (a, b)
    }

    /// A Core wired to a live socket and a collecting sink — the
    /// dispatcher/reader threads are not needed: tests drive
    /// dispatch()/on_message() synchronously and read the peer's wire
    /// frames to assert the CDP translation.
    fn mkcore() -> (Core, TcpStream, Arc<Mutex<Vec<(String, u64, String)>>>) {
        let (a, b) = sockpair();
        let got = Arc::new(Mutex::new(Vec::new()));
        let got2 = got.clone();
        let core = Core {
            writer: Mutex::new(a),
            next_bidi_id: Mutex::new(0),
            next_cdp_id: Mutex::new(0),
            sink: Mutex::new(Some(Box::new(move |k, id, p| {
                got2.lock()
                    .unwrap()
                    .push((k.to_string(), id, p.to_string()));
            }))),
            state: Mutex::new(State::default()),
            alive: Arc::new(AtomicBool::new(true)),
        };
        (core, b, got)
    }

    /// Read one masked client frame on the peer end -> parsed JSON.
    fn read_wire(s: &mut TcpStream) -> Value {
        s.set_read_timeout(Some(std::time::Duration::from_secs(5)))
            .ok();
        let mut hdr = [0u8; 2];
        s.read_exact(&mut hdr).unwrap();
        assert_eq!(hdr[0], 0x81);
        assert!(hdr[1] & 0x80 != 0, "client frames must be masked");
        let mut len = (hdr[1] & 0x7f) as usize;
        if len == 126 {
            let mut e = [0u8; 2];
            s.read_exact(&mut e).unwrap();
            len = u16::from_be_bytes(e) as usize;
        } else if len == 127 {
            let mut e = [0u8; 8];
            s.read_exact(&mut e).unwrap();
            len = u64::from_be_bytes(e) as usize;
        }
        let mut mask = [0u8; 4];
        s.read_exact(&mut mask).unwrap();
        let mut p = vec![0u8; len];
        s.read_exact(&mut p).unwrap();
        for i in 0..len {
            p[i] ^= mask[i % 4];
        }
        serde_json::from_slice(&p).unwrap()
    }

    fn response_for(got: &Arc<Mutex<Vec<(String, u64, String)>>>, id: u64) -> Option<String> {
        got.lock()
            .unwrap()
            .iter()
            .find(|(k, i, _)| k == "response" && *i == id)
            .map(|(_, _, p)| p.clone())
    }

    fn events_named(
        got: &Arc<Mutex<Vec<(String, u64, String)>>>,
        name: &str,
    ) -> Vec<String> {
        got.lock()
            .unwrap()
            .iter()
            .filter(|(k, _, p)| k == "event" && p.contains(name))
            .map(|(_, _, p)| p.clone())
            .collect()
    }

    #[test]
    fn translators_cover_all_shapes() {
        // translate_eval: plain typed results.
        let v = translate_eval(&json!({"result": {"type": "number", "value": 7}}));
        assert_eq!(
            v,
            json!({"type": "success", "result": {"type": "number", "value": 7}})
        );
        let v = translate_eval(&json!({"result": {"type": "string", "value": "hi"}}));
        assert_eq!(v["result"]["value"], "hi");
        // exceptionDetails wins over the result payload.
        let v = translate_eval(&json!({
            "result": {"type": "number", "value": 1},
            "exceptionDetails": {"text": "boom"}
        }));
        assert_eq!(v["type"], "exception");
        assert_eq!(v["exceptionDetails"]["text"], "boom");
        let v = translate_eval(&json!({
            "exceptionDetails": {"exception": {"description": "nested"}}
        }));
        assert_eq!(v["exceptionDetails"]["text"], "nested");
        let v = translate_eval(&json!({"exceptionDetails": {}}));
        assert_eq!(v["exceptionDetails"]["text"], "evaluation failed");
        // undefined / missing result.
        let v = translate_eval(&json!({}));
        assert_eq!(v["result"]["type"], "undefined");
        assert!(v["result"].get("value").is_none());
        // null subtype folds to "null".
        let v = translate_eval(&json!({"result": {"type": "object", "subtype": "null"}}));
        assert_eq!(v["result"]["type"], "null");
        // NaN/Infinity travel as unserializableValue.
        let v = translate_eval(
            &json!({"result": {"type": "number", "unserializableValue": "NaN"}}),
        );
        assert_eq!(
            v["result"]["value"],
            json!({"type": "special", "value": "NaN"})
        );
        // object without a value falls back to its description.
        let v = translate_eval(
            &json!({"result": {"type": "object", "description": "Map(2)"}}),
        );
        assert_eq!(v["result"]["value"], "Map(2)");
        // object with neither value nor description emits no value key.
        let v = translate_eval(&json!({"result": {"type": "object"}}));
        assert!(v["result"].get("value").is_none());

        // translate_tree: only pages become contexts.
        assert_eq!(translate_tree(&json!({})), json!({"contexts": []}));
        assert_eq!(
            translate_tree(&json!({"targetInfos": 42})),
            json!({"contexts": []})
        );
        let v = translate_tree(&json!({"targetInfos": [
            {"targetId": "P", "type": "page", "url": "u", "title": "t"},
            {"targetId": "W", "type": "worker", "url": "w"},
            {"targetId": "S", "type": "service_worker"}
        ]}));
        let ctx = v["contexts"].as_array().unwrap();
        assert_eq!(ctx.len(), 1);
        assert_eq!(ctx[0]["context"], "P");
        assert_eq!(ctx[0]["children"], json!([]));

        // translate_cookies: field rename expires->expiry, empty-safe.
        assert_eq!(translate_cookies(&json!({})), json!({"cookies": []}));
        let v = translate_cookies(&json!({"cookies": [{
            "name": "n", "value": "v", "domain": "d", "path": "/",
            "secure": true, "httpOnly": false, "expires": 9
        }]}));
        assert_eq!(v["cookies"][0]["name"], "n");
        assert_eq!(v["cookies"][0]["expiry"], 9);
    }

    #[test]
    fn core_dispatch_and_session_flow() {
        let (core, mut peer, got) = mkcore();

        // session.new answers locally; nothing goes on the wire.
        core.dispatch(1, "session.new", json!({})).unwrap();
        let r = response_for(&got, 1).expect("session.new response");
        assert!(r.contains("arora-bidi-1"), "{r}");

        // Unknown method -> InvalidArgument, no wire traffic.
        assert!(core.dispatch(2, "bogus.method", json!({})).is_err());
        assert!(response_for(&got, 2).is_none());

        // subscribe registers group prefixes + drives discoverTargets.
        core.dispatch(
            3,
            "session.subscribe",
            json!({"events": ["network.beforeRequestSent", "log", "browsingContext.contextCreated"]}),
        )
        .unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Target.setDiscoverTargets");
        assert!(response_for(&got, 3).is_some());
        {
            let st = core.state.lock().unwrap();
            assert!(st.subscriptions.contains("network"));
            assert!(st.subscriptions.contains("log"));
            assert!(st.subscriptions.contains("browsingContext"));
        }

        // A second subscribe with an attached session re-runs the
        // domain enables on it.
        core.state
            .lock()
            .unwrap()
            .sessions
            .insert("CTX".into(), "S1".into());
        core.dispatch(4, "session.subscribe", json!({"events": ["network"]}))
            .unwrap();
        // Wire frames: setDiscoverTargets + one enable per subscribed
        // domain group (network, log, browsingContext->Page).
        let mut methods = Vec::new();
        for _ in 0..5 {
            methods.push(
                read_wire(&mut peer)["method"].as_str().unwrap().to_string(),
            );
        }
        for want in [
            "Target.setDiscoverTargets",
            "Network.enable",
            "Log.enable",
            "Runtime.enable",
            // DEVT07: the browsingContext group also arms Page for the
            // footer's DOMContentLoaded/Load timings.
            "Page.enable",
        ] {
            assert!(methods.iter().any(|m| m == want), "{methods:?}");
        }

        // getTree: CDP call goes out; the translated answer lands when
        // the reply is fed back through on_message.
        core.dispatch(5, "browsingContext.getTree", json!({}))
            .unwrap();
        let req = read_wire(&mut peer);
        assert_eq!(req["method"], "Target.getTargets");
        core.on_message(&json!({"id": req["id"], "result": {"targetInfos": [
            {"targetId": "P1", "type": "page", "url": "http://a/", "title": "A"},
            {"targetId": "W1", "type": "worker", "url": "http://w/"}
        ]}}));
        let r = response_for(&got, 5).expect("tree response");
        assert!(r.contains("P1") && !r.contains("W1"), "{r}");

        // navigate on a session-less context: attach first, then the
        // command rides the new session.  A second command for the same
        // context while the attach is in flight queues behind it.
        core.dispatch(
            6,
            "browsingContext.navigate",
            json!({"context": "NAV", "url": "http://x/"}),
        )
        .unwrap();
        core.dispatch(
            7,
            "script.evaluate",
            json!({"expression": "2", "target": {"context": "NAV"}, "awaitPromise": true}),
        )
        .unwrap();
        let attach = read_wire(&mut peer);
        assert_eq!(attach["method"], "Target.attachToTarget");
        assert_eq!(attach["params"]["targetId"], "NAV");
        core.on_message(
            &json!({"id": attach["id"], "result": {"sessionId": "SNAV"}}),
        );
        // On attach the session gains domain enables first (subscribed),
        // then the two queued commands in order.
        let mut seen = Vec::new();
        for _ in 0..6 {
            seen.push(read_wire(&mut peer));
        }
        let methods: Vec<&str> = seen
            .iter()
            .filter_map(|m| m["method"].as_str())
            .collect();
        assert!(
            methods.contains(&"Network.enable")
                && methods.contains(&"Log.enable")
                && methods.contains(&"Runtime.enable")
                && methods.contains(&"Page.enable"),
            "{methods:?}"
        );
        let nav = seen
            .iter()
            .find(|m| m["method"] == "Page.navigate")
            .expect("Page.navigate on wire");
        assert_eq!(nav["params"]["url"], "http://x/");
        assert_eq!(nav["sessionId"], "SNAV");
        let eval = seen
            .iter()
            .find(|m| m["method"] == "Runtime.evaluate")
            .expect("Runtime.evaluate on wire");
        assert_eq!(eval["params"]["expression"], "2");
        assert_eq!(eval["params"]["returnByValue"], true);
        assert_eq!(eval["params"]["awaitPromise"], true);
        core.on_message(&json!({"id": nav["id"], "result": {"frameId": "F1"}}));
        core.on_message(&json!({"id": eval["id"], "result": {"result": {"type": "number", "value": 2}}}));
        let r = response_for(&got, 6).expect("nav response");
        assert!(r.contains("F1"), "{r}");
        let r = response_for(&got, 7).expect("eval response");
        assert!(r.contains("\"type\":\"number\"") && r.contains("\"value\":2"), "{r}");

        // Session now exists — a repeat navigate goes straight out.
        core.dispatch(
            8,
            "browsingContext.navigate",
            json!({"context": "NAV", "url": "http://y/"}),
        )
        .unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Page.navigate");
        assert_eq!(m["sessionId"], "SNAV");

        // network.setCacheBehavior with no contexts is a GLOBAL
        // toggle — one Network.setCacheDisabled per live session
        // (CTX/S1 + NAV/SNAV here), remembered for late attaches.
        core.dispatch(
            110,
            "network.setCacheBehavior",
            json!({"cacheBehavior": "bypass"}),
        )
        .unwrap();
        let mut cache_targets = Vec::new();
        for _ in 0..2 {
            let m = read_wire(&mut peer);
            assert_eq!(m["method"], "Network.setCacheDisabled");
            assert_eq!(m["params"]["cacheDisabled"], true);
            cache_targets.push(m["sessionId"].as_str().unwrap().to_string());
        }
        cache_targets.sort();
        assert_eq!(cache_targets, ["S1", "SNAV"], "{cache_targets:?}");
        let r = response_for(&got, 110).expect("cache response");
        assert!(r.contains("result"), "{r}");

        // A late attach inherits the disabled cache right after its
        // domain enables, before the queued command.
        core.dispatch(
            111,
            "script.evaluate",
            json!({"expression": "1", "target": {"context": "LATE"}}),
        )
        .unwrap();
        let attach = read_wire(&mut peer);
        assert_eq!(attach["method"], "Target.attachToTarget");
        core.on_message(
            &json!({"id": attach["id"], "result": {"sessionId": "SLATE"}}),
        );
        let mut cache_off = false;
        let mut saw_eval = false;
        for _ in 0..6 {
            let m = read_wire(&mut peer);
            if m["method"] == "Network.setCacheDisabled" {
                assert_eq!(m["sessionId"], "SLATE");
                assert_eq!(m["params"]["cacheDisabled"], true);
                cache_off = true;
            }
            if m["method"] == "Runtime.evaluate" {
                saw_eval = true;
            }
        }
        assert!(cache_off && saw_eval, "late attach re-applies cacheDisabled");

        // Named contexts scope the toggle to their session only and
        // leave the global flag alone.
        core.dispatch(
            112,
            "network.setCacheBehavior",
            json!({"contexts": ["LATE"], "cacheBehavior": "default"}),
        )
        .unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Network.setCacheDisabled");
        assert_eq!(m["sessionId"], "SLATE");
        assert_eq!(m["params"]["cacheDisabled"], false);
        assert!(response_for(&got, 112).is_some());
        assert!(core.state.lock().unwrap().cache_disabled);

        // Global "default" clears the flag and touches every session.
        core.dispatch(
            113,
            "network.setCacheBehavior",
            json!({"cacheBehavior": "default"}),
        )
        .unwrap();
        let mut cache_targets = Vec::new();
        for _ in 0..3 {
            let m = read_wire(&mut peer);
            assert_eq!(m["method"], "Network.setCacheDisabled");
            assert_eq!(m["params"]["cacheDisabled"], false);
            cache_targets.push(m["sessionId"].as_str().unwrap().to_string());
        }
        cache_targets.sort();
        assert_eq!(cache_targets, ["S1", "SLATE", "SNAV"], "{cache_targets:?}");
        assert!(!core.state.lock().unwrap().cache_disabled);

        // getCookies honours an explicit urls list.
        core.dispatch(
            9,
            "storage.getCookies",
            json!({"context": "NAV", "urls": ["http://x/"]}),
        )
        .unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Network.getCookies");
        assert_eq!(m["params"]["urls"], json!(["http://x/"]));
        core.on_message(&json!({"id": m["id"], "result": {"cookies": [
            {"name": "a", "value": "b", "domain": "d", "path": "/",
             "secure": true, "httpOnly": false, "expires": 9}
        ]}}));
        let r = response_for(&got, 9).expect("cookies response");
        assert!(r.contains("\"name\":\"a\"") && r.contains("\"expiry\":9"), "{r}");

        // No urls and no context: the context table's URL scopes it.
        core.on_message(&json!({
            "method": "Target.targetCreated",
            "params": {"targetInfo": {"type": "page", "targetId": "NAV",
                "url": "http://nav/", "title": "N"}}
        }));
        core.dispatch(10, "storage.getCookies", json!({})).unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Network.getCookies");
        assert_eq!(m["params"]["urls"], json!(["http://nav/"]));
        core.on_message(&json!({"id": m["id"], "result": {"cookies": []}}));

        // context given explicitly via params.context also resolves.
        // (params.context variant for script.evaluate's fallback arm)
        core.dispatch(
            11,
            "script.evaluate",
            json!({"expression": "1", "context": "NAV"}),
        )
        .unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Runtime.evaluate");

        // close -> Target.closeTarget, Raw answer passthrough.
        core.dispatch(12, "browsingContext.close", json!({"context": "NAV"}))
            .unwrap();
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Target.closeTarget");
        assert_eq!(m["params"]["targetId"], "NAV");
        core.on_message(&json!({"id": m["id"], "result": {"success": true}}));
        let r = response_for(&got, 12).expect("close response");
        assert!(r.contains("success"), "{r}");

        // A CDP error against an Answer action is forwarded verbatim.
        core.dispatch(13, "browsingContext.getTree", json!({}))
            .unwrap();
        let m = read_wire(&mut peer);
        core.on_message(&json!({"id": m["id"], "error": {"message": "nope"}}));
        let r = response_for(&got, 13).expect("err response");
        assert!(r.contains("nope"), "{r}");

        // A failed attach answers every queued follow-up with the error.
        for id in [14u64, 15] {
            core.dispatch(
                id,
                "script.evaluate",
                json!({"expression": "1", "target": {"context": "FAIL"}}),
            )
            .unwrap();
        }
        let m = read_wire(&mut peer);
        assert_eq!(m["method"], "Target.attachToTarget");
        core.on_message(
            &json!({"id": m["id"], "error": {"message": "attach denied"}}),
        );
        for id in [14u64, 15] {
            let r = response_for(&got, id).expect("attach-err response");
            assert!(r.contains("attach denied"), "{r}");
        }

        // Unknown / id-less replies are ignored.
        core.on_message(&json!({"id": 424242, "result": {}}));
        core.on_message(&json!({"result": {}}));

        // session.end flips the liveness flag and still answers.
        core.dispatch(16, "session.end", json!({})).unwrap();
        assert!(!core.alive.load(Ordering::SeqCst));
        assert!(response_for(&got, 16).is_some());
    }

    #[test]
    fn core_event_translations() {
        let (core, mut peer, got) = mkcore();
        core.dispatch(
            1,
            "session.subscribe",
            json!({"events": ["browsingContext", "network", "log"]}),
        )
        .unwrap();
        let _ = read_wire(&mut peer); // Target.setDiscoverTargets

        // targetCreated: page -> contextCreated; other types skipped.
        core.on_message(&json!({
            "method": "Target.targetCreated",
            "params": {"targetInfo": {"type": "page", "targetId": "T1",
                "url": "http://t/", "title": "T"}}
        }));
        let e = events_named(&got, "browsingContext.contextCreated");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("\"context\":\"T1\""), "{e:?}");
        core.on_message(&json!({
            "method": "Target.targetCreated",
            "params": {"targetInfo": {"type": "worker", "targetId": "W9"}}
        }));
        assert_eq!(events_named(&got, "browsingContext.contextCreated").len(), 1);

        // targetInfoChanged emits navigationStarted once per url change.
        core.on_message(&json!({
            "method": "Target.targetInfoChanged",
            "params": {"targetInfo": {"type": "page", "targetId": "T1",
                "url": "http://t2/"}}
        }));
        let e = events_named(&got, "browsingContext.navigationStarted");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("http://t2/"), "{e:?}");
        core.on_message(&json!({
            "method": "Target.targetInfoChanged",
            "params": {"targetInfo": {"type": "page", "targetId": "T1",
                "url": "http://t2/"}}
        }));
        core.on_message(&json!({
            "method": "Target.targetInfoChanged",
            "params": {"targetInfo": {"type": "worker", "targetId": "T1",
                "url": "http://t3/"}}
        }));
        assert_eq!(events_named(&got, "browsingContext.navigationStarted").len(), 1);

        core.on_message(&json!({
            "method": "Target.targetDestroyed",
            "params": {"targetId": "T1"}
        }));
        let e = events_named(&got, "browsingContext.contextDestroyed");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("T1"), "{e:?}");

        // Network.* — context resolves through the session map; an
        // unmapped session serializes as null rather than guessing.
        core.state
            .lock()
            .unwrap()
            .sessions
            .insert("T2".into(), "S2".into());
        core.on_message(&json!({
            "method": "Network.requestWillBeSent", "sessionId": "S2",
            "params": {"requestId": "r1", "timestamp": 12.5, "type": "Document",
                "initiator": {"type": "other"},
                "request": {"url": "http://r/", "method": "GET"}}
        }));
        let e = events_named(&got, "network.beforeRequestSent");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("\"context\":\"T2\"") && e[0].contains("http://r/"), "{e:?}");
        // DEVT06: the CDP extras the Chrome-style table needs ride along —
        // monotonic timestamp in ms, resource type, initiator.
        assert!(e[0].contains("\"timestamp\":12500.0")
            && e[0].contains("\"type\":\"Document\"")
            && e[0].contains("\"initiator\":{\"type\":\"other\"}"), "{e:?}");
        core.on_message(&json!({
            "method": "Network.responseReceived", "sessionId": "S2",
            "params": {"requestId": "r1", "timestamp": 12.6, "type": "Document",
                "response": {"url": "http://r/", "status": 200,
                    "statusText": "OK", "mimeType": "text/html",
                    "encodedDataLength": 64, "fromDiskCache": true,
                    "timing": {"receiveHeadersEnd": 18.0}}}
        }));
        let e = events_named(&got, "network.responseStarted");
        assert!(e[0].contains("\"status\":200"), "{e:?}");
        assert!(e[0].contains("\"timestamp\":12600.0")
            && e[0].contains("\"mimeType\":\"text/html\"")
            && e[0].contains("\"type\":\"Document\""), "{e:?}");
        // DEVT07: cache provenance + raw timing ride through for the
        // Size column and the two-tone waterfall bar.
        assert!(e[0].contains("\"fromDiskCache\":true")
            && e[0].contains("\"receiveHeadersEnd\":18.0"), "{e:?}");

        // DEVT07: memory-cache hits and page-load milestones translate.
        core.on_message(&json!({
            "method": "Network.requestServedFromCache", "sessionId": "S2",
            "params": {"requestId": "r1"}
        }));
        let e = events_named(&got, "network.requestServedFromCache");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("\"context\":\"T2\"")
            && e[0].contains("\"request\":\"r1\""), "{e:?}");
        core.on_message(&json!({
            "method": "Page.domContentEventFired", "sessionId": "S2",
            "params": {"timestamp": 12.7}
        }));
        let e = events_named(&got, "browsingContext.domContentLoaded");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("\"context\":\"T2\"")
            && e[0].contains("\"timestamp\":12700.0"), "{e:?}");
        core.on_message(&json!({
            "method": "Page.loadEventFired", "sessionId": "S2",
            "params": {"timestamp": 12.95}
        }));
        let e = events_named(&got, "browsingContext.load");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("\"timestamp\":12950.0"), "{e:?}");
        core.on_message(&json!({
            "method": "Network.loadingFinished", "sessionId": "S2",
            "params": {"requestId": "r1", "timestamp": 12.9,
                "encodedDataLength": 4096}
        }));
        let e = events_named(&got, "network.responseCompleted");
        assert_eq!(e.len(), 1);
        assert!(e[0].contains("\"timestamp\":12900.0")
            && e[0].contains("\"encodedDataLength\":4096"), "{e:?}");
        core.on_message(&json!({
            "method": "Network.loadingFailed", "sessionId": "S2",
            "params": {"requestId": "r2", "errorText": "net::ERR_X",
                "timestamp": 13.0, "type": "XHR"}
        }));
        let e = events_named(&got, "network.fetchError");
        assert!(e[0].contains("net::ERR_X"), "{e:?}");
        assert!(e[0].contains("\"timestamp\":13000.0")
            && e[0].contains("\"type\":\"XHR\""), "{e:?}");
        core.on_message(&json!({
            "method": "Network.requestWillBeSent", "sessionId": "NOPE",
            "params": {"requestId": "r9", "request": {"url": "u", "method": "GET"}}
        }));
        let e = events_named(&got, "network.beforeRequestSent");
        assert!(e[1].contains("\"context\":null"), "{e:?}");

        // DEVT07: decoded body bytes accumulate bridge-side per
        // requestId and ride the completion once — the per-chunk
        // dataReceived events never reach the panel.
        core.on_message(&json!({
            "method": "Network.dataReceived", "sessionId": "S2",
            "params": {"requestId": "r1", "dataLength": 3000,
                "encodedDataLength": 1900}
        }));
        core.on_message(&json!({
            "method": "Network.dataReceived", "sessionId": "S2",
            "params": {"requestId": "r1", "dataLength": 2500}
        }));
        assert!(events_named(&got, "network.dataReceived").is_empty());
        core.on_message(&json!({
            "method": "Network.loadingFinished", "sessionId": "S2",
            "params": {"requestId": "r1", "timestamp": 13.1,
                "encodedDataLength": 4096}
        }));
        let e = events_named(&got, "network.responseCompleted");
        assert!(e[1].contains("\"decodedDataLength\":5500"), "{e:?}");
        // A request with no chunks reports null, and a failed request
        // drops its accumulated bytes.
        core.on_message(&json!({
            "method": "Network.dataReceived", "sessionId": "S2",
            "params": {"requestId": "r2", "dataLength": 999}
        }));
        core.on_message(&json!({
            "method": "Network.loadingFailed", "sessionId": "S2",
            "params": {"requestId": "r2", "errorText": "x", "timestamp": 13.2}
        }));
        core.on_message(&json!({
            "method": "Network.loadingFinished", "sessionId": "S2",
            "params": {"requestId": "r2", "timestamp": 13.3}
        }));
        let e = events_named(&got, "network.responseCompleted");
        assert!(e[2].contains("\"decodedDataLength\":null"), "{e:?}");

        // Page lifecycle timings ride the browsingContext group on the
        // same monotonic clock as the network events.  The session
        // already emitted one of each above — the count is cumulative
        // and the fresh event is the last one.
        core.on_message(&json!({
            "method": "Page.domContentEventFired", "sessionId": "S2",
            "params": {"timestamp": 12.8}
        }));
        let e = events_named(&got, "browsingContext.domContentLoaded");
        assert_eq!(e.len(), 2);
        assert!(e[1].contains("\"timestamp\":12800.0")
            && e[1].contains("\"context\":\"T2\""), "{e:?}");
        core.on_message(&json!({
            "method": "Page.loadEventFired", "sessionId": "S2",
            "params": {"timestamp": 13.4}
        }));
        let e = events_named(&got, "browsingContext.load");
        assert_eq!(e.len(), 2);
        assert!(e[1].contains("\"timestamp\":13400.0"), "{e:?}");

        // Cache flags ride responseStarted so the Size column can mark
        // cache-served rows honestly.
        core.on_message(&json!({
            "method": "Network.responseReceived", "sessionId": "S2",
            "params": {"requestId": "r1c", "timestamp": 13.5, "type": "Image",
                "response": {"url": "http://c/p.png", "status": 200,
                    "fromDiskCache": true}}
        }));
        let e = events_named(&got, "network.responseStarted");
        assert!(e.last().unwrap().contains("\"fromDiskCache\":true"), "{e:?}");

        // console args prefer .value, then .description, then the type
        // name; non-string values serialize.
        core.on_message(&json!({
            "method": "Runtime.consoleAPICalled", "sessionId": "S2",
            "params": {"type": "warn", "args": [
                {"value": "hi"}, {"value": 42},
                {"description": "desc"}, {"type": "object"}
            ]}
        }));
        let e = events_named(&got, "log.entryAdded");
        let console = e.iter().find(|p| p.contains("console")).unwrap();
        assert!(console.contains("\"level\":\"warn\""), "{console}");
        assert!(console.contains("hi 42 desc object"), "{console}");

        // Log.entryAdded and exceptionThrown share the javascript lane.
        core.on_message(&json!({
            "method": "Log.entryAdded", "sessionId": "S2",
            "params": {"entry": {"level": "error", "text": "logline"}}
        }));
        core.on_message(&json!({
            "method": "Runtime.exceptionThrown", "sessionId": "S2",
            "params": {"exceptionDetails": {"text": "kaboom"}}
        }));
        core.on_message(&json!({
            "method": "Runtime.exceptionThrown", "sessionId": "S2",
            "params": {"exceptionDetails": {"exception": {"description": "nested"}}}
        }));
        let e = events_named(&got, "log.entryAdded");
        assert!(e.iter().any(|p| p.contains("logline") && p.contains("javascript")), "{e:?}");
        assert!(e.iter().any(|p| p.contains("kaboom")), "{e:?}");
        assert!(e.iter().any(|p| p.contains("nested")), "{e:?}");

        // Unknown CDP methods are dropped silently.
        let before = got.lock().unwrap().len();
        core.on_message(&json!({"method": "Page.frameNavigated", "params": {}}));
        assert_eq!(got.lock().unwrap().len(), before);
    }

    #[test]
    fn events_require_subscription() {
        let (core, _peer, got) = mkcore();
        // Nothing subscribed — every event arm early-outs.
        for msg in [
            json!({"method": "Target.targetCreated",
                "params": {"targetInfo": {"type": "page", "targetId": "T"}}}),
            json!({"method": "Target.targetDestroyed", "params": {"targetId": "T"}}),
            json!({"method": "Target.targetInfoChanged",
                "params": {"targetInfo": {"type": "page", "targetId": "T", "url": "u"}}}),
            json!({"method": "Network.requestWillBeSent", "sessionId": "S",
                "params": {"requestId": "r", "request": {}}}),
            json!({"method": "Network.responseReceived", "sessionId": "S",
                "params": {"requestId": "r", "response": {}}}),
            json!({"method": "Network.loadingFinished", "sessionId": "S",
                "params": {"requestId": "r"}}),
            json!({"method": "Network.loadingFailed", "sessionId": "S",
                "params": {"requestId": "r"}}),
            json!({"method": "Runtime.consoleAPICalled", "sessionId": "S",
                "params": {"args": []}}),
            json!({"method": "Log.entryAdded", "sessionId": "S",
                "params": {"entry": {}}}),
            json!({"method": "Runtime.exceptionThrown", "sessionId": "S",
                "params": {"exceptionDetails": {}}}),
            json!({"method": "Network.requestServedFromCache", "sessionId": "S",
                "params": {"requestId": "r"}}),
            json!({"method": "Page.domContentEventFired", "sessionId": "S",
                "params": {"timestamp": 1.0}}),
            json!({"method": "Page.loadEventFired", "sessionId": "S",
                "params": {"timestamp": 1.0}}),
        ] {
            core.on_message(&msg);
        }
        let g = got.lock().unwrap();
        assert!(!g.iter().any(|(k, _, _)| k == "event"), "{g:?}");
    }

    #[test]
    fn getcookies_without_context_errors() {
        let (core, _peer, got) = mkcore();
        core.dispatch(1, "storage.getCookies", json!({})).unwrap();
        let r = response_for(&got, 1).expect("error response");
        assert!(r.contains("no browsing context"), "{r}");
    }

    /* -------- ws codec over a socketpair -------- */

    /// Build an unmasked server->client frame.
    fn srv_frame(opcode: u8, payload: &[u8]) -> Vec<u8> {
        let mut f = vec![0x80 | opcode];
        let n = payload.len();
        if n < 126 {
            f.push(n as u8);
        } else if n < 65536 {
            f.push(126);
            f.extend_from_slice(&(n as u16).to_be_bytes());
        } else {
            f.push(127);
            f.extend_from_slice(&(n as u64).to_be_bytes());
        }
        f.extend_from_slice(payload);
        f
    }

    /// Read one frame on the peer end: returns (fin|opcode, unmasked
    /// payload).
    fn peer_frame(s: &mut TcpStream) -> (u8, Vec<u8>) {
        s.set_read_timeout(Some(std::time::Duration::from_secs(5)))
            .ok();
        let mut hdr = [0u8; 2];
        s.read_exact(&mut hdr).unwrap();
        let mut len = (hdr[1] & 0x7f) as usize;
        if len == 126 {
            let mut e = [0u8; 2];
            s.read_exact(&mut e).unwrap();
            len = u16::from_be_bytes(e) as usize;
        } else if len == 127 {
            let mut e = [0u8; 8];
            s.read_exact(&mut e).unwrap();
            len = u64::from_be_bytes(e) as usize;
        }
        let mut payload = vec![0u8; len];
        if hdr[1] & 0x80 != 0 {
            let mut mask = [0u8; 4];
            s.read_exact(&mut mask).unwrap();
            s.read_exact(&mut payload).unwrap();
            for i in 0..len {
                payload[i] ^= mask[i % 4];
            }
        } else {
            s.read_exact(&mut payload).unwrap();
        }
        (hdr[0], payload)
    }

    #[test]
    fn ws_send_text_frame_boundaries() {
        let (mut a, mut b) = sockpair();
        for n in [5usize, 125, 126, 127, 65535, 65536] {
            let payload = vec![b'x'; n];
            ws::send_text(&mut a, &payload).unwrap();
            let (head, got) = peer_frame(&mut b);
            assert_eq!(head, 0x81);
            assert_eq!(got, payload, "len {n}");
        }
    }

    #[test]
    fn ws_recv_text_opcodes_and_lengths() {
        let (a, mut b) = sockpair();
        let mut reader = a.try_clone().unwrap();
        let writer = Mutex::new(a);
        let mut data = Vec::new();

        // Plain text frame, then two coalesced frames in one write —
        // the persistent `data` buffer must carry the second across
        // calls.
        b.write_all(&srv_frame(1, b"hello")).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Text(s) => assert_eq!(s, "hello"),
            _ => panic!("expected text"),
        }
        b.write_all(&srv_frame(1, b"one"))
            .unwrap();
        b.write_all(&srv_frame(1, b"two")).unwrap();
        // Both may already sit in `data` — either way each call yields
        // one message in order.
        for want in ["one", "two"] {
            match ws::recv_text(&mut reader, &writer, &mut data) {
                ws::Incoming::Text(s) => assert_eq!(s, want),
                _ => panic!("expected {want}"),
            }
        }

        // 16-bit and 64-bit length encodings.
        let big = vec![b'y'; 200];
        b.write_all(&srv_frame(1, &big)).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Text(s) => assert_eq!(s.len(), 200),
            _ => panic!("expected 200-byte text"),
        }
        let huge = vec![b'z'; 66000];
        b.write_all(&srv_frame(1, &huge)).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Text(s) => assert_eq!(s.len(), 66000),
            _ => panic!("expected 66000-byte text"),
        }

        // Continuation frame is accepted as message text.
        b.write_all(&srv_frame(0, b"cont")).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Text(s) => assert_eq!(s, "cont"),
            _ => panic!("expected continuation text"),
        }

        // Ping is answered with a masked pong on the writer, then the
        // loop continues to the next real message.
        b.write_all(&srv_frame(9, b"pp")).unwrap();
        b.write_all(&srv_frame(1, b"after-ping")).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Text(s) => assert_eq!(s, "after-ping"),
            _ => panic!("expected post-ping text"),
        }
        let (head, payload) = peer_frame(&mut b);
        assert_eq!(head, 0x8a, "pong opcode");
        assert_eq!(payload, b"pp");

        // An unknown opcode is skipped, not fatal.
        b.write_all(&srv_frame(3, b"??")).unwrap();
        b.write_all(&srv_frame(1, b"ok")).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Text(s) => assert_eq!(s, "ok"),
            _ => panic!("expected text after unknown opcode"),
        }

        // Invalid UTF-8 payload -> Closed.
        b.write_all(&srv_frame(1, &[0xff, 0xfe])).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Closed => {}
            _ => panic!("invalid utf8 must close"),
        }

        // Close frame -> Closed.
        b.write_all(&srv_frame(8, b"")).unwrap();
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Closed => {}
            _ => panic!("close frame must yield Closed"),
        }

        // EOF -> Closed.
        drop(b);
        match ws::recv_text(&mut reader, &writer, &mut data) {
            ws::Incoming::Closed => {}
            _ => panic!("EOF must yield Closed"),
        }
    }

    /* -------- HTTP discovery + handshake edges -------- */

    /// One-shot HTTP responder for /json/version probes.
    fn http_once(reply: &'static [u8]) -> u16 {
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        thread::spawn(move || {
            if let Ok((mut s, _)) = l.accept() {
                let mut buf = Vec::new();
                let mut byte = [0u8; 1];
                while !buf.ends_with(b"\r\n\r\n") {
                    if s.read(&mut byte).unwrap_or(0) == 0 {
                        return;
                    }
                    buf.push(byte[0]);
                }
                let _ = s.write_all(reply);
            }
        });
        port
    }

    #[test]
    fn discover_path_variants() {
        let body = r#"{"webSocketDebuggerUrl":"ws://127.0.0.1:9/devtools/browser/GUID-1"}"#;
        let reply = format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
        let port = http_once(Box::leak(reply.into_bytes().into_boxed_slice()));
        let path = ws::discover_path("127.0.0.1", port).unwrap();
        assert_eq!(path, "/devtools/browser/GUID-1");

        // Missing the field is an Io failure.
        let body = r#"{"Browser":"x"}"#;
        let reply = format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
        let port = http_once(Box::leak(reply.into_bytes().into_boxed_slice()));
        assert!(ws::discover_path("127.0.0.1", port).is_err());

        // Malformed JSON is Corrupt.
        let body = "not json";
        let reply = format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {}\r\n\r\n{}",
            body.len(),
            body
        );
        let port = http_once(Box::leak(reply.into_bytes().into_boxed_slice()));
        assert!(ws::discover_path("127.0.0.1", port).is_err());

        // Connection refused — nothing listening.
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        drop(l);
        assert!(ws::discover_path("127.0.0.1", port).is_err());
    }

    #[test]
    fn handshake_rejection_paths() {
        // Non-101 status -> rejected with the status line in the error.
        let port = http_once(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
        let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        let r = ws::handshake(&mut s, "127.0.0.1", port, "/devtools/browser/x", "o");
        assert!(r.is_err());
        assert!(r.unwrap_err().msg.contains("403"));

        // Peer closes mid-handshake -> EOF error.
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        thread::spawn(move || {
            if let Ok((s, _)) = l.accept() {
                drop(s);
            }
        });
        let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        assert!(ws::handshake(&mut s, "127.0.0.1", port, "/x", "o").is_err());

        // Header flood without a terminator trips the size cap.
        let l = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = l.local_addr().unwrap().port();
        thread::spawn(move || {
            if let Ok((mut s, _)) = l.accept() {
                let mut buf = Vec::new();
                let mut byte = [0u8; 1];
                while !buf.ends_with(b"\r\n\r\n") {
                    if s.read(&mut byte).unwrap_or(0) == 0 {
                        return;
                    }
                    buf.push(byte[0]);
                }
                let _ = s.write_all(&vec![b'A'; 20000]);
                // keep the socket open until the client gives up
                std::thread::sleep(std::time::Duration::from_millis(500));
            }
        });
        let mut s = TcpStream::connect(("127.0.0.1", port)).unwrap();
        assert!(ws::handshake(&mut s, "127.0.0.1", port, "/x", "o").is_err());
    }

    /* -------- FFI boundary -------- */

    static FFI_HITS: AtomicUsize = AtomicUsize::new(0);
    static FFI_LAST_ID: std::sync::atomic::AtomicU64 =
        std::sync::atomic::AtomicU64::new(0);

    unsafe extern "C" fn ffi_cb(
        userdata: *mut c_void,
        kind: *const c_char,
        id: u64,
        _json: *const c_char,
    ) {
        assert_eq!(userdata as usize, 0xb1d1);
        assert!(!kind.is_null());
        FFI_HITS.fetch_add(1, Ordering::SeqCst);
        FFI_LAST_ID.store(id, Ordering::SeqCst);
    }

    #[test]
    fn ffi_bad_arguments_and_refused_connect() {
        unsafe {
            assert_eq!(rc_bidi_command(ptr::null_mut(), c"m".as_ptr(), ptr::null()), 0);
            assert_eq!(
                rc_bidi_command(ptr::null_mut(), ptr::null(), ptr::null()),
                0
            );
            assert_eq!(rc_bidi_is_alive(ptr::null()), 0);
            rc_bidi_free(ptr::null_mut()); // no-op, must not crash

            // Null host/path/origin all fail to NULL with the error set.
            assert!(rc_bidi_connect(
                ptr::null(),
                1,
                c"/x".as_ptr(),
                c"o".as_ptr(),
                ffi_cb,
                ptr::null_mut()
            )
            .is_null());
            assert!(rc_bidi_connect(
                c"127.0.0.1".as_ptr(),
                1,
                ptr::null(),
                c"o".as_ptr(),
                ffi_cb,
                ptr::null_mut()
            )
            .is_null());

            // A closed port fails the connect.
            let l = TcpListener::bind("127.0.0.1:0").unwrap();
            let port = l.local_addr().unwrap().port();
            drop(l);
            assert!(rc_bidi_connect(
                c"127.0.0.1".as_ptr(),
                port,
                c"/x".as_ptr(),
                c"o".as_ptr(),
                ffi_cb,
                ptr::null_mut()
            )
            .is_null());
        }
    }

    #[test]
    fn ffi_end_to_end() {
        let scripted = Arc::new(Mutex::new(Vec::new()));
        let server = FakeServer::start(scripted);
        unsafe {
            let client = rc_bidi_connect(
                c"127.0.0.1".as_ptr(),
                server.port,
                c"/devtools/browser/fake".as_ptr(),
                c"http://bidi.test.invalid".as_ptr(),
                ffi_cb,
                0xb1d1usize as *mut c_void,
            );
            assert!(!client.is_null());
            assert_eq!(rc_bidi_is_alive(client), 1);
            let before = FFI_HITS.load(Ordering::SeqCst);
            let id = rc_bidi_command(client, c"session.new".as_ptr(), ptr::null());
            assert!(id > 0);
            let t0 = std::time::Instant::now();
            while FFI_HITS.load(Ordering::SeqCst) == before {
                if t0.elapsed() > std::time::Duration::from_secs(3) {
                    panic!("no FFI response callback");
                }
                thread::sleep(std::time::Duration::from_millis(10));
            }
            assert_eq!(FFI_LAST_ID.load(Ordering::SeqCst), id);
            rc_bidi_free(client);
        }
        drop(server);
    }

    #[test]
    fn command_after_outbox_closed_answers_error() {
        let scripted = Arc::new(Mutex::new(Vec::new()));
        let server = FakeServer::start(scripted);
        let got: Arc<Mutex<Vec<(String, u64, String)>>> = Arc::new(Mutex::new(Vec::new()));
        let got2 = got.clone();
        let client = Client::connect(
            "127.0.0.1",
            server.port,
            "/devtools/browser/fake",
            "http://bidi.test.invalid",
        )
        .expect("connect");
        client.set_sink(Box::new(move |k, id, p| {
            got2.lock().unwrap().push((k.to_string(), id, p.to_string()));
        }));
        // Drop the sender — command() falls to the answer_err arm.
        client.outbox.lock().unwrap().take();
        let id = client.command("session.new", "{}");
        assert!(id > 0);
        {
            let g = got.lock().unwrap();
            assert!(g.iter().any(|(k, i, p)| k == "response"
                && *i == id
                && p.contains("channel closed")), "{g:?}");
        }
        // Guard must be released first: drop() joins the reader thread
        // and its final "state:closed" emit needs the same mutex.
        drop(client);
        drop(server);
    }
}
