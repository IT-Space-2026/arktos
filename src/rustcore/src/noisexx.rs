//! SYNC03 — the Noise_XX_25519_ChaChaPoly_SHA256 handshake the sync
//! pairing protocol rides.
//!
//! Noise_XX gives mutual authentication with unknown statics on both
//! sides: neither party needs the peer's static key in advance — each
//! static is delivered inside the handshake and authenticated by the
//! transcript hash and the application payload it carries (pairing
//! binds it to the Ed25519 device identity via a signature).
//!
//!   -> e                          (initiator ephemeral, plaintext)
//!   <- e, ee, s, es               (responder ephemeral, then its
//!                                  static + payload under the DH key)
//!   -> s, se                      (initiator static + payload)
//!
//! Everything after message 3 rides the two CipherStates produced by
//! split().  The transcript hash `h` commits to every exchanged byte —
//! pairing displays it as the word/emoji fingerprint both users
//! confirm, which is what turns XX into a MITM-proof ceremony.
//!
//! Primitives are the tree's reviewed providers: X25519 from
//! x25519-dalek (already in the lockfile for Arti), ChaCha20-Poly1305
//! + HMAC from ring / the hmac crate, SHA-256 from sha2.  No `snow`
//! dependency — the pattern is three message functions of ordinary
//! size, and staying on reviewed crates keeps the SEC21 pin policy
//! honest.  Implemented straight from the Noise spec (rev 34):
//! InitializeSymmetric / MixHash / MixKey / EncryptAndHash /
//! DecryptAndHash / Split, 96-bit nonces = u32 zeros || u64le counter.

use hmac::{Hmac, KeyInit as MacKeyInit, Mac};
use ring::aead::{Aad, LessSafeKey, Nonce, UnboundKey, CHACHA20_POLY1305};
use sha2::{Digest, Sha256};
use tor_llcrypto::pk::curve25519::{PublicKey, StaticSecret};

use crate::error::{fail, Fail, RcResult, RcStatus};

/// The Noise protocol name — longer than 32 bytes, so
/// InitializeSymmetric hashes it (spec rule), not pads.
const PROTOCOL_NAME: &[u8] = b"Noise_XX_25519_ChaChaPoly_SHA256";

const KEY_LEN: usize = 32;
const TAG_LEN: usize = 16;
/// Longest payload we accept inside one handshake/transport message —
/// pairing records are small JSON docs; this just bounds hostile
/// input before allocation.
pub const MAX_MESSAGE: usize = 64 * 1024;

fn crypto_fail(msg: impl Into<String>) -> Fail {
    Fail {
        status: RcStatus::Crypto,
        msg: msg.into(),
    }
}

fn sha256(parts: &[&[u8]]) -> [u8; 32] {
    let mut d = Sha256::new();
    for p in parts {
        d.update(p);
    }
    d.finalize().into()
}

/// The Noise spec's HKDF: salt = chaining key, ikm = input key
/// material, outputs T1|T2(|T3) each HASHLEN.  Written directly in
/// HMAC — three lines of spec, no extra crate surface.
fn hkdf(ck: &[u8; 32], ikm: &[u8], outputs: usize) -> [[u8; 32]; 3] {
    type H = Hmac<Sha256>;
    let mut temp = <H as MacKeyInit>::new_from_slice(ck).expect("HMAC takes any key length");
    temp.update(ikm);
    let temp = temp.finalize().into_bytes();
    let mut out = [[0u8; 32]; 3];
    let mut prev: Vec<u8> = Vec::new();
    for i in 0..outputs.max(1) {
        let mut h = <H as MacKeyInit>::new_from_slice(&temp).expect("HMAC takes any key length");
        h.update(&prev);
        h.update(&[(i + 1) as u8]);
        prev = h.finalize().into_bytes().to_vec();
        out[i].copy_from_slice(&prev);
    }
    out
}

fn nonce_for(n: u64) -> Nonce {
    let mut b = [0u8; 12];
    b[4..].copy_from_slice(&n.to_le_bytes());
    // The counter is a monotonically increasing per-CipherState value
    // — uniqueness per key is guaranteed by construction, which is
    // exactly what assume_unique_for_key asserts.
    Nonce::assume_unique_for_key(b)
}

/// One direction of a Noise transport (or the handshake's pending
/// cipher): a ChaCha20-Poly1305 key + monotone nonce counter.
struct CipherState {
    key: [u8; 32],
    n: u64,
}

