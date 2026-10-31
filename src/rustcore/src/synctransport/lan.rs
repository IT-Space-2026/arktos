//! SYNC10 — the "lan" [`SyncTransport`] backend: a QUIC fast path
//! between paired devices on the same local network, with mDNS
//! discovery.  Same seam, same framed bytes, same grant file — the
//! sync engine never learns which wire an op rode.
//!
//! Wire: one `quinn` endpoint is both listener and dialer; each
//! `connect` opens a fresh QUIC connection with one bidirectional
//! stream carrying `<u32 LE len><payload>` frames, byte-identical to
//! the tor link contract.  TLS 1.3 is mandatory in QUIC, and the
//! cert it carries is how auth stays honest: BOTH sides present a
//! self-signed Ed25519 cert whose key is the SYNC DEVICE IDENTITY
//! (`sync/device.key`), and each side's verifier extracts the peer's
//! SPKI and gates on this profile's authorized `member-<id16>`
//! grants plus the sync.db members table.  An unpaired device is
//! refused during the handshake — before a single app byte, exactly
//! the parity tor's restricted-discovery gate pins.
//!
//! Discovery: `mdns.rs` announces `_arktos-sync._tcp.local` (TXT:
//! `dev=<id64>`, `on=<onion>` when a tor endpoint exists).  Endpoints
//! a caller can dial: `lan://<device-id-hex-or-prefix>` (resolved
//! through discovery), `lan://<host>:<port>` and bare `<host>:<port>`
//! (direct dial — the peer must still pass the member gate).
//!
//! Honest edges: IPv4 only; `pair-*` grants are NOT honored — the
//! invite-derived gate is tor-specific key material, so the pairing
//! ceremony stays on tor (LAN links exist only between already-
//! paired devices — stricter than tor, never weaker).

pub mod devcert;
pub mod mdns;

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::fmt;
use std::net::{Ipv4Addr, SocketAddr};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, Once};
use std::time::Duration;

use tokio::io::AsyncWriteExt;
use rustls::client::danger::{HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier};
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::DistinguishedName;
use rustls::server::danger::{ClientCertVerified, ClientCertVerifier};
use rustls::{CertificateError, DigitallySignedStruct, Error as TlsError, SignatureScheme};
use serde_json::{json, Value};
use tokio::runtime::Runtime;
use tokio::sync::mpsc;

use crate::error::{Fail, RcResult, RcStatus};
use crate::sync::{hex, DEVICE_KEY_FILE};
use crate::synctransport::{
    authorized_set, bad_arg, encode_frame, io_fail, not_found, push_event, save_authorized,
    SyncTransport, TransportEvent, MAX_FRAME,
};
use devcert::{cert_pubkey, device_cert_der, device_key_der};

/// The default UDP port the QUIC listener binds — beside the onion
/// port, not on it (14430 is a circuit identifier, not a socket).
pub const SYNC_LAN_PORT: u16 = 14431;
/// Total wall-clock budget for one QUIC `connect` attempt set.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(15);
/// The stream preamble: every stream writer sends this before any
/// frame.  A QUIC stream only exists on the wire once its first
/// bytes are sent — the magic makes an opened stream visible to the
/// peer instantly (so `Incoming` fires at connect time, like the
/// tor/mem link contract), doubles as a wire-version marker, and
/// gives the dialer a deterministic admission signal to await: a
/// refused handshake's CONNECTION_CLOSE kills the magic read fast.
const WIRE_MAGIC: &[u8; 2] = b"A1";

fn install_crypto_provider() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
}

// ---- the device gate ------------------------------------------------------
//
// One policy, both directions: the peer certificate's SPKI Ed25519
// key IS the device id.  Admission requires (a) a `member-<id16>`
// grant in the shared authorized set — the same file tor's
// restricted-discovery gate replays — and (b) consistency with the
// members table when sync.db is readable: a row naming the device id
// must carry THIS key (kills an id16-prefix-collision forgery); no
// row is fine (pairing writes grant+member together, but the grant
// alone is the transport gate tor-side too).

/// rustls wants a std Error for `CertificateError::Other`.
#[derive(Debug)]
struct Denied(String);
impl fmt::Display for Denied {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "{}", self.0)
    }
}
impl std::error::Error for Denied {}

fn denied(msg: impl Into<String>) -> TlsError {
    TlsError::InvalidCertificate(CertificateError::Other(rustls::OtherError(Arc::new(
        Denied(msg.into()),
    ))))
}

/// (device_id, pubkey) consistency against `<dir>/sync.db`: a row for
/// the id must name THIS key.  Missing db/table/row is inconclusive
/// — the nick gate already did its job — never a verdict.
fn member_key_consistent(dir: &Path, device_id: &str, pubkey: &[u8; 32]) -> bool {
    let db = dir.join("sync.db");
    if !db.exists() {
        return true;
    }
    let conn = match rusqlite::Connection::open_with_flags(
        &db,
        rusqlite::OpenFlags::SQLITE_OPEN_READ_ONLY,
    ) {
        Ok(c) => c,
        Err(_) => return true,
    };
    let row: rusqlite::Result<Option<Vec<u8>>> = conn.query_row(
        "SELECT pubkey FROM members WHERE device_id=?1",
        rusqlite::params![device_id],
        |r| r.get(0),
    );
    match row {
        // No such member: the nick grant decides (the tables are
        // written together at pairing; read ordering can lag).
        Ok(Some(pk)) => pk == pubkey.as_slice(),
        _ => true,
    }
}

/// The admission decision, shared by the client and server verifier
/// halves: returns the peer's device id on success.
fn admit_peer(
    authorized: &Mutex<BTreeMap<String, String>>,
    dir: &Path,
    expected: Option<&str>,
    cert: &CertificateDer<'_>,
) -> Result<String, TlsError> {
    let pk = cert_pubkey(cert.as_ref())
        .ok_or_else(|| denied("peer cert is not an arktos Ed25519 device cert"))?;
    let id = hex(&pk);
    if let Some(want) = expected {
        if !id.starts_with(want) {
            return Err(denied(format!(
                "peer device {id} is not the expected {want}"
            )));
        }
    }
    let id16 = &id[..16.min(id.len())];
    let nick = format!("member-{id16}");
    let granted = authorized
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .contains_key(&nick);
    if !granted {
        return Err(denied(format!(
            "peer device {id16} has no member grant — not a paired device"
        )));
    }
    if !member_key_consistent(dir, &id, &pk) {
        return Err(denied(format!(
            "peer device {id16} key does not match the members table"
        )));
    }
    Ok(id)
}

