//! RC21: the per-profile side-panel notes store — the ordered
//! multi-note list NotesModel persists as
//! `<data>/profiles/<id>/notes.json`.
//!
//! Audit call (per the RC21 spec): the store is already a plain file
//! under the profile dir, so this is a read-compat module — the
//! on-disk format is the C++ writer's byte shape and the list
//! semantics are the model's:
//!
//!   * File: a COMPACT JSON array of
//!     {"body":..,"markdown":bool,"title":..} objects — the
//!     QJsonObject sorted-key emission order, which the
//!     BTreeMap-backed serde_json::Map reproduces verbatim.
//!   * Order: array order IS the list order — insert(row, …) lands
//!     at the clamped position, remove(row, count) takes a span,
//!     every read is by row index.
//!   * Load: a missing/unreadable file is an empty store (never an
//!     error); a document that is not a JSON array loads empty; a
//!     non-object element is a BLANK note (QJsonValue::toObject()
//!     yields {}); wrong-typed fields default (title/body "" unless
//!     a string, markdown false unless a bool) — QJsonValue::
//!     toString()/toBool() semantics, verified against Qt 6.12.
//!   * Persistence: mutations update the decoded list and mark the
//!     store dirty; sync() (and close — the model destructor's
//!     saveIfNeccessary equivalent) commits via temp+rename+fsync,
//!     creating the profile dir when needed.  This preserves the
//!     C++ AutoSaver debounce contract across the FFI boundary —
//!     the model's per-keystroke edits do not become per-keystroke
//!     writes.
//!
//! Binding: rc_notes_open(root, profile_id) binds
//! profiles/<id>/notes.json — the facade's per-persona lane.
//! A NULL/empty profile id mints a VOLATILE store: the off-the-
//! record contract NotesModel documents for private/tor windows —
//! CRUD works, sync() is a no-op, nothing ever reaches disk.
//! rc_notes_open_file(path) binds an explicit file — the
//! standalone-store spelling NotesModel(storageFile) uses (tests,
//! per-path callers); NULL/empty is volatile again.
//!
//! Mutations emit the "notes" change topic; facade callers get the
//! bound profile stamped.

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::profiles;
use crate::store;
use std::fs;
use std::path::PathBuf;
use std::sync::Mutex;

const FILE_NAME: &str = "notes.json";

/// Generous ceilings — the C++ store is unbounded, so these only
/// refuse what no real sidebar can produce.  A file over
/// MAX_FILE_BYTES reads as Corrupt; a file with more than
/// MAX_NOTES still loads (a cap on READ could strand real notes on
/// the next save) — the bound only gates new inserts.
const MAX_FILE_BYTES: u64 = 64 * 1024 * 1024;
const MAX_NOTES: usize = 65536;
const MAX_TITLE_LEN: usize = 1024 * 1024;
const MAX_BODY_LEN: usize = 16 * 1024 * 1024;

/// One note — the C++ NotesModel::Note field set verbatim.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct Note {
    pub title: String,
    pub body: String,
    pub markdown: bool,
}

struct State {
    /// None on a volatile (off-the-record) store — CRUD in memory,
    /// sync() a no-op.
    file: Option<PathBuf>,
    /// The persona a root+profile open bound — stamped on "notes"
    /// change topics by the facade; file-bound handles carry None
    /// (a bare path does not name a persona).
    profile: Option<String>,
    notes: Vec<Note>,
    dirty: bool,
}

/// The FFI-facing handle — one decoded list per open, mutex-
/// serialized like RcSettings; two handles on the same file are
/// last-writer-wins (the shared-conf caveat the C++ model's
/// one-instance-per-persona registry already avoids).
pub struct RcNotes {
    state: Mutex<State>,
}

impl RcNotes {
    /// Open the persona store: a profile slug binds
    /// `<data_root>/profiles/<id>/notes.json`; NULL/empty (the
    /// Option) mints a volatile off-the-record store.
    pub fn open(data_root: &str, profile_id: Option<&str>) -> RcResult<RcNotes> {
        match profile_id {
            Some(id) if !id.is_empty() => {
                if data_root.is_empty() {
                    return fail(RcStatus::InvalidArgument, "empty data root");
                }
                if !profiles::is_profile_id(id) {
                    return fail(RcStatus::InvalidArgument, "invalid profile id");
                }
                let file = PathBuf::from(data_root)
                    .join("profiles")
                    .join(id)
                    .join(FILE_NAME);
                Self::open_bound(file, Some(id.to_string()))
            }
            _ => Ok(Self::volatile()),
        }
    }

