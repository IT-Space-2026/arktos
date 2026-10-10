//! OSE01: the OpenSearch engine registry.
//!
//! Canonical, engine-neutral home for the browser's search-engine set —
//! the records previously persisted as one OpenSearch-1.1 XML descriptor
//! per engine under `<data dir>/searchengines/` plus the QSettings
//! openSearch group's keyword bindings, engine order and
//! removed-bundled blocklist.  All of that lives here now, in one
//! document:
//!
//!   { "version": 1,
//!     "order": ["Engine Name", ...],
//!     "engines": { "Engine Name": {record}, ... },
//!     "removed_bundled": ["Name", ...] }
//!
//! persisted atomically (write-temp + rename, 0600) to
//! `<data dir>/searchengines.json`.  Row-tolerant decode mirrors
//! sitedecisions: a malformed engine record or keyword list drops only
//! itself; a wholly unusable file degrades to an empty registry.
//!
//! Engine record JSON (rc_ose_get, rc_ose_put input):
//!   {"name","description","imageUrl",
//!    "search"|"suggestions"|"image":
//!        {"template","method","params":[["k","v"],...]},
//!    "keywords":[...]}
//!
//! — the same field map rc_opensearch_parse emits, plus "keywords".
//! "search" is required for a usable record (the Qt side's isValid());
//! the other slots are optional and absent when the engine lacks them.
//!
//! Bundled descriptors are vendored into the crate from
//! src/data/searchengines (the same files the no-rust build's qrc
//! ships — include_bytes! keeps a single source of truth).  They seed
//! the store through rc_ose_seed_bundled / rc_ose_restore_bundled —
//! never as an implicit read fallback, so deleting a bundled engine
//! stays deleted (removed_bundled gates resurrection).
//!
//! URL expansion (rc_ose_expand / rc_ose_url / rc_ose_keyword_url) is a
//! byte-identical port of OpenSearchEngine::parseTemplate + buildUrl,
//! including the two different percent-encoding tables Qt applies:
//!   * {searchTerms} substitution uses QUrl::toPercentEncoding —
//!     only [A-Za-z0-9-._~] survive raw.
//!   * the existing template query AND appended Param values go
//!     through QUrlQuery's component encoder — a wider raw set
//!     (probed on Qt 6.12: alnum -._~ plus !$'()*+,/:;?@[]), where a
//!     valid %XX triplet is preserved (uppercase-normalized) and a
//!     bare '%' encodes as %25.  Pair structure is preserved
//!     verbatim: bare keys stay bare, empty segments stay.
//! The Qt adapter wraps the returned string in QUrl::fromEncoded —
//! the same call the old code made on its intermediate result, so
//! downstream normalization is unchanged.
//!
//! Every mutation persists synchronously and reports whether anything
//! changed; the FFI layer emits the "searchengines" change topic on a
//! true change.

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::parsers;
use crate::store;
use serde_json::{json, Value};
use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{LazyLock, Mutex};

const MAX_FILE_BYTES: u64 = 4 * 1024 * 1024;
const MAX_ENGINES: usize = 512;
const MAX_NAME: usize = 256;
const MAX_FIELD: usize = 4096;
const MAX_PARAMS: usize = 64;
const MAX_PARAM_LEN: usize = 2048;
const MAX_KEYWORDS: usize = 64;
const MAX_KEYWORD_LEN: usize = 128;
const MAX_REMOVED: usize = 1024;
const MAX_DESCRIPTOR: usize = 1024 * 1024;

const FILE_VERSION: u32 = 1;

/// One <Url> endpoint of a descriptor — template + method + Param
/// pairs, mirroring the parsers.rs slot schema.
#[derive(Clone, Default, PartialEq)]
pub struct UrlSlot {
    pub template: String,
    pub method: String,
    pub params: Vec<(String, String)>,
}

#[derive(Clone, Default, PartialEq)]
pub struct Engine {
    pub name: String,
    pub description: String,
    pub image_url: String,
    pub search: Option<UrlSlot>,
    pub suggestions: Option<UrlSlot>,
    pub image: Option<UrlSlot>,
    pub keywords: Vec<String>,
}

impl Engine {
    /// The Qt side's isValid(): a usable engine needs a name and a
    /// search template.
    fn valid(&self) -> bool {
        !self.name.is_empty()
            && self
                .search
                .as_ref()
                .map(|s| !s.template.is_empty())
                .unwrap_or(false)
    }
}

#[derive(Default)]
struct Registry {
    engines: BTreeMap<String, Engine>,
    order: Vec<String>,
    removed_bundled: Vec<String>,
    loaded_dir: Option<PathBuf>,
}

fn registry() -> &'static LazyLock<Mutex<Registry>> {
    static REGISTRY: LazyLock<Mutex<Registry>> =
        LazyLock::new(|| Mutex::new(Registry::default()));
    &REGISTRY
}

fn file_path() -> RcResult<PathBuf> {
    Ok(store::lock().dir()?.join("searchengines.json"))
}

/// True once the store file exists — the Qt migration gate.
pub fn store_present() -> bool {
    file_path().map(|p| p.exists()).unwrap_or(false)
}

// ------------------------------------------------------------------
// (de)serialization
// ------------------------------------------------------------------

fn slot_json(slot: &UrlSlot) -> Value {
    let params: Vec<Value> = slot
        .params
        .iter()
        .map(|(k, v)| json!([k, v]))
        .collect();
    json!({
        "template": slot.template,
        "method": slot.method,
        "params": params,
    })
}

fn engine_json(engine: &Engine) -> Value {
    let mut root = serde_json::Map::new();
    root.insert("name".into(), json!(engine.name));
    root.insert("description".into(), json!(engine.description));
    root.insert("imageUrl".into(), json!(engine.image_url));
    if let Some(slot) = &engine.search {
        root.insert("search".into(), slot_json(slot));
    }
    if let Some(slot) = &engine.suggestions {
        root.insert("suggestions".into(), slot_json(slot));
    }
    if let Some(slot) = &engine.image {
        root.insert("image".into(), slot_json(slot));
    }
    root.insert("keywords".into(), json!(engine.keywords));
    Value::Object(root)
}

