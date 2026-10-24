//! torctl (RC14): Tor control-protocol client — the memory-safe Rust
//! port of src/tor/torcontrol.cpp.  Pure socket protocol, zero profile
//! data: it talks the control-spec line protocol over a loopback TCP
//! connection and never touches a store, so it is deliberately NOT a
//! facade module (no profile binding, no data dir).
//!
//! Wire contract (control-spec.txt): commands go out CRLF-terminated,
//! replies come back as
//!   <code>-<text>   mid-reply line
//!   <code>+<text>   data block follows, terminated by a lone "."
//!   <code> <text>   final reply line
//! and 650 lines are asynchronous events that may interleave with a
//! reply in progress — they are collected verbatim (code stripped)
//! for the Qt side's asyncEvent equivalent.
//!
//! Ported semantics that consumers rely on:
//!   * commands are serialized — at most one in flight, the rest
//!     FIFO-queued, exactly like the C++ sendNext() gate;
//!   * every submitted command completes exactly once: the final
//!     status code (250 / 4xx / 5xx) plus the collected reply lines,
//!     or code -1 with the partial lines when the connection dies;
//!   * closing fails every outstanding command before reporting the
//!     disconnect, so no caller hangs waiting on a dead daemon.
//!
//! No-leak rule: the control port is a trust boundary — connect is
//! loopback-only ("localhost" included, resolved and re-verified) and
//! AUTHENTICATE payloads are held in zeroizing buffers that wipe on
//! drop.  Command queue entries are zeroized the same way — a queued
//! AUTHENTICATE is exactly where the secret sits.
//!
//! Async surface: replies, events and link state are pulled
//! (take_reply/take_event/take_error/state); each push announces
//! itself through notify::emit("torctl") so the Qt bridge can queue a
//! drain onto the GUI thread.

use std::collections::VecDeque;
use std::io::{Read, Write};
use std::net::{IpAddr, Shutdown, SocketAddr, TcpStream, ToSocketAddrs};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::Duration;

use zeroize::Zeroizing;

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::notify;

/// rc_tor_state() return values — mirror the defines in rustcore.h.
pub const STATE_CONNECTED: i32 = 1;
pub const STATE_CLOSED: i32 = 2;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
/// Async-event / error queues are bounded so a chatty daemon (CIRC
/// floods) can never grow the handle without limit; oldest entries
/// drop first, matching the "advisory" weight of these lanes.
const EVENT_CAP: usize = 2048;
/// Reader chunk size — replies are line-oriented and small.
const READ_CHUNK: usize = 8192;

/// One completed command: the queue sequence number the submit call
/// returned, the final reply code (-1 when the connection died first)
/// and the collected payload lines (code prefixes stripped, data-block
/// payload appended raw — same shape the C++ callback produced).
pub struct Reply {
    pub seq: u64,
    pub code: i32,
    pub lines: Vec<String>,
}

struct Pending {
    seq: u64,
    /// The raw command bytes without the CRLF terminator.  Auth
    /// commands carry secrets — Zeroizing so a dropped/!flushed queue
    /// cannot leave the cookie or password in freed memory.
    command: Zeroizing<Vec<u8>>,
}

struct Inner {
    status: i32,
    /// Connection-level error text ("" for a clean EOF).  Distinct
    /// from `errors`, which holds protocol-level complaints about
    /// unparsable lines — the C++ errorMessage() lane.
    error: Option<String>,
    queue: VecDeque<Pending>,
    current: Option<Pending>,
    waiting: bool,
    in_data_block: bool,
    reply_lines: Vec<String>,
    completed: VecDeque<Reply>,
    events: VecDeque<String>,
    errors: VecDeque<String>,
}

struct Shared {
    state: Mutex<Inner>,
    writer: Mutex<Option<TcpStream>>,
    seq: AtomicU64,
}

fn lock(m: &Mutex<Inner>) -> MutexGuard<'_, Inner> {
    m.lock().unwrap_or_else(|e| e.into_inner())
}

/// Opaque FFI handle: one control connection plus its reader thread.
/// Replies/events/errors are pulled out of the shared state; pushes
/// into it emit the "torctl" topic.
pub struct RcTorCtl {
    inner: Arc<Shared>,
    reader: Mutex<Option<JoinHandle<()>>>,
}

