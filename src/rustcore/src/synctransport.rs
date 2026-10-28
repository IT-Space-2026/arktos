//! SYNC02 — serverless sync transport.
//!
//! Transports only ever move framed bytes; the sync engine (SYNC01)
//! stays transport-agnostic and the store adapters (SYNC04+) never
//! see a socket.  Two backends implement the same [`SyncTransport`]
//! seam so SYNC10's LAN fast path can plug in beside them:
//!
//!   "tor" — embedded Arti (feature `synctor`, on by default): a
//!       per-device v3 onion service whose hsid key lives in the Arti
//!       keystore under `<data root>/sync/tor/state`, so the onion
//!       address is stable across restarts.  Authorized-client gating
//!       rides restricted discovery (x25519 descriptor encryption):
//!       unpaired peers cannot decrypt the descriptor and are refused
//!       at the Tor layer, before a single app byte.  Outbound links
//!       are client circuits to a peer's onion.
//!   "mem" — in-process loopback for tests: same authorize/connect/
//!       frame semantics, no network — the parity contract the real
//!       backend is tested against.
//!   "lan" — SYNC10 fast path (feature `synclan`, on by default):
//!       direct QUIC links between paired devices on the same local
//!       network, discovered by mDNS.  TLS 1.3 inside QUIC carries a
//!       self-signed cert bound to each device's Ed25519 sync
//!       identity; the member grant set is the admission gate, so an
//!       unpaired device is refused during the handshake — the same
//!       pre-app-byte refusal tor's restricted discovery enforces.
//!   "auto" — SYNC10 composite (needs both features): the lan leg
//!       plus the tor leg behind one surface.  Onion endpoints
//!       resolve through LAN discovery first (peers advertise their
//!       onion in the mDNS TXT `on=` record) and fall back to a Tor
//!       circuit when the peer is not co-located.
//!
//! Async surface (the torctl.rs convention): every backend keeps a
//! pull queue of [`TransportEvent`]s drained by `poll()`; each push
//! announces itself through `notify::emit("synctransport")` so the
//! Qt side can queue a drain on the GUI thread.  mw ops in domain 19
//! drive start/stop/authorize/connect/send/poll over the envelope.
//!
//! Authorization model: pairing (SYNC03) hands the peer our
//! `descriptor:x25519:<pub>` discovery key and we record the peer's.
//! `authorize(nickname, key)` adds an authorized client to the onion
//! service (persisted under the transport dir so it survives restart
//! and reconfiguration); `discovery_key(peer_onion)` mints the
//! keypair WE use to reach THEIR service — the secret half never
//! leaves this device's keystore.
//!
//! Endpoint strings: tor endpoints are `"<56 chars>.onion:<port>"`
//! (port [`SYNC_ONION_PORT`]); mem endpoints are `"mem:<name>"`;
//! lan endpoints are `"lan://<device-id>"`, `"lan://<host>:<port>"`,
//! or a bare `"<host>:<port>"`.

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, LazyLock, Mutex, Weak};
use std::time::Duration;

use serde_json::{json, Value};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::notify;
use crate::sync::SYNC_DIR;

#[cfg(feature = "synctor")]
pub mod tor;

#[cfg(feature = "synclan")]
pub mod lan;

#[cfg(all(feature = "synclan", feature = "synctor"))]
pub mod auto;

/// Directory under `<data root>/sync/` holding the transport state
/// (Arti state+cache, authorized-clients file).
pub const TRANSPORT_DIR: &str = "tor";
/// The onion-service port the sync listener is published on.  The
/// port lives inside the rendezvous circuit — it is a protocol
/// constant, not a network resource.
pub const SYNC_ONION_PORT: u16 = 14430;
/// Persisted authorized-client set (JSON {nickname: discovery_key}).
pub const AUTH_FILE: &str = "authorized_clients.json";
/// Largest accepted frame payload — sync op batches ride frames.
pub const MAX_FRAME: usize = 16 * 1024 * 1024;

