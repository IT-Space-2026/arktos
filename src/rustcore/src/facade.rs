//! RC07: the `arktos_core.h` facade — the single stable C ABI the
//! engine backends program against.  A thin ownership layer over the
//! RC02–06 modules, not a parallel implementation: every ac_* op
//! re-asserts its handle's profile binding and forwards to the same
//! rc_* entry point, so the two surfaces can never diverge on disk.
//!
//! Binding model (RUSTCORE-ARCH §5): ac_settings and ac_profiles
//! return the modules' own per-scope handles unchanged (AcSettings
//! IS RcSettings, AcProfiles IS RcProfiles — the header's typedefs
//! say so).  History, bookmarks and the site-decision/policy stores
//! are process-global behind one anchor each — the open database
//! path, the loaded-file stamp and the data dir — so their facade
//! handles own the RESOLVED anchor and every op re-asserts it first:
//! a no-op when the anchor already matches, a re-open/re-load/
//! re-anchor when a different profile's op ran in between.  Same
//! cost shape as the rustCoreEnsureDataDirFor re-anchoring the
//! facade retires for its callers.
//!
//! Ownership: the AcCore tracks every handle its accessors minted;
//! ac_*_close unlinks and frees one, ac_shutdown frees them all.  A
//! handle may only be used while its AcCore lives — the usual
//! handle-lifetime rule, stated in RUSTCORE-ABI.md.
//!
//! Notifications: facade ops stamp the bound profile through
//! notify::profile_hint around the forwarded call, so a
//! (module, profile_id) subscriber can tell which persona's store
//! moved.  Mutations arriving over the legacy rc_* surface have no
//! hint and report a NULL profile to core subscribers.

use std::collections::HashMap;
use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::path::{Path, PathBuf};
use std::ptr;
use std::sync::{LazyLock, Mutex};

use crate::error::{self, RcStatus};
use crate::{bookmarks, icons, notify, profiles, settings, util};

/// The facade ABI version — AC_API_VERSION in arktos_core.h.  Bump
/// on every ABI-affecting addition; never renumber existing symbols
/// or enum values.  v3 added AcCookies (RC15).  v4 added AcIcons
/// (RC16).  v5 added AcContainers (RC19).
pub const AC_API_VERSION: u32 = 5;

const AC_CAP_SETTINGS: u64 = 0x01;
const AC_CAP_PROFILES: u64 = 0x02;
const AC_CAP_HISTORY: u64 = 0x04;
const AC_CAP_BOOKMARKS: u64 = 0x08;
const AC_CAP_POLICY: u64 = 0x10;
const AC_CAP_SESSION: u64 = 0x20;
const AC_CAP_COOKIES: u64 = 0x40;
const AC_CAP_ICONS: u64 = 0x80;
const AC_CAP_CONTAINERS: u64 = 0x100;
const AC_CAP_ALL: u64 = AC_CAP_SETTINGS
    | AC_CAP_PROFILES
    | AC_CAP_HISTORY
    | AC_CAP_BOOKMARKS
    | AC_CAP_POLICY
    | AC_CAP_SESSION
    | AC_CAP_COOKIES
    | AC_CAP_ICONS
    | AC_CAP_CONTAINERS;

const DEFAULT_PROFILE: &str = "default";

/// Mirrors AcConfig in arktos_core.h — the layout is frozen and
/// append-only; callers tag it with struct_size = sizeof(AcConfig).
#[repr(C)]
pub struct AcConfig {
    /// sizeof(AcConfig) as seen by the caller's header.
    pub struct_size: u32,
    /// The AC_API_VERSION the caller was built against; 0 = any.
    pub api_version: u32,
    /// The application data root (BrowserPaths::dataRoot) — required.
    pub data_root: *const c_char,
    /// Informational app name — may be NULL.
    pub app_name: *const c_char,
    /// Display-name seed for the synthesized "default" registry row —
    /// may be NULL (the module's "Profile" fallback applies).
    pub default_profile_name: *const c_char,
}

/// The facade root: owns the data root every profile-bound path
/// derives from and the handles its accessors minted — ac_shutdown
/// frees them all.
pub struct AcCore {
    root: CString,
    default_name: Option<CString>,
    handles: Mutex<Vec<Live>>,
}

/// A live minted handle, tagged by kind so ac_shutdown frees it
/// correctly.  usize (not raw pointers) keeps the whole Vec Send,
/// which the Mutex around it honestly requires.
enum Live {
    Settings(usize),
    Profiles(usize),
    History(usize),
    Bookmarks(usize),
    Policy(usize),
    Session(usize),
    Cookies(usize),
    Icons(usize),
    Containers(usize),
}

impl Live {
    fn key(&self) -> usize {
        match *self {
            Live::Settings(p)
            | Live::Profiles(p)
            | Live::History(p)
            | Live::Bookmarks(p)
            | Live::Policy(p)
            | Live::Session(p)
            | Live::Cookies(p)
            | Live::Icons(p)
            | Live::Containers(p) => p,
        }
    }
}

/// handle ptr -> owning core ptr.  Lets an ac_*_close unlink its
/// handle from the owner's list so a later ac_shutdown can't double
/// free it; entries are dropped both on close and on shutdown.
static OWNERS: LazyLock<Mutex<HashMap<usize, usize>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

/// Profile-bound facade handles for the process-global stores.
/// Each owns the resolved anchor; ops re-assert it before
/// forwarding (the re-assert is a no-op on the hot path — see each
/// field's note).
pub struct AcHistory {
    /// The effective profile id (non-slug input resolved to
    /// "default") — stamped on "history" notifications.
    profile: String,
    /// profiles/<id>/history.db — rc_hist_open no-ops when the same
    /// path is already open.
    db_path: CString,
}

pub struct AcBookmarks {
    profile: String,
    /// profiles/<id>/bookmarks.xbel.
    xbel_path: CString,
    /// What this handle last anchored: the global tree's
    /// (epoch, loaded_path) after our last ensure — consecutive ops
    /// skip the reload entirely, a same-file reload by another
    /// writer is adopted instead of repeated, and a load_mem tree
    /// (loaded_path None) stays live until a foreign load displaces
    /// it.
    bind: Mutex<(u64, Option<PathBuf>)>,
}

pub struct AcPolicy {
    profile: String,
    /// profiles/<id> — rc_set_data_dir early-returns when the dir is
    /// already anchored.
    dir: CString,
    /// profiles/<id>/history.db — the first-seen input to the site-
    /// trust composite; re-opened per sitetrust op (same-path no-op).
    hist_db: CString,
}

/// RC13: the session store is fully path-explicit — no process-global
/// anchor at all, so the handle only needs its bound dir; every op
/// resolves profiles/<id>/session.dat + session.state under it.
pub struct AcSession {
    /// profiles/<id> — mkpath'd at bind time like every bound dir.
    dir: CString,
}

/// RC15: the cookie database is process-global behind the open-path
/// anchor (like history) — the handle owns the resolved db path and
/// every op re-asserts it (a no-op on the hot path).
pub struct AcCookies {
    /// The effective profile id — stamped on "cookies" notifications.
    profile: String,
    /// profiles/<id>/cookies.db.
    db_path: CString,
}

/// RC16: the icon cache is fully path-explicit like the session
/// store — the handle carries the resolved profiles/<id>/ dir and
/// every op works under its cache/icons/ lane directly, so there is
/// no process-global anchor to re-assert at all.
pub struct AcIcons {
    /// The effective profile id — stamped on "icons" notifications.
    profile: String,
    /// profiles/<id> — the icons module derives cache/icons under it
    /// (and reads the legacy siblings, icons/ + history.db, for the
    /// one-shot import).
    dir: CString,
}

/// RC19: the container registry lives in the profile scope's
/// settings.json — a typed specialization over the settings store,
/// so the handle carries only the binding (profile + data root).
/// Every op opens a SHORT-LIVED RcSettings on the bound scope,
/// forwards to the rc_containers_* entry point and closes it: a
/// long-lived second handle on the same settings.json would hold a
/// stale decoded document (the "unshared conf file" caveat the
/// bridge notes) — a fresh open per op reads the file as committed,
/// so a registry the C++ ProfileSettings mirror wrote mid-process
/// is honored, and ours reach it the same way.
pub struct AcContainers {
    /// The effective profile id — stamped on "containers"
    /// notifications.
    profile: String,
    /// The core's data root — the rc_settings_open root argument.
    root: CString,
}

// ---- shared plumbing ---------------------------------------------------

fn core_ref<'a>(core: *const AcCore) -> error::RcResult<&'a AcCore> {
    unsafe { core.as_ref() }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: "bad core handle".into(),
    })
}

