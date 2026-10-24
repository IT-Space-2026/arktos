//! RC16: the per-profile site-icon/favicon cache — icon records keyed
//! by page-url/origin, pointing at PNG files under the profile's
//! CACHE dir (`profiles/<id>/cache/icons/`), not the data dir: a
//! favicon is disposable by definition, refetchable on the next load,
//! and the profile vault's "cache" exclusion already holds the dir
//! aside unsealed.  Fetch stays Qt/engine-side (the page already has
//! the bytes — QWebEnginePage::iconChanged hands over a QIcon);
//! this module owns the index, dedup-by-host, expiry and the file
//! lifecycle.
//!
//! Layout under the bound profile dir:
//!
//!   cache/icons/icons.json   — {"version":1,"imported":bool,
//!                               "entries":{<key>:{file,updated,pages}}}
//!   cache/icons/<key>.png    — one file per icon key
//!
//! The icon key is the LocationBarSiteIcon/HistoryManager contract
//! (`historyIconKey`): the lowercase host of the page url, or the
//! whole url when it has no host (data:, file:) — every page of a
//! host shares one file, which IS the dedup-by-host rule.  `pages`
//! records which page urls named the icon (bounded — the audit trail
//! the spec's "page-url keyed records" asks for) without changing
//! the shared file.
//!
//! Expiry: `sweep` drops entries not refreshed at-or-before a cutoff
//! (the caller passes the history retention boundary — icons age out
//! with the visits they belong to and re-record on the next load's
//! iconChanged), and every fresh index load reconciles the store:
//! index rows whose file vanished drop out, unreferenced files are
//! unlinked.
//!
//! Cutover (USER DATA): when a profile first touches the store with
//! no index yet, the legacy sources are folded in once — the
//! pre-rustcore `<profile>/icons/<pct-host>.png` files (the no-rust
//! HistoryManager layout) and the `icons` table of the sibling
//! `history.db` (the HIST01 rust store this module succeeds).  Both
//! are READ-only imports — sources stay in place, and a clear that
//! emptied them cannot resurrect icons.  The history.db table itself
//! is not dropped: rc_hist_icon_* remains exported (append-only ABI)
//! for legacy callers.

use std::collections::BTreeMap;
use std::collections::HashMap;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::sync::{LazyLock, Mutex};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::omnibox;
use crate::store;

const INDEX_FILE: &str = "icons.json";
/// The import-once marker lives OUTSIDE the icons dir
/// (<profile>/cache/icons.done): an explicit clear wipes the cache
/// and must not resurrect icons from the legacy sources on next
/// touch — the stamp surviving the wipe is what enforces it.
const IMPORT_STAMP: &str = "icons.done";
const VERSION: i64 = 1;

// Bounds: favicons are small; a corrupt or hostile input must not
// allocate absurdly or mint pathological file names.
const MAX_KEY: usize = 2048;
const MAX_PNG: usize = 4 * 1024 * 1024;
const MAX_PAGES_PER_ICON: usize = 24;
const MAX_ENTRIES: usize = 100_000;
const MAX_INDEX_BYTES: u64 = 64 * 1024 * 1024;
const MAX_NAME_LEN: usize = 180; // keep file names inside NAME_MAX

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn io_fail(ctx: &str, e: io::Error) -> Fail {
    Fail {
        status: RcStatus::Io,
        msg: format!("{ctx}: {e}"),
    }
}

/// The icon key for a page url — `historyIconKey` verbatim: the
/// lowercase host, or the whole url when it carries none.
pub fn key_for(url: &str) -> RcResult<String> {
    let host = omnibox::url_host(url).to_lowercase();
    let key = if host.is_empty() { url } else { host.as_str() };
    if key.is_empty() {
        return fail(RcStatus::InvalidArgument, "empty icon url");
    }
    if key.len() > MAX_KEY {
        return fail(RcStatus::InvalidArgument, "icon key too long");
    }
    Ok(key.to_string())
}