fn verify_ed25519_sig(
    message: &[u8],
    cert: &CertificateDer<'_>,
    dss: &DigitallySignedStruct,
) -> Result<HandshakeSignatureValid, TlsError> {
    if dss.scheme != SignatureScheme::ED25519 {
        return Err(denied("peer signed with a non-Ed25519 scheme"));
    }
    let pk = cert_pubkey(cert.as_ref())
        .ok_or_else(|| denied("peer cert is not an arktos Ed25519 device cert"))?;
    ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, pk)
        .verify(message, dss.signature())
        .map_err(|_| denied("bad handshake signature"))?;
    Ok(HandshakeSignatureValid::assertion())
}

/// Server half: the connecting device's cert must name a paired
/// member of this profile.
#[derive(Debug)]
struct ServerGate {
    dir: PathBuf,
    authorized: Arc<Mutex<BTreeMap<String, String>>>,
    hints: Vec<DistinguishedName>,
}

impl ClientCertVerifier for ServerGate {
    fn offer_client_auth(&self) -> bool {
        true
    }
    fn client_auth_mandatory(&self) -> bool {
        true
    }
    fn root_hint_subjects(&self) -> &[DistinguishedName] {
        &self.hints
    }
    fn verify_client_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _now: UnixTime,
    ) -> Result<ClientCertVerified, TlsError> {
        admit_peer(&self.authorized, &self.dir, None, end_entity)?;
        Ok(ClientCertVerified::assertion())
    }
    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, TlsError> {
        Err(denied("TLS 1.2 is not spoken on the sync LAN"))
    }
    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, TlsError> {
        verify_ed25519_sig(message, cert, dss)
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![SignatureScheme::ED25519]
    }
}

/// Client half: the server cert must be a paired member, and — when
/// the dial named a device — exactly that device.
#[derive(Debug)]
struct ClientGate {
    dir: PathBuf,
    authorized: Arc<Mutex<BTreeMap<String, String>>>,
    expected: Option<String>,
}

impl ServerCertVerifier for ClientGate {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        _intermediates: &[CertificateDer<'_>],
        _server_name: &ServerName<'_>,
        _ocsp_response: &[u8],
        _now: UnixTime,
    ) -> Result<ServerCertVerified, TlsError> {
        admit_peer(
            &self.authorized,
            &self.dir,
            self.expected.as_deref(),
            end_entity,
        )?;
        Ok(ServerCertVerified::assertion())
    }
    fn verify_tls12_signature(
        &self,
        _message: &[u8],
        _cert: &CertificateDer<'_>,
        _dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, TlsError> {
        Err(denied("TLS 1.2 is not spoken on the sync LAN"))
    }
    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, TlsError> {
        verify_ed25519_sig(message, cert, dss)
    }
    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        vec![SignatureScheme::ED25519]
    }
}

// ---- the transport -----------------------------------------------------------

/// Tunables so tests can run off the standard ports/groups.
#[derive(Clone)]
pub struct LanConfig {
    /// UDP port for the QUIC listener (0 = ephemeral).
    pub quic_port: u16,
    /// Multicast group + port for mDNS (standard 224.0.0.251:5353).
    pub mdns_group: Ipv4Addr,
    pub mdns_port: u16,
    /// Interface for the multicast membership (UNSPECIFIED = any).
    pub mdns_iface: Ipv4Addr,
    /// Extra IPv4s to advertise besides the primary outbound one.
    pub extra_addrs: Vec<Ipv4Addr>,
    /// Onion endpoint for the TXT `on=` record; `None` lets the
    /// transport derive it from the tor keystore when that feature
    /// is built, `Some("")` forces none.
    pub onion: Option<String>,
    /// mDNS re-query period.
    pub query_every: Duration,
}

impl Default for LanConfig {
    fn default() -> Self {
        LanConfig {
            quic_port: SYNC_LAN_PORT,
            mdns_group: mdns::MDNS_GROUP,
            mdns_port: mdns::MDNS_PORT,
            mdns_iface: Ipv4Addr::UNSPECIFIED,
            extra_addrs: Vec::new(),
            onion: None,
            query_every: Duration::from_secs(30),
        }
    }
}

/// One live link: the frame queue feeding its writer pump plus, for
/// client-dialed links, the owning QUIC connection — dropping the
/// last Connection handle closes the connection, so the link entry
/// must hold it for the link's lifetime.  Server-accepted links hold
/// None: their conn lives inside the accept pump's accept_bi loop.
struct LinkEntry {
    tx: mpsc::UnboundedSender<Vec<u8>>,
    _conn: Option<quinn::Connection>,
}

/// link id -> the link entry.
type Links = HashMap<u64, LinkEntry>;

struct Live {
    endpoint: quinn::Endpoint,
    mdns: Option<Arc<mdns::MdnsService>>,
}

pub struct LanTransport {
    /// `<data root>/sync` — the grant file and sync.db live here.
    dir: PathBuf,
    device_id: String,
    /// The QUIC port we bound (the mDNS SRV advertises it).
    port: u16,
    endpoint_str: String,
    rt: Mutex<Option<Runtime>>,
    handle: tokio::runtime::Handle,
    authorized: Arc<Mutex<BTreeMap<String, String>>>,
    links: Arc<Mutex<Links>>,
    events: Arc<Mutex<VecDeque<TransportEvent>>>,
    live: Mutex<Option<Live>>,
    next_link: AtomicU64,
}

/// Read the device PKCS#8 — `load_or_create_identity` guarantees the
/// file exists and is valid, and rustls wants the raw document.
fn device_pkcs8(dir: &Path) -> RcResult<Vec<u8>> {
    let pair_dir = dir.to_path_buf();
    let (_pair, _pk) = crate::sync::load_or_create_identity(&pair_dir)?;
    std::fs::read(pair_dir.join(DEVICE_KEY_FILE)).map_err(|e| io_fail("read device.key", e))
}

fn rustls_tls13() -> rustls::crypto::CryptoProvider {
    rustls::crypto::ring::default_provider()
}