fn slot_from_json(v: &Value) -> Option<UrlSlot> {
    let obj = v.as_object()?;
    let template = obj.get("template")?.as_str()?;
    if template.len() > MAX_FIELD {
        return None;
    }
    let method = obj
        .get("method")
        .and_then(|m| m.as_str())
        .unwrap_or("")
        .to_string();
    let mut params = Vec::new();
    if let Some(list) = obj.get("params").and_then(|p| p.as_array()) {
        for pair in list {
            let kv = pair.as_array()?;
            if kv.len() != 2 {
                return None;
            }
            let (k, v) = (kv[0].as_str()?, kv[1].as_str()?);
            if k.len() > MAX_PARAM_LEN || v.len() > MAX_PARAM_LEN {
                return None;
            }
            params.push((k.to_string(), v.to_string()));
            if params.len() > MAX_PARAMS {
                return None;
            }
        }
    }
    Some(UrlSlot {
        template: template.to_string(),
        method,
        params,
    })
}

fn keywords_from_json(v: &Value) -> Option<Vec<String>> {
    let list = v.as_array()?;
    let mut out = Vec::new();
    for item in list {
        let kw = item.as_str()?;
        if kw.is_empty() || kw.len() > MAX_KEYWORD_LEN {
            return None;
        }
        out.push(kw.to_string());
        if out.len() > MAX_KEYWORDS {
            return None;
        }
    }
    Some(out)
}

/// Tolerant record decode: returns None when anything the record
/// needs is malformed — the caller drops just this record.
fn engine_from_json(v: &Value) -> Option<Engine> {
    let obj = v.as_object()?;
    let name = obj.get("name")?.as_str()?;
    if name.is_empty() || name.len() > MAX_NAME {
        return None;
    }
    let field = |key: &str| -> Option<String> {
        match obj.get(key) {
            None => Some(String::new()),
            Some(v) => {
                let s = v.as_str()?;
                if s.len() > MAX_FIELD {
                    None
                } else {
                    Some(s.to_string())
                }
            }
        }
    };
    let mut engine = Engine {
        name: name.to_string(),
        description: field("description")?,
        image_url: field("imageUrl")?,
        search: None,
        suggestions: None,
        image: None,
        keywords: Vec::new(),
    };
    for (key, slot) in [("search", 0), ("suggestions", 1), ("image", 2)] {
        if let Some(v) = obj.get(key) {
            let parsed = slot_from_json(v)?;
            match slot {
                0 => engine.search = Some(parsed),
                1 => engine.suggestions = Some(parsed),
                _ => engine.image = Some(parsed),
            }
        }
    }
    if let Some(v) = obj.get("keywords") {
        engine.keywords = keywords_from_json(v)?;
    }
    Some(engine)
}

fn load_from_disk(state: &mut Registry) -> RcResult<()> {
    let path = file_path()?;
    if !path.exists() {
        state.engines.clear();
        state.order.clear();
        state.removed_bundled.clear();
        return Ok(());
    }
    if std::fs::metadata(&path)?.len() > MAX_FILE_BYTES {
        return fail(
            RcStatus::Corrupt,
            "searchengines.json exceeds the size bound",
        );
    }
    let bytes = std::fs::read(&path)?;
    let doc: Value = serde_json::from_slice(&bytes).unwrap_or(Value::Null);

    let mut engines = BTreeMap::new();
    if let Some(map) = doc.get("engines").and_then(|e| e.as_object()) {
        for (name, record) in map {
            if engines.len() >= MAX_ENGINES {
                break;
            }
            // The map key is authoritative for the slot but the record
            // must carry the same name — a mismatch drops the record.
            match engine_from_json(record) {
                Some(engine) if engine.name == *name => {
                    engines.insert(name.clone(), engine);
                }
                _ => {}
            }
        }
    }

    let mut order = Vec::new();
    if let Some(list) = doc.get("order").and_then(|o| o.as_array()) {
        for item in list {
            if let Some(name) = item.as_str() {
                if engines.contains_key(name) && !order.iter().any(|n| n == name)
                {
                    order.push(name.to_string());
                }
            }
        }
    }

    let mut removed = Vec::new();
    if let Some(list) = doc
        .get("removed_bundled")
        .and_then(|r| r.as_array())
    {
        for item in list {
            if let Some(name) = item.as_str() {
                if !name.is_empty()
                    && name.len() <= MAX_NAME
                    && removed.len() < MAX_REMOVED
                    && !removed.iter().any(|n: &String| n == name)
                {
                    removed.push(name.to_string());
                }
            }
        }
    }

    state.engines = engines;
    state.order = order;
    state.removed_bundled = removed;
    Ok(())
}

fn persist(state: &Registry) -> RcResult<()> {
    let path = file_path()?;
    if state.engines.is_empty() && state.removed_bundled.is_empty() {
        // An emptied registry removes its file — store_present()
        // then reports "nothing stored" (the migration gate re-arms).
        if path.exists() {
            std::fs::remove_file(&path)?;
        }
        return Ok(());
    }
    let engines: serde_json::Map<String, Value> = state
        .engines
        .iter()
        .map(|(name, engine)| (name.clone(), engine_json(engine)))
        .collect();
    let order: Vec<String> = effective_order(state);
    let doc = json!({
        "version": FILE_VERSION,
        "order": order,
        "engines": Value::Object(engines),
        "removed_bundled": state.removed_bundled,
    });
    let bytes = serde_json::to_vec(&doc).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode searchengines: {e}"),
    })?;
    store::atomic_write(&path, &bytes)
}

/// order[] filtered to existing engines, stragglers appended —
/// allEnginesNames()' persisted-order semantics.
fn effective_order(state: &Registry) -> Vec<String> {
    let mut out: Vec<String> = state
        .order
        .iter()
        .filter(|n| state.engines.contains_key(*n))
        .cloned()
        .collect();
    for name in state.engines.keys() {
        if !out.iter().any(|n| n == name) {
            out.push(name.clone());
        }
    }
    out
}

