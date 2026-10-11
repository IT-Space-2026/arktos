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
//!   addresses u32le count, then per address (ADDR01/02 tail — files
//!   written before it simply end after the forms, which a reader
//!   decodes as an empty list; readers predating it ignore the tail):
//!     country      u32le len + utf8
//!     name         u32le len + utf8
//!     organization u32le len + utf8
//!     street       u32le len + utf8
//!     city         u32le len + utf8
//!     state        u32le len + utf8
//!     zip          u32le len + utf8
//!     phone        u32le len + utf8
//!     email        u32le len + utf8
//!     last_used    i64le (UTC seconds, 0 = never)
//! ```
//!
//! The C ABI carries each record set as a JSON array:
//!   [{"url","name","has_password":bool,"elements":[["k","v"],...]}]
//!   [{"country","name","organization","street","city","state","zip",
//!     "phone","email","last_used":int}]
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
const MAX_ADDRESSES: usize = 16384;
const MAX_FIELD: usize = 1024 * 1024;
const MAX_JSON_IN: usize = 64 * 1024 * 1024;

#[derive(Clone, Debug, PartialEq)]
pub struct FormRecord {
    pub url: String,
    pub name: String,
    pub has_password: bool,
    pub elements: Vec<(String, String)>,
}

// ADDR01/02: a saved postal/contact address — same record the Qt
// side's autofill.dat carries in its own appended section.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct AddressRecord {
    pub country: String,
    pub name: String,
    pub organization: String,
    pub street: String,
    pub city: String,
    pub state: String,
    pub zip: String,
    pub phone: String,
    pub email: String,
    pub last_used: i64,
}

// The whole payload: the original form block plus the address tail.
#[derive(Clone, Debug, PartialEq, Default)]
pub struct AutofillData {
    pub forms: Vec<FormRecord>,
    pub addresses: Vec<AddressRecord>,
}

// ---- binary codec -----------------------------------------------------

fn serialize(data: &AutofillData) -> Vec<u8> {
    let mut out = Vec::new();
    out.extend_from_slice(MAGIC);
    let forms = &data.forms;
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
    // ADDR01/02 tail — appended so a store written before it still
    // parses whole (its readers simply stop after the forms).
    out.extend_from_slice(&(data.addresses.len() as u32).to_le_bytes());
    for address in &data.addresses {
        put_str(&mut out, &address.country);
        put_str(&mut out, &address.name);
        put_str(&mut out, &address.organization);
        put_str(&mut out, &address.street);
        put_str(&mut out, &address.city);
        put_str(&mut out, &address.state);
        put_str(&mut out, &address.zip);
        put_str(&mut out, &address.phone);
        put_str(&mut out, &address.email);
        out.extend_from_slice(&address.last_used.to_le_bytes());
    }
    out
}

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    fail(RcStatus::Corrupt, msg)
}

fn parse(data: &[u8]) -> RcResult<AutofillData> {
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
    // The ADDR01/02 address tail — absent on stores written before it
    // (they simply end after the forms, which decodes as an empty
    // list).  Writes are atomic, so a started-but-truncated tail is
    // corruption, not a format variant.
    let mut addresses = Vec::new();
    if at < data.len() {
        let count = count_of(&mut at, MAX_ADDRESSES)?;
        addresses.reserve(count.min(64));
        for _ in 0..count {
            let last_used = |at: &mut usize| -> RcResult<i64> {
                Ok(i64::from_le_bytes(take(at, 8)?.try_into().unwrap()))
            };
            addresses.push(AddressRecord {
                country: string(&mut at)?,
                name: string(&mut at)?,
                organization: string(&mut at)?,
                street: string(&mut at)?,
                city: string(&mut at)?,
                state: string(&mut at)?,
                zip: string(&mut at)?,
                phone: string(&mut at)?,
                email: string(&mut at)?,
                last_used: last_used(&mut at)?,
            });
        }
    }
    Ok(AutofillData { forms, addresses })
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

// ---- JSON bridge: addresses -----------------------------------------------
//
// The Qt adapter mirrors an Address as
// {"country","name","organization","street","city","state","zip",
//  "phone","email","last_used":int}.  Missing fields default like the
// form records do; malformed entries reject the whole set.

fn address_record_to_json(address: &AddressRecord) -> Value {
    json!({
        "country": address.country,
        "name": address.name,
        "organization": address.organization,
        "street": address.street,
        "city": address.city,
        "state": address.state,
        "zip": address.zip,
        "phone": address.phone,
        "email": address.email,
        "last_used": address.last_used,
    })
}

fn json_str_or_default(o: &Map<String, Value>, key: &str) -> String {
    o.get(key).and_then(Value::as_str).unwrap_or("").to_owned()
}

fn address_from_json(v: &Value) -> RcResult<AddressRecord> {
    let o = v.as_object().ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "autofill json: address is not an object".into(),
    })?;
    let last_used = match o.get("last_used") {
        None => 0,
        Some(v) => v.as_i64().ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "autofill json: bad last_used".into(),
        })?,
    };
    Ok(AddressRecord {
        country: json_str_or_default(o, "country"),
        name: json_str_or_default(o, "name"),
        organization: json_str_or_default(o, "organization"),
        street: json_str_or_default(o, "street"),
        city: json_str_or_default(o, "city"),
        state: json_str_or_default(o, "state"),
        zip: json_str_or_default(o, "zip"),
        phone: json_str_or_default(o, "phone"),
        email: json_str_or_default(o, "email"),
        last_used,
    })
}

