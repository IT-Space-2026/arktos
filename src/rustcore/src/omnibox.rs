//! OMNI01: the omnibox routing decision + history frecency ranking —
//! the Qt shell thin-wraps these instead of owning the logic.
//!
//! ## Classifier
//!
//! `classify` reproduces TabWidget::guessUrlFromString's verdict —
//! including its embedded `QUrl::fromUserInput` heuristics — so the
//! FFI result can drive the location bar identically.  The mapping is
//! mechanical: every rule below has a Qt counterpart, and the autotest
//! corpus-diffs the two implementations on a captured input set.
//!
//!   1. `keyword <terms>` engine shortcuts win first (the keyword set
//!      is caller-supplied; engines stay Qt-owned, the verdict names
//!      the keyword and the shell resolves the search URL).
//!   2. A `QUrl::fromUserInput` port decides the url guess:
//!      a. a bare IPv6 literal ("::1") -> "http://[::1]"
//!      b. an absolute filesystem path -> file verdict
//!      c. raw parses as a scheme-ful URL and the http-prepended parse
//!         yields no port (i.e. "host:port" where "port" is not a
//!         number counts as schemed, but "localhost:8080" does not)
//!         -> navigate verbatim
//!      d. otherwise the http(s)-prepended parse is the guess when it
//!         is valid and has a host or a path; "ftp.<anything>" flips
//!         the prepended scheme to ftp like Qt does.
//!   3. Input with whitespace, or no usable guess at all, falls back:
//!      search-engine verdict when `search_fallback` is set, else the
//!      historic bare-"http://" guess (the searchEngineFallback opt-out
//!      semantics).
//!   4. A guessed http(s) url — one whose scheme the user did not
//!      literally type — only navigates when the text is address-shaped
//!      (localhost / *.localhost / host:port / a dotted host).  Bare
//!      words fall back to search.
//!
//! Verdict JSON:
//!   {"kind":"navigate","url":...}   load this url (web/ftp)
//!   {"kind":"internal","url":...}   explicit non-web scheme typed
//!                                   (about:, mailto:, qrc:, ...)
//!   {"kind":"file","path":...}      local filesystem path
//!   {"kind":"search","engine":null|<kw>,"query":...}
//!                                   resolve via the context engine,
//!                                   or the keyword's engine
//!
//! ## Frecency
//!
//! `frecency` reproduces HistoryFilterModel's per-visit decay sum:
//! each visit scores by its age in *local calendar days* (Qt computes
//! QDateTime::daysTo on the date parts, not on elapsed time):
//!   <= 1 day -> 100, < 5 -> 90, < 15 -> 70, < 31 -> 50,
//!   < 91 -> 30, else 10.
//! `typed` (typed-in visits) and `bookmarked` are the documented
//! extension points — both zero on today's store, so ordering on
//! existing data is unchanged.

use serde_json::{json, Value};

/// Typed-visit boost per visit (currently always 0 — the store does
/// not record typedness yet).
const TYPED_BOOST: i64 = 20;
/// Bookmark boost: a bookmarked url outranks a visited-but-unsaved
/// one of the same age.
const BOOKMARK_BOOST: i64 = 100;

// --------------------------------------------------------------------
// QUrl::fromUserInput port
// --------------------------------------------------------------------

/// Parsed enough to answer the two questions fromUserInput asks of the
/// http-prepended url: is it valid, and what is its port / host / path.
struct Prepended {
    valid: bool,
    host_nonempty: bool,
    tail_nonempty: bool,
    port: Option<u16>,
}

fn has_ctl(s: &str) -> bool {
    s.chars().any(|c| c < '\u{20}' || c == '\u{7f}')
}

/// Index of the ':' ending an RFC scheme, if `s` opens with one.
fn scheme_end(s: &str) -> Option<usize> {
    let mut it = s.char_indices();
    match it.next() {
        Some((_, c)) if c.is_ascii_alphabetic() => {}
        _ => return None,
    }
    for (i, c) in it {
        if c == ':' {
            return Some(i);
        }
        if !(c.is_ascii_alphanumeric() || c == '+' || c == '-' || c == '.') {
            return None;
        }
    }
    None
}

