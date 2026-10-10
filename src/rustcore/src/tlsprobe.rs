//! SEC22: second-opinion TLS verification — an embedder-side chain
//! probe that evaluates a host's certificate INDEPENDENTLY of whatever
//! the engine decided.
//!
//! On WebEngine this is additive defense-in-depth: a second verifier
//! catching what Chromium's missed (or confirming its verdict).  On a
//! future engine without a cert-error delegate (the ENG03 libservo
//! spike found none) the same verdict is the only chain check the
//! shell can offer — the Qt side consumes the identical JSON either
//! way.
//!
//! What a probe does:
//!   * TCP connect + a REAL TLS handshake to host:port (TLS 1.2/1.3
//!     only — rustls cannot speak anything older — ALPN "http/1.1",
//!     SNI = the host, 5 s connect + 5 s per-io timeouts under a 15 s
//!     total budget).
//!   * Chain evaluation by rustls-webpki against the platform root
//!     store (rustls-native-certs) plus any caller-added roots — the
//!     same class of verifier Chromium runs, with no shared code path.
//!   * The peer chain is summarized from the wire certs themselves
//!     (x509-parser), not from the engine's view of them.
//!
//! Honesty rules (the Qt chip lives or dies by these):
//!   * "verified"  — the handshake completed AND the chain validated.
//!   * "warning"   — the chain was evaluated and FAILED; error_class
//!     says why: expired, not-yet-valid, bad-hostname, untrusted-root,
//!     broken-chain, weak-signature, revoked.
//!   * "unverified" — the chain was never evaluated: DNS/connect/
//!     timeout/refused ("network"), a TLS-protocol failure before the
//!     chain arrived ("protocol"), or an unusable root store
//!     ("root-store").  A network hiccup is NOT a bad chain and must
//!     never render as one.
//!   * "refused"   — the probe declined to run: private/loopback/LAN/
//!     .onion targets are refused unless the caller passes
//!     RC_TLS_F_ALLOW_LOCAL (SSRF discipline, matching the Qt-side
//!     isPrivateOrLocalHost rule and DLACC04's).  The Qt gate never
//!     issues these probes; the refusal exists so no future caller can
//!     accidentally turn the probe into an intranet scanner.
//!
//! Deliberately absent: OCSP/CRL revocation checking ("not-checked"
//! is reported verbatim in the verdict) and any proxy support — the
//! probe is a direct handshake, so the Qt side must not issue it when
//! the app's traffic is supposed to ride a SOCKS/HTTP proxy (tor mode
//! included: a direct handshake would de-anonymize the user).

use std::io::{self, Write};
use std::net::{IpAddr, Ipv4Addr, Ipv6Addr, TcpStream, ToSocketAddrs};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use rustls::client::danger::{
    HandshakeSignatureValid, ServerCertVerified, ServerCertVerifier,
};
use rustls::client::WebPkiServerVerifier;
use rustls::pki_types::{CertificateDer, ServerName, UnixTime};
use rustls::{
    CertificateError, ClientConfig, ClientConnection, DigitallySignedStruct,
    DistinguishedName, RootCertStore, SignatureScheme,
};
use serde_json::{json, Value};
use x509_parser::prelude::FromDer;

use crate::error::{Fail, RcResult, RcStatus};

/// Callers pass RC_TLS_F_ALLOW_LOCAL (see rustcore.h) to probe
/// private/loopback targets — the fixture-server seam the autotests
/// use.  Production callers pass 0.
pub const FLAG_ALLOW_LOCAL: u32 = 0x01;

const CONNECT_TIMEOUT: Duration = Duration::from_secs(5);
const IO_TIMEOUT: Duration = Duration::from_secs(5);
const TOTAL_BUDGET: Duration = Duration::from_secs(15);
/// Resolution can return a long list (happy-eyeballs style); probing
/// every entry would outlive the budget, so cap the attempts.
const MAX_ADDR_ATTEMPTS: usize = 4;

/// Caller-installed roots — the test seam (fixture CA) and the future
/// enterprise/user-installed-root hook.  Kept as DER so the store is
/// rebuilt per probe (roots are cheap to re-parse and a reload stays
/// atomic).
static EXTRA_ROOTS: Mutex<Vec<Vec<u8>>> = Mutex::new(Vec::new());

