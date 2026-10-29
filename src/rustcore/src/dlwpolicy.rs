//! MD04: dlwpolicy — the download-interception decision layer.
//!
//! `DownloadManager`/`DownloadItem` used to own every "what happens
//! to this response?" decision in C++: the suggested-name sanitizer,
//! the dangerous-extension and executable-mime threat tables, the
//! save-as prompt gate, the external-handler handoff gate, the
//! rustdl engine claim, the finished-row removal verdict and the
//! download_N_* history persistence.  Those decisions are pure
//! policy — they compute over the url, the response metadata and a
//! per-profile rule set — so they live here behind one JSON manifest
//! in / verdict out contract, the same shape navpolicy/permpolicy
//! use:
//!
//!   manifest in -> {"op":"verdict","url","mime","content_type",
//!                   "content_disposition","suggested_name",
//!                   "context":"response"|"download_request",
//!                   "save_as":b}
//!   verdict out -> {"action":"block","reason":"blocked-extension",...}
//!                | {"action":"external","program":..,"args":[..,url]}
//!                | {"action":"display","reason":"displayable-mime"}
//!                | {"action":"download"|"ask","safe_name":..,
//!                   "warn":b,"warn_reason":..}
//!
//! The rules store is two-tier, both under the persona root:
//!
//!   * the LEGACY keys stay where they always were — the bound
//!     profile scope's settings.json `downloadmanager/` group
//!     (alwaysPromptForFileName, downloadDirectory,
//!     removeDownloadsPolicy, external, externalPath, engine) — read
//!     here through the bound RcSettings.  The settings UI and the
//!     C++ writers keep owning those keys through ProfileSettings
//!     (the RC08 canonical store), so a facade caller and the dialog
//!     read the same bytes.  The legacy settings.ini pair imports on
//!     first open inside the RC02 machinery — nothing re-imports or
//!     rewrites it here.
//!   * the NEW keys (always_save_mimes, blocked_extensions) and the
//!     download history records live in profiles/<id>/downloads.json
//!     — a dedicated document this module owns outright.  Keeping
//!     them out of settings.json matters: ProfileSettings caches a
//!     decoded document per scope and a stale persist would clobber
//!     keys it never heard of, so facade-authored keys get their own
//!     file (the RC19 "unshared conf file" caveat).  Record reads
//!     fall back to the legacy download_N_* settings keys only while
//!     downloads.json is absent — the first facade-side save seals
//!     the file as authoritative, so pre-move records stay readable
//!     and are never stranded.
//!
//! Mirroring rules (kept 1:1 with downloadmanager.cpp):
//!   * sanitize_name is DownloadItem::sanitizeFileName verbatim —
//!     both separator kinds, the control-char strip, the trailing-dot
//!     chop, the "."/".." guard and the 200-UTF-16-unit cap with the
//!     ≤16-unit suffix preservation;
//!   * the threat tables are the C++ QSets verbatim — warn-required,
//!     never block-by-default (generic application/octet-stream stays
//!     out of the mime table exactly like the C++ comment says);
//!   * the external gate keeps its scheme floor (http/https/ftp —
//!     file:, data:, blob:, javascript: never reach an argv, SEC09)
//!     and the quoted-token program split is the QRegularExpression
//!     `"([^"]+)"|([^ ]+)` globalMatch semantics;
//!   * the engine claim keeps DLACC06's shape: selected (absent/empty
//!     key defaults to rust), http(s)-only, and in tor mode the
//!     managed SOCKS proxy rule — the caller reports tor/proxy state,
//!     the module never asks;
//!   * row removal keeps updateRow()'s pair: a finished OTR row or a
//!     SuccessFullDownload policy hit removes the row;
//!   * the OTR exclusion stays a WRITE-side rule — the caller simply
//!     does not put off-the-record items into records_save, exactly
//!     like save() skipped them (SEC07);
//!   * the removeDownloadsPolicy "Exit" quirk is preserved caller-
//!     side: with policy Exit the caller never invokes records_save
//!     (the old early-return), so records written before stay put.
//!
//! Everything that needs the live process stays caller-side:
//! QProcess::startDetached for the external handoff, the QMessageBox
//! prompts (dangerous-file warning, save-as dialog), the filesystem
//! collision dedup and the model updates.  This module only answers
//! questions.

use serde_json::{json, Value};
use std::path::{Path, PathBuf};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::settings::RcSettings;
use crate::store;

/// The module's own document — profiles/<id>/downloads.json.  Holds
/// the facade-authored rule lists and the download history records;
/// the legacy `downloadmanager/*` settings keys are only ever READ
/// here (Qt still owns their writes).
const FILE_NAME: &str = "downloads.json";
const FILE_VERSION: u32 = 1;

/// The settings group the legacy keys live under — ProfileSettings
/// keyPath() flattens "downloadmanager" + "/" + leaf.
const GROUP: &str = "downloadmanager";

const MAX_URL_LEN: usize = 8192;
const MAX_NAME_LEN: usize = 4096;
const MAX_MIME_LEN: usize = 512;
const MAX_PROGRAM_LEN: usize = 4096;
const MAX_RULE_LIST: usize = 1024;
const MAX_RECORDS: usize = 10000;
const MAX_FILE_BYTES: u64 = 16 * 1024 * 1024;

// ---- pure helpers ----------------------------------------------------

