//! mw (MB02): the middleware envelope — the versioned, typed
//! command/query request-response surface MW-ARCH §2 chose as THE
//! bridge between Qt callers, the rustcore stores and the engine
//! adapters.  One entry point, ac_mw_dispatch(), frames every
//! domain op in the same 20-byte little-endian header + JSON
//! payload so versioning, correlation, error shaping and panic
//! containment live in exactly one place instead of ~470 entry
//! points.
//!
//! Frame (requests and responses share it — see arktos_core.h):
//!
//!   off  size  field
//!    0    2    magic "AM"                — cheap non-frame reject
//!    2    1    envelope schema version  — v1
//!    3    1    msg_type  request: 1=query 2=command
//!                       response: 3=ok 4=error
//!    4    8    corr_id                  — caller-chosen, echoed
//!   12    1    domain                  — registry below
//!   13    1    flags                   — v1: 0 (reserved)
//!   14    2    op                      — per-domain op id
//!   16    4    payload_len             — bytes following
//!   20    N    payload                 — UTF-8 JSON; may be empty
//!
//! Contract (MW-ARCH §2.1–2.3):
//!
//!   * ac_mw_dispatch's AcStatus return is transport-level only —
//!     NULL core/out or an unreadable request buffer fail bare with
//!     *out = {NULL,0}.  Anything that reads as a frame attempt,
//!     however malformed, yields AC_OK and a type=4 error envelope
//!     (corr_id 0 when the header itself did not parse); malformed
//!     input is a structured error, never a panic and never a bare
//!     status.
//!   * corr_id echoes verbatim — the only ordering/matching key.
//!   * Requests declare their class in msg_type and it MUST match
//!     the op's registered class: the byte is what a future
//!     read-only context gates on, so a mis-declared request is a
//!     malformed one, not a silent override.
//!   * Handles never cross the frame: scoped payloads carry
//!     {"profile":...} and the dispatcher resolves the bound store
//!     per call (the flat ac_* handle's re-assert contract, minus
//!     the handle).
//!   * Error payload {"code","status","message"}: code is the
//!     stable enum below; status carries the underlying RcStatus
//!     for "op_failed" (0 for pure envelope faults); message is
//!     also stashed for ac_last_error() on the same thread.
//!   * *out is caller-owned — freed with ac_buffer_free, the
//!     existing ownership pattern.
//!
//! MB02 lands the envelope + the registry skeleton + a proving
//! slice (mw meta ops, settings KV, session file ops, one
//! stateless service).  MB05+ walk mw.describe to extend the op
//! tables — append-only, never renumber.

use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::ptr;

use serde_json::{json, Value};

use crate::error::{self, RcStatus};
use crate::facade::{self, AcCore};
use crate::{
    blocklist, bookmarks, containers, cookies, extverify, history, icons, notes, notify,
    omnibox, opensearch, profiles, profilevault, session, settings, sitedecisions,
    sitetrust, store, sync, syncbookmarks, synchistory, syncpair, synctabs, synctransport, tlsprobe, tranco, updates, util,
};

/// The envelope schema version — the frame's `version` byte and the
/// ac_mw_version() report.  v1 is the only version; older-but-known
/// versions stay dispatchable when a v2 ever lands (MW-ARCH §4).
pub const MW_ENVELOPE_VERSION: u32 = 1;

const MAGIC: [u8; 2] = *b"AM";
const HEADER_LEN: usize = 20;

const TY_QUERY: u8 = 1;
const TY_COMMAND: u8 = 2;
const TY_OK: u8 = 3;
const TY_ERROR: u8 = 4;

/// The error-code vocabulary — a stable string enum, append-only.
const E_BAD_VERSION: &str = "bad_version";
const E_MALFORMED: &str = "malformed";
const E_UNKNOWN_DOMAIN: &str = "unknown_domain";
const E_UNKNOWN_OP: &str = "unknown_op";
const E_BAD_PAYLOAD: &str = "bad_payload";
const E_OP_FAILED: &str = "op_failed";

const ERROR_CODES: [&str; 6] = [
    E_BAD_VERSION,
    E_MALFORMED,
    E_UNKNOWN_DOMAIN,
    E_UNKNOWN_OP,
    E_BAD_PAYLOAD,
    E_OP_FAILED,
];

/// Change topics the notify lane can emit today — the events array
/// mw.describe reports (MB03 formalizes namespaced topics on top;
/// these are the strings notify::emit sites actually fire).
const EVENT_TOPICS: [&str; 15] = [
    "autofill",
    "bookmarks",
    "containers",
    "cookies",
    "credentials",
    "history",
    "icons",
    "notes",
    "profiles",
    "searchengines",
    "settings",
    "sitedecisions",
    "sync",
    "torctl",
    "updates",
];

// ---- framing ------------------------------------------------------------

struct Request<'a> {
    msg_type: u8,
    corr_id: u64,
    domain: u8,
    op: u16,
    payload: &'a [u8],
}

/// A parse/validation failure carrying everything the error frame
/// needs — corr_id/domain/op are 0 when the header itself did not
/// parse.
struct ErrFrame {
    corr_id: u64,
    domain: u8,
    op: u16,
    code: &'static str,
    status: i32,
    msg: String,
}

fn build_frame(msg_type: u8, corr_id: u64, domain: u8, op: u16, payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(HEADER_LEN + payload.len());
    v.extend_from_slice(&MAGIC);
    v.push(MW_ENVELOPE_VERSION as u8);
    v.push(msg_type);
    v.extend_from_slice(&corr_id.to_le_bytes());
    v.push(domain);
    v.push(0); // flags — reserved
    v.extend_from_slice(&op.to_le_bytes());
    v.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    v.extend_from_slice(payload);
    v
}

/// Serializes the {"code","status","message"} error object into a
/// type=4 frame and mirrors the message into the thread-local error
/// slot (what ac_last_error() would have stashed — MW-ARCH §2.2).
fn error_frame(e: &ErrFrame) -> Vec<u8> {
    error::set_error(&e.msg);
    let payload = serde_json::to_vec(&json!({
        "code": e.code,
        "status": e.status,
        "message": e.msg,
    }))
    .unwrap_or_default();
    build_frame(TY_ERROR, e.corr_id, e.domain, e.op, &payload)
}

fn parse_request(buf: &[u8]) -> Result<Request<'_>, ErrFrame> {
    let no_header = |msg: &str| ErrFrame {
        corr_id: 0,
        domain: 0,
        op: 0,
        code: E_MALFORMED,
        status: 0,
        msg: msg.into(),
    };
    if buf.len() < HEADER_LEN {
        return Err(no_header("frame shorter than the 20-byte header"));
    }
    if buf[0..2] != MAGIC {
        return Err(no_header("bad frame magic"));
    }
    let version = buf[2];
    let msg_type = buf[3];
    let corr_id = u64::from_le_bytes(buf[4..12].try_into().unwrap_or([0; 8]));
    let domain = buf[12];
    // buf[13] is the flags byte — reserved in v1, not enforced.
    let op = u16::from_le_bytes(buf[14..16].try_into().unwrap_or([0; 2]));
    let payload_len = u32::from_le_bytes(buf[16..20].try_into().unwrap_or([0; 4])) as usize;
    let known = |code: &'static str, msg: String| ErrFrame {
        corr_id,
        domain,
        op,
        code,
        status: 0,
        msg,
    };
    if u32::from(version) != MW_ENVELOPE_VERSION {
        return Err(known(
            E_BAD_VERSION,
            format!("envelope version {version} not supported (max {MW_ENVELOPE_VERSION})"),
        ));
    }
    if !matches!(msg_type, TY_QUERY | TY_COMMAND) {
        return Err(known(
            E_MALFORMED,
            format!("msg_type {msg_type} is not a request class"),
        ));
    }
    let avail = buf.len() - HEADER_LEN;
    if payload_len > avail {
        return Err(known(
            E_MALFORMED,
            format!("payload_len {payload_len} exceeds the {avail} bytes in frame"),
        ));
    }
    if payload_len < avail {
        return Err(known(
            E_MALFORMED,
            format!("{} trailing bytes after payload", avail - payload_len),
        ));
    }
    Ok(Request {
        msg_type,
        corr_id,
        domain,
        op,
        payload: &buf[HEADER_LEN..],
    })
}

// ---- op registry ----------------------------------------------------------

#[derive(Clone, Copy, PartialEq, Eq)]
enum OpClass {
    Query,
    Command,
}

impl OpClass {
    fn as_str(self) -> &'static str {
        match self {
            OpClass::Query => "query",
            OpClass::Command => "command",
        }
    }
}

/// Everything a handler may reach.  Today that is the facade root
/// (the data root per-call profile binding resolves under); MB05+
/// may grow it (subscriber tables, caps) without touching handler
/// signatures.
struct MwCtx<'a> {
    core: &'a AcCore,
}

/// Handler failures: BadPayload is a pure envelope fault (status 0);
/// Failed carries the store's RcStatus through as "op_failed".
enum OpErr {
    BadPayload(String),
    Failed(error::Fail),
}

impl From<error::Fail> for OpErr {
    fn from(f: error::Fail) -> OpErr {
        OpErr::Failed(f)
    }
}

fn bad_payload<T>(msg: impl Into<String>) -> Result<T, OpErr> {
    Err(OpErr::BadPayload(msg.into()))
}

type Handler = fn(&MwCtx, &Value) -> Result<Value, OpErr>;

struct Op {
    id: u16,
    name: &'static str,
    class: OpClass,
    /// True when the op binds a profile per call (payload "profile").
    profile_scoped: bool,
    /// The envelope version that introduced the op (append-only).
    since: u8,
    handler: Handler,
}

struct Domain {
    id: u8,
    name: &'static str,
    ops: &'static [Op],
}

// ---- arg helpers ---------------------------------------------------------

fn arg_str<'a>(p: &'a Value, name: &str) -> Result<&'a str, OpErr> {
    p.get(name)
        .and_then(Value::as_str)
        .ok_or_else(|| OpErr::BadPayload(format!("missing string field \"{name}\"")))
}

fn arg_i64(p: &Value, name: &str) -> Result<i64, OpErr> {
    p.get(name)
        .and_then(Value::as_i64)
        .ok_or_else(|| OpErr::BadPayload(format!("missing integer field \"{name}\"")))
}

/// Optional string field — null and absent both read as None.
fn opt_str<'a>(p: &'a Value, name: &str) -> Result<Option<&'a str>, OpErr> {
    match p.get(name) {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) => Ok(Some(s.as_str())),
        Some(_) => bad_payload(format!("\"{name}\" must be a string or null")),
    }
}

/// Optional non-negative integer — null and absent both read as
/// None; a non-number or negative value is a bad payload, never a
/// silent default.
fn opt_u64(p: &Value, name: &str) -> Result<Option<u64>, OpErr> {
    match p.get(name) {
        None | Some(Value::Null) => Ok(None),
        Some(v) => v
            .as_u64()
            .map(Some)
            .ok_or_else(|| OpErr::BadPayload(format!("\"{name}\" must be a non-negative integer"))),
    }
}

/// Optional signed integer — null and absent both read as None.
fn opt_i64(p: &Value, name: &str) -> Result<Option<i64>, OpErr> {
    match p.get(name) {
        None | Some(Value::Null) => Ok(None),
        Some(v) => v
            .as_i64()
            .map(Some)
            .ok_or_else(|| OpErr::BadPayload(format!("\"{name}\" must be an integer"))),
    }
}

/// The {"profile":"<id>"|null} field every scoped op accepts
/// (MW-ARCH §2.3): absent/null/"" resolves through the domain's own
/// scope rule; a non-slug id resolves to "default" per the facade's
/// bound_profile convention.
fn profile_arg<'a>(p: &'a Value) -> Result<Option<&'a str>, OpErr> {
    match p.get("profile") {
        None | Some(Value::Null) => Ok(None),
        Some(Value::String(s)) if s.is_empty() => Ok(None),
        Some(Value::String(s)) => Ok(Some(s.as_str())),
        Some(_) => bad_payload("\"profile\" must be a string or null"),
    }
}

// ---- base64 -----------------------------------------------------------------
//
// Byte payloads cross the JSON envelope base64'd — icon PNGs, vault
// passphrases, manifest bytes.  Field names carry the _b64 suffix so
// the encoding is never ambiguous on either side.

const B64_ALPHABET: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub(crate) fn b64_encode(data: &[u8]) -> String {
    let mut out = String::with_capacity((data.len() + 2) / 3 * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0] as u32;
        let b1 = *chunk.get(1).unwrap_or(&0) as u32;
        let b2 = *chunk.get(2).unwrap_or(&0) as u32;
        let n = (b0 << 16) | (b1 << 8) | b2;
        out.push(B64_ALPHABET[(n >> 18) as usize & 63] as char);
        out.push(B64_ALPHABET[(n >> 12) as usize & 63] as char);
        out.push(if chunk.len() > 1 {
            B64_ALPHABET[(n >> 6) as usize & 63] as char
        } else {
            '='
        });
        out.push(if chunk.len() > 2 {
            B64_ALPHABET[n as usize & 63] as char
        } else {
            '='
        });
    }
    out
}

/// RFC 4648 strict decode — padding only at the tail, alphabet-only
/// otherwise.  None on any malformed input.
pub(crate) fn b64_decode(s: &str) -> Option<Vec<u8>> {
    let bytes = s.as_bytes();
    if bytes.len() % 4 != 0 {
        return None;
    }
    let val = |c: u8| -> Option<u32> {
        Some(match c {
            b'A'..=b'Z' => (c - b'A') as u32,
            b'a'..=b'z' => (c - b'a' + 26) as u32,
            b'0'..=b'9' => (c - b'0' + 52) as u32,
            b'+' => 62,
            b'/' => 63,
            _ => return None,
        })
    };
    let chunks = bytes.len() / 4;
    let mut out = Vec::with_capacity(chunks * 3);
    for (i, chunk) in bytes.chunks(4).enumerate() {
        let last = i + 1 == chunks;
        let mut n = 0u32;
        let mut pad = 0usize;
        for &c in chunk {
            if c == b'=' {
                if !last || pad == 2 {
                    return None;
                }
                pad += 1;
                n <<= 6;
            } else {
                if pad > 0 {
                    return None; // data after padding
                }
                n = (n << 6) | val(c)?;
            }
        }
        out.push((n >> 16) as u8);
        if pad < 2 {
            out.push((n >> 8) as u8);
        }
        if pad < 1 {
            out.push(n as u8);
        }
    }
    Some(out)
}

fn arg_b64(p: &Value, name: &str) -> Result<Vec<u8>, OpErr> {
    let s = arg_str(p, name)?;
    b64_decode(s)
        .ok_or_else(|| OpErr::BadPayload(format!("\"{name}\" is not valid base64")))
}

// ---- profile binding ---------------------------------------------------------
//
// The envelope's handle-free contract (MW-ARCH §2.3): every scoped op
// resolves {"profile"} to the effective persona id and its
// profiles/<id>/ dir — the same anchor an Ac* handle's bind computes
// (profile_dir mkpaths, so first-touch writes can't ENOENT).  The
// returned id is what change events stamp.
fn bound_profile_dir(ctx: &MwCtx, p: &Value) -> Result<(PathBuf, String), OpErr> {
    let eff = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let root = std::path::Path::new(ctx.core.data_root());
    Ok((facade::profile_dir(root, &eff), eff))
}

/// A store path that must reach a C-string contract — a non-UTF-8
/// path is an environment fault (InvalidArgument), not a bad payload.
fn path_str(p: &Path) -> Result<&str, OpErr> {
    p.to_str().ok_or_else(|| {
        OpErr::Failed(error::Fail {
            status: RcStatus::InvalidArgument,
            msg: "path is not UTF-8".into(),
        })
    })
}

// ---- mw (domain 0) ---------------------------------------------------------

fn describe_doc() -> Value {
    json!({
        "envelope_version": MW_ENVELOPE_VERSION,
        "header_len": HEADER_LEN,
        "error_codes": ERROR_CODES,
        "domains": DOMAINS
            .iter()
            .map(|d| json!({
                "id": d.id,
                "name": d.name,
                "ops": d.ops
                    .iter()
                    .map(|o| json!({
                        "id": o.id,
                        "name": o.name,
                        "class": o.class.as_str(),
                        "profile_scoped": o.profile_scoped,
                        "since": o.since,
                    }))
                    .collect::<Vec<Value>>(),
            }))
            .collect::<Vec<Value>>(),
        "events": EVENT_TOPICS,
    })
}

fn op_mw_describe(_ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    Ok(describe_doc())
}

fn op_mw_version(_ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    Ok(json!({
        "envelope_version": MW_ENVELOPE_VERSION,
        "api_version": facade::AC_API_VERSION,
    }))
}

fn op_mw_ping(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    Ok(json!({ "pong": true, "echo": p }))
}

/// Fires one namespaced event at the ac_events_* lane — the bridge
/// lane's own provoke hook (acprobe's event round-trip drives it,
/// so do conformance harnesses that cannot reach a store mutation).
/// `topic` must be a declared registry topic: subscribers are
/// promised *some* producer, and a typo'd topic is a bad payload,
/// never a silent fire.  `fields` lands in the v1 document beside
/// the envelope keys.
fn op_mw_emit(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let topic = arg_str(p, "topic")?;
    if !notify::EVENT_TOPICS.iter().any(|t| t.topic == topic) {
        return bad_payload(format!("topic \"{topic}\" is not a declared event topic"));
    }
    let fields = p.get("fields").cloned().unwrap_or_else(|| json!({}));
    notify::emit_event(topic, fields);
    Ok(json!({ "ok": true, "topic": topic }))
}

const MW_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "describe",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_mw_describe,
    },
    Op {
        id: 0x0002,
        name: "version",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_mw_version,
    },
    Op {
        id: 0x0003,
        name: "ping",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_mw_ping,
    },
    Op {
        id: 0x0004,
        name: "emit",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_mw_emit,
    },
];

// ---- settings (domain 1) ----------------------------------------------------
//
// The proving slice for a store-backed domain: a short-lived
// RcSettings per call — the AcContainers precedent, so a registry
// the C++ mirror wrote mid-process is honored and alternating
// personas stay cheap.  Commands emit "settings" stamped with the
// bound scope, the flat path's notify contract verbatim.

/// Opens the scope {"profile"} selects: absent/null/"" is the global
/// scope, a non-slug id resolves to "default" (the ac_settings rule).
/// Returns the handle plus the stamped profile id (None = global).
fn settings_scope(ctx: &MwCtx, p: &Value) -> Result<(settings::RcSettings, Option<String>), OpErr> {
    let pid = profile_arg(p)?
        .map(|s| facade::effective_profile(s).to_string());
    let h = settings::RcSettings::open(ctx.core.data_root(), pid.as_deref())?;
    Ok((h, pid))
}

fn op_settings_get(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let key = arg_str(p, "key")?;
    let (h, _pid) = settings_scope(ctx, p)?;
    // Same {"t","v"} typed doc rc_settings_get hands out.
    Ok(settings::value_to_json(&h.get(key)?))
}

fn op_settings_get_string(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let key = arg_str(p, "key")?;
    let (h, _pid) = settings_scope(ctx, p)?;
    Ok(json!({ "value": h.get_string(key)? }))
}

fn op_settings_list(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let prefix = match p.get("prefix") {
        None | Some(Value::Null) => None,
        Some(Value::String(s)) => Some(s.as_str()),
        Some(_) => return bad_payload("\"prefix\" must be a string or null"),
    };
    let (h, _pid) = settings_scope(ctx, p)?;
    Ok(json!({ "keys": h.list(prefix)? }))
}

fn op_settings_set(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let key = arg_str(p, "key")?;
    let raw = p
        .get("value")
        .ok_or_else(|| OpErr::BadPayload("missing \"value\" doc".into()))?;
    let value = settings::value_from_json(raw)
        .ok_or_else(|| OpErr::BadPayload("\"value\" is not a typed settings doc".into()))?;
    let (h, pid) = settings_scope(ctx, p)?;
    let _hint = pid.as_deref().map(notify::profile_hint);
    h.set(key, value)?;
    // Flat-path parity: emit on every Ok, stamped with the scope.
    notify::emit("settings");
    Ok(json!({ "ok": true }))
}

fn op_settings_remove(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let key = arg_str(p, "key")?;
    let (h, pid) = settings_scope(ctx, p)?;
    let _hint = pid.as_deref().map(notify::profile_hint);
    h.remove(key)?;
    notify::emit("settings");
    Ok(json!({ "ok": true }))
}

