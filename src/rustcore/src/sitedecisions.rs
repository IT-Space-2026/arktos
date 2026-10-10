//! SITED01: the consolidated per-site decision store.
//!
//! One durable, engine-neutral home for every "what did the user decide
//! for this site?" record the shell keeps — web-permission grants,
//! JavaScript allow/block rules, container assignments, pop-up
//! exceptions, HTTPS-only allowances, cookie rules and adblock
//! whitelist entries.  Before this module each of those lived in its
//! own QSettings group (or, for the adblock whitelist, in the custom
//! filter file); every Qt adapter re-implemented the same
//! load/list/insert/flush dance, and nothing existed outside the
//! QSettings world for a future non-Qt engine to reuse.
//!
//! Data model:
//!
//!   kind  -> ( host-or-origin key  ->  decision value )
//!
//! Kinds are short slugs owned by the Qt adapters
//! ("webperm", "js", "container", "popup", "http-allow",
//!  "cookie", "adblock").  Keys are already-normalized host strings or
//! opaque origin encodings — normalization is the adapter's business;
//! the store is deliberately kind-agnostic so a new decision class does
//! not need a schema change.  Values are small strings
//! ("grant"/"deny", "allow"/"block", a container uuid, "session", …).
//!
//! The whole store is a single JSON document:
//!
//!   { "version": 1, "kinds": { "js": { "example.com": "block" } } }
//!
//! persisted atomically (write-temp + rename) so a crash mid-flush can
//! never leave a torn file.  Parsing is tolerant: a corrupt file, a
//! wrong shape, or a malformed kind/key/value drops only the offending
//! rows — valid neighbours survive.  An absent file is an empty store;
//! a wholly unusable file is treated as empty as well (the records are
//! preferences, not secrets — losing them degrades to re-prompting).
//!
//! Threading model matches the other rustcore stores: one Mutex around
//! the whole thing plus a monotonically increasing generation counter
//! that callers can poll to detect change.  All mutations persist
//! synchronously so a crash between "user clicked Allow" and "settings
//! dialog flushed" cannot resurrect a revoked decision.
//!
//! The Qt side consumes this through sitedecisionstore.{h,cpp}, which
//! keeps the legacy QSettings paths alive for CONFIG+=no-rust builds.

use crate::error::{fail, RcResult, RcStatus};
use crate::store;
use std::collections::BTreeMap;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{LazyLock, Mutex};

const MAX_KIND_LEN: usize = 64;
const MAX_KEY_LEN: usize = 1024;
const MAX_VALUE_LEN: usize = 2048;
const MAX_ENTRIES_PER_KIND: usize = 8192;
const MAX_KINDS: usize = 64;
const MAX_FILE_BYTES: u64 = 4 * 1024 * 1024;

const FILE_VERSION: u32 = 1;

type Kinds = BTreeMap<String, BTreeMap<String, String>>;

struct SiteDecisions {
    kinds: Kinds,
    /// The data dir the in-memory rows were loaded from.  A
    /// `rc_set_data_dir` switch (or a first call before it) changes
    /// this — the next access re-reads the new dir's file instead of
    /// serving, or clobbering, rows belonging to the old one.
    loaded_dir: Option<std::path::PathBuf>,
}

fn store() -> &'static LazyLock<Mutex<SiteDecisions>> {
    static STORE: LazyLock<Mutex<SiteDecisions>> = LazyLock::new(|| {
        Mutex::new(SiteDecisions {
            kinds: Kinds::new(),
            loaded_dir: None,
        })
    });
    &STORE
}

static GENERATION: AtomicU64 = AtomicU64::new(0);

fn file_path() -> RcResult<std::path::PathBuf> {
    Ok(store::lock().dir()?.join("sitedecisions.json"))
}

fn check_kind(kind: &str) -> RcResult<()> {
    if kind.is_empty()
        || kind.len() > MAX_KIND_LEN
        || !kind
            .chars()
            .all(|c| c.is_ascii_lowercase() || c.is_ascii_digit() || c == '-')
    {
        return fail(
            RcStatus::InvalidArgument,
            format!("invalid site-decision kind {kind:?}"),
        );
    }
    Ok(())
}

fn check_key(key: &str) -> RcResult<()> {
    if key.is_empty() || key.len() > MAX_KEY_LEN {
        return fail(RcStatus::InvalidArgument, "invalid site-decision key");
    }
    Ok(())
}

