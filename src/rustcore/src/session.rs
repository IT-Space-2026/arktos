//! RCORE03: the canonical session store — saveSession /
//! restoreLastSession's serialization lives here, engine-neutral.
//! RC13 makes it per-profile: each persona owns
//! `profiles/<id>/session.dat` (the app writes one manifest per
//! persona, engines bind one through the facade), plus a
//! `session.state` lifecycle file carrying the clean-shutdown flag /
//! crash-dirty marker / last-exit record the "restore previous
//! session?" decision reads on every engine alike.
//!
//! On-disk format (`<profile dir>/session.dat`) is a versioned binary
//! schema owned by the crate:
//!
//! ```text
//!   magic   "ARSS" (4 bytes)
//!   version u32 = 4        (v1-v3 blobs — no v4 tails — still decode)
//!   windows u32 count, then per window:
//!     shell   u32 len + bytes   — opaque window-chrome blob (the
//!                               Qt shell's own serialization; the
//!                               core never interprets it)
//!     current i32               — index into tabs, -1 = none
//!     tabs    u32 count, then per tab:
//!       url       u32 len + utf8
//!       container u32 len + utf8 — container binding ("" = default)
//!       group     u32 len + utf8 — tab-group id ("" = ungrouped)
//!       engine    u32 len + utf8 — engine tag ("webengine", ...)
//!       state     u32 len + bytes — OPAQUE per-tab engine-state blob
//!                                   (WebEngine's serialized history
//!                                   today; a Servo backend supplies
//!                                   its own — the format survives
//!                                   the engine swap untouched)
//!       pinned    u8 (0/1)        — v3: TABS04 pin flag
//!       title     u32 len + utf8  — v3: TABS12 rename override
//!                                   ("" = live page title)
//!       hasScroll u8 (0/1)        — v4: RC13 recorded viewport
//!       scrollX   f64 le          —   position (only when hasScroll
//!       scrollY   f64 le          —   is 1); absent = the tab's
//!                                   engine blob owns the answer
//!     groups  u32 count, then per group:
//!       id     u32 len + utf8
//!       name   u32 len + utf8
//!       color  u32 len + utf8 — "#aarrggbb"
//!       collapsed u8 (0/1)
//!     profile u32 len + utf8    — v2 tail: the window's persona id
//!                                 ("" = "default"; MPROF03)
//!     activeContainer u32 len + utf8 — v3 tail: the window's level-1
//!                                 container selection ("" = default;
//!                                 CONT06/SESS01)
//! ```
//!
//! The C ABI carries the same structure as a JSON manifest — opaque
//! byte fields ride as base64 text (the core stores them verbatim,
//! never decoding what it owns opaquely):
//!
//! ```json
//! {"version":1,"windows":[{"shell":"<b64>","current":0,
//!   "tabs":[{"url":"","container":"","group":"",
//!            "engine":"webengine","state":"<b64>",
//!            "pinned":true,"title":"rename"}],
//!   "groups":[{"id":"","name":"","color":"#aarrggbb",
//!              "collapsed":false}],
//!   "profile":"<persona-id>",
//!   "activeContainer":"<container-id>"}]}
//! ```
//!
//! Two manifest keys never reach disk: `"private":true` on a window
//! drops the whole window record, on a tab drops just that tab (the
//! saved current index remaps onto the survivors, mirroring the C++
//! writer's own off-the-record skip) — a producer that serializes a
//! private view cannot leak it through the store.  A tab may also
//! carry `"scroll":{"x":N,"y":N}` (v4): the viewport position an
//! engine backend recorded at save time.
//!
//! The lifecycle file `<profile dir>/session.state` is JSON
//! `{version, active, lastExit, crashCount, restoring}` written
//! atomically: `active` arms at session start and clears only on a
//! recorded exit, so a still-armed marker on the next boot IS the
//! crash signal; `restoring` is the restore-loop guard (armed before
//! tabs reopen, cleared by the next successful save — a mid-restore
//! crash reads back armed and prompts instead of looping).
//!
//! Corrupt-blob resilience is a core property: every count is bounds-
//! checked against the remaining bytes before a single allocation, so
//! a malformed or truncated file can never wedge the decoder, loop a
//! crash prompt, or silently drop the window set — it is rejected
//! whole with RcStatus::Corrupt.  Writes are atomic (temp + fsync +
//! rename, mode 0600 via store::atomic_write): the session blob is
//! browsing history and gets the same custody as credentials.

use std::fs;
use std::path::PathBuf;

use serde_json::{json, Map, Value};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::store;

/// On-disk magic — "Arktos RuSt Session".
pub const MAGIC: &[u8; 4] = b"ARSS";
/// Manifest schema version — the JSON side.  Adding the optional
/// per-window "profile" persona id stayed backwards compatible, so it
/// did not bump this; readers ignore unknown window keys.
pub const VERSION: u32 = 1;
/// Binary blob version — the on-disk side.  v2 appends the per-window
/// profile id string to each window record; v1 blobs (written before
/// MPROF03 window binding) still decode with profile "".  v3 appends
/// per-tab pinned flag + rename override and a per-window active
/// container header (SESS01); older blobs decode with pinned 0,
/// title "" and no recorded header (the manifest omits the key).
const BIN_VERSION: u32 = 4;
const BIN_VERSION_V1: u32 = 1;
pub const FILE_NAME: &str = "session.dat";
/// The lifecycle marker — RC13's clean-shutdown flag + crash-dirty
/// record, one per profile dir next to session.dat.
pub const STATE_FILE_NAME: &str = "session.state";

// Bounds — generous enough for real sessions (data: urls, long
// histories), small enough that a corrupt count field cannot allocate
// absurdly.  Every length is also checked against the bytes actually
// remaining, so these caps are the second line, not the only one.
const MAX_WINDOWS: usize = 1024;
const MAX_TABS: usize = 16384;
const MAX_GROUPS: usize = 4096;
const MAX_FIELD: usize = 64 * 1024 * 1024; // per string/field
const MAX_BLOB: usize = 256 * 1024 * 1024; // per opaque blob
const MAX_JSON_IN: usize = 512 * 1024 * 1024;
const MAX_SCROLL: f64 = 1e9; // recorded viewport coordinate magnitude

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::Corrupt, msg)
}