/// Resolves `host` for a control connection — loopback only.  IP
/// literals must satisfy is_loopback(); "localhost" resolves and every
/// answer must be loopback so a poisoned hosts entry cannot pivot the
/// control channel off-box — the caller tries each in order.  Anything
/// else is InvalidArgument.
fn resolve_loopback(host: &str, port: u16) -> RcResult<Vec<SocketAddr>> {
    let h = host.trim();
    let h = if h.len() > 2 && h.starts_with('[') && h.ends_with(']') {
        &h[1..h.len() - 1]
    } else {
        h
    };
    if let Ok(ip) = h.parse::<IpAddr>() {
        if ip.is_loopback() {
            return Ok(vec![SocketAddr::new(ip, port)]);
        }
        return fail(
            RcStatus::InvalidArgument,
            "tor control host must be loopback",
        );
    }
    if h.eq_ignore_ascii_case("localhost") {
        let addrs: Vec<SocketAddr> = match (h, port).to_socket_addrs() {
            Ok(it) => it.collect(),
            Err(e) => {
                return fail(RcStatus::Io, format!("localhost resolve: {e}"));
            }
        };
        if !addrs.is_empty()
            && addrs.iter().all(|a| a.ip().is_loopback())
        {
            return Ok(addrs);
        }
        return fail(
            RcStatus::InvalidArgument,
            "localhost did not resolve to loopback",
        );
    }
    fail(
        RcStatus::InvalidArgument,
        "tor control host must be a loopback address",
    )
}

/// Fails every queued/in-flight command with the -1 spelling and marks
/// the link closed.  The C++ disconnected() handler did the same —
/// callbacks must fire exactly once, even on a dead daemon.
fn mark_closed(st: &mut Inner, err: Option<String>) {
    if st.status == STATE_CLOSED {
        return;
    }
    st.status = STATE_CLOSED;
    if st.error.is_none() {
        st.error = err;
    }
    st.waiting = false;
    st.in_data_block = false;
    if let Some(cur) = st.current.take() {
        let lines = std::mem::take(&mut st.reply_lines);
        st.completed.push_back(Reply {
            seq: cur.seq,
            code: -1,
            lines,
        });
    } else {
        st.reply_lines.clear();
    }
    while let Some(p) = st.queue.pop_front() {
        st.completed.push_back(Reply {
            seq: p.seq,
            code: -1,
            lines: Vec::new(),
        });
    }
}

/// Sends the next queued command when none is outstanding — the
/// sendNext() gate of the C++ port.  A write failure closes the link
/// (the failed command lands in the -1 flush like any other).
fn pump(shared: &Shared, st: &mut Inner) {
    if st.waiting || st.status != STATE_CONNECTED || st.queue.is_empty() {
        return;
    }
    let pending = st.queue.pop_front().unwrap();
    let sent = {
        let mut wr = shared
            .writer
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        match wr.as_mut() {
            Some(w) => w
                .write_all(&pending.command)
                .and_then(|_| w.write_all(b"\r\n"))
                .is_ok(),
            None => false,
        }
    };
    st.current = Some(pending);
    if sent {
        st.waiting = true;
        st.in_data_block = false;
        st.reply_lines.clear();
    } else {
        mark_closed(st, Some("control connection write failed".into()));
    }
}

fn push_bounded(q: &mut VecDeque<String>, item: String) {
    if q.len() >= EVENT_CAP {
        q.pop_front();
    }
    q.push_back(item);
}

/// Parse one CRLF-free line.  Returns true when something was pushed
/// into a drainable queue (completed reply, event, protocol error) so
/// the caller emits, mirroring torcontrol.cpp::onReadyRead branch for
/// branch: data-block lines first (a "650" inside a block is payload),
/// then async events, then coded reply lines.
fn process_line(shared: &Shared, st: &mut Inner, raw: &[u8]) -> bool {
    if st.in_data_block {
        if raw == b"." {
            st.in_data_block = false;
        } else {
            st.reply_lines
                .push(String::from_utf8_lossy(raw).into_owned());
        }
        return false;
    }
    if raw.len() >= 4 && &raw[..3] == b"650" && raw[3] == b' ' {
        push_bounded(
            &mut st.events,
            String::from_utf8_lossy(&raw[4..]).into_owned(),
        );
        return true;
    }
    let code_ok = raw.len() >= 4
        && raw[..3].iter().all(|b| b.is_ascii_digit());
    if !code_ok {
        push_bounded(
            &mut st.errors,
            format!(
                "unparsable control line: {}",
                String::from_utf8_lossy(raw)
            ),
        );
        return true;
    }
    let code = (raw[0] - b'0') as i32 * 100
        + (raw[1] - b'0') as i32 * 10
        + (raw[2] - b'0') as i32;
    let sep = raw[3];
    let text = String::from_utf8_lossy(&raw[4..]).into_owned();
    match sep {
        b'-' => st.reply_lines.push(text),
        b'+' => {
            st.reply_lines.push(text);
            st.in_data_block = true;
        }
        b' ' => {
            st.reply_lines.push(text);
            if let Some(cur) = st.current.take() {
                st.completed.push_back(Reply {
                    seq: cur.seq,
                    code,
                    lines: std::mem::take(&mut st.reply_lines),
                });
            }
            st.waiting = false;
            st.in_data_block = false;
            st.reply_lines.clear();
            pump(shared, st);
            return true;
        }
        _ => {
            push_bounded(
                &mut st.errors,
                format!(
                    "unparsable control line: {}",
                    String::from_utf8_lossy(raw)
                ),
            );
            return true;
        }
    }
    false
}

