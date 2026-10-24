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
    let dir = tmpdir("credaf");
    init(&dir);

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

    // Addresses: the ADDR01/02 tail shares the sealed payload.
    let addrs = take_str(unsafe { crate::rc_autofill_addresses() });
    assert_eq!(addrs.as_deref(), Some("[]"));
    let arecs = br#"[{"name":"Ada","country":"US","street":"1 Main St","last_used":7}]"#;
    assert_eq!(
        unsafe { crate::rc_autofill_set_addresses(arecs.as_ptr(), arecs.len()) },
        RcStatus::Ok
    );
    let listed = take_str(unsafe { crate::rc_autofill_addresses() }).unwrap();
    assert!(listed.contains("Ada") && listed.contains("last_used\":7"));
    // Forms are untouched by the address replace.
    let listed = take_str(unsafe { crate::rc_autofill_forms() }).unwrap();
    assert!(listed.contains("https://f/"));
    assert_eq!(
        unsafe { crate::rc_autofill_set_addresses(b"[42]".as_ptr(), 4) },
        RcStatus::InvalidArgument
    );

    // RC17 custody leg over the real ABI: the sealed payload is
    // 0600 and the store dir was tightened to 0700 (the tmpdir
    // starts at the umask default, typically 0755).
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let mode = |p: &std::path::Path| {
            std::fs::metadata(p).unwrap().permissions().mode() & 0o777
        };
        let store_file = std::path::Path::new(&dir).join("autofill-store.dat");
        assert_eq!(mode(&store_file), 0o600);
        assert_eq!(mode(std::path::Path::new(&dir)), 0o700);
    }

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
    // Per-file override probe: null/absent names read as 0.
    assert_eq!(unsafe { crate::rc_tranco_file_entries(ptr::null()) }, 0);
    let missing = cs("no-such-file.csv");
    assert_eq!(unsafe { crate::rc_tranco_file_entries(missing.as_ptr()) }, 0);

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

    // Site-trust composite over the FFI: threat -> red, ranked+https
    // -> green, ranked+http / unknown -> amber, reload re-merges.
    assert!(unsafe { crate::rc_sitetrust_lookup(ptr::null(), 1) }.is_null());
    let body = b"threat.example\n";
    assert_eq!(
        unsafe { crate::rc_blocklist_load(body.as_ptr(), body.len()) },
        RcStatus::Ok
    );
    let threat = cs("threat.example");
    let j = take_str(unsafe { crate::rc_sitetrust_lookup(threat.as_ptr(), 1) })
        .unwrap();
    assert!(j.contains("\"level\":\"red\""), "{}", j);
    assert!(j.contains("\"threat\""), "{}", j);
    let rows = b"2,ranked.example\n";
    assert_eq!(
        unsafe { crate::rc_tranco_load(rows.as_ptr(), rows.len()) },
        RcStatus::Ok
    );
    let ranked = cs("ranked.example");
    let j = take_str(unsafe { crate::rc_sitetrust_lookup(ranked.as_ptr(), 1) })
        .unwrap();
    assert!(j.contains("\"level\":\"green\""), "{}", j);
    let j = take_str(unsafe { crate::rc_sitetrust_lookup(ranked.as_ptr(), 0) })
        .unwrap();
    assert!(j.contains("\"level\":\"amber\""), "{}", j);
    let j = take_str(unsafe { crate::rc_sitetrust_lookup(plain.as_ptr(), 1) })
        .unwrap();
    assert!(j.contains("\"level\":\"amber\""), "{}", j);
    // TRUST03: the opts variant — masking the threat probe off reads
    // the same listed host amber with no "threat" signal at all.
    let j = take_str(unsafe {
        crate::rc_sitetrust_lookup_opts(
            threat.as_ptr(),
            1,
            crate::sitetrust::PROBE_ALL & !crate::sitetrust::PROBE_THREATLIST,
        )
    })
    .unwrap();
    assert!(j.contains("\"level\":\"amber\""), "{}", j);
    assert!(!j.contains("threat"), "{}", j);
    // The composite reload re-merges both seeds over the fixtures.
    assert_eq!(unsafe { crate::rc_sitetrust_reload() }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_tranco_rank(google.as_ptr()) }, 1);
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
    assert!(String::from_utf8_lossy(&verdict).contains("\"lang\""));
    // Repeat calls over the same input return byte-identical verdicts —
    // the reader overlay must never render a different article on retry.
    let mut out2 = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_readability_extract(
                article.as_ptr(),
                article.len(),
                base.as_ptr(),
                &mut out2,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out2), verdict);
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

    // Top-hosts ranking over the same store — the most-visited host
    // leads with its newest visit as the representative entry.
    let tops = take_str(unsafe { crate::rc_history_top_hosts(10) });
    assert!(tops.unwrap_or_default().contains("one"));

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

    // RC04: the filtered-listing query surface.
    let q_all = take_str(unsafe { crate::rc_hist_query(cs("{}").as_ptr()) })
        .unwrap();
    let all: serde_json::Value = serde_json::from_str(&q_all).unwrap();
    assert_eq!(all.as_array().unwrap().len(), 2);
    let term = take_str(unsafe {
        crate::rc_hist_query(cs(r#"{"term":"two"}"#).as_ptr())
    })
    .unwrap();
    assert_eq!(
        serde_json::from_str::<serde_json::Value>(&term).unwrap()
            .as_array()
            .unwrap()
            .len(),
        1
    );
    let ranged = take_str(unsafe {
        crate::rc_hist_query(
            cs(r#"{"from_ms":1500,"until_ms":2500}"#).as_ptr(),
        )
    })
    .unwrap();
    let ranged: serde_json::Value = serde_json::from_str(&ranged).unwrap();
    assert_eq!(ranged.as_array().unwrap().len(), 1);
    assert_eq!(ranged[0]["url"], "http://two/");
    let page = take_str(unsafe {
        crate::rc_hist_query(cs(r#"{"offset":1,"limit":1}"#).as_ptr())
    })
    .unwrap();
    let page: serde_json::Value = serde_json::from_str(&page).unwrap();
    assert_eq!(page.as_array().unwrap().len(), 1);
    assert_eq!(page[0]["url"], "http://one/");
    // Malformed specs are errors, never "match all".
    assert!(unsafe { crate::rc_hist_query(cs("[1]").as_ptr()) }.is_null());
    assert!(unsafe { crate::rc_hist_query(ptr::null()) }.is_null());

    // RC04: bulk retention expiry returns the deleted count and
    // respects the inclusive boundary.
    let mut removed: i64 = -1;
    assert_eq!(
        unsafe { crate::rc_hist_expire_older_than(999, &mut removed) },
        RcStatus::Ok
    );
    assert_eq!(removed, 0);
    assert_eq!(
        unsafe {
            crate::rc_hist_expire_older_than(1000, ptr::null_mut())
        },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_hist_count() }, 1);

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

    // RC13 lifecycle marker over the same anchored dir: arm -> dirty,
    // a second start reports the unclean run and counts it, a clean
    // exit disarms; the restore guard is its own flag.
    assert_eq!(unsafe { crate::rc_session_dirty() }, 0);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_session_last_exit(&mut out) },
        RcStatus::NotFound
    );
    assert_eq!(unsafe { crate::rc_session_mark_start() }, 0);
    assert_eq!(unsafe { crate::rc_session_dirty() }, 1);
    assert_eq!(unsafe { crate::rc_session_mark_start() }, 1);
    assert_eq!(unsafe { crate::rc_session_crash_count() }, 1);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_session_last_exit(&mut out) },
        RcStatus::Ok
    );
    let rec = take_buf(&mut out);
    assert_eq!(
        String::from_utf8_lossy(&rec),
        "unclean (crash or external kill)"
    );
    assert_eq!(unsafe { crate::rc_session_mark_exit(0) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_session_dirty() }, 0);
    assert_eq!(unsafe { crate::rc_session_crash_count() }, 0);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_session_last_exit(&mut out) },
        RcStatus::Ok
    );
    let rec = take_buf(&mut out);
    assert_eq!(String::from_utf8_lossy(&rec), "clean rc=0");
    assert_eq!(
        unsafe { crate::rc_session_set_restoring(1) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_session_restoring() }, 1);
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
    let text = cs("https://arktos.example/");
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

