//! Change-notification callback registry — the Rust half of the
//! callback -> Qt-signal bridge.  One process-wide callback (a fn
//! pointer + opaque userdata) is enough for Arora: the Qt side
//! (RustCoreBridge) re-emits each topic as a queued Qt signal, so a
//! Rust worker thread never touches the GUI thread directly.
//!
//! Emitters MUST call emit() after releasing the store lock — the
//! callback re-enters this library on the Qt side.

use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::sync::Mutex;

pub type RcChangeCallback = unsafe extern "C" fn(*const c_char, *mut c_void);

static CALLBACK: Mutex<Option<(RcChangeCallback, usize)>> = Mutex::new(None);

pub fn set_callback(cb: Option<RcChangeCallback>, userdata: *mut c_void) {
    let mut slot = CALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    *slot = cb.map(|f| (f, userdata as usize));
}

/// Topics so far: "credentials" (rc_cred_* map changed).  Later
/// components add their own; the string is the contract.
pub fn emit(topic: &str) {
    let cb = *CALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    if let Some((f, userdata)) = cb {
        let c = CString::new(topic).unwrap_or_default();
        unsafe { f(c.as_ptr(), userdata as *mut c_void) }
    }
}
