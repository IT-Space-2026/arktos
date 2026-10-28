//! arktos-rustcore — Arktos's shared, engine-neutral Rust core behind a
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
//!   * tranco (TRUST04): the Tranco top-site rank table — vendored
//!     top-100k seed + merged data-dir top-1M update (poison-safe:
//!     an update only adds, never un-ranks), exact+registrable host
//!     probes for the site-trust signals.  No remote lookups.
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
//!   * autofill (RCORE05): the saved-form record store under the same
//!     AEAD + Argon2id custody as credentials — sealed
//!     autofill-store.dat, one unlock opens both.
//!   * tlsprobe (SEC22): second-opinion TLS verification — a real
//!     handshake to host:port evaluated by rustls+webpki against the
//!     platform roots, independent of the engine's verdict, reported
//!     as a JSON status/error-class/chain summary.

//!   * extverify (EXT06): extension-package verification — zip/CRX3
//!     structure checks, update-payload SHA-256 vs the declared hash,
//!     and the manifest.json schema+permission parse — all before Qt
//!     sees a byte of the package.
//!   * opensearch (OSE01): the canonical search-engine registry —
//!     engine records, keyword bindings, display order and the
//!     removed-bundled blocklist in <data dir>/searchengines.json,
//!     plus the byte-identical {searchTerms} URL expansion the old
//!     Qt buildUrl produced.
//!   * palette (CPAL01): the command palette's matching decision —
//!     the ported subsequence fuzzy scorer plus MRU recency ranking;
//!     the Qt shell keeps the item registry and marshals {match, id}
//!     rows in, ordered indices back.
//!   * qrcode (QRC01): QR symbol encoding for "Show QR for this
//!     page" — Nayuki's qrcodegen-rs, the same code lineage as the
//!     vendored C++ encoder it replaces, so the module matrix is
//!     bit-identical; the Qt side keeps painting it.
//!   * useragent (UAG01): effective-UA construction (de-badged
//!     vanilla UA + presented Chrome milestone), the client-hints
//!     brand version, the useragents.xml preset parse and the
//!     per-site UA spoof table (a "uaspoof" kind inside the
//!     sitedecisions store).
//!   * bookmarkhtml (BKIMP01): Netscape bookmark-file import/export
//!     at the store boundary — the converter.cpp forgiving-tag parse
//!     redone memory-safe, the url-scheme insert gate behind a
//!     caller-passed bookmarklet flag, merge/dedup for idempotent
//!     re-imports, and the atomic 0600 NETSCAPE-file writer.
//!   * clearplan (CLR01): the clear-private-data decision — a pure
//!     (period, categories, scope) -> ordered operation list
//!     function the Qt shell executes per-store; profile scoping is
//!     baked in so an OTR clear can never reach persistent stores.
//!   * lookalike (TRUST05): IDN/homograph detection — a UTS #39
//!     confusable skeleton of the host probed against a skeleton
//!     index over the Tranco table, flagging only when the visual
//!     twin is a DIFFERENT popular domain (warn on popular
//!     lookalikes, not on every punycode name).
//!   * sitetrust (TRUST01): the composite site-trust verdict —
//!     blocklist membership, Tranco rank, homograph lookalike, https
//!     state and history first-seen folded into one advisory
//!     green/amber/red JSON verdict for the lock-icon badge.  A
//!     warning signal only; it never blocks.
//!   * profilevault (PVAULT01): password-sealed profile archives
//!     ("ARV1") — a two-tier wrapped vault key (random vault_key under
//!     an Argon2id-derived KEK, so a password change is a re-wrap),
//!     chunked streaming encryption (AES-256-GCM default;
//!     Threefish-1024-XEX + Skein-MAC opt-in), honest distinct
//!     WrongPassword / Tampered errors, and a two-pass unseal that
//!     cannot leave partial output.
//!   * facade (RC07): the stable arktos_core.h ABI — AcCore owning
//!     per-profile-bound module handles over the RC02–06 stores
//!     (ac_settings/ac_profiles/ac_history/ac_bookmarks/ac_policy),
//!     ac_* thin forwards to the rc_* entry points, and the
//!     (module, profile_id) change-callback lane.  The rc_* surface
//!     stays live for legacy callers until RC08 migrates them.
//!   * torctl (RC14): the Tor control-protocol client — loopback-only
//!     connect, COOKIE + password AUTHENTICATE (secrets zeroized),
//!     serialized commands with exactly-once reply completion, async
//!     650-event capture and a pull-drain FFI announced through the
//!     "torctl" change topic.  Deliberately not a facade module: it
//!     binds no profile and owns no store.
//!   * cookies (RC15): the per-profile cookie database —
//!     <profile>/cookies.db, container-partitioned, Qt-jar-verified
//!     matching (host-only/domain reach, RFC6265 §5.1.4 paths, secure
//!     gating, SameSite send filter), Set-Cookie parse, toRawForm
//!     import/export and an authoritative/mirror role flag that
//!     honestly reports whether the file IS the jar (Servo) or a
//!     durable copy behind an engine-owned one (Chromium).
//!   * containers (RC19): the Firefox-style container registry —
//!     a typed specialization over the profile scope's settings
//!     store (the same "containers" group ContainerManager
//!     persists), plus the storage-namespace map engines partition
//!     cookies/cache/site data by (storage root, VSEC02 cache tree,
//!     Qt residue dir, storage name, cookie-partition key).
//!   * updates (RC20): the update-check's policy + parse half —
//!     GitHub-release manifest parse behind the trusted-download-URL
//!     gate, normalize/compare, the platform asset pick, channel
//!     rules (stable/beta vs draft/prerelease), and the
//!     last-check/snooze/skipped state as typed helpers over the
//!     GLOBAL settings scope.  The network fetch stays Qt-side.
//!   * navpolicy (MD02): the acceptNavigationRequest-time verdicts —
//!     scheme classification vs the browser-handled allowlist, the
//!     SEC02 external-handoff gate with per-profile scheme overrides,
//!     per-profile blocked hosts, the abp-subscribe rewrite, the
//!     staged-pdf viewer clamp, the file: directory reroute, the
//!     CONT04 container divert and the sanitize-on-view pdf claim —
//!     phase-scoped so the Qt page's kept gates stay in their exact
//!     order; the override rules live in the bound profile's
//!     sitedecisions.json ("nav-scheme"/"nav-block" kinds).
//!   * permpolicy (MD03): the SEC05 permission-broker verdicts — the
//!     hard-deny/ask-capable feature tables, the security-origin key
//!     normalization, and the remembered+session decision pipeline
//!     behind rc_perm_call.  Durable rows are sitedecisions.json's
//!     "webperm" kind; session answers are process-global and
//!     OTR-namespaced.  Dialogs stay Qt-side.
//!   * dlwpolicy (MD04): the download-interception verdicts — the
//!     suggested-name sanitizer, the dangerous-extension/executable-
//!     mime threat tables, the save-as gate, the external-handler
//!     handoff, the rustdl engine claim, the finished-row removal
//!     verdict and the download history records behind rc_dlw_call.
//!     Legacy downloadmanager/* keys read from the bound profile's
//!     settings.json; facade-authored rules and records live in
//!     downloads.json beside it.  Dialogs, QProcess and the
//!     filesystem stay Qt-side.
//!   * privacy (MD05): the per-profile privacy-policy store view —
//!     the typed privacy/* flag reads (incl. the referer-bool
//!     migration and the urlCleaning/stripTrackingParams alias), the
//!     per-persona Sec-GPC decision the interceptors consult at
//!     request time, and the unified {gpc,strip,adblock} verdict the
//!     tracker-strip and adblock policy reads share.  Read-only over
//!     the bound scope's settings.json — Qt keeps the writes and all
//!     execution.
//!   * urlclean (MD06): the per-profile URL-sanitize policy — the
//!     editable ruleset override (profiles/<id>/urlclean.json, the
//!     urlstrip grammar), the copy-path toggle, the nav-path toggle
//!     read shared with the privacy module, the legacy
//!     urlcleaner/extraParams merge, and the clean_policy composite
//!     the snapshot loader ships to policy::strip_target.  The
//!     clipboard write and the redirect application stay Qt-side.
//!   * sesspolicy (MD08): the session snapshot/restore policy —
//!     the AutoSaver debounce + ceiling, the dirty-on-what class
//!     gate, the retention caps (max age / windows / tabs), the
//!     crash-restore answer and the startup-mode fallback, typed
//!     reads over the bound profile's "session/" settings rows.
//!     Read-only; the timers, the restore prompt and the manifest
//!     truncation stay Qt-side.
//!   * notes (RC21): the per-profile side-panel note list —
//!     profiles/<id>/notes.json in the C++ writer's compact
//!     sorted-key JSON shape, row-addressed CRUD preserving the
//!     model's insert/remove/display order, a volatile
//!     off-the-record binding, and a dirty+sync contract that
//!     carries the Qt AutoSaver debounce across the FFI boundary.
//!
mod autofill;
mod bidi;
mod blocklist;
mod bookmarkhtml;
mod bookmarks;
mod caps;
mod clearplan;
mod confusables_table;
mod containers;
mod cookies;
mod cred;
mod dlwpolicy;
mod error;
mod extverify;
mod facade;
mod history;
mod icons;
mod inifmt;
mod lookalike;
mod mw;
mod navpolicy;
mod noisexx;
mod notes;
mod notify;
mod omnibox;
mod opensearch;
mod palette;
mod parsers;
mod pdfdoc;
mod pdfsanitize;
mod permpolicy;
mod policy;
mod privacy;
mod profiles;
mod profilevault;
mod qrcode;
mod readability;
mod session;
mod sesspolicy;
mod settings;
mod sitedecisions;
mod sitetrust;
mod store;
mod sync;
mod syncbookmarks;
mod synchistory;
pub mod syncpair;
pub mod synctransport;
mod tlsprobe;
mod torctl;
mod tranco;
mod updates;
mod urlclean;
mod urlstrip;
mod useragent;
mod util;

// COV05: C-ABI surface coverage — drives every rc_* entry point.
#[cfg(test)]
mod ffi_surface;

use std::ffi::CString;
use std::os::raw::{c_char, c_int, c_void};
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_passphrase_enabled() -> i32 {
    catch_unwind(AssertUnwindSafe(|| store::lock().passphrase_enabled() as i32))
        .unwrap_or(0)
}

/// False only in passphrase mode while no derived key is cached.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_key_file_create() -> RcStatus {
    status_of(|| store::lock().key_file_create())
}

/// Deletes securestore.key and drops the cached copy.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_key_file_delete() -> RcStatus {
    status_of(|| store::lock().key_file_delete())
}

/// Deletes securestore.kdf and drops the cached derived key.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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

// ---- autofill store (RCORE05) -------------------------------------------
//
// The saved-form records live in <data dir>/autofill-store.dat — a
// sealed "ARSEC1" blob under the same custody key as credentials.dat,
// so one unlock opens both and every passphrase/lock transition
// covers it.  The legacy autofill.dat is a read-only import source:
// the Qt adapter parses it with its own bounded reader and hands the
// records over via rc_autofill_set_forms, then retires the file.

/// Whether the Rust autofill store exists on disk — the Qt side's
/// "does the legacy file still need importing" check.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_autofill_store_present() -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        store::lock().autofill_store_present() as i32
    }))
    .unwrap_or(0)
}

/// JSON array of every stored form:
/// [{"url","name","has_password":bool,"elements":[["k","v"],...]}]
/// Free with rc_string_free(); NULL on error (locked store, corrupt
/// file — rc_last_error_message says which).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_autofill_forms() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match store::lock().autofill_json() {
            Ok(j) => util::to_c_string(j),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Replaces the whole record set from the JSON array and persists it
/// atomically; emits the "autofill" change topic when the set
/// actually changed.  RC_INVALID_ARGUMENT on malformed JSON — the
/// store keeps its previous contents then.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_autofill_set_forms(
    json: *const u8,
    len: usize,
) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        let records = autofill::forms_from_json(data)?;
        changed = store::lock().autofill_set(records)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("autofill");
    }
    st
}

/// JSON array of every stored address (ADDR01/02):
/// [{"country","name","organization","street","city","state","zip",
///   "phone","email","last_used":int}]
/// Free with rc_string_free(); NULL on error (locked store, corrupt
/// file — rc_last_error_message says which).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_autofill_addresses() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match store::lock().autofill_addresses_json() {
            Ok(j) => util::to_c_string(j),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Replaces the whole address set from the JSON array and persists it
/// atomically; emits the "autofill" change topic when the set
/// actually changed.  RC_INVALID_ARGUMENT on malformed JSON — the
/// store keeps its previous contents then.
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_autofill_set_addresses(
    json: *const u8,
    len: usize,
) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        let records = autofill::addresses_from_json(data)?;
        changed = store::lock().autofill_set_addresses(records)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("autofill");
    }
    st
}

/// Re-seals autofill-store.dat between explicit keys during a custody
/// transition; no-op when the file does not exist.
///
/// # Safety
/// Both pointers must read 32 bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_autofill_reseal(
    from: *const u8,
    to: *const u8,
) -> RcStatus {
    status_of(|| {
        let from = unsafe { util::key32(from) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad from-key pointer".into(),
        })?;
        let to = unsafe { util::key32(to) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad to-key pointer".into(),
        })?;
        store::lock().autofill_reseal(from, to)
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_urlstrip_reload() -> RcStatus {
    status_of(urlstrip::reload)
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_blocklist_reload() -> RcStatus {
    status_of(blocklist::reload)
}

/// Entry count of the active list — diagnostics/tests.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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

/// PERF01: the verdict-state generation counter.  Bumped by every
/// mutation that could change an evaluate() verdict — snapshot loads,
/// downgrade-mark/allowance writes, urlstrip ruleset and domain
/// blocklist swaps, and the data-dir switch that rebuilds them.  The
/// Qt-side verdict cache stamps each entry with the generation it was
/// computed under and re-reads this once per request — a trivial FFI
/// call that replaces the whole marshal→evaluate→parse round trip on
/// a hit.
#[no_mangle]
pub extern "C" fn rc_policy_eval_generation() -> u64 {
    policy::eval_generation()
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

// ---- navigation-request policy (MD02) ----------------------------------
// The acceptNavigationRequest-time verdicts: scheme classification, the
// external-handoff gate, per-profile scheme/host overrides, the abp
// rewrite, the staged-pdf clamp, the file-dir reroute, the container
// divert and the pdf claim.  The Qt page marshals a manifest per consult
// phase and applies the verdict; all execution stays shell-side.

/// Evaluates one navigation manifest (JSON) and returns the verdict as
/// a JSON string — see navpolicy.rs for the phase/verdict grammar.
/// NULL on malformed input or panic — the adapter must fail open
/// (treat NULL as "continue", never as a block).
/// Free the result with rc_string_free().
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_nav_decide(
    json: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => {
                let verdict = navpolicy::decide(&manifest);
                util::to_c_string(verdict.to_string())
            }
            None => {
                error::set_error("bad nav manifest");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Generic granular navpolicy endpoint — JSON in {"op":..., params},
/// JSON out.  Ops: decide, browser_scheme, scheme_class,
/// rate_limit_interstitial, untrusted_input, page_link, staged_viewer,
/// nav_rule, plus the MD09 popup-gate ops (popup_decide,
/// popup_is_allowed, popup_allow, popup_remove, popup_list,
/// popup_clear, popup_clear_session).  NULL on error — see
/// rc_last_error_message().  Free the result with rc_string_free().
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_nav_call(
    json: *const u8,
    len: usize,
) -> *mut c_char {
    let (result, mutated) = catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match navpolicy::call_json(&manifest) {
                Ok(result) => {
                    let mutated = result
                        .get("mutated")
                        .and_then(|x| x.as_bool())
                        .unwrap_or(false);
                    (util::to_c_string(result.to_string()), mutated)
                }
                Err(e) => {
                    error::set_error(&e.msg);
                    (ptr::null_mut(), false)
                }
            },
            None => {
                error::set_error("bad nav call payload");
                (ptr::null_mut(), false)
            }
        }
    }))
    .unwrap_or((ptr::null_mut(), false));
    if mutated {
        notify::emit("sitedecisions");
    }
    result
}

// ---- web-permission policy (MD03) --------------------------------------
// The SEC05 broker's decision layer: the feature tables, the origin
// key normalization, the remembered+session pipeline.  Durable rows
// are sitedecisions.json's "webperm" kind (the anchored dir's file,
// like every store consumer); the modal queue and the engine's
// grant()/deny() stay Qt-side.

/// Generic granular permpolicy endpoint — JSON in {"op":..., params},
/// JSON out.  Ops: decide, stored, resolve, remove, clear,
/// feature_class, key_for, session_clear — see permpolicy.rs.
/// NULL on error — see rc_last_error_message(); a caller that cannot
/// get a verdict must fail toward its own local consult (never a
/// silent grant).  Free the result with rc_string_free().
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_perm_call(
    json: *const u8,
    len: usize,
) -> *mut c_char {
    let (result, mutated) = catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match permpolicy::call_json(&manifest) {
                Ok(result) => {
                    let mutated = result
                        .get("mutated")
                        .and_then(|x| x.as_bool())
                        .unwrap_or(false);
                    (util::to_c_string(result.to_string()), mutated)
                }
                Err(e) => {
                    error::set_error(&e.msg);
                    (ptr::null_mut(), false)
                }
            },
            None => {
                error::set_error("bad perm call payload");
                (ptr::null_mut(), false)
            }
        }
    }))
    .unwrap_or((ptr::null_mut(), false));
    if mutated {
        notify::emit("sitedecisions");
    }
    result
}

// ---- download-interception policy (MD04) ------------------------------
// The DownloadManager's decision layer: the suggested-name sanitizer,
// the dangerous-extension/executable-mime threat tables, the
// save-as prompt gate, the external-handler handoff, the rustdl
// engine claim, the finished-row removal verdict, the per-profile
// rule view and the download history records.  The legacy
// downloadmanager/* keys keep living in the bound profile's
// settings.json (the C++ writers still own them); the facade-
// authored rules and records live in downloads.json beside it.
// The Qt manager keeps the dialogs, the QProcess handoff and the
// filesystem execution — this surface only answers questions.

