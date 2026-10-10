//! arora-rustcore — Arora's shared, engine-neutral Rust core behind a
//! C ABI (the Mozilla application-services pattern).
//!
//! Conventions every component here follows:
//!   * All entry points are panic-safe (catch_unwind) and treat every
//!     pointer as untrusted — bad input degrades to a status code,
//!     never to UB or a crash crossing the FFI boundary.
//!   * Fallible calls return RcStatus; the accompanying message is
//!     available via rc_last_error_message() on the same thread.
//!   * Byte results are handed out as RcBuffer (free with
//!     rc_buffer_free); strings as char* (free with rc_string_free).
//!   * State changes announce themselves through the single
//!     rc_set_change_callback hook; RustCoreBridge re-emits them as
//!     queued Qt signals.
//!
//! Landed components:
//!   * credentials (RCORE01 part B): SecureStore custody semantics —
//!     AES-256-GCM "ARSEC1" blobs, securestore.key file custody,
//!     Argon2id master passphrase via securestore.kdf, plus the
//!     rc_cred_* named-credential map in credentials.dat.
//!   * urlstrip (SEC17): ClearURLs-style tracking-parameter rules —
//!     vendored JSON ruleset + data-dir override, strip over C FFI for
//!     the request interceptors.
//!   * blocklist (SEC18): local anti-phishing/malware domain list —
//!     vendored seed + merged data-dir override, exact+suffix host
//!     matching for the request interceptors.  No remote lookups.
//!   * parsers (SEC19): memory-safe parsing of attacker-influenced
//!     document formats — OpenSearch descriptors, gupdate extension
//!     manifests, suggestions replies and the XBEL structural gate.
//!     Qt XML/JSON only ever sees this crate's own output.
//!   * bookmarks (RCORE02a): the canonical bookmark tree — XBEL on
//!     disk, handle-addressed nodes, CRUD + lazy per-node queries
//!     (no bulk tree marshaling) and atomic saves.
//!   * history (RCORE02b): the canonical history store — rusqlite
//!     visits + per-host icon tables in <data dir>/history.db,
//!     write-through so the file is always current.
//!   * session (RCORE03): the canonical session store — versioned
//!     binary schema in <data dir>/session.dat, atomic writes, fully
//!     bounded decode, opaque per-window/per-tab engine blobs so the
//!     format survives an engine swap.
//!   * omnibox (OMNI01): the location-bar routing decision
//!     (url-or-search classification incl. the QUrl::fromUserInput
//!     heuristics) and the frecency-ranked history suggestions the
//!     completer displays — the Qt side thin-shells both.

mod bidi;
mod blocklist;
mod bookmarks;
mod cred;
mod error;
mod history;
mod notify;
mod omnibox;
mod parsers;
mod pdfsanitize;
mod policy;
mod session;
mod store;
mod urlstrip;
mod util;

use std::ffi::CString;
use std::os::raw::{c_char, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

pub use error::RcStatus;

/// Bytes owned by the caller after return; release the allocation
/// with rc_buffer_free().
#[repr(C)]
pub struct RcBuffer {
    pub data: *mut u8,
    pub len: usize,
}

fn status_of(f: impl FnOnce() -> error::RcResult<()>) -> RcStatus {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(Ok(())) => {
            error::clear_error();
            RcStatus::Ok
        }
        Ok(Err(e)) => {
            error::set_error(&e.msg);
            e.status
        }
        Err(_) => {
            error::set_error("panic inside rustcore");
            RcStatus::Crypto
        }
    }
}

fn buffer_out(out: *mut RcBuffer, v: Vec<u8>) {
    if let Some(out) = unsafe { out.as_mut() } {
        let (data, len) = util::into_raw_buffer(v);
        out.data = data;
        out.len = len;
    }
}

// ---- housekeeping --------------------------------------------------

/// Points the store at the application data directory.  Idempotent and
/// cheap — the Qt shim calls it before every operation so test-mode
/// path switches keep working.
///
/// # Safety
/// `utf8_path` must be NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn rc_set_data_dir(utf8_path: *const c_char) -> RcStatus {
    status_of(|| {
        let dir = unsafe { util::cstr(utf8_path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad data dir pointer".into(),
        })?;
        store::lock().set_data_dir(dir)
    })
}

/// Last error message on this thread ("" after a successful call).
/// Free the returned pointer with rc_string_free().
#[no_mangle]
pub unsafe extern "C" fn rc_last_error_message() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(error::last_error_copy))
        .unwrap_or(ptr::null_mut())
}

/// Frees a string handed out by this library.
///
/// # Safety
/// `s` must come from this library, or be null.
#[no_mangle]
pub unsafe extern "C" fn rc_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

/// Frees a buffer handed out by this library.
#[no_mangle]
pub unsafe extern "C" fn rc_buffer_free(buf: RcBuffer) {
    if !buf.data.is_null() {
        drop(unsafe {
            Box::from_raw(std::ptr::slice_from_raw_parts_mut(buf.data, buf.len))
        });
    }
}

/// Registers (or clears, with null) the process-wide change callback.
/// Invoked synchronously after mutations commit, never while the
/// store lock is held; the Qt bridge queues onto the GUI thread.
///
/// # Safety
/// `userdata` is passed back verbatim.
#[no_mangle]
pub unsafe extern "C" fn rc_set_change_callback(
    cb: Option<notify::RcChangeCallback>,
    userdata: *mut c_void,
) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        notify::set_callback(cb, userdata);
    }));
}

// ---- custody -------------------------------------------------------

/// Crypto + custody usable.  Mirrors the old isAvailable(): in file
/// mode it loads or mints securestore.key.
#[no_mangle]
pub unsafe extern "C" fn rc_is_available() -> i32 {
    catch_unwind(AssertUnwindSafe(|| store::lock().is_available() as i32))
        .unwrap_or(0)
}

/// True when `blob` carries the "ARSEC1" sealed-format magic.
///
/// # Safety
/// `blob` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_is_sealed(blob: *const u8, len: usize) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(blob, len) } {
            Some(b) => (b.len() >= 6 && &b[..6] == store::SEAL_MAGIC) as i32,
            None => 0,
        }
    }))
    .unwrap_or(0)
}

