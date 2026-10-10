//! UAG01: user-agent construction + the per-site spoof table.
//!
//! The UA string logic used to live scattered across browserprofile.cpp
//! (the de-badged vanilla-UA build + the client-hints brand version)
//! and useragentmenu.cpp (the useragents.xml preset parse).  It is
//! pure string work — exactly the kind of engine-neutral component the
//! user asked to see in Rust — so it lives here now, with the Qt side
//! kept as the no-rust implementation, the FFI-failure fallback and
//! the parity-test seam (the CPAL01 convention).
//!
//! Three pieces:
//!
//!   * build_ua — the effective-UA decision.  A context JSON carries
//!     the engine's factory UA, the Chrome milestone Arora presents
//!     (UA03) and any configured override; the override wins verbatim,
//!     otherwise the factory string is de-badged (the
//!     "QtWebEngine/<ver>" token Google's bot check fingerprints,
//!     UA01) and its Chrome/<major> is bumped to the presentation
//!     version.
//!   * brand_version — the Sec-CH-UA full version a UA implies: the
//!     UA's own Chrome major over the real engine build tail
//!     ("155.0.7339.225"), the shape real Chrome sends.
//!   * presets — the useragents.xml switcher list parse.  The file is
//!     an installable data file (a distro/admin copy replaces the
//!     bundled resource), so it is third-party-shaped input: the
//!     parse runs in memory-safe Rust and Qt's XML reader never sees
//!     the document (the SEC19 discipline).
//!
//!   * spoof table — the per-site UA override map.  There was never a
//!     Qt-side equivalent (QtWebKit's userAgentForUrl hook has no
//!     WebEngine peer), so this is new state, stored as the "uaspoof"
//!     kind inside the sitedecisions store — the same durable
//!     kind→host→value map every other per-site decision already
//!     uses.  The clearnet interceptor applies a hit as the
//!     User-Agent request header; the tor interceptor never consults
//!     it (a per-site UA on a tor page would re-identify the session
//!     the uniform-UA pin exists to protect).

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::parsers;
use crate::sitedecisions;

const MAX_CONTEXT_BYTES: usize = 16 * 1024;
const MAX_UA_BYTES: usize = 4 * 1024;
const MAX_PRESET_FILE_BYTES: usize = 256 * 1024;
const MAX_PRESET_ENTRIES: usize = 256;

/// The sitedecisions kind the per-site UA spoof table lives under.
/// Values are complete UA strings; keys are lowercase hosts matched
/// by the store's longest-suffix rule.
pub const SPOOF_KIND: &str = "uaspoof";

/// Removes every `\s*QtWebEngine/\S+` occurrence — the Rust
/// transliteration of the QRegularExpression the C++ builder ran.
/// `\s` is read as char::is_whitespace (a superset of what any real
/// UA carries); `\S+` is the non-whitespace run after the token.
fn remove_qtwebengine_token(ua: &str) -> String {
    const TOKEN: &str = "QtWebEngine/";
    let mut out = String::with_capacity(ua.len());
    let mut rest = ua;
    while let Some(pos) = rest.find(TOKEN) {
        let after = &rest[pos + TOKEN.len()..];
        // \S+ needs at least one non-whitespace char right after the
        // slash — "QtWebEngine/" ending the string or followed by
        // whitespace fails the whole regex and the text stays.
        if after.chars().next().map_or(true, |c| c.is_whitespace()) {
            out.push_str(&rest[..pos + TOKEN.len()]);
            rest = after;
            continue;
        }
        // The token start can walk back over the whitespace the
        // regex's leading \s* would have swallowed — emit only up to
        // the last non-whitespace char before the match.
        let head = &rest[..pos];
        let kept = head.trim_end();
        out.push_str(kept);
        let tail = after.trim_start_matches(|c: char| !c.is_whitespace());
        rest = tail;
    }
    out.push_str(rest);
    out
}

