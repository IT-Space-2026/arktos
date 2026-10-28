//! RC02: the typed per-profile settings store — the first
//! profile-bound-handle module (the facade's `ac_settings(core, id)`
//! shape ahead of RC07).
//!
//! Replaces what ProfileSettings persists: a key-value map covering
//! the QSettings surface the tree uses — strings, bools, ints,
//! doubles, byte arrays and string lists — under one versioned JSON
//! document per scope, written atomically (temp + fsync + rename,
//! 0600 — the QSaveFile contract store::atomic_write provides).
//!
//! Two scopes, both under the caller-supplied data root:
//!
//!   profile  <root>/profiles/<id>/settings.json
//!   global   <root>/settings.json
//!
//! `rc_settings_open(root, profile_id)` binds the scope at open —
//! the rc_set_data_dir re-anchoring dance the older stores do is
//! unnecessary here, and alternating personas is cheap.
//!
//! Legacy import: the QSettings ini pair is a one-shot READ source.
//! On first open of a profile scope, profiles/<id>/settings.ini is
//! imported wholesale (the C++ migrateFromGlobal pass already folded
//! the classified global keys into it).  The global ini — which
//! lives outside the data root, at a path only Qt knows — is offered
//! through rc_settings_import_ini(): profile-scope handles take only
//! isProfileKey()-classified keys, global-scope handles take the
//! complement.  Both lanes fill absent keys only (the
//! migrateFromGlobal precedent: copy-not-move, existing wins), mark
//! their completion inside the JSON document, and never delete or
//! rewrite the ini files.  See inifmt.rs for the wire grammar.
//!
//! Typed getters convert like QVariant does: a scalar read back
//! from ini is a string, and "2" serves get_int fine — callers keep
//! their toInt()/toBool() habits.
//!
//! Every mutation writes through to disk (the sitedecisions
//! discipline: a crash can never resurrect a removed key or lose a
//! set) and the FFI layer emits the "settings" change topic.

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::inifmt;
use crate::store;
use crate::syncsettings;
use std::collections::BTreeMap;
use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;

const FILE_VERSION: u32 = 1;
const FILE_NAME: &str = "settings.json";
const LEGACY_INI_NAME: &str = "settings.ini";

const MAX_KEY_LEN: usize = 4096;
const MAX_STRING_LEN: usize = 4 * 1024 * 1024;
const MAX_BYTES_LEN: usize = 16 * 1024 * 1024;
const MAX_LIST_LEN: usize = 4096;
const MAX_ENTRIES: usize = 65536;
const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;

/// The typed value set — one wider than the RC02 brief (string/
/// bool/int/double/bytes) because QSettings round-trips
/// QStringList and losing it would strand real user keys.
#[derive(Clone, Debug, PartialEq)]
pub enum Value {
    Str(String),
    Bool(bool),
    Int(i64),
    Dbl(f64),
    Bytes(Vec<u8>),
    List(Vec<String>),
}

/// Which store an open handle owns.  `Profile(id)` reads
/// profiles/<id>/ under the data root; `Global` is the data root
/// itself — the scope app-global keys move to in a later wave
/// (RUSTCORE-ARCH §4.1).
#[derive(Clone, Debug, PartialEq)]
pub enum Scope {
    Global,
    Profile(String),
}

struct State {
    /// The canonical settings.json path.
    file: PathBuf,
    /// profiles/<id>/settings.ini for the profile scope — the
    /// automatic first-open import source.
    legacy_ini: Option<PathBuf>,
    scope: Scope,
    values: BTreeMap<String, Value>,
    /// One-shot import markers persisted inside the document —
    /// `profile` covers the automatic ini read, `extra` the
    /// explicit rc_settings_import_ini() lane (the global ini).
    imported_profile: bool,
    imported_extra: bool,
}

/// The FFI-facing handle.  Each handle owns its decoded map and a
/// mutex; two handles on the same file are last-writer-wins, the
/// same caveat QSettings documents for shared conf files.
pub struct RcSettings {
    state: Mutex<State>,
}

