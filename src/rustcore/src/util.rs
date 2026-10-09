//! Pointer/buffer marshaling helpers for the C ABI.  All functions
//! treat their inputs as untrusted — a bad pointer degrades to an
//! error or None, never to UB across the boundary.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::ptr;
use std::slice;

/// Borrows `len` bytes at `ptr`.  A null pointer is legal only for
/// zero-length input.
pub unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> Option<&'a [u8]> {
    if ptr.is_null() {
        return if len == 0 { Some(&[]) } else { None };
    }
    Some(unsafe { slice::from_raw_parts(ptr, len) })
}

/// Borrows a fixed 32-byte key.
pub unsafe fn key32<'a>(ptr: *const u8) -> Option<&'a [u8; 32]> {
    if ptr.is_null() {
        return None;
    }
    Some(unsafe { &*(ptr as *const [u8; 32]) })
}

/// Borrows a NUL-terminated UTF-8 string.
pub unsafe fn cstr<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(ptr) }.to_str().ok()
}

/// Hands a string to C; caller frees with rc_string_free().
pub fn to_c_string(s: String) -> *mut c_char {
    CString::new(s).map_or(ptr::null_mut(), |c| c.into_raw())
}

/// Moves `v` into a boxed slice the caller frees with rc_buffer_free().
/// Empty vectors still return a non-null dangling pointer — legal for
/// the matching free, and keeps "empty result" distinct from null.
pub fn into_raw_buffer(v: Vec<u8>) -> (*mut u8, usize) {
    let len = v.len();
    let b = v.into_boxed_slice();
    (Box::into_raw(b) as *mut u8, len)
}