fn op_settings_contains(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let key = arg_str(p, "key")?;
    let (h, _pid) = settings_scope(ctx, p)?;
    Ok(json!({ "present": h.contains(key)? }))
}

/// rc_settings_sync parity: a flush with no change topic —
/// mutations already write through, so nothing moved to announce.
fn op_settings_sync(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (h, _pid) = settings_scope(ctx, p)?;
    h.sync()?;
    Ok(json!({ "ok": true }))
}

/// rc_settings_import_ini parity: the absent-keys-only merge.  The
/// imported markers persist inside settings.json, so the per-call
/// bind keeps the flat path's one-shot semantics; "settings" emits
/// on every Ok like the flat spelling.
fn op_settings_import_ini(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let path = arg_str(p, "path")?;
    let (h, pid) = settings_scope(ctx, p)?;
    let _hint = pid.as_deref().map(notify::profile_hint);
    h.import_ini(path)?;
    notify::emit("settings");
    Ok(json!({ "ok": true }))
}

const SETTINGS_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "get",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_settings_get,
    },
    Op {
        id: 0x0002,
        name: "get_string",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_settings_get_string,
    },
    Op {
        id: 0x0003,
        name: "list",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_settings_list,
    },
    Op {
        id: 0x0004,
        name: "set",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_settings_set,
    },
    Op {
        id: 0x0005,
        name: "remove",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_settings_remove,
    },
    Op {
        id: 0x0006,
        name: "contains",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_settings_contains,
    },
    Op {
        id: 0x0007,
        name: "sync",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_settings_sync,
    },
    Op {
        id: 0x0008,
        name: "import_ini",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_settings_import_ini,
    },
];

// ---- profiles (domain 2) ------------------------------------------------------
//
// The persona registry — RcProfiles opens on the core's data root,
// the same handle ac_profiles mints.  reload mirrors
// rc_profiles_reload's contract: persisted silently, no change topic
// on either surface.

fn profiles_handle(ctx: &MwCtx) -> Result<profiles::RcProfiles, OpErr> {
    profiles::RcProfiles::open(ctx.core.data_root(), ctx.core.default_name())
        .map_err(OpErr::Failed)
}

fn op_profiles_list(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let rows: Vec<Value> = profiles_handle(ctx)?
        .list()
        .iter()
        .map(|p| {
            json!({
                "id": p.id,
                "name": p.name,
                "avatar": p.avatar,
                "createdAt": p.created_at,
            })
        })
        .collect();
    Ok(json!({ "profiles": rows }))
}

fn op_profiles_reload(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    profiles_handle(ctx)?.reload()?;
    Ok(json!({ "ok": true }))
}

fn profile_json(p: profiles::ProfileRecord) -> Value {
    json!({
        "id": p.id,
        "name": p.name,
        "avatar": p.avatar,
        "createdAt": p.created_at,
    })
}

fn op_profiles_get(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_str(p, "id")?;
    let row = profiles_handle(ctx)?
        .get(id)
        .ok_or_else(|| error::Fail {
            status: RcStatus::NotFound,
            msg: "no such profile".into(),
        })?;
    Ok(profile_json(row))
}

// The registry mutations mirror rc_profiles_*'s emit: "profiles" on
// every Ok, unscoped — the registry is global, so no profile hint.
fn op_profiles_create(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "display_name")?;
    let id = profiles_handle(ctx)?.create(name)?;
    notify::emit("profiles");
    Ok(json!({ "id": id }))
}

fn op_profiles_rename(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_str(p, "id")?;
    let name = arg_str(p, "name")?;
    profiles_handle(ctx)?.rename(id, name)?;
    notify::emit("profiles");
    Ok(json!({ "ok": true }))
}

fn op_profiles_set_avatar(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_str(p, "id")?;
    let name = opt_str(p, "name")?;
    profiles_handle(ctx)?.set_avatar(id, name)?;
    notify::emit("profiles");
    Ok(json!({ "ok": true }))
}

fn op_profiles_delete(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_str(p, "id")?;
    profiles_handle(ctx)?.delete(id)?;
    notify::emit("profiles");
    Ok(json!({ "ok": true }))
}

const PROFILES_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "list",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_list,
    },
    Op {
        id: 0x0002,
        name: "reload",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_reload,
    },
    Op {
        id: 0x0003,
        name: "get",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_get,
    },
    Op {
        id: 0x0004,
        name: "create",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_create,
    },
    Op {
        id: 0x0005,
        name: "rename",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_rename,
    },
    Op {
        id: 0x0006,
        name: "set_avatar",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_set_avatar,
    },
    Op {
        id: 0x0007,
        name: "delete",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_profiles_delete,
    },
];

// ---- history (domain 3) -------------------------------------------------------
//
// Process-global store behind the open-path anchor — the envelope
// binds profiles/<id>/history.db per call (the AcHistory re-assert,
// minus the handle).  Commands mirror rc_hist_*'s dual emit: the
// legacy "history" topic plus namespaced "history/changed" stamped
// with the bound profile.

fn history_db(ctx: &MwCtx, p: &Value) -> Result<(String, String), OpErr> {
    let (dir, eff) = bound_profile_dir(ctx, p)?;
    Ok((path_str(&dir.join("history.db"))?.to_string(), eff))
}

fn op_history_count(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (db, _eff) = history_db(ctx, p)?;
    let n = history::with(|h| -> error::RcResult<i64> {
        h.open(Some(db.as_str()))?;
        h.count()
    })?;
    Ok(json!({ "count": n }))
}

fn op_history_add(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let url = arg_str(p, "url")?;
    let title = opt_str(p, "title")?.unwrap_or("");
    let ts_ms = arg_i64(p, "ts_ms")?;
    let (db, eff) = history_db(ctx, p)?;
    history::with(|h| -> error::RcResult<()> {
        h.open(Some(db.as_str()))?;
        h.add(url, title, ts_ms)
    })?;
    // Flat-path parity: hint first, then both notification lanes.
    let _hint = notify::profile_hint(&eff);
    notify::emit("history");
    notify::emit_event("history/changed", json!({ "op": "visit" }));
    Ok(json!({ "ok": true }))
}

fn op_history_query(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let spec = p
        .get("spec")
        .ok_or_else(|| OpErr::BadPayload("missing \"spec\" doc".into()))?;
    let spec_json = serde_json::to_string(spec).unwrap_or_else(|_| "{}".into());
    let (db, _eff) = history_db(ctx, p)?;
    let out = history::with(|h| -> error::RcResult<String> {
        h.open(Some(db.as_str()))?;
        h.query(&spec_json)
    })?;
    // The store answers a JSON array — hand it back as the payload.
    Ok(serde_json::from_str(&out).unwrap_or(Value::Null))
}

/// ac_hist_suggest / rc_history_suggest parity: the completer's
/// ranked rows — `now` is computed here, as in the flat spelling.
fn op_history_suggest(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let term = arg_str(p, "term")?;
    let limit = opt_i64(p, "limit")?.unwrap_or(0);
    let (db, _eff) = history_db(ctx, p)?;
    let out = history::with(|h| -> error::RcResult<String> {
        h.open(Some(db.as_str()))?;
        let now_ms = std::time::SystemTime::now()
            .duration_since(std::time::UNIX_EPOCH)
            .map(|d| d.as_millis() as i64)
            .unwrap_or(0);
        h.suggest(term, limit, now_ms)
    })?;
    Ok(serde_json::from_str(&out).unwrap_or(Value::Null))
}

/// ac_history_exists parity: a path stat for the legacy-import
/// gate — never opens the database.
fn op_history_exists(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (db, _eff) = history_db(ctx, p)?;
    Ok(json!({
        "exists": history::HistoryStore::exists_on_disk(Some(db.as_str())),
    }))
}

/// The flat rc_hist_* notification pair, verbatim: the legacy
/// "history" topic plus namespaced "history/changed", both stamped
/// with the bound profile the way ac_hist_*'s hint did.
fn history_emit(eff: &str, verb: &str) {
    let _hint = notify::profile_hint(eff);
    notify::emit("history");
    notify::emit_event("history/changed", json!({ "op": verb }));
}

fn op_history_update_title(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let url = arg_str(p, "url")?;
    let title = arg_str(p, "title")?;
    let (db, eff) = history_db(ctx, p)?;
    history::with(|h| -> error::RcResult<()> {
        h.open(Some(db.as_str()))?;
        h.update_title(url, title)
    })?;
    history_emit(&eff, "title");
    Ok(json!({ "ok": true }))
}

fn op_history_remove(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let url = arg_str(p, "url")?;
    let title = arg_str(p, "title")?;
    let ts_ms = arg_i64(p, "ts_ms")?;
    let (db, eff) = history_db(ctx, p)?;
    history::with(|h| -> error::RcResult<()> {
        h.open(Some(db.as_str()))?;
        h.remove(url, title, ts_ms)
    })?;
    history_emit(&eff, "remove");
    Ok(json!({ "ok": true }))
}

fn op_history_clear(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (db, eff) = history_db(ctx, p)?;
    history::with(|h| -> error::RcResult<()> {
        h.open(Some(db.as_str()))?;
        h.clear()
    })?;
    history_emit(&eff, "clear");
    Ok(json!({ "ok": true }))
}

fn op_history_clear_since(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let since_ms = arg_i64(p, "since_ms")?;
    let (db, eff) = history_db(ctx, p)?;
    history::with(|h| -> error::RcResult<()> {
        h.open(Some(db.as_str()))?;
        h.clear_since(since_ms)
    })?;
    history_emit(&eff, "clear_since");
    Ok(json!({ "ok": true }))
}

fn op_history_expire(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let cutoff_ms = arg_i64(p, "cutoff_ms")?;
    let (db, eff) = history_db(ctx, p)?;
    let removed = history::with(|h| -> error::RcResult<i64> {
        h.open(Some(db.as_str()))?;
        h.expire_older_than(cutoff_ms)
    })?;
    history_emit(&eff, "expire");
    Ok(json!({ "removed": removed }))
}

const HISTORY_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "count",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_history_count,
    },
    Op {
        id: 0x0002,
        name: "add",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_history_add,
    },
    Op {
        id: 0x0003,
        name: "query",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_history_query,
    },
    Op {
        id: 0x0004,
        name: "suggest",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_history_suggest,
    },
    Op {
        id: 0x0005,
        name: "exists",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_history_exists,
    },
    Op {
        id: 0x0006,
        name: "update_title",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_history_update_title,
    },
    Op {
        id: 0x0007,
        name: "remove",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_history_remove,
    },
    Op {
        id: 0x0008,
        name: "clear",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_history_clear,
    },
    Op {
        id: 0x0009,
        name: "clear_since",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_history_clear_since,
    },
    Op {
        id: 0x000a,
        name: "expire",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_history_expire,
    },
];

// ---- bookmarks (domain 4) ------------------------------------------------------
//
// Process-global tree behind the loaded-file anchor —
// ensure_path() is the AcBookmarks re-assert: a displaced tree
// flushes before re-anchoring, a same-file bind is a no-op.  A load
// that ran emits "bookmarks" like rc_bm_load does.

fn bookmarks_xbel(ctx: &MwCtx, p: &Value) -> Result<(PathBuf, String), OpErr> {
    let (dir, eff) = bound_profile_dir(ctx, p)?;
    Ok((dir.join("bookmarks.xbel"), eff))
}

fn op_bookmarks_get(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (xbel, _eff) = bookmarks_xbel(ctx, p)?;
    let id = opt_u64(p, "id")?;
    bookmarks::with(|b| -> error::RcResult<Value> {
        b.ensure_path(&xbel)?;
        match id {
            None => {
                let root = b.root();
                Ok(json!({
                    "root": root,
                    "child_count": b.child_count(root).unwrap_or(0),
                }))
            }
            Some(h) => {
                let doc = b.get_json(h).ok_or_else(|| error::Fail {
                    status: RcStatus::NotFound,
                    msg: format!("no bookmark node {h}"),
                })?;
                Ok(serde_json::from_str(&doc).unwrap_or(Value::Null))
            }
        }
    })
    .map_err(OpErr::Failed)
}

fn op_bookmarks_create(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let node = p
        .get("node")
        .ok_or_else(|| OpErr::BadPayload("missing \"node\" doc".into()))?;
    // row -1 appends, like the flat insert contract.
    let row = opt_i64(p, "row")?.unwrap_or(-1);
    let parent_arg = opt_u64(p, "parent")?;
    let (xbel, eff) = bookmarks_xbel(ctx, p)?;
    let (created, loaded) = bookmarks::with(|b| -> error::RcResult<(u64, bool)> {
        let loaded = b.ensure_path(&xbel)?;
        let parent = parent_arg.unwrap_or_else(|| b.root());
        let h = b.create(parent, row, node)?;
        Ok((h, loaded))
    })?;
    let _hint = notify::profile_hint(&eff);
    if loaded {
        notify::emit("bookmarks"); // the bind's load, like rc_bm_load
    }
    notify::emit("bookmarks");
    Ok(json!({ "id": created }))
}

const BOOKMARKS_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "get",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_bookmarks_get,
    },
    Op {
        id: 0x0002,
        name: "create",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_bookmarks_create,
    },
];

// ---- policy (domain 5) ---------------------------------------------------------
//
// Sitedecisions live behind the data-dir anchor — the envelope
// re-anchors profiles/<id>/ per call, exactly what ensure_policy
// does for the flat handle (rc_set_data_dir no-ops on a matching
// anchor).  Emits mirror rc_sitedec_*: "sitedecisions" on every Ok.

fn policy_bind(ctx: &MwCtx, p: &Value) -> Result<String, OpErr> {
    let (dir, eff) = bound_profile_dir(ctx, p)?;
    store::lock()
        .set_data_dir(path_str(&dir)?)
        .map_err(OpErr::Failed)?;
    Ok(eff)
}

fn op_sitedec_get(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    let key = arg_str(p, "key")?;
    policy_bind(ctx, p)?;
    let v = sitedecisions::get(kind, key)?;
    Ok(json!({ "value": v }))
}

fn op_sitedec_set(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    let key = arg_str(p, "key")?;
    let value = arg_str(p, "value")?;
    let eff = policy_bind(ctx, p)?;
    let changed = sitedecisions::set(kind, key, value)?;
    let _hint = notify::profile_hint(&eff);
    notify::emit("sitedecisions");
    Ok(json!({ "ok": true, "changed": changed }))
}

fn op_sitedec_lookup(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    let host = arg_str(p, "host")?;
    policy_bind(ctx, p)?;
    let hit = sitedecisions::lookup(kind, host)?;
    match hit {
        Some((key, value)) => Ok(json!({ "key": key, "value": value })),
        None => Ok(json!({ "key": Value::Null, "value": Value::Null })),
    }
}

/// rc_sitedec_* command parity: an Ok write emits "sitedecisions"
/// whether or not a row changed (the flat lane emits on status, not
/// the changed flag it also computes — the response still reports
/// it for callers that care).
fn sitedec_emit(eff: &str, changed: bool) -> Value {
    let _hint = notify::profile_hint(eff);
    notify::emit("sitedecisions");
    json!({ "ok": true, "changed": changed })
}

fn op_sitedec_remove(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    let key = arg_str(p, "key")?;
    let eff = policy_bind(ctx, p)?;
    let changed = sitedecisions::remove(kind, key)?;
    Ok(sitedec_emit(&eff, changed))
}

fn op_sitedec_clear(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    let eff = policy_bind(ctx, p)?;
    let changed = sitedecisions::clear_kind(kind)?;
    Ok(sitedec_emit(&eff, changed))
}

/// entries is the {"key":"value",...} object replace_kind consumes —
/// the "in-memory list is authoritative" mirror-write.
fn op_sitedec_replace(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    let entries = p
        .get("entries")
        .filter(|v| v.is_object())
        .ok_or_else(|| OpErr::BadPayload("missing \"entries\" object".into()))?;
    let bytes = serde_json::to_vec(entries)
        .map_err(|e| OpErr::BadPayload(format!("entries doc: {e}")))?;
    let eff = policy_bind(ctx, p)?;
    let changed = sitedecisions::replace_kind(kind, &bytes)?;
    Ok(sitedec_emit(&eff, changed))
}

fn op_sitedec_list(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let kind = arg_str(p, "kind")?;
    policy_bind(ctx, p)?;
    let doc = sitedecisions::list_json(kind)?;
    Ok(json!({ "entries": serde_json::from_str(&doc).unwrap_or_else(|_| json!({})) }))
}

/// The whole store as one document — the payload IS the snapshot
/// (same shape rc_sitedec_snapshot returns as a string).
fn op_sitedec_snapshot(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    policy_bind(ctx, p)?;
    let doc = sitedecisions::snapshot_json()?;
    Ok(serde_json::from_str(&doc).unwrap_or(Value::Null))
}

fn op_sitedec_store_present(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (dir, _eff) = bound_profile_dir(ctx, p)?;
    store::lock()
        .set_data_dir(path_str(&dir)?)
        .map_err(OpErr::Failed)?;
    Ok(json!({ "present": sitedecisions::store_present() }))
}

/// Reload/reset emit nothing — the flat lane is a bare status
/// (rc_sitedec_reload/rc_sitedec_reset have no notify call).
fn op_sitedec_reload(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    policy_bind(ctx, p)?;
    sitedecisions::reload()?;
    Ok(json!({ "ok": true }))
}

fn op_sitedec_reset(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    policy_bind(ctx, p)?;
    sitedecisions::reset()?;
    Ok(json!({ "ok": true }))
}

// The AcPolicy handle also anchors the domain blocklist, tranco and
// site-trust stores under the bound profile dir — the envelope's
// policy_bind re-asserts the same dir per call.

fn op_blocklist_reload(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    policy_bind(ctx, p)?;
    blocklist::reload()?;
    Ok(json!({ "ok": true }))
}

fn op_tranco_file_entries(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    policy_bind(ctx, p)?;
    Ok(json!({ "entries": tranco::file_entries(name) }))
}

fn op_tranco_reload(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    policy_bind(ctx, p)?;
    tranco::reload()?;
    Ok(json!({ "ok": true }))
}

/// ac_sitetrust_lookup_opts parity: the composite verdict document.
/// The bound profile's history db opens beside the data dir — the
/// first-seen rollup must never read another persona's visits.
fn op_sitetrust_lookup(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let host = arg_str(p, "host")?;
    let is_https = p.get("https").and_then(Value::as_bool).unwrap_or(false);
    let probes = opt_u64(p, "probes")?.unwrap_or(0) as u32;
    let (dir, _eff) = bound_profile_dir(ctx, p)?;
    store::lock()
        .set_data_dir(path_str(&dir)?)
        .map_err(OpErr::Failed)?;
    let hist_db = path_str(&dir.join("history.db"))?.to_string();
    history::with(|h| h.open(Some(hist_db.as_str())))?;
    let doc = sitetrust::lookup_opts(host, is_https, probes);
    Ok(serde_json::from_str(&doc).unwrap_or(Value::Null))
}

const POLICY_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "sitedec_get",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_get,
    },
    Op {
        id: 0x0002,
        name: "sitedec_set",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_set,
    },
    Op {
        id: 0x0003,
        name: "sitedec_lookup",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_lookup,
    },
    Op {
        id: 0x0004,
        name: "sitedec_remove",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_remove,
    },
    Op {
        id: 0x0005,
        name: "sitedec_clear",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_clear,
    },
    Op {
        id: 0x0006,
        name: "sitedec_replace",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_replace,
    },
    Op {
        id: 0x0007,
        name: "sitedec_list",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_list,
    },
    Op {
        id: 0x0008,
        name: "sitedec_snapshot",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_snapshot,
    },
    Op {
        id: 0x0009,
        name: "sitedec_store_present",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_store_present,
    },
    Op {
        id: 0x000a,
        name: "sitedec_reload",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_reload,
    },
    Op {
        id: 0x000b,
        name: "sitedec_reset",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_sitedec_reset,
    },
    Op {
        id: 0x000c,
        name: "blocklist_reload",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_blocklist_reload,
    },
    Op {
        id: 0x000d,
        name: "tranco_file_entries",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_tranco_file_entries,
    },
    Op {
        id: 0x000e,
        name: "tranco_reload",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_tranco_reload,
    },
    Op {
        id: 0x000f,
        name: "sitetrust_lookup",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_sitetrust_lookup,
    },
];

// ---- session (domain 6) ------------------------------------------------------
//
// Path-bound like the AcSession handle: every op resolves
// profiles/<id>/session.dat under the core root — no process-global
// anchoring.  Session ops emit no change events by contract
// (per-run scratch state, MW-ARCH §5).

