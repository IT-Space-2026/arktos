//! ENG02: NavigationPolicy — the engine-agnostic request-policy core.
//!
//! Every branching decision the request interceptors and the
//! cookie-store gate used to make in C++ lives here as pure verdict
//! functions over a compact JSON manifest:
//!
//!   request in  -> {"url", "first_party_url", "resource_type",
//!                   "method", "headers", "scope", "tor_mode",
//!                   "script_allowed", "min_referer_level"}
//!   verdict out -> {"action": "pass"}
//!                | {"action": "block", "reason": ...}
//!                | {"action": "redirect", "url": ..., "reason": ...}
//!                | {"action": "allow", "referer": {"op": ...}}
//!
//! The C++ interceptor is reduced to marshaling the manifest and
//! applying the verdict (info.block / info.redirect / setHttpHeader);
//! the adblock matcher stays a delegated stage the adapter runs on an
//! "allow" verdict.  Everything here is memory-safe Rust over
//! attacker-controlled URLs and headers.
//!
//! State the policy needs is pushed in, never pulled: loadSettings()
//! on the GUI thread ships a snapshot (rc_policy_load_snapshot); the
//! JSCTL per-site script grant arrives pre-resolved in the manifest
//! ("script_allowed"); the cookie jar ships its own rule lists per
//! call (rc_policy_cookie_filter).  Downgrade marks, session
//! allowances and the consume-once blocked-navigation registries live
//! behind the mutex — every entry point is safe on the engine's IO
//! thread.
//!
//! Mirroring rules (kept 1:1 with the pre-refactor C++):
//!   * private/local/loopback classifiers match QHostAddress's
//!     semantics incl. bracketed IPv6 and .localhost/.local/.onion;
//!   * same_site uses the last-two-labels approximation AdBlockRule
//!     uses — deliberately no public-suffix list;
//!   * referer_origin drops the scheme's default port like
//!     QUrl::port() == -1 does;
//!   * bounds: 256 downgrade marks / http allowances per scope, 64
//!     recorded blocked navigations — a full set fails closed on the
//!     block, silently on the bookkeeping.

use serde_json::{json, Value};
use std::collections::{HashMap, HashSet};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr};
use std::sync::{Mutex, MutexGuard, OnceLock, RwLock, RwLockReadGuard};
use std::time::{SystemTime, UNIX_EPOCH};

use crate::blocklist;
use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::urlstrip;

// ---- resource-type ordinals -------------------------------------------
// Mirrors QWebEngineUrlRequestInfo::ResourceType / Engine::ResourceType
// (engineinterface.h) — the manifest carries the raw engine ordinal so
// a second backend maps its own type set in the same slots.
const RT_MAIN_FRAME: i64 = 0;
const RT_SCRIPT: i64 = 3;
const RT_FONT: i64 = 5;
const RT_WORKER: i64 = 9;
const RT_SHARED_WORKER: i64 = 10;
const RT_PREFETCH: i64 = 11;
const RT_PING: i64 = 14;
const RT_SERVICE_WORKER: i64 = 15;
const RT_CSP_REPORT: i64 = 16;
const RT_WEBSOCKET: i64 = 254;

// RefererPolicy levels (PrivacyRequestInterceptor::RefererPolicy).
const REFERER_ENGINE_DEFAULT: i64 = 0;
const REFERER_TRIMMED: i64 = 1;
const REFERER_STRICT: i64 = 2;
const REFERER_NEVER: i64 = 3;

// SecurityLevel tiers.
const SECURITY_SAFER: i64 = 1;

// QWebEngineLoadingInfo::ErrorDomain — only connection-layer failures
// are TLS evidence (see failure_implies_downgrade).
const CONNECTION_ERROR_DOMAIN: i64 = 2;

// CookieJar::AcceptPolicy ordinals.
const COOKIE_ACCEPT_ALWAYS: i64 = 0;
const COOKIE_ACCEPT_NEVER: i64 = 1;
const COOKIE_ACCEPT_FIRST_PARTY: i64 = 2;

const MAX_DOWNGRADED_HOSTS: usize = 256;
const MAX_HTTP_ALLOWED_HOSTS: usize = 256;
const MAX_BLOCKED_ALLOWED_HOSTS: usize = 256;
const MAX_BLOCKED_NAVS: usize = 64;

fn now_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

// ---- snapshot --------------------------------------------------------

/// Immutable policy toggles pushed by the GUI thread.  Defaults mirror
/// the pre-refactor C++ statics exactly — privacy defaults on, the
/// breakage-prone opt-ins off.
#[derive(Clone)]
struct Snapshot {
    https_first: bool,
    https_only: bool,
    referer_policy: i64,
    security_level: i64,
    block_pings: bool,
    block_remote_fonts: bool,
    block_prefetch: bool,
    block_third_party_ws: bool,
    strip_tracking_params: bool,
    domain_blocklist: bool,
}

impl Default for Snapshot {
    fn default() -> Self {
        Snapshot {
            https_first: true,
            https_only: true,
            referer_policy: REFERER_TRIMMED,
            security_level: 0,
            block_pings: true,
            block_remote_fonts: false,
            block_prefetch: true,
            block_third_party_ws: false,
            strip_tracking_params: true,
            domain_blocklist: true,
        }
    }
}

impl Snapshot {
    fn flag(&self, name: &str) -> Option<Value> {
        match name {
            "https_first" => Some(json!(self.https_first)),
            "https_only" => Some(json!(self.https_only)),
            "referer_policy" => Some(json!(self.referer_policy)),
            "security_level" => Some(json!(self.security_level)),
            "block_pings" => Some(json!(self.block_pings)),
            "block_remote_fonts" => Some(json!(self.block_remote_fonts)),
            "block_prefetch" => Some(json!(self.block_prefetch)),
            "block_third_party_ws" => Some(json!(self.block_third_party_ws)),
            "strip_tracking_params" => Some(json!(self.strip_tracking_params)),
            "domain_blocklist" => Some(json!(self.domain_blocklist)),
            _ => None,
        }
    }
}

/// Mutable half: per-scope downgrade marks, session/persisted
/// allowances and the consume-once blocked-navigation registries.
/// Everything bounded — a hostile page cannot grow any set without
/// limit, and a full set fails the bookkeeping, never the block.
struct PolicyState {
    /// scope -> host -> mark time (ms since epoch).
    downgraded: HashMap<String, HashMap<String, i64>>,
    downgrade_ttl_ms: i64,
    session_http_allowed: HashSet<String>,
    persisted_http_allowed: HashSet<String>,
    session_blocked_allowed: HashSet<String>,
    blocked_http_navs: HashSet<String>,
    blocked_domain_navs: HashSet<String>,
}

pub struct Policy {
    snapshot: RwLock<Snapshot>,
    state: Mutex<PolicyState>,
}

static POLICY: OnceLock<Policy> = OnceLock::new();

impl Policy {
    fn new() -> Self {
        Policy {
            snapshot: RwLock::new(Snapshot::default()),
            state: Mutex::new(PolicyState {
                downgraded: HashMap::new(),
                downgrade_ttl_ms: 30 * 60 * 1000,
                session_http_allowed: HashSet::new(),
                persisted_http_allowed: HashSet::new(),
                session_blocked_allowed: HashSet::new(),
                blocked_http_navs: HashSet::new(),
                blocked_domain_navs: HashSet::new(),
            }),
        }
    }

    fn snapshot(&self) -> RwLockReadGuard<'_, Snapshot> {
        self.snapshot.read().unwrap_or_else(|e| e.into_inner())
    }

    fn set_snapshot(&self, snap: Snapshot) {
        let mut guard = self.snapshot.write().unwrap_or_else(|e| e.into_inner());
        *guard = snap;
    }

    fn state(&self) -> MutexGuard<'_, PolicyState> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