/// rc_tls_add_root: validate + install an extra trust anchor.
/// RC_CORRUPT when the bytes are not a parseable X.509 certificate.
pub fn add_root(der: &[u8]) -> RcResult<()> {
    if x509_parser::certificate::X509Certificate::from_der(der).is_err() {
        return crate::error::fail(RcStatus::Corrupt, "root is not valid DER X.509");
    }
    let mut roots = EXTRA_ROOTS
        .lock()
        .map_err(|_| Fail {
            status: RcStatus::Unavailable,
            msg: "root store lock poisoned".into(),
        })?;
    // Dedup: repeated installs (test re-arms) must not grow the list.
    if !roots.iter().any(|r| r == der) {
        roots.push(der.to_vec());
    }
    Ok(())
}

/// rc_tls_clear_roots: drop the caller-installed anchors (test seam;
/// the platform roots are reloaded per probe and unaffected).
pub fn clear_roots() -> RcResult<()> {
    EXTRA_ROOTS
        .lock()
        .map_err(|_| Fail {
            status: RcStatus::Unavailable,
            msg: "root store lock poisoned".into(),
        })?
        .clear();
    Ok(())
}

/// Mirror of the Qt side's PrivacyRequestInterceptor::isPrivateOrLocalHost
/// — keep the two lists in step: loopback, private/link-local LAN
/// ranges, CGNAT, benchmarking, .localhost/.local/.onion.  Hostnames
/// that are not IP literals pass (a name's reachability is a DNS
/// question, answered per-hop by the resolver like any browser fetch).
fn is_private_or_local(host: &str) -> bool {
    let mut h = host.trim_end_matches('.').to_lowercase();
    // QUrl::host() keeps IPv6 literals bracketed; accept both forms.
    if h.starts_with('[') && h.ends_with(']') {
        h = h[1..h.len() - 1].to_string();
    }
    if h.is_empty()
        || h == "localhost"
        || h.ends_with(".localhost")
        || h.ends_with(".local")
        || h.ends_with(".onion")
    {
        return true;
    }
    match h.parse::<IpAddr>() {
        Ok(IpAddr::V4(ip)) => v4_private(ip),
        Ok(IpAddr::V6(ip)) => {
            ip.is_loopback()
                || ip.is_multicast()
                || ip.is_unspecified()
                // fc00::/7 ULA and fe80::/10 link-local.
                || (ip.segments()[0] & 0xfe00) == 0xfc00
                || (ip.segments()[0] & 0xffc0) == 0xfe80
                // v4-mapped/v4-compatible literals embed a v4 address.
                || ip.to_ipv4_mapped().map(v4_private).unwrap_or(false)
        }
        Err(_) => false,
    }
}

fn v4_private(ip: Ipv4Addr) -> bool {
    let o = ip.octets();
    ip.is_loopback()            // 127.0.0.0/8
        || ip.is_multicast()    // 224.0.0.0/4
        || ip.is_unspecified()  // 0.0.0.0
        || ip.is_broadcast()
        || o[0] == 10                                   // 10.0.0.0/8
        || (o[0] == 172 && (o[1] & 0xf0) == 16)         // 172.16.0.0/12
        || (o[0] == 192 && o[1] == 168)                 // 192.168.0.0/16
        || (o[0] == 169 && o[1] == 254)                 // 169.254.0.0/16
        || (o[0] == 100 && (o[1] & 0xc0) == 64)         // 100.64.0.0/10 CGNAT
        || (o[0] == 198 && (o[1] & 0xfe) == 18)         // 198.18.0.0/15
        || o[0] == 0                                    // 0.0.0.0/8 "this host"
        || o[0] >= 240 // 240.0.0.0/4 reserved
}

/// The verdict handed to the Qt side as a JSON document.
struct Verdict {
    status: &'static str,
    error_class: Option<&'static str>,
    detail: String,
}

/// Wraps the platform-roots WebPkiServerVerifier and stashes the
/// chain it was asked to evaluate — verification stays entirely
/// webpki's, the capture only observes.  (A failed handshake is the
/// interesting case for the UI, and peer_certificates() is not
/// populated reliably on the error path.)
#[derive(Debug)]
struct CaptureVerifier {
    inner: Arc<WebPkiServerVerifier>,
    captured: Arc<Mutex<Vec<CertificateDer<'static>>>>,
}

