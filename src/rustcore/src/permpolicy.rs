//! MD03: permpolicy — the SEC05 permission-broker decision layer.
//!
//! `WebPermissionManager` used to own every "may this page have this
//! feature?" decision in C++: the hard-deny table, the ask-capable
//! set, the security-origin key normalization, the remembered-decision
//! store and the session-scoped answer map.  The durable rows moved to
//! sitedecisions.json under SITED01; this module moves the remaining
//! pure policy behind one JSON manifest in / verdict out contract so
//! a non-Qt shell — or a second engine backend — resolves requests
//! identically:
//!
//!   manifest in -> {"origin","type","otr"}
//!   verdict out -> {"action":"deny","source":"feature"}
//!                | {"action":"allow"|"deny","source":"stored"|"session"}
//!                | {"action":"ask"}
//!
//! Mirroring rules (kept 1:1 with webpermissionmanager.cpp):
//!   * the feature tables are the compiled-in security boundary:
//!     capture devices, screen capture, clipboard reads, local-font
//!     enumeration and anything unmapped deny outright — the prompt
//!     is the ONLY path to grant, so a type that is not promptable
//!     resolves deny before any store is consulted;
//!   * the origin key is the bare scheme://host:port — userinfo,
//!     path, query and fragment stripped — percent-encoded whole,
//!     then "/<TypeName>".  The manifest carries QUrl::toEncoded()
//!     output, so host case, default ports and IDN are already
//!     normalized exactly as the legacy keyFor produced them; the
//!     strip + encode half lives here byte-identically;
//!   * the persistent store is sitedecisions.json's "webperm" kind —
//!     persona-independent per MPROF02, so the caller anchors the
//!     default profile's dir exactly like the Qt adapters do;
//!   * session answers are process-global and OTR-namespaced
//!     ("otr/" + key) — an OTR page reads the persistent store but
//!     its answers can never bleed into normal browsing or outlive
//!     the run;
//!   * answering a prompt ALWAYS session-records (a page looping
//!     requestPermission() must not spam modals); persisting is
//!     opt-in and never happens off-the-record;
//!   * removing a stored row also drops both session variants;
//!     clearing empties the kind and the whole session map.
//!
//! The dialog machinery — serializing the modal queue, building the
//! QMessageBox, resolving the engine's permission object — stays
//! Qt-side execution; this module only answers questions.

use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::sync::{LazyLock, Mutex};

use crate::error::{fail, RcResult, RcStatus};
use crate::sitedecisions;

/// The durable kind — SiteDecisionStore::KindWebPermission, the same
/// rows importLegacyWebPermissions replayed from the legacy
/// "webpermissions" QSettings group.
const KIND_WEBPERM: &str = "webperm";

/// QWebEnginePermission::PermissionType names (QMetaEnum key spellings
/// — the wire strings the persisted rows already carry) denied before
/// the store or the prompt is consulted.  WebPermissionManager::
/// isDenied verbatim.
const HARD_DENY: [&str; 8] = [
    "MediaAudioCapture",
    "MediaVideoCapture",
    "MediaAudioVideoCapture",
    "DesktopVideoCapture",
    "DesktopAudioVideoCapture",
    "ClipboardReadWrite",
    "LocalFontsAccess",
    "Unsupported",
];

/// The ask-capable set — WebPermissionManager::isPromptable verbatim.
/// Anything not listed resolves deny by the `!isPromptable` half of
/// the old gate, so an enum value this build does not know fails
/// closed, never open.
const PROMPTABLE: [&str; 3] = ["Notifications", "Geolocation", "MouseLock"];

const MAX_ORIGIN_LEN: usize = 4096;
const MAX_TYPE_LEN: usize = 64;
const MAX_SESSION_ENTRIES: usize = 8192;

// ---- session-scoped answers ----------------------------------------------
// The anti-prompt-storm layer: answers that were not persisted (the
// checkbox was off, or the page is off-the-record) live for the run.
// Process-global like the Qt hash it replaces — one store behind one
// Mutex, OTR answers namespaced so they cannot bleed into normal
// browsing.