fn policy() -> &'static Policy {
    POLICY.get_or_init(Policy::new)
}

// ---- minimal URL decomposition ----------------------------------------
// The manifest carries QUrl::toEncoded() output — already normalized
// (lowercase scheme/host, default ports dropped).  The policy needs
// scheme/host/port/query presence only; strings stay byte-verbatim so
// redirects preserve the engine's exact spelling.

/// Scheme text before the first ':', lowercased.  "" when absent.
fn url_scheme(url: &str) -> &str {
    match url.find(':') {
        Some(i) => &url[..i],
        None => "",
    }
}

/// authority = between "://" and the next '/', '?' or '#'.
fn url_authority(url: &str) -> &str {
    let start = match url.find("://") {
        Some(i) => i + 3,
        None => return "",
    };
    let rest = &url[start..];
    let end = rest
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(rest.len());
    &rest[..end]
}

/// Host portion of the authority: userinfo stripped, :port stripped
/// (bracket-aware for IPv6 literals), brackets dropped, lowercased —
/// the same shape QUrl::host() hands the C++ callers minus the
/// brackets they also strip before comparing.
fn url_host(url: &str) -> String {
    let authority = url_authority(url);
    let hostport = match authority.rfind('@') {
        Some(i) => &authority[i + 1..],
        None => authority,
    };
    let host = if hostport.starts_with('[') {
        match hostport.find(']') {
            Some(i) => &hostport[1..i],
            None => hostport,
        }
    } else {
        match hostport.rfind(':') {
            Some(i) => &hostport[..i],
            None => hostport,
        }
    };
    host.to_lowercase()
}

/// scheme's default port — used to drop ":default" like QUrl does.
fn default_port(scheme: &str) -> Option<u16> {
    match scheme {
        "http" | "ws" => Some(80),
        "https" | "wss" => Some(443),
        _ => None,
    }
}

/// scheme://host[:port]/ — the "origin" a trimmed Referer reduces to.
/// Userinfo is dropped (QUrl rebuilds a bare authority the same way)
/// and a port equal to the scheme's default disappears — QUrl::port()
/// reports -1 for it.
fn referer_origin(url: &str) -> String {
    let scheme = url_scheme(url).to_lowercase();
    let authority = url_authority(url);
    let hostport = match authority.rfind('@') {
        Some(i) => &authority[i + 1..],
        None => authority,
    };
    let mut out = hostport.to_string();
    // Drop an explicit default port — the C++ path only calls
    // setPort() when QUrl::port() != -1.
    if !hostport.starts_with('[') {
        if let Some(i) = hostport.rfind(':') {
            if let Ok(port) = hostport[i + 1..].parse::<u16>() {
                if default_port(&scheme) == Some(port) {
                    out.truncate(i);
                }
            }
        }
    } else if let Some(close) = hostport.find(']') {
        let tail = &hostport[close + 1..];
        if let Some(digits) = tail.strip_prefix(':') {
            if let Ok(port) = digits.parse::<u16>() {
                if default_port(&scheme) == Some(port) {
                    out.truncate(close + 1);
                }
            }
        }
    }
    format!("{}://{}/", scheme, out)
}

/// http -> https scheme swap, everything else byte-identical (the
/// post-swap QUrl::setScheme equivalent; a surviving port is by
/// construction non-default and stays).
fn scheme_https(url: &str) -> String {
    match url.find(':') {
        Some(i) => format!("https{}", &url[i..]),
        None => url.to_string(),
    }
}

fn url_has_query(url: &str) -> bool {
    match url.find('?') {
        Some(i) => url.find('#').map(|h| i < h).unwrap_or(true),
        None => false,
    }
}

fn is_web_request_scheme(scheme: &str) -> bool {
    matches!(scheme, "http" | "https" | "ws" | "wss")
}

// ---- host classifiers ---------------------------------------------------

/// Parse a host string to an IpAddr, or None for a hostname.  Strict
/// dotted-quad IPv4 and std IPv6 (bracket-free — url_host() already
/// strips them) — what QHostAddress recognizes.
fn parse_ip(host: &str) -> Option<IpAddr> {
    host.parse::<Ipv4Addr>()
        .map(IpAddr::V4)
        .ok()
        .or_else(|| host.parse::<Ipv6Addr>().map(IpAddr::V6).ok())
}

fn in_v4_subnet(addr: Ipv4Addr, base: Ipv4Addr, bits: u32) -> bool {
    let mask = if bits == 0 { 0 } else { u32::MAX << (32 - bits) };
    (u32::from(addr) & mask) == (u32::from(base) & mask)
}

fn in_v6_subnet(addr: Ipv6Addr, base: Ipv6Addr, bits: u32) -> bool {
    let mask = if bits == 0 { 0 } else { u128::MAX << (128 - bits) };
    (u128::from(addr) & mask) == (u128::from(base) & mask)
}

/// DLACC04/SAFE01 boundary: loopback, private/link-local LAN space,
/// .localhost/.local/.onion — hosts where a plaintext http service is
/// legitimate and the https upgrade must not fire.  Mirrors the C++
/// QHostAddress table 1:1.
fn is_private_or_local_host(host: &str) -> bool {
    if host.is_empty() {
        return true;
    }
    let lowered = host.to_lowercase();
    if lowered == "localhost"
        || lowered.ends_with(".localhost")
        || lowered.ends_with(".local")
        || lowered.ends_with(".onion")
    {
        return true;
    }
    let addr = match parse_ip(&lowered) {
        Some(a) => a,
        None => return false, // hostname — public candidate
    };
    match addr {
        IpAddr::V4(v4) => {
            if v4.is_loopback() || v4.is_multicast() {
                return true;
            }
            const NETS: &[(u8, u8, u8, u8, u32)] = &[
                (10, 0, 0, 0, 8),
                (172, 16, 0, 0, 12),
                (192, 168, 0, 0, 16),
                (169, 254, 0, 0, 16),
                (100, 64, 0, 0, 10),  // CGNAT
                (198, 18, 0, 0, 15),  // benchmarking
            ];
            for &(a, b, c, d, bits) in NETS {
                if in_v4_subnet(v4, Ipv4Addr::new(a, b, c, d), bits) {
                    return true;
                }
            }
            false
        }
        IpAddr::V6(v6) => {
            if v6.is_loopback() || v6.is_multicast() {
                return true;
            }
            in_v6_subnet(v6, "fc00::".parse().unwrap(), 7)
                || in_v6_subnet(v6, "fe80::".parse().unwrap(), 10)
        }
    }
}

/// Narrower than is_private_or_local_host: only a real loopback page
/// is a "potentially trustworthy" secure context — LAN hosts keep the
/// Safer script block.
fn is_loopback_host(host: &str) -> bool {
    let lowered = host.to_lowercase();
    if lowered == "localhost" || lowered.ends_with(".localhost") {
        return true;
    }
    match parse_ip(&lowered) {
        Some(ip) => ip.is_loopback(),
        None => false,
    }
}

// ---- shared predicates ---------------------------------------------------

/// Same last-two-labels approximation AdBlockRule uses for its
/// first-party test — no public-suffix list, so unrelated hosts under
/// multi-level public suffixes read as same-party.
fn same_site(a: &str, b: &str) -> bool {
    if a.is_empty() || b.is_empty() {
        return a == b;
    }
    let la = a.to_lowercase();
    let lb = b.to_lowercase();
    if la == lb
        || la.ends_with(&format!(".{}", lb))
        || lb.ends_with(&format!(".{}", la))
    {
        return true;
    }
    let base = |s: &str| -> String {
        let parts: Vec<&str> = s.split('.').collect();
        if parts.len() > 2 {
            parts[parts.len() - 2..].join(".")
        } else {
            s.to_string()
        }
    };
    base(&la) == base(&lb)
}