fn server_tls_config(
    dir: &Path,
    authorized: &Arc<Mutex<BTreeMap<String, String>>>,
    cert: &CertificateDer<'static>,
    key_der: rustls::pki_types::PrivateKeyDer<'static>,
) -> RcResult<rustls::ServerConfig> {
    let cfg = rustls::ServerConfig::builder_with_provider(Arc::new(rustls_tls13()))
        .with_protocol_versions(&[&rustls::version::TLS13])
        .map_err(|e| io_fail("tls13 config", e))?
        .with_client_cert_verifier(Arc::new(ServerGate {
            dir: dir.to_path_buf(),
            authorized: Arc::clone(authorized),
            hints: Vec::new(),
        }))
        .with_single_cert(vec![cert.clone()], key_der)
        .map_err(|e| io_fail("server cert", e))?;
    Ok(cfg)
}

fn client_tls_config(
    dir: &Path,
    authorized: &Arc<Mutex<BTreeMap<String, String>>>,
    cert: &CertificateDer<'static>,
    pkcs8: &[u8],
    expected: Option<String>,
) -> RcResult<rustls::ClientConfig> {
    let key = device_key_der(pkcs8);
    let cfg = rustls::ClientConfig::builder_with_provider(Arc::new(rustls_tls13()))
        .with_protocol_versions(&[&rustls::version::TLS13])
        .map_err(|e| io_fail("tls13 config", e))?
        .dangerous()
        .with_custom_certificate_verifier(Arc::new(ClientGate {
            dir: dir.to_path_buf(),
            authorized: Arc::clone(authorized),
            expected,
        }))
        .with_client_auth_cert(vec![cert.clone()], key)
        .map_err(|e| io_fail("client cert", e))?;
    Ok(cfg)
}

/// What a connect target resolves to.
enum Dial {
    /// Discovered device: candidate addrs + mandatory exact id.
    Peer(Vec<SocketAddr>, String),
    /// Direct host:port — the member gate still applies; `expected`
    /// carries an optional id16 the caller pinned.
    Direct(Vec<SocketAddr>, Option<String>),
}

impl LanTransport {
    /// Start the QUIC listener + mDNS discovery under `dir`
    /// (`<data root>/sync`) on the standard ports.
    pub fn start(dir: &Path) -> RcResult<Arc<Self>> {
        Self::start_with(dir, LanConfig::default())
    }

    /// The configurable variant — tests run on ephemeral ports and a
    /// scoped multicast group; the transport semantics are identical.
    pub fn start_with(dir: &Path, cfg: LanConfig) -> RcResult<Arc<Self>> {
        install_crypto_provider();
        std::fs::create_dir_all(dir).map_err(|e| io_fail("create sync dir", e))?;
        let pkcs8 = device_pkcs8(dir)?;
        let pair = ring::signature::Ed25519KeyPair::from_pkcs8(&pkcs8)
            .map_err(|e| Fail {
                status: RcStatus::Crypto,
                msg: format!("device.key reload: {e}"),
            })?;
        let mut pubbuf = [0u8; 32];
        pubbuf.copy_from_slice(ring::signature::KeyPair::public_key(&pair).as_ref());
        let device_id = hex(&pubbuf);
        let id16 = &device_id[..16];

        let authorized = Arc::new(Mutex::new(authorized_set(dir)));
        let cert = device_cert_der(&pair, &format!("arktos-{id16}"));

        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(|e| io_fail("tokio runtime", e))?;
        let handle = rt.handle().clone();

        // The server half: TLS 1.3 + mandatory client cert through
        // the member gate.  The client config is per-connect (the
        // expected-device check is dial-specific), built lazily.
        let srv = server_tls_config(
            dir,
            &authorized,
            &cert,
            device_key_der(&pkcs8),
        )?;
        let quic_srv = quinn::crypto::rustls::QuicServerConfig::try_from(Arc::new(srv))
            .map_err(|e| io_fail("quic server crypto", e))?;
        let server_config = quinn::ServerConfig::with_crypto(Arc::new(quic_srv));
        // Endpoint::server resolves the TokioRuntime via the current
        // handle — it must run inside the runtime's context.
        let ep = handle
            .block_on(async move {
                quinn::Endpoint::server(
                    server_config,
                    SocketAddr::from((Ipv4Addr::UNSPECIFIED, cfg.quic_port)),
                )
            })
            .map_err(|e| io_fail("bind quic endpoint", e))?;
        let port = ep
            .local_addr()
            .map(|a| a.port())
            .unwrap_or(cfg.quic_port);

        let t = Arc::new(LanTransport {
            dir: dir.to_path_buf(),
            device_id: device_id.clone(),
            port,
            endpoint_str: format!("lan://{device_id}"),
            rt: Mutex::new(None),
            handle,
            authorized,
            links: Arc::new(Mutex::new(HashMap::new())),
            events: Arc::new(Mutex::new(VecDeque::new())),
            live: Mutex::new(None),
            next_link: AtomicU64::new(1),
        });
        // The runtime must outlive the transport but is owned by it —
        // parked here, dropped+shutdown on stop().
        *t.rt.lock().unwrap_or_else(|e| e.into_inner()) = Some(rt);

        // mDNS: announce our SRV/TXT/A and browse for peers.  A
        // discovery failure does not sink the transport — LAN links
        // still work by direct dial, only co-location learning is
        // lost (recorded on the transport for status honesty).
        let addrs = mdns::local_addrs(&cfg.extra_addrs);
        let onion = match &cfg.onion {
            Some(o) => o.clone(),
            None => Self::derive_onion(dir),
        };
        let mut txt = vec![
            b"v=1".to_vec(),
            format!("dev={device_id}").into_bytes(),
        ];
        if !onion.is_empty() {
            txt.push(format!("on={onion}").into_bytes());
        }
        let mdns = match mdns::MdnsService::start(mdns::MdnsConfig {
            instance: format!("{id16}.{}", mdns::SERVICE_TYPE),
            host: format!("arktos-{id16}.local"),
            port,
            txt,
            addrs,
            group: cfg.mdns_group,
            mport: cfg.mdns_port,
            iface: cfg.mdns_iface,
            query_every: cfg.query_every,
        }) {
            Ok(m) => Some(m),
            Err(e) => {
                // Discovery loss must stay non-fatal, but silent was
                // the bug: peers:[] + live leg reads as "nothing on
                // the LAN" when the announce half actually died.
                eprintln!("arktos-sync: mdns discovery failed to start: {e}");
                None
            }
        };

        *t.live.lock().unwrap_or_else(|e| e.into_inner()) = Some(Live {
            endpoint: ep,
            mdns,
        });
        t.spawn_accept();
        push_event(&t.events, TransportEvent::Ready);
        Ok(t)
    }