impl ServerCertVerifier for CaptureVerifier {
    fn verify_server_cert(
        &self,
        end_entity: &CertificateDer<'_>,
        intermediates: &[CertificateDer<'_>],
        server_name: &ServerName<'_>,
        ocsp_response: &[u8],
        now: UnixTime,
    ) -> Result<ServerCertVerified, rustls::Error> {
        let mut chain = Vec::with_capacity(intermediates.len() + 1);
        chain.push(CertificateDer::from(end_entity.as_ref().to_vec()));
        chain.extend(
            intermediates
                .iter()
                .map(|c| CertificateDer::from(c.as_ref().to_vec())),
        );
        if let Ok(mut slot) = self.captured.lock() {
            *slot = chain;
        }
        self.inner.verify_server_cert(
            end_entity,
            intermediates,
            server_name,
            ocsp_response,
            now,
        )
    }

    fn verify_tls12_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        self.inner.verify_tls12_signature(message, cert, dss)
    }

    fn verify_tls13_signature(
        &self,
        message: &[u8],
        cert: &CertificateDer<'_>,
        dss: &DigitallySignedStruct,
    ) -> Result<HandshakeSignatureValid, rustls::Error> {
        self.inner.verify_tls13_signature(message, cert, dss)
    }

    fn supported_verify_schemes(&self) -> Vec<SignatureScheme> {
        self.inner.supported_verify_schemes()
    }

    fn root_hint_subjects(&self) -> Option<&[DistinguishedName]> {
        self.inner.root_hint_subjects()
    }
}

impl Verdict {
    fn new(status: &'static str, class: Option<&'static str>, detail: impl Into<String>) -> Self {
        Verdict {
            status,
            error_class: class,
            detail: detail.into(),
        }
    }
    fn refused(class: &'static str, detail: impl Into<String>) -> Self {
        Self::new("refused", Some(class), detail)
    }
    fn unverified(class: &'static str, detail: impl Into<String>) -> Self {
        Self::new("unverified", Some(class), detail)
    }
    fn warning(class: &'static str, detail: impl Into<String>) -> Self {
        Self::new("warning", Some(class), detail)
    }
    fn verified() -> Self {
        Self::new("verified", None, "")
    }
}