fn check_value(value: &str) -> RcResult<()> {
    if value.is_empty() || value.len() > MAX_VALUE_LEN {
        return fail(RcStatus::InvalidArgument, "invalid site-decision value");
    }
    Ok(())
}

fn generation() -> u64 {
    GENERATION.load(Ordering::Acquire)
}

fn bump() {
    GENERATION.fetch_add(1, Ordering::AcqRel);
}

/// Re-read the disk file into memory.  Parse failures degrade to an
/// empty store rather than an error — a corrupt preferences file must
/// not wedge every per-site policy check behind it.  (The FFI layer
/// still reports RC_CORRUPT on the *file*, but callers of this loader
/// get an empty store plus the generation bump.)
fn load_from_disk(state: &mut SiteDecisions) -> RcResult<()> {
    let path = file_path()?;
    if !path.exists() {
        state.kinds.clear();
        return Ok(());
    }
    let meta = std::fs::metadata(&path)?;
    if meta.len() > MAX_FILE_BYTES {
        state.kinds.clear();
        return fail(
            RcStatus::Corrupt,
            "sitedecisions.json exceeds the size bound",
        );
    }
    let bytes = std::fs::read(&path)?;
    // Row-tolerant parse: walk the document as a Value so a single
    // malformed kind or entry drops only itself, never a valid
    // neighbour (spec: corrupt-row tolerance).  A document that is not
    // an object at all yields an empty store — same as a missing file.
    let doc: serde_json::Value =
        serde_json::from_slice(&bytes).unwrap_or(serde_json::Value::Null);
    let mut kinds = Kinds::new();
    if let Some(kind_map) = doc.get("kinds").and_then(|k| k.as_object()) {
        for (kind, rows) in kind_map {
            if kind.is_empty() || kind.len() > MAX_KIND_LEN {
                continue;
            }
            let Some(rows) = rows.as_object() else {
                continue;
            };
            let mut entries = BTreeMap::new();
            for (key, value) in rows {
                let Some(value) = value.as_str() else {
                    continue;
                };
                if key.is_empty()
                    || key.len() > MAX_KEY_LEN
                    || value.is_empty()
                    || value.len() > MAX_VALUE_LEN
                {
                    continue;
                }
                if entries.len() >= MAX_ENTRIES_PER_KIND {
                    break;
                }
                entries.insert(key.clone(), value.to_string());
            }
            if !entries.is_empty() {
                kinds.insert(kind.clone(), entries);
            }
            if kinds.len() >= MAX_KINDS {
                break;
            }
        }
    }
    state.kinds = kinds;
    Ok(())
}

fn persist(state: &SiteDecisions) -> RcResult<()> {
    let path = file_path()?;
    // An emptied store removes its file outright — `store_present`
    // then doubles as a precise "any decisions on disk?" probe for
    // the legacy-migration gate.
    if state.kinds.is_empty() {
        if path.exists() {
            std::fs::remove_file(&path)?;
        }
        return Ok(());
    }
    let file = serde_json::json!({
        "version": FILE_VERSION,
        "kinds": state.kinds,
    });
    let bytes = serde_json::to_vec(&file).map_err(|e| crate::error::Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode sitedecisions: {e}"),
    })?;
    store::atomic_write(&path, &bytes)
}

/// True once the store file exists — the Qt migration code uses this
/// to decide whether first-run legacy import has already happened.
pub fn store_present() -> bool {
    file_path().map(|p| p.exists()).unwrap_or(false)
}

fn ensure_loaded(state: &mut SiteDecisions) {
    let dir = file_path()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()));
    if state.loaded_dir != dir {
        // First touch — or the data dir moved under us (test-mode
        // path switches rely on this).  A corrupt file degrades to
        // "empty" rather than an error.
        let _ = load_from_disk(state);
        state.loaded_dir = dir;
        bump();
    }
}

pub fn get(kind: &str, key: &str) -> RcResult<Option<String>> {
    check_kind(kind)?;
    check_key(key)?;
    let lock = store();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state);
    Ok(state
        .kinds
        .get(kind)
        .and_then(|entries| entries.get(key))
        .cloned())
}

fn mutate<F>(state: &mut SiteDecisions, f: F) -> RcResult<bool>
where
    F: FnOnce(&mut Kinds) -> bool,
{
    ensure_loaded(state);
    if !f(&mut state.kinds) {
        return Ok(false);
    }
    persist(state)?;
    bump();
    Ok(true)
}

