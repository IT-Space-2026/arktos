//! SYNC02 — the "tor" [`SyncTransport`] backend: embedded Arti.
//!
//! One long-lived tokio runtime (2 workers) owns every async piece —
//! the Arti [`TorClient`], the published onion service, the accept
//! pump, and the per-link read/write pumps.  The trait surface stays
//! synchronous: outbound work is `handle.block_on`ed and inbound
//! work lands on the pull queue the `poll()` op drains.
//!
//! State layout under `<data root>/sync/tor/`:
//!   state/      Arti state dir — the keystore with the service hsid
//!               (the onion address derives from it, so it is stable
//!               across restarts) and the per-peer discovery keypairs.
//!   cache/      Arti cache dir (directory documents).
//!   authorized_clients.json — the persisted {nickname: pubkey}
//!               restricted-discovery grant set, replayed into the
//!               service config at every launch and on every live
//!               reconfigure.
//!
//! Restricted discovery is ALWAYS on — the sync listener exists only
//! for paired peers, so an empty grant set correctly refuses the
//! whole network at the descriptor layer.

use std::collections::{BTreeMap, HashMap, VecDeque};
use std::path::{Path, PathBuf};
use std::str::FromStr;
use std::sync::atomic::Ordering;
use std::sync::{Arc, Mutex, Once};
use std::time::{Duration, Instant};

use arti_client::config::onion_service::OnionServiceConfigBuilder;
use arti_client::config::TorClientConfigBuilder;
use arti_client::{DataStream, TorClient, TorClientConfig};
use futures::{AsyncReadExt, AsyncWriteExt, StreamExt};
use safelog::DisplayRedacted;
use tokio::runtime::Runtime;
use tokio::sync::mpsc;
use tor_cell::relaycell::msg::Connected;
use tor_config::Reconfigure;
use arti_client::ErrorKind;
use tor_hscrypto::pk::{HsClientDescEncKey, HsClientDescEncSecretKey};
use tor_llcrypto::pk::curve25519;
use tor_config_path::CfgPath;
use tor_hsservice::config::restricted_discovery::{
    DirectoryKeyProviderBuilder, HsClientNickname,
};
use tor_hsservice::{
    handle_rend_requests, HsId, HsNickname, OnionServiceConfig, RunningOnionService,
};
use tor_keymgr::KeystoreSelector;
use tor_rtcompat::PreferredRuntime;

use crate::error::{Fail, RcResult, RcStatus};
use crate::synctransport::{
    authorized_set, backoff_delay, bad_arg, encode_frame, io_fail, not_found,
    push_event, save_authorized, SyncTransport, TransportEvent, CONNECT_DEADLINE, MAX_FRAME,
    SYNC_ONION_PORT, TRANSPORT_DIR,
};
use arti_client::HasKind;

/// The onion service's hs nickname inside the Arti keystore — key
/// material is namespaced by it, so it must be a stable constant.
const SVC_NICKNAME: &str = "arksync";
/// How many discovery-key failures still count as "peer refused us"
/// rather than transient flakiness worth the full backoff budget.
/// A missing/incorrect client-auth key cannot heal by retrying.
const AUTH_FAIL_LIMIT: u32 = 2;

struct Live {
    svc: Arc<RunningOnionService>,
}

/// link id -> the frame queue feeding that link's writer pump.
type Links = HashMap<u64, mpsc::UnboundedSender<Vec<u8>>>;

pub struct TorTransport {
    /// `<data root>/sync` — the authorized-clients file lives at the
    /// sync dir level (shared with the engine), Arti state below it.
    dir: PathBuf,
    endpoint: String,
    rt: Mutex<Option<Runtime>>,
    handle: tokio::runtime::Handle,
    /// The client from build time — created unbootstrapped, driven to
    /// readiness by a background task so start() never blocks the mw
    /// caller on directory fetch.  connect() awaits bootstrap on it.
    client: Arc<TorClient<PreferredRuntime>>,
    authorized: Mutex<BTreeMap<String, String>>,
    links: Arc<Mutex<Links>>,
    events: Arc<Mutex<VecDeque<TransportEvent>>>,
    live: Mutex<Option<Live>>,
    next_link: std::sync::atomic::AtomicU64,
}

/// Directory the always-present key_dirs provider points at, under
/// the transport root — grants are static_keys-driven, so the dir
/// exists mainly to satisfy the non-empty-provider rule.
const AUTH_DIR_PLACEHOLDER: &str = "authorized_clients";

