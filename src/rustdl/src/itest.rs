//! End-to-end tests through the C ABI against a local fixture server —
//! the acceptance bar from DLACC03: a ranged fixture serving a file in
//! N segments must merge byte-exact, progress counters must advance,
//! cancel must clean part files, and a server without ranges must fall
//! back to single-stream (the engine absorbs it, not the browser).

use std::ffi::CString;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
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

struct Fixture {
    port: u16,
    stop: Arc<AtomicBool>,
    hits: Arc<AtomicUsize>,
    join: Option<std::thread::JoinHandle<()>>,
}

impl Fixture {
    /// `ranges`: answer Range with 206 slices (else always 200 full).
    /// `throttle_bytes`: per-connection write chunk size + 5ms sleep —
    /// keeps big transfers in flight long enough to poll mid-state.
    fn start(body: Arc<Vec<u8>>, ranges: bool, throttle_bytes: usize) -> Fixture {
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let stop = Arc::new(AtomicBool::new(false));
        let hits = Arc::new(AtomicUsize::new(0));
        let (s2, h2) = (Arc::clone(&stop), Arc::clone(&hits));
        let join = std::thread::spawn(move || {
            for conn in listener.incoming() {
                if s2.load(Ordering::SeqCst) {
                    return;
                }
                let Ok(stream) = conn else { continue };
                let (body, hits) = (Arc::clone(&body), Arc::clone(&h2));
                let s3 = Arc::clone(&s2);
                std::thread::spawn(move || {
                    let _ = serve(stream, &body, ranges, throttle_bytes, hits, s3);
                });
            }
        });
        Fixture {
            port,
            stop,
            hits,
            join: Some(join),
        }
    }

    fn url(&self, path: &str) -> CString {
        CString::new(format!("http://127.0.0.1:{}{}", self.port, path)).unwrap()
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

fn read_request(stream: &mut TcpStream) -> Option<(String, String, Option<String>)> {
    let mut reader = BufReader::new(stream.try_clone().ok()?);
    let mut line = String::new();
    reader.read_line(&mut line).ok()?;
    let mut parts = line.split_whitespace();
    let method = parts.next()?.to_string();
    let path = parts.next()?.to_string();
    let mut range = None;
    loop {
        line.clear();
        let n = reader.read_line(&mut line).ok()?;
        if n == 0 || line == "\r\n" {
            break;
        }
        if let Some(v) = line.to_lowercase().strip_prefix("range:") {
            range = Some(v.trim().to_string());
        }
    }
    Some((method, path, range))
}

fn serve(
    mut stream: TcpStream,
    body: &[u8],
    ranges: bool,
    throttle: usize,
    hits: Arc<AtomicUsize>,
    stop: Arc<AtomicBool>,
) -> std::io::Result<()> {
    let Some((method, path, range)) = read_request(&mut stream) else {
        return Ok(());
    };
    hits.fetch_add(1, Ordering::SeqCst);

    // Redirect hop used by the redirect test.
    if path == "/redir" {
        let head = format!(
            "HTTP/1.1 302 Found\r\nLocation: /file\r\nContent-Length: 0\r\n\r\n"
        );
        stream.write_all(head.as_bytes())?;
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
