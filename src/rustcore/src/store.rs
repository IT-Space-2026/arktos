//! Credential custody: the SecureStore key-file / master-passphrase
//! state machine, ported to Rust (RCORE01 part B).
//!
//! On-disk formats are byte-identical to the C++ implementation this
//! replaces, so stores written by either side open on the other:
//!
//!   securestore.key — 32 raw key bytes, mode 0600.
//!   securestore.kdf — "ARKDF1" | memoryKiB u32le | passes u32le |
//!                     lanes u32le | salt[16] | verifier blob.
//!   sealed blob     — "ARSEC1" | nonce[12] | tag[16] | ciphertext
//!                     (AES-256-GCM; the tag sits before the
//!                     ciphertext, unlike the usual ct||tag layout).
//!   credentials.dat — one sealed blob holding the rc_cred_* map
//!                     (u32le count, then nlen|name|vlen|value rows).
//!
//! Key custody mirrors the old semantics exactly: either a random
//! on-disk key, OR an Argon2id passphrase-derived key that lives only
//! in RAM behind `derived_key` — never both, and key material is never
//! written in passphrase mode.

use std::collections::BTreeMap;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, MutexGuard};

use aes_gcm::aead::{consts::U12, generic_array::GenericArray, Aead, KeyInit};
use aes_gcm::Aes256Gcm;
use argon2::{Algorithm, Argon2, Params, Version};
use zeroize::Zeroizing;

use crate::error::{fail, Fail, RcResult, RcStatus};

pub const KEY_SIZE: usize = 32;
pub const NONCE_SIZE: usize = 12;
pub const TAG_SIZE: usize = 16;
pub const SEAL_MAGIC: &[u8; 6] = b"ARSEC1";
pub const HEADER_SIZE: usize = 6 + NONCE_SIZE + TAG_SIZE;

pub const KDF_MAGIC: &[u8; 6] = b"ARKDF1";
pub const KDF_SALT_SIZE: usize = 16;
pub const KDF_MEMORY_KIB: u32 = 65536;
pub const KDF_PASSES: u32 = 3;
pub const KDF_LANES: u32 = 4;
pub const VERIFIER_MESSAGE: &[u8] = b"ARORA-KDF-VERIFY";

pub const KEY_FILE: &str = "securestore.key";
pub const KDF_FILE: &str = "securestore.kdf";
pub const CRED_FILE: &str = "credentials.dat";

/// Fixed-size key material that wipes itself on drop.
pub type Key = Zeroizing<[u8; KEY_SIZE]>;

/// Credentials map: name -> secret bytes.
pub type CredMap = BTreeMap<String, Zeroizing<Vec<u8>>>;

#[derive(Default)]
pub struct Store {
    data_dir: Option<PathBuf>,
    file_key: Option<Key>,
    file_key_loaded: bool,
    pub derived_key: Option<Key>,
    /// Lazily decrypted rc_cred_* map; dropped again by lock().
    pub creds: Option<CredMap>,
    /// Lazily decrypted rc_autofill_* records; dropped with the keys.
    pub autofill: Option<Vec<crate::autofill::FormRecord>>,
}

static STORE: Mutex<Store> = Mutex::new(Store {
    data_dir: None,
    file_key: None,
    file_key_loaded: false,
    derived_key: None,
    creds: None,
    autofill: None,
});

/// Locks the shared store, surviving a poisoned mutex (a panicked FFI
/// call must not wedge the store for the rest of the process).
pub fn lock() -> MutexGuard<'static, Store> {
    STORE.lock().unwrap_or_else(|e| e.into_inner())
}

impl Store {
    pub fn set_data_dir(&mut self, dir: &str) -> RcResult<()> {
        if dir.is_empty() {
            return fail(RcStatus::InvalidArgument, "empty data dir");
        }
        let path = PathBuf::from(dir);
        if self.data_dir.as_ref() != Some(&path) {
            self.data_dir = Some(path);
            // A different dir is a different custody domain — drop
            // every cached key and the decoded credential map so no
            // state leaks across stores (test-mode dir switches rely
            // on this).
            self.file_key = None;
            self.file_key_loaded = false;
            self.derived_key = None;
            self.creds = None;
            self.autofill = None;
        }
        Ok(())
    }

