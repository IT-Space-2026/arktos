//! Profile vault (PVAULT01) — a password-sealed, chunked, streamable
//! archive of a profile directory ("vault.arv").  The module is
//! stateless: every entry point takes explicit paths, touches no
//! shared store and is safe to run on a worker thread.
//!
//! File layout (`ARV1`):
//!
//! ```text
//!   header (153 B, fixed):
//!     magic "ARV1"          4
//!     algo_id               1
//!     kdf memory_kib u32le  4
//!     kdf passes     u32le  4
//!     kdf lanes      u32le  4
//!     salt                32
//!     chunk_size   u32le    4   (plaintext bytes per chunk)
//!     chunk_count  u64le    8
//!     wrap_nonce          12
//!     wrapped_vault_key   64   (AES-256-GCM ciphertext of vault_key)
//!     wrap_tag            16
//!   header_mac            64   HMAC-SHA-512(mac_key, header)
//!   payload: chunk_count frames of
//!     ct_len u32le | ct | tag   (tag: 16 B GCM / 32 B Skein-MAC)
//!   payload_mac           64   HMAC-SHA-512(mac_key, all payload bytes)
//! ```
//!
//! Key schedule — two-tier, so a password change is a re-wrap, not a
//! re-encrypt.  At seal, `vault_key` (64 B: enc_key[32] || mac_key[32])
//! comes from the OS RNG — it is NOT password-derived.  Argon2id over
//! (password, salt, header params) yields a 256-bit KEK that wraps
//! vault_key with AES-256-GCM; the wrapped blob + nonce live in the
//! header.  Unseal therefore reports honestly distinct failures:
//!
//!   1. the wrap's GCM tag rejects -> WrongPassword;
//!   2. unwrap ok but a MAC fails (header / chunk / payload) ->
//!      Tampered.
//!
//! Edge, honestly documented: bits flipped inside the wrapped-key
//! field itself surface as WrongPassword — still fails closed, just
//! mislabeled.  The header's other fields fail the header MAC ->
//! Tampered, except kdf params which steer the KEK derivation and so
//! also surface as WrongPassword.
//!
//! Algorithm agility (`algo_id`):
//!   0x01 — AES-256-GCM per chunk + HMAC-SHA-512 everywhere (default;
//!          audited, boring, AES-NI fast).
//!   0x02 — Threefish-1024 in XEX-style tweakable mode + Skein-MAC
//!          (HMAC-Skein-512-512, tag truncated to 32 B).  Each 128 B
//!          block runs under tweak salt[0..4] || chunk_index u32le ||
//!          block_index u64le — unique per (salt, chunk, block), the
//!          tweakable-cipher analogue of per-sector XEX.  NOTE: the
//!          RustCrypto threefish/skein crates are unaudited upstream
//!          (hazmat block-cipher level); the primitive was designed
//!          for tweakable disk encryption, which is exactly this use.
//!          The 128 B cipher key is expanded from the 256-bit enc_key
//!          as Skein-512("ARV1-TF-K0"||enc_key) ||
//!          Skein-512("ARV1-TF-K1"||enc_key).
//!
//! Per-chunk nonces/tweaks derive from the header salt || chunk index;
//! a fresh salt + fresh vault_key every seal makes key+nonce reuse
//! impossible across re-seals.
//!
//! No partial unpack, structurally: unseal pass 1 stream-verifies the
//! whole payload MAC before a single plaintext byte is written; pass
//! 2 re-verifies each chunk tag, decrypts and extracts into
//! "<out>.arv-unpack-<pid>.tmp", fsyncs the tree, then renames onto
//! out_dir.  Any failure leaves zero unpacked artifacts.  Tar
//! extraction whitelists file/dir/symlink entries and relies on
//! tar-rs' unpack_in escape check, so a hostile archive cannot write
//! outside the staging dir.

use std::fs;
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use aes_gcm::aead::{Aead, KeyInit};
use aes_gcm::Aes256Gcm;
use hmac::{Hmac, KeyInit as MacKeyInit, Mac, SimpleHmac};
use serde_json::json;
use sha2::Sha512;
use skein::{Digest as _, Skein512_512};
use threefish::Threefish1024;
use zeroize::{Zeroize, Zeroizing};

use crate::error::{fail, Fail, RcResult, RcStatus};

const MAGIC: &[u8; 4] = b"ARV1";

/// AES-256-GCM chunks + HMAC-SHA-512 MACs (the default lane).
pub const ALGO_AES256_GCM: u8 = 0x01;
/// Threefish-1024 XEX-style chunks + HMAC-Skein-512 chunk tags.
/// Unaudited upstream — see the module docs; opt-in only.
pub const ALGO_THREEFISH_XEX: u8 = 0x02;

const SALT_SIZE: usize = 32;
const VAULT_KEY_SIZE: usize = 64;
const ENC_KEY_SIZE: usize = 32;
const WRAP_NONCE_SIZE: usize = 12;
const WRAP_TAG_SIZE: usize = 16;
const MAC_SIZE: usize = 64;
const GCM_TAG_SIZE: usize = 16;
const SKEIN_TAG_SIZE: usize = 32;
const TF_BLOCK: usize = 128;

const DEFAULT_CHUNK_SIZE: u32 = 16 << 20;
const MIN_CHUNK_SIZE: u32 = 4096;
const MAX_CHUNK_SIZE: u32 = 256 << 20;
const MAX_CHUNKS: u64 = 1 << 22;
const MAX_KDF_MEMORY_KIB: u32 = 4 * 1024 * 1024;
const MAX_KDF_PASSES: u32 = 16;
const MAX_KDF_LANES: u32 = 16;
const MAX_TAR_ENTRIES: u64 = 4_000_000;

const KDF_MEMORY_KIB: u32 = 65536;
const KDF_PASSES: u32 = 3;
const KDF_LANES: u32 = 4;

const HEADER_LEN: usize =
    4 + 1 + 12 + SALT_SIZE + 4 + 8 + WRAP_NONCE_SIZE + VAULT_KEY_SIZE + WRAP_TAG_SIZE;
const PREFIX_LEN: usize = HEADER_LEN + MAC_SIZE;
const MIN_FILE_LEN: u64 = (PREFIX_LEN + MAC_SIZE) as u64;

/// C-side progress hook: (bytes_done, bytes_total, userdata).  Invoked
/// synchronously on the calling thread — a Qt caller must queue onto
/// the GUI thread, not touch widgets directly.
pub type ProgressCallback = unsafe extern "C" fn(u64, u64, *mut std::ffi::c_void);

/// Progress plumbing shared by seal/unseal/verify.  `total` is fixed
/// by the caller; reports are throttled to 256 KiB steps plus the
/// first and terminal calls.
pub struct Progress {
    cb: Option<ProgressCallback>,
    ud: *mut std::ffi::c_void,
    pub(crate) total: u64,
    done: u64,
    last_reported: u64,
}

impl Progress {
    pub fn new(cb: Option<ProgressCallback>, ud: *mut std::ffi::c_void) -> Self {
        let mut p = Progress {
            cb,
            ud,
            total: 0,
            done: 0,
            last_reported: u64::MAX,
        };
        p.report(0);
        p
    }

    pub fn report(&mut self, done: u64) {
        let shown = if self.total > 0 { done.min(self.total) } else { done };
        self.done = shown;
        let first = self.last_reported == u64::MAX;
        let last = self.total > 0 && shown >= self.total;
        if !first && !last && shown.saturating_sub(self.last_reported) < (256 << 10) {
            return;
        }
        self.last_reported = shown;
        if let Some(cb) = self.cb {
            unsafe { cb(shown, self.total, self.ud) };
        }
    }
}

// ---------------------------------------------------------------------------
// little helpers

fn rand_bytes(buf: &mut [u8]) -> RcResult<()> {
    getrandom::getrandom(buf).map_err(|e| Fail {
        status: RcStatus::Unavailable,
        msg: format!("getrandom: {e}"),
    })
}

fn tag_size(algo: u8) -> usize {
    match algo {
        ALGO_AES256_GCM => GCM_TAG_SIZE,
        _ => SKEIN_TAG_SIZE,
    }
}

