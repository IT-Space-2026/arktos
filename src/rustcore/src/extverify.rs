//! EXT06: extension-package verification — the untrusted-bytes
//! boundary of the extension system.  Extension packages (.zip and
//! .crx) and self-update payloads are attacker-influenced data that
//! used to flow straight into Qt's installer; this module proves the
//! structure out in memory-safe Rust before Qt sees a byte.
//!
//!   * rc_ext_verify_package — whole-package verdict as JSON:
//!     {"status","error","format","signing","signature_check",
//!      "pinning","crx_id","sha256","expected_sha256","entries",
//!      "uncompressed_total","manifest"}
//!       - "status": "valid" | "rejected" ("error" carries the reason
//!         on rejection — RC_OK is returned either way; only argument
//!         errors produce a non-Ok RcStatus).
//!       - "format": "zip" | "crx3" | null.
//!       - "signing": "unsigned" | "signed".  A CRX3 whose header
//!         carries no proofs is "unsigned" — a classification, not a
//!         failure (community and self-hosted extensions ship
//!         unsigned).  Plain zips have no signature concept and read
//!         "unsigned" too.
//!       - "signature_check": honesty field — "structure-only" always
//!         in v1: the CRX3 header's protobuf shape is verified
//!         (proofs present, signed_header_data parses, crx_id well
//!         formed) but the RSA/ECDSA signatures are NOT
//!         cryptographically verified — Chromium-Web-Store-grade
//!         verification is v2 scope, same conclusion EXT05(e) reached.
//!       - "pinning": "not-requested" | "no-pinning-configured" — a
//!         caller-supplied pinned pubkey is acknowledged and honestly
//!         reported as unconfigured in v1.
//!       - "expected_sha256": "match" | "mismatch" | "not-declared" —
//!         the update manifest's declared digest vs the downloaded
//!         bytes (the tamper check available TODAY without store
//!         signatures).  A mismatch rejects the package.
//!       - "manifest": the rc_ext_manifest_check verdict for the
//!         embedded manifest.json, or null when the package carries
//!         none (or it cannot be read within bounds).
//!
//!     Zip sanity before Qt's unzip code runs: end-of-central-dir
//!     located and bounds-checked, multi-disk and zip64 refused,
//!     every central-dir entry walked — entry-count and
//!     uncompressed-size caps enforced, local-header offsets verified,
//!     and entry names rejected on absolute paths, drive letters, NUL
//!     bytes and `..` components (both '/' and '\' count as
//!     separators — an extraction-time traversal must never write
//!     outside the target dir).
//!
//!   * rc_ext_manifest_check — the full manifest.json schema +
//!     permission parse in Rust (serde_json, 1 MiB bound — closes the
//!     SEC19 "manifest.json stays Qt" gap now that manifests arrive
//!     inside untrusted packages).  Result JSON:
//!     {"valid","manifest_version","name","version","description",
//!      "update_url","key","has_background","has_action",
//!      "content_script_count","permissions","host_permissions",
//!      "unsupported","unverified","dangerous","errors"}
//!     "unsupported"/"unverified" mirror ExtensionManager's chrome.*
//!     delegate tables (Qt keeps its own copies for no-Rust builds —
//!     the lists must move in lockstep).  "dangerous" is the
//!     EXT05 deny-by-default consent set: nativeMessaging, debugger,
//!     webRequestBlocking, cookies, browsingData, and <all_urls>/
//!     wildcard host_permissions.  RC_CORRUPT means the JSON itself
//!     could not be read — field-level problems land in "errors".

use serde_json::{json, Map, Value};
use sha2::{Digest, Sha256};

use crate::error::{Fail, RcResult, RcStatus};

/// Mirrors ExtensionManager's kUpdatePackageMaxBytes.
const MAX_PACKAGE: usize = 64 * 1024 * 1024;
/// CRX3 protobuf header bound — the whole proof block, generous.
const MAX_CRX_HEADER: usize = 4 * 1024 * 1024;
/// Central-directory entry cap — real extensions carry tens.
const MAX_ZIP_ENTRIES: u64 = 10_000;
/// Sum of uncompressed member sizes — a zip-bomb ceiling a browser
/// extension can never legitimately reach.
const MAX_UNCOMPRESSED_TOTAL: u64 = 512 * 1024 * 1024;
/// Per-member uncompressed cap.
const MAX_ENTRY_UNCOMPRESSED: u64 = 64 * 1024 * 1024;
/// manifest.json cap — mirrors inspectManifest's bound.
const MAX_MANIFEST: usize = 1024 * 1024;

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    Err(Fail {
        status: RcStatus::Corrupt,
        msg: msg.into(),
    })
}

// ------------------------------------------------------------------
// zip structure validation
// ------------------------------------------------------------------

fn le16(b: &[u8], off: usize) -> u16 {
    u16::from_le_bytes([b[off], b[off + 1]])
}