/// RFC3986-unreserved percent encoding — the same name shape the
/// legacy store produced for hosts (alnum, '-', '.', '_', '~' raw),
/// applied to every byte so an arbitrary key stays a safe basename.
fn pct_encode(key: &str) -> String {
    let mut out = String::with_capacity(key.len());
    for &b in key.as_bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'.' | b'_' | b'~') {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let mut out = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            let hex = |c: u8| -> Option<u8> {
                match c {
                    b'0'..=b'9' => Some(c - b'0'),
                    b'a'..=b'f' => Some(c - b'a' + 10),
                    b'A'..=b'F' => Some(c - b'A' + 10),
                    _ => None,
                }
            };
            match (hex(b[i + 1]), hex(b[i + 2])) {
                (Some(h), Some(l)) => {
                    out.push(h * 16 + l);
                    i += 3;
                    continue;
                }
                _ => {}
            }
        }
        out.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

/// The icon file's basename inside the icons dir.  Keys that would
/// exceed the filesystem name limit collapse to a content digest —
/// deterministic, still one file per key.
fn file_name_for(key: &str) -> String {
    let enc = pct_encode(key);
    if enc.len() <= MAX_NAME_LEN {
        return format!("{enc}.png");
    }
    let digest = Sha256::digest(key.as_bytes());
    let mut hex = String::with_capacity(32);
    for b in &digest[..16] {
        hex.push_str(&format!("{b:02x}"));
    }
    format!("u-{hex}.png")
}

/// One index row: the shared file plus the page urls that named it.
#[derive(Clone, Debug, Default)]
struct IconEntry {
    file: String,
    updated: i64,
    pages: BTreeMap<String, i64>,
}

#[derive(Default)]
struct IconIndex {
    imported: bool,
    entries: BTreeMap<String, IconEntry>,
}

impl IconIndex {
    fn to_json(&self) -> Vec<u8> {
        let entries: serde_json::Map<String, Value> = self
            .entries
            .iter()
            .map(|(k, e)| {
                (
                    k.clone(),
                    json!({
                        "file": e.file,
                        "updated": e.updated,
                        "pages": e.pages,
                    }),
                )
            })
            .collect();
        serde_json::to_vec(&json!({
            "version": VERSION,
            "imported": self.imported,
            "entries": Value::Object(entries),
        }))
        .unwrap_or_else(|_| b"{}".to_vec())
    }

    /// Tolerant parse: a malformed index reads as an empty store —
    /// this is a cache, so corruption means rebuild, not a hard
    /// error (the caller re-imports the legacy sources then).
    fn from_json(data: &[u8]) -> IconIndex {
        let v: Value = match serde_json::from_slice(data) {
            Ok(v) => v,
            Err(_) => return IconIndex::default(),
        };
        let mut index = IconIndex {
            imported: v.get("imported").and_then(Value::as_bool).unwrap_or(false),
            ..IconIndex::default()
        };
        if let Some(entries) = v.get("entries").and_then(Value::as_object) {
            for (key, ev) in entries {
                let file = match ev.get("file").and_then(Value::as_str) {
                    Some(f) if is_safe_name(f) => f.to_string(),
                    _ => continue, // a row that can't name its file is dead
                };
                let updated = ev.get("updated").and_then(Value::as_i64).unwrap_or(0);
                let mut pages = BTreeMap::new();
                if let Some(pm) = ev.get("pages").and_then(Value::as_object) {
                    for (u, t) in pm.iter().take(MAX_PAGES_PER_ICON) {
                        pages.insert(u.clone(), t.as_i64().unwrap_or(0));
                    }
                }
                index.entries.insert(key.clone(), IconEntry { file, updated, pages });
            }
        }
        index
    }
}

/// A basename the index may reference: no separators, no dot-dots —
/// an index is a cache artifact, never an escape route out of the
/// icons dir.
fn is_safe_name(name: &str) -> bool {
    !name.is_empty()
        && name.len() <= 255
        && name != "."
        && name != ".."
        && !name.contains(['/', '\\', '\0'])
}

// ---- process-local index cache ------------------------------------------
//
// icon() lookups run per url repaint — the index stays in memory per
// icons dir, re-read only when the file's (mtime, len) stamp moved
// (another process or an external edit), matching the anchored-store
// convention of a cheap compare on the hot path.

struct CachedIndex {
    index: IconIndex,
    stamp: Option<(u64, u64)>, // (mtime ms, len) of icons.json
}

static CACHE: LazyLock<Mutex<HashMap<PathBuf, CachedIndex>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

fn icons_dir(profile_dir: &Path) -> PathBuf {
    profile_dir.join("cache").join("icons")
}