/// rc_tls_check: run the probe and render the verdict JSON.  This is
/// the whole contract — a JSON document on every path, never an FFI
/// error for a reachable-but-bad chain.
pub fn check(host: &str, port: u16, flags: u32) -> Value {
    let host = host.trim();
    if host.is_empty() {
        return verdict_json(Verdict::refused("invalid-host", "empty host"), host, port, None, &[]);
    }
    if port == 0 {
        return verdict_json(Verdict::refused("invalid-port", "port 0"), host, port, None, &[]);
    }
    let allow_local = flags & FLAG_ALLOW_LOCAL != 0;
    if !allow_local && is_private_or_local(host) {
        return verdict_json(
            Verdict::refused(
                "private-host",
                "private/loopback targets are not probed (pass RC_TLS_F_ALLOW_LOCAL to override)",
            ),
            host,
            port,
            None,
            &[],
        );
    }

    // Root store: platform anchors + caller-installed extras.  Loaded
    // per probe so an OS-level store refresh is picked up and a stale
    // cache can never pin an old anchor.
    let mut roots = RootCertStore::empty();
    let native = rustls_native_certs::load_native_certs();
    let native_count = native.certs.len();
    roots.add_parsable_certificates(native.certs);
    let extra_roots: Vec<Vec<u8>> = EXTRA_ROOTS
        .lock()
        .map(|r| r.clone())
        .unwrap_or_default();
    let mut extra_count = 0usize;
    for der in &extra_roots {
        if roots.add(CertificateDer::from(der.clone())).is_ok() {
            extra_count += 1;
        }
    }
    if native_count == 0 && extra_count == 0 {
        return verdict_json(
            Verdict::unverified(
                "root-store",
                "no trust anchors available (platform root store empty or unreadable)",
            ),
            host,
            port,
            None,
            &[],
        );
    }

    let provider = Arc::new(rustls::crypto::ring::default_provider());
    let captured = Arc::new(Mutex::new(Vec::new()));
    let verifier: Arc<dyn ServerCertVerifier> =
        match WebPkiServerVerifier::builder(Arc::new(roots)).build() {
            Ok(inner) => Arc::new(CaptureVerifier {
                inner,
                captured: captured.clone(),
            }),
            Err(e) => {
                return verdict_json(
                    Verdict::unverified("protocol", format!("tls verifier: {e}")),
                    host,
                    port,
                    None,
                    &[],
                );
            }
        };
    let mut config = match ClientConfig::builder_with_provider(provider)
        .with_safe_default_protocol_versions()
    {
        Ok(stage) => stage
            .dangerous()
            .with_custom_certificate_verifier(verifier)
            .with_no_client_auth(),
        Err(e) => {
            return verdict_json(
                Verdict::unverified("protocol", format!("tls config: {e}")),
                host,
                port,
                None,
                &[],
            );
        }
    };
    config.alpn_protocols = vec![b"http/1.1".to_vec()];

    let server_name = match ServerName::try_from(host.to_string()) {
        Ok(n) => n,
        Err(e) => {
            return verdict_json(
                Verdict::refused("invalid-host", format!("unusable server name: {e}")),
                host,
                port,
                None,
                &[],
            );
        }
    };

    let deadline = Instant::now() + TOTAL_BUDGET;

    // DNS + connect.  Resolution failure is a network event, not a
    // verdict on the certificate.
    let addrs = match (host, port).to_socket_addrs() {
        Ok(it) => it.take(MAX_ADDR_ATTEMPTS).collect::<Vec<_>>(),
        Err(e) => {
            return verdict_json(
                Verdict::unverified("network", format!("dns: {e}")),
                host,
                port,
                None,
                &[],
            );
        }
    };
    let mut stream: Option<TcpStream> = None;
    let mut last_err: Option<io::Error> = None;
    for addr in &addrs {
        let remaining = deadline.saturating_duration_since(Instant::now());
        let budget = remaining.min(CONNECT_TIMEOUT);
        if budget.is_zero() {
            break;
        }
        match TcpStream::connect_timeout(addr, budget) {
            Ok(s) => {
                stream = Some(s);
                break;
            }
            Err(e) => last_err = Some(e),
        }
    }
    let mut stream = match stream {
        Some(s) => s,
        None => {
            let detail = last_err
                .map(|e| format!("connect: {e}"))
                .unwrap_or_else(|| "connect: no addresses".to_string());
            return verdict_json(
                Verdict::unverified("network", detail),
                host,
                port,
                None,
                &[],
            );
        }
    };
    let _ = stream.set_read_timeout(Some(IO_TIMEOUT));
    let _ = stream.set_write_timeout(Some(IO_TIMEOUT));
    let _ = stream.set_nodelay(true);

    let mut conn = match ClientConnection::new(Arc::new(config), server_name) {
        Ok(c) => c,
        Err(e) => {
            return verdict_json(
                Verdict::unverified("protocol", format!("tls init: {e}")),
                host,
                port,
                None,
                &[],
            );
        }
    };

    // Drive the handshake packet loop by hand so rustls errors reach
    // us unwrapped — complete_io() would fold them into io::Error.
    let mut tls_err: Option<rustls::Error> = None;
    let mut io_err: Option<io::Error> = None;
    while conn.is_handshaking() {
        if Instant::now() > deadline {
            io_err = Some(io::Error::new(io::ErrorKind::TimedOut, "handshake budget"));
            break;
        }
        if conn.wants_write() {
            match conn.write_tls(&mut stream) {
                Ok(_) => {
                    if let Err(e) = stream.flush() {
                        io_err = Some(e);
                        break;
                    }
                }
                Err(e) => {
                    io_err = Some(e);
                    break;
                }
            }
        }
        if conn.wants_read() && conn.is_handshaking() {
            match conn.read_tls(&mut stream) {
                Ok(0) => {
                    io_err = Some(io::Error::new(
                        io::ErrorKind::UnexpectedEof,
                        "peer closed during handshake",
                    ));
                    break;
                }
                Ok(_) => {}
                Err(e) => {
                    io_err = Some(e);
                    break;
                }
            }
            if let Err(e) = conn.process_new_packets() {
                tls_err = Some(e);
                break;
            }
        }
        // Neither direction wants progress but the handshake is not
        // done — cannot happen for a client conn, but never spin.
        if !conn.wants_read() && !conn.wants_write() {
            break;
        }
    }

    // The chain the verifier actually evaluated — captured inside the
    // verify callback so it is present even when verification fails
    // (peer_certificates() is not guaranteed populated on the error
    // path).  Fall back to the connection's view for servers that
    // somehow present a chain the verifier never ran.
    let chain_der: Vec<CertificateDer<'static>> = captured
        .lock()
        .map(|c| c.clone())
        .unwrap_or_default();
    let chain_der = if chain_der.is_empty() {
        conn.peer_certificates()
            .map(|c| c.to_vec())
            .unwrap_or_default()
    } else {
        chain_der
    };
    let chain = chain_summary(&chain_der);

    let negotiated = if !conn.is_handshaking() && tls_err.is_none() {
        Some(json!({
            "tls_version": conn.protocol_version()
                .map(|v| format!("{v:?}")),
            "cipher_suite": conn.negotiated_cipher_suite()
                .map(|c| format!("{:?}", c.suite())),
            "alpn": conn.alpn_protocol()
                .map(|a| String::from_utf8_lossy(a).to_string()),
        }))
    } else {
        None
    };

    let verdict = if let Some(e) = tls_err {
        classify(&e)
    } else if conn.is_handshaking() {
        // Left the loop still handshaking: an io failure or the budget.
        let detail = io_err
            .map(|e| format!("{e}"))
            .unwrap_or_else(|| "handshake did not complete".to_string());
        Verdict::unverified("network", detail)
    } else {
        Verdict::verified()
    };

    let mut out = verdict_json(verdict, host, port, negotiated, &chain);
    // Report the anchor inventory so a "verified" can be audited back
    // to what actually trusted it.
    out["roots"] = json!({"native": native_count, "extra": extra_count});
    out
}

