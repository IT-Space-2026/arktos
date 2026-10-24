//! Qt `QSettings::IniFormat` reader (RC02) — the legacy settings
//! import path.
//!
//! The importer only needs READ coverage: settings.json is the
//! canonical store going forward and ini is never written.  The
//! grammar below was verified byte-for-byte against Qt 6.12's
//! writer:
//!
//!   - `key path` splits at the FIRST '/': head is the ini section,
//!     tail is the key.  Inside both, '%' introduces a two-hex-digit
//!     escape (latin1 code point — `ü` writes %FC) and a bare '\'
//!     encodes a literal '/' inside the key tail (a real backslash
//!     writes %5C).
//!   - Values are split at top-level commas (commas inside "..."
//!     quotes do not split); >1 element reads back as a QStringList.
//!   - Each element: surrounding whitespace is trimmed; a "..."
//!     wrapper strips and inner escapes decode; unquoted elements
//!     decode the same escapes: \a \b \f \n \r \t \v \0 \' \" \\ and
//!     \x + up to four hex digits (a unicode code point).
//!   - Decoded text starting '@' is a type escape: "@@" is a literal
//!     leading '@', "@Invalid()" is a null QVariant (skipped), and
//!     "@ByteArray(...)" decodes its inner text to latin1 bytes.
//!     Other "@Type(...)" payloads (QVariant/QDataStream blobs,
//!     readable @Rect/@DateTime forms) are preserved verbatim —
//!     lossless for a store that only models
//!     string/bool/int/double/bytes/list.
//!   - Plain scalars carry NO type on disk: "2" reads back as the
//!     QString "2" (callers convert via toInt()/toBool()).  The
//!     importer therefore stores them as strings and the typed
//!     getters do QVariant-style conversion.
//!   - Key paths normalize like Qt's: '/'-separated segments, empty
//!     segments dropped ("https://x" collapses to "https:/x",
//!     trailing '/' is lost).
//!   - "[General]" (and the legacy "[%General%]") hold root keys;
//!     ';' and '#' lines are comments.

use std::collections::BTreeMap;

/// One imported value — the typed-store shapes plus `Raw` for
/// faithfully preserved "@Type(...)" escapes the store cannot
/// model.
#[derive(Clone, Debug, PartialEq)]
pub enum IniValue {
    Str(String),
    Bytes(Vec<u8>),
    List(Vec<String>),
    /// Verbatim "@Type(...)" text (or anything '@'-formed we do not
    /// understand) — imported as a string so nothing is destroyed.
    Raw(String),
}

/// Parse a QSettings ini document into normalized key-path ->
/// value rows.  Tolerant by design (import source): malformed
/// lines are skipped, never fatal.
pub fn parse(data: &[u8]) -> BTreeMap<String, IniValue> {
    let mut text = String::from_utf8_lossy(data).into_owned();
    if text.starts_with('\u{feff}') {
        text.remove(0);
    }
    let mut out = BTreeMap::new();
    let mut group = String::new();
    for line in text.split('\n') {
        let line = line.strip_suffix('\r').unwrap_or(line);
        let trimmed = line.trim();
        if trimmed.is_empty()
            || trimmed.starts_with(';')
            || trimmed.starts_with('#')
        {
            continue;
        }
        if let Some(rest) = trimmed.strip_prefix('[') {
            if let Some(name) = rest.strip_suffix(']') {
                let name = unescape_key(name.trim());
                // Root keys live under [General] (or the Qt4-era
                // [%General%]); anything else is a group name.
                group = if name.eq_ignore_ascii_case("general")
                    || name.eq_ignore_ascii_case("%general%")
                {
                    String::new()
                } else {
                    name
                };
            }
            continue;
        }
        let Some(eq) = line.find('=') else {
            continue;
        };
        let key = normalize_key(&unescape_key(line[..eq].trim()));
        if key.is_empty() {
            continue;
        }
        let path = if group.is_empty() {
            key
        } else {
            let mut p = group.clone();
            p.push('/');
            p.push_str(&key);
            p
        };
        if let Some(value) = parse_value(&line[eq + 1..]) {
            out.insert(path, value);
        }
    }
    out
}

/// Collapse a '/'-separated key path the way QSettings does:
/// empty segments are dropped outright.
pub fn normalize_key(key: &str) -> String {
    key.split('/')
        .filter(|seg| !seg.is_empty())
        .collect::<Vec<_>>()
        .join("/")
}