/// CookieJar::isOnDomainList — exact, ".suffix" or bare-suffix match.
fn on_domain_list(rules: &[String], domain: &str) -> bool {
    for rule in rules {
        if let Some(without_dot) = rule.strip_prefix('.') {
            if domain.ends_with(rule.as_str()) || domain == without_dot {
                return true;
            }
        } else if domain.ends_with(&format!(".{}", rule)) || domain == rule {
            return true;
        }
    }
    false
}

/// PING01/PING02: sendBeacon, <a ping> audits, CSP report uploads and
/// any engine-marked Reporting-API delivery (Sec-Fetch-Dest: report).
fn is_ping_telemetry(resource_type: i64, headers: &[(String, String)]) -> bool {
    resource_type == RT_PING
        || resource_type == RT_CSP_REPORT
        || headers.iter().any(|(k, v)| k == "Sec-Fetch-Dest" && v == "report")
}

/// SAFE04: opt-in remote-font block (ResourceTypeFontResource) and the
/// default-on prefetch block — typed prefetch loads plus the
/// speculation-rules navigations Chromium mislabels as MainFrame but
/// still marks with Purpose/Sec-Purpose: prefetch.
fn should_block_resource(
    snap: &Snapshot,
    resource_type: i64,
    headers: &[(String, String)],
) -> bool {
    if resource_type == RT_FONT {
        return snap.block_remote_fonts;
    }
    if !snap.block_prefetch {
        return false;
    }
    if resource_type == RT_PREFETCH {
        return true;
    }
    headers.iter().any(|(k, v)| {
        let key = k.to_lowercase();
        (key == "sec-purpose" || key == "purpose")
            && v.to_lowercase().contains("prefetch")
    })
}

/// XSLEAK03: opt-in cross-site WebSocket block (the upgrade request is
/// interceptor-visible; a request with no first-party context has no
/// page it could be third-party to).
fn should_block_ws(
    snap: &Snapshot,
    first_party_url: &str,
    request_url: &str,
    resource_type: i64,
) -> bool {
    if resource_type != RT_WEBSOCKET || !snap.block_third_party_ws {
        return false;
    }
    let fp_host = url_host(first_party_url);
    if fp_host.is_empty() {
        return false;
    }
    !same_site(&fp_host, &url_host(request_url))
}

/// SECLVL Safer+: script-execution fetches on insecure http pages —
/// external scripts, workers, service workers (loopback exempt; a
/// JSCTL Allow grant beats the tier).
fn should_block_script(
    snap: &Snapshot,
    first_party_url: &str,
    resource_type: i64,
    script_allowed: bool,
) -> bool {
    if snap.security_level < SECURITY_SAFER {
        return false;
    }
    if !matches!(
        resource_type,
        RT_SCRIPT | RT_WORKER | RT_SHARED_WORKER | RT_SERVICE_WORKER
    ) {
        return false;
    }
    if url_scheme(first_party_url).to_lowercase() != "http" {
        return false;
    }
    if script_allowed {
        return false;
    }
    !is_loopback_host(&url_host(first_party_url))
}

// ---- downgrade marks (SAFE07) -------------------------------------------

fn is_marked_downgraded(state: &mut PolicyState, scope: &str, host: &str) -> bool {
    let marks = match state.downgraded.get_mut(scope) {
        Some(m) => m,
        None => return false,
    };
    let key = host.to_lowercase();
    match marks.get(&key) {
        Some(&mark_ms) => {
            if now_ms() - mark_ms > state.downgrade_ttl_ms {
                marks.remove(&key);
                false
            } else {
                true
            }
        }
        None => false,
    }
}

/// Refresh/insert path — returns true when this call newly marked the
/// host (a refresh extends the re-probe window without counting as
/// new; a full set fails closed).
fn mark_host_downgraded(state: &mut PolicyState, scope: &str, host: &str) -> bool {
    let marks = state.downgraded.entry(scope.to_string()).or_default();
    if let Some(mark) = marks.get_mut(host) {
        *mark = now_ms();
        return false;
    }
    if marks.len() >= MAX_DOWNGRADED_HOSTS {
        return false;
    }
    marks.insert(host.to_string(), now_ms());
    true
}

/// SAFE07 error gate: only genuine connection/TLS failures say
/// anything about the origin's TLS.  The net_error code table is
/// verbatim from the C++ — resolver faults, local socket state,
/// throttles, client-cert/pinning enforcement and proxy plumbing are
/// all excluded.
fn failure_implies_downgrade(error_domain: i64, error_code: i64) -> bool {
    if error_domain != CONNECTION_ERROR_DOMAIN {
        return false;
    }
    !matches!(
        error_code,
        -105 | -106 | -108 | -110 | -111 | -115 | -117 | -119 | -120 | -121
            | -124 | -127 | -130 | -131 | -133 | -134 | -135 | -136 | -137
            | -138 | -139 | -140 | -141 | -142 | -145 | -147 | -150 | -151
            | -154 | -156 | -160 | -161 | -162 | -163 | -164 | -166 | -168
            | -169 | -170 | -171 | -173 | -174 | -176 | -177 | -178 | -179
            | -186 | -187 | -188 | -189 | -190
    )
}

// ---- https-first / https-only -------------------------------------------

fn is_upgrade_candidate(state: &mut PolicyState, url: &str, scope: &str) -> bool {
    if url_scheme(url).to_lowercase() != "http" {
        return false;
    }
    let host = url_host(url);
    if is_private_or_local_host(&host) {
        return false;
    }
    !is_marked_downgraded(state, scope, &host)
}

fn should_warn_http(
    snap: &Snapshot,
    state: &mut PolicyState,
    url: &str,
    scope: &str,
) -> bool {
    if !snap.https_only || url_scheme(url).to_lowercase() != "http" {
        return false;
    }
    let host = url_host(url);
    if host.is_empty() || is_private_or_local_host(&host) {
        return false;
    }
    if state.session_http_allowed.contains(&host)
        || state.persisted_http_allowed.contains(&host)
    {
        return false;
    }
    // The https-first upgrade claims upgradeable hosts first — only a
    // host that can no longer be upgraded away still warns.
    if snap.https_first && is_upgrade_candidate(state, url, scope) {
        return false;
    }
    true
}

fn should_warn_form_post(
    snap: &Snapshot,
    state: &mut PolicyState,
    url: &str,
    scope: &str,
) -> bool {
    if url_scheme(url).to_lowercase() != "http" {
        return false;
    }
    let host = url_host(url);
    if host.is_empty() || is_private_or_local_host(&host) {
        return false;
    }
    if snap.https_first && is_upgrade_candidate(state, url, scope) {
        return false;
    }
    true
}

// ---- referer (REF01) -----------------------------------------------------

/// The Referer value that should go on the wire — EngineDefault keeps
/// source verbatim, Never clears it, the strict-origin downgrade rule
/// clears https->http everywhere, cross-site is spoofed to the
/// target's own origin at Trimmed and cleared at Strict, and same-site
/// always collapses to the source's bare origin.
fn rewritten_referer(level: i64, source: &str, target: &str) -> String {
    if level <= REFERER_ENGINE_DEFAULT {
        return source.to_string();
    }
    if level == REFERER_NEVER {
        return String::new();
    }
    if url_scheme(source).to_lowercase() == "https"
        && url_scheme(target).to_lowercase() == "http"
    {
        return String::new();
    }
    let cross_site = !same_site(&url_host(source), &url_host(target));
    if cross_site {
        if level >= REFERER_STRICT {
            return String::new();
        }
        return referer_origin(target);
    }
    referer_origin(source)
}

fn referrer_meta_value(level: i64) -> &'static str {
    match level {
        REFERER_TRIMMED => "strict-origin",
        REFERER_STRICT => "same-origin",
        REFERER_NEVER => "no-referrer",
        _ => "",
    }
}