fn is_host_char(c: char) -> bool {
    c.is_ascii_alphanumeric() || c == '-' || c == '_' || !c.is_ascii()
}

fn is_host_edge(c: char) -> bool {
    // A label may not begin or end with '-' (Qt rejects "-x", "x-",
    // "-.b"); '_' and non-ASCII are fine at the edges.
    c.is_ascii_alphanumeric() || c == '_' || !c.is_ascii()
}

/// QUrl's tolerant host check, as probed against Qt 6.12: labels are
/// dot-separated, a trailing dot is legal, empty non-final labels and
/// '-'-edged labels are not, '%' and other punctuation are not host
/// characters.
fn host_ok(host: &str) -> bool {
    if host.is_empty() {
        return true;
    }
    let mut labels = host.split('.').peekable();
    while let Some(label) = labels.next() {
        if label.is_empty() {
            // Only the trailing label may be empty (FQDN dot).
            return labels.peek().is_none();
        }
        let mut chars = label.chars();
        if !is_host_edge(chars.next().unwrap()) {
            return false;
        }
        let mut last = '\0';
        for c in chars.by_ref() {
            if !is_host_char(c) {
                return false;
            }
            last = c;
        }
        if last != '\0' && !is_host_edge(last) {
            return false;
        }
    }
    true
}

/// Splits an authority component into (host, port) and validates it
/// the way the probed QUrl does: userinfo ends at the last '@',
/// bracketed literals keep colons inside, an empty port field is
/// "absent", anything else after the last ':' must be digits and
/// <= 65535.
fn authority_ok(auth: &str) -> (bool, bool, Option<u16>) {
    if auth.chars().any(|c| c == ' ' || c < '\u{20}' || c == '\u{7f}') {
        return (false, false, None);
    }
    let hostport = match auth.rfind('@') {
        Some(i) => &auth[i + 1..],
        None => auth,
    };
    if let Some(rest) = hostport.strip_prefix('[') {
        let end = match rest.find(']') {
            Some(e) => e,
            None => return (false, false, None),
        };
        let tail = &rest[end + 1..];
        if tail.is_empty() {
            return (true, !rest[..end].is_empty(), None);
        }
        let port_s = match tail.strip_prefix(':') {
            Some(p) => p,
            None => return (false, false, None),
        };
        // Digits past 65535 make the url invalid, not just port-less.
        if !port_digits(port_s) || port_value(port_s).is_none() {
            return (false, false, None);
        }
        return (true, true, port_value(port_s));
    }
    let (host, port_s) = match hostport.rfind(':') {
        Some(i) => (&hostport[..i], Some(&hostport[i + 1..])),
        None => (hostport, None),
    };
    if !host_ok(host) {
        return (false, false, None);
    }
    match port_s {
        None => (true, !host.is_empty(), None),
        Some(p) if p.is_empty() => (true, !host.is_empty(), None),
        Some(p) => match port_value(p) {
            Some(v) => (true, !host.is_empty(), Some(v)),
            None => (false, false, None),
        },
    }
}

fn port_digits(p: &str) -> bool {
    !p.is_empty() && p.bytes().all(|b| b.is_ascii_digit())
}

fn port_value(p: &str) -> Option<u16> {
    if !port_digits(p) {
        return None;
    }
    p.parse::<u32>().ok().filter(|&v| v <= 65535).map(|v| v as u16)
}

/// Parses "http://"+input the way the guess needs it.
fn parse_prepended(input: &str) -> Prepended {
    if has_ctl(input) {
        return Prepended {
            valid: false,
            host_nonempty: false,
            tail_nonempty: false,
            port: None,
        };
    }
    let rest = input;
    let aend = rest
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(rest.len());
    let (valid, host_nonempty, port) = authority_ok(&rest[..aend]);
    Prepended {
        valid,
        host_nonempty,
        tail_nonempty: aend < rest.len(),
        port,
    }
}