impl CipherState {
    fn encrypt(&self, ad: &[u8], plaintext: &[u8]) -> RcResult<Vec<u8>> {
        let k = LessSafeKey::new(
            UnboundKey::new(&CHACHA20_POLY1305, &self.key)
                .map_err(|e| crypto_fail(format!("cipher key: {e}")))?,
        );
        let mut buf = plaintext.to_vec();
        k.seal_in_place_append_tag(nonce_for(self.n), Aad::from(ad), &mut buf)
            .map_err(|e| crypto_fail(format!("seal: {e}")))?;
        Ok(buf)
    }

    fn decrypt(&self, ad: &[u8], ciphertext: &[u8]) -> RcResult<Vec<u8>> {
        if ciphertext.len() < TAG_LEN {
            return fail(RcStatus::Crypto, "ciphertext shorter than tag");
        }
        let k = LessSafeKey::new(
            UnboundKey::new(&CHACHA20_POLY1305, &self.key)
                .map_err(|e| crypto_fail(format!("cipher key: {e}")))?,
        );
        let mut buf = ciphertext.to_vec();
        let pt = k
            .open_in_place(nonce_for(self.n), Aad::from(ad), &mut buf)
            .map_err(|_| crypto_fail("handshake payload failed AEAD authentication"))?;
        Ok(pt.to_vec())
    }
}

/// ck/h chaining plus the pending cipher — the spec's
/// SymmetricState.
struct SymmetricState {
    ck: [u8; 32],
    h: [u8; 32],
    k: Option<CipherState>,
}

impl SymmetricState {
    fn new(prologue: &[u8]) -> SymmetricState {
        let h: [u8; 32] = if PROTOCOL_NAME.len() <= KEY_LEN {
            let mut b = [0u8; 32];
            b[..PROTOCOL_NAME.len()].copy_from_slice(PROTOCOL_NAME);
            b
        } else {
            sha256(&[PROTOCOL_NAME])
        };
        let mut s = SymmetricState {
            ck: h,
            h,
            k: None,
        };
        s.mix_hash(prologue);
        s
    }

    fn mix_hash(&mut self, data: &[u8]) {
        self.h = sha256(&[&self.h, data]);
    }

    fn mix_key(&mut self, ikm: &[u8]) {
        let out = hkdf(&self.ck, ikm, 2);
        self.ck = out[0];
        self.k = Some(CipherState {
            key: out[1],
            n: 0,
        });
    }

    fn encrypt_and_hash(&mut self, plaintext: &[u8]) -> RcResult<Vec<u8>> {
        let ct = match &mut self.k {
            Some(c) => {
                let ct = c.encrypt(&self.h, plaintext)?;
                c.n += 1;
                ct
            }
            None => plaintext.to_vec(),
        };
        self.mix_hash(&ct);
        Ok(ct)
    }

    fn decrypt_and_hash(&mut self, ciphertext: &[u8]) -> RcResult<Vec<u8>> {
        let pt = match &mut self.k {
            Some(c) => {
                let pt = c.decrypt(&self.h, ciphertext)?;
                c.n += 1;
                pt
            }
            None => ciphertext.to_vec(),
        };
        self.mix_hash(ciphertext);
        Ok(pt)
    }

    fn split(self) -> (CipherState, CipherState) {
        let out = hkdf(&self.ck, &[], 2);
        (
            CipherState {
                key: out[0],
                n: 0,
            },
            CipherState {
                key: out[1],
                n: 0,
            },
        )
    }
}

fn dh(secret: &StaticSecret, public: &PublicKey) -> [u8; 32] {
    *secret.diffie_hellman(public).as_bytes()
}

/// Fill `buf` with OS entropy through the tree's reviewed RNG.
pub fn rand_bytes<const N: usize>() -> RcResult<[u8; N]> {
    let mut b = [0u8; N];
    getrandom::getrandom(&mut b).map_err(|e| Fail {
        status: RcStatus::Unavailable,
        msg: format!("system rng: {e}"),
    })?;
    Ok(b)
}

/// The XX handshake state machine.  Construct as initiator or
/// responder with the SAME prologue (pairing's invite token), then
/// drive the three messages; into_transport() yields the two
/// directional CipherStates plus the transcript fingerprint.
///
/// Every message is one byte string — the caller maps them 1:1 onto
/// transport frames (synctransport's <len><payload> framing carries
/// them untouched).
pub struct Handshake {
    sym: SymmetricState,
    initiator: bool,
    /// Our static — the signed transport subkey pairing issues.
    s: StaticSecret,
    e: Option<StaticSecret>,
    re: Option<PublicKey>,
    rs: Option<PublicKey>,
}

impl Handshake {
    fn new(initiator: bool, prologue: &[u8], our_static: StaticSecret) -> Handshake {
        Handshake {
            sym: SymmetricState::new(prologue),
            initiator,
            s: our_static,
            e: None,
            re: None,
            rs: None,
        }
    }