/// Reader thread: owns the read half, splits the byte stream into
/// CRLF lines and drives the reply/event queues.  On EOF or error it
/// fails every outstanding command and marks the link closed.
fn read_loop(shared: Arc<Shared>, mut stream: TcpStream) {
    let mut buf: Vec<u8> = Vec::new();
    let mut chunk = [0u8; READ_CHUNK];
    loop {
        let n = match stream.read(&mut chunk) {
            Ok(0) => {
                let mut st = lock(&shared.state);
                mark_closed(&mut st, None);
                drop(st);
                notify::emit("torctl");
                return;
            }
            Ok(n) => n,
            Err(e) => {
                let mut st = lock(&shared.state);
                mark_closed(&mut st, Some(format!("control read: {e}")));
                drop(st);
                notify::emit("torctl");
                return;
            }
        };
        buf.extend_from_slice(&chunk[..n]);
        let mut emit = false;
        loop {
            let end = match buf.windows(2).position(|w| w == b"\r\n") {
                Some(p) => p,
                None => break,
            };
            let line: Vec<u8> = buf.drain(..end + 2).take(end).collect();
            let mut st = lock(&shared.state);
            emit |= process_line(&shared, &mut st, &line);
            drop(st);
        }
        if emit {
            notify::emit("torctl");
        }
    }
}

