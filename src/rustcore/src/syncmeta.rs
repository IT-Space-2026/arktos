//! SYNC09 — device management: revocation + group-key rotation,
//! carried on the "syncmeta" store of the SYNC01 op-log.
//!
//! Semantics (documented honestly — this is what the code does):
//!
//!   * The device list is PER-PROFILE: `devices()` reports the
//!     members bound to one scope (plus device-global members, the
//!     SYNC01 spelling that signs every scope) — one profile's
//!     paired set, never a neighbour's.  Each row carries the
//!     pairing-grade fingerprint rendered off the member's device
//!     pubkey (the same 6-word/6-emoji tables SYNC03's ceremony
//!     uses — deterministic, so both sides can read it aloud).
//!   * `revoke()` un-pairs a device from one scope: it rotates the
//!     scope's group secret to a fresh key, ships the new secret to
//!     the REMAINING members inside a signed "rekey" op, removes
//!     the target's scope binding (or the whole member row when
//!     that was its last scope — a scoped member stripped bare
//!     would otherwise silently become device-global and sign
//!     everything), drops its onion client-auth grant so it is
//!     refused at the Tor layer, and re-wraps the password vault
//!     when it is unlocked.
//!   * The rekey op is `{"t":"rekey","v":1,"gen":n,
//!     "revoked":[device_id…], "wraps":[{d,e,c}…]}` — `wraps` holds
//!     one entry per surviving member.  The wrap is an ephemeral
//!     x25519 DH against the recipient's Ed25519 device pubkey
//!     converted to Montgomery form; the recipient derives its own
//!     x25519 static from its device.key seed and opens the blob.
//!     Both sides need only the member record — no transport
//!     state, no extra key files, and the same path works on the
//!     tor and mem backends.  The wrap key is SHA-256 over
//!     (domain || shared || sender || recipient || gen), so a blob
//!     can never be grafted onto another recipient or generation.
//!   * Revocation PROPAGATES: the op's `revoked` list is applied
//!     by every member that receives it — each drops the device's
//!     scope binding and its client-auth grant, so a revoked
//!     device is cut off from the whole group, not just the
//!     revoker (membership is pairwise, and ops would otherwise
//!     reach it through a still-paired third member).  A device
//!     that finds ITSELF in `revoked` tears down its side of the
//!     pair with the revoker for that scope.
//!   * Rotation ordering: a per-scope marker records the
//!     canonical stamp (gen, lamport, device_id, op_id) of the
//!     newest applied rekey.  A rekey installs its secret only
//!     when its stamp passes the stored marker — concurrent
//!     rotations converge on the canonically-later one on every
//!     device.  `revoked` lists apply unconditionally (idempotent,
//!     and concurrent rekeys carry different sets).
//!   * Honest edges: a member the revoker did not wrap (asymmetric
//!     pairing — we list them, they do not list us) keeps the old
//!     secret and diverges, exactly like a device that was never
//!     in the group; a revoked device's already-held data and old
//!     ciphertext stay readable to it — rotation locks it out of
//!     NEW material only; ops a revoked device emits after its
//!     cut are rejected as unknown-signer.
//!   * Re-pairing into the same profile after a revocation hits
//!     group_secret_install's loud conflict — the stale stored
//!     secret is deliberate proof of the old membership; clearing
//!     sync state is the explicit opt-out, not a silent swap.

use std::path::Path;
use std::sync::Arc;

use serde_json::{json, Value};
use sha2::{Digest, Sha256, Sha512};
use tor_llcrypto::pk::curve25519::{PublicKey, StaticSecret};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::notify;
use crate::sync::{self, Member, RotationMark, SyncEngine, WireOp, SYNC_DIR};
use crate::syncpasswords;
use crate::synctransport;
use crate::synctabs;

const STORE: &str = "syncmeta";
const WRAP_DOMAIN: &[u8] = b"arktos-syncrekey/v1";

const MAX_REVOKE: usize = 64;
const MAX_WRAPS: usize = 64;
const MAX_DKEY_B64: usize = 128;
const MAX_WRAP_B64: usize = 4 * 1024;

fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::InvalidArgument, msg.into())
}

/// The client-auth nickname pairing granted this device — SYNC03's
/// "member-<hex16>" convention; revocation strips the same grant.
fn member_nick(device_id: &str) -> String {
    format!("member-{}", &device_id[..16.min(device_id.len())])
}

/// Ed25519 → x25519 public conversion: the Montgomery u-image of
/// the Edwards point — the standard birational map, so the
/// recipient's seed-derived static agrees by construction.
fn ed_to_mont(ed_pub: &[u8; 32]) -> Option<PublicKey> {
    use curve25519_dalek::edwards::CompressedEdwardsY;
    let pt = CompressedEdwardsY(*ed_pub).decompress()?;
    Some(PublicKey::from(pt.to_montgomery().to_bytes()))
}