fn field_str<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(|x| x.as_str()).unwrap_or("")
}

fn field_bool(v: &Value, key: &str) -> bool {
    v.get(key).and_then(|x| x.as_bool()).unwrap_or(false)
}

/// DownloadItem::sanitizeFileName verbatim — the untrusted suggested
/// name reduced to a safe basename, or "" when nothing survives.
/// QString semantics are UTF-16: the length cap counts units, not
/// bytes, so the cap path works in unit space and a mid-pair cut
/// degrades to U+FFFD the same way Qt's dangling surrogate did.
pub fn sanitize_name(suggested: &str) -> String {
    const MAX_NAME_UNITS: usize = 200;
    // Keep only the last path component, checking both separators so
    // a Windows-style "..\name" cannot traverse either.
    let last = suggested
        .rsplit(|c| c == '/' || c == '\\')
        .next()
        .unwrap_or("");
    // Control characters corrupt the label display and the
    // filesystem.
    let cleaned: String = last
        .chars()
        .filter(|&c| c >= '\u{20}' && c != '\u{7f}')
        .collect();
    let mut name = cleaned.trim().to_string();
    // CA12/W5: on Windows the NTFS-forbidden set maps to '_' (':' is
    // the sharp edge — "f.pdf:evil.exe" would land the payload on an
    // alternate data stream) and trailing spaces are stripped so the
    // dedup suffix tracks the on-disk name.  Mirrors the C++ lane's
    // Q_OS_WIN arm.
    if cfg!(windows) {
        name = name
            .chars()
            .map(|c| if "<>:\"|?*".contains(c) { '_' } else { c })
            .collect();
        while name.ends_with(' ') {
            name.pop();
        }
    }
    while name.ends_with('.') {
        name.pop();
    }
    // "." and ".." resolve outside the download directory.
    if name.is_empty() || name == "." || name == ".." {
        return String::new();
    }
    // CA12/W5: device stems are illegal even with an extension —
    // "con.txt" cannot exist; prefix the stem like the C++ lane.
    if cfg!(windows) {
        let stem = name.split('.').next().unwrap_or("").to_lowercase();
        if crate::profiles::is_reserved_device_stem(&stem) {
            name = format!("_{name}");
        }
    }
    // NAME_MAX headroom for the "-NN" dedup suffix, keeping the
    // extension readable.
    let units: Vec<u16> = name.encode_utf16().collect();
    if units.len() > MAX_NAME_UNITS {
        let suffix = match name.rfind('.') {
            // dot > 0 (a leading dot names no suffix) and the tail
            // fits in 16 units — Qt's name.size() - dot <= 16.
            Some(b) if b > 0
                && units.len() - name[..b].encode_utf16().count() <= 16 =>
            {
                name[b..].to_string()
            }
            _ => String::new(),
        };
        let keep = MAX_NAME_UNITS - suffix.encode_utf16().count();
        let prefix = String::from_utf16_lossy(&units[..keep]);
        name = format!("{prefix}{suffix}");
    }
    name
}

/// QFileInfo::suffix().toLower() — the tail after the last dot; a
/// leading-dot name and a trailing dot both yield "".
fn suffix_lc(name: &str) -> String {
    match name.rfind('.') {
        Some(b) if b > 0 && b + 1 < name.len() => {
            name[b + 1..].to_lowercase()
        }
        _ => String::new(),
    }
}

/// mimeType.section(';',0,0).trimmed().toLower() — the bare type.
fn normalize_mime(mime: &str) -> String {
    mime.split(';')
        .next()
        .unwrap_or("")
        .trim()
        .to_lowercase()
}

/// Suffixes that run commands, install software, or auto-execute when
/// the file is opened — covering Linux, Windows and macOS.  The C++
/// QSet verbatim (sorted for binary search).
const DANGEROUS_EXT: [&str; 33] = [
    "apk", "appimage", "bash", "bat", "cmd", "com", "csh", "deb",
    "desktop", "dll", "dmg", "exe", "hta", "jar", "js", "jse", "ksh",
    "lnk", "msc", "msi", "pif", "pkg", "ps1", "reg", "rpm", "run",
    "scr", "sh", "vb", "vbe", "vbs", "wsf", "wsh",
];

/// Content types that describe a program or installer even when the
/// file name pretends to be something innocuous.  Generic
/// application/octet-stream stays out on purpose — it labels ordinary
/// downloads and would warn on everything.
const EXEC_MIME: [&str; 16] = [
    "application/java-archive",
    "application/vnd.android.package-archive",
    "application/vnd.debian.binary-package",
    "application/vnd.microsoft.portable-executable",
    "application/x-apple-diskimage",
    "application/x-deb",
    "application/x-desktop",
    "application/x-executable",
    "application/x-ms-dos-executable",
    "application/x-msdownload",
    "application/x-msdos-program",
    "application/x-ms-installer",
    "application/x-ms-shortcut",
    "application/x-msi",
    "application/x-rpm",
    "application/x-shellscript",
];