fn le32(b: &[u8], off: usize) -> u32 {
    u32::from_le_bytes([b[off], b[off + 1], b[off + 2], b[off + 3]])
}

/// True when `name` must never reach an extractor: absolute paths,
/// drive letters, NUL bytes and `..` components (both separators).
fn unsafe_member_name(name: &[u8]) -> bool {
    if name.is_empty() || name.contains(&0) {
        return true;
    }
    if name[0] == b'/' || name[0] == b'\\' {
        return true;
    }
    if name.len() >= 2 && name[1] == b':' && name[0].is_ascii_alphabetic() {
        return true;
    }
    name.split(|&c| c == b'/' || c == b'\\')
        .any(|comp| comp == b"..")
}

struct ZipReport {
    entries: u64,
    uncompressed_total: u64,
    manifest: Option<Vec<u8>>,
    /// manifest.json exists but could not be read within bounds.
    manifest_unreadable: Option<String>,
}

/// Reads the stored member data behind a central-dir entry.
fn member_data<'a>(
    zip: &'a [u8],
    local_off: u32,
    comp_size: u32,
) -> Result<&'a [u8], String> {
    let off = local_off as usize;
    if off + 30 > zip.len() || le32(zip, off) != 0x0403_4b50 {
        return Err("member local header is out of bounds".into());
    }
    let name_len = le16(zip, off + 26) as usize;
    let extra_len = le16(zip, off + 28) as usize;
    let data = off
        .checked_add(30)
        .and_then(|v| v.checked_add(name_len))
        .and_then(|v| v.checked_add(extra_len))
        .ok_or_else(|| "member offsets overflow".to_string())?;
    let end = data
        .checked_add(comp_size as usize)
        .ok_or_else(|| "member size overflows".to_string())?;
    if end > zip.len() {
        return Err("member data extends past end of archive".into());
    }
    Ok(&zip[data..end])
}

/// Walks the central directory end to end, enforcing caps and member
/// names, and pulls manifest.json out when it is readable.
fn check_zip(zip: &[u8]) -> Result<ZipReport, String> {
    // End of central directory: signature 50 4b 05 06, fixed 22-byte
    // record plus a comment of up to 64 KiB — scan the tail window.
    if zip.len() < 22 {
        return Err("not a zip archive (no end of central directory)".into());
    }
    let window = zip.len().min(22 + 0xFFFF);
    let tail = &zip[zip.len() - window..];
    let mut eocd = None;
    for i in (0..tail.len() - 3).rev() {
        if tail[i] == 0x50 && tail[i + 1] == 0x4b && tail[i + 2] == 0x05
            && tail[i + 3] == 0x06
        {
            eocd = Some(zip.len() - window + i);
            break;
        }
    }
    let eocd = eocd.ok_or_else(|| {
        "not a zip archive (no end of central directory)".to_string()
    })?;
    if eocd + 22 > zip.len() {
        return Err("end of central directory is truncated".into());
    }
    let comment_len = le16(zip, eocd + 20) as usize;
    if eocd + 22 + comment_len != zip.len() {
        return Err("trailing data after the end of central directory".into());
    }
    if le16(zip, eocd + 4) != 0 || le16(zip, eocd + 6) != 0 {
        return Err("multi-disk archives are not supported".into());
    }
    let entries = le16(zip, eocd + 10) as u64;
    if le16(zip, eocd + 8) as u64 != entries {
        return Err("central directory spans multiple disks".into());
    }
    let cd_size = le32(zip, eocd + 12) as usize;
    let cd_offset = le32(zip, eocd + 16) as usize;
    if entries == 0xFFFF || cd_size == 0xFFFF_FFFF || cd_offset == 0xFFFF_FFFF {
        return Err("zip64 archives are not supported".into());
    }
    if entries > MAX_ZIP_ENTRIES {
        return Err(format!("too many entries ({entries})"));
    }
    let cd_end = cd_offset
        .checked_add(cd_size)
        .ok_or_else(|| "central directory offsets overflow".to_string())?;
    if cd_end > eocd {
        return Err("central directory overlaps the record trailer".into());
    }

    let mut report = ZipReport {
        entries,
        uncompressed_total: 0,
        manifest: None,
        manifest_unreadable: None,
    };
    let mut pos = cd_offset;
    let mut manifest_at: Option<(u32, u16, u32, u32)> = None;
    for _ in 0..entries {
        if pos + 46 > cd_end || le32(zip, pos) != 0x0201_4b50 {
            return Err("central directory entry is truncated".into());
        }
        let method = le16(zip, pos + 10);
        let comp_size = le32(zip, pos + 20);
        let uncomp_size = le32(zip, pos + 24);
        let name_len = le16(zip, pos + 28) as usize;
        let extra_len = le16(zip, pos + 30) as usize;
        let cmt_len = le16(zip, pos + 32) as usize;
        let local_off = le32(zip, pos + 42);
        if comp_size == 0xFFFF_FFFF || uncomp_size == 0xFFFF_FFFF
            || local_off == 0xFFFF_FFFF
        {
            return Err("zip64 entries are not supported".into());
        }
        let name_end = pos + 46 + name_len;
        if name_end > cd_end {
            return Err("member name extends past the central directory".into());
        }
        let name = &zip[pos + 46..name_end];
        if unsafe_member_name(name) {
            return Err(format!(
                "unsafe member name {:?}",
                String::from_utf8_lossy(name)
            ));
        }
        // The member data itself must sit inside the archive.
        member_data(zip, local_off, comp_size)?;
        report.uncompressed_total += uncomp_size as u64;
        if uncomp_size as u64 > MAX_ENTRY_UNCOMPRESSED {
            return Err("a member expands past the per-entry size cap".into());
        }
        if report.uncompressed_total > MAX_UNCOMPRESSED_TOTAL {
            return Err("archive expands past the total size cap".into());
        }
        if name == b"manifest.json" {
            manifest_at = Some((local_off, method, comp_size, uncomp_size));
        }
        pos = name_end
            .checked_add(extra_len)
            .and_then(|v| v.checked_add(cmt_len))
            .ok_or_else(|| "entry offsets overflow".to_string())?;
    }
    if pos != cd_end {
        return Err("central directory size does not match its entries".into());
    }

    if let Some((local_off, method, comp_size, uncomp_size)) = manifest_at {
        report.manifest_unreadable = (|| -> Option<String> {
            if uncomp_size as usize > MAX_MANIFEST {
                return Some("manifest.json exceeds the size cap".into());
            }
            let data = match member_data(zip, local_off, comp_size) {
                Ok(d) => d,
                Err(e) => return Some(e),
            };
            let raw = match method {
                0 => data.to_vec(),
                8 => match miniz_oxide::inflate::decompress_to_vec_with_limit(
                    data,
                    MAX_MANIFEST,
                ) {
                    Ok(v) => v,
                    Err(_) => {
                        return Some("manifest.json does not inflate".into())
                    }
                },
                m => return Some(format!("manifest.json uses method {m}")),
            };
            if raw.len() != uncomp_size as usize {
                return Some("manifest.json size mismatch".into());
            }
            report.manifest = Some(raw);
            None
        })();
    }
    Ok(report)
}

