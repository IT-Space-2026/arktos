//! Named-credential store (rc_cred_*) — a small key/value map sealed
//! as a single blob in credentials.dat under the current custody key.
//! This is the forward-looking half of RCORE01: future components use
//! it directly, while the Qt SecureStore shim drives the custody
//! primitives underneath it.
//!
//! File contents after opening: u32le count, then per entry
//! u32le nameLen | name utf8 | u32le valueLen | value bytes.

use std::fs;

use zeroize::Zeroizing;

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::store::{CredMap, Store, KEY_SIZE};

const MAX_NAME_LEN: usize = 1024;
const MAX_VALUE_LEN: usize = 1 << 20;

fn serialize(map: &CredMap) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(&(map.len() as u32).to_le_bytes());
    for (name, value) in map {
        out.extend_from_slice(&(name.len() as u32).to_le_bytes());
        out.extend_from_slice(name.as_bytes());
        out.extend_from_slice(&(value.len() as u32).to_le_bytes());
        out.extend_from_slice(value);
    }
    out
}

fn parse(data: &[u8]) -> RcResult<CredMap> {
    let mut map = CredMap::new();
    let mut at = 0usize;
    let take = |at: &mut usize, n: usize| -> RcResult<&[u8]> {
        if data.len() - *at < n {
            return fail(RcStatus::Corrupt, "truncated credential map");
        }
        let s = &data[*at..*at + n];
        *at += n;
        Ok(s)
    };
    let count = u32::from_le_bytes(take(&mut at, 4)?.try_into().unwrap());
    for _ in 0..count {
        let nlen = u32::from_le_bytes(take(&mut at, 4)?.try_into().unwrap()) as usize;
        let name = take(&mut at, nlen)?;
        let name = std::str::from_utf8(name).map_err(|_| Fail {
            status: RcStatus::Corrupt,
            msg: "credential name is not utf-8".into(),
        })?;
        let vlen = u32::from_le_bytes(take(&mut at, 4)?.try_into().unwrap()) as usize;
        let value = take(&mut at, vlen)?;
        map.insert(name.to_string(), Zeroizing::new(value.to_vec()));
    }
    Ok(map)
}

impl Store {
    /// Decoded credential map, decrypting credentials.dat on first
    /// access under the current custody key (file-key fallback
    /// included via open()).
    pub fn creds(&mut self) -> RcResult<&mut CredMap> {
        if self.creds.is_none() {
            let path = self.cred_path()?;
            let map = if path.exists() {
                let blob = fs::read(&path)?;
                let plain = self.open(&blob)?;
                parse(&plain)?
            } else {
                CredMap::new()
            };
            self.creds = Some(map);
        }
        Ok(self.creds.as_mut().unwrap())
    }

    fn save_creds(&mut self) -> RcResult<()> {
        let plain = serialize(self.creds.as_ref().unwrap());
        let blob = self.seal(&plain)?;
        crate::store::atomic_write(&self.cred_path()?, &blob)
    }

    pub fn cred_get(&mut self, name: &str) -> RcResult<Vec<u8>> {
        match self.creds()?.get(name) {
            Some(v) => Ok(v.to_vec()),
            None => fail(RcStatus::NotFound, "no such credential"),
        }
    }

    /// Returns Ok(false) when nothing changed (same value already
    /// stored) — the caller uses that to skip the change callback.
    pub fn cred_put(&mut self, name: &str, value: &[u8]) -> RcResult<bool> {
        if name.is_empty() || name.len() > MAX_NAME_LEN {
            return fail(RcStatus::InvalidArgument, "bad credential name");
        }
        if value.len() > MAX_VALUE_LEN {
            return fail(RcStatus::InvalidArgument, "credential too large");
        }
        if self.creds()?.get(name).map(|v| v.as_slice()) == Some(value) {
            return Ok(false);
        }
        self.creds()?
            .insert(name.to_string(), Zeroizing::new(value.to_vec()));
        self.save_creds()?;
        Ok(true)
    }

    pub fn cred_remove(&mut self, name: &str) -> RcResult<bool> {
        if self.creds()?.remove(name).is_none() {
            return fail(RcStatus::NotFound, "no such credential");
        }
        self.save_creds()?;
        Ok(true)
    }

    pub fn cred_names(&mut self) -> RcResult<Vec<String>> {
        Ok(self.creds()?.keys().cloned().collect())
    }

    /// Re-seals credentials.dat from one explicit key to another —
    /// the custody-transition hook the Qt side invokes alongside its
    /// own consumer re-sealing.  Missing file is a no-op; a file that
    /// does not open under `from` surfaces Corrupt/Crypto so the
    /// caller can warn-and-continue like the other consumers.
    pub fn cred_reseal(
        &mut self,
        from: &[u8; KEY_SIZE],
        to: &[u8; KEY_SIZE],
    ) -> RcResult<()> {
        let path = self.cred_path()?;
        if !path.exists() {
            return Ok(());
        }
        let blob = fs::read(&path)?;
        let plain = crate::store::open_with_key(from, &blob)?;
        let out = crate::store::seal_with_key(to, &plain)?;
        crate::store::atomic_write(&path, &out)
    }

    /// Whole-op passphrase rotation for consumers that have no
    /// external stores to re-seal (the Qt SecureStore shim instead
    /// drives kdf_create + explicit-key re-sealing stage by stage so
    /// its autofill.dat/QSettings consumers keep crash ordering).
    /// Covers every Rust-owned custody file: credentials.dat and
    /// autofill-store.dat.
    pub fn cred_change_passphrase(&mut self, pass: &[u8]) -> RcResult<()> {
        if !self.passphrase_enabled() {
            return fail(
                RcStatus::NotEnabled,
                "passphrase protection is not enabled",
            );
        }
        let old = match &self.derived_key {
            Some(k) => Zeroizing::new(**k),
            None => return fail(RcStatus::Locked, "the store is locked"),
        };
        self.kdf_create(pass)?;
        let new = Zeroizing::new(**self.derived_key.as_ref().unwrap());
        self.cred_reseal(&old, &new)?;
        self.autofill_reseal(&old, &new)
    }
}
