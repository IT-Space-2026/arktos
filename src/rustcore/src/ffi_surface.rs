//! FFI-surface coverage tests (COV05).
//!
//! Every `rc_*` entry point is exercised over the C ABI itself —
//! happy path plus bad-argument path — so the marshaling/validation
//! layer in lib.rs is measured inside the crate rather than only
//! through the Qt adapters.  Tests that touch process-global stores
//! serialize on store::test_lock() like the module tests do.

use crate::error::RcStatus;
use crate::RcBuffer;
use std::ffi::{CStr, CString};
use std::os::raw::c_void;
use std::ptr;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::MutexGuard;

fn guard() -> MutexGuard<'static, ()> {
    crate::store::test_lock()
}

fn tmpdir(tag: &str) -> String {
    let dir = std::env::temp_dir().join(format!(
        "rustcore-ffi-{}-{}-{}",
        std::process::id(),
        tag,
        std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .unwrap()
            .as_nanos()
    ));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).unwrap();
    dir.to_string_lossy().into_owned()
}

fn cs(s: &str) -> CString {
    CString::new(s).unwrap()
}

/// Consume an FFI string result: copy it out, free it, hand back the text.
fn take_str(p: *mut std::os::raw::c_char) -> Option<String> {
    if p.is_null() {
        return None;
    }
    let s = unsafe { CStr::from_ptr(p) }
        .to_string_lossy()
        .into_owned();
    unsafe { crate::rc_string_free(p) };
    Some(s)
}

fn take_buf(b: &mut RcBuffer) -> Vec<u8> {
    if b.data.is_null() {
        return Vec::new();
    }
    let v = unsafe { std::slice::from_raw_parts(b.data, b.len) }.to_vec();
    unsafe {
        crate::rc_buffer_free(RcBuffer {
            data: b.data,
            len: b.len,
        })
    };
    b.data = ptr::null_mut();
    b.len = 0;
    v
}

fn out_buf() -> RcBuffer {
    RcBuffer {
        data: ptr::null_mut(),
        len: 0,
    }
}

fn init(dir: &str) {
    let c = cs(dir);
    assert_eq!(unsafe { crate::rc_set_data_dir(c.as_ptr()) }, RcStatus::Ok);
}

static CB_HITS: AtomicUsize = AtomicUsize::new(0);
static CB_LAST: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn count_cb(topic: *const std::os::raw::c_char, ud: *mut c_void) {
    CB_HITS.fetch_add(1, Ordering::SeqCst);
    CB_LAST.store(ud as usize, Ordering::SeqCst);
    assert!(!topic.is_null());
    let _ = unsafe { CStr::from_ptr(topic) };
}