impl RcSettings {
    /// Open (creating on first write) the store for `profile_id`
    /// under `data_root` — NULL/empty id selects the global scope.
    /// A profile-scope open runs the one-shot settings.ini import
    /// when the legacy file is present.
    pub fn open(data_root: &str, profile_id: Option<&str>) -> RcResult<RcSettings> {
        if data_root.is_empty() {
            return fail(RcStatus::InvalidArgument, "empty data root");
        }
        let root = PathBuf::from(data_root);
        let (dir, scope) = match profile_id {
            Some(id) if !id.is_empty() => {
                check_profile_id(id)?;
                (
                    root.join("profiles").join(id),
                    Scope::Profile(id.to_string()),
                )
            }
            _ => (root.clone(), Scope::Global),
        };
        let legacy_ini = match &scope {
            Scope::Profile(_) => Some(dir.join(LEGACY_INI_NAME)),
            Scope::Global => None,
        };
        let mut state = State {
            file: dir.join(FILE_NAME),
            legacy_ini,
            scope,
            values: BTreeMap::new(),
            imported_profile: false,
            imported_extra: false,
        };
        load(&mut state)?;
        if !state.imported_profile {
            if let Some(ini) = state.legacy_ini.clone() {
                // One-shot on success: a missing ini costs one stat
                // per open, and a failed read retries next launch
                // rather than silently burning the migration.
                if ini.exists() && merge_ini(&mut state, &ini, false).is_ok() {
                    state.imported_profile = true;
                    persist(&state)?;
                }
            }
        }
        Ok(RcSettings {
            state: Mutex::new(state),
        })
    }

    /// Explicit legacy-ini import — the lane for the global
    /// QSettings file (outside the data root).  Profile scope keeps
    /// only isProfileKey()-classified keys, global scope the
    /// complement; absent keys only; one-shot per handle.
    pub fn import_ini(&self, path: &str) -> RcResult<()> {
        let mut state = self.lock();
        if state.imported_extra {
            return Ok(());
        }
        let ini = PathBuf::from(path);
        if ini.exists() {
            merge_ini(&mut state, &ini, true)?;
        }
        state.imported_extra = true;
        persist(&state)
    }

    pub fn contains(&self, key: &str) -> RcResult<bool> {
        let key = check_key(key)?;
        let state = self.lock();
        Ok(state.values.contains_key(&key))
    }

    /// The raw stored value — the JSON accessor the Qt bridge uses
    /// to rebuild a faithful QVariant.
    pub fn get(&self, key: &str) -> RcResult<Value> {
        let key = check_key(key)?;
        let state = self.lock();
        state.values.get(&key).cloned().ok_or_else(|| Fail {
            status: RcStatus::NotFound,
            msg: "no such settings key".into(),
        })
    }

    /// QVariant-style typed reads: convert through the canonical
    /// string form, never fail on a representable value.  Absent
    /// keys are RC_NOT_FOUND.
    pub fn get_string(&self, key: &str) -> RcResult<String> {
        Ok(canonical_string(&self.get(key)?))
    }

    pub fn get_bool(&self, key: &str) -> RcResult<bool> {
        let s = canonical_string(&self.get(key)?);
        Ok(parse_bool(&s))
    }

    pub fn get_int(&self, key: &str) -> RcResult<i64> {
        match self.get(key)? {
            Value::Int(v) => Ok(v),
            Value::Dbl(v) => Ok(v as i64),
            Value::Bool(v) => Ok(v as i64),
            other => Ok(canonical_string(&other)
                .trim()
                .parse::<i64>()
                .unwrap_or(0)),
        }
    }

    pub fn get_double(&self, key: &str) -> RcResult<f64> {
        match self.get(key)? {
            Value::Dbl(v) => Ok(v),
            Value::Int(v) => Ok(v as f64),
            Value::Bool(v) => Ok(v as i32 as f64),
            other => Ok(canonical_string(&other)
                .trim()
                .parse::<f64>()
                .unwrap_or(0.0)),
        }
    }

    pub fn get_bytes(&self, key: &str) -> RcResult<Vec<u8>> {
        match self.get(key)? {
            Value::Bytes(v) => Ok(v),
            other => Ok(canonical_string(&other).into_bytes()),
        }
    }

    pub fn get_list(&self, key: &str) -> RcResult<Vec<String>> {
        match self.get(key)? {
            Value::List(v) => Ok(v),
            other => Ok(vec![canonical_string(&other)]),
        }
    }