// ---- binary writer ---------------------------------------------------

struct Put(Vec<u8>);

impl Put {
    fn u32(&mut self, v: u32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn i32(&mut self, v: i32) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn u8(&mut self, v: u8) {
        self.0.push(v);
    }
    fn f64(&mut self, v: f64) {
        self.0.extend_from_slice(&v.to_le_bytes());
    }
    fn bytes(&mut self, b: &[u8]) {
        self.u32(b.len() as u32);
        self.0.extend_from_slice(b);
    }
    fn str(&mut self, s: &str) {
        self.bytes(s.as_bytes());
    }
}

// ---- bounded binary reader -------------------------------------------

struct Take<'a> {
    buf: &'a [u8],
    pos: usize,
}

impl<'a> Take<'a> {
    fn new(buf: &'a [u8]) -> Self {
        Take { buf, pos: 0 }
    }
    fn remaining(&self) -> usize {
        self.buf.len() - self.pos
    }
    fn u32(&mut self) -> RcResult<u32> {
        if self.remaining() < 4 {
            return corrupt("session: truncated u32");
        }
        let v = u32::from_le_bytes(self.buf[self.pos..self.pos + 4].try_into().unwrap());
        self.pos += 4;
        Ok(v)
    }
    fn i32(&mut self) -> RcResult<i32> {
        Ok(self.u32()? as i32)
    }
    fn u8(&mut self) -> RcResult<u8> {
        if self.remaining() < 1 {
            return corrupt("session: truncated byte");
        }
        let v = self.buf[self.pos];
        self.pos += 1;
        Ok(v)
    }
    fn f64(&mut self) -> RcResult<f64> {
        if self.remaining() < 8 {
            return corrupt("session: truncated f64");
        }
        let v = f64::from_le_bytes(self.buf[self.pos..self.pos + 8].try_into().unwrap());
        self.pos += 8;
        Ok(v)
    }
    fn bytes(&mut self, cap: usize) -> RcResult<&'a [u8]> {
        let len = self.u32()? as usize;
        if len > cap {
            return corrupt("session: field over cap");
        }
        if len > self.remaining() {
            return corrupt("session: truncated field");
        }
        let out = &self.buf[self.pos..self.pos + len];
        self.pos += len;
        Ok(out)
    }
    fn str(&mut self) -> RcResult<&'a str> {
        let b = self.bytes(MAX_FIELD)?;
        std::str::from_utf8(b).map_err(|_| Fail {
            status: RcStatus::Corrupt,
            msg: "session: field not UTF-8".into(),
        })
    }
}

// ---- manifest (JSON) -> model ----------------------------------------

#[derive(Default)]
struct TabRec {
    url: String,
    container: String,
    group: String,
    engine: String,
    state: Vec<u8>, // base64 text on the JSON side; stored verbatim
    pinned: bool,
    title: String,
    // RC13: the recorded viewport position ("scroll":{"x","y"}).
    // None = no record — the engine-state blob may still carry its own.
    scroll: Option<(f64, f64)>,
}

#[derive(Default)]
struct GroupRec {
    id: String,
    name: String,
    color: String,
    collapsed: bool,
}

#[derive(Default)]
struct WindowRec {
    shell: Vec<u8>,
    current: i32,
    tabs: Vec<TabRec>,
    groups: Vec<GroupRec>,
    // MPROF03: the persona the window is bound to — "" reads as the
    // "default" profile on the C++ side.
    profile: String,
    // CONT06/SESS01: the window's active level-1 container selection.
    // Some("") is the recorded DEFAULT header — it must round-trip so
    // restore can tell "saved on the default level" apart from a
    // pre-v3 record that carried no selection at all (None).
    header: Option<String>,
}

fn jstr(obj: &Map<String, Value>, key: &str) -> RcResult<String> {
    match obj.get(key) {
        None | Some(Value::Null) => Ok(String::new()),
        Some(Value::String(s)) => {
            if s.len() > MAX_FIELD {
                return corrupt("session manifest: string over cap");
            }
            Ok(s.clone())
        }
        _ => corrupt(format!("session manifest: '{key}' not a string")),
    }
}

