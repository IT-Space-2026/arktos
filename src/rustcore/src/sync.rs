//! SYNC01 — serverless sync engine core, engine-agnostic at the
//! MW-ARCH store boundary.
//!
//! No server anywhere: every device is a peer.  This module is the
//! plumbing the later SYNC rows build on — identity, the group
//! membership store, the signed op-log, and the deterministic
//! apply/dedup machinery.  Transports (SYNC02 Tor, SYNC10 LAN) only
//! ever move wire ops; store adapters (SYNC04+) register appliers
//! that turn verified remote ops into local store mutations.
//!
//!   <data root>/sync/device.key — Ed25519 PKCS#8 identity, 0600.
//!       Generated once, persisted, NEVER synced — a device id is
//!       the hex of its 32-byte public key.
//!   <data root>/sync/sync.db    — members + oplog (WAL sqlite,
//!       same pragmas as cookies.db).
//!
//! Wire op (JSON, `v:1`): a content-addressed, signed CRDT op.
//!
//!   {op_id, store, scope, device_id, lamport, wall_ms, body, sig_b64}
//!
//! The signature covers a fixed little-endian frame over the op's
//! content fields (see signed_frame()); `op_id` is the hex SHA-256
//! of that frame, so dedup is content-addressed and transport-
//! agnostic.  `scope` is the profile the op belongs to ("" = the
//! device-global scope) — the sync group itself is device-global,
//! so SyncEngine opens on the data root, not a profile dir.
//!
//! Apply semantics: an incoming batch is verified (Ed25519 against
//! the signer's member pubkey, or our own key for loopback), deduped
//! by op_id, then sorted into the canonical order (lamport,
//! device_id, op_id) before being handed to the registered applier —
//! arrival order can never change the apply order.  Ops for a store
//! with no registered applier stay durable in the oplog flagged
//! applied=0 and are reported as `deferred`, never silently dropped.

use std::collections::BTreeMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex, RwLock};

use ring::rand::SystemRandom;
use ring::signature::{Ed25519KeyPair, KeyPair, UnparsedPublicKey, ED25519};
use rusqlite::{params, Connection};
use serde_json::Value;
use sha2::{Digest, Sha256};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::store;

pub const SYNC_DIR: &str = "sync";
pub const DEVICE_KEY_FILE: &str = "device.key";
pub const DB_FILE: &str = "sync.db";
const SCHEMA: &str = "2";

/// The stores the op-log tails — SYNC04–SYNC08 land the per-store
/// CRDT adapters that produce and consume their bodies.
pub const SYNCED_STORES: [&str; 5] =
    ["bookmarks", "history", "tabs", "settings", "passwords"];

const OP_FRAME_MAGIC: &[u8; 6] = b"AROPS1";
const WIRE_VERSION: i64 = 1;