#[test]
fn housekeeping_and_change_callback() {
    let _g = guard();
    assert_eq!(
        unsafe { crate::rc_set_data_dir(ptr::null()) },
        RcStatus::InvalidArgument
    );
    // The error channel reports the failed call on this thread.
    let msg = take_str(unsafe { crate::rc_last_error_message() });
    assert!(msg.is_some());
    init(&tmpdir("house"));
    assert!(take_str(unsafe { crate::rc_last_error_message() }).is_some());

    // NULL-tolerant frees never crash.
    unsafe { crate::rc_string_free(ptr::null_mut()) };
    unsafe { crate::rc_buffer_free(out_buf()) };

    // The callback bridge fires on mutations and carries userdata.
    unsafe { crate::rc_set_change_callback(Some(count_cb), 0x5eedusize as *mut c_void) };
    let before = CB_HITS.load(Ordering::SeqCst);
    let k = cs("permk");
    let v = cs("permv");
    assert_eq!(
        unsafe { crate::rc_sitedec_set(k.as_ptr(), k.as_ptr(), v.as_ptr()) },
        RcStatus::Ok
    );
    assert!(CB_HITS.load(Ordering::SeqCst) > before);
    assert_eq!(CB_LAST.load(Ordering::SeqCst), 0x5eed);
    unsafe { crate::rc_set_change_callback(None, ptr::null_mut()) };
    let n = CB_HITS.load(Ordering::SeqCst);
    assert_eq!(
        unsafe { crate::rc_sitedec_remove(k.as_ptr(), k.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(CB_HITS.load(Ordering::SeqCst), n); // cleared — no more calls
}

#[test]
fn custody_and_blob_crypto_surface() {
    let _g = guard();
    init(&tmpdir("custody"));

    assert!(unsafe { crate::rc_is_available() } != 0);
    assert_eq!(unsafe { crate::rc_passphrase_enabled() }, 0);
    assert_eq!(unsafe { crate::rc_is_unlocked() }, 1);

    // rc_is_sealed on garbage and NULL.
    assert_eq!(unsafe { crate::rc_is_sealed(ptr::null(), 0) }, 0);
    assert_eq!(unsafe { crate::rc_is_sealed(b"junk".as_ptr(), 4) }, 0);

    // seal/open round trip + bad-argument forms.
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_seal(ptr::null(), 3, &mut out) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_seal(b"abc".as_ptr(), 3, &mut out) },
        RcStatus::Ok
    );
    let blob = take_buf(&mut out);
    assert_eq!(unsafe { crate::rc_is_sealed(blob.as_ptr(), blob.len()) }, 1);
    let mut plain = out_buf();
    assert_eq!(
        unsafe { crate::rc_open(blob.as_ptr(), blob.len(), &mut plain) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut plain), b"abc");
    assert_eq!(
        unsafe { crate::rc_open(b"ARSEC1".as_ptr(), 6, &mut plain) },
        RcStatus::Corrupt
    );

    // Explicit-key seal/open + key-pointer validation.
    let key = [7u8; 32];
    let mut sealed = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_seal_with_key(ptr::null(), b"x".as_ptr(), 1, &mut sealed)
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rc_seal_with_key(key.as_ptr(), b"kdata".as_ptr(), 5, &mut sealed)
        },
        RcStatus::Ok
    );
    let kblob = take_buf(&mut sealed);
    let mut kout = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_open_with_key(key.as_ptr(), kblob.as_ptr(), kblob.len(), &mut kout)
        },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut kout), b"kdata");
    let wrong = [9u8; 32];
    assert_ne!(
        unsafe {
            crate::rc_open_with_key(
                wrong.as_ptr(),
                kblob.as_ptr(),
                kblob.len(),
                &mut kout,
            )
        },
        RcStatus::Ok
    );

    // unlock/lock without a passphrase store.
    assert_ne!(
        unsafe { crate::rc_unlock(b"whatever".as_ptr(), 8) },
        RcStatus::Ok
    );
    unsafe { crate::rc_lock() };

    // Key-file lifecycle + copies.
    let _ = unsafe { crate::rc_key_file_delete() };
    assert_eq!(unsafe { crate::rc_key_file_create() }, RcStatus::Ok);
    let mut copy = [0u8; 32];
    assert_eq!(unsafe { crate::rc_key_file_copy(copy.as_mut_ptr()) }, RcStatus::Ok);
    assert!(copy.iter().any(|b| *b != 0));
    assert_eq!(
        unsafe { crate::rc_key_file_copy(ptr::null_mut()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_key_file_delete() }, RcStatus::Ok);
    // File custody re-mints on demand — copy still succeeds.
    assert_eq!(unsafe { crate::rc_key_file_copy(copy.as_mut_ptr()) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_kdf_file_delete() }, RcStatus::Ok);

    // Passphrase lifecycle: create -> change -> lock -> unlock.
    let pass = b"ffi passphrase";
    assert_eq!(
        unsafe { crate::rc_kdf_create(pass.as_ptr(), pass.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_passphrase_enabled() }, 1);
    let pass2 = b"ffi passphrase 2";
    assert_eq!(
        unsafe { crate::rc_cred_change_passphrase(pass2.as_ptr(), pass2.len()) },
        RcStatus::Ok
    );
    unsafe { crate::rc_lock() };
    assert_eq!(unsafe { crate::rc_is_unlocked() }, 0);
    assert_eq!(
        unsafe { crate::rc_unlock(pass2.as_ptr(), pass2.len()) },
        RcStatus::Ok
    );
    let mut derived = [0u8; 32];
    assert_eq!(
        unsafe { crate::rc_derived_key_copy(derived.as_mut_ptr()) },
        RcStatus::Ok
    );
    assert!(derived.iter().any(|b| *b != 0));
    assert_eq!(
        unsafe { crate::rc_derived_key_copy(ptr::null_mut()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_kdf_create(ptr::null(), 0) },
        RcStatus::EmptyPassphrase
    );

    // cred_reseal argument validation (the happy path is covered by
    // the passphrase-switch test in lib.rs::tests).
    assert_eq!(
        unsafe { crate::rc_cred_reseal(ptr::null(), key.as_ptr()) },
        RcStatus::InvalidArgument
    );
}

#[test]
fn cred_and_autofill_surface() {
    let _g = guard();
    init(&tmpdir("credaf"));

    // Bad-argument forms on the cred family.
    let name = cs("site/login");
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_cred_get(ptr::null(), &mut out) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_cred_put(ptr::null(), b"v".as_ptr(), 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_cred_remove(ptr::null()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_cred_get(name.as_ptr(), &mut out) },
        RcStatus::NotFound
    );

    // Autofill: absent -> set -> present -> list -> reseal.
    assert_eq!(unsafe { crate::rc_autofill_store_present() }, 0);
    let forms = take_str(unsafe { crate::rc_autofill_forms() });
    assert_eq!(forms.as_deref(), Some("[]"));
    assert_eq!(
        unsafe { crate::rc_autofill_set_forms(ptr::null(), 4) },
        RcStatus::InvalidArgument
    );
    assert_ne!(
        unsafe { crate::rc_autofill_set_forms(b"not json".as_ptr(), 8) },
        RcStatus::Ok
    );
    let recs = br#"[{"url":"https://f/","name":"login","has_password":true,"elements":[["u","me"],["p","x"]]}]"#;
    assert_eq!(
        unsafe { crate::rc_autofill_set_forms(recs.as_ptr(), recs.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_autofill_store_present() }, 1);
    let listed = take_str(unsafe { crate::rc_autofill_forms() }).unwrap();
    assert!(listed.contains("https://f/"));

    // Reseal under a wrong from-key fails closed — AEAD reject.
    let (k1, k2) = ([1u8; 32], [2u8; 32]);
    assert_eq!(
        unsafe { crate::rc_autofill_reseal(k1.as_ptr(), k2.as_ptr()) },
        RcStatus::Crypto
    );
    assert_eq!(
        unsafe { crate::rc_autofill_reseal(ptr::null(), k2.as_ptr()) },
        RcStatus::InvalidArgument
    );
}

#[test]
fn urlstrip_and_blocklist_surface() {
    let _g = guard();
    init(&tmpdir("strip"));

    // Deterministic ruleset — never depend on what an earlier test left.
    let rules = br#"{"version":1,"params":["utm_*","fbclid"],"exceptions":[]}"#;
    assert_eq!(
        unsafe { crate::rc_urlstrip_load_rules(rules.as_ptr(), rules.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_urlstrip_load_rules(b"@@".as_ptr(), 2) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_urlstrip_load_rules(ptr::null(), 2) },
        RcStatus::InvalidArgument
    );
    let dirty = cs("https://x/?utm_source=a&real=1&fbclid=z");
    let clean = take_str(unsafe { crate::rc_urlstrip(dirty.as_ptr()) }).unwrap();
    assert_eq!(clean, "https://x/?real=1");
    let plain = cs("https://x/?real=1");
    let same = take_str(unsafe { crate::rc_urlstrip(plain.as_ptr()) }).unwrap();
    assert_eq!(same, "https://x/?real=1");
    assert!(unsafe { crate::rc_urlstrip(ptr::null()) }.is_null());
    assert_eq!(unsafe { crate::rc_urlstrip_reload() }, RcStatus::Ok);

    // Blocklist over the FFI: load -> check -> count -> reload.
    assert_eq!(unsafe { crate::rc_blocklist_check(ptr::null()) }, 0);
    let body = b"evil.example\n0.0.0.0 badhost.test\nhttps://listed.test/path\n";
    assert_eq!(
        unsafe { crate::rc_blocklist_load(body.as_ptr(), body.len()) },
        RcStatus::Ok
    );
    let evil = cs("sub.evil.example");
    assert_eq!(unsafe { crate::rc_blocklist_check(evil.as_ptr()) }, 1);
    let good = cs("innocent.example");
    assert_eq!(unsafe { crate::rc_blocklist_check(good.as_ptr()) }, 0);
    assert!(unsafe { crate::rc_blocklist_count() } >= 3);
    assert_eq!(
        unsafe { crate::rc_blocklist_load(b"  \n".as_ptr(), 3) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_blocklist_load(b"\xff\xfe".as_ptr(), 2) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_blocklist_load(ptr::null(), 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_blocklist_reload() }, RcStatus::Ok);

    // Tranco rank table over the FFI: load -> rank -> count -> reload.
    assert_eq!(unsafe { crate::rc_tranco_rank(ptr::null()) }, 0);
    let rows = b"3,ranked.example\n17,sub.deep.example\n";
    assert_eq!(
        unsafe { crate::rc_tranco_load(rows.as_ptr(), rows.len()) },
        RcStatus::Ok
    );
    let host = cs("www.ranked.example");
    assert_eq!(unsafe { crate::rc_tranco_rank(host.as_ptr()) }, 3);
    let deep = cs("a.b.sub.deep.example");
    assert_eq!(unsafe { crate::rc_tranco_rank(deep.as_ptr()) }, 17);
    let unlisted = cs("unlisted.invalid");
    assert_eq!(unsafe { crate::rc_tranco_rank(unlisted.as_ptr()) }, 0);
    assert!(unsafe { crate::rc_tranco_count() } >= 2);
    assert_eq!(
        unsafe { crate::rc_tranco_load(b"# none\n".as_ptr(), 7) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_tranco_load(b"\xff\xfe".as_ptr(), 2) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_tranco_load(ptr::null(), 1) },
        RcStatus::InvalidArgument
    );
    // Reload restores the vendored-seed coverage on top of the load.
    assert_eq!(unsafe { crate::rc_tranco_reload() }, RcStatus::Ok);
    let google = cs("google.com");
    assert_eq!(unsafe { crate::rc_tranco_rank(google.as_ptr()) }, 1);

    // Lookalike probe over the FFI: a confusable twin of a ranked
    // domain flags; the real domain and unranked names stay clean.
    assert!(unsafe { crate::rc_lookalike_check(ptr::null()) }.is_null());
    let rows = b"3,paypal.example\n";
    assert_eq!(
        unsafe { crate::rc_tranco_load(rows.as_ptr(), rows.len()) },
        RcStatus::Ok
    );
    let twin = cs("paypa1.example");
    let j = take_str(unsafe { crate::rc_lookalike_check(twin.as_ptr()) }).unwrap();
    assert!(j.contains("\"verdict\":\"lookalike\""), "{}", j);
    assert!(j.contains("\"lookalike_of\":\"paypal.example\""), "{}", j);
    let real = cs("paypal.example");
    let j = take_str(unsafe { crate::rc_lookalike_check(real.as_ptr()) }).unwrap();
    assert!(j.contains("\"verdict\":\"clean\""), "{}", j);
    let plain = cs("unranked.example");
    let j = take_str(unsafe { crate::rc_lookalike_check(plain.as_ptr()) }).unwrap();
    assert!(j.contains("\"verdict\":\"clean\""), "{}", j);
    // Restore the seed so later tests see the stock table again.
    assert_eq!(unsafe { crate::rc_tranco_reload() }, RcStatus::Ok);
}

#[test]
fn parser_entry_points() {
    let mut out = out_buf();
    let desc = br#"<?xml version="1.0"?>
<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <ShortName>T</ShortName>
  <Url method="get" type="text/html" template="http://t/?q={searchTerms}"/>
</OpenSearchDescription>"#;
    assert_eq!(
        unsafe { crate::rc_opensearch_parse(desc.as_ptr(), desc.len(), &mut out) },
        RcStatus::Ok
    );
    let json = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&json).contains("\"T\""));
    assert_eq!(
        unsafe { crate::rc_opensearch_parse(b"<x".as_ptr(), 2, &mut out) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_opensearch_parse(ptr::null(), 2, &mut out) },
        RcStatus::InvalidArgument
    );

    let gupdate = b"<gupdate protocol='2.0'><app appid='a'>\
        <updatecheck status='ok' codebase='https://x/p.zip' version='2'/>\
        </app></gupdate>";
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_updatemanifest_parse(gupdate.as_ptr(), gupdate.len(), &mut out)
        },
        RcStatus::Ok
    );
    let json = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&json).contains("https://x/p.zip"));
    assert_eq!(
        unsafe { crate::rc_updatemanifest_parse(b"<gupdate>".as_ptr(), 9, &mut out) },
        RcStatus::Corrupt
    );

    assert_eq!(
        unsafe { crate::rc_xbel_check(b"<xbel version='1.0'/>".as_ptr(), 21) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_xbel_check(b"<xbel version='2.0'/>".as_ptr(), 21) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_xbel_check(ptr::null(), 3) },
        RcStatus::InvalidArgument
    );

    let sugg = br#"["term",["alpha","beta"]]"#;
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_suggest_parse(sugg.as_ptr(), sugg.len(), &mut out) },
        RcStatus::Ok
    );
    let json = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&json).contains("alpha"));
    assert_eq!(
        unsafe { crate::rc_suggest_parse(b"{".as_ptr(), 1, &mut out) },
        RcStatus::Corrupt
    );
}