/// Parses the JSON address list the Qt adapter sends — same all-or-
/// nothing contract as forms_from_json.
pub fn addresses_from_json(data: &[u8]) -> RcResult<Vec<AddressRecord>> {
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
    if list.len() > MAX_ADDRESSES {
        return fail(
            RcStatus::InvalidArgument,
            "autofill json: too many addresses",
        );
    }
    list.iter().map(address_from_json).collect()
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

    /// Decoded payload, decrypting autofill-store.dat on first access
    /// under the current custody key (file-key fallback included via
    /// open()).
    pub fn autofill_data(&mut self) -> RcResult<&mut AutofillData> {
        if self.autofill.is_none() {
            let path = self.autofill_path()?;
            let data = if path.exists() {
                let blob = fs::read(&path)?;
                let plain = self.open(&blob)?;
                parse(&plain)?
            } else {
                AutofillData::default()
            };
            self.autofill = Some(data);
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
        if self.autofill_data()?.forms.as_slice() == records.as_slice() {
            return Ok(false);
        }
        self.autofill_data()?.forms = records;
        self.save_autofill()?;
        Ok(true)
    }

    /// Same wholesale replace for the ADDR01/02 address list — the
    /// sections share one sealed payload, so either write re-seals
    /// both.
    pub fn autofill_set_addresses(
        &mut self,
        records: Vec<AddressRecord>,
    ) -> RcResult<bool> {
        if self.autofill_data()?.addresses.as_slice() == records.as_slice() {
            return Ok(false);
        }
        self.autofill_data()?.addresses = records;
        self.save_autofill()?;
        Ok(true)
    }

    /// JSON listing for the Qt adapter.
    pub fn autofill_json(&mut self) -> RcResult<String> {
        let records: Vec<Value> = self
            .autofill_data()?
            .forms
            .iter()
            .map(record_to_json)
            .collect();
        Ok(Value::Array(records).to_string())
    }

    /// JSON listing of the saved-address set for the Qt adapter.
    pub fn autofill_addresses_json(&mut self) -> RcResult<String> {
        let records: Vec<Value> = self
            .autofill_data()?
            .addresses
            .iter()
            .map(address_record_to_json)
            .collect();
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

    fn sample_address() -> AddressRecord {
        AddressRecord {
            country: "US".into(),
            name: "Ada".into(),
            organization: "Example".into(),
            street: "1 Main St\nApt 2".into(),
            city: "Springfield".into(),
            state: "IL".into(),
            zip: "62701".into(),
            phone: "+1 555 0100".into(),
            email: "ada@example.com".into(),
            last_used: 1_700_000_000,
        }
    }

    #[test]
    fn codec_round_trip() {
        let data = AutofillData {
            forms: sample(),
            addresses: vec![sample_address()],
        };
        assert_eq!(parse(&serialize(&data)).unwrap(), data);
        assert_eq!(
            parse(&serialize(&AutofillData::default())).unwrap(),
            AutofillData::default()
        );
        // ADDR01/02 compat: a payload that ends after the forms (the
        // pre-address schema) decodes with an empty address list.
        let mut legacy = Vec::new();
        legacy.extend_from_slice(MAGIC);
        let forms_only = AutofillData {
            forms: sample(),
            addresses: Vec::new(),
        };
        // Reproduce the old layout by serializing just the forms block.
        legacy.extend_from_slice(
            &(forms_only.forms.len() as u32).to_le_bytes(),
        );
        let put_str = |out: &mut Vec<u8>, s: &str| {
            out.extend_from_slice(&(s.len() as u32).to_le_bytes());
            out.extend_from_slice(s.as_bytes());
        };
        for form in &forms_only.forms {
            put_str(&mut legacy, &form.url);
            put_str(&mut legacy, &form.name);
            legacy.push(u8::from(form.has_password));
            legacy.extend_from_slice(
                &(form.elements.len() as u32).to_le_bytes(),
            );
            for (k, v) in &form.elements {
                put_str(&mut legacy, k);
                put_str(&mut legacy, v);
            }
        }
        let decoded = parse(&legacy).unwrap();
        assert_eq!(decoded.forms, forms_only.forms);
        assert!(decoded.addresses.is_empty());
    }

    #[test]
    fn codec_rejects_garbage() {
        // Wrong magic.
        assert!(parse(b"NOPE").is_err());
        // Truncated everywhere: counts promise more than the bytes hold.
        let good = serialize(&AutofillData {
            forms: sample(),
            addresses: vec![sample_address()],
        });
        // The one legal prefix: the pre-ADDR01 layout ends after the
        // forms block, which still parses as forms+empty addresses.
        let forms_boundary =
            serialize(&AutofillData {
                forms: sample(),
                addresses: Vec::new(),
            })
            .len()
            - 4;
        for n in 0..good.len() {
            if n == forms_boundary {
                continue;
            }
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

    #[test]
    fn address_json_round_trip() {
        let addresses = vec![sample_address()];
        let json = Value::Array(
            addresses.iter().map(address_record_to_json).collect(),
        );
        assert_eq!(
            addresses_from_json(json.to_string().as_bytes()).unwrap(),
            addresses
        );
        // Bad shapes reject whole; missing fields default.
        assert!(addresses_from_json(b"{}").is_err());
        assert!(addresses_from_json(b"[42]").is_err());
        let got = addresses_from_json(b"[{\"name\":\"Ada\"}]").unwrap();
        assert_eq!(got[0].name, "Ada");
        assert_eq!(got[0].last_used, 0);
        assert!(addresses_from_json(b"[{\"last_used\":\"x\"}]").is_err());
    }
}
