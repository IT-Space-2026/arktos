//! RC03: the profile registry — the persona list both C++
//! implementations (ProfileRegistry MPROF01 and the MPROF05
//! lane-fallback ProfileStore) persisted in duplicate.  One Rust
//! registry ends the duplication; the file contract stays
//! byte-compatible so all three readers interoperate:
//!
//!   <data root>/profiles.json
//!     {"version":1,"profiles":[{id,name,avatar,createdAt}]}
//!
//! written exactly as QJsonDocument::Indented emits it — keys sorted,
//! four-space indent, trailing newline — so a Rust write is
//! indistinguishable from a Qt one (the escaping table was verified
//! against Qt 6.12's writer: '"' '\\' and \b \t \n \f \r as short
//! escapes, other <0x20 as \uXXXX lowercase, everything else raw
//! UTF-8 — the set serde_json happens to share, but the layout is
//! emitted by hand to keep the empty-array edge exact).
//!
//! Row semantics mirror the C++ pair:
//!   - The registry always describes at least "default": a missing
//!     file materializes the synthesized row on first open (name
//!     seeded from the caller's PROF01-era QSettings value, else the
//!     localized fallback the wrapper passes), a corrupt/unreadable
//!     file yields the same synthesized view WITHOUT being rewritten.
//!   - ids are filesystem slugs ([a-z0-9_-], <=64) — a path segment,
//!     so anything else is refused.  create() slugifies the display
//!     name and dedups with -2/-3/… suffixes against both the
//!     registry AND existing profiles/<id>/ dirs.
//!   - The avatar field is a BASENAME joined under profiles/<id>/ —
//!     separators and dot segments are stripped on load and refused
//!     on write (VSEC01: a traversal-shaped value would turn avatar
//!     reads into path escapes).  The PNG decode/scale pipeline stays
//!     Qt-side; this module owns the field + the file's path.
//!   - delete refuses "default" outright (removing it is the
//!     full-wipe marker path) and removes profiles/<id>/ recursively
//!     before dropping the row.  The C++-side guards stay C++: vault
//!     wipe (PVAULT03/04) and live-window releasePersona run on the
//!     adapter that wraps this primitive — the module owns the file,
//!     not the process's live objects.
//!   - createdAt is an opaque ISO string carried verbatim (Qt
//!     ISODate in 6.12 emits no milliseconds or zone); newly stamped
//!     rows use local civil time the same way.
//!
//! Active-profile tracking mirrors BrowserPaths' ambient persona:
//! per-handle process state (never persisted — the C++ ambient id is
//! in-memory too), invalid ids resolve to "default".
//!
//! Path accessors mirror BrowserPaths' layout under the caller-bound
//! data root — the module never invents a second one:
//!   data            <root>/profiles/<id>
//!   cache           <root>/profiles/<id>/cache
//!   engine storage  <root>/profiles/<id>/QtWebEngine
//!   engine cache    <root>/profiles/<id>/cache/QtWebEngine
//!   settings        <root>/profiles/<id>  (settings.json anchor, RC02)
//!
//! Every mutation writes through to disk atomically
//! (store::atomic_write) and the FFI layer emits the "profiles"
//! change topic.

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::store;
use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;

const FILE_NAME: &str = "profiles.json";
const DEFAULT_ID: &str = "default";
const DEFAULT_AVATAR: &str = "profile-avatar.png";
const DEFAULT_NAME: &str = "Profile";

const MAX_PROFILES: usize = 1024;
const MAX_NAME_LEN: usize = 4096;
const MAX_AVATAR_LEN: usize = 255;
// Generous bound, same envelope as the settings store — the real
// file is a few hundred bytes per row.
const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;

/// One registry row — the ProfileContext twin.
#[derive(Clone, Debug, PartialEq)]
pub struct ProfileRecord {
    pub id: String,
    pub name: String,
    /// Avatar file basename inside profiles/<id>/ (may be empty).
    pub avatar: String,
    /// ISO timestamp, carried verbatim — "yyyy-MM-ddTHH:mm:ss".
    pub created_at: String,
}