/// rustls error -> (status, error_class).  Only a certificate that was
/// evaluated and rejected earns "warning"; transport/protocol failures
/// stay "unverified".
fn classify(err: &rustls::Error) -> Verdict {
    match err {
        rustls::Error::InvalidCertificate(ce) => {
            Verdict::warning(cert_class(ce), format!("{ce}"))
        }
        // Alerts, version/cipher negotiation failure, close-notify
        // surprises: the chain was not the failure.
        _ => Verdict::unverified("protocol", format!("{err}")),
    }
}

/// CertificateError -> the stable class string the Qt chip displays.
/// Keep the vocabulary small and documented in rustcore.h — new
/// variants land on "broken-chain" rather than inventing words.
fn cert_class(ce: &CertificateError) -> &'static str {
    use CertificateError as CE;
    match ce {
        CE::Expired | CE::ExpiredContext { .. } => "expired",
        CE::NotValidYet | CE::NotValidYetContext { .. } => "not-yet-valid",
        CE::NotValidForName | CE::NotValidForNameContext { .. } => "bad-hostname",
        CE::UnknownIssuer => "untrusted-root",
        CE::BadSignature => "broken-chain",
        // sha1/md5 and friends land here: webpki simply has no
        // algorithm implementation for them, so the chain cannot be
        // verified — that is the weak-signature class the task names.
        CE::UnsupportedSignatureAlgorithmContext { .. }
        | CE::UnsupportedSignatureAlgorithmForPublicKeyContext { .. } => "weak-signature",
        #[allow(deprecated)]
        CE::UnsupportedSignatureAlgorithm => "weak-signature",
        CE::Revoked => "revoked",
        CE::Other(inner) => other_cert_class(inner),
        _ => "broken-chain",
    }
}