    pub fn set(&self, key: &str, value: Value) -> RcResult<()> {
        let key = check_key(key)?;
        check_value(&value)?;
        if self.set_apply(&key, &value)? {
            self.emit_after(&key, Some(&value));
        }
        Ok(())
    }

    /// SYNC07's apply lane — set() minus the op emission, so a
    /// merged remote op can never echo back onto the log.
    /// Returns whether the store actually moved (a replay applying
    /// the same value persists nothing).
    pub(crate) fn set_applied(&self, key: &str, value: Value) -> RcResult<bool> {
        let key = check_key(key)?;
        check_value(&value)?;
        self.set_apply(&key, &value)
    }

    /// The shared mutation half — insert + persist, reporting
    /// whether the map actually moved (no-op writes persist
    /// nothing and emit nothing).
    fn set_apply(&self, key: &str, value: &Value) -> RcResult<bool> {
        let mut state = self.lock();
        if state.values.len() >= MAX_ENTRIES
            && !state.values.contains_key(key)
        {
            return fail(
                RcStatus::InvalidArgument,
                "settings store entry bound reached",
            );
        }
        if state.values.get(key) == Some(value) {
            return Ok(false);
        }
        state.values.insert(key.to_string(), value.clone());
        persist(&state)?;
        Ok(true)
    }

    pub fn remove(&self, key: &str) -> RcResult<()> {
        let key = check_key(key)?;
        if self.remove_apply(&key)? {
            self.emit_after(&key, None);
        }
        Ok(())
    }

    /// SYNC07's apply lane — remove() minus the op emission.
    /// Returns whether the store actually moved.
    pub(crate) fn remove_applied(&self, key: &str) -> RcResult<bool> {
        let key = check_key(key)?;
        self.remove_apply(&key)
    }

    /// The shared mutation half — drop + persist, reporting
    /// whether the map actually moved.
    fn remove_apply(&self, key: &str) -> RcResult<bool> {
        let mut state = self.lock();
        if state.values.remove(key).is_none() {
            return Ok(false);
        }
        persist(&state)?;
        Ok(true)
    }

    /// SYNC07's post-mutation hook — sign the change onto the
    /// profile's sync op-log (best-effort; the write is already
    /// durable).  Runs after the store lock drops; the global
    /// scope and excluded keys never emit.
    fn emit_after(&self, key: &str, value: Option<&Value>) {
        let (file, local) = {
            let state = self.lock();
            let local = match state.values.get(syncsettings::EXCLUDE_KEY) {
                Some(Value::List(l)) => l.clone(),
                _ => Vec::new(),
            };
            (state.file.clone(), local)
        };
        syncsettings::emit_local(&file, key, &local, value);
    }

    /// Keys under `prefix` — a boundary match: `prefix` itself and
    /// `prefix/…` descendants; empty prefix lists everything.
    pub fn list(&self, prefix: Option<&str>) -> RcResult<Vec<String>> {
        let state = self.lock();
        let prefix = match prefix {
            Some(p) if !p.is_empty() => inifmt::normalize_key(p),
            _ => return Ok(state.values.keys().cloned().collect()),
        };
        Ok(state
            .values
            .keys()
            .filter(|k| {
                k.as_str() == prefix.as_str()
                    || k.starts_with(&format!("{prefix}/"))
            })
            .cloned()
            .collect())
    }

    /// Flush — mutations already write through, so this exists for
    /// the QSettings sync() call shape.
    pub fn sync(&self) -> RcResult<()> {
        persist(&self.lock())
    }

    /// The profile id this handle is bound to — `None` for the
    /// global scope.  The facade (RC07) stamps it on "settings"
    /// change topics so scoped subscribers can tell which persona's
    /// store moved.
    pub fn profile_id(&self) -> Option<String> {
        match &self.lock().scope {
            Scope::Global => None,
            Scope::Profile(id) => Some(id.clone()),
        }
    }

