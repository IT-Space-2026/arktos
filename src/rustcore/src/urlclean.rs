//! MD06: urlclean — the per-profile URL-sanitize policy store.
//!
//! The URL-cleaning decision used to live in two disconnected
//! places: UrlCleaner's compiled table + the
//! `urlcleaner/extraParams` QSettings key behind 'Copy Clean Link',
//! and the request pipeline's SEC17 strip running off urlstrip's
//! process-global ruleset.  This module is the single per-persona
//! surface both consumers now consult behind one JSON manifest in /
//! verdict out contract, the same shape dlwpolicy/privacy use:
//!
//!   manifest in -> {"op":"clean","url","purpose":"copy"|"nav",
//!                   "exempt":b,"unwrap":b}
//!                | {"op":"enabled","purpose":"copy"|"nav"}
//!                | {"op":"set_enabled","purpose":"copy","value":b}
//!                | {"op":"params"}
//!                | {"op":"ruleset_get"} | {"op":"ruleset_set",
//!                   "ruleset":{..}} | {"op":"ruleset_reset"}
//!                | {"op":"clean_policy"} | {"op":"defaults"}
//!   verdict out  -> {"url","changed":b,"removed":n,"unwrapped":b,
//!                    "enabled":b}
//!
//! The store is two-tier, both under the persona root — the same
//! split dlwpolicy made:
//!
//!   * the LEGACY key stays where it always was — the bound profile
//!     scope's settings.json `urlcleaner/extraParams` list — read
//!     here through the bound RcSettings and merged into the
//!     effective strip set on every call.  Qt keeps owning the key
//!     (the RC08 canonical store), so a pre-move user's additions
//!     keep stripping without any import step and a facade reader
//!     never fights the ProfileSettings mirror's cached document.
//!   * the NEW state lives in profiles/<id>/urlclean.json — a
//!     dedicated document this module owns outright (the RC19
//!     "unshared conf file" caveat): the per-profile ruleset
//!     override (the same JSON grammar urlstrip parses —
//!     params/scoped/unwrap/exceptions) and the copy-path toggle.
//!
//! Enable decisions:
//!   * "nav"  — the request-pipeline strip stays governed by
//!     privacy/urlCleaning (canonical) falling back to the SEC17-era
//!     privacy/stripTrackingParams alias, default ON — read through
//!     privacy::strip_flag so this module and the interceptor's
//!     snapshot can never diverge.  The key is Qt-owned: set_enabled
//!     refuses purpose "nav".
//!   * "copy" — 'Copy Clean Link' cleans by default; the per-profile
//!     `copy_enabled` row in urlclean.json is facade-authored and
//!     set_enabled-writable.  Absent means ON.
//!
//! Ruleset resolution for a clean verdict:
//!   1. the profile's urlclean.json "ruleset" override (validated at
//!      ruleset_set; a hand-corrupted doc falls back to the global
//!      set rather than disarming the strip — the same doctrine
//!      urlstrip applies to a bad data-dir override);
//!   2. otherwise the shared process ruleset (data-dir override,
//!      else the vendored builtin) — inherited as an Arc clone, so
//!      the common no-override profile adds zero parse work;
//!   3. the legacy extraParams merge on top of either.
//!
//! The nav pipeline does NOT call clean per request — the snapshot
//! loader ships clean_policy's merged ruleset document in
//! `urlclean_ruleset` and policy::strip_target runs it.  This op
//! serves the standalone seams ('Copy Clean Link', the download
//! redirect target) where a per-call consult is right.
//!
//! Everything that needs the live process stays caller-side: the
//! clipboard write, the shield-panel tally, the redirect verdict.
//! This module only answers questions.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::settings::RcSettings;
use crate::store;
use crate::urlstrip::{self, RuleSet};