/// Evaluates one download-policy manifest (JSON) and returns the
/// verdict/result as a JSON string — see dlwpolicy.rs for the op
/// grammar.  `h` is the bound profile's RcSettings (rc_settings_open
/// or ac_dlw_call's per-op open); NULL is legal for the pure ops
/// (sanitize_name, threat, external_argv) — store-bound ops then
/// fail with a stashed error, never a guessed rule.  Mutating ops
/// emit the "dlpolicy" change topic.  Free the result with
/// rc_string_free().
///
/// # Safety
/// `h` must come from rc_settings_open or be NULL; `json` must point
/// to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_dlw_call(
    h: *const settings::RcSettings,
    json: *const u8,
    len: usize,
) -> *mut c_char {
    let (result, mutated) = catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { h.as_ref() };
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match dlwpolicy::call_json(s, &manifest) {
                Ok(result) => {
                    let mutated = result
                        .get("mutated")
                        .and_then(|x| x.as_bool())
                        .unwrap_or(false);
                    (util::to_c_string(result.to_string()), mutated)
                }
                Err(e) => {
                    error::set_error(&e.msg);
                    (ptr::null_mut(), false)
                }
            },
            None => {
                error::set_error("bad dlw call payload");
                (ptr::null_mut(), false)
            }
        }
    }))
    .unwrap_or((ptr::null_mut(), false));
    if mutated {
        notify::emit("dlpolicy");
    }
    result
}

// ---- privacy policy store view (MD05) ---------------------------------
// The per-persona privacy/* flag reads, the Sec-GPC decision the
// request interceptors consult at request time, and the unified
// {gpc,strip,adblock} verdict.  Read-only — the Qt writers own the
// keys; this surface only answers questions.

/// Evaluates one privacy manifest (JSON) against the bound profile's
/// settings store — see privacy.rs for the op grammar (flags, flag,
/// gpc, verdict; "defaults" is the only NULL-store-legal op).  NULL
/// on malformed input, a missing store or panic — the adapter must
/// keep its local consult, never guess a toggle.  Free the result
/// with rc_string_free().
///
/// # Safety
/// `h` must come from rc_settings_open or be NULL; `json` must point
/// to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_privacy_call(
    h: *const settings::RcSettings,
    json: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { h.as_ref() };
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match privacy::call_json(s, &manifest) {
                Ok(result) => util::to_c_string(result.to_string()),
                Err(e) => {
                    error::set_error(&e.msg);
                    ptr::null_mut()
                }
            },
            None => {
                error::set_error("bad privacy call payload");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The per-profile Sec-GPC decision — 1 when the bound persona sends
/// "Sec-GPC: 1" (its gpcSignal row, or unconditionally when
/// tor_mode != 0), 0 when not, -1 on error.  The engine adapters
/// call this at request time; a -1 keeps their verdict-bit
/// fallback, never a phantom off/on flip.
///
/// # Safety
/// `h` must come from rc_settings_open/ac_privacy or be NULL (which
/// answers -1).
#[no_mangle]
pub unsafe extern "C" fn rc_privacy_gpc(
    h: *const settings::RcSettings,
    tor_mode: c_int,
) -> c_int {
    catch_unwind(AssertUnwindSafe(|| match unsafe { h.as_ref() } {
        Some(s) => {
            if privacy::gpc(s, tor_mode != 0) {
                1
            } else {
                0
            }
        }
        None => {
            error::set_error("rc_privacy_gpc needs a bound profile store");
            -1
        }
    }))
    .unwrap_or(-1)
}

// ---- URL-sanitize policy (MD06) ---------------------------------------
// The per-profile URL-cleaning decision layer: the editable ruleset
// override (profiles/<id>/urlclean.json, urlstrip grammar), the
// copy-path toggle, the nav-path toggle read shared with the privacy
// module (privacy/urlCleaning stays Qt-owned), the legacy
// urlcleaner/extraParams merge, and the clean_policy composite the
// snapshot loader ships to policy::strip_target.  The clipboard
// write and the redirect application stay caller-side — this surface
// only answers questions.

/// Evaluates one urlclean manifest (JSON) against the bound
/// profile's stores — see urlclean.rs for the op grammar (clean,
/// enabled, set_enabled, params, ruleset_get/set/reset,
/// clean_policy; "defaults" is the only NULL-store-legal op).  NULL
/// on malformed input, a missing store or panic — the adapter must
/// keep its local consult, never guess a verdict.  Mutating ops emit
/// the "urlclean" change topic.  Free the result with
/// rc_string_free().
///
/// # Safety
/// `h` must come from rc_settings_open or be NULL; `json` must point
/// to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_urlclean_call(
    h: *const settings::RcSettings,
    json: *const u8,
    len: usize,
) -> *mut c_char {
    let (result, mutated) = catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { h.as_ref() };
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match urlclean::call_json(s, &manifest) {
                Ok(result) => {
                    let mutated = result
                        .get("mutated")
                        .and_then(|x| x.as_bool())
                        .unwrap_or(false);
                    (util::to_c_string(result.to_string()), mutated)
                }
                Err(e) => {
                    error::set_error(&e.msg);
                    (ptr::null_mut(), false)
                }
            },
            None => {
                error::set_error("bad urlclean call payload");
                (ptr::null_mut(), false)
            }
        }
    }))
    .unwrap_or((ptr::null_mut(), false));
    if mutated {
        // The nav pipeline reads the ruleset off the snapshot, so a
        // ruleset write only lands at the next snapshot push — the
        // generation bump still matters: memoized verdicts computed
        // under the old set must never outlive it.
        policy::bump_eval_generation();
        notify::emit("urlclean");
    }
    result
}

// ---- Tranco rank table (TRUST04) -------------------------------------

/// The host's Tranco rank, or 0 when unlisted (Tranco ranks start at
/// 1).  The probe hits the exact host first, then walks up to the
/// most specific listed ancestor — the registrable-domain lookup the
/// site-trust signals need.  Never fails on content — a bad pointer
/// or malformed host reads as "unlisted", and a no-data-dir core
/// answers from the vendored top-100k seed.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_tranco_rank(host: *const c_char) -> u32 {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(host) } {
            Some(h) => tranco::rank(h),
            None => 0,
        }
    }))
    .unwrap_or(0)
}

/// Swaps in a caller-supplied `rank,domain` table body (update/test
/// seam).  RC_CORRUPT when the body yields no usable entries; the
/// previous table stays active.
///
/// # Safety
/// `text` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tranco_load(
    text: *const u8,
    len: usize,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(text, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad tranco pointer".into(),
        })?;
        let body = std::str::from_utf8(data).map_err(|_| error::Fail {
            status: RcStatus::Corrupt,
            msg: "tranco: not UTF-8".into(),
        })?;
        tranco::load(body)
    })
}

/// Re-runs the vendored-seed ∪ `<data dir>/tranco-list.dat` merge —
/// the updater writes the file (top-1m.csv.zip body or plaintext
/// CSV) before calling this.  RC_CORRUPT when an override exists but
/// yields no usable entries; the previous table stays active.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_tranco_reload() -> RcStatus {
    status_of(tranco::reload)
}

/// Entry count of the active table — diagnostics/tests.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_tranco_count() -> usize {
    catch_unwind(AssertUnwindSafe(tranco::count)).unwrap_or(0)
}

/// Usable-entry count of one data-dir override file
/// ("tranco-list.dat", "majestic-million.csv"), parsed under the
/// grammar its name selects — the updater's per-file validation
/// probe before a reload: a body that yields zero entries is junk
/// and must not replace a good copy.  0 when the data dir is unset,
/// the file is absent/unreadable/oversized or yields nothing.
///
/// # Safety
/// `name` must be NUL-terminated UTF-8, or null (reads as 0).
#[no_mangle]
pub unsafe extern "C" fn rc_tranco_file_entries(name: *const c_char) -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(name) } {
            Some(n) => tranco::file_entries(n),
            None => 0,
        }
    }))
    .unwrap_or(0)
}

// ---- homograph lookalike detector (TRUST05) -------------------------

/// IDN/homograph verdict as JSON:
///   {"verdict":"lookalike","lookalike_of":"<ranked domain>",
///    "mixed_script":<bool>,"skeleton":"<skeleton>"}
/// or
///   {"verdict":"clean","mixed_script":<bool>,"skeleton":"..."}
/// The host is punycode-decoded, case-folded and mapped through the
/// UTS #39 confusable table, then probed against the Tranco table's
/// skeleton index — it flags only when the skeleton coincides with a
/// DIFFERENT popular domain, so benign IDNs resembling nothing
/// popular stay clean.  Returns null on a bad pointer; the verdict is
/// a warning signal (TRUST01 composites it), never a block.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_lookalike_check(host: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(host) } {
            Some(h) => util::to_c_string(lookalike::verdict_json(h)),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- site-trust composite (TRUST01) --------------------------------

/// Composite site-trust verdict as JSON (free with rc_string_free),
/// or NULL on a bad pointer:
///   {"level":"green"|"amber"|"red","signals":[...],"rank":N,
///    "lookalike_of":"..."}
/// Inputs: blocklist membership, Tranco rank, the homograph
/// lookalike probe, the caller-passed https state and the history
/// store's first-seen rollup.  signals[] reports each input that
/// evaluated true — "threat", "lookalike", "rank", "first_seen" —
/// and "lookalike_of" appears only when the lookalike probe fired.
/// The verdict is advisory (a badge state), never a block; an
/// un-evaluable host reads as amber with no signals.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_sitetrust_lookup(
    host: *const c_char,
    is_https: i32,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(host) } {
            Some(h) => util::to_c_string(sitetrust::lookup(h, is_https != 0)),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// rc_sitetrust_lookup variant carrying the probe-selection mask —
/// `probes` is a bitmask of the RC_SITETRUST_PROBE_* bits
/// (sitetrust::PROBE_*); a cleared bit skips that advisory probe
/// entirely so a Preferences switch (TRUST03) can suppress the
/// lookalike or threat-list input without touching the verdict
/// shape.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8, or null.
#[no_mangle]
pub unsafe extern "C" fn rc_sitetrust_lookup_opts(
    host: *const c_char,
    is_https: i32,
    probes: u32,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(host) } {
            Some(h) => util::to_c_string(sitetrust::lookup_opts(
                h,
                is_https != 0,
                probes,
            )),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Re-runs both input-table merges the composite reads (blocklist +
/// Tranco) — the list updaters call this after dropping a new file.
/// Both reloads run even when the first fails; the first error is
/// reported.
///
/// # Safety
/// Takes no pointers; safe to call from C.
#[no_mangle]
pub unsafe extern "C" fn rc_sitetrust_reload() -> RcStatus {
    status_of(sitetrust::reload)
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

// ---- pdf document rasterization (PDF02) -----------------------------

/// Opaque handle for an open document — a heap `pdfdoc::PdfDocument`
/// pointer.  0 is invalid.  One document lives per `--pdf-worker`
/// process; the API is single-threaded by contract.
///
/// # Safety
/// `data` must point to `len` readable bytes.  The caller owns the
/// returned handle and must release it with rc_pdfdoc_close().
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_open(data: *const u8, len: usize) -> usize {
    catch_unwind(AssertUnwindSafe(|| {
        let input = match unsafe { util::bytes(data, len) } {
            Some(b) => b.to_vec(),
            None => return 0,
        };
        match pdfdoc::PdfDocument::open(input) {
            Ok(doc) => Box::into_raw(Box::new(doc)) as usize,
            Err(msg) => {
                error::set_error(&msg);
                0
            }
        }
    }))
    .unwrap_or(0)
}

fn pdfdoc_ref<'a>(handle: usize) -> Option<&'a mut pdfdoc::PdfDocument> {
    if handle == 0 {
        return None;
    }
    unsafe { (handle as *mut pdfdoc::PdfDocument).as_mut() }
}

/// Page count, or 0 on a bad handle.
///
/// # Safety
/// `doc` must be a live handle from rc_pdfdoc_open().
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_page_count(doc: usize) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        pdfdoc_ref(doc).map(|d| d.page_count() as i32).unwrap_or(0)
    }))
    .unwrap_or(0)
}

/// Page dimensions in PDF points (1/72 inch).
///
/// # Safety
/// `doc` must be a live handle; `w`/`h` are writable out pointers.
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_page_size(
    doc: usize,
    page: i32,
    w: *mut f64,
    h: *mut f64,
) -> RcStatus {
    status_of(|| {
        let d = pdfdoc_ref(doc).ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad pdfdoc handle".into(),
        })?;
        let (pw, ph) = d.page_size(page.max(0) as usize).ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "page out of range".into(),
        })?;
        if let Some(p) = unsafe { w.as_mut() } {
            *p = pw;
        }
        if let Some(p) = unsafe { h.as_mut() } {
            *p = ph;
        }
        Ok(())
    })
}

/// Document /Title, or NULL.  rc_string_free the result.
///
/// # Safety
/// `doc` must be a live handle.
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_title(doc: usize) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        pdfdoc_ref(doc)
            .and_then(|d| d.title())
            .map(util::to_c_string)
            .unwrap_or(ptr::null_mut())
    }))
    .unwrap_or(ptr::null_mut())
}

/// Rasterize `page` at `scale` (1.0 = 72 dpi) into PNG bytes
/// (rc_buffer_free).  RC_CORRUPT on render failure, RC_INVALID_ARGUMENT
/// on a bad handle/page or a bitmap over the pixel cap.
///
/// # Safety
/// `doc` must be a live handle; `out` receives a buffer to release
/// with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_render_png(
    doc: usize,
    page: i32,
    scale: f64,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let d = pdfdoc_ref(doc).ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad pdfdoc handle".into(),
        })?;
        if !(scale > 0.0 && scale.is_finite()) {
            return error::fail(RcStatus::InvalidArgument, "bad scale");
        }
        let png = d
            .render_png(page.max(0) as usize, scale)
            .map_err(|msg| error::Fail {
                status: RcStatus::Corrupt,
                msg,
            })?;
        buffer_out(out, png);
        Ok(())
    })
}

/// Page index holding the next/prev `needle` match from `from_page`
/// (dir >= 0 scans forward, < 0 backward, wraps once), or -1.  Text is
/// page-granular — the view jumps to the page, it cannot paint hit
/// rects.  `out_total` optionally receives the document-wide
/// occurrence count.
///
/// # Safety
/// `doc` must be a live handle; `needle` is a NUL-terminated UTF-8
/// string; `out_total` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_find(
    doc: usize,
    needle: *const c_char,
    from_page: i32,
    dir: i32,
    out_total: *mut i32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(d) = pdfdoc_ref(doc) else {
            return -1;
        };
        let Some(text) = (unsafe { util::cstr(needle) }) else {
            return -1;
        };
        if let Some(p) = unsafe { out_total.as_mut() } {
            *p = d.count_occurrences(text) as i32;
        }
        d.find(text, i64::from(from_page), i64::from(dir))
            .map(|p| p as i32)
            .unwrap_or(-1)
    }))
    .unwrap_or(-1)
}

/// Releases a document handle.  NULL-tolerant (0 is a no-op).
///
/// # Safety
/// `doc` must be a live handle or 0; after this call it is invalid.
#[no_mangle]
pub unsafe extern "C" fn rc_pdfdoc_close(doc: usize) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if doc != 0 {
            drop(unsafe { Box::from_raw(doc as *mut pdfdoc::PdfDocument) });
        }
    }));
}

// ---- reader-mode extraction (RDR01) ---------------------------------

/// The isProbablyReaderable verdict on the page's serialized DOM —
/// the affordance check ReaderMode::probe() runs on every load.
/// 1 when the document looks article-like, 0 otherwise (bad input
/// reads as "not an article", never an error).
///
/// # Safety
/// `html` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_readability_probe(html: *const u8, len: usize) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::bytes(html, len) } {
            Some(b) => readability::probably_readable(b).unwrap_or(false) as i32,
            None => 0,
        }
    }))
    .unwrap_or(0)
}

/// Probe + extract in one pass — the ReaderMode::enter() path.  The
/// JSON verdict (rc_buffer_free) always carries
/// {"ok","probably","title","byline","siteName","excerpt","dir",
/// "lang","length","content"} — "content" is the article fragment sanitized
/// against the reader-view ruleset and is populated only when "ok".
/// RC_INVALID_ARGUMENT on bad pointers or oversized input.
///
/// # Safety
/// `html` must point to `len` readable bytes; `base_url` is a
/// NUL-terminated absolute page URL or NULL; `out` receives a buffer
/// to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_readability_extract(
    html: *const u8,
    len: usize,
    base_url: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(html, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad document pointer".into(),
        })?;
        let url = unsafe { util::cstr(base_url) };
        let json = readability::extract_json(data, url)?;
        buffer_out(out, json.into_bytes());
        Ok(())
    })
}

// ---- extension-package verification (EXT06) ------------------------
//
// The untrusted-bytes boundary of the extension system: packages and
// update payloads are verified in Rust before Qt's installer sees
// them.  Both entry points return a JSON verdict — see extverify.rs
// for the schema.

/// Verifies an extension package (zip or CRX3) in memory and returns
/// the verdict JSON in `out` (rc_buffer_free).  RC_OK means a verdict
/// was produced — the JSON "status" field says "valid" or "rejected"
/// ("error" carries the reason).  `expected_sha256` is the update
/// manifest's declared 32-byte digest, or null when undeclared;
/// `pinned_pubkey` is reserved for signature pinning (v1 reports
/// "no-pinning-configured" when supplied).
///
/// # Safety
/// `pkg` must point to `len` readable bytes; `expected_sha256` is null
/// or points to 32 bytes; `pinned_pubkey` is null or points to
/// `pubkey_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_ext_verify_package(
    pkg: *const u8,
    len: usize,
    expected_sha256: *const u8,
    pinned_pubkey: *const u8,
    pubkey_len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(pkg, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad package pointer".into(),
        })?;
        let expected = if expected_sha256.is_null() {
            None
        } else {
            let e = unsafe { util::bytes(expected_sha256, 32) }.ok_or_else(|| {
                error::Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "bad hash pointer".into(),
                }
            })?;
            Some(e)
        };
        let pubkey = if pinned_pubkey.is_null() {
            None
        } else {
            Some(unsafe { util::bytes(pinned_pubkey, pubkey_len) }.ok_or_else(
                || error::Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "bad pubkey pointer".into(),
                },
            )?)
        };
        let json = extverify::verify_package(data, expected, pubkey)?;
        buffer_out(out, json);
        Ok(())
    })
}

/// Parses + validates a manifest.json document (<= 1 MiB) into the
/// permission-classification JSON consumed by the review dialog —
/// see extverify.rs.  RC_CORRUPT on unreadable JSON.
///
/// # Safety
/// `json` must point to `len` readable bytes; `out` receives a buffer
/// to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_ext_manifest_check(
    json: *const u8,
    len: usize,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad manifest pointer".into(),
        })?;
        let checked = extverify::manifest_check(data)?;
        buffer_out(out, checked);
        Ok(())
    })
}