// ------------------------------------------------------------------
// CRX3 header (protobuf wire format, structural verification)
// ------------------------------------------------------------------

fn pb_varint(b: &[u8], pos: &mut usize) -> Result<u64, String> {
    let mut value: u64 = 0;
    for i in 0..10 {
        if *pos >= b.len() {
            return Err("truncated varint".into());
        }
        let byte = b[*pos];
        *pos += 1;
        if i == 9 && byte > 1 {
            return Err("varint overflows".into());
        }
        value |= ((byte & 0x7F) as u64) << (7 * i);
        if byte & 0x80 == 0 {
            return Ok(value);
        }
    }
    Err("unterminated varint".into())
}

/// Walks a length-delimited protobuf body, calling `field` for each
/// wire-type-2 field.  Other wire types are skipped by length.
fn pb_walk(
    body: &[u8],
    mut field: impl FnMut(u64, &[u8]) -> Result<(), String>,
) -> Result<(), String> {
    let mut pos = 0usize;
    while pos < body.len() {
        let tag = pb_varint(body, &mut pos)?;
        let num = tag >> 3;
        let skip = match tag & 7 {
            0 => {
                pb_varint(body, &mut pos)?;
                0
            }
            1 => 8,
            2 => pb_varint(body, &mut pos)? as usize,
            5 => 4,
            w => return Err(format!("unsupported wire type {w}")),
        };
        let end = pos
            .checked_add(skip)
            .ok_or_else(|| "field length overflows".to_string())?;
        if end > body.len() {
            return Err("field extends past the message".into());
        }
        if tag & 7 == 2 {
            field(num, &body[pos..end])?;
        }
        pos = end;
    }
    Ok(())
}

struct CrxHeader {
    proofs: usize,
    crx_id: Option<Vec<u8>>,
}

fn parse_crx_header(header: &[u8]) -> Result<CrxHeader, String> {
    let mut out = CrxHeader {
        proofs: 0,
        crx_id: None,
    };
    pb_walk(header, |num, body| {
        match num {
            // sha256_with_rsa (2) / sha256_with_ecdsa (3):
            // AsymmetricKeyProof {bytes public_key=1, signature=2}.
            2 | 3 => {
                let mut has_key = false;
                let mut has_sig = false;
                pb_walk(body, |num, value| {
                    if num == 1 && !value.is_empty() {
                        has_key = true;
                    }
                    if num == 2 && !value.is_empty() {
                        has_sig = true;
                    }
                    Ok(())
                })?;
                if !has_key || !has_sig {
                    return Err("a key proof lacks its public key or signature"
                        .into());
                }
                out.proofs += 1;
            }
            // signed_header_data (5): SignedData {bytes crx_id=1}.
            5 => {
                let mut id: Option<Vec<u8>> = None;
                pb_walk(body, |num, value| {
                    if num == 1 {
                        id = Some(value.to_vec());
                    }
                    Ok(())
                })?;
                match id {
                    Some(v) if v.len() == 16 => out.crx_id = Some(v),
                    _ => {
                        return Err(
                            "signed_header_data carries no valid crx_id".into()
                        )
                    }
                }
            }
            _ => {} // verified_contents and unknown fields skipped
        }
        Ok(())
    })?;
    Ok(out)
}