/// One drained event from a transport's pull queue.
#[derive(Debug)]
pub enum TransportEvent {
    /// The listener is up and serving (tor: service launched and
    /// able to publish; mem: registered in the loop table).
    Ready,
    /// Tor bootstrap progress, 0-100 — coarse status for the UI.
    Bootstrap(u8),
    /// A peer opened a link to our listener.
    Incoming { link: u64, from: String },
    /// A complete frame arrived on a link.
    Frame { link: u64, payload: Vec<u8> },
    /// A link went away — the peer closed or the transport died.
    LinkClosed { link: u64, reason: String },
    /// The transport itself failed (tor bootstrap died, service
    /// lost its directory, …) — `stop` is the recovery.
    Failed { reason: String },
}

/// The transport seam every backend implements.  All calls are
/// synchronous — backends run their own runtime internally and
/// deliver inbound activity through [`TransportEvent`]s.
pub trait SyncTransport: Send + Sync {
    /// Human-readable backend name ("tor" / "mem").
    fn backend(&self) -> &'static str;
    /// Our published endpoint ("<onion>:<port>" / "mem:<name>") —
    /// what a peer dials.
    fn endpoint(&self) -> RcResult<String>;
    /// Grant `nickname` (a client-auth identity) access to our
    /// listener.  `discovery_key` is the peer's public
    /// `descriptor:x25519:` key for tor; ignored by mem.
    fn authorize(&self, nickname: &str, discovery_key: &str) -> RcResult<()>;
    /// Revoke a previously authorized nickname.  Returns whether a
    /// grant existed.
    fn deauthorize(&self, nickname: &str) -> RcResult<bool>;
    /// Currently authorized nicknames.
    fn authorized(&self) -> Vec<String>;
    /// Mint (or reuse) the discovery keypair we use to reach the
    /// peer service at `peer_onion`, returning the PUBLIC half —
    /// `descriptor:x25519:<base32>` — which the peer must authorize
    /// on its side.  The secret half never leaves this device.
    /// Only meaningful on the tor backend.
    fn discovery_key(&self, peer_onion: &str) -> RcResult<String>;
    /// Install a peer-supplied descriptor-encryption SECRET for
    /// `peer_onion` into our client keystore (SYNC03): the pairing
    /// invite's derived client-auth key — how a not-yet-authorized
    /// joiner reaches the inviter's restricted service exactly once.
    /// Backends without descriptor auth treat it as a no-op.
    fn import_discovery_key(&self, peer_onion: &str, secret: &[u8; 32]) -> RcResult<()> {
        let _ = (peer_onion, secret);
        fail(
            RcStatus::Unavailable,
            "this backend has no discovery keys to import",
        )
    }
    /// Rotate our discovery keypair for `peer_onion` to a fresh
    /// permanent one (SYNC03): called after the invite-derived key
    /// did its one-time job so the QR material's blast radius stays
    /// the ceremony itself.  Returns the new PUBLIC key the peer
    /// should authorize; "" on backends with name-based auth.
    fn rotate_discovery_key(&self, peer_onion: &str) -> RcResult<String> {
        let _ = peer_onion;
        fail(
            RcStatus::Unavailable,
            "this backend has no discovery keys to rotate",
        )
    }
    /// Open an outbound link to `endpoint` identifying ourselves as
    /// `as_nickname` (the identity the peer authorized).  Returns
    /// the link id.  Refused when the peer's gate rejects us —
    /// on tor, before a single app byte is exchanged.
    fn connect(&self, endpoint: &str, as_nickname: &str) -> RcResult<u64>;
    /// Queue one frame for sending on a link.
    fn send(&self, link: u64, frame: &[u8]) -> RcResult<()>;
    /// Close one link.  Returns whether it existed.
    fn close_link(&self, link: u64) -> RcResult<bool>;
    /// Drain the event queue.
    fn poll(&self) -> Vec<TransportEvent>;
    /// Count of live links (for status reporting).
    fn link_count(&self) -> usize;
    /// SYNC10: the discovered-LAN-peer view for status reporting —
    /// device_id/onion/port/addrs per discovered device.  Empty on
    /// backends without discovery.
    fn discovered_peers_json(&self) -> Vec<Value> {
        Vec::new()
    }
    /// Tear the transport down.  Idempotent.
    fn stop(&self);
}