const MAX_BODY_BYTES: usize = 1024 * 1024;
const MAX_STORE_LEN: usize = 64;
const MAX_SCOPE_LEN: usize = 128;
const MAX_ONION_LEN: usize = 256;

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn hex(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for b in data {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

/// Canonical JSON: objects serialize with sorted keys, no
/// whitespace — deterministic regardless of serde_json's
/// preserve_order feature, so the signed bytes are stable for any
/// producer/verifier pair running this code.
fn canon(v: &Value, out: &mut String) {
    match v {
        Value::Null => out.push_str("null"),
        Value::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Value::Number(n) => out.push_str(&n.to_string()),
        Value::String(s) => out.push_str(&serde_json::to_string(s).unwrap_or_default()),
        Value::Array(items) => {
            out.push('[');
            for (i, item) in items.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                canon(item, out);
            }
            out.push(']');
        }
        Value::Object(map) => {
            let sorted: BTreeMap<&String, &Value> = map.iter().collect();
            out.push('{');
            for (i, (k, v)) in sorted.iter().enumerate() {
                if i > 0 {
                    out.push(',');
                }
                out.push_str(&serde_json::to_string(k).unwrap_or_default());
                out.push(':');
                canon(v, out);
            }
            out.push('}');
        }
    }
}

/// The byte frame a device signs: magic | len-prefixed fields |
/// lamport | wall_ms | len-prefixed canonical body.  op_id =
/// SHA-256 over the whole frame — the dedup key commits to every
/// signed field.
fn signed_frame(
    store: &str,
    scope: &str,
    device_id: &str,
    lamport: u64,
    wall_ms: i64,
    body_canon: &str,
) -> Vec<u8> {
    let mut v = Vec::new();
    let field = |v: &mut Vec<u8>, s: &str| {
        v.extend_from_slice(&(s.len() as u32).to_le_bytes());
        v.extend_from_slice(s.as_bytes());
    };
    v.extend_from_slice(OP_FRAME_MAGIC);
    field(&mut v, store);
    field(&mut v, scope);
    field(&mut v, device_id);
    v.extend_from_slice(&lamport.to_le_bytes());
    v.extend_from_slice(&wall_ms.to_le_bytes());
    field(&mut v, body_canon);
    v
}

/// A member of the sync group — one paired device.  `scopes` lists
/// the profile sync groups the device is bound to (SYNC03: pairing
/// binds a peer to ONE profile's group; an empty list means a
/// device-global member — the SYNC01 spelling, kept for ops logged
/// before scope groups existed).
#[derive(Clone, Debug, PartialEq)]
pub struct Member {
    pub device_id: String,
    pub pubkey: [u8; 32],
    pub onion_addr: String,
    pub last_seen_ms: i64,
    pub added_ms: i64,
    pub scopes: Vec<String>,
}

/// One op on the wire — the shape export() emits and import()
/// consumes.  `sig` is the raw 64-byte Ed25519 signature.
#[derive(Clone, Debug)]
pub struct WireOp {
    pub op_id: String,
    pub store: String,
    pub scope: String,
    pub device_id: String,
    pub lamport: u64,
    pub wall_ms: i64,
    pub body: Value,
    pub sig: Vec<u8>,
}

impl WireOp {
    fn frame(&self) -> Vec<u8> {
        let mut body = String::new();
        canon(&self.body, &mut body);
        signed_frame(
            &self.store,
            &self.scope,
            &self.device_id,
            self.lamport,
            self.wall_ms,
            &body,
        )
    }

    /// The content-addressed dedup key — recomputed from content,
    /// never trusted from the wire.
    pub fn content_id(&self) -> String {
        hex(&Sha256::digest(self.frame()))
    }

    /// Canonical wire order for apply: (lamport, device_id, op_id).
    fn order_key(&self) -> (u64, &str, &str) {
        (self.lamport, self.device_id.as_str(), self.op_id.as_str())
    }

    pub fn to_json(&self) -> Value {
        serde_json::json!({
            "v": WIRE_VERSION,
            "op_id": self.op_id,
            "store": self.store,
            "scope": self.scope,
            "device_id": self.device_id,
            "lamport": self.lamport,
            "wall_ms": self.wall_ms,
            "body": self.body,
            "sig_b64": crate::mw::b64_encode(&self.sig),
        })
    }

    /// Strict wire-doc parse — Err(reason) is the per-op rejection
    /// string import() reports, never a panic on hostile input.
    pub fn from_json(v: &Value) -> Result<WireOp, String> {
        if v.get("v").and_then(Value::as_i64) != Some(WIRE_VERSION) {
            return Err("bad wire version".into());
        }
        let s = |name: &str, max: usize| -> Result<String, String> {
            let s = v
                .get(name)
                .and_then(Value::as_str)
                .ok_or_else(|| format!("missing string field \"{name}\""))?;
            if s.len() > max {
                return Err(format!("\"{name}\" exceeds {max} bytes"));
            }
            Ok(s.to_string())
        };
        let store = s("store", MAX_STORE_LEN)?;
        let scope = s("scope", MAX_SCOPE_LEN)?;
        let device_id = s("device_id", 128)?;
        let op_id = s("op_id", 128)?;
        let lamport = v
            .get("lamport")
            .and_then(Value::as_u64)
            .ok_or_else(|| "\"lamport\" must be a non-negative integer".to_string())?;
        let wall_ms = v
            .get("wall_ms")
            .and_then(Value::as_i64)
            .ok_or_else(|| "\"wall_ms\" must be an integer".to_string())?;
        let body = v
            .get("body")
            .cloned()
            .ok_or_else(|| "missing \"body\" doc".to_string())?;
        let sig_b64 = s("sig_b64", 128)?;
        let sig = crate::mw::b64_decode(&sig_b64)
            .ok_or_else(|| "\"sig_b64\" is not valid base64".to_string())?;
        if sig.len() != 64 {
            return Err("\"sig_b64\" must decode to a 64-byte signature".into());
        }
        Ok(WireOp {
            op_id,
            store,
            scope,
            device_id,
            lamport,
            wall_ms,
            body,
            sig,
        })
    }
}

/// The apply half of a synced-store adapter — `scope` is the op's
/// profile scope, `body` the CRDT payload.  Must be idempotent:
/// dedup means an op applies at most once per device, but a retry
/// after a mid-apply crash can re-deliver it.
pub type Applier = Arc<dyn Fn(&str, &Value) -> RcResult<()> + Send + Sync>;

static APPLIERS: RwLock<BTreeMap<String, Applier>> = RwLock::new(BTreeMap::new());

/// The seam SYNC04+ store adapters plug into: register the apply
/// function for a synced store name.  Unused until those adapters
/// land — the crate-internal pub keeps the seam inside rustcore.
#[allow(dead_code)]
pub fn register_applier(store: &str, f: Applier) {
    APPLIERS
        .write()
        .unwrap_or_else(|e| e.into_inner())
        .insert(store.to_string(), f);
}

/// Test hook — drop a registration so one suite's applier cannot
/// leak into a neighbour's import.
#[cfg(test)]
pub fn unregister_applier(store: &str) {
    APPLIERS
        .write()
        .unwrap_or_else(|e| e.into_inner())
        .remove(store);
}

fn applier_for(store: &str) -> Option<Applier> {
    APPLIERS
        .read()
        .unwrap_or_else(|e| e.into_inner())
        .get(store)
        .cloned()
}

/// Per-op rejection — why a wire op was refused.
#[derive(Debug)]
pub struct Reject {
    pub op_id: String,
    pub reason: String,
}

/// What an import() batch did, honestly counted.
#[derive(Debug, Default)]
pub struct ImportReport {
    /// Verified + new + inserted into the oplog.
    pub accepted: u32,
    /// Handed to a registered store applier.
    pub applied: u32,
    /// Accepted but no applier registered for the store — durable
    /// in the log, drainable later.
    pub deferred: u32,
    /// Already known op_ids (idempotent re-delivery).
    pub duplicates: u32,
    /// Refused ops with reasons.
    pub rejected: Vec<Reject>,
    /// A registered applier returned an error — the op stays in the
    /// log flagged applied=0 for a later retry.
    pub apply_errors: Vec<Reject>,
}

struct State {
    conn: Connection,
    key: Ed25519KeyPair,
    pubkey: [u8; 32],
}

/// The per-device sync engine: identity + membership + op-log for
/// one data root.  Open per call like RcSettings — the MW handlers
/// bind it to the core's data root.
pub struct SyncEngine {
    state: Mutex<State>,
}

fn load_or_create_identity(dir: &std::path::Path) -> RcResult<(Ed25519KeyPair, [u8; 32])> {
    let path = dir.join(DEVICE_KEY_FILE);
    if path.exists() {
        let bytes = std::fs::read(&path)?;
        if let Ok(pair) = Ed25519KeyPair::from_pkcs8(&bytes) {
            let mut pubkey = [0u8; 32];
            pubkey.copy_from_slice(pair.public_key().as_ref());
            store::tighten_perms(&path);
            return Ok((pair, pubkey));
        }
        // Corrupt identity: refusing to silently re-key keeps an
        // attacker who can write one file from swapping the device's
        // whole sync identity.  Honest Corrupt, like the .kdf path.
        return fail(
            RcStatus::Corrupt,
            "sync/device.key is not a valid Ed25519 keypair",
        );
    }
    let doc = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).map_err(|e| Fail {
        status: RcStatus::Unavailable,
        msg: format!("ed25519 keygen: {e}"),
    })?;
    let pair = Ed25519KeyPair::from_pkcs8(doc.as_ref()).map_err(|e| Fail {
        status: RcStatus::Crypto,
        msg: format!("ed25519 reload: {e}"),
    })?;
    store::atomic_write(&path, doc.as_ref())?;
    let mut pubkey = [0u8; 32];
    pubkey.copy_from_slice(pair.public_key().as_ref());
    Ok((pair, pubkey))
}

