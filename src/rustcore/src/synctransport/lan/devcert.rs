//! SYNC10 — the LAN device certificate.
//!
//! Every LAN link is a QUIC (TLS 1.3) connection where BOTH peers
//! present a self-signed Ed25519 certificate whose public key IS the
//! sync device identity (`sync/device.key`, SYNC01).  There is no CA
//! and no cert chain: the peer-side verifier extracts the SPKI key,
//! maps it to a device id, and gates on this profile's authorized
//! member set — the LAN equivalent of tor's restricted-discovery
//! refusal, at the TLS layer before a single app byte.
//!
//! The cert is deliberately minimal — a fixed-shape DER document we
//! emit and parse ourselves.  Validity dates are nominal (the
//! verifier ignores them; the device key IS the credential, so a
//! notAfter would only strand paired devices).

use ring::signature::{Ed25519KeyPair, KeyPair};
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use x509_parser::prelude::FromDer;

/// OID 1.3.101.112 — Ed25519 signature/public-key algorithm (RFC 8410).
const OID_ED25519: &[u8] = &[0x06, 0x03, 0x2B, 0x65, 0x70];
/// OID 2.5.4.3 — commonName.
const OID_CN: &[u8] = &[0x06, 0x03, 0x55, 0x04, 0x03];

fn der_len(n: usize) -> Vec<u8> {
    if n < 0x80 {
        vec![n as u8]
    } else if n < 0x100 {
        vec![0x81, n as u8]
    } else {
        vec![0x82, (n >> 8) as u8, n as u8]
    }
}

fn der_tlv(tag: u8, body: &[u8]) -> Vec<u8> {
    let mut v = Vec::with_capacity(body.len() + 4);
    v.push(tag);
    v.extend_from_slice(&der_len(body.len()));
    v.extend_from_slice(body);
    v
}

fn der_seq(body: &[u8]) -> Vec<u8> {
    der_tlv(0x30, body)
}

fn der_set(body: &[u8]) -> Vec<u8> {
    der_tlv(0x31, body)
}

fn der_int(n: u64) -> Vec<u8> {
    let bytes = n.to_be_bytes();
    let first = bytes
        .iter()
        .position(|b| *b != 0)
        .unwrap_or(bytes.len() - 1);
    let mut body = bytes[first..].to_vec();
    if body[0] & 0x80 != 0 {
        body.insert(0, 0);
    }
    der_tlv(0x02, &body)
}

fn der_oid(oid_tlv: &[u8]) -> Vec<u8> {
    oid_tlv.to_vec()
}

fn der_utf8(s: &str) -> Vec<u8> {
    der_tlv(0x0C, s.as_bytes())
}

fn der_utctime(s: &str) -> Vec<u8> {
    der_tlv(0x17, s.as_bytes())
}

fn der_bitstring(bytes: &[u8]) -> Vec<u8> {
    let mut body = Vec::with_capacity(bytes.len() + 1);
    body.push(0); // unused-bits octet
    body.extend_from_slice(bytes);
    der_tlv(0x03, &body)
}

/// `CN=<cn>` as a one-RDN X.501 Name.
fn der_name(cn: &str) -> Vec<u8> {
    let atv = der_seq(&[der_oid(OID_CN), der_utf8(cn)].concat());
    der_seq(&der_set(&atv))
}

/// Build the self-signed Ed25519 device certificate for `key`,
/// carrying `cn` ("arktos-<id16>") as subject+issuer.  The returned
/// DER is a complete X.509 Certificate; the signing key never leaves
/// the caller.
pub fn device_cert_der(key: &Ed25519KeyPair, cn: &str) -> CertificateDer<'static> {
    let pubkey = key.public_key().as_ref();
    let alg = der_seq(&der_oid(OID_ED25519));
    let spki = der_seq(&[alg.clone(), der_bitstring(pubkey)].concat());
    let validity = der_seq(
        &[der_utctime("250101000000Z"), der_utctime("490101000000Z")].concat(),
    );
    let name = der_name(cn);
    let tbs = der_seq(
        &[
            der_tlv(0xA0, &der_int(2)), // [0] EXPLICIT version v3
            der_int(1),                 // serial
            alg.clone(),                // signature alg
            name.clone(),               // issuer
            validity,
            name,                       // subject
            spki,
        ]
        .concat(),
    );
    let sig = key.sign(&tbs);
    let cert = der_seq(&[tbs, alg, der_bitstring(sig.as_ref())].concat());
    CertificateDer::from(cert)
}