/// True when securestore.kdf exists (passphrase custody armed).
#[no_mangle]
pub unsafe extern "C" fn rc_passphrase_enabled() -> i32 {
    catch_unwind(AssertUnwindSafe(|| store::lock().passphrase_enabled() as i32))
        .unwrap_or(0)
}

/// False only in passphrase mode while no derived key is cached.
#[no_mangle]
pub unsafe extern "C" fn rc_is_unlocked() -> i32 {
    catch_unwind(AssertUnwindSafe(|| store::lock().is_unlocked() as i32))
        .unwrap_or(0)
}

// ---- blob crypto ---------------------------------------------------

/// Seal `plain` under the current custody key.
///
/// # Safety
/// `plain` must point to `len` readable bytes; `out` receives a
/// buffer to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_seal(
    plain: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(plain, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad input pointer".into(),
        })?;
        let blob = store::lock().seal(data)?;
        buffer_out(out, blob);
        Ok(())
    })
}

/// Verify + open a sealed blob.  RcStatus::Crypto is an authentication
/// (tamper/wrong-key) failure; Corrupt is malformed input.
///
/// # Safety
/// Same contract as rc_seal.
#[no_mangle]
pub unsafe extern "C" fn rc_open(
    blob: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(blob, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad input pointer".into(),
        })?;
        let plain = store::lock().open(data)?;
        buffer_out(out, plain);
        Ok(())
    })
}

/// Seal under an explicit 32-byte key — for custody transitions that
/// re-seal consumer stores between the old and new custody keys.
///
/// # Safety
/// `key` must point to 32 readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_seal_with_key(
    key: *const u8,
    plain: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let key = unsafe { util::key32(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let data = unsafe { util::bytes(plain, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad input pointer".into(),
        })?;
        let blob = store::seal_with_key(key, data)?;
        buffer_out(out, blob);
        Ok(())
    })
}

/// Open under an explicit 32-byte key.
///
/// # Safety
/// `key` must point to 32 readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_open_with_key(
    key: *const u8,
    blob: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let key = unsafe { util::key32(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let data = unsafe { util::bytes(blob, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad input pointer".into(),
        })?;
        let plain = store::open_with_key(key, data)?;
        buffer_out(out, plain);
        Ok(())
    })
}

// ---- passphrase custody --------------------------------------------

/// Derive + verify the passphrase against securestore.kdf.  Returns
/// WrongPassphrase when the verifier rejects the derived key.
///
/// # Safety
/// `pass` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_unlock(pass: *const u8, len: usize) -> RcStatus {
    status_of(|| {
        let pass = unsafe { util::bytes(pass, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad passphrase pointer".into(),
        })?;
        store::lock().unlock(pass)
    })
}

/// Drops every cached key (derived + file) and the decoded credential
/// map; key material is zeroized.
#[no_mangle]
pub unsafe extern "C" fn rc_lock() {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        store::lock().lock();
    }));
}

/// Writes a fresh securestore.kdf (new salt + verifier) and adopts the
/// derived key — the shared stage of enable/change-passphrase.  The
/// caller re-seals external consumers and retires the old artifact.
///
/// # Safety
/// `pass` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_kdf_create(pass: *const u8, len: usize) -> RcStatus {
    status_of(|| {
        let pass = unsafe { util::bytes(pass, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad passphrase pointer".into(),
        })?;
        store::lock().kdf_create(pass)
    })
}

/// Writes a fresh random securestore.key (0600) and caches it.
#[no_mangle]
pub unsafe extern "C" fn rc_key_file_create() -> RcStatus {
    status_of(|| store::lock().key_file_create())
}

/// Deletes securestore.key and drops the cached copy.
#[no_mangle]
pub unsafe extern "C" fn rc_key_file_delete() -> RcStatus {
    status_of(|| store::lock().key_file_delete())
}

/// Deletes securestore.kdf and drops the cached derived key.
#[no_mangle]
pub unsafe extern "C" fn rc_kdf_file_delete() -> RcStatus {
    status_of(|| store::lock().kdf_file_delete())
}

/// Copies the file key into `out32` (load-or-create semantics).
///
/// # Safety
/// `out32` must point to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_key_file_copy(out32: *mut u8) -> RcStatus {
    status_of(|| {
        let key = store::lock().load_or_create_file_key()?;
        match unsafe { out32.as_mut() } {
            Some(out) => {
                unsafe { std::ptr::copy_nonoverlapping(key.as_ptr(), out, store::KEY_SIZE) };
                Ok(())
            }
            None => error::fail(RcStatus::InvalidArgument, "bad key out pointer"),
        }
    })
}

/// Copies the cached derived key into `out32`, or RcStatus::Locked.
///
/// # Safety
/// `out32` must point to 32 writable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_derived_key_copy(out32: *mut u8) -> RcStatus {
    status_of(|| {
        let s = store::lock();
        match &s.derived_key {
            Some(key) => match unsafe { out32.as_mut() } {
                Some(out) => {
                    unsafe { std::ptr::copy_nonoverlapping(key.as_ptr(), out, store::KEY_SIZE) };
                    Ok(())
                }
                None => error::fail(RcStatus::InvalidArgument, "bad key out pointer"),
            },
            None => error::fail(RcStatus::Locked, "the store is locked"),
        }
    })
}

// ---- named credentials ----------------------------------------------

/// Fetches the credential `name` into `out` (rc_buffer_free).
///
/// # Safety
/// `name` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_cred_get(name: *const c_char, out: *mut RcBuffer) -> RcStatus {
    status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        let v = store::lock().cred_get(name)?;
        buffer_out(out, v);
        Ok(())
    })
}

/// Stores `value` under `name` and persists the map; fires the
/// "credentials" change callback when the value actually changed.
///
/// # Safety
/// `name` NUL-terminated UTF-8; `value` points to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_cred_put(
    name: *const c_char,
    value: *const u8,
    len: usize,
) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        let value = unsafe { util::bytes(value, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad value pointer".into(),
        })?;
        changed = store::lock().cred_put(name, value)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("credentials");
    }
    st
}

