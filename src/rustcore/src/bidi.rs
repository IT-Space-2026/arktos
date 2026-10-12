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
//!   Events out: browsingContext.*, log.entryAdded (console+log),
//!     network.beforeRequestSent / responseStarted / responseCompleted
//!     / fetchError
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
                        "request": {
                            "request": params["requestId"],
                            "url": req["url"],
                            "method": req["method"],
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
                        "request": { "request": params["requestId"], "url": resp["url"] },
                        "response": { "url": resp["url"], "status": resp["status"] },
                    }),
                );
            }
            "Network.loadingFinished" => {
                if !subscribed("network") {
                    return;
                }
                self.event(
                    "network.responseCompleted",
                    json!({
                        "context": self.context_for_session(session),
                        "request": { "request": params["requestId"] },
                        "response": {},
                    }),
                );
            }
            "Network.loadingFailed" => {
                if !subscribed("network") {
                    return;
                }
                self.event(
                    "network.fetchError",
                    json!({
                        "context": self.context_for_session(session),
                        "request": { "request": params["requestId"] },
                        "errorText": params["errorText"],
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
}
