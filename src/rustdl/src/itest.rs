//! End-to-end tests through the C ABI against a local fixture server —
//! the acceptance bar from DLACC03: a ranged fixture serving a file in
//! N segments must merge byte-exact, progress counters must advance,
//! cancel must clean part files, and a server without ranges must fall
//! back to single-stream (the engine absorbs it, not the browser).

use std::ffi::CString;
use std::io::{BufRead, BufReader, Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use crate::*;

/// dl_set_temp_dir is process-global — serialize the FFI tests so
/// each one's part-file accounting stays its own.  A panicking test
/// must not cascade through the lock.
static LOCK: Mutex<()> = Mutex::new(());

fn lock() -> std::sync::MutexGuard<'static, ()> {
    LOCK.lock().unwrap_or_else(|e| e.into_inner())
}

/// Deterministic content so the merged file has an exact expected body.
fn fill_pattern(len: usize) -> Vec<u8> {
    (0..len).map(|i| ((i * 31 + 7) % 251) as u8).collect()
}

/// One captured request: method, raw path, lowercased header map.
type Captured = (String, String, std::collections::HashMap<String, String>);

struct Fixture {
    port: u16,
    stop: Arc<AtomicBool>,
    hits: Arc<AtomicUsize>,
    requests: Arc<Mutex<Vec<Captured>>>,
    join: Option<std::thread::JoinHandle<()>>,
}

impl Fixture {
    /// `ranges`: answer Range with 206 slices (else always 200 full).
    /// `throttle_bytes`: per-connection write chunk size + 5ms sleep —
    /// keeps big transfers in flight long enough to poll mid-state.
    fn start(body: Arc<Vec<u8>>, ranges: bool, throttle_bytes: usize) -> Fixture {
        Self::start_opts(body, ranges, throttle_bytes, None)
    }

    /// `cross_target`: when set, /redir-cross 302s to this absolute
    /// URL — the cross-domain redirect fixture.
    fn start_opts(
        body: Arc<Vec<u8>>,
        ranges: bool,
        throttle_bytes: usize,
        cross_target: Option<String>,
    ) -> Fixture {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let stop = Arc::new(AtomicBool::new(false));
        let hits = Arc::new(AtomicUsize::new(0));
        let requests = Arc::new(Mutex::new(Vec::new()));
        let (s2, h2) = (Arc::clone(&stop), Arc::clone(&hits));
        let r2 = Arc::clone(&requests);
        let join = std::thread::spawn(move || {
            for conn in listener.incoming() {
                if s2.load(Ordering::SeqCst) {
                    return;
                }
                let Ok(stream) = conn else { continue };
                let (body, hits) = (Arc::clone(&body), Arc::clone(&h2));
                let (s3, r3) = (Arc::clone(&s2), Arc::clone(&r2));
                let cross = cross_target.clone();
                std::thread::spawn(move || {
                    let _ = serve(stream, &body, ranges, throttle_bytes, hits, s3, r3, cross);
                });
            }
        });
        Fixture {
            port,
            stop,
            hits,
            requests,
            join: Some(join),
        }
    }

    fn url(&self, path: &str) -> CString {
        CString::new(format!("http://127.0.0.1:{}{}", self.port, path)).unwrap()
    }