impl SyncEngine {
    /// Open (creating on first use) the sync store under
    /// `<data_root>/sync/` — the identity keypair is minted on first
    /// open and a malformed one is a hard Corrupt, never a silent
    /// re-key.
    pub fn open(data_root: &str) -> RcResult<SyncEngine> {
        if data_root.is_empty() {
            return fail(RcStatus::InvalidArgument, "empty data root");
        }
        let dir = PathBuf::from(data_root).join(SYNC_DIR);
        std::fs::create_dir_all(&dir).map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot create {}: {e}", dir.display()),
        })?;
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            let _ = std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700));
        }
        let (key, pubkey) = load_or_create_identity(&dir)?;
        let db = dir.join(DB_FILE);
        let conn = Connection::open(&db).map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot open {}: {e}", db.display()),
        })?;
        conn.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             PRAGMA busy_timeout = 5000;
             CREATE TABLE IF NOT EXISTS meta(
                 key TEXT PRIMARY KEY,
                 value TEXT NOT NULL);
             CREATE TABLE IF NOT EXISTS members(
                 device_id TEXT PRIMARY KEY,
                 pubkey BLOB NOT NULL,
                 onion_addr TEXT NOT NULL DEFAULT '',
                 last_seen INTEGER NOT NULL DEFAULT 0,
                 added INTEGER NOT NULL DEFAULT 0);
             CREATE TABLE IF NOT EXISTS member_scopes(
                 device_id TEXT NOT NULL,
                 scope TEXT NOT NULL,
                 added INTEGER NOT NULL DEFAULT 0,
                 PRIMARY KEY(device_id, scope));
             CREATE TABLE IF NOT EXISTS oplog(
                 seq INTEGER PRIMARY KEY AUTOINCREMENT,
                 op_id TEXT NOT NULL UNIQUE,
                 origin TEXT NOT NULL,
                 store TEXT NOT NULL,
                 scope TEXT NOT NULL DEFAULT '',
                 device_id TEXT NOT NULL,
                 lamport INTEGER NOT NULL,
                 wall_ms INTEGER NOT NULL,
                 body TEXT NOT NULL,
                 sig BLOB NOT NULL,
                 applied INTEGER NOT NULL DEFAULT 0);
             CREATE INDEX IF NOT EXISTS oplog_store
                 ON oplog(store, seq);
             CREATE INDEX IF NOT EXISTS oplog_pending
                 ON oplog(store, applied);",
        )
        .map_err(|e| sql_fail("schema", e))?;
        if meta_get(&conn, "schema")?.as_deref() != Some(SCHEMA) {
            conn.execute(
                "INSERT INTO meta(key, value) VALUES('schema', ?1)
                 ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                params![SCHEMA],
            )
            .map_err(|e| sql_fail("meta write", e))?;
        }
        Ok(SyncEngine {
            state: Mutex::new(State { conn, key, pubkey }),
        })
    }

    /// This device's id — hex of the Ed25519 public key.  Stable
    /// across restarts, never leaves the membership store as
    /// anything but a peer row someone else paired.
    pub fn device_id(&self) -> String {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        hex(&st.pubkey)
    }

    pub fn public_key(&self) -> [u8; 32] {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.pubkey
    }

    /// Pair a device: records pubkey + onion address, keyed by the
    /// derived device id.  Upserts — re-pairing refreshes the onion
    /// address without moving `added`.  Pairing our own key is a
    /// protocol error, not a member row.
    ///
    /// `scope` binds the peer to ONE profile's sync group (SYNC03) —
    /// "" records a device-global member (the SYNC01 spelling, used
    /// by the loopback legs).  A scoped member signs only its own
    /// scope's ops on import; a device-global member signs any.
    pub fn member_add(&self, pubkey: &[u8; 32], onion_addr: &str, scope: &str) -> RcResult<String> {
        if onion_addr.len() > MAX_ONION_LEN {
            return fail(RcStatus::InvalidArgument, "onion address too long");
        }
        if scope.len() > MAX_SCOPE_LEN {
            return fail(RcStatus::InvalidArgument, "scope too long");
        }
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let id = hex(pubkey);
        if id == hex(&st.pubkey) {
            return fail(RcStatus::InvalidArgument, "cannot pair with self");
        }
        st.conn
            .execute(
                "INSERT INTO members(device_id, pubkey, onion_addr, added)
                 VALUES(?1, ?2, ?3, ?4)
                 ON CONFLICT(device_id) DO UPDATE SET
                     pubkey = excluded.pubkey,
                     onion_addr = excluded.onion_addr",
                params![id, &pubkey[..], onion_addr, now_ms()],
            )
            .map_err(|e| sql_fail("member add", e))?;
        if !scope.is_empty() {
            st.conn
                .execute(
                    "INSERT OR IGNORE INTO member_scopes(device_id, scope, added)
                     VALUES(?1, ?2, ?3)",
                    params![id, scope, now_ms()],
                )
                .map_err(|e| sql_fail("member scope add", e))?;
        }
        Ok(id)
    }

    /// Drop one scope binding; the member row stays (other scopes
    /// and the identity record survive a per-profile unpair —
    /// SYNC09's revocation leans on this).
    #[allow(dead_code)]
    pub fn member_scope_remove(&self, device_id: &str, scope: &str) -> RcResult<bool> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let n = st
            .conn
            .execute(
                "DELETE FROM member_scopes WHERE device_id=?1 AND scope=?2",
                params![device_id, scope],
            )
            .map_err(|e| sql_fail("member scope remove", e))?;
        Ok(n > 0)
    }

    pub fn member_remove(&self, device_id: &str) -> RcResult<bool> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.conn
            .execute(
                "DELETE FROM member_scopes WHERE device_id=?1",
                params![device_id],
            )
            .map_err(|e| sql_fail("member scopes remove", e))?;
        let n = st
            .conn
            .execute("DELETE FROM members WHERE device_id=?1", params![device_id])
            .map_err(|e| sql_fail("member remove", e))?;
        Ok(n > 0)
    }

    /// Bump a member's last-seen — `None` stamps now.  Unknown
    /// device ids are NotFound, never an implicit member row.
    pub fn member_seen(&self, device_id: &str, ts_ms: Option<i64>) -> RcResult<()> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let n = st
            .conn
            .execute(
                "UPDATE members SET last_seen=?2 WHERE device_id=?1",
                params![device_id, ts_ms.unwrap_or_else(now_ms)],
            )
            .map_err(|e| sql_fail("member seen", e))?;
        if n == 0 {
            return fail(RcStatus::NotFound, "unknown sync device");
        }
        Ok(())
    }

    pub fn members_list(&self) -> RcResult<Vec<Member>> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut q = st
            .conn
            .prepare(
                "SELECT device_id, pubkey, onion_addr, last_seen, added
                 FROM members ORDER BY added, device_id",
            )
            .map_err(|e| sql_fail("members query", e))?;
        let rows = q
            .query_map([], |r| {
                let pk: Vec<u8> = r.get(1)?;
                let mut pubkey = [0u8; 32];
                if pk.len() == 32 {
                    pubkey.copy_from_slice(&pk);
                }
                Ok(Member {
                    device_id: r.get(0)?,
                    pubkey,
                    onion_addr: r.get(2)?,
                    last_seen_ms: r.get(3)?,
                    added_ms: r.get(4)?,
                    scopes: Vec::new(),
                })
            })
            .map_err(|e| sql_fail("members query", e))?;
        let mut members = rows
            .collect::<rusqlite::Result<Vec<_>>>()
            .map_err(|e| sql_fail("members read", e))?;
        for m in &mut members {
            m.scopes = member_scopes(&st.conn, &m.device_id)?;
        }
        Ok(members)
    }

    /// The op-log tailing seam: a synced-store adapter calls this
    /// after a local mutation commits.  Signs the op under the
    /// device key, stamps the next Lamport tick, appends it to the
    /// log already-applied — the local change happened before the
    /// op exists.
    pub fn append(&self, store_name: &str, scope: &str, body: &Value) -> RcResult<WireOp> {
        if !SYNCED_STORES.contains(&store_name) {
            return fail(
                RcStatus::InvalidArgument,
                format!("\"{store_name}\" is not a synced store"),
            );
        }
        if scope.len() > MAX_SCOPE_LEN {
            return fail(RcStatus::InvalidArgument, "scope too long");
        }
        let mut canon_body = String::new();
        canon(body, &mut canon_body);
        if canon_body.len() > MAX_BODY_BYTES {
            return fail(RcStatus::InvalidArgument, "op body too large");
        }
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let lamport = next_lamport(&st.conn)?;
        let wall_ms = now_ms();
        let device_id = hex(&st.pubkey);
        let frame = signed_frame(store_name, scope, &device_id, lamport, wall_ms, &canon_body);
        let op_id = hex(&Sha256::digest(&frame));
        let sig = st.key.sign(&frame).as_ref().to_vec();
        st.conn
            .execute(
                "INSERT INTO oplog(op_id, origin, store, scope, device_id,
                                    lamport, wall_ms, body, sig, applied)
                 VALUES(?1,'local',?2,?3,?4,?5,?6,?7,?8,1)",
                params![
                    op_id, store_name, scope, device_id, lamport as i64,
                    wall_ms, canon_body, sig
                ],
            )
            .map_err(|e| sql_fail("oplog append", e))?;
        Ok(WireOp {
            op_id,
            store: store_name.to_string(),
            scope: scope.to_string(),
            device_id,
            lamport,
            wall_ms,
            body: body.clone(),
            sig,
        })
    }

    /// The transport drain: every logged op with seq > `since_seq`,
    /// optionally one store only, in log order.  SYNC02's shipper
    /// tails this per store.
    pub fn export(&self, store_name: Option<&str>, since_seq: i64) -> RcResult<Vec<WireOp>> {
        if let Some(s) = store_name {
            if !SYNCED_STORES.contains(&s) {
                return fail(
                    RcStatus::InvalidArgument,
                    format!("\"{s}\" is not a synced store"),
                );
            }
        }
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let sql = if store_name.is_some() {
            "SELECT op_id, store, scope, device_id, lamport, wall_ms, body, sig
             FROM oplog WHERE seq > ?1 AND store = ?2 ORDER BY seq"
        } else {
            "SELECT op_id, store, scope, device_id, lamport, wall_ms, body, sig
             FROM oplog WHERE seq > ?1 ORDER BY seq"
        };
        let mut q = st.conn.prepare(sql).map_err(|e| sql_fail("export", e))?;
        let rows = match store_name {
            Some(s) => q
                .query_map(params![since_seq, s], read_op_row)
                .map_err(|e| sql_fail("export", e))?,
            None => q
                .query_map(params![since_seq], read_op_row)
                .map_err(|e| sql_fail("export", e))?,
        };
        let mut out = Vec::new();
        for row in rows {
            let (op_id, store_name, scope, device_id, lamport, wall_ms, body_txt, sig) =
                row.map_err(|e| sql_fail("export", e))?;
            let body: Value = serde_json::from_str(&body_txt).unwrap_or(Value::Null);
            out.push(WireOp {
                op_id,
                store: store_name,
                scope,
                device_id,
                lamport: lamport as u64,
                wall_ms,
                body,
                sig,
            });
        }
        Ok(out)
    }

    /// Ops accepted for a store that no applier has consumed yet —
    /// a late-registering adapter drains these on startup.
    pub fn pending_apply(&self, store_name: &str) -> RcResult<Vec<WireOp>> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let mut q = st
            .conn
            .prepare(
                "SELECT op_id, store, scope, device_id, lamport, wall_ms, body, sig
                 FROM oplog WHERE store=?1 AND applied=0
                 ORDER BY lamport, device_id, op_id",
            )
            .map_err(|e| sql_fail("pending_apply", e))?;
        let rows = q
            .query_map(params![store_name], read_op_row)
            .map_err(|e| sql_fail("pending_apply", e))?;
        let mut out = Vec::new();
        for row in rows {
            let (op_id, store_name, scope, device_id, lamport, wall_ms, body_txt, sig) =
                row.map_err(|e| sql_fail("pending_apply", e))?;
            let body: Value = serde_json::from_str(&body_txt).unwrap_or(Value::Null);
            out.push(WireOp {
                op_id,
                store: store_name,
                scope,
                device_id,
                lamport: lamport as u64,
                wall_ms,
                body,
                sig,
            });
        }
        Ok(out)
    }

    /// Consume a batch of remote ops.  Each op is verified against
    /// the signer's member pubkey (or our own key — loopback self-
    /// sync), deduped by its recomputed content id, then the whole
    /// accepted batch is applied in canonical (lamport, device_id,
    /// op_id) order — arrival order never changes apply order.
    /// Nothing here trusts a wire field it did not recompute.
    pub fn import(&self, docs: &[Value]) -> RcResult<ImportReport> {
        let mut report = ImportReport::default();
        let mut fresh: Vec<WireOp> = Vec::new();
        {
            let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
            let own_id = hex(&st.pubkey);
            for doc in docs {
                let op = match WireOp::from_json(doc) {
                    Ok(o) => o,
                    Err(reason) => {
                        report.rejected.push(Reject {
                            op_id: doc
                                .get("op_id")
                                .and_then(Value::as_str)
                                .unwrap_or("?")
                                .to_string(),
                            reason,
                        });
                        continue;
                    }
                };
                let content_id = op.content_id();
                if content_id != op.op_id {
                    report.rejected.push(Reject {
                        op_id: op.op_id,
                        reason: "op_id does not match signed content".into(),
                    });
                    continue;
                }
                if !SYNCED_STORES.contains(&op.store.as_str()) {
                    report.rejected.push(Reject {
                        op_id: op.op_id,
                        reason: format!("\"{}\" is not a synced store", op.store),
                    });
                    continue;
                }
                let signer: Option<[u8; 32]> = if op.device_id == own_id {
                    Some(st.pubkey)
                } else {
                    member_pubkey(&st.conn, &op.device_id)?
                };
                let Some(pubkey) = signer else {
                    report.rejected.push(Reject {
                        op_id: op.op_id,
                        reason: "unknown device — not a sync member".into(),
                    });
                    continue;
                };
                // SYNC03 scope binding: a member bound to profile
                // groups signs only ITS groups' ops — the paired
                // peer "sees only that profile's stores".  A member
                // with no scope rows is device-global (SYNC01
                // spelling) and signs any scope; our own key is
                // unrestricted by construction.
                if op.device_id != own_id
                    && !member_scope_allows(&st.conn, &op.device_id, &op.scope)?
                {
                    report.rejected.push(Reject {
                        op_id: op.op_id,
                        reason: format!(
                            "device is not a member of the \"{}\" sync group",
                            op.scope
                        ),
                    });
                    continue;
                }
                if UnparsedPublicKey::new(&ED25519, &pubkey)
                    .verify(&op.frame(), &op.sig)
                    .is_err()
                {
                    report.rejected.push(Reject {
                        op_id: op.op_id,
                        reason: "signature verification failed".into(),
                    });
                    continue;
                }
                // Lamport merge rule: our clock must pass every
                // op we accept, so later local ops order after them.
                bump_lamport(&st.conn, op.lamport + 1)?;
                let mut canon_body = String::new();
                canon(&op.body, &mut canon_body);
                let changed = st
                    .conn
                    .execute(
                        "INSERT OR IGNORE INTO oplog(
                             op_id, origin, store, scope, device_id,
                             lamport, wall_ms, body, sig, applied)
                         VALUES(?1,'remote',?2,?3,?4,?5,?6,?7,?8,0)",
                        params![
                            op.op_id, op.store, op.scope, op.device_id,
                            op.lamport as i64, op.wall_ms, canon_body, op.sig
                        ],
                    )
                    .map_err(|e| sql_fail("oplog import", e))?;
                if changed == 0 {
                    report.duplicates += 1;
                    continue;
                }
                report.accepted += 1;
                fresh.push(op);
            }
        }
        // Deterministic apply: canonical order, never arrival order.
        fresh.sort_by(|a, b| a.order_key().cmp(&b.order_key()));
        for op in fresh {
            match applier_for(&op.store) {
                Some(f) => match f(&op.scope, &op.body) {
                    Ok(()) => {
                        self.mark_applied(&op.op_id)?;
                        report.applied += 1;
                    }
                    Err(e) => report.apply_errors.push(Reject {
                        op_id: op.op_id,
                        reason: e.msg,
                    }),
                },
                None => report.deferred += 1,
            }
        }
        Ok(report)
    }

    fn mark_applied(&self, op_id: &str) -> RcResult<()> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.conn
            .execute(
                "UPDATE oplog SET applied=1 WHERE op_id=?1",
                params![op_id],
            )
            .map_err(|e| sql_fail("mark_applied", e))?;
        Ok(())
    }

    /// A retry pass for ops a registered adapter has not consumed —
    /// same canonical order import() applies in.
    pub fn drain_pending(&self, store_name: &str) -> RcResult<ImportReport> {
        let pending = self.pending_apply(store_name)?;
        let mut report = ImportReport::default();
        for op in pending {
            match applier_for(&op.store) {
                Some(f) => match f(&op.scope, &op.body) {
                    Ok(()) => {
                        self.mark_applied(&op.op_id)?;
                        report.applied += 1;
                    }
                    Err(e) => report.apply_errors.push(Reject {
                        op_id: op.op_id,
                        reason: e.msg,
                    }),
                },
                None => report.deferred += 1,
            }
        }
        Ok(report)
    }

    /// Sign `data` under the device identity — SYNC03 uses it to
    /// bind the Noise transport static to the Ed25519 identity in
    /// the pairing handshake's signed-payload field.
    pub fn sign(&self, data: &[u8]) -> Vec<u8> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        st.key.sign(data).as_ref().to_vec()
    }

    /// The 32-byte secret of `scope`'s sync group — None when the
    /// group has no secret yet.  Minted once by the device that owns
    /// the group (the pairing inviter), carried to joiners inside
    /// the Noise channel (SYNC03), consumed by the SYNC04+ store
    /// adapters.
    #[allow(dead_code)]
    pub fn group_secret(&self, scope: &str) -> RcResult<Option<[u8; 32]>> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        group_secret_read(&st.conn, scope)
    }

    /// The group's secret, minting on first ask — the inviter-side
    /// spelling (a group exists once someone invites to it).
    pub fn group_secret_get_or_create(&self, scope: &str) -> RcResult<[u8; 32]> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(k) = group_secret_read(&st.conn, scope)? {
            return Ok(k);
        }
        let key = crate::noisexx::rand_bytes::<32>()?;
        st.conn
            .execute(
                "INSERT INTO meta(key, value) VALUES(?1, ?2)
                 ON CONFLICT(key) DO UPDATE SET value = excluded.value",
                params![group_secret_key(scope), hex(&key)],
            )
            .map_err(|e| sql_fail("group secret write", e))?;
        Ok(key)
    }

    /// The joiner-side spelling: install the secret the inviter
    /// sent.  Idempotent on an identical existing key; a DIFFERENT
    /// stored key is a hard Crypto error — silently swapping group
    /// secrets would quietly move the device into another sync
    /// group (rotation is SYNC09's deliberate act, not a side
    /// effect of re-pairing).
    pub fn group_secret_install(&self, scope: &str, key: &[u8; 32]) -> RcResult<()> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(existing) = group_secret_read(&st.conn, scope)? {
            if &existing == key {
                return Ok(());
            }
            return fail(
                RcStatus::Crypto,
                format!("sync group secret for scope \"{scope}\" conflicts with the stored one"),
            );
        }
        st.conn
            .execute(
                "INSERT INTO meta(key, value) VALUES(?1, ?2)",
                params![group_secret_key(scope), hex(key)],
            )
            .map_err(|e| sql_fail("group secret write", e))?;
        Ok(())
    }

    pub fn stats(&self) -> RcResult<Value> {
        let st = self.state.lock().unwrap_or_else(|e| e.into_inner());
        let members: i64 = st
            .conn
            .query_row("SELECT COUNT(*) FROM members", [], |r| r.get(0))
            .map_err(|e| sql_fail("stats", e))?;
        let ops: i64 = st
            .conn
            .query_row("SELECT COUNT(*) FROM oplog", [], |r| r.get(0))
            .map_err(|e| sql_fail("stats", e))?;
        let mut stores = serde_json::Map::new();
        {
            let mut q = st
                .conn
                .prepare(
                    "SELECT store, COUNT(*), COALESCE(MAX(seq),0),
                            SUM(CASE WHEN applied=0 THEN 1 ELSE 0 END)
                     FROM oplog GROUP BY store",
                )
                .map_err(|e| sql_fail("stats", e))?;
            let rows = q
                .query_map([], |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, i64>(1)?,
                        r.get::<_, i64>(2)?,
                        r.get::<_, Option<i64>>(3)?.unwrap_or(0),
                    ))
                })
                .map_err(|e| sql_fail("stats", e))?;
            for row in rows {
                let (name, count, last_seq, pending) =
                    row.map_err(|e| sql_fail("stats", e))?;
                stores.insert(
                    name,
                    serde_json::json!({
                        "ops": count, "last_seq": last_seq,
                        "pending_apply": pending,
                    }),
                );
            }
        }
        Ok(serde_json::json!({
            "device_id": hex(&st.pubkey),
            "lamport": cur_lamport(&st.conn)?,
            "members": members,
            "ops": ops,
            "stores": Value::Object(stores),
        }))
    }
}