fn ensure_loaded(state: &mut Registry) -> RcResult<()> {
    let dir = file_path()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()));
    if state.loaded_dir != dir {
        // A corrupt file degrades to an empty registry (preferences,
        // not secrets) — same discipline as sitedecisions.
        let _ = load_from_disk(state);
        state.loaded_dir = dir;
    }
    Ok(())
}

fn mutate<F>(state: &mut Registry, f: F) -> RcResult<bool>
where
    F: FnOnce(&mut Registry) -> bool,
{
    ensure_loaded(state)?;
    if !f(state) {
        return Ok(false);
    }
    persist(state)?;
    Ok(true)
}

// ------------------------------------------------------------------
// registry operations (crate API; the FFI layer thin-wraps these)
// ------------------------------------------------------------------

/// Engine names in display order — JSON array.
pub fn list_json() -> RcResult<String> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    Ok(serde_json::to_string(&effective_order(&state))
        .unwrap_or_else(|_| "[]".into()))
}

/// One engine record as JSON (None when absent).
pub fn get_json(name: &str) -> RcResult<Option<String>> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    Ok(state
        .engines
        .get(name)
        .map(|e| engine_json(e).to_string()))
}

/// Upsert an engine record from its JSON form (the rc_ose_put
/// payload).  A "keywords" member replaces the engine's keyword list;
/// its absence preserves the existing list (save-sync pushes carry no
/// keywords).  The record must satisfy the Qt isValid() contract:
/// non-empty name + non-empty search template.
pub fn put_json(payload: &[u8]) -> RcResult<bool> {
    let doc: Value = serde_json::from_slice(payload).map_err(|e| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("engine record is not JSON: {e}"),
    })?;
    let has_keywords = doc.get("keywords").is_some();
    let engine = engine_from_json(&doc).ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "engine record failed schema bounds".into(),
    })?;
    if !engine.valid() {
        return fail(
            RcStatus::InvalidArgument,
            "engine needs a name and a search template",
        );
    }
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        if s.engines.len() >= MAX_ENGINES && !s.engines.contains_key(&engine.name)
        {
            return false;
        }
        let mut record = engine.clone();
        if !has_keywords {
            if let Some(old) = s.engines.get(&engine.name) {
                record.keywords = old.keywords.clone();
            }
        }
        if s.engines.get(&engine.name) == Some(&record) {
            return false;
        }
        if !s.order.iter().any(|n| n == &record.name) {
            s.order.push(record.name.clone());
        }
        s.engines.insert(record.name.clone(), record);
        true
    })
}

/// Parse an OpenSearch descriptor and upsert it (keywords preserved
/// on replace) — the descriptor-file import path.  The descriptor
/// bytes go through the same bounded, DTD-refusing parsers.rs walk
/// as rc_opensearch_parse.
pub fn import_descriptor(xml: &[u8]) -> RcResult<bool> {
    if xml.len() > MAX_DESCRIPTOR {
        return fail(
            RcStatus::Corrupt,
            "the OpenSearch description is too large",
        );
    }
    let parsed = parsers::opensearch(xml)?;
    let doc: Value = serde_json::from_slice(&parsed).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("descriptor JSON: {e}"),
    })?;
    let engine = engine_from_json(&doc).ok_or_else(|| Fail {
        status: RcStatus::Corrupt,
        msg: "descriptor produced an unusable record".into(),
    })?;
    if !engine.valid() {
        return fail(RcStatus::Corrupt, "descriptor is not a usable engine");
    }
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        if s.engines.len() >= MAX_ENGINES && !s.engines.contains_key(&engine.name)
        {
            return false;
        }
        let mut record = engine.clone();
        if let Some(old) = s.engines.get(&engine.name) {
            record.keywords = old.keywords.clone();
            if *old == record {
                return false;
            }
        }
        if !s.order.iter().any(|n| n == &record.name) {
            s.order.push(record.name.clone());
        }
        s.engines.insert(record.name.clone(), record);
        true
    })
}

pub fn remove(name: &str) -> RcResult<bool> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        if s.engines.remove(name).is_none() {
            return false;
        }
        s.order.retain(|n| n != name);
        true
    })
}

/// Re-key an engine keeping its order slot and keywords; fails when
/// the source is absent or the target is empty/taken — the Qt
/// renameEngine contract.
pub fn rename(old: &str, new: &str) -> RcResult<bool> {
    let trimmed = new.trim();
    if trimmed.is_empty() || trimmed.len() > MAX_NAME || trimmed == old {
        return fail(RcStatus::InvalidArgument, "invalid new engine name");
    }
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    if !state.engines.contains_key(old) {
        return fail(RcStatus::NotFound, "no such engine");
    }
    if state.engines.contains_key(trimmed) {
        return fail(
            RcStatus::InvalidArgument,
            "an engine with that name already exists",
        );
    }
    mutate(&mut state, |s| {
        let mut record = s.engines.remove(old).unwrap();
        record.name = trimmed.to_string();
        s.engines.insert(trimmed.to_string(), record);
        match s.order.iter().position(|n| n == old) {
            Some(i) => s.order[i] = trimmed.to_string(),
            None => s.order.push(trimmed.to_string()),
        }
        true
    })
}

/// Replace the display order with a JSON name array: listed existing
/// names lead, unlisted engines keep their previous tail order.
pub fn reorder(payload: &[u8]) -> RcResult<bool> {
    let doc: Value = serde_json::from_slice(payload).map_err(|e| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("order payload is not JSON: {e}"),
    })?;
    let list = doc.as_array().ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "order payload is not an array".into(),
    })?;
    let mut requested = Vec::new();
    for item in list {
        let name = item.as_str().ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "order entries must be strings".into(),
        })?;
        requested.push(name.to_string());
    }
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        let mut next: Vec<String> = requested
            .iter()
            .filter(|n| s.engines.contains_key(*n))
            .cloned()
            .collect();
        next.dedup();
        for name in &s.order {
            if s.engines.contains_key(name) && !next.iter().any(|n| n == name)
            {
                next.push(name.clone());
            }
        }
        for name in s.engines.keys() {
            if !next.iter().any(|n| n == name) {
                next.push(name.clone());
            }
        }
        if next == s.order {
            return false;
        }
        s.order = next;
        true
    })
}