    /// Header of the first request that hit `path` ("" when absent).
    fn header(&self, path: &str, name: &str) -> String {
        self.requests
            .lock()
            .unwrap()
            .iter()
            .find(|(_, p, _)| p == path)
            .and_then(|(_, _, h)| h.get(name).cloned())
            .unwrap_or_default()
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        // Wake the accept loop so the thread can exit.
        let _ = TcpStream::connect(("127.0.0.1", self.port));
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

fn read_request(
    stream: &mut TcpStream,
) -> Option<(String, String, Option<String>, std::collections::HashMap<String, String>)> {
    let mut reader = BufReader::new(stream.try_clone().ok()?);
    let mut line = String::new();
    reader.read_line(&mut line).ok()?;
    let mut parts = line.split_whitespace();
    let method = parts.next()?.to_string();
    let path = parts.next()?.to_string();
    let mut range = None;
    let mut headers = std::collections::HashMap::new();
    loop {
        line.clear();
        let n = reader.read_line(&mut line).ok()?;
        if n == 0 || line == "\r\n" {
            break;
        }
        if let Some((k, v)) = line.trim_end().split_once(':') {
            let k = k.trim().to_lowercase();
            let v = v.trim().to_string();
            if k == "range" {
                range = Some(v.clone());
            }
            headers.insert(k, v);
        }
    }
    Some((method, path, range, headers))
}

fn serve(
    mut stream: TcpStream,
    body: &[u8],
    ranges: bool,
    throttle: usize,
    hits: Arc<AtomicUsize>,
    stop: Arc<AtomicBool>,
    requests: Arc<Mutex<Vec<Captured>>>,
    cross_target: Option<String>,
) -> std::io::Result<()> {
    let Some((method, path, range, headers)) = read_request(&mut stream) else {
        return Ok(());
    };
    hits.fetch_add(1, Ordering::SeqCst);
    requests
        .lock()
        .unwrap()
        .push((method.clone(), path.clone(), headers));

    // Redirect hop used by the redirect test.
    if path == "/redir" {
        let head = format!(
            "HTTP/1.1 302 Found\r\nLocation: /file\r\nContent-Length: 0\r\n\r\n"
        );
        stream.write_all(head.as_bytes())?;
        return Ok(());
    }
    // Cross-host redirect — a different registrable domain key than
    // 127.0.0.1 (e.g. localhost), exercising the cookie strip.
    if path == "/redir-cross" {
        if let Some(target) = cross_target {
            let head = format!(
                "HTTP/1.1 302 Found\r\nLocation: {target}\r\nContent-Length: 0\r\n\r\n"
            );
            stream.write_all(head.as_bytes())?;
        }
        return Ok(());
    }
    // Early-death probe: the HEAD lies about the length, then the GET
    // writes a fraction of the body and drops the connection — a
    // failure that lands AFTER the dest-dir staging file exists
    // (DLACC05: failed transfers must not leave it behind).
    if path == "/dies" {
        let total = body.len() as i64;
        if method == "HEAD" {
            let head = format!(
                "HTTP/1.1 200 OK\r\nContent-Length: {total}\r\nContent-Type: application/octet-stream\r\n\r\n"
            );
            stream.write_all(head.as_bytes())?;
        } else {
            let head = format!(
                "HTTP/1.1 200 OK\r\nContent-Length: {total}\r\nContent-Type: application/octet-stream\r\n\r\n"
            );
            stream.write_all(head.as_bytes())?;
            stream.write_all(&body[..body.len().min(1000)])?;
            stream.flush()?;
        }
        return Ok(());
    }
    // Traversal probe: hostile suggested name via Content-Disposition.
    let cd = if path == "/traversal" {
        "Content-Disposition: attachment; filename=\"../../evil.txt\"\r\n"
    } else {
        ""
    };

    let total = body.len() as i64;
    let write_body = |stream: &mut TcpStream,
                      slice: &[u8],
                      throttle: usize,
                      stop: &Arc<AtomicBool>|
     -> std::io::Result<()> {
        if throttle == 0 {
            stream.write_all(slice)?;
        } else {
            for chunk in slice.chunks(throttle) {
                if stop.load(Ordering::SeqCst) {
                    return Ok(());
                }
                stream.write_all(chunk)?;
                stream.flush()?;
                std::thread::sleep(Duration::from_millis(5));
            }
        }
        Ok(())
    };

    if method == "HEAD" {
        let head = format!(
            "HTTP/1.1 200 OK\r\nContent-Length: {total}\r\n{}{}Content-Type: application/octet-stream\r\n\r\n",
            cd,
            if ranges { "Accept-Ranges: bytes\r\n" } else { "" }
        );
        stream.write_all(head.as_bytes())?;
        return Ok(());
    }

    if ranges {
        if let Some(r) = &range {
            if let Some(spec) = r.strip_prefix("bytes=") {
                let (a, b) = spec.split_once('-').unwrap();
                let start: i64 = a.parse().unwrap();
                let end: i64 = if b.is_empty() {
                    total - 1
                } else {
                    b.parse().unwrap()
                }
                .min(total - 1);
                let n = (end - start + 1) as usize;
                let head = format!(
                    "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes {start}-{end}/{total}\r\nContent-Length: {n}\r\n{cd}\r\n"
                );
                stream.write_all(head.as_bytes())?;
                return write_body(
                    &mut stream,
                    &body[start as usize..=end as usize],
                    throttle,
                    &stop,
                );
            }
        }
    }
    let head = format!(
        "HTTP/1.1 200 OK\r\nContent-Length: {total}\r\n{cd}Content-Type: application/octet-stream\r\n\r\n"
    );
    stream.write_all(head.as_bytes())?;
    write_body(&mut stream, body, throttle, &stop)
}

fn cstr(s: &str) -> CString {
    CString::new(s).unwrap()
}

// ---- DLACC04: policy gate + proxy parity -----------------------------

/// C-side gate call log: (url, prev_url) per invocation.
static GATE_LOG: Mutex<Vec<(String, String)>> = Mutex::new(Vec::new());

unsafe fn write_buf(dst: *mut std::os::raw::c_char, cap: usize, bytes: &[u8]) {
    if dst.is_null() || cap == 0 {
        return;
    }
    let n = bytes.len().min(cap - 1);
    unsafe { std::ptr::copy_nonoverlapping(bytes.as_ptr(), dst as *mut u8, n) };
    unsafe { *dst.add(n) = 0 };
}

/// Test gate: /blocked paths get refused, /upgrade is rewritten to
/// /file, everything else is allowed.  Every call is logged so tests
/// can assert the gate really ran per hop.
unsafe extern "C" fn test_gate(
    url: *const std::os::raw::c_char,
    prev: *const std::os::raw::c_char,
    _first_party: *const std::os::raw::c_char,
    _scope: *const std::os::raw::c_char,
    out_url: *mut std::os::raw::c_char,
    out_cap: usize,
    out_reason: *mut std::os::raw::c_char,
    reason_cap: usize,
    _ctx: *mut std::os::raw::c_void,
) -> i32 {
    let u = unsafe { CStr::from_ptr(url) }.to_string_lossy().into_owned();
    let p = if prev.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(prev) }.to_string_lossy().into_owned()
    };
    GATE_LOG.lock().unwrap().push((u.clone(), p));
    if u.contains("/blocked") {
        let r = CString::new("gate block: test rule").unwrap();
        unsafe { write_buf(out_reason, reason_cap, r.as_bytes_with_nul()) };
        return 1;
    }
    if u.contains("/upgrade") {
        let t = CString::new(u.replace("/upgrade", "/file")).unwrap();
        unsafe { write_buf(out_url, out_cap, t.as_bytes_with_nul()) };
        return 2;
    }
    0
}