    /// Initiator: prologue is the invite token the QR carried.
    pub fn initiator(prologue: &[u8], our_static: StaticSecret) -> Handshake {
        Handshake::new(true, prologue, our_static)
    }

    /// Responder: prologue is the token recovered from message 1's
    /// plaintext payload (peek_msg1_payload).
    pub fn responder(prologue: &[u8], our_static: StaticSecret) -> Handshake {
        Handshake::new(false, prologue, our_static)
    }

    /// The raw payload section of a message-1 frame — plaintext at
    /// msg[32..].  The responder reads the invite token out of it to
    /// select the prologue BEFORE constructing the Handshake.
    pub fn peek_msg1_payload(msg: &[u8]) -> RcResult<&[u8]> {
        if msg.len() < KEY_LEN || msg.len() > MAX_MESSAGE {
            return fail(
                RcStatus::Crypto,
                format!("bad handshake message 1 length {}", msg.len()),
            );
        }
        Ok(&msg[KEY_LEN..])
    }

    /// -> e  (msg1 = e_pub || payload, all plaintext)
    pub fn write_msg1(&mut self, payload: &[u8]) -> RcResult<Vec<u8>> {
        if !self.initiator {
            return fail(RcStatus::InvalidArgument, "write_msg1 is initiator-side");
        }
        let e = StaticSecret::from(rand_bytes::<32>()?);
        let e_pub = PublicKey::from(&e);
        self.e = Some(e);
        let mut msg = Vec::with_capacity(KEY_LEN + payload.len());
        msg.extend_from_slice(e_pub.as_bytes());
        self.sym.mix_hash(e_pub.as_bytes());
        msg.extend_from_slice(&self.sym.encrypt_and_hash(payload)?);
        Ok(msg)
    }

    /// Responder reads msg1: mix e_pub, decrypt+hash the (plaintext)
    /// payload — returns it for the app layer's token consistency
    /// check.
    pub fn read_msg1(&mut self, msg: &[u8]) -> RcResult<Vec<u8>> {
        if self.initiator {
            return fail(RcStatus::InvalidArgument, "read_msg1 is responder-side");
        }
        let payload = Self::peek_msg1_payload(msg)?.to_vec();
        let mut e_pub = [0u8; 32];
        e_pub.copy_from_slice(&msg[..KEY_LEN]);
        self.re = Some(PublicKey::from(e_pub));
        self.sym.mix_hash(&e_pub);
        self.sym.decrypt_and_hash(&payload)
    }

    /// <- e, ee, s, es  (msg2 = e_pub || enc_s || enc_payload)
    pub fn write_msg2(&mut self, payload: &[u8]) -> RcResult<Vec<u8>> {
        if self.initiator {
            return fail(RcStatus::InvalidArgument, "write_msg2 is responder-side");
        }
        let re = self.re.ok_or_else(|| crypto_fail("msg2 before msg1"))?;
        let e = StaticSecret::from(rand_bytes::<32>()?);
        let e_pub = PublicKey::from(&e);
        let mut msg = Vec::with_capacity(KEY_LEN + KEY_LEN + TAG_LEN + payload.len() + TAG_LEN);
        msg.extend_from_slice(e_pub.as_bytes());
        self.sym.mix_hash(e_pub.as_bytes());
        self.sym.mix_key(&dh(&e, &re));
        msg.extend_from_slice(
            &self.sym.encrypt_and_hash(self.static_public().as_bytes())?,
        );
        self.sym.mix_key(&dh(&self.s, &re));
        msg.extend_from_slice(&self.sym.encrypt_and_hash(payload)?);
        self.e = Some(e);
        Ok(msg)
    }

    /// Initiator reads msg2 — learns the responder static (rs).
    pub fn read_msg2(&mut self, msg: &[u8]) -> RcResult<Vec<u8>> {
        if !self.initiator {
            return fail(RcStatus::InvalidArgument, "read_msg2 is initiator-side");
        }
        let want = KEY_LEN + KEY_LEN + TAG_LEN;
        if msg.len() < want || msg.len() > MAX_MESSAGE {
            return fail(
                RcStatus::Crypto,
                format!("bad handshake message 2 length {}", msg.len()),
            );
        }
        let mut e_pub = [0u8; 32];
        e_pub.copy_from_slice(&msg[..KEY_LEN]);
        self.re = Some(PublicKey::from(e_pub));
        self.sym.mix_hash(&e_pub);
        let e = self.e.take().ok_or_else(|| crypto_fail("msg2 before msg1"))?;
        self.sym.mix_key(&dh(&e, &PublicKey::from(e_pub)));
        let s_ct = &msg[KEY_LEN..KEY_LEN + KEY_LEN + TAG_LEN];
        let s_pt = self.sym.decrypt_and_hash(s_ct)?;
        let mut rs = [0u8; 32];
        rs.copy_from_slice(&s_pt);
        self.rs = Some(PublicKey::from(rs));
        self.sym.mix_key(&dh(&e, &PublicKey::from(rs)));
        self.sym.decrypt_and_hash(&msg[want..])
    }