/// Build the onion-service config carrying `set` as the static
/// restricted-discovery grant map.  A `key_dirs` provider always
/// rides along — enabled-with-zero-providers is a config error, so
/// the (possibly empty) auth dir keeps the "refuse everyone until
/// paired" posture valid.
fn svc_config_for(
    set: &BTreeMap<String, String>,
    auth_dir: &Path,
) -> RcResult<OnionServiceConfig> {
    let mut b = OnionServiceConfigBuilder::default();
    // A wider intro-point set: a flapping publish reads as
    // DegradedUnreachable at three points — twelve survives the loss.
    b.num_intro_points(12);
    b.nickname(
        HsNickname::from_str(SVC_NICKNAME)
            .map_err(|e| io_fail("service nickname", e))?,
    );
    let rd = b.restricted_discovery();
    rd.enabled(true);
    let mut dirp = DirectoryKeyProviderBuilder::default();
    dirp.path(CfgPath::new_literal(auth_dir));
    rd.key_dirs().access().push(dirp);
    for (nick, key) in set {
        let nick = HsClientNickname::from_str(nick)
            .map_err(|e| io_fail("authorized nickname", e))?;
        let key = HsClientDescEncKey::from_str(key)
            .map_err(|e| io_fail("authorized discovery key", e))?;
        rd.static_keys().access().push((nick, key));
    }
    b.build().map_err(|e| io_fail("onion service config", e))
}

/// The Arti client config for one transport dir — a plain two-dir
/// layout (keystore rides inside the state dir).
fn client_config(root: &Path) -> RcResult<TorClientConfig> {
    TorClientConfigBuilder::from_directories(root.join("state"), root.join("cache"))
        .build()
        .map_err(|e| io_fail("arti client config", e))
}

/// fs_mistrust requires the Arti state/cache dirs to be private —
/// create them 0700 rather than inheriting umask.
fn ensure_dir_700(path: &Path) -> RcResult<()> {
    std::fs::create_dir_all(path).map_err(|e| io_fail("create arti dir", e))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))
            .map_err(|e| io_fail("chmod arti dir", e))?;
    }
    Ok(())
}

/// Parse a peer's onion into an HsId, tolerating the canonical
/// "<onion>:<port>" endpoint spelling — the transport's own
/// endpoint() and member onion_addr carry the sync port, while
/// keystore lookups are service-identity-only.  (SYNC11: pairing
/// over real Tor fed the port straight into HsId::from_str, the
/// rotate failed to "" — masked by unwrap_or_default — and the
/// inviter's member grant then failed authorize on the nickname
/// fallback.  mem never parsed the key, so the hermetic legs
/// could not see it.)
fn parse_hsid(peer_onion: &str) -> RcResult<HsId> {
    let bare = peer_onion.split(':').next().unwrap_or(peer_onion);
    HsId::from_str(bare)
        .map_err(|e| bad_arg::<()>(format!("peer onion address: {e}")).unwrap_err())
}

/// get-or-generate for the per-peer discovery key — pairing ops are
/// retryable, so keygen must be idempotent: an existing keystore
/// entry is returned, never re-rolled.
fn discovery_key_get_or_gen<C>(client: &C, hsid: HsId) -> RcResult<tor_hscrypto::pk::HsClientDescEncKey>
where
    C: DiscoveryKeys,
{
    if let Some(k) = client
        .get_service_discovery_key(hsid)
        .map_err(|e| io_fail("discovery key lookup", e))?
    {
        return Ok(k);
    }
    match client.generate_service_discovery_key(KeystoreSelector::Primary, hsid) {
        Ok(k) => Ok(k),
        Err(_) => client
            .get_service_discovery_key(hsid)
            .map_err(|e| io_fail("discovery key lookup", e))?
            .ok_or_else(|| io_fail("discovery keygen", "generate failed and lookup empty")),
    }
}

/// The discovery-key surface shared by the live and inert clients.
trait DiscoveryKeys {
    fn generate_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
    ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey>;
    fn get_service_discovery_key(
        &self,
        hsid: HsId,
    ) -> arti_client::Result<Option<tor_hscrypto::pk::HsClientDescEncKey>>;
    fn insert_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
        secret: HsClientDescEncSecretKey,
    ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey>;
    fn remove_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
    ) -> arti_client::Result<Option<()>>;
    fn rotate_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
    ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey>;
}