    /// Our onion endpoint for the TXT record, derived keystore-only
    /// when the tor feature is built.  "" when unavailable — the TXT
    /// simply omits `on=` then.
    #[cfg(feature = "synctor")]
    fn derive_onion(dir: &Path) -> String {
        super::tor::endpoint_for(dir).unwrap_or_default()
    }
    #[cfg(not(feature = "synctor"))]
    fn derive_onion(_dir: &Path) -> String {
        String::new()
    }

    /// The accept pump: each QUIC connection's bidi streams each
    /// become a link.  Peer device id comes from the negotiated
    /// client cert for the `from` label.
    fn spawn_accept(self: &Arc<Self>) {
        let Some(ep) = self
            .live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map(|l| l.endpoint.clone())
        else {
            return;
        };
        let t = Arc::clone(self);
        self.handle.spawn(async move {
            loop {
                let Some(incoming) = ep.accept().await else { break };
                let t = Arc::clone(&t);
                let h = t.handle.clone();
                h.spawn(async move {
                    let conn = match incoming.await {
                        Ok(c) => c,
                        Err(_) => return, // gate refused or handshake died — silent by design
                    };
                    let from = peer_label(&conn);
                    loop {
                        match conn.accept_bi().await {
                            Ok((send, recv)) => {
                                let id = t.register_link(send, recv);
                                push_event(&t.events, TransportEvent::Incoming {
                                    link: id,
                                    from: from.clone(),
                                });
                            }
                            Err(_) => break,
                        }
                    }
                });
            }
        });
    }

    /// Wire one stream into the link table — the same shape as the
    /// tor backend: unbounded queue -> writer pump; reader pump ->
    /// Frame/LinkClosed events.  Accepted links send their magic
    /// preamble immediately (that's the dialer's admission signal)
    /// and validate the peer's before framing.
    fn register_link(&self, send: quinn::SendStream, recv: quinn::RecvStream) -> u64 {
        let id = self.next_link.fetch_add(1, Ordering::SeqCst);
        let (tx, rx) = mpsc::unbounded_channel::<Vec<u8>>();
        self.links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(id, LinkEntry { tx, _conn: None });
        spawn_writer(send, rx, true, &self.handle);
        spawn_reader(recv, true, id, &self.links, &self.events, &self.handle);
        id
    }

    /// Resolve a `connect` endpoint string into dial candidates.
    fn resolve(&self, endpoint: &str, as_nickname: &str) -> RcResult<Dial> {
        let body = endpoint.strip_prefix("lan://").unwrap_or(endpoint);
        // Discovered-device dial: a bare 64-hex (or id16) token.
        let looks_like_id = !body.contains(':')
            && body.chars().all(|c| c.is_ascii_hexdigit())
            && (body.len() == 64 || body.len() == 16);
        if looks_like_id {
            let peer = self
                .discovered(body)
                .ok_or_else(|| Fail {
                    status: RcStatus::NotFound,
                    msg: format!("device {}… is not discovered on the LAN", &body[..8.min(body.len())]),
                })?;
            let addrs: Vec<SocketAddr> = peer
                .addrs
                .iter()
                .map(|a| SocketAddr::from((*a, peer.port)))
                .collect();
            if addrs.is_empty() {
                return Err(io_fail("dial", format!("peer {body} has no LAN address")));
            }
            return Ok(Dial::Peer(addrs, body.to_string()));
        }
        // Direct host:port (bracketed v6 refused — LAN is v4 only).
        let (host, port) = body
            .rsplit_once(':')
            .and_then(|(h, p)| p.parse::<u16>().ok().map(|p| (h, p)))
            .ok_or_else(|| {
                bad_arg::<()>(format!("lan endpoint \"{endpoint}\" — want lan://<id>, lan://<host>:<port>, or <host>:<port>"))
                    .unwrap_err()
            })?;
        let mut addrs = Vec::new();
        for a in std::net::ToSocketAddrs::to_socket_addrs(&(host, port))
            .map_err(|e| io_fail("resolve lan endpoint", e))?
        {
            addrs.push(a);
        }
        if addrs.is_empty() {
            return Err(io_fail("resolve lan endpoint", "no addresses"));
        }
        // An optional "member-<id16>" nickname pins the expected peer.
        let expected = as_nickname
            .strip_prefix("member-")
            .map(str::to_string);
        Ok(Dial::Direct(addrs, expected))
    }

    /// The discovery table lookup (live transport only).
    fn discovered(&self, device_id: &str) -> Option<mdns::PeerRecord> {
        self.live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .and_then(|l| l.mdns.as_ref())
            .and_then(|m| m.find(device_id))
    }

    /// The discovered peers — for the status op and the `auto`
    /// transport's routing decision.
    pub fn discovered_peers(&self) -> Vec<mdns::PeerRecord> {
        self.live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .and_then(|l| l.mdns.as_ref())
            .map(|m| m.peers())
            .unwrap_or_default()
    }

    /// Discovery lookup by advertised onion — `auto`'s tor->lan map.
    pub fn discovered_by_onion(&self, onion: &str) -> Option<mdns::PeerRecord> {
        self.live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .and_then(|l| l.mdns.as_ref())
            .and_then(|m| m.find_by_onion(onion))
    }

    /// The shared authorized grant set (the `auto` backend forwards
    /// grant ops to both legs).
    pub fn authorized_map(&self) -> Arc<Mutex<BTreeMap<String, String>>> {
        Arc::clone(&self.authorized)
    }

    /// Whether mDNS actually came up (status honesty — a lan backend
    /// without discovery still dials directly).
    pub fn discovery_active(&self) -> bool {
        self.live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .and_then(|l| l.mdns.as_ref())
            .is_some()
    }

    /// Our QUIC port (what the SRV record advertised).
    pub fn quic_port(&self) -> u16 {
        self.port
    }
}

/// The `from` label for an inbound link: the peer's device id from
/// its negotiated cert plus its source address — what a UI shows
/// beside "incoming link".
fn peer_label(conn: &quinn::Connection) -> String {
    let id = conn
        .peer_identity()
        .and_then(|any| {
            any.downcast::<Vec<CertificateDer<'static>>>()
                .ok()
                .and_then(|certs| certs.first().and_then(|c| cert_pubkey(c.as_ref())))
        })
        .map(|pk| hex(&pk))
        .unwrap_or_else(|| "unknown".to_string());
    let id16 = &id[..16.min(id.len())];
    format!("lan://{id16}@{}", conn.remote_address().ip())
}