/// Resolves the bound profile's dir; the stamped id is the
/// effective profile (a non-slug/absent id resolves to "default").
fn session_dir(ctx: &MwCtx, p: &Value) -> Result<(PathBuf, String), OpErr> {
    let eff = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let root = std::path::Path::new(ctx.core.data_root());
    Ok((facade::profile_dir(root, &eff), eff))
}

/// The bound profile's session.dat.
fn session_file(ctx: &MwCtx, p: &Value) -> Result<(PathBuf, String), OpErr> {
    let (dir, eff) = session_dir(ctx, p)?;
    Ok((dir.join("session.dat"), eff))
}

fn op_session_exists(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (path, _pid) = session_file(ctx, p)?;
    Ok(json!({ "exists": session::exists_at(&path) }))
}

fn op_session_load(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (path, _pid) = session_file(ctx, p)?;
    let doc = session::load_at(&path)?;
    // The manifest is a JSON document by contract; hand it back as
    // the payload value itself.
    Ok(serde_json::from_slice(&doc).unwrap_or(Value::Null))
}

fn op_session_save(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (path, _pid) = session_file(ctx, p)?;
    let doc = p
        .get("session")
        .ok_or_else(|| OpErr::BadPayload("missing \"session\" doc".into()))?;
    let bytes = serde_json::to_vec(doc)
        .map_err(|e| OpErr::BadPayload(format!("session doc not serializable: {e}")))?;
    session::save_at(&path, &bytes)?;
    Ok(json!({ "ok": true }))
}

fn op_session_clear(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (path, _pid) = session_file(ctx, p)?;
    session::clear_at(&path)?;
    Ok(json!({ "ok": true }))
}

// The session.state lifecycle marker — the same _in(dir) ops
// ac_session_mark_* run on their bound profile dir.

fn op_session_mark_start(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (dir, _pid) = session_dir(ctx, p)?;
    let dirty = session::mark_start_in(&dir)?;
    Ok(json!({ "ok": true, "dirty": dirty }))
}

fn opt_i32(p: &Value, name: &str) -> Result<i32, OpErr> {
    let v = arg_i64(p, name)?;
    i32::try_from(v)
        .map_err(|_| OpErr::BadPayload(format!("\"{name}\" out of i32 range")))
}

fn op_session_mark_exit(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let code = opt_i32(p, "code")?;
    let (dir, _pid) = session_dir(ctx, p)?;
    session::mark_exit_in(&dir, code)?;
    Ok(json!({ "ok": true }))
}

fn op_session_mark_terminated(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let signal = opt_i32(p, "signal")?;
    let (dir, _pid) = session_dir(ctx, p)?;
    session::mark_terminated_in(&dir, signal)?;
    Ok(json!({ "ok": true }))
}

const SESSION_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "exists",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_session_exists,
    },
    Op {
        id: 0x0002,
        name: "load",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_session_load,
    },
    Op {
        id: 0x0003,
        name: "save",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_session_save,
    },
    Op {
        id: 0x0004,
        name: "clear",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_session_clear,
    },
    Op {
        id: 0x0005,
        name: "mark_start",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_session_mark_start,
    },
    Op {
        id: 0x0006,
        name: "mark_exit",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_session_mark_exit,
    },
    Op {
        id: 0x0007,
        name: "mark_terminated",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_session_mark_terminated,
    },
];

// ---- cookies (domain 7) --------------------------------------------------------
//
// Process-global store behind the open-path anchor, like history —
// the envelope binds profiles/<id>/cookies.db per call.  "cookies"
// emits only when the write actually changed a row (the
// SetOutcome::Noop rule rc_cookies_set keeps).

fn cookies_db(ctx: &MwCtx, p: &Value) -> Result<(String, String), OpErr> {
    let (dir, eff) = bound_profile_dir(ctx, p)?;
    Ok((path_str(&dir.join("cookies.db"))?.to_string(), eff))
}

fn container_arg<'a>(p: &'a Value) -> Result<Option<&'a str>, OpErr> {
    opt_str(p, "container")
}

fn op_cookies_count(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let container = container_arg(p)?;
    let (db, _eff) = cookies_db(ctx, p)?;
    let n = cookies::with(|s| -> error::RcResult<i64> {
        s.open(Some(db.as_str()))?;
        s.count(container)
    })?;
    Ok(json!({ "count": n }))
}

fn op_cookies_list(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let container = container_arg(p)?;
    let (db, _eff) = cookies_db(ctx, p)?;
    let rows = cookies::with(|s| -> error::RcResult<Vec<Value>> {
        s.open(Some(db.as_str()))?;
        s.list(container)
            .map(|v| v.iter().map(|c| c.to_json()).collect())
    })?;
    Ok(json!({ "cookies": rows }))
}

fn op_cookies_set(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let rec = p
        .get("record")
        .ok_or_else(|| OpErr::BadPayload("missing \"record\" doc".into()))?;
    let cookie = cookies::Cookie::from_json(rec)?;
    let (db, eff) = cookies_db(ctx, p)?;
    let outcome = cookies::with(|s| -> error::RcResult<cookies::SetOutcome> {
        s.open(Some(db.as_str()))?;
        s.set(&cookie)
    })?;
    let changed = outcome != cookies::SetOutcome::Noop;
    if changed {
        let _hint = notify::profile_hint(&eff);
        notify::emit("cookies");
    }
    let name = match outcome {
        cookies::SetOutcome::Stored => "stored",
        cookies::SetOutcome::Deleted => "deleted",
        cookies::SetOutcome::Noop => "noop",
    };
    Ok(json!({ "ok": true, "outcome": name }))
}

const COOKIES_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "count",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_cookies_count,
    },
    Op {
        id: 0x0002,
        name: "list",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_cookies_list,
    },
    Op {
        id: 0x0003,
        name: "set",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_cookies_set,
    },
];

// ---- icons (domain 8) ------------------------------------------------------------
//
// Fully path-explicit (cache/icons/ under the bound profile dir) —
// no process-global anchor, same shape as the session ops.  The PNG
// crosses the payload base64'd ("png_b64").

fn icons_dir(ctx: &MwCtx, p: &Value) -> Result<(PathBuf, String), OpErr> {
    bound_profile_dir(ctx, p)
}

fn op_icons_count(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (dir, _eff) = icons_dir(ctx, p)?;
    Ok(json!({ "count": icons::count_in(&dir)? }))
}

fn op_icons_put(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let url = arg_str(p, "url")?;
    let png = arg_b64(p, "png_b64")?;
    let (dir, eff) = icons_dir(ctx, p)?;
    let changed = icons::put_in(&dir, url, &png)?;
    if changed {
        let _hint = notify::profile_hint(&eff);
        notify::emit("icons");
    }
    Ok(json!({ "ok": true, "changed": changed }))
}

fn op_icons_get(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let url = arg_str(p, "url")?;
    let (dir, _eff) = icons_dir(ctx, p)?;
    let png = icons::get_in(&dir, url)?;
    Ok(json!({ "png_b64": b64_encode(&png) }))
}

/// rc_icons_clear parity: emit only when the cache actually held
/// rows — an already-empty wipe is silent.
fn op_icons_clear(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (dir, eff) = icons_dir(ctx, p)?;
    let had = icons::count_in(&dir).unwrap_or(0) > 0;
    icons::clear_in(&dir)?;
    if had {
        let _hint = notify::profile_hint(&eff);
        notify::emit("icons");
    }
    Ok(json!({ "ok": true }))
}

/// rc_icons_sweep parity: drop records stale at-or-before cutoff_ms,
/// report the count, emit only when something went.
fn op_icons_sweep(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let cutoff_ms = arg_i64(p, "cutoff_ms")?;
    let (dir, eff) = icons_dir(ctx, p)?;
    let removed = icons::sweep_in(&dir, cutoff_ms)?;
    if removed > 0 {
        let _hint = notify::profile_hint(&eff);
        notify::emit("icons");
    }
    Ok(json!({ "ok": true, "removed": removed }))
}

const ICONS_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "count",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_icons_count,
    },
    Op {
        id: 0x0002,
        name: "put",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_icons_put,
    },
    Op {
        id: 0x0003,
        name: "get",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_icons_get,
    },
    Op {
        id: 0x0004,
        name: "clear",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_icons_clear,
    },
    Op {
        id: 0x0005,
        name: "sweep",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_icons_sweep,
    },
];

// ---- containers (domain 9) ---------------------------------------------------------
//
// The registry is a typed specialization over the profile scope's
// settings.json — a short-lived RcSettings per call, the AcContainers
// precedent the settings handlers already follow.

fn containers_scope(ctx: &MwCtx, p: &Value) -> Result<(settings::RcSettings, String), OpErr> {
    let eff = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let s = settings::RcSettings::open(ctx.core.data_root(), Some(eff.as_str()))?;
    Ok((s, eff))
}

fn op_containers_list(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (s, _eff) = containers_scope(ctx, p)?;
    let rows: Vec<Value> = containers::list(&s)?
        .iter()
        .map(containers::to_json)
        .collect();
    Ok(json!({ "containers": rows }))
}

fn op_containers_create(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let color = opt_str(p, "color")?.unwrap_or("");
    let icon = opt_str(p, "icon")?;
    let (s, eff) = containers_scope(ctx, p)?;
    let c = containers::create(&s, name, color, icon)?;
    let _hint = notify::profile_hint(&eff);
    notify::emit("containers");
    Ok(containers::to_json(&c))
}

const CONTAINERS_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "list",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_containers_list,
    },
    Op {
        id: 0x0002,
        name: "create",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_containers_create,
    },
];

// ---- notes (domain 10) -------------------------------------------------------------
//
// profiles/<id>/notes.json — RcNotes drops flush a dirty store, so a
// per-call handle keeps the debounce contract without the envelope
// inventing a second sync rule.

fn notes_handle(ctx: &MwCtx, p: &Value) -> Result<(notes::RcNotes, String), OpErr> {
    let eff = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let h = notes::RcNotes::open(ctx.core.data_root(), Some(eff.as_str()))?;
    Ok((h, eff))
}

fn op_notes_count(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (h, _eff) = notes_handle(ctx, p)?;
    Ok(json!({ "count": h.count() }))
}

fn op_notes_list(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let (h, _eff) = notes_handle(ctx, p)?;
    let rows: Vec<Value> = h
        .list()
        .iter()
        .map(|n| {
            json!({
                "title": n.title,
                "body": n.body,
                "markdown": n.markdown,
            })
        })
        .collect();
    Ok(json!({ "notes": rows }))
}

fn op_notes_insert(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let row = arg_i64(p, "row")?;
    let title = opt_str(p, "title")?.unwrap_or("");
    let body = opt_str(p, "body")?.unwrap_or("");
    let (h, eff) = notes_handle(ctx, p)?;
    let at = h.insert(row, title, body)?;
    let _hint = notify::profile_hint(&eff);
    notify::emit("notes");
    Ok(json!({ "ok": true, "row": at }))
}

const NOTES_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "count",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_notes_count,
    },
    Op {
        id: 0x0002,
        name: "list",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_notes_list,
    },
    Op {
        id: 0x0003,
        name: "insert",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_notes_insert,
    },
];

// ---- custody (domain 11) ------------------------------------------------------------
//
// The credential map — profile-dir custody like the flat rc_cred_*
// callers get after rustCoreEnsureDataDirFor: the envelope re-anchors
// the shared Store per call (a dir switch drops cached keys, the
// documented re-assert cost).  The unlocked-custody *handle* itself
// stays flat (MW-ARCH §2.3) — these ops are the self-contained
// read/write slice.

fn custody_bind(ctx: &MwCtx, p: &Value) -> Result<String, OpErr> {
    let (dir, eff) = bound_profile_dir(ctx, p)?;
    store::lock()
        .set_data_dir(path_str(&dir)?)
        .map_err(OpErr::Failed)?;
    Ok(eff)
}

fn op_custody_state(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    custody_bind(ctx, p)?;
    let mut s = store::lock();
    Ok(json!({
        "available": s.is_available(),
        "unlocked": s.is_unlocked(),
        "passphrase_enabled": s.passphrase_enabled(),
    }))
}

fn op_custody_cred_names(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    custody_bind(ctx, p)?;
    let names = store::lock().cred_names()?;
    Ok(json!({ "names": names }))
}

fn op_custody_cred_put(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let value = arg_b64(p, "value_b64")?;
    let eff = custody_bind(ctx, p)?;
    let changed = store::lock().cred_put(name, &value)?;
    if changed {
        let _hint = notify::profile_hint(&eff);
        notify::emit("credentials");
    }
    Ok(json!({ "ok": true, "changed": changed }))
}

const CUSTODY_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "state",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_custody_state,
    },
    Op {
        id: 0x0002,
        name: "cred_names",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_custody_cred_names,
    },
    Op {
        id: 0x0003,
        name: "cred_put",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_custody_cred_put,
    },
];

// ---- opensearch (domain 12) ----------------------------------------------------------
//
// searchengines.json lives under profiles/<eff>/ — the flat lane's
// rustCoreEnsureDataDir() anchors the ACTIVE profile's dir, and the
// engine set + keyword bindings + bundled blocklist are per-persona
// state.  The envelope binds the same dir per call; "profile"
// carries the scope like the other persona-bound domains.

fn opensearch_bind(ctx: &MwCtx, p: &Value) -> Result<String, OpErr> {
    let (dir, eff) = bound_profile_dir(ctx, p)?;
    store::lock()
        .set_data_dir(path_str(&dir)?)
        .map_err(OpErr::Failed)?;
    Ok(eff)
}

fn op_ose_list(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    opensearch_bind(ctx, p)?;
    let doc = opensearch::list_json()?;
    // The store answers a bare name array — wrap it like the other
    // list ops so every domain's payload is a self-describing doc.
    let names = serde_json::from_str(&doc).unwrap_or_else(|_| json!([]));
    Ok(json!({ "engines": names }))
}

fn op_ose_put(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let engine = p
        .get("engine")
        .ok_or_else(|| OpErr::BadPayload("missing \"engine\" doc".into()))?;
    let bytes = serde_json::to_vec(engine)
        .map_err(|e| OpErr::BadPayload(format!("engine doc: {e}")))?;
    let eff = opensearch_bind(ctx, p)?;
    let changed = opensearch::put_json(&bytes)?;
    if changed {
        let _hint = notify::profile_hint(&eff);
        notify::emit("searchengines");
    }
    Ok(json!({ "ok": true, "changed": changed }))
}

/// rc_ose_* command parity: "searchengines" emits only when the
/// store reports a real change, scoped to the bound persona like
/// the other profile stores.
fn ose_changed(eff: &str, changed: bool) -> Value {
    if changed {
        let _hint = notify::profile_hint(eff);
        notify::emit("searchengines");
    }
    json!({ "ok": true, "changed": changed })
}

/// A store-side JSON-array string re-parsed into a payload field —
/// the list_json-family functions answer serialized arrays.
fn parse_json_field(doc: &str) -> Value {
    serde_json::from_str(doc).unwrap_or_else(|_| json!([]))
}

fn op_ose_store_present(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    opensearch_bind(ctx, p)?;
    Ok(json!({ "present": opensearch::store_present() }))
}

/// One engine record — {"engine": doc} or null when absent (the
/// flat lane's NULL record), never an error on a miss.
fn op_ose_get(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    opensearch_bind(ctx, p)?;
    match opensearch::get_json(name)? {
        Some(doc) => Ok(json!({ "engine": serde_json::from_str(&doc)
            .unwrap_or(Value::Null) })),
        None => Ok(json!({ "engine": Value::Null })),
    }
}

fn op_ose_remove(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::remove(name)?))
}

fn op_ose_rename(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let old = arg_str(p, "old")?;
    let new = arg_str(p, "new")?;
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::rename(old, new)?))
}

/// order is the display-order name array reorder() consumes.
fn op_ose_reorder(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let order = p
        .get("order")
        .filter(|v| v.is_array())
        .ok_or_else(|| OpErr::BadPayload("missing \"order\" array".into()))?;
    let bytes = serde_json::to_vec(order)
        .map_err(|e| OpErr::BadPayload(format!("order doc: {e}")))?;
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::reorder(&bytes)?))
}

/// keywords is the string array set_keywords consumes — the store
/// strips the listed keywords off every other engine.
fn op_ose_set_keywords(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let keywords = p
        .get("keywords")
        .filter(|v| v.is_array())
        .ok_or_else(|| OpErr::BadPayload("missing \"keywords\" array".into()))?;
    let bytes = serde_json::to_vec(keywords)
        .map_err(|e| OpErr::BadPayload(format!("keywords doc: {e}")))?;
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::set_keywords(name, &bytes)?))
}

/// xml / xml_b64 carry the OpenSearch 1.1 descriptor — the parser
/// handles arbitrary encodings, so the b64 spelling keeps non-UTF-8
/// descriptors byte-exact like the flat lane's raw bytes.
fn op_ose_import(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let bytes = match opt_str(p, "xml")? {
        Some(s) => s.as_bytes().to_vec(),
        None => arg_b64(p, "xml_b64")?,
    };
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::import_descriptor(&bytes)?))
}

fn op_ose_engine_for_keyword(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let keyword = arg_str(p, "keyword")?;
    opensearch_bind(ctx, p)?;
    match opensearch::engine_for_keyword(keyword)? {
        Some(name) => Ok(json!({ "name": name })),
        None => Ok(json!({ "name": Value::Null })),
    }
}

fn op_ose_keywords(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    opensearch_bind(ctx, p)?;
    Ok(json!({ "keywords": parse_json_field(&opensearch::keywords_json()?) }))
}

fn op_ose_removed_bundled(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    opensearch_bind(ctx, p)?;
    Ok(json!({ "names": parse_json_field(&opensearch::removed_bundled_json()?) }))
}

fn op_ose_block_bundled(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::block_bundled(name)?))
}

fn op_ose_unblock_bundled(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::unblock_bundled(name)?))
}

fn op_ose_bundled_names(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    opensearch_bind(ctx, p)?;
    Ok(json!({ "names": parse_json_field(&opensearch::bundled_names_json()) }))
}

fn op_ose_seed_bundled(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::seed_bundled()?))
}

fn op_ose_restore_bundled(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let eff = opensearch_bind(ctx, p)?;
    Ok(ose_changed(&eff, opensearch::restore_bundled()?))
}

/// spec is the buildUrl/parseTemplate spec doc
/// {"template","method","params","term","language","source"} —
/// expand_json's contract, null on a bad spec.
fn op_ose_expand(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let spec = p
        .get("spec")
        .filter(|v| v.is_object())
        .ok_or_else(|| OpErr::BadPayload("missing \"spec\" object".into()))?;
    let bytes = serde_json::to_vec(spec)
        .map_err(|e| OpErr::BadPayload(format!("spec doc: {e}")))?;
    opensearch_bind(ctx, p)?;
    match opensearch::expand_json(&bytes) {
        Some(url) => Ok(json!({ "url": url })),
        None => Ok(json!({ "url": Value::Null })),
    }
}

/// Engine + slot kind + term -> the expanded URL, null on absence —
/// term/language/source default "" like the flat lane's NULL args.
fn op_ose_url(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let name = arg_str(p, "name")?;
    let kind = opt_str(p, "kind")?.unwrap_or("");
    let term = opt_str(p, "term")?.unwrap_or("");
    let language = opt_str(p, "language")?.unwrap_or("");
    let source = opt_str(p, "source")?.unwrap_or("");
    opensearch_bind(ctx, p)?;
    match opensearch::engine_url(name, kind, term, language, source)? {
        Some(url) => Ok(json!({ "url": url })),
        None => Ok(json!({ "url": Value::Null })),
    }
}

fn op_ose_keyword_url(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let keyword = arg_str(p, "keyword")?;
    let term = opt_str(p, "term")?.unwrap_or("");
    let language = opt_str(p, "language")?.unwrap_or("");
    let source = opt_str(p, "source")?.unwrap_or("");
    opensearch_bind(ctx, p)?;
    match opensearch::keyword_url(keyword, term, language, source)? {
        Some(url) => Ok(json!({ "url": url })),
        None => Ok(json!({ "url": Value::Null })),
    }
}

/// rc_ose_reset parity — the test-suite reset seam emits nothing
/// (the flat spelling is a bare status too).
fn op_ose_reset(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    opensearch_bind(ctx, p)?;
    opensearch::reset()?;
    Ok(json!({ "ok": true }))
}