    /// The directory this scope's settings.json lives in —
    /// <root>/profiles/<id>/ for a profile scope, <root> for the
    /// global scope — so typed specializations (RC19 containers,
    /// MD04 dlwpolicy) can anchor sibling stores beside it.
    pub fn scope_dir(&self) -> PathBuf {
        self.lock()
            .file
            .parent()
            .map(|p| p.to_path_buf())
            .unwrap_or_default()
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

fn check_profile_id(id: &str) -> RcResult<()> {
    if id.len() > 64
        || id.is_empty()
        || !id
            .bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
    {
        return fail(RcStatus::InvalidArgument, "invalid profile id");
    }
    Ok(())
}

pub(crate) fn check_key(key: &str) -> RcResult<String> {
    let key = inifmt::normalize_key(key);
    if key.is_empty() || key.len() > MAX_KEY_LEN {
        return fail(RcStatus::InvalidArgument, "invalid settings key");
    }
    if key.chars().any(|c| c.is_control() && c != '\t') {
        return fail(RcStatus::InvalidArgument, "invalid settings key");
    }
    Ok(key)
}

fn check_value(v: &Value) -> RcResult<()> {
    match v {
        Value::Str(s) if s.len() > MAX_STRING_LEN => {
            fail(RcStatus::InvalidArgument, "settings string too large")
        }
        Value::Bytes(b) if b.len() > MAX_BYTES_LEN => {
            fail(RcStatus::InvalidArgument, "settings bytes too large")
        }
        Value::List(l)
            if l.len() > MAX_LIST_LEN
                || l.iter().any(|s| s.len() > MAX_STRING_LEN) =>
        {
            fail(RcStatus::InvalidArgument, "settings list too large")
        }
        Value::Dbl(d) if !d.is_finite() => fail(
            RcStatus::InvalidArgument,
            "non-finite double is not representable",
        ),
        _ => Ok(()),
    }
}

/// The QVariant toString() equivalent — how a value reads back as
/// plain text.
fn canonical_string(v: &Value) -> String {
    match v {
        Value::Str(s) => s.clone(),
        Value::Bool(b) => if *b { "true" } else { "false" }.to_string(),
        Value::Int(i) => i.to_string(),
        Value::Dbl(d) => d.to_string(),
        Value::Bytes(b) => String::from_utf8_lossy(b).into_owned(),
        Value::List(l) => l.join(", "),
    }
}

/// QString -> bool, the QVariant conversion: "true" (any case) or a
/// nonzero number.
fn parse_bool(s: &str) -> bool {
    s.eq_ignore_ascii_case("true")
        || s.trim().parse::<f64>().map(|v| v != 0.0).unwrap_or(false)
}

// ---- persistence ---------------------------------------------------

fn hex_encode(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 2);
    for b in bytes {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

fn hex_decode(s: &str) -> Option<Vec<u8>> {
    if s.len() % 2 != 0 {
        return None;
    }
    (0..s.len() / 2)
        .map(|i| u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).ok())
        .collect()
}

pub(crate) fn value_to_json(v: &Value) -> serde_json::Value {
    match v {
        Value::Str(s) => serde_json::json!({"t": "s", "v": s}),
        Value::Bool(b) => serde_json::json!({"t": "b", "v": b}),
        Value::Int(i) => serde_json::json!({"t": "i", "v": i}),
        Value::Dbl(d) => serde_json::json!({"t": "d", "v": d}),
        Value::Bytes(b) => serde_json::json!({"t": "x", "v": hex_encode(b)}),
        Value::List(l) => serde_json::json!({"t": "l", "v": l}),
    }
}

pub(crate) fn value_from_json(v: &serde_json::Value) -> Option<Value> {
    let t = v.get("t")?.as_str()?;
    let val = v.get("v")?;
    Some(match t {
        "s" => Value::Str(val.as_str()?.to_string()),
        "b" => Value::Bool(val.as_bool()?),
        "i" => Value::Int(val.as_i64()?),
        "d" => Value::Dbl(val.as_f64()?),
        "x" => Value::Bytes(hex_decode(val.as_str()?)?),
        "l" => Value::List(
            val.as_array()?
                .iter()
                .map(|e| e.as_str().map(String::from))
                .collect::<Option<Vec<_>>>()?,
        ),
        _ => return None,
    })
}

fn persist(state: &State) -> RcResult<()> {
    if let Some(parent) = state.file.parent() {
        fs::create_dir_all(parent)?;
    }
    let scope = match &state.scope {
        Scope::Global => "global".to_string(),
        Scope::Profile(id) => format!("profile:{id}"),
    };
    let values: serde_json::Map<String, serde_json::Value> = state
        .values
        .iter()
        .map(|(k, v)| (k.clone(), value_to_json(v)))
        .collect();
    let doc = serde_json::json!({
        "version": FILE_VERSION,
        "scope": scope,
        "imported": {
            "profile": state.imported_profile,
            "extra": state.imported_extra,
        },
        "values": values,
    });
    let bytes = serde_json::to_vec(&doc).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode settings: {e}"),
    })?;
    store::atomic_write(&state.file, &bytes)
}