#[test]
fn profilevault_surface() {
    let _g = guard();
    let base = tmpdir("pvault");
    let src = format!("{base}/src");
    std::fs::create_dir_all(format!("{src}/sub")).unwrap();
    std::fs::write(format!("{src}/a.txt"), b"payload-one").unwrap();
    std::fs::write(format!("{src}/sub/b.txt"), b"payload-two").unwrap();
    let vault = format!("{base}/v.arv");
    let out = format!("{base}/out");

    let dir = cs(&src);
    let vpath = cs(&vault);
    let outd = cs(&out);
    let pw = b"vault-pw";

    // Bad-argument forms first.
    assert_eq!(
        unsafe {
            crate::rv_seal(ptr::null(), pw.as_ptr(), pw.len(), 0x01, vpath.as_ptr(), None, ptr::null_mut())
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rv_seal(dir.as_ptr(), pw.as_ptr(), pw.len(), 0x77, vpath.as_ptr(), None, ptr::null_mut())
        },
        RcStatus::UnsupportedAlgo
    );
    assert_eq!(
        unsafe {
            crate::rv_seal(dir.as_ptr(), ptr::null(), 4, 0x01, vpath.as_ptr(), None, ptr::null_mut())
        },
        RcStatus::InvalidArgument
    );
    assert!(unsafe { crate::rv_inspect(ptr::null()) }.is_null());
    assert_eq!(
        unsafe { crate::rv_verify(ptr::null(), pw.as_ptr(), pw.len()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rv_unseal(vpath.as_ptr(), pw.as_ptr(), pw.len(), ptr::null(), None, ptr::null_mut())
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rv_rewrap(vpath.as_ptr(), pw.as_ptr(), pw.len(), ptr::null(), 2)
        },
        RcStatus::InvalidArgument
    );

    // Happy path over the C ABI: seal -> inspect -> verify -> unseal.
    assert_eq!(
        unsafe {
            crate::rv_seal(dir.as_ptr(), pw.as_ptr(), pw.len(), 0x01, vpath.as_ptr(), None, ptr::null_mut())
        },
        RcStatus::Ok
    );
    let j = take_str(unsafe { crate::rv_inspect(vpath.as_ptr()) }).unwrap();
    assert!(j.contains("\"algo\":1"), "{j}");
    assert!(j.contains("\"kdf\":\"argon2id\""), "{j}");
    assert_eq!(
        unsafe { crate::rv_verify(vpath.as_ptr(), pw.as_ptr(), pw.len()) },
        RcStatus::Ok
    );
    let bad = b"nope";
    assert_eq!(
        unsafe { crate::rv_verify(vpath.as_ptr(), bad.as_ptr(), bad.len()) },
        RcStatus::WrongPassword
    );
    assert_eq!(
        unsafe {
            crate::rv_unseal(vpath.as_ptr(), pw.as_ptr(), pw.len(), outd.as_ptr(), None, ptr::null_mut())
        },
        RcStatus::Ok
    );
    assert_eq!(
        std::fs::read(format!("{out}/a.txt")).unwrap(),
        b"payload-one"
    );

    // Rewrap: old password rejected afterwards, new one opens.
    let np = b"new-vault-pw";
    assert_eq!(
        unsafe { crate::rv_rewrap(vpath.as_ptr(), pw.as_ptr(), pw.len(), np.as_ptr(), np.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rv_verify(vpath.as_ptr(), pw.as_ptr(), pw.len()) },
        RcStatus::WrongPassword
    );
    assert_eq!(
        unsafe { crate::rv_verify(vpath.as_ptr(), np.as_ptr(), np.len()) },
        RcStatus::Ok
    );
    let out2 = format!("{base}/out2");
    let outd2 = cs(&out2);
    assert_eq!(
        unsafe {
            crate::rv_unseal(vpath.as_ptr(), np.as_ptr(), np.len(), outd2.as_ptr(), None, ptr::null_mut())
        },
        RcStatus::Ok
    );
    assert_eq!(
        std::fs::read(format!("{out2}/sub/b.txt")).unwrap(),
        b"payload-two"
    );
}

// RC02: the typed per-profile settings store — handle lifecycle,
// every typed get/set, ini import, and the bad-argument paths.
#[test]
fn settings_surface() {
    let _g = guard();
    let root = tmpdir("settings");
    let croot = cs(&root);

    // Bad-argument forms first.
    assert!(unsafe { crate::rc_settings_open(ptr::null(), ptr::null()) }.is_null());
    assert!(take_str(unsafe { crate::rc_last_error_message() })
        .unwrap()
        .contains("data root"));
    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), ptr::null()) };
    assert!(!h.is_null());
    let key = cs("privacy/securityLevel");
    assert_eq!(
        unsafe { crate::rc_settings_get_bool(ptr::null_mut(), key.as_ptr(), ptr::null_mut()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_settings_get_bool(h, ptr::null(), ptr::null_mut()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_settings_get_bool(h, key.as_ptr(), ptr::null_mut()) },
        RcStatus::NotFound
    );
    assert_eq!(
        unsafe { crate::rc_settings_set_string(h, key.as_ptr(), ptr::null()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_settings_import_ini(h, ptr::null()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_settings_contains(ptr::null_mut(), key.as_ptr()) }, 0);

    // Typed writes + typed reads over the C ABI.
    let bval = cs("bools/t");
    assert_eq!(
        unsafe { crate::rc_settings_set_bool(h, bval.as_ptr(), 1) },
        RcStatus::Ok
    );
    let mut b = 0i32;
    assert_eq!(
        unsafe { crate::rc_settings_get_bool(h, bval.as_ptr(), &mut b) },
        RcStatus::Ok
    );
    assert_eq!(b, 1);

    assert_eq!(
        unsafe { crate::rc_settings_set_int(h, key.as_ptr(), 2) },
        RcStatus::Ok
    );
    let mut i = 0i64;
    assert_eq!(
        unsafe { crate::rc_settings_get_int(h, key.as_ptr(), &mut i) },
        RcStatus::Ok
    );
    assert_eq!(i, 2);
    assert_eq!(unsafe { crate::rc_settings_contains(h, key.as_ptr()) }, 1);

    let dkey = cs("num/zoom");
    assert_eq!(
        unsafe { crate::rc_settings_set_double(h, dkey.as_ptr(), 1.5) },
        RcStatus::Ok
    );
    let mut d = 0.0f64;
    assert_eq!(
        unsafe { crate::rc_settings_get_double(h, dkey.as_ptr(), &mut d) },
        RcStatus::Ok
    );
    assert_eq!(d, 1.5);

    let skey = cs("MainWindow/home");
    let sval = cs("https://example.test/");
    assert_eq!(
        unsafe { crate::rc_settings_set_string(h, skey.as_ptr(), sval.as_ptr()) },
        RcStatus::Ok
    );
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_settings_get_string(h, skey.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out), b"https://example.test/");
    // The typed JSON accessor.
    assert_eq!(
        unsafe { crate::rc_settings_get(h, skey.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert!(String::from_utf8(take_buf(&mut out))
        .unwrap()
        .contains("\"t\":\"s\""));

    let blob = [0xde, 0xad, 0xbe, 0xef];
    let bkey = cs("MainWindow/geometry");
    assert_eq!(
        unsafe {
            crate::rc_settings_set_bytes(h, bkey.as_ptr(), blob.as_ptr(), blob.len())
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_settings_get_bytes(h, bkey.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out), blob);

    let lkey = cs("searchShortcuts/engines");
    let list = br#"["ddg","g"]"#;
    assert_eq!(
        unsafe {
            crate::rc_settings_set_list(h, lkey.as_ptr(), list.as_ptr(), list.len())
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_settings_get_list(h, lkey.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert!(String::from_utf8(take_buf(&mut out))
        .unwrap()
        .contains("ddg"));
    // A malformed list payload is rejected without touching the store.
    assert_eq!(
        unsafe {
            crate::rc_settings_set_list(h, lkey.as_ptr(), b"nope".as_ptr(), 4)
        },
        RcStatus::InvalidArgument
    );

    // list-prefix + remove + sync + ini import + close.
    let listed = take_str(unsafe {
        crate::rc_settings_list(h, cs("privacy").as_ptr())
    })
    .unwrap();
    assert_eq!(listed, r#"["privacy/securityLevel"]"#);
    let all = take_str(unsafe { crate::rc_settings_list(h, ptr::null()) }).unwrap();
    assert!(all.contains("MainWindow/home"));
    assert_eq!(
        unsafe { crate::rc_settings_remove(h, key.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_settings_contains(h, key.as_ptr()) }, 0);
    assert_eq!(unsafe { crate::rc_settings_sync(h) }, RcStatus::Ok);

    // The real-QSettings fixture: profile ini under profiles/<id>/
    // imports on first open through the C ABI itself.
    let inid = format!("{root}/profiles/fixture");
    std::fs::create_dir_all(&inid).unwrap();
    std::fs::copy(
        concat!(env!("CARGO_MANIFEST_DIR"), "/testdata/profile-settings.ini"),
        format!("{inid}/settings.ini"),
    )
    .unwrap();
    let pid = cs("fixture");
    let ph = unsafe { crate::rc_settings_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!ph.is_null());
    let fk = cs("history/historyLimit");
    assert_eq!(
        unsafe { crate::rc_settings_get_int(ph, fk.as_ptr(), &mut i) },
        RcStatus::Ok
    );
    assert_eq!(i, 30);
    let bk = cs("MainWindow/geometry");
    assert_eq!(
        unsafe { crate::rc_settings_get_bytes(ph, bk.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out)[..2], [0x01, 0xd9]);

    // Explicit import of a "global" ini filters by scope and fills
    // absent keys only — the fixture's securityLevel=2 wins.
    let gini = format!("{root}/global.conf");
    std::fs::write(&gini, "[privacy]\nsecurityLevel=9\nextraKey=9\n\n[MainWindow]\ndefaultEngine=servo\n").unwrap();
    let gpath = cs(&gini);
    assert_eq!(
        unsafe { crate::rc_settings_import_ini(ph, gpath.as_ptr()) },
        RcStatus::Ok
    );
    let ek = cs("MainWindow/defaultEngine");
    assert_eq!(unsafe { crate::rc_settings_contains(ph, ek.as_ptr()) }, 0);
    let pk = cs("privacy/securityLevel");
    assert_eq!(
        unsafe { crate::rc_settings_get_int(ph, pk.as_ptr(), &mut i) },
        RcStatus::Ok
    );
    assert_eq!(i, 2);
    let xk = cs("privacy/extraKey");
    assert_eq!(
        unsafe { crate::rc_settings_get_int(ph, xk.as_ptr(), &mut i) },
        RcStatus::Ok
    );
    assert_eq!(i, 9);

    unsafe {
        crate::rc_settings_close(ph);
        crate::rc_settings_close(h);
        crate::rc_settings_close(ptr::null_mut());
    }
    let _ = std::fs::remove_dir_all(&root);
}

// RC19: the container registry + namespace map over the C ABI —
// CRUD on a profile-bound settings handle, the C++ layout's keys
// landing byte-for-byte, and the derivation entry points.
#[test]
fn containers_surface() {
    let _g = guard();
    let root = tmpdir("containers");
    let croot = cs(&root);
    let pid = cs("work");

    // Bad handles/args degrade, never crash.
    assert!(unsafe { crate::rc_containers_list(ptr::null_mut()) }.is_null());
    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!h.is_null());
    assert!(unsafe { crate::rc_containers_get(h, ptr::null()) }.is_null());
    assert!(unsafe {
        crate::rc_containers_create(h, ptr::null(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_containers_create(h, b"nope".as_ptr(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_containers_create(ptr::null_mut(), b"{}".as_ptr(), 2)
    }
    .is_null());
    // An empty registry reads as [].
    let list = take_str(unsafe { crate::rc_containers_list(h) }).unwrap();
    assert_eq!(list, "[]");

    // create -> the row JSON carries the minted uuid-shape id.
    let spec = b"{\"name\":\"Work\",\"color\":\"#ff8800\",\"icon\":\"w\"}";
    let made = take_str(unsafe {
        crate::rc_containers_create(h, spec.as_ptr(), spec.len())
    })
    .unwrap();
    let row: serde_json::Value = serde_json::from_str(&made).unwrap();
    let id = row["id"].as_str().unwrap().to_string();
    assert_eq!(id.len(), 36);
    assert!(id.chars().all(|c| c.is_ascii_hexdigit() || c == '-'));
    assert_eq!(row["name"], "Work");
    assert_eq!(row["icon"], "w");
    // Blank names refuse.
    let blank = b"{\"name\":\" \"}";
    assert!(unsafe {
        crate::rc_containers_create(h, blank.as_ptr(), blank.len())
    }
    .is_null());

    // The C++ layout's keys landed: order list + per-id fields.
    let ok = cs("containers/order");
    let mut buf = out_buf();
    assert_eq!(
        unsafe { crate::rc_settings_get_list(h, ok.as_ptr(), &mut buf) },
        RcStatus::Ok
    );
    let order: Vec<String> =
        serde_json::from_slice(&take_buf(&mut buf)).unwrap();
    assert_eq!(order, vec![id.clone()]);
    let cid = cs(&id);
    let got = take_str(unsafe { crate::rc_containers_get(h, cid.as_ptr()) });
    let row: serde_json::Value =
        serde_json::from_str(&got.unwrap()).unwrap();
    assert_eq!(row["color"], "#ff8800");

    // update: rename + recolor + clear the icon; unknown ids 404.
    let patch = b"{\"name\":\"Vault\",\"icon\":null}";
    assert_eq!(
        unsafe {
            crate::rc_containers_update(h, cid.as_ptr(), patch.as_ptr(), patch.len())
        },
        RcStatus::Ok
    );
    let row: serde_json::Value = serde_json::from_str(
        &take_str(unsafe { crate::rc_containers_get(h, cid.as_ptr()) })
            .unwrap(),
    )
    .unwrap();
    assert_eq!(row["name"], "Vault");
    assert_eq!(row["color"], "#ff8800");
    assert_eq!(row["icon"], "");
    assert_eq!(
        unsafe {
            crate::rc_containers_update(
                h,
                cs("ghost").as_ptr(),
                patch.as_ptr(),
                patch.len(),
            )
        },
        RcStatus::NotFound
    );
    assert_eq!(
        unsafe {
            crate::rc_containers_update(h, cid.as_ptr(), b"nope".as_ptr(), 4)
        },
        RcStatus::InvalidArgument
    );
    // An unregistered id reads as NULL without an error.
    assert!(unsafe { crate::rc_containers_get(h, cs("ghost").as_ptr()) }
        .is_null());

    // The namespace derivation — the ContainerManager path contract
    // verbatim plus the cookie-partition key.
    let ns = take_str(unsafe {
        crate::rc_containers_namespace(croot.as_ptr(), pid.as_ptr(), cid.as_ptr())
    })
    .unwrap();
    let ns: serde_json::Value = serde_json::from_str(&ns).unwrap();
    assert_eq!(
        ns["storage_name"],
        format!("arktos-container-work-{id}")
    );
    assert_eq!(
        ns["storage_path"],
        format!("{root}/profiles/work/containers/{id}")
    );
    assert_eq!(
        ns["cache_path"],
        format!("{root}/profiles/work/cache/containers/{id}")
    );
    assert_eq!(
        ns["default_data_path"],
        format!("{root}/QtWebEngine/arktos-container-work-{id}")
    );
    assert_eq!(ns["cookie_partition"], id);
    // The default container and bad ids have no namespace.
    assert!(unsafe {
        crate::rc_containers_namespace(croot.as_ptr(), pid.as_ptr(), cs("").as_ptr())
    }
    .is_null());
    assert!(unsafe {
        crate::rc_containers_namespace(
            ptr::null(),
            pid.as_ptr(),
            cid.as_ptr(),
        )
    }
    .is_null());

    // remove -> group + order entry gone; second remove 404s.
    assert_eq!(
        unsafe { crate::rc_containers_remove(h, cid.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_containers_remove(h, cid.as_ptr()) },
        RcStatus::NotFound
    );
    assert_eq!(
        unsafe { crate::rc_containers_remove(h, ptr::null()) },
        RcStatus::InvalidArgument
    );
    let list = take_str(unsafe { crate::rc_containers_list(h) }).unwrap();
    assert_eq!(list, "[]");

    unsafe { crate::rc_settings_close(h) };
    let _ = std::fs::remove_dir_all(&root);
}

// RC03: the profile registry — handle lifecycle, CRUD, the ambient
// id and the BrowserPaths-mirroring path accessors, all over the C
// ABI.
#[test]
fn profiles_surface() {
    let _g = guard();
    let root = tmpdir("profiles");
    let croot = cs(&root);
    let seed = cs("Seeded");

    // Bad opens return NULL and stash the reason.
    assert!(unsafe { crate::rc_profiles_open(ptr::null(), ptr::null()) }.is_null());
    let bad = CString::new([b'n', b'o', 0xff]).unwrap();
    assert!(
        unsafe { crate::rc_profiles_open(croot.as_ptr(), bad.as_ptr()) }.is_null()
    );

    let h = unsafe { crate::rc_profiles_open(croot.as_ptr(), seed.as_ptr()) };
    assert!(!h.is_null());

    // Fresh open materializes the default row — seeded name.
    let listed = take_str(unsafe { crate::rc_profiles_list(h) }).unwrap();
    assert!(listed.contains("\"id\":\"default\""));
    assert!(listed.contains("\"name\":\"Seeded\""));
    assert!(std::path::Path::new(&format!("{root}/profiles.json")).exists());
    let file = take_str(unsafe { crate::rc_profiles_file(h) }).unwrap();
    assert_eq!(file, format!("{root}/profiles.json"));

    // Bad-handle / bad-pointer reads degrade, never crash.
    assert!(take_str(unsafe { crate::rc_profiles_list(ptr::null_mut()) }).is_none());
    assert!(take_str(unsafe { crate::rc_profiles_get(h, ptr::null()) }).is_none());
    assert!(take_str(unsafe { crate::rc_profiles_get(h, cs("ghost").as_ptr()) }).is_none());
    assert_eq!(
        unsafe { crate::rc_profiles_rename(h, ptr::null(), cs("x").as_ptr()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rc_profiles_rename(
                h,
                cs("ghost").as_ptr(),
                cs("x").as_ptr()
            )
        },
        RcStatus::NotFound
    );

    // create -> unique slug id; the row reads back over the ABI.
    let new_id = take_str(unsafe {
        crate::rc_profiles_create(h, cs("Work Stuff!").as_ptr())
    })
    .unwrap();
    assert_eq!(new_id, "work-stuff");
    let row = take_str(unsafe {
        crate::rc_profiles_get(h, cs("work-stuff").as_ptr())
    })
    .unwrap();
    assert!(row.contains("\"name\":\"Work Stuff!\""));
    assert!(std::path::Path::new(&format!("{root}/profiles/work-stuff"))
        .is_dir());
    assert!(take_str(unsafe {
        crate::rc_profiles_create(h, ptr::null())
    })
    .is_none());

    // rename + avatar + path accessors.
    assert_eq!(
        unsafe {
            crate::rc_profiles_rename(h, cs("work-stuff").as_ptr(), cs("Day Job").as_ptr())
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe {
            crate::rc_profiles_set_avatar(
                h,
                cs("work-stuff").as_ptr(),
                cs("../evil.png").as_ptr()
            )
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rc_profiles_set_avatar(
                h,
                cs("work-stuff").as_ptr(),
                cs("me.png").as_ptr()
            )
        },
        RcStatus::Ok
    );
    let dir = take_str(unsafe {
        crate::rc_profiles_dir(h, cs("work-stuff").as_ptr())
    })
    .unwrap();
    assert_eq!(dir, format!("{root}/profiles/work-stuff"));
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_cache_dir(h, cs("w").as_ptr()) }).unwrap(),
        format!("{root}/profiles/w/cache")
    );
    assert_eq!(
        take_str(unsafe {
            crate::rc_profiles_engine_storage_dir(h, cs("w").as_ptr())
        })
        .unwrap(),
        format!("{root}/profiles/w/QtWebEngine")
    );
    assert_eq!(
        take_str(unsafe {
            crate::rc_profiles_engine_cache_dir(h, cs("w").as_ptr())
        })
        .unwrap(),
        format!("{root}/profiles/w/cache/QtWebEngine")
    );
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_settings_dir(h, cs("w").as_ptr()) }).unwrap(),
        format!("{root}/profiles/w")
    );
    assert_eq!(
        take_str(unsafe {
            crate::rc_profiles_avatar_path(h, cs("work-stuff").as_ptr())
        })
        .unwrap(),
        format!("{root}/profiles/work-stuff/me.png")
    );
    // A non-slug id resolves to "default", never escapes the tree.
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_dir(h, cs("../x").as_ptr()) }).unwrap(),
        format!("{root}/profiles/default")
    );
    // NULL id resolves to default too.
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_dir(h, ptr::null()) }).unwrap(),
        format!("{root}/profiles/default")
    );

    // Ambient persona — per-handle process state.
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_active(h) }).unwrap(),
        "default"
    );
    assert_eq!(
        unsafe { crate::rc_profiles_set_active(h, cs("work-stuff").as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_active(h) }).unwrap(),
        "work-stuff"
    );
    assert_eq!(
        unsafe { crate::rc_profiles_set_active(h, cs("../nope").as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_active(h) }).unwrap(),
        "default"
    );

    // slugify over the ABI; NULL tolerated.
    assert_eq!(
        take_str(unsafe { crate::rc_profiles_slugify(cs("My Stuff").as_ptr()) }).unwrap(),
        "my-stuff"
    );
    assert!(take_str(unsafe { crate::rc_profiles_slugify(ptr::null()) }).is_none());

    // delete refuses default/non-slug, removes a real profile's row
    // and dir.
    assert_eq!(
        unsafe { crate::rc_profiles_delete(h, cs("default").as_ptr()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_profiles_delete(h, cs("work-stuff").as_ptr()) },
        RcStatus::Ok
    );
    assert!(!std::path::Path::new(&format!("{root}/profiles/work-stuff")).exists());
    assert!(take_str(unsafe {
        crate::rc_profiles_get(h, cs("work-stuff").as_ptr())
    })
    .is_none());
    assert_eq!(
        unsafe { crate::rc_profiles_delete(h, cs("work-stuff").as_ptr()) },
        RcStatus::NotFound
    );
    assert_eq!(
        unsafe { crate::rc_profiles_reload(h) },
        RcStatus::Ok
    );

    unsafe {
        crate::rc_profiles_close(h);
        crate::rc_profiles_close(ptr::null_mut());
    }
    let _ = std::fs::remove_dir_all(&root);
}

// ---- RC15: per-profile cookie database --------------------------------

#[test]
fn cookies_ffi_roundtrip() {
    let _g = guard();
    let dir = tmpdir("rc-cookies");
    init(&dir);

    // Bad-input legs: every entry point degrades, none crash.  (The
    // store is process-global — a prior test may have left it open,
    // so the not-initialized legs live in the module's own tests.)
    assert_eq!(
        unsafe { crate::rc_cookies_set(b"{}".as_ptr(), 2) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_cookies_set(ptr::null(), 0) },
        RcStatus::InvalidArgument
    );

    // Open on the anchored data dir (NULL path).
    assert_eq!(
        unsafe { crate::rc_cookies_open(ptr::null()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_cookies_is_open() }, 1);
    assert!(std::path::Path::new(&dir).join("cookies.db").exists());
    assert_eq!(unsafe { crate::rc_cookies_role() }, 0); // authoritative
    assert_eq!(unsafe { crate::rc_cookies_set_role(1) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_cookies_role() }, 1); // mirror
    assert_eq!(
        unsafe { crate::rc_cookies_set_role(9) },
        RcStatus::InvalidArgument
    );

    // CRUD over the JSON record lane.
    let rec = br#"{"name":"sid","value":"v1","domain":".example.com",
        "path":"/","expires":2000000000000,"secure":true,
        "httpOnly":true,"sameSite":2}"#;
    assert_eq!(
        unsafe { crate::rc_cookies_set(rec.as_ptr(), rec.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_cookies_count(ptr::null()) }, 1);
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::rc_cookies_get(
                cs("example.com").as_ptr(),
                cs("sid").as_ptr(),
                cs("/").as_ptr(),
                ptr::null(),
                &mut out,
            )
        },
        RcStatus::Ok
    );
    let got: serde_json::Value =
        serde_json::from_slice(&take_buf(&mut out)).unwrap();
    assert_eq!(got["value"], "v1");
    assert_eq!(got["sameSite"], 2);
    assert_eq!(got["hostOnly"], false);
    assert_eq!(got["httpOnly"], true);
    // A miss is RC_NOT_FOUND.
    assert_eq!(
        unsafe {
            crate::rc_cookies_get(
                cs("example.com").as_ptr(),
                cs("nope").as_ptr(),
                cs("/").as_ptr(),
                ptr::null(),
                &mut out_buf(),
            )
        },
        RcStatus::NotFound
    );

    // The send-match: https sees the secure row, http does not.
    let req = br#"{"url":"https://sub.example.com/"}"#;
    let names =
        take_str(unsafe { crate::rc_cookies_for_url(req.as_ptr(), req.len()) })
            .unwrap();
    assert!(names.contains("sid"));
    let req = br#"{"url":"http://sub.example.com/"}"#;
    let names =
        take_str(unsafe { crate::rc_cookies_for_url(req.as_ptr(), req.len()) })
            .unwrap();
    assert!(!names.contains("sid"));

    // The Set-Cookie ingest lane — live parse + store verdict.
    let verdict = take_str(unsafe {
        crate::rc_cookies_store_header(
            cs("https://m.example.com/deep/x").as_ptr(),
            cs("tok=abc; Max-Age=3600; Secure; SameSite=Strict").as_ptr(),
            ptr::null(),
        )
    })
    .unwrap();
    assert!(verdict.contains("\"stored\":true"));
    let got: serde_json::Value = serde_json::from_slice(
        &{
            let mut b = out_buf();
            assert_eq!(
                unsafe {
                    crate::rc_cookies_get(
                        cs("m.example.com").as_ptr(),
                        cs("tok").as_ptr(),
                        cs("/deep").as_ptr(),
                        ptr::null(),
                        &mut b,
                    )
                },
                RcStatus::Ok
            );
            take_buf(&mut b)
        },
    )
    .unwrap();
    assert_eq!(got["sameSite"], 3);
    assert_eq!(got["hostOnly"], true); // no Domain attr -> host-exact

    // The gate consults the sibling sitedecisions.json — seed a block
    // rule through the sibling store's own surface, then gate it.
    assert_eq!(
        unsafe {
            crate::rc_sitedec_set(
                cs("cookie").as_ptr(),
                cs("bad.example").as_ptr(),
                cs("block").as_ptr(),
            )
        },
        RcStatus::Ok
    );
    let gate_in = br#"{"host":"bad.example"}"#;
    let verdict =
        take_str(unsafe { crate::rc_cookies_gate(gate_in.as_ptr(), gate_in.len()) })
            .unwrap();
    assert!(verdict.contains("\"accept\":false"));
    let gate_in = br#"{"host":"ok.example","accept_policy":0}"#;
    let verdict =
        take_str(unsafe { crate::rc_cookies_gate(gate_in.as_ptr(), gate_in.len()) })
            .unwrap();
    assert!(verdict.contains("\"accept\":true"));

    // Import the real Qt toRawForm fixture (6 domain-carrying rows
    // land; the host-cookie line is skipped).
    let fixture = cs(&format!(
        "{}/testdata/cookies/export.txt",
        env!("CARGO_MANIFEST_DIR")
    ));
    let report = take_str(unsafe {
        crate::rc_cookies_import(fixture.as_ptr(), ptr::null())
    })
    .unwrap();
    assert!(report.contains("\"imported\":6"));
    assert!(report.contains("\"skipped\":1"));
    // Export round-trips through the same file shape.
    let out_file = format!("{dir}/cookies-export.txt");
    let cout = cs(&out_file);
    assert_eq!(
        unsafe { crate::rc_cookies_export(cout.as_ptr(), ptr::null()) },
        RcStatus::Ok
    );
    let exported = std::fs::read_to_string(&out_file).unwrap();
    assert!(exported.contains("sid=a1b2c3; secure; HttpOnly; SameSite=Lax"));
    assert!(exported.contains("domain=.example.com"));

    // Host sweep + partition clear + purge ops.
    assert_eq!(
        unsafe {
            crate::rc_cookies_remove_for_host(
                cs("example.com").as_ptr(),
                ptr::null(),
            )
        },
        RcStatus::Ok
    );
    // expired-purge drops the 1970 row the fixture carried.
    assert_eq!(
        unsafe { crate::rc_cookies_purge_expired(9999999999999) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_cookies_purge_session() },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_cookies_clear(ptr::null()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_cookies_count(ptr::null()) }, 0);
    assert_eq!(
        unsafe {
            crate::rc_cookies_remove(
                cs("x.com").as_ptr(),
                cs("n").as_ptr(),
                cs("/").as_ptr(),
                ptr::null(),
            )
        },
        RcStatus::Ok
    );
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn icons_ffi_roundtrip() {
    let _g = guard();
    // The anchored data dir IS the profile dir — the rc_* lane lands
    // on <dir>/cache/icons (rustCoreEnsureDataDirFor's contract).
    let dir = tmpdir("rc-icons");
    init(&dir);

    // Bad-input legs: every entry point degrades, none crash.
    assert_eq!(
        unsafe { crate::rc_icons_put(ptr::null(), b"x".as_ptr(), 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_icons_put(cs("https://x/").as_ptr(), ptr::null(), 4) },
        RcStatus::InvalidArgument
    );
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_icons_get(ptr::null(), &mut out) },
        RcStatus::InvalidArgument
    );
    assert!(unsafe { crate::rc_icons_key(ptr::null()) }.is_null());

    // Round-trip + dedup-by-host through the C ABI.
    let png = b"\x89PNG-ffi";
    let url = cs("https://Icon.Example/a");
    let url2 = cs("https://icon.example/b");
    assert_eq!(
        unsafe { crate::rc_icons_put(url.as_ptr(), png.as_ptr(), png.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_icons_count() }, 1);
    // A second page of the same host rewrites the one record.
    assert_eq!(
        unsafe { crate::rc_icons_put(url2.as_ptr(), png.as_ptr(), png.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_icons_count() }, 1);
    assert_eq!(
        unsafe { crate::rc_icons_get(url.as_ptr(), &mut out) },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out), png);
    // The derived key matches the historyIconKey contract.
    assert_eq!(
        take_str(unsafe { crate::rc_icons_key(url.as_ptr()) }).as_deref(),
        Some("icon.example")
    );
    let list = take_str(unsafe { crate::rc_icons_list() }).unwrap();
    assert!(list.contains("\"key\":\"icon.example\""));
    // Files live under the cache lane, never the data root proper.
    assert!(
        std::path::Path::new(&dir)
            .join("cache/icons/icons.json")
            .exists()
    );
    assert!(!std::path::Path::new(&dir).join("icons").exists());

    // Miss is NotFound; the sweep drops only aged rows.
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_icons_get(cs("https://other/").as_ptr(), &mut out) },
        RcStatus::NotFound
    );
    let mut removed: i64 = -1;
    assert_eq!(
        unsafe { crate::rc_icons_sweep(0, &mut removed) },
        RcStatus::Ok
    );
    assert_eq!(removed, 0);
    assert_eq!(
        unsafe { crate::rc_icons_sweep(9999999999999, &mut removed) },
        RcStatus::Ok
    );
    assert_eq!(removed, 1);
    assert_eq!(unsafe { crate::rc_icons_count() }, 0);

    // remove + clear legs.
    assert_eq!(
        unsafe { crate::rc_icons_put(url.as_ptr(), png.as_ptr(), png.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_icons_remove(url.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_icons_count() }, 0);
    assert_eq!(
        unsafe { crate::rc_icons_put(url.as_ptr(), png.as_ptr(), png.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_icons_clear() }, RcStatus::Ok);
    // The wipe removes the store wholesale; a subsequent read lazily
    // rebuilds the (empty) index — the imported flag must persist.
    assert!(
        !std::path::Path::new(&dir)
            .join("cache/icons")
            .exists()
    );
    assert_eq!(unsafe { crate::rc_icons_count() }, 0);
    let _ = std::fs::remove_dir_all(&dir);
}

// ---- RC07: the arktos_core.h facade -------------------------------------
//
// The facade forwards every op to the same rc_* entry points, so the
// parity assertions are cheap: the interesting seam is the
// profile-bound re-anchoring (two personas interleaved over the
// process-global stores must never cross) and the
// (module, profile_id) callback stamping.

fn ac_cfg<'a>(root: &'a CString) -> crate::facade::AcConfig {
    crate::facade::AcConfig {
        struct_size: std::mem::size_of::<crate::facade::AcConfig>() as u32,
        api_version: crate::facade::AC_API_VERSION,
        data_root: root.as_ptr(),
        app_name: ptr::null(),
        default_profile_name: ptr::null(),
    }
}

#[test]
fn facade_init_and_accessors() {
    let _g = guard();
    let root = tmpdir("ac-init");
    let croot = cs(&root);

    // housekeeping
    assert_eq!(
        crate::facade::ac_api_version(),
        crate::facade::AC_API_VERSION
    );
    assert_ne!(crate::facade::ac_capabilities(), 0);

    // rejected configs fail clean with a stashed error
    assert!(unsafe { crate::facade::ac_init(ptr::null()) }.is_null());
    assert!(!unsafe { crate::facade::ac_last_error() }.is_null());
    let mut bad = ac_cfg(&croot);
    bad.struct_size = 4;
    assert!(unsafe { crate::facade::ac_init(&bad) }.is_null());
    let mut bad = ac_cfg(&croot);
    bad.api_version = 999;
    assert!(unsafe { crate::facade::ac_init(&bad) }.is_null());
    let mut bad = ac_cfg(&croot);
    bad.data_root = ptr::null();
    assert!(unsafe { crate::facade::ac_init(&bad) }.is_null());

    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());

    // every accessor mints a usable handle
    let gs = unsafe { crate::facade::ac_settings(core, ptr::null()) };
    let ps = unsafe { crate::facade::ac_settings(core, cs("work").as_ptr()) };
    let pr = unsafe { crate::facade::ac_profiles(core) };
    let h = unsafe { crate::facade::ac_history(core, cs("work").as_ptr()) };
    let b = unsafe { crate::facade::ac_bookmarks(core, cs("work").as_ptr()) };
    let p = unsafe { crate::facade::ac_policy(core, cs("work").as_ptr()) };
    assert!(!gs.is_null() && !ps.is_null() && !pr.is_null());
    assert!(!h.is_null() && !b.is_null() && !p.is_null());
    assert_eq!(
        unsafe { crate::facade::ac_history_exists(core, cs("work").as_ptr()) },
        1
    );
    assert_eq!(
        unsafe { crate::facade::ac_history_exists(core, cs("nobody").as_ptr()) },
        0
    );

    // AcSettings IS RcSettings — a legacy call on the facade-minted
    // handle stays legal, and the ac_* spelling reads it back.
    assert_eq!(
        unsafe {
            crate::rc_settings_set_string(
                ps,
                cs("k").as_ptr(),
                cs("v").as_ptr(),
            )
        },
        RcStatus::Ok
    );
    let mut got = out_buf();
    assert_eq!(
        unsafe {
            crate::facade::ac_settings_get_string(ps, cs("k").as_ptr(), &mut got)
        },
        RcStatus::Ok
    );
    assert_eq!(String::from_utf8_lossy(&take_buf(&mut got)), "v");
    assert_eq!(
        unsafe { crate::facade::ac_settings_sync(ps) },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/work/settings.json")
        .exists());

    // profiles through the facade spelling
    let list = take_str(unsafe { crate::facade::ac_profiles_list(pr) }).unwrap();
    assert!(list.contains("\"id\":\"default\""));
    assert!(
        take_str(unsafe { crate::facade::ac_profiles_slugify(cs("Me Work").as_ptr()) })
            .unwrap()
            == "me-work"
    );

    // history via the bound handle — the ac write is visible to the
    // legacy rc_* reader (same open store).
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                h,
                cs("https://a.example/").as_ptr(),
                cs("A").as_ptr(),
                1_000,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_hist_count(h) }, 1);
    assert_eq!(unsafe { crate::rc_hist_count() }, 1);
    let sugg =
        take_str(unsafe { crate::facade::ac_hist_suggest(h, cs("a.example").as_ptr(), 5) })
            .unwrap();
    assert!(sugg.contains("https://a.example/"));

    // bookmarks via the bound handle — rc_* ops land in the same tree.
    let r = unsafe { crate::facade::ac_bm_root(b) };
    assert_ne!(r, 0);
    let node = unsafe {
        crate::facade::ac_bm_create(
            b,
            r,
            -1,
            cs(r#"{"title":"X","url":"https://x.example/"}"#).as_ptr(),
        )
    };
    assert_ne!(node, 0);
    assert_eq!(
        unsafe { crate::rc_bm_find(cs("https://x.example/").as_ptr()) },
        node
    );
    assert_eq!(unsafe { crate::facade::ac_bm_child_count(b, r) }, 1);
    assert_eq!(
        unsafe { crate::facade::ac_bm_save(b, ptr::null()) },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/work/bookmarks.xbel")
        .exists());
    assert_eq!(
        unsafe { crate::facade::ac_bm_reload(b) },
        RcStatus::Ok
    );

    // policy via the bound handle — the sitedec write lands in the
    // profile's own directory.
    assert_eq!(
        unsafe {
            crate::facade::ac_sitedec_set(
                p,
                cs("cookie").as_ptr(),
                cs("x.example").as_ptr(),
                cs("block").as_ptr(),
            )
        },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/work/sitedecisions.json")
        .exists());
    assert_eq!(
        unsafe { crate::facade::ac_blocklist_reload(p) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe {
            crate::facade::ac_blocklist_check(p, cs("not-blocked.example").as_ptr())
        },
        0
    );
    let snap = take_str(unsafe { crate::facade::ac_sitedec_snapshot(p) }).unwrap();
    assert!(snap.contains("x.example"));
    assert!(crate::facade::ac_policy_eval_generation() > 0);

    // RC13: the session handle is path-bound — writes land in the
    // bound profile's dir and reads never touch the global anchor.
    let s = unsafe { crate::facade::ac_session(core, cs("work").as_ptr()) };
    assert!(!s.is_null());
    assert_eq!(unsafe { crate::facade::ac_session_exists(s) }, 0);
    let manifest = br#"{"version":1,"windows":[{"shell":"","current":0,
        "tabs":[{"url":"https://w/","engine":"servo","state":"",
                 "scroll":{"x":0,"y":120}}]}]}"#;
    assert_eq!(
        unsafe {
            crate::facade::ac_session_save(s, manifest.as_ptr(), manifest.len())
        },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/work/session.dat")
        .exists());
    assert_eq!(unsafe { crate::facade::ac_session_exists(s) }, 1);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::facade::ac_session_load(s, &mut out) },
        RcStatus::Ok
    );
    let loaded = take_buf(&mut out);
    let text = String::from_utf8_lossy(&loaded);
    assert!(text.contains("https://w/") && text.contains("\"scroll\""));
    // The lifecycle marker binds the same profile dir.
    assert_eq!(unsafe { crate::facade::ac_session_dirty(s) }, 0);
    assert_eq!(unsafe { crate::facade::ac_session_mark_start(s) }, 0);
    assert_eq!(unsafe { crate::facade::ac_session_dirty(s) }, 1);
    assert_eq!(unsafe { crate::facade::ac_session_mark_start(s) }, 1);
    assert_eq!(unsafe { crate::facade::ac_session_crash_count(s) }, 1);
    assert_eq!(
        unsafe { crate::facade::ac_session_mark_exit(s, 0) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_session_dirty(s) }, 0);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::facade::ac_session_last_exit(s, &mut out) },
        RcStatus::Ok
    );
    assert_eq!(String::from_utf8_lossy(&take_buf(&mut out)), "clean rc=0");
    assert_eq!(
        unsafe { crate::facade::ac_session_set_restoring(s, 1) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_session_restoring(s) }, 1);
    assert!(std::path::Path::new(&root)
        .join("profiles/work/session.state")
        .exists());
    // A different persona's handle sees none of it.
    let s2 = unsafe { crate::facade::ac_session(core, ptr::null()) };
    assert!(!s2.is_null());
    assert_eq!(unsafe { crate::facade::ac_session_exists(s2) }, 0);
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::facade::ac_session_load(s2, &mut out) },
        RcStatus::NotFound
    );
    unsafe {
        crate::facade::ac_session_clear(s);
        crate::facade::ac_session_close(s2);
        crate::facade::ac_session_close(s);
    }
    assert!(!std::path::Path::new(&root)
        .join("profiles/work/session.dat")
        .exists());

    // RC15: the cookies handle binds profiles/<id>/cookies.db — the
    // accessor opens it now and ops re-assert the bound path.
    let ck = unsafe { crate::facade::ac_cookies(core, cs("work").as_ptr()) };
    assert!(!ck.is_null());
    assert!(std::path::Path::new(&root)
        .join("profiles/work/cookies.db")
        .exists());
    assert_eq!(unsafe { crate::facade::ac_cookies_role(ck) }, 0);
    let rec = br#"{"name":"n","value":"v","domain":".example.com",
        "expires":2000000000000}"#;
    assert_eq!(
        unsafe { crate::facade::ac_cookies_set(ck, rec.as_ptr(), rec.len()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_cookies_count(ck, ptr::null()) },
        1
    );
    let list = take_str(unsafe {
        crate::facade::ac_cookies_list(ck, ptr::null())
    })
    .unwrap();
    assert!(list.contains("\"name\":\"n\""));
    let req = br#"{"url":"https://www.example.com/"}"#;
    let matched = take_str(unsafe {
        crate::facade::ac_cookies_for_url(ck, req.as_ptr(), req.len())
    })
    .unwrap();
    assert!(matched.contains("\"name\":\"n\""));
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::facade::ac_cookies_get(
                ck,
                cs("example.com").as_ptr(),
                cs("n").as_ptr(),
                cs("/").as_ptr(),
                ptr::null(),
                &mut out,
            )
        },
        RcStatus::Ok
    );
    take_buf(&mut out);
    // The verdict lanes forward over the bound handle.
    let verdict = take_str(unsafe {
        crate::facade::ac_cookies_store_header(
            ck,
            cs("https://h.example.com/").as_ptr(),
            cs("sh=1; Max-Age=60").as_ptr(),
            ptr::null(),
        )
    })
    .unwrap();
    assert!(verdict.contains("\"stored\":true"));
    let gate_in = br#"{"host":"h.example.com"}"#;
    let gate = take_str(unsafe {
        crate::facade::ac_cookies_gate(ck, gate_in.as_ptr(), gate_in.len())
    })
    .unwrap();
    assert!(gate.contains("\"accept\":true"));
    let fixture = cs(&format!(
        "{}/testdata/cookies/export.txt",
        env!("CARGO_MANIFEST_DIR")
    ));
    let report = take_str(unsafe {
        crate::facade::ac_cookies_import(ck, fixture.as_ptr(), ptr::null())
    })
    .unwrap();
    assert!(report.contains("\"imported\":6"));
    let out_file = format!("{root}/cookies-out.txt");
    assert_eq!(
        unsafe {
            crate::facade::ac_cookies_export(ck, cs(&out_file).as_ptr(), ptr::null())
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_cookies_purge_session(ck) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe {
            crate::facade::ac_cookies_remove_for_host(
                ck,
                cs("example.com").as_ptr(),
                ptr::null(),
            )
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_cookies_clear(ck, ptr::null()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_cookies_count(ck, ptr::null()) },
        0
    );

    // RC16: the icons handle is path-bound to profiles/<id> — the
    // cache lands under its cache/icons lane and never touches the
    // process-global anchor.
    let ic = unsafe { crate::facade::ac_icons(core, cs("work").as_ptr()) };
    assert!(!ic.is_null());
    assert_eq!(
        take_str(unsafe {
            crate::facade::ac_icons_key(ic, cs("https://ICON.example/x").as_ptr())
        })
        .as_deref(),
        Some("icon.example")
    );
    let png = b"\x89PNG-ac";
    assert_eq!(
        unsafe {
            crate::facade::ac_icons_put(
                ic,
                cs("https://icon.example/a").as_ptr(),
                png.as_ptr(),
                png.len(),
            )
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe {
            crate::facade::ac_icons_put(
                ic,
                cs("https://icon.example/b").as_ptr(),
                png.as_ptr(),
                png.len(),
            )
        },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_icons_count(ic) }, 1);
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::facade::ac_icons_get(
                ic,
                cs("https://icon.example/a").as_ptr(),
                &mut out,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(take_buf(&mut out), png);
    assert!(std::path::Path::new(&root)
        .join("profiles/work/cache/icons/icons.json")
        .exists());
    assert!(!std::path::Path::new(&root)
        .join("profiles/work/icons")
        .exists());
    let list = take_str(unsafe { crate::facade::ac_icons_list(ic) }).unwrap();
    assert!(list.contains("\"key\":\"icon.example\""));
    let mut removed: i64 = -1;
    assert_eq!(
        unsafe { crate::facade::ac_icons_sweep(ic, 9999999999999, &mut removed) },
        RcStatus::Ok
    );
    assert_eq!(removed, 1);
    assert_eq!(unsafe { crate::facade::ac_icons_count(ic) }, 0);
    assert_eq!(
        unsafe {
            crate::facade::ac_icons_put(
                ic,
                cs("https://icon.example/").as_ptr(),
                png.as_ptr(),
                png.len(),
            )
        },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_icons_remove(ic, cs("https://icon.example/").as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_icons_clear(ic) },
        RcStatus::Ok
    );

    // RC19: the containers handle binds the persona's settings
    // scope — registry ops land in profiles/<id>/settings.json's
    // "containers" group, the namespace derives its paths.
    let ct = unsafe { crate::facade::ac_containers(core, cs("work").as_ptr()) };
    assert!(!ct.is_null());
    let spec = b"{\"name\":\"Side\",\"color\":\"#00aa88\"}";
    let made = take_str(unsafe {
        crate::facade::ac_containers_create(ct, spec.as_ptr(), spec.len())
    })
    .unwrap();
    let row: serde_json::Value = serde_json::from_str(&made).unwrap();
    let cid_s = row["id"].as_str().unwrap().to_string();
    let cid = cs(&cid_s);
    let listed = take_str(unsafe { crate::facade::ac_containers_list(ct) }).unwrap();
    assert!(listed.contains(&cid_s));
    let ns = take_str(unsafe {
        crate::facade::ac_containers_namespace(ct, cid.as_ptr())
    })
    .unwrap();
    let ns: serde_json::Value = serde_json::from_str(&ns).unwrap();
    assert_eq!(
        ns["storage_path"],
        format!("{root}/profiles/work/containers/{cid_s}")
    );
    assert_eq!(ns["cookie_partition"], cid_s);
    let patch = b"{\"color\":\"#112233\"}";
    assert_eq!(
        unsafe {
            crate::facade::ac_containers_update(
                ct,
                cid.as_ptr(),
                patch.as_ptr(),
                patch.len(),
            )
        },
        RcStatus::Ok
    );
    let row: serde_json::Value = serde_json::from_str(
        &take_str(unsafe {
            crate::facade::ac_containers_get(ct, cid.as_ptr())
        })
        .unwrap(),
    )
    .unwrap();
    assert_eq!(row["color"], "#112233");
    assert_eq!(
        unsafe { crate::facade::ac_containers_remove(ct, cid.as_ptr()) },
        RcStatus::Ok
    );
    assert!(unsafe {
        crate::facade::ac_containers_get(ct, cid.as_ptr())
    }
    .is_null());

    // RC21: the notes handle binds profiles/<id>/notes.json.
    let nt = unsafe { crate::facade::ac_notes(core, cs("work").as_ptr()) };
    assert!(!nt.is_null());
    assert_eq!(unsafe { crate::facade::ac_notes_is_persistent(nt) }, 1);
    assert_eq!(
        unsafe {
            crate::facade::ac_notes_insert(
                nt,
                0,
                cs("facade note").as_ptr(),
                cs("body").as_ptr(),
            )
        },
        0
    );
    assert_eq!(unsafe { crate::facade::ac_notes_count(nt) }, 1);
    assert_eq!(
        unsafe { crate::facade::ac_notes_sync(nt) },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/work/notes.json")
        .exists());
    unsafe { crate::facade::ac_notes_close(nt) };

    // explicit closes, then shutdown reclaims the rest — both
    // NULL-tolerant.
    unsafe {
        crate::facade::ac_containers_close(ct);
        crate::facade::ac_icons_close(ic);
        crate::facade::ac_cookies_close(ck);
        crate::facade::ac_hist_close(h);
        crate::facade::ac_bm_close(b);
        crate::facade::ac_policy_close(p);
        crate::facade::ac_settings_close(gs);
        crate::facade::ac_settings_close(ps);
        crate::facade::ac_profiles_close(pr);
        crate::facade::ac_shutdown(core);
        crate::facade::ac_shutdown(ptr::null_mut());
    }
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn facade_profile_isolation() {
    let _g = guard();
    let root = tmpdir("ac-iso");
    let croot = cs(&root);
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());

    // Two bound handles over the process-global history store — each
    // call re-anchors to its own profile.
    let ha = unsafe { crate::facade::ac_history(core, cs("alice").as_ptr()) };
    let hb = unsafe { crate::facade::ac_history(core, cs("bob").as_ptr()) };
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                ha,
                cs("https://alice.example/").as_ptr(),
                cs("A").as_ptr(),
                10,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_hist_count(hb) }, 0);
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                hb,
                cs("https://bob.example/").as_ptr(),
                cs("B").as_ptr(),
                20,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_hist_count(ha) }, 1);
    assert_eq!(unsafe { crate::facade::ac_hist_count(hb) }, 1);
    unsafe {
        crate::facade::ac_hist_close(ha);
        crate::facade::ac_hist_close(hb);
    }

    // Same for bookmarks — interleaved ops stay per-profile.
    let ba = unsafe { crate::facade::ac_bookmarks(core, cs("alice").as_ptr()) };
    let bb = unsafe { crate::facade::ac_bookmarks(core, cs("bob").as_ptr()) };
    let ra = unsafe { crate::facade::ac_bm_root(ba) };
    let created = unsafe {
        crate::facade::ac_bm_create(
            ba,
            ra,
            -1,
            cs(r#"{"title":"a","url":"https://a1.example/"}"#).as_ptr(),
        )
    };
    if created == 0 {
        panic!(
            "ac_bm_create failed: ra={ra} err={:?}",
            take_str(unsafe { crate::facade::ac_last_error() })
        );
    }
    assert_eq!(
        unsafe { crate::facade::ac_bm_find(bb, cs("https://a1.example/").as_ptr()) },
        0
    );
    let rb = unsafe { crate::facade::ac_bm_root(bb) };
    assert_ne!(
        unsafe {
            crate::facade::ac_bm_create(
                bb,
                rb,
                -1,
                cs(r#"{"title":"b","url":"https://b1.example/"}"#).as_ptr(),
            )
        },
        0
    );
    assert_ne!(
        unsafe { crate::facade::ac_bm_find(ba, cs("https://a1.example/").as_ptr()) },
        0
    );
    assert_ne!(
        unsafe { crate::facade::ac_bm_find(bb, cs("https://b1.example/").as_ptr()) },
        0
    );
    assert_eq!(
        unsafe { crate::facade::ac_bm_save(ba, ptr::null()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_bm_save(bb, ptr::null()) },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/alice/bookmarks.xbel")
        .exists());
    assert!(std::path::Path::new(&root)
        .join("profiles/bob/bookmarks.xbel")
        .exists());
    unsafe {
        crate::facade::ac_bm_close(ba);
        crate::facade::ac_bm_close(bb);
    }

    // Policy isolation: each persona's sitedecisions.json.
    let pa = unsafe { crate::facade::ac_policy(core, cs("alice").as_ptr()) };
    let pb = unsafe { crate::facade::ac_policy(core, cs("bob").as_ptr()) };
    assert_eq!(
        unsafe {
            crate::facade::ac_sitedec_set(
                pa,
                cs("cookie").as_ptr(),
                cs("alice.example").as_ptr(),
                cs("block").as_ptr(),
            )
        },
        RcStatus::Ok
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/alice/sitedecisions.json")
        .exists());
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::facade::ac_sitedec_get(
                pb,
                cs("cookie").as_ptr(),
                cs("alice.example").as_ptr(),
                &mut out,
            )
        },
        RcStatus::NotFound
    );
    unsafe {
        crate::facade::ac_policy_close(pa);
        crate::facade::ac_policy_close(pb);
    }

    // Cookie isolation: alice's rows never reach bob's db.
    let ca = unsafe { crate::facade::ac_cookies(core, cs("alice").as_ptr()) };
    let cb = unsafe { crate::facade::ac_cookies(core, cs("bob").as_ptr()) };
    let rec = br#"{"name":"n","value":"a","domain":".alice.example",
        "expires":2000000000000}"#;
    assert_eq!(
        unsafe { crate::facade::ac_cookies_set(ca, rec.as_ptr(), rec.len()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_cookies_count(ca, ptr::null()) }, 1);
    assert_eq!(unsafe { crate::facade::ac_cookies_count(cb, ptr::null()) }, 0);
    assert!(std::path::Path::new(&root)
        .join("profiles/alice/cookies.db")
        .exists());
    assert!(std::path::Path::new(&root)
        .join("profiles/bob/cookies.db")
        .exists());
    unsafe {
        crate::facade::ac_cookies_close(ca);
        crate::facade::ac_cookies_close(cb);
    }

    // Icon isolation: each persona's cache/icons lane is its own.
    let ia = unsafe { crate::facade::ac_icons(core, cs("alice").as_ptr()) };
    let ib = unsafe { crate::facade::ac_icons(core, cs("bob").as_ptr()) };
    let png = b"\x89PNG-iso";
    assert_eq!(
        unsafe {
            crate::facade::ac_icons_put(
                ia,
                cs("https://alice.example/").as_ptr(),
                png.as_ptr(),
                png.len(),
            )
        },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_icons_count(ia) }, 1);
    assert_eq!(unsafe { crate::facade::ac_icons_count(ib) }, 0);
    let mut out = out_buf();
    assert_eq!(
        unsafe {
            crate::facade::ac_icons_get(
                ib,
                cs("https://alice.example/").as_ptr(),
                &mut out,
            )
        },
        RcStatus::NotFound
    );
    assert!(std::path::Path::new(&root)
        .join("profiles/alice/cache/icons/icons.json")
        .exists());
    // Bob's store holds no icon files — his count read above may have
    // persisted the (empty) index itself, which carries no rows.
    assert!(
        std::fs::read_dir(
            std::path::Path::new(&root).join("profiles/bob/cache/icons")
        )
        .map(|rd| !rd.flatten().any(|e| {
            e.file_name().to_string_lossy().ends_with(".png")
        }))
        .unwrap_or(true)
    );
    unsafe {
        crate::facade::ac_icons_close(ia);
        crate::facade::ac_icons_close(ib);
        crate::facade::ac_shutdown(core);
    }
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn facade_callbacks_carry_profile() {
    let _g = guard();
    let root = tmpdir("ac-cb");
    let croot = cs(&root);
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());

    static AC_HITS: AtomicUsize = AtomicUsize::new(0);
    thread_local! {
        static LAST: std::cell::RefCell<(String, String)> =
            const { std::cell::RefCell::new((String::new(), String::new())) };
    }
    unsafe extern "C" fn cb(
        topic: *const std::os::raw::c_char,
        profile: *const std::os::raw::c_char,
        _ud: *mut c_void,
    ) {
        let t = unsafe { CStr::from_ptr(topic) }
            .to_string_lossy()
            .into_owned();
        let p = if profile.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(profile) }
                .to_string_lossy()
                .into_owned()
        };
        LAST.with(|l| *l.borrow_mut() = (t, p));
        AC_HITS.fetch_add(1, Ordering::SeqCst);
    }
    unsafe {
        crate::facade::ac_set_change_callback(core, Some(cb), ptr::null_mut())
    };

    // A facade-bound mutation stamps the profile.
    let h = unsafe { crate::facade::ac_history(core, cs("work").as_ptr()) };
    let base = AC_HITS.load(Ordering::SeqCst);
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                h,
                cs("https://n.example/").as_ptr(),
                cs("N").as_ptr(),
                1,
            )
        },
        RcStatus::Ok
    );
    assert!(AC_HITS.load(Ordering::SeqCst) > base);
    LAST.with(|l| {
        let (t, p) = l.borrow().clone();
        assert_eq!(t, "history");
        assert_eq!(p, "work");
    });
    unsafe { crate::facade::ac_hist_close(h) };

    // A legacy rc_* mutation reaches the subscriber with a NULL
    // profile (no binding to report).
    let db = cs(&format!("{root}/profiles/work/history.db"));
    assert_eq!(
        unsafe { crate::rc_hist_open(db.as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe {
            crate::rc_hist_add(
                cs("https://legacy.example/").as_ptr(),
                cs("L").as_ptr(),
                2,
            )
        },
        RcStatus::Ok
    );
    LAST.with(|l| {
        let (t, p) = l.borrow().clone();
        assert_eq!(t, "history");
        assert!(p.is_empty());
    });

    // Settings writes stamp their scope's profile; the global scope
    // stamps NULL.
    let s = unsafe { crate::facade::ac_settings(core, cs("work").as_ptr()) };
    assert_eq!(
        unsafe {
            crate::facade::ac_settings_set_string(
                s,
                cs("k").as_ptr(),
                cs("v").as_ptr(),
            )
        },
        RcStatus::Ok
    );
    LAST.with(|l| {
        let (t, p) = l.borrow().clone();
        assert_eq!(t, "settings");
        assert_eq!(p, "work");
    });
    unsafe { crate::facade::ac_settings_close(s) };

    // A cookie mutation stamps the "cookies" topic + bound profile.
    let ck = unsafe { crate::facade::ac_cookies(core, cs("work").as_ptr()) };
    let rec = br#"{"name":"n","domain":".example.com",
        "expires":2000000000000}"#;
    assert_eq!(
        unsafe { crate::facade::ac_cookies_set(ck, rec.as_ptr(), rec.len()) },
        RcStatus::Ok
    );
    LAST.with(|l| {
        let (t, p) = l.borrow().clone();
        assert_eq!(t, "cookies");
        assert_eq!(p, "work");
    });
    unsafe { crate::facade::ac_cookies_close(ck) };

    // Clearing the callback silences it; shutdown after clear is safe.
    unsafe { crate::facade::ac_set_change_callback(core, None, ptr::null_mut()) };
    let before = AC_HITS.load(Ordering::SeqCst);
    let h2 = unsafe { crate::facade::ac_history(core, cs("work").as_ptr()) };
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                h2,
                cs("https://silent.example/").as_ptr(),
                cs("S").as_ptr(),
                3,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(AC_HITS.load(Ordering::SeqCst), before);
    unsafe {
        crate::facade::ac_hist_close(h2);
        crate::facade::ac_shutdown(core);
    }
    let _ = std::fs::remove_dir_all(&root);
}