/// The threat verdict behind confirmSafeToSave — the dialog itself
/// stays Qt-side.  warn_reason mirrors the C++ branch choice:
/// the extension hit drives the "can harm your computer" text, a
/// bare mime hit the "disguised software" text.
fn threat(file_name: &str, mime: &str) -> (bool, bool, bool, &'static str) {
    let dangerous_name = DANGEROUS_EXT.contains(&suffix_lc(file_name).as_str());
    let executable_mime = EXEC_MIME.contains(&normalize_mime(mime).as_str());
    let reason = if dangerous_name {
        "dangerous-extension"
    } else if executable_mime {
        "executable-mime"
    } else {
        ""
    };
    (
        dangerous_name,
        executable_mime,
        dangerous_name || executable_mime,
        reason,
    )
}

/// The external-handler program split — the C++ QRegularExpression
/// `"([^"]+)"|([^ ]+)` globalMatch: a quoted span captures its
/// interior, anything else captures a non-space run (quotes inside
/// it included); unmatched positions advance one char, which only
/// spaces ever are.
fn split_program(program: &str) -> Vec<String> {
    let b: Vec<char> = program.chars().collect();
    let mut out = Vec::new();
    let mut i = 0;
    while i < b.len() {
        if b[i] == '"' {
            // `"([^"]+)"` — needs a NON-EMPTY interior; `""` falls to
            // the non-space run like the regex does.
            if let Some(off) = b[i + 1..].iter().position(|&c| c == '"') {
                if off > 0 {
                    out.push(b[i + 1..i + 1 + off].iter().collect());
                    i += off + 2;
                    continue;
                }
            }
        }
        if b[i] != ' ' {
            let start = i;
            while i < b.len() && b[i] != ' ' {
                i += 1;
            }
            out.push(b[start..i].iter().collect());
        } else {
            i += 1;
        }
    }
    out
}

/// QUrl::path() approximated on encoded-url text: hierarchical urls
/// give the post-authority part up to ?/#, opaque schemes the whole
/// post-colon part.  sanitize_name's last-component cut lands the
/// same basename the C++ fed it.
fn url_path(url: &str) -> &str {
    let rest = match url.find("://") {
        Some(i) => {
            let after = &url[i + 3..];
            match after.find('/') {
                Some(j) => &after[j..],
                None => "",
            }
        }
        None => match url.find(':') {
            Some(i) => &url[i + 1..],
            None => "",
        },
    };
    let end = rest
        .find(|c| c == '?' || c == '#')
        .unwrap_or(rest.len());
    &rest[..end]
}

/// Mimes the engines render inline — the "display" half of the
/// download-or-display question.  Exact entries plus the four
/// navigable prefix classes; anything unlisted answers "download".
fn displayable_mime(mime: &str) -> bool {
    const EXACT: [&str; 5] = [
        "application/json",
        "application/pdf",
        "application/xhtml+xml",
        "application/xml",
        "image/svg+xml",
    ];
    EXACT.contains(&mime)
        || mime.starts_with("text/")
        || mime.starts_with("image/")
        || mime.starts_with("audio/")
        || mime.starts_with("video/")
}

// ---- the downloads.json document --------------------------------------

/// The facade-authored half: rules Qt never wrote plus the record
/// list.  `present` marks the file authoritative over the legacy
/// settings keys — created only on the first facade-side write.
#[derive(Default)]
struct DlFile {
    present: bool,
    always_save_mimes: Vec<String>,
    blocked_extensions: Vec<String>,
    records: Vec<Record>,
}

#[derive(Clone, Default)]
struct Record {
    url: String,
    location: String,
    done: bool,
    started: String,
    finished: String,
    size: i64,
}

fn record_json(r: &Record) -> Value {
    json!({
        "url": r.url,
        "location": r.location,
        "done": r.done,
        "started": r.started,
        "finished": r.finished,
        "size": r.size,
    })
}

fn str_list(v: Option<&Value>, cap: usize) -> Vec<String> {
    v.and_then(|x| x.as_array())
        .map(|a| {
            a.iter()
                .filter_map(|e| e.as_str().map(String::from))
                .take(cap)
                .collect()
        })
        .unwrap_or_default()
}

fn file_path(dir: &Path) -> PathBuf {
    dir.join(FILE_NAME)
}

/// Row-tolerant like the other stores: a malformed field drops only
/// itself, an unusable document is "absent" — the legacy settings
/// keys then remain the read source.
fn load_file(dir: &Path) -> RcResult<DlFile> {
    let path = file_path(dir);
    if !path.exists() {
        return Ok(DlFile::default());
    }
    let meta = std::fs::metadata(&path)?;
    if meta.len() > MAX_FILE_BYTES {
        return fail(RcStatus::Corrupt, "downloads.json exceeds the size bound");
    }
    let bytes = std::fs::read(&path)?;
    let doc: Value = serde_json::from_slice(&bytes).unwrap_or(Value::Null);
    let mut f = DlFile {
        present: true,
        ..DlFile::default()
    };
    if doc
        .get("version")
        .and_then(|v| v.as_u64())
        .map(|v| v != FILE_VERSION as u64)
        .unwrap_or(true)
    {
        // Not ours — present (so a stray file still seals the legacy
        // fallback off) but contributes nothing.
        return Ok(f);
    }
    if let Some(rules) = doc.get("rules") {
        f.always_save_mimes = str_list(rules.get("always_save_mimes"), MAX_RULE_LIST);
        f.blocked_extensions = str_list(rules.get("blocked_extensions"), MAX_RULE_LIST);
    }
    if let Some(rows) = doc.get("records").and_then(|r| r.as_array()) {
        for r in rows.iter().take(MAX_RECORDS) {
            f.records.push(Record {
                url: field_str(r, "url").to_string(),
                location: field_str(r, "location").to_string(),
                done: r.get("done").and_then(|x| x.as_bool()).unwrap_or(true),
                started: field_str(r, "started").to_string(),
                finished: field_str(r, "finished").to_string(),
                size: r.get("size").and_then(|x| x.as_i64()).unwrap_or(-1),
            });
        }
    }
    Ok(f)
}

