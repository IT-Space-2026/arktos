//! arora-rustdl — Arora's accelerated download engine behind a C ABI
//! (DLACC03/DLACC06).  Same conventions as rustcore:
//!   * All entry points are panic-safe (catch_unwind) and treat every
//!     pointer as untrusted — bad input degrades to a status code.
//!   * Fallible calls return DlStatus; the call-site message is in
//!     dl_last_error_message() on the same thread; worker failures
//!     surface through dl_error_message(handle).
//!   * Downloads are registry handles (ids), never raw pointers.
//!   * Polling, not callbacks, crosses the FFI — dl_poll is cheap
//!     atomic reads; the Qt side runs it on a ~2 Hz QTimer.

mod cookies;
mod engine;
mod error;
#[cfg(test)]
mod itest;
mod sanitize;

use std::collections::HashMap;
use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::PathBuf;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, OnceLock};

pub use error::DlStatus;
use error::Fail;

/// Download handle handed to C — an id into REGISTRY.
pub type DlHandle = u64;

/// Poll result — mirrors DlProgress in rustdl.h.
#[repr(C)]
#[derive(Debug)]
pub struct DlProgress {
    pub state: i32,
    pub bytes_done: i64,
    pub bytes_total: i64,
    pub speed_bps: i64,
    pub connections: i32,
    pub reserved: i32,
}

fn registry() -> &'static Mutex<HashMap<DlHandle, Arc<engine::Download>>> {
    static R: OnceLock<Mutex<HashMap<DlHandle, Arc<engine::Download>>>> = OnceLock::new();
    R.get_or_init(|| Mutex::new(HashMap::new()))
}

fn temp_root() -> &'static Mutex<Option<PathBuf>> {
    static T: OnceLock<Mutex<Option<PathBuf>>> = OnceLock::new();
    T.get_or_init(|| Mutex::new(None))
}

static NEXT_ID: AtomicU64 = AtomicU64::new(1);

fn status_of(f: impl FnOnce() -> error::DlResult<()>) -> DlStatus {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => {
            error::clear_error();
            DlStatus::Ok
        }
        Ok(Err(e)) => {
            error::set_error(&e.msg);
            e.status
        }
        Err(_) => {
            error::set_error("panic inside rustdl");
            DlStatus::Unavailable
        }
    }
}

unsafe fn cstr<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

fn to_c_string(s: String) -> *mut c_char {
    CString::new(s).map_or(ptr::null_mut(), |c| c.into_raw())
}

fn lookup(handle: DlHandle) -> Option<Arc<engine::Download>> {
    registry().lock().unwrap().get(&handle).map(Arc::clone)
}

// ---- housekeeping ----------------------------------------------------

/// Root for per-download part-file directories.  Each dl_start creates
/// a randomized 0700 subdirectory beneath it; the whole subtree is
/// removed when the transfer ends.
///
/// # Safety
/// `utf8_path` must be NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn dl_set_temp_dir(utf8_path: *const c_char) -> DlStatus {
    status_of(|| {
        let dir = unsafe { cstr(utf8_path) }.ok_or_else(|| Fail {
            status: DlStatus::InvalidArgument,
            msg: "bad temp dir pointer".into(),
        })?;
        let p = PathBuf::from(dir);
        std::fs::create_dir_all(&p)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&p, std::fs::Permissions::from_mode(0o700));
        }
        *temp_root().lock().unwrap() = Some(p);
        Ok(())
    })
}

/// Always 1 when the crate is linked — presence probe for the UI.
#[no_mangle]
pub unsafe extern "C" fn dl_is_available() -> i32 {
    1
}

/// Last call-site error on this thread ("" after success).
/// Free with dl_string_free().
#[no_mangle]
pub unsafe extern "C" fn dl_last_error_message() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(error::last_error_copy)).unwrap_or(ptr::null_mut())
}

/// Frees a string handed out by this library.
///
/// # Safety
/// `s` must come from this library, or be null.
#[no_mangle]
pub unsafe extern "C" fn dl_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

// ---- downloads --------------------------------------------------------

