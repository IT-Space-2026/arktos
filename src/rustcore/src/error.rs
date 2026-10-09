//! Status codes + thread-local last-error plumbing shared by every
//! rustcore component.  Keep this file dependency-free: later modules
//! (bookmarks, sessions, filters) reuse it unchanged.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::c_char;

/// Every fallible entry point returns one of these.  `Ok` is the only
/// success value; on failure a human-readable message is stashed in
/// thread-local storage for rc_last_error_message().
#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum RcStatus {
    Ok = 0,
    InvalidArgument = 1,
    Io = 2,
    /// Credential store locked (passphrase mode, no derived key).
    Locked = 3,
    /// KDF ran but the verifier rejected the key — wrong passphrase.
    WrongPassphrase = 4,
    /// AEAD authentication or seal/decrypt failure.
    Crypto = 5,
    /// rc_set_data_dir() has not been called yet.
    NotInitialized = 6,
    /// On-disk artifact is malformed (bad magic, truncated).
    Corrupt = 7,
    AlreadyEnabled = 8,
    NotEnabled = 9,
    /// Required OS facility (RNG) unavailable.
    Unavailable = 10,
    /// Named credential does not exist.
    NotFound = 11,
    EmptyPassphrase = 12,
}

#[derive(Debug)]
pub struct Fail {
    pub status: RcStatus,
    pub msg: String,
}

pub type RcResult<T> = Result<T, Fail>;

pub fn fail<T>(status: RcStatus, msg: impl Into<String>) -> RcResult<T> {
    Err(Fail {
        status,
        msg: msg.into(),
    })
}

impl From<std::io::Error> for Fail {
    fn from(e: std::io::Error) -> Self {
        Fail {
            status: RcStatus::Io,
            msg: format!("io: {e}"),
        }
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
/// rc_string_free().  Returns an empty string when nothing failed.
pub fn last_error_copy() -> *mut c_char {
    LAST_ERROR.with(|slot| {
        CString::new(slot.borrow().to_bytes())
            .map(|c| c.into_raw())
            .unwrap_or(std::ptr::null_mut())
    })
}