fn fail<T>(status: RcStatus, msg: &str) -> Result<T, RcStatus> {
    error::set_error(msg);
    Err(status)
}

/// BrowserPaths::profileDataDir semantics: a non-slug id — NULL,
/// empty, or not [a-z0-9_-]{1,64} — resolves to "default".
fn effective_profile(id: &str) -> &str {
    if profiles::is_profile_id(id) {
        id
    } else {
        DEFAULT_PROFILE
    }
}

/// The id a profile-bound accessor binds: NULL/"" -> "default".
fn bound_profile(p: *const c_char) -> String {
    let s = if p.is_null() {
        ""
    } else {
        unsafe { util::cstr(p) }.unwrap_or("")
    };
    effective_profile(s).to_string()
}

/// <root>/profiles/<id> — the layout profiles.rs mirrors from
/// BrowserPaths, computed here so bound handles don't need a
/// registry open.  BrowserPaths::profileDataDir mkpaths; bound
/// handles need the same guarantee or first-touch writes under a
/// fresh data root fail ENOENT, so the dir is created on the way
/// out (best-effort — a real failure surfaces at the first write).
fn profile_dir(root: &Path, id: &str) -> PathBuf {
    let dir = root.join("profiles").join(id);
    let _ = std::fs::create_dir_all(&dir);
    dir
}

fn track(core: &AcCore, live: Live) {
    OWNERS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(live.key(), core as *const AcCore as usize);
    core.handles
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .push(live);
}

/// Removes the handle -> owner link, returning the owning core while
/// it is still alive.  A miss means the handle was already closed or
/// its core is gone — either way there is nothing left to unlink.
fn untrack(ptr: usize) -> Option<*mut AcCore> {
    OWNERS
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(&ptr)
        .map(|c| c as *mut AcCore)
}

fn unlink(core: *mut AcCore, ptr: usize) {
    if let Some(c) = unsafe { core.as_ref() } {
        c.handles
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .retain(|l| l.key() != ptr);
    }
}

unsafe fn free_live(live: Live) {
    match live {
        Live::Settings(p) => unsafe {
            crate::rc_settings_close(p as *mut settings::RcSettings)
        },
        Live::Profiles(p) => unsafe {
            crate::rc_profiles_close(p as *mut profiles::RcProfiles)
        },
        Live::History(p) => drop(unsafe { Box::from_raw(p as *mut AcHistory) }),
        Live::Bookmarks(p) => {
            drop(unsafe { Box::from_raw(p as *mut AcBookmarks) })
        }
        Live::Policy(p) => drop(unsafe { Box::from_raw(p as *mut AcPolicy) }),
        Live::Session(p) => {
            drop(unsafe { Box::from_raw(p as *mut AcSession) })
        }
        Live::Cookies(p) => {
            drop(unsafe { Box::from_raw(p as *mut AcCookies) })
        }
        Live::Icons(p) => drop(unsafe { Box::from_raw(p as *mut AcIcons) }),
        Live::Containers(p) => {
            drop(unsafe { Box::from_raw(p as *mut AcContainers) })
        }
    }
}

// ---- housekeeping --------------------------------------------------------

/// The facade ABI version — append-only bumps only.
#[no_mangle]
pub extern "C" fn ac_api_version() -> u32 {
    AC_API_VERSION
}

/// Bitmask of the modules this build carries — every bit of
/// AC_CAP_* today; future optional modules report 0 when absent.
#[no_mangle]
pub extern "C" fn ac_capabilities() -> u64 {
    AC_CAP_ALL
}

/// Thread-local last error — the same channel rc_last_error_message
/// serves; caller frees with ac_string_free.
///
/// # Safety
/// Returned pointer is caller-owned.
#[no_mangle]
pub unsafe extern "C" fn ac_last_error() -> *mut c_char {
    unsafe { crate::rc_last_error_message() }
}

/// # Safety
/// `s` must come from this library, or be NULL.
#[no_mangle]
pub unsafe extern "C" fn ac_string_free(s: *mut c_char) {
    unsafe { crate::rc_string_free(s) }
}

/// # Safety
/// `buf` must come from this library.
#[no_mangle]
pub unsafe extern "C" fn ac_buffer_free(buf: crate::RcBuffer) {
    unsafe { crate::rc_buffer_free(buf) }
}

/// Creates the facade root.  `cfg` may be NULL (fails) but a valid
/// one needs only struct_size + data_root; api_version newer than
/// the library's is refused.  NULL + stashed error on failure.
///
/// # Safety
/// All string pointers must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn ac_init(cfg: *const AcConfig) -> *mut AcCore {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let cfg = match unsafe { cfg.as_ref() } {
            Some(c) => c,
            None => {
                error::set_error("NULL AcConfig");
                return ptr::null_mut();
            }
        };
        // struct_size is the forward-compat tag: a caller compiled
        // against a shorter header can't promise data_root exists.
        if (cfg.struct_size as usize) < std::mem::size_of::<AcConfig>() {
            error::set_error("AcConfig.struct_size too small");
            return ptr::null_mut();
        }
        if cfg.api_version > AC_API_VERSION {
            error::set_error("caller requires a newer facade ABI");
            return ptr::null_mut();
        }
        let root = match unsafe { util::cstr(cfg.data_root) } {
            Some(r) if !r.is_empty() => r.to_string(),
            _ => {
                error::set_error("AcConfig.data_root is required");
                return ptr::null_mut();
            }
        };
        let default_name = if cfg.default_profile_name.is_null() {
            None
        } else {
            unsafe { util::cstr(cfg.default_profile_name) }
                .map(|s| CString::new(s).unwrap_or_default())
        };
        Box::into_raw(Box::new(AcCore {
            root: CString::new(root).unwrap_or_default(),
            default_name,
            handles: Mutex::new(Vec::new()),
        }))
    }))
    .unwrap_or_else(|_| {
        error::set_error("panic inside rustcore");
        ptr::null_mut()
    })
}

/// Frees the core and every handle it minted.  NULL-tolerant.
/// Handles become invalid at this call — using one afterwards is a
/// use-after-free, same rule as the rc_* handles.
///
/// # Safety
/// `core` must come from ac_init and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn ac_shutdown(core: *mut AcCore) {
    if core.is_null() {
        return;
    }
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        notify::set_core_callback(core as usize, None, ptr::null_mut());
        let core = unsafe { Box::from_raw(core) };
        let handles: Vec<Live> = std::mem::take(
            &mut *core.handles.lock().unwrap_or_else(|e| e.into_inner()),
        );
        for live in handles {
            OWNERS
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .remove(&live.key());
            unsafe { free_live(live) };
        }
    }));
}

/// Registers (or clears, with NULL cb) this core's
/// (module, profile_id) change callback — set once per core, fired
/// for every store mutation with the bound profile stamped (NULL
/// for unscoped/global mutations).  Invoked synchronously on the
/// mutating thread, never while a store lock is held; the consumer
/// must hop threads itself (RustCoreBridge queues onto the GUI
/// thread).
///
/// # Safety
/// `userdata` is passed back verbatim.
#[no_mangle]
pub unsafe extern "C" fn ac_set_change_callback(
    core: *mut AcCore,
    cb: Option<notify::AcChangeCallback>,
    userdata: *mut c_void,
) {
    let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        if !core.is_null() {
            notify::set_core_callback(core as usize, cb, userdata);
        }
    }));
}

// ---- module accessors ----------------------------------------------------