/// One transport per data root, started/stopped through the mw ops.
static TRANSPORTS: LazyLock<Mutex<HashMap<PathBuf, Arc<dyn SyncTransport>>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

fn transports() -> std::sync::MutexGuard<'static, HashMap<PathBuf, Arc<dyn SyncTransport>>> {
    TRANSPORTS.lock().unwrap_or_else(|e| e.into_inner())
}

/// The transport currently running under `data_root`, if any.
pub fn running(data_root: &Path) -> Option<Arc<dyn SyncTransport>> {
    transports().get(data_root).cloned()
}

/// Start backend `backend` ("tor" / "mem" / "lan" / "auto") under `data_root`.
/// Re-starting while one runs is idempotent — the live transport is
/// returned, never a second instance racing the same state dir.
pub fn start(data_root: &Path, backend: &str, name: &str) -> RcResult<Arc<dyn SyncTransport>> {
    let dir = data_root.join(SYNC_DIR);
    let _ = &dir;
    match backend {
        #[cfg(feature = "synctor")]
        "tor" => {
            let t = tor::TorTransport::start(&dir)?;
            Ok(register(data_root, t))
        }
        #[cfg(not(feature = "synctor"))]
        "tor" => fail(
            RcStatus::Unavailable,
            "rustcore built without the synctor feature — tor backend unavailable",
        ),
        #[cfg(feature = "synclan")]
        "lan" => {
            let t = lan::LanTransport::start(&dir)?;
            Ok(register(data_root, t))
        }
        #[cfg(not(feature = "synclan"))]
        "lan" => fail(
            RcStatus::Unavailable,
            "rustcore built without the synclan feature — lan backend unavailable",
        ),
        #[cfg(all(feature = "synclan", feature = "synctor"))]
        "auto" => {
            let t = auto::AutoTransport::start(&dir)?;
            Ok(register(data_root, t))
        }
        #[cfg(not(all(feature = "synclan", feature = "synctor")))]
        "auto" => fail(
            RcStatus::Unavailable,
            "auto backend needs both synclan and synctor features built",
        ),
        "mem" => {
            let t = LoopbackTransport::start(name);
            Ok(register(data_root, t))
        }
        other => fail(
            RcStatus::InvalidArgument,
            format!("unknown sync transport backend \"{other}\""),
        ),
    }
}

/// SYNC10: start the "lan" backend under `data_root` on explicit
/// ports — the mw op maps its `{quic_port, mdns_port, mdns_group}`
/// knobs onto this; the standard `start(root,"lan")` path uses the
/// default config.
#[cfg(feature = "synclan")]
pub fn start_lan(
    data_root: &Path,
    cfg: lan::LanConfig,
) -> RcResult<Arc<dyn SyncTransport>> {
    let t = lan::LanTransport::start_with(&data_root.join(SYNC_DIR), cfg)?;
    Ok(register(data_root, t))
}

fn register(data_root: &Path, t: Arc<dyn SyncTransport>) -> Arc<dyn SyncTransport> {
    let mut map = transports();
    if let Some(existing) = map.get(data_root) {
        // Idempotent restart: the live transport wins — a second
        // instance on the same state dir would race the keystore.
        t.stop();
        return existing.clone();
    }
    map.insert(data_root.to_path_buf(), t.clone());
    t
}

/// Our public discovery key for the peer service at `peer_onion`.
/// Works with the transport down — the key lives in the keystore,
/// minted once per peer hsid (idempotent, never a re-roll).
pub fn discovery_key(data_root: &Path, peer_onion: &str) -> RcResult<String> {
    if let Some(t) = running(data_root) {
        return t.discovery_key(peer_onion);
    }
    #[cfg(feature = "synctor")]
    {
        tor::discovery_key(&data_root.join(SYNC_DIR), peer_onion)
    }
    #[cfg(not(feature = "synctor"))]
    {
        let _ = (data_root, peer_onion);
        fail(
            RcStatus::Unavailable,
            "rustcore built without the synctor feature — discovery keys unavailable",
        )
    }
}