impl RcTorCtl {
    /// Opens a control connection to host:port.  Synchronous with a
    /// bounded timeout — loopback connects resolve or refuse inside
    /// milliseconds, and a synchronous open keeps the state machine
    /// honest: a returned handle is always Connected (the C++ async
    /// connected signal maps to "open returned").  Fails rather than
    /// returns a handle when the daemon refuses.
    pub fn open(host: &str, port: u16) -> RcResult<RcTorCtl> {
        let addrs = resolve_loopback(host, port)?;
        // Try each loopback answer in resolver order (localhost may
        // answer ::1 while the listener sits on 127.0.0.1 only).
        let mut last_err: Option<std::io::Error> = None;
        let mut stream = None;
        for addr in &addrs {
            match TcpStream::connect_timeout(addr, CONNECT_TIMEOUT) {
                Ok(s) => {
                    stream = Some(s);
                    break;
                }
                Err(e) => last_err = Some(e),
            }
        }
        let stream = stream.ok_or_else(|| Fail {
            status: RcStatus::Io,
            msg: match last_err {
                Some(e) => format!("control connect: {e}"),
                None => "control connect: no loopback address".into(),
            },
        })?;
        let _ = stream.set_nodelay(true);
        let writer = stream.try_clone().map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("control clone: {e}"),
        })?;
        let shared = Arc::new(Shared {
            state: Mutex::new(Inner {
                status: STATE_CONNECTED,
                error: None,
                queue: VecDeque::new(),
                current: None,
                waiting: false,
                in_data_block: false,
                reply_lines: Vec::new(),
                completed: VecDeque::new(),
                events: VecDeque::new(),
                errors: VecDeque::new(),
            }),
            writer: Mutex::new(Some(writer)),
            seq: AtomicU64::new(1),
        });
        let reader_shared = Arc::clone(&shared);
        let reader = std::thread::Builder::new()
            .name("torctl-reader".into())
            .spawn(move || read_loop(reader_shared, stream))
            .map_err(|e| Fail {
                status: RcStatus::Io,
                msg: format!("reader spawn: {e}"),
            })?;
        Ok(RcTorCtl {
            inner: shared,
            reader: Mutex::new(Some(reader)),
        })
    }

    pub fn state(&self) -> i32 {
        lock(&self.inner.state).status
    }

    /// Connection error text, "" for a clean EOF or open link.
    pub fn error_string(&self) -> String {
        lock(&self.inner.state)
            .error
            .clone()
            .unwrap_or_default()
    }

    /// Queues a command buffer (secrets ride in a Zeroizing so the
    /// queued AUTHENTICATE wipes on flush/drop) and pumps it when the
    /// link is free.  Returns the sequence number matching the
    /// eventual take_reply() entry.
    fn enqueue(&self, command: Zeroizing<Vec<u8>>) -> RcResult<u64> {
        let mut st = lock(&self.inner.state);
        if st.status != STATE_CONNECTED {
            return fail(RcStatus::Io, "control connection not open");
        }
        let seq = self.inner.seq.fetch_add(1, Ordering::SeqCst);
        st.queue.push_back(Pending {
            seq,
            command,
        });
        pump(&self.inner, &mut st);
        Ok(seq)
    }

    /// Queues a textual command — must be exactly one line (the CRLF
    /// guard doubles as the command-injection boundary: a payload
    /// containing a newline could smuggle a second command).
    pub fn send(&self, command: &str) -> RcResult<u64> {
        if command.is_empty()
            || command.bytes().any(|b| b == b'\r' || b == b'\n')
        {
            return fail(
                RcStatus::InvalidArgument,
                "control command must be one line",
            );
        }
        self.enqueue(Zeroizing::new(command.as_bytes().to_vec()))
    }

    /// Cookie auth: the 32-byte control_auth_cookie as one hex token.
    pub fn auth_cookie(&self, cookie: &[u8]) -> RcResult<u64> {
        if cookie.len() != 32 {
            return fail(
                RcStatus::InvalidArgument,
                "bad control_auth_cookie length",
            );
        }
        // Assemble in a zeroizing buffer — the hex string is the
        // secret's wire shape and lives in the send queue afterwards.
        let mut cmd = Zeroizing::new(Vec::with_capacity(14 + 64));
        cmd.extend_from_slice(b"AUTHENTICATE ");
        for b in cookie {
            cmd.push(b"0123456789abcdef"[(b >> 4) as usize]);
            cmd.push(b"0123456789abcdef"[(b & 0x0f) as usize]);
        }
        self.enqueue(cmd)
    }

    /// Cookie auth by file path — the secret never crosses the FFI
    /// boundary as a buffer; the read happens in-process.
    pub fn auth_cookie_file(&self, path: &str) -> RcResult<u64> {
        let cookie = std::fs::read(path).map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot read {path}: {e}"),
        })?;
        self.auth_cookie(&cookie)
    }

    /// Hashed-password auth: AUTHENTICATE "<QuotedString>".  Per
    /// control-spec the password is a quoted string; non-printable /
    /// non-ASCII bytes go out as \ooo octal escapes, '"' and '\' are
    /// backslash-escaped — the set tor's unescape_string() decodes.
    pub fn auth_password(&self, pass: &[u8]) -> RcResult<u64> {
        if pass.is_empty() {
            return fail(RcStatus::InvalidArgument, "empty password");
        }
        let mut cmd = Zeroizing::new(Vec::with_capacity(pass.len() + 16));
        cmd.extend_from_slice(b"AUTHENTICATE \"");
        for &b in pass {
            match b {
                b'"' => cmd.extend_from_slice(b"\\\""),
                b'\\' => cmd.extend_from_slice(b"\\\\"),
                0x20..=0x7e => cmd.push(b),
                _ => {
                    cmd.push(b'\\');
                    cmd.push(b'0' + ((b >> 6) & 7));
                    cmd.push(b'0' + ((b >> 3) & 7));
                    cmd.push(b'0' + (b & 7));
                }
            }
        }
        cmd.push(b'"');
        self.enqueue(cmd)
    }

    /// Token check for command arguments (GETINFO keys, event names,
    /// signal names): printable, no whitespace/quotes — the characters
    /// that would break the line grammar.
    fn check_token(token: &str, what: &str) -> RcResult<()> {
        if token.is_empty()
            || !token
                .bytes()
                .all(|b| (0x21..=0x7e).contains(&b) && b != b'"')
        {
            return fail(
                RcStatus::InvalidArgument,
                format!("bad {what}"),
            );
        }
        Ok(())
    }

    pub fn getinfo(&self, key: &str) -> RcResult<u64> {
        // GETINFO accepts space-separated key lists ("a b").
        if key.is_empty() {
            return fail(RcStatus::InvalidArgument, "bad getinfo key");
        }
        for part in key.split(' ') {
            Self::check_token(part, "getinfo key")?;
        }
        self.send(&format!("GETINFO {key}"))
    }

    /// `events` is the space-separated event list ("CIRC STREAM" …).
    pub fn setevents(&self, events: &str) -> RcResult<u64> {
        let mut out = String::from("SETEVENTS");
        let mut any = false;
        for ev in events.split_whitespace() {
            Self::check_token(ev, "event name")?;
            out.push(' ');
            out.push_str(ev);
            any = true;
        }
        if !any {
            // Bare SETEVENTS unsubscribes — legal per spec.
            return self.send("SETEVENTS");
        }
        self.send(&out)
    }

    /// TAKEOWNERSHIP — tor exits when this connection closes (the
    /// daemon can never be orphaned by a crash).
    pub fn takeownership(&self) -> RcResult<u64> {
        self.send("TAKEOWNERSHIP")
    }

    /// SIGNAL <name> — NEWNYM, SHUTDOWN, RELOAD, …
    pub fn signal(&self, name: &str) -> RcResult<u64> {
        Self::check_token(name, "signal name")?;
        self.send(&format!("SIGNAL {name}"))
    }

    /// Pops the oldest completed reply.
    pub fn take_reply(&self) -> Option<Reply> {
        lock(&self.inner.state).completed.pop_front()
    }

    /// Pops the oldest asynchronous event line (code 650 stripped).
    pub fn take_event(&self) -> Option<String> {
        lock(&self.inner.state).events.pop_front()
    }

    /// Pops the oldest protocol complaint (unparsable lines etc.).
    pub fn take_error(&self) -> Option<String> {
        lock(&self.inner.state).errors.pop_front()
    }

    /// Severs the link: fails outstanding commands (-1), shuts the
    /// socket down and joins the reader.  Idempotent; also runs on
    /// drop so a leaked handle cannot orphan the thread.
    pub fn close(&self) {
        {
            let mut st = lock(&self.inner.state);
            mark_closed(&mut st, None);
        }
        if let Some(w) = self
            .inner
            .writer
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
        {
            let _ = w.shutdown(Shutdown::Both);
        }
        if let Some(j) = self
            .reader
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .take()
        {
            let _ = j.join();
        }
        notify::emit("torctl");
    }
}