/// Unpacks a verified extension package (.zip, or the zip payload
/// behind a CRX3 header) into `out_dir_utf8` — the BW02 staging half
/// of hosted-only background routing: Arktos hands the engine a
/// doctored directory instead of the raw package, so the bundled /
/// installed copy never registers a native MV3 service worker the
/// hosted runtime owns.  RC_OK on success; error text rides
/// rc_last_error_message().  RC_CORRUPT when the archive fails the
/// same structural checks rc_ext_verify_package enforces.
///
/// # Safety
/// `pkg` must point to `len` readable bytes; `out_dir_utf8` must be a
/// NUL-terminated UTF-8 path the process may write into.
#[no_mangle]
pub unsafe extern "C" fn rc_ext_unpack_package(
    pkg: *const u8,
    len: usize,
    out_dir_utf8: *const std::os::raw::c_char,
) -> RcStatus {
    status_of(|| {
        let data = unsafe { util::bytes(pkg, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad package pointer".into(),
        })?;
        let dir = unsafe { util::cstr(out_dir_utf8) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad out-dir pointer".into(),
        })?;
        extverify::unpack_package(data, std::path::Path::new(dir))?;
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_child_count(node: u64) -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        bookmarks::with(|b| b.child_count(node).map(|c| c as i64).unwrap_or(-1))
    }))
    .unwrap_or(-1)
}

/// Handle of `node`'s child at `row`, or 0.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_child_at(node: u64, row: i64) -> u64 {
    catch_unwind(AssertUnwindSafe(|| {
        bookmarks::with(|b| b.child_at(node, row).unwrap_or(0))
    }))
    .unwrap_or(0)
}

/// Parent handle of `node`, or 0 (root / detached / bad handle).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_detach(node: u64) -> RcStatus {
    let st = status_of(|| bookmarks::with(|b| b.detach(node)));
    if st == RcStatus::Ok {
        notify::emit("bookmarks");
    }
    st
}

/// Frees a detached subtree for good.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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

///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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

/// BKIMP01: parses a Netscape bookmark HTML document and inserts the
/// tree under `parent` (a root or folder handle), applying the
/// store's url-scheme policy at the boundary — http/https/ftp/mailto
/// pass, javascript: passes only with RC_BM_IMPORT_BOOKMARKLETS, every
/// other scheme is refused and counted.  RC_BM_IMPORT_DRY_RUN runs
/// the full scan + accounting without inserting (the Qt adapter's
/// consent probe).
///
/// Returns a JSON summary
/// {"imported","folders","separators","duplicates","skipped",
///  "skipped_by_scheme":{scheme:count},"truncated"} — free with
/// rc_string_free(); NULL on error (rc_last_error_message).
///
/// # Safety
/// `html` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_import_html(
    parent: u64,
    html: *const u8,
    len: usize,
    flags: i32,
) -> *mut c_char {
    match catch_unwind(AssertUnwindSafe(|| {
        let data = match unsafe { util::bytes(html, len) } {
            Some(d) => d,
            None => {
                error::set_error("bad html pointer");
                return (ptr::null_mut(), false);
            }
        };
        match bookmarks::with(|b| {
            bookmarkhtml::import_html(
                b,
                parent,
                data,
                flags & bookmarkhtml::IMPORT_BOOKMARKLETS != 0,
                flags & bookmarkhtml::IMPORT_DRY_RUN != 0,
            )
        }) {
            Ok((json, created)) => (util::to_c_string(json), created),
            Err(e) => {
                error::set_error(&e.msg);
                (ptr::null_mut(), false)
            }
        }
    })) {
        Ok((p, created)) => {
            if created {
                notify::emit("bookmarks");
            }
            p
        }
        Err(_) => {
            error::set_error("panic inside rustcore");
            ptr::null_mut()
        }
    }
}

/// Serializes the tree as a NETSCAPE-Bookmark-file-1 document and
/// installs it at `path` atomically (temp + fsync + rename, mode
/// 0600 on unix — exports can carry token-bearing urls).
///
/// # Safety
/// `path` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_bm_export_html(path: *const c_char) -> RcStatus {
    status_of(|| {
        let p = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad path pointer".into(),
        })?;
        bookmarks::with(|b| bookmarkhtml::save_html_path(b, std::path::Path::new(p)))
    })
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
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
        notify::emit_event("history/changed", serde_json::json!({"op": "visit"}));
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
        notify::emit_event("history/changed", serde_json::json!({"op": "title"}));
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
        notify::emit_event("history/changed", serde_json::json!({"op": "remove"}));
    }
    st
}

/// Drops every visit (icons are kept — clearIcons is separate,
/// matching HistoryManager::clear() vs clearIcons()).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_clear() -> RcStatus {
    let st = status_of(|| history::with(|h| h.clear()));
    if st == RcStatus::Ok {
        notify::emit("history");
        notify::emit_event("history/changed", serde_json::json!({"op": "clear"}));
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_icon_clear() -> RcStatus {
    status_of(|| history::with(|s| s.icon_clear()))
}

/// Deletes every visit at or after `since_ms` (ms-epoch) — the
/// ranged leg of a clear-private-data plan (CLR01).  Icons are kept.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_clear_since(since_ms: i64) -> RcStatus {
    let st = status_of(|| history::with(|s| s.clear_since(since_ms)));
    if st == RcStatus::Ok {
        notify::emit("history");
        notify::emit_event(
            "history/changed",
            serde_json::json!({"op": "clear_since"}),
        );
    }
    st
}

/// Deletes every visit at or before `cutoff_ms` (ms-epoch) — the bulk
/// retention expiry behind HistoryManager's daysToExpire rule; the
/// caller computes the local-time boundary.  Icons are kept.
/// `out_removed` may be null; when given it receives the deleted
/// row count.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_expire_older_than(
    cutoff_ms: i64,
    out_removed: *mut i64,
) -> RcStatus {
    let st = status_of(|| {
        let removed = history::with(|s| s.expire_older_than(cutoff_ms))?;
        if !out_removed.is_null() {
            unsafe { *out_removed = removed };
        }
        Ok(())
    });
    if st == RcStatus::Ok {
        notify::emit("history");
        notify::emit_event(
            "history/changed",
            serde_json::json!({"op": "expire"}),
        );
    }
    st
}

/// Runs a filtered listing query — the store-side query surface
/// (RC04).  `spec_json` is an object, all fields optional:
///   "term"     case-insensitive substring on url OR title (the
///              history dialog's all-columns fixed-string match)
///   "from_ms"  inclusive visited_at lower bound (ms-epoch)
///   "until_ms" inclusive visited_at upper bound
///   "limit"    max rows returned — absent or <0 is unbounded
///   "offset"   rows skipped after filtering
/// Returns a JSON array [{"url","title","ts"}, ...] in the same
/// deduped newest-first order rc_hist_entry_at iterates.  NULL on a
/// malformed spec or an unopened store; free with rc_string_free().
///
/// # Safety
/// `spec_json` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_hist_query(spec_json: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(spec) = (unsafe { util::cstr(spec_json) }) else {
            error::set_error("bad query pointer");
            return ptr::null_mut();
        };
        history::with(|h| match h.query(spec) {
            Ok(j) => util::to_c_string(j),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        })
    }))
    .unwrap_or(ptr::null_mut())
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
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_session_exists() -> i32 {
    catch_unwind(AssertUnwindSafe(session::exists)).unwrap_or(false) as i32
}

/// Deletes the session file; absent is not an error.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_session_clear() -> RcStatus {
    status_of(session::clear)
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

// ---- session lifecycle marker (RC13) -------------------------------------
//
// session.state next to session.dat: the clean-shutdown flag +
// crash-dirty record + restore-loop guard, so "restore previous
// session?" works identically on every engine.  Same semantics as
// BrowserProfile's markSession* sentinels — the C++ side drives both.

/// Boots the session marker: returns 1 when the previous run's armed
/// marker survived (it died uncleanly — crash counter bumped,
/// "unclean" breadcrumb written), 0 on a clean/absent record, -1 on
/// error (rc_last_error_message).  Either way this run's marker is
/// now armed.
///
/// # Safety
/// No pointers; thread-safe behind the store lock.
#[no_mangle]
pub unsafe extern "C" fn rc_session_mark_start() -> i32 {
    catch_unwind(AssertUnwindSafe(|| match session::mark_start() {
        Ok(dirty) => dirty as i32,
        Err(e) => {
            error::set_error(&e.msg);
            -1
        }
    }))
    .unwrap_or(-1)
}

/// Records a clean exit — disarms the marker, resets the crash
/// counter, breadcrumbs "clean rc=N".
///
/// # Safety
/// No pointers; thread-safe behind the store lock.
#[no_mangle]
pub unsafe extern "C" fn rc_session_mark_exit(code: i32) -> RcStatus {
    status_of(|| session::mark_exit(code))
}

/// Records a handled signal termination — clean for crash-loop
/// purposes, but the breadcrumb names the signal.
///
/// # Safety
/// No pointers; thread-safe behind the store lock.
#[no_mangle]
pub unsafe extern "C" fn rc_session_mark_terminated(signal: i32) -> RcStatus {
    status_of(|| session::mark_terminated(signal))
}

/// True while the session marker reads armed — at boot, a still-armed
/// marker means the previous run never recorded an exit.
///
/// # Safety
/// No pointers.
#[no_mangle]
pub unsafe extern "C" fn rc_session_dirty() -> i32 {
    catch_unwind(AssertUnwindSafe(session::dirty)).unwrap_or(false) as i32
}

/// The consecutive-unclean count — the crash-loop signal.
///
/// # Safety
/// No pointers.
#[no_mangle]
pub unsafe extern "C" fn rc_session_crash_count() -> i32 {
    catch_unwind(AssertUnwindSafe(|| session::crash_count() as i32)).unwrap_or(0)
}

/// The last recorded exit breadcrumb ("clean rc=0", "terminated by
/// signal N (NAME)", "unclean (crash or external kill)").
/// RC_NOT_FOUND when none was ever written.
///
/// # Safety
/// `out` receives a buffer to release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_session_last_exit(out: *mut RcBuffer) -> RcStatus {
    status_of(|| {
        match session::last_exit() {
            Some(s) => {
                buffer_out(out, s.into_bytes());
                Ok(())
            }
            None => error::fail(RcStatus::NotFound, "no recorded exit"),
        }
    })
}

/// Arms/disarms the restore-loop guard — set before tabs reopen,
/// cleared by the next successful save.
///
/// # Safety
/// No pointers; thread-safe behind the store lock.
#[no_mangle]
pub unsafe extern "C" fn rc_session_set_restoring(on: i32) -> RcStatus {
    status_of(|| session::set_restoring(on != 0))
}

/// True while the restore guard reads armed — a crash mid-restore
/// leaves it set so the next boot prompts instead of looping.
///
/// # Safety
/// No pointers.
#[no_mangle]
pub unsafe extern "C" fn rc_session_restoring() -> i32 {
    catch_unwind(AssertUnwindSafe(session::restoring)).unwrap_or(false) as i32
}

// ---- session snapshot/restore policy (MD08) ---------------------------
// The session pipeline's decision layer: the AutoSaver debounce +
// ceiling, the dirty-on-what class gate, the retention caps (snapshot
// max age / window / tab counts), the crash-restore answer and the
// startup-mode fallback.  Typed reads over the bound profile's
// settings.json ("session/" prefix) — the Qt writers own the keys;
// this surface only answers questions.  The timers, the restore
// prompt and the manifest truncation all stay Qt-side.

/// Evaluates one session-policy manifest (JSON) against the bound
/// profile's settings store — see sesspolicy.rs for the op grammar
/// (defaults, policy, snapshot, dirty, retention, stale, restore;
/// "defaults" is the only NULL-store-legal op, and the other reads
/// degrade to the compiled-in constants on an absent store — only
/// "stale" genuinely needs the dir).  NULL on malformed input or
/// panic — the caller keeps its compiled-in consult, never guesses.
/// Read-only: no change topic is emitted.  Free the result with
/// rc_string_free().
///
/// # Safety
/// `h` must come from rc_settings_open or be NULL; `json` must point
/// to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_sesspolicy_call(
    h: *const settings::RcSettings,
    json: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let s = unsafe { h.as_ref() };
        match unsafe { util::bytes(json, len) }
            .and_then(|d| policy::parse_json(d).ok())
        {
            Some(manifest) => match sesspolicy::call_json(s, &manifest) {
                Ok(result) => util::to_c_string(result.to_string()),
                Err(e) => {
                    error::set_error(&e.msg);
                    ptr::null_mut()
                }
            },
            None => {
                error::set_error("bad sesspolicy call payload");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
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

/// Most-visited hosts for chrome.topSites (MD27) — a JSON array
/// `[{"host","url","title","visits","first_seen"}, ...]` ordered by
/// visit count (descending, earliest first_seen breaking ties), each
/// host's newest visit supplying the representative url/title.
/// `limit` <= 0 picks the default 20.  NULL on error (store not
/// open: rc_last_error_message).  Free with rc_string_free().
#[no_mangle]
pub unsafe extern "C" fn rc_history_top_hosts(
    limit: i64,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match history::with(|h| h.top_hosts(limit)) {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- command palette (CPAL01) ------------------------------------------
//
// The palette's query -> ranked-items decision.  The row registry
// stays Qt-side (QActions, WebViews, bookmark handles cannot cross
// the FFI) — the shell marshals {match text, id} per row and reads
// ordered indices back.

/// The palette fuzzy scorer — the CommandPalette::fuzzyScore port
/// plus the documented camel-hump boundary bonus.  Returns >= 0 on a
/// match (higher is better), -1 on no match or a bad pointer.
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_pal_score(
    query: *const c_char,
    candidate: *const c_char,
) -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        match (unsafe { util::cstr(query) }, unsafe { util::cstr(candidate) }) {
            (Some(q), Some(c)) => palette::fuzzy_score(q, c),
            _ => -1,
        }
    }))
    .unwrap_or(-1)
}

/// Rank a whole palette item set: request JSON
///   {"items":[{"match":"...","id":"..."},...], "mru":["id",...]}
/// -> JSON array [{"index":n,"score":n},...] of the matched rows in
/// display order (score desc, ties keep item order; mru position adds
/// the legacy 60-index boost).  NULL on a bad pointer or malformed
/// request (rc_last_error_message).  Free with rc_string_free().
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_pal_match(
    query: *const c_char,
    items_json: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let q = match unsafe { util::cstr(query) } {
            Some(s) => s,
            None => {
                error::set_error("bad query pointer");
                return ptr::null_mut();
            }
        };
        let text = match unsafe { util::cstr(items_json) } {
            Some(s) => s,
            None => {
                error::set_error("bad items pointer");
                return ptr::null_mut();
            }
        };
        match palette::match_json(q, text) {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- QR encoding (QRC01) ---------------------------------------------
//
// Nayuki's qrcodegen-rs behind the ABI — same code lineage as the
// vendored C++ encoder, so the module matrix is bit-identical.

/// Encodes `text` (UTF-8) into a QR symbol at `ecc_level`
/// (0=Low 1=Medium 2=Quartile 3=High — the qrcodegen Ecc ordinals).
/// `out` receives u32le size followed by size*size module bytes,
/// row-major, 1=dark (no quiet zone).  RC_INVALID_ARGUMENT on a bad
/// pointer, out-of-range level or over-capacity payload.
///
/// # Safety
/// `text` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_qr_encode(
    text: *const c_char,
    ecc_level: i32,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let t = unsafe { util::cstr(text) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad text pointer".into(),
        })?;
        let v = qrcode::encode(t, ecc_level)?;
        buffer_out(out, v);
        Ok(())
    })
}

// ---- tlsprobe (SEC22) -----------------------------------------------
//
// Second-opinion TLS verification.  rc_tls_check performs a real
// handshake to host:port and evaluates the chain with rustls+webpki
// against the platform roots — independent of the engine's verifier.
// The result is ALWAYS a JSON verdict document (see tlsprobe.rs and
// rustcore.h for the status/error-class vocabulary), never an FFI
// error for a reachable-but-bad chain; NULL is reserved for bad
// arguments.

/// Flags accepted by rc_tls_check.
pub const RC_TLS_F_ALLOW_LOCAL: u32 = tlsprobe::FLAG_ALLOW_LOCAL;

/// Second-opinion TLS check.  Performs a blocking handshake (5 s
/// connect, 5 s per-io, 15 s total budget) — callers run it on a
/// worker thread, never the UI thread.  Returns the verdict JSON:
///   {"status":"verified"|"warning"|"unverified"|"refused",
///    "error_class":null|<class>, "detail":..., "host":..., "port":...,
///    "revocation":"not-checked",
///    "negotiated":{"tls_version","cipher_suite","alpn"}|null,
///    "roots":{"native":n,"extra":m},
///    "chain":[{"subject","issuer","serial","not_before","not_after",
///              "signature_algorithm","sans":[...]}]}
/// error_class vocabulary: expired, not-yet-valid, bad-hostname,
/// untrusted-root, broken-chain, weak-signature, revoked, protocol,
/// network, root-store, private-host, invalid-host, invalid-port.
/// Free the result with rc_string_free(); NULL on bad arguments.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tls_check(
    host: *const c_char,
    port: u16,
    flags: u32,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(host) } {
            Some(h) => util::to_c_string(tlsprobe::check(h, port, flags).to_string()),
            None => {
                error::set_error("bad host pointer");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Installs an extra DER trust anchor consulted by subsequent probes —
/// the fixture-CA seam for tests and the hook for enterprise/user
/// roots.  RC_CORRUPT when the bytes do not parse as X.509.
///
/// # Safety
/// `der` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_tls_add_root(der: *const u8, len: usize) -> RcStatus {
    status_of(|| {
        let der = unsafe { util::bytes(der, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad der pointer".into(),
        })?;
        tlsprobe::add_root(der)
    })
}

/// Drops every caller-installed anchor (the platform store is loaded
/// per probe and unaffected).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_tls_clear_roots() -> RcStatus {
    status_of(tlsprobe::clear_roots)
}

// ---- SITED01: consolidated per-site decision store ----
//
// One durable kind/key/value store behind sitedecisions.json — the
// Qt side reaches it through sitedecisionstore.{h,cpp}; every mutating
// call persists atomically and emits the "sitedecisions" notification
// topic so UI consumers refresh through the queued bridge.

/// Reads `kind`'s row for `key` into `out`.  RC_OK with len>0 on a
/// hit, RC_NOT_FOUND on a miss.
///
/// # Safety
/// `kind`/`key` must be NUL-terminated UTF-8; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_get(
    kind: *const c_char,
    key: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let kind = unsafe { util::cstr(kind) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad kind pointer".into(),
        })?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        match sitedecisions::get(kind, key)? {
            Some(value) => {
                buffer_out(out, value.into_bytes());
                Ok(())
            }
            None => Err(error::Fail {
                status: RcStatus::NotFound,
                msg: "no such site decision".into(),
            }),
        }
    })
}