/// SYNC03: install the invite-derived discovery SECRET for
/// `peer_onion` — the joiner half of the pairing bootstrap.  With a
/// live transport the trait route wins (mem answers a no-op Ok);
/// with none the tor keystore path handles it offline.
pub fn import_discovery_key(
    data_root: &Path,
    peer_onion: &str,
    secret: &[u8; 32],
) -> RcResult<()> {
    if let Some(t) = running(data_root) {
        return t.import_discovery_key(peer_onion, secret);
    }
    #[cfg(feature = "synctor")]
    {
        tor::import_discovery_key(&data_root.join(SYNC_DIR), peer_onion, secret)
    }
    #[cfg(not(feature = "synctor"))]
    {
        let _ = (data_root, peer_onion, secret);
        fail(
            RcStatus::Unavailable,
            "rustcore built without the synctor feature — discovery keys unavailable",
        )
    }
}

/// SYNC03: rotate to a fresh permanent discovery key for
/// `peer_onion`, returning the new public half for the peer to
/// authorize.  Empty string when the running backend is name-based
/// (mem — nothing to rotate); tor works offline via the inert
/// keystore when no transport runs.
pub fn rotate_discovery_key(data_root: &Path, peer_onion: &str) -> RcResult<String> {
    if let Some(t) = running(data_root) {
        return t.rotate_discovery_key(peer_onion);
    }
    #[cfg(feature = "synctor")]
    {
        tor::rotate_discovery_key(&data_root.join(SYNC_DIR), peer_onion)
    }
    #[cfg(not(feature = "synctor"))]
    {
        let _ = (data_root, peer_onion);
        fail(
            RcStatus::Unavailable,
            "rustcore built without the synctor feature — discovery keys unavailable",
        )
    }
}

/// Grant `nickname` a discovery key — on the live transport when
/// one runs (its authorize() persists + reconfigures), otherwise
/// straight into authorized_clients.json so the grant is replayed
/// at the next launch (SYNC03's invite can outlive a transport
/// restart).
pub fn authorize_grant(data_root: &Path, nickname: &str, discovery_key: &str) -> RcResult<()> {
    let dir = data_root.join(SYNC_DIR);
    if let Some(t) = running(data_root) {
        return t.authorize(nickname, discovery_key);
    }
    let mut set = load_authorized(&dir)?;
    set.insert(nickname.to_string(), discovery_key.to_string());
    save_authorized(&dir, &set)
}

/// Revoke a grant — live transport first, else the persisted file.
pub fn deauthorize_grant(data_root: &Path, nickname: &str) -> RcResult<bool> {
    let dir = data_root.join(SYNC_DIR);
    if let Some(t) = running(data_root) {
        return t.deauthorize(nickname);
    }
    let mut set = load_authorized(&dir)?;
    if set.remove(nickname).is_some() {
        save_authorized(&dir, &set)?;
        return Ok(true);
    }
    Ok(false)
}

/// Stop and deregister the transport under `data_root`.
pub fn stop(data_root: &Path) -> bool {
    if let Some(t) = transports().remove(data_root) {
        t.stop();
        true
    } else {
        false
    }
}

fn push_event(q: &Mutex<VecDeque<TransportEvent>>, ev: TransportEvent) {
    q.lock().unwrap_or_else(|e| e.into_inner()).push_back(ev);
    notify::emit("synctransport");
}

fn drain_events(q: &Mutex<VecDeque<TransportEvent>>) -> Vec<TransportEvent> {
    q.lock()
        .unwrap_or_else(|e| e.into_inner())
        .drain(..)
        .collect()
}

fn io_fail(what: &str, e: impl std::fmt::Display) -> Fail {
    Fail {
        status: RcStatus::Io,
        msg: format!("{what}: {e}"),
    }
}