/// Opens the settings scope for `profile_id` (NULL/empty = the
/// global scope — the rc_settings_open contract verbatim; a non-slug
/// id binds "default").  The returned handle IS an RcSettings —
/// rc_settings_* calls on it stay legal during the RC08 transition.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_settings_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_settings(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut settings::RcSettings {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let scope = if profile_id.is_null() {
            None
        } else {
            match unsafe { util::cstr(profile_id) } {
                Some("") | None => None,
                Some(s) => Some(CString::new(effective_profile(s)).unwrap_or_default()),
            }
        };
        let h = unsafe {
            crate::rc_settings_open(
                core.root.as_ptr(),
                scope.as_ref().map_or(ptr::null(), |s| s.as_ptr()),
            )
        };
        if !h.is_null() {
            track(core, Live::Settings(h as usize));
        }
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Opens the persona registry under the core's data root — the
/// returned handle IS an RcProfiles.
///
/// # Safety
/// Free with ac_profiles_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_profiles(core: *mut AcCore) -> *mut profiles::RcProfiles {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let h = unsafe {
            crate::rc_profiles_open(
                core.root.as_ptr(),
                core.default_name
                    .as_ref()
                    .map_or(ptr::null(), |s| s.as_ptr()),
            )
        };
        if !h.is_null() {
            track(core, Live::Profiles(h as usize));
        }
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Binds the history store of `profile_id` ("default" for NULL or a
/// non-slug id) and opens its database now — open failures surface
/// here, not inside a later op.  NULL + stashed error on failure.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_hist_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_history(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcHistory {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let db = match CString::new(
            profile_dir(&root, &eff)
                .join("history.db")
                .to_string_lossy()
                .into_owned(),
        ) {
            Ok(c) => c,
            Err(_) => {
                error::set_error("history path is not UTF-8");
                return ptr::null_mut();
            }
        };
        // Bind now — an open failure is the accessor's to report.
        if unsafe { crate::rc_hist_open(db.as_ptr()) } != RcStatus::Ok {
            return ptr::null_mut();
        }
        let h = Box::into_raw(Box::new(AcHistory {
            profile: eff,
            db_path: db,
        }));
        track(core, Live::History(h as usize));
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Whether the profile's history.db exists on disk — the legacy-file
/// import gate.  Never opens the database.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn ac_history_exists(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> i32 {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(_) => return 0,
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let db = match CString::new(
            profile_dir(&root, &eff)
                .join("history.db")
                .to_string_lossy()
                .into_owned(),
        ) {
            Ok(c) => c,
            Err(_) => return 0,
        };
        unsafe { crate::rc_hist_exists(db.as_ptr()) }
    }))
    .unwrap_or(0)
}

/// Binds the bookmark tree of `profile_id` and loads its
/// bookmarks.xbel now (a missing file is an empty tree).  NULL +
/// stashed error on failure.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_bm_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_bookmarks(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcBookmarks {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let xbel = profile_dir(&root, &eff).join("bookmarks.xbel");
        let _hint = notify::profile_hint(&eff);
        match bookmarks::with(|b| {
            b.ensure_path(&xbel).map(|loaded| {
                (
                    loaded,
                    b.epoch(),
                    b.loaded_path().map(|p| p.to_path_buf()),
                )
            })
        }) {
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
            Ok((loaded, epoch, stamp)) => {
                if loaded {
                    notify::emit("bookmarks");
                }
                let xbel_c = match CString::new(
                    xbel.to_string_lossy().into_owned(),
                ) {
                    Ok(c) => c,
                    Err(_) => {
                        error::set_error("bookmarks path is not UTF-8");
                        return ptr::null_mut();
                    }
                };
                let h = Box::into_raw(Box::new(AcBookmarks {
                    profile: eff,
                    xbel_path: xbel_c,
                    bind: Mutex::new((epoch, stamp)),
                }));
                track(core, Live::Bookmarks(h as usize));
                h
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Binds the site-decision/policy surface of `profile_id` — no open
/// step (the store reads lazily), so the handle is always minted on
/// a valid core.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_policy_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_policy(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcPolicy {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let dir = profile_dir(&root, &eff);
        let dir_c = match CString::new(dir.to_string_lossy().into_owned()) {
            Ok(c) => c,
            Err(_) => {
                error::set_error("profile dir is not UTF-8");
                return ptr::null_mut();
            }
        };
        let hist_c = match CString::new(
            dir.join("history.db").to_string_lossy().into_owned(),
        ) {
            Ok(c) => c,
            Err(_) => {
                error::set_error("history path is not UTF-8");
                return ptr::null_mut();
            }
        };
        let h = Box::into_raw(Box::new(AcPolicy {
            profile: eff,
            dir: dir_c,
            hist_db: hist_c,
        }));
        track(core, Live::Policy(h as usize));
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Binds the session store of `profile_id` — profiles/<id>/session.dat
/// plus its session.state lifecycle marker (RC13).  The store is
/// path-explicit, so the handle only carries the resolved dir; every
/// op works under it directly with no process-global re-anchoring.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_session_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_session(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcSession {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let dir = profile_dir(&root, &eff);
        let dir_c = match CString::new(dir.to_string_lossy().into_owned()) {
            Ok(c) => c,
            Err(_) => {
                error::set_error("profile dir is not UTF-8");
                return ptr::null_mut();
            }
        };
        let h = Box::into_raw(Box::new(AcSession { dir: dir_c }));
        track(core, Live::Session(h as usize));
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Binds the cookie database of `profile_id` and opens
/// profiles/<id>/cookies.db now — open failures surface here, not
/// inside a later op (same contract as ac_history).  NULL + stashed
/// error on failure.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_cookies_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_cookies(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcCookies {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let db = match CString::new(
            profile_dir(&root, &eff)
                .join("cookies.db")
                .to_string_lossy()
                .into_owned(),
        ) {
            Ok(c) => c,
            Err(_) => {
                error::set_error("cookies path is not UTF-8");
                return ptr::null_mut();
            }
        };
        // Bind now — an open failure is the accessor's to report.
        if unsafe { crate::rc_cookies_open(db.as_ptr()) } != RcStatus::Ok {
            return ptr::null_mut();
        }
        let h = Box::into_raw(Box::new(AcCookies {
            profile: eff,
            db_path: db,
        }));
        track(core, Live::Cookies(h as usize));
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Binds the icon cache of `profile_id` — profiles/<id>/cache/icons/
/// (RC16).  Path-bound like ac_session: the handle only carries the
/// resolved profile dir; every op works under it with no
/// process-global anchoring.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_icons_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_icons(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcIcons {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let root = PathBuf::from(core.root.to_string_lossy().into_owned());
        let dir = profile_dir(&root, &eff);
        let dir_c = match CString::new(dir.to_string_lossy().into_owned()) {
            Ok(c) => c,
            Err(_) => {
                error::set_error("profile dir is not UTF-8");
                return ptr::null_mut();
            }
        };
        let h = Box::into_raw(Box::new(AcIcons {
            profile: eff,
            dir: dir_c,
        }));
        track(core, Live::Icons(h as usize));
        h
    }))
    .unwrap_or(ptr::null_mut())
}

/// Binds the container registry + namespace map of `profile_id`
/// (RC19) — the persona whose "containers" settings group the ops
/// read and write.  No open step (registry ops open the scope's
/// settings store per call), so the handle is always minted on a
/// valid core.
///
/// # Safety
/// `profile_id` must be NUL-terminated UTF-8 or NULL.  Free with
/// ac_containers_close; the handle dies with its core.
#[no_mangle]
pub unsafe extern "C" fn ac_containers(
    core: *mut AcCore,
    profile_id: *const c_char,
) -> *mut AcContainers {
    std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
        let core = match core_ref(core) {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let eff = bound_profile(profile_id);
        let h = Box::into_raw(Box::new(AcContainers {
            profile: eff,
            root: core.root.clone(),
        }));
        track(core, Live::Containers(h as usize));
        h
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- handle closes ---------------------------------------------------------

/// Releases a settings handle — NULL-tolerant; also legal on a
/// handle rc_settings_open minted directly (the untrack lookup just
/// misses).
///
/// # Safety
/// `h` must come from ac_settings/rc_settings_open, unused after.
#[no_mangle]
pub unsafe extern "C" fn ac_settings_close(h: *mut settings::RcSettings) {
    if h.is_null() {
        return;
    }
    if let Some(core) = untrack(h as usize) {
        unlink(core, h as usize);
    }
    unsafe { crate::rc_settings_close(h) }
}

/// Releases a profiles handle — NULL-tolerant, rc-opened handles
/// accepted like ac_settings_close.
///
/// # Safety
/// `h` must come from ac_profiles/rc_profiles_open, unused after.
#[no_mangle]
pub unsafe extern "C" fn ac_profiles_close(h: *mut profiles::RcProfiles) {
    if h.is_null() {
        return;
    }
    if let Some(core) = untrack(h as usize) {
        unlink(core, h as usize);
    }
    unsafe { crate::rc_profiles_close(h) }
}

macro_rules! ac_close {
    ($name:ident, $h:ty) => {
        /// # Safety
        /// `h` must come from the matching accessor, unused after.
        #[no_mangle]
        pub unsafe extern "C" fn $name(h: *mut $h) {
            if h.is_null() {
                return;
            }
            if let Some(core) = untrack(h as usize) {
                unlink(core, h as usize);
            }
            let _ = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                drop(unsafe { Box::from_raw(h) });
            }));
        }
    };
}

ac_close!(ac_hist_close, AcHistory);
ac_close!(ac_bm_close, AcBookmarks);
ac_close!(ac_policy_close, AcPolicy);
ac_close!(ac_session_close, AcSession);
ac_close!(ac_cookies_close, AcCookies);
ac_close!(ac_icons_close, AcIcons);
ac_close!(ac_containers_close, AcContainers);