/// Shared oplog row projection: op_id, store, scope, device_id,
/// lamport, wall_ms, body, sig.
fn read_op_row(
    r: &rusqlite::Row,
) -> rusqlite::Result<(String, String, String, String, i64, i64, String, Vec<u8>)> {
    Ok((
        r.get(0)?,
        r.get(1)?,
        r.get(2)?,
        r.get(3)?,
        r.get(4)?,
        r.get(5)?,
        r.get(6)?,
        r.get(7)?,
    ))
}

fn sql_fail(ctx: &str, e: rusqlite::Error) -> Fail {
    Fail {
        status: RcStatus::Io,
        msg: format!("{ctx}: {e}"),
    }
}

fn meta_get(conn: &Connection, key: &str) -> RcResult<Option<String>> {
    conn.query_row("SELECT value FROM meta WHERE key=?1", params![key], |r| {
        r.get(0)
    })
    .map(Some)
    .or_else(|e| match e {
        rusqlite::Error::QueryReturnedNoRows => Ok(None),
        e => Err(sql_fail("meta read", e)),
    })
}

fn cur_lamport(conn: &Connection) -> RcResult<u64> {
    Ok(meta_get(conn, "lamport")?
        .and_then(|v| v.parse().ok())
        .unwrap_or(0))
}