fn header_value<'a>(headers: &'a [(String, String)], name: &str) -> Option<&'a str> {
    headers
        .iter()
        .find(|(k, _)| k == name)
        .map(|(_, v)| v.as_str())
}

/// The referer stage shared by both pipeline modes.  An absent or
/// non-http referer stays untouched — a hardening feature never
/// synthesizes a header the sender asked to withhold.
fn referer_directive(
    snap: &Snapshot,
    url: &str,
    headers: &[(String, String)],
    min_level: i64,
) -> Value {
    let level = snap.referer_policy.max(min_level);
    if level == REFERER_ENGINE_DEFAULT {
        return json!({"op": "keep"});
    }
    let scheme = url_scheme(url).to_lowercase();
    if scheme != "http" && scheme != "https" {
        return json!({"op": "keep"});
    }
    let referer = match header_value(headers, "Referer") {
        Some(v) if !v.is_empty() => v,
        _ => return json!({"op": "keep"}),
    };
    let ref_scheme = url_scheme(referer).to_lowercase();
    if ref_scheme != "http" && ref_scheme != "https" {
        return json!({"op": "keep"});
    }
    json!({"op": "set", "value": rewritten_referer(level, referer, url)})
}

// ---- domain blocklist (SEC18) --------------------------------------------

fn is_domain_blocked(host: &str) -> bool {
    if host.is_empty() {
        return false;
    }
    blocklist::check(host)
}

fn should_block_domain(snap: &Snapshot, state: &PolicyState, url: &str) -> bool {
    if !snap.domain_blocklist {
        return false;
    }
    let scheme = url_scheme(url).to_lowercase();
    if scheme != "http" && scheme != "https" {
        return false;
    }
    let host = url_host(url);
    if host.is_empty() || state.session_blocked_allowed.contains(&host) {
        return false;
    }
    is_domain_blocked(&host)
}

fn record_blocked_nav(state: &mut PolicyState, domain: bool, url: &str) {
    let set = if domain {
        &mut state.blocked_domain_navs
    } else {
        &mut state.blocked_http_navs
    };
    // A flooded set cannot keep the interstitial mapping — the block
    // still happens, the page just falls back to the generic error.
    if set.len() < MAX_BLOCKED_NAVS {
        set.insert(url.to_string());
    }
}

// ---- manifest --------------------------------------------------------------

/// One outgoing request as the policy sees it — the engine adapter's
/// marshaled view; nothing Qt-specific may appear here.
pub struct Request {
    pub url: String,
    pub first_party_url: String,
    pub resource_type: i64,
    pub method: String,
    pub headers: Vec<(String, String)>,
    pub scope: String,
    pub tor_mode: bool,
    pub script_allowed: bool,
    pub min_referer_level: i64,
}

impl Request {
    fn from_json(v: &Value) -> Option<Request> {
        let url = v.get("url")?.as_str()?.to_string();
        let headers = v
            .get("headers")
            .and_then(|h| h.as_array())
            .map(|arr| {
                arr.iter()
                    .filter_map(|p| {
                        let pair = p.as_array()?;
                        Some((
                            pair.first()?.as_str()?.to_string(),
                            pair.get(1)?.as_str()?.to_string(),
                        ))
                    })
                    .collect()
            })
            .unwrap_or_default();
        Some(Request {
            url,
            first_party_url: v
                .get("first_party_url")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string(),
            resource_type: v
                .get("resource_type")
                .and_then(|x| x.as_i64())
                .unwrap_or(255),
            method: v
                .get("method")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string(),
            headers,
            scope: v
                .get("scope")
                .and_then(|x| x.as_str())
                .unwrap_or("")
                .to_string(),
            tor_mode: v
                .get("tor_mode")
                .and_then(|x| x.as_bool())
                .unwrap_or(false),
            script_allowed: v
                .get("script_allowed")
                .and_then(|x| x.as_bool())
                .unwrap_or(false),
            min_referer_level: v
                .get("min_referer_level")
                .and_then(|x| x.as_i64())
                .unwrap_or(0),
        })
    }
}

// ---- the pipeline --------------------------------------------------------------

fn block(reason: &str) -> Value {
    json!({"action": "block", "reason": reason})
}

fn redirect(url: String, reason: &str) -> Value {
    json!({"action": "redirect", "url": url, "reason": reason})
}

/// The strip+upgrade merge both modes share: a tracked URL and an
/// upgradable one still cost a single redirect between them.
fn strip_target(snap: &Snapshot, req: &Request) -> String {
    if !snap.strip_tracking_params
        || (req.method != "GET" && req.method != "HEAD")
        || !url_has_query(&req.url)
    {
        return req.url.clone();
    }
    let scheme = url_scheme(&req.url).to_lowercase();
    if scheme != "http" && scheme != "https" {
        return req.url.clone();
    }
    urlstrip::strip(&req.url)
}

fn evaluate_privacy(snap: &Snapshot, req: &Request) -> Value {
    // PING01: telemetry-shaped uploads die first.
    if snap.block_pings && is_ping_telemetry(req.resource_type, &req.headers) {
        return block("ping");
    }
    // SAFE04: remote fonts + prefetch.
    if should_block_resource(snap, req.resource_type, &req.headers) {
        return block("resource");
    }
    // XSLEAK03: opt-in cross-site WebSocket.
    if should_block_ws(
        snap,
        &req.first_party_url,
        &req.url,
        req.resource_type,
    ) {
        return block("websocket");
    }
    // SEC18: listed hostile domains die before the redirect stages —
    // no point paying a redirect into a domain the next hop refuses.
    if req.resource_type == RT_MAIN_FRAME
        && should_block_domain(snap, &policy().state(), &req.url)
    {
        let mut state = policy().state();
        record_blocked_nav(&mut state, true, &req.url);
        return block("domain");
    }
    let mut state = policy().state();
    // SEC17 + SAFE01 merge: strip fires on GET/HEAD, the https-first
    // upgrade takes a main-frame http candidate — one redirect either
    // way.
    let mut target = strip_target(snap, req);
    if snap.https_first
        && req.resource_type == RT_MAIN_FRAME
        && is_upgrade_candidate(&mut state, &req.url, &req.scope)
    {
        target = scheme_https(&target);
        return redirect(target, "https-first");
    }
    if target != req.url {
        return redirect(target, "strip");
    }
    // SAFE01: an http: main frame still standing after the upgrade
    // pass is refused; WebPage consumes the recorded refusal for the
    // warning interstitial.
    if req.resource_type == RT_MAIN_FRAME
        && should_warn_http(snap, &mut state, &req.url, &req.scope)
    {
        record_blocked_nav(&mut state, false, &req.url);
        return block("https-only");
    }
    // SECLVL Safer+: script fetches on insecure http pages die before
    // the referer stage — a dead request needs no header surgery.
    if should_block_script(
        snap,
        &req.first_party_url,
        req.resource_type,
        req.script_allowed,
    ) {
        return block("script");
    }
    json!({
        "action": "allow",
        "referer": referer_directive(snap, &req.url, &req.headers, req.min_referer_level),
    })
}

