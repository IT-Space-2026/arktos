//! Suggested-file-name sanitization — the Rust-side mirror of
//! DownloadItem::sanitizeFileName, plus the pieces Chromium used to
//! handle for us (drive letters, Windows device names, %-escapes).
//! A hostile server or URL must never steer the engine outside the
//! destination directory.

/// Reserved DOS device stems — `CON`, `CON.txt`, `con.` are all
/// aliases of the same device on Windows.
const DEVICE_NAMES: &[&str] = &[
    "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8",
    "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
];

fn hex_val(b: u8) -> Option<u8> {
    match b {
        b'0'..=b'9' => Some(b - b'0'),
        b'a'..=b'f' => Some(b - b'a' + 10),
        b'A'..=b'F' => Some(b - b'A' + 10),
        _ => None,
    }
}

/// Decodes `%XX` escapes — but only for characters that can never
/// alter path semantics (never `/`, `\`, `.`, control chars, NUL, or
/// `:`), so an encoded traversal cannot launder itself back into a
/// separator or a device name through this path.
fn percent_decode_safe(input: &str) -> String {
    let bytes = input.as_bytes();
    let mut out = String::with_capacity(input.len());
    let mut i = 0;
    while i < bytes.len() {
        if bytes[i] == b'%' && i + 2 < bytes.len() + 1 {
            if let (Some(h), Some(l)) = (
                bytes.get(i + 1).and_then(|b| hex_val(*b)),
                bytes.get(i + 2).and_then(|b| hex_val(*b)),
            ) {
                let v = (h << 4) | l;
                // ASCII printable, non-structural characters only.
                if (0x21..=0x7e).contains(&v)
                    && v != b'/'
                    && v != b'\\'
                    && v != b'.'
                    && v != b':'
                    && v != b'%'
                {
                    out.push(v as char);
                    i += 3;
                    continue;
                }
            }
        }
        out.push(bytes[i] as char);
        i += 1;
    }
    out
}

/// Sanitizes a server/URL-suggested file name to a bare, safe file
/// name.  Returns None when nothing usable remains (caller falls
/// back to a generated name).  Mirrors the Qt-side
/// DownloadItem::sanitizeFileName contract and adds the Windows
/// hazards Chromium folded in before: drive letters, UNC leads,
/// reserved device names, trailing dots/spaces.
pub fn file_name(suggested: &str) -> Option<String> {
    let decoded = percent_decode_safe(suggested);

    // Keep only the last path component — check both separators so a
    // Windows-style "..\name" cannot traverse either.
    let mut name = decoded;
    if let Some(pos) = name.rfind(['/', '\\']) {
        name = name.split_at(pos + 1).1.to_string();
    }
    // A drive-letter prefix ("C:foo") is just a name on POSIX but a
    // rooted path on Windows — strip any "<letter>:" lead.
    let b = name.as_bytes();
    if b.len() >= 2 && b[1] == b':' && b[0].is_ascii_alphabetic() {
        name = name.split_at(2).1.to_string();
    }

    // Drop control characters outright; they corrupt labels and
    // filesystems alike.
    let cleaned: String = name
        .chars()
        .filter(|c| *c >= '\u{20}' && *c != '\u{7f}')
        .collect();
    let mut name = cleaned.trim().to_string();
    // Trailing dots/spaces are invisible-but-significant on Windows.
    while name.ends_with('.') || name.ends_with(' ') {
        name.pop();
    }

    if name.is_empty() || name == "." || name == ".." {
        return None;
    }

    // A device name would open a Windows device, not a file.
    let stem = name.split('.').next().unwrap_or("").to_lowercase();
    if DEVICE_NAMES.contains(&stem.as_str()) {
        name = format!("_{name}");
    }

    // NAME_MAX is 255 bytes on common filesystems; leave headroom for
    // the "-NN" dedup suffix while keeping the extension readable.
    const MAX_LEN: usize = 200;
    if name.chars().count() > MAX_LEN {
        let keep: String = name.chars().take(MAX_LEN).collect();
        name = match name.rfind('.') {
            Some(dot) if name.len() - dot <= 16 => {
                let ext = &name[dot..];
                let room = MAX_LEN.saturating_sub(ext.chars().count());
                let mut base: String = keep.chars().take(room).collect();
                base.push_str(ext);
                base
            }
            _ => keep,
        };
    }

    if name.is_empty() {
        return None;
    }
    Some(name)
}

/// `name` with a "N." numeric insertion for dedup — `file.txt` ->
/// `file-3.txt` when `n == 3`.
pub fn dedup_name(name: &str, n: u32) -> String {
    match name.rfind('.') {
        Some(dot) if dot > 0 => format!("{}-{}{}", &name[..dot], n, &name[dot..]),
        _ => format!("{name}-{n}"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn traversal_stripped() {
        assert_eq!(file_name("../../evil"), Some("evil".to_string()));
        assert_eq!(file_name("..\\..\\evil"), Some("evil".to_string()));
        assert_eq!(file_name("/etc/passwd"), Some("passwd".to_string()));
        assert_eq!(file_name("a/b/c/file.txt"), Some("file.txt".to_string()));
    }

    #[test]
    fn dotfiles_and_empty() {
        assert_eq!(file_name(""), None);
        assert_eq!(file_name("."), None);
        assert_eq!(file_name(".."), None);
        assert_eq!(file_name("../../"), None);
        assert_eq!(file_name("   "), None);
    }

    #[test]
    fn windows_hazards() {
        assert_eq!(file_name("C:boot.ini"), Some("boot.ini".to_string()));
        assert_eq!(file_name("CON"), Some("_CON".to_string()));
        assert_eq!(file_name("con.txt"), Some("_con.txt".to_string()));
        assert_eq!(file_name("lpt3"), Some("_lpt3".to_string()));
        assert_eq!(file_name("name."), Some("name".to_string()));
        assert_eq!(file_name("name..."), Some("name".to_string()));
    }

    #[test]
    fn control_chars() {
        assert_eq!(file_name("a\u{0}b.txt"), Some("ab.txt".to_string()));
        assert_eq!(file_name("nu\u{7}l"), Some("_nul".to_string()));
    }

    #[test]
    fn percent_decoding() {
        assert_eq!(file_name("my%20file.txt"), Some("my%20file.txt".to_string()));
        // Space is not whitelisted either — stays encoded literally.
        assert_eq!(file_name("a%41b"), Some("aAb".to_string()));
        // Encoded separators/dots can never re-materialize.
        assert_eq!(file_name("%2e%2e%2fevil"), Some("%2e%2e%2fevil".to_string()));
    }

    #[test]
    fn dedup() {
        assert_eq!(dedup_name("file.txt", 2), "file-2.txt");
        assert_eq!(dedup_name("noext", 1), "noext-1");
        assert_eq!(dedup_name(".hidden", 1), ".hidden-1");
    }

    #[test]
    fn long_names() {
        let long = "x".repeat(300) + ".bin";
        let got = file_name(&long).unwrap();
        assert!(got.chars().count() <= 200);
        assert!(got.ends_with(".bin"));
    }
}