/// Persisted Lamport clock — survives restarts so a device never
/// re-issues a tick a peer could mis-order.
fn next_lamport(conn: &Connection) -> RcResult<u64> {
    let next = cur_lamport(conn)? + 1;
    conn.execute(
        "INSERT INTO meta(key, value) VALUES('lamport', ?1)
         ON CONFLICT(key) DO UPDATE SET value = excluded.value",
        params![next.to_string()],
    )
    .map_err(|e| sql_fail("lamport", e))?;
    Ok(next)
}

/// Merge rule: the clock only ever moves forward past a seen tick.
fn bump_lamport(conn: &Connection, at_least: u64) -> RcResult<()> {
    if at_least > cur_lamport(conn)? {
        conn.execute(
            "INSERT INTO meta(key, value) VALUES('lamport', ?1)
             ON CONFLICT(key) DO UPDATE SET value = excluded.value",
            params![at_least.to_string()],
        )
        .map_err(|e| sql_fail("lamport", e))?;
    }
    Ok(())
}

/// The scopes bound to one member — empty Vec is a device-global
/// member (pre-SYNC03 pairings) or an unknown device.
fn member_scopes(conn: &Connection, device_id: &str) -> RcResult<Vec<String>> {
    let mut q = st_member_scopes(conn)?;
    let rows = q
        .query_map(params![device_id], |r| r.get::<_, String>(0))
        .map_err(|e| sql_fail("member scopes read", e))?;
    rows.collect::<rusqlite::Result<Vec<_>>>()
        .map_err(|e| sql_fail("member scopes read", e))
}