fn manifest_to_model(json: &[u8]) -> RcResult<Vec<WindowRec>> {
    if json.len() > MAX_JSON_IN {
        return corrupt("session manifest: input over cap");
    }
    let v: Value = serde_json::from_slice(json).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("session manifest: bad JSON: {e}"),
    })?;
    let root = v.as_object().ok_or_else(|| Fail {
        status: RcStatus::Corrupt,
        msg: "session manifest: root not an object".into(),
    })?;
    match root.get("version") {
        None | Some(Value::Null) => {}
        Some(v) => {
            if v.as_u64() != Some(VERSION as u64) {
                return corrupt("session manifest: unsupported version");
            }
        }
    }
    let windows = match root.get("windows") {
        Some(Value::Array(w)) => w.clone(),
        None | Some(Value::Null) => Vec::new(),
        _ => return corrupt("session manifest: 'windows' not an array"),
    };
    if windows.len() > MAX_WINDOWS {
        return corrupt("session manifest: too many windows");
    }
    let mut out = Vec::with_capacity(windows.len());
    for wv in &windows {
        let w = wv.as_object().ok_or_else(|| Fail {
            status: RcStatus::Corrupt,
            msg: "session manifest: window not an object".into(),
        })?;
        // RC13: a private window never persists — not even an empty
        // record.  The C++ writer already skips it; the store enforces
        // the same rule so no producer can sneak one through.
        if w.get("private").and_then(Value::as_bool).unwrap_or(false) {
            continue;
        }
        let mut rec = WindowRec {
            shell: jstr(w, "shell")?.into_bytes(),
            current: -1,
            tabs: Vec::new(),
            groups: Vec::new(),
            profile: jstr(w, "profile")?,
            // Key presence is the signal — "activeContainer":""
            // records the default header; an absent key means the
            // manifest never recorded a selection.
            header: match w.get("activeContainer") {
                None | Some(Value::Null) => None,
                Some(_) => Some(jstr(w, "activeContainer")?),
            },
        };
        if let Some(c) = w.get("current") {
            match c.as_i64() {
                Some(i) if i >= i32::MIN as i64 && i <= i32::MAX as i64 => {
                    rec.current = i as i32
                }
                _ => return corrupt("session manifest: bad 'current'"),
            }
        }
        if rec.shell.len() > MAX_BLOB {
            return corrupt("session manifest: shell over cap");
        }
        if let Some(tv) = w.get("tabs") {
            let tabs = tv.as_array().ok_or_else(|| Fail {
                status: RcStatus::Corrupt,
                msg: "session manifest: 'tabs' not an array".into(),
            })?;
            if tabs.len() > MAX_TABS {
                return corrupt("session manifest: too many tabs");
            }
            // RC13: "private":true tabs drop out of the record like
            // the C++ writer's off-the-record skip — the saved current
            // index remaps onto the survivors (-1 when the dropped tab
            // WAS the current one, same as collectSessionSnapshot's
            // unset index).
            let mut dropped_before_current = 0i64;
            let mut dropped_current = false;
            for (idx, tv) in tabs.iter().enumerate() {
                let t = tv.as_object().ok_or_else(|| Fail {
                    status: RcStatus::Corrupt,
                    msg: "session manifest: tab not an object".into(),
                })?;
                if t.get("private").and_then(Value::as_bool).unwrap_or(false) {
                    match (idx as i64).cmp(&(rec.current as i64)) {
                        std::cmp::Ordering::Less => dropped_before_current += 1,
                        std::cmp::Ordering::Equal => dropped_current = true,
                        std::cmp::Ordering::Greater => {}
                    }
                    continue;
                }
                let engine = match t.get("engine") {
                    None | Some(Value::Null) => "webengine".to_string(),
                    _ => jstr(t, "engine")?,
                };
                let state = jstr(t, "state")?.into_bytes();
                if state.len() > MAX_BLOB {
                    return corrupt("session manifest: tab state over cap");
                }
                let scroll = match t.get("scroll") {
                    None | Some(Value::Null) => None,
                    Some(Value::Object(s)) => {
                        let x = s.get("x").and_then(Value::as_f64);
                        let y = s.get("y").and_then(Value::as_f64);
                        match (x, y) {
                            (Some(x), Some(y))
                                if x.is_finite() && y.is_finite()
                                    && x.abs() <= MAX_SCROLL
                                    && y.abs() <= MAX_SCROLL =>
                            {
                                Some((x, y))
                            }
                            _ => {
                                return corrupt(
                                    "session manifest: bad 'scroll'",
                                )
                            }
                        }
                    }
                    _ => {
                        return corrupt(
                            "session manifest: 'scroll' not an object",
                        )
                    }
                };
                rec.tabs.push(TabRec {
                    url: jstr(t, "url")?,
                    container: jstr(t, "container")?,
                    group: jstr(t, "group")?,
                    engine,
                    state,
                    pinned: t
                        .get("pinned")
                        .and_then(Value::as_bool)
                        .unwrap_or(false),
                    title: jstr(t, "title")?,
                    scroll,
                });
            }
            if dropped_current {
                rec.current = -1;
            } else {
                rec.current -= dropped_before_current as i32;
            }
        }
        if let Some(gv) = w.get("groups") {
            let groups = gv.as_array().ok_or_else(|| Fail {
                status: RcStatus::Corrupt,
                msg: "session manifest: 'groups' not an array".into(),
            })?;
            if groups.len() > MAX_GROUPS {
                return corrupt("session manifest: too many groups");
            }
            for gv in groups {
                let g = gv.as_object().ok_or_else(|| Fail {
                    status: RcStatus::Corrupt,
                    msg: "session manifest: group not an object".into(),
                })?;
                rec.groups.push(GroupRec {
                    id: jstr(g, "id")?,
                    name: jstr(g, "name")?,
                    color: jstr(g, "color")?,
                    collapsed: g
                        .get("collapsed")
                        .and_then(Value::as_bool)
                        .unwrap_or(false),
                });
            }
        }
        out.push(rec);
    }
    Ok(out)
}

// ---- encode / decode ---------------------------------------------------

fn model_to_binary(windows: &[WindowRec]) -> Vec<u8> {
    let mut p = Put(Vec::with_capacity(1024));
    p.0.extend_from_slice(MAGIC);
    p.u32(BIN_VERSION);
    p.u32(windows.len() as u32);
    for w in windows {
        p.bytes(&w.shell);
        p.i32(w.current);
        p.u32(w.tabs.len() as u32);
        for t in &w.tabs {
            p.str(&t.url);
            p.str(&t.container);
            p.str(&t.group);
            p.str(&t.engine);
            p.bytes(&t.state);
            // v3 fields: pin flag + rename override.
            p.u8(t.pinned as u8);
            p.str(&t.title);
            // v4 field: recorded scroll position (flag + pair only
            // when set).
            match t.scroll {
                Some((x, y)) => {
                    p.u8(1);
                    p.f64(x);
                    p.f64(y);
                }
                None => p.u8(0),
            }
        }
        p.u32(w.groups.len() as u32);
        for g in &w.groups {
            p.str(&g.id);
            p.str(&g.name);
            p.str(&g.color);
            p.u8(g.collapsed as u8);
        }
        // v2 tail: the window's persona id.
        p.str(&w.profile);
        // v3 tail: the window's active container header — an absent
        // selection encodes as "" and reads back as the default level
        // (the distinction only matters across the manifest edge).
        p.str(w.header.as_deref().unwrap_or(""));
    }
    p.0
}