    pub(crate) fn dir(&self) -> RcResult<&Path> {
        self.data_dir.as_deref().ok_or_else(|| Fail {
            status: RcStatus::NotInitialized,
            msg: "rc_set_data_dir() has not been called".into(),
        })
    }

    pub fn key_path(&self) -> RcResult<PathBuf> {
        Ok(self.dir()?.join(KEY_FILE))
    }

    pub fn kdf_path(&self) -> RcResult<PathBuf> {
        Ok(self.dir()?.join(KDF_FILE))
    }

    pub fn cred_path(&self) -> RcResult<PathBuf> {
        Ok(self.dir()?.join(CRED_FILE))
    }

    /// Passphrase mode is defined by the KDF file existing — the same
    /// predicate the C++ version used (QFile::exists on .kdf).
    pub fn passphrase_enabled(&self) -> bool {
        self.kdf_path().map(|p| p.exists()).unwrap_or(false)
    }

    pub fn is_unlocked(&self) -> bool {
        !self.passphrase_enabled() || self.derived_key.is_some()
    }

    /// `evp() && key custody healthy` — in Rust the AEAD is compiled
    /// in, so availability is purely a custody/filesystem question.
    pub fn is_available(&mut self) -> bool {
        self.passphrase_enabled() || self.load_or_create_file_key().is_ok()
    }

    fn random<const N: usize>() -> RcResult<[u8; N]> {
        let mut b = [0u8; N];
        getrandom::getrandom(&mut b).map_err(|e| Fail {
            status: RcStatus::Unavailable,
            msg: format!("getrandom: {e}"),
        })?;
        Ok(b)
    }

    /// The file key, read from disk or freshly minted (0600) when
    /// absent/corrupt — matches the old loadOrCreateFileKey().
    pub fn load_or_create_file_key(&mut self) -> RcResult<Key> {
        if !self.file_key_loaded {
            let path = self.key_path()?;
            if path.exists() {
                if let Ok(bytes) = fs::read(&path) {
                    if bytes.len() == KEY_SIZE {
                        tighten_perms(&path);
                        let mut k = [0u8; KEY_SIZE];
                        k.copy_from_slice(&bytes);
                        self.file_key = Some(Zeroizing::new(k));
                    }
                    // Wrong size: treated as corrupt — mint a fresh
                    // key below, same as the C++ path.
                }
            }
            if self.file_key.is_none() {
                let k = Self::random::<KEY_SIZE>()?;
                atomic_write(&path, &k)?;
                self.file_key = Some(Zeroizing::new(k));
            }
            self.file_key_loaded = true;
        }
        Ok(Zeroizing::new(**self.file_key.as_ref().unwrap()))
    }

    /// The key the current custody mode seals/opens with.  In
    /// passphrase mode a locked store is an error — the interactive
    /// retry lives on the Qt side.
    pub fn current_key(&mut self) -> RcResult<Key> {
        if self.passphrase_enabled() {
            match &self.derived_key {
                Some(k) => Ok(Zeroizing::new(**k)),
                None => fail(RcStatus::Locked, "credential store is locked"),
            }
        } else {
            self.load_or_create_file_key()
        }
    }

    pub fn seal(&mut self, plain: &[u8]) -> RcResult<Vec<u8>> {
        let key = self.current_key()?;
        seal_with_key(&key, plain)
    }

    pub fn open(&mut self, blob: &[u8]) -> RcResult<Vec<u8>> {
        let key = self.current_key()?;
        match open_with_key(&key, blob) {
            Ok(p) => Ok(p),
            Err(first) => {
                // Transition residue: in passphrase mode a blob sealed
                // before the migration (or mid-transition) still opens
                // under the on-disk key while that file exists.
                if self.passphrase_enabled()
                    && self.key_path().map(|p| p.exists()).unwrap_or(false)
                {
                    let legacy = self.load_or_create_file_key()?;
                    open_with_key(&legacy, blob).map_err(|_| first)
                } else {
                    Err(first)
                }
            }
        }
    }