/// Replace an engine's keyword bindings (JSON string array).  A
/// keyword maps to at most one engine — matching the old
/// QHash<keyword, engine> semantics, newly bound keywords are
/// stripped from every other engine first.
pub fn set_keywords(name: &str, payload: &[u8]) -> RcResult<bool> {
    let doc: Value = serde_json::from_slice(payload).map_err(|e| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("keywords payload is not JSON: {e}"),
    })?;
    let list = keywords_from_json(&doc).ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "keywords payload failed schema bounds".into(),
    })?;
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    if !state.engines.contains_key(name) {
        return fail(RcStatus::NotFound, "no such engine");
    }
    mutate(&mut state, |s| {
        let mut changed = false;
        for (engine_name, engine) in s.engines.iter_mut() {
            if engine_name == name {
                continue;
            }
            let before = engine.keywords.len();
            engine.keywords.retain(|k| !list.contains(k));
            changed |= engine.keywords.len() != before;
        }
        let target = s.engines.get_mut(name).unwrap();
        // Later duplicates collapse to one binding — the QHash the
        // Qt side replaced stored each keyword once.
        let mut deduped: Vec<String> = Vec::new();
        for kw in &list {
            if !kw.is_empty() && !deduped.contains(kw) {
                deduped.push(kw.clone());
            }
        }
        if target.keywords != deduped {
            target.keywords = deduped;
            changed = true;
        }
        changed
    })
}

/// The engine name a keyword resolves to, walking in display order.
pub fn engine_for_keyword(keyword: &str) -> RcResult<Option<String>> {
    if keyword.is_empty() || keyword.len() > MAX_KEYWORD_LEN {
        return Ok(None);
    }
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    for name in effective_order(&state) {
        if state
            .engines
            .get(&name)
            .map(|e| e.keywords.iter().any(|k| k == keyword))
            .unwrap_or(false)
        {
            return Ok(Some(name));
        }
    }
    Ok(None)
}

/// Every bound keyword — JSON array, display order.
pub fn keywords_json() -> RcResult<String> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    let mut out: Vec<String> = Vec::new();
    for name in effective_order(&state) {
        if let Some(engine) = state.engines.get(&name) {
            for kw in &engine.keywords {
                if !out.contains(kw) {
                    out.push(kw.clone());
                }
            }
        }
    }
    Ok(serde_json::to_string(&out).unwrap_or_else(|_| "[]".into()))
}

/// The removed-bundled blocklist — JSON array.
pub fn removed_bundled_json() -> RcResult<String> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    Ok(serde_json::to_string(&state.removed_bundled)
        .unwrap_or_else(|_| "[]".into()))
}

/// Add `name` to the bundled-removal blocklist (idempotent).
pub fn block_bundled(name: &str) -> RcResult<bool> {
    if name.is_empty() || name.len() > MAX_NAME {
        return fail(RcStatus::InvalidArgument, "invalid bundled name");
    }
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        if s.removed_bundled.iter().any(|n| n == name)
            || s.removed_bundled.len() >= MAX_REMOVED
        {
            return false;
        }
        s.removed_bundled.push(name.to_string());
        true
    })
}

/// Drop `name` from the blocklist — renaming a user engine to a
/// blocked name unblocks it (the Qt renameEngine semantic).
pub fn unblock_bundled(name: &str) -> RcResult<bool> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        let before = s.removed_bundled.len();
        s.removed_bundled.retain(|n| n != name);
        s.removed_bundled.len() != before
    })
}

/// Wipe the registry and remove the file — the test-suite reset
/// seam.  An empty persist deletes searchengines.json, which re-arms
/// the Qt-side legacy migration gate (store_present()).
pub fn reset() -> RcResult<()> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    state.engines.clear();
    state.order.clear();
    state.removed_bundled.clear();
    persist(&state)?;
    Ok(())
}

// ------------------------------------------------------------------
// bundled descriptors (vendored — same files the qrc ships)
// ------------------------------------------------------------------

static BUNDLED_XMLS: &[&[u8]] = &[
    include_bytes!("../../data/searchengines/DuckDuckGo.xml"),
    include_bytes!("../../data/searchengines/Google.xml"),
    include_bytes!("../../data/searchengines/Google_Im_Feeling_Lucky.xml"),
    include_bytes!("../../data/searchengines/Reddit.xml"),
    include_bytes!("../../data/searchengines/Wikipedia_en.xml"),
    include_bytes!("../../data/searchengines/Yahoo.xml"),
    include_bytes!("../../data/searchengines/YouTube.xml"),
];

/// The vendored engines, parsed once, sorted by name for a
/// deterministic canonical order.
fn bundled() -> &'static [Engine] {
    static BUNDLED: LazyLock<Vec<Engine>> = LazyLock::new(|| {
        let mut engines: Vec<Engine> = BUNDLED_XMLS
            .iter()
            .filter_map(|xml| {
                let json = parsers::opensearch(xml).ok()?;
                let doc: Value = serde_json::from_slice(&json).ok()?;
                engine_from_json(&doc)
            })
            .collect();
        engines.sort_by(|a, b| a.name.cmp(&b.name));
        engines
    });
    &BUNDLED
}

pub fn bundled_names_json() -> String {
    let names: Vec<&str> = bundled().iter().map(|e| e.name.as_str()).collect();
    serde_json::to_string(&names).unwrap_or_else(|_| "[]".into())
}

/// Insert every bundled engine that is absent and not blocked —
/// load()'s bundled merge: existing (user-modified) records win.
pub fn seed_bundled() -> RcResult<bool> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        let mut changed = false;
        for engine in bundled() {
            if s.engines.contains_key(&engine.name)
                || s.removed_bundled.iter().any(|n| n == &engine.name)
            {
                continue;
            }
            s.order.push(engine.name.clone());
            s.engines.insert(engine.name.clone(), engine.clone());
            changed = true;
        }
        changed
    })
}

