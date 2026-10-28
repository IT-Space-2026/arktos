//! SYNC08 — the passwords CRDT adapter on the SYNC01 op-log.
//!
//! STRICTLY OPT-IN: nothing for this store emits, decrypts, or even
//! reads the autofill file until `enable()` unlocks the scope's
//! vault with the user's passphrase.  Without that opt-in the
//! adapter is inert — local form writes carry no ops and remote ops
//! sit durable-but-pending in the log, ciphertext only.
//!
//! Synced data: the autofill store's password-bearing form records
//! (`<root>/profiles/<scope>/autofill-store.dat`, has_password
//! records only — plain form fills and addresses never leave the
//! device).  One record per url, the same dedup the Qt side applies.
//!
//! Semantics (documented honestly — this is what the code does):
//!
//!   * The vault key is Argon2id over the user's sync passphrase,
//!     salted by SHA-256("arktos-pwvault-salt/v1" || group_secret) —
//!     the SYNC03 group secret every member already holds from
//!     pairing, so key material itself NEVER rides an op.  Salt is
//!     bound to the group: members derive the same key from the same
//!     passphrase, outsiders cannot precompute.  SYNC09 note:
//!     rotating the group secret re-bases the salt, so the re-wrap
//!     it performs re-seals password fields under the re-derived
//!     key — only the fields the device itself last wrote
//!     (SYNC09): republishing a peer's field at a fresh stamp
//!     would manufacture LWW recency, and the one-deep previous
//!     key keeps pre-rotation ciphertext readable anyway.
//!   * Wire ops carry ciphertext only — keys are never a payload:
//!       {"t":"pwv","v":<b64>}             — group verifier marker
//!       {"t":"set","e":<eid>,"f":[<b64>…],"r":[<b64>…]}
//!       {"t":"del","e":<eid>}
//!     eid = hex-SHA-256("ARPW1\0" || url) — deterministic, so a
//!     record two devices saved pre-sync names the same element.
//!     "f" blobs seal "sf\0|eid|field\0|value"; "r" blobs seal
//!     "rf\0|eid|field" (field REMOVED).  Fields: "url", "name",
//!     "el.<key>" per element.  A peer reading the body sees only
//!     the eid (a url hash — brute-forceable for known sites, like
//!     SYNC05's visit ids) and sealed blobs.
//!   * pwv is the group verifier: the first enabler plants
//!     seal(vault_key, VERIFIER).  Later enables probe-decrypt the
//!     newest pwv in the log — wrong passphrase = WrongPassphrase,
//!     the gate that keeps "wrong-passphrase peer sees only
//!     ciphertext" a refusal instead of a silent dead vault.  A
//!     device that enables before any pwv reaches it cannot be
//!     probed; two passphrases in one group fork the vault (each
//!     side holds ops it cannot read — the honest no-server edge,
//!     not a bug the code hides).
//!   * Field-level LWW: every field resolves independently on the
//!     op's (lamport, device_id) stamp — the op-log's total order
//!     makes a single latest stamp per field the equivalent of a
//!     version vector (the stamp IS the version the field last
//!     moved under).  A strictly newer op moves the fields it
//!     carries; "r" blobs tombstone a field at that stamp; stale
//!     fields drop.  `del` tombstones the whole entry: sets at or
//!     below the tombstone are stale, later ones resurrect.
//!   * Apply merges into the local form WITHOUT clobbering
//!     fields the group never mentioned: a local element key with
//!     no frontier entry survives a merge (it is a local-only
//!     field, not a deleted one) and emits on the next local diff.
//!   * Emission rides Store::autofill_set — the funnel the flat
//!     rc_*/Qt mutation lanes already share — diffed against the
//!     decrypted synced image, best-effort like the other adapters
//!     (a failed append leaves durable local truth the next
//!     advertise re-offers).  Apply writes through
//!     autofill_set_applied — no emit, a merged op can never echo.
//!   * Locked vault = deferred lane: apply errors `Locked`, the op
//!     stays applied=0, and `enable()`'s drain retries it — this
//!     is the op-log deferred seam working as designed, so a peer
//!     that never unlocks still holds (and forwards) the
//!     ciphertext ops.
//!
//! Durability: no adapter db — the op-log replayed+decrypted on
//! unlock rebuilds the frontier, tombstones and the synced image.
//!
//! Lock ordering: the store mutex nests OUTSIDE the adapter map
//! (emit_after is called under store::lock by autofill_set), so
//! the apply lane never holds the adapter lock across a store
//! call — the merge stages under the adapter, releases, then
//! writes the store.

use std::collections::{BTreeMap, BTreeSet};
use std::path::Path;
use std::sync::{Arc, Mutex};

use serde_json::{json, Value};
use sha2::{Digest, Sha256};

use crate::autofill::FormRecord;
use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::facade;
use crate::notify;
use crate::store::{self, Key, KDF_LANES, KDF_MEMORY_KIB, KDF_PASSES};
use crate::sync::{self, SyncEngine, WireOp};

const STORE: &str = "passwords";

/// (lamport, device_id) — the total order the per-field LWW sorts
/// by; under the op-log's canonical order it is each field's
/// version stamp.
type Stamp = (u64, String);

fn stamp_of(op: &WireOp) -> Stamp {
    (op.lamport, op.device_id.clone())
}

fn invalid<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::InvalidArgument, msg.into())
}

fn locked<T>() -> RcResult<T> {
    fail(
        RcStatus::Locked,
        "password sync vault is locked or never enabled",
    )
}

/// The verifier plaintext pwv ops seal — the passphrase probe.
const VERIFIER: &[u8] = b"ARKTOS-PWVAULT-VERIFY/v1";
/// The salt domain — the group secret binds derivation to the
/// paired group (members share it, outsiders cannot precompute).
const SALT_DOMAIN: &[u8] = b"arktos-pwvault-salt/v1";

const MAX_EID: usize = 64;
const MAX_BLOBS: usize = 128;
const MAX_B64: usize = 32 * 1024;