/// Records `value` under (`kind`, `key`), persists atomically, and
/// emits the change notification only when the stored row changed.
///
/// # Safety
/// All pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_set(
    kind: *const c_char,
    key: *const c_char,
    value: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let kind = unsafe { util::cstr(kind) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad kind pointer".into(),
        })?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let value =
            unsafe { util::cstr(value) }.ok_or_else(|| error::Fail {
                status: RcStatus::InvalidArgument,
                msg: "bad value pointer".into(),
            })?;
        sitedecisions::set(kind, key, value).map(|_| ())
    });
    if status == RcStatus::Ok {
        notify::emit("sitedecisions");
    }
    status
}

/// Drops the row for (`kind`, `key`); RC_OK whether or not it existed
/// (decision removal is idempotent).
///
/// # Safety
/// Pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_remove(
    kind: *const c_char,
    key: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let kind = unsafe { util::cstr(kind) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad kind pointer".into(),
        })?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        sitedecisions::remove(kind, key).map(|_| ())
    });
    if status == RcStatus::Ok {
        notify::emit("sitedecisions");
    }
    status
}

/// Removes every row of `kind` — the adapters' "clear all" path.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_clear(kind: *const c_char) -> RcStatus {
    let status = status_of(|| {
        let kind = unsafe { util::cstr(kind) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad kind pointer".into(),
        })?;
        sitedecisions::clear_kind(kind).map(|_| ())
    });
    if status == RcStatus::Ok {
        notify::emit("sitedecisions");
    }
    status
}

/// Replaces `kind`'s rows wholesale with a JSON object
/// `{"key":"value",...}` — the "in-memory list is authoritative"
/// mirror-write used by the list-shaped stores (popup/cookie/js).
///
/// # Safety
/// `json` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_replace(
    kind: *const c_char,
    json: *const u8,
    len: usize,
) -> RcStatus {
    let status = status_of(|| {
        let kind = unsafe { util::cstr(kind) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad kind pointer".into(),
        })?;
        let json = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        sitedecisions::replace_kind(kind, json).map(|_| ())
    });
    if status == RcStatus::Ok {
        notify::emit("sitedecisions");
    }
    status
}

/// Every row of `kind` as `{"key":"value",...}` JSON — caller frees
/// with rc_string_free().
///
/// # Safety
/// `kind` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_list(kind: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(kind) } {
            Some(kind) => match sitedecisions::list_json(kind) {
                Ok(json) => util::to_c_string(json),
                Err(e) => {
                    error::set_error(&e.msg);
                    ptr::null_mut()
                }
            },
            None => {
                error::set_error("bad kind pointer");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The whole store — `{"version":1,"generation":N,"kinds":{...}}` —
/// for read-locked IO-thread policy snapshots.
/// Caller frees with rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_snapshot() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match sitedecisions::snapshot_json() {
        Ok(json) => util::to_c_string(json),
        Err(e) => {
            error::set_error(&e.msg);
            ptr::null_mut()
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Host-suffix lookup: returns `{"key":"matched-key","value":"..."}`
/// for the longest matching stored host suffix of `host`, or NULL when
/// nothing matches.
///
/// # Safety
/// `kind`/`host` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_lookup(
    kind: *const c_char,
    host: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let kind = match unsafe { util::cstr(kind) } {
            Some(k) => k,
            None => {
                error::set_error("bad kind pointer");
                return ptr::null_mut();
            }
        };
        let host = match unsafe { util::cstr(host) } {
            Some(h) => h,
            None => {
                error::set_error("bad host pointer");
                return ptr::null_mut();
            }
        };
        match sitedecisions::lookup(kind, host) {
            Ok(Some((key, value))) => util::to_c_string(
                serde_json::json!({"key": key, "value": value}).to_string(),
            ),
            Ok(None) => ptr::null_mut(),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// 1 when the store file exists — the Qt migration code's
/// "has anything been stored yet?" gate.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_store_present() -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        i32::from(sitedecisions::store_present())
    }))
    .unwrap_or(0)
}

/// Re-reads the disk file into the in-memory store.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_reload() -> RcStatus {
    status_of(sitedecisions::reload)
}

/// Empties every kind (file removed) — the "clear site data" and
/// test-suite reset path.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_sitedec_reset() -> RcStatus {
    status_of(sitedecisions::reset)
}

// ---- RC15: per-profile cookie database ----
//
// The Servo/WPE jar's persistent home and Chromium's durable mirror:
// cookies.db under the profile dir (or the anchored data dir on this
// surface), container-partitioned rows, Qt-jar-verified matching, and
// a stored role flag distinguishing "this file IS the jar" from "the
// engine owns the jar; this is its on-disk shadow".  Mutations emit
// the "cookies" change topic; returned records are the JSON shape
// cookies::Cookie::to_json emits.

/// NULL-or-NUL-terminated-UTF-8 for the optional string params the
/// cookie surface takes: NULL -> Ok(None); a non-NULL pointer that is
/// not valid UTF-8 is an error, never a silent default.
unsafe fn cookies_opt(ptr: *const c_char) -> error::RcResult<Option<String>> {
    if ptr.is_null() {
        return Ok(None);
    }
    unsafe { util::cstr(ptr) }
        .map(|s| Some(s.to_string()))
        .ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad string pointer".into(),
        })
}

/// Opens the cookie database.  `path` NULL resolves to
/// <data dir>/cookies.db; an explicit path binds a profile dir (the
/// facade's per-profile lane passes profiles/<id>/cookies.db).
///
/// # Safety
/// `path` must be NULL or NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_open(path: *const c_char) -> RcStatus {
    status_of(|| {
        let path = unsafe { cookies_opt(path) }?;
        cookies::with(|s| s.open(path.as_deref()))
    })
}

/// 1 while a database is open — the cheap probe (never errors).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_is_open() -> i32 {
    catch_unwind(AssertUnwindSafe(|| i32::from(cookies::with(|s| s.is_open()))))
        .unwrap_or(0)
}

/// The declared mode — 0 authoritative (this file IS the jar), 1
/// mirror (an engine owns the jar; this is its durable shadow),
/// -1 on error (not open).  Honest capability reporting: the caller
/// decides what to trust based on who declared the role.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_role() -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        cookies::with(|s| s.role()).unwrap_or(-1)
    }))
    .unwrap_or(-1)
}

/// Declares the mode (0 or 1) — recorded in the db's meta table so
/// tooling reading the file later knows what it last served.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_set_role(role: i64) -> RcStatus {
    status_of(|| cookies::with(|s| s.set_role(role)))
}

/// Upserts one cookie record — the JSON shape Cookie::to_json emits
/// ({"container":"","name":"","value":"","domain":"","path":"/",
/// "expires":ms|null,"secure":b,"httpOnly":b,"sameSite":n,
/// "hostOnly":b,"created":ms}).  An already-past expiry deletes the
/// row (the Set-Cookie eviction rule); an identical rewrite is a
/// no-op (no "cookies" notification).
///
/// # Safety
/// `json` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_set(json: *const u8, len: usize) -> RcStatus {
    let mut changed = false;
    let status = status_of(|| {
        let json = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        let v: serde_json::Value = serde_json::from_slice(json).map_err(|e| {
            error::Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("cookie json: {e}"),
            }
        })?;
        let c = cookies::Cookie::from_json(&v)?;
        cookies::with(|s| s.set(&c)).map(|o| {
            changed = o != cookies::SetOutcome::Noop;
        })
    });
    if status == RcStatus::Ok && changed {
        notify::emit("cookies");
    }
    status
}

/// One cookie by key — RC_NOT_FOUND when absent; the record JSON in
/// `out` (free with rc_buffer_free).
///
/// # Safety
/// `domain`/`name`/`path` must be NUL-terminated UTF-8; `container`
/// may be NULL ("" = the default partition); `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_get(
    domain: *const c_char,
    name: *const c_char,
    path: *const c_char,
    container: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let domain = unsafe { util::cstr(domain) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad domain pointer".into(),
        })?;
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        let path = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad path pointer".into(),
        })?;
        let container = unsafe { cookies_opt(container) }?.unwrap_or_default();
        let c = cookies::with(|s| s.get(&container, &domain, &name, &path))?;
        buffer_out(out, c.to_json().to_string().into_bytes());
        Ok(())
    })
}

/// Deletes one cookie by key — RC_OK either way.
///
/// # Safety
/// `domain`/`name`/`path` must be NUL-terminated UTF-8; `container`
/// may be NULL ("" = default).
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_remove(
    domain: *const c_char,
    name: *const c_char,
    path: *const c_char,
    container: *const c_char,
) -> RcStatus {
    let mut removed = false;
    let status = status_of(|| {
        let domain = unsafe { util::cstr(domain) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad domain pointer".into(),
        })?;
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        let path = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad path pointer".into(),
        })?;
        let container = unsafe { cookies_opt(container) }?.unwrap_or_default();
        cookies::with(|s| s.remove(&container, &domain, &name, &path))
            .map(|r| removed = r)
    });
    if status == RcStatus::Ok && removed {
        notify::emit("cookies");
    }
    status
}

/// Every cookie of a partition (NULL container = the whole db) as a
/// JSON array of records — the cookie-manager listing lane.
/// Caller frees with rc_string_free().
///
/// # Safety
/// `container` must be NULL or NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_list(container: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let container = match unsafe { cookies_opt(container) } {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match cookies::with(|s| s.list(container.as_deref())) {
            Ok(rows) => {
                let arr: Vec<serde_json::Value> =
                    rows.iter().map(cookies::Cookie::to_json).collect();
                util::to_c_string(serde_json::Value::Array(arr).to_string())
            }
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The send-match — which cookies ride a request.  `request` is JSON:
/// {"url":"https://h/p","container":"","first_party":"h"|null,
/// "now_ms":0} — first_party engages the SameSite cross-site filter;
/// now_ms 0 = system clock.  Returns a JSON array of records in
/// RFC6265 §5.4 send order (longest path, then earliest created).
/// Caller frees with rc_string_free().
///
/// # Safety
/// `request` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_for_url(
    request: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let body = match unsafe { util::bytes(request, len) } {
            Some(b) => b,
            None => {
                error::set_error("bad request pointer");
                return ptr::null_mut();
            }
        };
        let v: serde_json::Value = match serde_json::from_slice(body) {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&format!("for_url json: {e}"));
                return ptr::null_mut();
            }
        };
        match cookies::ForUrl::from_json(&v)
            .and_then(|req| cookies::with(|s| s.for_url(&req)))
        {
            Ok(rows) => {
                let arr: Vec<serde_json::Value> =
                    rows.iter().map(cookies::Cookie::to_json).collect();
                util::to_c_string(serde_json::Value::Array(arr).to_string())
            }
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Parses + stores one Set-Cookie header value against `url` — the
/// authoritative backend's ingest lane.  Returns a verdict JSON
/// {"stored":bool,"reason":"stored"|"deleted"|"…"} (caller frees).
///
/// # Safety
/// `url`/`header` must be NUL-terminated UTF-8; `container` may be
/// NULL ("" = default).
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_store_header(
    url: *const c_char,
    header: *const c_char,
    container: *const c_char,
) -> *mut c_char {
    let mut stored = false;
    let out = catch_unwind(AssertUnwindSafe(|| {
        let url = match unsafe { util::cstr(url) } {
            Some(u) => u,
            None => {
                error::set_error("bad url pointer");
                return ptr::null_mut();
            }
        };
        let header = match unsafe { util::cstr(header) } {
            Some(h) => h,
            None => {
                error::set_error("bad header pointer");
                return ptr::null_mut();
            }
        };
        let container = match unsafe { cookies_opt(container) } {
            Ok(c) => c.unwrap_or_default(),
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match cookies::with(|s| s.store_header(&container, &url, &header)) {
            Ok(v) => {
                stored = v.get("stored").and_then(serde_json::Value::as_bool)
                    == Some(true);
                util::to_c_string(v.to_string())
            }
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut());
    if stored {
        notify::emit("cookies");
    }
    out
}

/// The cookie-policy gate — may this attempt proceed, and with what
/// rewrites.  Consults the sibling sitedecisions.json "cookie" kind
/// (the file CookieJar's exceptions already persist through) plus the
/// caller-supplied policy snapshot.  Input/output shapes are
/// documented on cookies::gate.  Caller frees the returned JSON.
///
/// # Safety
/// `input` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_gate(
    input: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let body = match unsafe { util::bytes(input, len) } {
            Some(b) => b,
            None => {
                error::set_error("bad gate input pointer");
                return ptr::null_mut();
            }
        };
        let v: serde_json::Value = match serde_json::from_slice(body) {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&format!("gate json: {e}"));
                return ptr::null_mut();
            }
        };
        match cookies::with(|s| {
            let dir = s.dir().ok_or_else(|| error::Fail {
                status: RcStatus::NotInitialized,
                msg: "rc_cookies_open() has not been called".into(),
            })?;
            cookies::gate(&dir, &v)
        }) {
            Ok(verdict) => util::to_c_string(verdict.to_string()),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Imports a QNetworkCookie::toRawForm file — one raw line per row,
/// the shape the vendored NetworkCookieJar saveState streams and
/// testdata/cookies/export.txt carries.  Lines lacking a Domain
/// attribute are skipped (there is no request host to key them on).
/// Returns a JSON report {"imported":N,"skipped":M} — caller frees.
///
/// # Safety
/// `path` must be NUL-terminated UTF-8; `container` may be NULL
/// ("" = default partition).
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_import(
    path: *const c_char,
    container: *const c_char,
) -> *mut c_char {
    let mut imported = false;
    let out = catch_unwind(AssertUnwindSafe(|| {
        let path = match unsafe { util::cstr(path) } {
            Some(p) => p,
            None => {
                error::set_error("bad import path pointer");
                return ptr::null_mut();
            }
        };
        let container = match unsafe { cookies_opt(container) } {
            Ok(c) => c.unwrap_or_default(),
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let meta = match std::fs::metadata(&path) {
            Ok(m) => m,
            Err(e) => {
                error::set_error(&format!("import source: {e}"));
                return ptr::null_mut();
            }
        };
        if meta.len() > 32 * 1024 * 1024 {
            error::set_error("import file over 32 MiB cap");
            return ptr::null_mut();
        }
        let text = match std::fs::read_to_string(&path) {
            Ok(t) => t,
            Err(e) => {
                error::set_error(&format!("import read: {e}"));
                return ptr::null_mut();
            }
        };
        match cookies::with(|s| s.import_text(&container, &text)) {
            Ok(v) => {
                imported = v.get("imported").and_then(serde_json::Value::as_u64)
                    .unwrap_or(0)
                    > 0;
                util::to_c_string(v.to_string())
            }
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut());
    if imported {
        notify::emit("cookies");
    }
    out
}

/// Exports the partition (NULL container = all) as toRawForm lines —
/// the same file shape rc_cookies_import reads back.  Written
/// atomically under the caller's path.
///
/// # Safety
/// `path` must be NUL-terminated UTF-8; `container` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_export(
    path: *const c_char,
    container: *const c_char,
) -> RcStatus {
    status_of(|| {
        let path = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad export path pointer".into(),
        })?;
        let container = unsafe { cookies_opt(container) }?;
        let text = cookies::with(|s| s.export_text(container.as_deref()))?;
        store::atomic_write(std::path::Path::new(&path), text.as_bytes())
    })
}

/// Wipes a partition — `container` NULL wipes the whole database.
/// RC_OK either way.
///
/// # Safety
/// `container` must be NULL or NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_clear(container: *const c_char) -> RcStatus {
    let container = match unsafe { cookies_opt(container) } {
        Ok(c) => c,
        Err(e) => {
            error::set_error(&e.msg);
            return e.status;
        }
    };
    let mut wiped = 0u64;
    let status = status_of(|| {
        cookies::with(|s| s.clear(container.as_deref())).map(|n| wiped = n)
    });
    if status == RcStatus::Ok && wiped > 0 {
        notify::emit("cookies");
    }
    status
}

/// The shield panel's "clear site data" sweep — drops rows scoped to
/// the host or a subdomain it controls, plus rows on a parent domain
/// the site still receives (CookieJar::removeCookiesForHost verbatim).
///
/// # Safety
/// `host` must be NUL-terminated UTF-8; `container` may be NULL
/// ("" = default partition).
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_remove_for_host(
    host: *const c_char,
    container: *const c_char,
) -> RcStatus {
    let mut removed = 0u64;
    let status = status_of(|| {
        let host = unsafe { util::cstr(host) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad host pointer".into(),
        })?;
        let container = unsafe { cookies_opt(container) }?.unwrap_or_default();
        cookies::with(|s| s.remove_for_host(&container, &host)).map(|n| {
            removed = n;
        })
    });
    if status == RcStatus::Ok && removed > 0 {
        notify::emit("cookies");
    }
    status
}

/// Row count — a partition, or the whole db on a NULL container.
/// -1 on error (not open).
///
/// # Safety
/// `container` must be NULL or NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_count(container: *const c_char) -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        let container = match unsafe { cookies_opt(container) } {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        cookies::with(|s| s.count(container.as_deref())).unwrap_or(-1)
    }))
    .unwrap_or(-1)
}

/// Deletes rows whose expiry is at or before `now_ms`.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_purge_expired(now_ms: i64) -> RcStatus {
    let mut n = 0u64;
    let status = status_of(|| {
        cookies::with(|s| s.purge_expired(now_ms)).map(|c| n = c)
    });
    if status == RcStatus::Ok && n > 0 {
        notify::emit("cookies");
    }
    status
}

/// The exit-sweep: drops every session row (the C++ jar's endSession
/// equivalent — also run automatically at open).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_cookies_purge_session() -> RcStatus {
    let mut n = 0u64;
    let status = status_of(|| cookies::with(|s| s.purge_session()).map(|c| n = c));
    if status == RcStatus::Ok && n > 0 {
        notify::emit("cookies");
    }
    status
}

// ---- RC16: per-profile site-icon cache ----
//
// The disposable lane: icon records keyed by page-url/origin, pointing
// at PNG files under <data dir>/cache/icons/ — never the data root.
// Fetch is Qt/engine-side (the page already carries the bytes); this
// store owns the index, the dedup-by-host file naming, the expiry
// sweep and the file lifecycle.  Mutations emit the "icons" change
// topic.  See icons.rs for the layout and the legacy import.

/// Stores the favicon PNG for `url`'s icon key — the lowercase host,
/// or the whole url when it carries no host (the historyIconKey rule
/// the LocationBarSiteIcon consumers rely on).  Every page of a host
/// shares one file.  A rewrite carrying identical bytes is a no-op.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8; `png` points to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_put(
    url: *const c_char,
    png: *const u8,
    len: usize,
) -> RcStatus {
    let mut changed = false;
    let status = status_of(|| {
        let url = unsafe { util::cstr(url) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad icon url pointer".into(),
        })?;
        let data = unsafe { util::bytes(png, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad icon pointer".into(),
        })?;
        icons::put(url, data).map(|c| changed = c)
    });
    if status == RcStatus::Ok && changed {
        notify::emit("icons");
    }
    status
}