    /// -> s, se  (msg3 = enc_s || enc_payload)
    pub fn write_msg3(&mut self, payload: &[u8]) -> RcResult<Vec<u8>> {
        if !self.initiator {
            return fail(RcStatus::InvalidArgument, "write_msg3 is initiator-side");
        }
        let re = self.re.ok_or_else(|| crypto_fail("msg3 before msg2"))?;
        let mut msg = Vec::with_capacity(KEY_LEN + TAG_LEN + payload.len() + TAG_LEN);
        msg.extend_from_slice(
            &self.sym.encrypt_and_hash(self.static_public().as_bytes())?,
        );
        self.sym.mix_key(&dh(&self.s, &re));
        msg.extend_from_slice(&self.sym.encrypt_and_hash(payload)?);
        Ok(msg)
    }

    /// Responder reads msg3 — learns the initiator static (rs).
    pub fn read_msg3(&mut self, msg: &[u8]) -> RcResult<Vec<u8>> {
        if self.initiator {
            return fail(RcStatus::InvalidArgument, "read_msg3 is responder-side");
        }
        let want = KEY_LEN + TAG_LEN;
        if msg.len() < want || msg.len() > MAX_MESSAGE {
            return fail(
                RcStatus::Crypto,
                format!("bad handshake message 3 length {}", msg.len()),
            );
        }
        let s_pt = self.sym.decrypt_and_hash(&msg[..want])?;
        let mut rs = [0u8; 32];
        rs.copy_from_slice(&s_pt);
        self.sym.mix_key(&dh(&re_key(&self.e), &PublicKey::from(rs)));
        self.rs = Some(PublicKey::from(rs));
        self.sym.decrypt_and_hash(&msg[want..])
    }

    /// Our static public key — the bytes a peer verifies against the
    /// Ed25519 signature in the pairing payload.
    pub fn static_public(&self) -> PublicKey {
        PublicKey::from(&self.s)
    }

    /// The peer's static after it arrived (msg2 for the initiator,
    /// msg3 for the responder).
    pub fn remote_static(&self) -> Option<PublicKey> {
        self.rs
    }

    /// Handshake complete: split into the two directional transport
    /// ciphers.  Initiator sends on the first, responder on the
    /// second — the Noise convention.
    pub fn into_transport(self) -> NoiseSession {
        let fingerprint = self.sym.h;
        let (c1, c2) = self.sym.split();
        let (send, recv) = if self.initiator {
            (c1, c2)
        } else {
            (c2, c1)
        };
        NoiseSession {
            send,
            recv,
            fingerprint,
        }
    }
}

/// se/es DH on the responder side uses our OWN ephemeral secret —
/// pulled out of the Option with a loud invariant rather than a
/// silent wrong-key.
fn re_key(e: &Option<StaticSecret>) -> &StaticSecret {
    e.as_ref().expect("Noise responder ephemeral unset")
}

/// Post-handshake duplex: authenticated encryption in both
/// directions plus the transcript fingerprint both users compare.
pub struct NoiseSession {
    send: CipherState,
    recv: CipherState,
    /// SHA-256 transcript of the entire handshake — identical on
    /// both sides only when there was no man in the middle.
    pub fingerprint: [u8; 32],
}

impl NoiseSession {
    /// One sealed transport record — the aad is a domain tag, not
    /// the transcript (post-split the spec drops the ad entirely;
    /// the tag is our framing label and is itself authenticated).
    pub fn seal(&mut self, plaintext: &[u8]) -> RcResult<Vec<u8>> {
        let ct = self.send.encrypt(b"arktos-pair-record", plaintext)?;
        self.send.n += 1;
        Ok(ct)
    }