#[test]
fn facade_bad_handles_fail_clean() {
    let _g = guard();
    let root = tmpdir("ac-bad");
    let croot = cs(&root);
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());

    // NULL handles return the failure spelling, never crash.
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                ptr::null(),
                cs("u").as_ptr(),
                cs("t").as_ptr(),
                1,
            )
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::facade::ac_hist_count(ptr::null()) }, -1);
    assert_eq!(unsafe { crate::facade::ac_bm_root(ptr::null()) }, 0);
    assert_eq!(
        unsafe {
            crate::facade::ac_sitedec_set(
                ptr::null(),
                cs("cookie").as_ptr(),
                cs("k").as_ptr(),
                cs("v").as_ptr(),
            )
        },
        RcStatus::InvalidArgument
    );
    let err = take_str(unsafe { crate::facade::ac_last_error() }).unwrap();
    assert!(err.contains("bad"));
    // Cookies legs on NULL handles degrade the same way.
    assert_eq!(
        unsafe { crate::facade::ac_cookies_role(ptr::null()) },
        -1
    );
    assert_eq!(
        unsafe {
            crate::facade::ac_cookies_set(ptr::null(), b"{}".as_ptr(), 2)
        },
        RcStatus::InvalidArgument
    );
    assert!(unsafe {
        crate::facade::ac_cookies_list(ptr::null(), ptr::null())
    }
    .is_null());
    assert_eq!(
        unsafe {
            crate::facade::ac_cookies_purge_expired(ptr::null(), 0)
        },
        RcStatus::InvalidArgument
    );
    // Accessors on a NULL core fail, not crash.
    assert!(unsafe { crate::facade::ac_settings(ptr::null_mut(), ptr::null()) }
        .is_null());
    assert!(unsafe { crate::facade::ac_cookies(ptr::null_mut(), ptr::null()) }
        .is_null());
    unsafe { crate::facade::ac_shutdown(core) };
    let _ = std::fs::remove_dir_all(&root);
}