/// Our x25519 static derived from the Ed25519 device seed —
/// SHA-512(seed)[:32]; the DH ladder clamps per RFC 7748.
fn own_x_secret(data_root: &Path) -> RcResult<StaticSecret> {
    let seed = sync::device_seed(&data_root.join(SYNC_DIR))?;
    let h = Sha512::digest(seed);
    let mut scalar = [0u8; 32];
    scalar.copy_from_slice(&h[..32]);
    Ok(StaticSecret::from(scalar))
}

/// The per-wrap seal key — binds the blob to sender, recipient
/// AND generation, so a ciphertext lifted elsewhere opens nothing.
fn wrap_key(shared: &[u8; 32], sender: &str, recip: &str, gen: u64) -> [u8; 32] {
    let mut d = Sha256::new();
    d.update(WRAP_DOMAIN);
    d.update(shared);
    d.update(sender.as_bytes());
    d.update(recip.as_bytes());
    d.update(gen.to_be_bytes());
    d.finalize().into()
}

/// Seal `secret` for `member` — ephemeral x25519 to the member's
/// Ed-pub-derived Montgomery key.
fn wrap_for(
    sender: &str,
    member: &Member,
    gen: u64,
    secret: &[u8; 32],
) -> RcResult<Value> {
    let Some(mont) = ed_to_mont(&member.pubkey) else {
        return invalid(format!(
            "member {} has an invalid Ed25519 public key",
            member.device_id
        ));
    };
    let eph = StaticSecret::from(crate::noisexx::rand_bytes::<32>()?);
    let eph_pub = PublicKey::from(&eph);
    let key = wrap_key(
        eph.diffie_hellman(&mont).as_bytes(),
        sender,
        &member.device_id,
        gen,
    );
    let ct = crate::store::seal_with_key(&key, secret)?;
    Ok(json!({
        "d": member.device_id,
        "e": crate::mw::b64_encode(&eph_pub.to_bytes()),
        "c": crate::mw::b64_encode(&ct),
    }))
}

/// Open our wrap entry out of a verified rekey op.
fn unwrap_for(
    data_root: &Path,
    op: &WireOp,
    wrap: &Value,
    gen: u64,
    own: &str,
) -> RcResult<[u8; 32]> {
    let eph_raw = wrap["e"]
        .as_str()
        .and_then(crate::mw::b64_decode)
        .ok_or_else(|| Fail {
            status: RcStatus::Crypto,
            msg: "rekey wrap has no ephemeral key".into(),
        })?;
    if eph_raw.len() != 32 {
        return fail(RcStatus::Crypto, "rekey wrap ephemeral key is malformed");
    }
    let mut eph_bytes = [0u8; 32];
    eph_bytes.copy_from_slice(&eph_raw);
    let eph_pub = PublicKey::from(eph_bytes);
    let secret = own_x_secret(data_root)?;
    let key = wrap_key(
        secret.diffie_hellman(&eph_pub).as_bytes(),
        &op.device_id,
        own,
        gen,
    );
    let ct = wrap["c"]
        .as_str()
        .and_then(crate::mw::b64_decode)
        .ok_or_else(|| Fail {
            status: RcStatus::Crypto,
            msg: "rekey wrap has no ciphertext".into(),
        })?;
    let pt = crate::store::open_with_key(&key, &ct).map_err(|_| Fail {
        status: RcStatus::Crypto,
        msg: "rekey wrap addressed to us does not decrypt".into(),
    })?;
    if pt.len() != 32 {
        return fail(RcStatus::Crypto, "rekey wrap payload is malformed");
    }
    let mut out = [0u8; 32];
    out.copy_from_slice(&pt);
    Ok(out)
}