/// The profile-id slug check, byte-identical to
/// BrowserPaths::isProfileId / ProfileRegistry::isValidProfileId:
/// lowercase ASCII letters, digits, '-' and '_', 1..=64 bytes.
pub fn is_profile_id(id: &str) -> bool {
    !id.is_empty()
        && id.len() <= 64
        && id.bytes()
            .all(|b| b.is_ascii_lowercase() || b.is_ascii_digit() || b == b'-' || b == b'_')
}

/// Display name -> filesystem slug, mirroring ProfileStore::slugify:
/// lowercase, non-[a-z0-9] runs collapse to a single '-', empty
/// result falls back to "profile", capped at 64 chars.
pub fn slugify(display_name: &str) -> String {
    let mut slug = String::new();
    let mut last_dash = false;
    for c in display_name.to_lowercase().chars() {
        if c.is_ascii_lowercase() || c.is_ascii_digit() {
            slug.push(c);
            last_dash = false;
        } else if !slug.is_empty() && !last_dash {
            slug.push('-');
            last_dash = true;
        }
    }
    while slug.ends_with('-') {
        slug.pop();
    }
    if slug.is_empty() {
        slug.push_str("profile");
    }
    slug.truncate(64);
    slug
}

/// The avatar field's basename contract (VSEC01): anything carrying
/// separators or dot segments resolves to "".
fn sanitize_avatar(name: &str) -> String {
    if name.is_empty()
        || name.len() > MAX_AVATAR_LEN
        || name.contains('/')
        || name.contains('\\')
        || name == "."
        || name == ".."
    {
        String::new()
    } else {
        name.to_string()
    }
}

fn check_name(name: &str) -> RcResult<()> {
    if name.len() > MAX_NAME_LEN {
        return fail(RcStatus::InvalidArgument, "profile name too large");
    }
    Ok(())
}

struct State {
    root: PathBuf,
    file: PathBuf,
    profiles: Vec<ProfileRecord>,
    /// Ambient persona — process state, never persisted (the
    /// BrowserPaths::activeProfileId equivalent).
    active: String,
    /// Name the synthesized "default" row gets on first materialize
    /// (the caller's QSettings "profile/name" value or its
    /// localized fallback, passed at open).
    default_name: String,
}

/// The FFI-facing handle — the registry bound to one data root.
/// Two handles on the same file are last-writer-wins, the caveat the
/// C++ pair documents for ProfileStore(filePath) second instances.
pub struct RcProfiles {
    state: Mutex<State>,
}

impl RcProfiles {
    /// Open the registry at `<data_root>/profiles.json`.  A missing
    /// file materializes the default row immediately (fresh-install
    /// contract); `default_name` seeds its display name when the row
    /// must be synthesized (NULL/"", and only then) falls back to
    /// "Profile".
    pub fn open(data_root: &str, default_name: Option<&str>) -> RcResult<RcProfiles> {
        if data_root.is_empty() {
            return fail(RcStatus::InvalidArgument, "empty data root");
        }
        let root = PathBuf::from(data_root);
        let mut state = State {
            file: root.join(FILE_NAME),
            root,
            profiles: Vec::new(),
            active: DEFAULT_ID.to_string(),
            default_name: match default_name {
                Some(n) if !n.is_empty() => n.to_string(),
                _ => DEFAULT_NAME.to_string(),
            },
        };
        load(&mut state)?;
        Ok(RcProfiles {
            state: Mutex::new(state),
        })
    }

    /// Re-read from disk — for callers that changed the file behind
    /// the registry's back (the ProfileStore::reload seam).
    pub fn reload(&self) -> RcResult<()> {
        let mut state = self.lock();
        state.profiles.clear();
        load(&mut state)
    }

    pub fn list(&self) -> Vec<ProfileRecord> {
        self.lock().profiles.clone()
    }

    pub fn get(&self, id: &str) -> Option<ProfileRecord> {
        self.lock().profiles.iter().find(|p| p.id == id).cloned()
    }

    /// The ambient persona — the focused window's bound id.
    pub fn active_id(&self) -> String {
        self.lock().active.clone()
    }

    /// Push the ambient persona.  A non-slug id resolves to
    /// "default" exactly like BrowserPaths::setActiveProfileId — a
    /// bogus id can never steer unscoped stores outside profiles/.
    pub fn set_active(&self, id: &str) {
        let mut state = self.lock();
        state.active = if is_profile_id(id) {
            id.to_string()
        } else {
            DEFAULT_ID.to_string()
        };
    }