/// Minimal SOCKS5 server for the proxy-parity proof: no-auth greeting,
/// CONNECT always answered with success, then the tunnel is spliced to
/// `forward_to` — the requested target is IGNORED.  A client that
/// bypassed the proxy or dialed the (dead) URL port directly can never
/// reach the real file.
struct SocksFixture {
    port: u16,
    saw_connect: Arc<AtomicBool>,
    stop: Arc<AtomicBool>,
    join: Option<std::thread::JoinHandle<()>>,
}

impl SocksFixture {
    fn start(forward_to: std::net::SocketAddr) -> SocksFixture {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let saw_connect = Arc::new(AtomicBool::new(false));
        let stop = Arc::new(AtomicBool::new(false));
        let (sc, st) = (Arc::clone(&saw_connect), Arc::clone(&stop));
        let join = std::thread::spawn(move || {
            for conn in listener.incoming() {
                if st.load(Ordering::SeqCst) {
                    return;
                }
                let Ok(stream) = conn else { continue };
                let sc2 = Arc::clone(&sc);
                std::thread::spawn(move || {
                    let _ = socks_session(stream, forward_to, sc2);
                });
            }
        });
        SocksFixture {
            port,
            saw_connect,
            stop,
            join: Some(join),
        }
    }
}

impl Drop for SocksFixture {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::SeqCst);
        let _ = TcpStream::connect(("127.0.0.1", self.port));
        if let Some(j) = self.join.take() {
            let _ = j.join();
        }
    }
}