const OPENSEARCH_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "list",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_list,
    },
    Op {
        id: 0x0002,
        name: "put",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_put,
    },
    Op {
        id: 0x0003,
        name: "store_present",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_store_present,
    },
    Op {
        id: 0x0004,
        name: "get",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_get,
    },
    Op {
        id: 0x0005,
        name: "remove",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_remove,
    },
    Op {
        id: 0x0006,
        name: "rename",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_rename,
    },
    Op {
        id: 0x0007,
        name: "reorder",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_reorder,
    },
    Op {
        id: 0x0008,
        name: "set_keywords",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_set_keywords,
    },
    Op {
        id: 0x0009,
        name: "import",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_import,
    },
    Op {
        id: 0x000a,
        name: "engine_for_keyword",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_engine_for_keyword,
    },
    Op {
        id: 0x000b,
        name: "keywords",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_keywords,
    },
    Op {
        id: 0x000c,
        name: "removed_bundled",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_removed_bundled,
    },
    Op {
        id: 0x000d,
        name: "block_bundled",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_block_bundled,
    },
    Op {
        id: 0x000e,
        name: "unblock_bundled",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_unblock_bundled,
    },
    Op {
        id: 0x000f,
        name: "bundled_names",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_bundled_names,
    },
    Op {
        id: 0x0010,
        name: "seed_bundled",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_seed_bundled,
    },
    Op {
        id: 0x0011,
        name: "restore_bundled",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_restore_bundled,
    },
    Op {
        id: 0x0012,
        name: "expand",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_expand,
    },
    Op {
        id: 0x0013,
        name: "url",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_url,
    },
    Op {
        id: 0x0014,
        name: "keyword_url",
        class: OpClass::Query,
        profile_scoped: true,
        since: 1,
        handler: op_ose_keyword_url,
    },
    Op {
        id: 0x0015,
        name: "reset",
        class: OpClass::Command,
        profile_scoped: true,
        since: 1,
        handler: op_ose_reset,
    },
];

// ---- updates (domain 13) --------------------------------------------------------------
//
// Update-check policy/state — a GLOBAL-scope settings specialization
// (updates::read_state's own contract: profile-bound handles would
// silently scope the keys to the persona, which nothing writes).

fn op_updates_state(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let s = settings::RcSettings::open(ctx.core.data_root(), None)?;
    Ok(updates::read_state(&s)?.to_json())
}

fn op_updates_record_check(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let now_ms = arg_i64(p, "now_ms")?;
    let s = settings::RcSettings::open(ctx.core.data_root(), None)?;
    updates::record_check(&s, now_ms)?;
    notify::emit("updates");
    Ok(json!({ "ok": true }))
}

const UPDATES_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "state",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_updates_state,
    },
    Op {
        id: 0x0002,
        name: "record_check",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_updates_record_check,
    },
];

// ---- services (domain 14) -----------------------------------------------------
//
// Stateless queries/commands — no store, no profile, no emits.

fn op_classify_input(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let input = arg_str(p, "input")?;
    let keywords: Vec<String> = p
        .get("keywords")
        .and_then(Value::as_array)
        .map(|a| {
            a.iter()
                .filter_map(|k| k.as_str().map(String::from))
                .collect()
        })
        .unwrap_or_default();
    let search_fallback = p
        .get("search_fallback")
        .and_then(Value::as_bool)
        .unwrap_or(true);
    // The rc_classify_input argument contract, verbatim.
    Ok(omnibox::classify(input, &keywords, search_fallback))
}

/// The second-opinion TLS probe — a COMMAND, not a query: it performs
/// a live handshake against host:port (MW-ARCH §1's "submits work to
/// a live channel"), the one honest network exception the keep-out
/// list grants.  The verdict JSON reports every outcome — a refused
/// or unreachable target is still an Ok response, never an op fault.
fn op_tls_probe(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let host = arg_str(p, "host")?;
    let port = arg_i64(p, "port")?;
    if !(1..=65535).contains(&port) {
        return bad_payload("\"port\" must be 1..=65535");
    }
    let flags = opt_u64(p, "flags")?.unwrap_or(0) as u32;
    Ok(tlsprobe::check(host, port as u16, flags))
}

const SERVICES_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "classify_input",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_classify_input,
    },
    Op {
        id: 0x0002,
        name: "tls_probe",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_tls_probe,
    },
];

// ---- extverify (domain 15) -------------------------------------------------------
//
// Extension package verification — pure transform, so the domain is
// query-only BY DESIGN: a verification owns no state and writing a
// fake "command" would be table-padding.  Command-class requests
// correctly refuse with unknown_op.

fn op_ext_manifest_check(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let data = arg_b64(p, "data_b64")?;
    let report = extverify::manifest_check(&data)?;
    // The verdict is a JSON document; hand it back as the payload.
    Ok(serde_json::from_slice(&report).unwrap_or(Value::Null))
}

const EXTVERIFY_OPS: &[Op] = &[Op {
    id: 0x0001,
    name: "manifest_check",
    class: OpClass::Query,
    profile_scoped: false,
    since: 1,
    handler: op_ext_manifest_check,
}];

// ---- vault (domain 16) -------------------------------------------------------------
//
// The rv_* archive ops — path+passphrase self-contained calls, no
// handle state (the *unlocked custody* handle is what §2.3 keeps
// flat, and it lives in the custody domain anyway).  Paths are
// explicit by design: vault ops name their own dir/blob, the
// envelope does not bind them under profiles/.

fn arg_path<'a>(p: &'a Value, name: &str) -> Result<PathBuf, OpErr> {
    Ok(PathBuf::from(arg_str(p, name)?))
}

fn op_vault_inspect(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let path = arg_path(p, "path")?;
    let doc = profilevault::inspect_file(&path)?;
    Ok(serde_json::from_str(&doc).unwrap_or(Value::Null))
}

fn op_vault_seal(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let dir = arg_path(p, "dir")?;
    let out = arg_path(p, "out")?;
    let pass = arg_b64(p, "pass_b64")?;
    let algo = opt_u64(p, "algo")?.unwrap_or(1) as u8;
    let mut progress = profilevault::Progress::new(None, ptr::null_mut());
    profilevault::seal_dir(&dir, &pass, algo, &out, &mut progress)?;
    Ok(json!({ "ok": true, "out": out.to_string_lossy() }))
}

fn op_vault_verify(_ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let path = arg_path(p, "path")?;
    let pass = arg_b64(p, "pass_b64")?;
    let mut progress = profilevault::Progress::new(None, ptr::null_mut());
    profilevault::verify_file(&path, &pass, &mut progress)?;
    Ok(json!({ "ok": true }))
}

const VAULT_OPS: &[Op] = &[
    Op {
        id: 0x0001,
        name: "inspect",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_vault_inspect,
    },
    Op {
        id: 0x0002,
        name: "seal",
        class: OpClass::Command,
        profile_scoped: false,
        since: 1,
        handler: op_vault_seal,
    },
    Op {
        id: 0x0003,
        name: "verify",
        class: OpClass::Query,
        profile_scoped: false,
        since: 1,
        handler: op_vault_verify,
    },
];

// ---- sync (domain 19) -------------------------------------------------------------
//
// SYNC01: the serverless sync engine core — device-global by design
// (the sync group spans the device, not one persona), so the engine
// opens on the core's data root and every op is profile_scoped=false.
// Wire ops cross the envelope as the JSON docs sync::WireOp emits;
// raw bytes (pubkeys, signatures) ride the _b64 convention.
//
//   identity        — this device's id + pubkey (query)
//   members_list    — paired devices (query)
//   member_add      — pair a device: pubkey + onion addr (command)
//   member_remove   — unpair (command)
//   member_seen     — bump last-seen (command)
//   append          — the op-log tailing seam: sign + log a CRDT op
//                     for one synced store (command)
//   export          — ops after a seq, the transport drain (query)
//   import          — verify/dedup/apply a batch of remote ops
//                     (command)
//   stats           — engine counters incl. per-store heads (query)
//   drain           — retry the ops a store adapter hasn't consumed
//                     yet (command)
//
// "sync" emits on member changes, local appends, and imports that
// accepted new ops — transports and the device-management UI
// (SYNC09) hang off it.

fn sync_engine(ctx: &MwCtx) -> Result<sync::SyncEngine, OpErr> {
    // SYNC04/05/06: keep the store-adapter apply lanes bound to the
    // core this call is for — the applier seam is process-global.
    syncbookmarks::install(ctx.core.data_root());
    synchistory::install(ctx.core.data_root());
    synctabs::install(ctx.core.data_root());
    sync::SyncEngine::open(ctx.core.data_root()).map_err(OpErr::from)
}

fn member_json(m: &sync::Member) -> Value {
    json!({
        "device_id": m.device_id,
        "pubkey_b64": b64_encode(&m.pubkey),
        "onion_addr": m.onion_addr,
        "last_seen_ms": m.last_seen_ms,
        "added_ms": m.added_ms,
        "scopes": m.scopes,
    })
}

fn op_sync_identity(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let e = sync_engine(ctx)?;
    Ok(json!({
        "device_id": e.device_id(),
        "pubkey_b64": b64_encode(&e.public_key()),
    }))
}

fn op_sync_members_list(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let e = sync_engine(ctx)?;
    let members: Vec<Value> = e.members_list()?.iter().map(member_json).collect();
    Ok(json!({ "members": members }))
}

fn op_sync_member_add(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let pk_b64 = arg_str(p, "pubkey_b64")?;
    let pk = b64_decode(pk_b64)
        .ok_or_else(|| OpErr::BadPayload("\"pubkey_b64\" is not valid base64".into()))?;
    let pk: [u8; 32] = pk
        .try_into()
        .map_err(|_| OpErr::BadPayload("\"pubkey_b64\" must decode to 32 bytes".into()))?;
    let onion = opt_str(p, "onion_addr")?.unwrap_or("");
    let scope = opt_str(p, "scope")?.unwrap_or("");
    let e = sync_engine(ctx)?;
    let id = e.member_add(&pk, onion, scope)?;
    notify::emit("sync");
    Ok(json!({ "ok": true, "device_id": id }))
}

fn op_sync_member_remove(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_str(p, "device_id")?;
    let e = sync_engine(ctx)?;
    let removed = e.member_remove(id)?;
    if removed {
        notify::emit("sync");
    }
    Ok(json!({ "ok": true, "removed": removed }))
}

fn op_sync_member_seen(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_str(p, "device_id")?;
    let ts = opt_i64(p, "last_seen_ms")?;
    let e = sync_engine(ctx)?;
    e.member_seen(id, ts)?;
    notify::emit("sync");
    Ok(json!({ "ok": true }))
}

fn op_sync_append(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let store_name = arg_str(p, "store")?;
    let scope = opt_str(p, "scope")?.unwrap_or("");
    let body = p
        .get("body")
        .ok_or_else(|| OpErr::BadPayload("missing \"body\" doc".into()))?;
    let e = sync_engine(ctx)?;
    let op = e.append(store_name, scope, body)?;
    notify::emit("sync");
    Ok(json!({
        "ok": true,
        "op_id": op.op_id,
        "lamport": op.lamport,
    }))
}

fn op_sync_export(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let store_name = opt_str(p, "store")?;
    let since = opt_i64(p, "since_seq")?.unwrap_or(0);
    let e = sync_engine(ctx)?;
    let ops: Vec<Value> = e
        .export(store_name, since)?
        .iter()
        .map(|o| o.to_json())
        .collect();
    Ok(json!({ "ops": ops }))
}

fn op_sync_import(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let docs = match p.get("ops") {
        Some(Value::Array(a)) => a.clone(),
        Some(_) => return bad_payload("\"ops\" must be an array"),
        None => return bad_payload("missing \"ops\" array"),
    };
    let e = sync_engine(ctx)?;
    let rep = e.import(&docs)?;
    if rep.accepted > 0 {
        notify::emit("sync");
    }
    Ok(json!({
        "ok": true,
        "accepted": rep.accepted,
        "applied": rep.applied,
        "deferred": rep.deferred,
        "duplicates": rep.duplicates,
        "rejected": rep.rejected.iter().map(|r| json!({
            "op_id": r.op_id, "reason": r.reason
        })).collect::<Vec<_>>(),
        "apply_errors": rep.apply_errors.iter().map(|r| json!({
            "op_id": r.op_id, "reason": r.reason
        })).collect::<Vec<_>>(),
    }))
}

/// Retry lane for ops a store adapter has not consumed yet —
/// pending count is read first so the response can report it.
fn op_sync_drain(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let store_name = arg_str(p, "store")?;
    let e = sync_engine(ctx)?;
    let pending = e.pending_apply(store_name)?.len();
    let rep = e.drain_pending(store_name)?;
    Ok(json!({
        "ok": true,
        "pending_before": pending,
        "applied": rep.applied,
        "deferred": rep.deferred,
        "apply_errors": rep.apply_errors.iter().map(|r| json!({
            "op_id": r.op_id, "reason": r.reason
        })).collect::<Vec<_>>(),
    }))
}

fn op_sync_stats(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    sync_engine(ctx)?.stats().map_err(OpErr::from)
}


// SYNC02: the transport half of the domain — same SyncTransport
// seam for the embedded-Arti "tor" backend (feature synctor) and the
// hermetic "mem" loopback.  The pull model is torctl's: inbound
// activity lands as events drained by transport_poll, announced via
// notify::emit("synctransport").
//
//   transport_start      {backend:"tor"|"mem", name?} — boot the
//                        listener; idempotent (command)
//   transport_stop       — tear it down (command)
//   transport_status     — running/endpoint/grants/link count
//                        (query)
//   transport_authorize  {nickname, discovery_key?} — grant the peer
//                        Tor-layer client auth (command)
//   transport_deauthorize{nickname} — revoke (command)
//   transport_keygen     {onion} — our public discovery key for the
//                        peer's service; keystore-only, works with
//                        the transport down (command)
//   transport_connect    {endpoint, nickname?} — open an outbound
//                        link (command)
//   transport_send       {link_id, payload_b64} — one frame
//                        (command)
//   transport_close      {link_id} — drop a link (command)
//   transport_poll       — drain the event queue (query)

fn sync_transport(
    ctx: &MwCtx,
) -> Result<std::sync::Arc<dyn synctransport::SyncTransport>, OpErr> {
    synctransport::running(Path::new(ctx.core.data_root())).ok_or_else(|| {
        error::Fail {
            status: RcStatus::NotEnabled,
            msg: "sync transport is not started".into(),
        }
        .into()
    })
}

fn op_sync_transport_start(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let backend = opt_str(p, "backend")?.unwrap_or("tor");
    let name = opt_str(p, "name")?.unwrap_or("");
    if backend == "mem" && name.is_empty() {
        return bad_payload("\"name\" is required for the mem backend");
    }
    let t = synctransport::start(Path::new(ctx.core.data_root()), backend, name)?;
    notify::emit("sync");
    Ok(json!({
        "ok": true,
        "backend": t.backend(),
        "endpoint": t.endpoint()?,
    }))
}

fn op_sync_transport_stop(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let stopped = synctransport::stop(Path::new(ctx.core.data_root()));
    Ok(json!({ "ok": true, "stopped": stopped }))
}

fn op_sync_transport_status(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let root = Path::new(ctx.core.data_root());
    match synctransport::running(root) {
        Some(t) => Ok(json!({
            "running": true,
            "backend": t.backend(),
            "endpoint": t.endpoint()?,
            "authorized": t.authorized(),
            "links": t.link_count(),
        })),
        None => Ok(json!({
            "running": false,
            "authorized": synctransport::authorized_set(&root.join(sync::SYNC_DIR))
                .keys().cloned().collect::<Vec<_>>(),
        })),
    }
}

fn op_sync_transport_authorize(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let nick = arg_str(p, "nickname")?;
    let key = opt_str(p, "discovery_key")?.unwrap_or("");
    sync_transport(ctx)?.authorize(nick, key)?;
    notify::emit("sync");
    Ok(json!({ "ok": true }))
}

fn op_sync_transport_deauthorize(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let nick = arg_str(p, "nickname")?;
    let removed = sync_transport(ctx)?.deauthorize(nick)?;
    if removed {
        notify::emit("sync");
    }
    Ok(json!({ "ok": true, "removed": removed }))
}

fn op_sync_transport_keygen(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let onion = arg_str(p, "onion")?;
    let key = synctransport::discovery_key(Path::new(ctx.core.data_root()), onion)?;
    Ok(json!({ "ok": true, "discovery_key": key }))
}

fn op_sync_transport_connect(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let endpoint = arg_str(p, "endpoint")?;
    let nick = opt_str(p, "nickname")?.unwrap_or("self");
    let id = sync_transport(ctx)?.connect(endpoint, nick)?;
    Ok(json!({ "ok": true, "link_id": id }))
}

fn op_sync_transport_send(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let link = arg_i64(p, "link_id")? as u64;
    let payload = b64_decode(arg_str(p, "payload_b64")?)
        .ok_or_else(|| OpErr::BadPayload("\"payload_b64\" is not valid base64".into()))?;
    sync_transport(ctx)?.send(link, &payload)?;
    Ok(json!({ "ok": true }))
}

fn op_sync_transport_close(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let link = arg_i64(p, "link_id")? as u64;
    let removed = sync_transport(ctx)?.close_link(link)?;
    Ok(json!({ "ok": true, "removed": removed }))
}

fn op_sync_transport_poll(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let evs: Vec<Value> = sync_transport(ctx)?
        .poll()
        .iter()
        .map(|e| match e {
            synctransport::TransportEvent::Ready => json!({"type": "ready"}),
            synctransport::TransportEvent::Bootstrap(pct) => {
                json!({"type": "bootstrap", "pct": pct})
            }
            synctransport::TransportEvent::Incoming { link, from } => {
                json!({"type": "incoming", "link_id": link, "from": from})
            }
            synctransport::TransportEvent::Frame { link, payload } => json!({
                "type": "frame",
                "link_id": link,
                "payload_b64": b64_encode(payload),
            }),
            synctransport::TransportEvent::LinkClosed { link, reason } => {
                json!({"type": "closed", "link_id": link, "reason": reason})
            }
            synctransport::TransportEvent::Failed { reason } => {
                json!({"type": "failed", "reason": reason})
            }
        })
        .collect();
    Ok(json!({ "events": evs }))
}

// SYNC03: pairing — the ceremony that turns two devices into one
// profile's sync group.  An invite is a compact one-time payload
// (QR text or grouped-base32 manual code): inviter onion, invite
// token, short device-key hash.  The token doubles as the Noise
// prologue and the derived one-time restricted-discovery key —
// that is how a not-yet-authorized joiner reaches the gated onion
// exactly once.  Noise_XX then authenticates both statics, the
// Ed25519 device signatures authenticate the statics, and the
// transcript hash becomes the 6-word/6-emoji fingerprint both
// users compare.  Only after BOTH confirm does the profile's
// group secret + the inviter's discovery key cross the channel,
// and the peer lands in member_scopes bound to that profile —
// never a neighbour's group.
//
//   pair_begin   {profile?} — mint the invite + session; returns
//                {pair_id, qr, code, endpoint, scope} (command)
//   pair_join    {payload} — QR text or manual code; connects,
//                starts the handshake (command)
//   pair_poll    {pair_id?} — drain transport events, advance
//                sessions, report state (+ fingerprint when ready)
//                (command — it mutates the state machine)
//   pair_confirm {pair_id, ok} — the fingerprint decision
//                (command)
//   pair_cancel  {pair_id} — abort best-effort (command)
//   pair_list    — live sessions (query)

fn op_sync_pair_begin(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let scope = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let root = std::path::Path::new(ctx.core.data_root());
    syncpair::invite_begin(root, &scope).map_err(OpErr::from)
}

fn op_sync_pair_join(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let payload = arg_str(p, "payload")?;
    let root = std::path::Path::new(ctx.core.data_root());
    let id = syncpair::join_begin(root, payload)?;
    Ok(json!({ "ok": true, "pair_id": id }))
}

fn op_sync_pair_poll(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = opt_i64(p, "pair_id")?.unwrap_or(0) as u64;
    let root = std::path::Path::new(ctx.core.data_root());
    syncpair::poll(root, id).map_err(OpErr::from)
}

fn op_sync_pair_confirm(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_i64(p, "pair_id")? as u64;
    let ok = p
        .get("ok")
        .and_then(Value::as_bool)
        .ok_or_else(|| OpErr::BadPayload("missing \"ok\" bool".into()))?;
    let root = std::path::Path::new(ctx.core.data_root());
    syncpair::confirm(root, id, ok)?;
    Ok(json!({ "ok": true }))
}

fn op_sync_pair_cancel(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let id = arg_i64(p, "pair_id")? as u64;
    let root = std::path::Path::new(ctx.core.data_root());
    syncpair::cancel(root, id)?;
    Ok(json!({ "ok": true }))
}