/// Starts a download worker.  `connections` clamps to 1..16 (0 = 8).
/// `suggested_name`, `cookie_file` and `options_json` may be null;
/// options_json currently understands {"user_agent","referer","proxy"}.
///
/// # Safety
/// All string pointers must be NUL-terminated UTF-8 or null;
/// `out_handle` must be writable.
#[no_mangle]
pub unsafe extern "C" fn dl_start(
    url: *const c_char,
    dest_dir: *const c_char,
    suggested_name: *const c_char,
    connections: i32,
    cookie_file: *const c_char,
    options_json: *const c_char,
    out_handle: *mut DlHandle,
) -> DlStatus {
    status_of(|| {
        let out = unsafe { out_handle.as_mut() }.ok_or_else(|| Fail {
            status: DlStatus::InvalidArgument,
            msg: "bad out-handle pointer".into(),
        })?;
        let url = unsafe { cstr(url) }.ok_or_else(|| Fail {
            status: DlStatus::InvalidArgument,
            msg: "bad url pointer".into(),
        })?;
        let dest = unsafe { cstr(dest_dir) }.ok_or_else(|| Fail {
            status: DlStatus::InvalidArgument,
            msg: "bad dest dir pointer".into(),
        })?;
        let root = temp_root().lock().unwrap().clone().ok_or_else(|| Fail {
            status: DlStatus::Unavailable,
            msg: "dl_set_temp_dir not called".into(),
        })?;

        let mut options = engine::Options::default();
        if let Some(json) = unsafe { cstr(options_json) } {
            let v: serde_json::Value =
                serde_json::from_str(json).map_err(|_| Fail {
                    status: DlStatus::InvalidArgument,
                    msg: "bad options json".into(),
                })?;
            options.user_agent = v
                .get("user_agent")
                .and_then(|x| x.as_str())
                .map(String::from);
            options.referer = v
                .get("referer")
                .and_then(|x| x.as_str())
                .map(String::from);
            options.proxy = v
                .get("proxy")
                .and_then(|x| x.as_str())
                .map(String::from);
        }

        let job = engine::Job {
            url: url.to_string(),
            dest_dir: PathBuf::from(dest),
            suggested: unsafe { cstr(suggested_name) }.map(String::from),
            connections: if connections <= 0 {
                engine::DEFAULT_CONNECTIONS
            } else {
                connections as usize
            },
            cookie_file: unsafe { cstr(cookie_file) }.map(PathBuf::from),
            options,
        };

        // Per-download randomized work dir (0700) — the random name
        // also keeps the destination file name out of the tmp tree.
        let id = NEXT_ID.fetch_add(1, Ordering::SeqCst);
        let nanos = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_nanos())
            .unwrap_or(0);
        let work = root.join(format!(
            "dl-{:x}-{:x}-{:x}",
            std::process::id(),
            id,
            nanos
        ));
        std::fs::create_dir_all(&work)?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&work, std::fs::Permissions::from_mode(0o700));
        }

        let dl = engine::Download::new();
        registry().lock().unwrap().insert(id, Arc::clone(&dl));
        let worker_dl = Arc::clone(&dl);
        let cleanup_dir = work.clone();
        match std::thread::Builder::new()
            .name(format!("rustdl-{id}"))
            .spawn(move || engine::run(worker_dl, job, work))
        {
            Ok(_) => {
                *out = id;
                Ok(())
            }
            Err(e) => {
                registry().lock().unwrap().remove(&id);
                let _ = std::fs::remove_dir_all(&cleanup_dir);
                Err(Fail {
                    status: DlStatus::Unavailable,
                    msg: format!("spawn: {e}"),
                })
            }
        }
    })
}

/// Reads the handle's progress counters into `out`.
///
/// # Safety
/// `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn dl_poll(handle: DlHandle, out: *mut DlProgress) -> DlStatus {
    status_of(|| {
        let out = unsafe { out.as_mut() }.ok_or_else(|| Fail {
            status: DlStatus::InvalidArgument,
            msg: "bad progress pointer".into(),
        })?;
        let dl = lookup(handle).ok_or_else(|| Fail {
            status: DlStatus::NotFound,
            msg: "unknown download handle".into(),
        })?;
        let (state, done, total, bps, conns) = dl.poll();
        *out = DlProgress {
            state,
            bytes_done: done,
            bytes_total: total,
            speed_bps: bps,
            connections: conns,
            reserved: 0,
        };
        Ok(())
    })
}

/// Flags the transfer to stop.  DL_BUSY when it already reached a
/// terminal state — the outcome is unchanged either way.
#[no_mangle]
pub unsafe extern "C" fn dl_cancel(handle: DlHandle) -> DlStatus {
    status_of(|| {
        let dl = lookup(handle).ok_or_else(|| Fail {
            status: DlStatus::NotFound,
            msg: "unknown download handle".into(),
        })?;
        let state = dl.state.load(Ordering::SeqCst);
        if state >= engine::DlState::Done as i32 {
            return error::fail(DlStatus::Busy, "download already finished");
        }
        dl.cancel.store(true, Ordering::SeqCst);
        Ok(())
    })
}

/// Cancels (if still running) and drops the handle.  The worker
/// thread keeps enough state to finish its own cleanup; the tmp dir
/// is always removed before it exits.
#[no_mangle]
pub unsafe extern "C" fn dl_free(handle: DlHandle) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Some(dl) = registry().lock().unwrap().remove(&handle) {
            dl.cancel.store(true, Ordering::SeqCst);
        }
    }));
}

/// Worker-side error text ("" while none).  Free with dl_string_free().
#[no_mangle]
pub unsafe extern "C" fn dl_error_message(handle: DlHandle) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match lookup(handle) {
        Some(dl) => to_c_string(dl.error_string()),
        None => to_c_string("unknown download handle".into()),
    }))
    .unwrap_or(ptr::null_mut())
}

/// The sanitized file name the engine adopted.  Free with
/// dl_string_free().
#[no_mangle]
pub unsafe extern "C" fn dl_file_name(handle: DlHandle) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match lookup(handle) {
        Some(dl) => to_c_string(dl.file_name()),
        None => ptr::null_mut(),
    }))
    .unwrap_or(ptr::null_mut())
}

/// Final destination path once DL_DONE ("" before).  Free with
/// dl_string_free().
#[no_mangle]
pub unsafe extern "C" fn dl_output_path(handle: DlHandle) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match lookup(handle) {
        Some(dl) => to_c_string(dl.output_path()),
        None => ptr::null_mut(),
    }))
    .unwrap_or(ptr::null_mut())
}