/// Removes `name`; NotFound when absent.
#[no_mangle]
pub unsafe extern "C" fn rc_cred_remove(name: *const c_char) -> RcStatus {
    let st = status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        store::lock().cred_remove(name).map(|_| ())
    });
    if st == RcStatus::Ok {
        notify::emit("credentials");
    }
    st
}

/// JSON array of credential names — free with rc_string_free().
#[no_mangle]
pub unsafe extern "C" fn rc_cred_list() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match store::lock().cred_names() {
            Ok(names) => util::to_c_string(
                serde_json::to_string(&names).unwrap_or_else(|_| "[]".into()),
            ),
            Err(_) => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Whole-op passphrase rotation covering the Rust-owned credential
/// file — validates custody, rewrites .kdf under a fresh salt and
/// re-seals credentials.dat.  (The Qt SecureStore shim keeps its own
/// staged version because it must interleave external consumer
/// re-sealing between the .kdf write and the .key retirement.)
///
/// # Safety
/// `pass` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_cred_change_passphrase(
    pass: *const u8,
    len: usize,
) -> RcStatus {
    status_of(|| {
        let pass = unsafe { util::bytes(pass, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad passphrase pointer".into(),
        })?;
        store::lock().cred_change_passphrase(pass)
    })
}

/// Re-seals credentials.dat from one explicit key to another during a
/// custody transition; no-op when the file does not exist.
///
/// # Safety
/// Both pointers must read 32 bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_cred_reseal(from: *const u8, to: *const u8) -> RcStatus {
    status_of(|| {
        let from = unsafe { util::key32(from) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad from-key pointer".into(),
        })?;
        let to = unsafe { util::key32(to) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad to-key pointer".into(),
        })?;
        store::lock().cred_reseal(from, to)
    })
}

// ---- URL cleaning (SEC17) --------------------------------------------

/// Returns `url` with tracking query parameters removed, or a copy of
/// the input when no rule fired.  NULL on argument error — see
/// rc_last_error_message().  Free with rc_string_free().
///
/// # Safety
/// `url` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_urlstrip(url: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(url) } {
            Some(u) => util::to_c_string(urlstrip::strip(u)),
            None => {
                error::set_error("bad url pointer");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Replaces the active strip ruleset with the given JSON document —
/// the update/test seam.  RC_CORRUPT on malformed input; the previous
/// ruleset stays active then.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_urlstrip_load_rules(
    json: *const u8,
    len: usize,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad rules pointer".into(),
        })?;
        let text = std::str::from_utf8(data).map_err(|_| error::Fail {
            status: RcStatus::Corrupt,
            msg: "urlstrip rules: not UTF-8".into(),
        })?;
        urlstrip::load_rules(text)
    })
}

/// Re-reads `<data dir>/urlstrip-rules.json` (or the vendored ruleset
/// when absent).  RC_CORRUPT when an override exists but does not
/// parse — the previous ruleset stays active.
#[no_mangle]
pub unsafe extern "C" fn rc_urlstrip_reload() -> RcStatus {
    status_of(|| urlstrip::reload())
}

// ---- domain blocklist (SEC18) ----------------------------------------

/// True when `host` equals a listed domain or sits beneath one.
/// Never fails on content — a bad pointer or malformed host reads as
/// "not listed", and a no-data-dir core answers from the vendored
/// seed.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_blocklist_check(host: *const c_char) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(host) } {
            Some(h) => blocklist::check(h) as i32,
            None => 0,
        }
    }))
    .unwrap_or(0)
}

/// Replaces the active list with the given text body (plain domains,
/// hostfile rows, URL rows — see the module docs).  RC_CORRUPT when
/// the body yields no usable entries; the previous list stays active.
///
/// # Safety
/// `text` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_blocklist_load(
    text: *const u8,
    len: usize,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(text, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad blocklist pointer".into(),
        })?;
        let body = std::str::from_utf8(data).map_err(|_| error::Fail {
            status: RcStatus::Corrupt,
            msg: "blocklist: not UTF-8".into(),
        })?;
        blocklist::load(body)
    })
}

/// Re-runs the vendored-seed ∪ `<data dir>/blocklist-domains.txt`
/// merge.  RC_CORRUPT when an override exists but yields zero usable
/// entries — the previous list stays active.
#[no_mangle]
pub unsafe extern "C" fn rc_blocklist_reload() -> RcStatus {
    status_of(|| blocklist::reload())
}

/// Entry count of the active list — diagnostics/tests.
#[no_mangle]
pub unsafe extern "C" fn rc_blocklist_count() -> usize {
    catch_unwind(AssertUnwindSafe(blocklist::count)).unwrap_or(0)
}

// ---- navigation policy (ENG02) ------------------------------------------
// The engine-agnostic request-policy core.  The Qt interceptors marshal
// a request manifest in; verdicts come out.  No Qt types cross here.

/// Pushes the policy snapshot (privacy toggles + https-only exceptions)
/// from the GUI thread.  JSON body, RC_CORRUPT on malformed input.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_policy_load_snapshot(
    json: *const u8,
    len: usize,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad snapshot pointer".into(),
        })?;
        let manifest = policy::parse_json(data)?;
        policy::load_snapshot_json(&manifest)
    })
}

/// Evaluates one request manifest (JSON) and returns the verdict as a
/// JSON string: {"action":"pass"|"block"|"redirect"|"allow", ...}.
/// NULL on malformed input or panic — the adapter must fail open
/// (treat NULL as "allow", never as a block).
/// Free the result with rc_string_free().
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_policy_evaluate(
    json: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => {
                let verdict = policy::evaluate_json(&manifest);
                util::to_c_string(verdict.to_string())
            }
            None => {
                error::set_error("bad policy manifest");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Cookie-gate decision: 1 = accept the cookie, 0 = reject,
/// -1 on malformed input (adapter fails open → accept).
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_policy_cookie_filter(
    json: *const u8,
    len: usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
            .and_then(|m| policy::cookie_filter_json(&m))
        {
            Some(allow) => allow as i32,
            None => -1,
        }
    }))
    .unwrap_or(-1)
}