// ------------------------------------------------------------------
// package verification
// ------------------------------------------------------------------

fn hex(data: &[u8]) -> String {
    let mut s = String::with_capacity(data.len() * 2);
    for b in data {
        s.push_str(&format!("{b:02x}"));
    }
    s
}

fn reject(error: impl Into<String>, extra: Map<String, Value>) -> RcResult<Vec<u8>> {
    let mut v = Map::new();
    v.insert("status".into(), json!("rejected"));
    v.insert("error".into(), json!(error.into()));
    v.insert("format".into(), Value::Null);
    v.insert("signing".into(), json!("unknown"));
    v.insert("signature_check".into(), json!("none"));
    v.insert("pinning".into(), json!("not-requested"));
    v.insert("crx_id".into(), Value::Null);
    v.insert("sha256".into(), Value::Null);
    v.insert("expected_sha256".into(), json!("not-declared"));
    v.insert("entries".into(), json!(0));
    v.insert("uncompressed_total".into(), json!(0));
    v.insert("manifest".into(), Value::Null);
    for (k, val) in extra {
        v.insert(k, val);
    }
    Ok(Value::Object(v).to_string().into_bytes())
}

/// rc_ext_verify_package body.  `expected_sha256` is the update
/// manifest's declared digest (None when the server didn't provide
/// one); `pinned_pubkey` is reserved for signature pinning — v1
/// always reports "no-pinning-configured" when it is supplied.
pub fn verify_package(
    pkg: &[u8],
    expected_sha256: Option<&[u8]>,
    pinned_pubkey: Option<&[u8]>,
) -> RcResult<Vec<u8>> {
    if pkg.len() > MAX_PACKAGE {
        return reject("the package exceeds the size cap", Map::new());
    }
    if pkg.is_empty() {
        return reject("the package is empty", Map::new());
    }
    let digest = Sha256::digest(pkg);
    let pinning = if pinned_pubkey.is_some() {
        "no-pinning-configured"
    } else {
        "not-requested"
    };

    // Format split: plain zip, or a CRX3 wrapper around a zip.
    let (format, zip, mut signing, mut crx_id) =
        if pkg.starts_with(b"Cr24") {
            if pkg.len() < 12 {
                return reject("truncated CRX3 header", Map::new());
            }
            let version = le32(pkg, 4);
            if version != 3 {
                return reject(
                    format!("unsupported CRX version {version}"),
                    Map::new(),
                );
            }
            let hlen = le32(pkg, 8) as usize;
            if hlen > MAX_CRX_HEADER || 12usize.saturating_add(hlen) > pkg.len()
            {
                return reject("the CRX3 header is out of bounds", Map::new());
            }
            let parsed = match parse_crx_header(&pkg[12..12 + hlen]) {
                Ok(h) => h,
                Err(e) => {
                    return reject(
                        format!("malformed CRX3 header: {e}"),
                        Map::new(),
                    )
                }
            };
            let signing = if parsed.proofs > 0 {
                "signed"
            } else {
                "unsigned"
            };
            let crx_id = parsed.crx_id.map(|id| hex(&id));
            ("crx3", &pkg[12 + hlen..], signing, crx_id)
        } else if pkg.starts_with(b"PK\x03\x04") {
            ("zip", &pkg[..], "unsigned", None)
        } else {
            return reject(
                "unrecognized package format (not a zip or CRX3)",
                Map::new(),
            );
        };

    let report = match check_zip(zip) {
        Ok(r) => r,
        Err(e) => {
            let mut extra = Map::new();
            extra.insert("format".into(), json!(format));
            extra.insert("signing".into(), json!(signing));
            extra.insert("sha256".into(), json!(hex(&digest)));
            extra.insert("pinning".into(), json!(pinning));
            extra.insert("crx_id".into(), match crx_id.take() {
                Some(id) => json!(id),
                None => Value::Null,
            });
            return reject(format!("zip check failed: {e}"), extra);
        }
    };

    let expected = match expected_sha256 {
        None => "not-declared",
        Some(e) if e == &digest[..] => "match",
        Some(_) => "mismatch",
    };
    if expected == "mismatch" {
        let mut extra = Map::new();
        extra.insert("format".into(), json!(format));
        extra.insert("signing".into(), json!(signing));
        extra.insert("sha256".into(), json!(hex(&digest)));
        extra.insert("pinning".into(), json!(pinning));
        extra.insert("expected_sha256".into(), json!("mismatch"));
        extra.insert("entries".into(), json!(report.entries));
        return reject(
            "sha256 mismatch against the update manifest's declared hash",
            extra,
        );
    }

    // The manifest rides inside the verdict so the review dialog
    // consumes pre-validated fields — never the raw JSON.
    let manifest = match report.manifest.as_deref() {
        Some(raw) => match manifest_check(raw) {
            Ok(v) => serde_json::from_slice::<Value>(&v)
                .unwrap_or(Value::Null),
            Err(e) => json!({ "valid": false, "errors": [e.msg] }),
        },
        None => match &report.manifest_unreadable {
            Some(e) => json!({ "valid": false, "errors": [e] }),
            None => Value::Null,
        },
    };

    if signing == "signed" && crx_id.is_none() {
        // proofs without signed_header_data cannot happen here — a
        // malformed header already rejected — but keep the invariant.
        signing = "unsigned";
    }

    Ok(json!({
        "status": "valid",
        "error": Value::Null,
        "format": format,
        "signing": signing,
        "signature_check": if signing == "signed" {
            "structure-only"
        } else {
            "none"
        },
        "pinning": pinning,
        "crx_id": crx_id,
        "sha256": hex(&digest),
        "expected_sha256": expected,
        "entries": report.entries,
        "uncompressed_total": report.uncompressed_total,
        "manifest": manifest,
    })
    .to_string()
    .into_bytes())
}

