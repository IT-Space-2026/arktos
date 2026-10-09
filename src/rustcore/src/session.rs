//! RCORE03: the canonical session store — saveSession /
//! restoreLastSession's serialization lives here, engine-neutral.
//!
//! On-disk format (`<data dir>/session.dat`) is a versioned binary
//! schema owned by the crate:
//!
//! ```text
//!   magic   "ARSS" (4 bytes)
//!   version u32 = 1
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
//!     groups  u32 count, then per group:
//!       id     u32 len + utf8
//!       name   u32 len + utf8
//!       color  u32 len + utf8 — "#aarrggbb"
//!       collapsed u8 (0/1)
//! ```
//!
//! The C ABI carries the same structure as a JSON manifest — opaque
//! byte fields ride as base64 text (the core stores them verbatim,
//! never decoding what it owns opaquely):
//!
//! ```json
//! {"version":1,"windows":[{"shell":"<b64>","current":0,
//!   "tabs":[{"url":"","container":"","group":"",
//!            "engine":"webengine","state":"<b64>"}],
//!   "groups":[{"id":"","name":"","color":"#aarrggbb",
//!              "collapsed":false}]}]}
//! ```
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

/// On-disk magic — "Arora RuSt Session".
pub const MAGIC: &[u8; 4] = b"ARSS";
pub const VERSION: u32 = 1;
pub const FILE_NAME: &str = "session.dat";

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
        let mut rec = WindowRec {
            shell: jstr(w, "shell")?.into_bytes(),
            current: -1,
            tabs: Vec::new(),
            groups: Vec::new(),
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
            for tv in tabs {
                let t = tv.as_object().ok_or_else(|| Fail {
                    status: RcStatus::Corrupt,
                    msg: "session manifest: tab not an object".into(),
                })?;
                let engine = match t.get("engine") {
                    None | Some(Value::Null) => "webengine".to_string(),
                    _ => jstr(t, "engine")?,
                };
                let state = jstr(t, "state")?.into_bytes();
                if state.len() > MAX_BLOB {
                    return corrupt("session manifest: tab state over cap");
                }
                rec.tabs.push(TabRec {
                    url: jstr(t, "url")?,
                    container: jstr(t, "container")?,
                    group: jstr(t, "group")?,
                    engine,
                    state,
                });
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
    p.u32(VERSION);
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
        }
        p.u32(w.groups.len() as u32);
        for g in &w.groups {
            p.str(&g.id);
            p.str(&g.name);
            p.str(&g.color);
            p.u8(g.collapsed as u8);
        }
    }
    p.0
}

fn binary_to_model(blob: &[u8]) -> RcResult<Vec<WindowRec>> {
    let mut r = Take::new(blob);
    if r.remaining() < 4 || &r.buf[..4] != MAGIC {
        return corrupt("session: bad magic");
    }
    r.pos = 4;
    if r.u32()? != VERSION {
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
        windows.push(WindowRec {
            shell,
            current,
            tabs,
            groups,
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
                    json!({
                        "url": t.url,
                        "container": t.container,
                        "group": t.group,
                        "engine": t.engine,
                        "state": String::from_utf8_lossy(&t.state),
                    })
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
            json!({
                "shell": String::from_utf8_lossy(&w.shell),
                "current": w.current,
                "tabs": tabs,
                "groups": groups,
            })
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
    let blob = encode(json)?;
    let path = session_path()?;
    store::atomic_write(&path, &blob)
}

/// Read + decode the session file into the canonical JSON manifest.
/// RcStatus::NotFound when no session exists, Corrupt on malformation.
pub fn load() -> RcResult<Vec<u8>> {
    let path = session_path()?;
    let blob = match fs::read(&path) {
        Ok(b) => b,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            return fail(RcStatus::NotFound, "no saved session")
        }
        Err(e) => return Err(e.into()),
    };
    decode(&blob)
}

/// Remove the session file; absent is not an error.
pub fn clear() -> RcResult<()> {
    let path = session_path()?;
    match fs::remove_file(&path) {
        Ok(()) => Ok(()),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => Ok(()),
        Err(e) => Err(e.into()),
    }
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
                         "state": "aGlzdG9yeQ=="},
                        {"url": "https://b.example/", "container": "work",
                         "group": "g1", "engine": "webengine",
                         "state": ""},
                        {"url": "https://c.example/", "container": "",
                         "group": "g1", "engine": "webengine",
                         "state": ""}
                    ],
                    "groups": [
                        {"id": "g1", "name": "research",
                         "color": "#ff336699", "collapsed": false}
                    ]
                },
                {
                    "shell": "",
                    "current": -1,
                    "tabs": [],
                    "groups": []
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
}