/// Generic granular-policy endpoint — JSON in {"op":..., params},
/// JSON out.  Covers every historical decision point (downgrade
/// marks, allowances, referer rewriting, host classifiers, blocked-nav
/// registries...).  NULL on error — see rc_last_error_message().
/// Free the result with rc_string_free().
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_policy_call(
    json: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match policy::call_json(&manifest) {
                Ok(result) => util::to_c_string(result.to_string()),
                Err(e) => {
                    error::set_error(&e.msg);
                    ptr::null_mut()
                }
            },
            None => {
                error::set_error("bad policy call payload");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- untrusted-document parsers (SEC19) ----------------------------

/// Parses an OpenSearch 1.1 description document (<= 1 MiB) into a
/// JSON field map — see parsers.rs for the schema.  The Qt reader
/// raises the returned error verbatim; malformed input is RC_CORRUPT.
///
/// # Safety
/// `xml` must point to `len` readable bytes; `out` receives a buffer
/// to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_opensearch_parse(
    xml: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(xml, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad document pointer".into(),
        })?;
        let json = parsers::opensearch(data)?;
        buffer_out(out, json);
        Ok(())
    })
}

/// Parses a gupdate extension-update manifest (<= 1 MiB) into
/// {"offers":[[appid,status,codebase,version],...]}.  RC_CORRUPT on
/// malformed XML — the caller keeps all policy decisions.
///
/// # Safety
/// Same contract as rc_opensearch_parse.
#[no_mangle]
pub unsafe extern "C" fn rc_updatemanifest_parse(
    xml: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(xml, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad manifest pointer".into(),
        })?;
        let json = parsers::update_manifest(data)?;
        buffer_out(out, json);
        Ok(())
    })
}

/// Structural gate for XBEL bookmark documents (<= 64 MiB):
/// well-formed, <xbel> root with absent/"1.0" version, nesting within
/// the reader's bound.  RC_OK lets Qt parse; RC_CORRUPT refuses.
///
/// # Safety
/// `xml` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_xbel_check(xml: *const u8, len: usize) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(xml, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad document pointer".into(),
        })?;
        parsers::xbel_check(data)
    })
}

/// Parses an OpenSearch suggestions reply into a JSON array of
/// strings.  RC_CORRUPT on malformed input or a reply that is not the
/// [term, [...]] shape — the caller emits nothing either way.
///
/// # Safety
/// Same contract as rc_opensearch_parse.
#[no_mangle]
pub unsafe extern "C" fn rc_suggest_parse(
    json_in: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(json_in, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad reply pointer".into(),
        })?;
        let json = parsers::suggestions(data)?;
        buffer_out(out, json);
        Ok(())
    })
}

// ---- PDF sanitization (PDF01) ----------------------------------------
//
// The in-browser viewer never sees remote PDF bytes directly: the Qt
// side downloads the file, hands the body here and displays only the
// sanitized rewrite.  RC_CORRUPT means the input was not a parseable
// PDF — the caller then refuses the view rather than falling back to
// unsanitized bytes.

/// Rewrites a PDF body with its action surface stripped (JS/Launch/
/// OpenAction/AA triggers, embedded files, XFA, remote submits).
///
/// # Safety
/// `data` must point to `len` readable bytes; `out` receives a buffer
/// to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_pdf_sanitize(
    data: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let input = unsafe { util::bytes(data, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad input pointer".into(),
        })?;
        let cleaned = pdfsanitize::sanitize(input).map_err(|msg| error::Fail {
            status: RcStatus::Corrupt,
            msg,
        })?;
        buffer_out(out, cleaned);
        Ok(())
    })
}

// ---- bookmark store (RCORE02a) -------------------------------------
//
// The canonical bookmark tree lives here; the Qt side keeps
// BookmarkNode proxies bound to node handles.  Models enumerate
// children one node per call — there is deliberately no bulk
// tree-marshaling entry point.

/// The root node handle — always valid once the store exists.
/// Handle 0 is the null handle everywhere below.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_root() -> u64 {
    catch_unwind(AssertUnwindSafe(|| bookmarks::with(|b| b.root()))).unwrap_or(0)
}

/// Loads (replaces) the tree from an XBEL file; a missing file is an
/// empty store, not an error.  RC_CORRUPT on malformed input — the
/// previous tree stays untouched then.
///
/// # Safety
/// `path` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_load(path: *const c_char) -> RcStatus {
    let st = status_of(|| {
        let p = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad path pointer".into(),
        })?;
        bookmarks::with(|b| b.load_path(std::path::Path::new(p)))
    });
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// Loads the tree from an in-memory XBEL document — for content Qt
/// owns (the bundled :defaultbookmarks.xbel resource, test fixtures).
///
/// # Safety
/// `xbel` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_load_mem(xbel: *const u8, len: usize) -> RcStatus {
    let st = status_of(|| {
        let data = unsafe { util::bytes(xbel, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad document pointer".into(),
        })?;
        bookmarks::with(|b| b.load_bytes(data))
    });
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// Serializes the tree to `path` atomically (temp + fsync + rename).
///
/// # Safety
/// `path` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_save(path: *const c_char) -> RcStatus {
    status_of(|| {
        let p = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad path pointer".into(),
        })?;
        bookmarks::with(|b| b.save_path(std::path::Path::new(p)))
    })
}

/// Child count of `node`, or -1 on a bad handle.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_child_count(node: u64) -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        bookmarks::with(|b| b.child_count(node).map(|c| c as i64).unwrap_or(-1))
    }))
    .unwrap_or(-1)
}

/// Handle of `node`'s child at `row`, or 0.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_child_at(node: u64, row: i64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        bookmarks::with(|b| b.child_at(node, row).unwrap_or(0))
    }))
    .unwrap_or(0)
}

/// Parent handle of `node`, or 0 (root / detached / bad handle).
#[no_mangle]
pub unsafe extern "C" fn rc_bm_parent(node: u64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        bookmarks::with(|b| b.parent_of(node).unwrap_or(0))
    }))
    .unwrap_or(0)
}