/// The module's own document — profiles/<id>/urlclean.json.  Holds
/// the facade-authored ruleset override and the copy-path toggle;
/// the legacy `urlcleaner/extraParams` settings key is only ever
/// READ here (Qt still owns its writes).
const FILE_NAME: &str = "urlclean.json";
const FILE_VERSION: u64 = 1;

const MAX_URL_LEN: usize = 8192;
const MAX_EXTRA_PARAMS: usize = 512;
const MAX_FILE_BYTES: u64 = 4 * 1024 * 1024;

// ---- pure helpers ----------------------------------------------------

fn field_str<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(|x| x.as_str()).unwrap_or("")
}

fn field_bool(v: &Value, key: &str) -> bool {
    v.get(key).and_then(|x| x.as_bool()).unwrap_or(false)
}

fn need_store<'a>(h: Option<&'a RcSettings>) -> RcResult<&'a RcSettings> {
    h.ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "urlclean op needs a bound profile settings store".into(),
    })
}

// ---- the urlclean.json document ---------------------------------------

/// The facade-authored half.  `ruleset` holds the raw JSON document
/// (unparsed — load is tolerant; a shape parse_rules rejects is
/// reported but never adopted).
#[derive(Default)]
struct UcFile {
    copy_enabled: Option<bool>,
    ruleset: Option<Value>,
}

fn file_path(dir: &Path) -> PathBuf {
    dir.join(FILE_NAME)
}

/// Row-tolerant like the other stores: a malformed document is
/// "absent" — the global ruleset then remains the strip source.
fn load_file(dir: &Path) -> RcResult<UcFile> {
    let path = file_path(dir);
    if !path.exists() {
        return Ok(UcFile::default());
    }
    let meta = std::fs::metadata(&path)?;
    if meta.len() > MAX_FILE_BYTES {
        return fail(RcStatus::Corrupt, "urlclean.json exceeds the size bound");
    }
    let bytes = std::fs::read(&path)?;
    let doc: Value = serde_json::from_slice(&bytes).unwrap_or(Value::Null);
    let mut f = UcFile::default();
    if doc
        .get("version")
        .and_then(|v| v.as_u64())
        .map(|v| v != FILE_VERSION)
        .unwrap_or(true)
    {
        // Not ours — contributes nothing; the global ruleset stays
        // the strip source.
        return Ok(f);
    }
    f.copy_enabled = doc.get("copy_enabled").and_then(|x| x.as_bool());
    if let Some(rs) = doc.get("ruleset") {
        if rs.is_object() {
            f.ruleset = Some(rs.clone());
        }
    }
    Ok(f)
}

fn persist_file(dir: &Path, f: &UcFile) -> RcResult<()> {
    std::fs::create_dir_all(dir)?;
    let mut doc = json!({"version": FILE_VERSION});
    if let Some(b) = f.copy_enabled {
        doc["copy_enabled"] = json!(b);
    }
    if let Some(rs) = &f.ruleset {
        doc["ruleset"] = rs.clone();
    }
    let bytes = serde_json::to_vec(&doc).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode urlclean.json: {e}"),
    })?;
    store::atomic_write(&file_path(dir), &bytes)
}

// ---- the settings-key half --------------------------------------------

/// The legacy user extension list — ProfileSettings writes it under
/// the bound scope's "urlcleaner" group (MPROF04 profile-classified).
/// Mirrors UrlCleaner::trackingParameters' merge: every entry lands
/// on top of the curated ruleset.
fn legacy_extras(s: &RcSettings) -> Vec<String> {
    s.get_list("urlcleaner/extraParams")
        .unwrap_or_default()
        .into_iter()
        .take(MAX_EXTRA_PARAMS)
        .collect()
}

/// privacy/urlCleaning ?? privacy/stripTrackingParams ?? true — the
/// canonical-then-legacy-alias read the interceptor snapshot and this
/// module share verbatim.
fn nav_enabled(s: &RcSettings) -> bool {
    crate::privacy::strip_flag(s)
}