fn op_sync_pair_list(ctx: &MwCtx, _p: &Value) -> Result<Value, OpErr> {
    let root = std::path::Path::new(ctx.core.data_root());
    Ok(json!({ "sessions": syncpair::list(root) }))
}

// SYNC06: open-tabs presence — an ephemeral per-device doc on the
// "tabs" store, NOT a durable CRDT.  tabs_publish signs the caller's
// whole tab set as one presence doc (the Qt side snapshots its
// windows; an empty windows array is a real publish — the
// "everything closed" wipe); tabs_peers reports the newest doc every
// OTHER device published for the scope.  The apply lane keeps docs
// in memory and writes no store — a peer's open tabs are a
// read-only view, never a visit, so receiving presence can never
// create history on this device.
//
//   tabs_publish {profile?, windows:[{current, tabs:[{url, title?,
//                  pinned?, container?}]}]} — publish this device's
//                  open tab set (command)
//   tabs_peers   {profile?} — {devices:[{device_id, updated_ms,
//                  windows}]} newest-first-proof LWW view (query)

fn op_sync_tabs_publish(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let scope = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let windows = p
        .get("windows")
        .ok_or_else(|| OpErr::BadPayload("missing \"windows\" array".into()))?;
    let op = synctabs::publish(ctx.core.data_root(), &scope, windows)?;
    notify::emit("sync");
    Ok(json!({
        "ok": true,
        "op_id": op.op_id,
        "lamport": op.lamport,
    }))
}

fn op_sync_tabs_peers(ctx: &MwCtx, p: &Value) -> Result<Value, OpErr> {
    let scope = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let devices = synctabs::peers(ctx.core.data_root(), &scope)?;
    Ok(json!({ "devices": devices }))
}

const SYNC_OPS: &[Op] = &[
    Op { id: 0x0001, name: "identity", class: OpClass::Query,
         profile_scoped: false, since: 1, handler: op_sync_identity },
    Op { id: 0x0002, name: "members_list", class: OpClass::Query,
         profile_scoped: false, since: 1, handler: op_sync_members_list },
    Op { id: 0x0003, name: "member_add", class: OpClass::Command,
         profile_scoped: false, since: 1, handler: op_sync_member_add },
    Op { id: 0x0004, name: "member_remove", class: OpClass::Command,
         profile_scoped: false, since: 1, handler: op_sync_member_remove },
    Op { id: 0x0005, name: "member_seen", class: OpClass::Command,
         profile_scoped: false, since: 1, handler: op_sync_member_seen },
    Op { id: 0x0006, name: "append", class: OpClass::Command,
         profile_scoped: false, since: 1, handler: op_sync_append },
    Op { id: 0x0007, name: "export", class: OpClass::Query,
         profile_scoped: false, since: 1, handler: op_sync_export },
    Op { id: 0x0008, name: "import", class: OpClass::Command,
         profile_scoped: false, since: 1, handler: op_sync_import },
    Op { id: 0x0009, name: "stats", class: OpClass::Query,
         profile_scoped: false, since: 1, handler: op_sync_stats },
    Op { id: 0x000a, name: "drain", class: OpClass::Command,
         profile_scoped: false, since: 1, handler: op_sync_drain },
    Op { id: 0x000b, name: "transport_start", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_start },
    Op { id: 0x000c, name: "transport_stop", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_stop },
    Op { id: 0x000d, name: "transport_status", class: OpClass::Query,
         profile_scoped: false, since: 2, handler: op_sync_transport_status },
    Op { id: 0x000e, name: "transport_authorize", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_authorize },
    Op { id: 0x000f, name: "transport_deauthorize", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_deauthorize },
    Op { id: 0x0010, name: "transport_keygen", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_keygen },
    Op { id: 0x0011, name: "transport_connect", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_connect },
    Op { id: 0x0012, name: "transport_send", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_send },
    Op { id: 0x0013, name: "transport_close", class: OpClass::Command,
         profile_scoped: false, since: 2, handler: op_sync_transport_close },
    Op { id: 0x0014, name: "transport_poll", class: OpClass::Query,
         profile_scoped: false, since: 2, handler: op_sync_transport_poll },
    Op { id: 0x0015, name: "pair_begin", class: OpClass::Command,
         profile_scoped: false, since: 3, handler: op_sync_pair_begin },
    Op { id: 0x0016, name: "pair_join", class: OpClass::Command,
         profile_scoped: false, since: 3, handler: op_sync_pair_join },
    Op { id: 0x0017, name: "pair_poll", class: OpClass::Command,
         profile_scoped: false, since: 3, handler: op_sync_pair_poll },
    Op { id: 0x0018, name: "pair_confirm", class: OpClass::Command,
         profile_scoped: false, since: 3, handler: op_sync_pair_confirm },
    Op { id: 0x0019, name: "pair_cancel", class: OpClass::Command,
         profile_scoped: false, since: 3, handler: op_sync_pair_cancel },
    Op { id: 0x001a, name: "pair_list", class: OpClass::Query,
         profile_scoped: false, since: 3, handler: op_sync_pair_list },
    Op { id: 0x001b, name: "tabs_publish", class: OpClass::Command,
         profile_scoped: false, since: 4, handler: op_sync_tabs_publish },
    Op { id: 0x001c, name: "tabs_peers", class: OpClass::Query,
         profile_scoped: false, since: 4, handler: op_sync_tabs_peers },
];

// ---- registry ------------------------------------------------------------------
//
// Domain ids are MW-ARCH §7's, frozen here.  tor and bidi register
// their names but stay op-less BY DESIGN — §2.3 keeps the tor
// control link and the BiDi client on the flat surface, so
// mw.describe reports an honest empty ops list for them.

const DOMAINS: &[Domain] = &[
    Domain { id: 0, name: "mw", ops: MW_OPS },
    Domain { id: 1, name: "settings", ops: SETTINGS_OPS },
    Domain { id: 2, name: "profiles", ops: PROFILES_OPS },
    Domain { id: 3, name: "history", ops: HISTORY_OPS },
    Domain { id: 4, name: "bookmarks", ops: BOOKMARKS_OPS },
    Domain { id: 5, name: "policy", ops: POLICY_OPS },
    Domain { id: 6, name: "session", ops: SESSION_OPS },
    Domain { id: 7, name: "cookies", ops: COOKIES_OPS },
    Domain { id: 8, name: "icons", ops: ICONS_OPS },
    Domain { id: 9, name: "containers", ops: CONTAINERS_OPS },
    Domain { id: 10, name: "notes", ops: NOTES_OPS },
    Domain { id: 11, name: "custody", ops: CUSTODY_OPS },
    Domain { id: 12, name: "opensearch", ops: OPENSEARCH_OPS },
    Domain { id: 13, name: "updates", ops: UPDATES_OPS },
    Domain { id: 14, name: "services", ops: SERVICES_OPS },
    Domain { id: 15, name: "extverify", ops: EXTVERIFY_OPS },
    Domain { id: 16, name: "vault", ops: VAULT_OPS },
    Domain { id: 17, name: "tor", ops: &[] },
    Domain { id: 18, name: "bidi", ops: &[] },
    Domain { id: 19, name: "sync", ops: SYNC_OPS },
];

// ---- dispatch --------------------------------------------------------------------

fn fail_frame(req: &Request, code: &'static str, status: i32, msg: String) -> Vec<u8> {
    error_frame(&ErrFrame {
        corr_id: req.corr_id,
        domain: req.domain,
        op: req.op,
        code,
        status,
        msg,
    })
}

/// Parses + dispatches one request frame, always producing a
/// response frame.  The op handler itself is panic-contained so a
/// module panic degrades to an "op_failed" envelope with the
/// correlation still echoed — never a cross-FFI unwind.
fn dispatch(core: &AcCore, buf: &[u8]) -> Vec<u8> {
    let req = match parse_request(buf) {
        Ok(r) => r,
        Err(e) => return error_frame(&e),
    };
    let Some(domain) = DOMAINS.iter().find(|d| d.id == req.domain) else {
        return fail_frame(
            &req,
            E_UNKNOWN_DOMAIN,
            0,
            format!("domain 0x{:02x} not registered", req.domain),
        );
    };
    let Some(op) = domain.ops.iter().find(|o| o.id == req.op) else {
        return fail_frame(
            &req,
            E_UNKNOWN_OP,
            0,
            format!("{}: op 0x{:04x} not registered", domain.name, req.op),
        );
    };
    let declared = match req.msg_type {
        TY_QUERY => OpClass::Query,
        _ => OpClass::Command,
    };
    if declared != op.class {
        return fail_frame(
            &req,
            E_MALFORMED,
            0,
            format!(
                "{}.{} is a {}, not a {}",
                domain.name,
                op.name,
                op.class.as_str(),
                declared.as_str(),
            ),
        );
    }
    // v1 payloads are JSON; an empty payload reads as null.  A
    // non-JSON byte lane is the flags byte's job if it ever lands.
    let payload = if req.payload.is_empty() {
        Value::Null
    } else {
        match serde_json::from_slice::<Value>(req.payload) {
            Ok(v) => v,
            Err(e) => {
                return fail_frame(&req, E_BAD_PAYLOAD, 0, format!("payload is not JSON: {e}"))
            }
        }
    };
    let ctx = MwCtx { core };
    match catch_unwind(AssertUnwindSafe(|| (op.handler)(&ctx, &payload))) {
        Ok(Ok(v)) => {
            error::clear_error();
            let body = serde_json::to_vec(&v).unwrap_or_else(|_| b"null".to_vec());
            build_frame(TY_OK, req.corr_id, req.domain, req.op, &body)
        }
        Ok(Err(OpErr::BadPayload(msg))) => fail_frame(&req, E_BAD_PAYLOAD, 0, msg),
        Ok(Err(OpErr::Failed(f))) => {
            fail_frame(&req, E_OP_FAILED, f.status as i32, f.msg)
        }
        Err(_) => fail_frame(
            &req,
            E_OP_FAILED,
            RcStatus::Crypto as i32,
            "panic inside rustcore".into(),
        ),
    }
}

// ---- C ABI ------------------------------------------------------------------------

/// The max envelope schema version this library accepts — the
/// frame's version byte, not the facade ABI version.
#[no_mangle]
pub extern "C" fn ac_mw_version() -> u32 {
    MW_ENVELOPE_VERSION
}

/// Dispatches one request frame into a caller-owned response frame.
/// Transport-level failures (NULL core/out, unreadable req) return a
/// bare status with *out = {NULL,0}; every frame attempt — even a
/// malformed one — returns RC_OK with a type=3/type=4 response
/// envelope in *out.  Synchronous; queries may run on any thread,
/// commands serialize on their store's lock like the flat surface.
///
/// # Safety
/// `core` must come from ac_init; `req` must point to `req_len`
/// readable bytes (a NULL req is legal only with req_len 0); `out`
/// must be writable.  Free the result with ac_buffer_free.
#[no_mangle]
pub unsafe extern "C" fn ac_mw_dispatch(
    core: *mut AcCore,
    req: *const u8,
    req_len: usize,
    out: *mut crate::RcBuffer,
) -> RcStatus {
    let Some(slot) = (unsafe { out.as_mut() }) else {
        error::set_error("bad out pointer");
        return RcStatus::InvalidArgument;
    };
    slot.data = ptr::null_mut();
    slot.len = 0;
    let Some(core) = (unsafe { core.as_ref() }) else {
        error::set_error("bad core handle");
        return RcStatus::InvalidArgument;
    };
    let Some(bytes) = (unsafe { util::bytes(req, req_len) }) else {
        error::set_error("bad request buffer");
        return RcStatus::InvalidArgument;
    };
    let resp = catch_unwind(AssertUnwindSafe(|| dispatch(core, bytes))).unwrap_or_else(|_| {
        error_frame(&ErrFrame {
            corr_id: 0,
            domain: 0,
            op: 0,
            code: E_OP_FAILED,
            status: RcStatus::Crypto as i32,
            msg: "panic inside rustcore".into(),
        })
    });
    crate::buffer_out(out, resp);
    RcStatus::Ok
}