fn persist_file(dir: &Path, f: &DlFile) -> RcResult<()> {
    std::fs::create_dir_all(dir)?;
    let doc = json!({
        "version": FILE_VERSION,
        "rules": {
            "always_save_mimes": f.always_save_mimes,
            "blocked_extensions": f.blocked_extensions,
        },
        "records": f.records.iter().map(record_json).collect::<Vec<_>>(),
    });
    let bytes = serde_json::to_vec(&doc).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode downloads.json: {e}"),
    })?;
    store::atomic_write(&file_path(dir), &bytes)
}

// ---- the settings-key half ---------------------------------------------

fn skey(leaf: &str) -> String {
    format!("{GROUP}/{leaf}")
}

fn setting_string(h: &RcSettings, leaf: &str) -> Option<String> {
    h.get_string(&skey(leaf)).ok()
}

fn setting_bool(h: &RcSettings, leaf: &str) -> Option<bool> {
    h.get_bool(&skey(leaf)).ok()
}

/// The rule set the verdict composes over — legacy keys from the
/// bound settings scope, facade-authored lists from downloads.json.
/// Manifest fields named identically override the store read so a
/// caller can consult statelessly.
#[derive(Default)]
struct Rules {
    always_prompt: bool,
    download_directory: String,
    remove_policy: String,
    external_enabled: bool,
    external_path: String,
    engine: Option<String>,
    always_save_mimes: Vec<String>,
    blocked_extensions: Vec<String>,
}

fn load_rules(s: &RcSettings, m: &Value) -> RcResult<Rules> {
    let mut r = Rules::default();
    r.always_prompt = setting_bool(s, "alwaysPromptForFileName").unwrap_or(false);
    r.download_directory = setting_string(s, "downloadDirectory").unwrap_or_default();
    r.remove_policy =
        setting_string(s, "removeDownloadsPolicy").unwrap_or_else(|| "Never".into());
    r.external_enabled = setting_bool(s, "external").unwrap_or(false);
    r.external_path = setting_string(s, "externalPath").unwrap_or_default();
    r.engine = setting_string(s, "engine");
    let f = load_file(&s.scope_dir())?;
    r.always_save_mimes = f.always_save_mimes;
    r.blocked_extensions = f.blocked_extensions;
    if let Some(v) = m.get("always_prompt").and_then(|x| x.as_bool()) {
        r.always_prompt = v;
    }
    if let Some(v) = m.get("remove_policy").and_then(|x| x.as_str()) {
        r.remove_policy = v.to_string();
    }
    if let Some(v) = m.get("external_enabled").and_then(|x| x.as_bool()) {
        r.external_enabled = v;
    }
    if let Some(v) = m.get("external_path").and_then(|x| x.as_str()) {
        r.external_path = v.to_string();
    }
    if let Some(v) = m.get("engine").and_then(|x| x.as_str()) {
        r.engine = Some(v.to_string());
    }
    if let Some(v) = m.get("always_save_mimes") {
        r.always_save_mimes = str_list(Some(v), MAX_RULE_LIST);
    }
    if let Some(v) = m.get("blocked_extensions") {
        r.blocked_extensions = str_list(Some(v), MAX_RULE_LIST);
    }
    Ok(r)
}

// ---- the verdict pipeline ----------------------------------------------

/// The external-handoff gate — enabled + a non-empty program + the
/// http/https/ftp scheme floor + a non-empty argv split.  Returns
/// the argv (program tokens + the url appended) on pass.
fn external_gate(scheme: &str, url: &str, r: &Rules) -> Option<Vec<String>> {
    if !r.external_enabled || r.external_path.trim().is_empty() {
        return None;
    }
    if !matches!(scheme, "http" | "https" | "ftp") {
        return None;
    }
    let mut args = split_program(r.external_path.trim());
    if args.is_empty() {
        return None;
    }
    args.push(url.to_string());
    Some(args)
}

/// RustDownloadEngine::isSelected — absent or empty key means the
/// rust engine (DLACC06 default); "rust"/"accelerated" or the legacy
/// int 1 also select, anything else opts out.
fn engine_selected(engine: Option<&str>) -> bool {
    let s = engine.unwrap_or("").trim().to_lowercase();
    if s.is_empty() {
        return true;
    }
    s.contains("rust")
        || s.contains("accelerated")
        || s.parse::<i64>().map(|v| v == 1).unwrap_or(false)
}