/// Raw-parse of the typed text: a scheme prefix makes it a scheme URL;
/// without one, a ':' in the first segment is what makes QUrl call it
/// invalid ("127.0.0.1:3000", "[::1]").  Everything else tolerantly
/// parses — including spaces in the path.
fn raw_valid_and_scheme(input: &str) -> (bool, bool) {
    if has_ctl(input) {
        return (false, false);
    }
    if let Some(i) = scheme_end(input) {
        let rest = &input[i + 1..];
        if let Some(auth_rest) = rest.strip_prefix("//") {
            let aend = auth_rest
                .find(|c| c == '/' || c == '?' || c == '#')
                .unwrap_or(auth_rest.len());
            let (ok, _, _) = authority_ok(&auth_rest[..aend]);
            return (ok, true);
        }
        return (true, true);
    }
    let seg_end = input
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(input.len());
    (!input[..seg_end].contains(':'), false)
}

/// ftp scheme adjustment — QUrl's adjustFtpPath turns a "//" path into
/// "/%2F..." so the literal-slash stays on parse.
fn adjust_ftp(url: String) -> String {
    if !url.starts_with("ftp://") {
        return url;
    }
    let rest = &url[6..];
    let aend = rest
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(rest.len());
    let path = &rest[aend..];
    if path.starts_with("//") {
        return format!("{}/%2F{}", &url[..6 + aend], &path[2..]);
    }
    url
}

enum Resolved {
    Url(String),
    File(String),
}

/// QUrl::fromUserInput with an empty working directory, ported against
/// the Qt 6.12 source and probed behavior.
fn from_user_input(t: &str) -> Option<Resolved> {
    if t.is_empty() {
        return None;
    }
    // Bare IPv6 literal ("::1") -> http://[::1]
    if t.parse::<std::net::Ipv6Addr>().is_ok() {
        return Some(Resolved::Url(format!("http://[{t}]")));
    }
    let (raw_valid, raw_has_scheme) = raw_valid_and_scheme(t);
    if std::path::Path::new(t).is_absolute() {
        return Some(Resolved::File(t.to_string()));
    }
    let prep = parse_prepended(t);
    if raw_valid && raw_has_scheme && prep.port.is_none() {
        return Some(Resolved::Url(adjust_ftp(t.to_string())));
    }
    if prep.valid && (prep.host_nonempty || prep.tail_nonempty) {
        // "ftp.<...>" flips the prepended scheme to ftp.
        let hostscheme = match t.find('.') {
            Some(i) => &t[..i],
            None => t,
        };
        if hostscheme.eq_ignore_ascii_case("ftp") {
            return Some(Resolved::Url(adjust_ftp(format!("ftp://{t}"))));
        }
        return Some(Resolved::Url(format!("http://{t}")));
    }
    None
}

// --------------------------------------------------------------------
// looksLikeAddress port (tabwidget.cpp)
// --------------------------------------------------------------------

fn looks_like_address(text: &str) -> bool {
    let host = match text.find('/') {
        Some(i) => &text[..i],
        None => text,
    };
    if host.eq_ignore_ascii_case("localhost")
        || host.to_ascii_lowercase().ends_with(".localhost")
    {
        return true;
    }
    if hostport_shape(text) {
        return true;
    }
    match host.find('.') {
        Some(dot) => dot > 0,
        None => false,
    }
}

/// ^(\[[0-9a-fA-F:]+\]|[A-Za-z0-9._~-]+):\d+(/.*)?$ — a bracketed
/// literal's inner colons never split the head.
fn hostport_shape(text: &str) -> bool {
    let rest = if let Some(inner_all) = text.strip_prefix('[') {
        let close = match inner_all.find(']') {
            Some(i) => i,
            None => return false,
        };
        let inner = &inner_all[..close];
        if inner.is_empty()
            || !inner.chars().all(|c| c.is_ascii_hexdigit() || c == ':')
        {
            return false;
        }
        match inner_all[close + 1..].strip_prefix(':') {
            Some(p) => p,
            None => return false,
        }
    } else {
        let i = match text.find(':') {
            Some(i) => i,
            None => return false,
        };
        let head = &text[..i];
        if head.is_empty()
            || !head.chars().all(|c| {
                c.is_ascii_alphanumeric()
                    || c == '.'
                    || c == '_'
                    || c == '~'
                    || c == '-'
            })
        {
            return false;
        }
        &text[i + 1..]
    };
    let port = match rest.find('/') {
        Some(i) => &rest[..i],
        None => rest,
    };
    !port.is_empty() && port.bytes().all(|b| b.is_ascii_digit())
}