#[test]
fn readability_and_extverify_entry_points() {
    let article = {
        let mut h = String::from("<html><head><title>Art</title></head><body><article><h1>t</h1>");
        for _ in 0..40 {
            h.push_str("<p>Some sufficiently long article paragraph body text to look like content.</p>");
        }
        h.push_str("</article></body></html>");
        h
    };
    assert_eq!(
        unsafe { crate::rc_readability_probe(article.as_ptr(), article.len()) },
        1
    );
    assert_eq!(unsafe { crate::rc_readability_probe(b"nope".as_ptr(), 4) }, 0);
    assert_eq!(unsafe { crate::rc_readability_probe(ptr::null(), 4) }, 0);

    let mut out = out_buf();
    let base = cs("https://ex.com/page");
    assert_eq!(
        unsafe {
            crate::rc_readability_extract(
                article.as_ptr(),
                article.len(),
                base.as_ptr(),
                &mut out,
            )
        },
        RcStatus::Ok
    );
    let verdict = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&verdict).contains("\"ok\":true"));
    assert_eq!(
        unsafe {
            crate::rc_readability_extract(ptr::null(), 4, base.as_ptr(), &mut out)
        },
        RcStatus::InvalidArgument
    );

    // Minimal stored-method zip (manifest + payload member).
    const MANIFEST: &[u8] =
        br#"{"manifest_version":3,"name":"T","version":"1.0","permissions":["storage"]}"#;
    let zip = build_zip(&[(b"manifest.json", 0, MANIFEST), (b"bg.js", 0, b"//x")]);
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_ext_verify_package(
                zip.as_ptr(),
                zip.len(),
                ptr::null(),
                ptr::null(),
                0,
                &mut out,
            )
        },
        RcStatus::Ok
    );
    let v = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&v).contains("\"status\":\"valid\""));

    // A pinned key flips the signing verdict to the honest stub note;
    // a declared-but-wrong hash rejects the package.
    let bad_hash = [0xEEu8; 32];
    let key = [1u8; 64];
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_ext_verify_package(
                zip.as_ptr(),
                zip.len(),
                bad_hash.as_ptr(),
                key.as_ptr(),
                key.len(),
                &mut out,
            )
        },
        RcStatus::Ok
    );
    let v = take_buf(&mut out);
    let vs = String::from_utf8_lossy(&v).into_owned();
    assert!(vs.contains("rejected") || vs.contains("no-pinning"));
    assert_eq!(
        unsafe {
            crate::rc_ext_verify_package(
                ptr::null(),
                4,
                ptr::null(),
                ptr::null(),
                0,
                &mut out,
            )
        },
        RcStatus::InvalidArgument
    );

    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_ext_manifest_check(MANIFEST.as_ptr(), MANIFEST.len(), &mut out) },
        RcStatus::Ok
    );
    let v = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&v).contains("storage"));
    assert_eq!(
        unsafe { crate::rc_ext_manifest_check(b"@@".as_ptr(), 2, &mut out) },
        RcStatus::Corrupt
    );
}