/// The provider-wrapped error inside CertificateError::Other — the
/// ring/webpki signature rejects surface here rather than as named
/// variants on some rustls versions.
// Older webpki emits the deprecated unit variants alongside the
// Context ones; both spellings map to the same class.
#[allow(deprecated)]
fn other_cert_class(inner: &rustls::OtherError) -> &'static str {
    let e: &dyn std::error::Error = inner.0.as_ref();
    if let Some(w) = e.downcast_ref::<webpki::Error>() {
        use webpki::Error as WE;
        return match w {
            WE::InvalidSignatureForPublicKey
            | WE::UnsupportedSignatureAlgorithmForPublicKey
            | WE::UnsupportedSignatureAlgorithmForPublicKeyContext(_)
            | WE::UnsupportedSignatureAlgorithm
            | WE::UnsupportedSignatureAlgorithmContext(_) => "weak-signature",
            WE::UnknownIssuer => "untrusted-root",
            WE::CertExpired { .. } => "expired",
            WE::CertNotValidYet { .. } => "not-yet-valid",
            WE::CertNotValidForName(_) => "bad-hostname",
            _ => "broken-chain",
        };
    }
    "broken-chain"
}

/// The wire chain summarized for the site panel — subject, issuer,
/// validity window, signature algorithm OID, SANs.  Parse failures
/// are reported in-line rather than dropping the entry (a broken cert
/// is itself information).
fn chain_summary(certs: &[CertificateDer<'static>]) -> Vec<Value> {
    use x509_parser::prelude::*;
    certs
        .iter()
        .map(|der| match X509Certificate::from_der(der.as_ref()) {
            Ok((_, cert)) => {
                let sans: Vec<String> = cert
                    .subject_alternative_name()
                    .ok()
                    .flatten()
                    .map(|ext| {
                        ext.value
                            .general_names
                            .iter()
                            .map(|n| match n {
                                GeneralName::DNSName(d) => (*d).to_string(),
                                GeneralName::IPAddress(b) => format!("ip:{}", ip_str(b)),
                                GeneralName::RFC822Name(s) => format!("mail:{s}"),
                                GeneralName::URI(u) => format!("uri:{u}"),
                                _ => format!("{n:?}"),
                            })
                            .collect()
                    })
                    .unwrap_or_default();
                json!({
                    "subject": cert.subject().to_string(),
                    "issuer": cert.issuer().to_string(),
                    "serial": format!("{}", cert.serial),
                    "not_before": cert.validity().not_before.timestamp(),
                    "not_after": cert.validity().not_after.timestamp(),
                    "signature_algorithm":
                        cert.signature_algorithm.algorithm.to_id_string(),
                    "sans": sans,
                })
            }
            Err(e) => json!({"parse_error": format!("{e}")}),
        })
        .collect()
}

fn ip_str(b: &[u8]) -> String {
    match b.len() {
        4 => Ipv4Addr::new(b[0], b[1], b[2], b[3]).to_string(),
        16 => {
            let mut o = [0u8; 16];
            o.copy_from_slice(b);
            Ipv6Addr::from(o).to_string()
        }
        _ => b
            .iter()
            .map(|x| format!("{x:02x}"))
            .collect::<String>(),
    }
}

fn verdict_json(
    v: Verdict,
    host: &str,
    port: u16,
    negotiated: Option<Value>,
    chain: &[Value],
) -> Value {
    json!({
        "status": v.status,
        "error_class": v.error_class,
        "detail": v.detail,
        "host": host,
        "port": port,
        // Honest reporting: v1 does no OCSP/CRL fetch — the field is
        // spelled out so nobody mistakes the probe for revocation-
        // aware verification.
        "revocation": "not-checked",
        "negotiated": negotiated,
        "chain": chain,
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
    use rustls::{ServerConfig, ServerConnection};
    use std::net::TcpListener;
    use std::thread;

    // Fixture material generated by data/testcerts/gen_testcerts.py:
    // a scratch CA plus server leaves exercising every verdict class.
    const CA_DER: &[u8] = include_bytes!("../data/testcerts/ca.der");
    const VALID_CERT: &[u8] = include_bytes!("../data/testcerts/valid.der");
    const VALID_KEY: &[u8] = include_bytes!("../data/testcerts/valid-key.der");
    const EXPIRED_CERT: &[u8] = include_bytes!("../data/testcerts/expired.der");
    const EXPIRED_KEY: &[u8] = include_bytes!("../data/testcerts/expired-key.der");
    const NOTYET_CERT: &[u8] = include_bytes!("../data/testcerts/notyet.der");
    const NOTYET_KEY: &[u8] = include_bytes!("../data/testcerts/notyet-key.der");
    const WRONGHOST_CERT: &[u8] = include_bytes!("../data/testcerts/wronghost.der");
    const WRONGHOST_KEY: &[u8] = include_bytes!("../data/testcerts/wronghost-key.der");
    const SELFSIGNED_CERT: &[u8] = include_bytes!("../data/testcerts/selfsigned.der");
    const SELFSIGNED_KEY: &[u8] = include_bytes!("../data/testcerts/selfsigned-key.der");

    // The fixture probes share the process-global EXTRA_ROOTS — run
    // them serialized or a clear_roots() in a parallel test would
    // silently strip the anchor mid-handshake.
    static TLS_TEST_LOCK: Mutex<()> = Mutex::new(());

    fn guard() -> std::sync::MutexGuard<'static, ()> {
        TLS_TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    // Re-installs unconditionally (add_root dedups) — a serialized
    // clear_roots() in another test must not leave the CA missing.
    fn trust_fixture_ca() {
        add_root(CA_DER).expect("fixture CA installs");
    }

    /// Minimal TLS-terminating fixture: accepts one connection, runs
    /// the server handshake, then hangs up.  The client may abort on a
    /// cert error — server-side errors are expected and ignored.
    fn serve_once(cert: &[u8], key: &[u8]) -> (u16, thread::JoinHandle<()>) {
        let provider = Arc::new(rustls::crypto::ring::default_provider());
        let config = ServerConfig::builder_with_provider(provider)
            .with_safe_default_protocol_versions()
            .expect("server protocol versions")
            .with_no_client_auth()
            .with_single_cert(
                vec![CertificateDer::from(cert.to_vec())],
                PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(key.to_vec())),
            )
            .expect("server config");
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind");
        let port = listener.local_addr().unwrap().port();
        let handle = thread::spawn(move || {
            if let Ok((mut stream, _)) = listener.accept() {
                let _ = stream.set_read_timeout(Some(Duration::from_secs(10)));
                let _ = stream.set_write_timeout(Some(Duration::from_secs(10)));
                let mut conn = ServerConnection::new(Arc::new(config)).unwrap();
                while conn.is_handshaking() {
                    if conn.complete_io(&mut stream).is_err() {
                        break;
                    }
                }
            }
        });
        (port, handle)
    }

    fn probe(host: &str, port: u16) -> Value {
        check(host, port, FLAG_ALLOW_LOCAL)
    }

    #[test]
    fn private_host_rules() {
        for h in [
            "localhost", "foo.localhost", "printer.local", "deadbeef.onion",
            "127.0.0.1", "127.55.0.9", "10.1.2.3", "172.16.0.1", "172.31.9.9",
            "192.168.1.1", "169.254.1.1", "100.64.0.1", "198.18.0.1",
            "0.0.0.0", "::1", "fc00::1", "fe80::1", "[::1]",
            "224.0.0.1", "",
        ] {
            assert!(is_private_or_local(h), "{h} must be local");
        }
        for h in [
            "example.com", "8.8.8.8", "1.1.1.1", "172.15.0.1", "172.32.0.1",
            "100.63.0.1", "100.128.0.1", "198.17.0.1", "2606:4700:4700::1111",
            "localhost.example.com",
        ] {
            assert!(!is_private_or_local(h), "{h} must be public");
        }
    }

    #[test]
    fn refusal_without_flag() {
        let _g = guard();
        let v = check("127.0.0.1", 443, 0);
        assert_eq!(v["status"], "refused");
        assert_eq!(v["error_class"], "private-host");
        let v = check("localhost", 443, 0);
        assert_eq!(v["status"], "refused");
    }

    #[test]
    fn refused_bad_arguments() {
        let _g = guard();
        assert_eq!(check("", 443, FLAG_ALLOW_LOCAL)["status"], "refused");
        assert_eq!(check("example.com", 0, FLAG_ALLOW_LOCAL)["status"], "refused");
    }

    #[test]
    fn network_failure_is_unverified_not_warning() {
        let _g = guard();
        // Bind, take the port, close — the probe then hits a refused
        // connection, which must never render as a bad chain.
        let port = TcpListener::bind("127.0.0.1:0")
            .unwrap()
            .local_addr()
            .unwrap()
            .port();
        let v = probe("127.0.0.1", port);
        assert_eq!(v["status"], "unverified");
        assert_eq!(v["error_class"], "network");
    }

    #[test]
    fn valid_chain_verifies() {
        let _g = guard();
        trust_fixture_ca();
        let (port, server) = serve_once(VALID_CERT, VALID_KEY);
        let v = probe("localhost", port);
        server.join().unwrap();
        assert_eq!(v["status"], "verified", "{v}");
        assert_eq!(v["error_class"], Value::Null);
        let chain = v["chain"].as_array().unwrap();
        assert_eq!(chain.len(), 1);
        assert!(chain[0]["sans"]
            .as_array()
            .unwrap()
            .iter()
            .any(|s| s == "localhost"));
        assert!(v["negotiated"]["cipher_suite"].is_string());
        assert_eq!(v["revocation"], "not-checked");
    }

    #[test]
    fn expired_chain_warns() {
        let _g = guard();
        trust_fixture_ca();
        let (port, server) = serve_once(EXPIRED_CERT, EXPIRED_KEY);
        let v = probe("localhost", port);
        server.join().unwrap();
        assert_eq!(v["status"], "warning", "{v}");
        assert_eq!(v["error_class"], "expired");
        // The chain still summarizes — the user needs to see what failed.
        assert_eq!(v["chain"].as_array().unwrap().len(), 1);
    }

    #[test]
    fn not_yet_valid_chain_warns() {
        let _g = guard();
        trust_fixture_ca();
        let (port, server) = serve_once(NOTYET_CERT, NOTYET_KEY);
        let v = probe("localhost", port);
        server.join().unwrap();
        assert_eq!(v["status"], "warning", "{v}");
        assert_eq!(v["error_class"], "not-yet-valid");
    }

    #[test]
    fn wrong_host_chain_warns() {
        let _g = guard();
        trust_fixture_ca();
        let (port, server) = serve_once(WRONGHOST_CERT, WRONGHOST_KEY);
        let v = probe("localhost", port);
        server.join().unwrap();
        assert_eq!(v["status"], "warning", "{v}");
        assert_eq!(v["error_class"], "bad-hostname");
    }

    #[test]
    fn self_signed_chain_warns() {
        let _g = guard();
        trust_fixture_ca();
        let (port, server) = serve_once(SELFSIGNED_CERT, SELFSIGNED_KEY);
        let v = probe("localhost", port);
        server.join().unwrap();
        assert_eq!(v["status"], "warning", "{v}");
        assert_eq!(v["error_class"], "untrusted-root");
    }

    #[test]
    fn cert_class_mapping() {
        let cases: Vec<(CertificateError, &str)> = vec![
            (CertificateError::Expired, "expired"),
            (CertificateError::NotValidYet, "not-yet-valid"),
            (CertificateError::NotValidForName, "bad-hostname"),
            (CertificateError::UnknownIssuer, "untrusted-root"),
            (CertificateError::BadSignature, "broken-chain"),
            (CertificateError::BadEncoding, "broken-chain"),
            (CertificateError::Revoked, "revoked"),
        ];
        for (ce, want) in cases {
            assert_eq!(cert_class(&ce), want, "{ce:?}");
        }
        let err = rustls::Error::InvalidCertificate(CertificateError::Expired);
        let v = classify(&err);
        assert_eq!(v.status, "warning");
        let err = rustls::Error::PeerIncompatible(
            rustls::PeerIncompatible::Tls12NotOffered,
        );
        assert_eq!(classify(&err).status, "unverified");
    }

    #[test]
    fn extra_root_lifecycle() {
        let _g = guard();
        clear_roots().unwrap();
        add_root(CA_DER).unwrap();
        add_root(CA_DER).unwrap(); // dedup
        assert_eq!(EXTRA_ROOTS.lock().unwrap().len(), 1);
        assert!(add_root(b"not a cert").is_err());
        clear_roots().unwrap();
        assert!(EXTRA_ROOTS.lock().unwrap().is_empty());
    }
}