// --------------------------------------------------------------------
// the verdict
// --------------------------------------------------------------------

fn search_verdict(engine: Option<&str>, query: &str) -> Value {
    match engine {
        Some(kw) => json!({"kind":"search","engine":kw,"query":query}),
        None => json!({"kind":"search","engine":null,"query":query}),
    }
}

fn fallback_verdict(trimmed: &str, search_fallback: bool) -> Value {
    if search_fallback {
        search_verdict(None, trimmed)
    } else {
        // The searchEngineFallback opt-out: the historic bare-http guess.
        json!({"kind":"navigate","url":format!("http://{trimmed}")})
    }
}

/// Routes omnibox text to a verdict — the full port of
/// TabWidget::guessUrlFromString's decision.
pub fn classify(input: &str, keywords: &[String], search_fallback: bool) -> Value {
    let trimmed = input.trim();

    // 1. 'keyword terms' engine shortcuts keep first priority.
    if let Some(i) = trimmed.find(' ') {
        if i > 0 {
            let terms = &trimmed[i + 1..];
            if !terms.is_empty() && keywords.iter().any(|k| k == &trimmed[..i]) {
                return search_verdict(Some(&trimmed[..i]), terms);
            }
        }
    }

    // 2. The url guess — the QUrl::fromUserInput port.
    let resolved = from_user_input(trimmed);

    // 3. Whitespace input or an unresolvable guess can only be a search
    //    (or the opt-out bare-http guess).
    let has_space = trimmed.chars().any(|c| c.is_whitespace());
    if trimmed.is_empty() || has_space {
        return fallback_verdict(trimmed, search_fallback);
    }
    let resolved = match resolved {
        Some(r) => r,
        None => return fallback_verdict(trimmed, search_fallback),
    };

    let url = match resolved {
        Resolved::File(p) => return json!({"kind":"file","path":p}),
        Resolved::Url(u) => u,
    };

    // 4. http(s) the user did not literally type is a *guess* — it only
    //    counts as an address when the text is address-shaped.
    let scheme = match scheme_end(&url) {
        Some(i) => url[..i].to_ascii_lowercase(),
        None => String::new(),
    };
    let typed_http = trimmed
        .get(..4)
        .map(|p| p.eq_ignore_ascii_case("http"))
        .unwrap_or(false);
    let guessed_http = (scheme == "http" || scheme == "https") && !typed_http;
    if !guessed_http {
        if scheme == "http" || scheme == "https" || scheme == "ftp" {
            return json!({"kind":"navigate","url":url});
        }
        return json!({"kind":"internal","url":url});
    }

    if looks_like_address(trimmed) {
        return json!({"kind":"navigate","url":url});
    }

    fallback_verdict(trimmed, search_fallback)
}

// --------------------------------------------------------------------
// frecency (HistoryFilterModel::frecencyScore port)
// --------------------------------------------------------------------

/// Per-visit decay score by age in local calendar days —
/// HistoryFilterModel::frecencyScore's buckets verbatim.
pub fn decay(days: i64) -> i64 {
    if days <= 1 {
        100
    } else if days < 5 {
        90
    } else if days < 15 {
        70
    } else if days < 31 {
        50
    } else if days < 91 {
        30
    } else {
        10
    }
}

/// Howard Hinnant's days-from-civil: days since 1970-01-01 for a
/// Gregorian y/m/d.
fn days_from_civil(y: i64, m: i64, d: i64) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + d - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

/// Local-civil day number for a millisecond epoch stamp — mirrors
/// QDateTime::fromMSecsSinceEpoch(ms).date() under the system zone.
/// Non-unix targets fall back to UTC days (scores shift at most by
/// one bucket boundary — documented, never wrong-signed).
#[cfg(unix)]
pub fn local_day_number(ms: i64) -> i64 {
    unsafe {
        let t = (ms.div_euclid(1000)) as libc::time_t;
        let mut tm: libc::tm = std::mem::zeroed();
        if libc::localtime_r(&t, &mut tm).is_null() {
            return ms.div_euclid(86_400_000);
        }
        days_from_civil(
            (tm.tm_year + 1900) as i64,
            (tm.tm_mon + 1) as i64,
            tm.tm_mday as i64,
        )
    }
}