fn evaluate_tor(snap: &Snapshot, req: &Request) -> Value {
    // SEC18: the blocklist applies under tor too — refuse listed
    // clearnet hosts before any redirect stage.
    if req.resource_type == RT_MAIN_FRAME
        && should_block_domain(snap, &policy().state(), &req.url)
    {
        let mut state = policy().state();
        record_blocked_nav(&mut state, true, &req.url);
        return block("domain");
    }
    // SEC17 + tor upgrade merge: tracked clearnet URLs strip, every
    // clearnet http: request upgrades (not just navigations); .onion
    // http: keeps its scheme and still strips.
    let mut target = strip_target(snap, req);
    let host = url_host(&req.url);
    if url_scheme(&req.url).to_lowercase() == "http" && !host.ends_with(".onion") {
        target = scheme_https(&target);
        return redirect(target, "upgrade");
    }
    if target != req.url {
        return redirect(target, "strip");
    }
    // PING01 / SAFE04 / XSLEAK03 — the shared toggles apply in tor
    // windows identically.
    if snap.block_pings && is_ping_telemetry(req.resource_type, &req.headers) {
        return block("ping");
    }
    if should_block_resource(snap, req.resource_type, &req.headers) {
        return block("resource");
    }
    if should_block_ws(
        snap,
        &req.first_party_url,
        &req.url,
        req.resource_type,
    ) {
        return block("websocket");
    }
    // SECLVL: the only http: left is .onion — Safer drops its script
    // fetches like everywhere else.
    if should_block_script(
        snap,
        &req.first_party_url,
        req.resource_type,
        req.script_allowed,
    ) {
        return block("script");
    }
    json!({
        "action": "allow",
        "referer": referer_directive(snap, &req.url, &req.headers, req.min_referer_level),
    })
}

/// The engine-agnostic verdict: a non-web scheme passes through
/// untouched (STALL01), everything else runs the mode's pipeline.
/// "allow" hands the request to the adapter's delegated matcher tail
/// (the adblock rule engine — its own pluggable policy stage).
pub fn evaluate_json(manifest: &Value) -> Value {
    let req = match Request::from_json(manifest) {
        Some(r) => r,
        None => return json!({"action": "pass"}),
    };
    let scheme = url_scheme(&req.url).to_lowercase();
    if !is_web_request_scheme(&scheme) {
        return json!({"action": "pass"});
    }
    let snap = policy().snapshot().clone();
    if req.tor_mode {
        evaluate_tor(&snap, &req)
    } else {
        evaluate_privacy(&snap, &req)
    }
}

// ---- cookie gate ------------------------------------------------------------

/// CookieJar's filter, verbatim: block list wins; a third-party hit
/// still needs an explicit allow when block-third-party is armed; the
/// accept policy then decides for everyone else.
pub fn cookie_filter_json(v: &Value) -> Option<bool> {
    let host = v.get("host")?.as_str()?;
    let third_party = v
        .get("third_party")
        .and_then(|x| x.as_bool())
        .unwrap_or(false);
    let block_3p = v
        .get("block_3p")
        .and_then(|x| x.as_bool())
        .unwrap_or(false);
    let accept_policy = v
        .get("accept_policy")
        .and_then(|x| x.as_i64())
        .unwrap_or(COOKIE_ACCEPT_FIRST_PARTY);
    let list = |key: &str| -> Vec<String> {
        v.get(key)
            .and_then(|x| x.as_array())
            .map(|a| {
                a.iter()
                    .filter_map(|s| s.as_str().map(String::from))
                    .collect()
            })
            .unwrap_or_default()
    };
    let block = list("block");
    let allow = list("allow");
    let allow_session = list("allow_session");

    let blocked = on_domain_list(&block, host);
    let allowed = !blocked && on_domain_list(&allow, host);
    let allowed_session = !blocked && !allowed && on_domain_list(&allow_session, host);
    if blocked {
        return Some(false);
    }
    if third_party && block_3p && !allowed && !allowed_session {
        return Some(false);
    }
    Some(match accept_policy {
        COOKIE_ACCEPT_ALWAYS => true,
        COOKIE_ACCEPT_NEVER => allowed || allowed_session,
        _ => allowed || allowed_session || !third_party,
    })
}

// ---- snapshot + op surface ----------------------------------------------------

/// Pushes a new policy snapshot (GUI thread).  The https-only
/// exception list refreshes the persisted-allowance set here — the
/// session additions keep their own storage.
pub fn load_snapshot_json(v: &Value) -> RcResult<()> {
    let mut snap = Snapshot::default();
    if let Some(b) = v.get("https_first").and_then(|x| x.as_bool()) {
        snap.https_first = b;
    }
    if let Some(b) = v.get("https_only").and_then(|x| x.as_bool()) {
        snap.https_only = b;
    }
    if let Some(n) = v.get("referer_policy").and_then(|x| x.as_i64()) {
        snap.referer_policy = n;
    }
    if let Some(n) = v.get("security_level").and_then(|x| x.as_i64()) {
        snap.security_level = n;
    }
    if let Some(b) = v.get("block_pings").and_then(|x| x.as_bool()) {
        snap.block_pings = b;
    }
    if let Some(b) = v.get("block_remote_fonts").and_then(|x| x.as_bool()) {
        snap.block_remote_fonts = b;
    }
    if let Some(b) = v.get("block_prefetch").and_then(|x| x.as_bool()) {
        snap.block_prefetch = b;
    }
    if let Some(b) = v.get("block_third_party_ws").and_then(|x| x.as_bool()) {
        snap.block_third_party_ws = b;
    }
    if let Some(b) = v.get("strip_tracking_params").and_then(|x| x.as_bool()) {
        snap.strip_tracking_params = b;
    }
    if let Some(b) = v.get("domain_blocklist").and_then(|x| x.as_bool()) {
        snap.domain_blocklist = b;
    }
    policy().set_snapshot(snap);
    if let Some(list) = v.get("https_only_exceptions").and_then(|x| x.as_array()) {
        let mut state = policy().state();
        state.persisted_http_allowed = list
            .iter()
            .filter_map(|s| s.as_str().map(str::to_lowercase))
            .filter(|h| !h.is_empty())
            .collect();
    }
    Ok(())
}