/// The ProfileSettings key-classification table (MPROF04 audit),
/// ported verbatim — the global-ini import lane keeps only the
/// keys the C++ facade would route to the profile store.
pub fn is_profile_key(path: &str) -> bool {
    const PROFILE_GROUPS: &[&str] = &[
        "AdBlock",
        "BrowserMainWindow",
        "LanguageManager",
        "MainWindow",
        "Settings",
        "SourceViewer",
        "autofill",
        "browser",
        "clearprivatedata",
        "commandPalette",
        "containers",
        "cookies",
        "downloadmanager",
        "extensions",
        "history",
        "navbar",
        "network",
        "openSearch",
        "performance",
        "popupExceptions",
        "privacy",
        "profile",
        "proxy",
        "reader",
        "scriptcontrol",
        "searchShortcuts",
        "sessions",
        "sidebar",
        "tabs",
        "toolbarsearch",
        "trust",
        "urlcleaner",
        "urlloading",
        "webpermissions",
        "websettings",
    ];
    const PROFILE_KEYS: &[&str] = &["userAgent"];
    const GLOBAL_EXCEPTIONS: &[&str] = &[
        "MainWindow/defaultEngine",
        "MainWindow/restoring",
        "browser/gpuCrashCount",
        "browser/lastExit",
        "browser/sessionActive",
        "browser/sessionActiveTor",
        "network/CaCertificates",
        "privacy/trancoListUpdated",
        "privacy/domainBlocklistUpdated",
    ];
    if GLOBAL_EXCEPTIONS.contains(&path) {
        return false;
    }
    let root = path.split('/').next().unwrap_or("");
    if PROFILE_GROUPS.contains(&root) {
        return true;
    }
    PROFILE_KEYS.contains(&path)
}

/// Unescape an ini key/section name: %XX -> latin1 code point,
/// '\' -> '/'.  Single pass so a decoded %5C is never re-read as
/// the path separator.
fn unescape_key(raw: &str) -> String {
    let mut out = String::with_capacity(raw.len());
    let mut chars = raw.chars();
    while let Some(c) = chars.next() {
        match c {
            '\\' => out.push('/'),
            '%' => {
                let hi = chars.next();
                let lo = chars.next();
                match (hi, lo) {
                    (Some(h), Some(l)) => {
                        let code = h
                            .to_digit(16)
                            .and_then(|h| l.to_digit(16).map(|l| h * 16 + l));
                        match code.and_then(char::from_u32) {
                            Some(d) => out.push(d),
                            None => {
                                out.push('%');
                                out.push(h);
                                out.push(l);
                            }
                        }
                    }
                    (Some(h), None) => {
                        out.push('%');
                        out.push(h);
                    }
                    _ => out.push('%'),
                }
            }
            c => out.push(c),
        }
    }
    out
}

/// Split a raw value at top-level commas — a comma inside a "..."
/// run (or behind a backslash escape) does not split.
fn split_elements(raw: &str) -> Vec<&str> {
    let mut out = Vec::new();
    let mut in_quotes = false;
    let mut escaped = false;
    let mut start = 0;
    for (i, c) in raw.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }
        match c {
            '\\' => escaped = true,
            '"' => in_quotes = !in_quotes,
            ',' if !in_quotes => {
                out.push(&raw[start..i]);
                start = i + 1;
            }
            _ => {}
        }
    }
    out.push(&raw[start..]);
    out
}

/// Decode one element: trim, strip the "..." wrapper when both
/// ends carry it, then resolve backslash escapes.
fn decode_element(raw: &str) -> String {
    let t = raw.trim();
    let inner = if t.len() >= 2 && t.starts_with('"') && t.ends_with('"') {
        &t[1..t.len() - 1]
    } else {
        t
    };
    let mut out = String::with_capacity(inner.len());
    let mut chars = inner.chars().peekable();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('x') | Some('X') => {
                // Up to four hex digits — Qt writes minimal-width
                // escapes and shields trailing hex-digit chars by
                // escaping them, so greedy parse is exact.
                let mut code: u32 = 0;
                let mut digits = 0;
                while digits < 4 {
                    match chars.peek().and_then(|c| c.to_digit(16)) {
                        Some(d) => {
                            code = code * 16 + d;
                            digits += 1;
                            chars.next();
                        }
                        None => break,
                    }
                }
                if digits == 0 {
                    out.push('x');
                } else if let Some(c) = char::from_u32(code) {
                    out.push(c);
                }
            }
            Some('a') => out.push('\x07'),
            Some('b') => out.push('\x08'),
            Some('f') => out.push('\x0c'),
            Some('n') => out.push('\n'),
            Some('r') => out.push('\r'),
            Some('t') => out.push('\t'),
            Some('v') => out.push('\x0b'),
            Some('0') => out.push('\0'),
            Some('\n') => {}
            Some(other) => out.push(other),
            None => {}
        }
    }
    out
}