fn read_exact(stream: &mut TcpStream, n: usize) -> std::io::Result<Vec<u8>> {
    let mut buf = vec![0u8; n];
    stream.read_exact(&mut buf)?;
    Ok(buf)
}

fn socks_session(
    mut stream: TcpStream,
    forward_to: std::net::SocketAddr,
    saw_connect: Arc<AtomicBool>,
) -> std::io::Result<()> {
    // Greeting: VER NMETHODS METHODS...
    let head = read_exact(&mut stream, 2)?;
    if head[0] != 0x05 {
        return Ok(());
    }
    let _methods = read_exact(&mut stream, head[1] as usize)?;
    stream.write_all(&[0x05, 0x00])?; // no-auth

    // Request: VER CMD RSV ATYP ADDR PORT — the address is parsed but
    // deliberately unused: the tunnel always goes to `forward_to`.
    let req = read_exact(&mut stream, 4)?;
    if req[0] != 0x05 || req[1] != 0x01 {
        return Ok(());
    }
    let _skip = match req[3] {
        0x01 => read_exact(&mut stream, 4 + 2)?,
        0x03 => {
            let len = read_exact(&mut stream, 1)?[0] as usize;
            read_exact(&mut stream, len + 2)?
        }
        0x04 => read_exact(&mut stream, 16 + 2)?,
        _ => return Ok(()),
    };
    saw_connect.store(true, Ordering::SeqCst);
    let mut target = TcpStream::connect(forward_to)?;
    stream.write_all(&[0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0])?;
    stream.flush()?;

    // Splice both ways.
    let mut up = stream.try_clone()?;
    let mut down = target.try_clone()?;
    let relay = std::thread::spawn(move || {
        let _ = std::io::copy(&mut up, &mut down);
        let _ = down.shutdown(std::net::Shutdown::Both);
    });
    let _ = std::io::copy(&mut target, &mut stream);
    let _ = stream.shutdown(std::net::Shutdown::Both);
    let _ = relay.join();
    Ok(())
}

#[test]
fn gate_rewrite_reaches_file() {
    let _g = lock();
    GATE_LOG.lock().unwrap().clear();
    assert_eq!(unsafe { dl_set_gate(Some(test_gate), std::ptr::null_mut()) }, DlStatus::Ok);
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(64 * 1024));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let h = cfg.start(&fx.url("/upgrade"), "up.bin", 2);
    wait_state(h, &[3], Duration::from_secs(15));
    assert_eq!(std::fs::read(cfg.dest.path().join("up.bin")).unwrap(), *body);
    // The gate saw the original URL before any wire request.
    let log = GATE_LOG.lock().unwrap();
    assert!(log.iter().any(|(u, _)| u.contains("/upgrade")));
    drop(log);
    unsafe {
        dl_set_gate(None, std::ptr::null_mut());
        dl_free(h)
    };
}

#[test]
fn gate_block_fails_closed() {
    let _g = lock();
    GATE_LOG.lock().unwrap().clear();
    assert_eq!(unsafe { dl_set_gate(Some(test_gate), std::ptr::null_mut()) }, DlStatus::Ok);
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(64 * 1024));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let h = cfg.start(&fx.url("/blocked"), "no.bin", 2);
    wait_state(h, &[4], Duration::from_secs(15));
    assert!(err_string(h).contains("gate block"));
    // The refusal happened before any wire request.
    assert_eq!(fx.hits.load(Ordering::SeqCst), 0);
    unsafe {
        dl_set_gate(None, std::ptr::null_mut());
        dl_free(h)
    };
}

#[test]
fn redirect_hops_are_regated() {
    let _g = lock();
    GATE_LOG.lock().unwrap().clear();
    assert_eq!(unsafe { dl_set_gate(Some(test_gate), std::ptr::null_mut()) }, DlStatus::Ok);
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(64 * 1024));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let h = cfg.start(&fx.url("/redir"), "rg.bin", 2);
    wait_state(h, &[3], Duration::from_secs(15));
    let log = GATE_LOG.lock().unwrap();
    // The hop onto /file carried /redir as its previous URL — proof
    // the gate ran on the redirect target, not just the start URL.
    assert!(
        log.iter().any(|(u, p)| u.contains("/file") && p.contains("/redir")),
        "redirect target never re-gated: {log:?}"
    );
    drop(log);
    unsafe {
        dl_set_gate(None, std::ptr::null_mut());
        dl_free(h)
    };
}