// ---- re-assert helpers ------------------------------------------------------

fn ensure_hist<'a>(h: *const AcHistory) -> Result<&'a AcHistory, RcStatus> {
    let h = match unsafe { h.as_ref() } {
        Some(h) => h,
        None => return fail(RcStatus::InvalidArgument, "bad history handle"),
    };
    match unsafe { crate::rc_hist_open(h.db_path.as_ptr()) } {
        RcStatus::Ok => Ok(h),
        st => Err(st),
    }
}

fn ensure_bm<'a>(h: *const AcBookmarks) -> Result<&'a AcBookmarks, RcStatus> {
    let h = match unsafe { h.as_ref() } {
        Some(h) => h,
        None => return fail(RcStatus::InvalidArgument, "bad bookmarks handle"),
    };
    let path = PathBuf::from(h.xbel_path.to_string_lossy().into_owned());
    let res = bookmarks::with(|b| -> error::RcResult<bool> {
        let mut bind = h.bind.lock().unwrap_or_else(|e| e.into_inner());
        if b.epoch() == bind.0 && b.loaded_path() == bind.1.as_deref() {
            return Ok(false); // still anchored where we left it
        }
        if b.loaded_path() == Some(path.as_path()) {
            // Same file, another epoch — adopt instead of reloading.
            bind.0 = b.epoch();
            bind.1 = b.loaded_path().map(|p| p.to_path_buf());
            return Ok(false);
        }
        let loaded = b.ensure_path(&path)?;
        bind.0 = b.epoch();
        bind.1 = b.loaded_path().map(|p| p.to_path_buf());
        Ok(loaded)
    });
    match res {
        Err(e) => {
            error::set_error(&e.msg);
            Err(e.status)
        }
        Ok(loaded) => {
            if loaded {
                // Mirror rc_bm_load's emit — outside the store lock.
                let _hint = notify::profile_hint(&h.profile);
                notify::emit("bookmarks");
            }
            Ok(h)
        }
    }
}

fn bm_ref<'a>(h: *const AcBookmarks) -> Result<&'a AcBookmarks, RcStatus> {
    match unsafe { h.as_ref() } {
        Some(h) => Ok(h),
        None => fail(RcStatus::InvalidArgument, "bad bookmarks handle"),
    }
}

fn ensure_policy<'a>(h: *const AcPolicy) -> Result<&'a AcPolicy, RcStatus> {
    let h = match unsafe { h.as_ref() } {
        Some(h) => h,
        None => return fail(RcStatus::InvalidArgument, "bad policy handle"),
    };
    match unsafe { crate::rc_set_data_dir(h.dir.as_ptr()) } {
        RcStatus::Ok => Ok(h),
        st => Err(st),
    }
}

/// The site-trust composite also reads the history store's
/// first-seen rollup — re-open the bound profile's db too so a
/// verdict can't leak a different persona's visits.
fn ensure_sitetrust<'a>(h: *const AcPolicy) -> Result<&'a AcPolicy, RcStatus> {
    let h = ensure_policy(h)?;
    match unsafe { crate::rc_hist_open(h.hist_db.as_ptr()) } {
        RcStatus::Ok => Ok(h),
        st => Err(st),
    }
}

// ---- op-wrapper macros ----------------------------------------------------

/// Handle -> AcStatus op: re-assert + hint + forward.
macro_rules! ac_status_op {
    ($name:ident, $h:ty, $ensure:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? )) => {
        /// # Safety
        /// Handle + pointer arguments per the rc_* contract.
        #[no_mangle]
        pub unsafe extern "C" fn $name(h: *const $h, $($a: $t),*) -> RcStatus {
            let h = match $ensure(h) {
                Ok(h) => h,
                Err(s) => return s,
            };
            let _hint = notify::profile_hint(&h.profile);
            unsafe { crate::$rc($($a),*) }
        }
    };
}

/// Handle -> char* op: NULL on a bad handle, same as rc_* on error.
macro_rules! ac_ptr_op {
    ($name:ident, $h:ty, $ensure:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? )) => {
        /// # Safety
        /// Handle + pointer arguments per the rc_* contract; free the
        /// result with ac_string_free.
        #[no_mangle]
        pub unsafe extern "C" fn $name(h: *const $h, $($a: $t),*) -> *mut c_char {
            let h = match $ensure(h) {
                Ok(h) => h,
                Err(_) => return ptr::null_mut(),
            };
            let _hint = notify::profile_hint(&h.profile);
            unsafe { crate::$rc($($a),*) }
        }
    };
}

/// Handle -> scalar op: the rc_* error value on a bad handle.
macro_rules! ac_val_op {
    ($name:ident, $h:ty, $ensure:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? ) -> $ret:ty = $bad:expr) => {
        /// # Safety
        /// Handle + pointer arguments per the rc_* contract.
        #[no_mangle]
        pub unsafe extern "C" fn $name(h: *const $h, $($a: $t),*) -> $ret {
            let h = match $ensure(h) {
                Ok(h) => h,
                Err(_) => return $bad,
            };
            let _hint = notify::profile_hint(&h.profile);
            unsafe { crate::$rc($($a),*) }
        }
    };
}

// ---- history (RC04) --------------------------------------------------------

ac_val_op!(ac_hist_count, AcHistory, ensure_hist, rc_hist_count() -> i64 = -1);
ac_ptr_op!(ac_hist_entry_at, AcHistory, ensure_hist, rc_hist_entry_at(row: i64));
ac_status_op!(ac_hist_add, AcHistory, ensure_hist,
    rc_hist_add(url: *const c_char, title: *const c_char, ts_ms: i64));
ac_status_op!(ac_hist_update_title, AcHistory, ensure_hist,
    rc_hist_update_title(url: *const c_char, title: *const c_char));
ac_status_op!(ac_hist_remove, AcHistory, ensure_hist,
    rc_hist_remove(url: *const c_char, title: *const c_char, ts_ms: i64));
ac_status_op!(ac_hist_clear, AcHistory, ensure_hist, rc_hist_clear());
ac_status_op!(ac_hist_clear_since, AcHistory, ensure_hist,
    rc_hist_clear_since(since_ms: i64));
ac_status_op!(ac_hist_expire_older_than, AcHistory, ensure_hist,
    rc_hist_expire_older_than(cutoff_ms: i64, out_removed: *mut i64));
ac_ptr_op!(ac_hist_query, AcHistory, ensure_hist,
    rc_hist_query(spec_json: *const c_char));
ac_status_op!(ac_hist_icon_set, AcHistory, ensure_hist,
    rc_hist_icon_set(host: *const c_char, png: *const u8, len: usize));
ac_status_op!(ac_hist_icon_get, AcHistory, ensure_hist,
    rc_hist_icon_get(host: *const c_char, out: *mut crate::RcBuffer));
ac_status_op!(ac_hist_icon_clear, AcHistory, ensure_hist, rc_hist_icon_clear());
ac_ptr_op!(ac_hist_suggest, AcHistory, ensure_hist,
    rc_history_suggest(term: *const c_char, limit: i64));

// ---- bookmarks (RC05) ------------------------------------------------------

/// Forced re-read of the bound file — the rc_bm_load seam; emits
/// "bookmarks" on success like the rc call does.
///
/// # Safety
/// `h` must come from ac_bookmarks.
#[no_mangle]
pub unsafe extern "C" fn ac_bm_reload(h: *const AcBookmarks) -> RcStatus {
    let h = match bm_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let path = PathBuf::from(h.xbel_path.to_string_lossy().into_owned());
    match bookmarks::with(|b| -> error::RcResult<()> {
        b.load_path(&path)?;
        let mut bind = h.bind.lock().unwrap_or_else(|e| e.into_inner());
        bind.0 = b.epoch();
        bind.1 = b.loaded_path().map(|p| p.to_path_buf());
        Ok(())
    }) {
        Ok(()) => {
            notify::emit("bookmarks");
            RcStatus::Ok
        }
        Err(e) => {
            error::set_error(&e.msg);
            e.status
        }
    }
}

/// Loads an in-memory XBEL document into the bound tree — the
/// bundled-default-bookmarks seam.  The tree becomes anonymous
/// (loaded_path None) until a foreign load displaces it; save it to
/// the bound file with ac_bm_save(h, NULL).
///
/// # Safety
/// `xbel` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn ac_bm_load_mem(
    h: *const AcBookmarks,
    xbel: *const u8,
    len: usize,
) -> RcStatus {
    let h = match bm_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let st = unsafe { crate::rc_bm_load_mem(xbel, len) };
    if st == RcStatus::Ok {
        bookmarks::with(|b| {
            let mut bind = h.bind.lock().unwrap_or_else(|e| e.into_inner());
            bind.0 = b.epoch();
            bind.1 = b.loaded_path().map(|p| p.to_path_buf());
        });
    }
    st
}