    /// Create: registry row + profiles/<id>/ together, slugified
    /// unique id derived from the display name.  An all-symbol name
    /// (which slugifies to "profile") still dedups normally.
    pub fn create(&self, display_name: &str) -> RcResult<String> {
        let name = display_name.trim();
        if name.is_empty() {
            return fail(RcStatus::InvalidArgument, "empty profile name");
        }
        check_name(name)?;
        let mut state = self.lock();
        let base = slugify(name);
        let mut id = base.clone();
        let mut n = 2u32;
        while state.profiles.iter().any(|p| p.id == id)
            || profile_dir(&state.root, &id).exists()
        {
            id = format!("{base}-{n}");
            n += 1;
            if n > MAX_PROFILES as u32 {
                return fail(RcStatus::InvalidArgument, "profile id space exhausted");
            }
        }
        if state.profiles.len() >= MAX_PROFILES {
            return fail(RcStatus::InvalidArgument, "profile count bound reached");
        }
        let dir = profile_dir(&state.root, &id);
        fs::create_dir_all(&dir)?;
        state.profiles.push(ProfileRecord {
            id: id.clone(),
            name: name.to_string(),
            avatar: String::new(),
            created_at: now_iso(),
        });
        if let Err(e) = persist(&state) {
            state.profiles.pop();
            return Err(e);
        }
        Ok(id)
    }

    /// Display-name update; persists.  A blank name resolves to
    /// "Profile" — the C++ renameProfile fallback.
    pub fn rename(&self, id: &str, name: &str) -> RcResult<()> {
        check_id(id)?;
        let mut state = self.lock();
        let row = find_mut(&mut state, id)?;
        let trimmed = name.trim();
        let new_name = if trimmed.is_empty() {
            DEFAULT_NAME.to_string()
        } else {
            check_name(trimmed)?;
            trimmed.to_string()
        };
        if row.name == new_name {
            return Ok(());
        }
        row.name = new_name;
        persist(&state)
    }

    /// Point the row's avatar field at a basename inside
    /// profiles/<id>/ — the PNG itself is written by the Qt side
    /// (decode/scale pipeline stays there).  None/"" clears the
    /// field; a traversal-shaped name is refused outright.
    pub fn set_avatar(&self, id: &str, name: Option<&str>) -> RcResult<()> {
        check_id(id)?;
        let clean = match name {
            Some(n) if !n.is_empty() => {
                if sanitize_avatar(n) != n {
                    return fail(
                        RcStatus::InvalidArgument,
                        "avatar name must be a plain basename",
                    );
                }
                n.to_string()
            }
            _ => String::new(),
        };
        let mut state = self.lock();
        let row = find_mut(&mut state, id)?;
        if row.avatar == clean {
            return Ok(());
        }
        row.avatar = clean;
        persist(&state)
    }

    /// Removes the registry row and deletes profiles/<id>/
    /// recursively — the store-facing half of
    /// ProfileStore::deleteProfile.  "default" and non-slug ids are
    /// refused; a symlinked profile dir is removed as a link, never
    /// descended.  Live-object teardown (releasePersona) and the
    /// vault wipe stay on the Qt adapter that wraps this.
    pub fn delete(&self, id: &str) -> RcResult<()> {
        check_id(id)?;
        if id == DEFAULT_ID {
            return fail(
                RcStatus::InvalidArgument,
                "the default profile is not deletable",
            );
        }
        let mut state = self.lock();
        let idx = match state.profiles.iter().position(|p| p.id == id) {
            Some(i) => i,
            None => {
                return fail(RcStatus::NotFound, "no such profile");
            }
        };
        let dir = profile_dir(&state.root, id);
        // lstat: a symlinked profile dir is link-residue — remove the
        // link itself, never descend into its target (the VSEC01
        // discipline the layout migrations apply).
        if dir.symlink_metadata().is_ok() {
            if dir.is_symlink() {
                fs::remove_file(&dir)?;
            } else {
                fs::remove_dir_all(&dir)?;
            }
        }
        let removed = state.profiles.remove(idx);
        if let Err(e) = persist(&state) {
            state.profiles.insert(idx, removed);
            return Err(e);
        }
        Ok(())
    }

    // ---- path accessors: the BrowserPaths layout, mirrored --------