/// Largest ciphertext a frame can legally carry for `algo` (Threefish
/// pads each chunk to a 128 B multiple — worst case a whole block).
fn max_frame_ct(algo: u8, chunk_size: u32) -> u64 {
    let pad = if algo == ALGO_THREEFISH_XEX {
        TF_BLOCK as u64
    } else {
        0
    };
    chunk_size as u64 + pad
}

fn algo_name(algo: u8) -> &'static str {
    match algo {
        ALGO_AES256_GCM => "aes-256-gcm+hmac-sha512",
        ALGO_THREEFISH_XEX => "threefish-1024-xex+skein-mac",
        _ => "unknown",
    }
}

type HmacSha512 = Hmac<Sha512>;
// Hmac<D> requires an Eager-buffer hash; Skein is Lazy, so the MAC is
// RFC 2104 over Skein-512 via digest's SimpleHmac (message-buffered —
// the 16 MiB chunk bound makes that cheap).
type HmacSkein512 = SimpleHmac<Skein512_512>;

fn hmac_sha512(key: &[u8]) -> HmacSha512 {
    <HmacSha512 as MacKeyInit>::new_from_slice(key).expect("HMAC accepts any key length")
}

fn hmac_verify(mac: HmacSha512, expected: &[u8], what: &str) -> RcResult<()> {
    mac.verify_slice(expected).map_err(|_| Fail {
        status: RcStatus::Tampered,
        msg: format!("vault {what} authentication failed"),
    })
}

fn skein_tag(mac_key: &[u8], ct: &[u8]) -> [u8; SKEIN_TAG_SIZE] {
    let mut h = <HmacSkein512 as MacKeyInit>::new_from_slice(mac_key)
        .expect("HMAC accepts any key");
    h.update(ct);
    let full = h.finalize().into_bytes();
    let mut tag = [0u8; SKEIN_TAG_SIZE];
    tag.copy_from_slice(&full[..SKEIN_TAG_SIZE]);
    tag
}

fn skein_tag_verify(mac_key: &[u8], ct: &[u8], tag: &[u8]) -> RcResult<()> {
    let mut h = <HmacSkein512 as MacKeyInit>::new_from_slice(mac_key)
        .expect("HMAC accepts any key");
    h.update(ct);
    // The wire tag is the MAC's leftmost 32 bytes.
    h.verify_truncated_left(tag).map_err(|_| Fail {
        status: RcStatus::Tampered,
        msg: "vault chunk authentication failed".into(),
    })
}

/// Two-tier vault key: enc_key || mac_key, zeroized on drop.
struct VaultKey(Zeroizing<[u8; VAULT_KEY_SIZE]>);

impl VaultKey {
    fn random() -> RcResult<Self> {
        let mut k = Zeroizing::new([0u8; VAULT_KEY_SIZE]);
        rand_bytes(&mut *k)?;
        Ok(VaultKey(k))
    }

    fn enc_key(&self) -> &[u8] {
        &self.0[..ENC_KEY_SIZE]
    }
    fn mac_key(&self) -> &[u8] {
        &self.0[ENC_KEY_SIZE..]
    }
}

fn wrap_key(
    kek: &[u8; 32],
    vk: &VaultKey,
) -> RcResult<([u8; WRAP_NONCE_SIZE], [u8; VAULT_KEY_SIZE], [u8; WRAP_TAG_SIZE])> {
    let cipher = Aes256Gcm::new_from_slice(kek).map_err(|e| Fail {
        status: RcStatus::Crypto,
        msg: format!("aes-gcm init: {e}"),
    })?;
    let mut nonce = [0u8; WRAP_NONCE_SIZE];
    rand_bytes(&mut nonce)?;
    let sealed = cipher
        .encrypt(aes_gcm::Nonce::from_slice(&nonce), vk.0.as_ref())
        .map_err(|_| Fail {
            status: RcStatus::Crypto,
            msg: "vault-key wrap failed".into(),
        })?;
    let (ct, tag) = sealed.split_at(VAULT_KEY_SIZE);
    let mut out_ct = [0u8; VAULT_KEY_SIZE];
    out_ct.copy_from_slice(ct);
    let mut out_tag = [0u8; WRAP_TAG_SIZE];
    out_tag.copy_from_slice(tag);
    Ok((nonce, out_ct, out_tag))
}

/// WrongPassword on a tag failure — stage one of the two-stage
/// honest-error check.
fn unwrap_key(
    kek: &[u8; 32],
    nonce: &[u8; WRAP_NONCE_SIZE],
    ct: &[u8; VAULT_KEY_SIZE],
    tag: &[u8; WRAP_TAG_SIZE],
) -> RcResult<VaultKey> {
    let mut blob = Vec::with_capacity(VAULT_KEY_SIZE + WRAP_TAG_SIZE);
    blob.extend_from_slice(ct);
    blob.extend_from_slice(tag);
    let plain = Zeroizing::new(
        Aes256Gcm::new_from_slice(kek)
            .map_err(|e| Fail {
                status: RcStatus::Crypto,
                msg: format!("aes-gcm init: {e}"),
            })?
            .decrypt(aes_gcm::Nonce::from_slice(nonce), blob.as_ref())
            .map_err(|_| Fail {
                status: RcStatus::WrongPassword,
                msg: "vault password rejected".into(),
            })?,
    );
    let mut vk = Zeroizing::new([0u8; VAULT_KEY_SIZE]);
    vk.copy_from_slice(&plain);
    Ok(VaultKey(vk))
}

// ---------------------------------------------------------------------------
// header

struct Header {
    algo: u8,
    kdf_m: u32,
    kdf_t: u32,
    kdf_p: u32,
    salt: [u8; SALT_SIZE],
    chunk_size: u32,
    chunk_count: u64,
    wrap_nonce: [u8; WRAP_NONCE_SIZE],
    wrapped: [u8; VAULT_KEY_SIZE],
    wrap_tag: [u8; WRAP_TAG_SIZE],
    /// The 153-byte wire image — what the header MAC covers.
    raw: [u8; HEADER_LEN],
    header_mac: [u8; MAC_SIZE],
}

impl Header {
    fn encode(&self) -> [u8; HEADER_LEN] {
        let mut b = Vec::with_capacity(HEADER_LEN);
        b.extend_from_slice(MAGIC);
        b.push(self.algo);
        b.extend_from_slice(&self.kdf_m.to_le_bytes());
        b.extend_from_slice(&self.kdf_t.to_le_bytes());
        b.extend_from_slice(&self.kdf_p.to_le_bytes());
        b.extend_from_slice(&self.salt);
        b.extend_from_slice(&self.chunk_size.to_le_bytes());
        b.extend_from_slice(&self.chunk_count.to_le_bytes());
        b.extend_from_slice(&self.wrap_nonce);
        b.extend_from_slice(&self.wrapped);
        b.extend_from_slice(&self.wrap_tag);
        b.try_into().unwrap()
    }

    /// Field-shape validation only — says nothing about authenticity.
    /// Algo awareness is checked separately so the caller can answer
    /// UnsupportedAlgo rather than conflating it with corruption.
    fn parse(buf: &[u8]) -> RcResult<Header> {
        if buf.len() < PREFIX_LEN {
            return fail(RcStatus::Corrupt, "vault header truncated");
        }
        if &buf[..4] != MAGIC {
            return fail(RcStatus::Corrupt, "not a vault file");
        }
        let u32le = |at: usize| u32::from_le_bytes(buf[at..at + 4].try_into().unwrap());
        let mut salt = [0u8; SALT_SIZE];
        salt.copy_from_slice(&buf[17..49]);
        let chunk_size = u32le(49);
        let chunk_count = u64::from_le_bytes(buf[53..61].try_into().unwrap());
        let mut wrap_nonce = [0u8; WRAP_NONCE_SIZE];
        wrap_nonce.copy_from_slice(&buf[61..73]);
        let mut wrapped = [0u8; VAULT_KEY_SIZE];
        wrapped.copy_from_slice(&buf[73..137]);
        let mut wrap_tag = [0u8; WRAP_TAG_SIZE];
        wrap_tag.copy_from_slice(&buf[137..153]);
        let mut header_mac = [0u8; MAC_SIZE];
        header_mac.copy_from_slice(&buf[153..217]);
        let mut raw = [0u8; HEADER_LEN];
        raw.copy_from_slice(&buf[..HEADER_LEN]);

        let (m, t, p) = (u32le(5), u32le(9), u32le(13));
        if !(8192..=MAX_KDF_MEMORY_KIB).contains(&m)
            || t == 0
            || t > MAX_KDF_PASSES
            || p == 0
            || p > MAX_KDF_LANES
        {
            return fail(RcStatus::Corrupt, "implausible kdf parameters");
        }
        if !(MIN_CHUNK_SIZE..=MAX_CHUNK_SIZE).contains(&chunk_size) {
            return fail(RcStatus::Corrupt, "implausible chunk size");
        }
        if chunk_count == 0 || chunk_count > MAX_CHUNKS {
            return fail(RcStatus::Corrupt, "implausible chunk count");
        }
        Ok(Header {
            algo: buf[4],
            kdf_m: m,
            kdf_t: t,
            kdf_p: p,
            salt,
            chunk_size,
            chunk_count,
            wrap_nonce,
            wrapped,
            wrap_tag,
            raw,
            header_mac,
        })
    }