/// "Restore defaults": clear the blocklist and re-add every bundled
/// engine, REPLACING same-named records — persisted copies can be
/// stale — while keeping their keyword bindings (the old code kept
/// bindings on the pointer identity, which survives a descriptor
/// swap).
pub fn restore_bundled() -> RcResult<bool> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    mutate(&mut state, |s| {
        let mut changed = !s.removed_bundled.is_empty();
        s.removed_bundled.clear();
        for bundled in bundled() {
            let mut record = bundled.clone();
            match s.engines.get(&record.name) {
                Some(old) => {
                    record.keywords = old.keywords.clone();
                    if *old == record {
                        continue;
                    }
                }
                None => {}
            }
            if !s.order.iter().any(|n| n == &record.name) {
                s.order.push(record.name.clone());
            }
            s.engines.insert(record.name.clone(), record);
            changed = true;
        }
        changed
    })
}

// ------------------------------------------------------------------
// URL expansion — the {searchTerms} contract
// ------------------------------------------------------------------

/// QUrl::toPercentEncoding: only the RFC-3986 unreserved set survives.
fn query_term_encode(term: &str, out: &mut String) {
    for &b in term.as_bytes() {
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_'
            | b'~' => out.push(b as char),
            _ => {
                out.push('%');
                out.push_str(&format!("{:02X}", b));
            }
        }
    }
}

/// QUrlQuery's query-component encoder (probed on Qt 6.12): a wider
/// raw set than toPercentEncoding, valid %XX triplets are preserved
/// (uppercase-normalized), a stray '%' encodes as %25.
fn query_component_encode(text: &str, out: &mut String) {
    let bytes = text.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        let b = bytes[i];
        if b == b'%' {
            if i + 2 < bytes.len()
                && bytes[i + 1].is_ascii_hexdigit()
                && bytes[i + 2].is_ascii_hexdigit()
            {
                out.push('%');
                out.push((bytes[i + 1] as char).to_ascii_uppercase());
                out.push((bytes[i + 2] as char).to_ascii_uppercase());
                i += 3;
                continue;
            }
            out.push_str("%25");
            i += 1;
            continue;
        }
        match b {
            b'A'..=b'Z' | b'a'..=b'z' | b'0'..=b'9' | b'-' | b'.' | b'_'
            | b'~' | b'!' | b'$' | b'\'' | b'(' | b')' | b'*' | b'+'
            | b',' | b'/' | b':' | b';' | b'?' | b'@' | b'[' | b']' => {
                out.push(b as char)
            }
            _ => {
                out.push('%');
                out.push_str(&format!("{:02X}", b));
            }
        }
        i += 1;
    }
}

/// The C++ parseTemplate(): the fixed substitution table, then the
/// {*:source} regex, then {searchTerms} last (so a substituted value
/// containing the literal text "{searchTerms}" expands again — the
/// original's exact replacement order).
fn parse_template(term: &str, templ: &str, language: &str, source: &str) -> String {
    let mut out = templ
        .replace("{count}", "20")
        .replace("{startIndex}", "0")
        .replace("{startPage}", "0")
        .replace("{language}", language)
        .replace("{inputEncoding}", "UTF-8")
        .replace("{outputEncoding}", "UTF-8");
    // \{([^}]*:|)source\??\} — "{source}", "{source?}", "{ns:source}",
    // "{ns:source?}" all rewrite to the application name.
    let mut rewritten = String::with_capacity(out.len());
    let bytes = out.as_bytes();
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] != b'{' {
            // Push one UTF-8 char, not a byte.
            let ch_len = out[i..].chars().next().map(|c| c.len_utf8()).unwrap_or(1);
            rewritten.push_str(&out[i..i + ch_len]);
            i += ch_len;
            continue;
        }
        let Some(close) = out[i..].find('}').map(|p| i + p) else {
            rewritten.push('{');
            i += 1;
            continue;
        };
        let inner = &out[i + 1..close];
        let matches = inner == "source"
            || inner == "source?"
            || inner.ends_with(":source")
            || inner.ends_with(":source?");
        if matches {
            rewritten.push_str(source);
        } else {
            rewritten.push_str(&out[i..=close]);
        }
        i = close + 1;
    }
    out = rewritten;
    let mut encoded = String::new();
    query_term_encode(term, &mut encoded);
    out.replace("{searchTerms}", &encoded)
}

/// One existing query pair -> encoded form: the '=' split and the
/// bare-key flag are preserved verbatim.
fn encode_query_pair(pair: &str, out: &mut String) {
    match pair.find('=') {
        Some(eq) => {
            query_component_encode(&pair[..eq], out);
            out.push('=');
            query_component_encode(&pair[eq + 1..], out);
        }
        None => query_component_encode(pair, out),
    }
}

/// The buildUrl() port: expand the template, then (non-POST only)
/// re-encode the existing query pairs and append the Param items —
/// QUrlQuery's normalize-on-setQuery behavior.
pub fn expand(
    templ: &str,
    method: &str,
    params: &[(String, String)],
    term: &str,
    language: &str,
    source: &str,
) -> String {
    if templ.is_empty() {
        return String::new();
    }
    let expanded = parse_template(term, templ, language, source);
    if method == "post" {
        // POST endpoints take the parameters as request body — the
        // URL is the expanded template verbatim (the old buildUrl
        // never touched the query for post).
        return expanded;
    }

    // The fragment is out of scope for the query surgery — split it
    // off and reattach verbatim.
    let (main, fragment) = match expanded.find('#') {
        Some(i) => (&expanded[..i], &expanded[i..]),
        None => (expanded.as_str(), ""),
    };

    let had_qmark = main.contains('?');
    let (base, existing) = match main.find('?') {
        Some(i) => (&main[..i], &main[i + 1..]),
        None => (main, ""),
    };

    // QUrlQuery normalizes the existing query on setQuery even when
    // no params get appended — re-encode every pair.
    let mut query = String::new();
    if !existing.is_empty() {
        for (idx, pair) in existing.split('&').enumerate() {
            if idx > 0 {
                query.push('&');
            }
            encode_query_pair(pair, &mut query);
        }
    }
    for (key, value) in params {
        if !query.is_empty() {
            query.push('&');
        }
        query_component_encode(key, &mut query);
        query.push('=');
        let expanded_value = parse_template(term, value, language, source);
        query_component_encode(&expanded_value, &mut query);
    }

    let mut out = String::with_capacity(base.len() + query.len() + fragment.len() + 1);
    out.push_str(base);
    if had_qmark || !query.is_empty() {
        out.push('?');
        out.push_str(&query);
    }
    out.push_str(fragment);
    out
}

