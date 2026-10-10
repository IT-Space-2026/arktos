//! RCORE05: the autofill record store — saved form data under the
//! same AEAD + Argon2id custody as rc_cred_* (one unlock opens both,
//! the same passphrase lifecycle and the same reseal transitions).
//!
//! On-disk format (`<data dir>/autofill-store.dat`) is the shared
//! "ARSEC1" sealed envelope holding a versioned, bounds-checked
//! binary payload the crate owns end to end:
//!
//! ```text
//!   magic    "AUT1" (4 bytes — version 1 of the schema)
//!   forms    u32le count, then per form:
//!     url          u32le len + utf8   (already query/fragment/user-
//!                                    info stripped by the caller)
//!     name         u32le len + utf8
//!     has_password u8 (0/1)
//!     elements     u32le count, then per element:
//!       key   u32le len + utf8
//!       value u32le len + utf8
//! ```
//!
//! The C ABI carries the same records as a JSON array:
//!   [{"url","name","has_password":bool,"elements":[["k","v"],...]}]
//!
//! Every count is bounds-checked against the bytes actually remaining
//! before any allocation, so a corrupt file is rejected whole with
//! RcStatus::Corrupt — never half-parsed into the store.  Writes are
//! atomic (temp + fsync + rename, 0600 via store::atomic_write).
//!
//! The legacy autofill.dat (ARSEC1-wrapped or Qt4-era plaintext
//! QDataStream) is a READ-ONLY import source: the Qt side parses it
//! once with its existing bounded reader and replays the records
//! through rc_autofill_set_forms, then retires the file.  New writes
//! never reach it — like the history.db migration, the two stores
//! keep separate names so a no-Rust build never trips over a payload
//! it cannot read.

use std::fs;
use std::path::PathBuf;

use serde_json::{json, Map, Value};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::store::{Store, KEY_SIZE};

pub const FILE_NAME: &str = "autofill-store.dat";
const MAGIC: &[u8; 4] = b"AUT1";

// Bounds — generous for real autofill sets, small enough that a
// corrupt count cannot allocate absurdly.  Lengths are always also
// checked against remaining bytes; these caps are the second line.
const MAX_FORMS: usize = 16384;
const MAX_ELEMENTS: usize = 4096;
const MAX_FIELD: usize = 1024 * 1024;
const MAX_JSON_IN: usize = 64 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq)]
pub struct FormRecord {
    pub url: String,
    pub name: String,
    pub has_password: bool,
    pub elements: Vec<(String, String)>,
}

// ---- binary codec -----------------------------------------------------

fn serialize(forms: &[FormRecord]) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&(forms.len() as u32).to_le_bytes());
    let put_str = |out: &mut Vec<u8>, s: &str| {
        out.extend_from_slice(&(s.len() as u32).to_le_bytes());
        out.extend_from_slice(s.as_bytes());
    };
    for form in forms {
        put_str(&mut out, &form.url);
        put_str(&mut out, &form.name);
        out.push(u8::from(form.has_password));
        out.extend_from_slice(&(form.elements.len() as u32).to_le_bytes());
        for (key, value) in &form.elements {
            put_str(&mut out, key);
            put_str(&mut out, value);
        }
    }
    out
}

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::Corrupt, msg)
}

fn parse(data: &[u8]) -> RcResult<Vec<FormRecord>> {
    let mut at = 0usize;
    let take = |at: &mut usize, n: usize| -> RcResult<&[u8]> {
        if data.len() - *at < n {
            return corrupt("autofill store: truncated");
        }
        let s = &data[*at..*at + n];
        *at += n;
        Ok(s)
    };
    let count_of = |at: &mut usize, cap: usize| -> RcResult<usize> {
        let n = u32::from_le_bytes(take(at, 4)?.try_into().unwrap()) as usize;
        if n > cap {
            return corrupt("autofill store: count over limit");
        }
        Ok(n)
    };
    let string = |at: &mut usize| -> RcResult<String> {
        let n = u32::from_le_bytes(take(at, 4)?.try_into().unwrap()) as usize;
        if n > MAX_FIELD {
            return corrupt("autofill store: field over limit");
        }
        let raw = take(at, n)?;
        std::str::from_utf8(raw)
            .map(str::to_owned)
            .map_err(|_| Fail {
                status: RcStatus::Corrupt,
                msg: "autofill store: field is not utf-8".into(),
            })
    };

    if take(&mut at, 4)? != MAGIC {
        return corrupt("autofill store: bad magic");
    }
    let count = count_of(&mut at, MAX_FORMS)?;
    let mut forms = Vec::with_capacity(count.min(64));
    for _ in 0..count {
        let url = string(&mut at)?;
        let name = string(&mut at)?;
        let has_password = take(&mut at, 1)?[0] != 0;
        let element_count = count_of(&mut at, MAX_ELEMENTS)?;
        let mut elements = Vec::with_capacity(element_count.min(32));
        for _ in 0..element_count {
            let key = string(&mut at)?;
            let value = string(&mut at)?;
            elements.push((key, value));
        }
        forms.push(FormRecord {
            url,
            name,
            has_password,
            elements,
        });
    }
    Ok(forms)
}