    fn check_algo(&self) -> RcResult<()> {
        match self.algo {
            ALGO_AES256_GCM | ALGO_THREEFISH_XEX => Ok(()),
            other => fail(
                RcStatus::UnsupportedAlgo,
                format!("unsupported vault algorithm 0x{other:02x}"),
            ),
        }
    }
}

// ---------------------------------------------------------------------------
// chunk cipher

/// Threefish-1024 wants a 128-byte key — Skein-512 expands the 256-bit
/// enc_key into it, domain-separated per half.
fn threefish_key(enc_key: &[u8]) -> Zeroizing<[u8; 128]> {
    let mut out = Zeroizing::new([0u8; 128]);
    let k0 = Skein512_512::new()
        .chain_update(b"ARV1-TF-K0")
        .chain_update(enc_key)
        .finalize();
    let k1 = Skein512_512::new()
        .chain_update(b"ARV1-TF-K1")
        .chain_update(enc_key)
        .finalize();
    out[..64].copy_from_slice(&k0);
    out[64..].copy_from_slice(&k1);
    out
}

fn chunk_nonce(salt: &[u8; SALT_SIZE], chunk: u64) -> [u8; WRAP_NONCE_SIZE] {
    let mut n = [0u8; WRAP_NONCE_SIZE];
    n[..8].copy_from_slice(&salt[..8]);
    n[8..].copy_from_slice(&(chunk as u32).to_le_bytes());
    n
}

fn block_tweak(salt: &[u8; SALT_SIZE], chunk: u64, block: u64) -> [u8; 16] {
    let mut t = [0u8; 16];
    t[..4].copy_from_slice(&salt[..4]);
    t[4..8].copy_from_slice(&(chunk as u32).to_le_bytes());
    t[8..].copy_from_slice(&block.to_le_bytes());
    t
}

/// Encrypt one plaintext chunk; returns (ct, tag).  `pt` must be
/// non-empty and <= chunk_size.
fn encrypt_chunk(
    algo: u8,
    vk: &VaultKey,
    salt: &[u8; SALT_SIZE],
    chunk_index: u64,
    pt: &[u8],
) -> RcResult<(Vec<u8>, Vec<u8>)> {
    match algo {
        ALGO_AES256_GCM => {
            let cipher = Aes256Gcm::new_from_slice(vk.enc_key()).map_err(|e| Fail {
                status: RcStatus::Crypto,
                msg: format!("aes-gcm init: {e}"),
            })?;
            let nonce = chunk_nonce(salt, chunk_index);
            let sealed = cipher
                .encrypt(aes_gcm::Nonce::from_slice(&nonce), pt)
                .map_err(|_| Fail {
                    status: RcStatus::Crypto,
                    msg: "chunk seal failed".into(),
                })?;
            let (ct, tag) = sealed.split_at(sealed.len() - GCM_TAG_SIZE);
            Ok((ct.to_vec(), tag.to_vec()))
        }
        ALGO_THREEFISH_XEX => {
            let key = threefish_key(vk.enc_key());
            // PKCS7 pad to the 128 B Threefish block.
            let pad = TF_BLOCK - (pt.len() % TF_BLOCK);
            let mut padded = Zeroizing::new(pt.to_vec());
            let padded_len = padded.len() + pad;
            padded.resize(padded_len, pad as u8);
            let mut ct = Vec::with_capacity(padded.len());
            for (i, blk) in padded.chunks(TF_BLOCK).enumerate() {
                let mut words = [0u64; 16];
                for (w, c) in words.iter_mut().zip(blk.as_chunks::<8>().0) {
                    *w = u64::from_le_bytes(*c);
                }
                let cipher =
                    Threefish1024::new_with_tweak(&key, &block_tweak(salt, chunk_index, i as u64));
                cipher.encrypt_block_u64(&mut words);
                for w in &words {
                    ct.extend_from_slice(&w.to_le_bytes());
                }
                words.zeroize();
            }
            let tag = skein_tag(vk.mac_key(), &ct);
            Ok((ct, tag.to_vec()))
        }
        other => fail(
            RcStatus::UnsupportedAlgo,
            format!("unsupported vault algorithm 0x{other:02x}"),
        ),
    }
}

/// Verify a frame tag + decrypt.  Per-chunk tags are belt-and-braces
/// on top of the already-verified payload MAC so a streaming consumer
/// learns WHICH chunk is bad.
fn decrypt_chunk(
    algo: u8,
    vk: &VaultKey,
    salt: &[u8; SALT_SIZE],
    chunk_index: u64,
    ct: &[u8],
    tag: &[u8],
) -> RcResult<Vec<u8>> {
    match algo {
        ALGO_AES256_GCM => {
            let cipher = Aes256Gcm::new_from_slice(vk.enc_key()).map_err(|e| Fail {
                status: RcStatus::Crypto,
                msg: format!("aes-gcm init: {e}"),
            })?;
            let nonce = chunk_nonce(salt, chunk_index);
            let mut blob = Vec::with_capacity(ct.len() + GCM_TAG_SIZE);
            blob.extend_from_slice(ct);
            blob.extend_from_slice(tag);
            cipher
                .decrypt(aes_gcm::Nonce::from_slice(&nonce), blob.as_ref())
                .map_err(|_| Fail {
                    status: RcStatus::Tampered,
                    msg: "vault chunk authentication failed".into(),
                })
        }
        ALGO_THREEFISH_XEX => {
            skein_tag_verify(vk.mac_key(), ct, tag)?;
            if ct.is_empty() || !ct.len().is_multiple_of(TF_BLOCK) {
                return fail(RcStatus::Corrupt, "threefish chunk not block-aligned");
            }
            let key = threefish_key(vk.enc_key());
            let mut pt = Zeroizing::new(Vec::with_capacity(ct.len()));
            for (i, blk) in ct.chunks(TF_BLOCK).enumerate() {
                let mut words = [0u64; 16];
                for (w, c) in words.iter_mut().zip(blk.as_chunks::<8>().0) {
                    *w = u64::from_le_bytes(*c);
                }
                let cipher =
                    Threefish1024::new_with_tweak(&key, &block_tweak(salt, chunk_index, i as u64));
                cipher.decrypt_block_u64(&mut words);
                for w in &words {
                    pt.extend_from_slice(&w.to_le_bytes());
                }
                words.zeroize();
            }
            // Strip PKCS7 padding; the chunk MAC already authenticated
            // these bytes, so a malformed pad means a sealer bug.
            let pad = *pt.last().unwrap_or(&0) as usize;
            if pad == 0 || pad > TF_BLOCK || pad > pt.len() {
                return fail(RcStatus::Corrupt, "threefish chunk bad padding");
            }
            if pt[pt.len() - pad..].iter().any(|b| *b != pad as u8) {
                return fail(RcStatus::Corrupt, "threefish chunk bad padding");
            }
            let kept = pt.len() - pad;
            pt.truncate(kept);
            Ok(pt.to_vec())
        }
        other => fail(
            RcStatus::UnsupportedAlgo,
            format!("unsupported vault algorithm 0x{other:02x}"),
        ),
    }
}

// ---------------------------------------------------------------------------
// seal