/// spec JSON -> expanded URL string (None on bad spec):
///   {"template","method","params":[[k,v],...],"term",
///    "language","source"}
pub fn expand_json(payload: &[u8]) -> Option<String> {
    let doc: Value = serde_json::from_slice(payload).ok()?;
    let obj = doc.as_object()?;
    let templ = obj.get("template")?.as_str()?;
    let term = obj.get("term").and_then(|t| t.as_str()).unwrap_or("");
    let method = obj
        .get("method")
        .and_then(|m| m.as_str())
        .unwrap_or("");
    let language = obj
        .get("language")
        .and_then(|l| l.as_str())
        .unwrap_or("");
    let source = obj
        .get("source")
        .and_then(|s| s.as_str())
        .unwrap_or("");
    let params: Vec<(String, String)> = match obj.get("params") {
        None => Vec::new(),
        Some(v) => {
            let list = v.as_array()?;
            let mut out = Vec::new();
            for pair in list {
                let kv = pair.as_array()?;
                if kv.len() != 2 {
                    return None;
                }
                out.push((
                    kv[0].as_str()?.to_string(),
                    kv[1].as_str()?.to_string(),
                ));
            }
            out
        }
    };
    Some(expand(templ, method, &params, term, language, source))
}

/// Registry-backed resolution: engine `name`, slot `kind`
/// ("search"|"suggestions"|"image"), `term` -> expanded URL.
pub fn engine_url(
    name: &str,
    kind: &str,
    term: &str,
    language: &str,
    source: &str,
) -> RcResult<Option<String>> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    ensure_loaded(&mut state)?;
    let Some(engine) = state.engines.get(name) else {
        return Ok(None);
    };
    let slot = match kind {
        "search" => engine.search.as_ref(),
        "suggestions" => engine.suggestions.as_ref(),
        "image" => engine.image.as_ref(),
        _ => return fail(RcStatus::InvalidArgument, "unknown url kind"),
    };
    Ok(slot.map(|s| {
        expand(&s.template, &s.method, &s.params, term, language, source)
    }))
}

/// keyword + terms -> the bound engine's search URL.
pub fn keyword_url(
    keyword: &str,
    term: &str,
    language: &str,
    source: &str,
) -> RcResult<Option<String>> {
    match engine_for_keyword(keyword)? {
        Some(name) => engine_url(&name, "search", term, language, source),
        None => Ok(None),
    }
}