fn index_stamp(path: &Path) -> Option<(u64, u64)> {
    let m = fs::metadata(path).ok()?;
    if !m.is_file() {
        return None;
    }
    let ms = m
        .modified()
        .ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0);
    Some((ms, m.len()))
}

fn persist(dir: &Path, index: &IconIndex) -> RcResult<Option<(u64, u64)>> {
    fs::create_dir_all(dir).map_err(|e| io_fail("icon dir", e))?;
    let path = dir.join(INDEX_FILE);
    store::atomic_write(&path, &index.to_json())?;
    Ok(index_stamp(&path))
}

/// Drops index rows whose file vanished and unlinks files no row
/// references — run on every fresh load so a partial delete (or a
/// torn old run) self-heals instead of leaking files.
fn reconcile(index: &mut IconIndex, dir: &Path) {
    let files: std::collections::HashSet<String> = index
        .entries
        .values()
        .map(|e| e.file.clone())
        .collect();
    index.entries.retain(|_, e| dir.join(&e.file).is_file());
    if let Ok(rd) = fs::read_dir(dir) {
        for ent in rd.flatten() {
            let name = ent.file_name().to_string_lossy().into_owned();
            if name == INDEX_FILE
                || name.ends_with(".tmp")
                || files.contains(&name)
            {
                continue;
            }
            if ent.file_type().map(|t| t.is_file()).unwrap_or(false) {
                let _ = fs::remove_file(ent.path());
            }
        }
    }
}

/// The first-touch fold of the legacy stores (see the module doc):
/// the pre-rustcore <profile>/icons/*.png files and the history.db
/// icons table, both read-only.
fn import_index(dir: &Path, profile_dir: &Path) -> IconIndex {
    let mut index = IconIndex {
        imported: true,
        ..IconIndex::default()
    };

    let install = |index: &mut IconIndex, key: String, png: &[u8], updated: i64| {
        if key.is_empty() || key.len() > MAX_KEY || png.is_empty() || png.len() > MAX_PNG {
            return;
        }
        let file = file_name_for(&key);
        let path = dir.join(&file);
        if fs::create_dir_all(dir).is_err()
            || store::atomic_write(&path, png).is_err()
        {
            return;
        }
        index.entries.insert(
            key,
            IconEntry {
                file,
                updated,
                pages: BTreeMap::new(),
            },
        );
    };

    // The no-rust layout: <profile>/icons/<pct-encoded host>.png.
    let legacy_dir = profile_dir.join("icons");
    if let Ok(rd) = fs::read_dir(&legacy_dir) {
        for ent in rd.flatten() {
            let name = ent.file_name().to_string_lossy().into_owned();
            let Some(stem) = name.strip_suffix(".png") else {
                continue;
            };
            let key = pct_decode(stem);
            let Ok(png) = fs::read(ent.path()) else { continue };
            let mtime = ent
                .metadata()
                .ok()
                .and_then(|m| m.modified().ok())
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map(|d| d.as_millis() as i64)
                .unwrap_or(0);
            install(&mut index, key, &png, mtime);
        }
    }

    // The HIST01 table inside the sibling history.db — read-only; a
    // missing db or table (or a locked WAL peer) is simply skipped.
    let db = profile_dir.join("history.db");
    if db.is_file() {
        if let Ok(conn) = rusqlite::Connection::open(&db) {
            if let Ok(mut stmt) =
                conn.prepare("SELECT host, png, updated_at FROM icons")
            {
                if let Ok(rows) = stmt.query_map([], |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, Vec<u8>>(1)?,
                        r.get::<_, i64>(2)?,
                    ))
                }) {
                    for row in rows.flatten() {
                        install(&mut index, row.0, &row.1, row.2);
                    }
                }
            }
        }
    }

    index
}

