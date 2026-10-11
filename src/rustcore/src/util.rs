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

/// Appends `s` to `out` as a JSON string literal, quotes included —
/// the crate's hand-rolled emitters share the one escaping rule
/// (control bytes are dropped, like the lookalike emitter does).
pub fn json_push_str(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' | '\\' => {
                out.push('\\');
                out.push(c);
            }
            c if (c as u32) < 0x20 => {}
            c => out.push(c),
        }
    }
    out.push('"');
}

/// Moves `v` into a boxed slice the caller frees with rc_buffer_free().
/// Empty vectors still return a non-null dangling pointer — legal for
/// the matching free, and keeps "empty result" distinct from null.
pub fn into_raw_buffer(v: Vec<u8>) -> (*mut u8, usize) {
    let len = v.len();
    let b = v.into_boxed_slice();
    (Box::into_raw(b) as *mut u8, len)
}

// ---- hostname normalization (blocklist/tranco shared hot path) -----

/// A query hostname normalized for probing: trimmed, ASCII-lowercased,
/// trailing dots stripped, `[v6]` brackets removed — held in a fixed
/// stack buffer so membership probes stay allocation-free (the
/// tranco::rank_of discipline blocklist::contains now shares).
/// Longer-than-DNS input is still probed on its rightmost bytes:
/// every listed entry is <= 253 bytes, so any matching label-
/// boundary suffix lives entirely inside the tail.
pub struct HostNorm {
    buf: [u8; 253],
    len: usize,
}

impl HostNorm {
    pub fn new(host: &str) -> Option<HostNorm> {
        let mut buf = [0u8; 253];
        let mut n = 0usize;
        let trimmed = host.trim();
        let input = if trimmed.len() > buf.len() {
            let mut start = trimmed.len() - buf.len();
            while !trimmed.is_char_boundary(start) {
                start += 1;
            }
            &trimmed[start..]
        } else {
            trimmed
        };
        for &b in input.as_bytes() {
            buf[n] = b.to_ascii_lowercase();
            n += 1;
        }
        while n > 0 && buf[n - 1] == b'.' {
            n -= 1;
        }
        if n == 0 {
            return None;
        }
        // QUrl::host() reports IPv6 literals bracketed.
        if buf[0] == b'[' && buf[n - 1] == b']' {
            buf.copy_within(1..n - 1, 0);
            n -= 2;
        }
        if n == 0 {
            return None;
        }
        // The normalization only touched ASCII bytes, so the result
        // stays valid UTF-8.
        std::str::from_utf8(&buf[..n]).ok()?;
        Some(HostNorm { buf, len: n })
    }

    pub fn as_str(&self) -> &str {
        // Valid UTF-8: checked at construction.
        std::str::from_utf8(&self.buf[..self.len]).unwrap_or("")
    }

    /// The whole name, then each label-boundary parent:
    /// "a.b.c" yields "a.b.c", "b.c", "c".
    pub fn suffixes(&self) -> HostSuffixes<'_> {
        HostSuffixes {
            rest: self.as_str(),
        }
    }
}

/// Conservative DNS-name gate shared by the blocklist and Tranco
/// entry parsers: one dot minimum (no TLD or localhost entries),
/// labels of [a-z0-9_-] only, sane lengths.  IP literals pass the
/// shape test — callers that list no IPs (tranco) reject them on
/// top; the blocklist keeps them for exact-match only.
pub fn valid_dns_shape(host: &str) -> bool {
    if host.len() > 253 || !host.contains('.') {
        return false;
    }
    host.split('.').all(|label| {
        !label.is_empty()
            && label.len() <= 63
            && label
                .bytes()
                .all(|b| b.is_ascii_alphanumeric() || b == b'-' || b == b'_')
    })
}

pub struct HostSuffixes<'a> {
    rest: &'a str,
}

impl<'a> Iterator for HostSuffixes<'a> {
    type Item = &'a str;

    fn next(&mut self) -> Option<&'a str> {
        if self.rest.is_empty() {
            return None;
        }
        let out = self.rest;
        self.rest = match self.rest.find('.') {
            Some(i) => &self.rest[i + 1..],
            None => "",
        };
        Some(out)
    }
}