/// Serializes the tree — NULL `path` writes the bound
/// bookmarks.xbel, an explicit path is the export-as seam.
///
/// # Safety
/// `path` must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn ac_bm_save(
    h: *const AcBookmarks,
    path: *const c_char,
) -> RcStatus {
    let h = match ensure_bm(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let p = if path.is_null() {
        h.xbel_path.as_ptr()
    } else {
        path
    };
    unsafe { crate::rc_bm_save(p) }
}

ac_val_op!(ac_bm_root, AcBookmarks, ensure_bm, rc_bm_root() -> u64 = 0);
ac_val_op!(ac_bm_child_count, AcBookmarks, ensure_bm,
    rc_bm_child_count(node: u64) -> i64 = -1);
ac_val_op!(ac_bm_child_at, AcBookmarks, ensure_bm,
    rc_bm_child_at(node: u64, row: i64) -> u64 = 0);
ac_val_op!(ac_bm_parent, AcBookmarks, ensure_bm,
    rc_bm_parent(node: u64) -> u64 = 0);
ac_ptr_op!(ac_bm_get, AcBookmarks, ensure_bm, rc_bm_get(node: u64));
ac_val_op!(ac_bm_create, AcBookmarks, ensure_bm,
    rc_bm_create(parent: u64, row: i64, json: *const c_char) -> u64 = 0);
ac_status_op!(ac_bm_attach, AcBookmarks, ensure_bm,
    rc_bm_attach(parent: u64, row: i64, node: u64));
ac_status_op!(ac_bm_detach, AcBookmarks, ensure_bm, rc_bm_detach(node: u64));
ac_status_op!(ac_bm_destroy, AcBookmarks, ensure_bm, rc_bm_destroy(node: u64));
ac_status_op!(ac_bm_set_title, AcBookmarks, ensure_bm,
    rc_bm_set_title(node: u64, value: *const c_char));
ac_status_op!(ac_bm_set_url, AcBookmarks, ensure_bm,
    rc_bm_set_url(node: u64, value: *const c_char));
ac_status_op!(ac_bm_set_desc, AcBookmarks, ensure_bm,
    rc_bm_set_desc(node: u64, value: *const c_char));
ac_status_op!(ac_bm_set_expanded, AcBookmarks, ensure_bm,
    rc_bm_set_expanded(node: u64, expanded: i32));
ac_status_op!(ac_bm_set_tags, AcBookmarks, ensure_bm,
    rc_bm_set_tags(node: u64, json: *const c_char));
ac_val_op!(ac_bm_find, AcBookmarks, ensure_bm,
    rc_bm_find(url: *const c_char) -> u64 = 0);
ac_ptr_op!(ac_bm_import_html, AcBookmarks, ensure_bm,
    rc_bm_import_html(parent: u64, html: *const u8, len: usize, flags: i32));
ac_status_op!(ac_bm_export_html, AcBookmarks, ensure_bm,
    rc_bm_export_html(path: *const c_char));

// ---- settings (RC02) ----------------------------------------------------------

fn settings_hint(h: *const settings::RcSettings) -> Option<notify::ProfileHint> {
    unsafe { h.as_ref() }
        .and_then(|s| s.profile_id())
        .map(|id| notify::profile_hint(&id))
}

/// Settings op -> AcStatus: profile hint from the bound scope +
/// forward.
macro_rules! ac_set_status {
    ($name:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? )) => {
        /// # Safety
        /// `h` must come from ac_settings/rc_settings_open; pointer
        /// arguments per the rc_* contract.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *mut settings::RcSettings,
            $($a: $t),*
        ) -> RcStatus {
            let _hint = settings_hint(h);
            unsafe { crate::$rc(h, $($a),*) }
        }
    };
}
macro_rules! ac_set_ptr {
    ($name:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? )) => {
        /// # Safety
        /// `h` must come from ac_settings/rc_settings_open; free the
        /// result with ac_string_free.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *mut settings::RcSettings,
            $($a: $t),*
        ) -> *mut c_char {
            let _hint = settings_hint(h);
            unsafe { crate::$rc(h, $($a),*) }
        }
    };
}
macro_rules! ac_set_val {
    ($name:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? ) -> $ret:ty) => {
        /// # Safety
        /// `h` must come from ac_settings/rc_settings_open.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *mut settings::RcSettings,
            $($a: $t),*
        ) -> $ret {
            let _hint = settings_hint(h);
            unsafe { crate::$rc(h, $($a),*) }
        }
    };
}

ac_set_status!(ac_settings_import_ini, rc_settings_import_ini(path: *const c_char));
ac_set_val!(ac_settings_contains, rc_settings_contains(key: *const c_char) -> i32);
ac_set_status!(ac_settings_get,
    rc_settings_get(key: *const c_char, out: *mut crate::RcBuffer));
ac_set_status!(ac_settings_get_string,
    rc_settings_get_string(key: *const c_char, out: *mut crate::RcBuffer));
ac_set_status!(ac_settings_get_bool,
    rc_settings_get_bool(key: *const c_char, out: *mut i32));
ac_set_status!(ac_settings_get_int,
    rc_settings_get_int(key: *const c_char, out: *mut i64));
ac_set_status!(ac_settings_get_double,
    rc_settings_get_double(key: *const c_char, out: *mut f64));
ac_set_status!(ac_settings_get_bytes,
    rc_settings_get_bytes(key: *const c_char, out: *mut crate::RcBuffer));
ac_set_status!(ac_settings_get_list,
    rc_settings_get_list(key: *const c_char, out: *mut crate::RcBuffer));
ac_set_status!(ac_settings_set_string,
    rc_settings_set_string(key: *const c_char, value: *const c_char));
ac_set_status!(ac_settings_set_bool,
    rc_settings_set_bool(key: *const c_char, value: i32));
ac_set_status!(ac_settings_set_int,
    rc_settings_set_int(key: *const c_char, value: i64));
ac_set_status!(ac_settings_set_double,
    rc_settings_set_double(key: *const c_char, value: f64));
ac_set_status!(ac_settings_set_bytes,
    rc_settings_set_bytes(key: *const c_char, value: *const u8, len: usize));
ac_set_status!(ac_settings_set_list,
    rc_settings_set_list(key: *const c_char, json: *const u8, len: usize));
ac_set_status!(ac_settings_remove, rc_settings_remove(key: *const c_char));
ac_set_ptr!(ac_settings_list, rc_settings_list(prefix: *const c_char));
ac_set_status!(ac_settings_sync, rc_settings_sync());

// ---- profiles (RC03) ----------------------------------------------------------

macro_rules! ac_prof_status {
    ($name:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? )) => {
        /// # Safety
        /// `h` must come from ac_profiles/rc_profiles_open.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *mut profiles::RcProfiles,
            $($a: $t),*
        ) -> RcStatus {
            unsafe { crate::$rc(h, $($a),*) }
        }
    };
}
macro_rules! ac_prof_ptr {
    ($name:ident, $rc:ident ( $($a:ident : $t:ty),* $(,)? )) => {
        /// # Safety
        /// `h` must come from ac_profiles/rc_profiles_open; free the
        /// result with ac_string_free.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *mut profiles::RcProfiles,
            $($a: $t),*
        ) -> *mut c_char {
            unsafe { crate::$rc(h, $($a),*) }
        }
    };
}

ac_prof_status!(ac_profiles_reload, rc_profiles_reload());
ac_prof_ptr!(ac_profiles_list, rc_profiles_list());
ac_prof_ptr!(ac_profiles_get, rc_profiles_get(id: *const c_char));
ac_prof_ptr!(ac_profiles_create,
    rc_profiles_create(display_name: *const c_char));
ac_prof_status!(ac_profiles_rename,
    rc_profiles_rename(id: *const c_char, name: *const c_char));
ac_prof_status!(ac_profiles_set_avatar,
    rc_profiles_set_avatar(id: *const c_char, name_or_null: *const c_char));
ac_prof_status!(ac_profiles_delete,
    rc_profiles_delete(id: *const c_char));
ac_prof_ptr!(ac_profiles_active, rc_profiles_active());
ac_prof_status!(ac_profiles_set_active,
    rc_profiles_set_active(id: *const c_char));
ac_prof_ptr!(ac_profiles_dir, rc_profiles_dir(id: *const c_char));
ac_prof_ptr!(ac_profiles_cache_dir,
    rc_profiles_cache_dir(id: *const c_char));