/// Runs `f` against the profile's cached index, loading/re-stamping
/// first.  `dirty` persistence is the closure's call — read ops
/// leave the file alone.
fn with_index<R>(
    profile_dir: &Path,
    f: impl FnOnce(&mut IconIndex, &Path) -> RcResult<R>,
) -> RcResult<R> {
    let dir = icons_dir(profile_dir);
    let index_path = dir.join(INDEX_FILE);
    let mut map = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    let stamp = index_stamp(&index_path);
    let stale = match map.get(&dir) {
        Some(c) => c.stamp != stamp,
        None => true,
    };
    if stale {
        let mut index = match stamp {
            Some(_) => {
                fs::read(&index_path)
                    .map(|b| {
                        if b.len() as u64 > MAX_INDEX_BYTES {
                            IconIndex::default()
                        } else {
                            IconIndex::from_json(&b)
                        }
                    })
                    .unwrap_or_default()
            }
            // No index yet: fold the legacy stores in unless the
            // once-only stamp says they were folded (or found empty)
            // already — the stamp outlives an explicit clear, and is
            // written only after the index persisted so a failed
            // write retries the idempotent fold instead of
            // suppressing it.
            None => {
                let stamp_path = profile_dir.join("cache").join(IMPORT_STAMP);
                let index = if stamp_path.is_file() {
                    IconIndex::default()
                } else {
                    import_index(&dir, profile_dir)
                };
                let stamp = persist(&dir, &index).unwrap_or(None);
                if stamp.is_some() {
                    let _ = fs::write(&stamp_path, b"1\n");
                }
                map.insert(dir.clone(), CachedIndex { index, stamp });
                let entry = map.get_mut(&dir).expect("index just loaded");
                return f(&mut entry.index, &dir);
            }
        };
        reconcile(&mut index, &dir);
        map.insert(dir.clone(), CachedIndex { index, stamp });
    }
    let entry = map.get_mut(&dir).expect("index just loaded");
    f(&mut entry.index, &dir)
}

/// Re-stamp after a successful mutation wrote icons.json.
fn restamp(dir: &Path) {
    let mut map = CACHE.lock().unwrap_or_else(|e| e.into_inner());
    if let Some(c) = map.get_mut(dir) {
        c.stamp = index_stamp(&dir.join(INDEX_FILE));
    }
}

/// Drops the cached index for `dir` (clear removes the store).
fn evict(dir: &Path) {
    CACHE
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .remove(dir);
}

// ---- the store ops --------------------------------------------------------

/// Records `png` as the icon for `page_url` — keyed by host (dedup:
/// every page of a host shares one file, the latest write wins) and
/// the page url is tracked in the record.  Returns true when the
/// stored icon actually changed — a page re-reporting identical
/// bytes only refreshes its index row.
pub fn put_in(profile_dir: &Path, page_url: &str, png: &[u8]) -> RcResult<bool> {
    let key = key_for(page_url)?;
    if png.is_empty() || png.len() > MAX_PNG {
        return fail(RcStatus::InvalidArgument, "bad icon payload");
    }
    let now = now_ms();
    let mut wrote_file = false;
    with_index(profile_dir, |index, dir| {
        if index.entries.len() >= MAX_ENTRIES && !index.entries.contains_key(&key) {
            // Evict the stalest entry so a hostile page set can't grow
            // the cache without bound.
            if let Some((old_key, old_file)) = index
                .entries
                .iter()
                .min_by_key(|(_, e)| e.updated)
                .map(|(k, e)| (k.clone(), e.file.clone()))
            {
                index.entries.remove(&old_key);
                let _ = fs::remove_file(dir.join(old_file));
            }
        }
        let entry = index.entries.entry(key.clone()).or_insert_with(|| IconEntry {
            file: file_name_for(&key),
            updated: 0,
            pages: BTreeMap::new(),
        });
        let file_path = dir.join(&entry.file);
        let unchanged = fs::read(&file_path).map(|old| old == png).unwrap_or(false);
        if !unchanged {
            fs::create_dir_all(dir).map_err(|e| io_fail("icon dir", e))?;
            store::atomic_write(&file_path, png)?;
            wrote_file = true;
        }
        entry.updated = now;
        entry.pages.insert(page_url.to_string(), now);
        while entry.pages.len() > MAX_PAGES_PER_ICON {
            if let Some(oldest) = entry
                .pages
                .iter()
                .min_by_key(|(_, t)| *t)
                .map(|(u, _)| u.clone())
            {
                entry.pages.remove(&oldest);
            } else {
                break;
            }
        }
        persist(dir, index)?;
        Ok(())
    })?;
    restamp(&icons_dir(profile_dir));
    Ok(wrote_file)
}