    /// Open the store bound to an explicit file — the standalone
    /// NotesModel(storageFile) lane.  An empty path is volatile.
    pub fn open_file(path: &str) -> RcResult<RcNotes> {
        if path.is_empty() {
            return Ok(Self::volatile());
        }
        Self::open_bound(PathBuf::from(path), None)
    }

    fn volatile() -> RcNotes {
        RcNotes {
            state: Mutex::new(State {
                file: None,
                profile: None,
                notes: Vec::new(),
                dirty: false,
            }),
        }
    }

    fn open_bound(file: PathBuf, profile: Option<String>) -> RcResult<RcNotes> {
        let notes = load_file(&file)?;
        Ok(RcNotes {
            state: Mutex::new(State {
                file: Some(file),
                profile,
                notes,
                dirty: false,
            }),
        })
    }

    pub fn count(&self) -> i64 {
        self.lock().notes.len() as i64
    }

    /// The bound persona id for the facade's change-stamp — None on
    /// volatile and file-bound handles.
    pub fn profile_id(&self) -> Option<String> {
        self.lock().profile.clone()
    }

    pub fn is_persistent(&self) -> bool {
        self.lock().file.is_some()
    }

    pub fn is_dirty(&self) -> bool {
        self.lock().dirty
    }

    /// One note by row — RC_NOT_FOUND out of range.
    pub fn get(&self, row: i64) -> RcResult<Note> {
        let state = self.lock();
        state
            .notes
            .get(usize::try_from(row).unwrap_or(usize::MAX))
            .cloned()
            .ok_or_else(|| Fail {
                status: RcStatus::NotFound,
                msg: "no such note".into(),
            })
    }

    /// The whole list — the persisted order.
    pub fn list(&self) -> Vec<Note> {
        self.lock().notes.clone()
    }

    /// insertNote — lands at the clamped row (qBound semantics:
    /// out-of-range positions append/prepend, never fail), markdown
    /// flag cleared, the new row returned.
    pub fn insert(&self, row: i64, title: &str, body: &str) -> RcResult<i64> {
        check_field(title, "title")?;
        check_field(body, "body")?;
        let mut state = self.lock();
        if state.notes.len() >= MAX_NOTES {
            return fail(RcStatus::InvalidArgument, "notes store bound reached");
        }
        let row = row.clamp(0, state.notes.len() as i64) as usize;
        state.notes.insert(
            row,
            Note {
                title: title.to_string(),
                body: body.to_string(),
                markdown: false,
            },
        );
        state.dirty = true;
        Ok(row as i64)
    }

    /// Qt::EditRole — rename.  Ok(false) on an identical value, like
    /// setData returning false: no dirty, no notification.
    pub fn set_title(&self, row: i64, value: &str) -> RcResult<bool> {
        check_field(value, "title")?;
        let mut state = self.lock();
        let note = note_mut(&mut state, row)?;
        if note.title == value {
            return Ok(false);
        }
        note.title = value.to_string();
        state.dirty = true;
        Ok(true)
    }

    /// BodyRole — the editor's committed text.
    pub fn set_body(&self, row: i64, value: &str) -> RcResult<bool> {
        check_field(value, "body")?;
        let mut state = self.lock();
        let note = note_mut(&mut state, row)?;
        if note.body == value {
            return Ok(false);
        }
        note.body = value.to_string();
        state.dirty = true;
        Ok(true)
    }

    /// MarkdownRole — the per-note render flag (NOTE03).
    pub fn set_markdown(&self, row: i64, on: bool) -> RcResult<bool> {
        let mut state = self.lock();
        let note = note_mut(&mut state, row)?;
        if note.markdown == on {
            return Ok(false);
        }
        note.markdown = on;
        state.dirty = true;
        Ok(true)
    }

    /// removeRows — RC_INVALID on a refused range (negative, empty
    /// or overflowing), mirroring the model's false return.
    pub fn remove(&self, row: i64, count: i64) -> RcResult<()> {
        let mut state = self.lock();
        let len = state.notes.len() as i64;
        if row < 0 || count <= 0 || row + count > len {
            return fail(RcStatus::InvalidArgument, "note range out of bounds");
        }
        state.notes.drain(row as usize..(row + count) as usize);
        state.dirty = true;
        Ok(())
    }