impl Drop for RcTorCtl {
    fn drop(&mut self) {
        self.close();
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::{BufRead, BufReader};
    use std::net::TcpListener;
    use std::path::PathBuf;
    use std::process::{Command, Stdio};
    use std::sync::atomic::AtomicUsize;

    /// Scripted control-port fixture: a listener on an ephemeral
    /// loopback port plus a transcript log every received command is
    /// appended to.  The handler closure plays the daemon half.
    struct MockTor {
        port: u16,
        transcript: Arc<Mutex<Vec<String>>>,
        join: Option<JoinHandle<()>>,
    }

    impl MockTor {
        fn spawn<F>(handler: F) -> MockTor
        where
            F: FnOnce(TcpStream, Arc<Mutex<Vec<String>>>) + Send + 'static,
        {
            let listener = TcpListener::bind((IpAddr::from([127, 0, 0, 1]), 0))
                .unwrap();
            let port = listener.local_addr().unwrap().port();
            let transcript = Arc::new(Mutex::new(Vec::new()));
            let t2 = Arc::clone(&transcript);
            let join = std::thread::spawn(move || {
                if let Ok((conn, _)) = listener.accept() {
                    handler(conn, t2);
                }
            });
            MockTor {
                port,
                transcript,
                join: Some(join),
            }
        }

        fn lines(&self) -> Vec<String> {
            self.transcript.lock().unwrap().clone()
        }
    }

    impl Drop for MockTor {
        fn drop(&mut self) {
            if let Some(j) = self.join.take() {
                let _ = j.join();
            }
        }
    }

    /// Reads commands off the mock socket, logging each line, and runs
    /// `on_cmd` for the reply bytes to send back.
    fn serve<F>(conn: TcpStream, transcript: Arc<Mutex<Vec<String>>>, mut on_cmd: F)
    where
        F: FnMut(&str) -> Option<String>,
    {
        let _ = conn.set_read_timeout(Some(Duration::from_secs(10)));
        let mut writer = conn.try_clone().unwrap();
        let mut reader = BufReader::new(conn);
        loop {
            let mut line = String::new();
            match reader.read_line(&mut line) {
                Ok(0) | Err(_) => return,
                Ok(_) => {}
            }
            let cmd = line.trim_end_matches("\r\n").to_string();
            transcript.lock().unwrap().push(cmd.clone());
            if let Some(reply) = on_cmd(&cmd) {
                if writer.write_all(reply.as_bytes()).is_err() {
                    return;
                }
            }
        }
    }

    /// Polls take_reply until the seq's entry lands (or 5s elapse).
    fn wait_reply(ctl: &RcTorCtl, seq: u64) -> Reply {
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(r) = ctl.take_reply() {
                assert_eq!(r.seq, seq);
                return r;
            }
            assert!(std::time::Instant::now() < deadline,
                    "timed out waiting for reply {seq}");
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn wait_event(ctl: &RcTorCtl) -> String {
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(e) = ctl.take_event() {
                return e;
            }
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn wait_closed(ctl: &RcTorCtl) {
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        while ctl.state() != STATE_CLOSED {
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    #[test]
    fn rejects_non_loopback_hosts() {
        for bad in ["8.8.8.8", "0.0.0.0", "192.168.1.1", "::",
                    "example.org", "10.0.0.1", "fe80::1"] {
            assert!(RcTorCtl::open(bad, 9051).is_err(), "accepted {bad}");
        }
    }

    #[test]
    fn connect_refused_is_an_error() {
        let l = TcpListener::bind((IpAddr::from([127, 0, 0, 1]), 0)).unwrap();
        let port = l.local_addr().unwrap().port();
        drop(l); // nothing listens now
        assert!(RcTorCtl::open("127.0.0.1", port).is_err());
    }

    #[test]
    fn accepts_loopback_literals() {
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |_| Some("250 OK\r\n".into()))
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        assert_eq!(ctl.state(), STATE_CONNECTED);
        let seq = ctl.send("GETINFO version").unwrap();
        let r = wait_reply(&ctl, seq);
        assert_eq!(r.code, 250);
        assert_eq!(r.lines, vec!["OK"]);
        ctl.close();
        assert_eq!(ctl.state(), STATE_CLOSED);
    }

    #[test]
    fn cookie_auth_sends_hex_token() {
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |cmd| {
                if cmd.starts_with("AUTHENTICATE") {
                    Some("250 OK\r\n".into())
                } else {
                    Some("552 Unrecognized\r\n".into())
                }
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let cookie: Vec<u8> = (0..32).collect();
        let seq = ctl.auth_cookie(&cookie).unwrap();
        let r = wait_reply(&ctl, seq);
        assert_eq!(r.code, 250);
        let hex: String = cookie.iter().map(|b| format!("{b:02x}")).collect();
        assert_eq!(
            mock.lines(),
            vec![format!("AUTHENTICATE {hex}")]
        );
        ctl.close();
    }

    #[test]
    fn cookie_auth_rejects_bad_length_offwire() {
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |_| Some("250 OK\r\n".into()))
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        assert!(ctl.auth_cookie(&[0u8; 31]).is_err());
        assert!(ctl.auth_cookie(&[0u8; 33]).is_err());
        std::thread::sleep(Duration::from_millis(100));
        assert!(mock.lines().is_empty());
        ctl.close();
    }

    #[test]
    fn cookie_file_auth() {
        let dir = std::env::temp_dir().join(format!(
            "arktos-torctl-cookie-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let cookie: Vec<u8> = (0..32).map(|i| i * 3 + 1).collect();
        let path = dir.join("control_auth_cookie");
        std::fs::write(&path, &cookie).unwrap();

        let hex: String = cookie.iter().map(|b| format!("{b:02x}")).collect();
        let want = format!("AUTHENTICATE {hex}");
        let mock = MockTor::spawn(move |conn, t| {
            serve(conn, t, move |cmd| {
                if cmd == want {
                    Some("250 OK\r\n".into())
                } else {
                    Some("515 Authentication failed\r\n".into())
                }
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        // The file-read error path never touches the wire.
        assert!(ctl.auth_cookie_file("/nonexistent/cookie").is_err());
        let seq = ctl.auth_cookie_file(path.to_str().unwrap()).unwrap();
        assert_eq!(wait_reply(&ctl, seq).code, 250);
        ctl.close();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn password_auth_escapes_quoted_string() {
        let want = String::from(
            "AUTHENTICATE \"a\\\"b\\\\c\\001\\377~ok\"",
        );
        let want2 = want.clone();
        let mock = MockTor::spawn(move |conn, t| {
            serve(conn, t, move |cmd| {
                if cmd == want2 {
                    Some("250 OK\r\n".into())
                } else {
                    Some(format!("551 saw {cmd}\r\n"))
                }
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let mut pass = Vec::from(&b"a\"b\\c"[..]);
        pass.push(0x01);
        pass.push(0xff);
        pass.extend_from_slice(b"~ok");
        let seq = ctl.auth_password(&pass).unwrap();
        assert_eq!(wait_reply(&ctl, seq).code, 250);
        assert_eq!(mock.lines(), vec![want]);
        ctl.close();
    }

    #[test]
    fn command_grammar() {
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |cmd| match cmd {
                "GETINFO circuit-status" => Some(
                    "250+circuit-status=\r\nline a\r\nline b\r\n.\r\n250 OK\r\n"
                        .into(),
                ),
                "SETEVENTS CIRC STREAM" => Some("250 OK\r\n".into()),
                "TAKEOWNERSHIP" => Some("250 OK\r\n".into()),
                "SIGNAL NEWNYM" => Some("250 OK\r\n".into()),
                _ => Some("552 Unrecognized\r\n".into()),
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();

        let s1 = ctl.getinfo("circuit-status").unwrap();
        let r1 = wait_reply(&ctl, s1);
        assert_eq!(r1.code, 250);
        // "250+circuit-status=" pushes the header line, then the
        // data-block payload lines follow raw.
        assert_eq!(r1.lines, vec!["circuit-status=", "line a", "line b", "OK"]);

        let s2 = ctl.setevents("CIRC  STREAM ").unwrap();
        assert_eq!(wait_reply(&ctl, s2).code, 250);
        let s3 = ctl.takeownership().unwrap();
        assert_eq!(wait_reply(&ctl, s3).code, 250);
        let s4 = ctl.signal("NEWNYM").unwrap();
        assert_eq!(wait_reply(&ctl, s4).code, 250);

        assert_eq!(
            mock.lines(),
            vec![
                "GETINFO circuit-status",
                "SETEVENTS CIRC STREAM",
                "TAKEOWNERSHIP",
                "SIGNAL NEWNYM",
            ]
        );
        // Injection guard: embedded CRLF never reaches the wire.
        assert!(ctl.send("GETINFO x\r\nSIGNAL SHUTDOWN").is_err());
        assert!(ctl.send("").is_err());
        assert!(ctl.signal("NEW NYM").is_err());
        ctl.close();
    }

    #[test]
    fn async_events_interleave_with_replies() {
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |cmd| {
                if cmd.starts_with("GETINFO") {
                    // An async event lands in the middle of the reply.
                    Some("250-first=1\r\n650 STATUS_CLIENT NOTICE BOOTSTRAP \
                          PROGRESS=7\r\n650 CIRC 9 BUILT $AA~G\r\n250 OK\r\n"
                        .into())
                } else {
                    Some("552 Unrecognized\r\n".into())
                }
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let seq = ctl.getinfo("interleave").unwrap();
        let r = wait_reply(&ctl, seq);
        assert_eq!(r.code, 250);
        assert_eq!(r.lines, vec!["first=1", "OK"]);
        assert_eq!(
            wait_event(&ctl),
            "STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=7"
        );
        assert_eq!(wait_event(&ctl), "CIRC 9 BUILT $AA~G");
        ctl.close();
    }

    #[test]
    fn commands_serialize_one_outstanding() {
        // Proves the one-command-in-flight gate: after reading cmd1
        // the mock verifies the pipe stays quiet for a beat (a
        // pipelining client would have flushed cmd2 already), replies,
        // then reads cmd2.
        let mock = MockTor::spawn(|conn, t| {
            let mut writer = conn.try_clone().unwrap();
            let mut reader = BufReader::new(conn);
            let mut line = String::new();
            reader.read_line(&mut line).unwrap();
            t.lock().unwrap().push(line.trim_end().to_string());
            let _ = reader
                .get_ref()
                .set_read_timeout(Some(Duration::from_millis(300)));
            line.clear();
            let early = reader.read_line(&mut line);
            assert!(
                early.is_err() || line.is_empty(),
                "second command arrived before the first reply"
            );
            let _ = reader.get_ref().set_read_timeout(None);
            writer.write_all(b"250-one\r\n250 OK\r\n").unwrap();
            line.clear();
            reader.read_line(&mut line).unwrap();
            t.lock().unwrap().push(line.trim_end().to_string());
            writer.write_all(b"250 OK\r\n").unwrap();
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let s1 = ctl.send("FIRST").unwrap();
        let s2 = ctl.send("SECOND").unwrap();
        let r1 = wait_reply(&ctl, s1);
        assert_eq!(r1.lines, vec!["one", "OK"]);
        assert_eq!(wait_reply(&ctl, s2).code, 250);
        assert_eq!(mock.lines(), vec!["FIRST", "SECOND"]);
        ctl.close();
    }

    #[test]
    fn disconnect_fails_pending_with_minus_one() {
        let mock = MockTor::spawn(|conn, t| {
            // Read one command, hang up without replying.
            let mut reader = BufReader::new(conn);
            let mut line = String::new();
            let _ = reader.read_line(&mut line);
            t.lock().unwrap().push(line.trim_end().to_string());
            // conn drops here -> EOF on the client
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let seq = ctl.send("GETINFO nothing").unwrap();
        wait_closed(&ctl);
        let r = wait_reply(&ctl, seq);
        assert_eq!(r.code, -1);
        assert_eq!(ctl.error_string(), "");
        ctl.close();
    }

    #[test]
    fn close_fails_queued_commands() {
        // Daemon accepts but never replies; the client queues three
        // commands — all must complete with -1 after close().
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |_| None) // never reply
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let s1 = ctl.send("A").unwrap();
        let s2 = ctl.send("B").unwrap();
        let s3 = ctl.send("C").unwrap();
        std::thread::sleep(Duration::from_millis(50));
        ctl.close();
        let mut seen = Vec::new();
        while let Some(r) = ctl.take_reply() {
            seen.push((r.seq, r.code));
        }
        seen.sort();
        assert_eq!(seen, vec![(s1, -1), (s2, -1), (s3, -1)]);
        // Sends on a closed link fail rather than queue forever.
        assert!(ctl.send("D").is_err());
    }

    #[test]
    fn unparsable_lines_go_to_error_lane() {
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |cmd| {
                if cmd.starts_with("GETINFO") {
                    Some("garbage line\r\n250 OK\r\n".into())
                } else {
                    Some("250 OK\r\n".into())
                }
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let seq = ctl.getinfo("x").unwrap();
        assert_eq!(wait_reply(&ctl, seq).code, 250);
        let deadline = std::time::Instant::now() + Duration::from_secs(5);
        loop {
            if let Some(e) = ctl.take_error() {
                assert!(e.starts_with("unparsable control line:"));
                break;
            }
            assert!(std::time::Instant::now() < deadline);
            std::thread::sleep(Duration::from_millis(5));
        }
        ctl.close();
    }

    static NOTIFY_HITS: AtomicUsize = AtomicUsize::new(0);

    unsafe extern "C" fn count_notify(
        _topic: *const std::os::raw::c_char,
        _ud: *mut std::os::raw::c_void,
    ) {
        NOTIFY_HITS.fetch_add(1, Ordering::SeqCst);
    }

    #[test]
    fn replies_and_events_emit_torctl_topic() {
        let _g = crate::store::test_lock();
        crate::notify::set_callback(Some(count_notify), std::ptr::null_mut());
        let before = NOTIFY_HITS.load(Ordering::SeqCst);
        let mock = MockTor::spawn(|conn, t| {
            serve(conn, t, |cmd| {
                if cmd.starts_with("GETINFO") {
                    Some("650 CIRC 1 LAUNCHED\r\n250 OK\r\n".into())
                } else {
                    Some("250 OK\r\n".into())
                }
            })
        });
        let ctl = RcTorCtl::open("127.0.0.1", mock.port).unwrap();
        let seq = ctl.getinfo("x").unwrap();
        wait_reply(&ctl, seq);
        assert!(NOTIFY_HITS.load(Ordering::SeqCst) > before);
        ctl.close();
        crate::notify::set_callback(None, std::ptr::null_mut());
    }

    // ---- optional real-daemon leg ------------------------------------
    // Skipped gracefully when no tor binary resolves: the mock legs
    // above are the contract; this one only proves the client speaks
    // to a real daemon when one exists (TorManager::resolveBinary's
    // search order, minus the app-dir legs a crate test cannot know).

    fn find_tor_binary() -> Option<PathBuf> {
        fn executable(p: PathBuf) -> Option<PathBuf> {
            if p.is_file() {
                Some(p)
            } else {
                None
            }
        }
        if let Ok(v) = std::env::var("ARKTOS_TOR_BINARY") {
            return executable(PathBuf::from(v));
        }
        if let Ok(path) = std::env::var("PATH") {
            for dir in path.split(':') {
                if let Some(p) = executable(PathBuf::from(dir).join("tor")) {
                    return Some(p);
                }
            }
        }
        for dir in ["/usr/bin", "/usr/local/bin", "/usr/sbin"] {
            if let Some(p) = executable(PathBuf::from(dir).join("tor")) {
                return Some(p);
            }
        }
        // Per-user expert bundle: <AppDataLocation>/tor/tor.
        if let Ok(home) = std::env::var("HOME") {
            let bundle = PathBuf::from(home)
                .join(".local/share/arktos/tor/tor");
            if let Some(p) = executable(bundle) {
                return Some(p);
            }
        }
        None
    }

    #[test]
    fn real_daemon_lifecycle() {
        let binary = match find_tor_binary() {
            Some(b) => b,
            None => {
                eprintln!("no tor binary — skipping real-daemon leg");
                return;
            }
        };
        let dir = std::env::temp_dir().join(format!(
            "arktos-torctl-real-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        let port_file = dir.join("control-port");
        let mut child = Command::new(&binary)
            .arg("--DataDirectory").arg(&dir)
            .arg("--ControlPort").arg("auto")
            .arg("--ControlPortWriteToFile").arg(&port_file)
            .arg("--CookieAuthentication").arg("1")
            .arg("--SocksPort").arg("0")
            .arg("--Log").arg("notice stderr")
            .arg("--RunAsDaemon").arg("0")
            .arg("--ClientOnly").arg("1")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .spawn()
            .expect("spawn tor");

        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            // tor writes the port file as soon as the listener is up.
            let deadline =
                std::time::Instant::now() + Duration::from_secs(30);
            let mut contents = String::new();
            while std::time::Instant::now() < deadline {
                if let Ok(c) = std::fs::read_to_string(&port_file) {
                    if c.contains("PORT=") {
                        contents = c;
                        break;
                    }
                }
                std::thread::sleep(Duration::from_millis(100));
            }
            let port: u16 = contents
                .split_whitespace()
                .find_map(|l| l.strip_prefix("PORT="))
                .and_then(|kv| kv.rsplit(':').next())
                .and_then(|p| p.parse().ok())
                .expect("tor never wrote a control-port file");

            let ctl = RcTorCtl::open("127.0.0.1", port).unwrap();
            let cookie = dir.join("control_auth_cookie");
            let seq = ctl.auth_cookie_file(cookie.to_str().unwrap()).unwrap();
            assert_eq!(wait_reply(&ctl, seq).code, 250);

            let seq = ctl.getinfo("version").unwrap();
            let r = wait_reply(&ctl, seq);
            assert_eq!(r.code, 250);
            assert!(r.lines.iter().any(|l| l.starts_with("version=")));

            let seq = ctl.signal("SHUTDOWN").unwrap();
            assert_eq!(wait_reply(&ctl, seq).code, 250);
            ctl.close();
        }));

        // The daemon must exit on SHUTDOWN — a bounded wait so a
        // wedged daemon can never hang the suite.
        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        loop {
            match child.try_wait() {
                Ok(Some(_)) => break,
                _ if std::time::Instant::now() >= deadline => {
                    let _ = child.kill();
                    let _ = child.wait();
                    break;
                }
                _ => std::thread::sleep(Duration::from_millis(100)),
            }
        }
        let _ = std::fs::remove_dir_all(&dir);
        result.unwrap();
    }
}