// ---- JSON bridge --------------------------------------------------------
//
// The Qt adapter mirrors a Form as {"url","name","has_password",
// "elements":[["key","value"],...]}.  Unknown keys are ignored so a
// future schema bump stays compatible; type mismatches reject the
// whole document (the store is atomic, never partially replaced).

fn record_to_json(form: &FormRecord) -> Value {
    let elements: Vec<Value> = form
        .elements
        .iter()
        .map(|(k, v)| json!([k, v]))
        .collect();
    json!({
        "url": form.url,
        "name": form.name,
        "has_password": form.has_password,
        "elements": elements,
    })
}

fn json_str<'a>(o: &'a Map<String, Value>, key: &str) -> RcResult<&'a str> {
    match o.get(key).and_then(Value::as_str) {
        Some(s) => Ok(s),
        None => fail(
            RcStatus::InvalidArgument,
            format!("autofill json: bad {key}"),
        ),
    }
}

fn record_from_json(v: &Value) -> RcResult<FormRecord> {
    let o = v.as_object().ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "autofill json: form is not an object".into(),
    })?;
    let url = json_str(o, "url")?.to_owned();
    let name = json_str(o, "name").unwrap_or_default().to_owned();
    let has_password = o
        .get("has_password")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let mut elements = Vec::new();
    if let Some(list) = o.get("elements") {
        let list = list.as_array().ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "autofill json: elements is not an array".into(),
        })?;
        if list.len() > MAX_ELEMENTS {
            return fail(
                RcStatus::InvalidArgument,
                "autofill json: too many elements",
            );
        }
        for pair in list {
            let pair = pair.as_array().ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "autofill json: element is not a pair".into(),
            })?;
            let key = pair.first().and_then(Value::as_str).ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "autofill json: element key is not a string".into(),
            })?;
            let value = pair
                .get(1)
                .and_then(Value::as_str)
                .ok_or_else(|| Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "autofill json: element value is not a string".into(),
                })?;
            elements.push((key.to_owned(), value.to_owned()));
        }
    }
    Ok(FormRecord {
        url,
        name,
        has_password,
        elements,
    })
}

/// Parses the JSON form list the Qt adapter sends.  Input is capped
/// and every entry type-checked — a malformed document rejects the
/// whole set, never partially replaces it.
pub fn forms_from_json(data: &[u8]) -> RcResult<Vec<FormRecord>> {
    if data.len() > MAX_JSON_IN {
        return fail(RcStatus::InvalidArgument, "autofill json too large");
    }
    let text = std::str::from_utf8(data).map_err(|_| Fail {
        status: RcStatus::InvalidArgument,
        msg: "autofill json: not UTF-8".into(),
    })?;
    let v: Value = serde_json::from_str(text).map_err(|e| Fail {
        status: RcStatus::InvalidArgument,
        msg: format!("autofill json: {e}"),
    })?;
    let list = v.as_array().ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "autofill json: not an array".into(),
    })?;
    if list.len() > MAX_FORMS {
        return fail(RcStatus::InvalidArgument, "autofill json: too many forms");
    }
    list.iter().map(record_from_json).collect()
}

// ---- Store integration ---------------------------------------------------

impl Store {
    pub fn autofill_path(&self) -> RcResult<PathBuf> {
        Ok(self.dir()?.join(FILE_NAME))
    }

    /// Whether the Rust store file exists — gates the Qt side's
    /// legacy autofill.dat import (import once, then the new store is
    /// canonical).
    pub fn autofill_store_present(&self) -> bool {
        self.autofill_path().map(|p| p.exists()).unwrap_or(false)
    }