/// The frame reader — identical contract to tor's link pump.
async fn read_loop(
    rd: &mut quinn::RecvStream,
    events: &Mutex<VecDeque<TransportEvent>>,
    link: u64,
) -> String {
    loop {
        let mut hdr = [0u8; 4];
        if let Err(e) = rd.read_exact(&mut hdr).await {
            return format!("peer closed ({e})");
        }
        let len = u32::from_le_bytes(hdr) as usize;
        if len > MAX_FRAME {
            return format!("oversize frame ({len} > {MAX_FRAME})");
        }
        let mut payload = vec![0u8; len];
        if let Err(e) = rd.read_exact(&mut payload).await {
            return format!("truncated frame ({e})");
        }
        push_event(events, TransportEvent::Frame { link, payload });
    }
}

/// The stream preamble check — a peer speaking anything else is a
/// protocol violator and the link dies as `bad magic`.
async fn expect_magic(rd: &mut quinn::RecvStream) -> Result<(), String> {
    let mut m = [0u8; 2];
    rd.read_exact(&mut m)
        .await
        .map_err(|e| format!("magic read: {e}"))?;
    if m != *WIRE_MAGIC {
        return Err(format!("bad magic {:02x?}", m));
    }
    Ok(())
}

/// The writer pump: `send_magic` first (accepted links — that's the
/// dialer's admission signal), then the frame queue.
fn spawn_writer(
    send: quinn::SendStream,
    mut rx: mpsc::UnboundedReceiver<Vec<u8>>,
    send_magic: bool,
    handle: &tokio::runtime::Handle,
) {
    let mut wr = send;
    handle.spawn(async move {
        if send_magic && (wr.write_all(WIRE_MAGIC).await.is_err() || wr.flush().await.is_err()) {
            return;
        }
        while let Some(payload) = rx.recv().await {
            let frame = encode_frame(&payload);
            if wr.write_all(&frame).await.is_err() || wr.flush().await.is_err() {
                break;
            }
        }
        let _ = wr.finish();
    });
}

/// The reader pump: optionally validate the peer's magic, run the
/// frame loop, then retire the link with a LinkClosed event.
fn spawn_reader(
    mut recv: quinn::RecvStream,
    validate_magic: bool,
    id: u64,
    links: &Arc<Mutex<Links>>,
    events: &Arc<Mutex<VecDeque<TransportEvent>>>,
    handle: &tokio::runtime::Handle,
) {
    let links = Arc::clone(links);
    let events = Arc::clone(events);
    handle.spawn(async move {
        let reason = match if validate_magic {
            expect_magic(&mut recv).await
        } else {
            Ok(())
        } {
            Err(r) => r,
            Ok(()) => read_loop(&mut recv, &events, id).await,
        };
        links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&id);
        push_event(&events, TransportEvent::LinkClosed { link: id, reason });
    });
}