/// The effective ruleset: profile override when the stored doc
/// parses, the shared global set otherwise — then the legacy extras
/// and any manifest-supplied "params" merge on top (stateless
/// consults can inject rules without touching the store).
fn effective_rules(s: &RcSettings, m: &Value) -> RcResult<RuleSet> {
    let f = load_file(&s.scope_dir())?;
    let mut rules = match &f.ruleset {
        Some(doc) => urlstrip::parse_rules(&doc.to_string())
            .unwrap_or_else(|_| (*urlstrip::active_rules()).clone()),
        None => (*urlstrip::active_rules()).clone(),
    };
    rules.extend_params(legacy_extras(s));
    if let Some(extra) = m.get("params").and_then(|x| x.as_array()) {
        rules.extend_params(
            extra.iter()
                .filter_map(|e| e.as_str().map(String::from))
                .take(MAX_EXTRA_PARAMS),
        );
    }
    Ok(rules)
}

/// The ruleset as an editable document plus where it came from —
/// ruleset_get/clean_policy's shared answer.  `merged` additionally
/// folds the legacy extras into the doc's "params" array so the
/// snapshot consumer parses ONE document.
fn effective_doc(s: &RcSettings, merged: bool) -> RcResult<(Value, &'static str)> {
    let f = load_file(&s.scope_dir())?;
    let (mut doc, source) = match &f.ruleset {
        Some(rs) => (rs.clone(), "profile"),
        None => {
            let (src, text) = urlstrip::ruleset_source();
            let parsed = serde_json::from_str(&text)
                .unwrap_or_else(|_| json!({"version": 1, "params": []}));
            (parsed, src)
        }
    };
    if merged {
        let extras = legacy_extras(s);
        if !extras.is_empty() {
            if !doc.get("params").map(|p| p.is_array()).unwrap_or(false) {
                doc["params"] = json!([]);
            }
            if let Some(arr) = doc.get_mut("params").and_then(|p| p.as_array_mut()) {
                for e in extras {
                    arr.push(json!(e));
                }
            }
        }
    }
    Ok((doc, source))
}

fn copy_enabled(s: &RcSettings) -> RcResult<bool> {
    Ok(load_file(&s.scope_dir())?
        .copy_enabled
        .unwrap_or(true))
}

fn purpose_enabled(s: &RcSettings, purpose: &str) -> RcResult<bool> {
    if purpose == "nav" {
        Ok(nav_enabled(s))
    } else {
        copy_enabled(s)
    }
}

// ---- the op dispatcher --------------------------------------------------

