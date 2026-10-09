//! The DLACC04 policy gate — a process-wide C hook consulted before
//! every wire request: the initial probe AND each redirect hop.
//!
//! The engine path gets QWebEngineUrlRequestInterceptor for free; the
//! custom engine does not, so the Qt shell replays the same policy
//! pipeline (https-first upgrade, HTTPS-Only veto, tracking-param
//! strip, domain blocklist, adblock match, redirect-to-private
//! refusal) through this hook.  With no gate installed every request
//! is allowed — the Qt side installs one before the first dl_start.
//!
//! The hook runs on Rust worker threads, possibly concurrently; the
//! contract in rustdl.h restricts it to thread-safe state (the
//! interceptor's lock-guarded snapshots, AdBlockNetwork::match).

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::sync::RwLock;

/// Mirrors DlGateFn in rustdl.h.
pub type GateFn = unsafe extern "C" fn(
    url: *const c_char,
    prev_url: *const c_char,
    first_party: *const c_char,
    scope: *const c_char,
    out_url: *mut c_char,
    out_url_cap: usize,
    out_reason: *mut c_char,
    out_reason_cap: usize,
    ctx: *mut c_void,
) -> i32;

/// Return codes — mirror DlGateAction in rustdl.h.
const ACTION_ALLOW: i32 = 0;
const ACTION_BLOCK: i32 = 1;
const ACTION_REWRITE: i32 = 2;

/// A gate rewrite only ever flips the scheme or strips query bytes —
/// the result is ≤ the input length.  32 KiB is generous headroom; a
/// rewrite that cannot fit is refused rather than truncated.
const OUT_URL_CAP: usize = 32 * 1024;
const OUT_REASON_CAP: usize = 512;

/// Raw pointer ctx stored as usize so the global stays Sync.
static GATE: RwLock<Option<(GateFn, usize)>> = RwLock::new(None);

pub fn set_gate(f: Option<GateFn>, ctx: *mut c_void) {
    *GATE.write().unwrap() = f.map(|g| (g, ctx as usize));
}

pub enum Decision {
    Allow,
    /// Human-facing reason; must never embed the URL query/fragment.
    Block(String),
    /// Absolute replacement URL — re-gated on the next iteration.
    Rewrite(String),
}

fn take(buf: &[u8]) -> String {
    // The buffer is always NUL-terminated by contract; a callback that
    // forgot yields a truncated-looking string, never an overread —
    // CStr stops at the first NUL.
    match CStr::from_bytes_until_nul(buf) {
        Ok(s) => s.to_string_lossy().into_owned(),
        Err(_) => String::new(),
    }
}

/// Runs the installed gate for one candidate hop.  `prev` is the URL
/// that redirected to `url` (None on the first request — the
/// public->private redirect refusal keys off it).
pub fn check(url: &str, prev: Option<&str>, first_party: &str, scope: &str) -> Decision {
    let Some((f, ctx)) = *GATE.read().unwrap() else {
        return Decision::Allow;
    };
    // Parsed URLs never contain NUL; a defensive empty string keeps
    // the call-site contract intact if one somehow appears.
    let curl = CString::new(url).unwrap_or_default();
    let cprev = CString::new(prev.unwrap_or("")).unwrap_or_default();
    let cfp = CString::new(first_party).unwrap_or_default();
    let cscope = CString::new(scope).unwrap_or_default();
    let mut out_url = vec![0u8; OUT_URL_CAP];
    let mut out_reason = vec![0u8; OUT_REASON_CAP];
    let code = unsafe {
        f(
            curl.as_ptr(),
            cprev.as_ptr(),
            cfp.as_ptr(),
            cscope.as_ptr(),
            out_url.as_mut_ptr() as *mut c_char,
            out_url.len(),
            out_reason.as_mut_ptr() as *mut c_char,
            out_reason.len(),
            ctx as *mut c_void,
        )
    };
    match code {
        ACTION_ALLOW => Decision::Allow,
        ACTION_REWRITE => {
            let target = take(&out_url);
            if target.is_empty() {
                Decision::Block("policy gate returned an empty rewrite".into())
            } else {
                Decision::Rewrite(target)
            }
        }
        ACTION_BLOCK => {
            let reason = take(&out_reason);
            Decision::Block(if reason.is_empty() {
                "blocked by download policy".into()
            } else {
                reason
            })
        }
        // Unknown codes fail closed — a confused gate must never turn
        // into a quiet allow.
        _ => Decision::Block("policy gate returned an unknown action".into()),
    }
}