impl SyncTransport for LanTransport {
    fn backend(&self) -> &'static str {
        "lan"
    }

    fn endpoint(&self) -> RcResult<String> {
        Ok(self.endpoint_str.clone())
    }

    fn authorize(&self, nickname: &str, discovery_key: &str) -> RcResult<()> {
        // The grant file is shared with tor — the lan gate reads the
        // nickname (`member-<id16>`), the value is tor's key
        // material, stored verbatim for that backend's use.
        if nickname.is_empty() {
            return bad_arg("empty grant nickname");
        }
        self.authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(nickname.to_string(), discovery_key.to_string());
        save_authorized(
            &self.dir,
            &self.authorized.lock().unwrap_or_else(|e| e.into_inner()),
        )
    }

    fn deauthorize(&self, nickname: &str) -> RcResult<bool> {
        let removed = self
            .authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(nickname)
            .is_some();
        if removed {
            save_authorized(
                &self.dir,
                &self.authorized.lock().unwrap_or_else(|e| e.into_inner()),
            )?;
        }
        Ok(removed)
    }

    fn authorized(&self) -> Vec<String> {
        self.authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .keys()
            .cloned()
            .collect()
    }

    fn discovery_key(&self, peer_onion: &str) -> RcResult<String> {
        // Discovery keys are tor-keystore material regardless of
        // which transport is running — delegate so pairing key ops
        // work identically on a LAN-only box.
        #[cfg(feature = "synctor")]
        {
            super::tor::discovery_key(&self.dir, peer_onion)
        }
        #[cfg(not(feature = "synctor"))]
        {
            let _ = peer_onion;
            fail(
                RcStatus::Unavailable,
                "rustcore built without the synctor feature — discovery keys unavailable",
            )
        }
    }

    fn import_discovery_key(&self, peer_onion: &str, secret: &[u8; 32]) -> RcResult<()> {
        #[cfg(feature = "synctor")]
        {
            super::tor::import_discovery_key(&self.dir, peer_onion, secret)
        }
        #[cfg(not(feature = "synctor"))]
        {
            let _ = (peer_onion, secret);
            fail(
                RcStatus::Unavailable,
                "rustcore built without the synctor feature — discovery keys unavailable",
            )
        }
    }

    fn rotate_discovery_key(&self, peer_onion: &str) -> RcResult<String> {
        #[cfg(feature = "synctor")]
        {
            super::tor::rotate_discovery_key(&self.dir, peer_onion)
        }
        #[cfg(not(feature = "synctor"))]
        {
            let _ = peer_onion;
            fail(
                RcStatus::Unavailable,
                "rustcore built without the synctor feature — discovery keys unavailable",
            )
        }
    }

    fn connect(&self, endpoint: &str, as_nickname: &str) -> RcResult<u64> {
        let dial = self.resolve(endpoint, as_nickname)?;
        let (addrs, expected) = match dial {
            Dial::Peer(a, id) => (a, Some(id)),
            Dial::Direct(a, e) => (a, e),
        };
        let ep = self
            .live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .as_ref()
            .map(|l| l.endpoint.clone())
            .ok_or_else(|| Fail {
                status: RcStatus::NotEnabled,
                msg: "lan transport not started".into(),
            })?;
        let pkcs8 = device_pkcs8(&self.dir)?;
        let pair = ring::signature::Ed25519KeyPair::from_pkcs8(&pkcs8).map_err(|e| Fail {
            status: RcStatus::Crypto,
            msg: format!("device.key reload: {e}"),
        })?;
        let cert = device_cert_der(&pair, &format!("arktos-{}", &self.device_id[..16]));
        let tls = client_tls_config(&self.dir, &self.authorized, &cert, &pkcs8, expected)?;
        let quic_cli = quinn::crypto::rustls::QuicClientConfig::try_from(Arc::new(tls))
            .map_err(|e| io_fail("quic client crypto", e))?;
        let client_config = quinn::ClientConfig::new(Arc::new(quic_cli));

        let links = Arc::clone(&self.links);
        let events = Arc::clone(&self.events);
        // Try each candidate address once; the QUIC handshake itself
        // is the auth gate — a refused device fails here, pre-app.
        self.handle.block_on(async move {
            let mut last_err: Option<Fail> = None;
            for addr in addrs {
                let connecting = match ep.connect_with(
                    client_config.clone(),
                    addr,
                    "arktos.local",
                ) {
                    Ok(c) => c,
                    Err(e) => {
                        last_err = Some(io_fail("quic connect", e));
                        continue;
                    }
                };
                match tokio::time::timeout(CONNECT_TIMEOUT, connecting).await {
                    Ok(Ok(conn)) => match conn.open_bi().await {
                        Ok((mut send, mut recv)) => {
                            // Our magic goes out first (it's what
                            // surfaces the stream to the peer's
                            // accept_bi); theirs is the admission
                            // signal — a refused handshake's
                            // CONNECTION_CLOSE fails the read fast.
                            if send.write_all(WIRE_MAGIC).await.is_err()
                                || send.flush().await.is_err()
                            {
                                last_err = Some(io_fail("stream preamble", "write failed"));
                                continue;
                            }
                            match expect_magic(&mut recv).await {
                                Ok(()) => {
                                    let id = self.next_link.fetch_add(1, Ordering::SeqCst);
                                    return Ok(register_link_with(
                                        id, send, recv, conn, &links, &events,
                                        &self.handle,
                                    ));
                                }
                                Err(r) => last_err = Some(io_fail("peer admission", r)),
                            }
                        }
                        Err(e) => last_err = Some(io_fail("open stream", e)),
                    },
                    Ok(Err(e)) => {
                        let msg = format!("{e}");
                        let status = if msg.contains("member grant")
                            || msg.contains("device cert")
                            || msg.contains("members table")
                        {
                            RcStatus::Crypto
                        } else {
                            RcStatus::Io
                        };
                        last_err = Some(Fail {
                            status,
                            msg: format!("peer {addr} refused: {msg}"),
                        });
                    }
                    Err(_) => last_err = Some(io_fail("quic connect", "timeout")),
                }
            }
            Err(last_err.unwrap_or_else(|| io_fail("quic connect", "no addresses")))
        })
    }

    fn send(&self, link: u64, frame: &[u8]) -> RcResult<()> {
        let links = self.links.lock().unwrap_or_else(|e| e.into_inner());
        let Some(entry) = links.get(&link) else {
            return not_found(format!("no link {link}"));
        };
        entry.tx.send(frame.to_vec()).map_err(|e| io_fail("send", e))
    }

    fn close_link(&self, link: u64) -> RcResult<bool> {
        Ok(self
            .links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .remove(&link)
            .is_some())
    }

    fn poll(&self) -> Vec<TransportEvent> {
        crate::synctransport::drain_events(&self.events)
    }

    fn link_count(&self) -> usize {
        self.links.lock().unwrap_or_else(|e| e.into_inner()).len()
    }

    fn discovered_peers_json(&self) -> Vec<Value> {
        self.discovered_peers()
            .iter()
            .map(|p| {
                json!({
                    "device_id": p.device_id,
                    "onion": p.onion,
                    "port": p.port,
                    "addrs": p.addrs.iter().map(Ipv4Addr::to_string).collect::<Vec<_>>(),
                    "age_ms": p.last_seen.elapsed().as_millis() as u64,
                })
            })
            .collect()
    }

    fn stop(&self) {
        let live = self
            .live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .take();
        if let Some(l) = live {
            l.endpoint.close(quinn::VarInt::from_u32(0), b"shutdown");
            if let Some(m) = l.mdns {
                m.stop();
            }
        }
        self.links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clear();
        let rt = self.rt.lock().unwrap_or_else(|e| e.into_inner()).take();
        if let Some(rt) = rt {
            rt.shutdown_timeout(Duration::from_secs(3));
        }
    }
}

impl Drop for LanTransport {
    fn drop(&mut self) {
        self.stop();
    }
}

/// Outbound-link registration (the server half's helper is a method;
/// the connect path registers without self's async context).  The
/// caller already wrote our magic on `send` and consumed the peer's
/// on `recv` — both pumps run straight into framing.
fn register_link_with(
    id: u64,
    send: quinn::SendStream,
    recv: quinn::RecvStream,
    conn: quinn::Connection,
    links: &Arc<Mutex<Links>>,
    events: &Arc<Mutex<VecDeque<TransportEvent>>>,
    handle: &tokio::runtime::Handle,
) -> u64 {
    let (tx, rx) = mpsc::unbounded_channel::<Vec<u8>>();
    links
        .lock()
        .unwrap_or_else(|e| e.into_inner())
        .insert(
            id,
            LinkEntry {
                tx,
                _conn: Some(conn),
            },
        );
    spawn_writer(send, rx, false, handle);
    spawn_reader(recv, false, id, links, events, handle);
    id
}

// ---- tests -------------------------------------------------------------------
//
// The e2e legs run REAL mDNS multicast + QUIC on the host's LAN
// interface — the same wire path production uses, on a scoped test
// group and ephemeral ports.  A box with no multicast-capable iface
// honestly skips the discovery legs (join fails -> skip note); the
// direct-dial legs need only a loopback UDP socket and always run.

#[cfg(test)]
mod tests {
    use super::mdns::PeerRecord;
    use super::*;
    use std::thread::sleep;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-lan-{}-{}-{}",
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

    fn test_group(n: u8) -> Ipv4Addr {
        Ipv4Addr::new(239, 255, 77, n)
    }

    fn cfg(group_n: u8, mport: u16) -> LanConfig {
        LanConfig {
            quic_port: 0,
            mdns_group: test_group(group_n),
            mdns_port: mport,
            mdns_iface: Ipv4Addr::UNSPECIFIED,
            extra_addrs: Vec::new(),
            onion: Some(format!("peer{group_n}.onion:14430")),
            query_every: Duration::from_millis(150),
        }
    }