#[cfg_attr(not(feature = "synctor"), allow(dead_code))]
fn bad_arg<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::InvalidArgument, msg)
}

fn not_found<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::NotFound, msg)
}

fn refused<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::Crypto, msg)
}

// ---- wire framing ----------------------------------------------------------
//
// A frame on a link is `<u32 LE len><payload>` — deliberately boring:
// the sync wire ops (sync::WireOp JSON docs) ride inside, and the
// transport stays content-blind.  MAX_FRAME bounds a hostile peer's
// announced length before any allocation happens.

/// Encode one payload as a wire frame.
pub fn encode_frame(payload: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(4 + payload.len());
    v.extend_from_slice(&(payload.len() as u32).to_le_bytes());
    v.extend_from_slice(payload);
    v
}

// ---- connect backoff ---------------------------------------------------------
//
// Fresh onion descriptors take tens of seconds to propagate through
// the HSDir ring, so `connect` retries transient failures behind a
// bounded exponential schedule (deterministic — the retry *policy*
// stays a caller decision for SYNC03+).

#[cfg_attr(not(feature = "synctor"), allow(dead_code))]
const BACKOFF_BASE_MS: u64 = 500;
#[cfg_attr(not(feature = "synctor"), allow(dead_code))]
const BACKOFF_CAP_MS: u64 = 30_000;
/// Total wall-clock budget for one `connect` before the last error
/// is surfaced to the caller.
#[cfg_attr(not(feature = "synctor"), allow(dead_code))]
const CONNECT_DEADLINE: Duration = Duration::from_secs(120);

/// Delay before attempt `attempt` (0-based retry number).
#[cfg_attr(not(feature = "synctor"), allow(dead_code))]
fn backoff_delay(attempt: u32) -> Duration {
    let shift = attempt.min(20);
    let ms = BACKOFF_BASE_MS.saturating_mul(1u64 << shift);
    Duration::from_millis(ms.min(BACKOFF_CAP_MS))
}

// ---- authorized-client persistence -------------------------------------------
//
// The authorized set is transport-layer pairing state: it survives
// restarts (paired devices keep access) and is replayed into the
// onion-service config at every launch.

fn auth_path(dir: &Path) -> PathBuf {
    dir.join(TRANSPORT_DIR).join(AUTH_FILE)
}

/// Load the persisted {nickname: discovery_key} set — a missing or
/// empty file is an empty set; a malformed file is Corrupt (never a
/// silent re-pair-everyone).
pub fn load_authorized(dir: &Path) -> RcResult<BTreeMap<String, String>> {
    let p = auth_path(dir);
    let raw = match std::fs::read_to_string(&p) {
        Ok(s) => s,
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(BTreeMap::new()),
        Err(e) => return Err(io_fail("read authorized_clients", e)),
    };
    let v: Value = serde_json::from_str(&raw)
        .map_err(|e| io_fail("parse authorized_clients", e))?;
    let obj = v
        .as_object()
        .ok_or_else(|| io_fail("parse authorized_clients", "top level is not an object"))?;
    Ok(obj
        .iter()
        .filter_map(|(k, v)| v.as_str().map(|s| (k.clone(), s.to_string())))
        .collect())
}

/// Persist the set atomically (tmp + rename) so a torn write can
/// never corrupt the grant list.
#[cfg_attr(not(feature = "synctor"), allow(dead_code))]
fn save_authorized(dir: &Path, set: &BTreeMap<String, String>) -> RcResult<()> {
    let root = dir.join(TRANSPORT_DIR);
    std::fs::create_dir_all(&root).map_err(|e| io_fail("create transport dir", e))?;
    let tmp = root.join(".authorized_clients.tmp");
    let body = serde_json::to_string_pretty(&json!(set))
        .map_err(|e| io_fail("serialize authorized_clients", e))?;
    std::fs::write(&tmp, body).map_err(|e| io_fail("write authorized_clients", e))?;
    std::fs::rename(&tmp, auth_path(dir)).map_err(|e| io_fail("rename authorized_clients", e))?;
    Ok(())
}

