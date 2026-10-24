//! MD05: privacy — the per-profile privacy-policy store view.
//!
//! The request interceptors' snapshot reads and the PRIV08 Sec-GPC
//! decision used to consult the "privacy" settings group directly.
//! Those rows are persona data (MPROF04 classifies privacy/* under
//! the profile scope), so they already live in the bound profile's
//! settings.json — this module is the typed read view over them
//! behind one JSON manifest in / verdict out contract, the same
//! shape dlwpolicy uses:
//!
//!   manifest in -> {"op":"flags"}                     -> all flags
//!                | {"op":"flag","name":<facade-name>} -> one flag
//!                | {"op":"gpc","tor_mode":b}          -> {"send":b}
//!                | {"op":"verdict","tor_mode":b}      -> unified read
//!                | {"op":"defaults"}                  -> pure, NULL ok
//!   verdict out  -> {"gpc":b,"strip":b,"adblock":b}
//!
//! Store keys (leaves under the bound scope's "privacy/" prefix —
//! the Qt writers keep owning them, this module only reads):
//!   httpsFirst, httpsOnly, refererPolicy (int; the PRIV01-era
//!   trimReferer bool is the migration fallback), securityLevel,
//!   blockPings, blockRemoteFonts, blockPrefetch,
//!   blockThirdPartyWebSockets, urlCleaning (the POL04 canonical
//!   toggle; SEC17's stripTrackingParams is the legacy alias),
//!   domainBlocklist, gpcSignal.  The unified verdict additionally
//!   folds "AdBlock/enabled" so the tracker-strip and adblock
//!   policy reads share this one helper — a second engine adapter
//!   answers "does this persona filter/strip/signal?" with a
//!   single call.
//!
//! Mirroring rules (kept 1:1 with privacyrequestinterceptor.cpp):
//!   * every missing or mistyped row resolves to the C++ default —
//!     a privacy toggle is fail-ON here (https-first, pings,
//!     prefetch, strip, blocklist, gpc all default true), never
//!     silently off;
//!   * the referer level is the stored int clamped to 0..3, else
//!     the trimReferer bool -> 1/0 migration;
//!   * the security level clamps to 0..2 (Standard..Safest);
//!   * tor_mode folds the pipeline's unconditional Sec-GPC into the
//!     verdict — per-user variance across tor windows is the
//!     fingerprint leak the uniform-constant doctrine exists to
//!     kill, so the bit is always set there (see PolicyVerdict::
//!     sendGpc in navigationpolicy.h).
//!
//! Read-only: there are no mutation ops and nothing is emitted —
//! the settings dialog and smoke harnesses write through
//! ProfileSettings, which the RC02 mirror already lands in this
//! store, and a facade reader must never fight their cached
//! document with a shadow write.

use serde_json::{json, Value};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::settings::RcSettings;

/// The settings-group prefix every leaf lives under.
const PRIVACY_PREFIX: &str = "privacy/";

/// The adblock group's enabled row — the third bit of the unified
/// verdict (separate group, same persona store).
const ADBLOCK_ENABLED_KEY: &str = "AdBlock/enabled";

fn key(leaf: &str) -> String {
    format!("{PRIVACY_PREFIX}{leaf}")
}

fn field_bool(v: &Value, key: &str) -> bool {
    v.get(key).and_then(|x| x.as_bool()).unwrap_or(false)
}

fn bool_flag(s: &RcSettings, leaf: &str, default: bool) -> bool {
    s.get_bool(&key(leaf)).unwrap_or(default)
}

/// privacy/urlCleaning ?? privacy/stripTrackingParams ?? true —
/// the canonical-then-legacy-alias read the C++ snapshot performs.
/// MD06: pub(crate) — urlclean's "nav" purpose consults the same
/// toggle so the two modules can never disagree.
pub(crate) fn strip_flag(s: &RcSettings) -> bool {
    match s.get_bool(&key("urlCleaning")) {
        Ok(v) => v,
        Err(_) => bool_flag(s, "stripTrackingParams", true),
    }
}