    /// Argon2id-derive + verify against the .kdf file's verifier blob.
    /// Wrong passphrases fail the GCM tag — no oracle.
    pub fn unlock(&mut self, pass: &[u8]) -> RcResult<()> {
        let path = self.kdf_path()?;
        if !path.exists() {
            return fail(
                RcStatus::NotEnabled,
                "passphrase protection is not enabled",
            );
        }
        let data = fs::read(&path)?;
        let params = parse_kdf(&data)?;
        let pass = Zeroizing::new(pass.to_vec());
        let candidate = derive_key(
            &pass,
            params.memory_kib,
            params.passes,
            params.lanes,
            &params.salt,
        )?;
        match open_with_key(&candidate, &params.verifier) {
            Ok(plain) if plain == VERIFIER_MESSAGE => {
                self.derived_key = Some(candidate);
                Ok(())
            }
            _ => fail(
                RcStatus::WrongPassphrase,
                "passphrase does not unlock the store",
            ),
        }
    }

    /// Writes a fresh .kdf (new salt + verifier) and adopts the
    /// derived key.  This is the shared stage of enable() and
    /// change-passphrase; consumer re-sealing happens around it.
    pub fn kdf_create(&mut self, pass: &[u8]) -> RcResult<()> {
        if pass.is_empty() {
            return fail(RcStatus::EmptyPassphrase, "empty passphrase");
        }
        let pass = Zeroizing::new(pass.to_vec());
        let salt = Self::random::<KDF_SALT_SIZE>()?;
        let key = derive_key(&pass, KDF_MEMORY_KIB, KDF_PASSES, KDF_LANES, &salt)?;
        let verifier = seal_with_key(&key, VERIFIER_MESSAGE)?;

        let mut data = Vec::with_capacity(34 + verifier.len());
        data.extend_from_slice(KDF_MAGIC);
        data.extend_from_slice(&KDF_MEMORY_KIB.to_le_bytes());
        data.extend_from_slice(&KDF_PASSES.to_le_bytes());
        data.extend_from_slice(&KDF_LANES.to_le_bytes());
        data.extend_from_slice(&salt);
        data.extend_from_slice(&verifier);
        atomic_write(&self.kdf_path()?, &data)?;
        self.derived_key = Some(key);
        Ok(())
    }

    /// Wipes every cached key and the decoded record caches.
    pub fn lock(&mut self) {
        self.derived_key = None;
        self.file_key = None;
        self.file_key_loaded = false;
        self.creds = None;
        self.autofill = None;
    }

    /// Writes a fresh random securestore.key and caches it.
    pub fn key_file_create(&mut self) -> RcResult<()> {
        let k = Self::random::<KEY_SIZE>()?;
        atomic_write(&self.key_path()?, &k)?;
        self.file_key = Some(Zeroizing::new(k));
        self.file_key_loaded = true;
        Ok(())
    }

    pub fn key_file_delete(&mut self) -> RcResult<()> {
        let path = self.key_path()?;
        if path.exists() {
            fs::remove_file(&path)?;
        }
        self.file_key = None;
        self.file_key_loaded = false;
        Ok(())
    }

    pub fn kdf_file_delete(&mut self) -> RcResult<()> {
        let path = self.kdf_path()?;
        if path.exists() {
            fs::remove_file(&path)?;
        }
        self.derived_key = None;
        Ok(())
    }
}

fn parse_kdf(data: &[u8]) -> RcResult<KdfParams> {
    // magic 6 | m u32 | t u32 | p u32 | salt 16 | verifier blob
    if data.len() < 6 + 12 + KDF_SALT_SIZE + HEADER_SIZE
        || &data[..6] != KDF_MAGIC
    {
        return fail(RcStatus::Corrupt, "malformed kdf file");
    }
    let u32le = |at: usize| u32::from_le_bytes(data[at..at + 4].try_into().unwrap());
    let mut salt = [0u8; KDF_SALT_SIZE];
    salt.copy_from_slice(&data[18..34]);
    let verifier = data[34..].to_vec();
    if verifier.len() < HEADER_SIZE || &verifier[..6] != SEAL_MAGIC {
        return fail(RcStatus::Corrupt, "malformed kdf verifier");
    }
    Ok(KdfParams {
        memory_kib: u32le(6),
        passes: u32le(10),
        lanes: u32le(14),
        salt,
        verifier,
    })
}

struct KdfParams {
    memory_kib: u32,
    passes: u32,
    lanes: u32,
    salt: [u8; KDF_SALT_SIZE],
    verifier: Vec<u8>,
}