/// Read the persisted set without a running transport (status op,
/// and the mem backend's parity view of "who would be allowed").
pub fn authorized_set(dir: &Path) -> BTreeMap<String, String> {
    load_authorized(dir).unwrap_or_default()
}

// ---- loopback backend ----------------------------------------------------------
//
// In-process parity backend: same authorize/connect/frame/close
// semantics as tor, zero network.  Besides keeping `cargo test`
// hermetic it pins the trait contract SYNC10's LAN transport must
// satisfy — a peer refused by the gate gets no Incoming link and no
// frames, ever.

/// Process-wide loop table: endpoint name -> live transport.
static LOOPS: LazyLock<Mutex<HashMap<String, Weak<LoopbackTransport>>>> =
    LazyLock::new(|| Mutex::new(HashMap::new()));

enum LoopMsg {
    Frame(Vec<u8>),
    Close(String),
}

pub struct LoopbackTransport {
    me: Mutex<Weak<LoopbackTransport>>,
    endpoint: String,
    authorized: Mutex<BTreeMap<String, String>>,
    /// link id -> the send half feeding the peer's reader pump.
    links: Mutex<HashMap<u64, std::sync::mpsc::Sender<LoopMsg>>>,
    events: Mutex<VecDeque<TransportEvent>>,
    next_link: AtomicU64,
    running: Mutex<bool>,
}

impl LoopbackTransport {
    /// Start a loopback listener on "mem:<name>".
    pub fn start(name: &str) -> Arc<Self> {
        let t = Arc::new(LoopbackTransport {
            me: Mutex::new(Weak::new()),
            endpoint: format!("mem:{name}"),
            authorized: Mutex::new(BTreeMap::new()),
            links: Mutex::new(HashMap::new()),
            events: Mutex::new(VecDeque::new()),
            next_link: AtomicU64::new(1),
            running: Mutex::new(true),
        });
        *t.me.lock().unwrap_or_else(|e| e.into_inner()) = Arc::downgrade(&t);
        LOOPS.lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(t.endpoint.clone(), Arc::downgrade(&t));
        push_event(&t.events, TransportEvent::Ready);
        t
    }

    /// Splice a reader pump that turns inbound `LoopMsg`s into
    /// Frame/LinkClosed events on this transport.  Loopback pumps
    /// are plain threads — there is no async runtime underneath.
    fn pump_inbound(&self, link: u64, rx: std::sync::mpsc::Receiver<LoopMsg>) {
        let t = match self.me.lock().unwrap_or_else(|e| e.into_inner()).upgrade() {
            Some(t) => t,
            None => return,
        };
        std::thread::spawn(move || loop {
            match rx.recv() {
                Ok(LoopMsg::Frame(payload)) => {
                    push_event(&t.events, TransportEvent::Frame { link, payload });
                }
                Ok(LoopMsg::Close(reason)) => {
                    push_event(&t.events, TransportEvent::LinkClosed { link, reason });
                    break;
                }
                Err(_) => {
                    push_event(&t.events, TransportEvent::LinkClosed {
                        link,
                        reason: "peer dropped".to_string(),
                    });
                    break;
                }
            }
        });
    }

    fn take_links(&self) -> HashMap<u64, std::sync::mpsc::Sender<LoopMsg>> {
        std::mem::take(&mut *self.links.lock().unwrap_or_else(|e| e.into_inner()))
    }
}

