//! Status codes + thread-local last-error plumbing, mirroring
//! rustcore's error.rs so the Qt side reads both crates the same way.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::c_char;

/// Every fallible entry point returns one of these.  `Ok` is the only
/// success value; on failure a human-readable message is stashed in
/// thread-local storage for dl_last_error_message().
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum DlStatus {
    Ok = 0,
    InvalidArgument = 1,
    Io = 2,
    /// Connect/TLS/reset-class failure talking to the server.
    Network = 3,
    /// The server answered with a non-2xx status.
    Http = 4,
    /// Unknown handle.
    NotFound = 5,
    /// dl_set_temp_dir not called, or a platform facility missing.
    Unavailable = 6,
    /// Terminal state already reached — nothing left to cancel.
    Busy = 7,
    /// The DLACC04 policy gate refused the request (adblock hit,
    /// HTTPS-Only veto, hostile redirect, missing tor proxy...).
    Blocked = 8,
}

#[derive(Debug)]
pub struct Fail {
    pub status: DlStatus,
    pub msg: String,
}

pub type DlResult<T> = Result<T, Fail>;

pub fn fail<T>(status: DlStatus, msg: impl Into<String>) -> DlResult<T> {
    Err(Fail {
        status,
        msg: msg.into(),
    })
}

impl From<std::io::Error> for Fail {
    fn from(e: std::io::Error) -> Self {
        Fail {
            status: DlStatus::Io,
            msg: format!("io: {e}"),
        }
    }
}

impl From<reqwest::Error> for Fail {
    fn from(e: reqwest::Error) -> Self {
        // reqwest::Error's Display can embed the request URL including
        // its query string — never forward it verbatim (DLACC05).
        let status = if e.is_status() {
            DlStatus::Http
        } else {
            DlStatus::Network
        };
        let kind = if e.is_status() {
            e.status()
                .map(|s| format!("http status {}", s.as_u16()))
                .unwrap_or_else(|| "http status".into())
        } else if e.is_connect() {
            "connect failed".to_string()
        } else if e.is_timeout() {
            "timed out".to_string()
        } else if e.is_redirect() {
            "redirect refused".to_string()
        } else {
            "network error".to_string()
        };
        Fail { status, msg: kind }
    }
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

pub fn set_error(msg: &str) {
    let c = CString::new(msg).unwrap_or_default();
    LAST_ERROR.with(|slot| *slot.borrow_mut() = c);
}

pub fn clear_error() {
    set_error("");
}

/// Heap copy of the current thread's last error — caller frees with
/// dl_string_free().  Returns an empty string when nothing failed.
pub fn last_error_copy() -> *mut c_char {
    LAST_ERROR.with(|slot| {
        CString::new(slot.borrow().to_bytes())
            .map(|c| c.into_raw())
            .unwrap_or(std::ptr::null_mut())
    })
}