#[cfg(not(unix))]
pub fn local_day_number(ms: i64) -> i64 {
    ms.div_euclid(86_400_000)
}

/// The ranking function: summed per-visit decay plus the typed/bookmark
/// boosts.  Reproduces HistoryFilterModel's aggregate score.
pub fn frecency(visit_ms: &[i64], now_ms: i64, typed: i64, bookmarked: bool) -> i64 {
    let now_day = local_day_number(now_ms);
    let base: i64 = visit_ms
        .iter()
        .map(|&v| decay(now_day - local_day_number(v)))
        .sum();
    base + typed.max(0) * TYPED_BOOST + if bookmarked { BOOKMARK_BOOST } else { 0 }
}

/// Host component of a stored/emitted url — "" for opaque schemes —
/// enough of QUrl::host() for the completer's boundary check.
pub fn url_host(url: &str) -> &str {
    let rest = match url.find("://") {
        Some(i) => &url[i + 3..],
        None => return "",
    };
    let aend = rest
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(rest.len());
    let auth = &rest[..aend];
    let hostport = match auth.rfind('@') {
        Some(i) => &auth[i + 1..],
        None => auth,
    };
    if let Some(b) = hostport.strip_prefix('[') {
        return match b.find(']') {
            Some(e) => &b[..e],
            None => "",
        };
    }
    match hostport.find(':') {
        Some(i) => &hostport[..i],
        None => hostport,
    }
}