/// JSON description of `node`:
/// {"type","title","url","desc","expanded","tags":[...]}.
/// Free with rc_string_free(); NULL on a bad handle.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_get(node: u64) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        bookmarks::with(|b| match b.get_json(node) {
            Some(j) => util::to_c_string(j),
            None => ptr::null_mut(),
        })
    }))
    .unwrap_or(ptr::null_mut())
}

/// Creates a node from a JSON description (same shape as rc_bm_get)
/// and links it under `parent` at `row` (-1 / past-the-end appends).
/// Returns the new handle, 0 on error (rc_last_error_message).
///
/// # Safety
/// `json` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_create(
    parent: u64,
    row: i64,
    json: *const c_char,
) -> u64 {
    match catch_unwind(AssertUnwindSafe(|| {
        let text = match unsafe { util::cstr(json) } {
            Some(t) => t,
            None => {
                error::set_error("bad json pointer");
                return 0;
            }
        };
        let v: serde_json::Value = match serde_json::from_str(text) {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&format!("bad node json: {e}"));
                return 0;
            }
        };
        match bookmarks::with(|b| b.create(parent, row, &v)) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                0
            }
        }
    })) {
        Ok(h) => {
            if h != 0 {
                notify::emit("bookmarks");
            }
            h
        }
        Err(_) => {
            error::set_error("panic inside rustcore");
            0
        }
    }
}

/// Links a detached node under `parent` at `row`.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_attach(parent: u64, row: i64, node: u64) -> RcStatus {
    let st = status_of(|| bookmarks::with(|b| b.attach(parent, row, node)));
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// Unlinks `node` from its parent; the subtree stays alive until
/// attach() or destroy() (this is what undo re-links).
#[no_mangle]
pub unsafe extern "C" fn rc_bm_detach(node: u64) -> RcStatus {
    let st = status_of(|| bookmarks::with(|b| b.detach(node)));
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// Frees a detached subtree for good.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_destroy(node: u64) -> RcStatus {
    let st = status_of(|| bookmarks::with(|b| b.destroy(node)));
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// # Safety
/// `value` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_set_title(node: u64, value: *const c_char) -> RcStatus {
    bm_set_str(node, value, "title")
}

/// # Safety
/// `value` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_set_url(node: u64, value: *const c_char) -> RcStatus {
    bm_set_str(node, value, "url")
}

/// # Safety
/// `value` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_set_desc(node: u64, value: *const c_char) -> RcStatus {
    bm_set_str(node, value, "desc")
}

unsafe fn bm_set_str(node: u64, value: *const c_char, field: &'static str) -> RcStatus {
    let st = status_of(|| {
        let v = unsafe { util::cstr(value) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad value pointer".into(),
        })?;
        bookmarks::with(|b| b.set_str(node, field, v))
    });
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

#[no_mangle]
pub unsafe extern "C" fn rc_bm_set_expanded(node: u64, expanded: i32) -> RcStatus {
    let st = status_of(|| bookmarks::with(|b| b.set_expanded(node, expanded != 0)));
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// Replaces the node's tag list from a JSON string array.
///
/// # Safety
/// `json` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_set_tags(node: u64, json: *const c_char) -> RcStatus {
    let st = status_of(|| {
        let text = unsafe { util::cstr(json) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad tags pointer".into(),
        })?;
        let v: serde_json::Value =
            serde_json::from_str(text).map_err(|e| error::Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("bad tags json: {e}"),
            })?;
        let tags = v
            .as_array()
            .map(|a| {
                a.iter()
                    .filter_map(|t| t.as_str().map(str::to_owned))
                    .collect()
            })
            .ok_or_else(|| error::Fail {
                status: RcStatus::InvalidArgument,
                msg: "tags must be a json array".into(),
            })?;
        bookmarks::with(|b| b.set_tags(node, tags))
    });
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// First bookmark handle whose url matches exactly — the "is this
/// page bookmarked" dedup query.  0 when absent.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_find(url: *const c_char) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(url) } {
            Some(u) => bookmarks::with(|b| b.find_url(u).unwrap_or(0)),
            None => 0,
        }
    }))
    .unwrap_or(0)
}

// ---- history store (RCORE02b) ----------------------------------------
//
// rusqlite-backed visit log + per-host icon table in
// <data dir>/history.db (explicit path override for tests).  Every
// mutation writes through — there is no save() call.

/// Opens the history database at `path_or_null`; NULL resolves to
/// <data dir>/history.db (requires rc_set_data_dir).  Reopening the
/// same path is a no-op.
///
/// # Safety
/// `path_or_null` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_open(path_or_null: *const c_char) -> RcStatus {
    status_of(|| {
        let p = if path_or_null.is_null() {
            None
        } else {
            Some(unsafe { util::cstr(path_or_null) }.ok_or_else(|| error::Fail {
                status: RcStatus::InvalidArgument,
                msg: "bad path pointer".into(),
            })?)
        };
        history::with(|h| h.open(p))
    })
}

/// Whether a history database exists on disk at `path_or_null`
/// (NULL = the default <data dir> path) without opening it — the Qt
/// side's "does the legacy file still need importing" check.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_exists(path_or_null: *const c_char) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let p = if path_or_null.is_null() {
            None
        } else {
            unsafe { util::cstr(path_or_null) }
        };
        match p {
            Some(p) => history::HistoryStore::exists_on_disk(Some(p)) as i32,
            None => history::HistoryStore::exists_on_disk(None) as i32,
        }
    }))
    .unwrap_or(0)
}

/// Entry count of the deduped listing, -1 when the store is not open.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_count() -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        history::with(|h| h.count().unwrap_or(-1))
    }))
    .unwrap_or(-1)
}

/// JSON {"url","title","ts"} for row `row` of the deduped
/// newest-first listing.  NULL out of range or on error.
/// Free with rc_string_free().
#[no_mangle]
pub unsafe extern "C" fn rc_hist_entry_at(row: i64) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        history::with(|h| match h.entry_at(row) {
            Ok(Some(j)) => util::to_c_string(j),
            Ok(None) => ptr::null_mut(),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        })
    }))
    .unwrap_or(ptr::null_mut())
}