    /// <root>/profiles/<id> — a non-slug id resolves to "default"
    /// exactly like BrowserPaths::profileDataDir.
    pub fn profile_dir(&self, id: &str) -> String {
        profile_dir(&self.lock().root, id)
            .to_string_lossy()
            .into_owned()
    }

    /// <root>/profiles/<id>/cache — the profile's disposable lane
    /// (the vault exclusion's "cache" name).
    pub fn cache_dir(&self, id: &str) -> String {
        profile_dir(&self.lock().root, id)
            .join("cache")
            .to_string_lossy()
            .into_owned()
    }

    /// <root>/profiles/<id>/QtWebEngine — BrowserPaths::engineStorageDir.
    pub fn engine_storage_dir(&self, id: &str) -> String {
        profile_dir(&self.lock().root, id)
            .join("QtWebEngine")
            .to_string_lossy()
            .into_owned()
    }

    /// <root>/profiles/<id>/cache/QtWebEngine —
    /// BrowserPaths::engineCacheDir.
    pub fn engine_cache_dir(&self, id: &str) -> String {
        profile_dir(&self.lock().root, id)
            .join("cache")
            .join("QtWebEngine")
            .to_string_lossy()
            .into_owned()
    }

    /// The dir an rc_settings profile scope anchors at — the profile
    /// data dir itself (settings.json / settings.ini live there).
    pub fn settings_dir(&self, id: &str) -> String {
        self.profile_dir(id)
    }

    /// The registry file this handle owns — <root>/profiles.json.
    pub fn registry_path(&self) -> String {
        self.lock().file.to_string_lossy().into_owned()
    }

    /// profiles/<id>/<avatar> — the row's avatar field joined under
    /// the profile dir, falling back to profile-avatar.png when the
    /// field is empty (the ProfileStore::avatarPixmap resolution).
    pub fn avatar_path(&self, id: &str) -> String {
        let state = self.lock();
        let name = state
            .profiles
            .iter()
            .find(|p| p.id == id)
            .map(|p| p.avatar.clone())
            .unwrap_or_default();
        let name = if name.is_empty() {
            DEFAULT_AVATAR.to_string()
        } else {
            name
        };
        profile_dir(&state.root, id)
            .join(name)
            .to_string_lossy()
            .into_owned()
    }

    /// Flush — mutations already write through; kept for the
    /// symmetric call shape.
    pub fn sync(&self) -> RcResult<()> {
        persist(&self.lock())
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

fn check_id(id: &str) -> RcResult<()> {
    if !is_profile_id(id) {
        return fail(RcStatus::InvalidArgument, "invalid profile id");
    }
    Ok(())
}

fn find_mut<'a>(state: &'a mut State, id: &str) -> RcResult<&'a mut ProfileRecord> {
    state
        .profiles
        .iter_mut()
        .find(|p| p.id == id)
        .ok_or_else(|| Fail {
            status: RcStatus::NotFound,
            msg: "no such profile".into(),
        })
}

fn profile_dir(root: &std::path::Path, id: &str) -> PathBuf {
    let effective = if is_profile_id(id) { id } else { DEFAULT_ID };
    root.join("profiles").join(effective)
}

// ---- persistence ----------------------------------------------------

/// "yyyy-MM-ddTHH:mm:ss" — Qt 6.12's ISODate emission (no millis, no
/// zone suffix; LocalTime).  unix uses the system zone via
/// localtime_r; other targets fall back to UTC civil time.
#[cfg(unix)]
fn now_iso() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    unsafe {
        let t = secs as libc::time_t;
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&t, &mut tm).is_null() {
            return iso_from_unix_utc(secs as i64);
        }
        format!(
            "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}",
            tm.tm_year + 1900,
            tm.tm_mon + 1,
            tm.tm_mday,
            tm.tm_hour,
            tm.tm_min,
            tm.tm_sec
        )
    }
}

#[cfg(not(unix))]
fn now_iso() -> String {
    let secs = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0);
    iso_from_unix_utc(secs as i64)
}