#[test]
fn cross_domain_redirect_strips_cookie() {
    let _g = lock();
    // Two names, one machine: 127.0.0.1 and localhost differ in
    // registrable-domain key, so the redirect hop is cross-domain.
    // fx_b is the redirect target (reached as "localhost"), fx_a the
    // entry point on 127.0.0.1 that issues the 302.
    if ("localhost", 0u16).to_socket_addrs().is_err() {
        return; // no localhost resolution on this host — skip
    }
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(8192));
    let fx_b = Fixture::start(Arc::clone(&body), true, 0);
    let cross = format!("http://localhost:{}/file", fx_b.port);
    let fx_a = Fixture::start_opts(Arc::clone(&body), true, 0, Some(cross));

    // Jar rows for BOTH hosts — the strip (not host matching) is what
    // must keep Cookie off the post-redirect hop.
    let mut jar = tempfile::NamedTempFile::new().unwrap();
    writeln!(
        jar,
        "127.0.0.1\tFALSE\t/\tFALSE\t0\tsess\tAAA\n\
         localhost\tFALSE\t/\tFALSE\t0\tsess\tBBB"
    )
    .unwrap();
    jar.flush().unwrap();

    let mut h: DlHandle = 0;
    let status = unsafe {
        dl_start(
            fx_a.url("/redir-cross").as_ptr(),
            cstr(cfg.dest.path().to_str().unwrap()).as_ptr(),
            cstr("x.bin").as_ptr(),
            2,
            cstr(jar.path().to_str().unwrap()).as_ptr(),
            std::ptr::null(),
            &mut h,
        )
    };
    assert_eq!(status, DlStatus::Ok);
    wait_state(h, &[3], Duration::from_secs(15));

    // The owning host's cookie went out on the first hop…
    assert_eq!(fx_a.header("/redir-cross", "cookie"), "sess=AAA");
    // …and the request produced BY the redirect (the first hit on the
    // new domain) carried no Cookie header — the "when the hop
    // changes registrable domain" strip.  Later fresh fetches against
    // the resolved URL may carry that host's own cookie (browser
    // semantics) — the inviolable rule is that the SOURCE host's
    // cookie is never forwarded cross-domain.
    let reqs = fx_b.requests.lock().unwrap();
    let hops: Vec<_> = reqs.iter().filter(|(_, p, _)| p == "/file").collect();
    assert!(!hops.is_empty());
    assert!(
        !hops[0].2.contains_key("cookie"),
        "cookie rode the domain-change hop: {:?}",
        hops[0].2
    );
    for (_, _, h) in &hops {
        assert_ne!(
            h.get("cookie").map(String::as_str).unwrap_or(""),
            "sess=AAA",
            "source host's cookie leaked cross-domain: {h:?}"
        );
    }
    drop(reqs);
    unsafe { dl_free(h) };
}

#[test]
fn require_proxy_never_goes_direct() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(8192));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let opts = cstr(r#"{"require_proxy":true}"#);
    let mut h: DlHandle = 0;
    let status = unsafe {
        dl_start(
            fx.url("/file").as_ptr(),
            cstr(cfg.dest.path().to_str().unwrap()).as_ptr(),
            cstr("np.bin").as_ptr(),
            2,
            std::ptr::null(),
            opts.as_ptr(),
            &mut h,
        )
    };
    assert_eq!(status, DlStatus::Ok);
    wait_state(h, &[4], Duration::from_secs(15));
    assert!(err_string(h).contains("proxy"));
    // Nothing touched the wire.
    assert_eq!(fx.hits.load(Ordering::SeqCst), 0);
    unsafe { dl_free(h) };
}

