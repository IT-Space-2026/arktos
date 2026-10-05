//! C FFI wrapper around Brave's adblock-rust engine for Arora (ADB02).
//!
//! Arora keeps its own subscription manager, rule storage and cosmetic
//! pipeline; this library only delegates the heavy matching work:
//!
//!   * `arora_adblock_engine_new`    — build an Engine from filter text
//!   * `arora_adblock_engine_add_resource` — register a $redirect= stub
//!   * `arora_adblock_engine_check`  — block/allow/redirect/removeparam
//!   * `arora_adblock_engine_cosmetic` — hide selectors + injected script
//!
//! All entry points are panic-safe (catch_unwind) and treat their inputs
//! as untrusted: bad pointers or unparseable requests degrade to
//! "allow", never to a crash crossing the FFI boundary.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;

use adblock::request::Request;
use adblock::resources::{MimeType, Resource, ResourceType};
use adblock::Engine;
use base64::{engine::general_purpose::STANDARD as BASE64, Engine as _};

/// Opaque handle owning an adblock::Engine.
///
/// `use_resources` replaces the engine's whole resource storage on
/// every call, so resources are accumulated here and replayed in full.
pub struct AroraAdBlockEngine {
    engine: Engine,
    resources: Vec<Resource>,
}

/// Decision returned by arora_adblock_engine_check().
///
/// `redirect_url` and `rewritten_url` are heap-allocated C strings the
/// caller must release with arora_adblock_string_free().  `redirect_url`
/// carries a `data:` URL produced from a bundled stub resource when a
/// `$redirect=` rule matched; `rewritten_url` is the URL with
/// `$removeparam` parameters stripped.
#[repr(C)]
pub struct AroraCheckResult {
    /// 0 = allow, 1 = block, 2 = redirect to redirect_url.
    pub action: i32,
    pub redirect_url: *mut c_char,
    pub rewritten_url: *mut c_char,
}

/// Cosmetic filtering payload for one document URL, as JSON:
/// `{"hide":[selectors...],"generichide":bool,"script":"..."}`.
/// `script` is empty unless scriptlet resources were registered.
#[repr(C)]
pub struct AroraCosmeticResult {
    pub json: *mut c_char,
}

unsafe fn cstr_to_string<'a>(ptr: *const c_char) -> Option<&'a str> {
    if ptr.is_null() {
        return None;
    }
    unsafe { CStr::from_ptr(ptr) }.to_str().ok()
}

fn to_c_string(s: String) -> *mut c_char {
    CString::new(s).map_or(ptr::null_mut(), |c| c.into_raw())
}

fn allow_result() -> AroraCheckResult {
    AroraCheckResult {
        action: 0,
        redirect_url: ptr::null_mut(),
        rewritten_url: ptr::null_mut(),
    }
}

/// Builds an engine from `len` bytes of UTF-8 filter text (ABP/uBO list
/// format, one rule per line).  Returns null on any failure.
///
/// # Safety
/// `rules` must point to `len` readable bytes.  The returned handle must
/// be freed with arora_adblock_engine_free().
#[no_mangle]
pub unsafe extern "C" fn arora_adblock_engine_new(
    rules: *const c_char,
    rules_len: usize,
) -> *mut AroraAdBlockEngine {
    catch_unwind(AssertUnwindSafe(|| {
        if rules.is_null() || rules_len == 0 {
            return ptr::null_mut();
        }
        let bytes = unsafe { std::slice::from_raw_parts(rules as *const u8, rules_len) };
        let Ok(text) = std::str::from_utf8(bytes) else {
            return ptr::null_mut();
        };
        Box::into_raw(Box::new(AroraAdBlockEngine {
            engine: Engine::new_with_list_text(text),
            resources: Vec::new(),
        }))
    }))
    .unwrap_or(ptr::null_mut())
}

/// Registers a stub resource so `$redirect=name` resolves to a `data:`
/// URL carrying `content` under `mime`.  Call once per lookup name
/// (the C++ side expands its alias table into plain names).
///
/// # Safety
/// `name`/`mime` must be NUL-terminated; `content` must point to `len`
/// readable bytes.
#[no_mangle]
pub unsafe extern "C" fn arora_adblock_engine_add_resource(
    engine: *mut AroraAdBlockEngine,
    name: *const c_char,
    mime: *const c_char,
    content: *const u8,
    content_len: usize,
) -> i32 {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(engine) = (unsafe { engine.as_mut() }) else {
            return -1;
        };
        let (Some(name), Some(mime)) =
            (unsafe { cstr_to_string(name) }, unsafe { cstr_to_string(mime) })
        else {
            return -1;
        };
        if content.is_null() && content_len > 0 {
            return -1;
        }
        let bytes = if content_len == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(content, content_len) }
        };
        engine.resources.push(Resource {
            name: name.to_string(),
            aliases: Vec::new(),
            kind: ResourceType::Mime(MimeType::from(mime)),
            content: BASE64.encode(bytes),
            dependencies: Vec::new(),
            permission: Default::default(),
        });
        // use_resources() replaces the storage wholesale — replay all
        // registered resources so earlier registrations survive.
        engine.engine.use_resources(engine.resources.iter().cloned());
        0
    }))
    .unwrap_or(-1)
}