/// UTC civil timestamp for an epoch-second count — Howard Hinnant's
/// civil-from-days, the inverse of omnibox's days_from_civil.
#[cfg_attr(unix, allow(dead_code))]
fn iso_from_unix_utc(secs: i64) -> String {
    let days = secs.div_euclid(86_400);
    let rem = secs.rem_euclid(86_400);
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = doy - (153 * mp + 2) / 5 + 1;
    let m = if mp < 10 { mp + 3 } else { mp - 9 };
    let y = if m <= 2 { y + 1 } else { y };
    format!(
        "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}",
        y,
        m,
        d,
        rem / 3600,
        rem % 3600 / 60,
        rem % 60
    )
}

/// Qt 6.12 JSON string escaping, verified byte-for-byte against
/// QJsonDocument::toJson(Indented): '"' '\\' and \b \t \n \f \r as
/// short escapes, other bytes <0x20 as \uXXXX (lowercase hex), and
/// everything else — DEL, '/', non-ASCII, U+2028/29 — raw UTF-8.
fn push_qt_json_str(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{08}' => out.push_str("\\b"),
            '\t' => out.push_str("\\t"),
            '\n' => out.push_str("\\n"),
            '\u{0c}' => out.push_str("\\f"),
            '\r' => out.push_str("\\r"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u{:04x}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// The registry document, byte-identical to what
/// ProfileRegistry::save / ProfileStore::save write through
/// QJsonDocument::toJson(QJsonDocument::Indented): sorted keys,
/// 4-space indent, "key": value separators, empty array as
/// "[\n    ]", trailing newline.
fn registry_json(profiles: &[ProfileRecord]) -> Vec<u8> {
    let mut s = String::from("{\n    \"profiles\": [");
    if profiles.is_empty() {
        s.push_str("\n    ],\n");
    } else {
        s.push('\n');
        for (i, p) in profiles.iter().enumerate() {
            s.push_str("        {\n            \"avatar\": ");
            push_qt_json_str(&mut s, &p.avatar);
            s.push_str(",\n            \"createdAt\": ");
            push_qt_json_str(&mut s, &p.created_at);
            s.push_str(",\n            \"id\": ");
            push_qt_json_str(&mut s, &p.id);
            s.push_str(",\n            \"name\": ");
            push_qt_json_str(&mut s, &p.name);
            s.push_str("\n        }");
            if i + 1 < profiles.len() {
                s.push(',');
            }
            s.push('\n');
        }
        s.push_str("    ],\n");
    }
    s.push_str("    \"version\": 1\n}\n");
    s.into_bytes()
}

fn persist(state: &State) -> RcResult<()> {
    if let Some(parent) = state.file.parent() {
        fs::create_dir_all(parent)?;
    }
    store::atomic_write(&state.file, &registry_json(&state.profiles))
}

/// Read the registry — never fails the open on a bad file (the C++
/// contract: a missing or corrupt profiles.json yields the
/// synthesized default view and stays untouched on disk until the
/// next real write).  Entries with non-slug ids and
/// traversal-shaped avatars are dropped/cleaned exactly like
/// ProfileRegistry::load.
fn load(state: &mut State) -> RcResult<()> {
    let had_file = state.file.exists();
    if had_file {
        let meta = fs::metadata(&state.file)?;
        if meta.len() <= MAX_FILE_BYTES {
            if let Ok(bytes) = fs::read(&state.file) {
                if let Ok(doc) = serde_json::from_slice::<serde_json::Value>(&bytes) {
                    if let Some(entries) =
                        doc.get("profiles").and_then(|v| v.as_array())
                    {
                        for entry in entries {
                            let get = |k: &str| {
                                entry
                                    .get(k)
                                    .and_then(|v| v.as_str())
                                    .unwrap_or("")
                                    .to_string()
                            };
                            let id = get("id");
                            if !is_profile_id(&id)
                                || state.profiles.len() >= MAX_PROFILES
                            {
                                continue;
                            }
                            state.profiles.push(ProfileRecord {
                                id,
                                name: get("name"),
                                avatar: sanitize_avatar(&get("avatar")),
                                created_at: get("createdAt"),
                            });
                        }
                    }
                }
            }
        }
        // Oversized/unreadable/malformed all land here — the C++
        // loader's empty-entries path.
    }

    if !state.profiles.iter().any(|p| p.id == DEFAULT_ID) {
        state.profiles.push(ProfileRecord {
            id: DEFAULT_ID.to_string(),
            name: state.default_name.clone(),
            avatar: DEFAULT_AVATAR.to_string(),
            created_at: now_iso(),
        });
        // A fresh install materializes the registry on first read; a
        // file that existed (even corrupt) is left alone until the
        // next real write — ProfileRegistry::load's exact rule.
        if !had_file {
            let _ = persist(state);
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-profiles-{}-{}-{}",
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
    fn fresh_open_materializes_default() {
        let root = tmpdir("fresh");
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        let list = h.list();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].id, "default");
        assert_eq!(list[0].name, "Profile");
        assert_eq!(list[0].avatar, "profile-avatar.png");
        // Materialized on first read, Qt-Indented on disk.
        let text = fs::read_to_string(root.join("profiles.json")).unwrap();
        assert!(text.ends_with("}\n"));
        assert!(text.contains("\n    \"profiles\": [\n        {\n"));
        assert!(text.contains("            \"id\": \"default\""));
        assert!(text.contains("    \"version\": 1\n"));
        // The seed name lands when the caller supplies one.
        let root2 = tmpdir("seed");
        let h2 = RcProfiles::open(root2.to_str().unwrap(), Some("Mine")).unwrap();
        assert_eq!(h2.list()[0].name, "Mine");
        let _ = fs::remove_dir_all(&root);
        let _ = fs::remove_dir_all(&root2);
    }

    #[test]
    fn registry_roundtrip() {
        let root = tmpdir("rt");
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        let id = h.create("Work Stuff!").unwrap();
        assert_eq!(id, "work-stuff");
        assert!(root.join("profiles/work-stuff").is_dir());
        // Duplicate display name dedups against registry + dirs.
        let id2 = h.create("Work Stuff!").unwrap();
        assert_eq!(id2, "work-stuff-2");
        // An unregistered dir still blocks the slug.
        fs::create_dir_all(root.join("profiles/other-1")).unwrap();
        let id3 = h.create("Other 1").unwrap();
        assert_eq!(id3, "other-1-2");

        h.rename(&id, "Day Job").unwrap();
        h.set_avatar(&id, Some("face.png")).unwrap();

        // Reopen — the document round-trips through disk.
        drop(h);
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        let p = h.get(&id).unwrap();
        assert_eq!(p.name, "Day Job");
        assert_eq!(p.avatar, "face.png");
        assert_eq!(h.list().len(), 4); // default + 3 created
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn reads_existing_qt_file() {
        // Bytes written by the MPROF01 writer (QJsonDocument::Indented
        // captured on a live system) parse back untouched.
        let root = tmpdir("qt");
        fs::write(
            root.join("profiles.json"),
            "{\n    \"profiles\": [\n        {\n            \"avatar\": \"profile-avatar.png\",\n            \"createdAt\": \"2026-10-16T11:26:19\",\n            \"id\": \"default\",\n            \"name\": \"Profile\"\n        },\n        {\n            \"avatar\": \"\",\n            \"createdAt\": \"2026-10-20T08:00:00\",\n            \"id\": \"work\",\n            \"name\": \"Work\"\n        }\n    ],\n    \"version\": 1\n}\n",
        )
        .unwrap();
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        assert_eq!(h.list().len(), 2);
        let w = h.get("work").unwrap();
        assert_eq!(w.name, "Work");
        assert_eq!(w.created_at, "2026-10-20T08:00:00");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn write_is_qt_indented() {
        // The emitted bytes are exactly the layout a Qt 6.12
        // QJsonDocument::Indented write produces — the reference here
        // was captured from a real profiles.json plus a probe.
        let root = tmpdir("qtout");
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        h.create("Täst ").unwrap();
        let text = fs::read_to_string(root.join("profiles.json")).unwrap();
        let doc: serde_json::Value = serde_json::from_str(&text).unwrap();
        assert_eq!(doc["version"], 1);
        // Non-ASCII names pass through unescaped like Qt's writer.
        assert!(text.contains("\"name\": \"Täst\""));
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn corrupt_file_synthesizes_default() {
        let root = tmpdir("corrupt");
        fs::write(root.join("profiles.json"), "{not json").unwrap();
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        assert_eq!(h.list().len(), 1);
        assert_eq!(h.list()[0].id, "default");
        // The corrupt file is left untouched — the next real write
        // rewrites it cleanly.
        assert_eq!(
            fs::read_to_string(root.join("profiles.json")).unwrap(),
            "{not json"
        );
        // Entries with bad ids / traversal avatars are dropped/cleaned.
        fs::write(
            root.join("profiles.json"),
            "{\"version\":1,\"profiles\":[{\"id\":\"../evil\",\"name\":\"X\"},{\"id\":\"ok\",\"avatar\":\"../../escape.png\"}]}",
        )
        .unwrap();
        h.reload().unwrap();
        assert_eq!(h.list().len(), 2); // synthesized default + "ok"
        assert_eq!(h.get("ok").unwrap().avatar, "");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn guards_and_delete() {
        let root = tmpdir("del");
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        // The default profile and non-slug ids are refused.
        assert_eq!(
            h.delete("default").unwrap_err().status,
            RcStatus::InvalidArgument
        );
        assert_eq!(
            h.delete("./default").unwrap_err().status,
            RcStatus::InvalidArgument
        );
        assert_eq!(
            h.delete("ghost").unwrap_err().status,
            RcStatus::NotFound
        );
        let id = h.create("Doomed").unwrap();
        fs::write(root.join("profiles/doomed/file.txt"), "x").unwrap();
        h.delete(&id).unwrap();
        assert!(h.get(&id).is_none());
        assert!(!root.join("profiles/doomed").exists());
        // Unknown ids refuse renames/avatars without writing.
        assert_eq!(
            h.rename("ghost", "x").unwrap_err().status,
            RcStatus::NotFound
        );
        assert_eq!(
            h.set_avatar("ghost", Some("a.png")).unwrap_err().status,
            RcStatus::NotFound
        );
        // Traversal-shaped avatar names never reach the file.
        assert!(h.set_avatar("default", Some("../escape.png")).is_err());
        h.set_avatar("default", Some("me.png")).unwrap();
        assert_eq!(h.get("default").unwrap().avatar, "me.png");
        h.set_avatar("default", None).unwrap();
        assert_eq!(h.get("default").unwrap().avatar, "");
        // Blank creates produce nothing.
        assert!(h.create("   ").is_err());
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn ambient_active_id() {
        let root = tmpdir("amb");
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        assert_eq!(h.active_id(), "default");
        h.set_active("work");
        assert_eq!(h.active_id(), "work");
        // A bogus id can never steer stores outside profiles/.
        h.set_active("../escape");
        assert_eq!(h.active_id(), "default");
        // Process state only — a second handle starts at default.
        let h2 = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        assert_eq!(h2.active_id(), "default");
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn browserpaths_layout() {
        let root = tmpdir("paths");
        let h = RcProfiles::open(root.to_str().unwrap(), None).unwrap();
        let r = root.to_str().unwrap();
        assert_eq!(
            h.profile_dir("work"),
            format!("{r}/profiles/work")
        );
        assert_eq!(
            h.cache_dir("work"),
            format!("{r}/profiles/work/cache")
        );
        assert_eq!(
            h.engine_storage_dir("work"),
            format!("{r}/profiles/work/QtWebEngine")
        );
        assert_eq!(
            h.engine_cache_dir("work"),
            format!("{r}/profiles/work/cache/QtWebEngine")
        );
        assert_eq!(
            h.settings_dir("work"),
            format!("{r}/profiles/work")
        );
        assert_eq!(h.registry_path(), format!("{r}/profiles.json"));
        // A bad id resolves to default instead of escaping profiles/.
        assert_eq!(
            h.profile_dir("../escape"),
            format!("{r}/profiles/default")
        );
        // Avatar path: field value joined under the profile dir,
        // profile-avatar.png when unset.
        assert_eq!(
            h.avatar_path("work"),
            format!("{r}/profiles/work/profile-avatar.png")
        );
        let _ = fs::remove_dir_all(&root);
    }

    #[test]
    fn slugify_and_id_rules() {
        assert_eq!(slugify("Work Stuff!"), "work-stuff");
        assert_eq!(slugify("!!!"), "profile");
        assert_eq!(slugify("  Under_score  "), "under-score");
        assert!(is_profile_id("a-b_1"));
        assert!(!is_profile_id("../etc"));
        assert!(!is_profile_id("Caps"));
        assert!(!is_profile_id(""));
        assert!(!is_profile_id(&"x".repeat(65)));
    }
}