    pub fn open(&mut self, ciphertext: &[u8]) -> RcResult<Vec<u8>> {
        let pt = self.recv.decrypt(b"arktos-pair-record", ciphertext)?;
        self.recv.n += 1;
        Ok(pt)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn keypair() -> (StaticSecret, PublicKey) {
        let s = StaticSecret::from(rand_bytes::<32>().unwrap());
        let p = PublicKey::from(&s);
        (s, p)
    }

    /// A straight-through XX: all three messages, payload round
    /// trips, identical transcripts both sides, sealed records
    /// exchanging in both directions.
    #[test]
    fn handshake_round_trip() {
        let (si, _pi) = keypair();
        let (sr, _pr) = keypair();
        let prologue = b"invite-token-32-bytes-padded-here!!!!";
        let mut i = Handshake::initiator(prologue, si);
        let mut r = Handshake::responder(prologue, sr);

        let m1 = i.write_msg1(b"hello-responder").unwrap();
        let i_e_pub = PublicKey::from(i.e.as_ref().unwrap());
        assert_eq!(&m1[..32], i_e_pub.as_bytes());
        assert_eq!(Handshake::peek_msg1_payload(&m1).unwrap(), b"hello-responder");
        assert_eq!(r.read_msg1(&m1).unwrap(), b"hello-responder");

        let m2 = r.write_msg2(b"hello-initiator").unwrap();
        assert_eq!(i.read_msg2(&m2).unwrap(), b"hello-initiator");
        assert!(i.remote_static().is_some());

        let m3 = i.write_msg3(b"initiator-identity").unwrap();
        assert_eq!(r.read_msg3(&m3).unwrap(), b"initiator-identity");
        assert!(r.remote_static().is_some());

        let mut ti = i.into_transport();
        let mut tr = r.into_transport();
        assert_eq!(ti.fingerprint, tr.fingerprint);

        let ct = ti.seal(b"record one").unwrap();
        assert_eq!(tr.open(&ct).unwrap(), b"record one");
        let ct = tr.seal(b"record two").unwrap();
        assert_eq!(ti.open(&ct).unwrap(), b"record two");
        // Counters advance — the same plaintext seals differently.
        let a = ti.seal(b"same").unwrap();
        let b = ti.seal(b"same").unwrap();
        assert_ne!(a, b);
    }

    /// A wrong prologue (a swapped/photographed-then-edited invite
    /// token) diverges the transcript: AEAD opens fail and, failing
    /// that, the fingerprints never match.
    #[test]
    fn wrong_prologue_breaks_handshake() {
        let (si, _pi) = keypair();
        let (sr, _pr) = keypair();
        let mut i = Handshake::initiator(b"token-A", si);
        let mut r = Handshake::responder(b"token-B", sr);

        let m1 = i.write_msg1(b"").unwrap();
        r.read_msg1(&m1).unwrap();
        let m2 = r.write_msg2(b"").unwrap();
        // The responder static arrives under a key the initiator
        // never computes — open must fail.
        assert!(i.read_msg2(&m2).is_err());
    }

    /// Bit-flipped ciphertext is an AEAD rejection, never a
    /// plaintext mutation.
    #[test]
    fn tampered_message_rejected() {
        let (si, _pi) = keypair();
        let (sr, _pr) = keypair();
        let prologue = b"tok";
        let mut i = Handshake::initiator(prologue, si);
        let mut r = Handshake::responder(prologue, sr);
        let m1 = i.write_msg1(b"x").unwrap();
        r.read_msg1(&m1).unwrap();
        let mut m2 = r.write_msg2(b"payload").unwrap();
        let last = m2.len() - 1;
        m2[last] ^= 0x01;
        assert!(i.read_msg2(&m2).is_err());
    }

    /// Empty payloads are legal at every step (the prologue alone
    /// authenticates message 1).
    #[test]
    fn empty_payloads_ok() {
        let (si, _pi) = keypair();
        let (sr, _pr) = keypair();
        let p = b"";
        let mut i = Handshake::initiator(p, si);
        let mut r = Handshake::responder(p, sr);
        let m1 = i.write_msg1(b"").unwrap();
        r.read_msg1(&m1).unwrap();
        let m2 = r.write_msg2(b"").unwrap();
        i.read_msg2(&m2).unwrap();
        let m3 = i.write_msg3(b"").unwrap();
        r.read_msg3(&m3).unwrap();
        let a = i.into_transport();
        let b = r.into_transport();
        assert_eq!(a.fingerprint, b.fingerprint);
    }

    /// Truncated/malformed frames are clean Crypto errors, never a
    /// panic or an out-of-bounds slice.
    #[test]
    fn malformed_frames_refused() {
        let (sr, _pr) = keypair();
        let mut r = Handshake::responder(b"tok", sr);
        assert!(r.read_msg1(&[0u8; 31]).is_err());
        assert!(r.read_msg1(&[]).is_err());
        assert!(r.read_msg3(&[0u8; 10]).is_err());
    }
}