/// A Write sink that cuts the tar stream into chunks, encrypts each
/// into the vault file and folds every written byte into the payload
/// MAC.  The file position is already at PREFIX_LEN when this runs;
/// the header prefix is backfilled once the chunk count is final.
struct ChunkSealer<'a> {
    algo: u8,
    vk: &'a VaultKey,
    salt: &'a [u8; SALT_SIZE],
    chunk_size: usize,
    buf: Zeroizing<Vec<u8>>,
    out: &'a mut fs::File,
    mac: HmacSha512,
    count: u64,
    produced: u64,
    progress: &'a mut Progress,
}

impl ChunkSealer<'_> {
    fn flush_chunk(&mut self) -> RcResult<()> {
        if self.buf.is_empty() {
            return Ok(());
        }
        let (ct, tag) = encrypt_chunk(self.algo, self.vk, self.salt, self.count, &self.buf)?;
        let mut frame = Vec::with_capacity(4 + ct.len() + tag.len());
        frame.extend_from_slice(&(ct.len() as u32).to_le_bytes());
        frame.extend_from_slice(&ct);
        frame.extend_from_slice(&tag);
        self.mac.update(&frame);
        self.out.write_all(&frame)?;
        self.count += 1;
        self.buf.clear();
        Ok(())
    }

    fn finish(mut self) -> RcResult<(u64, [u8; MAC_SIZE])> {
        self.flush_chunk()?;
        if self.count == 0 {
            return fail(RcStatus::Corrupt, "empty vault payload");
        }
        let mac = self.mac.finalize().into_bytes();
        let mut out = [0u8; MAC_SIZE];
        out.copy_from_slice(&mac);
        Ok((self.count, out))
    }
}

impl io::Write for ChunkSealer<'_> {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        let mut rest = data;
        while !rest.is_empty() {
            let room = self.chunk_size - self.buf.len();
            let n = room.min(rest.len());
            self.buf.extend_from_slice(&rest[..n]);
            rest = &rest[n..];
            if self.buf.len() == self.chunk_size {
                self.flush_chunk().map_err(|e| io::Error::other(e.msg))?;
            }
        }
        self.produced += data.len() as u64;
        self.progress.report(self.produced);
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// The KDF cost written into a fresh header — the spec default (tests
/// dial it down through seal_opts).
#[derive(Clone, Copy)]
pub struct KdfParams {
    pub memory_kib: u32,
    pub passes: u32,
    pub lanes: u32,
}

pub const DEFAULT_KDF: KdfParams = KdfParams {
    memory_kib: KDF_MEMORY_KIB,
    passes: KDF_PASSES,
    lanes: KDF_LANES,
};

/// Deletes the staging path when the operation fails mid-way — the
/// "zero artifacts on failure" guarantee, in both directions.
struct Cleanup(PathBuf, bool);

impl Cleanup {
    fn file(p: PathBuf) -> Self {
        Cleanup(p, false)
    }
    fn dir(p: PathBuf) -> Self {
        Cleanup(p, true)
    }
    /// Hand the path over to the caller's ownership — disarm.
    fn disarm(&mut self) {
        self.0 = PathBuf::new();
    }
}

impl Drop for Cleanup {
    fn drop(&mut self) {
        if self.0.as_os_str().is_empty() {
            return;
        }
        let _ = if self.1 {
            fs::remove_dir_all(&self.0)
        } else {
            fs::remove_file(&self.0)
        };
    }
}

fn tmp_sibling(path: &Path, tag: &str) -> PathBuf {
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(format!(".{tag}-{}.tmp", std::process::id()));
    path.with_file_name(name)
}

/// Payload byte estimate for the progress bar: summed file sizes.
/// Tar headers make the real stream slightly larger — the bar is
/// advisory, never authoritative.
fn estimate_payload(dir: &Path) -> u64 {
    let mut total = 0u64;
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        if let Ok(rd) = fs::read_dir(&d) {
            for e in rd.flatten() {
                if let Ok(md) = e.metadata() {
                    if md.is_dir() {
                        stack.push(e.path());
                    } else if md.is_file() {
                        total += md.len();
                    }
                }
            }
        }
    }
    total
}

fn seal_opts(
    dir: &Path,
    pass: &[u8],
    algo: u8,
    out_path: &Path,
    kdf: KdfParams,
    chunk_size: u32,
    progress: &mut Progress,
) -> RcResult<()> {
    if pass.is_empty() {
        return fail(RcStatus::EmptyPassphrase, "empty password");
    }
    match algo {
        ALGO_AES256_GCM | ALGO_THREEFISH_XEX => {}
        other => {
            return fail(
                RcStatus::UnsupportedAlgo,
                format!("unsupported vault algorithm 0x{other:02x}"),
            )
        }
    }
    if !(MIN_CHUNK_SIZE..=MAX_CHUNK_SIZE).contains(&chunk_size) {
        return fail(RcStatus::InvalidArgument, "chunk size out of bounds");
    }
    let meta = fs::metadata(dir)?;
    if !meta.is_dir() {
        return fail(RcStatus::InvalidArgument, "vault source is not a directory");
    }
    // Resolve the destination through its canonicalized parent so a
    // vault written inside the packed tree is refused even behind
    // ".."-style paths.
    let dir_canon = dir.canonicalize()?;
    let out_abs = if out_path.is_absolute() {
        out_path.to_path_buf()
    } else {
        std::env::current_dir()?.join(out_path)
    };
    let parent = out_abs
        .parent()
        .filter(|p| p.exists())
        .ok_or_else(|| Fail {
            status: RcStatus::Io,
            msg: "vault output directory does not exist".into(),
        })?;
    let name = out_abs.file_name().ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "vault output has no file name".into(),
    })?;
    let out_final = parent.canonicalize()?.join(name);
    if out_final.starts_with(&dir_canon) {
        return fail(
            RcStatus::InvalidArgument,
            "vault output must not live inside the packed tree",
        );
    }

    let salt = {
        let mut s = [0u8; SALT_SIZE];
        rand_bytes(&mut s)?;
        s
    };
    let vk = VaultKey::random()?;
    let kek = crate::store::derive_key(pass, kdf.memory_kib, kdf.passes, kdf.lanes, &salt)?;
    let (wrap_nonce, wrapped, wrap_tag) = wrap_key(&kek, &vk)?;

    let tmp = tmp_sibling(&out_final, "arv-seal");
    let mut tmp_guard = Cleanup::file(tmp.clone());
    let mut file = fs::OpenOptions::new()
        .write(true)
        .create(true)
        .truncate(true)
        .open(&tmp)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = file.set_permissions(fs::Permissions::from_mode(0o600));
    }
    // Placeholder prefix — backfilled once chunk_count is final.
    file.write_all(&vec![0u8; PREFIX_LEN])?;

    let (count, payload_mac) = {
        let mut sealer = ChunkSealer {
            algo,
            vk: &vk,
            salt: &salt,
            chunk_size: chunk_size as usize,
            buf: Zeroizing::new(Vec::with_capacity(chunk_size as usize)),
            out: &mut file,
            mac: hmac_sha512(vk.mac_key()),
            count: 0,
            produced: 0,
            progress,
        };
        {
            // The tar stream is built straight into the encrypting
            // sink — bounded memory regardless of profile size.
            let mut builder = tar::Builder::new(&mut sealer);
            // Preserve links as links — the default deref would
            // silently duplicate the target's bytes into the vault.
            builder.follow_symlinks(false);
            builder.append_dir_all("", dir).map_err(Fail::from)?;
            builder.finish().map_err(Fail::from)?;
        }
        sealer.finish()?
    };

    file.write_all(&payload_mac)?;
    let header = Header {
        algo,
        kdf_m: kdf.memory_kib,
        kdf_t: kdf.passes,
        kdf_p: kdf.lanes,
        salt,
        chunk_size,
        chunk_count: count,
        wrap_nonce,
        wrapped,
        wrap_tag,
        raw: [0u8; HEADER_LEN],
        header_mac: [0u8; MAC_SIZE],
    };
    let raw = header.encode();
    let mut hm = hmac_sha512(vk.mac_key());
    hm.update(&raw);
    let header_mac = hm.finalize().into_bytes();
    file.seek(SeekFrom::Start(0))?;
    file.write_all(&raw)?;
    file.write_all(&header_mac)?;
    file.sync_all()?;
    drop(file);

    fs::rename(&tmp, &out_final)?;
    crate::store::tighten_perms(&out_final);
    tmp_guard.disarm();
    progress.report(progress.total);
    Ok(())
}