#[test]
fn socks5h_routes_and_ignores_url_port() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(8192));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let fwd: std::net::SocketAddr = format!("127.0.0.1:{}", fx.port).parse().unwrap();
    let socks = SocksFixture::start(fwd);
    // The URL points at a dead port — only a real SOCKS tunnel (which
    // the fixture splices to the live server) can deliver the file.
    let url = cstr("http://127.0.0.1:1/file");
    let opts = cstr(&format!(
        r#"{{"proxy":"socks5h://127.0.0.1:{}","require_proxy":true}}"#,
        socks.port
    ));
    let mut h: DlHandle = 0;
    let status = unsafe {
        dl_start(
            url.as_ptr(),
            cstr(cfg.dest.path().to_str().unwrap()).as_ptr(),
            cstr("via-socks.bin").as_ptr(),
            2,
            std::ptr::null(),
            opts.as_ptr(),
            &mut h,
        )
    };
    assert_eq!(status, DlStatus::Ok);
    let p = wait_state(h, &[3, 4], Duration::from_secs(30));
    assert_eq!(p.state, 3, "download via socks failed: {}", err_string(h));
    assert_eq!(std::fs::read(cfg.dest.path().join("via-socks.bin")).unwrap(), *body);
    assert!(socks.saw_connect.load(Ordering::SeqCst));
    unsafe { dl_free(h) };
}

#[test]
fn segmented_with_cookie_file_and_full_options() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(2 * 1024 * 1024));
    let fx = Fixture::start(Arc::clone(&body), true, 4 * 1024);
    let mut jar = tempfile::NamedTempFile::new().unwrap();
    writeln!(jar, "127.0.0.1\tFALSE\t/\tFALSE\t0\tsess\tAAA").unwrap();
    jar.flush().unwrap();
    // The option surface the Qt side sends with a real page —
    // UA + first_party + scope + referer policy all at once.
    let opts = cstr(&format!(
        r#"{{"user_agent":"AroraTest/1.0","first_party":"http://127.0.0.1:{}/warmup","scope":"","referer_policy":0}}"#,
        fx.port
    ));
    let mut h: DlHandle = 0;
    assert_eq!(
        unsafe {
            dl_start(
                fx.url("/file").as_ptr(),
                cstr(cfg.dest.path().to_str().unwrap()).as_ptr(),
                cstr("ck.bin").as_ptr(),
                8,
                cstr(jar.path().to_str().unwrap()).as_ptr(),
                opts.as_ptr(),
                &mut h,
            )
        },
        DlStatus::Ok
    );
    let p = wait_state(h, &[3, 4], Duration::from_secs(60));
    assert_eq!(p.state, 3, "download failed: {}", err_string(h));
    assert_eq!(std::fs::read(cfg.dest.path().join("ck.bin")).unwrap(), *body);
    assert_eq!(fx.header("/file", "cookie"), "sess=AAA");
    unsafe { dl_free(h) };
}

#[test]
fn header_parity_ua_and_referer() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(8192));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    // REF01 Trimmed (1): cross-site -> the target's own origin — the
    // source page's identity must not cross.
    let opts = cstr(
        r#"{"user_agent":"ParityUA/9.9","first_party":"http://source.example/secret?page=1","referer_policy":1}"#,
    );
    let mut h: DlHandle = 0;
    assert_eq!(
        unsafe {
            dl_start(
                fx.url("/file").as_ptr(),
                cstr(cfg.dest.path().to_str().unwrap()).as_ptr(),
                cstr("ua.bin").as_ptr(),
                2,
                std::ptr::null(),
                opts.as_ptr(),
                &mut h,
            )
        },
        DlStatus::Ok
    );
    wait_state(h, &[3], Duration::from_secs(15));
    assert_eq!(fx.header("/file", "user-agent"), "ParityUA/9.9");
    assert_eq!(
        fx.header("/file", "referer"),
        format!("http://127.0.0.1:{}/", fx.port),
        "trimmed referer must be the target origin, never the source page"
    );
    unsafe { dl_free(h) };
}