/// The REF01 level: the stored int clamped 0..3, else the
/// trimReferer-bool migration (false -> EngineDefault, anything
/// else -> the Trimmed default).
fn referer_policy(s: &RcSettings) -> i64 {
    match s.get_int(&key("refererPolicy")) {
        Ok(v) => v.clamp(0, 3),
        Err(_) => i64::from(bool_flag(s, "trimReferer", true)),
    }
}

fn security_level(s: &RcSettings) -> i64 {
    s.get_int(&key("securityLevel"))
        .unwrap_or(0)
        .clamp(0, 2)
}

fn gpc_flag(s: &RcSettings) -> bool {
    bool_flag(s, "gpcSignal", true)
}

fn adblock_flag(s: &RcSettings) -> bool {
    s.get_bool(ADBLOCK_ENABLED_KEY).unwrap_or(true)
}

/// The Sec-GPC send decision for one bound persona: the tor
/// pipeline emits unconditionally, clearnet follows the toggle.
/// This is the rc_privacy_gpc verdict's in-crate half.
pub fn gpc(s: &RcSettings, tor_mode: bool) -> bool {
    tor_mode || gpc_flag(s)
}

/// The typed flag set — keys match the snapshot JSON
/// rc_policy_load_snapshot consumes, plus the unified verdict's
/// adblock bit.
pub fn flags(s: &RcSettings) -> Value {
    json!({
        "https_first": bool_flag(s, "httpsFirst", true),
        "https_only": bool_flag(s, "httpsOnly", true),
        "referer_policy": referer_policy(s),
        "security_level": security_level(s),
        "block_pings": bool_flag(s, "blockPings", true),
        "block_remote_fonts": bool_flag(s, "blockRemoteFonts", false),
        "block_prefetch": bool_flag(s, "blockPrefetch", true),
        "block_third_party_ws":
            bool_flag(s, "blockThirdPartyWebSockets", false),
        "strip_tracking_params": strip_flag(s),
        "domain_blocklist": bool_flag(s, "domainBlocklist", true),
        "gpc_signal": gpc_flag(s),
        "adblock_enabled": adblock_flag(s),
    })
}

/// The default flag set — what "flags" answers for a pristine
/// persona.  Pure, so the NULL-store "defaults" op and tests share
/// it.
fn default_flags() -> Value {
    json!({
        "https_first": true,
        "https_only": true,
        "referer_policy": 1,
        "security_level": 0,
        "block_pings": true,
        "block_remote_fonts": false,
        "block_prefetch": true,
        "block_third_party_ws": false,
        "strip_tracking_params": true,
        "domain_blocklist": true,
        "gpc_signal": true,
        "adblock_enabled": true,
    })
}

fn need_store<'a>(h: Option<&'a RcSettings>) -> RcResult<&'a RcSettings> {
    h.ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "privacy op needs a bound profile settings store".into(),
    })
}