/// Replaces every `Chrome/<digits>` with `Chrome/<major>` — the Rust
/// transliteration of the C++ replace() (all occurrences, digit run
/// only; a "Chrome/" not followed by a digit is left alone).
fn bump_chrome_major(ua: &str, major: u64) -> String {
    const TOKEN: &str = "Chrome/";
    let mut out = String::with_capacity(ua.len());
    let mut rest = ua;
    while let Some(pos) = rest.find(TOKEN) {
        let digits_start = pos + TOKEN.len();
        let digits_end = rest[digits_start..]
            .find(|c: char| !c.is_ascii_digit())
            .map(|i| digits_start + i)
            .unwrap_or(rest.len());
        if digits_end == digits_start {
            out.push_str(&rest[..digits_start]);
            rest = &rest[digits_start..];
            continue;
        }
        out.push_str(&rest[..digits_start]);
        out.push_str(&major.to_string());
        rest = &rest[digits_end..];
    }
    out.push_str(rest);
    out
}

/// The effective UA for a context
/// `{"factory_ua","presented_major","override"}`:
/// a non-empty override wins verbatim (a spoof preset or the user's
/// custom string); otherwise the factory UA is de-badged and its
/// Chrome milestone bumped.  Byte-identical to the C++ builder.
pub fn build_ua(context_json: &[u8]) -> RcResult<String> {
    if context_json.len() > MAX_CONTEXT_BYTES {
        return fail(RcStatus::InvalidArgument, "ua context too large");
    }
    let ctx: serde_json::Value =
        serde_json::from_slice(context_json).map_err(|e| Fail {
            status: RcStatus::InvalidArgument,
            msg: format!("ua context: {e}"),
        })?;
    let get_str = |key: &str| -> &str {
        ctx.get(key).and_then(|v| v.as_str()).unwrap_or("")
    };
    let override_ua = get_str("override");
    if !override_ua.is_empty() {
        if override_ua.len() > MAX_UA_BYTES {
            return fail(RcStatus::InvalidArgument, "override ua too large");
        }
        return Ok(override_ua.to_string());
    }
    let factory = get_str("factory_ua");
    if factory.len() > MAX_UA_BYTES {
        return fail(RcStatus::InvalidArgument, "factory ua too large");
    }
    let mut ua = remove_qtwebengine_token(factory);
    let major = ctx
        .get("presented_major")
        .and_then(|v| v.as_u64())
        .unwrap_or(0);
    if major > 0 {
        ua = bump_chrome_major(&ua, major);
    }
    Ok(ua)
}

/// The Sec-CH-UA full version a UA implies — the C++
/// presentedBrandVersion transliterated: the UA's own Chrome major
/// over the engine version's build tail, empty when the UA does not
/// claim Chrome.
pub fn brand_version(http_user_agent: &str, engine_version: &str) -> String {
    if http_user_agent.len() > MAX_UA_BYTES || engine_version.len() > 256 {
        return String::new();
    }
    const TOKEN: &str = "Chrome/";
    let Some(pos) = http_user_agent.find(TOKEN) else {
        return String::new();
    };
    let digits_start = pos + TOKEN.len();
    let digits_end = http_user_agent[digits_start..]
        .find(|c: char| !c.is_ascii_digit())
        .map(|i| digits_start + i)
        .unwrap_or(http_user_agent.len());
    if digits_end == digits_start {
        return String::new();
    }
    let major = &http_user_agent[digits_start..digits_end];
    // Qt: indexOf('.') > 0 keeps the build tail, else a synthesized
    // ".0.0.0" — an engine version starting with '.' or lacking one
    // both land on the synthesized tail.
    let tail = match engine_version.find('.') {
        Some(dot) if dot > 0 => &engine_version[dot..],
        _ => ".0.0.0",
    };
    format!("{major}{tail}")
}