fn session() -> &'static LazyLock<Mutex<BTreeMap<String, bool>>> {
    static MAP: LazyLock<Mutex<BTreeMap<String, bool>>> =
        LazyLock::new(|| Mutex::new(BTreeMap::new()));
    &MAP
}

/// sessionKeyFor verbatim: OTR answers live under their own prefix.
fn session_key(key: &str, otr: bool) -> String {
    if otr {
        format!("otr/{key}")
    } else {
        key.to_string()
    }
}

fn session_get(key: &str, otr: bool) -> Option<bool> {
    session()
        .lock()
        .unwrap()
        .get(&session_key(key, otr))
        .copied()
}

fn session_set(key: &str, otr: bool, granted: bool) {
    let mut map = session().lock().unwrap();
    // The Qt hash was unbounded, but it could only grow one answered
    // prompt at a time; the cap keeps that shape without letting a
    // corrupt caller grow memory forever.
    if map.len() >= MAX_SESSION_ENTRIES
        && !map.contains_key(&session_key(key, otr))
    {
        return;
    }
    map.insert(session_key(key, otr), granted);
}

/// removeEntry's session half — both variants, so revoking a stored
/// row also drops the answer the page is currently running under.
fn session_remove_both(key: &str) {
    let mut map = session().lock().unwrap();
    map.remove(&session_key(key, false));
    map.remove(&session_key(key, true));
}

// ---- origin-key normalization ---------------------------------------------
// The second half of WebPermissionManager::keyFor: given the origin as
// QUrl::toEncoded() text (already normalized — lowercase scheme/host,
// default ports dropped, IDN punycoded), produce
// percent-encode(scheme://host:port).  Stripping userinfo, path, query
// and fragment is what setUserInfo/setPath/setQuery/setFragment("") did
// in C++; for opaque schemes the whole post-colon part was the path,
// leaving a bare "scheme:".

/// Percent-encode per QByteArray::toPercentEncoding()'s default set:
/// every byte outside the URI unreserved group (ALPHA / DIGIT /
/// "-" / "." / "_" / "~") becomes %XX with uppercase hex.
fn percent_encode(s: &str) -> String {
    let mut out = String::with_capacity(s.len() * 3);
    const HEX: &[u8; 16] = b"0123456789ABCDEF";
    for &b in s.as_bytes() {
        let unreserved = b.is_ascii_alphanumeric()
            || b == b'-' || b == b'.' || b == b'_' || b == b'~';
        if unreserved {
            out.push(b as char);
        } else {
            out.push('%');
            out.push(HEX[(b >> 4) as usize] as char);
            out.push(HEX[(b & 0x0f) as usize] as char);
        }
    }
    out
}

/// scheme://authority (userinfo stripped) for hierarchical urls,
/// scheme: for opaque ones — still encoded-url text.
fn bare_origin(encoded: &str) -> String {
    let colon = match encoded.find(':') {
        Some(i) => i,
        None => return String::new(),
    };
    let mut out = String::with_capacity(colon + 3);
    out.push_str(&encoded[..colon]);
    out.push(':');
    let rest = &encoded[colon + 1..];
    if let Some(after_slashes) = rest.strip_prefix("//") {
        let authority_end = after_slashes
            .find(|c| c == '/' || c == '?' || c == '#')
            .unwrap_or(after_slashes.len());
        let authority = &after_slashes[..authority_end];
        let hostport = match authority.rfind('@') {
            Some(i) => &authority[i + 1..],
            None => authority,
        };
        out.push_str("//");
        out.push_str(hostport);
    }
    out
}

/// The full row key: percent-encoded bare origin + "/" + type name —
/// the shape both the legacy QSettings group and the imported
/// sitedecisions rows carry.
pub fn key_for(origin: &str, type_name: &str) -> String {
    format!("{}/{}", percent_encode(&bare_origin(origin)), type_name)
}

// ---- feature classification ------------------------------------------------

/// "denied" for the never-prompted surface, "promptable" for the
/// ask-capable set, "other" for anything unmapped — the old
/// `isDenied || !isPromptable` composite resolves deny on both the
/// first and third classes.
pub fn feature_class(type_name: &str) -> &'static str {
    if HARD_DENY.contains(&type_name) {
        "denied"
    } else if PROMPTABLE.contains(&type_name) {
        "promptable"
    } else {
        "other"
    }
}