/// Fetches the stored favicon PNG for `url`'s key; RC_NOT_FOUND when
/// the site has none.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8; `out` receives a buffer to
/// release with rc_buffer_free().
#[no_mangle]
pub unsafe extern "C" fn rc_icons_get(url: *const c_char, out: *mut RcBuffer) -> RcStatus {
    status_of(|| {
        let url = unsafe { util::cstr(url) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad icon url pointer".into(),
        })?;
        buffer_out(out, icons::get(url)?);
        Ok(())
    })
}

/// Drops the icon record for `url`'s key — RC_OK either way.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_remove(url: *const c_char) -> RcStatus {
    let mut removed = false;
    let status = status_of(|| {
        let url = unsafe { util::cstr(url) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad icon url pointer".into(),
        })?;
        icons::remove(url).map(|r| removed = r)
    });
    if status == RcStatus::Ok && removed {
        notify::emit("icons");
    }
    status
}

/// Wipes the profile's whole icon cache — the clearIconDatabase
/// equivalent.  RC_OK whether or not anything existed.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_clear() -> RcStatus {
    let had = catch_unwind(AssertUnwindSafe(|| icons::count().unwrap_or(0)))
        .unwrap_or(0)
        > 0;
    let status = status_of(icons::clear);
    if status == RcStatus::Ok && had {
        notify::emit("icons");
    }
    status
}

/// Expiry sweep: drops records not refreshed at-or-before `cutoff_ms`
/// and unlinks their files — the caller computes the retention
/// boundary (HistoryManager passes its history cutoff).  `out_removed`
/// may be NULL; when given it receives the dropped count.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_sweep(
    cutoff_ms: i64,
    out_removed: *mut i64,
) -> RcStatus {
    let mut removed = 0u64;
    let status = status_of(|| icons::sweep(cutoff_ms).map(|n| removed = n));
    if status == RcStatus::Ok {
        if !out_removed.is_null() {
            unsafe { *out_removed = removed as i64 };
        }
        if removed > 0 {
            notify::emit("icons");
        }
    }
    status
}

/// Record count; -1 on error (no anchored data dir).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_count() -> i64 {
    catch_unwind(AssertUnwindSafe(|| icons::count().unwrap_or(-1))).unwrap_or(-1)
}

/// The store's icon key for `url` — lowercase host, or the whole url
/// when hostless; the derivation both FFI spellings share.  Caller
/// frees with rc_string_free; NULL on invalid input.
///
/// # Safety
/// `url` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_key(url: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let url = match unsafe { util::cstr(url) } {
            Some(u) => u,
            None => {
                error::set_error("bad icon url pointer");
                return ptr::null_mut();
            }
        };
        match icons::key_for(url) {
            Ok(k) => util::to_c_string(k),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The index rows as a JSON array — [{"key","file","updated",
/// "pages":[]}] — caller frees with rc_string_free.  NULL on error.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_icons_list() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match icons::list() {
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
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- RC02: typed per-profile settings store ----
//
// The profile-bound key-value store replacing ProfileSettings'
// persistence — scope (global, or profiles/<id>/ under the data
// root) is bound at rc_settings_open(), so no rc_set_data_dir
// re-anchoring is needed and alternating personas is cheap.
// settings.json is canonical; the legacy QSettings ini pair is a
// one-shot read source (profile ini auto-imported on first open,
// the global ini through rc_settings_import_ini with the MPROF04
// classification filter).  Mutations persist atomically and emit
// the "settings" topic.

fn settings_ref<'a>(
    h: *const settings::RcSettings,
) -> error::RcResult<&'a settings::RcSettings> {
    unsafe { h.as_ref() }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: "bad settings handle".into(),
    })
}

/// Opens the store for `profile_id` (NULL/empty selects the global
/// scope) under `data_root` — a real path, not the process-global
/// data dir.  Returns an opaque handle or NULL (error stashed).
///
/// # Safety
/// String pointers must be NUL-terminated UTF-8; the returned
/// handle is released with rc_settings_close().
#[no_mangle]
pub unsafe extern "C" fn rc_settings_open(
    data_root: *const c_char,
    profile_id: *const c_char,
) -> *mut settings::RcSettings {
    catch_unwind(AssertUnwindSafe(|| {
        let root = match unsafe { util::cstr(data_root) } {
            Some(r) => r,
            None => {
                error::set_error("bad data root pointer");
                return ptr::null_mut();
            }
        };
        // NULL means global scope; a non-NULL pointer that is not
        // valid UTF-8 is an error, never a silent scope switch.
        let pid = if profile_id.is_null() {
            None
        } else {
            match unsafe { util::cstr(profile_id) } {
                Some(p) => Some(p),
                None => {
                    error::set_error("bad profile id pointer");
                    return ptr::null_mut();
                }
            }
        };
        match settings::RcSettings::open(root, pid) {
            Ok(h) => Box::into_raw(Box::new(h)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Releases a handle from rc_settings_open().  NULL-tolerant.
///
/// # Safety
/// `h` must come from rc_settings_open and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_close(h: *mut settings::RcSettings) {
    if !h.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(unsafe { Box::from_raw(h) });
        }));
    }
}

/// One-shot legacy import: merges a QSettings ini file at `path`.
/// Profile-scope handles keep only isProfileKey()-classified keys;
/// global-scope handles keep the complement.  Never overwrites an
/// existing key; a second call is a no-op.
///
/// # Safety
/// `path` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_import_ini(
    h: *mut settings::RcSettings,
    path: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let path = unsafe { util::cstr(path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad ini path pointer".into(),
        })?;
        h.import_ini(path)
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// 1 when `key` exists in this store.
///
/// # Safety
/// `key` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_contains(
    h: *const settings::RcSettings,
    key: *const c_char,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(_) => return 0,
        };
        match unsafe { util::cstr(key) } {
            Some(k) => i32::from(h.contains(k).unwrap_or(false)),
            None => 0,
        }
    }))
    .unwrap_or(0)
}

/// The stored value as `{"t":"s|b|i|d|x|l","v":...}` JSON —
/// RC_NOT_FOUND when absent.  The typed accessor the Qt bridge
/// rebuilds a QVariant from.
///
/// # Safety
/// `key` must be NUL-terminated UTF-8; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_get(
    h: *const settings::RcSettings,
    key: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let v = h.get(key)?;
        let json = settings::value_to_json(&v).to_string();
        buffer_out(out, json.into_bytes());
        Ok(())
    })
}

macro_rules! settings_getter {
    ($name:ident, $out:ty, $call:ident, $fill:expr) => {
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *const settings::RcSettings,
            key: *const c_char,
            out: *mut $out,
        ) -> RcStatus {
            status_of(|| {
                let h = settings_ref(h)?;
                let key =
                    unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
                        status: RcStatus::InvalidArgument,
                        msg: "bad key pointer".into(),
                    })?;
                let value = h.$call(key)?;
                match unsafe { out.as_mut() } {
                    Some(slot) => {
                        $fill(slot, value);
                        Ok(())
                    }
                    None => Err(error::Fail {
                        status: RcStatus::InvalidArgument,
                        msg: "bad out pointer".into(),
                    }),
                }
            })
        }
    };
}

settings_getter!(rc_settings_get_bool, i32, get_bool, |slot: &mut i32, v: bool| {
    *slot = i32::from(v)
});
settings_getter!(rc_settings_get_int, i64, get_int, |slot: &mut i64, v: i64| {
    *slot = v
});
settings_getter!(
    rc_settings_get_double,
    f64,
    get_double,
    |slot: &mut f64, v: f64| *slot = v
);

/// Reads `key` as UTF-8 text into `out` (QVariant toString()
/// semantics — any stored type converts).  RC_NOT_FOUND when absent.
///
/// # Safety
/// `key` must be NUL-terminated UTF-8; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_get_string(
    h: *const settings::RcSettings,
    key: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        buffer_out(out, h.get_string(key)?.into_bytes());
        Ok(())
    })
}

/// Reads `key` as raw bytes into `out`.  RC_NOT_FOUND when absent.
///
/// # Safety
/// `key` must be NUL-terminated UTF-8; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_get_bytes(
    h: *const settings::RcSettings,
    key: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        buffer_out(out, h.get_bytes(key)?);
        Ok(())
    })
}

/// Reads `key` as a JSON array of strings into `out`
/// (QStringList semantics — a scalar becomes a one-element list).
/// RC_NOT_FOUND when absent.
///
/// # Safety
/// `key` must be NUL-terminated UTF-8; `out` must be writable.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_get_list(
    h: *const settings::RcSettings,
    key: *const c_char,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let json = serde_json::to_string(&h.get_list(key)?).map_err(|e| {
            error::Fail {
                status: RcStatus::Corrupt,
                msg: format!("encode settings list: {e}"),
            }
        })?;
        buffer_out(out, json.into_bytes());
        Ok(())
    })
}

/// Stores a UTF-8 string; persists atomically and emits "settings".
///
/// # Safety
/// All pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_set_string(
    h: *mut settings::RcSettings,
    key: *const c_char,
    value: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let value = unsafe { util::cstr(value) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad value pointer".into(),
        })?;
        h.set(key, settings::Value::Str(value.to_string()))
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Stores a bool (0/1).  # Safety: `key` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_set_bool(
    h: *mut settings::RcSettings,
    key: *const c_char,
    value: i32,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        h.set(key, settings::Value::Bool(value != 0))
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Stores a 64-bit integer.  # Safety: `key` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_set_int(
    h: *mut settings::RcSettings,
    key: *const c_char,
    value: i64,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        h.set(key, settings::Value::Int(value))
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Stores a finite double.  # Safety: `key` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_set_double(
    h: *mut settings::RcSettings,
    key: *const c_char,
    value: f64,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        h.set(key, settings::Value::Dbl(value))
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Stores raw bytes.  # Safety: `value` must point to `len` readable
/// bytes; `key` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_set_bytes(
    h: *mut settings::RcSettings,
    key: *const c_char,
    value: *const u8,
    len: usize,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let v = unsafe { util::bytes(value, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad value pointer".into(),
        })?;
        h.set(key, settings::Value::Bytes(v.to_vec()))
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Stores a string list, marshaled as a JSON array of strings.
/// # Safety: `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_set_list(
    h: *mut settings::RcSettings,
    key: *const c_char,
    json: *const u8,
    len: usize,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        let raw = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        let list: Vec<String> =
            serde_json::from_slice(raw).map_err(|e| error::Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("settings list payload: {e}"),
            })?;
        h.set(key, settings::Value::List(list))
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Drops `key` (idempotent), persists, emits "settings".
/// # Safety: `key` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_remove(
    h: *mut settings::RcSettings,
    key: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let key = unsafe { util::cstr(key) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad key pointer".into(),
        })?;
        h.remove(key)
    });
    if status == RcStatus::Ok {
        notify::emit("settings");
    }
    status
}

/// Keys under `prefix` (boundary match — `prefix` and `prefix/…`;
/// NULL/empty lists all) as a JSON array.  rc_string_free result.
///
/// # Safety
/// `prefix` must be NULL or NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_list(
    h: *const settings::RcSettings,
    prefix: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let prefix = unsafe { util::cstr(prefix) };
        match h.list(prefix) {
            Ok(keys) => util::to_c_string(
                serde_json::to_string(&keys).unwrap_or_else(|_| "[]".into()),
            ),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Flushes the store — mutations already write through, so this is
/// the QSettings sync() call shape, kept for API completeness.
///
/// # Safety
/// `h` must come from rc_settings_open.
#[no_mangle]
pub unsafe extern "C" fn rc_settings_sync(
    h: *mut settings::RcSettings,
) -> RcStatus {
    status_of(|| settings_ref(h)?.sync())
}

// ---- RC19: container registry + storage-namespace map ----
//
// ContainerManager's registry is a typed specialization over the
// settings store — the "containers" group of the PROFILE scope, so
// every op here runs on an RcSettings handle bound to the owning
// persona (global-scope handles see no containers; the group is a
// classified persona key).  The id list ("order") plus per-id
// name/color/icon keys match the C++ layout byte-for-byte — a
// registry written here is what loadRegistry reads on the next
// scope open, and the C++ ProfileSettings mirror keeps the
// settings.ini compat bytes flowing when C++ authors the change.
// The namespace map is a pure derivation: the same storage root /
// VSEC02 cache tree / Qt residue dir / storage name ContainerManager
// computes, plus the cookie-partition key the cookies db (RC15)
// partitions rows on.  Mutations emit the "containers" topic.

fn containers_cstr<'a>(p: *const c_char, what: &str) -> error::RcResult<&'a str> {
    unsafe { util::cstr(p) }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("bad {what} pointer"),
    })
}

fn containers_json_out(v: serde_json::Value) -> *mut c_char {
    util::to_c_string(v.to_string())
}

/// The bound profile's container registry as a JSON array of
/// {"id","name","color","icon"} in creation order — NULL + stashed
/// error on a bad handle.  Free with rc_string_free().
///
/// # Safety
/// `h` must come from rc_settings_open/ac_settings.
#[no_mangle]
pub unsafe extern "C" fn rc_containers_list(
    h: *const settings::RcSettings,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match containers::list(h) {
            Ok(rows) => containers_json_out(serde_json::Value::Array(
                rows.iter().map(containers::to_json).collect(),
            )),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// One registry row {"id","name","color","icon"} — NULL when the id
/// is unregistered (a miss is not an error).  Free with
/// rc_string_free().
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_containers_get(
    h: *const settings::RcSettings,
    id: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let id = match containers_cstr(id, "container id") {
            Ok(i) => i,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match containers::get(h, id) {
            Ok(c) => containers_json_out(containers::to_json(&c)),
            Err(e) if e.status == RcStatus::NotFound => ptr::null_mut(),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Creates a container from JSON {"name","color","icon"?} — the id
/// is minted here (v4 uuid, the C++ hex-with-dashes shape) and
/// appended to "order".  Returns the row {"id",...}; NULL + stashed
/// error on failure.  Free with rc_string_free().
///
/// # Safety
/// `json` must point to `len` readable bytes of UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_containers_create(
    h: *mut settings::RcSettings,
    json: *const u8,
    len: usize,
) -> *mut c_char {
    let made = catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let raw = match unsafe { util::bytes(json, len) } {
            Some(r) => r,
            None => {
                error::set_error("bad container json pointer");
                return ptr::null_mut();
            }
        };
        let spec: serde_json::Value = match serde_json::from_slice(raw) {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&format!("container spec: {e}"));
                return ptr::null_mut();
            }
        };
        let name = spec.get("name").and_then(|v| v.as_str()).unwrap_or("");
        let color = spec.get("color").and_then(|v| v.as_str()).unwrap_or("");
        let icon = spec.get("icon").and_then(|v| v.as_str());
        match containers::create(h, name, color, icon) {
            Ok(c) => containers_json_out(containers::to_json(&c)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut());
    if !made.is_null() {
        notify::emit("containers");
    }
    made
}

/// Patches one registry row — JSON {"name"?,"color"?,"icon"?}; an
/// "icon" of "" (or null) clears it.  RC_NOT_FOUND on an
/// unregistered id.
///
/// # Safety
/// `id` must be NUL-terminated UTF-8; `json` points to `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_containers_update(
    h: *mut settings::RcSettings,
    id: *const c_char,
    json: *const u8,
    len: usize,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let id = containers_cstr(id, "container id")?;
        let raw = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad container patch pointer".into(),
        })?;
        let spec: serde_json::Value =
            serde_json::from_slice(raw).map_err(|e| error::Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("container patch: {e}"),
            })?;
        let mut patch = containers::Patch::default();
        if let Some(v) = spec.get("name") {
            patch.name =
                Some(v.as_str().ok_or_else(|| error::Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "container patch: name must be a string".into(),
                })?);
        }
        if let Some(v) = spec.get("color") {
            patch.color =
                Some(v.as_str().ok_or_else(|| error::Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "container patch: color must be a string".into(),
                })?);
        }
        if let Some(v) = spec.get("icon") {
            patch.icon =
                Some(if v.is_null() { "" } else {
                    v.as_str().ok_or_else(|| error::Fail {
                        status: RcStatus::InvalidArgument,
                        msg: "container patch: icon must be a string or null"
                            .into(),
                    })?
                });
        }
        containers::update(h, id, &patch)?;
        Ok(())
    });
    if status == RcStatus::Ok {
        notify::emit("containers");
    }
    status
}

/// removeRegistry()'s store half — the <id>/ group plus its "order"
/// entry.  RC_NOT_FOUND on an unregistered id; the on-disk storage
/// trees are the caller's job (rc_containers_namespace gives their
/// paths) — same split as deleteContainer.
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_containers_remove(
    h: *mut settings::RcSettings,
    id: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let id = containers_cstr(id, "container id")?;
        match containers::remove(h, id)? {
            true => Ok(()),
            false => Err(error::Fail {
                status: RcStatus::NotFound,
                msg: "no such container".into(),
            }),
        }
    });
    if status == RcStatus::Ok {
        notify::emit("containers");
    }
    status
}

/// The storage namespace for a registered container id — JSON
/// {"storage_name","storage_path","cache_path","default_data_path",
/// "cookie_partition"} matching ContainerManager's storagePath /
/// cachePathFor / defaultDataPathFor / storageNameFor, plus the
/// RC15 cookie-partition key.  `data_root` is the APP data root
/// (BrowserPaths::dataRoot).  NULL + stashed error on a bad id —
/// the default ("") container has no namespace of its own.
///
/// # Safety
/// All pointers must be NUL-terminated UTF-8; free the result with
/// rc_string_free().
#[no_mangle]
pub unsafe extern "C" fn rc_containers_namespace(
    data_root: *const c_char,
    profile_id: *const c_char,
    container_id: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let root = match containers_cstr(data_root, "data root") {
            Ok(r) => r,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let pid = match containers_cstr(profile_id, "profile id") {
            Ok(p) => p,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let cid = match containers_cstr(container_id, "container id") {
            Ok(c) => c,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match containers::namespace(root, pid, cid) {
            Ok(n) => containers_json_out(containers::namespace_json(&n)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- RC20: update-check policy + manifest parse ----
//
// UpdateChecker's non-network half.  The pure ops (normalize/
// compare, trusted-URL gate, manifest parse, platform pick, channel
// rule) are stateless; the check state (lastCheckMs, snoozeUntilMs,
// skippedVersion, channel) is a typed specialization over the
// settings store — GLOBAL scope by design (the check is
// application-level; "updates" is not an MPROF04 profile group), so
// callers pass a global-bound RcSettings handle (ac_settings(core,
// NULL) reaches the same store).  A profile-bound handle still
// works — it would scope the keys to that persona, which nothing
// else reads.  Mutations emit the "updates" topic.

fn updates_cstr<'a>(p: *const c_char, what: &str) -> error::RcResult<&'a str> {
    unsafe { util::cstr(p) }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("bad {what} pointer"),
    })
}

/// UpdateChecker::normalizedVersion — the normalized tag as a fresh
/// string (rc_string_free), NULL + stashed error on a bad pointer.
///
/// # Safety
/// `version` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_normalize_version(
    version: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match updates_cstr(version, "version") {
            Ok(v) => util::to_c_string(updates::normalize_version(v)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// UpdateChecker::compareVersions — -1/0/+1 on the normalized forms
/// (numeric elementwise when both sides parse, string compare else).
/// -2 with a stashed error on a bad pointer — never a verdict.
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_compare_versions(
    a: *const c_char,
    b: *const c_char,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let a = match updates_cstr(a, "version a") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -2;
            }
        };
        let b = match updates_cstr(b, "version b") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -2;
            }
        };
        updates::compare_versions(a, b)
    }))
    .unwrap_or(-2)
}

