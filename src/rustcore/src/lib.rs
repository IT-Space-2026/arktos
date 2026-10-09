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

mod cred;
mod error;
mod notify;
mod store;
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