/// The granular decision/state surface the C++ statics delegate to —
/// one JSON-in/JSON-out endpoint keeps the ABI to a handful of entry
/// points while every historical decision point stays reachable for
/// callers and tests.
pub fn call_json(v: &Value) -> RcResult<Value> {
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("");
    let arg_str = |key: &str| -> String {
        v.get(key)
            .and_then(|x| x.as_str())
            .unwrap_or("")
            .to_string()
    };
    let arg_i64 = |key: &str| -> i64 { v.get(key).and_then(|x| x.as_i64()).unwrap_or(0) };
    let arg_bool = |key: &str| -> bool {
        v.get(key).and_then(|x| x.as_bool()).unwrap_or(false)
    };
    let arg_headers = || -> Vec<(String, String)> {
        v.get("headers")
            .and_then(|h| h.as_array())
            .map(|arr| {
                arr.iter()
                    .filter_map(|p| {
                        let pair = p.as_array()?;
                        Some((
                            pair.first()?.as_str()?.to_string(),
                            pair.get(1)?.as_str()?.to_string(),
                        ))
                    })
                    .collect()
            })
            .unwrap_or_default()
    };
    let snap = policy().snapshot().clone();
    match op {
        "flag" => match snap.flag(&arg_str("name")) {
            Some(val) => Ok(val),
            None => fail(RcStatus::InvalidArgument, "unknown policy flag"),
        },
        "referrer_meta" => Ok(json!(referrer_meta_value(arg_i64("level")))),
        "rewritten_referer" => {
            Ok(json!(rewritten_referer(
                arg_i64("level"),
                &arg_str("source"),
                &arg_str("target"),
            )))
        }
        "referer_apply" => Ok(referer_directive(
            &snap,
            &arg_str("url"),
            &arg_headers(),
            arg_i64("min_level"),
        )),
        "upgrade_candidate" => {
            let mut state = policy().state();
            Ok(json!(is_upgrade_candidate(
                &mut state,
                &arg_str("url"),
                &arg_str("scope"),
            )))
        }
        "warn_http" => {
            let mut state = policy().state();
            Ok(json!(should_warn_http(
                &snap,
                &mut state,
                &arg_str("url"),
                &arg_str("scope"),
            )))
        }
        "warn_form_post" => {
            let mut state = policy().state();
            Ok(json!(should_warn_form_post(
                &snap,
                &mut state,
                &arg_str("url"),
                &arg_str("scope"),
            )))
        }
        "is_http_allowed" => {
            let host = arg_str("host").to_lowercase();
            let state = policy().state();
            Ok(json!(
                state.session_http_allowed.contains(&host)
                    || state.persisted_http_allowed.contains(&host)
            ))
        }
        // {"persist_needed": bool} — the caller writes QSettings when
        // a persistent grant isn't already listed; the policy keeps
        // only the host sets.
        "allow_http" => {
            let host = arg_str("host").to_lowercase();
            if host.is_empty() {
                return Ok(json!({"persist_needed": false}));
            }
            let persistent = arg_bool("persistent");
            let mut state = policy().state();
            if state.session_http_allowed.len() < MAX_HTTP_ALLOWED_HOSTS {
                state.session_http_allowed.insert(host.clone());
            }
            if !persistent {
                return Ok(json!({"persist_needed": false}));
            }
            let already = state.persisted_http_allowed.contains(&host);
            state.persisted_http_allowed.insert(host);
            Ok(json!({"persist_needed": !already}))
        }
        "clear_http_allowance" => {
            let host = arg_str("host").to_lowercase();
            let mut state = policy().state();
            state.session_http_allowed.remove(&host);
            state.persisted_http_allowed.remove(&host);
            Ok(Value::Null)
        }
        "http_exceptions" => {
            let state = policy().state();
            let mut hosts: Vec<&String> = state.persisted_http_allowed.iter().collect();
            hosts.sort();
            Ok(json!(hosts))
        }
        "is_downgraded" => {
            let mut state = policy().state();
            Ok(json!(is_marked_downgraded(
                &mut state,
                &arg_str("scope"),
                &arg_str("host"),
            )))
        }
        "mark_downgraded" => {
            let host = arg_str("host").to_lowercase();
            if !host.is_empty() && !is_private_or_local_host(&host) {
                let mut state = policy().state();
                mark_host_downgraded(&mut state, &arg_str("scope"), &host);
            }
            Ok(Value::Null)
        }
        "clear_downgraded" => {
            let host = arg_str("host").to_lowercase();
            let mut state = policy().state();
            if let Some(marks) = state.downgraded.get_mut(&arg_str("scope")) {
                marks.remove(&host);
            }
            Ok(Value::Null)
        }
        "clear_all_downgraded" => {
            policy().state().downgraded.clear();
            Ok(Value::Null)
        }
        "ttl_get" => Ok(json!(policy().state().downgrade_ttl_ms)),
        "ttl_set" => {
            policy().state().downgrade_ttl_ms = arg_i64("ms");
            Ok(Value::Null)
        }
        "failure_implies_downgrade" => Ok(json!(failure_implies_downgrade(
            arg_i64("domain"),
            arg_i64("code"),
        ))),
        "note_nav_failure" => {
            let url = arg_str("url");
            let domain = arg_i64("domain");
            let code = arg_i64("code");
            let scope = arg_str("scope");
            if !snap.https_first
                || url_scheme(&url).to_lowercase() != "https"
                || !failure_implies_downgrade(domain, code)
            {
                return Ok(json!(false));
            }
            let host = url_host(&url);
            if host.is_empty() || is_private_or_local_host(&host) {
                return Ok(json!(false));
            }
            let mut state = policy().state();
            Ok(json!(mark_host_downgraded(&mut state, &scope, &host)))
        }
        "record_blocked_nav" => {
            let mut state = policy().state();
            record_blocked_nav(&mut state, arg_str("kind") == "domain", &arg_str("url"));
            Ok(Value::Null)
        }
        "take_blocked_nav" => {
            let url = arg_str("url");
            let mut state = policy().state();
            let set = if arg_str("kind") == "domain" {
                &mut state.blocked_domain_navs
            } else {
                &mut state.blocked_http_navs
            };
            Ok(json!(set.remove(&url)))
        }
        "block_domain" => {
            let state = policy().state();
            Ok(json!(should_block_domain(&snap, &state, &arg_str("url"))))
        }
        "is_domain_blocked" => Ok(json!(is_domain_blocked(&arg_str("host")))),
        "is_blocked_domain_allowed" => {
            let host = arg_str("host").to_lowercase();
            Ok(json!(
                policy().state().session_blocked_allowed.contains(&host)
            ))
        }
        "allow_blocked_domain" => {
            let host = arg_str("host").to_lowercase();
            if !host.is_empty() {
                let mut state = policy().state();
                if state.session_blocked_allowed.len() < MAX_BLOCKED_ALLOWED_HOSTS {
                    state.session_blocked_allowed.insert(host);
                }
            }
            Ok(Value::Null)
        }
        "clear_blocked_domain_allowance" => {
            policy()
                .state()
                .session_blocked_allowed
                .remove(&arg_str("host").to_lowercase());
            Ok(Value::Null)
        }
        "clear_all_blocked_domain" => {
            let mut state = policy().state();
            state.session_blocked_allowed.clear();
            state.blocked_domain_navs.clear();
            Ok(Value::Null)
        }
        "is_ping" => Ok(json!(is_ping_telemetry(
            arg_i64("type"),
            &arg_headers(),
        ))),
        "block_resource" => Ok(json!(should_block_resource(
            &snap,
            arg_i64("type"),
            &arg_headers(),
        ))),
        "block_ws" => Ok(json!(should_block_ws(
            &snap,
            &arg_str("first_party_url"),
            &arg_str("url"),
            arg_i64("type"),
        ))),
        "block_script" => Ok(json!(should_block_script(
            &snap,
            &arg_str("first_party_url"),
            arg_i64("type"),
            arg_bool("script_allowed"),
        ))),
        "private_or_local" => Ok(json!(is_private_or_local_host(&arg_str("host")))),
        _ => fail(RcStatus::InvalidArgument, "unknown policy op"),
    }
}