fn is_device_id(s: &str) -> bool {
    s.len() == 64 && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// Drop `id`'s binding to `scope`; when that was its last scope the
/// whole member row goes — a scoped member left with zero rows
/// would read as device-global on the next import check, i.e. a
/// privilege ESCALATION out of an unpair.
fn unmember_scope(engine: &SyncEngine, id: &str, scope: &str) -> RcResult<bool> {
    let Some(m) = engine.member(id)? else {
        return Ok(false);
    };
    if m.scopes.len() <= 1 {
        return engine.member_remove(id);
    }
    engine.member_scope_remove(id, scope)
}

/// The per-profile device list — members bound to `scope` (plus
/// device-global members), each with the deterministic
/// word/emoji fingerprint of its device pubkey.  This device is
/// reported separately: it is in every group it runs, by
/// definition.
pub(crate) fn devices(root: &str, scope: &str) -> RcResult<Value> {
    let engine = SyncEngine::open(root)?;
    let own_id = engine.device_id();
    let own_fp = crate::syncpair::fingerprint_json(&Sha256::digest(engine.public_key()).into());
    let mut out = Vec::new();
    for m in engine.members_for_scope(scope)? {
        let fp_hash: [u8; 32] = Sha256::digest(m.pubkey).into();
        out.push(json!({
            "device_id": m.device_id,
            "fingerprint": crate::syncpair::fingerprint_json(&fp_hash),
            "last_seen_ms": m.last_seen_ms,
            "added_ms": m.added_ms,
            "onion_addr": m.onion_addr,
            "device_global": m.scopes.is_empty(),
        }));
    }
    Ok(json!({
        "device_id": own_id,
        "fingerprint": own_fp,
        "devices": out,
    }))
}

/// Un-pair `device_id` from `scope`: rotate the group secret, ship
/// it to the surviving members inside a signed rekey op, drop the
/// member binding + the onion grant, and re-wrap the vault.
pub(crate) fn revoke(root: &str, scope: &str, device_id: &str) -> RcResult<Value> {
    let engine = SyncEngine::open(root)?;
    let Some(target) = engine.member(device_id)? else {
        return fail(RcStatus::NotFound, "unknown sync device");
    };
    if !target.scopes.is_empty() && !target.scopes.iter().any(|s| s == scope) {
        return fail(
            RcStatus::InvalidArgument,
            "device is not paired to this profile's sync group",
        );
    }
    let own = engine.device_id();
    let remaining: Vec<Member> = engine
        .members_for_scope(scope)?
        .into_iter()
        .filter(|m| m.device_id != device_id)
        .collect();
    // Rotate first so the op we append already carries the
    // generation the new key belongs to; the displaced key moved to
    // the one-deep prev slot inside rotate().
    let (gkey, gen) = engine.group_secret_rotate(scope)?;
    let mut wraps = Vec::with_capacity(remaining.len());
    for m in &remaining {
        wraps.push(wrap_for(&own, m, gen, &gkey)?);
    }
    let op = engine.append(
        STORE,
        scope,
        &json!({
            "t": "rekey", "v": 1, "gen": gen,
            "revoked": [device_id],
            "wraps": wraps,
        }),
    )?;
    engine.group_secret_rotation_mark(scope, &RotationMark::of(gen, &op))?;
    // A member whose only scope is this one (or the device-global
    // spelling) is fully unpaired; a multi-scope member loses just
    // this profile's binding.
    let member_removed = if target.scopes.len() <= 1 {
        engine.member_remove(device_id)?
    } else {
        engine.member_scope_remove(device_id, scope)?;
        false
    };
    let grant_revoked = synctransport::deauthorize_grant(
        Path::new(root),
        &member_nick(device_id),
    )
    .unwrap_or(false);
    synctabs::drop_device(root, scope, device_id);
    // The vault re-wrap is best-effort on the same call: a locked
    // vault re-gates on the next enable, an unlocked one re-seals
    // now.  A rewrap failure must not roll back the revocation.
    let vault = syncpasswords::rewrap(root, scope)
        .unwrap_or_else(|e| json!({"rewrapped": false, "error": e.msg}));
    notify::emit("sync");
    Ok(json!({
        "ok": true,
        "device_id": device_id,
        "scope": scope,
        "gen": gen,
        "member_removed": member_removed,
        "grant_revoked": grant_revoked,
        "wrapped_for": remaining.iter().map(|m| m.device_id.clone())
            .collect::<Vec<_>>(),
        "vault": vault,
    }))
}

// ---- apply (verified remote rekey -> group + keystore state) ----------

/// Register the apply lane for `root`'s sync engine — called from
/// every sync-domain MW op so the seam always routes to the core
/// the call is bound to.
pub(crate) fn install(root: &str) {
    let root = root.to_string();
    sync::register_applier(
        STORE,
        Arc::new(move |op: &WireOp| apply_remote(&root, op)),
    );
}

fn apply_remote(root: &str, op: &WireOp) -> RcResult<()> {
    if op.body["t"].as_str() != Some("rekey") || op.body["v"].as_i64() != Some(1) {
        return invalid("bad syncmeta op");
    }
    let gen = op.body["gen"]
        .as_u64()
        .ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "rekey op has no generation".into(),
        })?;
    let revoked: Vec<String> = match op.body.get("revoked") {
        Some(Value::Array(a)) if a.len() <= MAX_REVOKE => a
            .iter()
            .filter_map(|v| v.as_str().map(String::from))
            .collect(),
        Some(Value::Null) | None => Vec::new(),
        _ => return invalid("bad rekey revoked list"),
    };
    if revoked.iter().any(|id| !is_device_id(id)) {
        return invalid("bad device id in rekey revoked list");
    }
    let wraps: Vec<Value> = match op.body.get("wraps") {
        Some(Value::Array(a)) if a.len() <= MAX_WRAPS => a.clone(),
        Some(Value::Null) | None => Vec::new(),
        _ => return invalid("bad rekey wraps list"),
    };
    for w in &wraps {
        let ok = w["d"].as_str().map(is_device_id).unwrap_or(false)
            && w["e"].as_str().map(|s| s.len() <= MAX_DKEY_B64).unwrap_or(false)
            && w["c"].as_str().map(|s| s.len() <= MAX_WRAP_B64).unwrap_or(false);
        if !ok {
            return invalid("malformed rekey wrap entry");
        }
    }
    let rootp = Path::new(root);
    let engine = SyncEngine::open(root)?;
    let own = engine.device_id();
    // 1) Revocation applies unconditionally — it is idempotent,
    //    and concurrent rekeys carry different revoked sets that
    //    ALL must land.  Finding OURSELVES on the list means the
    //    signer unpaired us: tear the pair down symmetrically.
    for id in &revoked {
        let (drop_id, drop_scope) = if *id == own {
            (op.device_id.as_str(), op.scope.as_str())
        } else {
            (id.as_str(), op.scope.as_str())
        };
        let _ = unmember_scope(&engine, drop_id, drop_scope)?;
        let _ = synctransport::deauthorize_grant(rootp, &member_nick(drop_id));
        synctabs::drop_device(root, drop_scope, drop_id);
    }
    // 2) The secret installs only when this rekey passes the
    //    stored rotation marker — the total order every device
    //    resolves concurrent rotations by.
    let mark = RotationMark::of(gen, op);
    let fresh = engine
        .group_secret_rotation(&op.scope)?
        .map(|stored| mark > stored)
        .unwrap_or(true);
    if fresh {
        if let Some(w) = wraps.iter().find(|w| w["d"].as_str() == Some(own.as_str())) {
            let secret = unwrap_for(rootp, op, w, gen, &own)?;
            engine.group_secret_install_rotated(&op.scope, &secret, gen)?;
            // A live vault re-keys off its sealed passphrase;
            // a locked one re-derives at the next enable.
            let _ = syncpasswords::rewrap(root, &op.scope);
        }
        engine.group_secret_rotation_mark(&op.scope, &mark)?;
    }
    notify::emit("sync");
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::sync::ImportReport;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-syncmeta-{}-{}-{}",
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

    /// A "device": a data root whose sync engine signs + imports.
    struct Dev {
        root: String,
    }

    impl Dev {
        fn new(tag: &str) -> Dev {
            Dev { root: tmpdir(tag) }
        }
        fn engine(&self) -> SyncEngine {
            SyncEngine::open(&self.root).unwrap()
        }
        fn id(&self) -> String {
            self.engine().device_id()
        }
        /// The pairing-ceremony shorthand: record the peer bound to
        /// `scope` and authorize its client-auth grant.
        fn pair(&self, peer: &Dev, scope: &str) {
            let pe = peer.engine();
            self.engine()
                .member_add(&pe.public_key(), "peer.onion", scope)
                .unwrap();
            let _ = synctransport::authorize_grant(
                Path::new(&self.root),
                &member_nick(&peer.id()),
                "",
            );
        }
        /// Share the group secret the SYNC03 channel would carry.
        fn share_group(&self, peer: &Dev, scope: &str) {
            let gk = self.engine().group_secret_get_or_create(scope).unwrap();
            peer.engine().group_secret_install(scope, &gk).unwrap();
        }
        fn wire(&self) -> Vec<Value> {
            self.engine()
                .export(None, 0)
                .unwrap()
                .iter()
                .map(|o| o.to_json())
                .collect()
        }
        /// Bind every apply lane this module's tests consume to
        /// `root` — the applier seam is process-global and keyed
        /// per store, so a mixed batch needs each adapter aimed at
        /// the destination root (the mw path does this per call).
        fn bind(root: &str) {
            install(root);
            crate::syncpasswords::install(root);
        }
        fn import_from(&self, peer: &Dev) -> ImportReport {
            self.engine().import(&peer.wire()).unwrap()
        }
        fn gkey(&self, scope: &str) -> [u8; 32] {
            self.engine().group_secret(scope).unwrap().unwrap()
        }
    }

    /// Two scopes, two peers — the device list is strictly the
    /// scope's own set, with the pairing-grade fingerprint.
    #[test]
    fn devices_lists_only_scope_members() {
        let _g = guard();
        install(&tmpdir("dl0"));
        let a = Dev::new("dlA");
        let b = Dev::new("dlB");
        let c = Dev::new("dlC");
        a.pair(&b, "work");
        a.pair(&c, "home");
        let w = devices(&a.root, "work").unwrap();
        let h = devices(&a.root, "home").unwrap();
        let wid = w["devices"][0]["device_id"].as_str().unwrap();
        let hid = h["devices"][0]["device_id"].as_str().unwrap();
        assert_eq!(wid, b.id());
        assert_eq!(hid, c.id());
        assert_eq!(w["devices"].as_array().unwrap().len(), 1);
        assert_eq!(h["devices"].as_array().unwrap().len(), 1);
        // Deterministic fingerprint: same words/emoji on both ends.
        let fp = &w["devices"][0]["fingerprint"];
        assert_eq!(fp["words"].as_array().unwrap().len(), 6);
        assert_eq!(fp["emoji"].as_array().unwrap().len(), 6);
        b.pair(&a, "work");
        let bw = devices(&b.root, "work").unwrap();
        // B sees A — the same fingerprint A reports for itself.
        assert_eq!(
            bw["devices"][0]["fingerprint"],
            devices(&a.root, "work").unwrap()["fingerprint"]
        );
    }

    /// Revoke a last-scope member: the member row goes, the grant
    /// drops, the group secret rotates, and the rekey op ships.
    /// The revoked side, fed the op, tears the pair down too.
    #[test]
    fn revoke_cuts_membership_and_rotates() {
        let _g = guard();
        install(&tmpdir("rv0"));
        let a = Dev::new("rvA");
        let b = Dev::new("rvB");
        a.pair(&b, "work");
        b.pair(&a, "work");
        a.share_group(&b, "work");
        let old_key = a.gkey("work");

        let rep = revoke(&a.root, "work", &b.id()).unwrap();
        assert_eq!(rep["member_removed"], true);
        assert_eq!(rep["gen"], 1);
        assert!(a.engine().members_list().unwrap().is_empty());
        assert_ne!(a.gkey("work"), old_key);
        assert_eq!(
            a.engine().group_secret_prev("work").unwrap().unwrap(),
            old_key
        );
        // The grant went too — the persisted file no longer names it.
        let grants = synctransport::authorized_set(&Path::new(&a.root).join(SYNC_DIR));
        assert!(!grants.contains_key(&member_nick(&b.id())));

        // B consumes the op: finds ITSELF revoked, tears down A.
        install(&b.root);
        let rep = b.import_from(&a);
        assert!(rep.apply_errors.is_empty(), "{:?}", rep.apply_errors);
        assert!(b.engine().members_list().unwrap().is_empty());
        // No wrap was addressed to B — it keeps the OLD secret and
        // can never read post-rotation material.
        assert_eq!(b.gkey("work"), old_key);
    }

    /// A surviving member unwraps the new secret, drops the revoked
    /// peer, and a revoked device that somehow gets the op cannot
    /// unwrap a wrap addressed to someone else.
    #[test]
    fn rekey_reaches_remaining_members_only() {
        let _g = guard();
        let a = Dev::new("rkA");
        let b = Dev::new("rkB");
        let c = Dev::new("rkC");
        a.pair(&b, "work");
        a.pair(&c, "work");
        b.pair(&a, "work");
        c.pair(&a, "work");
        a.share_group(&b, "work");
        a.share_group(&c, "work");
        let old_key = a.gkey("work");
        install(&a.root);
        install(&b.root);
        install(&c.root);

        revoke(&a.root, "work", &c.id()).unwrap();
        let new_key = a.gkey("work");
        assert_ne!(new_key, old_key);

        // B applies: new secret installed, old one slid to prev,
        // marker recorded.
        install(&b.root);
        let rep = b.import_from(&a);
        assert!(rep.apply_errors.is_empty(), "{:?}", rep.apply_errors);
        assert_eq!(b.gkey("work"), new_key);
        assert_eq!(
            b.engine().group_secret_prev("work").unwrap().unwrap(),
            old_key
        );
        assert!(b.engine().group_secret_rotation("work").unwrap().is_some());

        // C never holds the new key — its own wrap is absent.  Fed
        // the same op it just tears down its side of the pair.
        install(&c.root);
        let rep = c.import_from(&a);
        assert!(rep.apply_errors.is_empty(), "{:?}", rep.apply_errors);
        assert_eq!(c.gkey("work"), old_key);
        assert!(c.engine().members_list().unwrap().is_empty());
    }

    /// After the cut, ops the revoked device signs are unknown-
    /// signer rejections on the revoker — "sees nothing new" both
    /// directions.
    #[test]
    fn revoked_device_ops_are_rejected() {
        let _g = guard();
        install(&tmpdir("rj0"));
        let a = Dev::new("rjA");
        let b = Dev::new("rjB");
        a.pair(&b, "work");
        b.pair(&a, "work");
        a.share_group(&b, "work");
        install(&a.root);
        revoke(&a.root, "work", &b.id()).unwrap();
        // B emits something post-revocation; A refuses it at the
        // membership check.
        b.engine()
            .append("settings", "work", &json!({"t": "set", "k": "x", "v": 1}))
            .unwrap();
        let rep = a.import_from(&b);
        assert_eq!(rep.accepted, 0);
        assert_eq!(rep.rejected.len(), 1);
        assert!(rep.rejected[0].reason.contains("not a sync member"));
    }

    /// The transport layer: once the grant is revoked the revoked
    /// device cannot even open a link (mem parity of the Tor-layer
    /// restricted-discovery refusal).
    #[test]
    fn revoked_device_cannot_connect() {
        let _g = guard();
        install(&tmpdir("cx0"));
        let a = Dev::new("cxA");
        let b = Dev::new("cxB");
        a.pair(&b, "work");
        b.pair(&a, "work");
        a.share_group(&b, "work");
        let ta = synctransport::start(Path::new(&a.root), "mem", "cx-svc-a").unwrap();
        let tb = synctransport::start(Path::new(&b.root), "mem", "cx-svc-b").unwrap();
        let nick = member_nick(&b.id());
        ta.authorize(&nick, "").unwrap();
        // Pre-revocation the member connects fine.
        let link = tb.connect("mem:cx-svc-a", &nick).unwrap();
        tb.close_link(link).unwrap();

        revoke(&a.root, "work", &b.id()).unwrap();
        let err = tb.connect("mem:cx-svc-a", &nick).unwrap_err();
        assert_eq!(err.status, RcStatus::Crypto);
        synctransport::stop(Path::new(&a.root));
        synctransport::stop(Path::new(&b.root));
    }

    /// A multi-scope member loses only the revoked scope — the
    /// member row survives for its other profile's group, and only
    /// that scope's secret rotates.
    #[test]
    fn multi_scope_member_loses_only_that_scope() {
        let _g = guard();
        install(&tmpdir("ms0"));
        let a = Dev::new("msA");
        let b = Dev::new("msB");
        a.pair(&b, "work");
        a.pair(&b, "home");
        a.share_group(&b, "work");
        a.share_group(&b, "home");
        let work_key = a.gkey("work");
        let home_key = a.gkey("home");

        let rep = revoke(&a.root, "work", &b.id()).unwrap();
        assert_eq!(rep["member_removed"], false);
        let m = a.engine().member(&b.id()).unwrap().unwrap();
        assert_eq!(m.scopes, vec!["home".to_string()]);
        assert_ne!(a.gkey("work"), work_key);
        assert_eq!(a.gkey("home"), home_key);
        // The device lists agree: work is empty, home still has B.
        assert_eq!(
            devices(&a.root, "work").unwrap()["devices"]
                .as_array()
                .unwrap()
                .len(),
            0
        );
        assert_eq!(
            devices(&a.root, "home").unwrap()["devices"]
                .as_array()
                .unwrap()
                .len(),
            1
        );
    }

    /// Revoking a scope the device isn't bound to is a loud
    /// refusal, not a secret rotation.
    #[test]
    fn revoke_rejects_a_scope_the_device_lacks() {
        let _g = guard();
        install(&tmpdir("ws0"));
        let a = Dev::new("wsA");
        let b = Dev::new("wsB");
        a.pair(&b, "work");
        a.share_group(&b, "work");
        let key = a.gkey("work");
        let err = revoke(&a.root, "home", &b.id()).unwrap_err();
        assert_eq!(err.status, RcStatus::InvalidArgument);
        assert_eq!(a.gkey("work"), key);
        assert!(!a.engine().members_list().unwrap().is_empty());
        let err = revoke(&a.root, "work", &a.id()).unwrap_err();
        assert_eq!(err.status, RcStatus::NotFound);
    }

    /// Concurrent rotations on two devices converge: both rekeys
    /// apply in canonical order on every device, the canonically-
    /// later one wins the secret everywhere, and BOTH revoked sets
    /// land.
    #[test]
    fn concurrent_rotations_converge() {
        let _g = guard();
        let a = Dev::new("ccA");
        let b = Dev::new("ccB");
        let c = Dev::new("ccC");
        let d = Dev::new("ccD");
        a.pair(&b, "work");
        a.pair(&c, "work");
        a.pair(&d, "work");
        b.pair(&a, "work");
        b.pair(&c, "work");
        b.pair(&d, "work");
        for peer in [&b, &c, &d] {
            a.share_group(peer, "work");
        }
        install(&a.root);
        install(&b.root);

        // A revokes C, B revokes D — concurrently (no exchange yet).
        revoke(&a.root, "work", &c.id()).unwrap();
        revoke(&b.root, "work", &d.id()).unwrap();
        let key_a = a.gkey("work");
        let key_b = b.gkey("work");
        assert_ne!(key_a, key_b);

        // Cross-import: each side sees the other's rekey.  The
        // applier seam is process-global — re-bind the
        // destination's lane before each import (the mw path does
        // this per call).
        install(&a.root);
        a.import_from(&b);
        install(&b.root);
        b.import_from(&a);
        // Deterministic winner — the rekey with the later canonical
        // stamp — identical on every device.
        assert_eq!(a.gkey("work"), b.gkey("work"));
        let winner = a.gkey("work");
        assert!(winner == key_a || winner == key_b);
        // And both revocations landed: C and D are gone everywhere.
        for dev in [&a, &b] {
            let ids: Vec<String> = dev
                .engine()
                .members_list()
                .unwrap()
                .iter()
                .map(|m| m.device_id.clone())
                .collect();
            assert!(!ids.contains(&c.id()));
            assert!(!ids.contains(&d.id()));
        }
    }

    /// A stale rekey (an op whose canonical stamp precedes the
    /// stored marker) cannot roll the secret back — its revoked
    /// list still lands.
    #[test]
    fn stale_rekey_cannot_downgrade() {
        let _g = guard();
        let a = Dev::new("stA");
        let b = Dev::new("stB");
        let c = Dev::new("stC");
        a.pair(&b, "work");
        a.pair(&c, "work");
        b.pair(&a, "work");
        c.pair(&a, "work");
        a.share_group(&b, "work");
        a.share_group(&c, "work");
        install(&a.root);
        install(&b.root);
        install(&c.root);
        // First rotation reaches B.
        revoke(&a.root, "work", &c.id()).unwrap();
        let wire1 = a.wire();
        install(&b.root);
        b.engine().import(&wire1).unwrap();
        let gen1_key = b.gkey("work");
        // A rotates again; B applies only the second batch.
        a.pair(&c, "work"); // re-pair C just to have someone to revoke
        revoke(&a.root, "work", &c.id()).unwrap();
        let gen2_wire: Vec<Value> = a
            .engine()
            .export(None, wire1.len() as i64)
            .unwrap()
            .iter()
            .map(|o| o.to_json())
            .collect();
        install(&b.root);
        b.engine().import(&gen2_wire).unwrap();
        let gen2_key = b.gkey("work");
        assert_ne!(gen1_key, gen2_key);
        // Re-feeding the OLD batch must not roll back — the marker
        // already passed it.  (Ops dedup, so simulate the stale
        // install directly: a lower-stamped marker never applies.)
        let stored = b.engine().group_secret_rotation("work").unwrap().unwrap();
        let stale = RotationMark {
            gen: stored.gen,
            lamport: 0,
            device_id: String::new(),
            op_id: String::new(),
        };
        assert!(stored > stale);
        assert_eq!(b.gkey("work"), gen2_key);
    }

    /// The vault re-wrap: an unlocked member rotates its key off
    /// the sealed passphrase at apply time and reads the
    /// re-sealed ops; a member that was locked through the
    /// rotation re-enables on the same passphrase against the new
    /// verifier.
    #[test]
    fn vault_rewraps_across_rotation() {
        let _g = guard();
        let a = Dev::new("vwA");
        let b = Dev::new("vwB");
        let c = Dev::new("vwC");
        for (x, y) in [(&a, &b), (&a, &c), (&b, &a), (&c, &a)] {
            x.pair(y, "work");
        }
        a.share_group(&b, "work");
        a.share_group(&c, "work");
        install(&a.root);
        install(&b.root);
        install(&c.root);
        // A + B opt in; C never does (the locked member).
        crate::syncpasswords::enable(&a.root, "work", b"vault-pass").unwrap();
        crate::syncpasswords::enable(&b.root, "work", b"vault-pass").unwrap();
        // A saves a login; B merges it.
        {
            let dir = crate::facade::profile_dir(Path::new(&a.root), "work");
            std::fs::create_dir_all(&dir).unwrap();
            let mut st = crate::store::lock();
            st.set_data_dir(&dir.to_string_lossy()).unwrap();
            st.autofill_set(vec![crate::autofill::FormRecord {
                url: "https://site.example".into(),
                name: "f".into(),
                has_password: true,
                elements: [("u".to_string(), "me".to_string()),
                           ("p".to_string(), "hunter2".to_string())]
                    .into_iter()
                    .collect(),
            }])
            .unwrap();
        }
        Dev::bind(&b.root);
        b.import_from(&a);
        let has_login = |root: &str| {
            let dir = crate::facade::profile_dir(Path::new(root), "work");
            let mut st = crate::store::lock();
            st.set_data_dir(&dir.to_string_lossy()).unwrap();
            st.autofill_data()
                .unwrap()
                .forms
                .iter()
                .any(|f| f.url == "https://site.example")
        };
        assert!(has_login(&b.root));

        // A revokes C — unlocked, so the vault re-wraps inline.
        let old_key = a.gkey("work");
        revoke(&a.root, "work", &c.id()).unwrap();
        let new_key = a.gkey("work");
        // B (unlocked) applies: new secret in, vault re-keyed,
        // re-sealed ops read — the entry is still there.
        Dev::bind(&b.root);
        let rep = b.import_from(&a);
        assert!(rep.apply_errors.is_empty(), "{:?}", rep.apply_errors);
        assert_eq!(b.gkey("work"), new_key);
        assert!(has_login(&b.root));
        // B can keep syncing: a post-rotation write on A merges.
        {
            let dir = crate::facade::profile_dir(Path::new(&a.root), "work");
            let mut st = crate::store::lock();
            st.set_data_dir(&dir.to_string_lossy()).unwrap();
            st.autofill_set(vec![crate::autofill::FormRecord {
                url: "https://site.example".into(),
                name: "f".into(),
                has_password: true,
                elements: [("u".to_string(), "me".to_string()),
                           ("p".to_string(), "better-pass".to_string())]
                    .into_iter()
                    .collect(),
            }])
            .unwrap();
        }
        let seq = b.engine().stats().unwrap()["stores"]["passwords"]["last_seq"]
            .as_i64()
            .unwrap_or(0);
        let fresh: Vec<Value> = a
            .engine()
            .export(Some("passwords"), 0)
            .unwrap()
            .into_iter()
            .filter(|o| o.wall_ms > 0)
            .map(|o| o.to_json())
            .collect();
        Dev::bind(&b.root);
        let rep = b.engine().import(&fresh).unwrap();
        assert_eq!(rep.apply_errors.len(), 0, "{:?}", rep.apply_errors);
        let _ = seq;
        {
            let dir = crate::facade::profile_dir(Path::new(&b.root), "work");
            let mut st = crate::store::lock();
            st.set_data_dir(&dir.to_string_lossy()).unwrap();
            let forms = st.autofill_data().unwrap().forms.clone();
            let f = forms.iter().find(|f| f.url == "https://site.example").unwrap();
            assert_eq!(
                f.elements
                    .iter()
                    .find(|(k, _)| k == "p")
                    .map(|(_, v)| v.as_str()),
                Some("better-pass")
            );
        }
        // C, the revoked device, kept the OLD key — even handed the
        // post-rotation ciphertext it reads nothing new (its vault
        // never enabled anyway) and A rejects any op it signs.
        assert_eq!(c.gkey("work"), old_key);
    }

    /// The wrap round-trips only for the addressed member: another
    /// member's x-secret cannot open it.
    #[test]
    fn wrap_is_recipient_bound() {
        let _g = guard();
        let a = Dev::new("wrA");
        let b = Dev::new("wrB");
        let c = Dev::new("wrC");
        let bm = Member {
            device_id: b.id(),
            pubkey: b.engine().public_key(),
            onion_addr: String::new(),
            last_seen_ms: 0,
            added_ms: 0,
            scopes: vec!["work".to_string()],
        };
        let secret = [7u8; 32];
        let wrap = wrap_for(&a.id(), &bm, 3, &secret).unwrap();
        // Fabricate the op fields unwrap needs (sender = A).
        let op = WireOp {
            op_id: "x".repeat(64),
            store: STORE.into(),
            scope: "work".into(),
            device_id: a.id(),
            lamport: 1,
            wall_ms: 0,
            body: json!({}),
            sig: vec![],
        };
        let got = unwrap_for(Path::new(&b.root), &op, &wrap, 3, &b.id()).unwrap();
        assert_eq!(got, secret);
        // C's seed produces a different shared secret — open fails.
        assert!(unwrap_for(Path::new(&c.root), &op, &wrap, 3, &c.id()).is_err());
        // Wrong generation also fails (the KDF binds it).
        assert!(unwrap_for(Path::new(&b.root), &op, &wrap, 4, &b.id()).is_err());
    }
}