/// The icon bytes for `page_url`'s key — RcStatus::NotFound on a
/// miss (the C++ side paints its default icon then).
pub fn get_in(profile_dir: &Path, page_url: &str) -> RcResult<Vec<u8>> {
    let key = key_for(page_url)?;
    with_index(profile_dir, |index, dir| {
        let entry = index.entries.get(&key).ok_or_else(|| Fail {
            status: RcStatus::NotFound,
            msg: "no icon for key".into(),
        })?;
        fs::read(dir.join(&entry.file)).map_err(|e| {
            if e.kind() == io::ErrorKind::NotFound {
                Fail {
                    status: RcStatus::NotFound,
                    msg: "icon file vanished".into(),
                }
            } else {
                io_fail("icon read", e)
            }
        })
    })
}

/// Drops one key's record and its file — absent is not an error.
/// Returns whether anything was removed.
pub fn remove_in(profile_dir: &Path, page_url: &str) -> RcResult<bool> {
    let key = key_for(page_url)?;
    let removed = with_index(profile_dir, |index, dir| {
        if let Some(entry) = index.entries.remove(&key) {
            let _ = fs::remove_file(dir.join(&entry.file));
            persist(dir, index)?;
            Ok(true)
        } else {
            Ok(false)
        }
    })?;
    if removed {
        restamp(&icons_dir(profile_dir));
    }
    Ok(removed)
}

/// Wipes the whole cache dir for the profile — the
/// QWebSettings::clearIconDatabase-equivalent.  The index cache is
/// evicted so the next op rebuilds empty (and re-checks the emptied
/// legacy sources, which a full clearIcons also removed).
pub fn clear_in(profile_dir: &Path) -> RcResult<()> {
    let dir = icons_dir(profile_dir);
    match fs::remove_dir_all(&dir) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(io_fail("icon clear", e)),
    }
    evict(&dir);
    Ok(())
}

/// The expiry sweep: drops entries not refreshed at-or-before
/// `cutoff_ms` and their files (the orphan half of the lifecycle is
/// already handled by the load-time reconcile).  Returns the count.
pub fn sweep_in(profile_dir: &Path, cutoff_ms: i64) -> RcResult<u64> {
    let removed = with_index(profile_dir, |index, dir| {
        let stale: Vec<(String, String)> = index
            .entries
            .iter()
            .filter(|(_, e)| e.updated <= cutoff_ms)
            .map(|(k, e)| (k.clone(), e.file.clone()))
            .collect();
        let n = stale.len() as u64;
        for (key, file) in stale {
            index.entries.remove(&key);
            let _ = fs::remove_file(dir.join(file));
        }
        if n > 0 {
            persist(dir, index)?;
        }
        Ok(n)
    })?;
    if removed > 0 {
        restamp(&icons_dir(profile_dir));
    }
    Ok(removed)
}

/// Record count — the cheap probe; -1 is impossible (no store state
/// to fail on), so this returns the entries length.
pub fn count_in(profile_dir: &Path) -> RcResult<i64> {
    with_index(profile_dir, |index, _| Ok(index.entries.len() as i64))
}

/// The index as a JSON array [{"key","file","updated","pages"}] —
/// the inspection lane (tests, a cache viewer).
pub fn list_in(profile_dir: &Path) -> RcResult<Vec<u8>> {
    with_index(profile_dir, |index, _| {
        let rows: Vec<Value> = index
            .entries
            .iter()
            .map(|(k, e)| {
                json!({
                    "key": k,
                    "file": e.file,
                    "updated": e.updated,
                    "pages": e.pages.keys().collect::<Vec<_>>(),
                })
            })
            .collect();
        Ok(serde_json::to_vec(&Value::Array(rows))
            .unwrap_or_else(|_| b"[]".to_vec()))
    })
}

// ---- anchored wrappers (the rc_* lane) --------------------------------------
//
// The process-global data dir anchors at the profile dir
// (rustCoreEnsureDataDirFor), so the legacy spelling lands on the
// same profiles/<id>/cache/icons the AcIcons handle binds — one
// store, two spellings.

fn anchored_dir() -> RcResult<PathBuf> {
    Ok(store::lock().dir()?.to_path_buf())
}

pub fn put(page_url: &str, png: &[u8]) -> RcResult<bool> {
    put_in(&anchored_dir()?, page_url, png)
}