/// Load the JSON document.  Row-tolerant like the other stores: a
/// malformed value drops only itself; an unusable document is an
/// empty store (settings are preferences — degrading to defaults
/// beats wedging the browser).
fn load(state: &mut State) -> RcResult<()> {
    if !state.file.exists() {
        return Ok(());
    }
    let meta = fs::metadata(&state.file)?;
    if meta.len() > MAX_FILE_BYTES {
        return fail(RcStatus::Corrupt, "settings.json exceeds the size bound");
    }
    let bytes = fs::read(&state.file)?;
    let doc: serde_json::Value =
        serde_json::from_slice(&bytes).unwrap_or(serde_json::Value::Null);
    if doc
        .get("version")
        .and_then(|v| v.as_u64())
        .map(|v| v != FILE_VERSION as u64)
        .unwrap_or(true)
    {
        // Unknown/absent version — not ours; start empty rather than
        // guess at a future schema.
        return Ok(());
    }
    if let Some(imported) = doc.get("imported") {
        state.imported_profile = imported
            .get("profile")
            .and_then(|v| v.as_bool())
            .unwrap_or(false);
        state.imported_extra = imported
            .get("extra")
            .and_then(|v| v.as_bool())
            .unwrap_or(false);
    }
    if let Some(values) = doc.get("values").and_then(|v| v.as_object()) {
        for (key, raw) in values {
            if key.len() > MAX_KEY_LEN {
                continue;
            }
            if let Some(v) = value_from_json(raw) {
                if check_value(&v).is_ok() && state.values.len() < MAX_ENTRIES {
                    state.values.insert(key.clone(), v);
                }
            }
        }
    }
    Ok(())
}