macro_rules! discovery_keys_impl {
    ($ty:ty) => {
        impl DiscoveryKeys for $ty {
            fn generate_service_discovery_key(
                &self,
                selector: KeystoreSelector,
                hsid: HsId,
            ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey> {
                <$ty>::generate_service_discovery_key(self, selector, hsid)
            }
            fn get_service_discovery_key(
                &self,
                hsid: HsId,
            ) -> arti_client::Result<Option<tor_hscrypto::pk::HsClientDescEncKey>> {
                <$ty>::get_service_discovery_key(self, hsid)
            }
            fn insert_service_discovery_key(
                &self,
                selector: KeystoreSelector,
                hsid: HsId,
                secret: HsClientDescEncSecretKey,
            ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey> {
                <$ty>::insert_service_discovery_key(self, selector, hsid, secret)
            }
            fn remove_service_discovery_key(
                &self,
                selector: KeystoreSelector,
                hsid: HsId,
            ) -> arti_client::Result<Option<()>> {
                <$ty>::remove_service_discovery_key(self, selector, hsid)
            }
            fn rotate_service_discovery_key(
                &self,
                selector: KeystoreSelector,
                hsid: HsId,
            ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey> {
                <$ty>::rotate_service_discovery_key(self, selector, hsid)
            }
        }
    };
}

discovery_keys_impl!(TorClient<PreferredRuntime>);
discovery_keys_impl!(arti_client::InertTorClient);

/// The live client when the transport runs, else an inert one on
/// the same keystore — one type the pairing key ops can call
/// without caring which.
enum DiscClient {
    Live(Arc<TorClient<PreferredRuntime>>),
}

impl DiscoveryKeys for DiscClient {
    fn generate_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
    ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey> {
        match self {
            DiscClient::Live(c) => c.generate_service_discovery_key(selector, hsid),
        }
    }
    fn get_service_discovery_key(
        &self,
        hsid: HsId,
    ) -> arti_client::Result<Option<tor_hscrypto::pk::HsClientDescEncKey>> {
        match self {
            DiscClient::Live(c) => c.get_service_discovery_key(hsid),
        }
    }
    fn insert_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
        secret: HsClientDescEncSecretKey,
    ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey> {
        match self {
            DiscClient::Live(c) => c.insert_service_discovery_key(selector, hsid, secret),
        }
    }
    fn remove_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
    ) -> arti_client::Result<Option<()>> {
        match self {
            DiscClient::Live(c) => c.remove_service_discovery_key(selector, hsid),
        }
    }
    fn rotate_service_discovery_key(
        &self,
        selector: KeystoreSelector,
        hsid: HsId,
    ) -> arti_client::Result<tor_hscrypto::pk::HsClientDescEncKey> {
        match self {
            DiscClient::Live(c) => c.rotate_service_discovery_key(selector, hsid),
        }
    }
}

/// A keystore-only client for offline key operations — carries its
/// own PreferredRuntime internally so it works on plain threads.
fn inert_client(
    cfg: &TorClientConfig,
) -> RcResult<arti_client::InertTorClient> {
    let rt = PreferredRuntime::create().map_err(|e| io_fail("arti runtime", e))?;
    TorClient::with_runtime(rt)
        .config(cfg.clone())
        .create_inert()
        .map_err(|e| io_fail("inert tor client", e))
}

fn install_crypto_provider() {
    static ONCE: Once = Once::new();
    ONCE.call_once(|| {
        let _ = rustls::crypto::ring::default_provider().install_default();
    });
}

impl TorTransport {
    /// Start Arti under `dir` (`<data root>/sync`) and return
    /// immediately: bootstrap + onion publish run on the transport's
    /// runtime in the background, surfaced as Bootstrap/Ready/Failed
    /// events the UI drains concurrently.  A blocking start used to
    /// park the mw worker on directory fetch — pair dialogs queued
    /// behind it rendered nothing while Tor was slow.
    pub fn start(dir: &Path) -> RcResult<Arc<Self>> {
        install_crypto_provider();
        let root = dir.join(TRANSPORT_DIR);
        // fs_mistrust walks ancestors: a group/other-writable parent
        // (umask-loosened install, restored backup) makes Arti refuse
        // the keystore outright — normalize the chain we own.
        if let Some(parent) = dir.parent() {
            ensure_dir_700(parent)?;
        }
        ensure_dir_700(dir)?;
        ensure_dir_700(&root)?;
        for sub in ["state", "cache", AUTH_DIR_PLACEHOLDER] {
            ensure_dir_700(&root.join(sub))?;
        }
        let authorized = authorized_set(dir);
        let cfg = client_config(&root)?;

        let rt = tokio::runtime::Builder::new_multi_thread()
            .worker_threads(2)
            .enable_all()
            .build()
            .map_err(|e| io_fail("tokio runtime", e))?;
        let handle = rt.handle().clone();

        // The onion address is knowable before any network I/O: the
        // hsid lives in the keystore, generated once and reused.
        let svc = TorClient::<PreferredRuntime>::create_onion_service(
            &cfg,
            svc_config_for(&authorized, &root.join(AUTH_DIR_PLACEHOLDER))?,
        )
        .map_err(|e| io_fail("create onion service", e))?;
        let hsid = svc
            .generate_identity_key(KeystoreSelector::Primary)
            .map_err(|e| io_fail("onion identity", e))?;
        let endpoint = format!("{}:{SYNC_ONION_PORT}", hsid.display_unredacted());

        let arti_rt = PreferredRuntime::create()
            .map_err(|e| io_fail("arti runtime", e))?;
        let client = TorClient::with_runtime(arti_rt)
            .config(cfg)
            .create_unbootstrapped()
            .map_err(|e| io_fail("create tor client", e))?;

        let t = Arc::new(TorTransport {
            dir: dir.to_path_buf(),
            endpoint,
            handle,
            rt: Mutex::new(Some(rt)),
            client: client.clone(),
            authorized: Mutex::new(authorized),
            links: Arc::new(Mutex::new(HashMap::new())),
            events: Arc::new(Mutex::new(VecDeque::new())),
            live: Mutex::new(None),
            next_link: std::sync::atomic::AtomicU64::new(1),
        });

        // Bootstrap progress events ride the same pull queue the
        // app drains — the UI gets live status for free.
        {
            let events = Arc::clone(&t.events);
            let progress_client = client.clone();
            let handle = t.handle.clone();
            handle.spawn(async move {
                let mut evs = progress_client.bootstrap_events();
                while let Some(s) = evs.next().await {
                    push_event(
                        &events,
                        TransportEvent::Bootstrap(
                            (s.as_frac() * 100.0).round() as u8,
                        ),
                    );
                }
            });
        }
        // Bootstrap + onion publish run on the transport runtime —
        // start() returns immediately so the mw caller (pair dialog,
        // sync round) is never parked on directory fetch.  Progress
        // arrives as Bootstrap events, readiness as Ready, failure as
        // Failed; connect() awaits the same bootstrap internally.
        {
            let t = Arc::clone(&t);
            let auth_dir = root.join(AUTH_DIR_PLACEHOLDER);
            let handle = t.handle.clone();
            handle.spawn(async move {
                eprintln!("arktos-sync: tor bootstrap started");
                if let Err(e) = t.client.bootstrap().await {
                    eprintln!("arktos-sync: tor bootstrap failed: {e}");
                    push_event(&t.events, TransportEvent::Failed {
                        reason: format!("tor bootstrap: {e}"),
                    });
                    return;
                }
                eprintln!("arktos-sync: tor bootstrap done — launching onion service");
                let auth = t
                    .authorized
                    .lock()
                    .unwrap_or_else(|e| e.into_inner())
                    .clone();
                let svc_cfg = match svc_config_for(&auth, &auth_dir) {
                    Ok(c) => c,
                    Err(e) => {
                        push_event(&t.events, TransportEvent::Failed {
                            reason: format!("onion service config: {}", e.msg),
                        });
                        return;
                    }
                };
                let (running, rend) = match t.client.launch_onion_service(svc_cfg) {
                    Ok(Some(l)) => l,
                    Ok(None) => {
                        eprintln!("arktos-sync: launch onion service: disabled");
                        push_event(&t.events, TransportEvent::Failed {
                            reason: "launch onion service: service disabled".into(),
                        });
                        return;
                    }
                    Err(e) => {
                        eprintln!("arktos-sync: launch onion service failed: {e}");
                        push_event(&t.events, TransportEvent::Failed {
                            reason: format!("launch onion service: {e}"),
                        });
                        return;
                    }
                };
                eprintln!("arktos-sync: onion service launched — publishing");
                // Onion publish visibility — the connect deadline is
                // opaque without it: the terminal prints each state
                // change until the descriptor reaches the network.
                {
                    let svc = running.clone();
                    t.handle.spawn(async move {
                        let mut evs = svc.status_events();
                        while let Some(s) = evs.next().await {
                            eprintln!("arktos-sync: onion status {:?}", s.state());
                        }
                    });
                }
                *t.live.lock().unwrap_or_else(|e| e.into_inner()) =
                    Some(Live { svc: running });
                t.spawn_accept(rend);
                push_event(&t.events, TransportEvent::Ready);
            });
        }
        Ok(t)
    }

    /// The rend pump: every accepted rendezvous becomes a link whose
    /// DataStream feeds a frame reader + writer pair.
    fn spawn_accept(self: &Arc<Self>, rend: impl futures::Stream<Item = tor_hsservice::RendRequest> + Send + 'static) {
        let t = Arc::clone(self);
        self.handle.spawn(async move {
            let mut reqs = Box::pin(handle_rend_requests(rend));
            while let Some(req) = reqs.next().await {
                match req.accept(Connected::new_empty()).await {
                    Ok(stream) => {
                        let id = t.register_link(stream, true);
                        push_event(&t.events, TransportEvent::Incoming {
                            link: id,
                            from: "peer".to_string(),
                        });
                    }
                    Err(e) => push_event(
                        &t.events,
                        TransportEvent::Failed {
                            reason: format!("rendezvous accept: {e}"),
                        },
                    ),
                }
            }
        });
    }

    /// Wire one DataStream into the link table: unbounded frame
    /// queue -> writer pump; reader pump -> Frame/LinkClosed events.
    /// `announce` is not used — inbound callers emit Incoming
    /// themselves so the event carries the peer label.
    fn register_link(&self, stream: DataStream, _announce: bool) -> u64 {
        let id = self.next_link.fetch_add(1, Ordering::SeqCst);
        let (tx, mut rx) = mpsc::unbounded_channel::<Vec<u8>>();
        self.links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(id, tx);
        let (mut rd, mut wr) = stream.split();

        self.handle.spawn(async move {
            while let Some(payload) = rx.recv().await {
                let frame = encode_frame(&payload);
                if wr.write_all(&frame).await.is_err()
                    || wr.flush().await.is_err()
                {
                    break;
                }
            }
            let _ = wr.close().await;
        });

        let events = Arc::clone(&self.events);
        let links = Arc::clone(&self.links);
        self.handle.spawn(async move {
            let reason = read_loop(&mut rd, &events, id).await;
            links
                .lock()
                .unwrap_or_else(|e| e.into_inner())
                .remove(&id);
            push_event(&events, TransportEvent::LinkClosed { link: id, reason });
        });
        id
    }

    /// The keystore-bearing client for discovery-key ops — the
    /// transport's own client handle, usable for keystore ops even
    /// while bootstrap is still running (it exists from build time).
    /// The never-started case goes through the free `discovery_key`
    /// helper, which builds a throwaway inert client instead.
    fn discovery_client(&self) -> RcResult<DiscClient> {
        Ok(DiscClient::Live(self.client.clone()))
    }

    /// The client handle when running, else a throwaway inert
    /// client on the same keystore (pairing-time key ops must work
    /// before the transport is ever started — and need no runtime).
    fn discovery_key_impl(&self, hsid: HsId) -> RcResult<String> {
        let key = discovery_key_get_or_gen(&self.discovery_client()?, hsid)?;
        Ok(key.to_string())
    }

    /// Rebuild + apply the restricted-discovery config on the live
    /// service.  Descriptor republish is Tor-rate-limited; pairing
    /// changes are rare enough to ride it.
    fn reconfigure(&self) -> RcResult<()> {
        let live = self.live.lock().unwrap_or_else(|e| e.into_inner());
        let Some(l) = live.as_ref() else { return Ok(()) };
        let set = self.authorized.lock().unwrap_or_else(|e| e.into_inner()).clone();
        l.svc.reconfigure(
            svc_config_for(&set, &self.dir.join(TRANSPORT_DIR).join(AUTH_DIR_PLACEHOLDER))?,
            Reconfigure::AllOrNothing,
        )
            .map_err(|e| io_fail("reconfigure onion service", e))
    }
}

/// The onion endpoint a transport under `dir` would publish —
/// keystore-derived, computed offline (the inert service path).
/// `dir` is the sync dir (`<data root>/sync`).
pub fn endpoint_for(dir: &Path) -> RcResult<String> {
    install_crypto_provider();
    let root = dir.join(TRANSPORT_DIR);
    ensure_dir_700(&root)?;
    ensure_dir_700(&root.join("state"))?;
    ensure_dir_700(&root.join("cache"))?;
    let cfg = client_config(&root)?;
    let svc = TorClient::<PreferredRuntime>::create_onion_service(
        &cfg,
        svc_config_for(&BTreeMap::new(), &root.join(AUTH_DIR_PLACEHOLDER))?,
    )
    .map_err(|e| io_fail("create onion service", e))?;
    let hsid = svc
        .generate_identity_key(KeystoreSelector::Primary)
        .map_err(|e| io_fail("onion identity", e))?;
    Ok(format!("{}:{SYNC_ONION_PORT}", hsid.display_unredacted()))
}

/// Our public discovery key for `peer_onion` with no running
/// transport — opens a throwaway unbootstrapped client on the same
/// keystore so pairing-time key exchange works before first launch.
pub fn discovery_key(dir: &Path, peer_onion: &str) -> RcResult<String> {
    install_crypto_provider();
    let hsid = parse_hsid(peer_onion)?;
    let cfg = client_config(&dir.join(TRANSPORT_DIR))?;
    let key = discovery_key_get_or_gen(&inert_client(&cfg)?, hsid)?;
    Ok(key.to_string())
}

/// SYNC03: install a peer-supplied descriptor-encryption secret
/// into our client keystore for `peer_onion` — the invite-derived
/// bootstrap key.  Deterministic: any previous key for the hsid is
/// removed first so a re-scanned invite reinstalls instead of
/// colliding with the no-overwrite insert.  `dir` is the sync dir.
pub fn import_discovery_key(dir: &Path, peer_onion: &str, secret: &[u8; 32]) -> RcResult<()> {
    install_crypto_provider();
    let hsid = parse_hsid(peer_onion)?;
    let cfg = client_config(&dir.join(TRANSPORT_DIR))?;
    let client = inert_client(&cfg)?;
    let _ = client
        .remove_service_discovery_key(KeystoreSelector::Primary, hsid)
        .map_err(|e| io_fail("discovery key remove", e))?;
    let sk = HsClientDescEncSecretKey::from(curve25519::StaticSecret::from(*secret));
    client
        .insert_service_discovery_key(KeystoreSelector::Primary, hsid, sk)
        .map_err(|e| io_fail("discovery key insert", e))?;
    Ok(())
}

/// SYNC03: rotate our discovery keypair for `peer_onion` to a fresh
/// permanent key after the invite-derived one did its one-time job.
/// Returns the new public `descriptor:x25519:` key for the peer to
/// authorize.  Keystore-only — works with the transport down.
pub fn rotate_discovery_key(dir: &Path, peer_onion: &str) -> RcResult<String> {
    install_crypto_provider();
    let hsid = parse_hsid(peer_onion)?;
    let cfg = client_config(&dir.join(TRANSPORT_DIR))?;
    let client = inert_client(&cfg)?;
    let key = client
        .rotate_service_discovery_key(KeystoreSelector::Primary, hsid)
        .map_err(|e| io_fail("discovery key rotate", e))?;
    Ok(key.to_string())
}

/// The frame reader half of a link: pull `<u32 len><payload>` until
/// EOF/error, emitting Frame events.  Returns the close reason.
async fn read_loop(
    rd: &mut arti_client::DataReader,
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

impl SyncTransport for TorTransport {
    fn backend(&self) -> &'static str {
        "tor"
    }

    fn endpoint(&self) -> RcResult<String> {
        Ok(self.endpoint.clone())
    }

    fn authorize(&self, nickname: &str, discovery_key: &str) -> RcResult<()> {
        // Fail fast on garbage before it reaches the grant set — the
        // file is replayed into every future service config.
        HsClientNickname::from_str(nickname)
            .map_err(|e| bad_arg::<()>(format!("nickname: {e}")).unwrap_err())?;
        HsClientDescEncKey::from_str(discovery_key)
            .map_err(|e| bad_arg::<()>(format!("discovery key: {e}")).unwrap_err())?;
        self.authorized
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .insert(nickname.to_string(), discovery_key.to_string());
        save_authorized(
            &self.dir,
            &self.authorized.lock().unwrap_or_else(|e| e.into_inner()),
        )?;
        self.reconfigure()
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
            self.reconfigure()?;
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
        let hsid = parse_hsid(peer_onion)?;
        self.discovery_key_impl(hsid)
    }

    fn import_discovery_key(&self, peer_onion: &str, secret: &[u8; 32]) -> RcResult<()> {
        let hsid = parse_hsid(peer_onion)?;
        let client = self.discovery_client()?;
        let _ = client
            .remove_service_discovery_key(KeystoreSelector::Primary, hsid)
            .map_err(|e| io_fail("discovery key remove", e))?;
        let sk = HsClientDescEncSecretKey::from(curve25519::StaticSecret::from(*secret));
        client
            .insert_service_discovery_key(KeystoreSelector::Primary, hsid, sk)
            .map_err(|e| io_fail("discovery key insert", e))?;
        Ok(())
    }

    fn rotate_discovery_key(&self, peer_onion: &str) -> RcResult<String> {
        let hsid = parse_hsid(peer_onion)?;
        let client = self.discovery_client()?;
        let key = client
            .rotate_service_discovery_key(KeystoreSelector::Primary, hsid)
            .map_err(|e| io_fail("discovery key rotate", e))?;
        Ok(key.to_string())
    }

    fn connect(&self, endpoint: &str, _as_nickname: &str) -> RcResult<u64> {
        if self
            .rt
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .is_none()
        {
            return Err(Fail {
                status: RcStatus::NotEnabled,
                msg: "tor transport not started".into(),
            });
        }
        // relay://<onion>/<cmd>/<key> — dial the rendezvous relay and
        // write its OFFER/CLAIM preamble on the raw stream before the
        // link registers.  CLAIM waits for the splice ack ("OK\n").
        let mut cmd: Option<(String, String)> = None;
        let mut target = endpoint.to_string();
        if let Some(rest) = endpoint.strip_prefix("relay://") {
            let mut it = rest.split('/');
            let onion = it.next().unwrap_or("");
            target = format!("{onion}:443");
            if let (Some(c), Some(k)) = (it.next(), it.next()) {
                cmd = Some((c.to_string(), k.to_string()));
            }
        }
        let client = self.client.clone();
        let handle = self.handle.clone();
        let stream = handle.block_on(async move {
            let start = Instant::now();
            // start() returns before bootstrap — a dial placed while
            // it is still running waits here, inside the same budget.
            match tokio::time::timeout(CONNECT_DEADLINE, client.bootstrap()).await
            {
                Ok(Ok(())) => {}
                Ok(Err(e)) => return Err(io_fail("tor bootstrap", e)),
                Err(_) => {
                    return Err(io_fail(
                        "connect deadline",
                        format!("{target}: bootstrap did not finish"),
                    ))
                }
            }
            let mut attempt = 0u32;
            let mut auth_fails = 0u32;
            loop {
                match client.connect(target.as_str()).await {
                    Ok(s) => break Ok(s),
                    Err(e) => {
                        let kind = e.kind();
                        let refused = matches!(
                            kind,
                            ErrorKind::OnionServiceMissingClientAuth
                                | ErrorKind::OnionServiceWrongClientAuth
                        );
                        if refused {
                            auth_fails += 1;
                        }
                        if refused && auth_fails >= AUTH_FAIL_LIMIT {
                            break Err(Fail {
                                status: RcStatus::Crypto,
                                msg: format!(
                                    "peer {target} refused our client-auth key — \
                                     not authorized at the Tor layer"
                                ),
                            });
                        }
                        if start.elapsed() >= CONNECT_DEADLINE {
                            break Err(io_fail(
                                "connect deadline",
                                format!("{target}: {e}"),
                            ));
                        }
                        tokio::time::sleep(backoff_delay(attempt)).await;
                        attempt = attempt.saturating_add(1);
                    }
                }
            }
        })?;
        if let Some((c, k)) = cmd {
            let mut stream = stream;
            let line = format!("{c} {k}\n");
            handle.block_on(async {
                futures::AsyncWriteExt::write_all(&mut stream, line.as_bytes()).await
            })
            .map_err(|e| io_fail("relay preamble", e))?;
            if c == "CLAIM" {
                // The relay answers OK once spliced — bounded so a
                // stale offer can't hang the dial forever.
                let mut buf = [0u8; 3];
                handle.block_on(async {
                    tokio::time::timeout(
                        Duration::from_secs(60),
                        futures::AsyncReadExt::read_exact(&mut stream, &mut buf),
                    )
                    .await
                })
                .map_err(|_| io_fail("relay splice", "no offer matched"))?
                .map_err(|e| io_fail("relay splice", e))?;
                if &buf != b"OK\n" {
                    return Err(io_fail(
                        "relay splice",
                        format!("claim rejected: {}", String::from_utf8_lossy(&buf)),
                    ));
                }
            }
            return Ok(self.register_link(stream, false));
        }
        Ok(self.register_link(stream, false))
    }

    fn send(&self, link: u64, frame: &[u8]) -> RcResult<()> {
        let links = self.links.lock().unwrap_or_else(|e| e.into_inner());
        let Some(tx) = links.get(&link) else {
            return not_found(format!("no link {link}"));
        };
        tx.send(frame.to_vec())
            .map_err(|e| io_fail("send", e))
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

    fn stop(&self) {
        // Dropping the service stops publishing; dropping the client
        // tears circuits; draining links ends the writer pumps.
        let live = self
            .live
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .take();
        drop(live);
        self.links
            .lock()
            .unwrap_or_else(|e| e.into_inner())
            .clear();
        let rt = self.rt.lock().unwrap_or_else(|e| e.into_inner()).take();
        if let Some(rt) = rt {
            rt.shutdown_timeout(std::time::Duration::from_secs(3));
        }
    }
}

impl Drop for TorTransport {
    fn drop(&mut self) {
        self.stop();
    }
}

// ---- tests -------------------------------------------------------------------
//
// These legs are hermetic: keystore work (identity, discovery keys,
// the grant set) needs no network.  Live circuits are exercised by
// tools/synctor-e2e.sh against the real Tor network.

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-tor-{}-{}-{}",
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

    /// Identity is keystore-derived: the inert service mints the
    /// same hsid on every open of the same state dir.
    #[test]
    fn identity_is_stable_across_reopen() {
        let dir = tmpdir("ident");
        let root = dir.join(TRANSPORT_DIR);
        ensure_dir_700(&root.join("state")).unwrap();
        ensure_dir_700(&root.join("cache")).unwrap();
        let cfg = client_config(&root).unwrap();
        let set = BTreeMap::new();
        let addr = || {
            let svc = TorClient::<PreferredRuntime>::create_onion_service(
                &cfg,
                svc_config_for(&set, &root).unwrap(),
            )
            .unwrap();
            svc.generate_identity_key(KeystoreSelector::Primary)
                .unwrap()
                .display_unredacted()
                .to_string()
        };
        let a = addr();
        let b = addr();
        assert_eq!(a, b);
        assert!(a.ends_with(".onion"));
    }

    /// Discovery keys are per-peer-hsid keystore entries: generated
    /// offline, stable across calls, well-formed for the grant file.
    #[test]
    fn discovery_key_offline_and_stable() {
        let dir = tmpdir("dkey");
        let root = dir.join(TRANSPORT_DIR);
        ensure_dir_700(&root.join("state")).unwrap();
        ensure_dir_700(&root.join("cache")).unwrap();
        let cfg = client_config(&root).unwrap();
        let svc = TorClient::<PreferredRuntime>::create_onion_service(
            &cfg,
            svc_config_for(&BTreeMap::new(), &root).unwrap(),
        )
        .unwrap();
        let peer_hsid = svc
            .generate_identity_key(KeystoreSelector::Primary)
            .unwrap()
            .display_unredacted()
            .to_string();

        let client = inert_client(&cfg).unwrap();
        let k1 = discovery_key_get_or_gen(&client, HsId::from_str(&peer_hsid).unwrap())
            .unwrap()
            .to_string();
        let k2 = discovery_key_get_or_gen(&client, HsId::from_str(&peer_hsid).unwrap())
            .unwrap()
            .to_string();
        assert_eq!(k1, k2, "same keystore entry, not a re-roll");
        assert!(k1.starts_with("descriptor:x25519:"));
        // The generated key parses as a valid grant entry.
        HsClientDescEncKey::from_str(&k1).unwrap();
    }

    /// The service config carries the persisted grant set: an
    /// authorized peer key lands in static_keys, junk is refused.
    #[test]
    fn svc_config_carries_grant_set() {
        let mut set = BTreeMap::new();
        // empty grant set still builds (key_dirs satisfies the
        // non-empty-provider rule) — refuse everyone until paired.
        svc_config_for(&set, Path::new("/tmp")).unwrap();
        set.insert(
            "peer-a".to_string(),
            "descriptor:x25519:zprrmiv6dv6sjfl7sfbsvlj5vunpgcdfevz7m23ltlvtccxjqbka"
                .to_string(),
        );
        svc_config_for(&set, Path::new("/tmp")).unwrap();
        set.insert("bad nick!".to_string(), "descriptor:x25519:JUNK".to_string());
        assert!(svc_config_for(&set, Path::new("/tmp")).is_err());
    }
}