/// QRegularExpression "\b<term>" parity: true when `term` occurs in
/// `haystack` (case-insensitive) starting at a word boundary — the
/// completer's host/title tie-breaker bonus.
pub fn word_boundary_match(haystack: &str, term: &str) -> bool {
    if term.is_empty() {
        return true;
    }
    let hay = haystack.to_lowercase();
    let needle = term.to_lowercase();
    for (i, _) in hay.match_indices(&needle) {
        let boundary = hay[..i]
            .chars()
            .next_back()
            .map(|c| !(c.is_alphanumeric() || c == '_'))
            .unwrap_or(true);
        if boundary {
            return true;
        }
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    fn kw(list: &[&str]) -> Vec<String> {
        list.iter().map(|s| s.to_string()).collect()
    }

    fn classify_str(input: &str) -> Value {
        classify(input, &kw(&["ot"]), true)
    }

    #[test]
    fn keyword_shortcut_wins() {
        assert_eq!(
            classify_str("ot hello"),
            json!({"kind":"search","engine":"ot","query":"hello"})
        );
        // Even over address-shaped terms.
        assert_eq!(
            classify_str("ot a.b"),
            json!({"kind":"search","engine":"ot","query":"a.b"})
        );
        // Unknown keyword: no shortcut — "word terms" is a search.
        assert_eq!(
            classify_str("zz hello"),
            json!({"kind":"search","engine":null,"query":"zz hello"})
        );
        // Keyword alone (no terms) is not a shortcut.
        assert_eq!(
            classify_str("ot"),
            json!({"kind":"search","engine":null,"query":"ot"})
        );
    }

    #[test]
    fn address_shapes_navigate() {
        for input in [
            "docs.qt.io",
            "a.b",
            "localhost",
            "localhost.",
            "x.localhost",
            "localhost:8080",
            "[::1]:8080",
            "host:80/x",
            "127.0.0.1",
            "192.168.1.1",
             
        ] {
            let v = classify_str(input);
            assert_eq!(v["kind"], "navigate", "{input}");
        }
        assert_eq!(
            classify_str("docs.qt.io")["url"],
            "http://docs.qt.io"
        );
        assert_eq!(
            classify_str("localhost:8080")["url"],
            "http://localhost:8080"
        );
        assert_eq!(classify_str("ftp.example.com")["url"], "ftp://ftp.example.com");
    }

    #[test]
    fn bare_words_and_garbage_search() {
        for input in [
            "word", "hup", "ph", "test.foo bar", "a phrase with spaces",
            "x y", "-flag", ".hidden", "a..b", "~/foo", "./rel",
            "::1", "[::1]", "user@:80", "h;p",
        ] {
            let v = classify_str(input);
            assert_eq!(v["kind"], "search", "{input}");
            assert_eq!(v["engine"], serde_json::Value::Null, "{input}");
            assert_eq!(v["query"], input.trim(), "{input}");
        }
    }

    #[test]
    fn explicit_schemes_and_scheme_lookalikes() {
        for input in [
            "file:///etc/hostname",
            "localhost:abc", "example.com:99999", "host:65536", "word:",
            "a:b:c", "word:x", "C:\\foo",
        ] {
            let v = classify_str(input);
            assert_eq!(v["kind"], "internal", "{input}");
        }
        // http(s)/ftp typed explicitly stay "navigate".
        assert_eq!(classify_str("http://example.com/x")["kind"], "navigate");
        assert_eq!(classify_str("https://x")["kind"], "navigate");
        assert_eq!(classify_str("ftp://h/p")["kind"], "navigate");
        // Non-web schemes land as "internal".
        for input in ["about:config", "mailto:a@b.c", "javascript:void(0)",
                      "data:text/plain,hi", "qrc:/startpage.html",
                      "chrome://x", "view-source:x"] {
            assert_eq!(classify_str(input)["kind"], "internal", "{input}");
        }
        // Scheme lookalikes where the "port" is not numeric also go
        // verbatim — the Qt quirk where "localhost:abc" is a scheme url.
        assert_eq!(classify_str("localhost:abc")["url"], "localhost:abc");
        assert_eq!(classify_str("example.com:99999")["url"], "example.com:99999");
    }

    #[test]
    fn files_and_ipv6() {
        assert_eq!(
            classify_str("/etc/hostname"),
            json!({"kind":"file","path":"/etc/hostname"})
        );
        assert_eq!(
            classify_str("/abs/path"),
            json!({"kind":"file","path":"/abs/path"})
        );
        // A bare IPv6 literal resolves then fails looks_like_address —
        // search, exactly like the C++ path.
        assert_eq!(
            classify_str("::1"),
            json!({"kind":"search","engine":null,"query":"::1"})
        );
    }

    #[test]
    fn opt_out_restores_http_guess() {
        let v = classify("word", &kw(&[]), false);
        assert_eq!(v, json!({"kind":"navigate","url":"http://word"}));
        // But keyword search still wins when opted out.
        let v = classify("ot hi", &kw(&["ot"]), false);
        assert_eq!(v["kind"], "search");
        // And a real address still navigates.
        let v = classify("a.b", &kw(&[]), false);
        assert_eq!(v["url"], "http://a.b");
    }

    #[test]
    fn userinfo_and_ports() {
        assert_eq!(classify_str("user@host.com")["url"], "http://user@host.com");
        assert_eq!(classify_str("u:p@host.com")["kind"], "internal");
        assert_eq!(classify_str("h:443/")["url"], "http://h:443/");
        assert_eq!(classify_str("z:1")["url"], "http://z:1");
        assert_eq!(classify_str("h:080")["url"], "http://h:080");
    }

    #[test]
    fn decay_buckets() {
        assert_eq!(decay(0), 100);
        assert_eq!(decay(1), 100);
        assert_eq!(decay(2), 90);
        assert_eq!(decay(4), 90);
        assert_eq!(decay(5), 70);
        assert_eq!(decay(14), 70);
        assert_eq!(decay(15), 50);
        assert_eq!(decay(30), 50);
        assert_eq!(decay(31), 30);
        assert_eq!(decay(90), 30);
        assert_eq!(decay(91), 10);
        assert_eq!(decay(400), 10);
        assert_eq!(decay(-3), 100);
    }

    #[test]
    fn frecency_sums_and_boosts() {
        let now = 1_800_000_000_000i64;
        let day = 86_400_000i64;
        let s = frecency(&[now, now - 3 * day], now, 0, false);
        assert_eq!(s, 100 + 90);
        // typed + bookmark boosts are additive extensions.
        assert!(frecency(&[now], now, 2, true) > frecency(&[now], now, 0, false));
    }

    #[test]
    fn boundary_bonus() {
        assert!(word_boundary_match("dot.kde.org", "dot"));
        assert!(word_boundary_match("www.phoronix.com", "ph"));
        assert!(!word_boundary_match("slashdot.org", "dot"));
        assert!(word_boundary_match("Slashdot", "slash"));
        assert!(word_boundary_match("anything", ""));
    }
}