// ---- the verdict pipeline ---------------------------------------------------

fn field_str<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(|x| x.as_str()).unwrap_or("")
}

fn field_bool(v: &Value, key: &str) -> bool {
    v.get(key).and_then(|x| x.as_bool()).unwrap_or(false)
}

fn check_inputs(m: &Value) -> RcResult<()> {
    let origin = field_str(m, "origin");
    let type_name = field_str(m, "type");
    if origin.is_empty() || origin.len() > MAX_ORIGIN_LEN {
        return fail(RcStatus::InvalidArgument, "bad origin");
    }
    if type_name.is_empty() || type_name.len() > MAX_TYPE_LEN {
        return fail(RcStatus::InvalidArgument, "bad permission type");
    }
    Ok(())
}

/// storedDecision verbatim: persistent row first (OTR pages share the
/// store read-only), then the OTR-namespaced session answer.
/// Returns (granted, source).
fn stored(key: &str, otr: bool) -> Option<(bool, &'static str)> {
    if let Ok(Some(value)) = sitedecisions::get(KIND_WEBPERM, key) {
        return Some((value == "grant", "stored"));
    }
    if let Some(granted) = session_get(key, otr) {
        return Some((granted, "session"));
    }
    None
}

/// The handleRequest pipeline: hard-deny/non-promptable feature gate,
/// remembered decision, else ask.  The prompt and the engine resolve
/// stay caller-side execution.
pub fn decide(m: &Value) -> Value {
    let type_name = field_str(m, "type");
    if feature_class(type_name) != "promptable" {
        return json!({"action": "deny", "source": "feature"});
    }
    let otr = field_bool(m, "otr");
    let key = key_for(field_str(m, "origin"), type_name);
    if let Some((granted, source)) = stored(&key, otr) {
        return json!({
            "action": if granted { "allow" } else { "deny" },
            "source": source,
        });
    }
    json!({"action": "ask"})
}

/// The prompt-answer record: session-remember always, persist only on
/// opt-in and never off-the-record.  Returns the result object for
/// call_json — {"recorded":true,"persisted":b,"mutated":b}.
fn resolve(m: &Value) -> RcResult<Value> {
    check_inputs(m)?;
    let key = key_for(field_str(m, "origin"), field_str(m, "type"));
    let granted = field_bool(m, "granted");
    let otr = field_bool(m, "otr");
    session_set(&key, otr, granted);
    // "persisted" reports the write ran — the C++ side signaled
    // changed() on every remember-write whether or not the row moved,
    // and the settings UI refresh rides that signal.
    let persisted = field_bool(m, "remember") && !otr && {
        sitedecisions::set(
            KIND_WEBPERM,
            &key,
            if granted { "grant" } else { "deny" },
        )?;
        true
    };
    Ok(json!({
        "recorded": true,
        "persisted": persisted,
        "mutated": persisted,
    }))
}