/// The AUDIT04 download-URL gate — 1 trusted / 0 refused, -1 with a
/// stashed error on a bad pointer.
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_is_trusted_url(
    url: *const c_char,
    api_url: *const c_char,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let u = match updates_cstr(url, "url") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        let a = match updates_cstr(api_url, "api url") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        updates::is_trusted_download_url(u, a) as i32
    }))
    .unwrap_or(-1)
}

/// The channel rule — 1 when a manifest with `draft`/`prerelease`
/// flags is an offer on `channel` ("stable"/"beta", "" = stable),
/// 0 when it is not, -1 on an unknown channel or bad pointer.
///
/// # Safety
/// `channel` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_channel_accepts(
    channel: *const c_char,
    draft: i32,
    prerelease: i32,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let c = match updates_cstr(channel, "channel") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        match updates::channel_from_str(c) {
            Ok(c) => {
                updates::channel_accepts(c, draft != 0, prerelease != 0) as i32
            }
            Err(e) => {
                error::set_error(&e.msg);
                -1
            }
        }
    }))
    .unwrap_or(-1)
}

/// UpdateChecker::parseReleaseJson — a GitHub release manifest
/// (`payload`/`len`) gated against `api_url`.  Returns JSON
/// {"version","title","page_url","draft","prerelease",
/// "assets":[{"name","url","size"}]} — caller frees with
/// rc_string_free; NULL + stashed error on malformed input.
///
/// # Safety
/// `payload` must point to `len` readable bytes; `api_url` must be
/// NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_parse_manifest(
    payload: *const u8,
    len: usize,
    api_url: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let raw = match unsafe { util::bytes(payload, len) } {
            Some(r) => r,
            None => {
                error::set_error("bad manifest pointer");
                return ptr::null_mut();
            }
        };
        let api = match updates_cstr(api_url, "api url") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match updates::parse_release(raw, api) {
            Ok(r) => containers_json_out(serde_json::json!({
                "version": r.version,
                "title": r.title,
                "page_url": r.page_url,
                "draft": r.draft,
                "prerelease": r.prerelease,
                "assets": r.assets.iter().map(|a| serde_json::json!({
                    "name": a.name,
                    "url": a.url,
                    "size": a.size,
                })).collect::<Vec<_>>(),
            })),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// UpdateChecker::platformAssetsFor — `assets_json` is the
/// "assets" array parse_manifest emits (or any [{"name","url",
/// "size"}] list).  Returns {"candidates":[{...}],"ambiguous":bool}
/// preference-ordered; free with rc_string_free.  NULL + stashed
/// error on bad input.
///
/// # Safety
/// `os` and `os_release_id_like` must be NUL-terminated UTF-8
/// ("" is legal for the id-like); `assets_json` points to `len`
/// bytes of UTF-8 JSON.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_platform_pick(
    os: *const c_char,
    os_release_id_like: *const c_char,
    assets_json: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let os = match updates_cstr(os, "os") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let id_like = match updates_cstr(os_release_id_like, "os-release") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let raw = match unsafe { util::bytes(assets_json, len) } {
            Some(r) => r,
            None => {
                error::set_error("bad assets pointer");
                return ptr::null_mut();
            }
        };
        let spec: serde_json::Value = match serde_json::from_slice(raw) {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&format!("assets json: {e}"));
                return ptr::null_mut();
            }
        };
        let mut assets = Vec::new();
        for row in spec.as_array().cloned().unwrap_or_default() {
            assets.push(updates::ReleaseAsset {
                name: row.get("name").and_then(|x| x.as_str()).unwrap_or("").into(),
                url: row.get("url").and_then(|x| x.as_str()).unwrap_or("").into(),
                size: row.get("size").and_then(|x| x.as_f64()).map(|f| f as i64).unwrap_or(0),
            });
        }
        let pick = updates::platform_pick(os, id_like, &assets);
        containers_json_out(serde_json::json!({
            "candidates": pick.candidates.iter().map(|&i| serde_json::json!({
                "name": assets[i].name,
                "url": assets[i].url,
                "size": assets[i].size,
            })).collect::<Vec<_>>(),
            "ambiguous": pick.ambiguous,
        }))
    }))
    .unwrap_or(ptr::null_mut())
}

/// The persisted check state as JSON {"last_check_ms",
/// "snooze_until_ms","skipped_version","channel"} — the GLOBAL
/// scope's "updates/" keys.  NULL + stashed error on a bad handle;
/// free with rc_string_free.
///
/// # Safety
/// `h` must come from rc_settings_open/ac_settings.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_state(
    h: *const settings::RcSettings,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match updates::read_state(h) {
            Ok(s) => containers_json_out(s.to_json()),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Stamps the last-check time (ms since epoch).
///
/// # Safety
/// `h` must come from rc_settings_open/ac_settings.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_record_check(
    h: *mut settings::RcSettings,
    now_ms: i64,
) -> RcStatus {
    let status = status_of(|| updates::record_check(settings_ref(h)?, now_ms));
    if status == RcStatus::Ok {
        notify::emit("updates");
    }
    status
}

/// Snoozes update notifications until `until_ms` (<= 0 clears).
///
/// # Safety
/// `h` must come from rc_settings_open/ac_settings.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_snooze(
    h: *mut settings::RcSettings,
    until_ms: i64,
) -> RcStatus {
    let status = status_of(|| updates::snooze(settings_ref(h)?, until_ms));
    if status == RcStatus::Ok {
        notify::emit("updates");
    }
    status
}

/// The tag the user dismissed — NULL clears the filter.
///
/// # Safety
/// `version` must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_set_skipped(
    h: *mut settings::RcSettings,
    version: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = settings_ref(h)?;
        let v = if version.is_null() {
            None
        } else {
            Some(updates_cstr(version, "version")?)
        };
        updates::set_skipped(h, v)
    });
    if status == RcStatus::Ok {
        notify::emit("updates");
    }
    status
}

/// Persists the channel — "stable"/"beta" only; an unknown name is
/// RC_ERR_INVALID and changes nothing.
///
/// # Safety
/// `channel` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_set_channel(
    h: *mut settings::RcSettings,
    channel: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        updates::set_channel(
            settings_ref(h)?,
            updates_cstr(channel, "channel")?,
        )
    });
    if status == RcStatus::Ok {
        notify::emit("updates");
    }
    status
}

/// The notify verdict for a remote tag vs the local version at
/// `now_ms`: newer, unskipped, unsnoozed — 1 notify / 0 don't, -1 on
/// bad input.
///
/// # Safety
/// `h` must come from rc_settings_open/ac_settings; the versions
/// must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_updates_should_notify(
    h: *const settings::RcSettings,
    remote_version: *const c_char,
    local_version: *const c_char,
    now_ms: i64,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match settings_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        let remote = match containers_cstr(remote_version, "remote version") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        let local = match containers_cstr(local_version, "local version") {
            Ok(v) => v,
            Err(e) => {
                error::set_error(&e.msg);
                return -1;
            }
        };
        match updates::read_state(h) {
            Ok(s) => updates::should_notify(&s, remote, local, now_ms) as i32,
            Err(e) => {
                error::set_error(&e.msg);
                -1
            }
        }
    }))
    .unwrap_or(-1)
}

// ---- RC21: per-profile side-panel notes store ----
//
// The sidebar's ordered multi-note list — profiles/<id>/notes.json
// in the C++ writer's compact sorted-key JSON shape (see notes.rs
// for the byte-compat details).  rc_notes_open binds a persona's
// file; a NULL/empty profile id — or an empty path on
// rc_notes_open_file — mints the VOLATILE off-the-record store the
// model hands private/tor windows: CRUD in memory, sync() a no-op,
// nothing reaches disk.  Row-addressed ops mirror the model
// contract (clamped insert rows, refused remove ranges, identical-
// value setters reporting "unchanged"); mutations dirty the store
// and emit "notes", sync()/close commit atomically — the AutoSaver
// debounce rides across the boundary unchanged.

fn notes_ref<'a>(h: *const notes::RcNotes) -> error::RcResult<&'a notes::RcNotes> {
    unsafe { h.as_ref() }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: "bad notes handle".into(),
    })
}

/// Opens the notes store for `profile_id` under `data_root` — a
/// slug binds profiles/<id>/notes.json, NULL/empty mints a volatile
/// off-the-record store, a non-slug id is an error.  NULL handle +
/// stashed error on failure; release with rc_notes_close().
///
/// # Safety
/// String pointers must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_open(
    data_root: *const c_char,
    profile_id: *const c_char,
) -> *mut notes::RcNotes {
    catch_unwind(AssertUnwindSafe(|| {
        let root = unsafe { util::cstr(data_root) }.unwrap_or("");
        let id = match unsafe { util::cstr(profile_id) } {
            Some(s) if !s.is_empty() => Some(s),
            _ => None,
        };
        match notes::RcNotes::open(root, id) {
            Ok(h) => Box::into_raw(Box::new(h)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Opens the store bound to an explicit file — the standalone
/// NotesModel(storageFile) lane (tests, per-path callers).
/// NULL/empty mints a volatile store.  NULL + stashed error on
/// failure; release with rc_notes_close().
///
/// # Safety
/// `path` must be NUL-terminated UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_open_file(
    path: *const c_char,
) -> *mut notes::RcNotes {
    catch_unwind(AssertUnwindSafe(|| {
        let path = unsafe { util::cstr(path) }.unwrap_or("");
        match notes::RcNotes::open_file(path) {
            Ok(h) => Box::into_raw(Box::new(h)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Releases the store — a dirty persistent handle flushes first
/// (the model destructor's saveIfNeccessary).  NULL-tolerant.
///
/// # Safety
/// `h` must come from rc_notes_open/rc_notes_open_file/ac_notes,
/// unused after.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_close(h: *mut notes::RcNotes) {
    if h.is_null() {
        return;
    }
    let _ = catch_unwind(AssertUnwindSafe(|| {
        drop(unsafe { Box::from_raw(h) });
    }));
}

/// Row count — -1 on a bad handle.
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_count(h: *const notes::RcNotes) -> i64 {
    catch_unwind(AssertUnwindSafe(|| {
        notes_ref(h).map(|h| h.count()).unwrap_or(-1)
    }))
    .unwrap_or(-1)
}

/// One note as JSON {"body","markdown","title"} — NULL on a bad
/// handle or an out-of-range row (NotFound stashed).  Free with
/// rc_string_free().
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_get(
    h: *const notes::RcNotes,
    row: i64,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match notes_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        match h.get(row) {
            Ok(n) => util::to_c_string(
                serde_json::json!({
                    "body": n.body,
                    "markdown": n.markdown,
                    "title": n.title,
                })
                .to_string(),
            ),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The whole list as the persisted JSON array — the same shape the
/// file carries.  NULL on a bad handle; free with rc_string_free().
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_list(h: *const notes::RcNotes) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match notes_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        util::to_c_string(
            serde_json::Value::Array(
                h.list()
                    .iter()
                    .map(|n| {
                        serde_json::json!({
                            "body": n.body,
                            "markdown": n.markdown,
                            "title": n.title,
                        })
                    })
                    .collect(),
            )
            .to_string(),
        )
    }))
    .unwrap_or(ptr::null_mut())
}

/// insertNote — appends/inserts a {title, body} note at the
/// clamped row (out-of-range positions prepend/append like qBound,
/// never fail) with markdown cleared.  NULL title/body read as "".
/// Returns the new row or -1 on error.
///
/// # Safety
/// `h` must come from an open call; strings must be NUL-terminated
/// UTF-8 or NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_insert(
    h: *mut notes::RcNotes,
    row: i64,
    title: *const c_char,
    body: *const c_char,
) -> i64 {
    let mut out = -1i64;
    let status = status_of(|| {
        let h = notes_ref(h)?;
        let title = unsafe { util::cstr(title) }.unwrap_or("");
        let body = unsafe { util::cstr(body) }.unwrap_or("");
        h.insert(row, title, body).map(|r| out = r)
    });
    if status == RcStatus::Ok {
        notify::emit("notes");
    } else {
        out = -1;
    }
    out
}

/// Shared setter tail: emits "notes" and writes `out_changed`
/// (1 real change, 0 identical-value no-op — the model's setData
/// false return) on success.
fn notes_set_out(
    status: RcStatus,
    changed: bool,
    out_changed: *mut i32,
) -> RcStatus {
    if status == RcStatus::Ok {
        if !out_changed.is_null() {
            unsafe { *out_changed = changed as i32 };
        }
        if changed {
            notify::emit("notes");
        }
    }
    status
}

/// Qt::EditRole — rename the note.  RC_NOT_FOUND out of range;
/// an identical value is Ok + out_changed 0.
///
/// # Safety
/// `h` must come from an open call; `value` must be NUL-terminated
/// UTF-8; `out_changed` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_set_title(
    h: *mut notes::RcNotes,
    row: i64,
    value: *const c_char,
    out_changed: *mut i32,
) -> RcStatus {
    let mut changed = false;
    let status = status_of(|| {
        let h = notes_ref(h)?;
        let value = unsafe { util::cstr(value) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad title pointer".into(),
        })?;
        h.set_title(row, value).map(|c| changed = c)
    });
    notes_set_out(status, changed, out_changed)
}

/// BodyRole — the note's committed body text.  Same contract as
/// rc_notes_set_title.
///
/// # Safety
/// `h` must come from an open call; `value` must be NUL-terminated
/// UTF-8; `out_changed` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_set_body(
    h: *mut notes::RcNotes,
    row: i64,
    value: *const c_char,
    out_changed: *mut i32,
) -> RcStatus {
    let mut changed = false;
    let status = status_of(|| {
        let h = notes_ref(h)?;
        let value = unsafe { util::cstr(value) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad body pointer".into(),
        })?;
        h.set_body(row, value).map(|c| changed = c)
    });
    notes_set_out(status, changed, out_changed)
}

/// MarkdownRole — the per-note render flag.  Same contract as
/// rc_notes_set_title.
///
/// # Safety
/// `h` must come from an open call; `out_changed` may be NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_set_markdown(
    h: *mut notes::RcNotes,
    row: i64,
    on: i32,
    out_changed: *mut i32,
) -> RcStatus {
    let mut changed = false;
    let status = status_of(|| {
        let h = notes_ref(h)?;
        h.set_markdown(row, on != 0).map(|c| changed = c)
    });
    notes_set_out(status, changed, out_changed)
}

/// removeRows — deletes the [row, row+count) span.  RC_INVALID on
/// a refused range (negative, empty or overflowing — the model's
/// false return).
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_remove(
    h: *mut notes::RcNotes,
    row: i64,
    count: i64,
) -> RcStatus {
    let status = status_of(|| {
        let h = notes_ref(h)?;
        h.remove(row, count)
    });
    if status == RcStatus::Ok {
        notify::emit("notes");
    }
    status
}

/// Commit — the AutoSaver-fired save(): persists when dirty, a
/// no-op on a clean or volatile store.
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_sync(h: *mut notes::RcNotes) -> RcStatus {
    status_of(|| {
        let h = notes_ref(h)?;
        h.sync()
    })
}

/// 1 on a file-backed store, 0 on a volatile one or a bad handle —
/// the model's isPersistent().
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_is_persistent(h: *const notes::RcNotes) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        notes_ref(h).map(|h| h.is_persistent() as i32).unwrap_or(0)
    }))
    .unwrap_or(0)
}

/// 1 while uncommitted mutations exist, 0 clean/bad handle.
///
/// # Safety
/// `h` must come from an open call.
#[no_mangle]
pub unsafe extern "C" fn rc_notes_is_dirty(h: *const notes::RcNotes) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        notes_ref(h).map(|h| h.is_dirty() as i32).unwrap_or(0)
    }))
    .unwrap_or(0)
}

// ---- RC03: profile registry ----
//
// The persona list — <data root>/profiles.json, byte-compatible with
// the MPROF01 writer both C++ implementations (ProfileRegistry,
// ProfileStore) share.  The data root binds at rc_profiles_open();
// list/create/rename/avatar/delete cover the registry rows and the
// path accessors mirror BrowserPaths' per-profile layout so callers
// never invent a second one.  The active id is per-handle process
// state (the ambient persona), never persisted.  Mutations emit the
// "profiles" topic.  Vault markers and live-window teardown stay
// C++-side — the delete here is the store primitive the adapter
// wraps.

fn profiles_ref<'a>(
    h: *const profiles::RcProfiles,
) -> error::RcResult<&'a profiles::RcProfiles> {
    unsafe { h.as_ref() }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: "bad profiles handle".into(),
    })
}

/// Opens the registry under `data_root` — a real path, not the
/// process-global data dir.  `default_name_or_null` seeds the
/// display name of the synthesized "default" row (the caller's
/// QSettings "profile/name" value or its localized fallback).
/// Returns an opaque handle or NULL (error stashed).
///
/// # Safety
/// String pointers must be NUL-terminated UTF-8; the returned handle
/// is released with rc_profiles_close().
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_open(
    data_root: *const c_char,
    default_name_or_null: *const c_char,
) -> *mut profiles::RcProfiles {
    catch_unwind(AssertUnwindSafe(|| {
        let root = match unsafe { util::cstr(data_root) } {
            Some(r) => r,
            None => {
                error::set_error("bad data root pointer");
                return ptr::null_mut();
            }
        };
        let seed = if default_name_or_null.is_null() {
            None
        } else {
            match unsafe { util::cstr(default_name_or_null) } {
                Some(s) => Some(s),
                None => {
                    error::set_error("bad default name pointer");
                    return ptr::null_mut();
                }
            }
        };
        match profiles::RcProfiles::open(root, seed) {
            Ok(h) => Box::into_raw(Box::new(h)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Releases a handle from rc_profiles_open().  NULL-tolerant.
///
/// # Safety
/// `h` must come from rc_profiles_open and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_close(h: *mut profiles::RcProfiles) {
    if !h.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(unsafe { Box::from_raw(h) });
        }));
    }
}

/// Re-reads profiles.json from disk (the ProfileStore::reload seam).
///
/// # Safety
/// `h` must come from rc_profiles_open.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_reload(
    h: *mut profiles::RcProfiles,
) -> RcStatus {
    status_of(|| profiles_ref(h)?.reload())
}