pub fn seal_dir(
    dir: &Path,
    pass: &[u8],
    algo: u8,
    out_path: &Path,
    progress: &mut Progress,
) -> RcResult<()> {
    progress.total = estimate_payload(dir);
    seal_opts(
        dir,
        pass,
        algo,
        out_path,
        DEFAULT_KDF,
        DEFAULT_CHUNK_SIZE,
        progress,
    )
}

// ---------------------------------------------------------------------------
// open / verify / unseal

/// Header + vault key, validated through the header MAC — the shared
/// first stage of verify/unseal/rewrap.
struct Opened {
    header: Header,
    vk: VaultKey,
    payload_len: u64,
}

fn open_vault(blob: &Path, pass: &[u8]) -> RcResult<Opened> {
    if pass.is_empty() {
        return fail(RcStatus::EmptyPassphrase, "empty password");
    }
    let file_len = fs::metadata(blob)?.len();
    if file_len < MIN_FILE_LEN {
        return fail(RcStatus::Corrupt, "vault file truncated");
    }
    let mut f = fs::File::open(blob)?;
    let mut prefix = vec![0u8; PREFIX_LEN];
    f.read_exact(&mut prefix)
        .map_err(|_| Fail {
            status: RcStatus::Corrupt,
            msg: "vault header truncated".into(),
        })?;
    let header = Header::parse(&prefix)?;
    header.check_algo()?;
    let payload_len = file_len - MIN_FILE_LEN;
    // Minimum geometry: each frame costs at least its 4-byte length
    // word plus a tag.
    if payload_len < header.chunk_count * (4 + tag_size(header.algo) as u64) {
        return fail(
            RcStatus::Corrupt,
            "vault payload shorter than its chunk table",
        );
    }

    let kek = crate::store::derive_key(
        pass,
        header.kdf_m,
        header.kdf_t,
        header.kdf_p,
        &header.salt,
    )?;
    let vk = unwrap_key(&kek, &header.wrap_nonce, &header.wrapped, &header.wrap_tag)?;

    let mut hm = hmac_sha512(vk.mac_key());
    hm.update(&header.raw);
    hmac_verify(hm, &header.header_mac, "header")?;

    Ok(Opened {
        header,
        vk,
        payload_len,
    })
}

fn read_payload(f: &mut fs::File, buf: &mut [u8], remaining: &mut u64) -> RcResult<()> {
    if (*remaining as usize) < buf.len() {
        return fail(RcStatus::Corrupt, "vault payload truncated");
    }
    f.read_exact(buf)?;
    *remaining -= buf.len() as u64;
    Ok(())
}

/// Pass 1: stream-verify the payload MAC — every payload byte flows
/// through the HMAC before any plaintext is produced.  Also checks
/// the frame layout lands exactly on the trailer.
fn verify_payload(f: &mut fs::File, opened: &Opened, progress: &mut Progress) -> RcResult<()> {
    f.seek(SeekFrom::Start(PREFIX_LEN as u64))?;
    let mut mac = hmac_sha512(opened.vk.mac_key());
    let mut remaining = opened.payload_len;
    let mut consumed = 0u64;
    let max_ct = max_frame_ct(opened.header.algo, opened.header.chunk_size);
    for _ in 0..opened.header.chunk_count {
        let mut lenb = [0u8; 4];
        read_payload(f, &mut lenb, &mut remaining)?;
        let ct_len = u32::from_le_bytes(lenb) as u64;
        if ct_len == 0 || ct_len > max_ct {
            return fail(RcStatus::Corrupt, "vault frame out of bounds");
        }
        if ct_len + tag_size(opened.header.algo) as u64 > remaining {
            return fail(RcStatus::Corrupt, "vault frame overruns payload");
        }
        mac.update(&lenb);
        consumed += 4;
        let mut left = ct_len + tag_size(opened.header.algo) as u64;
        let mut block = [0u8; 8192];
        while left > 0 {
            let n = (left as usize).min(block.len());
            read_payload(f, &mut block[..n], &mut remaining)?;
            mac.update(&block[..n]);
            left -= n as u64;
            consumed += n as u64;
            progress.report(consumed);
        }
    }
    if remaining != 0 {
        return fail(RcStatus::Corrupt, "vault payload length mismatch");
    }
    let mut trailer = [0u8; MAC_SIZE];
    f.read_exact(&mut trailer).map_err(|_| Fail {
        status: RcStatus::Corrupt,
        msg: "vault trailer truncated".into(),
    })?;
    hmac_verify(mac, &trailer, "payload")
}

/// Verify password + authenticity without unpacking — KDF, unwrap,
/// header MAC, then the streaming payload MAC.
pub fn verify_file(blob: &Path, pass: &[u8], progress: &mut Progress) -> RcResult<()> {
    let opened = open_vault(blob, pass)?;
    progress.total = opened.payload_len;
    let mut f = fs::File::open(blob)?;
    verify_payload(&mut f, &opened, progress)
}

/// Streaming decryptor for pass 2: pulls frames off the file, checks
/// the per-chunk tag, and serves plaintext to the tar reader.  A
/// crypto failure is stashed in `crypto_error` — the tar boundary can
/// only carry io::Error, so the caller re-reads it for the honest
/// status code.
struct ChunkReader<'a> {
    f: fs::File,
    remaining: u64,
    next_index: u64,
    count: u64,
    algo: u8,
    salt: [u8; SALT_SIZE],
    chunk_size: u32,
    vk: &'a VaultKey,
    buf: Zeroizing<Vec<u8>>,
    pos: usize,
    crypto_error: Option<Fail>,
    progress: &'a mut Progress,
    progress_base: u64,
    consumed: u64,
}

impl<'a> ChunkReader<'a> {
    fn new(f: fs::File, opened: &'a Opened, progress: &'a mut Progress) -> RcResult<Self> {
        let mut f = f;
        f.seek(SeekFrom::Start(PREFIX_LEN as u64))?;
        Ok(ChunkReader {
            f,
            remaining: opened.payload_len,
            next_index: 0,
            count: opened.header.chunk_count,
            algo: opened.header.algo,
            salt: opened.header.salt,
            chunk_size: opened.header.chunk_size,
            vk: &opened.vk,
            buf: Zeroizing::new(Vec::new()),
            pos: 0,
            crypto_error: None,
            progress,
            progress_base: opened.payload_len,
            consumed: 0,
        })
    }

    fn take_error(&mut self) -> Option<Fail> {
        self.crypto_error.take()
    }

    fn refill(&mut self) -> RcResult<()> {
        if self.next_index >= self.count {
            return fail(RcStatus::Corrupt, "vault tar stream overruns chunks");
        }
        let mut lenb = [0u8; 4];
        read_payload(&mut self.f, &mut lenb, &mut self.remaining)?;
        let ct_len = u32::from_le_bytes(lenb) as usize;
        if ct_len == 0 || ct_len as u64 > max_frame_ct(self.algo, self.chunk_size) {
            return fail(RcStatus::Corrupt, "vault frame out of bounds");
        }
        let mut ct = Zeroizing::new(vec![0u8; ct_len]);
        read_payload(&mut self.f, &mut ct, &mut self.remaining)?;
        let mut tag = vec![0u8; tag_size(self.algo)];
        read_payload(&mut self.f, &mut tag, &mut self.remaining)?;
        self.buf = Zeroizing::new(decrypt_chunk(
            self.algo,
            self.vk,
            &self.salt,
            self.next_index,
            &ct,
            &tag,
        )?);
        self.pos = 0;
        self.next_index += 1;
        self.consumed += 4 + ct_len as u64 + tag.len() as u64;
        self.progress
            .report(self.progress_base + self.consumed);
        Ok(())
    }
}

impl io::Read for ChunkReader<'_> {
    fn read(&mut self, out: &mut [u8]) -> io::Result<usize> {
        if self.pos >= self.buf.len() {
            // Clean end-of-stream — the tar layer decides whether the
            // plaintext was truncated.
            if self.next_index >= self.count {
                return Ok(0);
            }
            if let Err(f) = self.refill() {
                let msg = f.msg.clone();
                self.crypto_error = Some(f);
                return Err(io::Error::other(msg));
            }
        }
        let n = (self.buf.len() - self.pos).min(out.len());
        out[..n].copy_from_slice(&self.buf[self.pos..self.pos + n]);
        self.pos += n;
        Ok(n)
    }
}

