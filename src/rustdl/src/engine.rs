//! The download engine proper: probe -> N ranged GETs -> merge ->
//! atomic rename, with a single-stream fallback for servers without
//! Accept-Ranges (an internal fallback — the request never bounces
//! back to the browser engine, per the DLACC03 decision).
//!
//! Redirects are followed manually (reqwest's automatic policy is
//! off): every hop re-runs the DLACC04 policy gate (the Qt-side
//! interceptor pipeline — https-first/HTTPS-Only, url-strip, domain
//! blocklist, adblock, public->private redirect refusal) and rebuilds
//! Cookie/Referer per hop so credentials never leak cross-origin.

use std::fs::{self, File, OpenOptions};
use std::io::{Read, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicI64, AtomicI32, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use reqwest::blocking::{Client, Response};
use reqwest::header::{self, HeaderMap, HeaderValue};
use reqwest::Method;

use crate::cookies;
use crate::error::{self, DlStatus, Fail};
use crate::gate;
use crate::sanitize;

/// Progress states — mirrors DlState in rustdl.h.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(i32)]
pub enum DlState {
    Probing = 0,
    Running = 1,
    Merging = 2,
    Done = 3,
    Failed = 4,
    Cancelled = 5,
}

/// Below this, segmentation costs more than it buys.
const MIN_SPLIT_BYTES: i64 = 512 * 1024;
/// Never create a segment smaller than this.
const MIN_SEGMENT_BYTES: i64 = 256 * 1024;
const MAX_CONNECTIONS: usize = 16;
pub const DEFAULT_CONNECTIONS: usize = 8;
const MAX_REDIRECTS: usize = 10;
const SEGMENT_ATTEMPTS: usize = 3;
const COPY_BUF: usize = 64 * 1024;

/// JSON options accepted in dl_start — the open-ended slot DLACC04
/// fills in (proxy, referer, UA parity, gate context).
#[derive(Default)]
pub struct Options {
    pub user_agent: Option<String>,
    /// Document URL the download was initiated from — the adblock
    /// first-party context AND the REF01 referer source.  A download
    /// with no page (typed URL) sends no Referer at all.
    pub first_party: Option<String>,
    /// The requesting profile's HTTPS-First downgrade scope
    /// (PrivacyRequestInterceptor::downgradeScope) — handed to the
    /// gate verbatim.
    pub scope: Option<String>,
    /// PrivacyRequestInterceptor::RefererPolicy value (0-3); the
    /// engine replays rewrittenReferer() per hop.
    pub referer_policy: i32,
    pub proxy: Option<String>,
    /// Tor windows: refuse to run rather than fall back to a direct
    /// socket — a clearnet fetch from a tor process is a
    /// de-anonymization bug (release-blocking per DLACC04).
    pub require_proxy: bool,
}

pub struct Job {
    pub url: String,
    pub dest_dir: PathBuf,
    pub suggested: Option<String>,
    pub connections: usize,
    pub cookie_file: Option<PathBuf>,
    pub options: Options,
}

pub struct Download {
    pub state: AtomicI32,
    pub bytes_done: AtomicI64,
    /// -1 while the server has not committed to a length.
    pub bytes_total: AtomicI64,
    pub connections: AtomicI32,
    pub cancel: AtomicBool,
    speed: Mutex<Speed>,
    error: Mutex<Option<String>>,
    file_name: Mutex<String>,
    output: Mutex<Option<PathBuf>>,
}

struct Speed {
    t: Instant,
    bytes: i64,
    bps: f64,
}

impl Speed {
    fn new() -> Self {
        Speed {
            t: Instant::now(),
            bytes: -1,
            bps: 0.0,
        }
    }

    /// Instantaneous rate since the previous sample, smoothed.  The
    /// baseline is always recorded — a first poll right after start
    /// must still arm the tracker for the next one.
    fn sample(&mut self, done: i64) -> i64 {
        let now = Instant::now();
        let dt = now.duration_since(self.t).as_secs_f64();
        if dt > 0.0001 && self.bytes >= 0 {
            let inst = (done - self.bytes) as f64 / dt;
            self.bps = if self.bps <= 0.0 {
                inst
            } else {
                0.6 * self.bps + 0.4 * inst
            };
        }
        self.t = now;
        self.bytes = done;
        self.bps as i64
    }
}

impl Download {
    pub fn new() -> Arc<Self> {
        Arc::new(Download {
            state: AtomicI32::new(DlState::Probing as i32),
            bytes_done: AtomicI64::new(0),
            bytes_total: AtomicI64::new(-1),
            connections: AtomicI32::new(0),
            cancel: AtomicBool::new(false),
            speed: Mutex::new(Speed::new()),
            error: Mutex::new(None),
            file_name: Mutex::new(String::new()),
            output: Mutex::new(None),
        })
    }

    fn set_state(&self, s: DlState) {
        self.state.store(s as i32, Ordering::SeqCst);
    }

    pub fn cancelled(&self) -> bool {
        self.cancel.load(Ordering::SeqCst)
    }

    fn check_cancel(&self) -> Result<(), Fail> {
        if self.cancelled() {
            Err(Fail {
                status: DlStatus::Busy,
                msg: "cancelled".into(),
            })
        } else {
            Ok(())
        }
    }

    fn fail_error(&self, msg: String) {
        *self.error.lock().unwrap() = Some(msg);
    }

    pub fn poll(&self) -> (i32, i64, i64, i64, i32) {
        let done = self.bytes_done.load(Ordering::SeqCst);
        let bps = self.speed.lock().unwrap().sample(done);
        (
            self.state.load(Ordering::SeqCst),
            done,
            self.bytes_total.load(Ordering::SeqCst),
            bps,
            self.connections.load(Ordering::SeqCst),
        )
    }

    pub fn error_string(&self) -> String {
        self.error.lock().unwrap().clone().unwrap_or_default()
    }

    pub fn file_name(&self) -> String {
        self.file_name.lock().unwrap().clone()
    }

    pub fn output_path(&self) -> String {
        self.output
            .lock()
            .unwrap()
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default()
    }
}

/// A URL redacted for logs/errors: scheme://host/path — the query and
/// fragment (where signed-URL tokens live) never reach a string that
/// can be logged (DLACC05).
fn redact(url: &reqwest::Url) -> String {
    let mut s = format!("{}://{}", url.scheme(), url.host_str().unwrap_or("?"));
    if let Some(port) = url.port() {
        s.push_str(&format!(":{port}"));
    }
    s.push_str(url.path());
    s
}

/// Last-2-labels domain key — cheap stand-in for eTLD+1.  Used only to
/// decide whether a redirect hop may keep the Referer; erring on the
/// "different" side is safe (DLACC04 refines with a real suffix list).
fn domain_key(url: &reqwest::Url) -> String {
    match url.host_str() {
        Some(h) => {
            let labels: Vec<&str> = h.split('.').collect();
            if labels.len() >= 2 {
                labels[labels.len() - 2..].join(".")
            } else {
                h.to_string()
            }
        }
        None => String::new(),
    }
}

/// Extra per-request inputs the redirect loop rebuilds per hop.
struct ReqCtx {
    user_agent: Option<String>,
    /// Parsed first_party option — the REF01 referer source.
    referer_source: Option<reqwest::Url>,
    referer_policy: i32,
    cookie_file: Option<PathBuf>,
    /// Gate arguments: the originating document URL and the profile's
    /// downgrade scope ("" when unknown — a valid scope).
    first_party: String,
    scope: String,
}

/// scheme://host[:port]/ — the origin a trimmed Referer carries.
fn origin_of(url: &reqwest::Url) -> String {
    let mut s = format!("{}://{}", url.scheme(), url.host_str().unwrap_or(""));
    if let Some(port) = url.port() {
        s.push_str(&format!(":{port}"));
    }
    s.push('/');
    s
}

/// Mirrors PrivacyRequestInterceptor::rewrittenReferer (REF01) — the
/// Referer value for a request to `target` initiated from document
/// `source`, under the persisted policy level.  The same
/// last-two-labels approximation the Qt side uses decides
/// same-site-ness (domain_key); documented divergence from a real
/// public-suffix list stands.
///
///   0 EngineDefault — Chromium strict-origin-when-cross-origin:
///     same-site sends the full source URL, cross-site the source
///     origin, https->http nothing.
///   1 Trimmed  — same-site: source origin; cross-site: the TARGET's
///     own origin (uBO referrer-spoof — the destination only sees
///     itself).  https->http still nothing.
///   2 Strict   — same-site: source origin; cross-site: nothing.
///   3 Never    — nothing, ever.
fn referer_for(
    policy: i32,
    source: &reqwest::Url,
    target: &reqwest::Url,
) -> Option<String> {
    if policy >= 3 {
        return None; // RefererNever
    }
    // strict-origin's downgrade rule applies at every level: an https
    // origin is never revealed inside a plaintext http request.
    if source.scheme() == "https" && target.scheme() == "http" {
        return None;
    }
    let same_site = domain_key(source) == domain_key(target);
    match policy {
        // EngineDefault: strict-origin-when-cross-origin.
        0 => Some(if same_site {
            source.as_str().to_string()
        } else {
            origin_of(source)
        }),
        // Trimmed: target-origin spoof cross-site.
        1 => Some(if same_site {
            origin_of(source)
        } else {
            origin_of(target)
        }),
        // Strict (and any unknown level — fail closed toward less
        // leakage, not more).
        _ => {
            if same_site {
                Some(origin_of(source))
            } else {
                None
            }
        }
    }
}

impl ReqCtx {
    /// Headers for one hop.  `prev` is the URL that redirected here —
    /// the Cookie header rides only same-domain hops (DLACC04's
    /// cross-origin strip), and the jar lookup additionally scopes to
    /// the hop's own host so a previous host's cookies can never
    /// follow the chain.  Authorization is never set — there is no
    /// source for one — so a redirect can never forward it.
    fn headers(&self, url: &reqwest::Url, prev: Option<&reqwest::Url>) -> HeaderMap {
        let mut h = HeaderMap::new();
        if let Some(ua) = &self.user_agent {
            if let Ok(v) = HeaderValue::from_str(ua) {
                h.insert(header::USER_AGENT, v);
            }
        }
        if let Some(src) = &self.referer_source {
            if let Some(r) = referer_for(self.referer_policy, src, url) {
                if let Ok(v) = HeaderValue::from_str(&r) {
                    h.insert(header::REFERER, v);
                }
            }
        }
        let same_domain_as_prev = prev
            .map(|p| domain_key(p) == domain_key(url))
            .unwrap_or(true);
        if same_domain_as_prev {
            if let Some(jar) = &self.cookie_file {
                if let (Some(host), Some(path)) = (url.host_str(), Some(url.path())) {
                    if let Some(v) = cookies::header_for(jar, host, path, url.scheme() == "https") {
                        if let Ok(v) = HeaderValue::from_str(&v) {
                            h.insert(header::COOKIE, v);
                        }
                    }
                }
            }
        }
        h
    }
}

/// One request plus manual redirect following.  Each hop re-runs the
/// DLACC04 policy gate (upgrade/veto/blocklist/adblock/SSRF rules —
/// Chromium never re-runs its interceptor on download redirects; this
/// engine deliberately does), rebuilds the headers scoped to the
/// hop's host, and refuses non-http(s) locations outright.  Gate
/// rewrites and wire redirects share the hop cap.
fn send(
    client: &Client,
    method: Method,
    url: &reqwest::Url,
    ctx: &ReqCtx,
    range: Option<(i64, i64)>,
) -> Result<Response, Fail> {
    let mut url = url.clone();
    let mut prev: Option<reqwest::Url> = None;
    for _hop in 0..=MAX_REDIRECTS {
        match gate::check(
            url.as_str(),
            prev.as_ref().map(|u| u.as_str()),
            &ctx.first_party,
            &ctx.scope,
        ) {
            gate::Decision::Allow => {}
            gate::Decision::Block(reason) => {
                return Err(Fail {
                    status: DlStatus::Blocked,
                    msg: reason,
                });
            }
            gate::Decision::Rewrite(target) => {
                let next = reqwest::Url::parse(&target).map_err(|_| Fail {
                    status: DlStatus::Blocked,
                    msg: format!("bad gate rewrite for {}", redact(&url)),
                })?;
                if next.scheme() != "http" && next.scheme() != "https" {
                    return error::fail(
                        DlStatus::Blocked,
                        format!("gate rewrite to {} refused", next.scheme()),
                    );
                }
                prev = Some(url);
                url = next;
                continue;
            }
        }
        let mut headers = ctx.headers(&url, prev.as_ref());
        if let Some((a, b)) = range {
            if let Ok(v) = HeaderValue::from_str(&format!("bytes={a}-{b}")) {
                headers.insert(header::RANGE, v);
            }
        }
        let resp = client
            .request(method.clone(), url.clone())
            .headers(headers)
            .send()
            .map_err(error::Fail::from)?;

        let status = resp.status();
        if !status.is_redirection() {
            return Ok(resp);
        }
        let loc = match resp.headers().get(header::LOCATION) {
            Some(l) => l.to_str().ok().map(|s| s.to_string()),
            None => None,
        };
        let Some(loc) = loc else {
            return Ok(resp); // 3xx without Location — caller reads the body
        };
        let next = url.join(&loc).map_err(|_| Fail {
            status: DlStatus::Network,
            msg: format!("bad redirect location from {}", redact(&url)),
        })?;
        if next.scheme() != "http" && next.scheme() != "https" {
            return error::fail(
                DlStatus::Network,
                format!("refused redirect to {} from {}", next.scheme(), redact(&url)),
            );
        }
        prev = Some(url);
        url = next;
    }
    error::fail(DlStatus::Network, "too many redirects")
}

fn require_success(resp: &Response, what: &str, url: &reqwest::Url) -> Result<(), Fail> {
    let st = resp.status();
    if st.is_success() || st.as_u16() == 206 {
        Ok(())
    } else {
        error::fail(
            DlStatus::Http,
            format!("{what} {}: status {}", redact(url), st.as_u16()),
        )
    }
}

struct Probe {
    total: Option<i64>,
    ranges: bool,
    /// Final URL after redirects.
    url: reqwest::Url,
    /// When the probe used GET, the live response carrying the file
    /// body — reused as the single-stream body so the fallback path
    /// costs one request, not two.
    body: Option<Response>,
    content_disposition: Option<String>,
}

fn probe(client: &Client, url: &reqwest::Url, ctx: &ReqCtx) -> Result<Probe, Fail> {
    // First choice: HEAD.
    match send(client, Method::HEAD, url, ctx, None) {
        Ok(resp) if resp.status().is_success() => {
            let total = resp
                .headers()
                .get(header::CONTENT_LENGTH)
                .and_then(|v| v.to_str().ok())
                .and_then(|s| s.parse::<i64>().ok());
            let ranges = resp
                .headers()
                .get(header::ACCEPT_RANGES)
                .and_then(|v| v.to_str().ok())
                .map(|s| s.to_lowercase().contains("bytes"))
                .unwrap_or(false);
            let cd = resp
                .headers()
                .get(header::CONTENT_DISPOSITION)
                .and_then(|v| v.to_str().ok())
                .map(|s| s.to_string());
            return Ok(Probe {
                total,
                ranges,
                url: resp.url().clone(),
                body: None,
                content_disposition: cd,
            });
        }
        _ => {}
    }

    // Fallback: a 1-byte ranged GET answers everything HEAD would
    // (and proves ranges actually work, not just claim to).
    let resp = send(client, Method::GET, url, ctx, Some((0, 0)))?;
    let cd = resp
        .headers()
        .get(header::CONTENT_DISPOSITION)
        .and_then(|v| v.to_str().ok())
        .map(|s| s.to_string());
    if resp.status().as_u16() == 206 {
        // "Content-Range: bytes 0-0/TOTAL"
        let total = resp
            .headers()
            .get(header::CONTENT_RANGE)
            .and_then(|v| v.to_str().ok())
            .and_then(|s| s.rsplit('/').next())
            .and_then(|s| s.trim().parse::<i64>().ok());
        return Ok(Probe {
            total,
            ranges: true,
            url: resp.url().clone(),
            body: Some(resp),
            content_disposition: cd,
        });
    }
    require_success(&resp, "probe", url)?;
    let total = resp
        .headers()
        .get(header::CONTENT_LENGTH)
        .and_then(|v| v.to_str().ok())
        .and_then(|s| s.parse::<i64>().ok());
    Ok(Probe {
        total,
        ranges: false,
        url: resp.url().clone(),
        body: Some(resp),
        content_disposition: cd,
    })
}

/// `filename*` / `filename` from a Content-Disposition value.
fn disposition_name(cd: &str) -> Option<String> {
    for part in cd.split(';').map(|p| p.trim()) {
        let Some((k, v)) = part.split_once('=') else {
            continue;
        };
        let k = k.trim().to_lowercase();
        let v = v.trim().trim_matches('"');
        if k == "filename*" {
            // RFC 5987: charset'lang'value — decoded bytes are UTF-8,
            // not Latin-1, so decode into a byte buffer first.
            let raw = v.rsplit('\'').next().unwrap_or(v);
            let mut bytes = Vec::with_capacity(raw.len());
            let b = raw.as_bytes();
            let mut i = 0;
            while i < b.len() {
                if b[i] == b'%' && i + 2 < b.len() + 1 {
                    if let Ok(s) = std::str::from_utf8(&b[i + 1..=i + 2]) {
                        if let Ok(n) = u8::from_str_radix(s, 16) {
                            bytes.push(n);
                            i += 3;
                            continue;
                        }
                    }
                }
                bytes.push(b[i]);
                i += 1;
            }
            return Some(String::from_utf8_lossy(&bytes).into_owned());
        }
        if k == "filename" {
            return Some(v.to_string());
        }
    }
    None
}

fn stream_to(mut resp: Response, file: &mut File, dl: &Download) -> Result<i64, Fail> {
    let mut buf = vec![0u8; COPY_BUF];
    let mut wrote = 0i64;
    loop {
        dl.check_cancel()?;
        let n = resp.read(&mut buf).map_err(|e| Fail {
            status: DlStatus::Network,
            msg: format!("read: {e}"),
        })?;
        if n == 0 {
            break;
        }
        file.write_all(&buf[..n])?;
        wrote += n as i64;
        dl.bytes_done.fetch_add(n as i64, Ordering::SeqCst);
    }
    Ok(wrote)
}

/// Fetches [start, end] into `part`, resuming the range across
/// retries from the bytes already on disk.
fn fetch_segment(
    client: &Client,
    url: &reqwest::Url,
    ctx: &ReqCtx,
    part: &Path,
    start: i64,
    end: i64,
    dl: &Download,
) -> Result<(), Fail> {
    let mut attempt = 0;
    loop {
        attempt += 1;
        let have = fs::metadata(part).map(|m| m.len() as i64).unwrap_or(0);
        if have >= end - start + 1 {
            return Ok(());
        }
        dl.check_cancel()?;
        let from = start + have;
        let resp = send(client, Method::GET, url, ctx, Some((from, end)))?;
        if resp.status().as_u16() == 200 {
            // The server ignored Range outright — no real support.
            return error::fail(DlStatus::Http, "ranges refused");
        }
        if resp.status().as_u16() != 206 {
            return error::fail(
                DlStatus::Http,
                format!("segment status {}", resp.status().as_u16()),
            );
        }
        let mut f = OpenOptions::new().create(true).append(true).open(part)?;
        let r = stream_to(resp, &mut f, dl);
        match r {
            Ok(_) if fs::metadata(part).map(|m| m.len() as i64).unwrap_or(0)
                >= end - start + 1 =>
            {
                return Ok(())
            }
            _ if attempt >= SEGMENT_ATTEMPTS => {
                return r.map(|_| ()).or_else(|_| {
                    error::fail(DlStatus::Network, "segment truncated")
                })
            }
            _ => std::thread::sleep(Duration::from_millis(150 * attempt as u64)),
        }
    }
}

/// Copies segment files in order into the staging file.
fn merge(parts: &[PathBuf], staging: &Path, dl: &Download) -> Result<i64, Fail> {
    let mut out = OpenOptions::new()
        .create(true)
        .write(true)
        .truncate(true)
        .open(staging)?;
    let mut buf = vec![0u8; COPY_BUF];
    let mut total = 0i64;
    for p in parts {
        let mut f = File::open(p)?;
        loop {
            dl.check_cancel()?;
            let n = f.read(&mut buf)?;
            if n == 0 {
                break;
            }
            out.write_all(&buf[..n])?;
            total += n as i64;
        }
    }
    out.flush()?;
    out.sync_all()?;
    Ok(total)
}

/// Picks the destination name — sanitized suggestion, then the
/// server's Content-Disposition, then the URL's last path segment.
fn choose_name(job: &Job, probe: &Probe) -> String {
    if let Some(s) = &job.suggested {
        if let Some(n) = sanitize::file_name(s) {
            return n;
        }
    }
    if let Some(cd) = &probe.content_disposition {
        if let Some(raw) = disposition_name(cd) {
            if let Some(n) = sanitize::file_name(&raw) {
                return n;
            }
        }
    }
    if let Some(n) = sanitize::file_name(probe.url.path()) {
        if n != "/" {
            return n;
        }
    }
    "unnamed_download".to_string()
}

/// Resolves `<dir>/<name>` to a path that does not clobber an existing
/// file, appending "-N" before the extension when needed.
fn dedup_path(dir: &Path, name: &str) -> PathBuf {
    let candidate = dir.join(name);
    if !candidate.exists() {
        return candidate;
    }
    for n in 1..=99u32 {
        let c = dir.join(sanitize::dedup_name(name, n));
        if !c.exists() {
            return c;
        }
    }
    // Never rename over an existing file: fall back to a timestamped
    // variant rather than clobbering the user's data.
    let stamp = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    dir.join(sanitize::dedup_name(name, (stamp % 9000 + 1000) as u32))
}

/// The whole transfer.  Returns the final destination path.
fn run_job(dl: &Arc<Download>, job: Job, tmp_dir: PathBuf) -> Result<PathBuf, Fail> {
    let url = reqwest::Url::parse(&job.url).map_err(|_| Fail {
        status: DlStatus::InvalidArgument,
        msg: "unparseable url".into(),
    })?;
    if url.scheme() != "http" && url.scheme() != "https" {
        return error::fail(
            DlStatus::InvalidArgument,
            format!("unsupported scheme {}", url.scheme()),
        );
    }

    // DLACC04 hard rule: a tor-mode download must NEVER go direct.
    // require_proxy refuses the job outright when no proxy made it
    // into the options — the Qt side normally refuses earlier (the
    // selector falls back to the engine path), this is the backstop.
    if job.options.require_proxy && job.options.proxy.is_none() {
        return error::fail(
            DlStatus::Blocked,
            "proxy required for this download but none configured",
        );
    }

    let mut builder = Client::builder()
        .connect_timeout(Duration::from_secs(15))
        .redirect(reqwest::redirect::Policy::none());
    if let Some(ua) = &job.options.user_agent {
        builder = builder.user_agent(ua.clone());
    }
    if let Some(p) = &job.options.proxy {
        let mut proxy = reqwest::Proxy::all(p).map_err(|_| Fail {
            status: DlStatus::InvalidArgument,
            msg: "bad proxy url".into(),
        })?;
        if !job.options.require_proxy {
            // Chromium bypasses the configured proxy for loopback;
            // mirror that for a normal-mode proxy.  Under
            // require_proxy (tor) NOTHING bypasses — even localhost
            // must traverse the SOCKS listener.
            proxy = proxy.no_proxy(reqwest::NoProxy::from_string(
                "localhost,127.0.0.1,::1",
            ));
        }
        builder = builder.proxy(proxy);
    }
    let client = builder.build().map_err(error::Fail::from)?;

    let referer_source = job
        .options
        .first_party
        .as_deref()
        .filter(|s| !s.is_empty())
        .and_then(|s| reqwest::Url::parse(s).ok())
        .filter(|u| u.scheme() == "http" || u.scheme() == "https");
    let ctx = ReqCtx {
        user_agent: job.options.user_agent.clone(),
        referer_source,
        referer_policy: job.options.referer_policy,
        cookie_file: job.cookie_file.clone(),
        first_party: job.options.first_party.clone().unwrap_or_default(),
        scope: job.options.scope.clone().unwrap_or_default(),
    };

    dl.set_state(DlState::Probing);
    let pr = probe(&client, &url, &ctx)?;
    if let Some(t) = pr.total {
        dl.bytes_total.store(t, Ordering::SeqCst);
    }
    let name = choose_name(&job, &pr);
    *dl.file_name.lock().unwrap() = name.clone();
    let dest = dedup_path(&job.dest_dir, &name);
    // Staging lives next to the final name so the completion rename is
    // atomic on the same filesystem (the part files themselves stay in
    // the private tmp dir).
    let staging = job.dest_dir.join(format!(".{name}.ardl"));

    let total = pr.total.unwrap_or(-1);
    let seg_count = if pr.ranges && total >= MIN_SPLIT_BYTES {
        let by_min = (total / MIN_SEGMENT_BYTES).max(1) as usize;
        job.connections.clamp(1, MAX_CONNECTIONS).min(by_min)
    } else {
        1
    };
    dl.connections.store(seg_count as i32, Ordering::SeqCst);
    dl.set_state(DlState::Running);

    if seg_count <= 1 {
        // Single-stream fallback — when the probe already fetched the
        // file body (a 200 reply to the 1-byte range GET), stream it
        // instead of requesting again; a 206 probe body must restart.
        let resp = match pr.body {
            Some(r) if r.status().as_u16() == 200 => r,
            _ => send(&client, Method::GET, &pr.url, &ctx, None)?,
        };
        require_success(&resp, "get", &pr.url)?;
        if dl.bytes_total.load(Ordering::SeqCst) < 0 {
            if let Some(t) = resp
                .headers()
                .get(header::CONTENT_LENGTH)
                .and_then(|v| v.to_str().ok())
                .and_then(|s| s.parse::<i64>().ok())
            {
                dl.bytes_total.store(t, Ordering::SeqCst);
            }
        }
        let mut f = OpenOptions::new()
            .create(true)
            .write(true)
            .truncate(true)
            .open(&staging)?;
        stream_to(resp, &mut f, dl)?;
        f.flush()?;
        f.sync_all()?;
    } else {
        dl.check_cancel()?;
        let mut ranges = Vec::with_capacity(seg_count);
        let base = total / seg_count as i64;
        let rem = total % seg_count as i64;
        let mut off = 0i64;
        for i in 0..seg_count {
            let len = base + if i == 0 { rem } else { 0 };
            ranges.push((off, off + len - 1));
            off += len;
        }
        let parts: Vec<PathBuf> = (0..seg_count)
            .map(|i| tmp_dir.join(format!("seg{i}.part")))
            .collect();
        let urls = pr.url.clone();
        let ctx = &ctx;
        let outcome: Result<(), Fail> = std::thread::scope(|s| {
            let mut hs = Vec::with_capacity(seg_count);
            for i in 0..seg_count {
                let (a, b) = ranges[i];
                let part = parts[i].clone();
                let dl = Arc::clone(dl);
                let client = &client;
                let url = &urls;
                hs.push(s.spawn(move || {
                    fetch_segment(client, url, ctx, &part, a, b, &dl)
                }));
            }
            let mut first_err: Option<Fail> = None;
            for h in hs {
                match h.join() {
                    Ok(Ok(())) => {}
                    Ok(Err(e)) => {
                        dl.cancel.store(true, Ordering::SeqCst);
                        if first_err.is_none() {
                            first_err = Some(e);
                        }
                    }
                    Err(_) => {
                        if first_err.is_none() {
                            first_err = Some(Fail {
                                status: DlStatus::Unavailable,
                                msg: "segment worker panicked".into(),
                            });
                        }
                    }
                }
            }
            match first_err {
                Some(e) => Err(e),
                None => Ok(()),
            }
        });
        if let Err(e) = outcome {
            if dl.cancelled() {
                return Err(Fail {
                    status: DlStatus::Busy,
                    msg: "cancelled".into(),
                });
            }
            // A refused range mid-transfer → restart single-stream
            // rather than failing the user's download.
            if e.msg == "ranges refused" {
                dl.cancel.store(false, Ordering::SeqCst);
                dl.bytes_done.store(0, Ordering::SeqCst);
                dl.connections.store(1, Ordering::SeqCst);
                let resp = send(&client, Method::GET, &pr.url, &ctx, None)?;
                require_success(&resp, "get", &pr.url)?;
                let mut f = OpenOptions::new()
                    .create(true)
                    .write(true)
                    .truncate(true)
                    .open(&staging)?;
                stream_to(resp, &mut f, dl)?;
                f.flush()?;
                f.sync_all()?;
                return commit(dl, &staging, &dest, total);
            }
            return Err(e);
        }
        dl.set_state(DlState::Merging);
        let merged = merge(&parts, &staging, dl)?;
        if merged != total {
            return error::fail(
                DlStatus::Io,
                format!("size check: merged {merged} != expected {total}"),
            );
        }
    }
    commit(dl, &staging, &dest, total)
}

fn commit(
    dl: &Arc<Download>,
    staging: &Path,
    dest: &Path,
    expected: i64,
) -> Result<PathBuf, Fail> {
    dl.check_cancel()?;
    if expected >= 0 {
        let got = fs::metadata(staging).map(|m| m.len() as i64).unwrap_or(-1);
        if got != expected {
            return error::fail(
                DlStatus::Io,
                format!("size check: wrote {got} != expected {expected}"),
            );
        }
    }
    fs::rename(staging, dest).or_else(|_| -> std::io::Result<()> {
        // EXDEV fallback: same-dir staging makes this unreachable on
        // sane setups, but copy+remove covers exotic mounts.
        fs::copy(staging, dest).map(|_| ())?;
        fs::remove_file(staging)
    })?;
    Ok(dest.to_path_buf())
}

/// The worker-thread entry: run the job, land in a terminal state,
/// and always clean the tmp dir on the way out.
pub fn run(dl: Arc<Download>, job: Job, tmp_dir: PathBuf) {
    let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        run_job(&dl, job, tmp_dir.clone())
    }));
    let cancelled = dl.cancelled();
    match result {
        Ok(Ok(path)) => {
            *dl.output.lock().unwrap() = Some(path);
            dl.set_state(DlState::Done);
        }
        Ok(Err(e)) if cancelled || e.msg == "cancelled" => {
            dl.set_state(DlState::Cancelled);
        }
        Ok(Err(e)) => {
            dl.fail_error(e.msg);
            dl.set_state(DlState::Failed);
        }
        Err(_) => {
            dl.fail_error("worker panicked".into());
            dl.set_state(DlState::Failed);
        }
    }
    dl.connections.store(0, Ordering::SeqCst);
    let _ = fs::remove_dir_all(&tmp_dir);
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn redaction() {
        let u = reqwest::Url::parse("https://ex.com/a/b?sig=SECRET&x=1#frag").unwrap();
        assert_eq!(redact(&u), "https://ex.com/a/b");
    }

    #[test]
    fn domain_keys() {
        assert_eq!(
            domain_key(&reqwest::Url::parse("https://a.b.c/x").unwrap()),
            "b.c"
        );
        assert_eq!(
            domain_key(&reqwest::Url::parse("https://localhost/x").unwrap()),
            "localhost"
        );
    }

    #[test]
    fn referer_policies() {
        let src = reqwest::Url::parse("https://page.example/deep/path?q=1").unwrap();
        let same = reqwest::Url::parse("https://cdn.page.example/f.bin").unwrap();
        let cross = reqwest::Url::parse("https://other.org/f.bin").unwrap();
        let plain = reqwest::Url::parse("http://other.org/f.bin").unwrap();

        // EngineDefault — strict-origin-when-cross-origin.
        assert_eq!(referer_for(0, &src, &same).as_deref(), Some(src.as_str()));
        assert_eq!(
            referer_for(0, &src, &cross).as_deref(),
            Some("https://page.example/")
        );
        // Trimmed — same-site: source origin; cross-site: target-origin
        // spoof (the destination only ever sees itself).
        assert_eq!(
            referer_for(1, &src, &same).as_deref(),
            Some("https://page.example/")
        );
        assert_eq!(
            referer_for(1, &src, &cross).as_deref(),
            Some("https://other.org/")
        );
        // Strict — same-site: source origin; cross-site: nothing.
        assert_eq!(
            referer_for(2, &src, &same).as_deref(),
            Some("https://page.example/")
        );
        assert_eq!(referer_for(2, &src, &cross), None);
        // Never — nothing anywhere.
        assert_eq!(referer_for(3, &src, &same), None);
        // The https->http downgrade withholds at every level.
        assert_eq!(referer_for(0, &src, &plain), None);
        assert_eq!(referer_for(1, &src, &plain), None);
        assert_eq!(referer_for(2, &src, &plain), None);
    }

    #[test]
    fn content_disposition() {
        assert_eq!(
            disposition_name("attachment; filename=\"report.pdf\""),
            Some("report.pdf".into())
        );
        assert_eq!(
            disposition_name("attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf"),
            Some("résumé.pdf".into())
        );
    }

    #[test]
    fn dedup_paths() {
        let d = tempfile::tempdir().unwrap();
        let p = dedup_path(d.path(), "f.txt");
        assert_eq!(p, d.path().join("f.txt"));
        File::create(&p).unwrap();
        let p2 = dedup_path(d.path(), "f.txt");
        assert_eq!(p2, d.path().join("f-1.txt"));
    }
}