/// Every profile as a JSON array of
/// {id,name,avatar,createdAt} objects — the row order the file holds.
/// rc_string_free the result; NULL on error.
///
/// # Safety
/// `h` must come from rc_profiles_open.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_list(
    h: *mut profiles::RcProfiles,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match profiles_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let rows: Vec<serde_json::Value> = h
            .list()
            .iter()
            .map(|p| {
                serde_json::json!({
                    "id": p.id,
                    "name": p.name,
                    "avatar": p.avatar,
                    "createdAt": p.created_at,
                })
            })
            .collect();
        util::to_c_string(
            serde_json::to_string(&rows).unwrap_or_else(|_| "[]".into()),
        )
    }))
    .unwrap_or(ptr::null_mut())
}

/// One profile as a JSON {id,name,avatar,createdAt} object; NULL on
/// error (RC_NOT_FOUND stashed for an unknown id).
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_get(
    h: *mut profiles::RcProfiles,
    id: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let h = match profiles_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let id = match unsafe { util::cstr(id) } {
            Some(i) => i,
            None => {
                error::set_error("bad id pointer");
                return ptr::null_mut();
            }
        };
        match h.get(id) {
            Some(p) => util::to_c_string(
                serde_json::json!({
                    "id": p.id,
                    "name": p.name,
                    "avatar": p.avatar,
                    "createdAt": p.created_at,
                })
                .to_string(),
            ),
            None => {
                error::set_error("no such profile");
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Creates a profile from `display_name`: slugified unique id,
/// profiles/<id>/ created, registry row appended + persisted.
/// Returns the new id (rc_string_free); NULL on failure.
///
/// # Safety
/// `display_name` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_create(
    h: *mut profiles::RcProfiles,
    display_name: *const c_char,
) -> *mut c_char {
    let out = catch_unwind(AssertUnwindSafe(|| {
        let h = match profiles_ref(h) {
            Ok(h) => h,
            Err(e) => {
                error::set_error(&e.msg);
                return ptr::null_mut();
            }
        };
        let name = match unsafe { util::cstr(display_name) } {
            Some(n) => n,
            None => {
                error::set_error("bad name pointer");
                return ptr::null_mut();
            }
        };
        match h.create(name) {
            Ok(id) => util::to_c_string(id),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut());
    if !out.is_null() {
        notify::emit("profiles");
    }
    out
}

/// Renames a profile; persists and emits "profiles".  A blank name
/// resolves to "Profile" like ProfileStore::renameProfile.
///
/// # Safety
/// Both strings must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_rename(
    h: *mut profiles::RcProfiles,
    id: *const c_char,
    name: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = profiles_ref(h)?;
        let id = unsafe { util::cstr(id) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad id pointer".into(),
        })?;
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        h.rename(id, name)
    });
    if status == RcStatus::Ok {
        notify::emit("profiles");
    }
    status
}

/// Points a profile's avatar field at a basename inside
/// profiles/<id>/ (the PNG itself is the Qt side's to write);
/// NULL/empty clears the field.  Separators and dot segments are
/// refused.  Persists and emits "profiles".
///
/// # Safety
/// `id` must be NUL-terminated UTF-8; `name_or_null` likewise or NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_set_avatar(
    h: *mut profiles::RcProfiles,
    id: *const c_char,
    name_or_null: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = profiles_ref(h)?;
        let id = unsafe { util::cstr(id) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad id pointer".into(),
        })?;
        let name = if name_or_null.is_null() {
            None
        } else {
            Some(unsafe { util::cstr(name_or_null) }.ok_or_else(|| {
                error::Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "bad avatar pointer".into(),
                }
            })?)
        };
        h.set_avatar(id, name)
    });
    if status == RcStatus::Ok {
        notify::emit("profiles");
    }
    status
}

/// Deletes a profile: removes the registry row and profiles/<id>/
/// recursively.  "default" and non-slug ids are refused
/// (RC_INVALID_ARGUMENT), unknown ids RC_NOT_FOUND.  Emits
/// "profiles".
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_delete(
    h: *mut profiles::RcProfiles,
    id: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let h = profiles_ref(h)?;
        let id = unsafe { util::cstr(id) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad id pointer".into(),
        })?;
        h.delete(id)
    });
    if status == RcStatus::Ok {
        notify::emit("profiles");
    }
    status
}

/// The ambient persona id — per-handle process state mirroring
/// BrowserPaths::activeProfileId ("default" before any push).
/// rc_string_free the result.
///
/// # Safety
/// `h` must come from rc_profiles_open.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_active(
    h: *mut profiles::RcProfiles,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match profiles_ref(h) {
            Ok(h) => util::to_c_string(h.active_id()),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Pushes the ambient persona — a non-slug id resolves to "default"
/// like BrowserPaths::setActiveProfileId.  Process state only: not
/// persisted, no change topic.
///
/// # Safety
/// `id` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_set_active(
    h: *mut profiles::RcProfiles,
    id: *const c_char,
) -> RcStatus {
    status_of(|| {
        let h = profiles_ref(h)?;
        let id = unsafe { util::cstr(id) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad id pointer".into(),
        })?;
        h.set_active(id);
        Ok(())
    })
}

macro_rules! profiles_path_getter {
    ($name:ident, $call:ident) => {
        /// # Safety
        /// `id` must be NULL (resolves to "default") or
        /// NUL-terminated UTF-8.
        #[no_mangle]
        pub unsafe extern "C" fn $name(
            h: *mut profiles::RcProfiles,
            id: *const c_char,
        ) -> *mut c_char {
            catch_unwind(AssertUnwindSafe(|| {
                let h = match profiles_ref(h) {
                    Ok(h) => h,
                    Err(e) => {
                        error::set_error(&e.msg);
                        return ptr::null_mut();
                    }
                };
                let id = unsafe { util::cstr(id) }.unwrap_or("");
                util::to_c_string(h.$call(id))
            }))
            .unwrap_or(ptr::null_mut())
        }
    };
}

// <root>/profiles/<id> — BrowserPaths::profileDataDir.  A non-slug
// id resolves to "default" rather than escaping the tree.
// rc_string_free the result.
profiles_path_getter!(rc_profiles_dir, profile_dir);

// <root>/profiles/<id>/cache — the profile's disposable lane.
// rc_string_free the result.
profiles_path_getter!(rc_profiles_cache_dir, cache_dir);

// <root>/profiles/<id>/QtWebEngine — BrowserPaths::engineStorageDir.
// rc_string_free the result.
profiles_path_getter!(rc_profiles_engine_storage_dir, engine_storage_dir);

// <root>/profiles/<id>/cache/QtWebEngine —
// BrowserPaths::engineCacheDir.  rc_string_free the result.
profiles_path_getter!(rc_profiles_engine_cache_dir, engine_cache_dir);

// The dir an rc_settings profile scope anchors at — the profile
// data dir itself.  rc_string_free the result.
profiles_path_getter!(rc_profiles_settings_dir, settings_dir);

// profiles/<id>/<avatar> — the row's avatar field joined under the
// profile dir, falling back to profile-avatar.png when unset.
// rc_string_free the result.
profiles_path_getter!(rc_profiles_avatar_path, avatar_path);

/// The registry file this handle owns — <root>/profiles.json.
/// rc_string_free the result.
///
/// # Safety
/// `h` must come from rc_profiles_open.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_file(
    h: *mut profiles::RcProfiles,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match profiles_ref(h) {
            Ok(h) => util::to_c_string(h.registry_path()),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Display name -> filesystem slug, mirroring
/// ProfileStore::slugify.  rc_string_free the result.
///
/// # Safety
/// `display_name` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_profiles_slugify(
    display_name: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match unsafe { util::cstr(display_name) } {
            Some(n) => util::to_c_string(profiles::slugify(n)),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- OSE01: OpenSearch engine registry ----
//
// The canonical search-engine store — searchengines.json under the
// data dir holds engine records (the rc_opensearch_parse field map
// plus "keywords"), the display order and the removed-bundled
// blocklist.  The vendored bundled descriptors seed through
// rc_ose_seed_bundled / rc_ose_restore_bundled; the Qt manager
// hydrates OpenSearchEngine objects from these records and every
// mutation writes through.  Mutations emit the "searchengines"
// change topic.

/// 1 when the store file exists — the legacy-import gate.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_store_present() -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        i32::from(opensearch::store_present())
    }))
    .unwrap_or(0)
}

/// Engine names in display order — JSON array; rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_list() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match opensearch::list_json() {
        Ok(json) => util::to_c_string(json),
        Err(e) => {
            error::set_error(&e.msg);
            ptr::null_mut()
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// One engine record as JSON — NULL when absent; rc_string_free().
///
/// # Safety
/// `name` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_get(name: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(name) = (unsafe { util::cstr(name) }) else {
            error::set_error("bad name pointer");
            return ptr::null_mut();
        };
        match opensearch::get_json(name) {
            Ok(Some(json)) => util::to_c_string(json),
            Ok(None) => ptr::null_mut(),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Upserts an engine record (JSON — name + search template required;
/// "keywords" replaces the bindings when present, preserved when
/// absent).  Persists and notifies on change.
///
/// # Safety
/// `json` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_put(json: *const u8, len: usize) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        changed = opensearch::put_json(data)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Parses an OpenSearch descriptor (bounded, DTD-refused) and upserts
/// it — the .xml import path; keywords survive a replace.
///
/// # Safety
/// `xml` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_import(xml: *const u8, len: usize) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let data = unsafe { util::bytes(xml, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad descriptor pointer".into(),
        })?;
        changed = opensearch::import_descriptor(data)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Drops an engine by name; RC_OK whether or not it existed.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_remove(name: *const c_char) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        changed = opensearch::remove(name)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Re-keys an engine keeping its order slot and keywords —
/// RC_NOT_FOUND on a missing source, RC_INVALID_ARGUMENT on an
/// empty/taken target.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_rename(
    old: *const c_char,
    new: *const c_char,
) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let old = unsafe { util::cstr(old) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad old-name pointer".into(),
        })?;
        let new = unsafe { util::cstr(new) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad new-name pointer".into(),
        })?;
        changed = opensearch::rename(old, new)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Replaces the display order with a JSON name array.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_reorder(json: *const u8, len: usize) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        changed = opensearch::reorder(data)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Replaces an engine's keyword bindings (JSON string array);
/// RC_NOT_FOUND when the engine does not exist.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_set_keywords(
    name: *const c_char,
    json: *const u8,
    len: usize,
) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        let data = unsafe { util::bytes(json, len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad json pointer".into(),
        })?;
        changed = opensearch::set_keywords(name, data)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// The engine name a keyword resolves to — NULL when unbound;
/// rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_engine_for_keyword(
    keyword: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(keyword) = (unsafe { util::cstr(keyword) }) else {
            return ptr::null_mut();
        };
        match opensearch::engine_for_keyword(keyword) {
            Ok(Some(name)) => util::to_c_string(name),
            _ => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Every bound keyword — JSON array; rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_keywords() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match opensearch::keywords_json() {
        Ok(json) => util::to_c_string(json),
        Err(e) => {
            error::set_error(&e.msg);
            ptr::null_mut()
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The removed-bundled blocklist — JSON array; rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_removed_bundled() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match opensearch::removed_bundled_json() {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Adds `name` to the bundled-removal blocklist.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_block_bundled(name: *const c_char) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        changed = opensearch::block_bundled(name)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Drops `name` from the blocklist.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_unblock_bundled(name: *const c_char) -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        let name = unsafe { util::cstr(name) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad name pointer".into(),
        })?;
        changed = opensearch::unblock_bundled(name)?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// The vendored bundled engine names — JSON array; rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_bundled_names() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        util::to_c_string(opensearch::bundled_names_json())
    }))
    .unwrap_or(ptr::null_mut())
}

/// Inserts every bundled engine that is absent and not blocked.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_seed_bundled() -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        changed = opensearch::seed_bundled()?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Clears the blocklist and re-adds every bundled engine, replacing
/// same-named records but keeping their keyword bindings — the
/// "restore defaults" semantic.
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_restore_bundled() -> RcStatus {
    let mut changed = false;
    let st = status_of(|| {
        changed = opensearch::restore_bundled()?;
        Ok(())
    });
    if st == RcStatus::Ok && changed {
        notify::emit("searchengines");
    }
    st
}

/// Wipes the registry and removes searchengines.json — the
/// test-suite reset seam (re-arms the Qt legacy-migration gate).
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_reset() -> RcStatus {
    status_of(opensearch::reset)
}

/// The buildUrl()/parseTemplate() expansion: spec JSON
/// {"template","method","params":[[k,v],...],"term","language",
/// "source"} -> the encoded URL string; NULL on a bad spec.
/// rc_string_free().
///
/// # Safety
/// `spec` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_expand(spec: *const u8, len: usize) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(data) = (unsafe { util::bytes(spec, len) }) else {
            return ptr::null_mut();
        };
        match opensearch::expand_json(data) {
            Some(url) => util::to_c_string(url),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Engine + slot kind ("search"|"suggestions"|"image") + term ->
/// expanded URL; NULL when the engine or slot is absent.
///
/// # Safety
/// All pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_url(
    name: *const c_char,
    kind: *const c_char,
    term: *const c_char,
    language: *const c_char,
    source: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(name) = (unsafe { util::cstr(name) }) else {
            return ptr::null_mut();
        };
        let kind = unsafe { util::cstr(kind) }.unwrap_or("");
        let term = unsafe { util::cstr(term) }.unwrap_or("");
        let language = unsafe { util::cstr(language) }.unwrap_or("");
        let source = unsafe { util::cstr(source) }.unwrap_or("");
        match opensearch::engine_url(name, kind, term, language, source) {
            Ok(Some(url)) => util::to_c_string(url),
            _ => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// keyword + terms -> the bound engine's search URL; NULL when the
/// keyword is unbound.
///
/// # Safety
/// All pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ose_keyword_url(
    keyword: *const c_char,
    term: *const c_char,
    language: *const c_char,
    source: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(keyword) = (unsafe { util::cstr(keyword) }) else {
            return ptr::null_mut();
        };
        let term = unsafe { util::cstr(term) }.unwrap_or("");
        let language = unsafe { util::cstr(language) }.unwrap_or("");
        let source = unsafe { util::cstr(source) }.unwrap_or("");
        match opensearch::keyword_url(keyword, term, language, source) {
            Ok(Some(url)) => util::to_c_string(url),
            _ => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- UAG01: user-agent construction + per-site spoof table ----
//
// The pure UA string logic — the effective-UA decision (override or
// de-badged/bumped factory UA), the Sec-CH-UA brand version a UA
// implies, and the useragents.xml preset parse — plus the per-site
// spoof table stored as the "uaspoof" kind inside the sitedecisions
// store.  The clearnet interceptor applies a spoof hit as the
// User-Agent request header; the tor interceptor never consults the
// table (uniform UA is the anonymity pin).  Writes emit the
// "sitedecisions" change topic — the table IS a site decision.

/// Builds the effective UA from a JSON context
/// `{"factory_ua","presented_major","override"}` — a non-empty
/// override wins verbatim; otherwise the factory UA loses its
/// "QtWebEngine/<ver>" token and its Chrome/<major> is bumped to the
/// presented milestone.  NULL on a bad context; rc_string_free.
///
/// # Safety
/// `context` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_build(
    context: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(ctx) = (unsafe { util::bytes(context, len) }) else {
            error::set_error("bad ua context pointer");
            return ptr::null_mut();
        };
        match useragent::build_ua(ctx) {
            Ok(ua) => util::to_c_string(ua),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The Sec-CH-UA full version a UA implies — the UA's own Chrome
/// major over the engine version's build tail, empty when the UA
/// does not claim Chrome.  Caller frees with rc_string_free().
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_brand_version(
    ua: *const c_char,
    engine_version: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let ua = unsafe { util::cstr(ua) }.unwrap_or("");
        let version = unsafe { util::cstr(engine_version) }.unwrap_or("");
        util::to_c_string(useragent::brand_version(ua, version))
    }))
    .unwrap_or(ptr::null_mut())
}

/// Parses a useragentswitcher document into a JSON array preserving
/// document order:
/// `[{"type":"separator"},{"type":"agent","description":..,"useragent":..}]`.
/// A malformed tail keeps the entries that parsed (the Qt reader's
/// log-and-continue semantic).  Caller frees with rc_string_free().
///
/// # Safety
/// `xml` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_presets(
    xml: *const u8,
    len: usize,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(data) = (unsafe { util::bytes(xml, len) }) else {
            error::set_error("bad ua presets pointer");
            return ptr::null_mut();
        };
        match useragent::presets(data) {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// The stored per-site UA override governing `host` (longest-suffix
/// match), or NULL when nothing applies.  rc_string_free.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_spoof(host: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(host) = (unsafe { util::cstr(host) }) else {
            return ptr::null_mut();
        };
        match useragent::spoof_for(host) {
            Ok(Some(ua)) => util::to_c_string(ua),
            _ => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Records (or overwrites) the per-site UA override for `host`.
/// Values carrying control bytes are refused — a spoof lands
/// verbatim in a User-Agent header, so CR/LF would be an injection.
///
/// # Safety
/// Both pointers must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_spoof_set(
    host: *const c_char,
    ua: *const c_char,
) -> RcStatus {
    let status = status_of(|| {
        let host = unsafe { util::cstr(host) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad host pointer".into(),
        })?;
        let ua = unsafe { util::cstr(ua) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad ua pointer".into(),
        })?;
        useragent::spoof_set(host, ua).map(|_| ())
    });
    if status == RcStatus::Ok {
        notify::emit("sitedecisions");
    }
    status
}

/// Drops the per-site override for `host`; RC_OK whether or not it
/// existed.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_spoof_remove(host: *const c_char) -> RcStatus {
    let status = status_of(|| {
        let host = unsafe { util::cstr(host) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad host pointer".into(),
        })?;
        useragent::spoof_remove(host).map(|_| ())
    });
    if status == RcStatus::Ok {
        notify::emit("sitedecisions");
    }
    status
}