/// Parses a useragentswitcher document into a JSON array preserving
/// document order:
///   [{"type":"separator"},
///    {"type":"agent","description":"...","useragent":"..."}, ...]
///
/// Mirrors the old QXmlStreamReader loop exactly — start elements
/// named "separator" or "useragent" anywhere in the document count,
/// missing attributes read as empty strings, and a malformed tail
/// keeps whatever parsed before it (the Qt code logged the error and
/// used the partial list).
pub fn presets(xml: &[u8]) -> RcResult<String> {
    if xml.len() > MAX_PRESET_FILE_BYTES {
        return fail(
            RcStatus::InvalidArgument,
            "useragents.xml exceeds the size bound",
        );
    }
    let data = parsers::bom_transcode(xml)?;
    let mut scan = parsers::Scan::new(&data);
    let mut entries = Vec::new();
    loop {
        match scan.next() {
            Ok(parsers::Ev::Start { name, attrs }) => {
                let local = parsers::local(&name);
                if local == "separator" {
                    entries.push(serde_json::json!({"type": "separator"}));
                } else if local == "useragent" {
                    entries.push(serde_json::json!({
                        "type": "agent",
                        "description": parsers::attr_or_empty(&attrs, "description"),
                        "useragent": parsers::attr_or_empty(&attrs, "useragent"),
                    }));
                }
            }
            Ok(parsers::Ev::Eof) => break,
            Ok(_) => {}
            // A malformed tail keeps the partial list, matching the
            // Qt reader's log-and-continue behavior — the menu gets
            // the presets that parsed rather than none.
            Err(_) => break,
        }
        if entries.len() >= MAX_PRESET_ENTRIES {
            break;
        }
    }
    serde_json::to_string(&entries).map_err(|e| Fail {
        status: RcStatus::Corrupt,
        msg: format!("encode presets: {e}"),
    })
}

/// A UA value must never carry header syntax — it lands verbatim in
/// an HTTP User-Agent header, so CR/LF/NUL or other control bytes are
/// an injection vector and are refused at the store boundary.
fn check_spoof_ua(ua: &str) -> RcResult<()> {
    if ua.len() > MAX_UA_BYTES || ua.is_empty() {
        return fail(RcStatus::InvalidArgument, "invalid spoof ua");
    }
    if ua.chars().any(|c| c.is_control()) {
        return fail(
            RcStatus::InvalidArgument,
            "spoof ua contains control characters",
        );
    }
    Ok(())
}

/// The stored per-site UA override governing `host` — longest-suffix
/// host match, same rule every sitedecisions consumer uses.
pub fn spoof_for(host: &str) -> RcResult<Option<String>> {
    Ok(sitedecisions::lookup(SPOOF_KIND, host)?.map(|(_, value)| value))
}

/// Record (or overwrite) the per-site override for `host`.  The key
/// is lowercased to match the lookup side; the value is injection-
/// checked above.
pub fn spoof_set(host: &str, ua: &str) -> RcResult<bool> {
    check_spoof_ua(ua)?;
    let host = host.trim().to_ascii_lowercase();
    sitedecisions::set(SPOOF_KIND, &host, ua)
}

/// Drop the override for `host` (idempotent).
pub fn spoof_remove(host: &str) -> RcResult<bool> {
    let host = host.trim().to_ascii_lowercase();
    sitedecisions::remove(SPOOF_KIND, &host)
}

/// Every override row as `{"host":"ua"}` JSON.
pub fn spoof_list() -> RcResult<String> {
    sitedecisions::list_json(SPOOF_KIND)
}

#[cfg(test)]
mod tests {
    use super::*;

    // ------------------------------------------------------------------
    // Independent re-implementations of the Qt semantics the ports must
    // match — written from the regex contracts, not by calling the code
    // under test, so a shared bug can't hide.
    // ------------------------------------------------------------------