// ------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn with_store(f: impl FnOnce()) {
        let _guard = crate::store::test_lock();
        let dir = std::env::temp_dir().join(format!(
            "arora-ose-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        store::lock().set_data_dir(dir.to_str().unwrap()).unwrap();
        f();
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn engine_json_str(name: &str, templ: &str) -> String {
        format!(
            r#"{{"name":"{}","description":"d","search":{{"template":"{}","method":"get","params":[]}}}}"#,
            name, templ
        )
    }

    #[test]
    fn put_get_remove_roundtrip() {
        with_store(|| {
            assert!(!store_present());
            assert_eq!(list_json().unwrap(), "[]");
            assert!(put_json(
                engine_json_str("Foo", "http://f/?q={searchTerms}").as_bytes()
            )
            .unwrap());
            // Identical re-put reports no change.
            assert!(!put_json(
                engine_json_str("Foo", "http://f/?q={searchTerms}").as_bytes()
            )
            .unwrap());
            assert!(store_present());
            assert_eq!(list_json().unwrap(), r#"["Foo"]"#);
            let rec: Value =
                serde_json::from_str(&get_json("Foo").unwrap().unwrap()).unwrap();
            assert_eq!(rec["search"]["template"], "http://f/?q={searchTerms}");
            // Validation: no name / no template is rejected.
            assert!(put_json(
                br#"{"name":"","search":{"template":"http://x/"}}"#
            )
            .is_err());
            assert!(put_json(
                br#"{"name":"Bad","search":{"template":"","method":"get","params":[]}}"#
            )
            .is_err());
            assert!(put_json(b"not json").is_err());
            assert!(remove("Foo").unwrap());
            assert!(!remove("Foo").unwrap());
            assert_eq!(get_json("Foo").unwrap(), None);
        });
    }

    #[test]
    fn order_survives_and_reorders() {
        with_store(|| {
            for (name, t) in [
                ("B", "http://b/"),
                ("A", "http://a/"),
                ("C", "http://c/"),
            ] {
                put_json(engine_json_str(name, t).as_bytes()).unwrap();
            }
            assert_eq!(list_json().unwrap(), r#"["B","A","C"]"#);
            reorder(br#"["C","B","A"]"#).unwrap();
            assert_eq!(list_json().unwrap(), r#"["C","B","A"]"#);
            // Unknown names drop out of the requested order; unlisted
            // engines keep their previous tail order.
            reorder(br#"["A","ghost"]"#).unwrap();
            assert_eq!(list_json().unwrap(), r#"["A","C","B"]"#);
            reorder(br#"["A","B","C"]"#).unwrap();
            // Persistence across a reload.
            reload().unwrap();
            assert_eq!(list_json().unwrap(), r#"["A","B","C"]"#);
        });
    }

    #[test]
    fn rename_keeps_slot_and_keywords() {
        with_store(|| {
            put_json(engine_json_str("Old", "http://o/").as_bytes()).unwrap();
            put_json(engine_json_str("Other", "http://x/").as_bytes())
                .unwrap();
            set_keywords("Old", br#"["kw"]"#).unwrap();
            rename("Old", "New").unwrap();
            assert_eq!(list_json().unwrap(), r#"["New","Other"]"#);
            assert!(get_json("Old").unwrap().is_none());
            let rec: Value =
                serde_json::from_str(&get_json("New").unwrap().unwrap())
                    .unwrap();
            assert_eq!(rec["keywords"], json!(["kw"]));
            assert_eq!(engine_for_keyword("kw").unwrap().unwrap(), "New");
            // Rejects.
            assert!(rename("missing", "X").is_err());
            assert!(rename("New", "").is_err());
            assert!(rename("New", "New").is_err());
            assert!(rename("New", "Other").is_err());
        });
    }

    #[test]
    fn keywords_are_exclusive() {
        with_store(|| {
            put_json(engine_json_str("One", "http://1/").as_bytes()).unwrap();
            put_json(engine_json_str("Two", "http://2/").as_bytes()).unwrap();
            set_keywords("One", br#"["a","b"]"#).unwrap();
            assert_eq!(engine_for_keyword("a").unwrap().unwrap(), "One");
            set_keywords("Two", br#"["b","c"]"#).unwrap();
            assert_eq!(engine_for_keyword("b").unwrap().unwrap(), "Two");
            assert_eq!(engine_for_keyword("a").unwrap().unwrap(), "One");
            let rec: Value =
                serde_json::from_str(&get_json("One").unwrap().unwrap())
                    .unwrap();
            assert_eq!(rec["keywords"], json!(["a"]));
            assert_eq!(keywords_json().unwrap(), r#"["a","b","c"]"#);
            // Clearing removes the bindings.
            set_keywords("Two", br#"[]"#).unwrap();
            assert!(engine_for_keyword("b").unwrap().is_none());
        });
    }

    #[test]
    fn bundled_seed_and_restore() {
        with_store(|| {
            assert!(seed_bundled().unwrap());
            let names: Vec<String> =
                serde_json::from_str(&list_json().unwrap()).unwrap();
            assert_eq!(names.len(), 7);
            assert!(names.contains(&"DuckDuckGo".to_string()));
            assert!(names.contains(&"YouTube".to_string()));
            // Idempotent — second seed changes nothing.
            assert!(!seed_bundled().unwrap());
            // Block + remove DuckDuckGo: a reseed must not resurrect it.
            block_bundled("DuckDuckGo").unwrap();
            remove("DuckDuckGo").unwrap();
            assert!(!seed_bundled().unwrap());
            assert!(!list_json().unwrap().contains("DuckDuckGo"));
            // restoreDefaults clears the blocklist and brings it back.
            assert!(restore_bundled().unwrap());
            let names: Vec<String> =
                serde_json::from_str(&list_json().unwrap()).unwrap();
            assert_eq!(names.len(), 7);
            // And it preserves keywords bound to a still-present record
            // (a second restore over identical records is a no-op).
            set_keywords("Google", br#"["g"]"#).unwrap();
            restore_bundled().unwrap();
            assert_eq!(engine_for_keyword("g").unwrap().unwrap(), "Google");
            // The bundled parse produced every slot the qrc engines carry.
            let rec: Value = serde_json::from_str(
                &get_json("DuckDuckGo").unwrap().unwrap(),
            )
            .unwrap();
            assert!(rec["search"]["template"].as_str().unwrap().contains(
                "{searchTerms}"
            ));
            assert!(rec["suggestions"].is_object());
            assert!(rec["image"].is_object());
        });
    }

    #[test]
    fn descriptor_import() {
        with_store(|| {
            let xml = br#"<?xml version="1.0"?>
<OpenSearchDescription xmlns="http://a9.com/-/spec/opensearch/1.1/">
  <ShortName>Imp</ShortName>
  <Url method="get" type="text/html" template="http://imp/?q={searchTerms}"/>
</OpenSearchDescription>"#;
            assert!(import_descriptor(xml).unwrap());
            let rec: Value =
                serde_json::from_str(&get_json("Imp").unwrap().unwrap())
                    .unwrap();
            assert_eq!(
                rec["search"]["template"],
                "http://imp/?q={searchTerms}"
            );
            // Malformed descriptors are refused.
            assert!(import_descriptor(b"<notxml").is_err());
            // Keywords survive a re-import (identical descriptor =
            // no change reported).
            set_keywords("Imp", br#"["i"]"#).unwrap();
            import_descriptor(xml).unwrap();
            assert_eq!(engine_for_keyword("i").unwrap().unwrap(), "Imp");
        });
    }

    #[test]
    fn corrupt_store_degrades() {
        with_store(|| {
            let path = file_path().unwrap();
            std::fs::write(&path, b"{\"engines\":{\"x\":1}").unwrap();
            // A torn file loads as an empty registry, no panic.
            assert_eq!(list_json().unwrap(), "[]");
            std::fs::write(
                &path,
                br#"{"version":1,"order":["A","ghost"],"engines":{"A":{"name":"A","search":{"template":"http://a/","method":"get","params":[]}},"B":{"name":"A"}}}"#,
            )
            .unwrap();
            reload().unwrap();
            // "A" survives, mismatched "B" record drops.
            assert_eq!(list_json().unwrap(), r#"["A"]"#);
        });
    }

    #[test]
    fn data_dir_switch_invalidates() {
        let _guard = crate::store::test_lock();
        let base = std::env::temp_dir()
            .join(format!("arora-ose-switch-{}", std::process::id()));
        let a = base.join("a");
        let b = base.join("b");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::create_dir_all(&b).unwrap();
        store::lock().set_data_dir(a.to_str().unwrap()).unwrap();
        put_json(engine_json_str("DirA", "http://a/").as_bytes()).unwrap();
        store::lock().set_data_dir(b.to_str().unwrap()).unwrap();
        assert_eq!(list_json().unwrap(), "[]");
        store::lock().set_data_dir(a.to_str().unwrap()).unwrap();
        assert_eq!(list_json().unwrap(), r#"["DirA"]"#);
        let _ = std::fs::remove_dir_all(&base);
    }

    // ---- expansion: golden vectors probed on Qt 6.12 -------------

    #[test]
    fn term_encoding_matches_qurl() {
        let cases = [
            ("hello world", "hello%20world"),
            ("a+b", "a%2Bb"),
            ("a&b=c", "a%26b%3Dc"),
            ("100%", "100%25"),
            ("ünïcødé", "%C3%BCn%C3%AFc%C3%B8d%C3%A9"),
            ("quote'and\"d", "quote%27and%22d"),
            ("<tag>", "%3Ctag%3E"),
            ("{curly}", "%7Bcurly%7D"),
            ("a/b\\c", "a%2Fb%5Cc"),
            ("?q#f", "%3Fq%23f"),
            ("~tilde_under.dash-x", "~tilde_under.dash-x"),
            ("semi;colon:at@dollar$paren()bang!star*comma,",
             "semi%3Bcolon%3Aat%40dollar%24paren%28%29bang%21star%2Acomma%2C"),
            ("café ☃", "caf%C3%A9%20%E2%98%83"),
            ("中文", "%E4%B8%AD%E6%96%87"),
            ("\n\t ", "%0A%09%20"),
        ];
        for (term, expected) in cases {
            let mut out = String::new();
            query_term_encode(term, &mut out);
            assert_eq!(out, expected, "term {term:?}");
        }
    }

    #[test]
    fn expand_matches_qt() {
        let lang = "en-US";
        let src = "Arora";
        let p: Vec<(String, String)> = Vec::new();
        let cases = [
            ("https://duckduckgo.com/?q={searchTerms}", "get", "hello world",
             "https://duckduckgo.com/?q=hello%20world"),
            ("http://www.google.com/search?hl={language}&q={searchTerms}", "get", "a+b",
             "http://www.google.com/search?hl=en-US&q=a%2Bb"),
            ("https://duckduckgo.com/ac/?q={searchTerms}&type=list", "get", "x&y",
             "https://duckduckgo.com/ac/?q=x%26y&type=list"),
            ("http://e.com/s?a=1+2&q={searchTerms}", "get", "t",
             "http://e.com/s?a=1+2&q=t"),
            ("http://e.com/s?a=%2B&q={searchTerms}", "get", "t",
             "http://e.com/s?a=%2B&q=t"),
            ("http://e.com/s", "post", "a b", "http://e.com/s"),
            ("http://e.com/{searchTerms}/p", "get", "slash/me",
             "http://e.com/slash%2Fme/p"),
            ("http://e.com/?s={arora:source}&q={searchTerms}", "get", "q",
             "http://e.com/?s=Arora&q=q"),
            ("http://e.com/?s={source?}&n={count}&i={startIndex}&p={startPage}&ie={inputEncoding}&oe={outputEncoding}", "get", "x",
             "http://e.com/?s=Arora&n=20&i=0&p=0&ie=UTF-8&oe=UTF-8"),
            ("http://e.com/?q=", "get", "x", "http://e.com/?q="),
        ];
        for (templ, method, term, expected) in cases {
            assert_eq!(
                expand(templ, method, &p, term, lang, src),
                expected,
                "template {templ:?}"
            );
        }
        // Params appended (get) / suppressed (post).
        let params = vec![
            ("q".to_string(), "{searchTerms}".to_string()),
            ("lang".to_string(), "{language}".to_string()),
        ];
        assert_eq!(
            expand("http://e.com/s", "get", &params, "a b+c", lang, src),
            "http://e.com/s?q=a%20b%2Bc&lang=en-US"
        );
        assert_eq!(
            expand("http://e.com/s", "post", &params, "a b", lang, src),
            "http://e.com/s"
        );
        let params2 = vec![
            ("q".to_string(), "static".to_string()),
            ("r".to_string(), "{searchTerms}".to_string()),
        ];
        assert_eq!(
            expand("http://e.com/s?x={searchTerms}", "get", &params2, "a&b", lang, src),
            "http://e.com/s?x=a%26b&q=static&r=a%26b"
        );
        // Empty param values keep the '='.
        let params3 = vec![
            ("empty".to_string(), String::new()),
            ("filled".to_string(), "v".to_string()),
        ];
        assert_eq!(
            expand("http://e.com/?q=", "get", &params3, "x", lang, src),
            "http://e.com/?q=&empty=&filled=v"
        );
        // Existing-query normalization: invalid %-triplets and raw
        // specials re-encode exactly like QUrlQuery.
        assert_eq!(
            expand("http://e.com/?q=a%2Gb", "get", &p, "x", lang, src),
            "http://e.com/?q=a%252Gb"
        );
        assert_eq!(
            expand("http://e.com/?q=a b", "get", &p, "x", lang, src),
            "http://e.com/?q=a%20b"
        );
        assert_eq!(
            expand("http://e.com/?q=a{c}", "get", &p, "x", lang, src),
            "http://e.com/?q=a%7Bc%7D"
        );
        assert_eq!(
            expand("http://e.com/?flag&x=1", "get",
                   &[("n".to_string(), "v".to_string())], "x", lang, src),
            "http://e.com/?flag&x=1&n=v"
        );
        assert_eq!(
            expand("http://e.com/?a=&b&&c=3", "get",
                   &[("n".to_string(), "v".to_string())], "x", lang, src),
            "http://e.com/?a=&b&&c=3&n=v"
        );
        // Fragment is split off, query appended ahead of it.
        assert_eq!(
            expand("http://e.com/?q=t#frag", "get",
                   &[("n".to_string(), "v".to_string())], "x", lang, src),
            "http://e.com/?q=t&n=v#frag"
        );
        assert_eq!(
            expand("http://e.com/#frag?notq", "get",
                   &[("n".to_string(), "v".to_string())], "x", lang, src),
            "http://e.com/?n=v#frag?notq"
        );
        // k=v=w: the value's '=' encodes.
        assert_eq!(
            expand("http://e.com/?k=v=w", "get",
                   &[("n".to_string(), "v".to_string())], "x", lang, src),
            "http://e.com/?k=v%3Dw&n=v"
        );
    }

    #[test]
    fn bundled_parse_parity() {
        // Every vendored descriptor parses into a valid engine.
        for engine in bundled() {
            assert!(engine.valid(), "bundled engine {} invalid", engine.name);
            assert!(!engine.name.is_empty());
        }
        assert_eq!(bundled().len(), 7);
    }
}

/// Re-read the disk file into memory (test/repair seam).
#[cfg(test)]
pub fn reload() -> RcResult<()> {
    let lock = registry();
    let mut state = lock.lock().unwrap();
    load_from_disk(&mut state)?;
    state.loaded_dir = file_path()
        .ok()
        .and_then(|p| p.parent().map(|d| d.to_path_buf()));
    Ok(())
}