    /// Commit — the AutoSaver-fired save().  Persists only when
    /// dirty; a volatile store and a clean one are both no-ops.
    /// The parent dir is created like the C++ mkpath so a fresh
    /// profile's first save cannot ENOENT.
    pub fn sync(&self) -> RcResult<()> {
        let mut state = self.lock();
        if !state.dirty {
            return Ok(());
        }
        let file = match &state.file {
            Some(f) => f.clone(),
            None => return Ok(()),
        };
        persist(&file, &state.notes)?;
        state.dirty = false;
        Ok(())
    }

    fn lock(&self) -> std::sync::MutexGuard<'_, State> {
        self.state.lock().unwrap_or_else(|e| e.into_inner())
    }
}

impl Drop for RcNotes {
    /// The model destructor's saveIfNeccessary — a dirty store
    /// flushes on close; a failure here has nowhere to report to.
    fn drop(&mut self) {
        let _ = self.sync();
    }
}

fn note_mut<'a>(state: &'a mut State, row: i64) -> RcResult<&'a mut Note> {
    state
        .notes
        .get_mut(usize::try_from(row).unwrap_or(usize::MAX))
        .ok_or_else(|| Fail {
            status: RcStatus::NotFound,
            msg: "no such note".into(),
        })
}

fn check_field(s: &str, what: &str) -> RcResult<()> {
    let bound = if what == "title" {
        MAX_TITLE_LEN
    } else {
        MAX_BODY_LEN
    };
    if s.len() > bound {
        fail(RcStatus::InvalidArgument, "note field too large")
    } else {
        Ok(())
    }
}

/// The C++ read path: only a JSON array yields notes; every element
/// decodes through QJsonValue::toObject()/toString()/toBool()
/// semantics — a non-object element is a blank note, a wrong-typed
/// field its default.  A missing/unreadable/corrupt file is an
/// empty store, never an error (the model's load() early-outs).
fn load_file(file: &PathBuf) -> RcResult<Vec<Note>> {
    let bytes = match fs::read(file) {
        Ok(b) => b,
        Err(_) => return Ok(Vec::new()),
    };
    if bytes.len() as u64 > MAX_FILE_BYTES {
        return fail(RcStatus::Corrupt, "notes.json exceeds the size bound");
    }
    let doc: serde_json::Value = match serde_json::from_slice(&bytes) {
        Ok(v) => v,
        Err(_) => return Ok(Vec::new()),
    };
    let array = match doc.as_array() {
        Some(a) => a,
        None => return Ok(Vec::new()),
    };
    Ok(array
        .iter()
        .map(|v| {
            let obj = v.as_object();
            Note {
                title: obj
                    .and_then(|o| o.get("title"))
                    .and_then(|s| s.as_str())
                    .unwrap_or("")
                    .to_string(),
                body: obj
                    .and_then(|o| o.get("body"))
                    .and_then(|s| s.as_str())
                    .unwrap_or("")
                    .to_string(),
                markdown: obj
                    .and_then(|o| o.get("markdown"))
                    .and_then(|b| b.as_bool())
                    .unwrap_or(false),
            }
        })
        .collect())
}

