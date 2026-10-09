//! Minimal Netscape cookie-file reader — enough for authenticated
//! downloads: the Qt side exports ONLY the target host's rows (DLACC05
//! owns that export); here we parse the file and build the Cookie
//! header.  Whole-jar files are tolerated but only rows matching the
//! request host are ever sent.

/// One parsed cookie row.
struct Cookie {
    domain: String,
    secure: bool,
    path: String,
    name: String,
    value: String,
}

/// Parses a Netscape-format cookie file into a `Cookie:` request
/// header value for `host`/`is_https`/`url_path`, or None when no row
/// matches.  Anything unreadable yields None — a download must never
/// fail because its optional cookie file was malformed.
pub fn header_for(path: &std::path::Path, host: &str, url_path: &str, is_https: bool) -> Option<String> {
    let body = std::fs::read_to_string(path).ok()?;
    let cookies = parse(&body);
    let host = host.to_lowercase();
    let mut pairs: Vec<String> = Vec::new();
    for c in cookies {
        if !domain_matches(&c.domain, &host) {
            continue;
        }
        if c.secure && !is_https {
            continue;
        }
        if !url_path.starts_with(&c.path) {
            continue;
        }
        pairs.push(format!("{}={}", c.name, c.value));
    }
    if pairs.is_empty() {
        None
    } else {
        Some(pairs.join("; "))
    }
}

fn parse(body: &str) -> Vec<Cookie> {
    let mut out = Vec::new();
    for line in body.lines() {
        let line = line.trim_end();
        if line.is_empty() || (line.starts_with('#') && !line.starts_with("#HttpOnly_")) {
            continue;
        }
        // #HttpOnly_ rows carry the marker as part of the domain field.
        let line = line.strip_prefix("#HttpOnly_").unwrap_or(line);
        let f: Vec<&str> = line.split('\t').collect();
        if f.len() < 7 {
            continue;
        }
        out.push(Cookie {
            domain: f[0].to_lowercase(),
            secure: f[3].eq_ignore_ascii_case("true"),
            path: if f[2].is_empty() { "/".into() } else { f[2].into() },
            name: f[5].into(),
            value: f[6].into(),
        });
    }
    out
}

/// Netscape rule: a leading-dot domain matches host == domain[1..]
/// and every subdomain; a bare domain matches exactly.
fn domain_matches(domain: &str, host: &str) -> bool {
    if let Some(base) = domain.strip_prefix('.') {
        host == base || host.ends_with(&format!(".{base}"))
    } else {
        host == domain
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_jar(body: &str) -> tempfile::NamedTempFile {
        let mut f = tempfile::NamedTempFile::new().unwrap();
        f.write_all(body.as_bytes()).unwrap();
        f.flush().unwrap();
        f
    }

    #[test]
    fn matching_rows_only() {
        let f = write_jar(
            ".example.com\tTRUE\t/\tFALSE\t0\tsess\tabc\n\
             other.org\tFALSE\t/\tFALSE\t0\tsess\tzzz\n",
        );
        assert_eq!(
            header_for(f.path(), "cdn.example.com", "/f", true),
            Some("sess=abc".to_string())
        );
        assert_eq!(header_for(f.path(), "other.org", "/f", true), Some("sess=zzz".to_string()));
        assert_eq!(header_for(f.path(), "nope.org", "/f", true), None);
    }

    #[test]
    fn secure_and_path_rules() {
        let f = write_jar(
            "example.com\tFALSE\t/\tTRUE\t0\ts\t1\n\
             example.com\tFALSE\t/app\tFALSE\t0\tp\t2\n",
        );
        // secure cookie refused on plain http
        assert_eq!(header_for(f.path(), "example.com", "/app/x", false), Some("p=2".into()));
        assert_eq!(
            header_for(f.path(), "example.com", "/app/x", true),
            Some("s=1; p=2".into())
        );
        // path mismatch drops the /app row
        assert_eq!(header_for(f.path(), "example.com", "/other", true), Some("s=1".into()));
    }

    #[test]
    fn malformed_never_fails() {
        let f = write_jar("garbage\n\t\t\t\n");
        assert_eq!(header_for(f.path(), "x.org", "/", true), None);
        assert_eq!(header_for(std::path::Path::new("/nonexistent"), "x.org", "/", true), None);
    }
}