/// The full verdict — see the module doc for the grammar.  `context`
/// "download_request" marks the consult where the engine already
/// classified the request as a download (handleDownloadRequested);
/// there "display" can never be the answer.  Default ("response") is
/// the headers-time question a request interceptor asks.
fn decide(m: &Value, r: &Rules) -> Value {
    let url = field_str(m, "url");
    let scheme = url
        .split(':')
        .next()
        .unwrap_or("")
        .to_lowercase();
    let suggested = {
        let s = field_str(m, "suggested_name");
        if s.is_empty() {
            url_path(url).to_string()
        } else {
            s.to_string()
        }
    };
    let mut safe = sanitize_name(&suggested);
    if safe.is_empty() {
        safe = "unnamed_download".into();
    }
    let ext = suffix_lc(&safe);
    if !ext.is_empty()
        && r.blocked_extensions.iter().any(|e| e.eq_ignore_ascii_case(&ext))
    {
        return json!({
            "action": "block",
            "reason": "blocked-extension",
            "safe_name": safe,
            "ext": ext,
        });
    }
    if let Some(args) = external_gate(&scheme, url, r) {
        return json!({
            "action": "external",
            "reason": "external-handler",
            "program": r.external_path.trim(),
            "args": args,
            "safe_name": safe,
            "ext": ext,
        });
    }
    let mime = {
        let ct = field_str(m, "content_type");
        normalize_mime(if ct.is_empty() { field_str(m, "mime") } else { ct })
    };
    let (_, _, warn, warn_reason) = threat(&safe, &mime);
    let save_as = field_bool(m, "save_as");
    let attachment = field_str(m, "content_disposition")
        .to_lowercase()
        .contains("attachment");
    let always_save = r
        .always_save_mimes
        .iter()
        .any(|x| x == &mime);
    if field_str(m, "context") != "download_request"
        && !save_as
        && !attachment
        && !always_save
        && displayable_mime(&mime)
    {
        return json!({
            "action": "display",
            "reason": "displayable-mime",
            "mime": mime,
            "safe_name": safe,
            "warn": warn,
            "warn_reason": warn_reason,
        });
    }
    let action = if r.always_prompt || save_as {
        "ask"
    } else {
        "download"
    };
    json!({
        "action": action,
        "reason": if always_save { "always-save-mime" } else { "download" },
        "safe_name": safe,
        "ext": ext,
        "mime": mime,
        "warn": warn,
        "warn_reason": warn_reason,
    })
}

// ---- records over the legacy settings keys ------------------------------

/// The save()/load() download_N_* row schema read back from the
/// bound settings scope — the pre-downloads.json persistence lane.
fn legacy_records(h: &RcSettings) -> Vec<Record> {
    let mut out = Vec::new();
    for i in 0..MAX_RECORDS {
        let base = format!("download_{i}_");
        let url = match setting_string(h, &format!("{base}url")) {
            Some(u) if !u.is_empty() => u,
            _ => break,
        };
        let location = setting_string(h, &format!("{base}location")).unwrap_or_default();
        if location.is_empty() {
            // save() wrote url+location together; an absent location
            // mirrors the C++ skip-a-partial-row without ending the
            // scan (the keys iterate contiguously).
            out.push(Record {
                url,
                done: true,
                ..Record::default()
            });
            continue;
        }
        out.push(Record {
            url,
            location,
            done: setting_bool(h, &format!("{base}done")).unwrap_or(true),
            started: setting_string(h, &format!("{base}started")).unwrap_or_default(),
            finished: setting_string(h, &format!("{base}finished")).unwrap_or_default(),
            size: h.get_int(&skey(&format!("{base}size"))).unwrap_or(-1),
        });
    }
    out
}

/// records_clear's legacy half — drop every download_N_* settings
/// row so a cleared history stays cleared under either reader.
fn legacy_records_clear(h: &RcSettings) -> RcResult<()> {
    for i in 0..MAX_RECORDS {
        let base = format!("download_{i}_");
        if h.get(&skey(&format!("{base}url"))).is_err() {
            break;
        }
        for leaf in ["url", "location", "done", "started", "finished", "size"] {
            let _ = h.remove(&skey(&format!("{base}{leaf}")));
        }
    }
    Ok(())
}

// ---- the op dispatcher --------------------------------------------------

fn need_store<'a>(h: Option<&'a RcSettings>) -> RcResult<&'a RcSettings> {
    h.ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "dlwpolicy op needs a bound profile settings store".into(),
    })
}

/// Facade name -> settings leaf for the read-only legacy keys; the
/// two writable rules live in downloads.json instead.
fn legacy_leaf(key: &str) -> Option<&'static str> {
    Some(match key {
        "always_prompt" => "alwaysPromptForFileName",
        "download_directory" => "downloadDirectory",
        "remove_policy" => "removeDownloadsPolicy",
        "external" => "external",
        "external_path" => "externalPath",
        "engine" => "engine",
        _ => return None,
    })
}