ac_prof_ptr!(ac_profiles_engine_storage_dir,
    rc_profiles_engine_storage_dir(id: *const c_char));
ac_prof_ptr!(ac_profiles_engine_cache_dir,
    rc_profiles_engine_cache_dir(id: *const c_char));
ac_prof_ptr!(ac_profiles_settings_dir,
    rc_profiles_settings_dir(id: *const c_char));
ac_prof_ptr!(ac_profiles_avatar_path,
    rc_profiles_avatar_path(id: *const c_char));
ac_prof_ptr!(ac_profiles_file, rc_profiles_file());

/// Display name -> filesystem slug — the standalone rc spelling.
///
/// # Safety
/// `display_name` must be NUL-terminated UTF-8; free the result with
/// ac_string_free.
#[no_mangle]
pub unsafe extern "C" fn ac_profiles_slugify(
    display_name: *const c_char,
) -> *mut c_char {
    unsafe { crate::rc_profiles_slugify(display_name) }
}

// ---- policy: site decisions + verdicts (RC06) --------------------------------

ac_status_op!(ac_sitedec_get, AcPolicy, ensure_policy,
    rc_sitedec_get(kind: *const c_char, key: *const c_char,
                   out: *mut crate::RcBuffer));
ac_status_op!(ac_sitedec_set, AcPolicy, ensure_policy,
    rc_sitedec_set(kind: *const c_char, key: *const c_char,
                   value: *const c_char));
ac_status_op!(ac_sitedec_remove, AcPolicy, ensure_policy,
    rc_sitedec_remove(kind: *const c_char, key: *const c_char));
ac_status_op!(ac_sitedec_clear, AcPolicy, ensure_policy,
    rc_sitedec_clear(kind: *const c_char));
ac_status_op!(ac_sitedec_replace, AcPolicy, ensure_policy,
    rc_sitedec_replace(kind: *const c_char, json: *const u8, len: usize));
ac_ptr_op!(ac_sitedec_list, AcPolicy, ensure_policy,
    rc_sitedec_list(kind: *const c_char));
ac_ptr_op!(ac_sitedec_snapshot, AcPolicy, ensure_policy,
    rc_sitedec_snapshot());
ac_ptr_op!(ac_sitedec_lookup, AcPolicy, ensure_policy,
    rc_sitedec_lookup(kind: *const c_char, host: *const c_char));
ac_val_op!(ac_sitedec_store_present, AcPolicy, ensure_policy,
    rc_sitedec_store_present() -> i32 = 0);
ac_status_op!(ac_sitedec_reload, AcPolicy, ensure_policy,
    rc_sitedec_reload());
ac_status_op!(ac_sitedec_reset, AcPolicy, ensure_policy,
    rc_sitedec_reset());

ac_status_op!(ac_policy_load_snapshot, AcPolicy, ensure_policy,
    rc_policy_load_snapshot(json: *const u8, len: usize));
ac_ptr_op!(ac_policy_evaluate, AcPolicy, ensure_policy,
    rc_policy_evaluate(json: *const u8, len: usize));
ac_val_op!(ac_policy_cookie_filter, AcPolicy, ensure_policy,
    rc_policy_cookie_filter(json: *const u8, len: usize) -> i32 = -1);
ac_ptr_op!(ac_policy_call, AcPolicy, ensure_policy,
    rc_policy_call(json: *const u8, len: usize));

/// The verdict-state generation counter — process-global, no handle
/// needed (the rc spelling applies verbatim).
#[no_mangle]
pub extern "C" fn ac_policy_eval_generation() -> u64 {
    crate::rc_policy_eval_generation()
}

ac_val_op!(ac_blocklist_check, AcPolicy, ensure_policy,
    rc_blocklist_check(host: *const c_char) -> i32 = 0);
ac_status_op!(ac_blocklist_load, AcPolicy, ensure_policy,
    rc_blocklist_load(text: *const u8, len: usize));
ac_status_op!(ac_blocklist_reload, AcPolicy, ensure_policy,
    rc_blocklist_reload());
ac_val_op!(ac_blocklist_count, AcPolicy, ensure_policy,
    rc_blocklist_count() -> usize = 0);

ac_val_op!(ac_tranco_rank, AcPolicy, ensure_policy,
    rc_tranco_rank(host: *const c_char) -> u32 = 0);
ac_status_op!(ac_tranco_load, AcPolicy, ensure_policy,
    rc_tranco_load(text: *const u8, len: usize));
ac_status_op!(ac_tranco_reload, AcPolicy, ensure_policy,
    rc_tranco_reload());
ac_val_op!(ac_tranco_count, AcPolicy, ensure_policy,
    rc_tranco_count() -> usize = 0);
ac_val_op!(ac_tranco_file_entries, AcPolicy, ensure_policy,
    rc_tranco_file_entries(name: *const c_char) -> usize = 0);

ac_ptr_op!(ac_sitetrust_lookup, AcPolicy, ensure_sitetrust,
    rc_sitetrust_lookup(host: *const c_char, is_https: i32));
ac_ptr_op!(ac_sitetrust_lookup_opts, AcPolicy, ensure_sitetrust,
    rc_sitetrust_lookup_opts(host: *const c_char, is_https: i32,
                             probes: u32));
ac_status_op!(ac_sitetrust_reload, AcPolicy, ensure_policy,
    rc_sitetrust_reload());
ac_ptr_op!(ac_lookalike_check, AcPolicy, ensure_policy,
    rc_lookalike_check(host: *const c_char));

ac_ptr_op!(ac_ua_spoof, AcPolicy, ensure_policy,
    rc_ua_spoof(host: *const c_char));
ac_status_op!(ac_ua_spoof_set, AcPolicy, ensure_policy,
    rc_ua_spoof_set(host: *const c_char, ua: *const c_char));
ac_status_op!(ac_ua_spoof_remove, AcPolicy, ensure_policy,
    rc_ua_spoof_remove(host: *const c_char));
ac_ptr_op!(ac_ua_spoof_list, AcPolicy, ensure_policy,
    rc_ua_spoof_list());

// ---- sessions (RC13) ---------------------------------------------------
//
// Path-explicit ops — they call the session module's *_at/*_in fns on
// the bound profile dir directly rather than forwarding through rc_*,
// so the handle needs no global re-anchor at all.  Session ops fire
// no change callbacks: the file is per-run scratch state (each save
// is the whole window set), not shared-observable data.

fn sess_ref<'a>(h: *const AcSession) -> Result<&'a AcSession, RcStatus> {
    match unsafe { h.as_ref() } {
        Some(h) => Ok(h),
        None => fail(RcStatus::InvalidArgument, "bad session handle"),
    }
}

fn sess_dir(h: &AcSession) -> PathBuf {
    PathBuf::from(h.dir.to_string_lossy().into_owned())
}

fn sess_file(h: &AcSession) -> PathBuf {
    sess_dir(h).join(crate::session::FILE_NAME)
}

fn sess_status(r: error::RcResult<()>) -> RcStatus {
    match r {
        Ok(()) => RcStatus::Ok,
        Err(e) => {
            error::set_error(&e.msg);
            e.status
        }
    }
}

/// Validates + atomically installs the bound profile's session
/// manifest — the rc_session_save contract on a bound path.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ac_session_save(
    h: *const AcSession,
    json: *const u8,
    len: usize,
) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let data = match unsafe { util::bytes(json, len) } {
        Some(d) => d,
        None => {
            error::set_error("bad session pointer");
            return RcStatus::InvalidArgument;
        }
    };
    sess_status(crate::session::save_at(&sess_file(h), data))
}

/// Decodes the bound profile's session into its canonical JSON
/// manifest (ac_buffer_free the result).  AC_ERR_NOT_FOUND when none.
///
/// # Safety
/// `out` receives a buffer to release with ac_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn ac_session_load(
    h: *const AcSession,
    out: *mut crate::RcBuffer,
) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    match crate::session::load_at(&sess_file(h)) {
        Ok(json) => {
            crate::buffer_out(out, json);
            RcStatus::Ok
        }
        Err(e) => {
            error::set_error(&e.msg);
            e.status
        }
    }
}

/// True when the bound profile's session file exists.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_exists(h: *const AcSession) -> i32 {
    match sess_ref(h) {
        Ok(h) => crate::session::exists_at(&sess_file(h)) as i32,
        Err(_) => 0,
    }
}

/// Deletes the bound profile's session file; absent is not an error.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_clear(h: *const AcSession) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    sess_status(crate::session::clear_at(&sess_file(h)))
}