/// Matches one network request.  `request_type` takes the webRequest
/// spellings ("script", "image", "xmlhttprequest", "main_frame", ...);
/// anything unrecognized maps to "other".  Returns an allow result when
/// the URL cannot be parsed.
///
/// # Safety
/// `url`/`source_url`/`request_type` must be NUL-terminated or null.
/// Free the returned strings with arora_adblock_string_free().
#[no_mangle]
pub unsafe extern "C" fn arora_adblock_engine_check(
    engine: *const AroraAdBlockEngine,
    url: *const c_char,
    source_url: *const c_char,
    request_type: *const c_char,
) -> AroraCheckResult {
    catch_unwind(AssertUnwindSafe(|| {
        let Some(engine) = (unsafe { engine.as_ref() }) else {
            return allow_result();
        };
        let Some(url) = (unsafe { cstr_to_string(url) }) else {
            return allow_result();
        };
        let source_url = unsafe { cstr_to_string(source_url) }.unwrap_or("");
        let request_type = unsafe { cstr_to_string(request_type) }.unwrap_or("other");

        let Ok(request) = Request::new(url, source_url, request_type, "GET") else {
            return allow_result();
        };
        let result = engine.engine.check_network_request(&request);

        let mut out = allow_result();
        if result.should_block() {
            if let Some(redirect) = result.redirect {
                out.action = 2;
                out.redirect_url = to_c_string(redirect);
            } else {
                out.action = 1;
            }
        } else if let Some(rewritten) = result.rewritten_url {
            out.rewritten_url = to_c_string(rewritten);
        }
        out
    }))
    .unwrap_or_else(|_| allow_result())
}

/// Returns the cosmetic filtering payload for `url` as a JSON string
/// the caller frees with arora_adblock_string_free(), or null.
///
/// # Safety
/// `url` must be NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn arora_adblock_engine_cosmetic(
    engine: *const AroraAdBlockEngine,
    url: *const c_char,
) -> *mut c_char {
    catch_unwind(AssertUnwindSafe(|| {
        let (Some(engine), Some(url)) =
            (unsafe { engine.as_ref() }, unsafe { cstr_to_string(url) })
        else {
            return ptr::null_mut();
        };
        let resources = engine.engine.url_cosmetic_resources(url);
        let json = serde_json::json!({
            "hide": resources.hide_selectors,
            "generichide": resources.generichide,
            "script": resources.injected_script,
        });
        to_c_string(json.to_string())
    }))
    .unwrap_or(ptr::null_mut())
}

/// Frees an engine handle.
///
/// # Safety
/// `engine` must be a handle returned by arora_adblock_engine_new(),
/// or null.
#[no_mangle]
pub unsafe extern "C" fn arora_adblock_engine_free(engine: *mut AroraAdBlockEngine) {
    if !engine.is_null() {
        drop(unsafe { Box::from_raw(engine) });
    }
}

/// Frees a string returned by this library.
///
/// # Safety
/// `s` must be a pointer handed out by this library, or null.
#[no_mangle]
pub unsafe extern "C" fn arora_adblock_string_free(s: *mut c_char) {
    if !s.is_null() {
        drop(unsafe { CString::from_raw(s) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    fn build(rules: &str) -> *mut AroraAdBlockEngine {
        unsafe { arora_adblock_engine_new(rules.as_ptr().cast(), rules.len()) }
    }

    fn add(e: *mut AroraAdBlockEngine, name: &str, mime: &str, body: &[u8]) {
        let name = CString::new(name).unwrap();
        let mime = CString::new(mime).unwrap();
        unsafe {
            arora_adblock_engine_add_resource(
                e, name.as_ptr(), mime.as_ptr(), body.as_ptr(), body.len(),
            );
        }
    }

    fn check(e: *mut AroraAdBlockEngine, url: &str, src: &str, ty: &str) -> AroraCheckResult {
        let url = CString::new(url).unwrap();
        let src = CString::new(src).unwrap();
        let ty = CString::new(ty).unwrap();
        unsafe { arora_adblock_engine_check(e, url.as_ptr(), src.as_ptr(), ty.as_ptr()) }
    }

    #[test]
    fn block_exception_redirect_removeparam() {
        let rules = "||ads.example^\n\
                     @@||ads.example/ok.js^\n\
                     ||redir.example/vast.xml$redirect=noop-vast-4.0\n\
                     ||param.example^$removeparam=utm_source\n";
        let e = build(rules);
        assert!(!e.is_null());
        add(e, "noop-vast-4.0", "text/xml", b"<VAST></VAST>");
        add(e, "noop.js", "application/javascript", b"()=>{}");

        assert_eq!(check(e, "http://ads.example/x", "http://a.example/", "script").action, 1);
        assert_eq!(check(e, "http://ads.example/ok.js", "http://a.example/", "script").action, 0);

        let r = check(e, "http://redir.example/vast.xml", "http://a.example/", "xmlhttprequest");
        assert_eq!(r.action, 2);
        assert!(!r.redirect_url.is_null());
        unsafe {
            let redir = CStr::from_ptr(r.redirect_url).to_string_lossy();
            assert!(redir.starts_with("data:text/xml;base64,"));
        }

        let r = check(e, "http://param.example/x?utm_source=1&k=2", "http://a.example/", "main_frame");
        assert_eq!(r.action, 0);
        unsafe {
            assert_eq!(CStr::from_ptr(r.rewritten_url).to_str().unwrap(),
                       "http://param.example/x?k=2");
        }
        unsafe { arora_adblock_engine_free(e) };
    }

    #[test]
    fn cosmetic_payload() {
        let e = build("smoke.example##.ad-banner\n");
        assert!(!e.is_null());
        let url = CString::new("http://smoke.example/").unwrap();
        let json = unsafe { arora_adblock_engine_cosmetic(e, url.as_ptr()) };
        assert!(!json.is_null());
        unsafe {
            let s = CStr::from_ptr(json).to_string_lossy();
            assert!(s.contains(".ad-banner"), "got {s}");
        }
        unsafe { arora_adblock_engine_free(e) };
    }
}