fn st_member_scopes(conn: &Connection) -> RcResult<rusqlite::Statement<'_>> {
    conn.prepare("SELECT scope FROM member_scopes WHERE device_id=?1 ORDER BY scope")
        .map_err(|e| sql_fail("member scopes query", e))
}

/// May `device_id` sign ops for `scope`?  A member with no scope
/// rows is device-global (SYNC01 semantics — the loopback tests and
/// any member added before scope pairing existed).  Once scope rows
/// exist the member is bound: only its listed groups pass.
fn member_scope_allows(conn: &Connection, device_id: &str, scope: &str) -> RcResult<bool> {
    let scopes = member_scopes(conn, device_id)?;
    if scopes.is_empty() {
        return Ok(true);
    }
    Ok(scopes.iter().any(|s| s == scope))
}

/// The meta-table key a scope's group secret lives under.
fn group_secret_key(scope: &str) -> String {
    format!("gkey:{scope}")
}

fn group_secret_read(conn: &Connection, scope: &str) -> RcResult<Option<[u8; 32]>> {
    let Some(raw) = meta_get(conn, &group_secret_key(scope))? else {
        return Ok(None);
    };
    let bytes: Vec<u8> = (0..raw.len())
        .step_by(2)
        .filter_map(|i| u8::from_str_radix(raw.get(i..i + 2)?, 16).ok())
        .collect();
    if bytes.len() != 32 {
        return fail(
            RcStatus::Corrupt,
            format!("stored sync group secret for scope \"{scope}\" is malformed"),
        );
    }
    let mut key = [0u8; 32];
    key.copy_from_slice(&bytes);
    Ok(Some(key))
}