/// Boots the bound profile's session marker — 1 when the previous
/// run died uncleanly, 0 on a clean/absent record, -1 on error.
/// Same semantics as rc_session_mark_start.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_mark_start(h: *const AcSession) -> i32 {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(_) => return -1,
    };
    match crate::session::mark_start_in(&sess_dir(h)) {
        Ok(dirty) => dirty as i32,
        Err(e) => {
            error::set_error(&e.msg);
            -1
        }
    }
}

/// Records a clean exit on the bound profile's marker.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_mark_exit(
    h: *const AcSession,
    code: i32,
) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    sess_status(crate::session::mark_exit_in(&sess_dir(h), code))
}

/// Records a handled signal termination on the bound profile's marker.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_mark_terminated(
    h: *const AcSession,
    signal: i32,
) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    sess_status(crate::session::mark_terminated_in(&sess_dir(h), signal))
}

/// True while the bound profile's session marker reads armed.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_dirty(h: *const AcSession) -> i32 {
    match sess_ref(h) {
        Ok(h) => crate::session::dirty_in(&sess_dir(h)) as i32,
        Err(_) => 0,
    }
}

/// The bound profile's consecutive-unclean count.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_crash_count(h: *const AcSession) -> i32 {
    match sess_ref(h) {
        Ok(h) => crate::session::crash_count_in(&sess_dir(h)) as i32,
        Err(_) => 0,
    }
}

/// The bound profile's last recorded exit breadcrumb —
/// AC_ERR_NOT_FOUND when none was ever written.
///
/// # Safety
/// `out` receives a buffer to release with ac_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn ac_session_last_exit(
    h: *const AcSession,
    out: *mut crate::RcBuffer,
) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    match crate::session::last_exit_in(&sess_dir(h)) {
        Some(s) => {
            crate::buffer_out(out, s.into_bytes());
            RcStatus::Ok
        }
        None => {
            error::set_error("no recorded exit");
            RcStatus::NotFound
        }
    }
}

/// Arms/disarms the bound profile's restore-loop guard.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_set_restoring(
    h: *const AcSession,
    on: i32,
) -> RcStatus {
    let h = match sess_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    sess_status(crate::session::set_restoring_in(&sess_dir(h), on != 0))
}

/// True while the bound profile's restore guard reads armed.
///
/// # Safety
/// `h` must come from ac_session.
#[no_mangle]
pub unsafe extern "C" fn ac_session_restoring(h: *const AcSession) -> i32 {
    match sess_ref(h) {
        Ok(h) => crate::session::restoring_in(&sess_dir(h)) as i32,
        Err(_) => 0,
    }
}

// ---- cookies (RC15) ----------------------------------------------------
//
// Process-global db behind the open-path anchor — the history model:
// ensure_cookies re-asserts profiles/<id>/cookies.db (a no-op on the
// hot path) then the op forwards verbatim.  The role flag is the
// honesty seam: a Servo/WPE backend declares authoritative (this file
// IS the jar); a Chromium mirror declares mirror (the engine owns the
// jar, this is its durable shadow).

fn ensure_cookies<'a>(h: *const AcCookies) -> Result<&'a AcCookies, RcStatus> {
    let h = match unsafe { h.as_ref() } {
        Some(h) => h,
        None => return fail(RcStatus::InvalidArgument, "bad cookies handle"),
    };
    match unsafe { crate::rc_cookies_open(h.db_path.as_ptr()) } {
        RcStatus::Ok => Ok(h),
        st => Err(st),
    }
}

ac_val_op!(ac_cookies_role, AcCookies, ensure_cookies,
    rc_cookies_role() -> i64 = -1);
ac_status_op!(ac_cookies_set_role, AcCookies, ensure_cookies,
    rc_cookies_set_role(role: i64));
ac_status_op!(ac_cookies_set, AcCookies, ensure_cookies,
    rc_cookies_set(json: *const u8, len: usize));
ac_status_op!(ac_cookies_get, AcCookies, ensure_cookies,
    rc_cookies_get(domain: *const c_char, name: *const c_char,
                   path: *const c_char, container: *const c_char,
                   out: *mut crate::RcBuffer));
ac_status_op!(ac_cookies_remove, AcCookies, ensure_cookies,
    rc_cookies_remove(domain: *const c_char, name: *const c_char,
                      path: *const c_char, container: *const c_char));
ac_ptr_op!(ac_cookies_list, AcCookies, ensure_cookies,
    rc_cookies_list(container: *const c_char));
ac_ptr_op!(ac_cookies_for_url, AcCookies, ensure_cookies,
    rc_cookies_for_url(request: *const u8, len: usize));
ac_ptr_op!(ac_cookies_store_header, AcCookies, ensure_cookies,
    rc_cookies_store_header(url: *const c_char, header: *const c_char,
                            container: *const c_char));
ac_ptr_op!(ac_cookies_gate, AcCookies, ensure_cookies,
    rc_cookies_gate(input: *const u8, len: usize));
ac_ptr_op!(ac_cookies_import, AcCookies, ensure_cookies,
    rc_cookies_import(path: *const c_char, container: *const c_char));
ac_status_op!(ac_cookies_export, AcCookies, ensure_cookies,
    rc_cookies_export(path: *const c_char, container: *const c_char));
ac_status_op!(ac_cookies_clear, AcCookies, ensure_cookies,
    rc_cookies_clear(container: *const c_char));
ac_status_op!(ac_cookies_remove_for_host, AcCookies, ensure_cookies,
    rc_cookies_remove_for_host(host: *const c_char,
                               container: *const c_char));
ac_val_op!(ac_cookies_count, AcCookies, ensure_cookies,
    rc_cookies_count(container: *const c_char) -> i64 = -1);
ac_status_op!(ac_cookies_purge_expired, AcCookies, ensure_cookies,
    rc_cookies_purge_expired(now_ms: i64));
ac_status_op!(ac_cookies_purge_session, AcCookies, ensure_cookies,
    rc_cookies_purge_session());

// ---- icons (RC16) ----------------------------------------------------
//
// Path-explicit like the session lane: the handle's profiles/<id>/ dir
// is resolved once at bind and every op works under its
// cache/icons/ subtree — no process-global anchor exists to
// re-assert.  Mutations emit "icons" stamped with the bound profile.

fn icons_ref<'a>(h: *const AcIcons) -> Result<&'a AcIcons, RcStatus> {
    match unsafe { h.as_ref() } {
        Some(h) => Ok(h),
        None => fail(RcStatus::InvalidArgument, "bad icons handle"),
    }
}

fn icons_profile_dir(h: &AcIcons) -> PathBuf {
    PathBuf::from(h.dir.to_string_lossy().into_owned())
}

fn icons_arg<'a>(ptr: *const c_char) -> Result<&'a str, RcStatus> {
    match unsafe { util::cstr(ptr) } {
        Some(s) => Ok(s),
        None => fail(RcStatus::InvalidArgument, "bad icon url pointer"),
    }
}

fn icons_st<T>(r: error::RcResult<T>) -> RcStatus {
    match r {
        Ok(_) => RcStatus::Ok,
        Err(e) => {
            error::set_error(&e.msg);
            e.status
        }
    }
}

/// Records the favicon PNG for `url`'s icon key in the bound
/// profile's cache — dedup-by-host, identical-bytes rewrites are
/// no-ops.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8; `png` points to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_put(
    h: *const AcIcons,
    url: *const c_char,
    png: *const u8,
    len: usize,
) -> RcStatus {
    let h = match icons_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let url = match icons_arg(url) {
        Ok(u) => u,
        Err(s) => return s,
    };
    let data = match unsafe { util::bytes(png, len) } {
        Some(d) => d,
        None => {
            error::set_error("bad icon pointer");
            return RcStatus::InvalidArgument;
        }
    };
    match icons::put_in(&icons_profile_dir(h), url, data) {
        Ok(true) => {
            notify::emit("icons");
            RcStatus::Ok
        }
        Ok(false) => RcStatus::Ok,
        Err(e) => icons_st::<()>(Err(e)),
    }
}

/// The stored PNG for `url`'s key into `out` — AC_ERR_NOT_FOUND on a
/// miss.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8; `out` receives a buffer to
/// release with ac_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn ac_icons_get(
    h: *const AcIcons,
    url: *const c_char,
    out: *mut crate::RcBuffer,
) -> RcStatus {
    let h = match icons_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let url = match icons_arg(url) {
        Ok(u) => u,
        Err(s) => return s,
    };
    match icons::get_in(&icons_profile_dir(h), url) {
        Ok(png) => {
            crate::buffer_out(out, png);
            RcStatus::Ok
        }
        Err(e) => icons_st::<()>(Err(e)),
    }
}