    /// Decoded record list, decrypting autofill-store.dat on first
    /// access under the current custody key (file-key fallback
    /// included via open()).
    pub fn autofill_records(&mut self) -> RcResult<&mut Vec<FormRecord>> {
        if self.autofill.is_none() {
            let path = self.autofill_path()?;
            let records = if path.exists() {
                let blob = fs::read(&path)?;
                let plain = self.open(&blob)?;
                parse(&plain)?
            } else {
                Vec::new()
            };
            self.autofill = Some(records);
        }
        Ok(self.autofill.as_mut().unwrap())
    }

    fn save_autofill(&mut self) -> RcResult<()> {
        let plain = serialize(self.autofill.as_ref().unwrap());
        let blob = self.seal(&plain)?;
        crate::store::atomic_write(&self.autofill_path()?, &blob)
    }

    /// Wholescale replace + persist.  Returns Ok(false) when the set
    /// is unchanged — the caller uses that to skip the change notify.
    pub fn autofill_set(&mut self, records: Vec<FormRecord>) -> RcResult<bool> {
        if self.autofill_records()?.as_slice() == records.as_slice() {
            return Ok(false);
        }
        *self.autofill_records()? = records;
        self.save_autofill()?;
        Ok(true)
    }

    /// JSON listing for the Qt adapter.
    pub fn autofill_json(&mut self) -> RcResult<String> {
        let records: Vec<Value> =
            self.autofill_records()?.iter().map(record_to_json).collect();
        Ok(Value::Array(records).to_string())
    }

    /// Re-seals autofill-store.dat between explicit keys during a
    /// custody transition — the same hook credentials.dat gets.
    /// Missing file is a no-op; a file that does not open under
    /// `from` surfaces Corrupt/Crypto so the caller can warn-and-
    /// continue like the other consumers.
    pub fn autofill_reseal(
        &mut self,
        from: &[u8; KEY_SIZE],
        to: &[u8; KEY_SIZE],
    ) -> RcResult<()> {
        let path = self.autofill_path()?;
        if !path.exists() {
            return Ok(());
        }
        let blob = fs::read(&path)?;
        let plain = crate::store::open_with_key(from, &blob)?;
        let out = crate::store::seal_with_key(to, &plain)?;
        crate::store::atomic_write(&path, &out)?;
        // Keep the decoded cache consistent with the new custody.
        self.autofill = None;
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> Vec<FormRecord> {
        vec![
            FormRecord {
                url: "https://a.example/login".into(),
                name: "loginform".into(),
                has_password: true,
                elements: vec![
                    ("user".into(), "alice".into()),
                    ("pass".into(), "s3cret".into()),
                ],
            },
            FormRecord {
                url: "https://b.example/checkout".into(),
                name: String::new(),
                has_password: false,
                elements: vec![("card".into(), "4111111111111111".into())],
            },
        ]
    }

    #[test]
    fn codec_round_trip() {
        let forms = sample();
        let blob = serialize(&forms);
        assert_eq!(parse(&blob).unwrap(), forms);
        assert_eq!(parse(&serialize(&[])).unwrap(), Vec::new());
    }

    #[test]
    fn codec_rejects_garbage() {
        // Wrong magic.
        assert!(parse(b"NOPE").is_err());
        // Truncated everywhere: counts promise more than the bytes hold.
        let good = serialize(&sample());
        for n in 0..good.len() {
            assert!(
                parse(&good[..n]).is_err(),
                "prefix {n} must not parse"
            );
        }
        // Count over limit.
        let mut bad = MAGIC.to_vec();
        bad.extend_from_slice(&(MAX_FORMS as u32 + 1).to_le_bytes());
        assert!(parse(&bad).is_err());
        // Trailing junk is tolerated (forward-compat headroom) — the
        // whole-prefix loop above proves nothing essential is skipped.
    }

    #[test]
    fn json_round_trip_and_validation() {
        let forms = sample();
        let json = Value::Array(forms.iter().map(record_to_json).collect());
        let text = json.to_string();
        assert_eq!(forms_from_json(text.as_bytes()).unwrap(), forms);

        // Bad shapes reject whole.
        assert!(forms_from_json(b"{}").is_err());
        assert!(forms_from_json(b"[42]").is_err());
        assert!(forms_from_json(b"[{\"url\":1}]").is_err());
        assert!(forms_from_json(
            b"[{\"url\":\"u\",\"elements\":[[\"k\"]]}]"
        )
        .is_err());
        // Missing optional fields get defaults.
        let got = forms_from_json(b"[{\"url\":\"u\"}]").unwrap();
        assert_eq!(got[0].name, "");
        assert!(!got[0].has_password);
        assert!(got[0].elements.is_empty());
    }
}