pub fn set(kind: &str, key: &str, value: &str) -> RcResult<bool> {
    check_kind(kind)?;
    check_key(key)?;
    check_value(value)?;
    let lock = store();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |kinds| {
        let entries = kinds.entry(kind.to_string()).or_default();
        if entries.get(key).map(String::as_str) == Some(value) {
            return false;
        }
        if entries.len() >= MAX_ENTRIES_PER_KIND {
            return false;
        }
        entries.insert(key.to_string(), value.to_string());
        true
    })
}

pub fn remove(kind: &str, key: &str) -> RcResult<bool> {
    check_kind(kind)?;
    check_key(key)?;
    let lock = store();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |kinds| {
        let entries = match kinds.get_mut(kind) {
            Some(e) => e,
            None => return false,
        };
        if entries.remove(key).is_none() {
            return false;
        }
        if entries.is_empty() {
            kinds.remove(kind);
        }
        true
    })
}

pub fn clear_kind(kind: &str) -> RcResult<bool> {
    check_kind(kind)?;
    let lock = store();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |kinds| kinds.remove(kind).is_some())
}

/// Replace every row of a kind — the adapters' "the in-memory list is
/// authoritative, mirror it" write path.  `entries` is a JSON object.
pub fn replace_kind(kind: &str, entries_json: &[u8]) -> RcResult<bool> {
    check_kind(kind)?;
    let entries: BTreeMap<String, String> =
        serde_json::from_slice(entries_json).map_err(|e| {
            crate::error::Fail {
                status: RcStatus::InvalidArgument,
                msg: format!("sitedecisions replace payload: {e}"),
            }
        })?;
    let bad: Vec<String> = entries
        .iter()
        .filter(|(k, v)| {
            k.is_empty() || k.len() > MAX_KEY_LEN || v.is_empty()
                || v.len() > MAX_VALUE_LEN
        })
        .map(|(k, _)| k.clone())
        .collect();
    if !bad.is_empty() || entries.len() > MAX_ENTRIES_PER_KIND {
        return fail(
            RcStatus::InvalidArgument,
            "sitedecisions replace payload exceeds bounds",
        );
    }
    let lock = store();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |kinds| {
        let same = kinds
            .get(kind)
            .map(|cur| *cur == entries)
            .unwrap_or(entries.is_empty());
        if same {
            return false;
        }
        if entries.is_empty() {
            kinds.remove(kind);
        } else {
            kinds.insert(kind.to_string(), entries.clone());
        }
        true
    })
}

/// Every row of a kind as a JSON object `{"key":"value"}` —
/// stable-sorted via BTreeMap.
pub fn list_json(kind: &str) -> RcResult<String> {
    check_kind(kind)?;
    let lock = store();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state);
    let empty = BTreeMap::new();
    let entries = state.kinds.get(kind).unwrap_or(&empty);
    serde_json::to_string(entries).map_err(|e| crate::error::Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode list: {e}"),
    })
}

/// The IO-thread snapshot: every kind's rows in one JSON document
/// `{"kinds":{"kind":{"key":"value"}},"version":N,"generation":G}`.
/// Qt reads it once and walks it under its own read lock — matching
/// how the QSettings-backed snapshots were consumed before.
pub fn snapshot_json() -> RcResult<String> {
    let lock = store();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state);
    let doc = serde_json::json!({
        "version": FILE_VERSION,
        "generation": generation(),
        "kinds": state.kinds,
    });
    serde_json::to_string(&doc).map_err(|e| crate::error::Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode snapshot: {e}"),
    })
}

/// Suffix-walk lookup: `www.example.com` tries `www.example.com`,
/// `example.com`, `com` — the first row wins.  This is the host-list
/// semantics the popup/script/adblock stores already implement in Qt;
/// exposing it in Rust lets an IO-thread adapter resolve a whole-page
/// decision from one locked read.
///
/// Returns `Some((matched_key, value))`.
pub fn lookup(kind: &str, host: &str) -> RcResult<Option<(String, String)>> {
    check_kind(kind)?;
    if host.is_empty() || host.len() > MAX_KEY_LEN {
        return fail(RcStatus::InvalidArgument, "invalid host");
    }
    let lock = store();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state);
    let entries = match state.kinds.get(kind) {
        Some(e) => e,
        None => return Ok(None),
    };
    let normalized = host.to_ascii_lowercase();
    let mut suffix = normalized.as_str();
    loop {
        if let Some(value) = entries.get(suffix) {
            return Ok(Some((suffix.to_string(), value.clone())));
        }
        match suffix.find('.') {
            Some(dot) => suffix = &suffix[dot + 1..],
            None => return Ok(None),
        }
    }
}