fn binary_to_model(blob: &[u8]) -> RcResult<Vec<WindowRec>> {
    let mut r = Take::new(blob);
    if r.remaining() < 4 || &r.buf[..4] != MAGIC {
        return corrupt("session: bad magic");
    }
    r.pos = 4;
    let bin_version = r.u32()?;
    if !(BIN_VERSION_V1..=BIN_VERSION).contains(&bin_version) {
        return corrupt("session: unsupported version");
    }
    let window_count = r.u32()? as usize;
    if window_count > MAX_WINDOWS || window_count > r.remaining() / 4 {
        return corrupt("session: window count over bound");
    }
    let mut windows = Vec::with_capacity(window_count);
    for _ in 0..window_count {
        let shell = r.bytes(MAX_BLOB)?.to_vec();
        let current = r.i32()?;
        let tab_count = r.u32()? as usize;
        if tab_count > MAX_TABS || tab_count > r.remaining() / 4 {
            return corrupt("session: tab count over bound");
        }
        let mut tabs = Vec::with_capacity(tab_count.min(1024));
        for _ in 0..tab_count {
            tabs.push(TabRec {
                url: r.str()?.to_owned(),
                container: r.str()?.to_owned(),
                group: r.str()?.to_owned(),
                engine: r.str()?.to_owned(),
                state: r.bytes(MAX_BLOB)?.to_vec(),
                // v1/v2 records end at state — they predate pinning
                // and renames, so the tabs read unpinned/untitled.
                pinned: bin_version >= 3 && r.u8()? != 0,
                title: if bin_version >= 3 {
                    r.str()?.to_owned()
                } else {
                    String::new()
                },
                // v1-v3 records carry no scroll tail — they predate
                // the recorded position and read with none.
                scroll: if bin_version >= 4 && r.u8()? != 0 {
                    Some((r.f64()?, r.f64()?))
                } else {
                    None
                },
            });
        }
        let group_count = r.u32()? as usize;
        if group_count > MAX_GROUPS || group_count > r.remaining() / 4 {
            return corrupt("session: group count over bound");
        }
        let mut groups = Vec::with_capacity(group_count.min(256));
        for _ in 0..group_count {
            groups.push(GroupRec {
                id: r.str()?.to_owned(),
                name: r.str()?.to_owned(),
                color: r.str()?.to_owned(),
                collapsed: r.u8()? != 0,
            });
        }
        // v1 records end at the group list — their windows predate the
        // persona binding and read as "default" upstream.
        let profile = if bin_version >= 2 {
            r.str()?.to_owned()
        } else {
            String::new()
        };
        // v3 tail: the window's level-1 container selection — earlier
        // blobs carried none, so they emit no key (the C++ side then
        // falls back to the current tab's container).
        let header = if bin_version >= 3 {
            Some(r.str()?.to_owned())
        } else {
            None
        };
        windows.push(WindowRec {
            shell,
            current,
            tabs,
            groups,
            profile,
            header,
        });
    }
    if r.remaining() != 0 {
        return corrupt("session: trailing bytes");
    }
    Ok(windows)
}

fn model_to_manifest(windows: &[WindowRec]) -> Vec<u8> {
    let wins: Vec<Value> = windows
        .iter()
        .map(|w| {
            let tabs: Vec<Value> = w
                .tabs
                .iter()
                .map(|t| {
                    let mut tab = json!({
                        "url": t.url,
                        "container": t.container,
                        "group": t.group,
                        "engine": t.engine,
                        "state": String::from_utf8_lossy(&t.state),
                    });
                    // Emit the v3 keys only when set — an unpinned,
                    // untitled tab keeps the pre-v3 shape.
                    if t.pinned {
                        tab["pinned"] = json!(true);
                    }
                    if !t.title.is_empty() {
                        tab["title"] = json!(t.title);
                    }
                    // v4: emit the recorded scroll only when set — an
                    // unrecorded tab keeps the pre-v4 shape.
                    if let Some((x, y)) = t.scroll {
                        tab["scroll"] = json!({"x": x, "y": y});
                    }
                    tab
                })
                .collect();
            let groups: Vec<Value> = w
                .groups
                .iter()
                .map(|g| {
                    json!({
                        "id": g.id,
                        "name": g.name,
                        "color": g.color,
                        "collapsed": g.collapsed,
                    })
                })
                .collect();
            let mut win = json!({
                "shell": String::from_utf8_lossy(&w.shell),
                "current": w.current,
                "tabs": tabs,
                "groups": groups,
            });
            // Emit the persona only when bound — a "default" window's
            // manifest stays identical to the pre-MPROF03 shape.
            if !w.profile.is_empty() {
                win["profile"] = json!(w.profile);
            }
            // Same for the level-1 selection, except Some("") still
            // emits: a recorded default header is not the same as a
            // window that never had the field.
            if let Some(header) = &w.header {
                win["activeContainer"] = json!(header);
            }
            win
        })
        .collect();
    serde_json::to_vec(&json!({
        "version": VERSION,
        "windows": wins,
    }))
    .unwrap_or_else(|_| b"{}".to_vec())
}

/// JSON manifest -> canonical binary blob.
pub fn encode(json: &[u8]) -> RcResult<Vec<u8>> {
    Ok(model_to_binary(&manifest_to_model(json)?))
}

/// Binary blob -> canonical JSON manifest.  RcStatus::Corrupt on any
/// malformation — the caller must treat the whole session as absent.
pub fn decode(blob: &[u8]) -> RcResult<Vec<u8>> {
    Ok(model_to_manifest(&binary_to_model(blob)?))
}

// ---- file store --------------------------------------------------------

fn session_path() -> RcResult<PathBuf> {
    Ok(store::lock().dir()?.join(FILE_NAME))
}

/// True when a session file exists (false when no data dir is set).
pub fn exists() -> bool {
    session_path().map(|p| p.is_file()).unwrap_or(false)
}

/// Validate, encode and atomically install the session file.
pub fn save(json: &[u8]) -> RcResult<()> {
    save_at(&session_path()?, json)
}

/// Read + decode the session file into the canonical JSON manifest.
/// RcStatus::NotFound when no session exists, Corrupt on malformation.
pub fn load() -> RcResult<Vec<u8>> {
    load_at(&session_path()?)
}

/// Remove the session file; absent is not an error.
pub fn clear() -> RcResult<()> {
    clear_at(&session_path()?)
}