/// Drops the record for `url`'s key — AC_OK either way.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_remove(
    h: *const AcIcons,
    url: *const c_char,
) -> RcStatus {
    let h = match icons_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let url = match icons_arg(url) {
        Ok(u) => u,
        Err(s) => return s,
    };
    match icons::remove_in(&icons_profile_dir(h), url) {
        Ok(true) => {
            notify::emit("icons");
            RcStatus::Ok
        }
        Ok(false) => RcStatus::Ok,
        Err(e) => icons_st::<()>(Err(e)),
    }
}

/// Wipes the bound profile's icon cache — the clearIconDatabase
/// equivalent.
///
/// # Safety
/// `h` must come from ac_icons.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_clear(h: *const AcIcons) -> RcStatus {
    let h = match icons_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let dir = icons_profile_dir(h);
    let had = icons::count_in(&dir).unwrap_or(0) > 0;
    let st = icons_st(icons::clear_in(&dir));
    if st == RcStatus::Ok && had {
        notify::emit("icons");
    }
    st
}

/// The expiry sweep — drops records not refreshed at-or-before
/// `cutoff_ms`; `out_removed` may be NULL.
///
/// # Safety
/// `h` must come from ac_icons.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_sweep(
    h: *const AcIcons,
    cutoff_ms: i64,
    out_removed: *mut i64,
) -> RcStatus {
    let h = match icons_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    match icons::sweep_in(&icons_profile_dir(h), cutoff_ms) {
        Ok(n) => {
            if !out_removed.is_null() {
                unsafe { *out_removed = n as i64 };
            }
            if n > 0 {
                notify::emit("icons");
            }
            RcStatus::Ok
        }
        Err(e) => icons_st::<()>(Err(e)),
    }
}

/// Record count; -1 on a bad handle.
///
/// # Safety
/// `h` must come from ac_icons.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_count(h: *const AcIcons) -> i64 {
    match icons_ref(h) {
        Ok(h) => icons::count_in(&icons_profile_dir(h)).unwrap_or(-1),
        Err(_) => -1,
    }
}

/// The store's icon key for `url` — caller frees with
/// ac_string_free; NULL on invalid input.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_key(
    h: *const AcIcons,
    url: *const c_char,
) -> *mut c_char {
    if icons_ref(h).is_err() {
        return ptr::null_mut();
    }
    match icons_arg(url) {
        Ok(u) => match icons::key_for(u) {
            Ok(k) => util::to_c_string(k),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        },
        Err(_) => ptr::null_mut(),
    }
}

/// The bound profile's index rows as a JSON array — caller frees
/// with ac_string_free; NULL on a bad handle.
///
/// # Safety
/// `h` must come from ac_icons.
#[no_mangle]
pub unsafe extern "C" fn ac_icons_list(h: *const AcIcons) -> *mut c_char {
    let h = match icons_ref(h) {
        Ok(h) => h,
        Err(_) => return ptr::null_mut(),
    };
    match icons::list_in(&icons_profile_dir(h)) {
        Ok(v) => match CString::new(v) {
            Ok(c) => c.into_raw(),
            Err(_) => {
                error::set_error("icon list is not utf8-clean");
                ptr::null_mut()
            }
        },
        Err(e) => {
            error::set_error(&e.msg);
            ptr::null_mut()
        }
    }
}

// ---- containers (RC19) ------------------------------------------------
//
// The registry is a typed specialization over the profile scope's
// settings store — every op opens a short-lived RcSettings on the
// bound scope, forwards to the rc_containers_* entry point and
// closes it (fresh decode per op, the AcContainers doc comment).
// Mutations emit "containers" stamped with the bound profile via
// the forwarded rc_* call.

fn containers_ref<'a>(
    h: *const AcContainers,
) -> Result<&'a AcContainers, RcStatus> {
    match unsafe { h.as_ref() } {
        Some(h) => Ok(h),
        None => fail(RcStatus::InvalidArgument, "bad containers handle"),
    }
}

/// The bound scope's settings store, opened for one op — NULL from
/// rc_settings_open already carries a stashed error.
fn containers_open(h: &AcContainers) -> Result<*mut settings::RcSettings, RcStatus> {
    let pid = match CString::new(h.profile.as_str()) {
        Ok(p) => p,
        Err(_) => return fail(RcStatus::InvalidArgument, "bad profile id"),
    };
    let s = unsafe { crate::rc_settings_open(h.root.as_ptr(), pid.as_ptr()) };
    if s.is_null() {
        return Err(RcStatus::Io);
    }
    Ok(s)
}

fn containers_close(s: *mut settings::RcSettings) {
    unsafe { crate::rc_settings_close(s) }
}

/// The bound profile's container registry — a JSON array of
/// {"id","name","color","icon"} in creation order.  Caller frees
/// with ac_string_free.
///
/// # Safety
/// `h` must come from ac_containers.
#[no_mangle]
pub unsafe extern "C" fn ac_containers_list(
    h: *const AcContainers,
) -> *mut c_char {
    let h = match containers_ref(h) {
        Ok(h) => h,
        Err(_) => return ptr::null_mut(),
    };
    let s = match containers_open(h) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    let out = unsafe { crate::rc_containers_list(s) };
    containers_close(s);
    out
}

/// One registry row {"id","name","color","icon"} — NULL when the id
/// is unregistered (a miss is not an error).  Caller frees with
/// ac_string_free.
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ac_containers_get(
    h: *const AcContainers,
    id: *const c_char,
) -> *mut c_char {
    let h = match containers_ref(h) {
        Ok(h) => h,
        Err(_) => return ptr::null_mut(),
    };
    let s = match containers_open(h) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    let out = unsafe { crate::rc_containers_get(s, id) };
    containers_close(s);
    out
}

/// Creates a container from JSON {"name","color","icon"?} — the id
/// is minted library-side.  Returns the row JSON; NULL + stashed
/// error on failure.  Caller frees with ac_string_free.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ac_containers_create(
    h: *const AcContainers,
    json: *const u8,
    len: usize,
) -> *mut c_char {
    let h = match containers_ref(h) {
        Ok(h) => h,
        Err(_) => return ptr::null_mut(),
    };
    let _hint = notify::profile_hint(&h.profile);
    let s = match containers_open(h) {
        Ok(s) => s,
        Err(_) => return ptr::null_mut(),
    };
    let out = unsafe { crate::rc_containers_create(s, json, len) };
    containers_close(s);
    out
}

/// Patches one registry row — JSON {"name"?,"color"?,"icon"?}; an
/// "icon" of "" or null clears it.  AC_ERR_NOT_FOUND on an
/// unregistered id.
///
/// # Safety
/// `id` must be NUL-terminated UTF-8; `json` points to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn ac_containers_update(
    h: *const AcContainers,
    id: *const c_char,
    json: *const u8,
    len: usize,
) -> RcStatus {
    let h = match containers_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let s = match containers_open(h) {
        Ok(s) => s,
        Err(st) => return st,
    };
    let st = unsafe { crate::rc_containers_update(s, id, json, len) };
    containers_close(s);
    st
}

/// The store half of deleteContainer — the registry group and the
/// "order" entry.  AC_ERR_NOT_FOUND on an unregistered id; the
/// on-disk storage trees are the caller's job
/// (ac_containers_namespace derives their paths).
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ac_containers_remove(
    h: *const AcContainers,
    id: *const c_char,
) -> RcStatus {
    let h = match containers_ref(h) {
        Ok(h) => h,
        Err(s) => return s,
    };
    let _hint = notify::profile_hint(&h.profile);
    let s = match containers_open(h) {
        Ok(s) => s,
        Err(st) => return st,
    };
    let st = unsafe { crate::rc_containers_remove(s, id) };
    containers_close(s);
    st
}

/// The bound profile's storage namespace for a registered id —
/// JSON {"storage_name","storage_path","cache_path",
/// "default_data_path","cookie_partition"}: the same paths
/// ContainerManager derives, plus the RC15 cookie-partition key.
/// Caller frees with ac_string_free.
///
/// # Safety
/// `container_id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn ac_containers_namespace(
    h: *const AcContainers,
    container_id: *const c_char,
) -> *mut c_char {
    let h = match containers_ref(h) {
        Ok(h) => h,
        Err(_) => return ptr::null_mut(),
    };
    let pid = match CString::new(h.profile.as_str()) {
        Ok(p) => p,
        Err(_) => return ptr::null_mut(),
    };
    unsafe {
        crate::rc_containers_namespace(h.root.as_ptr(), pid.as_ptr(), container_id)
    }
}