/// Appends a visit (ts_ms = QDateTime::toMSecsSinceEpoch).  Duplicates
/// are stored — dedup is a listing property, like the legacy file.
///
/// # Safety
/// `url`/`title` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_add(
    url: *const c_char,
    title: *const c_char,
    ts_ms: i64,
) -> RcStatus {
    let st = status_of(|| {
        let u = unsafe { util::cstr(url) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad url pointer".into(),
        })?;
        let t = unsafe { util::cstr(title) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad title pointer".into(),
        })?;
        history::with(|h| h.add(u, t, ts_ms))
    });
    if st == RcStatus::Ok {
        notify::emit("history");
    }
    st
}

/// Sets the title on the newest visit to `url`.
///
/// # Safety
/// `url`/`title` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_update_title(
    url: *const c_char,
    title: *const c_char,
) -> RcStatus {
    let st = status_of(|| {
        let u = unsafe { util::cstr(url) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad url pointer".into(),
        })?;
        let t = unsafe { util::cstr(title) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad title pointer".into(),
        })?;
        history::with(|h| h.update_title(u, t))
    });
    if st == RcStatus::Ok {
        notify::emit("history");
    }
    st
}

/// Removes the newest row exactly matching (url, title, ts_ms).
///
/// # Safety
/// `url`/`title` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_remove(
    url: *const c_char,
    title: *const c_char,
    ts_ms: i64,
) -> RcStatus {
    let st = status_of(|| {
        let u = unsafe { util::cstr(url) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad url pointer".into(),
        })?;
        let t = unsafe { util::cstr(title) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad title pointer".into(),
        })?;
        history::with(|h| h.remove(u, t, ts_ms))
    });
    if st == RcStatus::Ok {
        notify::emit("history");
    }
    st
}

/// Drops every visit (icons are kept — clearIcons is separate,
/// matching HistoryManager::clear() vs clearIcons()).
#[no_mangle]
pub unsafe extern "C" fn rc_hist_clear() -> RcStatus {
    let st = status_of(|| history::with(|h| h.clear()));
    if st == RcStatus::Ok {
        notify::emit("history");
    }
    st
}

/// Stores/updates the favicon PNG for `host` (per-host keying — the
/// HIST01 icon store folded into the core).
///
/// # Safety
/// `host` must be NUL-terminated UTF-8; `png` points to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_icon_set(
    host: *const c_char,
    png: *const u8,
    len: usize,
) -> RcStatus {
    status_of(|| {
        let h = unsafe { util::cstr(host) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad host pointer".into(),
        })?;
        let data = unsafe { util::bytes(png, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad icon pointer".into(),
        })?;
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        history::with(|s| s.icon_set(h, data, now))
    })
}

/// Fetches the stored favicon PNG for `host`; RC_NOT_FOUND when the
/// host has none.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8; `out` receives a buffer to
/// release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_hist_icon_get(host: *const c_char, out: *mut RcBuffer) -> RcStatus {
    status_of(|| {
        let h = unsafe { util::cstr(host) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad host pointer".into(),
        })?;
        let png = history::with(|s| s.icon_get(h))?;
        buffer_out(out, png);
        Ok(())
    })
}

/// Drops every stored favicon.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_icon_clear() -> RcStatus {
    status_of(|| history::with(|s| s.icon_clear()))
}

// ---- session store (RCORE03) -------------------------------------------

/// Validates the JSON session manifest and atomically installs it as
/// `<data dir>/session.dat` (temp + fsync + rename, 0600).  RC_CORRUPT
/// rejects a malformed manifest without touching the on-disk session.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_session_save(json: *const u8, len: usize) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad session pointer".into(),
        })?;
        session::save(data)
    })
}

/// Decodes the stored session into its canonical JSON manifest
/// (rc_buffer_free the result).  RC_NOT_FOUND when no session exists,
/// RC_CORRUPT when the file is malformed — the caller treats the
/// session as absent and never loops a crash prompt on it.
///
/// # Safety
/// `out` receives a buffer to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_session_load(out: *mut RcBuffer) -> RcStatus {
    status_of(|| {
        let json = session::load()?;
        buffer_out(out, json);
        Ok(())
    })
}

/// True when a session file exists in the data dir.
#[no_mangle]
pub unsafe extern "C" fn rc_session_exists() -> i32 {
    catch_unwind(AssertUnwindSafe(session::exists)).unwrap_or(false) as i32
}

/// Deletes the session file; absent is not an error.
#[no_mangle]
pub unsafe extern "C" fn rc_session_clear() -> RcStatus {
    status_of(|| session::clear())
}

/// Pure codec: JSON manifest -> canonical binary blob.  Test seam.
///
/// # Safety
/// `json` must point to `len` readable bytes; `out` receives a buffer
/// to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_session_encode(
    json: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad session pointer".into(),
        })?;
        buffer_out(out, session::encode(data)?);
        Ok(())
    })
}

/// Pure codec: binary blob -> canonical JSON manifest.  RC_CORRUPT on
/// any malformation; the decode is fully bounds-checked.
///
/// # Safety
/// `blob` must point to `len` readable bytes; `out` receives a buffer
/// to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_session_decode(
    blob: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(blob, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad session pointer".into(),
        })?;
        buffer_out(out, session::decode(data)?);
        Ok(())
    })
}

// ---- omnibox (OMNI01) -------------------------------------------------