#[test]
fn referer_never_sends_nothing() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(8192));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let opts = cstr(r#"{"first_party":"http://source.example/x","referer_policy":3}"#);
    let mut h: DlHandle = 0;
    assert_eq!(
        unsafe {
            dl_start(
                fx.url("/file").as_ptr(),
                cstr(cfg.dest.path().to_str().unwrap()).as_ptr(),
                cstr("nr.bin").as_ptr(),
                2,
                std::ptr::null(),
                opts.as_ptr(),
                &mut h,
            )
        },
        DlStatus::Ok
    );
    wait_state(h, &[3], Duration::from_secs(15));
    assert_eq!(fx.header("/file", "referer"), "");
    unsafe { dl_free(h) };
}

fn wait_state(h: DlHandle, states: &[i32], timeout: Duration) -> DlProgress {
    let deadline = Instant::now() + timeout;
    loop {
        let mut p = DlProgress {
            state: -1,
            bytes_done: 0,
            bytes_total: 0,
            speed_bps: 0,
            connections: 0,
            reserved: 0,
        };
        assert_eq!(unsafe { dl_poll(h, &mut p) }, DlStatus::Ok);
        if states.contains(&p.state) {
            return p;
        }
        assert!(Instant::now() < deadline, "timeout waiting for {states:?} (state {})", p.state);
        std::thread::sleep(Duration::from_millis(25));
    }
}

fn poll(h: DlHandle) -> DlProgress {
    let mut p = DlProgress {
        state: -1,
        bytes_done: 0,
        bytes_total: 0,
        speed_bps: 0,
        connections: 0,
        reserved: 0,
    };
    assert_eq!(unsafe { dl_poll(h, &mut p) }, DlStatus::Ok);
    p
}

fn err_string(h: DlHandle) -> String {
    unsafe {
        let p = dl_error_message(h);
        let s = if p.is_null() {
            String::new()
        } else {
            CString::from_raw(p).to_string_lossy().into_owned()
        };
        s
    }
}

struct Cfg {
    tmp: tempfile::TempDir,
    dest: tempfile::TempDir,
}

impl Cfg {
    fn new() -> Cfg {
        let c = Cfg {
            tmp: tempfile::tempdir().unwrap(),
            dest: tempfile::tempdir().unwrap(),
        };
        assert_eq!(
            unsafe { dl_set_temp_dir(cstr(c.tmp.path().to_str().unwrap()).as_ptr()) },
            DlStatus::Ok
        );
        c
    }

    fn start(&self, url: &CString, suggested: &str, conns: i32) -> DlHandle {
        let mut h: DlHandle = 0;
        let status = unsafe {
            dl_start(
                url.as_ptr(),
                cstr(self.dest.path().to_str().unwrap()).as_ptr(),
                cstr(suggested).as_ptr(),
                conns,
                std::ptr::null(),
                std::ptr::null(),
                &mut h,
            )
        };
        assert_eq!(status, DlStatus::Ok);
        h
    }

    fn tmp_entries(&self) -> usize {
        std::fs::read_dir(self.tmp.path()).unwrap().count()
    }
}