/// Parse the right-hand side of a `key=value` line.
fn parse_value(raw: &str) -> Option<IniValue> {
    let elements = split_elements(raw);
    if elements.len() > 1 {
        return Some(IniValue::List(
            elements.iter().map(|e| decode_element(e)).collect(),
        ));
    }
    let text = decode_element(elements[0]);
    if let Some(rest) = text.strip_prefix('@') {
        if let Some(at) = rest.strip_prefix('@') {
            // "@@x" is a literal "@x".
            let mut s = String::with_capacity(at.len() + 1);
            s.push('@');
            s.push_str(at);
            return Some(IniValue::Str(s));
        }
        if rest == "Invalid()" {
            return None;
        }
        if let Some(inner) = rest
            .strip_prefix("ByteArray(")
            .and_then(|r| r.strip_suffix(')'))
        {
            // Latin1 semantics: code points > 0xFF degrade to '?'
            // exactly like QString::toLatin1.
            return Some(IniValue::Bytes(
                inner
                    .chars()
                    .map(|c| {
                        let cp = c as u32;
                        if cp <= 0xFF {
                            cp as u8
                        } else {
                            b'?'
                        }
                    })
                    .collect(),
            ));
        }
        return Some(IniValue::Raw(text));
    }
    Some(IniValue::Str(text))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse1(line: &str) -> Option<IniValue> {
        parse(format!("[g]\nk={line}\n").as_bytes())
            .remove("g/k")
            .map(|v| v)
    }

    #[test]
    fn scalars_and_escapes() {
        assert_eq!(parse1("hello"), Some(IniValue::Str("hello".into())));
        assert_eq!(parse1("42"), Some(IniValue::Str("42".into())));
        assert_eq!(parse1("true"), Some(IniValue::Str("true".into())));
        assert_eq!(
            parse1("has \\\"quotes\\\""),
            Some(IniValue::Str("has \"quotes\"".into()))
        );
        assert_eq!(
            parse1("a\\\\b"),
            Some(IniValue::Str("a\\b".into()))
        );
        assert_eq!(
            parse1("line1\\nline2"),
            Some(IniValue::Str("line1\nline2".into()))
        );
        assert_eq!(parse1("@@weird"), Some(IniValue::Str("@weird".into())));
        assert_eq!(
            parse1("\"  padded  \""),
            Some(IniValue::Str("  padded  ".into()))
        );
        assert_eq!(parse1(""), Some(IniValue::Str("".into())));
        assert_eq!(parse1("@Invalid()"), None);
        assert_eq!(
            parse1("\"a;b,c=d\""),
            Some(IniValue::Str("a;b,c=d".into()))
        );
        assert_eq!(parse1("100% sure %25"),
            Some(IniValue::Str("100% sure %25".into())));
    }

    #[test]
    fn lists() {
        assert_eq!(
            parse1("one, two, three"),
            Some(IniValue::List(vec![
                "one".into(),
                "two".into(),
                "three".into()
            ]))
        );
        assert_eq!(
            parse1("\"a,b\", \" c \", d\\\"e"),
            Some(IniValue::List(vec![
                "a,b".into(),
                " c ".into(),
                "d\"e".into()
            ]))
        );
    }

    #[test]
    fn bytearray_and_exotic() {
        assert_eq!(
            parse1("@ByteArray(\\x1\\x61)"),
            Some(IniValue::Bytes(vec![0x01, 0x61]))
        );
        assert_eq!(
            parse1("@ByteArray(plainbytes)"),
            Some(IniValue::Bytes(b"plainbytes".to_vec()))
        );
        // Qt writes `)(;,="` bytes as "@ByteArray()(;,=\")" — quoted
        // because of ;,= and with \" escaping the quote byte.
        assert_eq!(
            parse1("\"@ByteArray()(;,=\\\")\"").unwrap(),
            IniValue::Bytes(vec![0x29, 0x28, 0x3b, 0x2c, 0x3d, 0x22])
        );
        assert_eq!(
            parse1("@Rect(1 2 3 4)"),
            Some(IniValue::Raw("@Rect(1 2 3 4)".into()))
        );
    }

    #[test]
    fn keys_and_sections() {
        let doc = parse(
            b"[General]\nroot=1\n\n[a]\nb\\c\\d=2\n[grp%2520x]\nk=3\n\
              [semi%3Bcolon]\nk=4\n[%20sec%20]\nk=5\n[back%5Cslash]\nk=6\n",
        );
        assert_eq!(doc["root"], IniValue::Str("1".into()));
        assert_eq!(doc["a/b/c/d"], IniValue::Str("2".into()));
        assert_eq!(doc["grp%20x/k"], IniValue::Str("3".into()));
        assert_eq!(doc["semi;colon/k"], IniValue::Str("4".into()));
        assert_eq!(doc[" sec /k"], IniValue::Str("5".into()));
        assert_eq!(doc["back\\slash/k"], IniValue::Str("6".into()));
        // %5C decodes to a backslash, never re-read as '/'.
        assert!(!doc.contains_key("back/slash/k"));
    }

    #[test]
    fn classification_matches_cpp() {
        assert!(is_profile_key("privacy/securityLevel"));
        assert!(is_profile_key("MainWindow/home"));
        assert!(is_profile_key("userAgent"));
        assert!(!is_profile_key("MainWindow/defaultEngine"));
        assert!(!is_profile_key("browser/gpuCrashCount"));
        assert!(!is_profile_key("network/CaCertificates"));
        assert!(!is_profile_key("sandbox/enabled"));
        assert!(!is_profile_key("anything/atAll"));
        assert!(!is_profile_key("meta/settingsMigrated"));
    }
}