/// Every spoof row as `{"host":"ua"}` JSON — caller frees with
/// rc_string_free().
///
/// # Safety
///
/// Exported C ABI — every pointer argument is untrusted: invalid
/// pointers/sizes degrade to an error return rather than UB, but
/// callers must still pass pointers valid for the documented
/// access (NUL-terminated strings, `len`-sized buffers, writable
/// out-params).  Returned `char*`/`RcBuffer` results are
/// caller-owned — free with `rc_string_free`/`rc_buffer_free`.
#[no_mangle]
pub unsafe extern "C" fn rc_ua_spoof_list() -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| match useragent::spoof_list() {
        Ok(json) => util::to_c_string(json),
        Err(e) => {
            error::set_error(&e.msg);
            ptr::null_mut()
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- clear-private-data decision engine (CLR01) -------------------------
//
// The "which stores get wiped" decision as a pure function — see
// clearplan.rs for the op vocabulary and the two scope rules.  The Qt
// shell executes the ordered list per-store; the core executes
// nothing itself.

/// rc_clearplan(period, categories, otr_active) -> JSON plan:
///   {"scope":"persistent"|"otr", "period":name,
///    "since_secs":n, "ops":[{...}]}
/// `period` is "all"/"everything"/"" for a whole-store wipe or
/// "hour"/"day"/"week" for a recent window (the "last-"-prefixed
/// spellings accepted); NULL or an unrecognized name -> NULL +
/// RC_INVALID_ARGUMENT-class error.  `categories` is the OR of the
/// RC_CLEAR_* bits; unknown bits are ignored.  `otr_active` non-zero
/// scopes the plan to the off-the-record context — the result then
/// contains no op that touches a persistent store.  Free the result
/// with rc_string_free().
///
/// # Safety
/// `period` must be NUL-terminated UTF-8, or NULL.
#[no_mangle]
pub unsafe extern "C" fn rc_clearplan(
    period: *const c_char,
    categories: u32,
    otr_active: i32,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let p = match unsafe { util::cstr(period) } {
            Some(s) => s,
            None => {
                error::set_error("bad period pointer");
                return ptr::null_mut();
            }
        };
        match clearplan::plan_json(p, categories, otr_active != 0) {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

// ---- profile vault (PVAULT01) ---------------------------------------------
//
// Password-sealed, chunked, streamable archives of a profile dir —
// see profilevault.rs for the ARV1 wire format, the two-tier key
// schedule and the no-partial-unpack guarantee.  Unlike the store
// modules these calls are stateless: they take explicit paths, touch
// no data dir and may run on any thread.  Progress rides an optional
// (done, total) byte callback fired on the calling thread.

/// Seal `dir` into `out_path` under `pass`.  `algo` is 0x01
/// (AES-256-GCM + HMAC-SHA-512, the default) or 0x02
/// (Threefish-1024-XEX + Skein-MAC).  The output lands atomically
/// (temp + rename, 0600) or not at all.
///
/// # Safety
/// `dir`/`out_path` must be NUL-terminated UTF-8; `pass` must point
/// to `pass_len` readable bytes.  `cb`/`userdata` are called back on
/// this thread — queue onto the GUI thread, never touch widgets.
#[no_mangle]
pub unsafe extern "C" fn rv_seal(
    dir: *const c_char,
    pass: *const u8,
    pass_len: usize,
    algo: u8,
    out_path: *const c_char,
    cb: Option<profilevault::ProgressCallback>,
    userdata: *mut c_void,
) -> RcStatus {
    status_of(|| {
        let dir = unsafe { util::cstr(dir) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad dir pointer".into(),
        })?;
        let out = unsafe { util::cstr(out_path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad out path pointer".into(),
        })?;
        let pass = unsafe { util::bytes(pass, pass_len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad password pointer".into(),
        })?;
        let pass = zeroize::Zeroizing::new(pass.to_vec());
        let mut progress = profilevault::Progress::new(cb, userdata);
        profilevault::seal_dir(
            std::path::Path::new(dir),
            &pass,
            algo,
            std::path::Path::new(out),
            &mut progress,
        )
    })
}

/// Verify + unpack `blob_path` into `out_dir` — two passes, zero
/// partial output on failure.  RC_WRONG_PASSWORD when the password is
/// wrong, RC_TAMPERED when authenticity fails, RC_CORRUPT when the
/// file is malformed, RC_UNSUPPORTED_ALGO for an unknown algo_id.
///
/// # Safety
/// Same contract as rv_seal.
#[no_mangle]
pub unsafe extern "C" fn rv_unseal(
    blob_path: *const c_char,
    pass: *const u8,
    pass_len: usize,
    out_dir: *const c_char,
    cb: Option<profilevault::ProgressCallback>,
    userdata: *mut c_void,
) -> RcStatus {
    status_of(|| {
        let blob = unsafe { util::cstr(blob_path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad vault path pointer".into(),
        })?;
        let out = unsafe { util::cstr(out_dir) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad out dir pointer".into(),
        })?;
        let pass = unsafe { util::bytes(pass, pass_len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad password pointer".into(),
        })?;
        let pass = zeroize::Zeroizing::new(pass.to_vec());
        let mut progress = profilevault::Progress::new(cb, userdata);
        profilevault::unseal_file(
            std::path::Path::new(blob),
            &pass,
            std::path::Path::new(out),
            &mut progress,
        )
    })
}

/// Password + authenticity check without unpacking — the fast leg a
/// UI runs to validate a vault before offering a restore.
///
/// # Safety
/// `blob_path` must be NUL-terminated UTF-8; `pass` must point to
/// `pass_len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rv_verify(
    blob_path: *const c_char,
    pass: *const u8,
    pass_len: usize,
) -> RcStatus {
    status_of(|| {
        let blob = unsafe { util::cstr(blob_path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad vault path pointer".into(),
        })?;
        let pass = unsafe { util::bytes(pass, pass_len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad password pointer".into(),
        })?;
        let pass = zeroize::Zeroizing::new(pass.to_vec());
        let mut progress = profilevault::Progress::new(None, ptr::null_mut());
        profilevault::verify_file(std::path::Path::new(blob), &pass, &mut progress)
    })
}

/// Unauthenticated header summary for UI display — JSON:
///   {"format":"ARV1","algo":N,"algo_name":...,"kdf":"argon2id",
///    "memory_kib","passes","lanes","chunk_size","chunk_count",
///    "payload_bytes","authenticated":false}
/// Fields are unverified until a password-bearing call succeeds —
/// display them as claims, not facts.  NULL on error; free the result
/// with rc_string_free().
///
/// # Safety
/// `blob_path` must be NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rv_inspect(blob_path: *const c_char) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let blob = match unsafe { util::cstr(blob_path) } {
            Some(s) => s,
            None => {
                error::set_error("bad vault path pointer");
                return ptr::null_mut();
            }
        };
        match profilevault::inspect_file(std::path::Path::new(blob)) {
            Ok(json) => util::to_c_string(json),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Re-wrap the vault key under `new_pass` — the cheap password change
/// (~217 bytes rewritten; the payload never moves).  RC_WRONG_PASSWORD
/// when `old_pass` does not open the vault.
///
/// # Safety
/// `blob_path` must be NUL-terminated UTF-8; both passwords must point
/// to their `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rv_rewrap(
    blob_path: *const c_char,
    old_pass: *const u8,
    old_len: usize,
    new_pass: *const u8,
    new_len: usize,
) -> RcStatus {
    status_of(|| {
        let blob = unsafe { util::cstr(blob_path) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad vault path pointer".into(),
        })?;
        let old = unsafe { util::bytes(old_pass, old_len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad old password pointer".into(),
        })?;
        let new = unsafe { util::bytes(new_pass, new_len) }.ok_or_else(|| error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "bad new password pointer".into(),
        })?;
        let old = zeroize::Zeroizing::new(old.to_vec());
        let new = zeroize::Zeroizing::new(new.to_vec());
        profilevault::rewrap_file(std::path::Path::new(blob), &old, &new)
    })
}

// ---- tor control client (RC14) --------------------------------------
//
// Tor control-protocol port of torcontrol.cpp: loopback-only connect,
// AUTHENTICATE (32-byte cookie hex token or quoted password), and the
// serialized command queue — one command in flight, every submission
// completing exactly once via take_reply (final code, or -1 when the
// link dies first).  Async 650 events and protocol complaints drain
// through take_event/take_error; each push emits "torctl" so the Qt
// bridge can queue a drain.  rc_tor_open connects synchronously with
// a bounded timeout — a returned handle is always live; a refused or
// non-loopback target is NULL + rc_last_error_message().
//
// No-leak contract: the host must be a loopback literal or
// "localhost" (resolved and re-verified), and AUTHENTICATE payloads
// are assembled in zeroizing buffers that wipe on queue flush/drop.

fn tor_ref<'a>(
    h: *const torctl::RcTorCtl,
) -> error::RcResult<&'a torctl::RcTorCtl> {
    unsafe { h.as_ref() }.ok_or_else(|| error::Fail {
        status: RcStatus::InvalidArgument,
        msg: "bad tor handle".into(),
    })
}

/// Sends `command` (no CRLF — must be a single line) and returns the
/// sequence number take_reply reports it under, or -1 with
/// rc_last_error_message() set.  Shared tail for every typed helper.
unsafe fn tor_send_seq(
    h: *const torctl::RcTorCtl,
    f: impl FnOnce(&torctl::RcTorCtl) -> error::RcResult<u64>,
) -> i64 {
    match catch_unwind(AssertUnwindSafe(|| match tor_ref(h) {
        Ok(h) => f(h),
        Err(e) => Err(e),
    })) {
        Ok(Ok(seq)) => {
            error::clear_error();
            seq as i64
        }
        Ok(Err(e)) => {
            error::set_error(&e.msg);
            -1
        }
        Err(_) => {
            error::set_error("panic inside rustcore");
            -1
        }
    }
}

/// Connects to a tor control port at host:port — loopback only.
/// Returns an opaque handle (rc_tor_close to release) or NULL with
/// rc_last_error_message() set.
///
/// # Safety
/// `host` must be NUL-terminated UTF-8 (an IP literal or "localhost").
#[no_mangle]
pub unsafe extern "C" fn rc_tor_open(
    host: *const c_char,
    port: u16,
) -> *mut torctl::RcTorCtl {
    catch_unwind(AssertUnwindSafe(|| {
        let host = match unsafe { util::cstr(host) } {
            Some(h) => h,
            None => {
                error::set_error("bad host pointer");
                return ptr::null_mut();
            }
        };
        match torctl::RcTorCtl::open(host, port) {
            Ok(h) => Box::into_raw(Box::new(h)),
            Err(e) => {
                error::set_error(&e.msg);
                ptr::null_mut()
            }
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Severs the link but keeps the handle valid for draining: the
/// in-flight/queued commands complete with code -1 and the state
/// reads RC_TOR_CLOSED — the disconnectFromHost() spelling.
///
/// # Safety
/// `h` must be a live rc_tor_open handle.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_disconnect(h: *mut torctl::RcTorCtl) {
    let _ = catch_unwind(AssertUnwindSafe(|| {
        if let Ok(h) = tor_ref(h) {
            h.close();
        }
    }));
}

/// Severs the link and releases the handle.  Outstanding commands
/// complete with code -1 before the close lands; NULL-tolerant.
///
/// # Safety
/// `h` must come from rc_tor_open and not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_close(h: *mut torctl::RcTorCtl) {
    if !h.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(unsafe { Box::from_raw(h) });
        }));
    }
}

/// Link state: 1 connected, 2 closed, 0 for a bad handle.
///
/// # Safety
/// `h` must be a live rc_tor_open handle.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_state(h: *const torctl::RcTorCtl) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        tor_ref(h).map(|h| h.state()).unwrap_or(0)
    }))
    .unwrap_or(0)
}

/// Connection error text ("" for a clean close or open link).
/// Free with rc_string_free().
///
/// # Safety
/// `h` must be a live rc_tor_open handle.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_error(h: *const torctl::RcTorCtl) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match tor_ref(h) {
            Ok(h) => util::to_c_string(h.error_string()),
            Err(_) => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Queues one control-protocol command line (no CRLF — a payload with
/// a newline is rejected, never split).  Returns the reply's sequence
/// number or -1.
///
/// # Safety
/// `h` live handle; `command` NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_send(
    h: *const torctl::RcTorCtl,
    command: *const c_char,
) -> i64 {
    let cmd = match unsafe { util::cstr(command) } {
        Some(c) => c.to_owned(),
        None => {
            error::set_error("bad command pointer");
            return -1;
        }
    };
    unsafe { tor_send_seq(h, move |h| h.send(&cmd)) }
}

/// AUTHENTICATE with the 32-byte control_auth_cookie (sent as hex).
/// Bad length is an error before anything hits the wire.
///
/// # Safety
/// `cookie` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_auth_cookie(
    h: *const torctl::RcTorCtl,
    cookie: *const u8,
    len: usize,
) -> i64 {
    let bytes = unsafe { util::bytes(cookie, len) }.map(|b| b.to_vec());
    match bytes {
        Some(c) => unsafe { tor_send_seq(h, move |h| h.auth_cookie(&c)) },
        None => {
            error::set_error("bad cookie pointer");
            -1
        }
    }
}

/// AUTHENTICATE with the cookie file at `path` — the secret is read
/// inside the library and never crosses the ABI as a buffer.
///
/// # Safety
/// `h` live handle; `path` NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_auth_cookie_file(
    h: *const torctl::RcTorCtl,
    path: *const c_char,
) -> i64 {
    let path = match unsafe { util::cstr(path) } {
        Some(p) => p.to_owned(),
        None => {
            error::set_error("bad cookie path pointer");
            return -1;
        }
    };
    unsafe { tor_send_seq(h, move |h| h.auth_cookie_file(&path)) }
}

/// AUTHENTICATE with a password (HashedControlPassword mode), sent as
/// an escaped QuotedString.
///
/// # Safety
/// `pass` must point to `len` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_auth_password(
    h: *const torctl::RcTorCtl,
    pass: *const u8,
    len: usize,
) -> i64 {
    let bytes = unsafe { util::bytes(pass, len) }.map(|b| b.to_vec());
    match bytes {
        Some(p) => unsafe { tor_send_seq(h, move |h| h.auth_password(&p)) },
        None => {
            error::set_error("bad password pointer");
            -1
        }
    }
}

/// GETINFO <key> (a space-separated key list is legal per spec).
///
/// # Safety
/// `h` live handle; `key` NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_getinfo(
    h: *const torctl::RcTorCtl,
    key: *const c_char,
) -> i64 {
    let key = match unsafe { util::cstr(key) } {
        Some(k) => k.to_owned(),
        None => {
            error::set_error("bad getinfo key pointer");
            return -1;
        }
    };
    unsafe { tor_send_seq(h, move |h| h.getinfo(&key)) }
}

/// SETEVENTS <list> — space-separated names ("CIRC STREAM").
///
/// # Safety
/// `h` live handle; `list` NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_setevents(
    h: *const torctl::RcTorCtl,
    list: *const c_char,
) -> i64 {
    let list = match unsafe { util::cstr(list) } {
        Some(l) => l.to_owned(),
        None => {
            error::set_error("bad event list pointer");
            return -1;
        }
    };
    unsafe { tor_send_seq(h, move |h| h.setevents(&list)) }
}

/// TAKEOWNERSHIP — tor exits when this connection closes.
///
/// # Safety
/// `h` must be a live rc_tor_open handle.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_takeownership(h: *const torctl::RcTorCtl) -> i64 {
    unsafe { tor_send_seq(h, |h| h.takeownership()) }
}

/// SIGNAL <name> — NEWNYM, SHUTDOWN, RELOAD, …
///
/// # Safety
/// `h` live handle; `name` NUL-terminated UTF-8.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_signal(
    h: *const torctl::RcTorCtl,
    name: *const c_char,
) -> i64 {
    let name = match unsafe { util::cstr(name) } {
        Some(n) => n.to_owned(),
        None => {
            error::set_error("bad signal name pointer");
            return -1;
        }
    };
    unsafe { tor_send_seq(h, move |h| h.signal(&name)) }
}

/// Pops the oldest completed reply as
/// {"seq":N,"code":C,"lines":[...]} into `out` (rc_buffer_free).
/// RC_NOT_FOUND when nothing is pending.
///
/// # Safety
/// `h` live handle; `out` writable.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_take_reply(
    h: *const torctl::RcTorCtl,
    out: *mut RcBuffer,
) -> RcStatus {
    status_of(|| {
        let h = tor_ref(h)?;
        let reply = h.take_reply().ok_or_else(|| error::Fail {
            status: RcStatus::NotFound,
            msg: "no completed reply".into(),
        })?;
        let json = serde_json::json!({
            "seq": reply.seq,
            "code": reply.code,
            "lines": reply.lines,
        });
        buffer_out(out, json.to_string().into_bytes());
        Ok(())
    })
}

/// Pops the oldest async event line (code 650 stripped), NULL when
/// the queue is empty.  Free with rc_string_free().
///
/// # Safety
/// `h` must be a live rc_tor_open handle.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_take_event(
    h: *const torctl::RcTorCtl,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match tor_ref(h).ok().and_then(|h| h.take_event()) {
            Some(e) => util::to_c_string(e),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

/// Pops the oldest protocol complaint (unparsable line etc.), NULL
/// when empty.  Free with rc_string_free().
///
/// # Safety
/// `h` must be a live rc_tor_open handle.
#[no_mangle]
pub unsafe extern "C" fn rc_tor_take_error(
    h: *const torctl::RcTorCtl,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        match tor_ref(h).ok().and_then(|h| h.take_error()) {
            Some(e) => util::to_c_string(e),
            None => ptr::null_mut(),
        }
    }))
    .unwrap_or(ptr::null_mut())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;
    use std::sync::MutexGuard;

    // Every test here drives the one process-global STORE — they must
    // run serialized or a parallel test's rc_set_data_dir switches
    // the dir mid-test.
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
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
    fn kdf_params_from_untrusted_file_are_bounded() {
        // VSEC01: securestore.kdf carries no MAC — a byte-edit can ask
        // for gigabytes of Argon2 memory and abort the process inside
        // the allocator.  parse_kdf must reject out-of-envelope params
        // BEFORE any derive runs.
        let _g = guard();
        let dir = tmpdir("kdfbound");
        init(&dir);
        let pass = b"passphrase1";
        assert_eq!(
            unsafe { rc_kdf_create(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
        let path = format!("{dir}/securestore.kdf");
        let good = std::fs::read(&path).unwrap();

        for (offset, bad) in [
            (6usize, u32::MAX),  // memory_kib — allocator DoS
            (6usize, 0),         // below argon2's floor too
            (10usize, u32::MAX), // passes
            (14usize, 0),        // lanes
        ] {
            let mut data = good.clone();
            data[offset..offset + 4].copy_from_slice(&bad.to_le_bytes());
            std::fs::write(&path, &data).unwrap();
            assert_eq!(
                unsafe { rc_unlock(pass.as_ptr(), pass.len()) },
                RcStatus::Corrupt,
                "byte-offset {offset} = {bad} must fail closed"
            );
        }

        // The honest file still unlocks — the bounds didn't break the
        // real envelope.
        std::fs::write(&path, &good).unwrap();
        assert_eq!(
            unsafe { rc_unlock(pass.as_ptr(), pass.len()) },
            RcStatus::Ok
        );
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