/// Wipe every kind — the "clear site data" nuclear path and the
/// test-suite reset seam.
pub fn reset() -> RcResult<()> {
    let lock = store();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state);
    if state.kinds.is_empty() && !store_present() {
        return Ok(());
    }
    state.kinds.clear();
    persist(&state)?;
    bump();
    Ok(())
}

/// Force a re-read of the disk file — test/repair seam for adapters
/// that cache hydrated rows.
pub fn reload() -> RcResult<()> {
    let lock = store();
    let mut state = lock.lock().unwrap();
    load_from_disk(&mut state)?;
    state.loaded_dir = file_path()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()));
    bump();
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    // Tests share the global store + data dir — serialize them on the
    // crate-wide store lock so no neighbouring module's set_data_dir
    // can move the store mid-test.
    fn with_store(f: impl FnOnce()) {
        let _guard = crate::store::test_lock();
        let dir = std::env::temp_dir().join(format!(
            "arora-sitedec-test-{}-{}",
            std::process::id(),
            generation()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        store::lock().set_data_dir(dir.to_str().unwrap()).unwrap();
        // Force a fresh load from the (empty) dir.
        let _ = reset();
        f();
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn set_get_remove_roundtrip() {
        with_store(|| {
            assert!(!store_present());
            assert_eq!(get("js", "example.com").unwrap(), None);
            assert!(set("js", "example.com", "block").unwrap());
            assert!(store_present());
            // Re-setting the same value reports no change.
            assert!(!set("js", "example.com", "block").unwrap());
            assert_eq!(
                get("js", "example.com").unwrap(),
                Some("block".to_string())
            );
            assert!(set("js", "example.com", "allow").unwrap());
            assert_eq!(
                get("js", "example.com").unwrap(),
                Some("allow".to_string())
            );
            assert!(remove("js", "example.com").unwrap());
            assert!(!remove("js", "example.com").unwrap());
            assert_eq!(get("js", "example.com").unwrap(), None);
        });
    }

    #[test]
    fn validation_bounds() {
        with_store(|| {
            assert!(set("BAD KIND", "k", "v").is_err());
            assert!(set("js", "", "v").is_err());
            assert!(set("js", "k", "").is_err());
            let long = "a".repeat(MAX_KEY_LEN + 1);
            assert!(set("js", &long, "v").is_err());
            let longv = "a".repeat(MAX_VALUE_LEN + 1);
            assert!(set("js", "k", &longv).is_err());
        });
    }

    #[test]
    fn list_and_snapshot() {
        with_store(|| {
            set("js", "a.example", "allow").unwrap();
            set("js", "b.example", "block").unwrap();
            set("popup", "pop.example", "allow").unwrap();
            let js: serde_json::Value =
                serde_json::from_str(&list_json("js").unwrap()).unwrap();
            assert_eq!(js["a.example"], "allow");
            assert_eq!(js["b.example"], "block");
            let snap: serde_json::Value =
                serde_json::from_str(&snapshot_json().unwrap()).unwrap();
            assert_eq!(snap["kinds"]["js"]["b.example"], "block");
            assert_eq!(snap["kinds"]["popup"]["pop.example"], "allow");
            // The snapshot reflects a change without a restart.
            set("popup", "new.example", "allow").unwrap();
            let snap2: serde_json::Value =
                serde_json::from_str(&snapshot_json().unwrap()).unwrap();
            assert_eq!(snap2["kinds"]["popup"]["new.example"], "allow");
        });
    }

    #[test]
    fn lookup_walks_suffixes() {
        with_store(|| {
            set("popup", "example.com", "allow").unwrap();
            assert_eq!(
                lookup("popup", "www.example.com").unwrap().unwrap().0,
                "example.com"
            );
            assert_eq!(
                lookup("popup", "deep.www.example.com")
                    .unwrap()
                    .unwrap()
                    .0,
                "example.com"
            );
            assert!(lookup("popup", "notexample.com").unwrap().is_none());
            // Exact rule beats the parent.
            set("popup", "www.example.com", "deny").unwrap();
            assert_eq!(
                lookup("popup", "www.example.com")
                    .unwrap()
                    .unwrap()
                    .1,
                "deny"
            );
        });
    }

    #[test]
    fn replace_kind_mirrors_list() {
        with_store(|| {
            let payload = br#"{"a.example":"allow","b.example":"block"}"#;
            assert!(replace_kind("js", payload).unwrap());
            assert_eq!(
                get("js", "a.example").unwrap(),
                Some("allow".to_string())
            );
            // Same content -> no change reported.
            assert!(!replace_kind("js", payload).unwrap());
            let shrink = br#"{"b.example":"block"}"#;
            assert!(replace_kind("js", shrink).unwrap());
            assert!(get("js", "a.example").unwrap().is_none());
            // Empty object clears the kind.
            assert!(replace_kind("js", b"{}").unwrap());
            assert!(list_json("js").unwrap().contains("{}"));
            // Malformed payload is rejected without touching the store.
            assert!(replace_kind("js", b"not json").is_err());
            assert!(replace_kind("js", br#"{"":"x"}"#).is_err());
        });
    }

    #[test]
    fn data_dir_switch_invalidates_cache() {
        let _guard = crate::store::test_lock();
        let base = std::env::temp_dir().join(format!(
            "arora-sitedec-switch-{}",
            std::process::id()
        ));
        let a = base.join("a");
        let b = base.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();

        store::lock().set_data_dir(a.to_str().unwrap()).unwrap();
        set("js", "a.example", "allow").unwrap();
        assert!(store_present());

        // A different dir is a different store: the cached rows must
        // not leak into it and must not be written back over its file.
        store::lock().set_data_dir(b.to_str().unwrap()).unwrap();
        assert!(!store_present());
        assert_eq!(get("js", "a.example").unwrap(), None);
        set("js", "b.example", "block").unwrap();

        // And back: A's file still carries only A's rows.
        store::lock().set_data_dir(a.to_str().unwrap()).unwrap();
        assert_eq!(
            get("js", "a.example").unwrap(),
            Some("allow".to_string())
        );
        assert_eq!(get("js", "b.example").unwrap(), None);
        let _ = reset();

        let _ = std::fs::remove_dir_all(&base);
    }

    #[test]
    fn corrupt_file_tolerates_valid_neighbours() {
        with_store(|| {
            // A file with a mix of valid and malformed rows loads the
            // valid ones and drops the rest.
            let path = file_path().unwrap();
            std::fs::write(
                &path,
                br#"{"version":1,"kinds":{
                    "js":{"ok.example":"allow","":42,"x":7},
                    "bad":42,
                    "popup":{"pop.example":"allow"}
                }}"#,
            )
            .unwrap();
            reload().unwrap();
            assert_eq!(
                get("js", "ok.example").unwrap(),
                Some("allow".to_string())
            );
            assert!(get("js", "x").unwrap().is_none());
            assert_eq!(
                get("popup", "pop.example").unwrap(),
                Some("allow".to_string())
            );
            // Unparsable entirely -> empty store, no error.
            std::fs::write(&path, b"{ not json").unwrap();
            reload().unwrap();
            assert!(get("js", "ok.example").unwrap().is_none());
            // A healed write round-trips.
            set("js", "back.example", "block").unwrap();
            reload().unwrap();
            assert_eq!(
                get("js", "back.example").unwrap(),
                Some("block".to_string())
            );
        });
    }

    #[test]
    fn persistence_survives_reload() {
        with_store(|| {
            set("webperm", "https%3A%2F%2Fh.example/Notifications", "grant")
                .unwrap();
            set("container", "h.example", "cont-1").unwrap();
            reload().unwrap();
            assert_eq!(
                get("container", "h.example").unwrap(),
                Some("cont-1".to_string())
            );
            let entries = list_json("webperm").unwrap();
            assert!(entries.contains("Notifications"));
        });
    }

    #[test]
    fn clear_kind_and_reset() {
        with_store(|| {
            set("js", "a", "x").unwrap();
            set("popup", "a", "x").unwrap();
            assert!(clear_kind("js").unwrap());
            assert!(!clear_kind("js").unwrap());
            assert!(get("popup", "a").unwrap().is_some());
            reset().unwrap();
            assert!(get("popup", "a").unwrap().is_none());
        });
    }
}