/// The C++ write path: a compact JSON array whose objects carry
/// keys in the QJsonObject sorted emission order — serde_json's
/// BTreeMap-backed Map emits alphabetically, so the two writers are
/// byte-identical (verified against Qt 6.12's QJsonDocument::toJson
/// Compact: \" \\ \n \t \uXXXX escapes, raw UTF-8 elsewhere).
fn persist(file: &PathBuf, notes: &[Note]) -> RcResult<()> {
    let array: Vec<serde_json::Value> = notes
        .iter()
        .map(|n| {
            serde_json::json!({
                "body": n.body,
                "markdown": n.markdown,
                "title": n.title,
            })
        })
        .collect();
    let bytes = serde_json::to_vec(&serde_json::Value::Array(array))
        .map_err(|e| Fail {
            status: RcStatus::Corrupt,
            msg: format!("notes encode: {e}"),
        })?;
    if let Some(parent) = file.parent() {
        fs::create_dir_all(parent)?;
    }
    store::atomic_write(file, &bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::fs;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-notes-{}-{}-{}",
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
    fn crud_roundtrip_and_persist() {
        let root = tmpdir("crud");
        let h = RcNotes::open(root.to_str().unwrap(), Some("work")).unwrap();
        assert!(h.is_persistent());
        assert_eq!(h.profile_id().as_deref(), Some("work"));
        assert_eq!(h.count(), 0);

        // Insert order semantics: append, append, prepend-at-0 —
        // and out-of-range positions clamp like qBound.
        assert_eq!(h.insert(99, "A", "body a").unwrap(), 0);
        assert_eq!(h.insert(9, "C", "body c").unwrap(), 1);
        assert_eq!(h.insert(1, "B", "body b").unwrap(), 1);
        assert_eq!(h.insert(-7, "Z", "lead").unwrap(), 0);
        assert_eq!(h.count(), 4);
        assert_eq!(h.get(0).unwrap().title, "Z");
        assert_eq!(h.get(1).unwrap().title, "A");
        assert_eq!(h.get(2).unwrap().title, "B");
        assert!(h.get(4).is_err());
        assert!(h.get(-1).is_err());

        // Field edits: changed reports true, identical reports
        // false and leaves the store untouched.
        assert!(h.set_title(2, "Groceries").unwrap());
        assert!(!h.set_title(2, "Groceries").unwrap());
        assert!(h.set_body(2, "milk\neggs").unwrap());
        assert!(h.set_markdown(2, true).unwrap());
        assert!(!h.set_markdown(2, true).unwrap());
        assert!(h.set_title(9, "x").is_err());

        // removeRows refusals then a real span.
        assert!(h.remove(-1, 1).is_err());
        assert!(h.remove(3, 2).is_err());
        assert!(h.remove(0, 0).is_err());
        h.remove(0, 1).unwrap();
        assert_eq!(h.count(), 3);

        // Dirty until sync commits; a clean sync is a no-op.
        assert!(h.is_dirty());
        h.sync().unwrap();
        assert!(!h.is_dirty());
        h.sync().unwrap();

        let file = root.join("profiles/work/notes.json");
        assert!(file.exists());

        // A fresh handle reads the committed list back identically.
        let h2 = RcNotes::open(root.to_str().unwrap(), Some("work")).unwrap();
        assert_eq!(h2.count(), 3);
        let n = h2.get(0).unwrap();
        assert_eq!(n.title, "A");
        assert_eq!(n.body, "body a");
        assert!(!n.markdown);
        let n = h2.get(1).unwrap();
        assert_eq!(n.title, "Groceries");
        assert_eq!(n.body, "milk\neggs");
        assert!(n.markdown);

        // Profile isolation: a second persona sees its own store.
        let h3 = RcNotes::open(root.to_str().unwrap(), Some("play")).unwrap();
        assert_eq!(h3.count(), 0);
        assert!(h3.insert(0, "p", "b").is_ok());
        assert_eq!(h2.count(), 3);
    }

    #[test]
    fn cpp_written_file_loads_identically() {
        let root = tmpdir("cppread");
        let dir = root.join("profiles/default");
        fs::create_dir_all(&dir).unwrap();
        // The exact byte shape Qt 6.12's QJsonDocument::toJson
        // (Compact) emits for two notes — sorted keys, \n and
        // \uXXXX escapes, raw UTF-8 — captured from a real writer.
        fs::write(
            dir.join("notes.json"),
            "[{\"body\":\"milk\\neggs \u{20ac}\u{7f}\\u0001\",\"markdown\":true,\"title\":\"Shopp\\\"ing\\n\"},{\"body\":\"tab\\tend\",\"markdown\":false,\"title\":\"\u{e9}\u{1f980}\"}]",
        )
        .unwrap();
        let h = RcNotes::open(root.to_str().unwrap(), Some("default")).unwrap();
        assert_eq!(h.count(), 2);
        let a = h.get(0).unwrap();
        assert_eq!(a.title, "Shopp\"ing\n");
        assert_eq!(a.body, "milk\neggs \u{20ac}\u{7f}\u{1}");
        assert!(a.markdown);
        let b = h.get(1).unwrap();
        assert_eq!(b.title, "\u{e9}\u{1f980}");
        assert_eq!(b.body, "tab\tend");
        assert!(!b.markdown);
    }

    #[test]
    fn rust_written_file_is_cpp_shaped() {
        let root = tmpdir("cppwrite");
        let h = RcNotes::open(root.to_str().unwrap(), Some("default")).unwrap();
        h.insert(0, "Shopp\"ing\n", "milk\neggs \u{20ac}\u{7f}\u{1}")
            .unwrap();
        h.set_markdown(0, true).unwrap();
        h.insert(1, "\u{e9}\u{1f980}", "tab\tend").unwrap();
        h.sync().unwrap();
        // Byte-identical to what the Qt writer emits for the same
        // notes — the C++ reader cannot tell which side wrote it.
        let bytes = fs::read(root.join("profiles/default/notes.json")).unwrap();
        assert_eq!(
            String::from_utf8(bytes).unwrap(),
            "[{\"body\":\"milk\\neggs \u{20ac}\u{7f}\\u0001\",\"markdown\":true,\"title\":\"Shopp\\\"ing\\n\"},{\"body\":\"tab\\tend\",\"markdown\":false,\"title\":\"\u{e9}\u{1f980}\"}]"
        );
    }

    #[test]
    fn load_edge_cases_mirror_the_model() {
        let root = tmpdir("edge");
        let dir = root.join("profiles/default");
        fs::create_dir_all(&dir).unwrap();
        let file = dir.join("notes.json");

        // Missing file → empty store, no error.
        let h = RcNotes::open(root.to_str().unwrap(), Some("default")).unwrap();
        assert_eq!(h.count(), 0);
        drop(h);

        // Non-array document → empty.
        fs::write(&file, "{\"a\":1}").unwrap();
        let h = RcNotes::open(root.to_str().unwrap(), Some("default")).unwrap();
        assert_eq!(h.count(), 0);
        drop(h);

        // Unparseable → empty.
        fs::write(&file, "not json").unwrap();
        let h = RcNotes::open(root.to_str().unwrap(), Some("default")).unwrap();
        assert_eq!(h.count(), 0);
        drop(h);

        // Non-object elements are blank notes; wrong-typed fields
        // default; a missing "markdown" reads false (pre-NOTE03
        // stores).
        fs::write(
            &file,
            "[1,{\"title\":5,\"body\":null,\"markdown\":\"x\"},{\"title\":\"old\",\"body\":\"still here\"}]",
        )
        .unwrap();
        let h = RcNotes::open(root.to_str().unwrap(), Some("default")).unwrap();
        assert_eq!(h.count(), 3);
        assert_eq!(h.get(0).unwrap(), Note::default());
        assert_eq!(h.get(1).unwrap(), Note::default());
        let last = h.get(2).unwrap();
        assert_eq!(last.title, "old");
        assert_eq!(last.body, "still here");
        assert!(!last.markdown);
    }

    #[test]
    fn volatile_store_never_touches_disk() {
        // The off-the-record lane: NULL/empty profile → in-memory
        // CRUD, sync() a no-op, nothing written.
        let root = tmpdir("otr");
        let h = RcNotes::open(root.to_str().unwrap(), None).unwrap();
        let h_empty = RcNotes::open(root.to_str().unwrap(), Some("")).unwrap();
        let h_file = RcNotes::open_file("").unwrap();
        for h in [&h, &h_empty, &h_file] {
            assert!(!h.is_persistent());
            assert!(h.profile_id().is_none());
            assert_eq!(h.insert(0, "private", "secret").unwrap(), 0);
            h.sync().unwrap();
            assert_eq!(h.count(), 1);
        }
        assert!(!root.join("profiles").exists());
        // Close flushes nothing either — a volatile store simply
        // dies with its handle.
        drop(h);
        drop(h_empty);
        drop(h_file);
        assert!(!root.join("profiles").exists());
    }

    #[test]
    fn open_file_binds_explicit_path() {
        let root = tmpdir("explicit");
        // A nested path whose parent does not exist yet — persist
        // mkpaths it like the C++ QDir().mkpath before QSaveFile.
        let file = root.join("deep/dir/notes.json");
        let h = RcNotes::open_file(file.to_str().unwrap()).unwrap();
        assert!(h.is_persistent());
        h.insert(0, "x", "y").unwrap();
        h.sync().unwrap();
        assert!(file.exists());
        let h2 = RcNotes::open_file(file.to_str().unwrap()).unwrap();
        assert_eq!(h2.count(), 1);
        assert_eq!(h2.get(0).unwrap().title, "x");
    }

    #[test]
    fn open_refusals_and_bounds() {
        let root = tmpdir("refuse");
        assert!(RcNotes::open("", Some("work")).is_err());
        assert!(RcNotes::open(root.to_str().unwrap(), Some("Bad Id")).is_err());
        assert!(RcNotes::open(root.to_str().unwrap(), Some("../x")).is_err());
        let h = RcNotes::open(root.to_str().unwrap(), Some("work")).unwrap();
        assert!(h.insert(0, &"t".repeat(MAX_TITLE_LEN + 1), "b").is_err());
        assert!(h.insert(0, "t", &"b".repeat(MAX_BODY_LEN + 1)).is_err());
        assert_eq!(h.count(), 0); // refused inserts changed nothing
    }
}