// ------------------------------------------------------------------
// manifest.json validation
// ------------------------------------------------------------------

// chrome.* APIs whose browser-side delegates Qt WebEngine does not
// provide — the same table ExtensionManager::unsupportedPermissions()
// carries for no-Rust builds; keep both copies in lockstep.
const UNSUPPORTED_PERMISSIONS: &[&str] = &[
    "accessibilityFeatures.modify",
    "accessibilityFeatures.read",
    "audio",
    "bookmarks",
    "browsingData",
    "certificateProvider",
    "contentSettings",
    "contextMenus",
    "cookies",
    "debugger",
    "desktopCapture",
    "documentScan",
    "downloads",
    "enterprise.deviceAttributes",
    "enterprise.hardwarePlatform",
    "enterprise.platformKeys",
    "fileBrowserHandler",
    "fileSystemProvider",
    "fontSettings",
    "gcm",
    "history",
    "identity",
    "idle",
    "input.ime",
    "loginState",
    "menus",
    "nativeMessaging",
    "networking.config",
    "notifications",
    "omnibox",
    "pageCapture",
    "platformKeys",
    "power",
    "printerProvider",
    "privacy",
    "processes",
    "proxy",
    "readingList",
    "sessions",
    "sidePanel",
    "system.cpu",
    "system.display",
    "system.memory",
    "system.storage",
    "tabCapture",
    "tabGroups",
    "tabs",
    "topSites",
    "tts",
    "ttsEngine",
    "vpnProvider",
    "wallpaper",
    "webAuthenticationProxy",
    "windows",
];

// Registered APIs whose Qt delegates are only partially plumbed —
// ExtensionManager::unverifiedPermissions()'s copy.
const UNVERIFIED_PERMISSIONS: &[&str] = &[
    "declarativeNetRequest",
    "declarativeNetRequestWithHostAccess",
    "declarativeNetRequestFeedback",
    "offscreen",
    "scripting",
    "webNavigation",
    "webRequest",
];

// The EXT05 deny-by-default consent set — permissions needing an
// explicit per-permission consent checkbox in the review dialog.
const DANGEROUS_PERMISSIONS: &[&str] = &[
    "nativeMessaging",
    "debugger",
    "webRequestBlocking",
    "cookies",
    "browsingData",
];

// Host_permissions patterns equivalent to "all websites".
const DANGEROUS_HOST_PATTERNS: &[&str] = &["<all_urls>", "*://*/*"];

fn strings(value: &Value) -> Vec<String> {
    // Mirrors the C++ reader: every array member through toString —
    // non-strings become "".
    value
        .as_array()
        .map(|a| {
            a.iter()
                .map(|v| v.as_str().unwrap_or("").to_string())
                .collect()
        })
        .unwrap_or_default()
}

fn string(value: &Value) -> String {
    value.as_str().unwrap_or("").to_string()
}