impl SyncTransport for LoopbackTransport {
    fn backend(&self) -> &'static str {
        "mem"
    }

    fn endpoint(&self) -> RcResult<String> {
        Ok(self.endpoint.clone())
    }

    fn authorize(&self, nickname: &str, _key: &str) -> RcResult<()> {
        self.authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(nickname.to_string(), String::new());
        Ok(())
    }

    fn deauthorize(&self, nickname: &str) -> RcResult<bool> {
        Ok(self
            .authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(nickname)
            .is_some())
    }

    fn authorized(&self) -> Vec<String> {
        self.authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .keys()
            .cloned()
            .collect()
    }

    fn discovery_key(&self, _peer_onion: &str) -> RcResult<String> {
        fail(
            RcStatus::Unavailable,
            "mem backend has no discovery keys — authorize is name-based",
        )
    }

    fn import_discovery_key(&self, _peer_onion: &str, _secret: &[u8; 32]) -> RcResult<()> {
        Ok(())
    }

    fn rotate_discovery_key(&self, _peer_onion: &str) -> RcResult<String> {
        Ok(String::new())
    }

    fn connect(&self, endpoint: &str, as_nickname: &str) -> RcResult<u64> {
        let target = {
            let table = LOOPS.lock().unwrap_or_else(|e| e.into_inner());
            table.get(endpoint).and_then(Weak::upgrade)
        }
        .ok_or_else(|| io_fail("connect", format_args!("no loopback listener at {endpoint}")))?;
        if !*target.running.lock().unwrap_or_else(|e| e.into_inner()) {
            return Err(io_fail("connect", format_args!("listener {endpoint} is stopped")));
        }
        {
            let authed = target.authorized.lock().unwrap_or_else(|e| e.into_inner());
            if !authed.contains_key(as_nickname) {
                return refused(format!(
                    "peer {endpoint} refused client \"{as_nickname}\" — not authorized"
                ));
            }
        }
        // Splice a duplex pair: our tx feeds their reader pump and
        // vice versa.  Both halves get their own link id — the
        // events land on the owning transport's queue.
        let (a_tx, a_rx) = std::sync::mpsc::channel::<LoopMsg>();
        let (b_tx, b_rx) = std::sync::mpsc::channel::<LoopMsg>();
        let our_id = self.next_link.fetch_add(1, Ordering::SeqCst);
        let their_id = target.next_link.fetch_add(1, Ordering::SeqCst);
        self.links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(our_id, a_tx);
        target
            .links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(their_id, b_tx);
        self.pump_inbound(our_id, b_rx);
        target.pump_inbound(their_id, a_rx);
        push_event(&target.events, TransportEvent::Incoming {
            link: their_id,
            from: self.endpoint.clone(),
        });
        Ok(our_id)
    }

    fn send(&self, link: u64, frame: &[u8]) -> RcResult<()> {
        let links = self.links.lock().unwrap_or_else(|e| e.into_inner());
        let Some(tx) = links.get(&link) else {
            return not_found(format!("no link {link}"));
        };
        tx.send(LoopMsg::Frame(frame.to_vec()))
            .map_err(|e| io_fail("send", e))
    }

    fn close_link(&self, link: u64) -> RcResult<bool> {
        let w = self
            .links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&link);
        match w {
            Some(tx) => {
                let _ = tx.send(LoopMsg::Close("closed by peer".to_string()));
                Ok(true)
            }
            None => Ok(false),
        }
    }

    fn poll(&self) -> Vec<TransportEvent> {
        drain_events(&self.events)
    }

    fn link_count(&self) -> usize {
        self.links.lock().unwrap_or_else(|e| e.into_inner()).len()
    }

    fn stop(&self) {
        *self.running.lock().unwrap_or_else(|e| e.into_inner()) = false;
        let links = self.take_links();
        for (_, tx) in links {
            let _ = tx.send(LoopMsg::Close("transport stopped".to_string()));
        }
        LOOPS.lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&self.endpoint);
    }
}