// Minimal zip writer mirroring extverify.rs's test helper (stored
// entries only — no deflate needed at the FFI level).
fn build_zip(entries: &[(&[u8], u16, &[u8])]) -> Vec<u8> {
    let mut out = Vec::new();
    let mut cdes = Vec::new();
    for (name, method, data) in entries {
        let off = out.len() as u32;
        out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
        out.extend_from_slice(&20u16.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&method.to_le_bytes());
        out.extend_from_slice(&[0u8; 4]);
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&(data.len() as u32).to_le_bytes());
        out.extend_from_slice(&(data.len() as u32).to_le_bytes());
        out.extend_from_slice(&(name.len() as u16).to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(name);
        out.extend_from_slice(data);
        cdes.push((off, *method, data.len() as u32, name));
    }
    let cd_offset = out.len() as u32;
    for (off, method, comp, name) in &cdes {
        out.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
        out.extend_from_slice(&[20u8, 0, 20, 0]);
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&method.to_le_bytes());
        out.extend_from_slice(&[0u8; 4]);
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&comp.to_le_bytes());
        out.extend_from_slice(&comp.to_le_bytes());
        out.extend_from_slice(&(name.len() as u16).to_le_bytes());
        out.extend_from_slice(&[0u8; 8]);
        out.extend_from_slice(&0u32.to_le_bytes());
        out.extend_from_slice(&off.to_le_bytes());
        out.extend_from_slice(name);
    }
    let cd_size = out.len() as u32 - cd_offset;
    out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
    out.extend_from_slice(&[0u8; 4]);
    out.extend_from_slice(&(cdes.len() as u16).to_le_bytes());
    out.extend_from_slice(&(cdes.len() as u16).to_le_bytes());
    out.extend_from_slice(&cd_size.to_le_bytes());
    out.extend_from_slice(&cd_offset.to_le_bytes());
    out.extend_from_slice(&0u16.to_le_bytes());
    out
}