/// Merge one parsed ini file into the store.  `filter_profile`
/// selects the lane: profile scope keeps isProfileKey() keys,
/// global keeps the complement, and the automatic first-open
/// import (filter off) takes the whole profile ini.  Existing keys
/// are never overwritten — copy-not-move, store wins.
fn merge_ini(state: &mut State, path: &PathBuf, filter: bool) -> RcResult<()> {
    let bytes = fs::read(path)?;
    let rows = inifmt::parse(&bytes);
    let profile_scope = matches!(state.scope, Scope::Profile(_));
    for (key, ini) in rows {
        if filter {
            if profile_scope != inifmt::is_profile_key(&key) {
                continue;
            }
        }
        if state.values.contains_key(&key) || state.values.len() >= MAX_ENTRIES {
            continue;
        }
        let value = match ini {
            inifmt::IniValue::Str(s) | inifmt::IniValue::Raw(s) => Value::Str(s),
            inifmt::IniValue::Bytes(b) => Value::Bytes(b),
            inifmt::IniValue::List(l) => Value::List(l),
        };
        state.values.insert(key, value);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-settings-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    #[test]
    fn typed_roundtrip() {
        let root = tmpdir("rt");
        let h = RcSettings::open(root.to_str().unwrap(), Some("work")).unwrap();
        h.set("privacy/securityLevel", Value::Int(2)).unwrap();
        h.set("privacy/clearOnExit", Value::Bool(true)).unwrap();
        h.set("MainWindow/zoom", Value::Dbl(1.25)).unwrap();
        h.set("MainWindow/home", Value::Str("https://x/".into()))
            .unwrap();
        h.set("MainWindow/geometry", Value::Bytes(vec![1, 2, 255]))
            .unwrap();
        h.set(
            "searchShortcuts/engines",
            Value::List(vec!["ddg".into(), "g".into()]),
        )
        .unwrap();

        assert_eq!(h.get_int("privacy/securityLevel").unwrap(), 2);
        assert!(h.get_bool("privacy/clearOnExit").unwrap());
        assert_eq!(h.get_double("MainWindow/zoom").unwrap(), 1.25);
        assert_eq!(h.get_string("MainWindow/home").unwrap(), "https://x/");
        assert_eq!(
            h.get_bytes("MainWindow/geometry").unwrap(),
            vec![1, 2, 255]
        );
        assert_eq!(
            h.get_list("searchShortcuts/engines").unwrap(),
            vec!["ddg".to_string(), "g".to_string()]
        );

        // Reopen — the document round-trips through disk.
        drop(h);
        let h = RcSettings::open(root.to_str().unwrap(), Some("work")).unwrap();
        assert_eq!(h.get_int("privacy/securityLevel").unwrap(), 2);
        assert!(h.contains("MainWindow/zoom").unwrap());
        h.remove("MainWindow/zoom").unwrap();
        assert!(!h.contains("MainWindow/zoom").unwrap());
        assert_eq!(
            h.get("nope").unwrap_err().status,
            RcStatus::NotFound
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn qt_style_conversions() {
        let root = tmpdir("conv");
        let h = RcSettings::open(root.to_str().unwrap(), None).unwrap();
        // Ini-imported scalars arrive as strings; typed reads convert.
        h.set("num/int", Value::Str("42".into())).unwrap();
        h.set("bools/t", Value::Str("true".into())).unwrap();
        h.set("num/d", Value::Str("1.25".into())).unwrap();
        assert_eq!(h.get_int("num/int").unwrap(), 42);
        assert!(h.get_bool("bools/t").unwrap());
        assert_eq!(h.get_double("num/d").unwrap(), 1.25);
        assert_eq!(h.get_string("num/int").unwrap(), "42");
        // Typed writes read back through the string lane too.
        h.set("num/b", Value::Int(7)).unwrap();
        assert_eq!(h.get_string("num/b").unwrap(), "7");
        assert_eq!(h.get_double("num/b").unwrap(), 7.0);
        h.set("l/one", Value::Str("solo".into())).unwrap();
        assert_eq!(h.get_list("l/one").unwrap(), vec!["solo".to_string()]);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn list_prefix_boundary() {
        let root = tmpdir("lp");
        let h = RcSettings::open(root.to_str().unwrap(), Some("p")).unwrap();
        h.set("privacy/a", Value::Int(1)).unwrap();
        h.set("privacy/deep/b", Value::Int(2)).unwrap();
        h.set("privacynot/c", Value::Int(3)).unwrap();
        let keys = h.list(Some("privacy")).unwrap();
        assert_eq!(keys, vec!["privacy/a", "privacy/deep/b"]);
        assert_eq!(h.list(None).unwrap().len(), 3);
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn fixture_ini_import() {
        // testdata/profile-settings.ini was written by real Qt
        // QSettings::IniFormat (the exact bytes ProfileSettings
        // produces) — the RC02 verify fixture.
        let fixture = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("testdata/profile-settings.ini");
        let root = tmpdir("imp");
        let pdir = root.join("profiles/fixture");
        fs::create_dir_all(&pdir).unwrap();
        fs::copy(&fixture, pdir.join("settings.ini")).unwrap();

        let h = RcSettings::open(root.to_str().unwrap(), Some("fixture"))
            .unwrap();
        // Every fixture row arrives through the typed API.
        assert_eq!(h.get_string("userAgent").unwrap(), "ArktosFixture/1.0");
        assert_eq!(h.get_int("privacy/securityLevel").unwrap(), 2);
        assert!(h.get_bool("privacy/clearOnExit").unwrap());
        assert!(!h.get_bool("privacy/blockPings").unwrap());
        assert_eq!(
            h.get_string("MainWindow/home").unwrap(),
            "https://example.test/start"
        );
        assert_eq!(h.get_double("MainWindow/zoom").unwrap(), 1.25);
        assert_eq!(h.get_int("cookies/acceptPolicy").unwrap(), 1);
        assert_eq!(h.get_int("history/historyLimit").unwrap(), 30);
        assert_eq!(
            h.get_string("downloadmanager/downloadDirectory").unwrap(),
            "/home/täst/dowñloads"
        );
        assert_eq!(
            h.get_list("searchShortcuts/engines").unwrap(),
            vec!["ddg".to_string(), "wikipedia".to_string(), "g".to_string()]
        );
        // A single-element QStringList reads back as a string — the
        // same lossy edge QSettings itself has.
        assert_eq!(
            h.get_string("searchShortcuts/single").unwrap(),
            "only-one"
        );
        assert_eq!(
            h.get_list("searchShortcuts/single").unwrap(),
            vec!["only-one".to_string()]
        );
        assert_eq!(
            h.get_string("reader/fontFamily").unwrap(),
            "  spaced value  "
        );
        assert_eq!(
            h.get_string("reader/customCss").unwrap(),
            "body { content: \"x,y\"; }"
        );
        // Escaped-key + nested-group shapes.
        assert_eq!(
            h.get_string("containers/a1b2/name").unwrap(),
            "Work"
        );
        assert_eq!(
            h.get_string("webpermissions/https:/ex%20ample.com/geo")
                .unwrap(),
            "grant"
        );
        // The array subtree is plain keys.
        assert_eq!(
            h.get_string("sessions/pinned/1/url").unwrap(),
            "https://a.test/"
        );
        assert_eq!(h.get_int("sessions/pinned/size").unwrap(), 2);
        // The byte array decoded through the \x escapes.
        assert_eq!(
            h.get_bytes("MainWindow/geometry").unwrap(),
            vec![0x01, 0xd9, 0xd0, 0xcb, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00]
        );
        // The C++ migration marker imported with the rest.
        assert!(h.get_bool("meta/settingsMigrated").unwrap());

        // Import is one-shot: edits win over the ini, and a second
        // open never re-imports.
        h.set("privacy/securityLevel", Value::Int(0)).unwrap();
        fs::copy(&fixture, pdir.join("settings.ini")).unwrap();
        drop(h);
        let h = RcSettings::open(root.to_str().unwrap(), Some("fixture"))
            .unwrap();
        assert_eq!(h.get_int("privacy/securityLevel").unwrap(), 0);
        // The ini file itself was never touched.
        assert_eq!(
            fs::read(pdir.join("settings.ini")).unwrap(),
            fs::read(&fixture).unwrap()
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn explicit_import_filters_by_scope() {
        let root = tmpdir("flt");
        // A "global" ini holding both classified and global keys.
        let ini = root.join("global.conf");
        fs::write(
            &ini,
            "[privacy]\nsecurityLevel=9\n\n[MainWindow]\n\
             defaultEngine=servo\nhome=https://home/\n\n\
             [General]\nuserAgent=UA/1\n",
        )
        .unwrap();

        let h = RcSettings::open(root.to_str().unwrap(), Some("f")).unwrap();
        h.import_ini(ini.to_str().unwrap()).unwrap();
        // Classified keys came in, app-global keys stayed out.
        assert_eq!(h.get_int("privacy/securityLevel").unwrap(), 9);
        assert_eq!(h.get_string("userAgent").unwrap(), "UA/1");
        assert_eq!(h.get_string("MainWindow/home").unwrap(), "https://home/");
        assert!(!h.contains("MainWindow/defaultEngine").unwrap());

        // Global scope takes the complement.
        let g = RcSettings::open(root.to_str().unwrap(), None).unwrap();
        g.import_ini(ini.to_str().unwrap()).unwrap();
        assert_eq!(
            g.get_string("MainWindow/defaultEngine").unwrap(),
            "servo"
        );
        assert!(!g.contains("privacy/securityLevel").unwrap());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn validation_and_scope_paths() {
        let root = tmpdir("val");
        assert!(RcSettings::open(root.to_str().unwrap(), Some("../x")).is_err());
        assert!(RcSettings::open(root.to_str().unwrap(), Some("Bad Id")).is_err());
        assert!(RcSettings::open("", Some("x")).is_err());

        let h = RcSettings::open(root.to_str().unwrap(), Some("ok")).unwrap();
        h.set("k", Value::Int(1)).unwrap();
        assert!(root.join("profiles/ok/settings.json").exists());
        assert!(h.set("k", Value::Dbl(f64::NAN)).is_err());
        assert!(h.set("", Value::Int(1)).is_err());
        // Empty-ish key forms normalize like Qt.
        h.set("//a//b/", Value::Int(2)).unwrap();
        assert!(h.contains("a/b").unwrap());
        let _ = fs::remove_dir_all(&root);
    }
}