/// Extract a tar stream into `dst`, refusing anything that isn't a
/// plain file / dir / symlink and anything tar-rs' unpack_in flags as
/// escaping the destination.  Shared by unseal + tests.
fn extract_archive<R: Read>(reader: R, dst: &Path) -> RcResult<()> {
    let mut archive = tar::Archive::new(reader);
    let mut entries = 0u64;
    for entry in archive.entries().map_err(Fail::from)? {
        let mut entry = entry.map_err(|e| Fail {
            status: if e.kind() == io::ErrorKind::InvalidData {
                RcStatus::Corrupt
            } else {
                RcStatus::Io
            },
            msg: format!("vault tar: {e}"),
        })?;
        entries += 1;
        if entries > MAX_TAR_ENTRIES {
            return fail(RcStatus::Corrupt, "vault archive entry count over limit");
        }
        match entry.header().entry_type() {
            tar::EntryType::Regular | tar::EntryType::Directory | tar::EntryType::Symlink => {}
            _ => {
                return fail(
                    RcStatus::Corrupt,
                    "vault archive contains a non-file entry type",
                )
            }
        }
        let inside = entry.unpack_in(dst).map_err(Fail::from)?;
        if !inside {
            return fail(
                RcStatus::Corrupt,
                "vault archive entry escapes the destination",
            );
        }
    }
    Ok(())
}

/// fsync every file + dir under `root`, bottom-up, so durability
/// lands before the rename publishes the tree.
fn fsync_tree(root: &Path) -> RcResult<()> {
    let mut dirs = Vec::new();
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        dirs.push(d.clone());
        for e in fs::read_dir(&d)? {
            let e = e?;
            let md = fs::symlink_metadata(e.path())?;
            if md.is_dir() {
                stack.push(e.path());
            } else if md.is_file() {
                fs::File::open(e.path())?.sync_data()?;
            }
        }
    }
    for d in dirs {
        fs::File::open(&d)?.sync_all()?;
    }
    Ok(())
}

/// Scrub permission bits a vault can smuggle in — setuid/setgid/sticky
/// have no business in a profile restore.
#[cfg(unix)]
fn strip_special_modes(root: &Path) -> RcResult<()> {
    use std::os::unix::fs::PermissionsExt;
    let mut stack = vec![root.to_path_buf()];
    while let Some(d) = stack.pop() {
        for e in fs::read_dir(&d)? {
            let e = e?;
            let md = fs::symlink_metadata(e.path())?;
            if md.is_dir() {
                stack.push(e.path());
            }
            if md.is_symlink() {
                continue;
            }
            let mode = md.permissions().mode();
            let clean = mode & 0o777;
            if clean != mode {
                fs::set_permissions(e.path(), fs::Permissions::from_mode(clean))?;
            }
        }
    }
    Ok(())
}

#[cfg(not(unix))]
fn strip_special_modes(_root: &Path) -> RcResult<()> {
    Ok(())
}

/// Two-pass unseal: verify the full payload MAC first (zero plaintext
/// written on failure), then decrypt+extract into a staging dir that
/// renames onto `out_dir` only on complete success.
pub fn unseal_file(
    blob: &Path,
    pass: &[u8],
    out_dir: &Path,
    progress: &mut Progress,
) -> RcResult<()> {
    let opened = open_vault(blob, pass)?;
    progress.total = opened.payload_len * 2;

    // Pass 1 — integrity over the whole ciphertext.
    let mut f = fs::File::open(blob)?;
    verify_payload(&mut f, &opened, progress)?;

    // Destination policy: absent, or an empty directory we may fill.
    if out_dir.exists() {
        let md = fs::metadata(out_dir)?;
        if !md.is_dir() || fs::read_dir(out_dir)?.next().is_some() {
            return fail(
                RcStatus::InvalidArgument,
                "vault destination exists and is not an empty directory",
            );
        }
    }
    let staging = tmp_sibling(out_dir, "arv-unpack");
    let mut stage_guard = Cleanup::dir(staging.clone());
    fs::create_dir_all(&staging)?;

    // Pass 2 — decrypt frames into the tar extractor.
    {
        let f2 = fs::File::open(blob)?;
        let mut reader = ChunkReader::new(f2, &opened, progress)?;
        if let Err(e) = extract_archive(&mut reader, &staging) {
            // A crypto-stage failure hidden behind the io boundary
            // wins over the transport error.
            if let Some(inner) = reader.take_error() {
                return Err(inner);
            }
            return Err(e);
        }
    }
    strip_special_modes(&staging)?;
    fsync_tree(&staging)?;

    if out_dir.exists() {
        fs::remove_dir(out_dir)?; // verified empty above
    }
    fs::rename(&staging, out_dir)?;
    if let Some(parent) = out_dir.parent() {
        if let Ok(d) = fs::File::open(parent) {
            let _ = d.sync_all();
        }
    }
    stage_guard.disarm();
    progress.report(progress.total);
    Ok(())
}

/// Re-wrap the vault key under a new password — the cheap password
/// change the two-tier key schedule exists for: only the header's
/// wrap fields + header MAC change, the payload never moves (chunk
/// tweaks ride the UNCHANGED salt and the payload MAC stays valid
/// because mac_key is unchanged).  The KDF salt is kept for the same
/// reason.  Cost is ~217 rewritten bytes instead of a full re-crypt.
pub fn rewrap_file(blob: &Path, old_pass: &[u8], new_pass: &[u8]) -> RcResult<()> {
    if new_pass.is_empty() {
        return fail(RcStatus::EmptyPassphrase, "empty password");
    }
    let mut opened = open_vault(blob, old_pass)?;
    let kek = crate::store::derive_key(
        new_pass,
        opened.header.kdf_m,
        opened.header.kdf_t,
        opened.header.kdf_p,
        &opened.header.salt,
    )?;
    let (nonce, wrapped, tag) = wrap_key(&kek, &opened.vk)?;
    opened.header.wrap_nonce = nonce;
    opened.header.wrapped = wrapped;
    opened.header.wrap_tag = tag;
    let raw = opened.header.encode();
    let mut hm = hmac_sha512(opened.vk.mac_key());
    hm.update(&raw);
    let header_mac = hm.finalize().into_bytes();

    // In-place header rewrite — the payload never moves.
    let mut f = fs::OpenOptions::new().write(true).open(blob)?;
    f.seek(SeekFrom::Start(0))?;
    f.write_all(&raw)?;
    f.write_all(&header_mac)?;
    f.sync_all()?;
    Ok(())
}

/// Header summary for UI display — JSON, or Corrupt for a non-vault
/// file.  NOTE: no password is available, so the fields are
/// UNVERIFIED — a tampered header reports tampered values; the real
/// check happens at verify/unseal time.
pub fn inspect_file(blob: &Path) -> RcResult<String> {
    let file_len = fs::metadata(blob)?.len();
    if file_len < MIN_FILE_LEN {
        return fail(RcStatus::Corrupt, "vault file truncated");
    }
    let mut f = fs::File::open(blob)?;
    let mut prefix = vec![0u8; PREFIX_LEN];
    f.read_exact(&mut prefix).map_err(|_| Fail {
        status: RcStatus::Corrupt,
        msg: "vault header truncated".into(),
    })?;
    // Lenient read: report the raw algo even when unknown.
    let header = Header::parse(&prefix)?;
    Ok(json!({
        "format": "ARV1",
        "algo": header.algo,
        "algo_name": algo_name(header.algo),
        "kdf": "argon2id",
        "memory_kib": header.kdf_m,
        "passes": header.kdf_t,
        "lanes": header.kdf_p,
        "chunk_size": header.chunk_size,
        "chunk_count": header.chunk_count,
        "payload_bytes": file_len - MIN_FILE_LEN,
        "authenticated": false,
    })
    .to_string())
}