    /// No-multicast variant: mdns bind is skipped entirely when the
    /// env can't join the group — direct dial still works.
    fn cfg_direct() -> LanConfig {
        LanConfig {
            quic_port: 0,
            mdns_group: Ipv4Addr::new(224, 0, 0, 1), // unroutable test group
            mdns_port: 15999,
            mdns_iface: Ipv4Addr::LOCALHOST,
            extra_addrs: vec![Ipv4Addr::LOCALHOST],
            onion: Some("direct.onion:14430".into()),
            query_every: Duration::from_secs(3600),
        }
    }

    fn member_nick(device_id: &str) -> String {
        format!("member-{}", &device_id[..16.min(device_id.len())])
    }

    /// Start a transport whose sync dir already holds an engine
    /// identity; returns (transport, device_id).
    fn start(data_root: &Path, cfg: LanConfig) -> Arc<LanTransport> {
        LanTransport::start_with(&data_root.join("sync"), cfg).unwrap()
    }

    fn engine(root: &Path) -> crate::sync::SyncEngine {
        crate::sync::SyncEngine::open(root.to_str().unwrap()).unwrap()
    }

    /// Grant `peer` access on `t` — the nickname gate plus the
    /// members-table key pin, the way pairing commit writes both.
    fn pair_into(t: &LanTransport, t_root: &Path, peer: &crate::sync::SyncEngine, peer_id: &str) {
        t.authorize(&member_nick(peer_id), "descriptor:x25519:test").unwrap();
        engine(t_root)
            .member_add(&peer.public_key(), "peer.onion", "")
            .unwrap();
    }

    fn wait_frame(t: &Arc<LanTransport>, link: u64, tries: u32) -> Vec<u8> {
        for _ in 0..tries {
            for ev in t.poll() {
                if let TransportEvent::Frame { link: l, payload } = ev {
                    if l == link {
                        return payload;
                    }
                }
            }
            sleep(Duration::from_millis(10));
        }
        panic!("no frame on link {link}");
    }

    fn wait_incoming(t: &Arc<LanTransport>, tries: u32) -> u64 {
        for _ in 0..tries {
            for ev in t.poll() {
                if let TransportEvent::Incoming { link, .. } = ev {
                    return link;
                }
            }
            sleep(Duration::from_millis(15));
        }
        panic!("no Incoming link");
    }

    /// A multicast-capable check: if the group join fails the env
    /// can't do LAN discovery at all — the leg skips honestly.
    fn multicast_ok() -> bool {
        mdns::bind_socket(test_group(240), 15400, Ipv4Addr::UNSPECIFIED).is_ok()
    }

    /// The full verify leg: two paired devices discover each other
    /// over real multicast, connect `lan://<id>`, exchange frames,
    /// close propagates.  Tor plays no part — LAN-only sync.
    #[test]
    fn lan_discovery_connect_ping_pong() {
        if !multicast_ok() {
            eprintln!("SKIP lan_discovery_connect_ping_pong: no multicast iface");
            return;
        }
        let (ra, rb) = (tmpdir("e2ea"), tmpdir("e2eb"));
        let (ea, eb) = (engine(&ra), engine(&rb));
        let (ida, idb) = (ea.device_id(), eb.device_id());

        let ta = start(&ra, cfg(201, 15501));
        let tb = start(&rb, cfg(201, 15501));
        pair_into(&ta, &ra, &eb, &idb);
        pair_into(&tb, &rb, &ea, &ida);

        // Real discovery: B's mDNS table learns A's device id + onion.
        let mut found: Option<PeerRecord> = None;
        for _ in 0..240 {
            if let Some(p) = tb
                .live
                .lock()
                .unwrap()
                .as_ref()
                .and_then(|l| l.mdns.clone())
                .and_then(|m| m.find(&ida))
            {
                found = Some(p);
                break;
            }
            sleep(Duration::from_millis(50));
        }
        let p = found.expect("B never discovered A over mDNS");
        assert_eq!(p.port, ta.quic_port());
        assert_eq!(p.onion, "peer201.onion:14430");
        assert!(!p.addrs.is_empty());

        // Dial by discovered device id — the QUIC handshake carries
        // the device-cert mTLS and the member gate admits.
        let our = tb.connect(&format!("lan://{ida}"), "self").unwrap();
        let theirs = wait_incoming(&ta, 200);
        tb.send(our, b"PING").unwrap();
        assert_eq!(wait_frame(&ta, theirs, 200), b"PING");
        ta.send(theirs, b"PONG").unwrap();
        assert_eq!(wait_frame(&tb, our, 200), b"PONG");

        // Close propagates as LinkClosed on the listener side.
        tb.close_link(our).unwrap();
        let mut saw = false;
        for _ in 0..200 {
            for ev in ta.poll() {
                if let TransportEvent::LinkClosed { link, .. } = ev {
                    saw |= link == theirs;
                }
            }
            if saw {
                break;
            }
            sleep(Duration::from_millis(15));
        }
        assert!(saw, "peer close never reached A");
        ta.stop();
        tb.stop();
    }