/// rc_ext_manifest_check body: manifest.json bytes -> the schema +
/// permission verdict JSON documented at the top of this module.
/// RC_CORRUPT means the bytes are not readable JSON at all.
pub fn manifest_check(data: &[u8]) -> RcResult<Vec<u8>> {
    if data.len() > MAX_MANIFEST {
        return corrupt("manifest.json exceeds the size cap");
    }
    let doc: Value = serde_json::from_slice(data).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("manifest.json is not valid JSON: {e}"),
    })?;
    let root = doc.as_object().ok_or_else(|| Fail {
        status: RcStatus::Corrupt,
        msg: "manifest.json is not a JSON object".into(),
    })?;

    let mut errors: Vec<String> = Vec::new();
    let manifest_version = root
        .get("manifest_version")
        .and_then(Value::as_i64)
        .unwrap_or(0);
    if manifest_version != 3 {
        errors.push(format!(
            "Qt WebEngine supports Manifest V3 extensions only \
             (this package declares version {manifest_version})"
        ));
    }

    let mut permissions = strings(root.get("permissions").unwrap_or(&Value::Null));
    permissions.extend(strings(
        root.get("optional_permissions").unwrap_or(&Value::Null),
    ));
    let host_permissions =
        strings(root.get("host_permissions").unwrap_or(&Value::Null));

    let unsupported: Vec<&str> = permissions
        .iter()
        .map(String::as_str)
        .filter(|p| UNSUPPORTED_PERMISSIONS.contains(p))
        .collect();
    let unverified: Vec<&str> = permissions
        .iter()
        .map(String::as_str)
        .filter(|p| UNVERIFIED_PERMISSIONS.contains(p))
        .collect();
    let mut dangerous: Vec<String> = permissions
        .iter()
        .filter(|p| DANGEROUS_PERMISSIONS.contains(&p.as_str()))
        .cloned()
        .collect();
    dangerous.extend(
        host_permissions
            .iter()
            .filter(|p| DANGEROUS_HOST_PATTERNS.contains(&p.as_str()))
            .cloned(),
    );

    Ok(json!({
        "valid": true,
        "manifest_version": manifest_version,
        "name": string(root.get("name").unwrap_or(&Value::Null)),
        "version": string(root.get("version").unwrap_or(&Value::Null)),
        "description": string(root.get("description").unwrap_or(&Value::Null)),
        "update_url": string(root.get("update_url").unwrap_or(&Value::Null)),
        "key": string(root.get("key").unwrap_or(&Value::Null)),
        "has_background": root.contains_key("background"),
        "has_action": root.contains_key("action")
            || root.contains_key("browser_action")
            || root.contains_key("page_action"),
        "content_script_count": root
            .get("content_scripts")
            .and_then(Value::as_array)
            .map_or(0, Vec::len),
        "permissions": permissions,
        "host_permissions": host_permissions,
        "unsupported": unsupported,
        "unverified": unverified,
        "dangerous": dangerous,
        "errors": errors,
    })
    .to_string()
    .into_bytes())
}