// ---- path-explicit ops (RC13) ------------------------------------------
//
// The profile-bound surface: an AcSession handle (or any caller that
// already resolved profiles/<id>/) drives these directly instead of
// re-anchoring the process-global data dir.  Same codec, same atomic
// install — the two spellings can never diverge on disk.

/// True when `path` names an existing session file.
pub fn exists_at(path: &std::path::Path) -> bool {
    path.is_file()
}

/// save() at an explicit file path.
pub fn save_at(path: &std::path::Path, json: &[u8]) -> RcResult<()> {
    let blob = encode(json)?;
    store::atomic_write(path, &blob)
}

/// load() at an explicit file path.
pub fn load_at(path: &std::path::Path) -> RcResult<Vec<u8>> {
    let blob = match fs::read(path) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return fail(RcStatus::NotFound, "no saved session")
        }
        Err(e) => return Err(e.into()),
    };
    decode(&blob)
}

/// clear() at an explicit file path.
pub fn clear_at(path: &std::path::Path) -> RcResult<()> {
    match fs::remove_file(path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.into()),
    }
}

// ---- lifecycle marker (RC13) -------------------------------------------
//
// session.state sits next to session.dat: {version, active, lastExit,
// crashCount, restoring} — the clean-shutdown flag + crash-dirty
// record the "restore previous session?" decision needs on every
// engine.  A malformed or unreadable file reads as the zero state
// (an absent marker, not an error): the boot path must never fail on
// a breadcrumb.

#[derive(Default, Clone)]
struct Lifecycle {
    active: bool,
    last_exit: String,
    crash_count: u32,
    restoring: bool,
}

fn state_path_in(dir: &std::path::Path) -> PathBuf {
    dir.join(STATE_FILE_NAME)
}

fn read_state(path: &std::path::Path) -> Lifecycle {
    let blob = match fs::read(path) {
        Ok(b) => b,
        Err(_) => return Lifecycle::default(),
    };
    let v: Value = match serde_json::from_slice(&blob) {
        Ok(v) => v,
        Err(_) => return Lifecycle::default(),
    };
    let obj = match v.as_object() {
        Some(o) => o,
        None => return Lifecycle::default(),
    };
    Lifecycle {
        active: obj.get("active").and_then(Value::as_bool).unwrap_or(false),
        last_exit: obj
            .get("lastExit")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string(),
        crash_count: obj
            .get("crashCount")
            .and_then(Value::as_u64)
            .unwrap_or(0) as u32,
        restoring: obj
            .get("restoring")
            .and_then(Value::as_bool)
            .unwrap_or(false),
    }
}

fn write_state(path: &std::path::Path, st: &Lifecycle) -> RcResult<()> {
    let json = serde_json::to_vec(&json!({
        "version": 1,
        "active": st.active,
        "lastExit": st.last_exit,
        "crashCount": st.crash_count,
        "restoring": st.restoring,
    }))
    .unwrap_or_else(|_| b"{}".to_vec());
    store::atomic_write(path, &json)
}

/// Boots the session marker in `dir`: reports whether the previous
/// run died uncleanly (armed marker still set), records the
/// "unclean" breadcrumb + bumps the crash counter when it did, then
/// arms the marker for THIS run.  Mirrors BrowserProfile's
/// markSessionStart sentinel semantics.
pub fn mark_start_in(dir: &std::path::Path) -> RcResult<bool> {
    let path = state_path_in(dir);
    let mut st = read_state(&path);
    let was_dirty = st.active;
    if was_dirty {
        st.crash_count = st.crash_count.saturating_add(1);
        st.last_exit = "unclean (crash or external kill)".to_string();
    }
    st.active = true;
    write_state(&path, &st)?;
    Ok(was_dirty)
}

/// Records a clean exit in `dir` — disarms the marker, clears the
/// crash counter, breadcrumbs "clean rc=N".
pub fn mark_exit_in(dir: &std::path::Path, code: i32) -> RcResult<()> {
    let path = state_path_in(dir);
    let mut st = read_state(&path);
    st.active = false;
    st.crash_count = 0;
    st.last_exit = format!("clean rc={code}");
    write_state(&path, &st)
}

/// Records a handled signal termination in `dir` — clean for
/// crash-loop purposes but the breadcrumb says how it died, never a
/// plain "rc=0".
pub fn mark_terminated_in(dir: &std::path::Path, signal: i32) -> RcResult<()> {
    let path = state_path_in(dir);
    let mut st = read_state(&path);
    st.active = false;
    st.crash_count = 0;
    let name = match signal {
        15 => "SIGTERM",
        2 => "SIGINT",
        1 => "SIGHUP",
        _ => "signal",
    };
    st.last_exit = format!("terminated by signal {signal} ({name})");
    write_state(&path, &st)
}

/// True while the session marker in `dir` reads armed — a still-armed
/// marker at boot means the previous run never recorded an exit.
pub fn dirty_in(dir: &std::path::Path) -> bool {
    read_state(&state_path_in(dir)).active
}

/// The last recorded exit breadcrumb — None when none was written.
pub fn last_exit_in(dir: &std::path::Path) -> Option<String> {
    let s = read_state(&state_path_in(dir)).last_exit;
    if s.is_empty() {
        None
    } else {
        Some(s)
    }
}

/// The consecutive-unclean count — the crash-loop signal.
pub fn crash_count_in(dir: &std::path::Path) -> u32 {
    read_state(&state_path_in(dir)).crash_count
}

/// Arms/disarms the restore-loop guard in `dir`.
pub fn set_restoring_in(dir: &std::path::Path, on: bool) -> RcResult<()> {
    let path = state_path_in(dir);
    let mut st = read_state(&path);
    st.restoring = on;
    write_state(&path, &st)
}

/// True while the restore guard reads armed — a crash mid-restore
/// leaves it set so the next boot prompts instead of looping.
pub fn restoring_in(dir: &std::path::Path) -> bool {
    read_state(&state_path_in(dir)).restoring
}

// Anchored variants — the rc_* surface (the data dir IS the bound
// profile dir for session callers).