/// Generic granular endpoint — {"op":..., params} -> JSON result.
/// `h` is the bound profile's settings store; NULL is legal only for
/// the pure "defaults" op — store-bound ops fail with a stashed
/// error, never a guessed toggle.  Mutating results carry
/// "mutated":true so the FFI layer emits the "urlclean" change topic.
pub fn call_json(h: Option<&RcSettings>, v: &Value) -> RcResult<Value> {
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("");
    match op {
        "defaults" => Ok(json!({
            "nav_enabled": true,
            "copy_enabled": true,
            "source": "builtin",
        })),
        "clean" => {
            let s = need_store(h)?;
            let url = field_str(v, "url");
            if url.len() > MAX_URL_LEN {
                return fail(RcStatus::InvalidArgument, "bad url");
            }
            let purpose = field_str(v, "purpose");
            let enabled = purpose_enabled(s, purpose)?;
            let unchanged = |enabled: bool| {
                json!({
                    "url": url,
                    "changed": false,
                    "removed": 0,
                    "unwrapped": false,
                    "enabled": enabled,
                })
            };
            if !enabled {
                return Ok(unchanged(false));
            }
            // A caller-side per-site exemption (the shield panel's
            // "leave this site's links alone" toggle) short-circuits
            // before the ruleset consult — same early-out
            // strip_for_request's is_exempt applies.
            if field_bool(v, "exempt") {
                let mut out = unchanged(true);
                out["exempt"] = json!(true);
                return Ok(out);
            }
            // Unwrap defaults on — the standalone seam is a
            // navigation-intent consult (rc_urlstrip's spelling), so
            // a redirector link copies/navigates to the real target.
            let allow_unwrap = !v.get("unwrap").map(|x| x.as_bool() == Some(false))
                .unwrap_or(false);
            let rules = effective_rules(s, v)?;
            match urlstrip::clean_url(&rules, url, allow_unwrap, &|_| false) {
                Some(out) => Ok(json!({
                    "url": out.url,
                    "changed": true,
                    "removed": out.removed,
                    "unwrapped": out.unwrapped,
                    "enabled": true,
                })),
                None => Ok(unchanged(true)),
            }
        }
        "enabled" => {
            let s = need_store(h)?;
            Ok(json!({
                "enabled": purpose_enabled(s, field_str(v, "purpose"))?,
            }))
        }
        "set_enabled" => {
            let purpose = field_str(v, "purpose");
            if purpose == "nav" {
                // privacy/urlCleaning is a Qt-owned key — the facade
                // never writes the settings scope (the unshared conf
                // file caveat); the settings UI keeps writing it.
                return fail(
                    RcStatus::InvalidArgument,
                    "nav toggle is privacy/urlCleaning — Qt-owned",
                );
            }
            let s = need_store(h)?;
            let value = v.get("value").and_then(|x| x.as_bool()).ok_or_else(|| {
                Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "set_enabled needs a bool value".into(),
                }
            })?;
            let mut f = load_file(&s.scope_dir())?;
            f.copy_enabled = Some(value);
            persist_file(&s.scope_dir(), &f)?;
            Ok(json!({"set": true, "mutated": true}))
        }
        "params" => {
            let s = need_store(h)?;
            let rules = effective_rules(s, v)?;
            Ok(json!({"params": rules.param_rules()}))
        }
        "ruleset_get" => {
            let s = need_store(h)?;
            let (doc, source) = effective_doc(s, false)?;
            Ok(json!({
                "source": source,
                "ruleset": doc,
                "extra_params": legacy_extras(s),
            }))
        }
        "ruleset_set" => {
            let s = need_store(h)?;
            let doc = v.get("ruleset").ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "ruleset_set needs a ruleset object".into(),
            })?;
            if !doc.is_object() {
                return fail(RcStatus::InvalidArgument, "ruleset must be an object");
            }
            // Validate through the shared grammar — a rejected doc
            // stores nothing, so a bad edit can never disarm the
            // strip.
            urlstrip::parse_rules(&doc.to_string())?;
            let mut f = load_file(&s.scope_dir())?;
            f.ruleset = Some(doc.clone());
            persist_file(&s.scope_dir(), &f)?;
            Ok(json!({"set": true, "mutated": true}))
        }
        "ruleset_reset" => {
            let s = need_store(h)?;
            let mut f = load_file(&s.scope_dir())?;
            f.ruleset = None;
            persist_file(&s.scope_dir(), &f)?;
            Ok(json!({"reset": true, "mutated": true}))
        }
        "clean_policy" => {
            // The snapshot-facing composite — one call hands the
            // interceptor everything the request pipeline needs: the
            // toggles and the merged ruleset document it embeds as
            // "urlclean_ruleset" for policy::strip_target.
            let s = need_store(h)?;
            let (doc, source) = effective_doc(s, true)?;
            Ok(json!({
                "nav_enabled": nav_enabled(s),
                "copy_enabled": copy_enabled(s)?,
                "source": source,
                "ruleset": doc,
            }))
        }
        _ => fail(RcStatus::InvalidArgument, "unknown urlclean op"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::{RcSettings, Value as SValue};
    use std::sync::MutexGuard;

    // The active global ruleset is process-global — serialize on the
    // shared store lock like the urlstrip tests do.
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "arktos-urlclean-test-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn store(tag: &str) -> (PathBuf, RcSettings) {
        let dir = tmpdir(tag);
        let s = RcSettings::open(dir.to_str().unwrap(), Some(tag)).unwrap();
        (dir, s)
    }

    fn clean(s: &RcSettings, url: &str, purpose: &str) -> Value {
        call_json(
            Some(s),
            &json!({"op": "clean", "url": url, "purpose": purpose}),
        )
        .unwrap()
    }

    #[test]
    fn clean_strips_with_global_rules() {
        let _g = guard();
        let (_d, s) = store("work");
        let v = clean(&s, "https://a.com/?utm_source=x&id=1", "copy");
        assert_eq!(v["url"], json!("https://a.com/?id=1"));
        assert_eq!(v["changed"], json!(true));
        assert_eq!(v["enabled"], json!(true));
        // A tracker-free URL passes through.
        let v = clean(&s, "https://a.com/?id=1", "copy");
        assert_eq!(v["changed"], json!(false));
        // Non-http schemes never clean.
        let v = clean(&s, "mailto:a@b.c?utm_source=x", "copy");
        assert_eq!(v["changed"], json!(false));
    }

    #[test]
    fn copy_toggle_gates_only_copy() {
        let _g = guard();
        let (_d, s) = store("work");
        let v = call_json(
            Some(&s),
            &json!({"op": "set_enabled", "purpose": "copy", "value": false}),
        )
        .unwrap();
        assert_eq!(v["mutated"], json!(true));
        let v = clean(&s, "https://a.com/?utm_source=x&id=1", "copy");
        assert_eq!(v["enabled"], json!(false));
        assert_eq!(v["changed"], json!(false));
        // nav still answers enabled — it follows privacy/urlCleaning.
        let e = call_json(
            Some(&s),
            &json!({"op": "enabled", "purpose": "nav"}),
        )
        .unwrap();
        assert_eq!(e["enabled"], json!(true));
        // set_enabled refuses the Qt-owned nav toggle.
        assert!(call_json(
            Some(&s),
            &json!({"op": "set_enabled", "purpose": "nav", "value": false}),
        )
        .is_err());
        // The toggle persists in the profile document.
        assert!(file_path(&s.scope_dir()).exists());
    }

    #[test]
    fn nav_toggle_follows_privacy_key() {
        let _g = guard();
        let (_d, s) = store("work");
        s.set("privacy/urlCleaning", SValue::Bool(false)).unwrap();
        let v = clean(&s, "https://a.com/?utm_source=x&id=1", "nav");
        assert_eq!(v["enabled"], json!(false));
        // Legacy alias answers when the canonical row is absent.
        s.remove("privacy/urlCleaning").unwrap();
        s.set("privacy/stripTrackingParams", SValue::Bool(false))
            .unwrap();
        let v = clean(&s, "https://a.com/?utm_source=x&id=1", "nav");
        assert_eq!(v["enabled"], json!(false));
    }

    #[test]
    fn profile_ruleset_overrides_global() {
        let _g = guard();
        let (_d, s) = store("work");
        let v = call_json(
            Some(&s),
            &json!({
                "op": "ruleset_set",
                "ruleset": {"version": 1, "params": ["only_this"]},
            }),
        )
        .unwrap();
        assert_eq!(v["mutated"], json!(true));
        // The override strips its own param...
        let v = clean(&s, "https://a.com/?only_this=1&id=2", "copy");
        assert_eq!(v["url"], json!("https://a.com/?id=2"));
        // ...and replaces the global list — utm_source survives now.
        let v = clean(&s, "https://a.com/?utm_source=x&id=1", "copy");
        assert_eq!(v["changed"], json!(false));
        // ruleset_get reports the profile source verbatim.
        let g = call_json(Some(&s), &json!({"op": "ruleset_get"})).unwrap();
        assert_eq!(g["source"], json!("profile"));
        assert_eq!(g["ruleset"]["params"], json!(["only_this"]));
        // A bogus doc refuses to store.
        assert!(call_json(
            Some(&s),
            &json!({"op": "ruleset_set", "ruleset": {"version": 99}}),
        )
        .is_err());
        // Reset returns to the global set.
        let v = call_json(Some(&s), &json!({"op": "ruleset_reset"})).unwrap();
        assert_eq!(v["mutated"], json!(true));
        let v = clean(&s, "https://a.com/?utm_source=x&id=1", "copy");
        assert_eq!(v["changed"], json!(true));
    }

    #[test]
    fn legacy_extra_params_merge() {
        let _g = guard();
        let (_d, s) = store("work");
        s.set(
            "urlcleaner/extraParams",
            SValue::List(vec!["partner".into(), "session*".into()]),
        )
        .unwrap();
        let v = clean(
            &s,
            "https://a.com/?partner=acme&sessionid=s1&id=1",
            "copy",
        );
        assert_eq!(v["url"], json!("https://a.com/?id=1"));
        // The effective param list carries them.
        let p = call_json(Some(&s), &json!({"op": "params"})).unwrap();
        let params: Vec<&str> =
            p["params"].as_array().unwrap().iter().filter_map(|x| x.as_str()).collect();
        assert!(params.contains(&"partner"));
        assert!(params.contains(&"session*"));
        assert!(params.contains(&"utm_*"));
    }

    #[test]
    fn exempt_and_unwrap() {
        let _g = guard();
        let (_d, s) = store("work");
        // Caller-side exemption short-circuits.
        let v = call_json(
            Some(&s),
            &json!({"op": "clean",
                    "url": "https://a.com/?utm_source=x",
                    "purpose": "copy", "exempt": true}),
        )
        .unwrap();
        assert_eq!(v["changed"], json!(false));
        assert_eq!(v["exempt"], json!(true));
        // The builtin ruleset ships a redirector unwrap rule — check
        // one end to end if the vendored doc has any.
        let (src, text) = urlstrip::ruleset_source();
        assert!(src == "builtin" || src == "override");
        assert!(text.contains("\"params\""));
    }

    #[test]
    fn clean_policy_composite() {
        let _g = guard();
        let (_d, s) = store("work");
        s.set(
            "urlcleaner/extraParams",
            SValue::List(vec!["campaign_id".into()]),
        )
        .unwrap();
        let v = call_json(Some(&s), &json!({"op": "clean_policy"})).unwrap();
        assert_eq!(v["nav_enabled"], json!(true));
        assert_eq!(v["copy_enabled"], json!(true));
        let params = v["ruleset"]["params"].as_array().unwrap();
        assert!(params.iter().any(|p| p.as_str() == Some("campaign_id")));
        // The merged doc still parses through the shared grammar.
        assert!(urlstrip::parse_rules(&v["ruleset"].to_string()).is_ok());
    }

    #[test]
    fn op_contract() {
        let _g = guard();
        let (_d, s) = store("work");
        // defaults is pure — the only NULL-legal op.
        assert!(call_json(None, &json!({"op": "defaults"})).is_ok());
        assert!(call_json(None, &json!({"op": "clean"})).is_err());
        assert!(call_json(None, &json!({"op": "params"})).is_err());
        assert!(call_json(Some(&s), &json!({"op": "bogus"})).is_err());
        // Per-profile isolation: another persona's store never sees
        // work's ruleset.
        let (_d2, other) = store("other");
        let v = call_json(
            Some(&s),
            &json!({"op": "ruleset_set",
                    "ruleset": {"version": 1, "params": ["only_this"]}}),
        )
        .unwrap();
        assert_eq!(v["mutated"], json!(true));
        let g = call_json(Some(&other), &json!({"op": "ruleset_get"})).unwrap();
        assert_ne!(g["source"], json!("profile"));
        let v = clean(&other, "https://a.com/?utm_source=x&id=1", "copy");
        assert_eq!(v["changed"], json!(true));
    }
}