/// JSON parse helper for the FFI layer — a malformed manifest is an
/// argument error, never a panic.
pub fn parse_json(bytes: &[u8]) -> RcResult<Value> {
    let text = std::str::from_utf8(bytes).map_err(|_| Fail {
        status: RcStatus::Corrupt,
        msg: "policy manifest: not UTF-8".into(),
    })?;
    serde_json::from_str(text).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("policy manifest: {e}"),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Mutex, MutexGuard};

    // The policy state is global — serialize every test touching it.
    static TEST_LOCK: Mutex<()> = Mutex::new(());
    fn guard() -> MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn reset() {
        load_snapshot_json(&json!({})).unwrap();
        let mut state = policy().state();
        state.downgraded.clear();
        state.session_http_allowed.clear();
        state.persisted_http_allowed.clear();
        state.session_blocked_allowed.clear();
        state.blocked_http_navs.clear();
        state.blocked_domain_navs.clear();
        state.downgrade_ttl_ms = 30 * 60 * 1000;
    }

    fn manifest(url: &str, rtype: i64) -> Value {
        json!({
            "url": url,
            "first_party_url": url,
            "resource_type": rtype,
            "method": "GET",
            "headers": [],
            "scope": "test",
        })
    }

    #[test]
    fn non_web_schemes_pass() {
        let _g = guard();
        reset();
        for url in [
            "file:///etc/passwd",
            "qrc:///start.html",
            "chrome://extensions",
            "data:text/html,<b>",
            "about:blank",
            "arora-file://x",
            "javascript:alert(1)",
        ] {
            let v = evaluate_json(&manifest(url, RT_MAIN_FRAME));
            assert_eq!(v["action"], "pass", "{url}");
        }
    }

    #[test]
    fn pings_and_reports_block() {
        let _g = guard();
        reset();
        let v = evaluate_json(&manifest("https://t.example/ping", RT_PING));
        assert_eq!(v["action"], "block");
        let v = evaluate_json(&manifest("https://t.example/r", RT_CSP_REPORT));
        assert_eq!(v["action"], "block");
        // Sec-Fetch-Dest: report on any type.
        let v = evaluate_json(&json!({
            "url": "https://t.example/x", "resource_type": 13, "method": "POST",
            "headers": [["Sec-Fetch-Dest", "report"]],
        }));
        assert_eq!(v["action"], "block");
        // Toggle off -> they sail through to the allow verdict.
        load_snapshot_json(&json!({"block_pings": false})).unwrap();
        let v = evaluate_json(&manifest("https://t.example/ping", RT_PING));
        assert_eq!(v["action"], "allow");
    }

    #[test]
    fn prefetch_and_font_blocks() {
        let _g = guard();
        reset();
        // Prefetch type blocked by default.
        let v = evaluate_json(&manifest("https://t.example/p", RT_PREFETCH));
        assert_eq!(v["action"], "block");
        // Speculation-rules prefetch mislabeled MainFrame — the header
        // catches it.
        let v = evaluate_json(&json!({
            "url": "https://t.example/next", "resource_type": RT_MAIN_FRAME,
            "method": "GET",
            "headers": [["Sec-Purpose", "prefetch"]],
        }));
        assert_eq!(v["action"], "block");
        // A font is allowed by default, blocked under the opt-in.
        let font = manifest("https://t.example/f.woff2", RT_FONT);
        assert_eq!(evaluate_json(&font)["action"], "allow");
        load_snapshot_json(&json!({"block_remote_fonts": true})).unwrap();
        assert_eq!(evaluate_json(&font)["action"], "block");
    }

    #[test]
    fn https_first_upgrades_public_mainframes() {
        let _g = guard();
        reset();
        let v = evaluate_json(&manifest("http://example.com/path?q=1", RT_MAIN_FRAME));
        assert_eq!(v["action"], "redirect");
        assert_eq!(v["url"], "https://example.com/path?q=1");
        assert_eq!(v["reason"], "https-first");
        // A subresource http: is neither upgraded nor refused —
        // https-first and https-only are both main-frame only.
        let v = evaluate_json(&manifest("http://example.com/img.png", 4));
        assert_eq!(v["action"], "allow");
        // Loopback + LAN + .onion are exempt.
        for url in [
            "http://localhost:8080/x",
            "http://127.0.0.1/x",
            "http://10.1.2.3/x",
            "http://192.168.1.1/x",
            "http://router.local/x",
            "http://abc.onion/x",
        ] {
            let v = evaluate_json(&manifest(url, RT_MAIN_FRAME));
            assert_ne!(v["action"], "redirect", "{url}");
        }
    }

    #[test]
    fn downgrade_mark_suppresses_upgrade_and_warns() {
        let _g = guard();
        reset();
        let url = "http://fails.example/";
        assert!(matches!(
            evaluate_json(&manifest(url, RT_MAIN_FRAME))["action"].as_str(),
            Some("redirect")
        ));
        // A failed https load marks the host — next http nav is not
        // upgraded but refused under https-only.
        let marked = call_json(&json!({
            "op": "note_nav_failure",
            "url": "https://fails.example/",
            "domain": CONNECTION_ERROR_DOMAIN,
            "code": -101, // ERR_CONNECTION_RESET — TLS evidence
            "scope": "test",
        }))
        .unwrap();
        assert_eq!(marked, true);
        let v = evaluate_json(&manifest(url, RT_MAIN_FRAME));
        assert_eq!(v["action"], "block");
        assert_eq!(v["reason"], "https-only");
        // The recorded refusal is consume-once.
        let took = call_json(&json!({
            "op": "take_blocked_nav", "kind": "http", "url": url,
        }))
        .unwrap();
        assert_eq!(took, true);
        let again = call_json(&json!({
            "op": "take_blocked_nav", "kind": "http", "url": url,
        }))
        .unwrap();
        assert_eq!(again, false);
        // TTL expiry re-probes: a zero/negative TTL expires instantly.
        call_json(&json!({"op": "ttl_set", "ms": -1})).unwrap();
        let v = evaluate_json(&manifest(url, RT_MAIN_FRAME));
        assert_eq!(v["action"], "redirect");
    }

    #[test]
    fn http_allowances_exempt_https_only() {
        let _g = guard();
        reset();
        // Downgrade the host so the upgrade pass does not claim it.
        call_json(&json!({
            "op": "mark_downgraded", "host": "ex.example", "scope": "test",
        }))
        .unwrap();
        let url = "http://ex.example/";
        assert_eq!(
            evaluate_json(&manifest(url, RT_MAIN_FRAME))["action"],
            "block"
        );
        // Session allowance exempts.
        call_json(&json!({
            "op": "allow_http", "host": "ex.example", "persistent": false,
        }))
        .unwrap();
        assert_eq!(
            evaluate_json(&manifest(url, RT_MAIN_FRAME))["action"],
            "allow"
        );
        // Persisted exception (snapshot refresh) exempts too.
        reset();
        call_json(&json!({
            "op": "mark_downgraded", "host": "ex.example", "scope": "test",
        }))
        .unwrap();
        load_snapshot_json(&json!({"https_only_exceptions": ["ex.example"]}))
            .unwrap();
        assert_eq!(
            evaluate_json(&manifest(url, RT_MAIN_FRAME))["action"],
            "allow"
        );
        let hosts = call_json(&json!({"op": "http_exceptions"})).unwrap();
        assert_eq!(hosts, json!(["ex.example"]));
    }

    #[test]
    fn script_block_needs_safer_and_http_first_party() {
        let _g = guard();
        reset();
        let req = |fp: &str| {
            json!({
                "url": "https://cdn.example/s.js",
                "first_party_url": fp,
                "resource_type": RT_SCRIPT,
                "method": "GET",
                "headers": [],
            })
        };
        // Standard tier: scripts on http pages are fine.
        assert_eq!(evaluate_json(&req("http://site.example/"))["action"], "allow");
        load_snapshot_json(&json!({"security_level": 1})).unwrap();
        // Safer: http page scripts die; loopback + https pages keep theirs.
        assert_eq!(evaluate_json(&req("http://site.example/"))["action"], "block");
        assert_eq!(evaluate_json(&req("http://127.0.0.1:8000/"))["action"], "allow");
        assert_eq!(evaluate_json(&req("https://site.example/"))["action"], "allow");
        // JSCTL grant wins.
        let mut granted = req("http://site.example/");
        granted["script_allowed"] = json!(true);
        assert_eq!(evaluate_json(&granted)["action"], "allow");
        // Non-script types are untouched.
        let mut img = req("http://site.example/");
        img["resource_type"] = json!(4);
        img["url"] = json!("https://cdn.example/i.png");
        assert_eq!(evaluate_json(&img)["action"], "allow");
    }

    #[test]
    fn referer_levels() {
        let _g = guard();
        reset();
        let req = |level: i64, referer: &str, target: &str| {
            load_snapshot_json(&json!({"referer_policy": level})).unwrap();
            json!({
                "url": target,
                "resource_type": 4,
                "method": "GET",
                "headers": [["Referer", referer]],
            })
        };
        // Trimmed: cross-site -> target's own origin.
        let v = evaluate_json(&req(1, "https://a.example/deep/path?q=1", "https://b.example/x"));
        assert_eq!(v["referer"]["value"], "https://b.example/");
        // Trimmed: same-site -> source origin.
        let v = evaluate_json(&req(1, "https://a.example/deep/path?q=1", "https://sub.a.example/x"));
        assert_eq!(v["referer"]["value"], "https://a.example/");
        // Strict: cross-site -> gone.
        let v = evaluate_json(&req(2, "https://a.example/p", "https://b.example/x"));
        assert_eq!(v["referer"]["value"], "");
        // Strict same-site still trims to origin.
        let v = evaluate_json(&req(2, "https://a.example/p?q=1", "https://a.example/x"));
        assert_eq!(v["referer"]["value"], "https://a.example/");
        // https -> http downgrade loses the header at every level > 0.
        let v = evaluate_json(&req(1, "https://a.example/p", "http://a.example/x"));
        assert_eq!(v["referer"]["value"], "");
        // Never: gone even same-site.
        let v = evaluate_json(&req(3, "https://a.example/p", "https://a.example/x"));
        assert_eq!(v["referer"]["value"], "");
        // EngineDefault: untouched.
        let v = evaluate_json(&req(0, "https://a.example/p?q=1", "https://b.example/x"));
        assert_eq!(v["referer"]["op"], "keep");
        // No header -> never synthesized.
        let v = evaluate_json(&json!({
            "url": "https://b.example/x", "resource_type": 4, "method": "GET",
            "headers": [],
        }));
        assert_eq!(v["referer"]["op"], "keep");
    }

    #[test]
    fn tor_mode_upgrades_everything_but_onion() {
        let _g = guard();
        reset();
        let tor = |url: &str, rtype: i64| {
            json!({
                "url": url, "resource_type": rtype, "method": "GET",
                "headers": [], "tor_mode": true,
            })
        };
        // Clearnet http on ANY resource type upgrades.
        let v = evaluate_json(&tor("http://example.com/i.png", 4));
        assert_eq!(v["action"], "redirect");
        assert_eq!(v["url"], "https://example.com/i.png");
        // .onion http: stays.
        let v = evaluate_json(&tor("http://abc.onion/i.png", 4));
        assert_eq!(v["action"], "allow");
        // Tor enforces at least Trimmed even under EngineDefault.
        load_snapshot_json(&json!({"referer_policy": 0})).unwrap();
        let v = evaluate_json(&json!({
            "url": "https://b.example/x", "resource_type": 4, "method": "GET",
            "headers": [["Referer", "https://a.example/p"]],
            "tor_mode": true, "min_referer_level": 1,
        }));
        assert_eq!(v["referer"]["value"], "https://b.example/");
    }

    #[test]
    fn cookie_gate_matrix() {
        let _g = guard();
        reset();
        let f = |accept: i64, b3p: bool, third: bool, host: &str, extra: Value| {
            let mut m = json!({
                "host": host, "third_party": third, "block_3p": b3p,
                "accept_policy": accept,
                "block": [], "allow": [], "allow_session": [],
            });
            for (k, v) in extra.as_object().unwrap() {
                m[k] = v.clone();
            }
            cookie_filter_json(&m)
        };
        // AcceptAlways / AcceptNever / FirstPartyOnly on a bare host.
        assert_eq!(f(0, false, false, "a.com", json!({})), Some(true));
        assert_eq!(f(1, false, false, "a.com", json!({})), Some(false));
        assert_eq!(f(2, false, false, "a.com", json!({})), Some(true));
        assert_eq!(f(2, false, true, "a.com", json!({})), Some(false));
        // Third-party toggle rejects only third parties.
        assert_eq!(f(0, true, true, "a.com", json!({})), Some(false));
        assert_eq!(f(0, true, false, "a.com", json!({})), Some(true));
        // Block list beats everything, allow lists beat the 3p rule.
        assert_eq!(
            f(0, false, false, "a.com", json!({"block": ["a.com"]})),
            Some(false)
        );
        assert_eq!(
            f(1, true, true, "a.com", json!({"allow": ["a.com"]})),
            Some(true)
        );
        assert_eq!(
            f(1, true, true, "a.com", json!({"allow_session": [".a.com"]})),
            Some(true)
        );
        // Under a blocked parent the allow list does not rescue.
        assert_eq!(
            f(1, true, true, "x.a.com", json!({"block": ["a.com"], "allow": ["x.a.com"]})),
            Some(false)
        );
        // Suffix rules match subdomains, not siblings.
        assert_eq!(
            f(2, false, true, "x.a.com", json!({"allow": ["a.com"]})),
            Some(true)
        );
        assert_eq!(
            f(2, false, true, "xa.com", json!({"allow": ["a.com"]})),
            Some(false)
        );
    }

    #[test]
    fn failure_table_matches_net_error_intent() {
        assert!(!failure_implies_downgrade(1, -101)); // wrong domain
        assert!(!failure_implies_downgrade(2, -105)); // DNS
        assert!(!failure_implies_downgrade(2, -106)); // offline
        assert!(!failure_implies_downgrade(2, -111)); // CONNECT refused
        assert!(!failure_implies_downgrade(2, -110)); // client cert
        assert!(!failure_implies_downgrade(2, -139)); // throttled
        assert!(failure_implies_downgrade(2, -101)); // conn reset
        assert!(failure_implies_downgrade(2, -102)); // refused
        assert!(failure_implies_downgrade(2, -118)); // timed out
        assert!(failure_implies_downgrade(2, -112)); // TLS version
    }

    #[test]
    fn host_classifiers() {
        assert!(is_private_or_local_host(""));
        assert!(is_private_or_local_host("localhost"));
        assert!(is_private_or_local_host("dev.localhost"));
        assert!(is_private_or_local_host("printer.local"));
        assert!(is_private_or_local_host("abc.onion"));
        assert!(is_private_or_local_host("127.0.0.1"));
        assert!(is_private_or_local_host("10.0.0.1"));
        assert!(is_private_or_local_host("172.16.5.4"));
        assert!(is_private_or_local_host("192.168.1.9"));
        assert!(is_private_or_local_host("169.254.3.3"));
        assert!(is_private_or_local_host("100.64.1.1"));
        assert!(is_private_or_local_host("198.18.0.5"));
        assert!(is_private_or_local_host("fd00::1"));
        assert!(is_private_or_local_host("fe80::1"));
        assert!(is_private_or_local_host("::1"));
        assert!(!is_private_or_local_host("example.com"));
        assert!(!is_private_or_local_host("8.8.8.8"));
        assert!(!is_private_or_local_host("11.0.0.1"));
        assert!(!is_private_or_local_host("172.15.0.1"));
        assert!(is_loopback_host("localhost"));
        assert!(is_loopback_host("127.0.0.5"));
        assert!(!is_loopback_host("10.0.0.1"));
        assert!(!is_loopback_host("printer.local"));
    }

    #[test]
    fn referer_origin_drops_default_ports() {
        assert_eq!(referer_origin("https://a.example:443/p"), "https://a.example/");
        assert_eq!(referer_origin("https://a.example:8443/p"), "https://a.example:8443/");
        assert_eq!(referer_origin("http://a.example:80/p"), "http://a.example/");
        assert_eq!(
            referer_origin("https://u:pw@a.example/p"),
            "https://a.example/"
        );
        assert_eq!(
            referer_origin("https://[2001:db8::1]:443/p"),
            "https://[2001:db8::1]/"
        );
    }

    #[test]
    fn same_site_semantics() {
        assert!(same_site("a.com", "a.com"));
        assert!(same_site("www.a.com", "a.com"));
        assert!(same_site("a.com", "www.a.com"));
        assert!(same_site("x.a.com", "y.a.com"));
        assert!(same_site("x.a.co.uk", "y.a.co.uk")); // no PSL — documented
        assert!(!same_site("a.com", "b.com"));
        assert!(!same_site("", "a.com"));
        assert!(same_site("", ""));
    }

    #[test]
    fn websockets_blocked_only_cross_site_opt_in() {
        let _g = guard();
        reset();
        let ws = |fp: &str| {
            json!({
                "url": "wss://ws.example/socket", "first_party_url": fp,
                "resource_type": RT_WEBSOCKET, "method": "GET", "headers": [],
            })
        };
        assert_eq!(evaluate_json(&ws("https://a.example/"))["action"], "allow");
        load_snapshot_json(&json!({"block_third_party_ws": true})).unwrap();
        assert_eq!(evaluate_json(&ws("https://a.example/"))["action"], "block");
        assert_eq!(
            evaluate_json(&ws("https://ws.example/"))["action"],
            "allow"
        );
    }
}