fn hex(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for b in data {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

/// The deterministic entry id — hex-SHA-256 over the record url.
/// The autofill store dedups forms by url, so one login per url is
/// the element every device names identically.
fn entry_id(url: &str) -> String {
    let mut d = Sha256::new();
    d.update(b"ARPW1\0");
    d.update(url.as_bytes());
    hex(&d.finalize())
}

fn is_eid(s: &str) -> bool {
    s.len() == MAX_EID && s.bytes().all(|b| b.is_ascii_hexdigit())
}

/// The vault key: Argon2id(passphrase, salt(group_secret)).  The
/// group secret is the SYNC03 pairing payload — already on every
/// member, never on the wire.
fn vault_key(passphrase: &[u8], gkey: &[u8; 32]) -> RcResult<Key> {
    let salt_full = Sha256::digest([SALT_DOMAIN, gkey].concat());
    store::derive_key(
        passphrase,
        KDF_MEMORY_KIB,
        KDF_PASSES,
        KDF_LANES,
        &salt_full[..store::KDF_SALT_SIZE],
    )
}

/// <root>/profiles/<scope> (the autofill store's data dir) →
/// (root, scope) — the binding every emit path funnels through;
/// anything else is not a synced profile store.
fn binding_scope(dir: &Path) -> Option<(String, String)> {
    let scope = dir.file_name()?.to_str()?.to_string();
    if dir.parent()?.file_name()?.to_str()? != "profiles" {
        return None;
    }
    let root = dir
        .parent()?
        .parent()?
        .to_string_lossy()
        .into_owned();
    Some((root, scope))
}

/// One scope's adapter state — no files of its own; the log
/// replayed+decrypted on unlock rebuilds everything here.
#[derive(Default)]
struct PwSync {
    /// The derived vault key while unlocked — strictly opt-in:
    /// None means emit nothing, apply nothing.
    key: Option<Key>,
    /// The PREVIOUS generation's key — one rotation deep
    /// (SYNC09): pre-rotation ciphertext stays readable across a
    /// rekey, while every new seal rides `key`.
    prev_key: Option<Key>,
    /// The passphrase sealed under `key` — re-wrap's way to
    /// re-derive on rotation without holding plaintext secrets.
    /// A leaked vault key already exposes the whole image, so the
    /// sealed passphrase adds no new surface.
    sealed_pass: Option<Vec<u8>>,
    hydrated: bool,
    /// (eid, field) -> newest op stamp — the LWW frontier.
    frontier: BTreeMap<(String, String), Stamp>,
    /// eid -> newest del stamp — sets at or below are stale.
    tomb: BTreeMap<String, Stamp>,
    /// eid -> field -> decrypted value — the merged synced image
    /// local diffs compare against.
    shadow: BTreeMap<String, BTreeMap<String, String>>,
}

/// The per-(root, scope) adapters.  Minted only by enable() —
/// an absent entry IS the locked/opted-out state.
static REG: Mutex<BTreeMap<(String, String), PwSync>> = Mutex::new(BTreeMap::new());

impl PwSync {
    // ---- sealed-field codecs -------------------------------------------------

    /// seal "sf\0|eid|field\0|value" — the inner frame binds the
    /// blob to entry+field, so a member cannot graft a ciphertext
    /// onto a field it did not name.
    fn seal_field(&self, eid: &str, fname: &str, val: &str) -> RcResult<String> {
        let key = self.key.as_ref().ok_or_else(|| Fail {
            status: RcStatus::Locked,
            msg: "password vault is locked".into(),
        })?;
        let mut pt = Vec::with_capacity(3 + eid.len() + 1 + fname.len() + 1 + val.len());
        pt.extend_from_slice(b"sf\0");
        pt.extend_from_slice(eid.as_bytes());
        pt.push(0);
        pt.extend_from_slice(fname.as_bytes());
        pt.push(0);
        pt.extend_from_slice(val.as_bytes());
        Ok(crate::mw::b64_encode(&store::seal_with_key(key, &pt)?))
    }

    /// seal "rf\0|eid|field" — a field removal.
    fn seal_rm(&self, eid: &str, fname: &str) -> RcResult<String> {
        let key = self.key.as_ref().ok_or_else(|| Fail {
            status: RcStatus::Locked,
            msg: "password vault is locked".into(),
        })?;
        let mut pt = Vec::new();
        pt.extend_from_slice(b"rf\0");
        pt.extend_from_slice(eid.as_bytes());
        pt.push(0);
        pt.extend_from_slice(fname.as_bytes());
        Ok(crate::mw::b64_encode(&store::seal_with_key(key, &pt)?))
    }

    /// Open a sealed blob under the current key, then the
    /// previous-generation key — a pre-rotation ciphertext is
    /// still legit group material for exactly one generation.
    fn open_blob(&self, b: &Value) -> Option<Vec<u8>> {
        let blob = crate::mw::b64_decode(b.as_str()?)?;
        if let Some(pt) = self
            .key
            .as_ref()
            .and_then(|k| store::open_with_key(k, &blob).ok())
        {
            return Some(pt);
        }
        self.prev_key
            .as_ref()
            .and_then(|k| store::open_with_key(k, &blob).ok())
    }

    /// Open an "sf" blob → (field, value), or None when the blob
    /// does not decrypt for this vault or binds a different eid.
    /// The "url" field must hash back to the eid it claims.
    fn open_field(&self, eid: &str, b: &Value) -> Option<(String, String)> {
        let pt = self.open_blob(b)?;
        let rest = pt.strip_prefix(b"sf\0")?;
        let (eid2, rest) = split0(rest)?;
        if eid2 != eid.as_bytes() {
            return None;
        }
        let (fname, val) = split0(rest)?;
        let fname = String::from_utf8(fname.to_vec()).ok()?;
        let val = String::from_utf8(val.to_vec()).ok()?;
        if fname != "url" && fname != "name" && !fname.starts_with("el.") {
            return None;
        }
        if fname == "url" && entry_id(&val) != eid {
            return None;
        }
        Some((fname, val))
    }

    /// Open an "rf" blob → the removed field name, same binding
    /// checks minus the value.
    fn open_rm(&self, eid: &str, b: &Value) -> Option<String> {
        let pt = self.open_blob(b)?;
        let rest = pt.strip_prefix(b"rf\0")?;
        let (eid2, fname) = split0(rest)?;
        if eid2 != eid.as_bytes() {
            return None;
        }
        let fname = String::from_utf8(fname.to_vec()).ok()?;
        if !fname.starts_with("el.") {
            return None;
        }
        Some(fname)
    }

    // ---- frontier / synced image ----------------------------------------------

    /// Rebuild frontier+tombstone+shadow from the durable log —
    /// replays this scope's ops in canonical order, decrypting set
    /// fields under the vault key (hydrate runs only unlocked).
    fn hydrate(&mut self, engine: &SyncEngine, scope: &str) -> RcResult<()> {
        if self.hydrated {
            return Ok(());
        }
        self.hydrated = true;
        let mut ops: Vec<WireOp> = engine
            .export(Some(STORE), 0)?
            .into_iter()
            .filter(|o| o.scope == scope)
            .collect();
        ops.sort_by(|a, b| {
            (a.lamport, &a.device_id, &a.op_id).cmp(&(b.lamport, &b.device_id, &b.op_id))
        });
        for op in &ops {
            self.replay(op);
        }
        Ok(())
    }

    /// One op against the in-memory image — the emit/apply/hydrate
    /// shared step.  Store writes are the apply lane's separate
    /// second phase.
    fn replay(&mut self, op: &WireOp) {
        let s = stamp_of(op);
        match op.body["t"].as_str() {
            Some("set") => {
                let Some(eid) = op.body["e"].as_str() else {
                    return;
                };
                if self.tomb.get(eid).map(|d| s <= *d).unwrap_or(false) {
                    return;
                }
                if let Some(blobs) = op.body["f"].as_array() {
                    for b in blobs {
                        if let Some((fname, val)) = self.open_field(eid, b) {
                            self.stage(eid, &fname, Some(&val), &s);
                        }
                    }
                }
                if let Some(blobs) = op.body["r"].as_array() {
                    for b in blobs {
                        if let Some(fname) = self.open_rm(eid, b) {
                            self.stage(eid, &fname, None, &s);
                        }
                    }
                }
            }
            Some("del") => {
                let Some(eid) = op.body["e"].as_str() else {
                    return;
                };
                if self.tomb.get(eid).map(|d| s <= *d).unwrap_or(false) {
                    return;
                }
                self.tomb.insert(eid.to_string(), s);
                self.shadow.remove(eid);
            }
            _ => {}
        }
    }

    /// The per-field LWW step: a strictly newer stamp moves the
    /// field; anything at or below drops.
    fn stage(&mut self, eid: &str, fname: &str, val: Option<&str>, s: &Stamp) {
        let k = (eid.to_string(), fname.to_string());
        if self
            .frontier
            .get(&k)
            .map(|prev| s <= prev)
            .unwrap_or(false)
        {
            return;
        }
        self.frontier.insert(k, s.clone());
        let ent = self.shadow.entry(eid.to_string()).or_default();
        match val {
            Some(v) => {
                ent.insert(fname.to_string(), v.to_string());
            }
            None => {
                ent.remove(fname);
            }
        }
    }
}

// ---- emit (local store write -> signed ops) --------------------------

/// Store::autofill_set's post-write hook — the funnel every form
/// mutation already shares.  Runs under the caller's store lock;
/// does no store work itself.  Best-effort BY DESIGN — a failed
/// emit leaves durable local truth the next advertise re-offers.
/// Locked/opted-out scopes return at once: strictly opt-in.
pub(crate) fn emit_after(dir: &Path, forms: &[FormRecord]) {
    let Some((root, scope)) = binding_scope(dir) else {
        return;
    };
    let Ok(engine) = SyncEngine::open(&root) else {
        return;
    };
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let Some(ps) = reg.get_mut(&(root, scope.clone())) else {
        return;
    };
    if ps.key.is_none() {
        return;
    }
    if !ps.hydrated && ps.hydrate(&engine, &scope).is_err() {
        return;
    }
    let _ = diff_emit(ps, &engine, &scope, forms);
}

/// Sign one op per entry the local password set moved vs the
/// synced image — set ops carry only the changed fields (each
/// sealed), del ops tombstone group-known entries the local set
/// dropped.  Frontier+shadow update as ops append so a second
/// diff in the same lifetime is a no-op.
fn diff_emit(
    ps: &mut PwSync,
    engine: &SyncEngine,
    scope: &str,
    forms: &[FormRecord],
) -> RcResult<()> {
    // The synced set = password-bearing records, keyed by eid.
    // Qt dedups forms by url; a duplicate url folds to its first
    // record rather than emitting two competing images.
    let mut local: BTreeMap<String, &FormRecord> = BTreeMap::new();
    for f in forms {
        if f.has_password {
            local.entry(entry_id(&f.url)).or_insert(f);
        }
    }
    for (eid, form) in &local {
        let mut f_blobs: Vec<Value> = Vec::new();
        let mut r_blobs: Vec<Value> = Vec::new();
        let mut staged: Vec<(String, Option<String>)> = Vec::new();
        let shadow = ps.shadow.get(eid.as_str());
        let differs = |fname: &str, val: &str| {
            shadow
                .and_then(|s| s.get(fname))
                .map(String::as_str)
                != Some(val)
        };
        // url/name ride as set fields — they are never removals.
        for (fname, val) in [("url", form.url.as_str()), ("name", form.name.as_str())] {
            if differs(fname, val) {
                f_blobs.push(json!(ps.seal_field(eid, fname, val)?));
                staged.push((fname.to_string(), Some(val.to_string())));
            }
        }
        for (k, v) in &form.elements {
            let fname = format!("el.{k}");
            if differs(&fname, v) {
                f_blobs.push(json!(ps.seal_field(eid, &fname, v)?));
                staged.push((fname, Some(v.clone())));
            }
        }
        if let Some(sh) = shadow {
            for k in sh.keys() {
                let Some(el) = k.strip_prefix("el.") else {
                    continue;
                };
                if form.elements.iter().any(|(ek, _)| ek == el) {
                    continue;
                }
                r_blobs.push(json!(ps.seal_rm(eid, k)?));
                staged.push((k.clone(), None));
            }
        }
        if f_blobs.is_empty() && r_blobs.is_empty() {
            continue;
        }
        let op = engine.append(
            STORE,
            scope,
            &json!({"t":"set","e":eid,"f":f_blobs,"r":r_blobs}),
        )?;
        let s = stamp_of(&op);
        for (fname, val) in staged {
            ps.stage(eid, &fname, val.as_deref(), &s);
        }
    }
    // Deletes: entries the group image knows AND can name (a url —
    // a partial record with no url field never materialized, so
    // there is no local record this could be a delete OF) that the
    // local password set dropped.
    let gone: Vec<String> = ps
        .shadow
        .iter()
        .filter(|(eid, f)| f.contains_key("url") && !local.contains_key(*eid))
        .map(|(eid, _)| eid.clone())
        .collect();
    for eid in gone {
        let op = engine.append(STORE, scope, &json!({"t":"del","e":eid}))?;
        let s = stamp_of(&op);
        ps.tomb.insert(eid.clone(), s);
        ps.shadow.remove(&eid);
    }
    Ok(())
}

// ---- apply (verified remote op -> store) ------------------------------

/// Register the apply lane for `root`'s sync engine — called from
/// every sync-domain MW op so the seam always routes to the core
/// the call is bound to.
pub(crate) fn install(root: &str) {
    let root = root.to_string();
    sync::register_applier(
        STORE,
        Arc::new(move |op: &WireOp| apply_remote(&root, op).map(|_| ())),
    );
}

fn apply_remote(root: &str, op: &WireOp) -> RcResult<bool> {
    let scope = facade::effective_profile(&op.scope).to_string();
    let t = op.body["t"].as_str().unwrap_or("");
    if t == "pwv" {
        // The group verifier — a marker for the enable probe, no
        // store mutation.  Shape-checked so malformed blobs still
        // count as honestly refused.
        return match op.body["v"].as_str() {
            Some(v) if !v.is_empty() && v.len() <= MAX_B64 => Ok(false),
            _ => invalid("bad pwv op"),
        };
    }
    if t != "set" && t != "del" {
        return invalid("bad passwords op");
    }
    let eid = op.body["e"].as_str().unwrap_or("");
    if !is_eid(eid) {
        return invalid("bad passwords entry id");
    }
    if t == "set" {
        for key in ["f", "r"] {
            match op.body.get(key) {
                None | Some(Value::Null) => {}
                Some(Value::Array(a))
                    if a.len() <= MAX_BLOBS
                        && a.iter().all(|b| {
                            b.as_str().map(|s| s.len() <= MAX_B64).unwrap_or(false)
                        }) => {}
                _ => return invalid("bad passwords field list"),
            }
        }
    }

    // Phase 1 — the in-memory merge under the adapter lock.  Never
    // held across the store write: emit_after nests the other way
    // (store lock -> adapter lock), so adapter -> store would
    // deadlock.
    let engine = SyncEngine::open(root)?;
    let (image, known) = {
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        let Some(ps) = reg.get_mut(&(root.to_string(), scope.clone())) else {
            return locked();
        };
        if ps.key.is_none() {
            return locked();
        }
        if !ps.hydrated {
            ps.hydrate(&engine, &scope)?;
        }
        ps.replay(op);
        let image = ps.shadow.get(eid).cloned().unwrap_or_default();
        let known: BTreeSet<String> = ps
            .frontier
            .keys()
            .filter(|(e, _)| e == eid)
            .map(|(_, f)| f.clone())
            .collect();
        (image, known)
    };

    // Phase 2 — the store write, adapter lock released.
    let dir = facade::profile_dir(Path::new(root), &scope);
    let changed = match t {
        "set" => match image.get("url") {
            Some(url) => materialize(&dir, url, &image, &known)?,
            // A partial record (no url yet) merges only into the
            // image — it materializes when the url field lands.
            None => false,
        },
        _ => remove_entry(&dir, eid)?,
    };
    if changed {
        let _hint = notify::profile_hint(&scope);
        notify::emit("autofill");
        notify::emit_event("autofill/changed", json!({ "op": t }));
    }
    Ok(changed)
}

/// Merge the synced image over the local record for `url`: every
/// group-known field (frontier entry) resolves to the image —
/// present fields take the image value, absent ones drop — while
/// local fields the group never mentioned survive untouched.
fn materialize(
    dir: &Path,
    url: &str,
    image: &BTreeMap<String, String>,
    known: &BTreeSet<String>,
) -> RcResult<bool> {
    let mut st = store::lock();
    st.set_data_dir(&dir.to_string_lossy())?;
    let forms = st.autofill_data()?.forms.clone();
    let mut merged: Option<FormRecord> = None;
    let mut out: Vec<FormRecord> = Vec::with_capacity(forms.len() + 1);
    for f in &forms {
        if f.url == url && merged.is_none() {
            let mut m = f.clone();
            if known.contains("name") {
                m.name = image.get("name").cloned().unwrap_or_default();
            }
            let mut elems: BTreeMap<String, String> = BTreeMap::new();
            for (k, v) in &m.elements {
                if !known.contains(&format!("el.{k}")) {
                    elems.insert(k.clone(), v.clone());
                }
            }
            for (k, v) in image {
                if let Some(el) = k.strip_prefix("el.") {
                    elems.insert(el.to_string(), v.clone());
                }
            }
            m.elements = elems.into_iter().collect();
            m.has_password = true;
            merged = Some(m);
        } else {
            out.push(f.clone());
        }
    }
    let m = match merged {
        Some(m) => m,
        None => FormRecord {
            url: url.to_string(),
            name: image.get("name").cloned().unwrap_or_default(),
            has_password: true,
            elements: image
                .iter()
                .filter_map(|(k, v)| {
                    k.strip_prefix("el.").map(|el| (el.to_string(), v.clone()))
                })
                .collect(),
        },
    };
    // Elements serialize sorted by key — deterministic on every
    // device from the same merged image.
    let pos = out.iter().position(|f| f.url == url);
    match pos {
        Some(i) => out[i] = m,
        None => out.push(m),
    }
    st.autofill_set_applied(out)
}

/// A tombstoned entry drops every local password record whose url
/// hashes to eid — the url itself never rides the op, so the scan
/// covers records the shadow image could not name.
fn remove_entry(dir: &Path, eid: &str) -> RcResult<bool> {
    let mut st = store::lock();
    st.set_data_dir(&dir.to_string_lossy())?;
    let forms = st.autofill_data()?.forms.clone();
    let kept: Vec<FormRecord> = forms
        .iter()
        .filter(|f| !(f.has_password && entry_id(&f.url) == eid))
        .cloned()
        .collect();
    if kept.len() == forms.len() {
        return Ok(false);
    }
    st.autofill_set_applied(kept)
}

// ---- the opt-in gate ----------------------------------------------------

/// The newest group verifier op in the log — the passphrase probe.
fn newest_pwv(engine: &SyncEngine, scope: &str) -> RcResult<Option<Vec<u8>>> {
    let mut ops: Vec<WireOp> = engine
        .export(Some(STORE), 0)?
        .into_iter()
        .filter(|o| o.scope == scope && o.body["t"].as_str() == Some("pwv"))
        .collect();
    ops.sort_by(|a, b| {
        (a.lamport, &a.device_id, &a.op_id).cmp(&(b.lamport, &b.device_id, &b.op_id))
    });
    let Some(op) = ops.last() else {
        return Ok(None);
    };
    Ok(op.body["v"]
        .as_str()
        .and_then(crate::mw::b64_decode))
}

/// Opt in + unlock: derive the vault key, gate it on the group's
/// verifier, then merge pending ciphertext and advertise the local
/// set.  Strictly the user's act — nothing reaches here on its own.
pub(crate) fn enable(root: &str, scope: &str, passphrase: &[u8]) -> RcResult<Value> {
    if passphrase.is_empty() {
        return fail(RcStatus::EmptyPassphrase, "empty passphrase");
    }
    let engine = SyncEngine::open(root)?;
    // The vault salt is bound to the group secret — no group, no
    // vault.  Pairing mints/installs it; absent here means this
    // profile was never paired into a sync group.
    let Some(gkey) = engine.group_secret(scope)? else {
        return fail(
            RcStatus::NotEnabled,
            "password sync needs the profile's sync group — pair a device for this profile first",
        );
    };
    let key = vault_key(passphrase, &gkey)?;
    // SYNC09: the previous generation's key — a member that was
    // locked through a rotation still probes the last pre-rotation
    // verifier with it (the passphrase outlives the group secret),
    // then reads the pre-rotation ciphertext off the same key.
    let prev = engine.group_secret_prev(scope)?;
    let prev_key = match &prev {
        Some(pk) => Some(vault_key(passphrase, pk)?),
        None => None,
    };
    let pwv = newest_pwv(&engine, scope)?;
    if let Some(blob) = &pwv {
        let probe = |k: &Key| match store::open_with_key(k, blob) {
            Ok(pt) => pt == VERIFIER,
            Err(_) => false,
        };
        if !probe(&key) && !prev_key.as_ref().map(probe).unwrap_or(false) {
            return fail(
                RcStatus::WrongPassphrase,
                "passphrase does not match the sync vault",
            );
        }
    }
    {
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        let ps = reg
            .entry((root.to_string(), scope.to_string()))
            .or_default();
        // A re-enable re-keys cleanly: fresh state, fresh hydrate.
        let mut fresh = PwSync::default();
        fresh.sealed_pass = Some(store::seal_with_key(&key, passphrase)?);
        fresh.key = Some(key);
        fresh.prev_key = prev_key;
        *ps = fresh;
        ps.hydrate(&engine, scope)?;
        if pwv.is_none() {
            // First enabler in the group plants the verifier every
            // later enable probes — ciphertext, never key material.
            let blob = store::seal_with_key(ps.key.as_ref().unwrap(), VERIFIER)?;
            engine.append(
                STORE,
                scope,
                &json!({"t":"pwv","v":crate::mw::b64_encode(&blob)}),
            )?;
        }
    }
    // Pending ciphertext merges now that the vault is open — BEFORE
    // the advertise diff so the local image it runs against already
    // holds what the group merged (a not-yet-applied remote entry
    // must not look like a local delete).
    let rep = engine.drain_pending(STORE)?;
    let dir = facade::profile_dir(Path::new(root), scope);
    let forms = {
        let mut st = store::lock();
        st.set_data_dir(&dir.to_string_lossy())?;
        st.autofill_data()?.forms.clone()
    };
    {
        let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(ps) = reg.get_mut(&(root.to_string(), scope.to_string())) {
            let _ = diff_emit(ps, &engine, scope, &forms);
        }
    }
    Ok(json!({
        "ok": true,
        "drained": rep.applied,
        "apply_errors": rep.apply_errors.len(),
    }))
}

/// Re-lock: drop the vault key AND the decrypted image — locked is
/// the full opt-out state again (emission stops, apply defers).
pub(crate) fn lock(root: &str, scope: &str) -> RcResult<Value> {
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let was = reg.remove(&(root.to_string(), scope.to_string()));
    Ok(json!({ "ok": true, "was_unlocked": was.map(|p| p.key.is_some()).unwrap_or(false) }))
}

/// SYNC09: re-key the vault after a group-secret rotation — the
/// "vault re-wrap" revocation performs for the remaining devices.
/// Locked vaults skip quietly: the next enable re-derives off the
/// passphrase (and still probes a pre-rotation verifier through
/// the one-deep prev key).  An unlocked vault recovers its
/// passphrase from the key-sealed copy, derives the new key, then
/// plants a fresh verifier and re-seals the whole merged image
/// under it — so the post-rotation log stands alone on the new
/// key and the revoked device's old ciphertext is the last thing
/// it can ever read.
pub(crate) fn rewrap(root: &str, scope: &str) -> RcResult<Value> {
    let engine = SyncEngine::open(root)?;
    let Some(gkey) = engine.group_secret(scope)? else {
        return fail(RcStatus::NotEnabled, "no sync group for this profile");
    };
    let mut reg = REG.lock().unwrap_or_else(|e| e.into_inner());
    let Some(ps) = reg.get_mut(&(root.to_string(), scope.to_string())) else {
        return Ok(json!({ "ok": true, "rewrapped": false, "reason": "locked" }));
    };
    let Some(old_key) = ps.key.clone() else {
        return Ok(json!({ "ok": true, "rewrapped": false, "reason": "locked" }));
    };
    let pass = ps
        .sealed_pass
        .as_ref()
        .and_then(|sp| store::open_with_key(&old_key, sp).ok())
        .ok_or_else(|| Fail {
            status: RcStatus::Locked,
            msg: "vault has no sealed passphrase to re-wrap with".into(),
        })?;
    let new_key = vault_key(&pass, &gkey)?;
    ps.sealed_pass = Some(store::seal_with_key(&new_key, &pass)?);
    ps.prev_key = Some(old_key);
    ps.key = Some(new_key);
    if !ps.hydrated {
        ps.hydrate(&engine, scope)?;
    }
    // Fresh verifier under the new key — members that were offline
    // through the rotation probe this one on their next enable.
    let blob = store::seal_with_key(ps.key.as_ref().unwrap(), VERIFIER)?;
    engine.append(
        STORE,
        scope,
        &json!({"t":"pwv","v":crate::mw::b64_encode(&blob)}),
    )?;
    // Re-seal the fields THIS device last wrote: one set op per
    // entry carrying its own-stamped fields under the new key.
    // Only own fields — re-emitting a peer's field at our fresh
    // stamp would manufacture recency and bury a genuinely newer
    // write of theirs under LWW (each device republishes its own
    // authorship, so the image is covered by the survivors).  A
    // field whose author was just revoked simply ages out under
    // the one-generation prev-key horizon.
    let own = engine.device_id();
    let eids: Vec<String> = ps.shadow.keys().cloned().collect();
    let mut resealed = 0usize;
    for eid in eids {
        let Some(fields) = ps.shadow.get(&eid).cloned() else {
            continue;
        };
        let mut f_blobs = Vec::new();
        for (fname, val) in &fields {
            let ours = ps
                .frontier
                .get(&(eid.clone(), fname.clone()))
                .map(|s| s.1 == own)
                .unwrap_or(false);
            if ours {
                f_blobs.push((fname.clone(), json!(ps.seal_field(&eid, fname, val)?)));
            }
        }
        if f_blobs.is_empty() {
            continue;
        }
        let op = engine.append(
            STORE,
            scope,
            &json!({
                "t":"set","e":eid,
                "f":f_blobs.iter().map(|(_, b)| b.clone()).collect::<Vec<_>>(),
                "r":[]
            }),
        )?;
        let s = stamp_of(&op);
        for (fname, _) in &f_blobs {
            let val = fields.get(fname).cloned();
            ps.stage(&eid, fname, val.as_deref(), &s);
        }
        resealed += 1;
    }
    Ok(json!({ "ok": true, "rewrapped": true, "entries": resealed }))
}

/// The status read model: is the vault open, does the group carry
/// a verifier, how many ops/pending ciphertexts the scope's log holds.
pub(crate) fn status(root: &str, scope: &str) -> RcResult<Value> {
    let engine = SyncEngine::open(root)?;
    let unlocked = {
        let reg = REG.lock().unwrap_or_else(|e| e.into_inner());
        reg.get(&(root.to_string(), scope.to_string()))
            .map(|p| p.key.is_some())
            .unwrap_or(false)
    };
    let mut ops = 0usize;
    let mut verifier = false;
    for op in engine.export(Some(STORE), 0)? {
        if op.scope != scope {
            continue;
        }
        ops += 1;
        if op.body["t"].as_str() == Some("pwv") {
            verifier = true;
        }
    }
    let pending = engine
        .pending_apply(STORE)?
        .iter()
        .filter(|o| o.scope == scope)
        .count();
    Ok(json!({
        "unlocked": unlocked,
        "enabled_ever": verifier || unlocked,
        "ops": ops,
        "pending_apply": pending,
    }))
}

/// Split at the first NUL — the sealed-field frame's separator.
fn split0(b: &[u8]) -> Option<(&[u8], &[u8])> {
    let i = b.iter().position(|c| *c == 0)?;
    Some((&b[..i], &b[i + 1..]))
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> String {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-pwsync-{}-{}-{}",
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

    fn pw_form(url: &str, name: &str, elements: &[(&str, &str)]) -> FormRecord {
        FormRecord {
            url: url.to_string(),
            name: name.to_string(),
            has_password: true,
            elements: elements
                .iter()
                .map(|(k, v)| (k.to_string(), v.to_string()))
                .collect(),
        }
    }

    /// A "device": a data root whose profile autofill store and
    /// sync engine sit side by side — the production shape.
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
        fn dir(&self, scope: &str) -> PathBuf {
            Path::new(&self.root).join("profiles").join(scope)
        }
        fn pair(&self, peer: &Dev, scope: &str) {
            let pe = peer.engine();
            self.engine()
                .member_add(&pe.public_key(), "peer.onion", scope)
                .unwrap();
        }
        /// Share the group secret the SYNC03 channel would carry.
        fn share_group(&self, peer: &Dev, scope: &str) {
            let gk = self.engine().group_secret_get_or_create(scope).unwrap();
            peer.engine().group_secret_install(scope, &gk).unwrap();
        }
        fn set_forms(&self, scope: &str, forms: Vec<FormRecord>) {
            let mut st = store::lock();
            st.set_data_dir(&self.dir(scope).to_string_lossy()).unwrap();
            st.autofill_set(forms).unwrap();
        }
        fn forms(&self, scope: &str) -> Vec<FormRecord> {
            let mut st = store::lock();
            st.set_data_dir(&self.dir(scope).to_string_lossy()).unwrap();
            st.autofill_data().unwrap().forms.clone()
        }
        fn enable(&self, scope: &str, pass: &[u8]) -> RcResult<Value> {
            enable(&self.root, scope, pass)
        }
        fn lock_scope(&self, scope: &str) -> Value {
            lock(&self.root, scope).unwrap()
        }
        fn wire(&self) -> Vec<Value> {
            self.engine()
                .export(Some(STORE), 0)
                .unwrap()
                .iter()
                .map(|o| o.to_json())
                .collect()
        }
        fn scope_wire(&self, scope: &str) -> Vec<Value> {
            self.wire()
                .into_iter()
                .filter(|o| o["scope"].as_str() == Some(scope))
                .collect()
        }
        /// Only the ops THIS device authored — an export also
        /// carries the remote ops the log merged.
        fn own_ops(&self, scope: &str) -> Vec<Value> {
            let id = self.engine().device_id();
            self.scope_wire(scope)
                .into_iter()
                .filter(|o| o["device_id"].as_str() == Some(id.as_str()))
                .collect()
        }
        fn import(&self, docs: &[Value]) -> sync::ImportReport {
            install(&self.root);
            self.engine().import(docs).unwrap()
        }
    }

    /// A paired, group-shared, vault-unlocked device pair — the
    /// baseline every data-flow leg starts from.
    fn paired(tag: &str) -> (Dev, Dev) {
        let a = Dev::new(&format!("{tag}A"));
        let b = Dev::new(&format!("{tag}B"));
        a.share_group(&b, "default");
        a.pair(&b, "default");
        b.pair(&a, "default");
        (a, b)
    }

    #[test]
    fn enable_requires_group_secret() {
        let _g = guard();
        let a = Dev::new("gs");
        // No sync group yet — the vault has no salt to derive
        // against and enable refuses, pointing at pairing.
        let e = a.enable("default", b"pw").unwrap_err();
        assert_eq!(e.status, RcStatus::NotEnabled);
        // Pairing's mint stands in for a joined group.
        a.engine().group_secret_get_or_create("default").unwrap();
        a.enable("default", b"pw").unwrap();
        // The first enabler plants the group verifier.
        let wire = a.scope_wire("default");
        assert_eq!(wire.len(), 1, "{wire:?}");
        assert_eq!(wire[0]["body"]["t"], "pwv");
        let st = status(&a.root, "default").unwrap();
        assert_eq!(st["unlocked"], true);
        assert_eq!(st["enabled_ever"], true);
        // Re-enable with the same passphrase re-gates cleanly.
        a.enable("default", b"pw").unwrap();
        // An empty passphrase is never a vault key.
        assert_eq!(
            a.enable("default", b"").unwrap_err().status,
            RcStatus::EmptyPassphrase
        );
    }

    #[test]
    fn entry_syncs_encrypted_and_decrypts_on_peer() {
        let _g = guard();
        let (a, b) = paired("rt");
        a.enable("default", b"correct horse").unwrap();
        b.enable("default", b"correct horse").unwrap();

        a.set_forms(
            "default",
            vec![pw_form(
                "https://ex/login",
                "loginform",
                &[("user", "alice"), ("pass", "s3cret")],
            )],
        );
        let wire = a.scope_wire("default");
        // pwv + the entry's set op.
        assert_eq!(wire.len(), 2, "{wire:?}");
        let set = &wire[1];
        assert_eq!(set["body"]["t"], "set");
        // Ciphertext only: no plaintext url, user, or secret — and
        // no field names either (they ride inside the seal).
        let blob = set["body"].to_string();
        for needle in ["s3cret", "alice", "https://ex", "loginform", "user", "pass"] {
            assert!(!blob.contains(needle), "plaintext leaked: {needle}");
        }
        assert!(is_eid(set["body"]["e"].as_str().unwrap()));

        let rep = b.import(&wire);
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        let forms = b.forms("default");
        assert_eq!(forms.len(), 1, "{forms:?}");
        assert_eq!(forms[0].url, "https://ex/login");
        assert_eq!(forms[0].name, "loginform");
        assert!(forms[0].has_password);
        assert_eq!(
            forms[0].elements,
            vec![
                ("pass".to_string(), "s3cret".to_string()),
                ("user".to_string(), "alice".to_string()),
            ]
        );
    }

    #[test]
    fn wrong_passphrase_peer_sees_only_ciphertext() {
        let _g = guard();
        let (a, b) = paired("wp");
        a.enable("default", b"right").unwrap();
        a.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "s3cret")])],
        );
        let wire = a.scope_wire("default");

        // B imports BEFORE enabling: the verifier applies (it needs
        // no key) and the ciphertext entry defers.
        let rep = b.import(&wire);
        assert_eq!(rep.accepted, 2);
        assert_eq!(rep.applied, 1, "pwv applies, entry defers: {rep:?}");
        assert_eq!(rep.apply_errors.len(), 1);
        assert!(b.forms("default").is_empty());
        let st = status(&b.root, "default").unwrap();
        assert_eq!(st["pending_apply"], 1);

        // The gate: a wrong passphrase fails the verifier probe —
        // the peer keeps ciphertext only.
        let e = b.enable("default", b"WRONG").unwrap_err();
        assert_eq!(e.status, RcStatus::WrongPassphrase);
        assert!(b.forms("default").is_empty());

        // The right passphrase unlocks; enable's drain applies the
        // deferred ciphertext.
        b.enable("default", b"right").unwrap();
        let forms = b.forms("default");
        assert_eq!(forms.len(), 1);
        assert_eq!(forms[0].elements[0], ("pass".to_string(), "s3cret".to_string()));
        assert_eq!(status(&b.root, "default").unwrap()["pending_apply"], 0);
    }

    #[test]
    fn field_level_lww_merges_concurrent_edits() {
        let _g = guard();
        let (a, b) = paired("lw");
        a.enable("default", b"pw").unwrap();
        b.enable("default", b"pw").unwrap();

        a.set_forms(
            "default",
            vec![pw_form(
                "https://ex",
                "oldname",
                &[("user", "alice"), ("pass", "p1")],
            )],
        );
        let rep = b.import(&a.scope_wire("default"));
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        assert_eq!(b.forms("default").len(), 1);

        // Concurrent edits, disjoint fields: A rotates the password,
        // B renames the record — field LWW merges BOTH.
        a.set_forms(
            "default",
            vec![pw_form(
                "https://ex",
                "oldname",
                &[("user", "alice"), ("pass", "p2")],
            )],
        );
        b.set_forms(
            "default",
            vec![pw_form(
                "https://ex",
                "newname",
                &[("user", "alice"), ("pass", "p1")],
            )],
        );
        let wa = a.scope_wire("default");
        let wb = b.scope_wire("default");
        b.import(&wa[2..]); // skip pwv dup
        a.import(&wb[1..]); // B's log: pwv? no — B has its own pwv

        let fa = a.forms("default");
        let fb = b.forms("default");
        assert_eq!(fa, fb, "devices diverged:\n{fa:?}\n{fb:?}");
        assert_eq!(fa.len(), 1);
        assert_eq!(fa[0].name, "newname");
        assert_eq!(
            fa[0].elements,
            vec![
                ("pass".to_string(), "p2".to_string()),
                ("user".to_string(), "alice".to_string()),
            ]
        );
    }

    #[test]
    fn stale_field_does_not_move_backwards() {
        let _g = guard();
        let (a, b) = paired("st");
        a.enable("default", b"pw").unwrap();
        b.enable("default", b"pw").unwrap();

        a.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "v1")])],
        );
        let first = a.scope_wire("default");
        a.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "v2")])],
        );
        let all = a.scope_wire("default");

        // Deliver the FIELD-DIFF op first, then the stale full op —
        // per-field LWW: the stale pass drops while the older op's
        // untouched fields still merge.  (all = [pwv, set(full v1),
        // set(el.pass v2)].)
        let rep = b.import(&all[2..]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        // A diff op alone can't materialize — no url field yet.
        assert!(b.forms("default").is_empty());
        let rep = b.import(&first[1..]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        let forms = b.forms("default");
        assert_eq!(forms.len(), 1, "{forms:?}");
        assert_eq!(
            forms[0].elements[0],
            ("pass".to_string(), "v2".to_string()),
            "the stale v1 field must not move the entry backwards"
        );
    }

    #[test]
    fn delete_propagates_and_stale_set_does_not_resurrect() {
        let _g = guard();
        let (a, b) = paired("dl");
        a.enable("default", b"pw").unwrap();
        b.enable("default", b"pw").unwrap();

        a.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "s3cret")])],
        );
        let set_wire = a.scope_wire("default");
        a.set_forms("default", vec![]); // local delete
        let del_wire = a.scope_wire("default");
        assert_eq!(del_wire.last().unwrap()["body"]["t"], "del");

        // Deliver del first, then the older set — LWW must refuse
        // the resurrection.
        let rep = b.import(&del_wire[2..]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert!(b.forms("default").is_empty());
        let rep = b.import(&set_wire[1..2]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert!(
            b.forms("default").is_empty(),
            "a set older than the del must not resurrect: {:?}",
            b.forms("default")
        );

        // In-order on the happy path: set lands, del removes.
        let (c, d) = paired("dl2");
        c.enable("default", b"pw").unwrap();
        d.enable("default", b"pw").unwrap();
        c.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "s3cret")])],
        );
        d.import(&c.scope_wire("default"));
        assert_eq!(d.forms("default").len(), 1);
        c.set_forms("default", vec![]);
        let rep = d.import(&c.scope_wire("default")[2..]);
        assert_eq!(rep.applied, 1);
        assert!(d.forms("default").is_empty());
    }

    #[test]
    fn strictly_opt_in_until_enable() {
        let _g = guard();
        let (a, b) = paired("oi");
        // No enable anywhere: local password writes emit NOTHING —
        // not even a pwv.
        a.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "s3cret")])],
        );
        assert!(a.scope_wire("default").is_empty());

        // A peer that never enables holds remote ops as pending
        // ciphertext and writes nothing locally.
        a.enable("default", b"pw").unwrap();
        a.set_forms(
            "default",
            vec![pw_form("https://two", "", &[("pass", "x")])],
        );
        let rep = b.import(&a.scope_wire("default"));
        assert!(rep.applied < rep.accepted, "{rep:?}");
        assert!(rep.apply_errors.len() >= 1);
        assert!(b.forms("default").is_empty());
        // B's own writes still emit nothing while locked — the
        // wire holds only the remote ops it imported.
        b.set_forms(
            "default",
            vec![pw_form("https://mine", "", &[("pass", "y")])],
        );
        assert!(b.own_ops("default").is_empty());
        assert_eq!(status(&b.root, "default").unwrap()["unlocked"], false);
    }

    #[test]
    fn pre_sync_entries_advertise_on_enable() {
        let _g = guard();
        let (a, b) = paired("ad");
        // Passwords written before the opt-in stay local until it.
        a.set_forms(
            "default",
            vec![pw_form("https://pre", "", &[("pass", "old")])],
        );
        assert!(a.scope_wire("default").is_empty());
        a.enable("default", b"pw").unwrap();
        let wire = a.scope_wire("default");
        // pwv + the advertised entry.
        assert_eq!(wire.len(), 2, "{wire:?}");
        assert_eq!(wire[1]["body"]["t"], "set");
        b.enable("default", b"pw").unwrap();
        let rep = b.import(&wire);
        assert_eq!(rep.applied, 2, "{:?}", rep.apply_errors);
        assert_eq!(b.forms("default")[0].url, "https://pre");
    }

    #[test]
    fn lock_drops_vault_and_emission() {
        let _g = guard();
        let (a, _b) = paired("lk");
        a.enable("default", b"pw").unwrap();
        a.set_forms(
            "default",
            vec![pw_form("https://ex", "", &[("pass", "1")])],
        );
        let before = a.scope_wire("default").len();
        let r = a.lock_scope("default");
        assert_eq!(r["was_unlocked"], true);
        assert_eq!(status(&a.root, "default").unwrap()["unlocked"], false);
        a.set_forms(
            "default",
            vec![pw_form("https://ex2", "", &[("pass", "2")])],
        );
        assert_eq!(a.scope_wire("default").len(), before);
        // Re-enable re-gates on the planted verifier.
        a.enable("default", b"pw").unwrap();
        // The while-locked write advertises on re-enable.
        let wire = a.scope_wire("default");
        assert!(wire.len() > before);
    }

    #[test]
    fn malformed_and_mismatched_ops_are_refused() {
        let _g = guard();
        let (a, b) = paired("bd");
        a.enable("default", b"pw").unwrap();
        b.enable("default", b"pw").unwrap();

        // Hand-built bad ops verify+import but the apply lane
        // refuses them.
        for body in [
            json!({"t":"set","e":"tooshort"}),
            json!({"t":"set"}),
            json!({"t":"del","e":"tooshort"}),
            json!({"t":"del"}),
            json!({"t":"pwv"}),
            json!({"t":"wat","e":entry_id("https://x")}),
        ] {
            let op = a.engine().append(STORE, "default", &body).unwrap();
            let rep = b.import(&[op.to_json()]);
            assert_eq!(rep.accepted, 1, "{body}");
            assert_eq!(rep.applied, 0, "{body}");
            assert_eq!(rep.apply_errors.len(), 1, "{body}");
        }

        // A well-formed op whose sealed field binds a DIFFERENT eid
        // drops the field, applies cleanly, writes nothing.
        let mut ps = PwSync::default();
        ps.key = Some(vault_key(
            b"pw",
            &a.engine().group_secret("default").unwrap().unwrap(),
        ).unwrap());
        let blob = ps
            .seal_field(&entry_id("https://real"), "url", "https://real")
            .unwrap();
        let op = a
            .engine()
            .append(
                STORE,
                "default",
                &json!({"t":"set","e":entry_id("https://forged"),"f":[blob]}),
            )
            .unwrap();
        let rep = b.import(&[op.to_json()]);
        assert_eq!(rep.applied, 1, "{:?}", rep.apply_errors);
        assert!(b.forms("default").is_empty());
    }
}
