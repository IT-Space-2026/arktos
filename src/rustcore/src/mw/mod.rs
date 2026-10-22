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
use std::path::PathBuf;
use std::ptr;

use serde_json::{json, Value};

use crate::error::{self, RcStatus};
use crate::facade::{self, AcCore};
use crate::{notify, omnibox, session, settings, util};

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
const EVENT_TOPICS: [&str; 14] = [
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
];

// ---- session (domain 6) ------------------------------------------------------
//
// Path-bound like the AcSession handle: every op resolves
// profiles/<id>/session.dat under the core root — no process-global
// anchoring.  Session ops emit no change events by contract
// (per-run scratch state, MW-ARCH §5).

/// Resolves the bound profile's session.dat; the stamped id is the
/// effective profile (a non-slug/absent id resolves to "default").
fn session_file(ctx: &MwCtx, p: &Value) -> Result<(PathBuf, String), OpErr> {
    let eff = facade::effective_profile(profile_arg(p)?.unwrap_or("")).to_string();
    let root = std::path::Path::new(ctx.core.data_root());
    Ok((facade::profile_dir(root, &eff).join("session.dat"), eff))
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

const SERVICES_OPS: &[Op] = &[Op {
    id: 0x0001,
    name: "classify_input",
    class: OpClass::Query,
    profile_scoped: false,
    since: 1,
    handler: op_classify_input,
}];

// ---- registry ------------------------------------------------------------------
//
// Domain ids are MW-ARCH §7's, frozen here.  Domains with no ops yet
// register their name so mw.describe reports the full map with an
// honest empty ops list.

const DOMAINS: &[Domain] = &[
    Domain { id: 0, name: "mw", ops: MW_OPS },
    Domain { id: 1, name: "settings", ops: SETTINGS_OPS },
    Domain { id: 2, name: "profiles", ops: &[] },
    Domain { id: 3, name: "history", ops: &[] },
    Domain { id: 4, name: "bookmarks", ops: &[] },
    Domain { id: 5, name: "policy", ops: &[] },
    Domain { id: 6, name: "session", ops: SESSION_OPS },
    Domain { id: 7, name: "cookies", ops: &[] },
    Domain { id: 8, name: "icons", ops: &[] },
    Domain { id: 9, name: "containers", ops: &[] },
    Domain { id: 10, name: "notes", ops: &[] },
    Domain { id: 11, name: "custody", ops: &[] },
    Domain { id: 12, name: "opensearch", ops: &[] },
    Domain { id: 13, name: "updates", ops: &[] },
    Domain { id: 14, name: "services", ops: SERVICES_OPS },
    Domain { id: 15, name: "extverify", ops: &[] },
    Domain { id: 16, name: "vault", ops: &[] },
    Domain { id: 17, name: "tor", ops: &[] },
    Domain { id: 18, name: "bidi", ops: &[] },
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
        assert_eq!(names, ["describe", "version", "ping"]);
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
        // name, no dispatchable surface yet).
        let r = call(core.0, &query(62, 3, 0x0001, ""));
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
}