pub fn get(page_url: &str) -> RcResult<Vec<u8>> {
    get_in(&anchored_dir()?, page_url)
}

pub fn remove(page_url: &str) -> RcResult<bool> {
    remove_in(&anchored_dir()?, page_url)
}

pub fn clear() -> RcResult<()> {
    clear_in(&anchored_dir()?)
}

pub fn sweep(cutoff_ms: i64) -> RcResult<u64> {
    sweep_in(&anchored_dir()?, cutoff_ms)
}

pub fn count() -> RcResult<i64> {
    count_in(&anchored_dir()?)
}

pub fn list() -> RcResult<Vec<u8>> {
    list_in(&anchored_dir()?)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-icons-{}-{}-{}",
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

    const PNG_A: &[u8] = b"\x89PNG-fake-a";
    const PNG_B: &[u8] = b"\x89PNG-fake-b";

    #[test]
    fn key_derivation_matches_historyiconkey() {
        assert_eq!(key_for("http://EXAMPLE.com/a").unwrap(), "example.com");
        assert_eq!(key_for("https://sub.example.com/x?y").unwrap(), "sub.example.com");
        assert_eq!(
            key_for("https://user@example.com:8443/p").unwrap(),
            "example.com"
        );
        // hostless urls keep the whole url as the key
        assert_eq!(key_for("data:image/png;base64,zz").unwrap(), "data:image/png;base64,zz");
        assert!(key_for("").is_err());
    }

    #[test]
    fn put_get_roundtrip_and_host_dedup() {
        let prof = tmp("dedup");
        assert!(put_in(&prof, "https://a.example/one", PNG_A).unwrap());
        // same host, different page: one file, shared icon
        let changed = put_in(&prof, "https://a.example/two", PNG_A).unwrap();
        assert!(!changed, "identical bytes only refresh the index");
        assert_eq!(get_in(&prof, "https://a.example/three").unwrap(), PNG_A);
        assert_eq!(count_in(&prof).unwrap(), 1);

        let files: Vec<_> = fs::read_dir(prof.join("cache/icons"))
            .unwrap()
            .flatten()
            .map(|e| e.file_name().to_string_lossy().into_owned())
            .collect();
        assert!(files.contains(&"icons.json".to_string()));
        assert!(files.contains(&"a.example.png".to_string()));
        assert_eq!(files.len(), 2);

        // both page urls recorded against the one host record
        let list = String::from_utf8(list_in(&prof).unwrap()).unwrap();
        assert!(list.contains("https://a.example/one"));
        assert!(list.contains("https://a.example/two"));

        // a changed icon rewrites the same file
        assert!(put_in(&prof, "https://a.example/new", PNG_B).unwrap());
        assert_eq!(get_in(&prof, "https://a.example/old").unwrap(), PNG_B);
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn entries_survive_index_reload() {
        let prof = tmp("reload");
        put_in(&prof, "https://persist.example/", PNG_A).unwrap();
        // force a reload by evicting the cache
        evict(&icons_dir(&prof));
        assert_eq!(get_in(&prof, "https://persist.example/x").unwrap(), PNG_A);
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn remove_and_clear() {
        let prof = tmp("rm");
        put_in(&prof, "https://gone.example/", PNG_A).unwrap();
        put_in(&prof, "https://kept.example/", PNG_B).unwrap();
        assert!(remove_in(&prof, "https://gone.example/").unwrap());
        assert!(!remove_in(&prof, "https://gone.example/").unwrap());
        assert!(get_in(&prof, "https://gone.example/").is_err());
        assert!(!prof.join("cache/icons/gone.example.png").exists());
        assert_eq!(count_in(&prof).unwrap(), 1);
        clear_in(&prof).unwrap();
        assert!(!prof.join("cache/icons").exists());
        assert_eq!(count_in(&prof).unwrap(), 0);
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn sweep_drops_aged_entries() {
        let prof = tmp("sweep");
        put_in(&prof, "https://old.example/", PNG_A).unwrap();
        put_in(&prof, "https://new.example/", PNG_B).unwrap();
        let now = now_ms();
        // age the old entry by hand-editing the index
        evict(&icons_dir(&prof));
        let index_path = prof.join("cache/icons/icons.json");
        let mut v: Value =
            serde_json::from_slice(&fs::read(&index_path).unwrap()).unwrap();
        v["entries"]["old.example"]["updated"] = json!(now - 10_000);
        fs::write(&index_path, serde_json::to_vec(&v).unwrap()).unwrap();

        assert_eq!(sweep_in(&prof, now - 5_000).unwrap(), 1);
        assert!(get_in(&prof, "https://old.example/").is_err());
        assert!(!prof.join("cache/icons/old.example.png").exists());
        assert_eq!(get_in(&prof, "https://new.example/").unwrap(), PNG_B);
        assert_eq!(sweep_in(&prof, now + 1).unwrap(), 1); // new.example too
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn reconcile_unlinks_orphans_and_drops_dead_rows() {
        let prof = tmp("recon");
        put_in(&prof, "https://live.example/", PNG_A).unwrap();
        put_in(&prof, "https://dead.example/", PNG_B).unwrap();
        // orphan file + a row pointing at a vanished file
        fs::write(prof.join("cache/icons/orphan.png"), b"x").unwrap();
        fs::remove_file(prof.join("cache/icons/dead.example.png")).unwrap();
        evict(&icons_dir(&prof));
        // a read triggers the reload + reconcile
        assert_eq!(get_in(&prof, "https://live.example/").unwrap(), PNG_A);
        assert!(get_in(&prof, "https://dead.example/").is_err());
        assert!(!prof.join("cache/icons/orphan.png").exists());
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn import_folds_legacy_dir_and_history_db_once() {
        let prof = tmp("import");
        // legacy no-rust store: <profile>/icons/<pct-host>.png
        let legacy = prof.join("icons");
        fs::create_dir_all(&legacy).unwrap();
        fs::write(legacy.join("legacy.example.png"), PNG_A).unwrap();
        fs::write(legacy.join("weird%20host.example.png"), PNG_B).unwrap();
        // HIST01 store: icons table inside <profile>/history.db
        let conn = rusqlite::Connection::open(prof.join("history.db")).unwrap();
        conn.execute_batch(
            "CREATE TABLE icons(host TEXT PRIMARY KEY, png BLOB, updated_at INTEGER);",
        )
        .unwrap();
        conn.execute(
            "INSERT INTO icons VALUES('db.example', ?1, 4242)",
            rusqlite::params![PNG_B],
        )
        .unwrap();
        drop(conn);

        // first touch folds both sources into the cache dir
        assert_eq!(get_in(&prof, "https://legacy.example/").unwrap(), PNG_A);
        assert_eq!(
            get_in(&prof, "https://weird host.example/").unwrap(),
            PNG_B
        );
        assert_eq!(get_in(&prof, "https://db.example/").unwrap(), PNG_B);
        assert!(prof.join("cache/icons/legacy.example.png").is_file());
        assert!(prof.join("cache/icons/db.example.png").is_file());
        // source files untouched (read-only import)
        assert!(legacy.join("legacy.example.png").is_file());
        // index persisted with the imported flag
        let idx = String::from_utf8(
            fs::read(prof.join("cache/icons/icons.json")).unwrap(),
        )
        .unwrap();
        assert!(idx.contains("\"imported\":true"));
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn cache_lives_under_cache_dir_never_data_root() {
        let prof = tmp("placement");
        put_in(&prof, "https://placed.example/", PNG_A).unwrap();
        assert!(prof.join("cache/icons/placed.example.png").is_file());
        assert!(!prof.join("icons").exists());
        let _ = fs::remove_dir_all(&prof);
    }

    #[test]
    fn hostile_inputs_rejected() {
        let prof = tmp("bounds");
        assert!(put_in(&prof, "", PNG_A).is_err());
        assert!(put_in(&prof, "https://h.example/", &[]).is_err());
        let huge = vec![0u8; MAX_PNG + 1];
        assert!(put_in(&prof, "https://h.example/", &huge).is_err());
        // a corrupt index reads as empty, never a hard error
        fs::create_dir_all(prof.join("cache/icons")).unwrap();
        fs::write(prof.join("cache/icons/icons.json"), b"{{{{").unwrap();
        evict(&icons_dir(&prof));
        assert_eq!(count_in(&prof).unwrap(), 0);
        let _ = fs::remove_dir_all(&prof);
    }
}