pub(crate) fn derive_key(pass: &[u8], m: u32, t: u32, p: u32, salt: &[u8]) -> RcResult<Key> {
    let params = Params::new(m, t, p, Some(KEY_SIZE)).map_err(|e| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("argon2 params: {e}"),
    })?;
    let mut out = [0u8; KEY_SIZE];
    Argon2::new(Algorithm::Argon2id, Version::V0x13, params)
        .hash_password_into(pass, salt, &mut out)
        .map_err(|e| Fail {
            status: RcStatus::Crypto,
            msg: format!("argon2: {e}"),
        })?;
    Ok(Zeroizing::new(out))
}

type Nonce96 = GenericArray<u8, U12>;

/// "ARSEC1" | nonce[12] | tag[16] | ciphertext under `key`.
pub fn seal_with_key(key: &[u8; KEY_SIZE], plain: &[u8]) -> RcResult<Vec<u8>> {
    let cipher = Aes256Gcm::new_from_slice(key).map_err(|e| Fail {
        status: RcStatus::Crypto,
        msg: format!("aes-gcm init: {e}"),
    })?;
    let mut nonce = [0u8; NONCE_SIZE];
    getrandom::getrandom(&mut nonce).map_err(|e| Fail {
        status: RcStatus::Unavailable,
        msg: format!("getrandom: {e}"),
    })?;
    // RustCrypto emits ciphertext||tag; the ARSEC1 framing keeps the
    // tag ahead of the ciphertext for C++-written blob compatibility.
    let sealed = cipher
        .encrypt(Nonce96::from_slice(&nonce), plain)
        .map_err(|_| Fail {
            status: RcStatus::Crypto,
            msg: "aes-gcm seal failed".into(),
        })?;
    let (ct, tag) = sealed.split_at(sealed.len() - TAG_SIZE);
    let mut out = Vec::with_capacity(HEADER_SIZE + ct.len());
    out.extend_from_slice(SEAL_MAGIC);
    out.extend_from_slice(&nonce);
    out.extend_from_slice(tag);
    out.extend_from_slice(ct);
    Ok(out)
}

/// Inverse of seal_with_key; `Crypto` on tag mismatch (tamper or
/// wrong key — deliberately indistinguishable), `Corrupt` on shape.
pub fn open_with_key(key: &[u8; KEY_SIZE], blob: &[u8]) -> RcResult<Vec<u8>> {
    if blob.len() < HEADER_SIZE || &blob[..6] != SEAL_MAGIC {
        return fail(RcStatus::Corrupt, "not a sealed blob");
    }
    let nonce = Nonce96::from_slice(&blob[6..6 + NONCE_SIZE]);
    let tag = &blob[6 + NONCE_SIZE..HEADER_SIZE];
    let ct = &blob[HEADER_SIZE..];
    let mut ct_tag = Vec::with_capacity(ct.len() + TAG_SIZE);
    ct_tag.extend_from_slice(ct);
    ct_tag.extend_from_slice(tag);
    Aes256Gcm::new_from_slice(key)
        .map_err(|e| Fail {
            status: RcStatus::Crypto,
            msg: format!("aes-gcm init: {e}"),
        })?
        .decrypt(nonce, ct_tag.as_ref())
        .map_err(|_| Fail {
            status: RcStatus::Crypto,
            msg: "sealed blob failed authentication".into(),
        })
}

/// Write-temp-then-rename with 0600 perms — the QSaveFile contract.
pub fn atomic_write(path: &Path, data: &[u8]) -> RcResult<()> {
    let mut name = path.file_name().unwrap_or_default().to_os_string();
    name.push(format!(".{}.tmp", std::process::id()));
    let tmp = path.with_file_name(name);
    {
        let mut opts = fs::OpenOptions::new();
        opts.write(true).create(true).truncate(true);
        #[cfg(unix)]
        {
            use std::os::unix::fs::OpenOptionsExt;
            opts.mode(0o600);
        }
        let mut f = opts.open(&tmp)?;
        f.write_all(data)?;
        f.sync_all()?;
    }
    fs::rename(&tmp, path)?;
    tighten_perms(path);
    Ok(())
}

fn tighten_perms(path: &Path) {
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        let _ = fs::set_permissions(path, fs::Permissions::from_mode(0o600));
    }
    #[cfg(not(unix))]
    let _ = path;
}