// ---- RC14 torctl surface ------------------------------------------------

/// Scripted control-port mock for the FFI legs: answers GETINFO with a
/// data-block reply plus one interleaved 650 event, everything else OK.
fn tor_mock() -> (u16, std::thread::JoinHandle<()>) {
    use std::io::{BufRead, BufReader, Write};
    use std::net::{IpAddr, TcpListener};
    let listener =
        TcpListener::bind((IpAddr::from([127, 0, 0, 1]), 0)).unwrap();
    let port = listener.local_addr().unwrap().port();
    let join = std::thread::spawn(move || {
        if let Ok((conn, _)) = listener.accept() {
            let _ = conn.set_read_timeout(Some(
                std::time::Duration::from_secs(10),
            ));
            let mut writer = conn.try_clone().unwrap();
            let mut reader = BufReader::new(conn);
            loop {
                let mut line = String::new();
                match reader.read_line(&mut line) {
                    Ok(0) | Err(_) => return,
                    Ok(_) => {}
                }
                let cmd = line.trim_end();
                let reply = if cmd.starts_with("GETINFO circ") {
                    "250+circuit-status=\r\n3 BUILT $AA~G\r\n.\r\n250 OK\r\n"
                } else if cmd.starts_with("GETINFO ev") {
                    "650 CIRC 4 BUILT $BB~M\r\n250-ev=1\r\n250 OK\r\n"
                } else {
                    "250 OK\r\n"
                };
                if writer.write_all(reply.as_bytes()).is_err() {
                    return;
                }
            }
        }
    });
    (port, join)
}