/// The location-bar routing decision.  `input` is the typed text;
/// `options` is a JSON object `{"keywords":[...],"search_fallback":bool}`
/// — both from the caller's live config so the verdict needs no hidden
/// state.  Returns a JSON verdict (see rustcore.h); NULL on a bad
/// pointer (rc_last_error_message).  Free with rc_string_free().
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_classify_input(
    input: *const c_char,
    options: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let input = match unsafe { util::cstr(input) } {
            Some(s) => s,
            None => {
                error::set_error("bad input pointer");
                return ptr::null_mut();
            }
        };
        let mut keywords: Vec<String> = Vec::new();
        let mut search_fallback = true;
        if let Some(opt) = unsafe { util::cstr(options) } {
            if let Ok(v) = serde_json::from_str::<serde_json::Value>(opt) {
                if let Some(arr) = v.get("keywords").and_then(|k| k.as_array()) {
                    keywords = arr
                        .iter()
                        .filter_map(|k| k.as_str().map(String::from))
                        .collect();
                }
                if let Some(b) =
                    v.get("search_fallback").and_then(|b| b.as_bool())
                {
                    search_fallback = b;
                }
            }
        }
        util::to_c_string(
            omnibox::classify(input, &keywords, search_fallback).to_string(),
        )
    }))
    .unwrap_or(ptr::null_mut())
}

/// Pure frecency scorer.  `json` is
/// `{"visits":[<visit ms-epoch>,...],"now_ms":<ms>,"typed":<n>,
/// "bookmarked":<bool>}` — now_ms/typed/bookmarked optional; now_ms
/// defaults to the current time.  `out` receives the score.
///
/// # Safety
/// `json` must be NUL-terminated UTF-8; `out` writable.
#[no_mangle]
pub unsafe extern "C" fn rc_frecency_score(
    json: *const c_char,
    out: *mut i64,
) -> RcStatus {
    status_of(|| {
        let text = unsafe { util::cstr(json) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad frecency input pointer".into(),
        })?;
        let v: serde_json::Value =
            serde_json::from_str(text).map_err(|_| error::Fail {
                status: RcStatus::Corrupt,
                msg: "frecency input: not JSON".into(),
            })?;
        let ages: Vec<i64> = v
            .get("visits")
            .and_then(|a| a.as_array())
            .map(|a| a.iter().filter_map(|e| e.as_i64()).collect())
            .unwrap_or_default();
        let now = v.get("now_ms").and_then(|n| n.as_i64()).unwrap_or_else(|| {
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .map(|d| d.as_millis() as i64)
                .unwrap_or(0)
        });
        let typed = v.get("typed").and_then(|t| t.as_i64()).unwrap_or(0);
        let bookmarked =
            v.get("bookmarked").and_then(|b| b.as_bool()).unwrap_or(false);
        match unsafe { out.as_mut() } {
            Some(slot) => {
                *slot = omnibox::frecency(&ages, now, typed, bookmarked);
                Ok(())
            }
            None => error::fail(RcStatus::InvalidArgument, "bad out pointer"),
        }
    })
}