/// Generic granular endpoint — {"op":..., params} -> JSON result.
/// `h` is the bound profile's settings store; NULL is legal only
/// for the pure "defaults" op — store-bound ops fail with a
/// stashed error, never a guessed flag.  Read-only: no result ever
/// carries "mutated".
pub fn call_json(h: Option<&RcSettings>, v: &Value) -> RcResult<Value> {
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("");
    match op {
        "defaults" => Ok(default_flags()),
        "flags" => Ok(flags(need_store(h)?)),
        "flag" => {
            let s = need_store(h)?;
            let name = v.get("name").and_then(|x| x.as_str()).unwrap_or("");
            let f = flags(s);
            match f.get(name) {
                Some(value) => Ok(json!({"name": name, "value": value})),
                None => fail(RcStatus::InvalidArgument, "unknown privacy flag"),
            }
        }
        "gpc" => {
            let s = need_store(h)?;
            Ok(json!({"send": gpc(s, field_bool(v, "tor_mode"))}))
        }
        "verdict" => {
            let s = need_store(h)?;
            Ok(json!({
                "gpc": gpc(s, field_bool(v, "tor_mode")),
                "strip": strip_flag(s),
                "adblock": adblock_flag(s),
            }))
        }
        _ => fail(RcStatus::InvalidArgument, "unknown privacy op"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::settings::{RcSettings, Value as SValue};

    fn tmpdir(tag: &str) -> std::path::PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "arktos-privacy-test-{}-{}-{}",
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

    fn store(tag: &str) -> (std::path::PathBuf, RcSettings) {
        let dir = tmpdir(tag);
        let s = RcSettings::open(dir.to_str().unwrap(), Some(tag))
            .unwrap();
        (dir, s)
    }

    #[test]
    fn defaults_fail_on_not_off() {
        let (_d, s) = store("work");
        let f = flags(&s);
        // Every fail-on toggle resolves true on a pristine store.
        for name in [
            "https_first",
            "https_only",
            "block_pings",
            "block_prefetch",
            "strip_tracking_params",
            "domain_blocklist",
            "gpc_signal",
            "adblock_enabled",
        ] {
            assert_eq!(f[name], json!(true), "{name}");
        }
        assert_eq!(f["block_remote_fonts"], json!(false));
        assert_eq!(f["block_third_party_ws"], json!(false));
        assert_eq!(f["referer_policy"], json!(1));
        assert_eq!(f["security_level"], json!(0));
    }

    #[test]
    fn flags_read_the_bound_scope() {
        let (_d, s) = store("work");
        s.set("privacy/gpcSignal", SValue::Bool(false)).unwrap();
        s.set("privacy/urlCleaning", SValue::Bool(false)).unwrap();
        s.set("AdBlock/enabled", SValue::Bool(false)).unwrap();
        s.set("privacy/securityLevel", SValue::Int(9)).unwrap();
        let f = flags(&s);
        assert_eq!(f["gpc_signal"], json!(false));
        assert_eq!(f["strip_tracking_params"], json!(false));
        assert_eq!(f["adblock_enabled"], json!(false));
        // Out-of-range levels clamp like qBound.
        assert_eq!(f["security_level"], json!(2));

        // urlCleaning wins over the legacy alias; the alias still
        // answers when the canonical row is absent.
        s.set("privacy/stripTrackingParams", SValue::Bool(true))
            .unwrap();
        assert_eq!(flags(&s)["strip_tracking_params"], json!(false));
        s.remove("privacy/urlCleaning").unwrap();
        assert_eq!(flags(&s)["strip_tracking_params"], json!(true));
    }

    #[test]
    fn referer_migration_matches_qt() {
        let (_d, s) = store("work");
        // No refererPolicy row: the PRIV01 bool migrates — default
        // true -> Trimmed, stored false -> EngineDefault.
        assert_eq!(referer_policy(&s), 1);
        s.set("privacy/trimReferer", SValue::Bool(false)).unwrap();
        assert_eq!(referer_policy(&s), 0);
        // A stored int wins outright, clamped to the enum range.
        s.set("privacy/refererPolicy", SValue::Int(7)).unwrap();
        assert_eq!(referer_policy(&s), 3);
    }

    #[test]
    fn gpc_and_verdict_fold_tor() {
        let (_d, s) = store("work");
        assert!(gpc(&s, false));
        s.set("privacy/gpcSignal", SValue::Bool(false)).unwrap();
        assert!(!gpc(&s, false));
        // Tor ignores the per-user bit — uniform constant.
        assert!(gpc(&s, true));

        let v = call_json(Some(&s), &json!({"op": "verdict"})).unwrap();
        assert_eq!(v["gpc"], json!(false));
        assert_eq!(v["strip"], json!(true));
        assert_eq!(v["adblock"], json!(true));
        let vt = call_json(
            Some(&s),
            &json!({"op": "verdict", "tor_mode": true}),
        )
        .unwrap();
        assert_eq!(vt["gpc"], json!(true));
    }

    #[test]
    fn op_contract() {
        let (_d, s) = store("work");
        // defaults is pure — the only NULL-legal op.
        assert!(call_json(None, &json!({"op": "defaults"})).is_ok());
        assert!(call_json(None, &json!({"op": "flags"})).is_err());
        assert!(call_json(None, &json!({"op": "gpc"})).is_err());
        assert!(call_json(Some(&s), &json!({"op": "bogus"})).is_err());
        assert!(
            call_json(Some(&s), &json!({"op": "flag", "name": "bogus"}))
                .is_err()
        );
        let one = call_json(
            Some(&s),
            &json!({"op": "flag", "name": "gpc_signal"}),
        )
        .unwrap();
        assert_eq!(one["value"], json!(true));
    }
}