#[test]
fn segmented_merge_is_byte_exact() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(10 * 1024 * 1024)); // spec size: 10 MB
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let h = cfg.start(&fx.url("/file"), "big.bin", 8);
    let p = wait_state(h, &[3], Duration::from_secs(60));
    assert_eq!(p.bytes_done, body.len() as i64);
    let out = cfg.dest.path().join("big.bin");
    assert_eq!(std::fs::read(&out).unwrap(), *body);
    // Multiple range hits prove the segments really ran in parallel.
    assert!(fx.hits.load(Ordering::SeqCst) > 2);
    // Part-file work dir is gone once the transfer commits.
    for _ in 0..40 {
        if cfg.tmp_entries() == 0 {
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(cfg.tmp_entries(), 0);
    unsafe { dl_free(h) };
}

#[test]
fn progress_advances_and_reports_speed() {
    let _g = lock();
    let cfg = Cfg::new();
    // ~2 KB per 5 ms per connection keeps the transfer in flight.
    let body = Arc::new(fill_pattern(2 * 1024 * 1024));
    let fx = Fixture::start(Arc::clone(&body), true, 2 * 1024);
    let h = cfg.start(&fx.url("/file"), "prog.bin", 4);
    let p0 = poll(h);
    std::thread::sleep(Duration::from_millis(600));
    let p1 = poll(h);
    assert!(p1.bytes_done > p0.bytes_done, "no progress: {p0:?} {p1:?}");
    assert!(p1.speed_bps > 0, "speed not reported: {p1:?}");
    wait_state(h, &[3], Duration::from_secs(60));
    unsafe { dl_free(h) };
}

#[test]
fn cancel_cleans_parts() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(4 * 1024 * 1024));
    let fx = Fixture::start(Arc::clone(&body), true, 4 * 1024);
    let h = cfg.start(&fx.url("/file"), "cancel.bin", 8);
    // Wait until bytes are actually flowing.
    wait_state(h, &[1], Duration::from_secs(10));
    for _ in 0..100 {
        if poll(h).bytes_done > 0 {
            break;
        }
        std::thread::sleep(Duration::from_millis(20));
    }
    let _ = unsafe { dl_cancel(h) };
    let p = wait_state(h, &[5], Duration::from_secs(15));
    assert_eq!(p.state, 5);
    // No partial output file, no leftover part dir.
    for _ in 0..60 {
        if cfg.tmp_entries() == 0 {
            break;
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    assert_eq!(cfg.tmp_entries(), 0);
    assert!(!cfg.dest.path().join("cancel.bin").exists());
    // The dest-dir staging file must die with the transfer (DLACC05).
    assert!(!cfg.dest.path().join(".cancel.bin.ardl").exists());
    unsafe { dl_free(h) };
}

#[test]
fn failed_transfer_removes_staging() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(1024 * 1024));
    let fx = Fixture::start(Arc::clone(&body), false, 0);
    // The server dies mid-body — the commit size check fails AFTER
    // the staging file exists, so removal is really exercised.
    let h = cfg.start(&fx.url("/dies"), "d.bin", 2);
    wait_state(h, &[4], Duration::from_secs(20));
    assert!(!cfg.dest.path().join(".d.bin.ardl").exists());
    assert!(!cfg.dest.path().join("d.bin").exists());
    unsafe { dl_free(h) };
}

#[test]
fn single_stream_fallback_merges() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(1024 * 1024));
    let fx = Fixture::start(Arc::clone(&body), false, 0); // no range support
    let h = cfg.start(&fx.url("/file"), "plain.bin", 8); // asks for 8 anyway
    wait_state(h, &[3], Duration::from_secs(30));
    assert_eq!(std::fs::read(cfg.dest.path().join("plain.bin")).unwrap(), *body);
    // HEAD probe + a single GET — no range traffic at all.
    assert!(fx.hits.load(Ordering::SeqCst) <= 3);
    unsafe { dl_free(h) };
}

#[test]
fn traversal_name_lands_inside_dest() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(4096));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let h = cfg.start(&fx.url("/traversal"), "../../evil.txt", 2);
    wait_state(h, &[3], Duration::from_secs(15));
    // The sanitized name lands INSIDE the destination dir — never the
    // parent, never an absolute path.
    assert!(cfg.dest.path().join("evil.txt").exists());
    assert!(!Path::new(cfg.dest.path().parent().unwrap()).join("evil.txt").exists());
    unsafe { dl_free(h) };
}

#[test]
fn redirect_chain_completes() {
    let _g = lock();
    let cfg = Cfg::new();
    let body = Arc::new(fill_pattern(8192));
    let fx = Fixture::start(Arc::clone(&body), true, 0);
    let h = cfg.start(&fx.url("/redir"), "red.bin", 2);
    wait_state(h, &[3], Duration::from_secs(15));
    assert_eq!(std::fs::read(cfg.dest.path().join("red.bin")).unwrap(), *body);
    unsafe { dl_free(h) };
}

#[test]
fn errors_redact_query() {
    let _g = lock();
    let cfg = Cfg::new();
    // Nothing listens on this port → connect failure must not echo the
    // token-bearing URL.
    let h = cfg.start(
        &cstr("http://127.0.0.1:9/x?sig=SECRETTOKEN"),
        "e.bin",
        2,
    );
    wait_state(h, &[4], Duration::from_secs(20));
    let e = err_string(h);
    assert!(!e.contains("SECRETTOKEN"), "error leaked url: {e}");
    unsafe { dl_free(h) };
}