/// The PKCS#8 private key form rustls wants, re-encoded from the
/// live keypair (device.key is already persisted as PKCS#8 — the
/// caller passes those same bytes through).
pub fn device_key_der(pkcs8: &[u8]) -> PrivateKeyDer<'static> {
    PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(pkcs8.to_vec()))
}

/// Extract the Ed25519 public key from a peer's certificate DER.
/// Returns `None` on malformed input or a non-Ed25519 SPKI — the
/// caller treats that as an authentication failure, never a bypass.
pub fn cert_pubkey(der: &[u8]) -> Option<[u8; 32]> {
    let (_, cert) = x509_parser::certificate::X509Certificate::from_der(der).ok()?;
    let spki = &cert.tbs_certificate.subject_pki;
    if spki.algorithm.algorithm.to_id_string() != "1.3.101.112" {
        return None;
    }
    let raw: &[u8] = spki.subject_public_key.data.as_ref();
    if raw.len() != 32 {
        return None;
    }
    let mut pk = [0u8; 32];
    pk.copy_from_slice(raw);
    Some(pk)
}

/// The cert is self-consistent: its signature verifies under its own
/// SPKI key.  Belt-and-braces — the TLS handshake signature is the
/// actual auth proof, but a cert that fails this is junk regardless.
pub fn cert_self_signed_ok(der: &[u8]) -> bool {
    let Some(pk) = cert_pubkey(der) else { return false };
    let Ok((_, cert)) = x509_parser::certificate::X509Certificate::from_der(der)
    else {
        return false;
    };
    let key = ring::signature::UnparsedPublicKey::new(&ring::signature::ED25519, pk);
    key.verify(
        cert.tbs_certificate.as_ref(),
        cert.signature_value.data.as_ref(),
    )
    .is_ok()
}

// ---- tests -------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use ring::rand::SystemRandom;

    fn gen() -> (Ed25519KeyPair, Vec<u8>) {
        let doc = Ed25519KeyPair::generate_pkcs8(&SystemRandom::new()).unwrap();
        let pair = Ed25519KeyPair::from_pkcs8(doc.as_ref()).unwrap();
        (pair, doc.as_ref().to_vec())
    }

    #[test]
    fn cert_round_trips_spki() {
        let (pair, _pkcs8) = gen();
        let der = device_cert_der(&pair, "arktos-0123456789abcdef");
        let pk = cert_pubkey(der.as_ref()).expect("SPKI unreadable");
        assert_eq!(&pk, pair.public_key().as_ref());
    }

    #[test]
    fn cert_parses_as_x509_and_self_signed() {
        let (pair, _) = gen();
        let der = device_cert_der(&pair, "arktos-feedfacecafebeed");
        let (_, cert) =
            x509_parser::certificate::X509Certificate::from_der(der.as_ref()).unwrap();
        assert_eq!(cert.tbs_certificate.subject, cert.tbs_certificate.issuer);
        assert!(cert
            .tbs_certificate
            .subject
            .to_string()
            .contains("arktos-feedfacecafebeed"));
        // The cert signature verifies under its own SPKI key.
        assert!(cert_self_signed_ok(der.as_ref()));
        // Junk and foreign-key material refuse cleanly.
        assert!(cert_pubkey(b"not der").is_none());
        assert!(cert_pubkey(&der.as_ref()[..der.as_ref().len() - 8]).is_none());
        assert!(!cert_self_signed_ok(b"not der"));
    }
}