// ------------------------------------------------------------------
// tests
// ------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    fn fields(json_bytes: &[u8]) -> Value {
        serde_json::from_slice(json_bytes).unwrap()
    }

    // Minimal zip writer: local headers + central dir + EOCD.
    // `entries`: (name, method 0|8, stored bytes).
    fn build_zip(entries: &[(&[u8], u16, &[u8])]) -> Vec<u8> {
        let mut out = Vec::new();
        let mut cdes = Vec::new();
        for (name, method, data) in entries {
            let uncomp = match method {
                8 => miniz_oxide::inflate::decompress_to_vec(data)
                    .unwrap()
                    .len() as u32,
                _ => data.len() as u32,
            };
            let off = out.len() as u32;
            out.extend_from_slice(&0x0403_4b50u32.to_le_bytes());
            out.extend_from_slice(&20u16.to_le_bytes()); // version needed
            out.extend_from_slice(&0u16.to_le_bytes()); // flags
            out.extend_from_slice(&method.to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes()); // time
            out.extend_from_slice(&0u16.to_le_bytes()); // date
            out.extend_from_slice(&0u32.to_le_bytes()); // crc (unused here)
            out.extend_from_slice(&(data.len() as u32).to_le_bytes());
            out.extend_from_slice(&uncomp.to_le_bytes());
            out.extend_from_slice(&(name.len() as u16).to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes()); // extra len
            out.extend_from_slice(name);
            out.extend_from_slice(data);
            cdes.push((off, *method, data.len() as u32, uncomp, name));
        }
        let cd_offset = out.len() as u32;
        for (off, method, comp, uncomp, name) in &cdes {
            out.extend_from_slice(&0x0201_4b50u32.to_le_bytes());
            out.extend_from_slice(&20u16.to_le_bytes()); // version made by
            out.extend_from_slice(&20u16.to_le_bytes()); // version needed
            out.extend_from_slice(&0u16.to_le_bytes()); // flags
            out.extend_from_slice(&method.to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes());
            out.extend_from_slice(&0u32.to_le_bytes());
            out.extend_from_slice(&comp.to_le_bytes());
            out.extend_from_slice(&uncomp.to_le_bytes());
            out.extend_from_slice(&(name.len() as u16).to_le_bytes());
            out.extend_from_slice(&0u16.to_le_bytes()); // extra
            out.extend_from_slice(&0u16.to_le_bytes()); // comment
            out.extend_from_slice(&0u16.to_le_bytes()); // disk
            out.extend_from_slice(&0u16.to_le_bytes()); // internal attr
            out.extend_from_slice(&0u32.to_le_bytes()); // external attr
            out.extend_from_slice(&off.to_le_bytes());
            out.extend_from_slice(name);
        }
        let cd_size = out.len() as u32 - cd_offset;
        out.extend_from_slice(&0x0605_4b50u32.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out.extend_from_slice(&(cdes.len() as u16).to_le_bytes());
        out.extend_from_slice(&(cdes.len() as u16).to_le_bytes());
        out.extend_from_slice(&cd_size.to_le_bytes());
        out.extend_from_slice(&cd_offset.to_le_bytes());
        out.extend_from_slice(&0u16.to_le_bytes());
        out
    }

    const MANIFEST: &[u8] = br#"{"manifest_version":3,"name":"T","version":"1.0","permissions":["storage"]}"#;

    fn zip_with_manifest() -> Vec<u8> {
        build_zip(&[
            (b"manifest.json", 0, MANIFEST),
            (b"bg.js", 0, b"//x"),
        ])
    }

    // Protobuf helpers for the CRX3 fixtures.
    fn pb_bytes(field: u64, body: &[u8]) -> Vec<u8> {
        let mut v = Vec::new();
        v.push(((field << 3) | 2) as u8);
        let mut len = body.len();
        loop {
            let mut b = (len & 0x7F) as u8;
            len >>= 7;
            if len > 0 {
                b |= 0x80;
            }
            v.push(b);
            if len == 0 {
                break;
            }
        }
        v.extend_from_slice(body);
        v
    }

    fn crx3(header_body: &[u8], payload: &[u8]) -> Vec<u8> {
        let mut v = b"Cr24".to_vec();
        v.extend_from_slice(&3u32.to_le_bytes());
        v.extend_from_slice(&(header_body.len() as u32).to_le_bytes());
        v.extend_from_slice(header_body);
        v.extend_from_slice(payload);
        v
    }

    #[test]
    fn valid_zip_unsigned() {
        let v = fields(&verify_package(&zip_with_manifest(), None, None).unwrap());
        assert_eq!(v["status"], "valid");
        assert_eq!(v["format"], "zip");
        assert_eq!(v["signing"], "unsigned");
        assert_eq!(v["entries"], 2);
        assert_eq!(v["expected_sha256"], "not-declared");
        assert_eq!(v["manifest"]["name"], "T");
        assert_eq!(v["manifest"]["manifest_version"], 3);
    }

    #[test]
    fn valid_zip_deflated_manifest() {
        let deflated = miniz_oxide::deflate::compress_to_vec(MANIFEST, 6);
        let zip = build_zip(&[(b"manifest.json", 8, &deflated)]);
        let v = fields(&verify_package(&zip, None, None).unwrap());
        assert_eq!(v["status"], "valid");
        assert_eq!(v["manifest"]["name"], "T");
    }

    #[test]
    fn hash_match_and_mismatch() {
        let zip = zip_with_manifest();
        let good: [u8; 32] = Sha256::digest(&zip).into();
        let v = fields(&verify_package(&zip, Some(&good), None).unwrap());
        assert_eq!(v["status"], "valid");
        assert_eq!(v["expected_sha256"], "match");
        let mut bad = good;
        bad[0] ^= 0xFF;
        let v = fields(&verify_package(&zip, Some(&bad), None).unwrap());
        assert_eq!(v["status"], "rejected");
        assert_eq!(v["expected_sha256"], "mismatch");
    }

    #[test]
    fn traversal_names_rejected() {
        for name in [
            &b"../../evil"[..],
            b"a/../../evil",
            b"..\\evil",
            b"/abs/path",
            b"C:\\evil",
            b"\\unc\\share",
            b"a/./../../x",
        ] {
            let zip = build_zip(&[(name, 0, b"x")]);
            let v = fields(&verify_package(&zip, None, None).unwrap());
            assert_eq!(
                v["status"], "rejected",
                "name {:?} must reject",
                String::from_utf8_lossy(name)
            );
        }
    }

    #[test]
    fn corrupt_zip_rejected() {
        // garbage
        let v = fields(&verify_package(b"not a package", None, None).unwrap());
        assert_eq!(v["status"], "rejected");
        // truncated archive
        let zip = zip_with_manifest();
        let v = fields(&verify_package(&zip[..zip.len() - 10], None, None).unwrap());
        assert_eq!(v["status"], "rejected");
        // EOCD count lies about entries
        let mut forged = zip_with_manifest();
        let n = forged.len();
        forged[n - 14] = 0xFF; // entries_total -> zip64 sentinel
        let v = fields(&verify_package(&forged, None, None).unwrap());
        assert_eq!(v["status"], "rejected");
    }

    #[test]
    fn crx3_unsigned() {
        // empty header body -> no proofs -> "unsigned", not "bad"
        let pkg = crx3(&[], &zip_with_manifest());
        let v = fields(&verify_package(&pkg, None, None).unwrap());
        assert_eq!(v["status"], "valid");
        assert_eq!(v["format"], "crx3");
        assert_eq!(v["signing"], "unsigned");
        assert_eq!(v["manifest"]["name"], "T");
    }

    #[test]
    fn crx3_signed_structure() {
        // CrxFileHeader: field 2 AsymmetricKeyProof{public_key,sig},
        // field 5 signed_header_data -> SignedData{crx_id}.
        let mut proof = pb_bytes(1, b"fake-pubkey");
        proof.extend_from_slice(&pb_bytes(2, b"fake-sig"));
        let mut header = pb_bytes(2, &proof);
        header.extend_from_slice(&pb_bytes(5, &pb_bytes(1, &[7u8; 16])));
        let pkg = crx3(&header, &zip_with_manifest());
        let v = fields(&verify_package(&pkg, None, None).unwrap());
        assert_eq!(v["status"], "valid");
        assert_eq!(v["signing"], "signed");
        assert_eq!(v["signature_check"], "structure-only");
        assert_eq!(
            v["crx_id"],
            "07070707070707070707070707070707"
        );
    }

    #[test]
    fn crx3_malformed() {
        // proofs without signed_header_data: still parses (unsigned
        // classification is a proofs property — but a proof that
        // fails internal structure rejects)
        let bad_proof = pb_bytes(2, &pb_bytes(1, b"only-key-no-sig"));
        let pkg = crx3(&bad_proof, &zip_with_manifest());
        let v = fields(&verify_package(&pkg, None, None).unwrap());
        assert_eq!(v["status"], "rejected");
        // bad crx version
        let mut pkg = zip_with_manifest();
        pkg.splice(..0, b"Cr24\x04\x00\x00\x00\x00\x00\x00\x00".iter().copied());
        let v = fields(&verify_package(&pkg, None, None).unwrap());
        assert_eq!(v["status"], "rejected");
        // truncated wire
        let pkg = crx3(&[0x12, 0x80], &zip_with_manifest());
        let v = fields(&verify_package(&pkg, None, None).unwrap());
        assert_eq!(v["status"], "rejected");
    }

    #[test]
    fn pinning_reported_honestly() {
        let v = fields(
            &verify_package(&zip_with_manifest(), None, Some(b"pubkey"))
                .unwrap(),
        );
        assert_eq!(v["pinning"], "no-pinning-configured");
        assert_eq!(v["status"], "valid");
    }

    #[test]
    fn manifest_check_classification() {
        let doc = br#"{"manifest_version":3,"name":"D","version":"2",
            "description":"d","permissions":["debugger","cookies","storage","scripting"],
            "optional_permissions":["nativeMessaging"],
            "host_permissions":["<all_urls>","https://a.example/*"],
            "update_url":"https://u.example/x.xml",
            "background":{"service_worker":"w.js"},
            "content_scripts":[{"js":["c.js"]}]}"#;
        let v = fields(&manifest_check(doc).unwrap());
        assert_eq!(v["valid"], true);
        assert_eq!(v["manifest_version"], 3);
        assert_eq!(v["has_background"], true);
        assert_eq!(v["content_script_count"], 1);
        // dangerous: the deny-by-default set, sorted by declaration
        let dangerous: Vec<&str> = v["dangerous"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert!(dangerous.contains(&"debugger"));
        assert!(dangerous.contains(&"cookies"));
        assert!(dangerous.contains(&"nativeMessaging"));
        assert!(dangerous.contains(&"<all_urls>"));
        assert!(!dangerous.contains(&"https://a.example/*"));
        // unsupported contains the Qt-unservable APIs it declares
        let unsupported: Vec<&str> = v["unsupported"]
            .as_array()
            .unwrap()
            .iter()
            .map(|x| x.as_str().unwrap())
            .collect();
        assert!(unsupported.contains(&"debugger"));
        assert!(unsupported.contains(&"cookies"));
        assert!(!unsupported.contains(&"storage"));
        // unverified: scripting is partially plumbed
        assert_eq!(v["unverified"], json!(["scripting"]));
        assert_eq!(v["update_url"], "https://u.example/x.xml");
        assert_eq!(v["errors"], json!([]));
    }

    #[test]
    fn manifest_check_errors() {
        // MV2 -> errors[] carries the reason (Qt disables approve)
        let v = fields(&manifest_check(
            br#"{"manifest_version":2,"name":"X"}"#,
        ).unwrap());
        assert_eq!(v["manifest_version"], 2);
        assert!(!v["errors"].as_array().unwrap().is_empty());
        // malformed JSON / non-object -> RC_CORRUPT
        assert!(manifest_check(b"{").is_err());
        assert!(manifest_check(b"[1,2]").is_err());
        assert!(manifest_check(&vec![b'x'; MAX_MANIFEST + 1]).is_err());
    }
}