#[test]
fn torctl_surface() {
    // Bad-argument legs first — NULL handles/pointers degrade to the
    // failure spelling, never a crash.
    assert!(unsafe { crate::rc_tor_open(ptr::null(), 9051) }.is_null());
    assert!(unsafe { crate::rc_tor_open(cs("8.8.8.8").as_ptr(), 9051) }
        .is_null());
    assert!(unsafe { crate::rc_tor_open(cs("example.org").as_ptr(), 9051) }
        .is_null());
    unsafe { crate::rc_tor_close(ptr::null_mut()) };
    assert_eq!(unsafe { crate::rc_tor_state(ptr::null()) }, 0);
    assert!(unsafe { crate::rc_tor_error(ptr::null()) }.is_null());
    assert!(unsafe { crate::rc_tor_take_event(ptr::null()) }.is_null());
    assert!(unsafe { crate::rc_tor_take_error(ptr::null()) }.is_null());
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_tor_take_reply(ptr::null(), &mut out) },
        RcStatus::InvalidArgument
    );
    for seq in [
        unsafe { crate::rc_tor_send(ptr::null(), cs("X").as_ptr()) },
        unsafe { crate::rc_tor_getinfo(ptr::null(), cs("k").as_ptr()) },
        unsafe { crate::rc_tor_setevents(ptr::null(), cs("CIRC").as_ptr()) },
        unsafe { crate::rc_tor_takeownership(ptr::null()) },
        unsafe { crate::rc_tor_signal(ptr::null(), cs("NEWNYM").as_ptr()) },
        unsafe {
            crate::rc_tor_auth_cookie(ptr::null(), b"c".as_ptr(), 32)
        },
        unsafe {
            crate::rc_tor_auth_cookie_file(ptr::null(), cs("/x").as_ptr())
        },
        unsafe {
            crate::rc_tor_auth_password(ptr::null(), b"p".as_ptr(), 1)
        },
    ] {
        assert_eq!(seq, -1);
    }

    // Live leg over the C ABI against the scripted mock.
    let (port, _join) = tor_mock();
    let h = unsafe { crate::rc_tor_open(cs("127.0.0.1").as_ptr(), port) };
    assert!(!h.is_null());
    assert_eq!(unsafe { crate::rc_tor_state(h) }, 1 /* CONNECTED */);
    assert_eq!(
        take_str(unsafe { crate::rc_tor_error(h) }).as_deref(),
        Some("")
    );

    // Bad-argument legs on a live handle.
    assert_eq!(unsafe { crate::rc_tor_send(h, ptr::null()) }, -1);
    assert_eq!(
        unsafe { crate::rc_tor_send(h, cs("a\rb").as_ptr()) },
        -1
    );
    assert_eq!(unsafe { crate::rc_tor_getinfo(h, ptr::null()) }, -1);
    assert_eq!(
        unsafe { crate::rc_tor_auth_cookie(h, ptr::null(), 32) },
        -1
    );
    assert_eq!(
        unsafe { crate::rc_tor_auth_cookie(h, b"x".as_ptr(), 31) },
        -1
    );
    assert_eq!(
        unsafe { crate::rc_tor_auth_password(h, ptr::null(), 0) },
        -1
    );

    let seq = unsafe { crate::rc_tor_getinfo(h, cs("circ").as_ptr()) };
    assert!(seq > 0);
    let deadline =
        std::time::Instant::now() + std::time::Duration::from_secs(5);
    let mut reply: Option<serde_json::Value> = None;
    while std::time::Instant::now() < deadline {
        let mut out = out_buf();
        if unsafe { crate::rc_tor_take_reply(h, &mut out) } == RcStatus::Ok {
            let raw = take_buf(&mut out);
            reply = Some(serde_json::from_slice(&raw).unwrap());
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    let reply = reply.expect("no reply landed");
    assert_eq!(reply["seq"].as_u64().unwrap(), seq as u64);
    assert_eq!(reply["code"].as_i64().unwrap(), 250);
    let lines: Vec<&str> = reply["lines"]
        .as_array()
        .unwrap()
        .iter()
        .map(|v| v.as_str().unwrap())
        .collect();
    assert_eq!(lines, vec!["circuit-status=", "3 BUILT $AA~G", "OK"]);

    // The 650 line queues as an async event, not into the reply.
    let seq = unsafe { crate::rc_tor_getinfo(h, cs("ev").as_ptr()) };
    assert!(seq > 0);
    let deadline =
        std::time::Instant::now() + std::time::Duration::from_secs(5);
    let mut event: Option<String> = None;
    while std::time::Instant::now() < deadline {
        if let Some(e) = take_str(unsafe { crate::rc_tor_take_event(h) }) {
            event = Some(e);
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }
    assert_eq!(event.as_deref(), Some("CIRC 4 BUILT $BB~M"));
    // ...and its reply still completes.
    while std::time::Instant::now() < deadline {
        let mut out = out_buf();
        if unsafe { crate::rc_tor_take_reply(h, &mut out) } == RcStatus::Ok {
            take_buf(&mut out);
            break;
        }
        std::thread::sleep(std::time::Duration::from_millis(5));
    }

    // Drained queues report the empty spelling.
    let mut out = out_buf();
    assert_eq!(
        unsafe { crate::rc_tor_take_reply(h, &mut out) },
        RcStatus::NotFound
    );
    assert!(unsafe { crate::rc_tor_take_event(h) }.is_null());
    assert!(unsafe { crate::rc_tor_take_error(h) }.is_null());

    unsafe { crate::rc_tor_close(h) };

    // A second open/close cycle proves a freed handle left nothing
    // behind — "localhost" exercises the name-resolution gate (any
    // resolved answer must still be loopback).
    let (port2, _j2) = tor_mock();
    let h2 = unsafe { crate::rc_tor_open(cs("localhost").as_ptr(), port2) };
    if !h2.is_null() {
        assert_eq!(unsafe { crate::rc_tor_state(h2) }, 1);
        unsafe { crate::rc_tor_close(h2) };
    }
}

// RC20: every rc_updates_* entry point — pure ops (bad-argument
// spellings plus one verdict leg each) and the settings-backed state
// surface on a global-scope handle.
#[test]
fn updates_surface() {
    let _g = guard();

    // Bad-argument legs first.
    assert!(unsafe { crate::rc_updates_normalize_version(ptr::null()) }
        .is_null());
    assert_eq!(
        unsafe { crate::rc_updates_compare_versions(ptr::null(), cs("1").as_ptr()) },
        -2
    );
    assert_eq!(
        unsafe { crate::rc_updates_compare_versions(cs("1").as_ptr(), ptr::null()) },
        -2
    );
    assert_eq!(
        unsafe { crate::rc_updates_is_trusted_url(ptr::null(), cs("https://x").as_ptr()) },
        -1
    );
    assert_eq!(
        unsafe { crate::rc_updates_is_trusted_url(cs("https://x").as_ptr(), ptr::null()) },
        -1
    );
    assert_eq!(
        unsafe { crate::rc_updates_channel_accepts(ptr::null(), 0, 0) },
        -1
    );
    assert!(unsafe {
        crate::rc_updates_parse_manifest(ptr::null(), 4, cs("https://a").as_ptr())
    }
    .is_null());
    assert!(unsafe {
        crate::rc_updates_parse_manifest(b"{}".as_ptr(), 2, ptr::null())
    }
    .is_null());
    assert!(unsafe {
        crate::rc_updates_platform_pick(cs("linux").as_ptr(), ptr::null(),
            b"[]".as_ptr(), 2)
    }
    .is_null());
    assert!(unsafe { crate::rc_updates_state(ptr::null_mut()) }.is_null());
    assert_eq!(
        unsafe { crate::rc_updates_record_check(ptr::null_mut(), 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_updates_snooze(ptr::null_mut(), 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_updates_set_skipped(ptr::null_mut(), cs("v1").as_ptr()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_updates_set_channel(ptr::null_mut(), cs("beta").as_ptr()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rc_updates_should_notify(ptr::null_mut(),
                cs("v2").as_ptr(), cs("v1").as_ptr(), 0)
        },
        -1
    );

    // Pure ops over the ABI.
    let n = take_str(unsafe {
        crate::rc_updates_normalize_version(cs("v1.2.3-rc1+x").as_ptr())
    })
    .unwrap();
    assert_eq!(n, "1.2.3");
    assert_eq!(
        unsafe {
            crate::rc_updates_compare_versions(cs("v0.13").as_ptr(), cs("0.12").as_ptr())
        },
        1
    );
    assert_eq!(
        unsafe {
            crate::rc_updates_compare_versions(cs("1.0").as_ptr(), cs("1.0.0").as_ptr())
        },
        0
    );
    let api = cs("https://api.github.com/x");
    assert_eq!(
        unsafe {
            crate::rc_updates_is_trusted_url(
                cs("https://github.com/a/b.deb").as_ptr(), api.as_ptr())
        },
        1
    );
    assert_eq!(
        unsafe {
            crate::rc_updates_is_trusted_url(
                cs("javascript:alert(1)").as_ptr(), api.as_ptr())
        },
        0
    );
    assert_eq!(
        unsafe { crate::rc_updates_channel_accepts(cs("stable").as_ptr(), 0, 1) },
        0
    );
    assert_eq!(
        unsafe { crate::rc_updates_channel_accepts(cs("beta").as_ptr(), 0, 1) },
        1
    );
    assert_eq!(
        unsafe { crate::rc_updates_channel_accepts(cs("nightly").as_ptr(), 0, 0) },
        -1
    );

    // Manifest parse + platform pick over the ABI.
    let manifest = br#"{"tag_name":"v9.9","name":"Arktos v9.9",
        "html_url":"https://github.com/o/r/releases/x",
        "assets":[
            {"name":"a.deb","browser_download_url":"https://github.com/x/a.deb","size":7},
            {"name":"b.exe","browser_download_url":"javascript:x","size":1}
        ]}"#;
    let parsed = take_str(unsafe {
        crate::rc_updates_parse_manifest(
            manifest.as_ptr(), manifest.len(), api.as_ptr())
    })
    .unwrap();
    let pj: serde_json::Value = serde_json::from_str(&parsed).unwrap();
    assert_eq!(pj["version"], "v9.9");
    assert_eq!(pj["draft"], false);
    assert_eq!(pj["assets"].as_array().unwrap().len(), 1);
    assert_eq!(pj["assets"][0]["name"], "a.deb");
    assert!(unsafe {
        crate::rc_updates_parse_manifest(b"nope{".as_ptr(), 5, api.as_ptr())
    }
    .is_null());

    let pick = take_str(unsafe {
        crate::rc_updates_platform_pick(
            cs("linux").as_ptr(), cs("debian").as_ptr(),
            pj["assets"].to_string().as_ptr(),
            pj["assets"].to_string().len())
    })
    .unwrap();
    let pk: serde_json::Value = serde_json::from_str(&pick).unwrap();
    assert_eq!(pk["candidates"].as_array().unwrap().len(), 1);
    assert_eq!(pk["ambiguous"], false);

    // The state surface on a global-scope settings handle.
    let root = tmpdir("updates");
    let croot = cs(&root);
    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), ptr::null()) };
    assert!(!h.is_null());
    let st = take_str(unsafe { crate::rc_updates_state(h) }).unwrap();
    let sj: serde_json::Value = serde_json::from_str(&st).unwrap();
    assert_eq!(sj["last_check_ms"], 0);
    assert_eq!(sj["channel"], "stable");

    assert_eq!(
        unsafe { crate::rc_updates_record_check(h, 1_700_000_000_000) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_updates_snooze(h, 1_800_000_000_000) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_updates_set_skipped(h, cs("v9.9").as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_updates_set_channel(h, cs("beta").as_ptr()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_updates_set_channel(h, cs("bogus").as_ptr()) },
        RcStatus::InvalidArgument
    );
    let st = take_str(unsafe { crate::rc_updates_state(h) }).unwrap();
    let sj: serde_json::Value = serde_json::from_str(&st).unwrap();
    assert_eq!(sj["last_check_ms"], 1_700_000_000_000i64);
    assert_eq!(sj["snooze_until_ms"], 1_800_000_000_000i64);
    assert_eq!(sj["skipped_version"], "v9.9");
    assert_eq!(sj["channel"], "beta");

    // The notify verdict reads the persisted state: snoozed + the
    // skipped tag both refuse; past the deadline a fresh tag fires.
    assert_eq!(
        unsafe {
            crate::rc_updates_should_notify(h,
                cs("v9.9").as_ptr(), cs("v1.0").as_ptr(), 0)
        },
        0
    );
    assert_eq!(
        unsafe {
            crate::rc_updates_should_notify(h,
                cs("v10").as_ptr(), cs("v1.0").as_ptr(), 2_000_000_000_000)
        },
        1
    );

    // NULL clears the skipped-version filter; <=0 clears the snooze.
    assert_eq!(
        unsafe { crate::rc_updates_set_skipped(h, ptr::null()) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::rc_updates_snooze(h, 0) }, RcStatus::Ok);
    let st = take_str(unsafe { crate::rc_updates_state(h) }).unwrap();
    let sj: serde_json::Value = serde_json::from_str(&st).unwrap();
    assert_eq!(sj["skipped_version"], "");
    assert_eq!(sj["snooze_until_ms"], 0);

    unsafe { crate::rc_settings_close(h) };
}

// RC21: every rc_notes_* entry point — the volatile off-the-record
// binding, the explicit-file lane, the root+profile lane and the
// facade's ac_notes_* forwards, happy path plus bad-argument legs.
#[test]
fn notes_surface() {
    let _g = guard();

    // Bad-argument legs first — NULL handles everywhere.
    assert_eq!(unsafe { crate::rc_notes_count(ptr::null_mut()) }, -1);
    assert!(unsafe { crate::rc_notes_get(ptr::null_mut(), 0) }.is_null());
    assert!(unsafe { crate::rc_notes_list(ptr::null_mut()) }.is_null());
    assert_eq!(
        unsafe {
            crate::rc_notes_insert(ptr::null_mut(), 0, cs("t").as_ptr(),
                cs("b").as_ptr())
        },
        -1
    );
    let mut changed = -1;
    assert_eq!(
        unsafe {
            crate::rc_notes_set_title(ptr::null_mut(), 0, cs("t").as_ptr(),
                &mut changed)
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe {
            crate::rc_notes_set_body(ptr::null_mut(), 0, cs("b").as_ptr(),
                &mut changed)
        },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_notes_set_markdown(ptr::null_mut(), 0, 1, &mut changed) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_notes_remove(ptr::null_mut(), 0, 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_notes_sync(ptr::null_mut()) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_notes_is_persistent(ptr::null_mut()) }, 0);
    assert_eq!(unsafe { crate::rc_notes_is_dirty(ptr::null_mut()) }, 0);
    unsafe { crate::rc_notes_close(ptr::null_mut()) }; // NULL-tolerant
    // A non-slug profile id refuses; an empty root with a slug does too.
    assert!(unsafe {
        crate::rc_notes_open(cs("/tmp/x").as_ptr(), cs("Bad Id").as_ptr())
    }
    .is_null());
    assert!(unsafe {
        crate::rc_notes_open(cs("").as_ptr(), cs("work").as_ptr())
    }
    .is_null());

    // The volatile lane — NULL profile id, CRUD in memory, sync and
    // close flush nothing.
    let v = unsafe { crate::rc_notes_open(cs("/nonexistent").as_ptr(), ptr::null()) };
    assert!(!v.is_null());
    assert_eq!(unsafe { crate::rc_notes_is_persistent(v) }, 0);
    assert_eq!(unsafe { crate::rc_notes_insert(v, 0, ptr::null(), cs("secret").as_ptr()) }, 0);
    assert_eq!(unsafe { crate::rc_notes_count(v) }, 1);
    assert_eq!(unsafe { crate::rc_notes_is_dirty(v) }, 1);
    assert_eq!(unsafe { crate::rc_notes_sync(v) }, RcStatus::Ok);
    unsafe { crate::rc_notes_close(v) };

    // The explicit-file lane — the standalone model's binding.
    let root = tmpdir("notes");
    let croot = cs(&root);
    let file = format!("{root}/deep/notes.json");
    let cfile = cs(&file);
    let h = unsafe { crate::rc_notes_open_file(cfile.as_ptr()) };
    assert!(!h.is_null());
    assert_eq!(unsafe { crate::rc_notes_is_persistent(h) }, 1);

    // insert at clamped rows; NULL title reads as "".
    assert_eq!(unsafe { crate::rc_notes_insert(h, 9, cs("A").as_ptr(), cs("body a").as_ptr()) }, 0);
    assert_eq!(unsafe { crate::rc_notes_insert(h, 0, ptr::null(), cs("lead").as_ptr()) }, 0);
    assert_eq!(unsafe { crate::rc_notes_count(h) }, 2);

    // get/list over the ABI — the persisted object shape.
    let g = take_str(unsafe { crate::rc_notes_get(h, 0) }).unwrap();
    let gj: serde_json::Value = serde_json::from_str(&g).unwrap();
    assert_eq!(gj["title"], "");
    assert_eq!(gj["body"], "lead");
    assert_eq!(gj["markdown"], false);
    let l = take_str(unsafe { crate::rc_notes_list(h) }).unwrap();
    let lj: serde_json::Value = serde_json::from_str(&l).unwrap();
    assert_eq!(lj.as_array().unwrap().len(), 2);
    assert_eq!(lj[1]["title"], "A");
    assert!(unsafe { crate::rc_notes_get(h, 7) }.is_null());

    // Setters: changed flags mirror the model's setData contract.
    assert_eq!(
        unsafe { crate::rc_notes_set_title(h, 0, cs("lead t").as_ptr(), &mut changed) },
        RcStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe { crate::rc_notes_set_title(h, 0, cs("lead t").as_ptr(), &mut changed) },
        RcStatus::Ok
    );
    assert_eq!(changed, 0); // identical value — the setData false return
    assert_eq!(
        unsafe { crate::rc_notes_set_body(h, 1, cs("milk").as_ptr(), &mut changed) },
        RcStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe { crate::rc_notes_set_markdown(h, 1, 1, &mut changed) },
        RcStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe { crate::rc_notes_set_title(h, 9, cs("x").as_ptr(), &mut changed) },
        RcStatus::NotFound
    );
    // NULL out_changed is legal; a NULL value pointer is not.
    assert_eq!(
        unsafe { crate::rc_notes_set_title(h, 0, cs("z").as_ptr(), ptr::null_mut()) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::rc_notes_set_title(h, 0, ptr::null(), &mut changed) },
        RcStatus::InvalidArgument
    );

    // removeRows refusals then a real span.
    assert_eq!(
        unsafe { crate::rc_notes_remove(h, -1, 1) },
        RcStatus::InvalidArgument
    );
    assert_eq!(
        unsafe { crate::rc_notes_remove(h, 1, 5) },
        RcStatus::InvalidArgument
    );
    assert_eq!(unsafe { crate::rc_notes_remove(h, 0, 1) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_notes_count(h) }, 1);

    // Dirty until sync commits — the file materializes with the
    // parent dir created like the C++ mkpath.
    assert_eq!(unsafe { crate::rc_notes_is_dirty(h) }, 1);
    assert_eq!(unsafe { crate::rc_notes_sync(h) }, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_notes_is_dirty(h) }, 0);
    assert!(std::path::Path::new(&file).exists());
    unsafe { crate::rc_notes_close(h) };

    // A C++-shaped file reads identically through the FFI — the
    // spec's cross-write leg, both directions.
    let h2 = unsafe { crate::rc_notes_open_file(cfile.as_ptr()) };
    assert!(!h2.is_null());
    assert_eq!(unsafe { crate::rc_notes_count(h2) }, 1);
    let g = take_str(unsafe { crate::rc_notes_get(h2, 0) }).unwrap();
    let gj: serde_json::Value = serde_json::from_str(&g).unwrap();
    assert_eq!(gj["title"], "A");
    assert_eq!(gj["body"], "milk");
    assert_eq!(gj["markdown"], true);
    unsafe { crate::rc_notes_close(h2) };

    // The root+profile lane — profiles/<id>/notes.json.
    let pid = cs("work");
    let hp = unsafe { crate::rc_notes_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!hp.is_null());
    assert_eq!(unsafe { crate::rc_notes_insert(hp, 0, cs("w").as_ptr(), cs("wb").as_ptr()) }, 0);
    unsafe { crate::rc_notes_close(hp) }; // close flushes the dirty store
    assert!(std::path::Path::new(&format!("{root}/profiles/work/notes.json")).exists());

    // The facade spelling on the same store — one file, two lanes.
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());
    let an = unsafe { crate::facade::ac_notes(core, pid.as_ptr()) };
    assert!(!an.is_null());
    assert_eq!(unsafe { crate::facade::ac_notes_count(an) }, 1);
    let l = take_str(unsafe { crate::facade::ac_notes_list(an) }).unwrap();
    assert!(l.contains("\"title\":\"w\""));
    assert_eq!(
        unsafe {
            crate::facade::ac_notes_set_body(an, 0, cs("wb2").as_ptr(),
                &mut changed)
        },
        RcStatus::Ok
    );
    assert_eq!(changed, 1);
    assert_eq!(
        unsafe { crate::facade::ac_notes_insert(an, 5, cs("w2").as_ptr(), ptr::null()) },
        1
    );
    let g = take_str(unsafe { crate::facade::ac_notes_get(an, 1) }).unwrap();
    assert!(g.contains("\"title\":\"w2\""));
    assert_eq!(
        unsafe { crate::facade::ac_notes_set_markdown(an, 1, 1, &mut changed) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_notes_is_dirty(an) }, 1);
    assert_eq!(unsafe { crate::facade::ac_notes_sync(an) }, RcStatus::Ok);
    assert_eq!(
        unsafe { crate::facade::ac_notes_remove(an, 0, 1) },
        RcStatus::Ok
    );
    assert_eq!(unsafe { crate::facade::ac_notes_count(an) }, 1);
    assert_eq!(unsafe { crate::facade::ac_notes_is_persistent(an) }, 1);
    unsafe { crate::facade::ac_notes_close(an) };
    unsafe { crate::facade::ac_shutdown(core) };
}

// MD04: the download-interception decision layer over the C ABI —
// pure ops on a NULL store, the store-bound ops on a profile-bound
// settings handle, the records/rules lanes, and the facade spelling.
#[test]
fn dlwpolicy_surface() {
    let _g = guard();
    let root = tmpdir("dlwpolicy");
    let croot = cs(&root);
    let pid = cs("work");

    // Pure ops answer on a NULL store; store-bound ops refuse.
    let pure = b"{\"op\":\"sanitize_name\",\"suggested\":\"a/b/c.zip\"}";
    let got = take_str(unsafe {
        crate::rc_dlw_call(ptr::null(), pure.as_ptr(), pure.len())
    })
    .unwrap();
    assert!(got.contains("\"name\":\"c.zip\""));
    let bound = b"{\"op\":\"records_list\"}";
    assert!(unsafe {
        crate::rc_dlw_call(ptr::null(), bound.as_ptr(), bound.len())
    }
    .is_null());

    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!h.is_null());
    // Bad payloads degrade, never crash.
    assert!(unsafe {
        crate::rc_dlw_call(h, ptr::null(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_dlw_call(h, b"nope".as_ptr(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_dlw_call(h, b"{\"op\":\"bogus\"}".as_ptr(), 15)
    }
    .is_null());

    // A verdict composes over the profile's rules — a blocked
    // extension authored through the facade lane blocks.
    let set = b"{\"op\":\"rule_set\",\"key\":\"blocked_extensions\",\"value\":[\"exe\"]}";
    let r = take_str(unsafe {
        crate::rc_dlw_call(h, set.as_ptr(), set.len())
    })
    .unwrap();
    assert!(r.contains("\"mutated\":true"));
    let verd = b"{\"op\":\"verdict\",\"url\":\"https://h/f.exe\"}";
    let v = take_str(unsafe {
        crate::rc_dlw_call(h, verd.as_ptr(), verd.len())
    })
    .unwrap();
    assert!(v.contains("\"action\":\"block\""));

    // The records lane round-trips and clears.
    let save = b"{\"op\":\"records_save\",\"records\":[{\"url\":\"https://h/a\",\"location\":\"/tmp/a\",\"done\":true,\"size\":7}]}";
    let r = take_str(unsafe {
        crate::rc_dlw_call(h, save.as_ptr(), save.len())
    })
    .unwrap();
    assert!(r.contains("\"saved\":1"));
    let l = take_str(unsafe {
        crate::rc_dlw_call(h, bound.as_ptr(), bound.len())
    })
    .unwrap();
    assert!(l.contains("\"url\":\"https://h/a\""));
    assert!(std::path::Path::new(&format!("{root}/profiles/work/downloads.json")).exists());
    let clr = b"{\"op\":\"records_clear\"}";
    let r = take_str(unsafe {
        crate::rc_dlw_call(h, clr.as_ptr(), clr.len())
    })
    .unwrap();
    assert!(r.contains("\"cleared\":true"));
    unsafe { crate::rc_settings_close(h) };

    // The facade spelling on the same profile — one store, two lanes.
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());
    let ah = unsafe { crate::facade::ac_dlwpolicy(core, pid.as_ptr()) };
    assert!(!ah.is_null());
    let v = take_str(unsafe {
        crate::facade::ac_dlw_call(ah, verd.as_ptr(), verd.len())
    })
    .unwrap();
    assert!(v.contains("\"action\":\"block\""));
    let l = take_str(unsafe {
        crate::facade::ac_dlw_call(ah, bound.as_ptr(), bound.len())
    })
    .unwrap();
    assert!(l.contains("\"records\":[]"));
    unsafe { crate::facade::ac_dlwpolicy_close(ah) };
    // A NULL facade handle degrades.
    assert!(unsafe {
        crate::facade::ac_dlw_call(ptr::null(), pure.as_ptr(), pure.len())
    }
    .is_null());
    unsafe { crate::facade::ac_shutdown(core) };
    let _ = std::fs::remove_dir_all(&root);
}

// MD05: the privacy-policy store view over the C ABI — the pure
// "defaults" op on a NULL store, the bound-profile flags/gpc/
// verdict lanes, the int gpc surface, and the facade spelling.
#[test]
fn privacy_surface() {
    let _g = guard();
    let root = tmpdir("privacy");
    let croot = cs(&root);
    let pid = cs("work");

    // "defaults" answers on a NULL store; store-bound ops refuse.
    let pure = b"{\"op\":\"defaults\"}";
    let got = take_str(unsafe {
        crate::rc_privacy_call(ptr::null(), pure.as_ptr(), pure.len())
    })
    .unwrap();
    assert!(got.contains("\"gpc_signal\":true"));
    let bound = b"{\"op\":\"flags\"}";
    assert!(unsafe {
        crate::rc_privacy_call(ptr::null(), bound.as_ptr(), bound.len())
    }
    .is_null());
    assert_eq!(
        unsafe { crate::rc_privacy_gpc(ptr::null_mut(), 0) },
        -1
    );

    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!h.is_null());
    // Bad payloads degrade, never crash.
    assert!(unsafe {
        crate::rc_privacy_call(h, ptr::null(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_privacy_call(h, b"nope".as_ptr(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_privacy_call(h, b"{\"op\":\"bogus\"}".as_ptr(), 15)
    }
    .is_null());

    // A pristine persona: every fail-on toggle true, gpc sends,
    // the unified verdict folds all three reads.
    let f = take_str(unsafe {
        crate::rc_privacy_call(h, bound.as_ptr(), bound.len())
    })
    .unwrap();
    assert!(f.contains("\"gpc_signal\":true"));
    assert!(f.contains("\"strip_tracking_params\":true"));
    assert!(f.contains("\"adblock_enabled\":true"));
    assert_eq!(unsafe { crate::rc_privacy_gpc(h, 0) }, 1);
    let verd = b"{\"op\":\"verdict\"}";
    let v = take_str(unsafe {
        crate::rc_privacy_call(h, verd.as_ptr(), verd.len())
    })
    .unwrap();
    assert!(v.contains("\"gpc\":true"));

    // A per-profile write flips only that persona's answer — and
    // tor_mode overrides either way.
    let gpckey = cs("privacy/gpcSignal");
    let set = unsafe {
        crate::rc_settings_set_bool(h, gpckey.as_ptr(), 0)
    };
    assert_eq!(set, RcStatus::Ok);
    assert_eq!(unsafe { crate::rc_privacy_gpc(h, 0) }, 0);
    assert_eq!(unsafe { crate::rc_privacy_gpc(h, 1) }, 1);
    let gq = b"{\"op\":\"gpc\"}";
    let g = take_str(unsafe {
        crate::rc_privacy_call(h, gq.as_ptr(), gq.len())
    })
    .unwrap();
    assert!(g.contains("\"send\":false"));
    unsafe { crate::rc_settings_close(h) };

    // The facade spelling on the same profile — one store, two
    // lanes.
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());
    let ah = unsafe { crate::facade::ac_privacy(core, pid.as_ptr()) };
    assert!(!ah.is_null());
    assert_eq!(unsafe { crate::facade::ac_privacy_gpc(ah, 0) }, 0);
    assert_eq!(unsafe { crate::facade::ac_privacy_gpc(ah, 1) }, 1);
    let other = unsafe {
        crate::facade::ac_privacy(core, cs("other").as_ptr())
    };
    assert_eq!(unsafe { crate::facade::ac_privacy_gpc(other, 0) }, 1);
    let vf = take_str(unsafe {
        crate::facade::ac_privacy_call(ah, verd.as_ptr(), verd.len())
    })
    .unwrap();
    assert!(vf.contains("\"gpc\":false"));
    unsafe { crate::facade::ac_privacy_close(ah) };
    unsafe { crate::facade::ac_privacy_close(other) };
    // NULL facade handles degrade.
    assert!(unsafe {
        crate::facade::ac_privacy_call(ptr::null(), pure.as_ptr(), pure.len())
    }
    .is_null());
    assert_eq!(unsafe { crate::facade::ac_privacy_gpc(ptr::null(), 0) }, -1);
    unsafe { crate::facade::ac_shutdown(core) };
    let _ = std::fs::remove_dir_all(&root);
}

// MD06: the per-persona URL-sanitize policy over the C ABI — the
// pure "defaults" op on a NULL store, the bound-profile clean /
// ruleset / toggle lanes, the legacy extraParams merge, and the
// facade spelling.
#[test]
fn urlclean_surface() {
    let _g = guard();
    let root = tmpdir("urlclean");
    let croot = cs(&root);
    let pid = cs("work");

    // "defaults" answers on a NULL store; store-bound ops refuse.
    let pure = b"{\"op\":\"defaults\"}";
    let got = take_str(unsafe {
        crate::rc_urlclean_call(ptr::null(), pure.as_ptr(), pure.len())
    })
    .unwrap();
    assert!(got.contains("\"copy_enabled\":true"));
    let bound = b"{\"op\":\"clean\",\"url\":\"https://a/?utm_source=x\"}";
    assert!(unsafe {
        crate::rc_urlclean_call(ptr::null(), bound.as_ptr(), bound.len())
    }
    .is_null());

    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!h.is_null());
    // Bad payloads degrade, never crash.
    assert!(unsafe {
        crate::rc_urlclean_call(h, ptr::null(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_urlclean_call(h, b"nope".as_ptr(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_urlclean_call(h, b"{\"op\":\"bogus\"}".as_ptr(), 15)
    }
    .is_null());

    // A pristine persona cleans with the vendored ruleset.
    let v = take_str(unsafe {
        crate::rc_urlclean_call(h, bound.as_ptr(), bound.len())
    })
    .unwrap();
    assert!(v.contains("\"changed\":true"));
    assert!(v.contains("https://a/\""));

    // The legacy extraParams list merges on top of the ruleset.
    let extras = cs("urlcleaner/extraParams");
    let list = b"[\"partner\",\"session*\"]";
    assert_eq!(
        unsafe {
            crate::rc_settings_set_list(h, extras.as_ptr(),
                                        list.as_ptr(), list.len())
        },
        RcStatus::Ok
    );
    let e = b"{\"op\":\"clean\",\"url\":\"https://a/?partner=p&id=1\",\"purpose\":\"copy\"}";
    let v = take_str(unsafe {
        crate::rc_urlclean_call(h, e.as_ptr(), e.len())
    })
    .unwrap();
    assert!(v.contains("https://a/?id=1"));

    // A per-profile ruleset override replaces the global list;
    // urlclean.json lands under the bound profile dir.
    let set = b"{\"op\":\"ruleset_set\",\"ruleset\":{\"version\":1,\"params\":[\"only_this\"]}}";
    let r = take_str(unsafe {
        crate::rc_urlclean_call(h, set.as_ptr(), set.len())
    })
    .unwrap();
    assert!(r.contains("\"mutated\":true"));
    assert!(std::path::Path::new(&format!("{root}/profiles/work/urlclean.json")).exists());
    let v = take_str(unsafe {
        crate::rc_urlclean_call(h, bound.as_ptr(), bound.len())
    })
    .unwrap();
    assert!(v.contains("\"changed\":false"));
    // The composite the snapshot loader embeds.
    let cp = b"{\"op\":\"clean_policy\"}";
    let v = take_str(unsafe {
        crate::rc_urlclean_call(h, cp.as_ptr(), cp.len())
    })
    .unwrap();
    assert!(v.contains("\"source\":\"profile\""));
    assert!(v.contains("only_this"));
    assert!(v.contains("partner"));
    let rs = b"{\"op\":\"ruleset_reset\"}";
    let r = take_str(unsafe {
        crate::rc_urlclean_call(h, rs.as_ptr(), rs.len())
    })
    .unwrap();
    assert!(r.contains("\"reset\":true"));

    // The copy toggle is facade-authored; the nav toggle stays
    // Qt-owned and refuses the write.
    let se = b"{\"op\":\"set_enabled\",\"purpose\":\"copy\",\"value\":false}";
    let r = take_str(unsafe {
        crate::rc_urlclean_call(h, se.as_ptr(), se.len())
    })
    .unwrap();
    assert!(r.contains("\"mutated\":true"));
    let v = take_str(unsafe {
        crate::rc_urlclean_call(h, e.as_ptr(), e.len())
    })
    .unwrap();
    assert!(v.contains("\"enabled\":false"));
    let sne = b"{\"op\":\"set_enabled\",\"purpose\":\"nav\",\"value\":false}";
    assert!(unsafe {
        crate::rc_urlclean_call(h, sne.as_ptr(), sne.len())
    }
    .is_null());
    unsafe { crate::rc_settings_close(h) };

    // The facade spelling on the same profile — one store, two
    // lanes.
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());
    let ah = unsafe { crate::facade::ac_urlclean(core, pid.as_ptr()) };
    assert!(!ah.is_null());
    let v = take_str(unsafe {
        crate::facade::ac_urlclean_call(ah, e.as_ptr(), e.len())
    })
    .unwrap();
    assert!(v.contains("\"enabled\":false"));
    let other = unsafe {
        crate::facade::ac_urlclean(core, cs("other").as_ptr())
    };
    let v = take_str(unsafe {
        crate::facade::ac_urlclean_call(other, bound.as_ptr(), bound.len())
    })
    .unwrap();
    assert!(v.contains("\"changed\":true"));
    unsafe { crate::facade::ac_urlclean_close(ah) };
    unsafe { crate::facade::ac_urlclean_close(other) };
    // A NULL facade handle degrades.
    assert!(unsafe {
        crate::facade::ac_urlclean_call(ptr::null(), pure.as_ptr(), pure.len())
    }
    .is_null());
    unsafe { crate::facade::ac_shutdown(core) };
    let _ = std::fs::remove_dir_all(&root);
}

// MD08: the session snapshot/restore policy view over the C ABI —
// the pure "defaults" op on a NULL store, the bound-profile reads
// (snapshot timing, dirty classes, retention, staleness, restore
// answers), and the facade spelling on the AcSession handle.
#[test]
fn sesspolicy_surface() {
    let _g = guard();
    let root = tmpdir("sesspolicy");
    let croot = cs(&root);
    let pid = cs("work");

    // "defaults" answers on a NULL store; "stale" refuses without
    // a bound dir; the other reads degrade to the compiled-in
    // constants rather than guessing.
    let pure = b"{\"op\":\"defaults\"}";
    let got = take_str(unsafe {
        crate::rc_sesspolicy_call(ptr::null(), pure.as_ptr(), pure.len())
    })
    .unwrap();
    assert!(got.contains("\"interval_ms\":3000"));
    assert!(got.contains("\"maxwait_ms\":15000"));
    assert!(got.contains("\"crash_restore\":\"ask\""));
    assert!(got.contains("\"startup_default\":2"));
    let stale = b"{\"op\":\"stale\"}";
    assert!(unsafe {
        crate::rc_sesspolicy_call(ptr::null(), stale.as_ptr(), stale.len())
    }
    .is_null());
    let snap = b"{\"op\":\"snapshot\"}";
    let got = take_str(unsafe {
        crate::rc_sesspolicy_call(ptr::null(), snap.as_ptr(), snap.len())
    })
    .unwrap();
    assert!(got.contains("\"interval_ms\":3000"));

    let h = unsafe { crate::rc_settings_open(croot.as_ptr(), pid.as_ptr()) };
    assert!(!h.is_null());
    // Bad payloads degrade, never crash.
    assert!(unsafe {
        crate::rc_sesspolicy_call(h, ptr::null(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_sesspolicy_call(h, b"nope".as_ptr(), 4)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_sesspolicy_call(h, b"{\"op\":\"bogus\"}".as_ptr(), 15)
    }
    .is_null());
    assert!(unsafe {
        crate::rc_sesspolicy_call(h, b"{\"op\":\"dirty\"}".as_ptr(), 14)
    }
    .is_null());

    // A pristine persona: every dirty class on, no staleness.
    let dk = b"{\"op\":\"dirty\",\"kind\":\"chrome\"}";
    let v = take_str(unsafe {
        crate::rc_sesspolicy_call(h, dk.as_ptr(), dk.len())
    })
    .unwrap();
    assert!(v.contains("\"dirty\":true"));
    let v = take_str(unsafe {
        crate::rc_sesspolicy_call(h, stale.as_ptr(), stale.len())
    })
    .unwrap();
    assert!(v.contains("\"stale\":false"));

    // A per-profile write flips only that persona's answer.
    let chrome_key = cs("session/dirtyOnChrome");
    assert_eq!(
        unsafe { crate::rc_settings_set_bool(h, chrome_key.as_ptr(), 0) },
        RcStatus::Ok
    );
    let v = take_str(unsafe {
        crate::rc_sesspolicy_call(h, dk.as_ptr(), dk.len())
    })
    .unwrap();
    assert!(v.contains("\"dirty\":false"));
    let tabs_kind = b"{\"op\":\"dirty\",\"kind\":\"tabs\"}";
    let v = take_str(unsafe {
        crate::rc_sesspolicy_call(h, tabs_kind.as_ptr(), tabs_kind.len())
    })
    .unwrap();
    assert!(v.contains("\"dirty\":true"));

    // The lifecycle op resolves the Qt-owned tab rows verbatim —
    // pristine answers the C++ defaults, a dialog row lands live.
    let lc = b"{\"op\":\"lifecycle\"}";
    let v = take_str(unsafe {
        crate::rc_sesspolicy_call(h, lc.as_ptr(), lc.len())
    })
    .unwrap();
    assert!(v.contains("\"suspend_idle_ms\":0"));
    assert!(v.contains("\"memory_purge\":true"));
    assert!(v.contains("\"lazy_restore\":false"));
    assert!(v.contains("\"lazy_restore_pinned\":true"));
    let suspend_key = cs("tabs/suspendTabs");
    assert_eq!(
        unsafe { crate::rc_settings_set_bool(h, suspend_key.as_ptr(), 1) },
        RcStatus::Ok
    );
    let v = take_str(unsafe {
        crate::rc_sesspolicy_call(h, lc.as_ptr(), lc.len())
    })
    .unwrap();
    assert!(v.contains("\"suspend_idle_ms\":1800000"));
    unsafe { crate::rc_settings_close(h) };

    // The facade spelling on the same profile — one store, two
    // lanes, and "defaults" never opens the store.
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());
    let ah = unsafe { crate::facade::ac_session(core, pid.as_ptr()) };
    assert!(!ah.is_null());
    let v = take_str(unsafe {
        crate::facade::ac_session_policy(ah, dk.as_ptr(), dk.len())
    })
    .unwrap();
    assert!(v.contains("\"dirty\":false"));
    let v = take_str(unsafe {
        crate::facade::ac_session_policy(ah, pure.as_ptr(), pure.len())
    })
    .unwrap();
    assert!(v.contains("\"interval_ms\":3000"));
    let other = unsafe { crate::facade::ac_session(core, cs("other").as_ptr()) };
    let v = take_str(unsafe {
        crate::facade::ac_session_policy(other, dk.as_ptr(), dk.len())
    })
    .unwrap();
    assert!(v.contains("\"dirty\":true"));
    unsafe { crate::facade::ac_session_close(ah) };
    unsafe { crate::facade::ac_session_close(other) };
    // A NULL facade handle degrades.
    assert!(unsafe {
        crate::facade::ac_session_policy(ptr::null(), pure.as_ptr(), pure.len())
    }
    .is_null());
    unsafe { crate::facade::ac_shutdown(core) };
    let _ = std::fs::remove_dir_all(&root);
}

// MD02/MD09: the navigation-policy endpoints over the C ABI — the
// phased decide manifest, the granular ops (predicates + the popup
// gate's decide/record surface), and the facade spellings on
// AcPolicy.  The popup matrix mirrors tst_popupblocker.
#[test]
fn navpolicy_surface() {
    let _g = guard();
    let root = tmpdir("navpolicy");
    let croot = cs(&root);
    // The persona-independent anchor — the dir must exist for the
    // persisted popup rows (atomic_write never mkpaths).
    std::fs::create_dir_all(format!("{root}/profiles/default")).unwrap();
    init(&format!("{root}/profiles/default"));

    // rc_nav_decide: bad payloads degrade to NULL (the adapter
    // fails open), a phased manifest answers a verdict.
    assert!(unsafe { crate::rc_nav_decide(ptr::null(), 4) }.is_null());
    assert!(unsafe { crate::rc_nav_decide(b"nope".as_ptr(), 4) }.is_null());
    let m = br#"{"url":"mailto:a@b.c","is_main_frame":true}"#;
    let v = take_str(unsafe {
        crate::rc_nav_decide(m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("\"action\":\"ask\""));
    let m = br#"{"url":"https://e.example/","is_main_frame":true}"#;
    let v = take_str(unsafe {
        crate::rc_nav_decide(m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("\"action\":\"continue\""));

    // rc_nav_call: bad payloads degrade, the predicates answer.
    assert!(unsafe { crate::rc_nav_call(ptr::null(), 4) }.is_null());
    assert!(unsafe { crate::rc_nav_call(b"nope".as_ptr(), 4) }.is_null());
    assert!(unsafe {
        crate::rc_nav_call(b"{\"op\":\"bogus\"}".as_ptr(), 15)
    }
    .is_null());
    let m = br#"{"op":"browser_scheme","scheme":"https"}"#;
    let v = take_str(unsafe {
        crate::rc_nav_call(m.as_ptr(), m.len())
    })
    .unwrap();
    assert_eq!(v, "true");

    // ---- popup gate parity (the tst_popupblocker matrix) ----
    let decide = |url: &str, enabled: bool| -> String {
        let m = format!(
            "{{\"op\":\"popup_decide\",\"url\":\"{url}\",\"shaped\":true,\"enabled\":{enabled}}}"
        );
        take_str(unsafe {
            crate::rc_nav_call(m.as_ptr(), m.len())
        })
        .unwrap()
    };
    // Enabled + shaped + http(s) gates; the flag and the scheme
    // floor allow everything else.
    assert!(decide("http://example.com/", true).contains("\"block\""));
    assert!(decide("https://example.com/", true).contains("\"block\""));
    assert!(decide("http://example.com/", false)
        .contains("\"allow\""));
    for u in [
        "file:///tmp/x.html",
        "about:blank",
        "data:text/html,x",
        "qrc:/x.html",
    ] {
        assert!(decide(u, true).contains("\"allow\""), "{u}");
    }

    // popup_allow/persist -> decide allows, subdomains cover,
    // siblings never; popup_remove scrubs; popup_list enumerates.
    let allow = |host: &str, persistent: bool| -> String {
        let m = format!(
            "{{\"op\":\"popup_allow\",\"host\":\"{host}\",\"persistent\":{persistent}}}"
        );
        take_str(unsafe {
            crate::rc_nav_call(m.as_ptr(), m.len())
        })
        .unwrap()
    };
    allow("example.com", true);
    assert!(decide("http://example.com/", true).contains("\"allow\""));
    assert!(decide("http://www.example.com/", true)
        .contains("\"allow\""));
    assert!(decide("http://notexample.com/", true)
        .contains("\"block\""));
    // Session overlay matches but never lists.
    allow("session.test", false);
    assert!(decide("http://www.session.test/", true)
        .contains("\"allow\""));
    let m = b"{\"op\":\"popup_list\"}";
    let v = take_str(unsafe {
        crate::rc_nav_call(m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("example.com"));
    assert!(!v.contains("session.test"));
    // popup_clear_session drops the overlay only.
    let m = b"{\"op\":\"popup_clear_session\"}";
    let v = take_str(unsafe {
        crate::rc_nav_call(m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("\"cleared\":true"));
    assert!(decide("http://session.test/", true).contains("\"block\""));
    assert!(decide("http://example.com/", true).contains("\"allow\""));
    // popup_remove scrubs the persistent row.
    let m = br#"{"op":"popup_remove","host":"example.com"}"#;
    let v = take_str(unsafe {
        crate::rc_nav_call(m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("\"removed_persistent\":true"));
    assert!(decide("http://example.com/", true).contains("\"block\""));

    // The facade spellings on a "default"-bound AcPolicy — the
    // persona-independent anchor the Qt adapter uses.
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());
    let ah = unsafe { crate::facade::ac_policy(core, cs("default").as_ptr()) };
    assert!(!ah.is_null());
    let m = b"{\"op\":\"popup_list\"}";
    let v = take_str(unsafe {
        crate::facade::ac_nav_call(ah, m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("\"hosts\":[]"));
    let a = br#"{"op":"popup_allow","host":"facade.example","persistent":true}"#;
    let v = take_str(unsafe {
        crate::facade::ac_nav_call(ah, a.as_ptr(), a.len())
    })
    .unwrap();
    assert!(v.contains("\"recorded\":true"));
    assert!(decide("http://facade.example/", true).contains("\"allow\""));
    let m = br#"{"url":"https://e.example/","is_main_frame":true}"#;
    let v = take_str(unsafe {
        crate::facade::ac_nav_decide(ah, m.as_ptr(), m.len())
    })
    .unwrap();
    assert!(v.contains("\"action\":\"continue\""));
    unsafe { crate::facade::ac_policy_close(ah) };
    // NULL facade handles degrade.
    assert!(unsafe {
        crate::facade::ac_nav_call(ptr::null(), m.as_ptr(), m.len())
    }
    .is_null());
    assert!(unsafe {
        crate::facade::ac_nav_decide(ptr::null(), m.as_ptr(), m.len())
    }
    .is_null());
    unsafe { crate::facade::ac_shutdown(core) };
    let _ = std::fs::remove_dir_all(&root);
}
// ---- MB03: the namespaced event lane ------------------------------------
//
// ac_events_* over the C ABI: per-core subscription with a topic
// filter, a v1 JSON payload stamped with the bound profile, and the
// registry.  The history mutation sites are the first real producer
// ("history/changed" alongside the legacy "history" emit).

#[test]
fn facade_event_lane_round_trip() {
    let _g = guard();
    let root = tmpdir("ac-ev");
    let croot = cs(&root);
    let cfg = ac_cfg(&croot);
    let core = unsafe { crate::facade::ac_init(&cfg) };
    assert!(!core.is_null());

    static HITS: AtomicUsize = AtomicUsize::new(0);
    thread_local! {
        static LAST: std::cell::RefCell<(String, String)> =
            const { std::cell::RefCell::new((String::new(), String::new())) };
    }
    unsafe extern "C" fn ev_cb(
        topic: *const std::os::raw::c_char,
        payload: *const std::os::raw::c_char,
        _ud: *mut c_void,
    ) {
        let t = unsafe { CStr::from_ptr(topic) }
            .to_string_lossy()
            .into_owned();
        let p = unsafe { CStr::from_ptr(payload) }
            .to_string_lossy()
            .into_owned();
        LAST.with(|l| *l.borrow_mut() = (t, p));
        HITS.fetch_add(1, Ordering::SeqCst);
    }

    // Registry: declared contract, readable without a core.
    let json =
        take_str(crate::facade::ac_events_topics()).expect("topics registry");
    let doc: serde_json::Value = serde_json::from_str(&json).unwrap();
    let rows = doc.as_array().unwrap();
    let hist = rows
        .iter()
        .find(|r| r["topic"] == "history/changed")
        .expect("history/changed declared");
    assert_eq!(hist["produced"], true);
    assert!(rows
        .iter()
        .any(|r| r["topic"] == "policy/changed" && r["produced"] == false));

    // Bad calls degrade, never crash.
    assert_eq!(
        unsafe {
            crate::facade::ac_events_subscribe(
                ptr::null_mut(),
                ptr::null(),
                Some(ev_cb),
                ptr::null_mut(),
            )
        },
        0
    );
    assert_eq!(
        unsafe {
            crate::facade::ac_events_subscribe(
                core,
                ptr::null(),
                None,
                ptr::null_mut(),
            )
        },
        0
    );

    // A "history/" subscriber observes the facade write, stamped
    // with the bound profile.
    let hsub = unsafe {
        crate::facade::ac_events_subscribe(
            core,
            cs("history/").as_ptr(),
            Some(ev_cb),
            ptr::null_mut(),
        )
    };
    assert_ne!(hsub, 0);
    // An exact-topic subscriber on an unrelated topic stays silent.
    let ssub = unsafe {
        crate::facade::ac_events_subscribe(
            core,
            cs("settings/changed").as_ptr(),
            Some(ev_cb),
            ptr::null_mut(),
        )
    };
    assert_ne!(ssub, 0);

    let h = unsafe { crate::facade::ac_history(core, cs("work").as_ptr()) };
    assert!(!h.is_null());
    let base = HITS.load(Ordering::SeqCst);
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                h,
                cs("https://ev.example/").as_ptr(),
                cs("EV").as_ptr(),
                1,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(HITS.load(Ordering::SeqCst) - base, 1);
    LAST.with(|l| {
        let (t, p) = l.borrow().clone();
        assert_eq!(t, "history/changed");
        let doc: serde_json::Value = serde_json::from_str(&p).unwrap();
        assert_eq!(doc["v"], 1);
        assert_eq!(doc["topic"], "history/changed");
        assert_eq!(doc["profile"], "work");
        assert_eq!(doc["op"], "visit");
    });

    // seen flipped on the registry after a real emit.
    let json = take_str(crate::facade::ac_events_topics()).unwrap();
    let doc: serde_json::Value = serde_json::from_str(&json).unwrap();
    assert_eq!(
        doc.as_array()
            .unwrap()
            .iter()
            .find(|r| r["topic"] == "history/changed")
            .unwrap()["seen"],
        true
    );

    // Unsubscribe is core-scoped: a second core cannot drop it.
    let root2 = tmpdir("ac-ev2");
    let croot2 = cs(&root2);
    let cfg2 = ac_cfg(&croot2);
    let core2 = unsafe { crate::facade::ac_init(&cfg2) };
    assert!(!core2.is_null());
    assert_eq!(
        unsafe { crate::facade::ac_events_unsubscribe(core2, hsub) },
        RcStatus::NotFound
    );
    unsafe { crate::facade::ac_shutdown(core2) };

    // unsubscribe silences; shutdown-after-unsubscribe is safe.
    assert_eq!(
        unsafe { crate::facade::ac_events_unsubscribe(core, hsub) },
        RcStatus::Ok
    );
    assert_eq!(
        unsafe { crate::facade::ac_events_unsubscribe(core, hsub) },
        RcStatus::NotFound
    );
    let n = HITS.load(Ordering::SeqCst);
    assert_eq!(
        unsafe {
            crate::facade::ac_hist_add(
                h,
                cs("https://ev2.example/").as_ptr(),
                cs("EV2").as_ptr(),
                2,
            )
        },
        RcStatus::Ok
    );
    assert_eq!(HITS.load(Ordering::SeqCst), n);
    unsafe { crate::facade::ac_hist_close(h) };

    // Shutdown clears silently — the wildcard sub never fires again.
    let wsub = unsafe {
        crate::facade::ac_events_subscribe(
            core,
            ptr::null(),
            Some(ev_cb),
            ptr::null_mut(),
        )
    };
    assert_ne!(wsub, 0);
    assert_eq!(
        unsafe { crate::facade::ac_events_unsubscribe(core, ssub) },
        RcStatus::Ok
    );
    unsafe { crate::facade::ac_shutdown(core) };
    let n = HITS.load(Ordering::SeqCst);
    init(&root);
    let db = cs(&format!("{root}/profiles/work/history.db"));
    assert_eq!(unsafe { crate::rc_hist_open(db.as_ptr()) }, RcStatus::Ok);
    assert_eq!(
        unsafe {
            crate::rc_hist_add(
                cs("https://post.example/").as_ptr(),
                cs("P").as_ptr(),
                3,
            )
        },
        RcStatus::Ok
    );
    // The dead core's wildcard sub must not fire; any hit here would
    // also prove emit_event still runs (it does — seen/legacy lanes
    // are independent), so only the count matters.
    assert_eq!(HITS.load(Ordering::SeqCst), n);
    let _ = std::fs::remove_dir_all(&root);
    let _ = std::fs::remove_dir_all(&root2);
}

// ---- MB04: the engine capability matrix ---------------------------------
//
// paths, and the schema the JSON emits.

fn caps_json(engine_id: *const std::os::raw::c_char) -> Option<String> {
    let mut out = out_buf();
    let st = unsafe { crate::facade::ac_engine_caps(engine_id, &mut out) };
    if st != RcStatus::Ok {
        assert!(out.data.is_null(), "a failed caps call leaves no buffer");
        return None;
    }
    let v = take_buf(&mut out);
    Some(String::from_utf8(v).unwrap())
}

#[test]
fn facade_engine_caps() {
    // NULL emits the whole matrix — all three engines, v1 schema.
    let all = caps_json(ptr::null()).expect("matrix");
    let doc: serde_json::Value = serde_json::from_str(&all).unwrap();
    assert_eq!(doc["v"], 1);
    let engines = doc["engines"].as_array().unwrap();
    assert_eq!(engines.len(), 3);
    for e in engines {
        let features = e["features"].as_object().unwrap();
        for req in [
            "devtools",
            "pdf_viewer",
            "extensions_mv2",
            "extensions_mv3",
            "webchannel",
            "otr_profile",
            "tor",
            "containers",
            "reader_mode",
            "downloads",
            "sessions",
            "favicons",
            "opensearch",
            "site_permissions",
            "autofill",
        ] {
            assert!(features.contains_key(req), "{} misses {}", e["engine"], req);
        }
    }

    // Canonical id and the backend->id() spelling resolve the same row.
    let qe = caps_json(cs("qtwebengine").as_ptr()).unwrap();
    let doc: serde_json::Value = serde_json::from_str(&qe).unwrap();
    assert_eq!(doc["engine"], "qtwebengine");
    assert_eq!(doc["features"]["devtools"]["state"], "supported");
    assert_eq!(
        caps_json(cs("webengine").as_ptr()).unwrap(),
        qe
    );

    // Honesty pins: servo devtools and webkit MV3 stay off.
    let servo: serde_json::Value = serde_json::from_str(
        &caps_json(cs("servo").as_ptr()).unwrap(),
    )
    .unwrap();
    assert_eq!(servo["features"]["devtools"]["state"], "unsupported");
    let wk: serde_json::Value = serde_json::from_str(
        &caps_json(cs("webkit").as_ptr()).unwrap(),
    )
    .unwrap();
    assert_eq!(wk["features"]["extensions_mv3"]["state"], "unsupported");

    // Unknown id fails clean with a stashed error; NULL out too.
    assert!(caps_json(cs("gecko").as_ptr()).is_none());
    assert!(!unsafe { crate::facade::ac_last_error() }.is_null());
    assert_eq!(
        unsafe {
            crate::facade::ac_engine_caps(
                cs("qtwebengine").as_ptr(),
                ptr::null_mut(),
            )
        },
        RcStatus::InvalidArgument
    );
}