/// Generic granular endpoint — {"op":..., params} -> JSON result.
/// `decide` is the consult-time pipeline; `stored` is the mid-queue
/// recheck; `resolve`/`remove`/`clear` are the record paths;
/// `feature_class`/`key_for`/`session_clear` are the standalone
/// predicates and test seams.
pub fn call_json(v: &Value) -> RcResult<Value> {
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("");
    match op {
        "decide" => {
            check_inputs(v)?;
            Ok(decide(v))
        }
        "stored" => {
            check_inputs(v)?;
            let key = key_for(field_str(v, "origin"), field_str(v, "type"));
            match stored(&key, field_bool(v, "otr")) {
                Some((granted, source)) => Ok(json!({
                    "found": true, "granted": granted, "source": source,
                })),
                None => Ok(json!({"found": false})),
            }
        }
        "resolve" => resolve(v),
        "remove" => {
            check_inputs(v)?;
            let key = key_for(field_str(v, "origin"), field_str(v, "type"));
            let removed = sitedecisions::remove(KIND_WEBPERM, &key)?;
            session_remove_both(&key);
            Ok(json!({"removed": removed, "mutated": true}))
        }
        "clear" => {
            sitedecisions::clear_kind(KIND_WEBPERM)?;
            session().lock().unwrap().clear();
            Ok(json!({"cleared": true, "mutated": true}))
        }
        "feature_class" => {
            Ok(json!({"class": feature_class(field_str(v, "type"))}))
        }
        "key_for" => {
            check_inputs(v)?;
            Ok(json!({
                "key": key_for(field_str(v, "origin"), field_str(v, "type")),
            }))
        }
        "session_clear" => {
            session().lock().unwrap().clear();
            Ok(json!({"cleared": true}))
        }
        _ => fail(RcStatus::InvalidArgument, "unknown permpolicy op"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn with_store(f: impl FnOnce()) {
        let _guard = crate::store::test_lock();
        let dir = std::env::temp_dir().join(format!(
            "arktos-perm-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        crate::store::lock()
            .set_data_dir(dir.to_str().unwrap())
            .unwrap();
        let _ = sitedecisions::reset();
        session().lock().unwrap().clear();
        f();
        let _ = sitedecisions::reset();
        session().lock().unwrap().clear();
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn decide_at(origin: &str, typ: &str, otr: bool) -> Value {
        decide(&json!({"origin": origin, "type": typ, "otr": otr}))
    }

    #[test]
    fn bare_origin_strips() {
        // keyFor's setUserInfo/setPath/setQuery/setFragment("") half.
        assert_eq!(bare_origin("https://example.com/"), "https://example.com");
        assert_eq!(
            bare_origin("https://user:pw@example.com:8443/p?q=1#f"),
            "https://example.com:8443"
        );
        assert_eq!(bare_origin("about:blank"), "about:");
        assert_eq!(bare_origin("mailto:a@b.c"), "mailto:");
        assert_eq!(
            bare_origin("file:///home/user/doc.pdf"),
            "file://"
        );
    }

    #[test]
    fn key_encoding_matches_qt() {
        // QString::fromUtf8(QUrl("http://127.0.0.1:9").toEncoded()
        //     .toPercentEncoding()) + "/Notifications"
        assert_eq!(
            key_for("http://127.0.0.1:9", "Notifications"),
            "http%3A%2F%2F127.0.0.1%3A9/Notifications"
        );
        assert_eq!(
            key_for("https://example.com:8443", "Geolocation"),
            "https%3A%2F%2Fexample.com%3A8443/Geolocation"
        );
    }

    #[test]
    fn feature_tables() {
        for t in [
            "MediaAudioCapture", "MediaVideoCapture",
            "MediaAudioVideoCapture", "DesktopVideoCapture",
            "DesktopAudioVideoCapture", "ClipboardReadWrite",
            "LocalFontsAccess", "Unsupported",
        ] {
            assert_eq!(feature_class(t), "denied", "{t}");
            assert_eq!(decide_at("https://x.test/", t, false)["action"],
                       "deny", "{t}");
        }
        for t in ["Notifications", "Geolocation", "MouseLock"] {
            assert_eq!(feature_class(t), "promptable", "{t}");
        }
        // Unmapped enum values fail closed.
        assert_eq!(feature_class("FutureFeature"), "other");
        assert_eq!(decide_at("https://x.test/", "FutureFeature", false)
                   ["action"], "deny");
    }

    #[test]
    fn pipeline_stored_then_session_then_ask() {
        with_store(|| {
            let origin = "https://example.com/";
            assert_eq!(decide_at(origin, "Notifications", false)["action"],
                       "ask");
            sitedecisions::set(
                KIND_WEBPERM,
                &key_for(origin, "Notifications"),
                "grant",
            )
            .unwrap();
            let v = decide_at(origin, "Notifications", false);
            assert_eq!(v["action"], "allow");
            assert_eq!(v["source"], "stored");
            // A stored deny is honored the same way.
            sitedecisions::set(
                KIND_WEBPERM,
                &key_for(origin, "Notifications"),
                "deny",
            )
            .unwrap();
            assert_eq!(decide_at(origin, "Notifications", false)["action"],
                       "deny");
        });
    }

    #[test]
    fn resolve_records_session_and_optin_persist() {
        with_store(|| {
            let origin = "https://example.com/";
            // remember=false: session-only.
            let r = call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Notifications", "granted": true,
                "remember": false, "otr": false,
            }))
            .unwrap();
            assert_eq!(r["persisted"], false);
            assert!(sitedecisions::get(
                KIND_WEBPERM, &key_for(origin, "Notifications"))
                .unwrap().is_none());
            let v = decide_at(origin, "Notifications", false);
            assert_eq!(v["action"], "allow");
            assert_eq!(v["source"], "session");

            // remember=true persists.
            let r = call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Geolocation", "granted": false,
                "remember": true, "otr": false,
            }))
            .unwrap();
            assert_eq!(r["persisted"], true);
            assert_eq!(
                sitedecisions::get(
                    KIND_WEBPERM, &key_for(origin, "Geolocation"))
                    .unwrap(),
                Some("deny".to_string()));
        });
    }

    #[test]
    fn otr_namespaces_and_never_persists() {
        with_store(|| {
            let origin = "https://example.com/";
            // OTR grant, remember requested — still never persisted.
            let r = call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Notifications", "granted": true,
                "remember": true, "otr": true,
            }))
            .unwrap();
            assert_eq!(r["persisted"], false);
            // Same session answers the OTR page again...
            assert_eq!(decide_at(origin, "Notifications", true)["action"],
                       "allow");
            // ...but normal browsing sees nothing.
            assert_eq!(decide_at(origin, "Notifications", false)["action"],
                       "ask");
            // And the persistent store stayed empty.
            assert!(sitedecisions::get(
                KIND_WEBPERM, &key_for(origin, "Notifications"))
                .unwrap().is_none());

            // OTR pages still READ stored rows.
            sitedecisions::set(
                KIND_WEBPERM,
                &key_for(origin, "Geolocation"),
                "grant",
            )
            .unwrap();
            assert_eq!(decide_at(origin, "Geolocation", true)["action"],
                       "allow");
        });
    }

    #[test]
    fn remove_drops_store_and_session() {
        with_store(|| {
            let origin = "https://example.com/";
            call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Notifications", "granted": true,
                "remember": true, "otr": false,
            }))
            .unwrap();
            call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Notifications", "granted": true,
                "remember": true, "otr": true,
            }))
            .unwrap();
            call_json(&json!({
                "op": "remove", "origin": origin, "type": "Notifications",
            }))
            .unwrap();
            // Both the stored row and BOTH session variants are gone.
            assert_eq!(decide_at(origin, "Notifications", false)["action"],
                       "ask");
            assert_eq!(decide_at(origin, "Notifications", true)["action"],
                       "ask");
        });
    }

    #[test]
    fn clear_empties_everything() {
        with_store(|| {
            let origin = "https://example.com/";
            call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Notifications", "granted": true,
                "remember": true, "otr": false,
            }))
            .unwrap();
            call_json(&json!({"op": "clear"})).unwrap();
            assert_eq!(decide_at(origin, "Notifications", false)["action"],
                       "ask");
        });
    }

    #[test]
    fn stored_recheck() {
        with_store(|| {
            let origin = "https://example.com/";
            let r = call_json(&json!({
                "op": "stored", "origin": origin,
                "type": "Notifications", "otr": false,
            }))
            .unwrap();
            assert_eq!(r["found"], false);
            call_json(&json!({
                "op": "resolve", "origin": origin,
                "type": "Notifications", "granted": false,
                "remember": false, "otr": false,
            }))
            .unwrap();
            let r = call_json(&json!({
                "op": "stored", "origin": origin,
                "type": "Notifications", "otr": false,
            }))
            .unwrap();
            assert_eq!(r["found"], true);
            assert_eq!(r["granted"], false);
            assert_eq!(r["source"], "session");
        });
    }

    #[test]
    fn feature_denied_never_reaches_store() {
        with_store(|| {
            let origin = "https://example.com/";
            // Even a planted grant cannot unlock a hard-denied type —
            // the feature gate runs first, exactly like handleRequest.
            sitedecisions::set(
                KIND_WEBPERM,
                &key_for(origin, "ClipboardReadWrite"),
                "grant",
            )
            .unwrap();
            assert_eq!(decide_at(origin, "ClipboardReadWrite", false)
                       ["action"], "deny");
        });
    }
}
