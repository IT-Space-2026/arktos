//! Change-notification callback registry — the Rust half of the
//! callback -> Qt-signal bridge.  One process-wide callback (a fn
//! pointer + opaque userdata) is enough for Arktos: the Qt side
//! (RustCoreBridge) re-emits each topic as a queued Qt signal, so a
//! Rust worker thread never touches the GUI thread directly.
//!
//! RC07 adds the facade lane: each AcCore registers its own
//! (module, profile_id) callback — the profile the mutating call was
//! bound to, stamped by the ac_* wrappers through a thread-local
//! hint.  A mutation that arrives over the legacy rc_* surface has
//! no bound profile and reports NULL.  Both lanes fire for every
//! emit: the legacy callback keeps its topic-string contract
//! unchanged until RC08 retires it.
//!
//! Emitters MUST call emit() after releasing the store lock — the
//! callback re-enters this library on the Qt side.

use std::cell::RefCell;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::ptr;
use std::sync::Mutex;

pub type RcChangeCallback = unsafe extern "C" fn(*const c_char, *mut c_void);

/// The facade callback: (module topic, bound profile id or NULL,
/// userdata) — the "dispatched per module+profile" contract
/// arktos_core.h documents.
pub type AcChangeCallback =
    unsafe extern "C" fn(*const c_char, *const c_char, *mut c_void);

static CALLBACK: Mutex<Option<(RcChangeCallback, usize)>> = Mutex::new(None);

/// Facade subscribers keyed by their AcCore token (the handle
/// pointer as usize — unique while the core lives).  Copied out
/// before dispatch so a callback that re-registers or shuts its
/// core down mid-emit cannot deadlock.
static CORE_CALLBACKS: Mutex<Vec<(usize, AcChangeCallback, usize)>> =
    Mutex::new(Vec::new());

pub fn set_callback(cb: Option<RcChangeCallback>, userdata: *mut c_void) {
    let mut slot = CALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    *slot = cb.map(|f| (f, userdata as usize));
}

/// Registers (cb = Some) or clears (cb = None) one core's callback.
/// ac_shutdown clears with the same token.
pub fn set_core_callback(
    token: usize,
    cb: Option<AcChangeCallback>,
    userdata: *mut c_void,
) {
    let mut subs = CORE_CALLBACKS.lock().unwrap_or_else(|e| e.into_inner());
    subs.retain(|(t, _, _)| *t != token);
    if let Some(f) = cb {
        subs.push((token, f, userdata as usize));
    }
}

thread_local! {
    static PROFILE_HINT: RefCell<Option<CString>> =
        const { RefCell::new(None) };
}

/// Stamps the profile the current thread's forwarded call is bound
/// to so emit() routes it to the right scope.  RAII: restores the
/// previous hint on drop, so nested or alternating binds compose.
/// Held for the duration of one forwarded rc_* call — emit() runs
/// synchronously inside it on the same thread.
pub struct ProfileHint(Option<CString>);

pub fn profile_hint(profile_id: &str) -> ProfileHint {
    let prev = PROFILE_HINT.with(|slot| {
        slot.borrow_mut()
            .replace(CString::new(profile_id).unwrap_or_default())
    });
    ProfileHint(prev)
}

impl Drop for ProfileHint {
    fn drop(&mut self) {
        PROFILE_HINT.with(|slot| *slot.borrow_mut() = self.0.take());
    }
}

/// Topics so far: "credentials", "bookmarks", "history", "autofill",
/// "settings", "profiles", "sitedecisions", "searchengines",
/// "cookies", "icons", "containers", "torctl" (RC14 — a pull-drain
/// wake-up, no store mutated) — the string is the contract; later
/// components add their own.
pub fn emit(topic: &str) {
    let cb = *CALLBACK.lock().unwrap_or_else(|e| e.into_inner());
    let topic_c = CString::new(topic).unwrap_or_default();
    if let Some((f, userdata)) = cb {
        unsafe { f(topic_c.as_ptr(), userdata as *mut c_void) }
    }
    let subs: Vec<(usize, AcChangeCallback, usize)> = CORE_CALLBACKS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .clone();
    if subs.is_empty() {
        return;
    }
    // Clone the hint before dispatch: a callback that re-enters the
    // library could move the TLS slot under a borrowed pointer.
    let hint = PROFILE_HINT.with(|slot| slot.borrow().clone());
    let hint_ptr = hint.as_ref().map_or(ptr::null(), |c| c.as_ptr());
    for (_, f, userdata) in subs {
        unsafe { f(topic_c.as_ptr(), hint_ptr, userdata as *mut c_void) }
    }
}