// ---- tests -------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use crate::facade::AcConfig;
    use std::ffi::{CStr, CString};
    use std::sync::MutexGuard;

    /// Serialize against every test that moves shared state — the
    /// settings commands below fire notify::emit, which must not
    /// land inside a neighbour's callback-registration window.
    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-mw-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir.to_string_lossy().into_owned()
    }

    struct TestCore(*mut AcCore);

    impl TestCore {
        fn new(dir: &str) -> TestCore {
            let root = CString::new(dir).unwrap();
            let cfg = AcConfig {
                struct_size: std::mem::size_of::<AcConfig>() as u32,
                api_version: facade::AC_API_VERSION,
                data_root: root.as_ptr(),
                app_name: ptr::null(),
                default_profile_name: ptr::null(),
            };
            let c = unsafe { facade::ac_init(&cfg) };
            assert!(!c.is_null(), "ac_init failed: {:?}", last_error());
            TestCore(c)
        }
    }

    impl Drop for TestCore {
        fn drop(&mut self) {
            unsafe { facade::ac_shutdown(self.0) };
        }
    }

    fn last_error() -> String {
        let p = unsafe { crate::rc_last_error_message() };
        if p.is_null() {
            return String::new();
        }
        let s = unsafe { CStr::from_ptr(p) }.to_string_lossy().into_owned();
        unsafe { crate::rc_string_free(p) };
        s
    }

    /// Build a request frame (the response builder is the same
    /// layout — only the type byte differs).
    fn request(msg_type: u8, corr: u64, domain: u8, op: u16, payload: &[u8]) -> Vec<u8> {
        build_frame(msg_type, corr, domain, op, payload)
    }

    fn query(corr: u64, domain: u8, op: u16, payload: &str) -> Vec<u8> {
        request(TY_QUERY, corr, domain, op, payload.as_bytes())
    }

    fn command(corr: u64, domain: u8, op: u16, payload: &str) -> Vec<u8> {
        request(TY_COMMAND, corr, domain, op, payload.as_bytes())
    }

    struct Resp {
        msg_type: u8,
        corr_id: u64,
        domain: u8,
        op: u16,
        payload: Value,
    }

    impl Resp {
        fn parse(buf: &[u8]) -> Resp {
            assert!(buf.len() >= HEADER_LEN, "response too short: {buf:02x?}");
            assert_eq!(&buf[0..2], &MAGIC, "response magic");
            assert_eq!(buf[2] as u32, MW_ENVELOPE_VERSION);
            let payload = if buf.len() == HEADER_LEN {
                Value::Null
            } else {
                serde_json::from_slice(&buf[HEADER_LEN..])
                    .unwrap_or_else(|e| panic!("response payload not JSON: {e}: {buf:02x?}"))
            };
            Resp {
                msg_type: buf[3],
                corr_id: u64::from_le_bytes(buf[4..12].try_into().unwrap()),
                domain: buf[12],
                op: u16::from_le_bytes(buf[14..16].try_into().unwrap()),
                payload,
            }
        }

        fn ok(&self) -> &Value {
            assert_eq!(self.msg_type, TY_OK, "payload: {}", self.payload);
            &self.payload
        }

        fn err(&self) -> (String, i64, String) {
            assert_eq!(self.msg_type, TY_ERROR, "payload: {}", self.payload);
            (
                self.payload["code"].as_str().unwrap_or("").to_string(),
                self.payload["status"].as_i64().unwrap_or(-1),
                self.payload["message"].as_str().unwrap_or("").to_string(),
            )
        }
    }

    fn dispatch_core(core: *mut AcCore, req: &[u8]) -> (RcStatus, Vec<u8>) {
        let mut out = crate::RcBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        let st = unsafe { ac_mw_dispatch(core, req.as_ptr(), req.len(), &mut out) };
        let v = if out.data.is_null() {
            Vec::new()
        } else {
            let v = unsafe { std::slice::from_raw_parts(out.data, out.len) }.to_vec();
            unsafe {
                crate::rc_buffer_free(crate::RcBuffer {
                    data: out.data,
                    len: out.len,
                })
            };
            v
        };
        (st, v)
    }

    /// The full FFI round trip: bytes in, RcBuffer out, parsed.
    fn call(core: *mut AcCore, req: &[u8]) -> Resp {
        let (st, buf) = dispatch_core(core, req);
        assert_eq!(st, RcStatus::Ok, "transport failure");
        Resp::parse(&buf)
    }

    #[test]
    fn ping_round_trip_and_corr_echo() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("ping"));
        let r = call(
            core.0,
            &query(0x0123_4567_89ab_cdef, 0, 0x0003, r#"{"x":1}"#),
        );
        assert_eq!(r.corr_id, 0x0123_4567_89ab_cdef);
        assert_eq!(r.domain, 0);
        assert_eq!(r.op, 3);
        let p = r.ok();
        assert_eq!(p["pong"], true);
        assert_eq!(p["echo"], json!({"x": 1}));
    }

    #[test]
    fn version_and_describe_are_self_consistent() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("ver"));
        let v = call(core.0, &query(7, 0, 0x0002, ""));
        let v = v.ok();
        assert_eq!(v["envelope_version"], 1);
        assert_eq!(v["api_version"], facade::AC_API_VERSION);

        // ac_mw_version is the flat discovery lane for the same byte.
        assert_eq!(ac_mw_version(), MW_ENVELOPE_VERSION);

        let d = call(core.0, &query(8, 0, 0x0001, ""));
        let d = d.ok().clone();
        assert_eq!(d["envelope_version"], 1);
        let domains = d["domains"].as_array().unwrap();
        // Every registry entry is reported — implemented or empty.
        assert_eq!(domains.len(), DOMAINS.len());
        for (i, dom) in DOMAINS.iter().enumerate() {
            assert_eq!(domains[i]["id"], dom.id);
            assert_eq!(domains[i]["name"], dom.name);
            assert_eq!(
                domains[i]["ops"].as_array().unwrap().len(),
                dom.ops.len(),
                "{}",
                dom.name
            );
        }
        // Every registered op round-trips through describe's table —
        // the MB05 probe will walk exactly this.
        let mw = &domains[0];
        let names: Vec<&str> = mw["ops"]
            .as_array()
            .unwrap()
            .iter()
            .map(|o| o["name"].as_str().unwrap())
            .collect();
        assert_eq!(names, ["describe", "version", "ping", "emit"]);
        let events = d["events"].as_array().unwrap();
        assert_eq!(events.len(), EVENT_TOPICS.len());
        assert!(d["error_codes"]
            .as_array()
            .unwrap()
            .iter()
            .any(|c| c == "op_failed"));
    }

    #[test]
    fn settings_command_query_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("set"));
        // Command class: set a string into the global scope.
        let r = call(
            core.0,
            &command(
                11,
                1,
                0x0004,
                r#"{"key":"MainWindow/home","value":{"t":"s","v":"https://x/"}}"#,
            ),
        );
        assert_eq!(r.corr_id, 11);
        assert_eq!(r.ok()["ok"], true);

        // Query class: the typed doc reads back through the envelope.
        let r = call(core.0, &query(12, 1, 0x0001, r#"{"key":"MainWindow/home"}"#));
        assert_eq!(r.ok(), &json!({"t": "s", "v": "https://x/"}));
        let r = call(
            core.0,
            &query(13, 1, 0x0002, r#"{"key":"MainWindow/home"}"#),
        );
        assert_eq!(r.ok()["value"], "https://x/");

        // A profile-scoped write lands under profiles/<id>/ and the
        // two scopes never cross.
        let r = call(
            core.0,
            &command(
                14,
                1,
                0x0004,
                r#"{"profile":"work","key":"k","value":{"t":"i","v":5}}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(15, 1, 0x0003, r#"{"profile":"work","prefix":"k"}"#),
        );
        assert_eq!(r.ok()["keys"], json!(["k"]));
        let r = call(core.0, &query(16, 1, 0x0001, r#"{"key":"k"}"#));
        let (code, status, _m) = r.err();
        // Global scope never saw the profile write — NotFound rides
        // as op_failed with the store's status carried through.
        assert_eq!(code, "op_failed");
        assert_eq!(status, RcStatus::NotFound as i64);

        // remove is an idempotent command.
        let r = call(core.0, &command(17, 1, 0x0005, r#"{"key":"MainWindow/home"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(18, 1, 0x0002, r#"{"key":"MainWindow/home"}"#),
        );
        assert_eq!(r.err().0, "op_failed");
    }

    #[test]
    fn settings_contains_sync_import_ini() {
        let _g = guard();
        let dir = tmpdir("setx");
        let core = TestCore::new(&dir);

        // contains is the boolean probe get's NotFound can't express.
        let r = call(core.0, &query(1, 1, 0x0006, r#"{"key":"k"}"#));
        assert_eq!(r.ok()["present"], false);
        let r = call(
            core.0,
            &command(2, 1, 0x0004, r#"{"key":"k","value":{"t":"b","v":true}}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(3, 1, 0x0006, r#"{"key":"k"}"#));
        assert_eq!(r.ok()["present"], true);

        // sync is a silent command — the flush shape, no verdict.
        let r = call(core.0, &command(4, 1, 0x0007, ""));
        assert_eq!(r.ok()["ok"], true);

        // import_ini merges absent keys only, one-shot per file
        // (the imported marker persists, so a second dispatch is a
        // no-op even though every op re-opens the store).
        // ("misc" is not a persona-classified group, so the global
        // scope's filter keeps it — a classified key would be the
        // profile scope's to import).
        let ini = std::path::Path::new(&dir).join("legacy.ini");
        std::fs::write(&ini, "[misc]\nthing=https://legacy/\n").unwrap();
        let payload =
            format!(r#"{{"path":"{}"}}"#, ini.to_string_lossy());
        let r = call(core.0, &command(5, 1, 0x0008, &payload));
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(6, 1, 0x0002, r#"{"key":"misc/thing"}"#),
        );
        assert_eq!(r.ok()["value"], "https://legacy/");
        // A stored key is never overwritten by the import.
        let r = call(
            core.0,
            &command(
                7,
                1,
                0x0004,
                r#"{"key":"misc/thing","value":{"t":"s","v":"https://new/"}}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &command(8, 1, 0x0008, &payload));
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(9, 1, 0x0002, r#"{"key":"misc/thing"}"#),
        );
        assert_eq!(r.ok()["value"], "https://new/");
    }

    #[test]
    fn session_file_ops_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("sess"));
        let r = call(core.0, &query(20, 6, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["exists"], false);

        // load on an absent session is op_failed/NotFound, not a
        // crash and not an empty doc.
        let r = call(core.0, &query(21, 6, 0x0002, r#"{"profile":"w1"}"#));
        let (code, status, _m) = r.err();
        assert_eq!(code, "op_failed");
        assert_eq!(status, RcStatus::NotFound as i64);

        let manifest = r#"{"version":1,"windows":[{"shell":"","current":-1,"engine":"webengine","tabs":[]}]}"#;
        let payload = format!(r#"{{"profile":"w1","session":{manifest}}}"#);
        let r = call(core.0, &command(22, 6, 0x0003, &payload));
        assert_eq!(r.ok()["ok"], true);

        let r = call(core.0, &query(23, 6, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["exists"], true);
        let r = call(core.0, &query(24, 6, 0x0002, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["version"], 1);
        assert!(r.ok()["windows"].is_array());

        // Profile binding is per call — another persona's session
        // stays absent under the same core.
        let r = call(core.0, &query(25, 6, 0x0001, r#"{"profile":"w2"}"#));
        assert_eq!(r.ok()["exists"], false);

        let r = call(core.0, &command(26, 6, 0x0004, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(27, 6, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["exists"], false);
    }

    #[test]
    fn services_stateless_query() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("svc"));
        let r = call(
            core.0,
            &query(30, 14, 0x0001, r#"{"input":"https://example.com"}"#),
        );
        let p = r.ok();
        assert_eq!(p["kind"], "navigate");
        // Keywords steer the verdict like the flat rc_ call.
        let r = call(
            core.0,
            &query(
                31,
                14,
                0x0001,
                r#"{"input":"g cats","keywords":["g"],"search_fallback":true}"#,
            ),
        );
        assert_eq!(r.ok()["kind"], "search");
    }

    #[test]
    fn malformed_frames_get_structured_errors() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("mal"));

        // Short frame — header never parsed: corr_id must be 0.
        let r = call(core.0, b"AM\x01\x01abc");
        assert_eq!(r.corr_id, 0);
        assert_eq!(r.err().0, "malformed");

        // Bad magic — still corr_id 0 (not a frame at all).
        let mut bad = query(99, 0, 3, "");
        bad[0] = b'X';
        let r = call(core.0, &bad);
        assert_eq!(r.corr_id, 0);
        assert_eq!(r.err().0, "malformed");

        // Empty request — a frame attempt: the error is in-band.
        let r = call(core.0, &[]);
        assert_eq!(r.err().0, "malformed");

        // Truncated payload — header parsed, corr echoes.
        let mut trunc = query(77, 0, 3, "{}");
        trunc.truncate(HEADER_LEN + 1);
        let r = call(core.0, &trunc);
        assert_eq!(r.corr_id, 77);
        assert_eq!(r.err().0, "malformed");

        // Declared payload_len larger than the frame.
        let mut liar = query(78, 0, 3, "{}");
        liar[16..20].copy_from_slice(&500u32.to_le_bytes());
        let r = call(core.0, &liar);
        assert_eq!(r.corr_id, 78);
        assert_eq!(r.err().0, "malformed");

        // Trailing bytes after the declared payload.
        let mut extra = query(79, 0, 3, "{}");
        extra.extend_from_slice(b"junk");
        let r = call(core.0, &extra);
        assert_eq!(r.corr_id, 79);
        assert_eq!(r.err().0, "malformed");

        // A non-request msg_type.
        let mut resp_type = query(80, 0, 3, "");
        resp_type[3] = TY_OK;
        let r = call(core.0, &resp_type);
        assert_eq!(r.corr_id, 80);
        assert_eq!(r.err().0, "malformed");
    }

    #[test]
    fn bad_version_rejected_with_corr_echo() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("bv"));
        for v in [0u8, 2, 99] {
            let mut req = query(55, 0, 0x0003, "");
            req[2] = v;
            let r = call(core.0, &req);
            assert_eq!(r.corr_id, 55);
            assert_eq!(r.err().0, "bad_version", "version {v}");
        }
    }

    #[test]
    fn unknown_domain_and_op_are_envelopes() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("unk"));
        let r = call(core.0, &query(60, 99, 1, ""));
        assert_eq!(r.corr_id, 60);
        assert_eq!(r.err().0, "unknown_domain");

        // A known domain with an unregistered op.
        let r = call(core.0, &query(61, 1, 0x00ff, ""));
        assert_eq!(r.corr_id, 61);
        assert_eq!(r.domain, 1);
        assert_eq!(r.op, 0x00ff);
        assert_eq!(r.err().0, "unknown_op");

        // An empty-ops domain reports unknown_op too (registered
        // name, no dispatchable surface — tor/bidi stay flat by
        // MW-ARCH §2.3).
        let r = call(core.0, &query(62, 17, 0x0001, ""));
        assert_eq!(r.err().0, "unknown_op");
    }

    #[test]
    fn msg_type_must_match_op_class() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("cls"));
        // A query op demanded as a command.
        let r = call(core.0, &command(70, 1, 0x0001, r#"{"key":"k"}"#));
        assert_eq!(r.err().0, "malformed");
        // A command op demanded as a query — this is the direction a
        // read-only context must never let slip through.
        let r = call(
            core.0,
            &query(71, 1, 0x0004, r#"{"key":"k","value":{"t":"s","v":"x"}}"#),
        );
        assert_eq!(r.err().0, "malformed");
        // Sanity: the same op as a real command works.
        let r = call(
            core.0,
            &command(72, 1, 0x0004, r#"{"key":"k","value":{"t":"s","v":"x"}}"#),
        );
        assert_eq!(r.msg_type, TY_OK);
    }

    #[test]
    fn bad_payload_shapes() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("bp"));
        // Non-JSON payload bytes.
        let r = call(core.0, &query(90, 1, 0x0001, "not json"));
        assert_eq!(r.corr_id, 90);
        assert_eq!(r.err().0, "bad_payload");

        // Missing required field.
        let r = call(core.0, &query(91, 1, 0x0001, "{}"));
        assert_eq!(r.err().0, "bad_payload");

        // Wrong field types.
        let r = call(
            core.0,
            &command(92, 1, 0x0004, r#"{"key":5,"value":{"t":"s","v":"x"}}"#),
        );
        assert_eq!(r.err().0, "bad_payload");
        let r = call(
            core.0,
            &command(93, 1, 0x0004, r#"{"key":"k","value":{"t":"zzz"}}"#),
        );
        assert_eq!(r.err().0, "bad_payload");
        let r = call(
            core.0,
            &command(94, 6, 0x0003, r#"{"profile":42,"session":{}}"#),
        );
        assert_eq!(r.err().0, "bad_payload");
    }

    #[test]
    fn transport_failures_are_bare_statuses() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("tp"));
        let req = query(1, 0, 0x0003, "");
        let mut out = crate::RcBuffer {
            data: ptr::null_mut(),
            len: 0,
        };
        // NULL core.
        assert_eq!(
            unsafe {
                ac_mw_dispatch(ptr::null_mut(), req.as_ptr(), req.len(), &mut out)
            },
            RcStatus::InvalidArgument
        );
        assert!(out.data.is_null());
        // NULL out.
        assert_eq!(
            unsafe {
                ac_mw_dispatch(core.0, req.as_ptr(), req.len(), ptr::null_mut())
            },
            RcStatus::InvalidArgument
        );
        // NULL req with a nonzero length — unreadable buffer.
        assert_eq!(
            unsafe { ac_mw_dispatch(core.0, ptr::null(), 8, &mut out) },
            RcStatus::InvalidArgument
        );
        assert!(out.data.is_null());
        // NULL req with zero length is an (empty) frame attempt —
        // malformed envelope in-band, not a bare status.
        assert_eq!(
            unsafe { ac_mw_dispatch(core.0, ptr::null(), 0, &mut out) },
            RcStatus::Ok
        );
        let v = unsafe { std::slice::from_raw_parts(out.data, out.len) }.to_vec();
        unsafe { crate::rc_buffer_free(out) };
        assert_eq!(Resp::parse(&v).err().0, "malformed");
        // The transport errors stashed their messages.
        assert!(!last_error().is_empty());
    }

    #[test]
    fn last_error_mirrors_the_envelope() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("le"));
        let r = call(core.0, &query(1, 1, 0x0001, r#"{"key":"nope"}"#));
        let (_code, _st, msg) = r.err();
        assert_eq!(last_error(), msg);
        let r = call(core.0, &query(2, 0, 0x0003, ""));
        assert_eq!(r.msg_type, TY_OK);
        assert_eq!(last_error(), "");
    }

    #[test]
    fn describe_ops_all_dispatch() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("desc"));
        // Honesty check: every op describe advertises resolves to a
        // real handler — a stale table entry would answer unknown_op.
        let d = call(core.0, &query(1, 0, 0x0001, ""));
        let d = d.ok().clone();
        for dom in d["domains"].as_array().unwrap() {
            let did = dom["id"].as_u64().unwrap() as u8;
            for op in dom["ops"].as_array().unwrap() {
                let oid = op["id"].as_u64().unwrap() as u16;
                let class = op["class"].as_str().unwrap();
                let ty = if class == "query" { TY_QUERY } else { TY_COMMAND };
                // An empty/missing-args payload may fail — but never
                // with unknown_op or unknown_domain.  A bare "{}"
                // succeeding outright is equally legal (mw.ping).
                let r = call(core.0, &request(ty, 1, did, oid, b"{}"));
                if r.msg_type == TY_ERROR {
                    let (code, _s, _m) = r.err();
                    assert!(
                        code == "bad_payload" || code == "op_failed",
                        "{}.{:?}: {code}",
                        dom["name"],
                        op["name"]
                    );
                }
            }
        }
    }

    #[test]
    fn b64_round_trip_and_strictness() {
        for bytes in [
            &b""[..],
            b"f",
            b"fo",
            b"foo",
            b"foob",
            b"fooba",
            b"foobar",
            &[0u8, 159, 146, 150, 255, 0],
        ] {
            let enc = b64_encode(bytes);
            assert_eq!(b64_decode(&enc).as_deref(), Some(bytes), "{enc}");
        }
        // Strict grammar: no missing/wrong padding, no data after
        // padding, no out-of-alphabet bytes.
        for bad in ["Zg", "Zg=", "Zg==Zg==", "Z==", "Zg=b", "Zm 9v", "Zm9v!", "Zm=v"] {
            assert_eq!(b64_decode(bad), None, "{bad}");
        }
    }

    #[test]
    fn profiles_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("pro"));
        let r = call(core.0, &query(1, 2, 0x0001, ""));
        let rows = r.ok()["profiles"].as_array().unwrap().clone();
        // A fresh core mints the default persona.
        assert!(rows.iter().any(|p| p["id"] == "default"), "{rows:?}");
        let r = call(core.0, &command(2, 2, 0x0002, ""));
        assert_eq!(r.ok()["ok"], true);

        // create -> get -> rename -> set_avatar -> delete round trip.
        let r = call(
            core.0,
            &command(3, 2, 0x0004, r#"{"display_name":"Work Stuff!"}"#),
        );
        let id = r.ok()["id"].as_str().unwrap().to_string();
        assert_eq!(id, "work-stuff");
        let payload = format!(r#"{{"id":"{id}"}}"#);
        let r = call(core.0, &query(4, 2, 0x0003, &payload));
        assert_eq!(r.ok()["name"], "Work Stuff!");
        let r = call(
            core.0,
            &command(
                5,
                2,
                0x0005,
                &format!(r#"{{"id":"{id}","name":"Day Job"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(6, 2, 0x0003, &payload));
        assert_eq!(r.ok()["name"], "Day Job");
        let r = call(
            core.0,
            &command(
                7,
                2,
                0x0006,
                &format!(r#"{{"id":"{id}","name":"face.png"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(8, 2, 0x0003, &payload));
        assert_eq!(r.ok()["avatar"], "face.png");
        // A null name clears the field; an unknown id is op_failed.
        let r = call(
            core.0,
            &command(9, 2, 0x0006, &format!(r#"{{"id":"{id}"}}"#)),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(10, 2, 0x0003, &payload));
        assert_eq!(r.ok()["avatar"], "");
        let r = call(
            core.0,
            &command(11, 2, 0x0007, &payload),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(12, 2, 0x0003, &payload));
        let (code, status, _m) = r.err();
        assert_eq!(code, "op_failed");
        assert_eq!(status, RcStatus::NotFound as i64);
        // "default" is undeletable like the flat spelling.
        let r = call(
            core.0,
            &command(13, 2, 0x0007, r#"{"id":"default"}"#),
        );
        assert_eq!(r.err().0, "op_failed");
    }

    #[test]
    fn history_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("his"));
        let r = call(core.0, &query(1, 3, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 0);

        let r = call(
            core.0,
            &command(
                2,
                3,
                0x0002,
                r#"{"profile":"w1","url":"https://a/","title":"A","ts_ms":111}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);

        let r = call(core.0, &query(3, 3, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 1);
        // The spec doc passes straight through to the store's query.
        let r = call(
            core.0,
            &query(4, 3, 0x0003, r#"{"profile":"w1","spec":{"limit":10}}"#),
        );
        let rows = r.ok().as_array().unwrap();
        assert!(rows.iter().any(|e| e["url"] == "https://a/"), "{rows:?}");

        // Persona isolation: the same core, another profile id.
        let r = call(core.0, &query(5, 3, 0x0001, r#"{"profile":"w2"}"#));
        assert_eq!(r.ok()["count"], 0);
    }

    #[test]
    fn history_extended_ops_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("hisx"));

        // exists is the path-stat probe — false before any write.
        let r = call(core.0, &query(1, 3, 0x0005, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["exists"], false);
        let r = call(
            core.0,
            &command(
                2,
                3,
                0x0002,
                r#"{"profile":"w1","url":"https://a/","title":"A","ts_ms":111}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(3, 3, 0x0005, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["exists"], true);

        // suggest ranks the row back like rc_history_suggest.
        let r = call(
            core.0,
            &query(4, 3, 0x0004, r#"{"profile":"w1","term":"a","limit":10}"#),
        );
        let rows = r.ok().as_array().unwrap().clone();
        assert!(rows.iter().any(|e| e["url"] == "https://a/"), "{rows:?}");

        // update_title rewrites the row; remove takes it back out.
        let r = call(
            core.0,
            &command(
                5,
                3,
                0x0006,
                r#"{"profile":"w1","url":"https://a/","title":"Renamed"}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(6, 3, 0x0003, r#"{"profile":"w1","spec":{}}"#),
        );
        assert_eq!(r.ok()[0]["title"], "Renamed");
        let r = call(
            core.0,
            &command(
                7,
                3,
                0x0007,
                r#"{"profile":"w1","url":"https://a/","title":"Renamed","ts_ms":111}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(8, 3, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 0);

        // expire reports the dropped row count; clear_since and
        // clear take the rest out.
        for (url, ts) in [("https://b/", 100i64), ("https://c/", 200)] {
            let p = format!(
                r#"{{"profile":"w1","url":"{url}","title":"","ts_ms":{ts}}}"#
            );
            assert_eq!(call(core.0, &command(9, 3, 0x0002, &p)).ok()["ok"], true);
        }
        let r = call(
            core.0,
            &command(10, 3, 0x000a, r#"{"profile":"w1","cutoff_ms":150}"#),
        );
        assert_eq!(r.ok()["removed"], 1);
        let r = call(
            core.0,
            &command(11, 3, 0x0009, r#"{"profile":"w1","since_ms":200}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(12, 3, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 0);
        let r = call(core.0, &command(13, 3, 0x0008, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
    }

    #[test]
    fn bookmarks_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("bm"));
        let r = call(core.0, &query(1, 4, 0x0001, r#"{"profile":"w1"}"#));
        let root = r.ok()["root"].as_u64().unwrap();
        assert_ne!(root, 0);

        let r = call(
            core.0,
            &command(
                2,
                4,
                0x0002,
                r#"{"profile":"w1","node":{"type":2,"title":"A","url":"https://a/"}}"#,
            ),
        );
        let id = r.ok()["id"].as_u64().unwrap();
        assert_ne!(id, 0);

        let payload = format!(r#"{{"profile":"w1","id":{id}}}"#);
        let r = call(core.0, &query(3, 4, 0x0001, &payload));
        assert_eq!(r.ok()["title"], "A");
        assert_eq!(r.ok()["url"], "https://a/");

        // A bogus node id is a structured op_failed, not a panic.
        let r = call(
            core.0,
            &query(4, 4, 0x0001, r#"{"profile":"w1","id":999999}"#),
        );
        let (code, status, _m) = r.err();
        assert_eq!(code, "op_failed");
        assert_eq!(status, RcStatus::NotFound as i64);
    }

    #[test]
    fn policy_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("pol"));
        // Miss reads as a null value — a query answering "no
        // decision" is Ok, not an error envelope.
        let r = call(
            core.0,
            &query(1, 5, 0x0001, r#"{"profile":"w1","kind":"zoom","key":"example.com"}"#),
        );
        assert_eq!(r.ok()["value"], Value::Null);

        let r = call(
            core.0,
            &command(
                2,
                5,
                0x0002,
                r#"{"profile":"w1","kind":"zoom","key":"example.com","value":"125"}"#,
            ),
        );
        assert_eq!(r.ok()["changed"], true);

        let r = call(
            core.0,
            &query(3, 5, 0x0001, r#"{"profile":"w1","kind":"zoom","key":"example.com"}"#),
        );
        assert_eq!(r.ok()["value"], "125");
        // The host-walk lookup hits the stored suffix rule.
        let r = call(
            core.0,
            &query(4, 5, 0x0003, r#"{"profile":"w1","kind":"zoom","host":"sub.example.com"}"#),
        );
        assert_eq!(r.ok()["key"], "example.com");
        assert_eq!(r.ok()["value"], "125");
    }

    #[test]
    fn cookies_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("cok"));
        let r = call(core.0, &query(1, 7, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 0);

        let rec = r#"{"profile":"w1","record":{"name":"s","value":"v","domain":"example.com","path":"/"}}"#;
        let r = call(core.0, &command(2, 7, 0x0003, rec));
        assert_eq!(r.ok()["outcome"], "stored");

        let r = call(core.0, &query(3, 7, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 1);
        let r = call(core.0, &query(4, 7, 0x0002, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["cookies"][0]["name"], "s");

        // Re-setting the identical record is a Noop — and stays Ok.
        let r = call(core.0, &command(5, 7, 0x0003, rec));
        assert_eq!(r.ok()["outcome"], "noop");
    }

    #[test]
    fn icons_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("ico"));
        let png = b64_encode(b"\x89PNG-fake");
        let set = format!(
            r#"{{"profile":"w1","url":"https://a/","png_b64":"{png}"}}"#
        );
        let r = call(core.0, &command(1, 8, 0x0002, &set));
        assert_eq!(r.ok()["changed"], true);

        let r = call(core.0, &query(2, 8, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 1);
        let r = call(
            core.0,
            &query(3, 8, 0x0003, r#"{"profile":"w1","url":"https://a/"}"#),
        );
        assert_eq!(r.ok()["png_b64"], png);

        // A miss is op_failed/NotFound; malformed b64 is bad_payload.
        let r = call(
            core.0,
            &query(4, 8, 0x0003, r#"{"profile":"w1","url":"https://nope/"}"#),
        );
        assert_eq!(r.err().1, RcStatus::NotFound as i64);
        let r = call(
            core.0,
            &command(
                5,
                8,
                0x0002,
                r#"{"profile":"w1","url":"https://a/","png_b64":"!!!"}"#,
            ),
        );
        assert_eq!(r.err().0, "bad_payload");
    }

    #[test]
    fn containers_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("con"));
        let r = call(core.0, &query(1, 9, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["containers"].as_array().unwrap().len(), 0);

        let r = call(
            core.0,
            &command(
                2,
                9,
                0x0002,
                r#"{"profile":"w1","name":"Work","color":"blue"}"#,
            ),
        );
        let c = r.ok();
        assert_eq!(c["name"], "Work");
        assert!(!c["id"].as_str().unwrap().is_empty());

        let r = call(core.0, &query(3, 9, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["containers"].as_array().unwrap().len(), 1);
        // A blank name is a store-level InvalidArgument — op_failed.
        let r = call(
            core.0,
            &command(4, 9, 0x0002, r#"{"profile":"w1","name":" "}"#),
        );
        assert_eq!(r.err().0, "op_failed");
    }

    #[test]
    fn notes_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("not"));
        let r = call(
            core.0,
            &command(
                1,
                10,
                0x0003,
                r#"{"profile":"w1","row":-1,"title":"T","body":"B"}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);

        let r = call(core.0, &query(2, 10, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 1);
        let r = call(core.0, &query(3, 10, 0x0002, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["notes"][0]["title"], "T");
        assert_eq!(r.ok()["notes"][0]["body"], "B");
    }

    #[test]
    fn custody_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("cus"));
        let r = call(core.0, &query(1, 11, 0x0001, r#"{"profile":"w1"}"#));
        let st = r.ok();
        assert!(st["available"].is_boolean());
        assert!(st["passphrase_enabled"].is_boolean());

        // put a credential (file-key custody — no passphrase needed),
        // read the name list back.
        let v = b64_encode(b"sekrit");
        let payload =
            format!(r#"{{"profile":"w1","name":"net/pw","value_b64":"{v}"}}"#);
        let r = call(core.0, &command(2, 11, 0x0003, &payload));
        assert_eq!(r.ok()["changed"], true);
        let r = call(core.0, &query(3, 11, 0x0002, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["names"], json!(["net/pw"]));
    }

    #[test]
    fn opensearch_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("ose"));
        let engine = r#"{"engine":{"name":"ProbeSE","search":{"template":"https://se/?q={searchTerms}"}}}"#;
        let r = call(core.0, &command(1, 12, 0x0002, engine));
        assert_eq!(r.ok()["changed"], true);

        let r = call(core.0, &query(2, 12, 0x0001, ""));
        let names: Vec<&str> = r.ok()["engines"]
            .as_array()
            .unwrap()
            .iter()
            .filter_map(|e| e.as_str())
            .collect();
        assert!(names.contains(&"ProbeSE"), "{names:?}");

        // An invalid record (no search template) is a store
        // InvalidArgument — op_failed, not bad_payload.
        let r = call(
            core.0,
            &command(3, 12, 0x0002, r#"{"engine":{"name":"Bogus"}}"#),
        );
        assert_eq!(r.err().0, "op_failed");
    }

    #[test]
    fn updates_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("upd"));
        let r = call(core.0, &query(1, 13, 0x0001, ""));
        assert_eq!(r.ok()["last_check_ms"], 0);

        let r = call(
            core.0,
            &command(2, 13, 0x0002, r#"{"now_ms":123456789}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(3, 13, 0x0001, ""));
        assert_eq!(r.ok()["last_check_ms"], 123456789);
        assert!(r.ok()["channel"].is_string());
    }

    #[test]
    fn services_tls_probe_is_a_command() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("tls"));
        // Port range validation is bad_payload before any socket.
        let r = call(
            core.0,
            &command(1, 14, 0x0002, r#"{"host":"127.0.0.1","port":0}"#),
        );
        assert_eq!(r.err().0, "bad_payload");
        // A live handshake against a refused port is still an Ok
        // envelope — the verdict JSON carries the failure.
        let r = call(
            core.0,
            &command(2, 14, 0x0002, r#"{"host":"127.0.0.1","port":1}"#),
        );
        let v = r.ok();
        assert!(v.is_object(), "{v}");
    }

    #[test]
    fn extverify_manifest_check() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("ext"));
        let manifest = b64_encode(br#"{"manifest_version":3,"name":"t","version":"1.0"}"#);
        let payload = format!(r#"{{"data_b64":"{manifest}"}}"#);
        let r = call(core.0, &query(1, 15, 0x0001, &payload));
        let doc = r.ok();
        assert!(doc.is_object(), "{doc}");

        // Non-JSON manifest bytes are Corrupt — an op_failed, and
        // malformed b64 never reaches the checker.
        let payload = format!(r#"{{"data_b64":"{}"}}"#, b64_encode(b"not json"));
        let r = call(core.0, &query(2, 15, 0x0001, &payload));
        assert_eq!(r.err().0, "op_failed");
        let r = call(core.0, &query(3, 15, 0x0001, r#"{"data_b64":"%%"}"#));
        assert_eq!(r.err().0, "bad_payload");
        // Query-only domain: a command demand is a class mismatch.
        let r = call(core.0, &command(4, 15, 0x0001, &payload));
        assert_eq!(r.err().0, "malformed");
    }

    #[test]
    fn vault_domain_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("vt"));
        // The vault takes explicit paths — stage a payload dir inside
        // the same temp root, seal it under a passphrase.
        let root = std::env::temp_dir().join(format!(
            "rustcore-mw-{}-vtsrc-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let src = root.join("src");
        std::fs::create_dir_all(&src).unwrap();
        std::fs::write(src.join("f.txt"), b"payload").unwrap();
        let blob = root.join("out.rv");
        let pass = b64_encode(b"a sufficiently long passphrase");

        let payload = format!(
            r#"{{"dir":"{}","out":"{}","pass_b64":"{pass}","algo":1}}"#,
            src.display(),
            blob.display()
        );
        let r = call(core.0, &command(1, 16, 0x0002, &payload));
        assert_eq!(r.ok()["ok"], true);

        let payload = format!(r#"{{"path":"{}"}}"#, blob.display());
        let r = call(core.0, &query(2, 16, 0x0001, &payload));
        let doc = r.ok();
        assert!(doc.is_object(), "{doc}");

        let payload = format!(
            r#"{{"path":"{}","pass_b64":"{pass}"}}"#,
            blob.display()
        );
        let r = call(core.0, &query(3, 16, 0x0003, &payload));
        assert_eq!(r.ok()["ok"], true);

        // A wrong passphrase is WrongPassword — op_failed, never a
        // crash.
        let wrong = b64_encode(b"the wrong passphrase entirely");
        let payload = format!(
            r#"{{"path":"{}","pass_b64":"{wrong}"}}"#,
            blob.display()
        );
        let r = call(core.0, &query(4, 16, 0x0003, &payload));
        let (code, status, _m) = r.err();
        assert_eq!(code, "op_failed");
        assert_eq!(status, RcStatus::WrongPassword as i64);
    }

    /// The mw.emit provoke hook fires the real subscriber lane —
    /// the path acprobe's --bridge leg drives.
    #[test]
    fn mw_emit_fires_declared_topic() {
        use std::sync::atomic::{AtomicUsize, Ordering};
        static HITS: AtomicUsize = AtomicUsize::new(0);
        unsafe extern "C" fn cb(
            _topic: *const std::ffi::c_char,
            _payload: *const std::ffi::c_char,
            _ud: *mut std::ffi::c_void,
        ) {
            HITS.fetch_add(1, Ordering::SeqCst);
        }
        let _g = guard();
        let core = TestCore::new(&tmpdir("emit"));
        let filter = std::ffi::CString::new("history/").unwrap();
        HITS.store(0, Ordering::SeqCst);
        let id = unsafe {
            facade::ac_events_subscribe(core.0, filter.as_ptr(), Some(cb), ptr::null_mut())
        };
        assert_ne!(id, 0);

        let r = call(
            core.0,
            &command(1, 0, 0x0004, r#"{"topic":"history/changed","fields":{"op":"visit"}}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        assert_eq!(HITS.load(Ordering::SeqCst), 1);

        // An undeclared topic is a bad payload, and emits nothing.
        let r = call(
            core.0,
            &command(2, 0, 0x0004, r#"{"topic":"bogus/thing"}"#),
        );
        assert_eq!(r.err().0, "bad_payload");
        assert_eq!(HITS.load(Ordering::SeqCst), 1);

        assert_eq!(
            unsafe { facade::ac_events_unsubscribe(core.0, id) },
            RcStatus::Ok
        );
    }

    #[test]
    fn policy_extended_ops_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("polx"));

        // store_present is the file-existence probe — false on a
        // fresh root, true once a write lands.
        let r = call(core.0, &query(1, 5, 0x0009, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["present"], false);
        let r = call(
            core.0,
            &command(
                2,
                5,
                0x0002,
                r#"{"profile":"w1","kind":"zoom","key":"a.example","value":"150"}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(3, 5, 0x0009, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["present"], true);

        // list + snapshot hand back the whole store surface.
        let r = call(
            core.0,
            &query(4, 5, 0x0007, r#"{"profile":"w1","kind":"zoom"}"#),
        );
        assert_eq!(r.ok()["entries"], json!({"a.example": "150"}));
        let r = call(core.0, &query(5, 5, 0x0008, r#"{"profile":"w1"}"#));
        // The snapshot doc is {version, generation, kinds:{kind:{k:v}}}.
        assert_eq!(r.ok()["kinds"]["zoom"], json!({"a.example": "150"}));

        // replace is the wholesale mirror-write.
        let r = call(
            core.0,
            &command(
                6,
                5,
                0x0006,
                r#"{"profile":"w1","kind":"zoom","entries":{"b.example":"200","c.example":"110"}}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(7, 5, 0x0007, r#"{"profile":"w1","kind":"zoom"}"#),
        );
        assert_eq!(
            r.ok()["entries"],
            json!({"b.example": "200", "c.example": "110"})
        );

        // remove is idempotent; a second pass reports unchanged.
        let r = call(
            core.0,
            &command(
                8,
                5,
                0x0004,
                r#"{"profile":"w1","kind":"zoom","key":"b.example"}"#,
            ),
        );
        assert_eq!(r.ok()["changed"], true);
        let r = call(
            core.0,
            &command(
                9,
                5,
                0x0004,
                r#"{"profile":"w1","kind":"zoom","key":"b.example"}"#,
            ),
        );
        assert_eq!(r.ok()["changed"], false);

        // reload and reset are bare-status commands.
        let r = call(core.0, &command(10, 5, 0x000a, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &command(11, 5, 0x0005, r#"{"profile":"w1","kind":"zoom"}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(12, 5, 0x0007, r#"{"profile":"w1","kind":"zoom"}"#),
        );
        assert_eq!(r.ok()["entries"], json!({}));
        let r = call(core.0, &command(13, 5, 0x000b, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(14, 5, 0x0009, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["present"], false);
    }

    #[test]
    fn policy_store_anchors_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("pols"));

        // tranco_file_entries answers 0 for an absent override file;
        // tranco_reload + blocklist_reload are Ok on a bare root.
        let r = call(
            core.0,
            &query(
                1,
                5,
                0x000d,
                r#"{"profile":"w1","name":"tranco-list.dat"}"#,
            ),
        );
        assert_eq!(r.ok()["entries"], 0);
        let r = call(core.0, &command(2, 5, 0x000e, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &command(3, 5, 0x000c, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);

        // sitetrust_lookup answers the composite verdict doc — an
        // unlisted host is amber with no signals.
        let r = call(
            core.0,
            &query(
                4,
                5,
                0x000f,
                r#"{"profile":"w1","host":"example.com","https":true,"probes":3}"#,
            ),
        );
        let v = r.ok();
        assert!(v["level"].is_string(), "{v}");
        assert!(v["signals"].is_array(), "{v}");
    }

    #[test]
    fn session_mark_ops_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("sessm"));

        // A first mark_start is clean (no prior dirty run), a second
        // without a mark between reports the dirty previous run.
        let r = call(core.0, &command(1, 6, 0x0005, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        assert_eq!(r.ok()["dirty"], false);
        let r = call(core.0, &command(2, 6, 0x0005, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["dirty"], true);

        // mark_exit records the clean exit; the next start is clean
        // again.  mark_terminated is the signal-death variant.
        let r = call(
            core.0,
            &command(3, 6, 0x0006, r#"{"profile":"w1","code":0}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &command(4, 6, 0x0005, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["dirty"], false);
        let r = call(
            core.0,
            &command(5, 6, 0x0007, r#"{"profile":"w1","signal":15}"#),
        );
        assert_eq!(r.ok()["ok"], true);

        // Another persona's marker is untouched.
        let r = call(core.0, &command(6, 6, 0x0005, r#"{"profile":"w2"}"#));
        assert_eq!(r.ok()["dirty"], false);
    }

    #[test]
    fn icons_clear_sweep_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("icox"));
        let png = b64_encode(b"\x89PNG-fake");
        let set = format!(
            r#"{{"profile":"w1","url":"https://a/","png_b64":"{png}"}}"#
        );
        let r = call(core.0, &command(1, 8, 0x0002, &set));
        assert_eq!(r.ok()["ok"], true);

        // A sweep with a future cutoff drops the fresh row; a past
        // cutoff is a no-op reporting 0.
        let r = call(
            core.0,
            &command(2, 8, 0x0005, r#"{"profile":"w1","cutoff_ms":0}"#),
        );
        assert_eq!(r.ok()["removed"], 0);
        let r = call(
            core.0,
            &command(
                3,
                8,
                0x0005,
                r#"{"profile":"w1","cutoff_ms":99999999999999}"#,
            ),
        );
        assert_eq!(r.ok()["removed"], 1);
        let r = call(core.0, &query(4, 8, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 0);

        // clear on an empty cache is silent-but-Ok; after a fresh put
        // it empties again.
        let r = call(core.0, &command(5, 8, 0x0004, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &command(6, 8, 0x0002, &set));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &command(7, 8, 0x0004, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(8, 8, 0x0001, r#"{"profile":"w1"}"#));
        assert_eq!(r.ok()["count"], 0);
    }

    #[test]
    fn opensearch_extended_ops_round_trip() {
        let _g = guard();
        let core = TestCore::new(&tmpdir("osex"));

        let r = call(core.0, &query(1, 12, 0x0003, ""));
        assert_eq!(r.ok()["present"], false);

        let engine = r#"{"engine":{"name":"ProbeSE","search":{"template":"https://se/?q={searchTerms}"}}}"#;
        let r = call(core.0, &command(2, 12, 0x0002, engine));
        assert_eq!(r.ok()["changed"], true);
        let r = call(core.0, &query(3, 12, 0x0003, ""));
        assert_eq!(r.ok()["present"], true);

        // get returns the whole record doc; a miss is a null engine.
        let r = call(
            core.0,
            &query(4, 12, 0x0004, r#"{"name":"ProbeSE"}"#),
        );
        assert_eq!(r.ok()["engine"]["name"], "ProbeSE");
        let r = call(core.0, &query(5, 12, 0x0004, r#"{"name":"Ghost"}"#));
        assert_eq!(r.ok()["engine"], Value::Null);

        // Keyword bindings: set -> engine_for_keyword -> keyword_url
        // -> the bound-keywords list.
        let r = call(
            core.0,
            &command(
                6,
                12,
                0x0008,
                r#"{"name":"ProbeSE","keywords":["ps","probe"]}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(7, 12, 0x000a, r#"{"keyword":"ps"}"#),
        );
        assert_eq!(r.ok()["name"], "ProbeSE");
        let r = call(
            core.0,
            &query(
                8,
                12,
                0x0014,
                r#"{"keyword":"ps","term":"cats","language":"en","source":"t"}"#,
            ),
        );
        assert_eq!(r.ok()["url"], "https://se/?q=cats");
        let r = call(core.0, &query(9, 12, 0x000b, ""));
        let kws = r.ok()["keywords"].as_array().unwrap().clone();
        assert!(kws.iter().any(|k| k == "ps"), "{kws:?}");

        // engine.url resolves the named engine + slot directly.
        let r = call(
            core.0,
            &query(
                10,
                12,
                0x0013,
                r#"{"name":"ProbeSE","kind":"search","term":"dogs"}"#,
            ),
        );
        assert_eq!(r.ok()["url"], "https://se/?q=dogs");
        let r = call(
            core.0,
            &query(
                11,
                12,
                0x0013,
                r#"{"name":"ProbeSE","kind":"bogus","term":"x"}"#,
            ),
        );
        assert_eq!(r.err().0, "op_failed");

        // expand is the parseTemplate spec -> URL primitive.
        let r = call(
            core.0,
            &query(
                12,
                12,
                0x0012,
                r#"{"spec":{"template":"https://se/?q={searchTerms}","method":"","params":[],"term":"owls","language":"","source":""}}"#,
            ),
        );
        assert_eq!(r.ok()["url"], "https://se/?q=owls");

        // rename keeps the bindings; reorder accepts the name list.
        let r = call(
            core.0,
            &command(13, 12, 0x0006, r#"{"old":"ProbeSE","new":"RenamedSE"}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(14, 12, 0x000a, r#"{"keyword":"ps"}"#),
        );
        assert_eq!(r.ok()["name"], "RenamedSE");
        let r = call(
            core.0,
            &command(15, 12, 0x0007, r#"{"order":["RenamedSE"]}"#),
        );
        assert_eq!(r.ok()["ok"], true);

        // The bundled blocklist: block -> removed_bundled -> unblock.
        let r = call(
            core.0,
            &command(16, 12, 0x000d, r#"{"name":"BundledX"}"#),
        );
        assert_eq!(r.ok()["changed"], true);
        let r = call(core.0, &query(17, 12, 0x000c, ""));
        let names = r.ok()["names"].as_array().unwrap().clone();
        assert!(names.iter().any(|n| n == "BundledX"), "{names:?}");
        let r = call(
            core.0,
            &command(18, 12, 0x000e, r#"{"name":"BundledX"}"#),
        );
        assert_eq!(r.ok()["changed"], true);
        let r = call(core.0, &query(19, 12, 0x000f, ""));
        assert!(r.ok()["names"].is_array());

        // seed/restore bundled are idempotent change-reporting
        // commands; import feeds a descriptor.
        let r = call(core.0, &command(20, 12, 0x0010, ""));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &command(21, 12, 0x0011, ""));
        assert_eq!(r.ok()["ok"], true);
        let xml = "<OpenSearchDescription xmlns=\"http://a9.com/-/spec/opensearch/1.1/\">\
            <ShortName>ImpSE</ShortName>\
            <Url type=\"text/html\" template=\"https://imp/?q={searchTerms}\"/>\
            </OpenSearchDescription>";
        let payload = format!(r#"{{"xml":{}}}"#, serde_json::to_string(xml).unwrap());
        let r = call(core.0, &command(22, 12, 0x0009, &payload));
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core.0,
            &query(23, 12, 0x0004, r#"{"name":"ImpSE"}"#),
        );
        assert_eq!(r.ok()["engine"]["name"], "ImpSE");

        // remove drops a record; reset wipes the whole store.
        let r = call(
            core.0,
            &command(24, 12, 0x0005, r#"{"name":"ImpSE"}"#),
        );
        assert_eq!(r.ok()["changed"], true);
        let r = call(core.0, &command(25, 12, 0x0015, ""));
        assert_eq!(r.ok()["ok"], true);
        let r = call(core.0, &query(26, 12, 0x0003, ""));
        assert_eq!(r.ok()["present"], false);
    }

    /// SYNC01: the sync domain over the envelope — identity,
    /// membership, the append/export/import plumbing, and the
    /// two-device loopback that proves ops round-trip signed.
    #[test]
    fn sync_domain_end_to_end() {
        let _g = guard();
        let core_a = TestCore::new(&tmpdir("syncA"));
        let core_b = TestCore::new(&tmpdir("syncB"));

        // identity: a 64-hex device id + a 32-byte pubkey.
        let r = call(core_a.0, &query(1, 19, 0x0001, ""));
        let p = r.ok();
        let a_id = p["device_id"].as_str().unwrap().to_string();
        assert_eq!(a_id.len(), 64);
        assert_eq!(b64_decode(p["pubkey_b64"].as_str().unwrap()).unwrap().len(), 32);

        // Self-pairing is a protocol error.
        let r = call(
            core_a.0,
            &command(
                2,
                19,
                0x0003,
                &format!(
                    r#"{{"pubkey_b64":"{}"}}"#,
                    p["pubkey_b64"].as_str().unwrap()
                ),
            ),
        );
        assert_eq!(r.err().0, "op_failed");

        // B's pubkey pairs into A's membership and back.
        let r = call(core_b.0, &query(3, 19, 0x0001, ""));
        let b_pk = r.ok()["pubkey_b64"].as_str().unwrap().to_string();
        let b_id = r.ok()["device_id"].as_str().unwrap().to_string();
        let r = call(
            core_a.0,
            &command(
                4,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{}","onion_addr":"peer.onion"}}"#, b_pk),
            ),
        );
        assert_eq!(r.ok()["device_id"].as_str().unwrap(), b_id);
        let r = call(core_a.0, &query(5, 19, 0x0002, ""));
        let members = r.ok()["members"].as_array().unwrap();
        assert_eq!(members.len(), 1);
        assert_eq!(members[0]["onion_addr"], "peer.onion");
        let r = call(
            core_a.0,
            &command(6, 19, 0x0005, &format!(r#"{{"device_id":"{}","last_seen_ms":99}}"#, b_id)),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(core_a.0, &query(7, 19, 0x0002, ""));
        assert_eq!(r.ok()["members"][0]["last_seen_ms"], 99);

        // A appends a settings op; the export carries the signed wire doc.
        let r = call(
            core_a.0,
            &command(8, 19, 0x0006, r#"{"store":"settings","body":{"set":{"k":1}}}"#),
        );
        assert_eq!(r.ok()["lamport"], 1);
        let r = call(core_a.0, &query(9, 19, 0x0007, ""));
        let ops = r.ok()["ops"].clone();
        assert_eq!(ops.as_array().unwrap().len(), 1);
        assert_eq!(ops[0]["device_id"].as_str().unwrap(), a_id);

        // B rejects the op until it knows A (unknown member), then
        // pairs A and applies it — the loopback round-trip.
        let r = call(
            core_b.0,
            &command(10, 19, 0x0008, &serde_json::to_string(&json!({"ops": ops})).unwrap()),
        );
        assert_eq!(r.ok()["rejected"].as_array().unwrap().len(), 1);
        let r = call(
            core_b.0,
            &command(
                11,
                19,
                0x0003,
                &format!(
                    r#"{{"pubkey_b64":"{}"}}"#,
                    call(core_a.0, &query(12, 19, 0x0001, "")).ok()["pubkey_b64"]
                        .as_str()
                        .unwrap()
                ),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_b.0,
            &command(13, 19, 0x0008, &serde_json::to_string(&json!({"ops": ops})).unwrap()),
        );
        let p = r.ok();
        assert_eq!(p["accepted"], 1);
        assert_eq!(p["deferred"], 1, "no adapter registered -> deferred");

        // Re-import dedups; drain reports the still-pending op.
        let r = call(
            core_b.0,
            &command(14, 19, 0x0008, &serde_json::to_string(&json!({"ops": ops})).unwrap()),
        );
        assert_eq!(r.ok()["duplicates"], 1);
        let r = call(
            core_b.0,
            &command(15, 19, 0x000a, r#"{"store":"settings"}"#),
        );
        assert_eq!(r.ok()["pending_before"], 1);
        assert_eq!(r.ok()["deferred"], 1);

        // stats reflects the merged log on B.
        let r = call(core_b.0, &query(16, 19, 0x0009, ""));
        let p = r.ok();
        assert_eq!(p["ops"], 1);
        assert_eq!(p["members"], 1);
        assert_eq!(p["stores"]["settings"]["pending_apply"], 1);

        // member_remove answers honestly both ways.
        let r = call(
            core_b.0,
            &command(17, 19, 0x0004, &format!(r#"{{"device_id":"{}"}}"#, a_id)),
        );
        assert_eq!(r.ok()["removed"], true);
        let r = call(
            core_b.0,
            &command(18, 19, 0x0004, &format!(r#"{{"device_id":"{}"}}"#, a_id)),
        );
        assert_eq!(r.ok()["removed"], false);
    }

    /// SYNC02: two cores exchanging over the mem backend through the
    /// envelope — the hermetic parity of the live-Tor e2e: grant ->
    /// connect -> frame ping -> revoke -> refuse.
    #[test]
    fn sync_transport_mem_end_to_end() {
        let _g = guard();
        let core_a = TestCore::new(&tmpdir("tpa"));
        let core_b = TestCore::new(&tmpdir("tpb"));

        // Nothing started: ops that need a link refuse cleanly.
        let r = call(core_a.0, &query(1, 19, 0x000d, ""));
        assert_eq!(r.ok()["running"], false);

        // Boot both listeners.
        let r = call(
            core_a.0,
            &command(2, 19, 0x000b, r#"{"backend":"mem","name":"A"}"#),
        );
        assert_eq!(r.ok()["endpoint"], "mem:A");
        let r = call(
            core_b.0,
            &command(3, 19, 0x000b, r#"{"backend":"mem","name":"B"}"#),
        );
        assert_eq!(r.ok()["endpoint"], "mem:B");

        // Unauthorized peer refused — no link is created on A.
        let r = call(
            core_b.0,
            &command(4, 19, 0x0011, r#"{"endpoint":"mem:A","nickname":"mallory"}"#),
        );
        assert_eq!(r.err().0, "op_failed");
        let r = call(core_a.0, &query(5, 19, 0x000d, ""));
        assert_eq!(r.ok()["links"], 0);

        // Grant, connect, verify the incoming announcement on A.
        let r = call(
            core_a.0,
            &command(6, 19, 0x000e, r#"{"nickname":"peerB"}"#),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_b.0,
            &command(7, 19, 0x0011, r#"{"endpoint":"mem:A","nickname":"peerB"}"#),
        );
        let b_link = r.ok()["link_id"].as_u64().unwrap();

        let mut a_link = None;
        for _ in 0..50 {
            let r = call(core_a.0, &query(8, 19, 0x0014, ""));
            for ev in r.ok()["events"].as_array().unwrap() {
                if ev["type"] == "incoming" {
                    a_link = ev["link_id"].as_u64();
                }
            }
            if a_link.is_some() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        let a_link = a_link.expect("A never saw the incoming link");

        // PING -> PONG over frames both ways, on the envelope.
        let ping = b64_encode(b"PING");
        let r = call(
            core_b.0,
            &command(
                9,
                19,
                0x0012,
                &format!(r#"{{"link_id":{b_link},"payload_b64":"{ping}"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let mut got = String::new();
        for _ in 0..50 {
            let r = call(core_a.0, &query(10, 19, 0x0014, ""));
            for ev in r.ok()["events"].as_array().unwrap() {
                if ev["type"] == "frame" && ev["link_id"] == a_link {
                    got = b64_decode(ev["payload_b64"].as_str().unwrap())
                        .map(|v| String::from_utf8_lossy(&v).into_owned())
                        .unwrap_or_default();
                }
            }
            if !got.is_empty() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        assert_eq!(got, "PING");

        let pong = b64_encode(b"PONG");
        let r = call(
            core_a.0,
            &command(
                11,
                19,
                0x0012,
                &format!(r#"{{"link_id":{a_link},"payload_b64":"{pong}"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let mut got = String::new();
        for _ in 0..50 {
            let r = call(core_b.0, &query(12, 19, 0x0014, ""));
            for ev in r.ok()["events"].as_array().unwrap() {
                if ev["type"] == "frame" && ev["link_id"] == b_link {
                    got = b64_decode(ev["payload_b64"].as_str().unwrap())
                        .map(|v| String::from_utf8_lossy(&v).into_owned())
                        .unwrap_or_default();
                }
            }
            if !got.is_empty() {
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(10));
        }
        assert_eq!(got, "PONG");

        // Revocation refuses NEW links (the live one survives, like
        // restricted discovery's documented caveat).
        let r = call(
            core_a.0,
            &command(13, 19, 0x000f, r#"{"nickname":"peerB"}"#),
        );
        assert_eq!(r.ok()["removed"], true);
        let r = call(
            core_b.0,
            &command(14, 19, 0x0011, r#"{"endpoint":"mem:A","nickname":"peerB"}"#),
        );
        assert_eq!(r.err().0, "op_failed");

        // Status reports honestly; stop is idempotent.
        let r = call(core_a.0, &query(15, 19, 0x000d, ""));
        assert_eq!(r.ok()["running"], true);
        assert_eq!(r.ok()["backend"], "mem");
        assert_eq!(r.ok()["links"], 1);
        let r = call(core_a.0, &command(16, 19, 0x000c, ""));
        assert_eq!(r.ok()["stopped"], true);
        let r = call(core_a.0, &command(17, 19, 0x000c, ""));
        assert_eq!(r.ok()["stopped"], false);
        let r = call(core_b.0, &command(18, 19, 0x000c, ""));
        assert_eq!(r.ok()["stopped"], true);
    }

    /// SYNC04: the bookmarks adapter over the real envelope — a
    /// bookmarks.create on core A lands a signed link op in A's log;
    /// importing on B applies it into B's own profile xbel.
    #[test]
    fn sync_bookmarks_adapter_end_to_end() {
        let _g = guard();
        let dir_a = tmpdir("bmsA");
        let dir_b = tmpdir("bmsB");
        let core_a = TestCore::new(&dir_a);
        let core_b = TestCore::new(&dir_b);

        // Pair each device into the other's "default" sync group.
        let a_pk = call(core_a.0, &query(1, 19, 0x0001, "")).ok()["pubkey_b64"]
            .as_str()
            .unwrap()
            .to_string();
        let b_pk = call(core_b.0, &query(2, 19, 0x0001, "")).ok()["pubkey_b64"]
            .as_str()
            .unwrap()
            .to_string();
        let r = call(
            core_a.0,
            &command(
                3,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{b_pk}","scope":"default"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_b.0,
            &command(
                4,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{a_pk}","scope":"default"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);

        // A creates a folder + bookmark through the bookmarks
        // domain — the store events flush into signed ops.
        let r = call(
            core_a.0,
            &command(
                5,
                4,
                0x0002,
                r#"{"profile":"default","node":{"type":1,"title":"F"},"row":-1}"#,
            ),
        );
        let f = r.ok()["id"].as_u64().unwrap();
        assert!(f > 1);
        let r = call(
            core_a.0,
            &command(
                6,
                4,
                0x0002,
                &format!(
                    r#"{{"profile":"default","parent":{f},"node":{{"type":2,"title":"bm","url":"http://x/"}},"row":-1}}"#
                ),
            ),
        );
        assert!(r.ok()["id"].as_u64().unwrap() > 1);

        // A's op-log now tails the store: two link ops, scope
        // "default".
        let r = call(
            core_a.0,
            &query(7, 19, 0x0007, r#"{"store":"bookmarks"}"#),
        );
        let ops = r.ok()["ops"].clone();
        assert_eq!(ops.as_array().unwrap().len(), 2);
        assert_eq!(ops[0]["scope"], "default");

        // B imports — the adapter binds B's profile file and lands
        // the subtree.
        let r = call(
            core_b.0,
            &command(
                8,
                19,
                0x0008,
                &serde_json::to_string(&json!({"ops": ops})).unwrap(),
            ),
        );
        let p = r.ok();
        assert_eq!(p["accepted"], 2, "{p}");
        assert_eq!(p["applied"], 2, "{p}");

        // B's tree now shows the folder.
        let r = call(core_b.0, &query(9, 4, 0x0001, r#"{"profile":"default"}"#));
        assert_eq!(r.ok()["child_count"], 1);
        // And B's op-log recorded the applied remote ops, so a
        // re-import dedups instead of re-applying.
        let r = call(
            core_b.0,
            &command(
                10,
                19,
                0x0008,
                &serde_json::to_string(&json!({"ops": ops})).unwrap(),
            ),
        );
        let p = r.ok();
        assert_eq!(p["duplicates"], 2, "{p}");
        assert_eq!(p["applied"], 0, "{p}");
        let _ = dir_b;
    }

    /// SYNC05: the history adapter over the real envelope — a
    /// history.add on core A lands a signed visit op in A's log;
    /// importing on B applies it into B's own profile history.db,
    /// and B's local deletes never travel back.
    #[test]
    fn sync_history_adapter_end_to_end() {
        let _g = guard();
        let dir_a = tmpdir("hsyA");
        let dir_b = tmpdir("hsyB");
        let core_a = TestCore::new(&dir_a);
        let core_b = TestCore::new(&dir_b);

        // Pair each device into the other's "default" sync group.
        let a_pk = call(core_a.0, &query(1, 19, 0x0001, "")).ok()["pubkey_b64"]
            .as_str()
            .unwrap()
            .to_string();
        let b_id = call(core_b.0, &query(2, 19, 0x0001, "")).ok()["device_id"]
            .as_str()
            .unwrap()
            .to_string();
        let b_pk = call(core_b.0, &query(22, 19, 0x0001, "")).ok()["pubkey_b64"]
            .as_str()
            .unwrap()
            .to_string();
        let r = call(
            core_a.0,
            &command(
                3,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{b_pk}","scope":"default"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_b.0,
            &command(
                4,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{a_pk}","scope":"default"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);

        // A records a visit through the history domain — the store
        // event flushes into a signed op.
        let r = call(
            core_a.0,
            &command(
                5,
                3,
                0x0002,
                r#"{"profile":"default","url":"http://a.example/","title":"A","ts_ms":4242}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);

        // A's op-log now tails the store: one visit op, scope
        // "default".
        let r = call(
            core_a.0,
            &query(6, 19, 0x0007, r#"{"store":"history"}"#),
        );
        let ops = r.ok()["ops"].clone();
        assert_eq!(ops.as_array().unwrap().len(), 1, "{ops}");
        assert_eq!(ops[0]["scope"], "default");
        assert_eq!(ops[0]["body"]["t"], "visit");

        // B imports — the adapter binds B's profile db and lands
        // the row.
        let r = call(
            core_b.0,
            &command(
                7,
                19,
                0x0008,
                &serde_json::to_string(&json!({"ops": ops})).unwrap(),
            ),
        );
        let p = r.ok();
        assert_eq!(p["accepted"], 1, "{p}");
        assert_eq!(p["applied"], 1, "{p}");

        // B's listing now shows the visit.
        let r = call(
            core_b.0,
            &query(8, 3, 0x0003, r#"{"profile":"default","spec":{}}"#),
        );
        let rows = r.ok().as_array().unwrap().clone();
        assert_eq!(rows.len(), 1, "{rows:?}");
        assert_eq!(rows[0]["url"], "http://a.example/");
        assert_eq!(rows[0]["ts"], 4242);

        // Re-import dedups; B's local remove emits no op back.
        let r = call(
            core_b.0,
            &command(
                9,
                19,
                0x0008,
                &serde_json::to_string(&json!({"ops": ops})).unwrap(),
            ),
        );
        assert_eq!(r.ok()["duplicates"], 1);
        let r = call(
            core_b.0,
            &command(
                10,
                3,
                0x0007,
                r#"{"profile":"default","url":"http://a.example/","title":"A","ts_ms":4242}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_b.0,
            &query(11, 19, 0x0007, r#"{"store":"history"}"#),
        );
        // The log carries A's imported remote op, but B must have
        // authored nothing — deletes never emit.
        let b_ops = r.ok()["ops"].as_array().unwrap();
        assert!(
            b_ops.iter().all(|o| o["device_id"].as_str().unwrap() != b_id),
            "a local delete must never emit an op: {b_ops:?}"
        );
        let _ = dir_b;
    }

    /// SYNC06: tab presence over the real envelope — tabs_publish on
    /// core A signs a whole-doc snapshot; B's import lands it in the
    /// in-memory view tabs_peers reads; a republish with a tab closed
    /// replaces the doc outright; and B's history stays EMPTY —
    /// presence is a view, never a visit.
    #[test]
    fn sync_tabs_presence_end_to_end() {
        let _g = guard();
        let dir_a = tmpdir("tsyA");
        let dir_b = tmpdir("tsyB");
        let core_a = TestCore::new(&dir_a);
        let core_b = TestCore::new(&dir_b);

        // Pair each device into the other's "default" sync group.
        let a_pk = call(core_a.0, &query(1, 19, 0x0001, "")).ok()["pubkey_b64"]
            .as_str()
            .unwrap()
            .to_string();
        let b_pk = call(core_b.0, &query(2, 19, 0x0001, "")).ok()["pubkey_b64"]
            .as_str()
            .unwrap()
            .to_string();
        let a_id = call(core_a.0, &query(21, 19, 0x0001, "")).ok()["device_id"]
            .as_str()
            .unwrap()
            .to_string();
        let r = call(
            core_a.0,
            &command(
                3,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{b_pk}","scope":"default"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_b.0,
            &command(
                4,
                19,
                0x0003,
                &format!(r#"{{"pubkey_b64":"{a_pk}","scope":"default"}}"#),
            ),
        );
        assert_eq!(r.ok()["ok"], true);

        // A publishes a two-tab window — one signed presence op.
        let r = call(
            core_a.0,
            &command(
                5,
                19,
                0x001b,
                r#"{"profile":"default","windows":[{"current":0,"tabs":[
                    {"url":"http://one/","title":"One"},
                    {"url":"http://two/","pinned":true}]}]}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true, "{:?}", r.err());
        let r = call(
            core_a.0,
            &query(6, 19, 0x0007, r#"{"store":"tabs"}"#),
        );
        let ops = r.ok()["ops"].clone();
        assert_eq!(ops.as_array().unwrap().len(), 1, "{ops}");
        assert_eq!(ops[0]["scope"], "default");
        assert_eq!(ops[0]["body"]["t"], "presence");

        // B imports — the view shows A's doc, not B's (empty) one.
        let r = call(
            core_b.0,
            &command(
                7,
                19,
                0x0008,
                &serde_json::to_string(&json!({"ops": ops})).unwrap(),
            ),
        );
        let p = r.ok();
        assert_eq!(p["accepted"], 1, "{p}");
        assert_eq!(p["applied"], 1, "{p}");
        let r = call(
            core_b.0,
            &query(8, 19, 0x001c, r#"{"profile":"default"}"#),
        );
        let devices = r.ok()["devices"].as_array().unwrap().clone();
        assert_eq!(devices.len(), 1, "{devices:?}");
        assert_eq!(devices[0]["device_id"], a_id);
        let tabs = devices[0]["windows"][0]["tabs"].as_array().unwrap();
        assert_eq!(tabs.len(), 2);
        assert_eq!(tabs[0]["title"], "One");
        assert_eq!(tabs[1]["pinned"], true);

        // A republishes with the second tab closed — the newer
        // whole-doc wins outright, so the close propagates.
        let r = call(
            core_a.0,
            &command(
                9,
                19,
                0x001b,
                r#"{"profile":"default","windows":[{"current":0,"tabs":[
                    {"url":"http://one/","title":"One"}]}]}"#,
            ),
        );
        assert_eq!(r.ok()["ok"], true);
        let r = call(
            core_a.0,
            &query(10, 19, 0x0007, r#"{"store":"tabs"}"#),
        );
        let ops = r.ok()["ops"].clone();
        assert_eq!(ops.as_array().unwrap().len(), 2);
        let r = call(
            core_b.0,
            &command(
                11,
                19,
                0x0008,
                &serde_json::to_string(&json!({"ops": ops})).unwrap(),
            ),
        );
        assert_eq!(r.ok()["applied"], 1, "{:?}", r.ok());
        let r = call(
            core_b.0,
            &query(12, 19, 0x001c, r#"{"profile":"default"}"#),
        );
        let devices = r.ok()["devices"].as_array().unwrap().clone();
        let tabs = devices[0]["windows"][0]["tabs"].as_array().unwrap();
        assert_eq!(tabs.len(), 1, "{devices:?}");
        assert_eq!(tabs[0]["url"], "http://one/");

        // The apply lane wrote NOTHING — B's profile history is
        // still empty.  A peer's open tabs are a view, not a visit.
        let r = call(
            core_b.0,
            &query(13, 3, 0x0003, r#"{"profile":"default","spec":{}}"#),
        );
        assert_eq!(r.ok().as_array().unwrap().len(), 0,
            "presence apply must never mint a history row");
        let _ = dir_b;
    }
}