    fn qt_remove_token(ua: &str) -> String {
        // QRegularExpression "\s*QtWebEngine/\S+" — remove() drops all
        // matches.  Built on the same whitespace definition for honest
        // parity (the corpus never exercises a difference).
        let mut out = String::new();
        let mut rest = ua;
        while let Some(pos) = rest.find("QtWebEngine/") {
            let after = &rest[pos + "QtWebEngine/".len()..];
            // \S+ must see a non-whitespace char right after the
            // slash or the match fails entirely.
            if after.chars().next().map_or(true, |c| c.is_whitespace()) {
                out.push_str(&rest[..pos + "QtWebEngine/".len()]);
                rest = after;
                continue;
            }
            let head = &rest[..pos];
            // \s* before the token: the maximal whitespace run ending
            // at pos belongs to the match.
            let ws_start = head
                .char_indices()
                .rev()
                .find(|(_, c)| !c.is_whitespace())
                .map(|(i, c)| i + c.len_utf8())
                .unwrap_or(0);
            out.push_str(&head[..ws_start]);
            let after = &rest[pos + "QtWebEngine/".len()..];
            let nws = after
                .find(|c: char| c.is_whitespace())
                .unwrap_or(after.len());
            rest = &after[nws..];
        }
        out.push_str(rest);
        out
    }

    fn qt_bump_chrome(ua: &str, major: u64) -> String {
        // QRegularExpression "Chrome/\d+" replaced by "Chrome/<major>"
        // — every match.
        let mut out = String::new();
        let mut rest = ua;
        loop {
            match rest.find("Chrome/") {
                Some(pos) => {
                    let start = pos + "Chrome/".len();
                    let end = rest[start..]
                        .find(|c: char| !c.is_ascii_digit())
                        .map(|i| start + i)
                        .unwrap_or(rest.len());
                    if end == start {
                        out.push_str(&rest[..start]);
                        rest = &rest[start..];
                    } else {
                        out.push_str(&rest[..start]);
                        out.push_str(&major.to_string());
                        rest = &rest[end..];
                    }
                }
                None => {
                    out.push_str(rest);
                    return out;
                }
            }
        }
    }

    fn ctx(factory: &str, major: u64, override_ua: &str) -> Vec<u8> {
        serde_json::json!({
            "factory_ua": factory,
            "presented_major": major,
            "override": override_ua,
        })
        .to_string()
        .into_bytes()
    }

    const FACTORY: &str = "Mozilla/5.0 (X11; Linux x86_64) \
        AppleWebKit/537.36 (KHTML, like Gecko) QtWebEngine/6.12.0 \
        Chrome/140.0.7339.225 Safari/537.36";

    #[test]
    fn build_debadges_and_bumps() {
        let ua = build_ua(&ctx(FACTORY, 155, "")).unwrap();
        assert!(!ua.contains("QtWebEngine"));
        assert!(ua.contains("Chrome/155.0.7339.225"));
        assert_eq!(
            ua,
            qt_bump_chrome(&qt_remove_token(FACTORY), 155)
        );
    }

    #[test]
    fn build_matches_qt_semantics_on_corpus() {
        let cases = [
            // Qt-badged factory string.
            FACTORY,
            // Already vanilla.
            "Mozilla/5.0 (X11) Chrome/140.0.0.0 Safari/537.36",
            // No Chrome token at all.
            "Mozilla/5.0 (X11) Safari/537.36",
            // Token at the very start/end.
            "QtWebEngine/6.12.0 Chrome/140",
            "Mozilla/5.0 Chrome/140 QtWebEngine/6.12.0",
            // Whitespace variants before the token.
            "A  QtWebEngine/6 B",
            "A\tQtWebEngine/6\tB",
            // Token glued to the previous word (no whitespace —
            // \s* matches empty and the token still goes).
            "AQWebEngine QtWebEngine/6 B",
            // Multiple badges and multiple Chrome tokens.
            "QtWebEngine/1 X QtWebEngine/2 Chrome/1.2 Chrome/3.4 Y",
            // Chrome/ with no digits — no match, left alone.
            "A Chrome/x Chrome/140 B",
            // QtWebEngine/ with no version run — \S+ fails, the whole
            // regex misses and the text survives untouched.
            "A QtWebEngine/ B",
            "A QtWebEngine/",
            "QtWebEngine/",
            // Empty.
            "",
        ];
        for case in cases {
            for &major in &[0u64, 140, 155] {
                let got = build_ua(&ctx(case, major, "")).unwrap();
                let mut want = qt_remove_token(case);
                if major > 0 {
                    want = qt_bump_chrome(&want, major);
                }
                assert_eq!(got, want, "factory={case:?} major={major}");
            }
        }
    }