fn anchored_dir() -> RcResult<PathBuf> {
    Ok(store::lock().dir()?.to_path_buf())
}

pub fn mark_start() -> RcResult<bool> {
    mark_start_in(&anchored_dir()?)
}

pub fn mark_exit(code: i32) -> RcResult<()> {
    mark_exit_in(&anchored_dir()?, code)
}

pub fn mark_terminated(signal: i32) -> RcResult<()> {
    mark_terminated_in(&anchored_dir()?, signal)
}

pub fn dirty() -> bool {
    anchored_dir().map(|d| dirty_in(&d)).unwrap_or(false)
}

pub fn last_exit() -> Option<String> {
    anchored_dir().ok().and_then(|d| last_exit_in(&d))
}

pub fn crash_count() -> u32 {
    anchored_dir().map(|d| crash_count_in(&d)).unwrap_or(0)
}

pub fn set_restoring(on: bool) -> RcResult<()> {
    set_restoring_in(&anchored_dir()?, on)
}

pub fn restoring() -> bool {
    anchored_dir().map(|d| restoring_in(&d)).unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn manifest() -> Vec<u8> {
        serde_json::to_vec(&json!({
            "version": 1,
            "windows": [
                {
                    "shell": "c2hlbGwtYmxvYg==",
                    "current": 1,
                    "tabs": [
                        {"url": "https://a.example/", "container": "",
                         "group": "", "engine": "webengine",
                         "state": "aGlzdG9yeQ==",
                         "pinned": true, "title": "Pinned Tab"},
                        {"url": "https://b.example/", "container": "work",
                         "group": "g1", "engine": "webengine",
                         "state": ""},
                        {"url": "https://c.example/", "container": "",
                         "group": "g1", "engine": "servo",
                         "state": ""}
                    ],
                    "groups": [
                        {"id": "g1", "name": "research",
                         "color": "#ff336699", "collapsed": false}
                    ],
                    "activeContainer": "work"
                },
                {
                    "shell": "",
                    "current": -1,
                    "tabs": [],
                    "groups": [],
                    "profile": "work"
                }
            ]
        }))
        .unwrap()
    }

    fn roundtrip(json: &[u8]) -> Value {
        let blob = encode(json).expect("encode");
        let out = decode(&blob).expect("decode");
        serde_json::from_slice(&out).expect("canonical json")
    }

    #[test]
    fn roundtrip_preserves_structure() {
        let v = roundtrip(&manifest());
        let wins = v["windows"].as_array().unwrap();
        assert_eq!(wins.len(), 2);
        assert_eq!(wins[0]["current"], 1);
        assert_eq!(wins[0]["shell"], "c2hlbGwtYmxvYg==");
        let tabs = wins[0]["tabs"].as_array().unwrap();
        assert_eq!(tabs.len(), 3);
        assert_eq!(tabs[1]["container"], "work");
        assert_eq!(tabs[1]["group"], "g1");
        assert_eq!(tabs[1]["engine"], "webengine");
        assert_eq!(tabs[0]["state"], "aGlzdG9yeQ==");
        let groups = wins[0]["groups"].as_array().unwrap();
        assert_eq!(groups[0]["name"], "research");
        assert_eq!(groups[0]["collapsed"], false);
        assert_eq!(wins[1]["tabs"].as_array().unwrap().len(), 0);
        assert_eq!(wins[1]["profile"], "work");
    }

    #[test]
    fn v3_fields_roundtrip_and_omit_when_default() {
        let v = roundtrip(&manifest());
        let win = &v["windows"][0];
        let tabs = win["tabs"].as_array().unwrap();
        // SESS01: pin flag, rename override, engine tag and the level-1
        // header all survive the rust round-trip; unset fields keep
        // their pre-v3 absence.
        assert_eq!(tabs[0]["pinned"], true);
        assert_eq!(tabs[0]["title"], "Pinned Tab");
        assert!(tabs[1].get("pinned").is_none());
        assert!(tabs[1].get("title").is_none());
        assert_eq!(tabs[2]["engine"], "servo");
        assert_eq!(win["activeContainer"], "work");
        // The v3 binary tail always carries a header — a window whose
        // manifest omitted the key canonicalizes to the recorded
        // default level; only pre-v3 blobs decode key-free.
        assert_eq!(v["windows"][1]["activeContainer"], "");
    }

    #[test]
    fn recorded_default_header_survives() {
        // SESS01: "activeContainer":"" is the recorded DEFAULT level —
        // it must not collapse into an absent key, or a restore
        // racing an active container filter would land on the wrong
        // header.
        let v = roundtrip(
            br#"{"windows":[{"shell":"","current":0,
                "activeContainer":"",
                "tabs":[{"url":"https://x/","container":"work",
                    "engine":"webengine","state":""}]}]}"#,
        );
        assert_eq!(v["windows"][0]["activeContainer"], "");
    }

    #[test]
    fn decodes_v2_blobs_without_v3_fields() {
        // A pre-SESS01 session.dat (bin v2: persona tail, no v3
        // fields) still restores — tabs read unpinned/untitled and
        // the window reads the default header.
        let mut blob = Vec::new();
        blob.extend_from_slice(MAGIC);
        blob.extend_from_slice(&2u32.to_le_bytes()); // bin v2
        blob.extend_from_slice(&1u32.to_le_bytes()); // 1 window
        blob.extend_from_slice(&4u32.to_le_bytes()); // shell len
        blob.extend_from_slice(b"shll");
        blob.extend_from_slice(&(-1i32).to_le_bytes()); // current
        blob.extend_from_slice(&1u32.to_le_bytes()); // 1 tab
        blob.extend_from_slice(&3u32.to_le_bytes()); // url len
        blob.extend_from_slice(b"u:l");
        blob.extend_from_slice(&0u32.to_le_bytes()); // container ""
        blob.extend_from_slice(&0u32.to_le_bytes()); // group ""
        blob.extend_from_slice(&9u32.to_le_bytes()); // engine "webengine"
        blob.extend_from_slice(b"webengine");
        blob.extend_from_slice(&0u32.to_le_bytes()); // state len 0
        blob.extend_from_slice(&0u32.to_le_bytes()); // 0 groups
        blob.extend_from_slice(&4u32.to_le_bytes()); // profile "work"
        blob.extend_from_slice(b"work");
        let out = decode(&blob).expect("v2 blob decodes");
        let v: Value = serde_json::from_slice(&out).unwrap();
        let win = &v["windows"][0];
        assert_eq!(win["profile"], "work");
        assert!(win.get("activeContainer").is_none());
        let tabs = win["tabs"].as_array().unwrap();
        assert_eq!(tabs[0]["url"], "u:l");
        assert!(tabs[0].get("pinned").is_none());
        assert!(tabs[0].get("title").is_none());
    }

    #[test]
    fn profile_roundtrips_and_omits_when_default() {
        let v = roundtrip(&manifest());
        // The persona-bound window keeps its id; the unbound one stays
        // key-free so a default window's manifest is unchanged.
        assert_eq!(v["windows"][1]["profile"], "work");
        assert!(v["windows"][0].get("profile").is_none());
    }

    #[test]
    fn decodes_v1_blobs_as_default_profile() {
        // A pre-MPROF03 session.dat (bin v1: no profile tail) still
        // restores — every window reads as persona "".
        let mut blob = Vec::new();
        blob.extend_from_slice(MAGIC);
        blob.extend_from_slice(&BIN_VERSION_V1.to_le_bytes());
        blob.extend_from_slice(&1u32.to_le_bytes()); // 1 window
        blob.extend_from_slice(&4u32.to_le_bytes()); // shell len
        blob.extend_from_slice(b"shll");
        blob.extend_from_slice(&(-1i32).to_le_bytes()); // current
        blob.extend_from_slice(&0u32.to_le_bytes()); // 0 tabs
        blob.extend_from_slice(&0u32.to_le_bytes()); // 0 groups
        let out = decode(&blob).expect("v1 blob decodes");
        let v: Value = serde_json::from_slice(&out).unwrap();
        assert_eq!(v["windows"].as_array().unwrap().len(), 1);
        assert!(v["windows"][0].get("profile").is_none());
        assert_eq!(v["windows"][0]["shell"], "shll");
    }

    #[test]
    fn defaults_fill_in() {
        // Minimal manifest — missing optional fields canonicalize.
        let v = roundtrip(br#"{"windows":[{"shell":"","current":0,"tabs":[{"url":"https://x/"}]}]}"#);
        let t = &v["windows"][0]["tabs"][0];
        assert_eq!(t["engine"], "webengine"); // absent -> default engine
        assert_eq!(t["container"], "");
        assert_eq!(v["version"], 1);
    }

    #[test]
    fn rejects_bad_magic() {
        assert!(decode(b"nope").is_err());
        assert!(decode(b"").is_err());
    }

    #[test]
    fn rejects_bad_version() {
        let mut blob = encode(&manifest()).unwrap();
        blob[4] = 9;
        assert!(decode(&blob).is_err());
    }

    #[test]
    fn rejects_truncation_everywhere() {
        let blob = encode(&manifest()).unwrap();
        for cut in 0..blob.len() {
            assert!(
                decode(&blob[..cut]).is_err(),
                "truncated at {cut} must not decode"
            );
        }
    }

    #[test]
    fn rejects_trailing_garbage() {
        let mut blob = encode(&manifest()).unwrap();
        blob.extend_from_slice(b"X");
        assert!(decode(&blob).is_err());
    }

    #[test]
    fn rejects_inflated_counts() {
        // A corrupt window-count must be caught by the remaining-bytes
        // bound, not by a giant allocation.
        let mut blob = Vec::new();
        blob.extend_from_slice(MAGIC);
        blob.extend_from_slice(&VERSION.to_le_bytes());
        blob.extend_from_slice(&u32::MAX.to_le_bytes());
        assert!(decode(&blob).is_err());
    }

    #[test]
    fn rejects_bad_manifest() {
        assert!(encode(b"not json").is_err());
        assert!(encode(br#"{"windows":"x"}"#).is_err());
        assert!(encode(br#"{"version":9,"windows":[]}"#).is_err());
        assert!(encode(br#"{"version":-1,"windows":[]}"#).is_err());
        assert!(encode(br#"{"version":"1","windows":[]}"#).is_err());
        assert!(encode(br#"{"windows":[{"tabs":[3]}]}"#).is_err());
    }

    #[test]
    fn empty_session_roundtrips() {
        let v = roundtrip(br#"{"version":1,"windows":[]}"#);
        assert_eq!(v["windows"].as_array().unwrap().len(), 0);
    }

    // ---- RC13: scroll position -------------------------------------

    #[test]
    fn scroll_roundtrips_and_omits_when_unset() {
        let v = roundtrip(
            br#"{"windows":[{"shell":"","current":0,
                "tabs":[
                    {"url":"https://a/","engine":"webengine","state":"",
                     "scroll":{"x":12.5,"y":340}},
                    {"url":"https://b/","engine":"servo","state":""}
                ]}]}"#,
        );
        let tabs = v["windows"][0]["tabs"].as_array().unwrap();
        assert_eq!(tabs[0]["scroll"]["x"], 12.5);
        assert_eq!(tabs[0]["scroll"]["y"], 340.0);
        // Unrecorded tabs keep the pre-v4 absence.
        assert!(tabs[1].get("scroll").is_none());
    }

    #[test]
    fn rejects_bad_scroll() {
        assert!(encode(br#"{"windows":[{"tabs":[{"url":"x","scroll":"hi"}]}]}"#).is_err());
        assert!(encode(
            br#"{"windows":[{"tabs":[{"url":"x","scroll":{"x":1}}]}]}"#
        )
        .is_err());
        assert!(encode(
            br#"{"windows":[{"tabs":[{"url":"x","scroll":{"x":1e30,"y":0}}]}]}"#
        )
        .is_err());
    }

    #[test]
    fn decodes_v3_blobs_without_scroll() {
        // A pre-RC13 session.dat (bin v3) still restores — its tabs
        // read with no recorded scroll.
        let mut blob = Vec::new();
        blob.extend_from_slice(MAGIC);
        blob.extend_from_slice(&3u32.to_le_bytes()); // bin v3
        blob.extend_from_slice(&1u32.to_le_bytes()); // 1 window
        blob.extend_from_slice(&0u32.to_le_bytes()); // shell ""
        blob.extend_from_slice(&0i32.to_le_bytes()); // current
        blob.extend_from_slice(&1u32.to_le_bytes()); // 1 tab
        blob.extend_from_slice(&3u32.to_le_bytes());
        blob.extend_from_slice(b"u:l");
        blob.extend_from_slice(&0u32.to_le_bytes()); // container
        blob.extend_from_slice(&0u32.to_le_bytes()); // group
        blob.extend_from_slice(&9u32.to_le_bytes());
        blob.extend_from_slice(b"webengine");
        blob.extend_from_slice(&0u32.to_le_bytes()); // state
        blob.push(1u8); // pinned
        blob.extend_from_slice(&1u32.to_le_bytes()); // title "T"
        blob.extend_from_slice(b"T");
        blob.extend_from_slice(&0u32.to_le_bytes()); // 0 groups
        blob.extend_from_slice(&0u32.to_le_bytes()); // profile ""
        blob.extend_from_slice(&0u32.to_le_bytes()); // activeContainer
        let out = decode(&blob).expect("v3 blob decodes");
        let v: Value = serde_json::from_slice(&out).unwrap();
        let tab = &v["windows"][0]["tabs"][0];
        assert_eq!(tab["pinned"], true);
        assert_eq!(tab["title"], "T");
        assert!(tab.get("scroll").is_none());
    }

    // ---- RC13: private exclusion ------------------------------------

    #[test]
    fn private_tabs_and_windows_never_persist() {
        let v = roundtrip(
            br#"{"windows":[
                {"shell":"a","current":2,
                 "tabs":[
                    {"url":"https://a/","state":""},
                    {"url":"https://secret-one/","state":"","private":true},
                    {"url":"https://b/","state":""},
                    {"url":"https://c/","state":""}
                 ]},
                {"shell":"p","current":0,"private":true,
                 "tabs":[{"url":"https://secret-window/","state":""}]},
                {"shell":"c","current":0,
                 "tabs":[{"url":"https://secret-only/","state":"",
                          "private":true}]}
            ]}"#,
        );
        let wins = v["windows"].as_array().unwrap();
        // The private window vanished whole; the mixed window kept
        // only its public tabs with current remapped 2 -> 1 (the
        // dropped tab sat before it).
        assert_eq!(wins.len(), 2);
        let tabs = wins[0]["tabs"].as_array().unwrap();
        assert_eq!(tabs.len(), 3);
        assert_eq!(tabs[0]["url"], "https://a/");
        assert_eq!(tabs[1]["url"], "https://b/");
        assert_eq!(tabs[2]["url"], "https://c/");
        assert_eq!(wins[0]["current"], 1);
        // The all-private window survives as an empty record (today's
        // C++ shape) but its tab and its current index are gone.
        assert_eq!(wins[1]["tabs"].as_array().unwrap().len(), 0);
        assert_eq!(wins[1]["current"], -1);
        // Nothing private can be read back anywhere.
        let text = serde_json::to_string(&v).unwrap();
        assert!(!text.contains("secret"));
    }

    #[test]
    fn dropped_current_tab_resets_index() {
        // The dropped tab WAS the current one — same unset result the
        // C++ snapshot produces when the current page is skipped.
        let v = roundtrip(
            br#"{"windows":[{"shell":"","current":0,
                "tabs":[{"url":"https://x/","state":"","private":true},
                        {"url":"https://y/","state":""}]}]}"#,
        );
        assert_eq!(v["windows"][0]["current"], -1);
    }

    // ---- RC13: lifecycle marker --------------------------------------

    #[test]
    fn lifecycle_clean_crash_and_guard() {
        let dir = std::env::temp_dir()
            .join(format!("rc-sess-lc-{}-a", std::process::id()));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();

        // Fresh boot: no marker, no record.
        assert!(!dirty_in(&dir));
        assert_eq!(last_exit_in(&dir), None);
        assert_eq!(crash_count_in(&dir), 0);
        assert_eq!(mark_start_in(&dir).unwrap(), false);
        assert!(dirty_in(&dir));

        // Two consecutive unclean starts — the counter climbs and the
        // record says how the last run ended.
        assert_eq!(mark_start_in(&dir).unwrap(), true);
        assert_eq!(crash_count_in(&dir), 1);
        assert_eq!(
            last_exit_in(&dir).as_deref(),
            Some("unclean (crash or external kill)")
        );
        assert_eq!(mark_start_in(&dir).unwrap(), true);
        assert_eq!(crash_count_in(&dir), 2);

        // A clean exit disarms, resets the counter and breadcrumbs.
        mark_exit_in(&dir, 0).unwrap();
        assert!(!dirty_in(&dir));
        assert_eq!(crash_count_in(&dir), 0);
        assert_eq!(last_exit_in(&dir).as_deref(), Some("clean rc=0"));
        assert_eq!(mark_start_in(&dir).unwrap(), false);

        // A handled signal is clean but never reads "rc=0".
        mark_terminated_in(&dir, 15).unwrap();
        assert!(!dirty_in(&dir));
        assert_eq!(
            last_exit_in(&dir).as_deref(),
            Some("terminated by signal 15 (SIGTERM)")
        );

        // The restore guard is its own flag.
        assert!(!restoring_in(&dir));
        set_restoring_in(&dir, true).unwrap();
        assert!(restoring_in(&dir));
        set_restoring_in(&dir, false).unwrap();
        assert!(!restoring_in(&dir));

        // A corrupt state file reads as the zero state — a breadcrumb
        // never fails boot.
        fs::write(state_path_in(&dir), b"garbage").unwrap();
        assert!(!dirty_in(&dir));
        assert_eq!(last_exit_in(&dir), None);

        let _ = fs::remove_dir_all(&dir);
    }
}