    /// The auth gate: an UNPAIRED device — valid cert, no member
    /// grant — is refused during the handshake; the listener never
    /// sees an Incoming link or a frame.  The LAN equivalent of
    /// tor's restricted-discovery refusal.
    #[test]
    fn unpaired_device_refused_pre_app() {
        let (ra, rm) = (tmpdir("ga"), tmpdir("gm"));
        let (ea, em) = (engine(&ra), engine(&rm));
        let (ida, _idm) = (ea.device_id(), em.device_id());
        let rc = tmpdir("gs");
        let ec = engine(&rc);
        let idc = ec.device_id();
        let ta = start(&ra, cfg_direct());
        let tm = start(&rm, cfg_direct());
        // Mallory fully trusts A on HIS side (grant + member row),
        // so his client verifier passes — the refusal that fires is
        // purely A's SERVER gate: M has no member grant there.
        pair_into(&tm, &rm, &ea, &ida);
        // A is paired with SOMEONE ELSE (not M) — a live member gate
        // exists, it just does not cover Mallory.
        pair_into(&ta, &ra, &ec, &idc);
        let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, ta.quic_port()));
        let r = tm.connect(&format!("lan://{addr}"), "self");
        assert!(r.is_err(), "unpaired device completed the handshake");
        // No link materialized on either side.
        assert_eq!(ta.link_count(), 0);
        assert_eq!(tm.link_count(), 0);
        for ev in ta.poll() {
            assert!(
                !matches!(ev, TransportEvent::Incoming { .. } | TransportEvent::Frame { .. }),
                "unpaired device reached the app layer"
            );
        }
        ta.stop();
        tm.stop();
    }

    /// The members-table pin: a grant alone is not enough — a device
    /// id colliding on the grant's id16 prefix but carrying a
    /// DIFFERENT full key than sync.db recorded is refused.  This is
    /// strictly stronger than tor's grant-only gate.
    #[test]
    fn members_table_pins_the_full_key() {
        let (ra, rm) = (tmpdir("pa"), tmpdir("pm"));
        let (ea, em) = (engine(&ra), engine(&rm));
        let (_ida, idm) = (ea.device_id(), em.device_id());
        let ta = start(&ra, cfg_direct());
        let tm = start(&rm, cfg_direct());
        // Grant exists for the device id16…
        ta.authorize(&member_nick(&idm), "descriptor:x25519:test").unwrap();
        // …but sync.db recorded a DIFFERENT pubkey for that id —
        // the member gate must refuse the forged cert.
        engine(&ra)
            .member_add(&[0xAAu8; 32], "forged.onion", "")
            .unwrap();
        // member_add indexes by its own id; force the collision:
        // write the row under the real device_id with a wrong key.
        {
            let db = ra.join("sync").join("sync.db");
            let c = rusqlite::Connection::open(&db).unwrap();
            c.execute(
                "INSERT OR REPLACE INTO members(device_id,pubkey,onion_addr,last_seen,added)
                 VALUES(?1,?2,'x',0,0)",
                rusqlite::params![idm, vec![0xAAu8; 32]],
            )
            .unwrap();
        }
        let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, ta.quic_port()));
        assert!(tm.connect(&format!("lan://{addr}"), "self").is_err());
        assert_eq!(ta.link_count(), 0);
        ta.stop();
        tm.stop();
    }

    /// Expected-device pinning on the dialer side: dialing
    /// `lan://<idX>` at an address that serves device B must fail —
    /// a co-located impostor can't stand in for the discovered peer.
    #[test]
    fn expected_device_mismatch_refused() {
        let (ra, rb) = (tmpdir("xa"), tmpdir("xb"));
        let (ea, eb) = (engine(&ra), engine(&rb));
        let (ida, idb) = (ea.device_id(), eb.device_id());
        let ta = start(&ra, cfg_direct());
        let tb = start(&rb, cfg_direct());
        pair_into(&ta, &ra, &eb, &idb);
        pair_into(&tb, &rb, &ea, &ida);
        // Claim we're dialing a different device id (all-F).
        let fake_id = "f".repeat(64);
        let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, ta.quic_port()));
        // Force the resolver: direct dial with a member- nick pin.
        assert!(tb
            .connect(&format!("lan://{addr}"), &member_nick(&fake_id))
            .is_err());
        // The genuine id16 pin admits.
        let our = tb
            .connect(&format!("lan://{addr}"), &member_nick(&ida))
            .unwrap();
        let theirs = wait_incoming(&ta, 100);
        tb.send(our, b"OK").unwrap();
        assert_eq!(wait_frame(&ta, theirs, 100), b"OK");
        ta.stop();
        tb.stop();
    }

    /// Undiscovered-id dial is a clean NotFound, not a hang.
    #[test]
    fn undiscovered_device_errors() {
        let ra = tmpdir("ud");
        let ta = start(&ra, cfg_direct());
        let r = ta.connect(&format!("lan://{}", "a".repeat(64)), "self");
        assert!(r.is_err());
        assert_eq!(r.unwrap_err().status, RcStatus::NotFound);
        assert!(ta.connect("lan://[::1]:1", "self").is_err());
        assert!(ta.connect("lan://bad", "self").is_err());
        ta.stop();
    }

    /// Grant churn is honored live: deauthorize cuts new handshakes
    /// (the live link survives, same rule tor enforces).
    #[test]
    fn deauthorize_cuts_future_links() {
        let (ra, rb) = (tmpdir("da"), tmpdir("db"));
        let (ea, eb) = (engine(&ra), engine(&rb));
        let (ida, idb) = (ea.device_id(), eb.device_id());
        let ta = start(&ra, cfg_direct());
        let tb = start(&rb, cfg_direct());
        pair_into(&ta, &ra, &eb, &idb);
        pair_into(&tb, &rb, &ea, &ida);
        let addr = SocketAddr::from((Ipv4Addr::LOCALHOST, ta.quic_port()));
        let our = tb.connect(&format!("lan://{addr}"), "self").unwrap();
        wait_incoming(&ta, 100);
        ta.deauthorize(&member_nick(&idb)).unwrap();
        assert!(tb.connect(&format!("lan://{addr}"), "self").is_err());
        // The existing link still works.
        let their_link = ta
            .links
            .lock()
            .unwrap()
            .keys()
            .next()
            .copied()
            .unwrap();
        tb.send(our, b"STILL").unwrap();
        assert_eq!(wait_frame(&ta, their_link, 100), b"STILL");
        ta.stop();
        tb.stop();
    }

    /// Restart binds a fresh ephemeral port and mints no new
    /// identity — device id is stable, like the onion address.
    #[test]
    fn restart_keeps_identity() {
        let ra = tmpdir("rk");
        let id = engine(&ra).device_id();
        let t1 = start(&ra, cfg_direct());
        assert_eq!(t1.endpoint().unwrap(), format!("lan://{id}"));
        let p1 = t1.quic_port();
        t1.stop();
        let t2 = start(&ra, cfg_direct());
        assert_eq!(t2.endpoint().unwrap(), format!("lan://{id}"));
        assert_ne!(t2.quic_port(), 0);
        let _ = p1;
        t2.stop();
    }

    /// A `lan://host:port` dial through ToSocketAddrs — hostname
    /// resolution path for human-typed endpoints.
    #[test]
    fn direct_host_dial() {
        let (ra, rb) = (tmpdir("ha"), tmpdir("hb"));
        let (ea, eb) = (engine(&ra), engine(&rb));
        let (ida, idb) = (ea.device_id(), eb.device_id());
        let ta = start(&ra, cfg_direct());
        let tb = start(&rb, cfg_direct());
        pair_into(&ta, &ra, &eb, &idb);
        pair_into(&tb, &rb, &ea, &ida);
        let our = tb
            .connect(&format!("127.0.0.1:{}", ta.quic_port()), "self")
            .unwrap();
        let theirs = wait_incoming(&ta, 100);
        tb.send(our, b"HI").unwrap();
        assert_eq!(wait_frame(&ta, theirs, 100), b"HI");
        ta.stop();
        tb.stop();
    }
}