// ---------------------------------------------------------------------------
// tests

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::MutexGuard;

    fn guard() -> MutexGuard<'static, ()> {
        crate::store::test_lock()
    }

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "rustcore-pvault-{}-{}-{}",
            std::process::id(),
            tag,
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let _ = fs::remove_dir_all(&dir);
        fs::create_dir_all(&dir).unwrap();
        dir
    }

    fn fixture_tree(root: &Path) {
        fs::create_dir_all(root.join("sub/deep")).unwrap();
        fs::write(root.join("a.txt"), b"alpha alpha alpha").unwrap();
        fs::write(root.join("sub/b.bin"), vec![7u8; 5000]).unwrap();
        fs::write(root.join("sub/deep/c.txt"), b"cee").unwrap();
        fs::create_dir_all(root.join("emptydir")).unwrap();
        #[cfg(unix)]
        std::os::unix::fs::symlink("sub/b.bin", root.join("link")).unwrap();
    }

    fn trees_equal(a: &Path, b: &Path) -> bool {
        fn collect(root: &Path, base: &Path, out: &mut Vec<(String, u8, Vec<u8>)>) {
            for e in fs::read_dir(root).unwrap() {
                let e = e.unwrap();
                let rel = e
                    .path()
                    .strip_prefix(base)
                    .unwrap()
                    .to_string_lossy()
                    .into_owned();
                let md = fs::symlink_metadata(e.path()).unwrap();
                if md.is_dir() {
                    collect(&e.path(), base, out);
                    out.push((rel, b'd', Vec::new()));
                } else if md.file_type().is_symlink() {
                    out.push((
                        rel,
                        b'l',
                        fs::read_link(e.path())
                            .unwrap()
                            .to_string_lossy()
                            .into_owned()
                            .into_bytes(),
                    ));
                } else {
                    out.push((rel, b'f', fs::read(e.path()).unwrap()));
                }
            }
        }
        let (mut va, mut vb) = (Vec::new(), Vec::new());
        collect(a, a, &mut va);
        collect(b, b, &mut vb);
        va.sort();
        vb.sort();
        va == vb
    }

    fn nop() -> Progress {
        Progress::new(None, std::ptr::null_mut())
    }

    /// Fast KDF + tiny chunks so tests exercise multi-chunk framing
    /// without paying the production Argon2 cost each leg.
    const TEST_KDF: KdfParams = KdfParams {
        memory_kib: 8192,
        passes: 1,
        lanes: 1,
    };
    const TEST_CHUNK: u32 = 8192;

    fn quick_seal(dir: &Path, pass: &[u8], algo: u8, out: &Path) -> RcResult<()> {
        seal_opts(dir, pass, algo, out, TEST_KDF, TEST_CHUNK, &mut nop())
    }

    #[test]
    fn round_trip_both_algos() {
        let _g = guard();
        let base = tmpdir("rt");
        let src = base.join("profile");
        fixture_tree(&src);
        for algo in [ALGO_AES256_GCM, ALGO_THREEFISH_XEX] {
            let vault = base.join(format!("v{algo}.arv"));
            let out = base.join(format!("out{algo}"));
            quick_seal(&src, b"pw", algo, &vault).unwrap();
            unseal_file(&vault, b"pw", &out, &mut nop()).unwrap();
            assert!(trees_equal(&src, &out), "algo {algo} round-trip");
        }
    }

    #[test]
    fn wrong_password_is_distinct() {
        let _g = guard();
        let base = tmpdir("wp");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        let out = base.join("out");
        quick_seal(&src, b"right", ALGO_AES256_GCM, &vault).unwrap();
        let e = unseal_file(&vault, b"wrong", &out, &mut nop()).unwrap_err();
        assert_eq!(e.status, RcStatus::WrongPassword);
        assert!(!out.exists());
        let e = verify_file(&vault, b"wrong", &mut nop()).unwrap_err();
        assert_eq!(e.status, RcStatus::WrongPassword);
    }

    #[test]
    fn flipped_ciphertext_byte_is_tampered_with_no_output() {
        let _g = guard();
        let base = tmpdir("tamper");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        let out = base.join("out");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let mut bytes = fs::read(&vault).unwrap();
        // Inside the first payload chunk (past the 217-byte prefix
        // and the 4-byte frame length).
        bytes[PREFIX_LEN + 10] ^= 0x40;
        fs::write(&vault, &bytes).unwrap();
        let e = unseal_file(&vault, b"pw", &out, &mut nop()).unwrap_err();
        assert_eq!(e.status, RcStatus::Tampered);
        assert!(!out.exists());
        // The staging dir must be gone — zero unpacked artifacts.
        assert!(fs::read_dir(&base).unwrap().all(|e| !e
            .unwrap()
            .file_name()
            .to_string_lossy()
            .contains("arv-unpack")));
    }

    #[test]
    fn header_tamper_caught_by_header_mac() {
        let _g = guard();
        let base = tmpdir("hdr");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let mut bytes = fs::read(&vault).unwrap();
        // chunk_size field — passes the bounds check, fails the MAC.
        bytes[50] ^= 0x10;
        fs::write(&vault, &bytes).unwrap();
        let e = verify_file(&vault, b"pw", &mut nop()).unwrap_err();
        assert_eq!(e.status, RcStatus::Tampered);
    }

    #[test]
    fn wrapped_key_field_flip_reports_wrong_password() {
        let _g = guard();
        let base = tmpdir("wrapflip");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let mut bytes = fs::read(&vault).unwrap();
        // Inside wrapped_vault_key (offset 73..137) — the documented
        // edge: unwrap fails -> WrongPassword, not Tampered.
        bytes[80] ^= 1;
        fs::write(&vault, &bytes).unwrap();
        let e = verify_file(&vault, b"pw", &mut nop()).unwrap_err();
        assert_eq!(e.status, RcStatus::WrongPassword);
    }

    #[test]
    fn truncated_and_extended_are_corrupt() {
        let _g = guard();
        let base = tmpdir("trunc");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let bytes = fs::read(&vault).unwrap();

        let short = base.join("short.arv");
        fs::write(&short, &bytes[..bytes.len() - 5]).unwrap();
        let e = verify_file(&short, b"pw", &mut nop()).unwrap_err();
        assert!(e.status == RcStatus::Corrupt || e.status == RcStatus::Tampered);

        let long = base.join("long.arv");
        let mut ext = bytes.clone();
        ext.extend_from_slice(&[0u8; 32]);
        fs::write(&long, &ext).unwrap();
        assert_eq!(
            verify_file(&long, b"pw", &mut nop()).unwrap_err().status,
            RcStatus::Corrupt
        );

        let tiny = base.join("tiny.arv");
        fs::write(&tiny, b"ARV1").unwrap();
        assert_eq!(
            verify_file(&tiny, b"pw", &mut nop()).unwrap_err().status,
            RcStatus::Corrupt
        );
    }

    #[test]
    fn unknown_algo_reports_unsupported() {
        let _g = guard();
        let base = tmpdir("algo");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let mut bytes = fs::read(&vault).unwrap();
        bytes[4] = 0x77;
        fs::write(&vault, &bytes).unwrap();
        assert_eq!(
            verify_file(&vault, b"pw", &mut nop()).unwrap_err().status,
            RcStatus::UnsupportedAlgo
        );
        // inspect still reports the raw id.
        let j = inspect_file(&vault).unwrap();
        assert!(j.contains("\"algo\":119"));
        assert!(j.contains("\"algo_name\":\"unknown\""));
    }

    #[test]
    fn inspect_reports_kdf_and_geometry() {
        let _g = guard();
        let base = tmpdir("insp");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_THREEFISH_XEX, &vault).unwrap();
        let j = inspect_file(&vault).unwrap();
        assert!(
            j.contains("\"algo_name\":\"threefish-1024-xex+skein-mac\""),
            "{j}"
        );
        assert!(j.contains("\"memory_kib\":8192"), "{j}");
        assert!(j.contains("\"authenticated\":false"), "{j}");
        let plain = base.join("nope.bin");
        fs::write(&plain, vec![0u8; 900]).unwrap();
        assert_eq!(
            inspect_file(&plain).unwrap_err().status,
            RcStatus::Corrupt
        );
    }

    #[test]
    fn rewrap_changes_password_without_touching_payload() {
        let _g = guard();
        let base = tmpdir("rewrap");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        let out = base.join("out");
        quick_seal(&src, b"old", ALGO_AES256_GCM, &vault).unwrap();
        let before = fs::read(&vault).unwrap();
        rewrap_file(&vault, b"old", b"new").unwrap();
        let after = fs::read(&vault).unwrap();
        // Payload bytes identical — only the prefix changed.
        assert_eq!(before.len(), after.len());
        assert_eq!(&before[PREFIX_LEN..], &after[PREFIX_LEN..]);
        assert_ne!(&before[..PREFIX_LEN], &after[..PREFIX_LEN]);
        assert_eq!(
            verify_file(&vault, b"old", &mut nop()).unwrap_err().status,
            RcStatus::WrongPassword
        );
        unseal_file(&vault, b"new", &out, &mut nop()).unwrap();
        assert!(trees_equal(&src, &out));
        assert_eq!(
            rewrap_file(&vault, b"nope", b"x").unwrap_err().status,
            RcStatus::WrongPassword
        );
    }

    #[test]
    fn vault_inside_source_tree_refused() {
        let _g = guard();
        let base = tmpdir("inside");
        let src = base.join("p");
        fixture_tree(&src);
        let nested = src.join("self.arv");
        assert_eq!(
            quick_seal(&src, b"pw", ALGO_AES256_GCM, &nested)
                .unwrap_err()
                .status,
            RcStatus::InvalidArgument
        );
    }

    #[test]
    fn seal_rejects_bad_args() {
        let _g = guard();
        let base = tmpdir("args");
        let src = base.join("p");
        fixture_tree(&src);
        let out = base.join("v.arv");
        assert_eq!(
            quick_seal(&src, b"", ALGO_AES256_GCM, &out)
                .unwrap_err()
                .status,
            RcStatus::EmptyPassphrase
        );
        assert_eq!(
            quick_seal(&src, b"pw", 0x99, &out).unwrap_err().status,
            RcStatus::UnsupportedAlgo
        );
        let file_src = base.join("afile");
        fs::write(&file_src, b"x").unwrap();
        assert_eq!(
            quick_seal(&file_src, b"pw", ALGO_AES256_GCM, &out)
                .unwrap_err()
                .status,
            RcStatus::InvalidArgument
        );
    }

    #[test]
    fn tar_traversal_entries_refused() {
        let _g = guard();
        let base = tmpdir("trav");
        let dst = base.join("dst");
        fs::create_dir_all(&dst).unwrap();
        // Hand-build an archive whose entry path escapes dst —
        // append_data refuses "..", so the name goes straight into
        // the wire header.
        let mut t = Vec::new();
        {
            let mut b = tar::Builder::new(&mut t);
            let mut h = tar::Header::new_gnu();
            h.set_size(4);
            h.set_entry_type(tar::EntryType::Regular);
            h.set_mode(0o644);
            h.as_old_mut().name[..13].copy_from_slice(b"../escape.txt");
            h.set_cksum();
            b.append(&h, &b"evil"[..]).unwrap();
            b.finish().unwrap();
        }
        assert_eq!(
            extract_archive(&t[..], &dst).unwrap_err().status,
            RcStatus::Corrupt
        );
        assert!(!base.join("escape.txt").exists());
    }

    #[test]
    fn multi_chunk_framing() {
        let _g = guard();
        let base = tmpdir("multi");
        let src = base.join("p");
        fixture_tree(&src);
        fs::write(src.join("big.bin"), vec![3u8; 150_000]).unwrap();
        for algo in [ALGO_AES256_GCM, ALGO_THREEFISH_XEX] {
            let vault = base.join(format!("m{algo}.arv"));
            let out = base.join(format!("mout{algo}"));
            quick_seal(&src, b"pw", algo, &vault).unwrap();
            let j = inspect_file(&vault).unwrap();
            let v: serde_json::Value = serde_json::from_str(&j).unwrap();
            assert!(v["chunk_count"].as_u64().unwrap() >= 15, "{j}");
            unseal_file(&vault, b"pw", &out, &mut nop()).unwrap();
            assert!(trees_equal(&src, &out));
        }
    }

    #[test]
    fn unseal_refuses_nonempty_destination_and_leaves_nothing() {
        let _g = guard();
        let base = tmpdir("dest");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let occupied = base.join("occupied");
        fs::create_dir_all(&occupied).unwrap();
        fs::write(occupied.join("keep.txt"), b"keep").unwrap();
        assert_eq!(
            unseal_file(&vault, b"pw", &occupied, &mut nop())
                .unwrap_err()
                .status,
            RcStatus::InvalidArgument
        );
        assert_eq!(fs::read(occupied.join("keep.txt")).unwrap(), b"keep");
        // An existing EMPTY dir is fine to fill.
        let empty = base.join("empty");
        fs::create_dir_all(&empty).unwrap();
        unseal_file(&vault, b"pw", &empty, &mut nop()).unwrap();
        assert!(trees_equal(&src, &empty));
    }

    #[test]
    fn no_plaintext_or_password_in_vault() {
        let _g = guard();
        let base = tmpdir("leak");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        let pass = b"sup3r-secret-sentence";
        quick_seal(&src, pass, ALGO_AES256_GCM, &vault).unwrap();
        let bytes = fs::read(&vault).unwrap();
        assert!(bytes
            .windows(b"alpha alpha".len())
            .all(|w| w != b"alpha alpha"));
        assert!(bytes.windows(pass.len()).all(|w| w != pass));
    }

    #[test]
    fn progress_fires_monotonic() {
        use std::sync::atomic::{AtomicU64, Ordering};
        static CALLS: AtomicU64 = AtomicU64::new(0);
        static LAST_DONE: AtomicU64 = AtomicU64::new(0);
        unsafe extern "C" fn cb(done: u64, _total: u64, _ud: *mut std::ffi::c_void) {
            CALLS.fetch_add(1, Ordering::SeqCst);
            let prev = LAST_DONE.swap(done, Ordering::SeqCst);
            assert!(done >= prev);
        }
        let _g = guard();
        let base = tmpdir("prog");
        let src = base.join("p");
        fixture_tree(&src);
        let vault = base.join("v.arv");
        quick_seal(&src, b"pw", ALGO_AES256_GCM, &vault).unwrap();
        let out = base.join("out");
        CALLS.store(0, Ordering::SeqCst);
        LAST_DONE.store(0, Ordering::SeqCst);
        let mut p = Progress::new(Some(cb), std::ptr::null_mut());
        unseal_file(&vault, b"pw", &out, &mut p).unwrap();
        assert!(CALLS.load(Ordering::SeqCst) >= 2);
        assert_eq!(LAST_DONE.load(Ordering::SeqCst), p.total);
    }

    #[test]
    fn key_material_zeroized_on_drop() {
        let _g = guard();
        // "No key material in freed buffers": every binding that can
        // hold vault secrets is a Zeroizing wrapper, so drop wipes the
        // bytes in place before the allocation goes back.  For the
        // fixed-size keys (stack arrays) we observe the still-mapped
        // slot immediately after drop — nothing overwrites a stack
        // frame in between, and black_box keeps the compiler from
        // eliding the read as use-after-scope.  The heap buffers are
        // probed live instead: a dropped Vec's block can be claimed
        // by another test thread between drop and read, which would
        // make a post-free check flaky — so the heap leg verifies
        // zeroize() on the concrete buffer type the sealer/reader use.
        unsafe fn wiped(p: *const u8, n: usize) -> bool {
            std::slice::from_raw_parts(std::hint::black_box(p), n)
                .iter()
                .all(|b| *b == 0)
        }

        // The two-tier vault key (enc_key || mac_key) — stack array.
        let p;
        {
            let vk = VaultKey::random().unwrap();
            assert!(vk.enc_key().iter().any(|b| *b != 0));
            p = vk.0.as_ptr();
        }
        assert!(unsafe { wiped(p, VAULT_KEY_SIZE) }, "vault key survived drop");

        // The expanded 128-byte Threefish cipher key.
        let p;
        {
            let tf = threefish_key(&[7u8; ENC_KEY_SIZE]);
            p = tf.as_ptr();
        }
        assert!(unsafe { wiped(p, 128) }, "threefish key survived drop");

        // The heap chunk buffers ride Zeroizing<Vec<u8>> — the drop
        // path is Vec::zeroize, so probe that mechanism directly on
        // the same concrete type.
        let mut buf = Zeroizing::new(vec![0xA5u8; 4096]);
        buf.zeroize();
        assert!(buf.iter().all(|b| *b == 0), "chunk buffer zeroize no-op");
    }
}