// ---- tests -------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-tp-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = std::fs::remove_dir_all(&dir);
        std::fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn wait_frame(t: &Arc<dyn SyncTransport>, link: u64, tries: u32) -> Vec<u8> {
        for _ in 0..tries {
            for ev in t.poll() {
                if let TransportEvent::Frame { link: l, payload } = ev {
                    if l == link {
                        return payload;
                    }
                }
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        panic!("no frame on link {link}");
    }

    #[test]
    fn frame_encode_layout() {
        let f = encode_frame(b"abc");
        assert_eq!(&f[..4], &3u32.to_le_bytes());
        assert_eq!(&f[4..], b"abc");
        assert_eq!(encode_frame(b"").len(), 4);
    }

    #[test]
    fn backoff_is_exponential_and_capped() {
        assert_eq!(backoff_delay(0), Duration::from_millis(500));
        assert_eq!(backoff_delay(1), Duration::from_millis(1000));
        assert_eq!(backoff_delay(4), Duration::from_millis(8000));
        assert_eq!(backoff_delay(20), Duration::from_millis(BACKOFF_CAP_MS));
        assert_eq!(backoff_delay(1000), Duration::from_millis(BACKOFF_CAP_MS));
        // Monotonic until the cap, then flat.
        let mut prev = Duration::ZERO;
        for i in 0..10 {
            let d = backoff_delay(i);
            assert!(d >= prev);
            prev = d;
        }
    }

    #[test]
    fn authorized_set_round_trip_and_corrupt_is_loud() {
        let dir = tmpdir("authrt");
        assert!(load_authorized(&dir).unwrap().is_empty());
        let mut set = BTreeMap::new();
        set.insert("peer1".to_string(), "descriptor:x25519:K".to_string());
        save_authorized(&dir, &set).unwrap();
        assert_eq!(load_authorized(&dir).unwrap(), set);
        assert_eq!(authorized_set(&dir), set);
        std::fs::write(auth_path(&dir), "{not json").unwrap();
        assert!(load_authorized(&dir).is_err());
    }

    #[test]
    fn loopback_gate_and_ping_parity() {
        let a: Arc<dyn SyncTransport> = LoopbackTransport::start("A");
        let b: Arc<dyn SyncTransport> = LoopbackTransport::start("B");

        // Unauthorized client is refused before any link exists.
        let r = b.connect("mem:A", "mallory");
        assert!(r.is_err());
        assert_eq!(a.link_count(), 0);

        // Authorized client links; the listener sees Incoming.
        a.authorize("bob", "").unwrap();
        let our_link = b.connect("mem:A", "bob").unwrap();
        let mut their_link = None;
        for _ in 0..100 {
            for ev in a.poll() {
                if let TransportEvent::Incoming { link, from } = ev {
                    assert_eq!(from, "mem:B");
                    their_link = Some(link);
                }
            }
            if their_link.is_some() {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        let their_link = their_link.expect("A never saw Incoming");

        // Frame parity both ways: the verify ping rides this path.
        b.send(our_link, b"PING").unwrap();
        assert_eq!(wait_frame(&a, their_link, 100), b"PING");
        a.send(their_link, b"PONG").unwrap();
        assert_eq!(wait_frame(&b, our_link, 100), b"PONG");

        // Deauthorize revokes future links; the live one survives.
        a.deauthorize("bob").unwrap();
        assert!(b.connect("mem:A", "bob").is_err());
        b.send(our_link, b"STILL").unwrap();
        assert_eq!(wait_frame(&a, their_link, 100), b"STILL");

        // Close propagates as LinkClosed on the peer.
        b.close_link(our_link).unwrap();
        let mut saw_close = false;
        for _ in 0..100 {
            for ev in a.poll() {
                if let TransportEvent::LinkClosed { link, .. } = ev {
                    saw_close |= link == their_link;
                }
            }
            if saw_close {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(saw_close, "peer close never reached A");
        a.stop();
        b.stop();
    }

    #[test]
    fn loopback_restart_same_endpoint_and_unknown_link_errors() {
        let a: Arc<dyn SyncTransport> = LoopbackTransport::start("C");
        assert_eq!(a.endpoint().unwrap(), "mem:C");
        assert!(a.send(999, b"x").is_err());
        assert_eq!(a.close_link(999).unwrap(), false);
        a.stop();
        // Endpoint frees on stop; a fresh start re-registers.
        let a2: Arc<dyn SyncTransport> = LoopbackTransport::start("C");
        assert_eq!(a2.endpoint().unwrap(), "mem:C");
        a2.stop();
    }
}