    #[test]
    fn build_override_wins_verbatim() {
        let ua = build_ua(&ctx(FACTORY, 155, "Custom/1.0")).unwrap();
        assert_eq!(ua, "Custom/1.0");
        // A whitespace-only override is still an override (Qt's
        // isEmpty() check, verbatim hand-off).
        let ua = build_ua(&ctx(FACTORY, 155, " ")).unwrap();
        assert_eq!(ua, " ");
    }

    #[test]
    fn build_rejects_bad_context() {
        assert!(build_ua(b"not json").is_err());
        let big = format!("{{\"override\":\"{}\"}}", "a".repeat(MAX_UA_BYTES + 1));
        assert!(build_ua(big.as_bytes()).is_err());
        let huge = vec![b' '; MAX_CONTEXT_BYTES + 1];
        assert!(build_ua(&huge).is_err());
        // Missing fields degrade to the de-badged empty/default.
        assert_eq!(build_ua(b"{}").unwrap(), "");
        assert_eq!(
            build_ua(br#"{"factory_ua":"A QtWebEngine/6 B"}"#).unwrap(),
            "A B"
        );
    }

    #[test]
    fn brand_version_ports_qt_logic() {
        // UA with Chrome + dotted engine version -> major + tail.
        assert_eq!(
            brand_version(
                "Mozilla/5.0 Chrome/155.0.0.0 Safari/537.36",
                "140.0.7339.225"
            ),
            "155.0.7339.225"
        );
        // Engine version without a dot tail -> synthesized ".0.0.0".
        assert_eq!(brand_version("Chrome/99 X", "140"), "99.0.0.0");
        // Engine version starting with a dot -> same synthesized tail
        // (Qt's dot>0 check).
        assert_eq!(brand_version("Chrome/99", ".5"), "99.0.0.0");
        // No Chrome/ -> empty (non-Chrome UA gets honest defaults).
        assert_eq!(brand_version("Mozilla/5.0 Firefox/143.0", "140.1"), "");
        // Chrome/ with no digits -> empty.
        assert_eq!(brand_version("X Chrome/ Y", "140.1"), "");
        // Only the first Chrome/ counts.
        assert_eq!(
            brand_version("Chrome/140 Chrome/999", "1.2.3.4"),
            "140.2.3.4"
        );
    }

    #[test]
    fn presets_parse_document_order() {
        let xml = br#"<?xml version="1.0"?>
<useragentswitcher>
    <separator/>
    <useragent description="Chrome (Windows)" useragent="Mozilla/5.0 Chrome/141"/>
    <useragent description="A &amp; B" useragent="X"/>
    <separator></separator>
</useragentswitcher>"#;
        let doc: serde_json::Value =
            serde_json::from_str(&presets(xml).unwrap()).unwrap();
        let arr = doc.as_array().unwrap();
        assert_eq!(arr.len(), 4);
        assert_eq!(arr[0]["type"], "separator");
        assert_eq!(arr[1]["type"], "agent");
        assert_eq!(arr[1]["description"], "Chrome (Windows)");
        assert_eq!(arr[1]["useragent"], "Mozilla/5.0 Chrome/141");
        // Entity in the attribute decodes (normalized_value).
        assert_eq!(arr[2]["description"], "A & B");
        assert_eq!(arr[3]["type"], "separator");
    }

    #[test]
    fn presets_missing_attrs_and_junk() {
        // The Qt loop read missing attributes as empty strings and
        // ignored unrelated elements.
        let xml = br#"<useragentswitcher>
            <other a="b"/>
            <useragent/>
        </useragentswitcher>"#;
        let doc: serde_json::Value =
            serde_json::from_str(&presets(xml).unwrap()).unwrap();
        let arr = doc.as_array().unwrap();
        assert_eq!(arr.len(), 1);
        assert_eq!(arr[0]["type"], "agent");
        assert_eq!(arr[0]["description"], "");
        assert_eq!(arr[0]["useragent"], "");
        // Not XML at all -> empty list, not an error (Qt logged and
        // produced zero actions).
        let doc: serde_json::Value =
            serde_json::from_str(&presets(b"garbage").unwrap()).unwrap();
        assert_eq!(doc.as_array().unwrap().len(), 0);
    }

    #[test]
    fn presets_bounded() {
        let big = vec![b' '; MAX_PRESET_FILE_BYTES + 1];
        assert!(presets(&big).is_err());
        // An unbounded preset flood stops at the entry cap.
        let mut xml = String::from("<r>");
        for _ in 0..MAX_PRESET_ENTRIES + 64 {
            xml.push_str("<useragent/>");
        }
        xml.push_str("</r>");
        let doc: serde_json::Value =
            serde_json::from_str(&presets(xml.as_bytes()).unwrap()).unwrap();
        assert_eq!(doc.as_array().unwrap().len(), MAX_PRESET_ENTRIES);
    }

    #[test]
    fn spoof_table_roundtrip() {
        let _guard = crate::store::test_lock();
        let dir = std::env::temp_dir().join(format!(
            "arora-ua-test-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        crate::store::lock()
            .set_data_dir(dir.to_str().unwrap())
            .unwrap();
        let _ = sitedecisions::reset();

        assert!(spoof_for("example.com").unwrap().is_none());
        assert!(spoof_set("Example.COM", "Spoof/1.0").unwrap());
        assert_eq!(
            spoof_for("example.com").unwrap(),
            Some("Spoof/1.0".to_string())
        );
        // Suffix rule: the parent rule governs a subdomain request.
        assert_eq!(
            spoof_for("www.example.com").unwrap(),
            Some("Spoof/1.0".to_string())
        );
        // A more specific rule wins.
        assert!(spoof_set("www.example.com", "Deep/2.0").unwrap());
        assert_eq!(
            spoof_for("www.example.com").unwrap(),
            Some("Deep/2.0".to_string())
        );
        assert_eq!(
            spoof_for("other.example.com").unwrap(),
            Some("Spoof/1.0".to_string())
        );
        // Unrelated hosts untouched.
        assert!(spoof_for("other.test").unwrap().is_none());
        let list: serde_json::Value =
            serde_json::from_str(&spoof_list().unwrap()).unwrap();
        assert_eq!(list["example.com"], "Spoof/1.0");
        assert_eq!(list["www.example.com"], "Deep/2.0");
        // Persistence: the rows are in sitedecisions.json.
        assert!(dir.join("sitedecisions.json").exists());
        assert!(spoof_remove("www.example.com").unwrap());
        assert_eq!(
            spoof_for("www.example.com").unwrap(),
            Some("Spoof/1.0".to_string())
        );
        assert!(spoof_remove("example.com").unwrap());
        assert!(spoof_for("example.com").unwrap().is_none());

        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn spoof_rejects_header_injection() {
        let _guard = crate::store::test_lock();
        let dir = std::env::temp_dir().join(format!(
            "arora-ua-inj-{}",
            std::process::id()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        crate::store::lock()
            .set_data_dir(dir.to_str().unwrap())
            .unwrap();
        let _ = sitedecisions::reset();

        for bad in [
            "Good/1.0\r\nX-Injected: 1",
            "Good/1.0\nSet-Cookie: x",
            "a\0b",
            "tab\tua",
        ] {
            assert!(spoof_set("evil.test", bad).is_err(), "{bad:?}");
        }
        assert!(spoof_set("evil.test", "").is_err());
        assert!(spoof_for("evil.test").unwrap().is_none());

        let _ = std::fs::remove_dir_all(&dir);
    }
}