fn member_pubkey(conn: &Connection, device_id: &str) -> RcResult<Option<[u8; 32]>> {
    conn.query_row(
        "SELECT pubkey FROM members WHERE device_id=?1",
        params![device_id],
        |r| r.get::<_, Vec<u8>>(0),
    )
    .map(|pk| {
        if pk.len() == 32 {
            let mut out = [0u8; 32];
            out.copy_from_slice(&pk);
            Some(out)
        } else {
            None
        }
    })
    .or_else(|e| match e {
        rusqlite::Error::QueryReturnedNoRows => Ok(None),
        e => Err(sql_fail("member lookup", e)),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-sync-{}-{}-{}",
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

    fn open(tag: &str) -> SyncEngine {
        SyncEngine::open(&tmpdir(tag)).unwrap()
    }

    #[test]
    fn identity_persists_across_reopen() {
        let _g = guard();
        let dir = tmpdir("ident");
        let e1 = SyncEngine::open(&dir).unwrap();
        let id1 = e1.device_id();
        let key_file = PathBuf::from(&dir).join(SYNC_DIR).join(DEVICE_KEY_FILE);
        assert!(key_file.exists());
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            assert_eq!(std::fs::metadata(&key_file).unwrap().permissions().mode() & 0o777, 0o600);
        }
        drop(e1);
        let e2 = SyncEngine::open(&dir).unwrap();
        assert_eq!(id1, e2.device_id(), "identity must survive reopen");
        assert_eq!(id1.len(), 64, "device id is hex(ed25519 pubkey)");
    }

    #[test]
    fn corrupt_identity_is_refused_not_rekeyed() {
        let _g = guard();
        let dir = tmpdir("corrupt");
        let e = SyncEngine::open(&dir).unwrap();
        drop(e);
        let key_file = PathBuf::from(&dir).join(SYNC_DIR).join(DEVICE_KEY_FILE);
        std::fs::write(&key_file, b"not a key").unwrap();
        match SyncEngine::open(&dir) {
            Err(e) => assert_eq!(e.status, RcStatus::Corrupt),
            Ok(_) => panic!("corrupt identity must refuse to open"),
        }
    }

    #[test]
    fn member_crud_and_self_rejected() {
        let _g = guard();
        let e = open("members");
        assert_eq!(e.members_list().unwrap().len(), 0);

        let other = SyncEngine::open(&tmpdir("other")).unwrap();
        let other_id = other.device_id();
        let pk = other.public_key();
        let id = e.member_add(&pk, "abc123.onion", "").unwrap();
        assert_eq!(id, other_id);

        let list = e.members_list().unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].device_id, other_id);
        assert_eq!(list[0].onion_addr, "abc123.onion");
        assert_eq!(list[0].pubkey, pk);
        assert!(list[0].added_ms > 0);

        // Upsert refreshes the onion, not the added stamp.
        e.member_add(&pk, "def456.onion", "").unwrap();
        let list = e.members_list().unwrap();
        assert_eq!(list.len(), 1);
        assert_eq!(list[0].onion_addr, "def456.onion");

        e.member_seen(&other_id, Some(4242)).unwrap();
        assert_eq!(e.members_list().unwrap()[0].last_seen_ms, 4242);
        assert_eq!(
            e.member_seen("deadbeef", None).unwrap_err().status,
            RcStatus::NotFound
        );

        let own = e.public_key();
        assert_eq!(
            e.member_add(&own, "", "").unwrap_err().status,
            RcStatus::InvalidArgument
        );

        assert!(e.member_remove(&other_id).unwrap());
        assert!(!e.member_remove(&other_id).unwrap());
        assert!(e.members_list().unwrap().is_empty());
    }

    #[test]
    fn append_signs_and_dedups() {
        let _g = guard();
        let e = open("append");
        let op = e
            .append("settings", "", &serde_json::json!({"set": {"k": "v"}}))
            .unwrap();
        assert_eq!(op.lamport, 1);
        assert_eq!(op.op_id.len(), 64);
        let op2 = e
            .append("history", "default", &serde_json::json!({"visit": "u"}))
            .unwrap();
        assert_eq!(op2.lamport, 2, "lamport ticks per local op");

        let all = e.export(None, 0).unwrap();
        assert_eq!(all.len(), 2);
        assert_eq!(all[0].op_id, op.op_id);

        let only_settings = e.export(Some("settings"), 0).unwrap();
        assert_eq!(only_settings.len(), 1);
        assert!(e.export(Some("settings"), 1).unwrap().is_empty());
        assert!(e.export(Some("bogus"), 0).is_err());
        assert!(e.append("bogus", "", &serde_json::json!(1)).is_err());
    }

    /// The loopback self-sync leg: ops exported from a device import
    /// back onto it — signatures verify against the own key, content
    /// ids dedup, and a registered applier sees the bodies.
    #[test]
    fn loopback_self_sync_round_trips_ops() {
        let _g = guard();
        let dir = tmpdir("loop");
        let e = SyncEngine::open(&dir).unwrap();

        let seen: Arc<Mutex<Vec<Value>>> = Arc::new(Mutex::new(Vec::new()));
        let seen2 = seen.clone();
        register_applier(
            "settings",
            Arc::new(move |_scope, body| {
                seen2.lock().unwrap().push(body.clone());
                Ok(())
            }),
        );

        e.append("settings", "", &serde_json::json!({"set": {"a": 1}}))
            .unwrap();
        e.append("settings", "work", &serde_json::json!({"set": {"b": 2}}))
            .unwrap();

        let wire: Vec<Value> = e.export(None, 0).unwrap().iter().map(|o| o.to_json()).collect();
        let rep = e.import(&wire).unwrap();
        // Already in our log — every op dedups, none re-apply.
        assert_eq!(rep.accepted, 0);
        assert_eq!(rep.duplicates, 2);
        assert_eq!(rep.applied, 0);
        assert!(rep.rejected.is_empty());

        // A peer device on its own data root consumes the same wire.
        let peer = SyncEngine::open(&tmpdir("peer")).unwrap();
        peer.member_add(&e.public_key(), "loop.onion", "").unwrap();
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.accepted, 2);
        assert_eq!(rep.applied, 2);
        assert_eq!(rep.duplicates, 0);
        assert!(rep.rejected.is_empty());
        let bodies = seen.lock().unwrap().clone();
        assert_eq!(
            bodies,
            vec![
                serde_json::json!({"set": {"a": 1}}),
                serde_json::json!({"set": {"b": 2}}),
            ]
        );

        // Re-import is idempotent — duplicates, nothing re-applies.
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.duplicates, 2);
        assert_eq!(rep.applied, 0);
        assert_eq!(seen.lock().unwrap().len(), 2);

        // The peer's lamport clock merged past the imported ticks.
        let p = peer.append("settings", "", &serde_json::json!({"set": {"c": 3}})).unwrap();
        assert!(p.lamport > 2, "peer clock must pass imported ticks");

        unregister_applier("settings");
    }

    #[test]
    fn apply_order_is_canonical_not_arrival() {
        let _g = guard();
        let dir_a = tmpdir("ordA");
        let a = SyncEngine::open(&dir_a).unwrap();
        // Three ops, lamports 1..3.
        for i in 0..3 {
            a.append("settings", "", &serde_json::json!({"n": i})).unwrap();
        }
        let mut wire: Vec<Value> =
            a.export(None, 0).unwrap().iter().map(|o| o.to_json()).collect();
        wire.reverse(); // deliver newest-first

        let order: Arc<Mutex<Vec<i64>>> = Arc::new(Mutex::new(Vec::new()));
        let order2 = order.clone();
        register_applier(
            "settings",
            Arc::new(move |_s, b| {
                order2.lock().unwrap().push(b["n"].as_i64().unwrap());
                Ok(())
            }),
        );

        let peer = SyncEngine::open(&tmpdir("ordP")).unwrap();
        peer.member_add(&a.public_key(), "", "").unwrap();
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.accepted, 3);
        assert_eq!(
            order.lock().unwrap().as_slice(),
            &[0, 1, 2],
            "apply must follow (lamport, device_id, op_id), not arrival"
        );
        unregister_applier("settings");
    }

    #[test]
    fn tampered_and_unknown_signers_rejected() {
        let _g = guard();
        let a = SyncEngine::open(&tmpdir("tA")).unwrap();
        a.append("settings", "", &serde_json::json!({"k": 1})).unwrap();
        let wire: Vec<Value> =
            a.export(None, 0).unwrap().iter().map(|o| o.to_json()).collect();

        // Unknown signer — not a member, not self.
        let peer = SyncEngine::open(&tmpdir("tP")).unwrap();
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.accepted, 0);
        assert_eq!(rep.rejected.len(), 1);
        assert!(rep.rejected[0].reason.contains("member"));

        // Member but tampered body — content id no longer matches.
        peer.member_add(&a.public_key(), "", "").unwrap();
        let mut forged = wire[0].clone();
        forged["body"] = serde_json::json!({"k": "evil"});
        let rep = peer.import(&[forged]).unwrap();
        assert_eq!(rep.accepted, 0);
        assert!(rep.rejected[0].reason.contains("op_id"));

        // Forged op_id but stale signature — sig check catches it.
        let mut forged = wire[0].clone();
        forged["body"] = serde_json::json!({"k": "evil"});
        let reframe = WireOp::from_json(&forged).unwrap();
        forged["op_id"] = serde_json::json!(reframe.content_id());
        let rep = peer.import(&[forged]).unwrap();
        assert_eq!(rep.accepted, 0);
        assert!(rep.rejected[0].reason.contains("signature"));

        // Malformed entries land in rejected, not a hard error.
        let rep = peer.import(&[serde_json::json!({"v": 9})]).unwrap();
        assert_eq!(rep.rejected.len(), 1);

        // Clean import still works after the garbage.
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.accepted, 1);
        assert_eq!(rep.deferred, 1, "no applier registered -> deferred");
    }

    #[test]
    fn deferred_ops_drain_when_applier_registers() {
        let _g = guard();
        let a = SyncEngine::open(&tmpdir("dA")).unwrap();
        a.append("history", "", &serde_json::json!({"visit": "u"})).unwrap();
        let wire: Vec<Value> =
            a.export(None, 0).unwrap().iter().map(|o| o.to_json()).collect();

        let peer = SyncEngine::open(&tmpdir("dP")).unwrap();
        peer.member_add(&a.public_key(), "", "").unwrap();
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.deferred, 1);
        assert_eq!(peer.pending_apply("history").unwrap().len(), 1);

        let hits: Arc<Mutex<u32>> = Arc::new(Mutex::new(0));
        let hits2 = hits.clone();
        register_applier(
            "history",
            Arc::new(move |_s, _b| {
                *hits2.lock().unwrap() += 1;
                Ok(())
            }),
        );
        let rep = peer.drain_pending("history").unwrap();
        assert_eq!(rep.applied, 1);
        assert_eq!(*hits.lock().unwrap(), 1);
        assert!(peer.pending_apply("history").unwrap().is_empty());
        unregister_applier("history");
    }

    #[test]
    fn apply_error_keeps_op_pending() {
        let _g = guard();
        let a = SyncEngine::open(&tmpdir("eA")).unwrap();
        a.append("tabs", "", &serde_json::json!({"open": "u"})).unwrap();
        let wire: Vec<Value> =
            a.export(None, 0).unwrap().iter().map(|o| o.to_json()).collect();

        register_applier(
            "tabs",
            Arc::new(|_s, _b| fail(RcStatus::Io, "adapter exploded")),
        );
        let peer = SyncEngine::open(&tmpdir("eP")).unwrap();
        peer.member_add(&a.public_key(), "", "").unwrap();
        let rep = peer.import(&wire).unwrap();
        assert_eq!(rep.accepted, 1);
        assert_eq!(rep.applied, 0);
        assert_eq!(rep.apply_errors.len(), 1);
        assert_eq!(peer.pending_apply("tabs").unwrap().len(), 1);
        unregister_applier("tabs");
    }

    #[test]
    fn stats_reflects_state() {
        let _g = guard();
        let e = open("stats");
        let s = e.stats().unwrap();
        assert_eq!(s["device_id"].as_str().unwrap(), e.device_id());
        assert_eq!(s["members"], 0);
        assert_eq!(s["ops"], 0);
        assert_eq!(s["lamport"], 0);
        e.append("settings", "", &serde_json::json!({"x": 1})).unwrap();
        let s = e.stats().unwrap();
        assert_eq!(s["ops"], 1);
        assert_eq!(s["lamport"], 1);
        assert_eq!(s["stores"]["settings"]["ops"], 1);
        assert_eq!(s["stores"]["settings"]["last_seq"], 1);
    }
}