/// Ranked history suggestions for the completer — a JSON array
/// `[{"url","title","ts","frecency","score"}, ...]` ordered by score
/// (descending), limited to `limit` rows (<=0 picks the default 100).
/// NULL on error (store not open: rc_last_error_message).  Free with
/// rc_string_free().
///
/// # Safety
/// `term` must be NUL-terminated UTF-8, or null (treated as "").
#[no_mangle]
pub unsafe extern "C" fn rc_history_suggest(
    term: *const c_char,
    limit: i64,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let term = unsafe { util::cstr(term) }.unwrap_or("");
        let now = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        match history::with(|h| h.suggest(term, limit, now)) {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;
    use std::sync::{Mutex, MutexGuard};

    // Every test here drives the one process-global STORE — they must
    // run serialized or a parallel test's rc_set_data_dir switches
    // the dir mid-test.
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    fn guard() -> MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-test-{}-{}",
            std::process::id(),
            tag
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.to_string_lossy().into_owned()
    }

    fn init(dir: &str) {
        let c = CString::new(dir).unwrap();
        assert_eq!(unsafe { rc_set_data_dir(c.as_ptr()) }, RcStatus::Ok);
    }

    fn seal(plain: &[u8]) -> Vec<u8> {
        let mut out = RcBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            unsafe { rc_seal(plain.as_ptr(), plain.len(), &mut out) },
            RcStatus::Ok
        );
        let v = unsafe { std::slice::from_raw_parts(out.data, out.len) }.to_vec();
        unsafe { rc_buffer_free(out) };
        v
    }

    fn open(blob: &[u8]) -> Result<Vec<u8>, RcStatus> {
        let mut out = RcBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        let st = unsafe { rc_open(blob.as_ptr(), blob.len(), &mut out) };
        if st != RcStatus::Ok {
            return Err(st);
        }
        let v = unsafe { std::slice::from_raw_parts(out.data, out.len) }.to_vec();
        unsafe { rc_buffer_free(out) };
        Ok(v)
    }

    #[test]
    fn file_key_roundtrip() {
        let _g = guard();
        init(&tmpdir("filekey"));
        let blob = seal(b"hello");
        assert!(unsafe { rc_is_sealed(blob.as_ptr(), blob.len()) } == 1);
        assert_eq!(open(&blob).unwrap(), b"hello");
        assert!(blob.starts_with(b"ARSEC1"));
    }

    #[test]
    fn tamper_rejected() {
        let _g = guard();
        init(&tmpdir("tamper"));
        let mut blob = seal(b"payload");
        let n = blob.len();
        blob[n - 1] ^= 1;
        assert!(open(&blob).is_err());
        blob[10] ^= 1; // nonce byte — also must fail
        assert!(open(&blob).is_err());
        assert!(open(b"garbage").is_err());
        assert!(open(b"ARSEC1").is_err()); // magic but truncated
    }

    #[test]
    fn passphrase_flow_and_wrong_pass() {
        let _g = guard();
        let dir = tmpdir("pass");
        init(&dir);
        assert_eq!(unsafe { rc_passphrase_enabled() }, 0);
        let pass = b"correct horse battery";
        assert_eq!(
            unsafe { rc_kdf_create(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        assert_eq!(unsafe { rc_passphrase_enabled() }, 1);
        assert_eq!(unsafe { rc_is_unlocked() }, 1);

        let blob = seal(b"secret data");
        unsafe { rc_lock() };
        assert_eq!(unsafe { rc_is_unlocked() }, 0);
        assert!(open(&blob).is_err()); // locked — no oracle

        let wrong = b"nope";
        assert_eq!(
            unsafe { rc_unlock(wrong.as_ptr(), wrong.len()) },
            RcStatus::WrongPassphrase
        );
        assert_eq!(unsafe { rc_is_unlocked() }, 0);
        assert_eq!(
            unsafe { rc_unlock(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        assert_eq!(open(&blob).unwrap(), b"secret data");
    }

    #[test]
    fn kdf_file_layout_and_no_key_material() {
        let _g = guard();
        let dir = tmpdir("kdf");
        init(&dir);
        let pass = b"passphrase1";
        assert_eq!(
            unsafe { rc_kdf_create(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        let data = std::fs::read(format!("{dir}/securestore.kdf")).unwrap();
        assert!(data.starts_with(b"ARKDF1"));
        // magic6 | m | t | p | salt16 | verifier — same layout the C++
        // reader parses.
        assert_eq!(u32::from_le_bytes(data[6..10].try_into().unwrap()), 65536);
        assert_eq!(u32::from_le_bytes(data[10..14].try_into().unwrap()), 3);
        assert_eq!(u32::from_le_bytes(data[14..18].try_into().unwrap()), 4);
        let salt = &data[18..34];
        assert!(data[34..].starts_with(b"ARSEC1"));
        assert!(!std::path::Path::new(&format!("{dir}/securestore.key")).exists());

        // Re-derive from the file and prove the key is nowhere in it.
        let derived = store::derive_key(pass, 65536, 3, 4, salt).unwrap();
        assert!(data
            .windows(store::KEY_SIZE)
            .all(|w| w != derived.as_slice()));
        // The passphrase isn't in there either.
        assert!(data.windows(pass.len()).all(|w| w != pass));
    }

    #[test]
    fn argon2_matches_bundled_c_vector() {
        let _g = guard();
        // Pinned against src/argon2id.c (which is itself verified
        // against libargon2 in tst_securestore): 32x0x01 password,
        // 16x0x02 salt, t=3 m=32KiB p=4.
        let key = store::derive_key(&[1u8; 32], 32, 3, 4, &[2u8; 16]).unwrap();
        let mut hex = String::new();
        for b in key.iter() {
            hex.push_str(&format!("{b:02x}"));
        }
        assert_eq!(
            hex,
            "03aab965c12001c9d7d0d2de33192c0494b684bb148196d73c1df1acaf6d0c2e"
        );
    }

    #[test]
    fn file_key_fallback_after_enable() {
        // A blob sealed under the file key must still open after the
        // switch to passphrase mode while securestore.key exists —
        // the crash-window guarantee the C++ transition relies on.
        let _g = guard();
        let dir = tmpdir("fallback");
        init(&dir);
        let blob = seal(b"pre-migration");
        let pass = b"new custody";
        assert_eq!(
            unsafe { rc_kdf_create(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        // .key file still on disk (Qt deletes it after re-sealing).
        assert_eq!(open(&blob).unwrap(), b"pre-migration");
        // Once the .key file is retired the old blob is unreadable.
        assert_eq!(unsafe { rc_key_file_delete() }, RcStatus::Ok);
        assert!(open(&blob).is_err());
    }

    #[test]
    fn cred_store_crud() {
        let _g = guard();
        let dir = tmpdir("cred");
        init(&dir);
        let name = CString::new("proxy/user").unwrap();
        let mut out = RcBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            unsafe { rc_cred_get(name.as_ptr(), &mut out) },
            RcStatus::NotFound
        );
        assert_eq!(
            unsafe { rc_cred_put(name.as_ptr(), b"s3cret".as_ptr(), 6) },
            RcStatus::Ok
        );
        assert_eq!(
            unsafe { rc_cred_get(name.as_ptr(), &mut out) },
            RcStatus::Ok
        );
        assert_eq!(
            unsafe { std::slice::from_raw_parts(out.data, out.len) },
            b"s3cret"
        );
        unsafe { rc_buffer_free(out) };

        let list = unsafe { rc_cred_list() };
        assert!(!list.is_null());
        let json = unsafe { CStr::from_ptr(list) }.to_string_lossy().into_owned();
        unsafe { rc_string_free(list) };
        assert_eq!(json, "[\"proxy/user\"]");

        assert_eq!(unsafe { rc_cred_remove(name.as_ptr()) }, RcStatus::Ok);
        assert_eq!(
            unsafe { rc_cred_remove(name.as_ptr()) },
            RcStatus::NotFound
        );
        // Reload path: data persists on disk across the cache drop.
        unsafe { rc_lock() };
        assert_eq!(
            unsafe { rc_cred_put(name.as_ptr(), b"x".as_ptr(), 1) },
            RcStatus::Ok
        );
    }

    #[test]
    fn cred_file_survives_passphrase_switch() {
        let _g = guard();
        let dir = tmpdir("credpass");
        init(&dir);
        let name = CString::new("k").unwrap();
        assert_eq!(
            unsafe { rc_cred_put(name.as_ptr(), b"v".as_ptr(), 1) },
            RcStatus::Ok
        );

        // Staged transition the Qt side performs: capture file key,
        // create kdf, reseal the cred file, retire the key file.
        let mut old = [0u8; 32];
        assert_eq!(unsafe { rc_key_file_copy(old.as_mut_ptr()) }, RcStatus::Ok);
        let pass = b"pw";
        assert_eq!(
            unsafe { rc_kdf_create(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        let mut new = [0u8; 32];
        assert_eq!(
            unsafe { rc_derived_key_copy(new.as_mut_ptr()) },
            RcStatus::Ok
        );
        assert_eq!(
            unsafe { rc_cred_reseal(old.as_ptr(), new.as_ptr()) },
            RcStatus::Ok
        );
        assert_eq!(unsafe { rc_key_file_delete() }, RcStatus::Ok);

        unsafe { rc_lock() };
        assert_eq!(
            unsafe { rc_unlock(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        let mut out = RcBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        assert_eq!(
            unsafe { rc_cred_get(name.as_ptr(), &mut out) },
            RcStatus::Ok
        );
        assert_eq!(unsafe { *out.data }, b'v');
        unsafe { rc_buffer_free(out) };
    }
}