/// Generic granular endpoint — {"op":..., params} -> JSON result.
/// `h` is the bound profile's settings store (NULL legal for the
/// pure ops: sanitize_name, threat, external_argv) — the profile
/// dir downloads.json lives under comes off the handle's scope.
/// Mutating results carry "mutated":true so the FFI layer emits the
/// change topic.
pub fn call_json(h: Option<&RcSettings>, v: &Value) -> RcResult<Value> {
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("");
    match op {
        "sanitize_name" => {
            let suggested = field_str(v, "suggested");
            if suggested.len() > MAX_NAME_LEN {
                return fail(RcStatus::InvalidArgument, "bad suggested name");
            }
            Ok(json!({"name": sanitize_name(suggested)}))
        }
        "threat" => {
            if field_str(v, "mime").len() > MAX_MIME_LEN {
                return fail(RcStatus::InvalidArgument, "bad mime");
            }
            let (dn, em, warn, reason) = threat(
                field_str(v, "file_name"),
                field_str(v, "mime"),
            );
            Ok(json!({
                "dangerous_name": dn,
                "executable_mime": em,
                "warn": warn,
                "warn_reason": reason,
            }))
        }
        "external_argv" => {
            let program = field_str(v, "program");
            if program.len() > MAX_PROGRAM_LEN {
                return fail(RcStatus::InvalidArgument, "bad program string");
            }
            Ok(json!({"args": split_program(program)}))
        }
        "verdict" => {
            let s = need_store(h)?;
            if field_str(v, "url").len() > MAX_URL_LEN
                || field_str(v, "mime").len() > MAX_MIME_LEN
                || field_str(v, "content_type").len() > MAX_MIME_LEN
            {
                return fail(RcStatus::InvalidArgument, "bad url or mime");
            }
            let r = load_rules(s, v)?;
            Ok(decide(v, &r))
        }
        "external_gate" => {
            let s = need_store(h)?;
            let scheme = field_str(v, "scheme").to_lowercase();
            let url = field_str(v, "url");
            let r = load_rules(s, v)?;
            match external_gate(&scheme, url, &r) {
                Some(args) => Ok(json!({
                    "handoff": true,
                    "program": r.external_path.trim(),
                    "args": args,
                })),
                None => Ok(json!({"handoff": false})),
            }
        }
        "engine_claim" => {
            let scheme = field_str(v, "scheme").to_lowercase();
            if !field_bool(v, "engine_available") {
                return Ok(json!({"claim": false, "reason": "engine-absent"}));
            }
            let s = need_store(h)?;
            let r = load_rules(s, v)?;
            if !engine_selected(r.engine.as_deref()) {
                return Ok(json!({"claim": false, "reason": "engine-not-selected"}));
            }
            if !matches!(scheme.as_str(), "http" | "https") {
                return Ok(json!({"claim": false, "reason": "scheme"}));
            }
            if field_bool(v, "tor_mode") && !field_bool(v, "proxy_socks5") {
                return Ok(json!({"claim": false, "reason": "tor-no-socks"}));
            }
            Ok(json!({"claim": true}))
        }
        "row_remove" => {
            let s = need_store(h)?;
            let r = load_rules(s, v)?;
            let remove = (!field_bool(v, "downloading") && field_bool(v, "otr"))
                || (field_bool(v, "downloaded_ok") && r.remove_policy == "SuccessFullDownload");
            Ok(json!({"remove": remove}))
        }
        "rules" => {
            let s = need_store(h)?;
            let r = load_rules(s, v)?;
            Ok(json!({
                "always_prompt": r.always_prompt,
                "download_directory": r.download_directory,
                "remove_policy": r.remove_policy,
                "engine": r.engine,
                "external": {
                    "enabled": r.external_enabled,
                    "program": r.external_path,
                },
                "always_save_mimes": r.always_save_mimes,
                "blocked_extensions": r.blocked_extensions,
            }))
        }
        "rule_get" => {
            let key = field_str(v, "key");
            match key {
                "always_save_mimes" | "blocked_extensions" => {
                    let s = need_store(h)?;
                    let f = load_file(&s.scope_dir())?;
                    let list = if key == "always_save_mimes" {
                        f.always_save_mimes
                    } else {
                        f.blocked_extensions
                    };
                    Ok(json!({"found": !list.is_empty(), "value": list}))
                }
                _ => match legacy_leaf(key) {
                    Some(leaf) => {
                        let s = need_store(h)?;
                        match s.get(&skey(leaf)) {
                            Ok(v) => Ok(json!({
                                "found": true,
                                "value": crate::settings::value_to_json(&v)["v"].clone(),
                            })),
                            Err(e) if e.status == RcStatus::NotFound => {
                                Ok(json!({"found": false}))
                            }
                            Err(e) => Err(e),
                        }
                    }
                    None => fail(RcStatus::InvalidArgument, "unknown dlwpolicy rule"),
                },
            }
        }
        "rule_set" => {
            let key = field_str(v, "key");
            let s = need_store(h)?;
            if !matches!(key, "always_save_mimes" | "blocked_extensions") {
                // The legacy keys' Qt writers own the settings lane —
                // a facade write here would go stale under the cached
                // ProfileSettings document (see the module doc).
                return fail(RcStatus::InvalidArgument, "dlwpolicy rule is read-only");
            }
            let value = v.get("value").ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "rule_set needs a value".into(),
            })?;
            let list = str_list(Some(value), MAX_RULE_LIST);
            let mut f = load_file(&s.scope_dir())?;
            if key == "always_save_mimes" {
                f.always_save_mimes = list;
            } else {
                f.blocked_extensions = list;
            }
            persist_file(&s.scope_dir(), &f)?;
            Ok(json!({"set": true, "mutated": true}))
        }
        "records_list" => {
            let s = need_store(h)?;
            let f = load_file(&s.scope_dir())?;
            let records = if f.present {
                f.records
            } else {
                legacy_records(s)
            };
            Ok(json!({
                "records": records.iter().map(record_json).collect::<Vec<_>>(),
            }))
        }
        "records_save" => {
            let s = need_store(h)?;
            let rows = v
                .get("records")
                .and_then(|r| r.as_array())
                .ok_or_else(|| Fail {
                    status: RcStatus::InvalidArgument,
                    msg: "records_save needs a records array".into(),
                })?;
            let mut f = load_file(&s.scope_dir())?;
            f.records.clear();
            for r in rows.iter().take(MAX_RECORDS) {
                let url = field_str(r, "url");
                let location = field_str(r, "location");
                if url.is_empty() || location.is_empty()
                    || url.len() > MAX_URL_LEN
                    || location.len() > MAX_NAME_LEN
                {
                    continue;
                }
                f.records.push(Record {
                    url: url.to_string(),
                    location: location.to_string(),
                    done: r.get("done").and_then(|x| x.as_bool()).unwrap_or(true),
                    started: field_str(r, "started").to_string(),
                    finished: field_str(r, "finished").to_string(),
                    size: r.get("size").and_then(|x| x.as_i64()).unwrap_or(-1),
                });
            }
            persist_file(&s.scope_dir(), &f)?;
            Ok(json!({"saved": f.records.len(), "mutated": true}))
        }
        "records_clear" => {
            let s = need_store(h)?;
            let mut f = load_file(&s.scope_dir())?;
            f.records.clear();
            persist_file(&s.scope_dir(), &f)?;
            legacy_records_clear(s)?;
            Ok(json!({"cleared": true, "mutated": true}))
        }
        _ => fail(RcStatus::InvalidArgument, "unknown dlwpolicy op"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn tmpdir(tag: &str) -> PathBuf {
        let dir = std::env::temp_dir().join(format!(
            "arktos-dlw-test-{}-{}-{}",
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

    #[test]
    fn sanitize_mirrors_qt() {
        // Both separators collapse to the last component.
        assert_eq!(sanitize_name("a/b/c.zip"), "c.zip");
        assert_eq!(sanitize_name("..\\evil\\c.exe"), "c.exe");
        // Control chars strip; the dot names refuse outright.
        assert_eq!(sanitize_name("a\u{7}b"), "ab");
        assert_eq!(sanitize_name("."), "");
        assert_eq!(sanitize_name(".."), "");
        assert_eq!(sanitize_name("   "), "");
        // Trailing dots chop before the guard.
        assert_eq!(sanitize_name("name..."), "name");
        // Overlong: a >16-unit tail drops the suffix.
        let long = format!("{}.toolongextension", "x".repeat(300));
        let got = sanitize_name(&long);
        assert_eq!(got.encode_utf16().count(), 200);
        assert!(!got.ends_with(".toolongextension"));
        // A short suffix is preserved on the left-cut.
        let long = format!("{}.zip", "y".repeat(300));
        let got = sanitize_name(&long);
        assert!(got.ends_with(".zip"));
        assert_eq!(got.encode_utf16().count(), 200);
    }

    #[test]
    fn threat_tables() {
        let (dn, em, warn, reason) = threat("setup.exe", "text/plain");
        assert!(dn && !em && warn && reason == "dangerous-extension");
        let (dn, em, warn, reason) = threat(
            "innocent.txt",
            "application/x-executable; charset=bin",
        );
        assert!(!dn && em && warn && reason == "executable-mime");
        // octet-stream deliberately not executable.
        let (_, _, warn, _) = threat("file.bin", "application/octet-stream");
        assert!(!warn);
        // leading-dot files carry no suffix.
        let (dn, ..) = threat(".exe", "text/plain");
        assert!(!dn);
    }

    #[test]
    fn program_split_matches_regex() {
        assert_eq!(
            split_program("prog -f \"arg with space\""),
            vec!["prog", "-f", "arg with space"]
        );
        // Unclosed quote: the run swallows it like [^ ]+ does.
        assert_eq!(split_program("prog \"unclosed"), vec!["prog", "\"unclosed"]);
        // Adjacent quotes are a non-space run, not an empty interior.
        assert_eq!(split_program("a \"\"b"), vec!["a", "\"\"b"]);
        assert!(split_program("   ").is_empty());
    }

    #[test]
    fn engine_claim_logic() {
        assert!(engine_selected(None));
        assert!(engine_selected(Some("")));
        assert!(engine_selected(Some("Rust")));
        assert!(engine_selected(Some("accelerated")));
        assert!(engine_selected(Some("1")));
        assert!(!engine_selected(Some("builtin")));
        let root = tmpdir("ec");
        let s = RcSettings::open(root.to_str().unwrap(), Some("prof"))
            .unwrap();
        let v = call_json(
            Some(&s),
            &json!({
                "op": "engine_claim",
                "scheme": "https",
                "engine_available": true,
                "engine": "builtin",
            }),
        )
        .unwrap();
        assert_eq!(v["claim"], false);
        let v = call_json(
            Some(&s),
            &json!({
                "op": "engine_claim",
                "scheme": "https",
                "engine_available": true,
                "engine": "",
                "tor_mode": true,
                "proxy_socks5": false,
            }),
        )
        .unwrap();
        assert_eq!(v["claim"], false);
        let v = call_json(
            Some(&s),
            &json!({
                "op": "engine_claim",
                "scheme": "https",
                "engine_available": true,
                "engine": "",
                "tor_mode": true,
                "proxy_socks5": true,
            }),
        )
        .unwrap();
        assert_eq!(v["claim"], true);
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn verdict_block_and_display() {
        let mut r = Rules::default();
        r.blocked_extensions = vec!["exe".into()];
        let v = decide(
            &json!({"url": "https://h/f.exe", "mime": "application/x-executable"}),
            &r,
        );
        assert_eq!(v["action"], "block");
        // Displayable mime + no attachment disposition -> display.
        let v = decide(
            &json!({"url": "https://h/p", "mime": "text/html"}),
            &r,
        );
        assert_eq!(v["action"], "display");
        // ...but not once the engine already classified it a download.
        let v = decide(
            &json!({"url": "https://h/p", "mime": "text/html",
                    "context": "download_request"}),
            &r,
        );
        assert_eq!(v["action"], "download");
        // always-save overrides the display verdict.
        let mut r2 = Rules::default();
        r2.always_save_mimes = vec!["text/html".into()];
        let v = decide(
            &json!({"url": "https://h/p", "mime": "text/html"}),
            &r2,
        );
        assert_eq!(v["action"], "download");
        assert_eq!(v["reason"], "always-save-mime");
        // always_prompt -> ask.
        let mut r3 = Rules::default();
        r3.always_prompt = true;
        let v = decide(&json!({"url": "https://h/f.zip"}), &r3);
        assert_eq!(v["action"], "ask");
    }

    #[test]
    fn verdict_external_gate() {
        let mut r = Rules::default();
        r.external_enabled = true;
        r.external_path = "/usr/bin/aria2c --dir \"/tmp/dl\"".into();
        let v = decide(&json!({"url": "https://h/f.iso"}), &r);
        assert_eq!(v["action"], "external");
        assert_eq!(
            v["args"],
            json!(["/usr/bin/aria2c", "--dir", "/tmp/dl", "https://h/f.iso"])
        );
        // The scheme floor refuses file:.
        let v = decide(&json!({"url": "file:///etc/passwd"}), &r);
        assert_eq!(v["action"], "download");
    }

    #[test]
    fn records_roundtrip_and_legacy_fallback() {
        let root = tmpdir("rr");
        let s = RcSettings::open(root.to_str().unwrap(), Some("prof"))
            .unwrap();
        // Legacy settings keys serve reads while downloads.json is
        // absent.
        s.set("downloadmanager/download_0_url", crate::settings::Value::Str("https://h/a".into()))
            .unwrap();
        s.set("downloadmanager/download_0_location", crate::settings::Value::Str("/tmp/a".into()))
            .unwrap();
        s.set("downloadmanager/download_0_done", crate::settings::Value::Bool(true))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "records_list"}))
            .unwrap();
        assert_eq!(v["records"].as_array().unwrap().len(), 1);
        assert_eq!(v["records"][0]["url"], "https://h/a");
        // The first facade save seals downloads.json — legacy rows
        // stop being the read source.
        call_json(
            Some(&s),
            &json!({
                "op": "records_save",
                "records": [{
                    "url": "https://h/b",
                    "location": "/tmp/b",
                    "done": false,
                    "size": 42,
                }],
            }),
        )
        .unwrap();
        let v = call_json(Some(&s), &json!({"op": "records_list"}))
            .unwrap();
        let rows = v["records"].as_array().unwrap();
        assert_eq!(rows.len(), 1);
        assert_eq!(rows[0]["url"], "https://h/b");
        assert_eq!(rows[0]["size"], 42);
        // Clear drops both lanes.
        call_json(Some(&s), &json!({"op": "records_clear"}))
            .unwrap();
        let v = call_json(Some(&s), &json!({"op": "records_list"}))
            .unwrap();
        assert_eq!(v["records"].as_array().unwrap().len(), 0);
        assert!(s.get("downloadmanager/download_0_url").is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn rule_set_writes_downloads_json() {
        let root = tmpdir("rs");
        let s = RcSettings::open(root.to_str().unwrap(), Some("prof"))
            .unwrap();
        call_json(
            Some(&s),
            &json!({
                "op": "rule_set",
                "key": "blocked_extensions",
                "value": ["scr", "bat"],
            }),
        )
        .unwrap();
        let v = call_json(
            Some(&s),
            &json!({"op": "rule_get", "key": "blocked_extensions"}),
        )
        .unwrap();
        assert_eq!(v["value"], json!(["scr", "bat"]));
        // A legacy key refuses the facade write lane.
        assert!(call_json(
            Some(&s),
            &json!({
                "op": "rule_set",
                "key": "always_prompt",
                "value": true,
            }),
        )
        .is_err());
        let _ = std::fs::remove_dir_all(&root);
    }

    #[test]
    fn row_remove_verdict() {
        let root = tmpdir("rm");
        let s = RcSettings::open(root.to_str().unwrap(), Some("prof"))
            .unwrap();
        let v = call_json(
            Some(&s),
            &json!({"op": "row_remove", "downloading": false, "otr": true}),
        )
        .unwrap();
        assert_eq!(v["remove"], true);
        let v = call_json(
            Some(&s),
            &json!({
                "op": "row_remove", "downloading": false, "otr": false,
                "downloaded_ok": true,
                "remove_policy": "SuccessFullDownload",
            }),
        )
        .unwrap();
        assert_eq!(v["remove"], true);
        let v = call_json(
            Some(&s),
            &json!({
                "op": "row_remove", "downloading": false, "otr": false,
                "downloaded_ok": true, "remove_policy": "Never",
            }),
        )
        .unwrap();
        assert_eq!(v["remove"], false);
        let _ = std::fs::remove_dir_all(&root);
    }
}