#[test]
fn bookmark_ffi_roundtrip() {
    let _g = guard();
    const XBEL: &[u8] = br#"<?xml version='1.0'?><!DOCTYPE xbel>
<xbel version='1.0'>
  <folder folded='no'><title>Bar</title>
    <bookmark href='http://a/'><title>A</title></bookmark>
    <separator/>
    <folder><title>Sub</title>
      <bookmark href='http://b/'><title>B</title><desc>d</desc></bookmark>
    </folder>
  </folder>
</xbel>"#;
    assert_eq!(
        unsafe { crate::rc_bm_load_mem(XBEL.as_ptr(), XBEL.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_bm_load_mem(b"<xbel><folder></xbel>".as_ptr(), 20) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_bm_load_mem(ptr::null(), 3) },
        RcStatus::InvalidArgument
    );

    let root = unsafe { crate::rc_bm_root() };
    assert_ne!(root, 0);
    assert!(unsafe { crate::rc_bm_child_count(root) } >= 1);
    assert_eq!(unsafe { crate::rc_bm_child_count(0xDEAD) }, -1);
    let top = unsafe { crate::rc_bm_child_at(root, 0) };
    assert_ne!(top, 0);
    assert_eq!(unsafe { crate::rc_bm_parent(top) }, root);
    assert_eq!(unsafe { crate::rc_bm_child_at(root, 99) }, 0);

    let meta = take_str(unsafe { crate::rc_bm_get(top) }).unwrap();
    assert!(meta.contains("\"type\":1")); // TYPE_FOLDER
    assert!(unsafe { crate::rc_bm_get(0xDEAD) }.is_null());

    let url = cs("http://a/");
    let found = unsafe { crate::rc_bm_find(url.as_ptr()) };
    assert_ne!(found, 0);
    assert_eq!(unsafe { crate::rc_bm_parent(found) }, top);
    let none = cs("http://absent/");
    assert_eq!(unsafe { crate::rc_bm_find(none.as_ptr()) }, 0);

    // Mutations through the ABI.
    let t = cs("Renamed");
    assert_eq!(unsafe { crate::rc_bm_set_title(top, t.as_ptr()) }, RcStatus::Ok);
    assert_eq!(
        unsafe { crate::rc_bm_set_title(top, ptr::null()) },
        RcStatus::InvalidArgument
    );
    let u = cs("http://a/changed");
    assert_eq!(unsafe { crate::rc_bm_set_url(found, u.as_ptr()) }, RcStatus::Ok);
    let d = cs("desc here");
    assert_eq!(unsafe { crate::rc_bm_set_desc(found, d.as_ptr()) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_bm_set_expanded(top, 0) }, RcStatus::Ok);
    let tags = cs(r#"["t1","t2"]"#);
    assert_eq!(unsafe { crate::rc_bm_set_tags(found, tags.as_ptr()) }, RcStatus::Ok);
    assert_eq!(
        unsafe { crate::rc_bm_set_tags(found, cs("{}").as_ptr()) },
        RcStatus::InvalidArgument
    );

    // create/detach/attach/destroy lifecycle.
    let spec = cs(r#"{"type":"url","title":"New","url":"http://n/"}"#);
    let node = unsafe { crate::rc_bm_create(root, -1, spec.as_ptr()) };
    assert_ne!(node, 0);
    assert_eq!(unsafe { crate::rc_bm_detach(node) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_bm_parent(node) }, 0);
    assert_eq!(unsafe { crate::rc_bm_attach(top, 0, node) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_bm_detach(node) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_bm_destroy(node) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_bm_create(root, 0, ptr::null()) }, 0);
    assert_eq!(
        unsafe { crate::rc_bm_create(root, 0, cs("{").as_ptr()) },
        0
    );
    assert_ne!(unsafe { crate::rc_bm_detach(0xDEAD) }, RcStatus::Ok);

    // save -> load round trip on disk.
    let dir = tmpdir("bmio");
    let path = format!("{dir}/bookmarks.xbel");
    let cp = cs(&path);
    assert_eq!(unsafe { crate::rc_bm_save(cp.as_ptr()) }, RcStatus::Ok);
    assert!(std::path::Path::new(&path).exists());
    assert_eq!(unsafe { crate::rc_bm_load(cp.as_ptr()) }, RcStatus::Ok);
    let missing = cs(&format!("{dir}/nope.xbel"));
    assert_eq!(unsafe { crate::rc_bm_load(missing.as_ptr()) }, RcStatus::Ok);
    assert_eq!(
        unsafe { crate::rc_bm_load(ptr::null()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_bm_save(ptr::null()) },
        RcStatus::InvalidArgument
    );
}

#[test]
fn bookmark_html_ffi() {
    let _g = guard();
    let root = unsafe { crate::rc_bm_root() };
    assert_ne!(root, 0);

    // Dry run counts without inserting; real run inserts under the
    // parent and reports the summary; the scheme gate counts the
    // refused urls by scheme.
    const HTML: &[u8] = br#"<DL><p>
<DT><H3>F</H3><DL><p>
<DT><A HREF="https://a/">A</A>
<DT><A HREF="javascript:x">J</A>
<DT><A HREF="data:,d">D</A>
<HR>
</DL><p></DL><p>"#;
    let probe = take_str(unsafe {
        crate::rc_bm_import_html(root, HTML.as_ptr(), HTML.len(), crate::bookmarkhtml::IMPORT_DRY_RUN)
    })
    .unwrap();
    let pj: serde_json::Value = serde_json::from_str(&probe).unwrap();
    assert_eq!(pj["imported"], 1);
    assert_eq!(pj["skipped"], 2);
    assert_eq!(pj["skipped_by_scheme"]["javascript"], 1);
    // nothing inserted by the probe
    assert_eq!(unsafe { crate::rc_bm_find(cs("https://a/").as_ptr()) }, 0);

    let bad_parent = unsafe {
        crate::rc_bm_import_html(0xDEAD, HTML.as_ptr(), HTML.len(), 0)
    };
    assert!(bad_parent.is_null());
    assert!(unsafe {
        crate::rc_bm_import_html(root, ptr::null(), 3, 0)
    }
    .is_null());

    let json = take_str(unsafe {
        crate::rc_bm_import_html(root, HTML.as_ptr(), HTML.len(), 0)
    })
    .unwrap();
    let v: serde_json::Value = serde_json::from_str(&json).unwrap();
    assert_eq!(v["imported"], 1);
    assert_eq!(v["separators"], 1);
    assert!(v["folders"].as_u64().unwrap() >= 2);
    assert_ne!(unsafe { crate::rc_bm_find(cs("https://a/").as_ptr()) }, 0);
    assert_eq!(
        unsafe { crate::rc_bm_find(cs("javascript:x").as_ptr()) },
        0
    );
    // consent flag admits the bookmarklet
    let json2 = take_str(unsafe {
        crate::rc_bm_import_html(
            root,
            HTML.as_ptr(),
            HTML.len(),
            crate::bookmarkhtml::IMPORT_BOOKMARKLETS,
        )
    })
    .unwrap();
    let v2: serde_json::Value = serde_json::from_str(&json2).unwrap();
    assert_eq!(v2["imported"], 1); // only the javascript url is new
    assert_ne!(
        unsafe { crate::rc_bm_find(cs("javascript:x").as_ptr()) },
        0
    );

    // export writes a parseable NETSCAPE file atomically.
    let dir = tmpdir("bmhtml");
    let path = format!("{dir}/out.html");
    let cp = cs(&path);
    assert_eq!(
        unsafe { crate::rc_bm_export_html(cp.as_ptr()) },
        RcStatus::Ok
    );
    let text = std::fs::read_to_string(&path).unwrap();
    assert!(text.contains("NETSCAPE-Bookmark-file-1"));
    assert!(text.contains("HREF=\"https://a/\""));
    assert_eq!(
        unsafe { crate::rc_bm_export_html(ptr::null()) },
        RcStatus::InvalidArgument
    );
    // and the exported file re-imports cleanly.
    let round = take_str(unsafe {
        crate::rc_bm_import_html(
            root,
            text.as_bytes().as_ptr(),
            text.len(),
            crate::bookmarkhtml::IMPORT_DRY_RUN,
        )
    })
    .unwrap();
    let rv: serde_json::Value = serde_json::from_str(&round).unwrap();
    assert!(rv["imported"].as_u64().unwrap() >= 1);
}

#[test]
fn history_ffi_roundtrip() {
    let _g = guard();
    let dir = tmpdir("hist");
    let db = format!("{dir}/history.db");
    let cdb = cs(&db);
    assert_eq!(unsafe { crate::rc_hist_open(cdb.as_ptr()) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_hist_exists(cdb.as_ptr()) }, 1);
    assert_eq!(unsafe { crate::rc_hist_count() }, 0);

    let u1 = cs("http://one/");
    let t1 = cs("One");
    assert_eq!(
        unsafe { crate::rc_hist_add(u1.as_ptr(), t1.as_ptr(), 1000) },
        RcStatus::Ok
    );
    let u2 = cs("http://two/");
    let t2 = cs("Two");
    assert_eq!(
        unsafe { crate::rc_hist_add(u2.as_ptr(), t2.as_ptr(), 2000) },
        RcStatus::Ok
    );
    assert!(unsafe { crate::rc_hist_count() } >= 2);
    assert_eq!(
        unsafe { crate::rc_hist_add(ptr::null(), t1.as_ptr(), 1) },
        RcStatus::InvalidArgument
    );

    let t1b = cs("One retitled");
    assert_eq!(
        unsafe { crate::rc_hist_update_title(u1.as_ptr(), t1b.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_hist_update_title(ptr::null(), t1.as_ptr()) },
        RcStatus::InvalidArgument
    );

    let row = take_str(unsafe { crate::rc_hist_entry_at(0) }).unwrap();
    assert!(row.contains("http://two/"));
    assert!(unsafe { crate::rc_hist_entry_at(99) }.is_null());

    // Suggestions over the same open store.
    let sugg = take_str(unsafe { crate::rc_history_suggest(u1.as_ptr(), 10) });
    assert!(sugg.unwrap_or_default().contains("http://one/"));

    // Icon table.
    let host = cs("one");
    let png = b"\x89PNG fake";
    assert_eq!(
        unsafe { crate::rc_hist_icon_set(host.as_ptr(), png.as_ptr(), png.len()) },
        RcStatus::Ok
    );
    let mut icon = out_buf();
    assert_eq!(
        unsafe { crate::rc_hist_icon_get(host.as_ptr(), &mut icon) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut icon), png);
    let absent = cs("absent");
    assert_eq!(
        unsafe { crate::rc_hist_icon_get(absent.as_ptr(), &mut icon) },
        RcStatus::NotFound
    );
    assert_eq!(
        unsafe { crate::rc_hist_icon_get(ptr::null(), &mut icon) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_hist_icon_clear() }, RcStatus::Ok);
    assert_eq!(
        unsafe { crate::rc_hist_icon_get(host.as_ptr(), &mut icon) },
        RcStatus::NotFound
    );

    // Ranged + full clears and remove.
    assert_eq!(
        unsafe { crate::rc_hist_remove(u2.as_ptr(), t2.as_ptr(), 2000) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_hist_remove(ptr::null(), t2.as_ptr(), 0) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_hist_clear_since(500) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_hist_count() }, 0);
    assert_eq!(
        unsafe { crate::rc_hist_add(u1.as_ptr(), t1.as_ptr(), 1) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_hist_clear() }, RcStatus::Ok);

    // NULL path resolves to <data dir>/history.db.
    init(&dir);
    assert_eq!(unsafe { crate::rc_hist_open(ptr::null()) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_hist_exists(ptr::null()) }, 1);
}

#[test]
fn session_ffi_roundtrip() {
    let _g = guard();
    init(&tmpdir("sess"));
    let manifest = br#"{"version":1,"windows":[{"shell":"","current":0,"tabs":[{"url":"https://x/"}]}]}"#;
    assert_eq!(unsafe { crate::rc_session_exists() }, 0);
    assert_eq!(
        unsafe { crate::rc_session_save(manifest.as_ptr(), manifest.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_session_exists() }, 1);
    let mut out = out_buf();
    assert_eq!(unsafe { crate::rc_session_load(&mut out) }, RcStatus::Ok);
    let back = take_buf(&mut out);
    assert!(String::from_utf8_lossy(&back).contains("https://x/"));
    assert_eq!(unsafe { crate::rc_session_clear() }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_session_exists() }, 0);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_session_load(&mut out) },
        RcStatus::NotFound
    );

    // Codec seam both directions + bad input.
    let mut enc = out_buf();
    assert_eq!(
        unsafe { crate::rc_session_encode(manifest.as_ptr(), manifest.len(), &mut enc) },
        RcStatus::Ok
    );
    let blob = take_buf(&mut enc);
    assert!(!blob.is_empty());
    let mut dec = out_buf();
    assert_eq!(
        unsafe { crate::rc_session_decode(blob.as_ptr(), blob.len(), &mut dec) },
        RcStatus::Ok
    );
    let json = take_buf(&mut dec);
    assert!(String::from_utf8_lossy(&json).contains("https://x/"));
    assert_eq!(
        unsafe { crate::rc_session_decode(b"AR".as_ptr(), 2, &mut dec) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_session_encode(ptr::null(), 2, &mut enc) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_session_save(b"{bad".as_ptr(), 4) },
        RcStatus::Corrupt
    );
}

#[test]
fn omnibox_palette_qr_entry_points() {
    let _g = guard();
    // classifier with and without options.
    let input = cs("https://example.com/");
    let verdict =
        take_str(unsafe { crate::rc_classify_input(input.as_ptr(), ptr::null()) })
            .unwrap();
    assert!(verdict.contains("navigate"));
    let opts = cs(r#"{"keywords":["wk"],"search_fallback":true}"#);
    let kw = cs("wk hello world");
    let verdict =
        take_str(unsafe { crate::rc_classify_input(kw.as_ptr(), opts.as_ptr()) })
            .unwrap();
    assert!(!verdict.is_empty());
    assert!(unsafe { crate::rc_classify_input(ptr::null(), ptr::null()) }.is_null());

    // frecency: valid doc, malformed doc, null out.
    let spec = cs(r#"{"visits":[1000,2000],"now_ms":3000,"typed":1,"bookmarked":true}"#);
    let mut score = 0i64;
    assert_eq!(
        unsafe { crate::rc_frecency_score(spec.as_ptr(), &mut score) },
        RcStatus::Ok
    );
    assert!(score > 0);
    assert_eq!(
        unsafe { crate::rc_frecency_score(cs("{bad").as_ptr(), &mut score) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_frecency_score(spec.as_ptr(), ptr::null_mut()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_frecency_score(ptr::null(), &mut score) },
        RcStatus::InvalidArgument
    );

    // palette scorer + matcher.
    let q = cs("bm");
    let cand = cs("Bookmark Manager");
    assert!(unsafe { crate::rc_pal_score(q.as_ptr(), cand.as_ptr()) } >= 0);
    assert_eq!(unsafe { crate::rc_pal_score(ptr::null(), cand.as_ptr()) }, -1);
    let items = cs(r#"{"items":[{"match":"Bookmark Manager","id":"a"},{"match":"Other","id":"b"}],"mru":["b"]}"#);
    let ranked =
        take_str(unsafe { crate::rc_pal_match(q.as_ptr(), items.as_ptr()) }).unwrap();
    assert!(ranked.contains("\"index\":0"));
    assert!(unsafe { crate::rc_pal_match(q.as_ptr(), cs("{").as_ptr()) }.is_null());
    assert!(unsafe { crate::rc_pal_match(ptr::null(), items.as_ptr()) }.is_null());

    // QR encoder happy + bounds.
    let mut out = out_buf();
    let text = cs("https://arora.example/");
    assert_eq!(
        unsafe { crate::rc_qr_encode(text.as_ptr(), 0, &mut out) },
        RcStatus::Ok
    );
    let m = take_buf(&mut out);
    let size = u32::from_le_bytes(m[..4].try_into().unwrap()) as usize;
    assert_eq!(m.len(), 4 + size * size);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_qr_encode(text.as_ptr(), 4, &mut out) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_qr_encode(ptr::null(), 0, &mut out) },
        RcStatus::InvalidArgument
    );
}

#[test]
fn tls_ffi_entry_points() {
    let _g = guard();
    // Private/loopback hosts are refused without the flag — no
    // network touched.
    let host = cs("127.0.0.1");
    let v = take_str(unsafe { crate::rc_tls_check(host.as_ptr(), 443, 0) }).unwrap();
    assert!(v.contains("private-host") || v.contains("refused"));
    assert!(unsafe { crate::rc_tls_check(ptr::null(), 443, 0) }.is_null());
    // With the flag a refused connection reports unverified/network
    // (port 1 refuses fast — deterministic, still localhost-only).
    let v = take_str(unsafe {
        crate::rc_tls_check(host.as_ptr(), 1, crate::RC_TLS_F_ALLOW_LOCAL)
    })
    .unwrap();
    assert!(v.contains("unverified") || v.contains("warning") || v.contains("refused"));

    const CA_DER: &[u8] = include_bytes!("../data/testcerts/ca.der");
    assert_eq!(
        unsafe { crate::rc_tls_add_root(CA_DER.as_ptr(), CA_DER.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_tls_add_root(b"junk".as_ptr(), 4) },
        RcStatus::Corrupt
    );
    assert_eq!(
        unsafe { crate::rc_tls_add_root(ptr::null(), 4) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_tls_clear_roots() }, RcStatus::Ok);
}

#[test]
fn sitedecisions_full_surface() {
    let _g = guard();
    init(&tmpdir("sitedec"));
    let kind = cs("testkind");
    let key = cs("example.com");
    let val = cs("allow");

    assert_eq!(unsafe { crate::rc_sitedec_store_present() }, 0);
    assert_eq!(unsafe { crate::rc_sitedec_reset() }, RcStatus::Ok);

    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_sitedec_get(kind.as_ptr(), key.as_ptr(), &mut out) },
        RcStatus::NotFound
    );
    assert_eq!(
        unsafe { crate::rc_sitedec_get(ptr::null(), key.as_ptr(), &mut out) },
        RcStatus::InvalidArgument
    );

    assert_eq!(
        unsafe { crate::rc_sitedec_set(kind.as_ptr(), key.as_ptr(), val.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_sitedec_set(kind.as_ptr(), ptr::null(), val.as_ptr()) },
        RcStatus::InvalidArgument
    );
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_sitedec_get(kind.as_ptr(), key.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out), b"allow");

    // Suffix lookup over the same row.
    let sub = cs("www.example.com");
    let hit = take_str(unsafe { crate::rc_sitedec_lookup(kind.as_ptr(), sub.as_ptr()) });
    assert!(hit.unwrap_or_default().contains("example.com"));
    let miss = cs("elsewhere.test");
    assert!(unsafe { crate::rc_sitedec_lookup(kind.as_ptr(), miss.as_ptr()) }.is_null());
    assert!(unsafe { crate::rc_sitedec_lookup(ptr::null(), sub.as_ptr()) }.is_null());

    let listed = take_str(unsafe { crate::rc_sitedec_list(kind.as_ptr()) }).unwrap();
    assert!(listed.contains("example.com"));
    assert!(unsafe { crate::rc_sitedec_list(ptr::null()) }.is_null());
    let snap = take_str(unsafe { crate::rc_sitedec_snapshot() }).unwrap();
    assert!(snap.contains("testkind"));

    // replace_kind then persistence round-trips.
    let rows = br#"{"a.test":"1","b.test":"2"}"#;
    assert_eq!(
        unsafe { crate::rc_sitedec_replace(kind.as_ptr(), rows.as_ptr(), rows.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_sitedec_store_present() }, 1);
    assert_eq!(unsafe { crate::rc_sitedec_reload() }, RcStatus::Ok);
    let listed = take_str(unsafe { crate::rc_sitedec_list(kind.as_ptr()) }).unwrap();
    assert!(listed.contains("a.test"));
    assert_eq!(
        unsafe { crate::rc_sitedec_replace(kind.as_ptr(), ptr::null(), 2) },
        RcStatus::InvalidArgument
    );

    assert_eq!(
        unsafe { crate::rc_sitedec_remove(kind.as_ptr(), cs("a.test").as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_sitedec_clear(kind.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_sitedec_clear(ptr::null()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_sitedec_reset() }, RcStatus::Ok);
}

#[test]
fn opensearch_registry_surface() {
    let _g = guard();
    init(&tmpdir("ose"));
    assert_eq!(unsafe { crate::rc_ose_reset() }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_ose_store_present() }, 0);

    // Seed the bundled engines.
    assert_eq!(unsafe { crate::rc_ose_seed_bundled() }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_ose_store_present() }, 1);
    let names = take_str(unsafe { crate::rc_ose_bundled_names() }).unwrap();
    assert!(names.contains("DuckDuckGo"));
    let list = take_str(unsafe { crate::rc_ose_list() }).unwrap();
    assert!(list.contains("DuckDuckGo"));

    let ddg = cs("DuckDuckGo");
    let rec = take_str(unsafe { crate::rc_ose_get(ddg.as_ptr()) }).unwrap();
    assert!(rec.contains("template"));
    let ghost = cs("Ghost Engine");
    assert!(unsafe { crate::rc_ose_get(ghost.as_ptr()) }.is_null());
    assert!(unsafe { crate::rc_ose_get(ptr::null()) }.is_null());

    // put/import/remove/rename/reorder/keywords over the ABI.
    let eng = br#"{"name":"Mine","search":{"template":"http://m/?q={searchTerms}","method":"get","params":[]}}"#;
    assert_eq!(
        unsafe { crate::rc_ose_put(eng.as_ptr(), eng.len()) },
        RcStatus::Ok
    );
    // A record without name/template is refused.
    assert_ne!(
        unsafe { crate::rc_ose_put(b"{}".as_ptr(), 2) },
        RcStatus::Ok
    );
    let kw = br#"["mk","mine"]"#;
    let mine = cs("Mine");
    assert_eq!(
        unsafe { crate::rc_ose_set_keywords(mine.as_ptr(), kw.as_ptr(), kw.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_ose_set_keywords(ghost.as_ptr(), kw.as_ptr(), kw.len()) },
        RcStatus::NotFound
    );
    let mk = cs("mk");
    let owner = take_str(unsafe { crate::rc_ose_engine_for_keyword(mk.as_ptr()) });
    assert_eq!(owner.as_deref(), Some("Mine"));
    let all = take_str(unsafe { crate::rc_ose_keywords() }).unwrap();
    assert!(all.contains("mk"));
    assert!(unsafe { crate::rc_ose_engine_for_keyword(ptr::null()) }.is_null());

    let renamed = cs("Mine2");
    assert_eq!(
        unsafe { crate::rc_ose_rename(mine.as_ptr(), renamed.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_ose_rename(ghost.as_ptr(), renamed.as_ptr()) },
        RcStatus::NotFound
    );

    let order = br#"["Mine2","DuckDuckGo"]"#;
    assert_eq!(
        unsafe { crate::rc_ose_reorder(order.as_ptr(), order.len()) },
        RcStatus::Ok
    );
    let list = take_str(unsafe { crate::rc_ose_list() }).unwrap();
    assert!(list.find("Mine2").unwrap() < list.find("DuckDuckGo").unwrap());

    // Descriptor import + bundled block/unblock bookkeeping.
    let xml = br#"<?xml version="1.0"?>
<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <ShortName>Imp</ShortName>
  <Url method="get" type="text/html" template="http://imp/?q={searchTerms}"/>
</OpenSearchDescription>"#;
    assert_eq!(
        unsafe { crate::rc_ose_import(xml.as_ptr(), xml.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_ose_import(b"garbage".as_ptr(), 7) },
        RcStatus::Corrupt
    );

    assert_eq!(
        unsafe { crate::rc_ose_block_bundled(ddg.as_ptr()) },
        RcStatus::Ok
    );
    let removed = take_str(unsafe { crate::rc_ose_removed_bundled() }).unwrap();
    assert!(removed.contains("DuckDuckGo"));
    assert_eq!(
        unsafe { crate::rc_ose_unblock_bundled(ddg.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_ose_restore_bundled() },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_ose_remove(ghost.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_ose_remove(ptr::null()) },
        RcStatus::InvalidArgument
    );

    // URL expansion paths.
    let spec = br#"{"template":"http://e/?q={searchTerms}","method":"get","params":[],"term":"a b","language":"en","source":""}"#;
    let url = take_str(unsafe { crate::rc_ose_expand(spec.as_ptr(), spec.len()) });
    assert!(url.unwrap_or_default().contains("a%20b"));
    assert!(unsafe { crate::rc_ose_expand(b"{}".as_ptr(), 2) }.is_null());
    assert!(unsafe { crate::rc_ose_expand(ptr::null(), 2) }.is_null());

    let kind = cs("search");
    let term = cs("hello world");
    let lang = cs("en");
    let empty = cs("");
    let url = take_str(unsafe {
        crate::rc_ose_url(ddg.as_ptr(), kind.as_ptr(), term.as_ptr(), lang.as_ptr(), empty.as_ptr())
    });
    assert!(url.is_some());
    assert!(unsafe {
        crate::rc_ose_url(ghost.as_ptr(), kind.as_ptr(), term.as_ptr(), lang.as_ptr(), empty.as_ptr())
    }
    .is_null());
    assert!(unsafe {
        crate::rc_ose_url(ptr::null(), kind.as_ptr(), term.as_ptr(), lang.as_ptr(), empty.as_ptr())
    }
    .is_null());
    let url = take_str(unsafe {
        crate::rc_ose_keyword_url(mk.as_ptr(), term.as_ptr(), lang.as_ptr(), empty.as_ptr())
    });
    assert!(url.unwrap_or_default().contains("http://m/?q="));
    let unbound = cs("nope-kw");
    assert!(unsafe {
        crate::rc_ose_keyword_url(unbound.as_ptr(), term.as_ptr(), lang.as_ptr(), empty.as_ptr())
    }
    .is_null());

    assert_eq!(unsafe { crate::rc_ose_reset() }, RcStatus::Ok);
}

#[test]
fn useragent_and_clearplan_entry_points() {
    let _g = guard();
    init(&tmpdir("ua"));

    let ctx = br#"{"factory_ua":"Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) QtWebEngine/6.12.0 Chrome/130.0.0.0 Safari/537.36","presented_major":141}"#;
    let ua = take_str(unsafe { crate::rc_ua_build(ctx.as_ptr(), ctx.len()) }).unwrap();
    assert!(ua.contains("Chrome/141"));
    assert!(!ua.contains("QtWebEngine"));
    assert!(unsafe { crate::rc_ua_build(b"{".as_ptr(), 1) }.is_null());
    assert!(unsafe { crate::rc_ua_build(ptr::null(), 1) }.is_null());

    let ua_c = cs(&ua);
    let eng = cs("6.12.0");
    let brand = take_str(unsafe {
        crate::rc_ua_brand_version(ua_c.as_ptr(), eng.as_ptr())
    });
    assert_eq!(brand.as_deref(), Some("141.12.0"));
    let nochrom = cs("Lynx/2.9");
    let brand = take_str(unsafe {
        crate::rc_ua_brand_version(nochrom.as_ptr(), eng.as_ptr())
    });
    assert_eq!(brand.as_deref(), Some(""));

    let presets = br#"<useragentswitcher>
        <separator/>
        <useragent description="Chrome" useragent="Mozilla/5.0 Chrome/141"/>
    </useragentswitcher>"#;
    let arr = take_str(unsafe { crate::rc_ua_presets(presets.as_ptr(), presets.len()) })
        .unwrap();
    assert!(arr.contains("Chrome"));
    assert!(unsafe { crate::rc_ua_presets(ptr::null(), 2) }.is_null());

    // Per-site spoof table (a sitedecisions kind).
    let host = cs("tracker.example");
    let spoof = cs("Mozilla/5.0 Fake");
    assert_eq!(
        unsafe { crate::rc_ua_spoof_set(host.as_ptr(), spoof.as_ptr()) },
        RcStatus::Ok
    );
    let sub = cs("www.tracker.example");
    let got = take_str(unsafe { crate::rc_ua_spoof(sub.as_ptr()) });
    assert_eq!(got.as_deref(), Some("Mozilla/5.0 Fake"));
    let list = take_str(unsafe { crate::rc_ua_spoof_list() }).unwrap();
    assert!(list.contains("tracker.example"));
    let inj = cs("bad\r\nvalue");
    assert_eq!(
        unsafe { crate::rc_ua_spoof_set(host.as_ptr(), inj.as_ptr()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_ua_spoof_remove(host.as_ptr()) },
        RcStatus::Ok
    );
    assert!(unsafe { crate::rc_ua_spoof(sub.as_ptr()) }.is_null());
    assert_eq!(
        unsafe { crate::rc_ua_spoof_set(ptr::null(), spoof.as_ptr()) },
        RcStatus::InvalidArgument
    );

    // clearplan over the ABI: valid periods, OTR scope, bad input.
    let all = cs("all");
    let plan = take_str(unsafe { crate::rc_clearplan(all.as_ptr(), 0xFF, 0) }).unwrap();
    assert!(plan.contains("\"ops\""));
    let hour = cs("last-hour");
    let plan = take_str(unsafe { crate::rc_clearplan(hour.as_ptr(), 0x01, 0) }).unwrap();
    assert!(plan.contains("\"since_secs\""));
    let plan = take_str(unsafe { crate::rc_clearplan(all.as_ptr(), 0xFF, 1) }).unwrap();
    assert!(plan.contains("\"scope\":\"otr\""));
    let bogus = cs("fortnight");
    assert!(unsafe { crate::rc_clearplan(bogus.as_ptr(), 0xFF, 0) }.is_null());
    assert!(unsafe { crate::rc_clearplan(ptr::null(), 0xFF, 0) }.is_null());
}
