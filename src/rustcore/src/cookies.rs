//! RC15: the per-profile cookie database — `cookies.db` beside
//! `history.db` in the profile dir (or the anchored data dir on the
//! rc_* surface), SQLite via rusqlite like the sibling stores.
//!
//! The honest two-mode split (the task's engine-owned reality check):
//!
//!   * **authoritative** (role 0) — the store IS the jar.  A Servo
//!     backend applies its CRUD/matching results to traffic; the
//!     file on disk is the source of truth.
//!   * **mirror** (role 1) — the engine owns the jar (Chromium's
//!     QWebEngineCookieStore cannot be displaced; the C++ CookieJar's
//!     in-memory m_cookies is exactly this kind of mirror today).
//!     The same ops keep a durable copy the dialogs/sync surface can
//!     read, but delete means "forget locally" — the engine may put a
//!     cookie back.  `role` is a persisted meta row so tooling can
//!     tell what a db last served; the ops themselves are identical —
//!     honesty lives in the flag and this contract, not in hidden
//!     behavior.
//!
//! Container partitioning is a first-class column, not a second
//! store: ContainerManager gives each persona a registry of
//! uuid-hex containers over ONE profile dir, so `container` ("" =
//! the default, unlabeled context) partitions every row inside the
//! profile's single cookies.db.
//!
//! Field set — what cookiejar/* + SiteDecisionStore policy need:
//! name, value, domain, path, expires (ms-epoch, NULL = session),
//! secure, httponly, samesite (QNetworkCookie::SameSite ordinals
//! 0..3), host_only, container, created.  Domain + expiry are what
//! the RC06 "cookie" kind and the keep/session rewrite rules key on;
//! samesite is stored verbatim and applied at send time (see
//! `for_url`).
//!
//! Matching semantics — verified against Qt 6.12's
//! QNetworkCookieJar, not assumed:
//!
//!   * a stored domain is normalized (lowercase, no leading dot); the
//!     leading-dot bit lives in `host_only`.  Qt keeps the Set-Cookie
//!     Domain attribute verbatim and treats a no-dot value as
//!     host-exact — `domain=sub.example.com` in an export does NOT
//!     match deep.sub.example.com in the C++ jar (measured).  This
//!     store mirrors that: `Domain` WITH a leading dot imports as a
//!     domain cookie, without one as host-only, and a header with no
//!     Domain attribute as host-only to the request host (RFC6265
//!     would call no-dot Domain a domain cookie — Qt deviates, and
//!     the store stays Qt-parity so an export re-imports with
//!     identical reach).
//!   * path matching is RFC6265 §5.1.4 boundary-aware: "/app" matches
//!     "/app" and "/app/x", never "/apple".
//!   * secure cookies only match https/wss requests; expired rows are
//!     skipped at query time and swept by purge_expired.
//!   * samesite (send-side): when the request supplies a first-party
//!     host that is not same-site, SameSite=Lax and SameSite=Strict
//!     rows drop out (Lax's top-level-navigation exception belongs
//!     to the caller — the jar answers "may the cookie ride this
//!     cross-site request").  The same-site test is the app's own
//!     navigationpolicy.cpp approximation (exact, parent/child, or
//!     last-two-labels base compare — there is no PSL in the crate).
//!   * result order follows RFC6265 §5.4: longer paths first,
//!     earlier creation breaks ties.
//!
//! Storage policy:
//!
//!   * session cookies (expires NULL) are swept at open() — a
//!     persistent db can only have inherited them from a dead run.
//!     purge_session() is the explicit exit-sweep seam.
//!   * `set` on a record whose expiry already passed deletes the row
//!     — the Set-Cookie protocol's eviction rule (RFC6265 §5.3).
//!   * replace preserves the original `created` stamp (RFC6265
//!     §5.3 step 11.3) so send-order stays stable.
//!   * same WAL/busy_timeout pragmas as history.db — two processes
//!     may share a profile dir.
//!
//! The `gate` op is the RC06 interplay: it reads the sibling
//! sitedecisions.json's "cookie" kind (block/allow/session host
//! rules — the file the C++ CookieJar exceptions already live in)
//! and folds in the accept/third-party/keep/session-length inputs to
//! answer "may this attempt proceed, and with what rewrites" — the
//! same verdict the IO-thread cookie filter gives Chromium, exposed
//! to backends whose jar this store is.
//!
//! Interchange with C++ is the QNetworkCookie::toRawForm shape —
//! `name=value; secure; HttpOnly; SameSite=X; expires=<Netscape
//! date>; domain=<d>; path=<p>` one line per cookie — what the
//! vendored NetworkCookieJar saveState streams and what a mirror
//! export produces.  rc_cookies_import reads a file of those lines;
//! rc_cookies_export emits them (domain re-derives the leading dot
//! from host_only, so a C++->rust->C++ round trip preserves reach).
//! testdata/cookies/export.txt was written by a real Qt 6.12 build
//! (tools/gen_cookie_export.cpp).

use std::path::{Path, PathBuf};
use std::sync::Mutex;

use rusqlite::{params, Connection};
use serde_json::{json, Value};

use crate::error::{fail, Fail, RcResult, RcStatus};
use crate::omnibox;
use crate::store;

pub const DB_FILE: &str = "cookies.db";
pub const SCHEMA: &str = "1";

/// The declared role — persisted in the meta table so a db reports
/// what it last served (see the module doc for the two-mode contract).
pub const ROLE_AUTHORITATIVE: i64 = 0;
pub const ROLE_MIRROR: i64 = 1;

/// QNetworkCookie::SameSite ordinals (verified against Qt 6.12).
pub const SAMESITE_DEFAULT: i64 = 0;
pub const SAMESITE_NONE: i64 = 1;
pub const SAMESITE_LAX: i64 = 2;
pub const SAMESITE_STRICT: i64 = 3;

/// CookieJar::AcceptPolicy ordinals (cookiejar.h).
pub const ACCEPT_ALWAYS: i64 = 0;
pub const ACCEPT_NEVER: i64 = 1;
pub const ACCEPT_FIRST_PARTY: i64 = 2;

/// CookieJar::KeepPolicy ordinals.
pub const KEEP_UNTIL_EXPIRE: i64 = 0;
#[allow(dead_code)]
pub const KEEP_UNTIL_EXIT: i64 = 1;
pub const KEEP_UNTIL_TIME_LIMIT: i64 = 2;

// Bounds — generous enough for real cookies (chunked JWTs, long SSO
// payloads), small enough that a corrupt input cannot allocate
// absurdly.  Path mirrors the C++ jar's maxCookiePathLength.
const MAX_NAME: usize = 8192;
const MAX_VALUE: usize = 8192;
const MAX_DOMAIN: usize = 253;
const MAX_PATH: usize = 1024;
const MAX_CONTAINER: usize = 64;
const MAX_ROWS: i64 = 200_000;
const MAX_IMPORT_BYTES: u64 = 32 * 1024 * 1024;
const MAX_IMPORT_LINES: usize = 1_000_000;

fn now_ms() -> i64 {
    std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as i64)
        .unwrap_or(0)
}

fn sql_fail(ctx: &str, e: rusqlite::Error) -> Fail {
    Fail {
        status: RcStatus::Io,
        msg: format!("{ctx}: {e}"),
    }
}

// ---- record ---------------------------------------------------------------

/// One stored cookie — the field list the spec fixes: every column
/// SiteDecisionStore policy or a send-match can key on.
#[derive(Clone, Debug, PartialEq)]
pub struct Cookie {
    /// Container partition — "" = the default, unlabeled context;
    /// ContainerManager's uuid-hex ids otherwise.
    pub container: String,
    pub name: String,
    pub value: String,
    /// Normalized: lowercase, no leading dot, no trailing dot.
    pub domain: String,
    pub path: String,
    /// ms-epoch; None = session cookie (dies at exit).
    pub expires: Option<i64>,
    pub secure: bool,
    pub http_only: bool,
    /// QNetworkCookie::SameSite ordinal (SAMESITE_* above).
    pub same_site: i64,
    /// Qt-parity: true when the cookie must match the host exactly
    /// (Set-Cookie with no Domain attribute, or a no-dot Domain).
    pub host_only: bool,
    /// ms-epoch insert stamp — the RFC6265 §5.4 ordering tiebreak.
    pub created: i64,
}

impl Default for Cookie {
    fn default() -> Self {
        Cookie {
            container: String::new(),
            name: String::new(),
            value: String::new(),
            domain: String::new(),
            path: "/".to_string(),
            expires: None,
            secure: false,
            http_only: false,
            same_site: SAMESITE_DEFAULT,
            host_only: false,
            created: 0,
        }
    }
}

fn check_container(container: &str) -> RcResult<()> {
    if container.len() > MAX_CONTAINER
        || container
            .chars()
            .any(|c| (c as u32) < 0x20 || c as u32 == 0x7f)
    {
        return fail(RcStatus::InvalidArgument, "invalid container id");
    }
    Ok(())
}

fn normalize_domain(domain: &str) -> RcResult<String> {
    let d = domain
        .trim()
        .trim_start_matches('.')
        .trim_end_matches('.')
        .to_lowercase();
    if d.is_empty() || d.len() > MAX_DOMAIN {
        return fail(RcStatus::InvalidArgument, "invalid cookie domain");
    }
    Ok(d)
}

fn check_name(name: &str) -> RcResult<()> {
    if name.is_empty()
        || name.len() > MAX_NAME
        || name.bytes().any(|b| b < 0x21 || b == b';' || b == b'=' || b == 0x7f)
    {
        return fail(RcStatus::InvalidArgument, "invalid cookie name");
    }
    Ok(())
}

fn check_value(value: &str) -> RcResult<()> {
    if value.len() > MAX_VALUE || value.bytes().any(|b| b < 0x20 && b != b'\t') {
        return fail(RcStatus::InvalidArgument, "invalid cookie value");
    }
    Ok(())
}

fn normalize_path(path: &str) -> RcResult<String> {
    if path.is_empty() {
        return Ok("/".to_string());
    }
    if !path.starts_with('/') || path.len() > MAX_PATH {
        return fail(RcStatus::InvalidArgument, "invalid cookie path");
    }
    Ok(path.to_string())
}

impl Cookie {
    /// The JSON record the FFI carries:
    /// {"container":"","name":"","value":"","domain":"","path":"/",
    ///   "expires":ms|null,"secure":b,"httpOnly":b,"sameSite":n,
    ///   "hostOnly":b,"created":ms}
    pub fn to_json(&self) -> Value {
        json!({
            "container": self.container,
            "name": self.name,
            "value": self.value,
            "domain": self.domain,
            "path": self.path,
            "expires": self.expires,
            "secure": self.secure,
            "httpOnly": self.http_only,
            "sameSite": self.same_site,
            "hostOnly": self.host_only,
            "created": self.created,
        })
    }

    /// Inverse of to_json — validation identical to a live store write.
    pub fn from_json(v: &Value) -> RcResult<Cookie> {
        let obj = v.as_object().ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "cookie record not an object".into(),
        })?;
        let s = |key: &str| -> RcResult<String> {
            match obj.get(key) {
                None | Some(Value::Null) => Ok(String::new()),
                Some(Value::String(s)) => Ok(s.clone()),
                _ => fail(
                    RcStatus::InvalidArgument,
                    format!("cookie '{key}' not a string"),
                ),
            }
        };
        let b = |key: &str, dflt: bool| -> RcResult<bool> {
            match obj.get(key) {
                None | Some(Value::Null) => Ok(dflt),
                Some(Value::Bool(b)) => Ok(*b),
                _ => fail(
                    RcStatus::InvalidArgument,
                    format!("cookie '{key}' not a bool"),
                ),
            }
        };
        let i = |key: &str, dflt: i64| -> RcResult<i64> {
            match obj.get(key) {
                None | Some(Value::Null) => Ok(dflt),
                Some(v) => v.as_i64().ok_or_else(|| Fail {
                    status: RcStatus::InvalidArgument,
                    msg: format!("cookie '{key}' not an int"),
                }),
            }
        };
        let expires = match obj.get("expires") {
            None | Some(Value::Null) => None,
            Some(v) => Some(v.as_i64().ok_or_else(|| Fail {
                status: RcStatus::InvalidArgument,
                msg: "cookie 'expires' not an int".into(),
            })?),
        };
        let mut c = Cookie {
            container: s("container")?,
            name: s("name")?,
            value: s("value")?,
            domain: normalize_domain(&s("domain")?)?,
            path: normalize_path(&s("path")?)?,
            expires,
            secure: b("secure", false)?,
            http_only: b("httpOnly", false)?,
            same_site: i("sameSite", SAMESITE_DEFAULT)?,
            host_only: b("hostOnly", false)?,
            created: i("created", 0)?,
        };
        check_container(&c.container)?;
        check_name(&c.name)?;
        check_value(&c.value)?;
        if !(SAMESITE_DEFAULT..=SAMESITE_STRICT).contains(&c.same_site) {
            return fail(RcStatus::InvalidArgument, "invalid sameSite");
        }
        // A domain written with an explicit leading dot in JSON is the
        // same domain-cookie the Set-Cookie parser produces.
        if c.domain.is_empty() {
            return fail(RcStatus::InvalidArgument, "cookie domain required");
        }
        if c.created < 0 {
            c.created = 0;
        }
        Ok(c)
    }
}

// ---- url helpers ----------------------------------------------------------

/// The request triple for_url/store_header operate on — enough of
/// QUrl for cookie matching (host/path/scheme; the port is not part
/// of the cookie model).
#[derive(Clone, Debug)]
pub struct UrlParts {
    pub scheme: String, // lowercased; "" when absent
    pub host: String,   // normalized like a cookie domain
    pub path: String,   // "/"-rooted (default "/")
}

pub fn url_parts(url: &str) -> Option<UrlParts> {
    let scheme_end = url.find("://")?;
    let scheme = url[..scheme_end].to_lowercase();
    let host = omnibox::url_host(url).to_lowercase();
    let host = host.trim_end_matches('.').to_string();
    if host.is_empty() {
        return None;
    }
    let rest = &url[scheme_end + 3..];
    let aend = rest.find(['/', '?', '#']).unwrap_or(rest.len());
    let tail = &rest[aend..];
    let path_end = tail.find(['?', '#']).unwrap_or(tail.len());
    let path = &tail[..path_end];
    let path = if path.is_empty() { "/" } else { path };
    Some(UrlParts {
        scheme,
        host,
        path: path.to_string(),
    })
}

/// navigationpolicy.cpp's sameSite() verbatim — same-site as the
/// app already computes it (exact, parent/child, or last-two-labels
/// base compare; the approximation is documented in both places).
fn same_site(a: &str, b: &str) -> bool {
    if a.is_empty() || b.is_empty() {
        return a == b;
    }
    let a = a.to_lowercase();
    let b = b.to_lowercase();
    if a == b
        || a.ends_with(&format!(".{b}"))
        || b.ends_with(&format!(".{a}"))
    {
        return true;
    }
    let base = |h: &str| -> String {
        h.rsplit('.')
            .take(2)
            .collect::<Vec<_>>()
            .into_iter()
            .rev()
            .collect::<Vec<_>>()
            .join(".")
    };
    base(&a) == base(&b)
}

/// RFC6265 §5.1.4 domain-match: a domain cookie covers the host and
/// its subdomains; a host_only cookie covers the host alone.
fn domain_match(cookie: &Cookie, host: &str) -> bool {
    if cookie.domain == host {
        return true;
    }
    !cookie.host_only && host.ends_with(&format!(".{}", cookie.domain))
}

/// RFC6265 §5.1.4 path-match — boundary-aware (the C++ jar's
/// ends-with-'/' append achieves the same shape).
fn path_match(cookie_path: &str, request_path: &str) -> bool {
    if cookie_path == request_path {
        return true;
    }
    if request_path.starts_with(cookie_path) {
        return cookie_path.ends_with('/')
            || request_path.as_bytes().get(cookie_path.len()) == Some(&b'/');
    }
    false
}

fn scheme_is_secure(scheme: &str) -> bool {
    scheme == "https" || scheme == "wss"
}

// ---- the store ---------------------------------------------------------------

pub struct CookieStore {
    conn: Option<Connection>,
    path: Option<PathBuf>,
}

static COOKIES: Mutex<CookieStore> = Mutex::new(CookieStore {
    conn: None,
    path: None,
});

/// Runs `f` against the shared cookie store, surviving a poisoned
/// mutex — the history store's `with` contract.
pub fn with<R>(f: impl FnOnce(&mut CookieStore) -> R) -> R {
    let mut guard = COOKIES.lock().unwrap_or_else(|e| e.into_inner());
    f(&mut guard)
}

fn read_row(row: &rusqlite::Row) -> rusqlite::Result<Cookie> {
    Ok(Cookie {
        container: row.get(0)?,
        domain: row.get(1)?,
        name: row.get(2)?,
        value: row.get(3)?,
        path: row.get(4)?,
        expires: row.get(5)?,
        secure: row.get::<_, i64>(6)? != 0,
        http_only: row.get::<_, i64>(7)? != 0,
        same_site: row.get(8)?,
        host_only: row.get::<_, i64>(9)? != 0,
        created: row.get(10)?,
    })
}

const SELECT: &str =
    "SELECT container, domain, name, value, path, expires, secure,
            httponly, samesite, host_only, created FROM cookies";

fn meta_get(conn: &Connection, key: &str) -> RcResult<Option<String>> {
    conn.query_row("SELECT value FROM meta WHERE key=?1", params![key], |r| {
        r.get(0)
    })
    .map(Some)
    .or_else(|e| match e {
        rusqlite::Error::QueryReturnedNoRows => Ok(None),
        e => Err(sql_fail("meta read", e)),
    })
}

impl CookieStore {
    /// Opens (or reuses) the database at `path`; `None` resolves to
    /// <data dir>/cookies.db.  Reopening the same path is a no-op —
    /// the facade's per-op re-assert rides that.
    ///
    /// Session cookies are swept at open: a persistent db can only
    /// have inherited them from a dead run (same caveat history.db
    /// documents for a second concurrent process — a peer's live
    /// session rows would go too).
    pub fn open(&mut self, path: Option<&str>) -> RcResult<()> {
        let path = match path {
            Some(p) => PathBuf::from(p),
            None => store::lock().dir()?.join(DB_FILE),
        };
        if self.conn.is_some() && self.path.as_deref() == Some(path.as_path()) {
            return Ok(());
        }
        if let Some(dir) = path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| Fail {
                status: RcStatus::Io,
                msg: format!("cannot create {}: {e}", dir.display()),
            })?;
        }
        let conn = Connection::open(&path).map_err(|e| Fail {
            status: RcStatus::Io,
            msg: format!("cannot open {}: {e}", path.display()),
        })?;
        conn.execute_batch(
            "PRAGMA journal_mode = WAL;
             PRAGMA synchronous = NORMAL;
             PRAGMA busy_timeout = 5000;
             CREATE TABLE IF NOT EXISTS meta(
                 key TEXT PRIMARY KEY,
                 value TEXT NOT NULL);
             CREATE TABLE IF NOT EXISTS cookies(
                 container TEXT NOT NULL,
                 domain TEXT NOT NULL,
                 name TEXT NOT NULL,
                 value TEXT NOT NULL,
                 path TEXT NOT NULL,
                 expires INTEGER,
                 secure INTEGER NOT NULL DEFAULT 0,
                 httponly INTEGER NOT NULL DEFAULT 0,
                 samesite INTEGER NOT NULL DEFAULT 0,
                 host_only INTEGER NOT NULL DEFAULT 0,
                 created INTEGER NOT NULL DEFAULT 0,
                 PRIMARY KEY(container, domain, name, path));
             CREATE INDEX IF NOT EXISTS cookies_domain
                 ON cookies(domain);",
        )
        .map_err(|e| sql_fail("schema", e))?;
        if meta_get(&conn, "schema")?.is_none() {
            conn.execute(
                "INSERT INTO meta(key, value) VALUES('schema', ?1)",
                params![SCHEMA],
            )
            .map_err(|e| sql_fail("meta write", e))?;
        }
        // Dead-run residue: session rows can never belong to a live
        // session — the process that set them exited.
        conn.execute("DELETE FROM cookies WHERE expires IS NULL", [])
            .map_err(|e| sql_fail("session sweep", e))?;
        self.conn = Some(conn);
        self.path = Some(path);
        Ok(())
    }

    /// The dir the open db lives in — the sitedecisions.json sibling
    /// the gate consults.
    pub fn dir(&self) -> Option<PathBuf> {
        self.path.as_ref().and_then(|p| p.parent().map(|d| d.to_path_buf()))
    }

    fn conn(&self) -> RcResult<&Connection> {
        self.conn.as_ref().ok_or_else(|| Fail {
            status: RcStatus::NotInitialized,
            msg: "rc_cookies_open() has not been called".into(),
        })
    }

    /// True while a db is open — the FFI's cheap probe.
    pub fn is_open(&self) -> bool {
        self.conn.is_some()
    }

    /// The declared mode — ROLE_AUTHORITATIVE | ROLE_MIRROR, read
    /// from meta (default authoritative for a fresh db).  Advisory:
    /// the ops are identical either way; the flag is how a mirror
    /// honestly reports that an engine owns the real jar.
    pub fn role(&self) -> RcResult<i64> {
        match meta_get(self.conn()?, "role")? {
            Some(v) => v
                .trim()
                .parse::<i64>()
                .map_err(|_| Fail {
                    status: RcStatus::Corrupt,
                    msg: "meta.role is not an int".into(),
                }),
            None => Ok(ROLE_AUTHORITATIVE),
        }
    }

    pub fn set_role(&mut self, role: i64) -> RcResult<()> {
        if role != ROLE_AUTHORITATIVE && role != ROLE_MIRROR {
            return fail(RcStatus::InvalidArgument, "unknown cookie role");
        }
        self.conn()?
            .execute(
                "INSERT INTO meta(key, value) VALUES('role', ?1)
                 ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                params![role.to_string()],
            )
            .map_err(|e| sql_fail("meta write", e))?;
        Ok(())
    }

    /// Upserts a cookie — the live-store op.  An already-past expiry
    /// deletes the row (RFC6265 §5.3 eviction); `created` is preserved
    /// across a replace so send order stays stable.  Reports whether
    /// the row set was written, deleted, or unchanged.
    pub fn set(&mut self, c: &Cookie) -> RcResult<SetOutcome> {
        check_container(&c.container)?;
        check_name(&c.name)?;
        check_value(&c.value)?;
        if c.domain.is_empty() || c.domain.len() > MAX_DOMAIN {
            return fail(RcStatus::InvalidArgument, "invalid cookie domain");
        }
        if c.path.is_empty() || c.path.len() > MAX_PATH || !c.path.starts_with('/') {
            return fail(RcStatus::InvalidArgument, "invalid cookie path");
        }
        if let Some(exp) = c.expires {
            if exp <= now_ms() {
                let n = self
                    .conn()?
                    .execute(
                        "DELETE FROM cookies WHERE container=?1 AND domain=?2
                         AND name=?3 AND path=?4",
                        params![c.container, c.domain, c.name, c.path],
                    )
                    .map_err(|e| sql_fail("delete", e))?;
                return Ok(if n > 0 {
                    SetOutcome::Deleted
                } else {
                    SetOutcome::Noop
                });
            }
        }
        if self.count(None)? >= MAX_ROWS {
            return fail(RcStatus::InvalidArgument, "cookie store is full");
        }
        let conn = self.conn()?;
        // Keep the original creation stamp on replace (RFC6265 §5.3
        // step 11.3) — creation order drives the sort tiebreak.
        let prior: Option<i64> = conn
            .query_row(
                "SELECT created FROM cookies WHERE container=?1 AND domain=?2
                 AND name=?3 AND path=?4",
                params![c.container, c.domain, c.name, c.path],
                |r| r.get(0),
            )
            .ok();
        let created = if c.created > 0 {
            c.created
        } else {
            prior.unwrap_or_else(now_ms)
        };
        let changed = conn
            .query_row(
                "SELECT value, expires, secure, httponly, samesite, host_only
                 FROM cookies WHERE container=?1 AND domain=?2 AND name=?3
                 AND path=?4",
                params![c.container, c.domain, c.name, c.path],
                |r| {
                    Ok((
                        r.get::<_, String>(0)?,
                        r.get::<_, Option<i64>>(1)?,
                        r.get::<_, i64>(2)?,
                        r.get::<_, i64>(3)?,
                        r.get::<_, i64>(4)?,
                        r.get::<_, i64>(5)?,
                    ))
                },
            )
            .ok()
            .map(|(v, e, s, h, ss, ho)| {
                v != c.value
                    || e != c.expires
                    || (s != 0) != c.secure
                    || (h != 0) != c.http_only
                    || ss != c.same_site
                    || (ho != 0) != c.host_only
            })
            .unwrap_or(true);
        if !changed {
            return Ok(SetOutcome::Noop);
        }
        conn.execute(
            "INSERT INTO cookies(container, domain, name, value, path,
                                expires, secure, httponly, samesite,
                                host_only, created)
             VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)
             ON CONFLICT(container, domain, name, path) DO UPDATE SET
                 value=excluded.value, expires=excluded.expires,
                 secure=excluded.secure, httponly=excluded.httponly,
                 samesite=excluded.samesite,
                 host_only=excluded.host_only,
                 created=excluded.created",
            params![
                c.container,
                c.domain,
                c.name,
                c.value,
                c.path,
                c.expires,
                c.secure as i64,
                c.http_only as i64,
                c.same_site,
                c.host_only as i64,
                created,
            ],
        )
        .map_err(|e| sql_fail("upsert", e))?;
        Ok(SetOutcome::Stored)
    }

    /// Direct insert — the import/restore lane: rows land verbatim
    /// (an expired export row stays until a sweep drops it, which is
    /// what restoring the C++ state file would do).
    fn insert_raw(&mut self, c: &Cookie) -> RcResult<()> {
        self.conn()?
            .execute(
                "INSERT INTO cookies(container, domain, name, value, path,
                                    expires, secure, httponly, samesite,
                                    host_only, created)
                 VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11)
                 ON CONFLICT(container, domain, name, path) DO UPDATE SET
                     value=excluded.value, expires=excluded.expires,
                     secure=excluded.secure, httponly=excluded.httponly,
                     samesite=excluded.samesite,
                     host_only=excluded.host_only,
                     created=excluded.created",
                params![
                    c.container,
                    c.domain,
                    c.name,
                    c.value,
                    c.path,
                    c.expires,
                    c.secure as i64,
                    c.http_only as i64,
                    c.same_site,
                    c.host_only as i64,
                    c.created,
                ],
            )
            .map_err(|e| sql_fail("insert", e))?;
        Ok(())
    }

    /// One row by key — RC_NOT_FOUND when absent.
    pub fn get(&mut self, container: &str, domain: &str, name: &str, path: &str)
        -> RcResult<Cookie>
    {
        check_container(container)?;
        check_name(name)?;
        let domain = normalize_domain(domain)?;
        let path = normalize_path(path)?;
        self.conn()?
            .query_row(
                &format!("{SELECT} WHERE container=?1 AND domain=?2
                          AND name=?3 AND path=?4"),
                params![container, domain, name, path],
                read_row,
            )
            .map_err(|e| match e {
                rusqlite::Error::QueryReturnedNoRows => Fail {
                    status: RcStatus::NotFound,
                    msg: "no such cookie".into(),
                },
                e => sql_fail("get", e),
            })
    }

    /// Deletes one row by key — reports whether it existed.
    pub fn remove(&mut self, container: &str, domain: &str, name: &str, path: &str)
        -> RcResult<bool>
    {
        check_container(container)?;
        check_name(name)?;
        let domain = normalize_domain(domain)?;
        let path = normalize_path(path)?;
        Ok(self.conn()?
            .execute(
                "DELETE FROM cookies WHERE container=?1 AND domain=?2
                 AND name=?3 AND path=?4",
                params![container, domain, name, path],
            )
            .map_err(|e| sql_fail("delete", e))?
            > 0)
    }

    /// The shield panel's "clear site data" sweep
    /// (CookieJar::removeCookiesForHost verbatim): drops rows scoped
    /// to the host or a subdomain it controls, plus rows on a parent
    /// domain the site still receives.  `host` "" clears nothing.
    pub fn remove_for_host(&mut self, container: &str, host: &str) -> RcResult<u64> {
        check_container(container)?;
        if host.is_empty() {
            return Ok(0); // C++ removeCookiesForHost early-out
        }
        let host = normalize_domain(host)?;
        let rows: Vec<(String, String, String)> = {
            let conn = self.conn()?;
            let mut st = conn
                .prepare(&format!("{SELECT} WHERE container=?1"))
                .map_err(|e| sql_fail("scan", e))?;
            let it = st
                .query_map(params![container], |r| {
                    Ok((r.get(1)?, r.get(2)?, r.get(4)?))
                })
                .map_err(|e| sql_fail("scan", e))?;
            it.collect::<rusqlite::Result<Vec<_>>>()
                .map_err(|e| sql_fail("scan", e))?
        };
        let mut removed = 0u64;
        let conn = self.conn()?;
        for (domain, name, path) in rows {
            let under = domain == host || domain.ends_with(&format!(".{host}"));
            let covers = host == domain || host.ends_with(&format!(".{domain}"));
            if under || covers {
                removed += conn
                    .execute(
                        "DELETE FROM cookies WHERE container=?1 AND domain=?2
                         AND name=?3 AND path=?4",
                        params![container, domain, name, path],
                    )
                    .map_err(|e| sql_fail("delete", e))? as u64;
            }
        }
        Ok(removed)
    }

    /// Wipes a partition — `container` None wipes the whole db.
    pub fn clear(&mut self, container: Option<&str>) -> RcResult<u64> {
        let conn = self.conn()?;
        match container {
            Some(c) => {
                check_container(c)?;
                Ok(conn
                    .execute(
                        "DELETE FROM cookies WHERE container=?1",
                        params![c],
                    )
                    .map_err(|e| sql_fail("clear", e))? as u64)
            }
            None => Ok(conn
                .execute("DELETE FROM cookies", [])
                .map_err(|e| sql_fail("clear", e))? as u64),
        }
    }

    /// Every row of a partition (None = all), sorted for a
    /// deterministic listing — domain, path, name.
    pub fn list(&mut self, container: Option<&str>) -> RcResult<Vec<Cookie>> {
        let conn = self.conn()?;
        let mut out = Vec::new();
        let sql = match container {
            Some(_) => format!("{SELECT} WHERE container=?1
                                ORDER BY domain, path, name"),
            None => format!("{SELECT} ORDER BY container, domain, path, name"),
        };
        let mut st = conn.prepare(&sql).map_err(|e| sql_fail("list", e))?;
        let rows = match container {
            Some(c) => st.query_map(params![c], read_row),
            None => st.query_map([], read_row),
        }
        .map_err(|e| sql_fail("list", e))?;
        for r in rows {
            out.push(r.map_err(|e| sql_fail("list row", e))?);
        }
        Ok(out)
    }

    /// Row count — a partition, or the whole db on None.
    pub fn count(&mut self, container: Option<&str>) -> RcResult<i64> {
        let conn = self.conn()?;
        match container {
            Some(c) => conn
                .query_row(
                    "SELECT COUNT(*) FROM cookies WHERE container=?1",
                    params![c],
                    |r| r.get(0),
                )
                .map_err(|e| sql_fail("count", e)),
            None => conn
                .query_row("SELECT COUNT(*) FROM cookies", [], |r| r.get(0))
                .map_err(|e| sql_fail("count", e)),
        }
    }

    /// Deletes rows whose expiry is at or before `now`; absent session
    /// rows are untouched (purge_session is their sweep).
    pub fn purge_expired(&mut self, now: i64) -> RcResult<u64> {
        Ok(self.conn()?
            .execute(
                "DELETE FROM cookies WHERE expires IS NOT NULL AND expires <= ?1",
                params![now],
            )
            .map_err(|e| sql_fail("purge", e))? as u64)
    }

    /// The exit-sweep: drops every session row (the C++ jar's
    /// endSession equivalent).
    pub fn purge_session(&mut self) -> RcResult<u64> {
        Ok(self.conn()?
            .execute("DELETE FROM cookies WHERE expires IS NULL", [])
            .map_err(|e| sql_fail("purge session", e))? as u64)
    }

    /// The send-match — which cookies ride `request`.  Mirrors the
    /// vendored jar's cookiesForUrl plus the SameSite send rule:
    /// host-only/domain reach, boundary-aware paths, secure gating,
    /// expiry (lazy-deleted like the C++ jar), and the optional
    /// first-party SameSite filter.  Ordered per RFC6265 §5.4.
    pub fn for_url(&mut self, req: &ForUrl) -> RcResult<Vec<Cookie>> {
        check_container(&req.container)?;
        let now = if req.now_ms > 0 { req.now_ms } else { now_ms() };
        let conn = self.conn()?;
        // Coarse select inside the partition; the exact rules run in
        // Rust below (host_only + path boundary are awkward in SQL).
        let mut st = conn
            .prepare(&format!("{SELECT} WHERE container=?1"))
            .map_err(|e| sql_fail("for_url", e))?;
        let rows = st
            .query_map(params![req.container], read_row)
            .map_err(|e| sql_fail("for_url", e))?;
        let mut out = Vec::new();
        let mut expired: Vec<(String, String, String)> = Vec::new();
        for r in rows {
            let c = r.map_err(|e| sql_fail("for_url row", e))?;
            if !domain_match(&c, &req.host) {
                continue;
            }
            if !path_match(&c.path, &req.path) {
                continue;
            }
            if c.secure && !req.secure {
                continue;
            }
            if let Some(exp) = c.expires {
                if exp <= now {
                    expired.push((c.domain.clone(), c.name.clone(), c.path.clone()));
                    continue;
                }
            }
            if let Some(fp) = &req.first_party {
                if !same_site(fp, &req.host)
                    && (c.same_site == SAMESITE_LAX || c.same_site == SAMESITE_STRICT)
                {
                    continue;
                }
            }
            out.push(c);
        }
        for (domain, name, path) in expired {
            let _ = conn.execute(
                "DELETE FROM cookies WHERE container=?1 AND domain=?2
                 AND name=?3 AND path=?4",
                params![req.container, domain, name, path],
            );
        }
        out.sort_by(|a, b| {
            b.path
                .len()
                .cmp(&a.path.len())
                .then(a.created.cmp(&b.created))
        });
        Ok(out)
    }

    /// Parses + stores one Set-Cookie line against `url`
    /// (rc_cookies_store_header; the authoritative backend's ingest).
    /// The verdict JSON names the outcome so the caller can log it.
    pub fn store_header(
        &mut self,
        container: &str,
        url: &str,
        header: &str,
    ) -> RcResult<Value> {
        let parts = match url_parts(url) {
            Some(p) => p,
            None => {
                return Ok(json!({"stored": false, "reason": "bad url"}))
            }
        };
        match parse_set_cookie(header, Some(&parts)) {
            Ok(mut c) => {
                c.container = container.to_string();
                match self.set(&c) {
                    Ok(SetOutcome::Deleted) => {
                        Ok(json!({"stored": true, "reason": "deleted"}))
                    }
                    Ok(_) => Ok(json!({"stored": true, "reason": "stored"})),
                    Err(e) => Ok(json!({"stored": false, "reason": e.msg})),
                }
            }
            Err(e) => Ok(json!({"stored": false, "reason": e.msg})),
        }
    }

    /// toRawForm lines out — the mirror's export and the interchange
    /// shape with C++.  `domain` re-derives the leading dot from
    /// host_only so a round trip preserves reach; emits nothing for a
    /// partition with no rows.
    pub fn export_text(&mut self, container: Option<&str>) -> RcResult<String> {
        let mut out = String::new();
        for c in self.list(container)? {
            out.push_str(&to_raw_form(&c));
            out.push('\n');
        }
        Ok(out)
    }

    /// The import lane: one toRawForm line per row of `text`
    /// (testdata/cookies/export.txt shape).  Lines that fail the
    /// Set-Cookie parse are skipped and counted — a hostile export
    /// degrades row-by-row, never whole-file.  Rows land in
    /// `container` ("" = default).
    pub fn import_text(&mut self, container: &str, text: &str) -> RcResult<Value> {
        check_container(container)?;
        if text.len() as u64 > MAX_IMPORT_BYTES {
            return fail(RcStatus::InvalidArgument, "import input over cap");
        }
        let mut imported = 0usize;
        let mut skipped = 0usize;
        for (i, line) in text.lines().enumerate() {
            if i >= MAX_IMPORT_LINES {
                break;
            }
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            match parse_set_cookie(line, None) {
                Ok(mut c) => {
                    c.container = container.to_string();
                    c.created = now_ms();
                    self.insert_raw(&c)?;
                    imported += 1;
                }
                Err(_) => skipped += 1,
            }
        }
        Ok(json!({"imported": imported, "skipped": skipped}))
    }
}

/// What `set` reports — the FFI surface maps these onto notify
/// decisions (Noop fires no callback).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum SetOutcome {
    Stored,
    Deleted,
    Noop,
}

/// The for_url request: url-derived host/path/secure plus the
/// container partition and an optional first-party host for the
/// SameSite send rule.  `now_ms` 0 = system clock (tests pin it).
#[derive(Clone, Debug)]
pub struct ForUrl {
    pub container: String,
    pub host: String,
    pub path: String,
    pub secure: bool,
    /// Normalized first-party host — Some(_) enables the SameSite
    /// cross-site filter, None answers the "any context" set.
    pub first_party: Option<String>,
    pub now_ms: i64,
}

impl ForUrl {
    /// Builds a request from the JSON the FFI carries:
    /// {"url":"https://h/p","container":"","first_party":"h",
    ///   "now_ms":0}
    pub fn from_json(v: &Value) -> RcResult<ForUrl> {
        let url = v.get("url").and_then(Value::as_str).unwrap_or("");
        let parts = url_parts(url).ok_or_else(|| Fail {
            status: RcStatus::InvalidArgument,
            msg: "cookies_for_url: bad url".into(),
        })?;
        let container = v
            .get("container")
            .and_then(Value::as_str)
            .unwrap_or("")
            .to_string();
        let first_party = match v.get("first_party") {
            None | Some(Value::Null) => None,
            Some(Value::String(s)) => {
                // Accept either a bare host or a full url.
                let host = if s.contains("://") {
                    omnibox::url_host(s).to_string()
                } else {
                    s.clone()
                };
                Some(
                    host.trim_end_matches('.')
                        .trim_start_matches('.')
                        .to_lowercase(),
                )
            }
            _ => {
                return fail(
                    RcStatus::InvalidArgument,
                    "cookies_for_url: bad first_party",
                )
            }
        };
        let now_ms = v.get("now_ms").and_then(Value::as_i64).unwrap_or(0);
        Ok(ForUrl {
            container,
            host: parts.host,
            path: parts.path,
            secure: scheme_is_secure(&parts.scheme),
            first_party,
            now_ms,
        })
    }
}

// ---- the cookie gate (RC06 interplay) --------------------------------------
//
// One verdict a backend applies before storing: which exception list
// the host hits in the sibling sitedecisions.json "cookie" kind, what
// the accept policy then decides, and which rewrites the keep/session
// rules demand.  Same precedence CookieJar::ruleForHost + the
// IO-thread filter resolve — block > allow > session > policy.

/// CookieJar::isOnDomainList ported: a rule with a leading dot covers
/// the host's tail; without one it must equal the host or a dotted
/// suffix.  Rules normalize like stored keys (lowercase).
fn on_domain_list(rules: &[String], domain: &str) -> bool {
    for rule in rules {
        if let Some(dotted) = rule.strip_prefix('.') {
            if domain.ends_with(rule) || domain == dotted {
                return true;
            }
        } else if domain.ends_with(&format!(".{rule}")) || domain == rule {
            return true;
        }
    }
    false
}

/// Reads <dir>/sitedecisions.json's "cookie" kind into the three
/// exception lists (block/allow/session) — the file layout
/// sitedecisions.rs owns, consumed read-only here so the gate works
/// on a bound profile dir without re-anchoring the process store.
/// A missing or malformed file yields empty lists (no exceptions),
/// matching that store's own absent-is-empty contract.
fn cookie_exceptions(dir: &Path) -> (Vec<String>, Vec<String>, Vec<String>) {
    let mut block = Vec::new();
    let mut allow = Vec::new();
    let mut session = Vec::new();
    let bytes = match std::fs::read(dir.join("sitedecisions.json")) {
        Ok(b) => b,
        Err(_) => return (block, allow, session),
    };
    let doc: Value = match serde_json::from_slice(&bytes) {
        Ok(v) => v,
        Err(_) => return (block, allow, session),
    };
    if let Some(rows) = doc
        .get("kinds")
        .and_then(|k| k.get("cookie"))
        .and_then(|c| c.as_object())
    {
        for (key, value) in rows {
            let Some(value) = value.as_str() else {
                continue;
            };
            match value {
                "block" => block.push(key.clone()),
                "allow" => allow.push(key.clone()),
                "session" => session.push(key.clone()),
                _ => {}
            }
        }
    }
    (block, allow, session)
}

/// The verdict: may this cookie attempt proceed, and with what
/// rewrites.  Input JSON (the rc_cookies_gate/ac_cookies_gate body):
///
///   {"host":"www.example.com",       — required (attempt origin host)
///    "third_party":false,            — QWebEngineCookieStore flag
///    "block_3p":true,                — CookieJar's toggle
///    "accept_policy":0|1|2,          — ACCEPT_* above
///    "keep_policy":0|1|2,            — KEEP_UNTIL_* above
///    "session_length":-1,            — days, -1 = unset
///    "filter_tracking":false,        — the __utm* drop
///    "name":"pref",                  — attempt's cookie name
///    "session":false,                — attempt has no expiry
///    "expires_ms":1893456000000}     — attempt's expiry (0 = none)
///
/// Output:
///   {"accept":bool,
///    "rule":"block"|"tracking"|"third-party"|"allow"|"session"|"policy",
///    "session_only":bool,            — store with expires=NULL
///    "expires_cap_ms":ms|null}       — clamp a longer expiry to this
pub fn gate(dir: &Path, input: &Value) -> RcResult<Value> {
    let host = input
        .get("host")
        .and_then(Value::as_str)
        .unwrap_or("")
        .to_lowercase();
    if host.is_empty() {
        return fail(RcStatus::InvalidArgument, "gate: host required");
    }
    let third_party = input
        .get("third_party")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let block_3p = input
        .get("block_3p")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let accept_policy = input
        .get("accept_policy")
        .and_then(Value::as_i64)
        .unwrap_or(ACCEPT_FIRST_PARTY);
    let keep_policy = input
        .get("keep_policy")
        .and_then(Value::as_i64)
        .unwrap_or(KEEP_UNTIL_EXPIRE);
    let session_length = input
        .get("session_length")
        .and_then(Value::as_i64)
        .unwrap_or(-1);
    let filter_tracking = input
        .get("filter_tracking")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let name = input
        .get("name")
        .and_then(Value::as_str)
        .unwrap_or("");
    let attempt_session = input
        .get("session")
        .and_then(Value::as_bool)
        .unwrap_or(false);
    let expires_ms = input
        .get("expires_ms")
        .and_then(Value::as_i64)
        .unwrap_or(0);

    let (block, allow, session) = cookie_exceptions(dir);
    let blocked = on_domain_list(&block, &host);
    let allowed = !blocked && on_domain_list(&allow, &host);
    let allowed_session = !blocked && !allowed && on_domain_list(&session, &host);

    // Same precedence the IO-thread filter resolves: block first;
    // the __utm* tracking drop fires before the accept policy (the
    // C++ jar drops it post-add — here the attempt never stores).
    let (accept, rule) = if blocked {
        (false, "block")
    } else if filter_tracking && name.starts_with("__utm") {
        (false, "tracking")
    } else if third_party && block_3p && !allowed && !allowed_session {
        (false, "third-party")
    } else if allowed {
        (true, "allow")
    } else if allowed_session {
        (true, "session")
    } else {
        let ok = match accept_policy {
            ACCEPT_ALWAYS => true,
            ACCEPT_NEVER => false,
            _ => !third_party,
        };
        (ok, "policy")
    };

    // The rewrites handleCookieAdded applies post-accept: an
    // allow-for-session hit strips expiry; a session cookie under a
    // sessionLength gains one; KeepUntilTimeLimit caps far expiry at
    // 90 days.
    let mut session_only = false;
    let mut expires_cap: Option<i64> = None;
    if accept {
        let now = now_ms();
        if allowed_session && !attempt_session {
            session_only = true;
        } else if attempt_session && session_length != -1 {
            expires_cap = Some(now.saturating_add(session_length * 86_400_000));
        }
        if keep_policy == KEEP_UNTIL_TIME_LIMIT && !attempt_session && !session_only {
            let cap = now.saturating_add(90 * 86_400_000);
            if expires_ms == 0 || expires_ms > cap {
                expires_cap = Some(cap);
            }
        }
    }
    Ok(json!({
        "accept": accept,
        "rule": rule,
        "session_only": session_only,
        "expires_cap_ms": expires_cap,
    }))
}

// ---- Set-Cookie parse (RFC6265 §5) ------------------------------------------

/// RFC6265 §5.1.1 token split — delimiter bytes are %x09 / %x20-2F /
/// %x3B-40 / %x5B-60 / %x7B-7E; anything else is a token byte.
fn is_date_delim(b: u8) -> bool {
    b == 0x09
        || (0x20..=0x2f).contains(&b)
        || (0x3b..=0x40).contains(&b)
        || (0x5b..=0x60).contains(&b)
        || (0x7b..=0x7e).contains(&b)
}

const MONTHS: [&str; 12] = [
    "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct",
    "nov", "dec",
];

/// Howard Hinnant's days_from_civil — proleptic Gregorian day number
/// (days since 1970-01-01); no chrono in the dependency tree.
fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = if y >= 0 { y } else { y - 399 } / 400;
    let yoe = y - era * 400;
    let mp = ((m + 9) % 12) as i64;
    let doy = (153 * mp + 2) / 5 + d as i64 - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146097 + doe - 719468
}

/// Inverse for the export's Netscape date render.
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719468;
    let era = if z >= 0 { z } else { z - 146096 } / 146097;
    let doe = z - era * 146097;
    let yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    let y = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = if mp < 10 { mp + 3 } else { mp - 9 } as u32;
    (if m <= 2 { y + 1 } else { y }, m, d)
}

/// The "Wdy, DD-Mon-YYYY hh:mm:ss GMT" render toRawForm emits.
fn render_cookie_date(ms: i64) -> String {
    const DAYS: [&str; 7] = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"];
    const MON: [&str; 12] = [
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
        "Oct", "Nov", "Dec",
    ];
    let secs = ms.div_euclid(1000);
    let days = secs.div_euclid(86400);
    let tod = secs.rem_euclid(86400);
    let (y, m, d) = civil_from_days(days);
    // 1970-01-01 was a Thursday (index 4 in the Sun-first table).
    let wday = DAYS[((days + 4).rem_euclid(7)) as usize];
    format!(
        "{wday}, {d:02}-{mon}-{y:04} {:02}:{:02}:{:02} GMT",
        tod / 3600,
        (tod % 3600) / 60,
        tod % 60,
        mon = MON[(m - 1) as usize]
    )
}

/// RFC6265 §5.1.1 cookie-date: tokenize, then find the time, day,
/// month and year slots (any order).  2-digit years map 70-99 ->
/// 1900s, 0-69 -> 2000s.  Returns ms-epoch.
fn parse_cookie_date(s: &str) -> Option<i64> {
    let mut hour = None;
    let mut minute = None;
    let mut second = None;
    let mut day = None;
    let mut month = None;
    let mut year = None;

    let b = s.as_bytes();
    let mut i = 0;
    while i < b.len() {
        // Skip delimiters.
        while i < b.len() && is_date_delim(b[i]) {
            i += 1;
        }
        if i >= b.len() {
            break;
        }
        let start = i;
        while i < b.len() && !is_date_delim(b[i]) {
            i += 1;
        }
        let tok = &s[start..i];

        // time-of-day hh:mm:ss (digits may carry a trailing non-digit
        // per the spec's production; we parse the leading digits).
        if hour.is_none() {
            let mut parts = tok.split(':');
            if let (Some(h), Some(m), Some(sec)) =
                (parts.next(), parts.next(), parts.next())
            {
                let lead = |p: &str| -> Option<u32> {
                    let digits: String =
                        p.chars().take_while(|c| c.is_ascii_digit()).collect();
                    if digits.is_empty() {
                        return None;
                    }
                    digits.parse().ok()
                };
                if let (Some(h), Some(m), Some(sec)) =
                    (lead(h), lead(m), lead(sec))
                {
                    if h <= 23 && m <= 59 && sec <= 60 {
                        hour = Some(h);
                        minute = Some(m);
                        second = Some(sec.min(59));
                        continue;
                    }
                }
            }
        }
        // day-of-month: 1-2 digits (+ spec-allowed trailing junk).
        if day.is_none() {
            let digits: String =
                tok.chars().take_while(|c| c.is_ascii_digit()).collect();
            if (1..=2).contains(&digits.len()) {
                if let Ok(d) = digits.parse::<u32>() {
                    if (1..=31).contains(&d) {
                        day = Some(d);
                        continue;
                    }
                }
            }
        }
        // month: first three letters match a month name.
        if month.is_none() && tok.len() >= 3 {
            let low = tok[..3].to_lowercase();
            if let Some(m) = MONTHS.iter().position(|&n| n == low) {
                month = Some(m as u32 + 1);
                continue;
            }
        }
        // year: 2-4 digits.
        if year.is_none() {
            let digits: String =
                tok.chars().take_while(|c| c.is_ascii_digit()).collect();
            if (2..=4).contains(&digits.len()) {
                if let Ok(mut y) = digits.parse::<i64>() {
                    y = match y {
                        70..=99 => y + 1900,
                        0..=69 => y + 2000,
                        _ => y,
                    };
                    year = Some(y);
                    continue;
                }
            }
        }
    }

    let (h, mi, s, d, m, y) = (
        hour?, minute?, second?, day?, month?, year?,
    );
    if y < 1601 {
        return None;
    }
    let days = days_from_civil(y, m, d);
    Some(((days * 24 + h as i64) * 60 + mi as i64) * 60 * 1000 + s as i64 * 1000)
}

/// Parses one Set-Cookie / toRawForm line into a Cookie.
///
///   * `url` Some: the live-store path — a missing Domain attribute
///     lands host_only on the request host, a supplied one must
///     domain-cover the request host (a cookie may not scope itself
///     to a foreign domain), and an absent Path falls back to the
///     request's default path (RFC6265 §5.1.4).
///   * `url` None: the import path — the Domain attribute is
///     required (there is no request host to key the row on) and a
///     missing Path defaults to "/".
///
/// Qt parity: a Domain attribute WITH a leading dot is a domain
/// cookie; WITHOUT one it is host-exact (measured on Qt 6.12 — see
/// the module doc).
pub fn parse_set_cookie(header: &str, url: Option<&UrlParts>) -> RcResult<Cookie> {
    let mut it = header.split(';');
    let pair = it.next().unwrap_or("").trim();
    let eq = pair.find('=').ok_or_else(|| Fail {
        status: RcStatus::InvalidArgument,
        msg: "set-cookie: no name=value pair".into(),
    })?;
    let name = pair[..eq].trim();
    let value = pair[eq + 1..].trim();
    check_name(name)?;
    check_value(value)?;

    let mut c = Cookie {
        name: name.to_string(),
        value: value.to_string(),
        ..Cookie::default()
    };
    let mut domain_attr: Option<(String, bool)> = None; // (value, had_dot)
    let mut path_attr_seen = false;
    // RFC6265 §5.2.2: a Max-Age attribute beats expires regardless
    // of order — once seen, later expires attrs are ignored.
    let mut max_age_wins = false;
    for attr in it {
        let attr = attr.trim();
        let (key, val) = match attr.find('=') {
            Some(i) => (attr[..i].trim(), attr[i + 1..].trim()),
            None => (attr, ""),
        };
        let low = key.to_lowercase();
        match low.as_str() {
            "expires" => {
                if !max_age_wins {
                    if let Some(ms) = parse_cookie_date(val) {
                        c.expires = Some(ms);
                    }
                }
            }
            "max-age" => {
                // A negative or zero value expires immediately.
                if let Some(stripped) = val.strip_prefix('-') {
                    if !stripped.is_empty()
                        && stripped.chars().all(|ch| ch.is_ascii_digit())
                    {
                        c.expires = Some(0);
                        max_age_wins = true;
                        continue;
                    }
                }
                if !val.is_empty()
                    && val.chars().all(|ch| ch.is_ascii_digit())
                {
                    if let Ok(secs) = val.parse::<i64>() {
                        c.expires =
                            Some(now_ms().saturating_add(secs.saturating_mul(1000)));
                        max_age_wins = true;
                    }
                }
            }
            "domain" => {
                let had_dot = val.starts_with('.');
                if let Ok(d) = normalize_domain(val) {
                    domain_attr = Some((d, had_dot));
                }
            }
            "path" => {
                path_attr_seen = true;
                // A non-"/"-leading Path attr is ignored per RFC6265
                // §5.1.4 (the default path applies).
                if val.starts_with('/') {
                    if let Ok(p) = normalize_path(val) {
                        c.path = p;
                    }
                }
            }
            "secure" => c.secure = true,
            "httponly" => c.http_only = true,
            "samesite" => {
                c.same_site = match val.to_lowercase().as_str() {
                    "none" => SAMESITE_NONE,
                    "lax" => SAMESITE_LAX,
                    "strict" => SAMESITE_STRICT,
                    _ => SAMESITE_DEFAULT,
                };
            }
            _ => {}
        }
    }

    match (domain_attr, url) {
        (Some((d, had_dot)), Some(parts)) => {
            // The attr must cover the request host — a cookie may
            // never scope itself outside its origin domain.
            if parts.host != d && !parts.host.ends_with(&format!(".{d}")) {
                return fail(
                    RcStatus::InvalidArgument,
                    "set-cookie: domain attr does not cover request host",
                );
            }
            c.domain = d;
            c.host_only = !had_dot;
        }
        (Some((d, had_dot)), None) => {
            c.domain = d;
            c.host_only = !had_dot;
        }
        (None, Some(parts)) => {
            c.domain = parts.host.clone();
            c.host_only = true;
        }
        (None, None) => {
            return fail(
                RcStatus::InvalidArgument,
                "set-cookie: no domain and no request host",
            );
        }
    }

    if let Some(parts) = url {
        if !path_attr_seen {
            // RFC6265 §5.1.4 default-path: the uri-path up to (not
            // including) its last '/'.
            c.path = default_path(&parts.path);
        }
    }
    Ok(c)
}

/// RFC6265 §5.1.4 default-path: uri-path up to the last '/'.
fn default_path(uri_path: &str) -> String {
    if !uri_path.starts_with('/') || uri_path == "/" {
        return "/".to_string();
    }
    match uri_path.rfind('/') {
        Some(0) | None => "/".to_string(),
        Some(i) => uri_path[..i].to_string(),
    }
}

/// The toRawForm render — attribute order matches Qt's emission:
/// name=value; secure; HttpOnly; SameSite=X; expires=…; domain=…;
/// path=….  The domain re-derives its leading dot from host_only so
/// the line re-parses to the same row.
pub fn to_raw_form(c: &Cookie) -> String {
    let mut out = format!("{}={}", c.name, c.value);
    if c.secure {
        out.push_str("; secure");
    }
    if c.http_only {
        out.push_str("; HttpOnly");
    }
    match c.same_site {
        SAMESITE_NONE => out.push_str("; SameSite=None"),
        SAMESITE_LAX => out.push_str("; SameSite=Lax"),
        SAMESITE_STRICT => out.push_str("; SameSite=Strict"),
        _ => {}
    }
    if let Some(exp) = c.expires {
        out.push_str(&format!("; expires={}", render_cookie_date(exp)));
    }
    if c.host_only {
        out.push_str(&format!("; domain={}", c.domain));
    } else {
        out.push_str(&format!("; domain=.{}", c.domain));
    }
    out.push_str(&format!("; path={}", c.path));
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmpdir(tag: &str) -> PathBuf {
        let d = std::env::temp_dir().join(format!(
            "arktos-cookies-test-{}-{tag}",
            std::process::id()
        ));
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    /// Opens a fresh store on an isolated temp dir.  The tests share
    /// the crate-global CookieStore, so each returns a lock guard the
    /// caller must hold for the whole test — the same pattern the
    /// sitedecisions tests use.
    fn open_store(tag: &str) -> (std::sync::MutexGuard<'static, ()>, PathBuf) {
        let guard = crate::store::test_lock();
        let dir = tmpdir(tag);
        let path = dir.join(DB_FILE);
        with(|s| s.open(Some(path.to_str().unwrap()))).unwrap();
        (guard, dir)
    }

    fn ck(name: &str, domain: &str) -> Cookie {
        Cookie {
            name: name.to_string(),
            value: "v".to_string(),
            domain: normalize_domain(domain).unwrap(),
            path: "/".to_string(),
            ..Cookie::default()
        }
    }

    #[test]
    fn crud_roundtrip() {
        let _guard = open_store("crud");
        let mut c = ck("sid", ".example.com");
        assert!(matches!(with(|s| s.set(&c)).unwrap(), SetOutcome::Stored));
        let got = with(|s| s.get("", "example.com", "sid", "/")).unwrap();
        assert_eq!(got.value, "v");
        assert!(!got.host_only);
        // Identical write reports Noop; a value change reports Stored.
        assert!(matches!(with(|s| s.set(&c)).unwrap(), SetOutcome::Noop));
        c.value = "v2".to_string();
        assert!(matches!(with(|s| s.set(&c)).unwrap(), SetOutcome::Stored));
        let got = with(|s| s.get("", "example.com", "sid", "/")).unwrap();
        assert_eq!(got.value, "v2");
        assert!(with(|s| s.remove("", "example.com", "sid", "/")).unwrap());
        assert!(!with(|s| s.remove("", "example.com", "sid", "/")).unwrap());
        assert!(with(|s| s.get("", "example.com", "sid", "/")).is_err());
        // Identity is (container, domain, name, path): same name on a
        // different path is a different cookie.
        let mut p = ck("sid", ".example.com");
        p.path = "/app".to_string();
        with(|s| s.set(&c)).unwrap();
        with(|s| s.set(&p)).unwrap();
        assert_eq!(with(|s| s.count(Some(""))).unwrap(), 2);
    }

    #[test]
    fn expiry_evicts_and_session_rows_purge_on_open() {
        let (_guard, dir) = open_store("exp");
        let mut c = ck("dead", ".example.com");
        c.expires = Some(now_ms() + 60_000);
        with(|s| s.set(&c)).unwrap();
        c.expires = Some(now_ms() - 1); // already past -> delete rule
        assert!(matches!(
            with(|s| s.set(&c)).unwrap(),
            SetOutcome::Deleted
        ));
        assert_eq!(with(|s| s.count(None)).unwrap(), 0);

        // A persistent row + a session row; reopening sweeps only the
        // session one.  Reopening the SAME path is the re-assert
        // no-op, so bounce through a second db to exercise the sweep.
        let mut keep = ck("keep", ".example.com");
        keep.expires = Some(now_ms() + 86_400_000);
        let sess = ck("tmp", ".example.com");
        with(|s| s.set(&keep)).unwrap();
        with(|s| s.set(&sess)).unwrap();
        assert_eq!(with(|s| s.count(None)).unwrap(), 2);
        let path = dir.join(DB_FILE);
        let other = tmpdir("exp2").join(DB_FILE);
        with(|s| s.open(Some(other.to_str().unwrap()))).unwrap();
        with(|s| s.open(Some(path.to_str().unwrap()))).unwrap();
        assert_eq!(with(|s| s.count(None)).unwrap(), 1);
        assert_eq!(
            with(|s| s.get("", "example.com", "keep", "/"))
                .unwrap()
                .name,
            "keep"
        );
        // And the explicit sweep.
        with(|s| s.set(&sess)).unwrap();
        assert_eq!(with(|s| s.purge_session()).unwrap(), 1);
    }

    #[test]
    fn for_url_matches_qt_jar() {
        let _guard = open_store("url");
        // The exact rows the Qt 6.12 jar was measured against:
        // leading-dot domain -> suffix reach; no-dot -> host-exact;
        // /app path boundary; secure excluded on http.
        let mut d = ck("d", ".example.com");
        d.host_only = false;
        d.created = 1;
        let mut h = ck("h", "sub.example.com");
        h.host_only = true;
        h.created = 2;
        let mut p = ck("p", ".example.com");
        p.path = "/app".to_string();
        p.created = 3;
        let mut s = ck("sec", ".example.com");
        s.secure = true;
        s.created = 4;
        for c in [&d, &h, &p, &s] {
            with(|st| st.set(c)).unwrap();
        }
        let names = |url: &str, fp: Option<&str>| -> Vec<String> {
            let req = ForUrl::from_json(&json!({
                "url": url, "container": "",
                "first_party": fp, "now_ms": now_ms()
            }))
            .unwrap();
            with(|st| st.for_url(&req))
                .unwrap()
                .into_iter()
                .map(|c| c.name)
                .collect()
        };
        // Mirrors the measured Qt output (the "sec" row this test
        // adds joins https only), order included — longest path
        // first, creation order inside a length.
        assert_eq!(names("https://example.com/", None), vec!["d", "sec"]);
        assert_eq!(
            names("https://sub.example.com/", None),
            vec!["d", "h", "sec"]
        );
        assert_eq!(
            names("https://deep.sub.example.com/", None),
            vec!["d", "sec"]
        );
        assert_eq!(
            names("https://sub.example.com/app/x", None),
            vec!["p", "d", "h", "sec"]
        );
        assert_eq!(
            names("https://example.com/app", None),
            vec!["p", "d", "sec"]
        );
        assert_eq!(names("http://sub.example.com/", None), vec!["d", "h"]);
        assert_eq!(names("http://example.com/", None), vec!["d"]);
        // Boundary: /apple is NOT under /app.
        assert_eq!(
            names("https://sub.example.com/apple", None),
            vec!["d", "h", "sec"]
        );
    }

    #[test]
    fn for_url_samesite_send_rule() {
        let _guard = open_store("ss");
        let mut lax = ck("lax", ".example.com");
        lax.same_site = SAMESITE_LAX;
        lax.created = 1;
        let mut strict = ck("strict", ".example.com");
        strict.same_site = SAMESITE_STRICT;
        strict.created = 2;
        let mut none = ck("none", ".example.com");
        none.same_site = SAMESITE_NONE;
        none.secure = true;
        none.created = 3;
        let mut plain = ck("plain", ".example.com");
        plain.created = 4;
        for c in [&lax, &strict, &none, &plain] {
            with(|s| s.set(c)).unwrap();
        }
        let names = |fp: Option<&str>| -> Vec<String> {
            let req = ForUrl::from_json(&json!({
                "url": "https://sub.example.com/", "container": "",
                "first_party": fp, "now_ms": now_ms()
            }))
            .unwrap();
            with(|st| st.for_url(&req))
                .unwrap()
                .into_iter()
                .map(|c| c.name)
                .collect()
        };
        // Same-site: everything rides, in creation order.
        assert_eq!(
            names(Some("example.com")),
            vec!["lax", "strict", "none", "plain"]
        );
        // Cross-site: Lax/Strict drop out; None and Default survive.
        assert_eq!(names(Some("tracker.other")), vec!["none", "plain"]);
        // No first-party supplied -> no samesite filter at all.
        assert_eq!(names(None).len(), 4);
    }

    #[test]
    fn container_partitions_isolate() {
        let _guard = open_store("cont");
        let mut a = ck("x", ".example.com");
        a.container = "aa11".to_string();
        let mut b = ck("x", ".example.com");
        b.container = "bb22".to_string();
        b.value = "other".to_string();
        with(|s| s.set(&a)).unwrap();
        with(|s| s.set(&b)).unwrap();
        assert_eq!(with(|s| s.count(Some("aa11"))).unwrap(), 1);
        assert_eq!(with(|s| s.count(Some("bb22"))).unwrap(), 1);
        // The same key in a second partition is an independent row.
        assert_eq!(
            with(|s| s.get("bb22", "example.com", "x", "/"))
                .unwrap()
                .value,
            "other"
        );
        // for_url is partition-scoped.
        let req = ForUrl::from_json(&json!({
            "url": "https://example.com/", "container": "aa11",
            "now_ms": now_ms()
        }))
        .unwrap();
        let got = with(|s| s.for_url(&req)).unwrap();
        assert_eq!(got.len(), 1);
        assert_eq!(got[0].value, "v");
        // Clearing a partition leaves the other alone.
        assert_eq!(with(|s| s.clear(Some("aa11"))).unwrap(), 1);
        assert_eq!(with(|s| s.count(Some("bb22"))).unwrap(), 1);
    }

    #[test]
    fn set_cookie_parse_live_and_export() {
        // Live parse against a request url: missing Domain ->
        // host-only to the request host; missing Path -> default.
        let parts = url_parts("https://www.example.com/deep/path/page").unwrap();
        let c = parse_set_cookie(
            "sid=abc; Secure; HttpOnly; SameSite=Lax; Max-Age=3600",
            Some(&parts),
        )
        .unwrap();
        assert_eq!(c.domain, "www.example.com");
        assert!(c.host_only);
        assert_eq!(c.path, "/deep/path");
        assert!(c.secure && c.http_only);
        assert_eq!(c.same_site, SAMESITE_LAX);
        assert!(c.expires.unwrap() > now_ms());

        // A Domain attr that does not cover the request host is
        // rejected outright.
        assert!(parse_set_cookie("x=1; domain=other.com", Some(&parts)).is_err());
        // Leading-dot Domain -> domain cookie; no-dot -> host-exact
        // (the Qt-verbatim rule).
        let d = parse_set_cookie("x=1; domain=.example.com", Some(&parts)).unwrap();
        assert_eq!(d.domain, "example.com");
        assert!(!d.host_only);
        let h = parse_set_cookie("x=1; domain=example.com", Some(&parts)).unwrap();
        assert!(h.host_only);

        // The expires attribute walks the Netscape date grammar.
        let e = parse_set_cookie(
            "x=1; expires=Tue, 01-Jan-2030 00:00:00 GMT; domain=.e.com",
            None,
        )
        .unwrap();
        assert_eq!(e.expires, Some(1893456000000));

        // toRawForm round-trip: a stored row exports a line that
        // re-parses to the same row.
        let mut c = ck("uname", ".example.com");
        c.value = "täst".to_string();
        c.secure = true;
        c.http_only = true;
        c.same_site = SAMESITE_STRICT;
        c.expires = Some(1893456000000);
        let line = to_raw_form(&c);
        assert!(line.starts_with("uname=täst; secure; HttpOnly; SameSite=Strict;"));
        let back = parse_set_cookie(&line, None).unwrap();
        assert_eq!(back.domain, c.domain);
        assert_eq!(back.same_site, c.same_site);
        assert_eq!(back.secure, c.secure);
        assert_eq!(back.http_only, c.http_only);
        assert_eq!(back.expires, c.expires);
        assert_eq!(back.host_only, c.host_only);
    }

    #[test]
    fn cookie_date_grammar() {
        // RFC6265 §5.1.1 — token-order tolerant shapes real sites emit.
        assert_eq!(
            parse_cookie_date("Tue, 01-Jan-2030 00:00:00 GMT"),
            Some(1893456000000)
        );
        assert_eq!(
            parse_cookie_date("Sun Nov  6 08:49:37 1994"),
            Some(784111777000)
        );
        assert_eq!(
            parse_cookie_date("01 Jan 2030 00:00:00"),
            Some(1893456000000)
        );
        // 2-digit years: 70-99 -> 1900s, 0-69 -> 2000s.
        assert_eq!(parse_cookie_date("01-Jan-70 00:00:00"), Some(0));
        assert_eq!(
            parse_cookie_date("01-Jan-69 00:00:00"),
            Some(3124224000000) // 2069-01-01
        );
        assert!(parse_cookie_date("garbage").is_none());
        assert!(parse_cookie_date("01-Jan-2030").is_none()); // no time
    }

    #[test]
    fn import_export_cpp_fixture() {
        let (_guard, dir) = open_store("imp");
        // testdata/cookies/export.txt — written by a real Qt 6.12
        // QNetworkCookie::toRawForm (tools/gen_cookie_export.cpp).
        let fixture = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("testdata/cookies/export.txt");
        let text = std::fs::read_to_string(&fixture).unwrap();
        let report = with(|s| s.import_text("", &text)).unwrap();
        // Six lines carry a Domain attribute; the host-cookie line
        // (no domain, no request context) is skipped — see the
        // parse contract.
        assert_eq!(report["imported"], 6);
        assert_eq!(report["skipped"], 1);
        // Domain cookie: suffix reach preserved.
        assert_eq!(
            with(|s| s.get("", "example.com", "sid", "/"))
                .unwrap()
                .same_site,
            SAMESITE_LAX
        );
        let sess = with(|s| s.get("", "sub.example.com", "pref", "/app")).unwrap();
        assert!(sess.expires.is_none()); // session row — but see below
        assert!(sess.host_only); // no-dot domain attr -> host-exact
        // The expired row imported verbatim (restore semantics —
        // a sweep drops it, not the import).
        assert!(with(|s| s.get("", "example.com", "old", "/")).is_ok());
        assert_eq!(with(|s| s.purge_expired(now_ms())).unwrap(), 1);
        // SameSite=None+secure and Strict rows landed.
        assert_eq!(
            with(|s| s.get("", "none.example", "track", "/"))
                .unwrap()
                .same_site,
            SAMESITE_NONE
        );
        // Export round-trips into the same file shape.
        let out = with(|s| s.export_text(None)).unwrap();
        assert!(out.contains("sid=a1b2c3; secure; HttpOnly; SameSite=Lax; expires=Tue, 01-Jan-2030 00:00:00 GMT; domain=.example.com; path=/"));
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn role_and_gate() {
        let (_guard, dir) = open_store("gate");
        assert_eq!(with(|s| s.role()).unwrap(), ROLE_AUTHORITATIVE);
        with(|s| s.set_role(ROLE_MIRROR)).unwrap();
        assert_eq!(with(|s| s.role()).unwrap(), ROLE_MIRROR);
        assert!(with(|s| s.set_role(7)).is_err());

        // Seed a sibling sitedecisions.json — the same file
        // CookieJar's exceptions persist through (RC06 interplay).
        std::fs::write(
            dir.join("sitedecisions.json"),
            serde_json::to_vec(&json!({
                "version": 1,
                "kinds": {"cookie": {
                    "bad.example": "block",
                    "ok.example": "allow",
                    "sess.example": "session",
                    ".wild.example": "block"
                }}
            }))
            .unwrap(),
        )
        .unwrap();
        let verdict = |input: Value| -> Value {
            with(|s| gate(&s.dir().unwrap(), &input)).unwrap()
        };
        // Blocked wins outright, even over an accept-always policy.
        assert_eq!(
            verdict(json!({"host": "bad.example",
                           "accept_policy": ACCEPT_ALWAYS}))["accept"],
            false
        );
        // A dotted rule covers subdomains; "rule" names the list.
        let v = verdict(json!({"host": "deep.wild.example"}));
        assert_eq!(v["accept"], false);
        assert_eq!(v["rule"], "block");
        // Third-party + block_3p rejects unless excepted.
        let v = verdict(json!({"host": "other.example",
                               "third_party": true, "block_3p": true}));
        assert_eq!(v["accept"], false);
        assert_eq!(v["rule"], "third-party");
        // An allow-list entry overrides the third-party block.
        let v = verdict(json!({"host": "ok.example",
                               "third_party": true, "block_3p": true}));
        assert_eq!(v["accept"], true);
        assert_eq!(v["rule"], "allow");
        // A session entry accepts and strips expiry on store.
        let v = verdict(json!({"host": "sess.example",
                               "third_party": true, "block_3p": true,
                               "expires_ms": now_ms() + 99_000_000}));
        assert_eq!(v["accept"], true);
        assert_eq!(v["session_only"], true);
        // The __utm* tracking drop fires before the accept policy.
        let v = verdict(json!({"host": "x.example",
                               "accept_policy": ACCEPT_ALWAYS,
                               "filter_tracking": true,
                               "name": "__utmz"}));
        assert_eq!(v["accept"], false);
        assert_eq!(v["rule"], "tracking");
        // Never-accept policy still honors an explicit exception.
        let v = verdict(json!({"host": "ok.example",
                               "accept_policy": ACCEPT_NEVER}));
        assert_eq!(v["accept"], true);
        // KeepUntilTimeLimit clamps a far expiry to ~90 days.
        let v = verdict(json!({"host": "x.example",
                               "keep_policy": KEEP_UNTIL_TIME_LIMIT,
                               "expires_ms": now_ms() + 365 * 86_400_000}));
        let cap = v["expires_cap_ms"].as_i64().unwrap();
        assert!(cap <= now_ms() + 91 * 86_400_000 && cap > now_ms());
    }

    #[test]
    fn mirror_role_is_honest_metadata() {
        let _guard = open_store("mirror");
        // A mirror-mode db runs the same ops — the flag records that
        // an engine owns the real jar (the RC15 two-mode contract).
        with(|s| s.set_role(ROLE_MIRROR)).unwrap();
        let mut c = ck("m", ".example.com");
        c.expires = Some(now_ms() + 60_000);
        assert!(matches!(with(|s| s.set(&c)).unwrap(), SetOutcome::Stored));
        assert_eq!(with(|s| s.role()).unwrap(), ROLE_MIRROR);
        // Reopen keeps the declared role persisted.
        with(|s| {
            let p = s.path.clone().unwrap();
            s.open(Some(p.to_str().unwrap()))
        })
        .unwrap();
        assert_eq!(with(|s| s.role()).unwrap(), ROLE_MIRROR);
        assert_eq!(with(|s| s.count(None)).unwrap(), 1);
    }

    #[test]
    fn bounds_and_bad_input() {
        let _guard = open_store("bounds");
        let mut bad = ck("", ".x.com");
        assert!(with(|s| s.set(&bad)).is_err());
        bad = ck("n;", ".x.com");
        assert!(with(|s| s.set(&bad)).is_err());
        bad = ck("n", ".x.com");
        bad.domain = String::new();
        assert!(with(|s| s.set(&bad)).is_err());
        bad.path = "relative".to_string();
        bad.domain = "x.com".to_string();
        assert!(with(|s| s.set(&bad)).is_err());
        bad.path = format!("/{}", "a".repeat(MAX_PATH));
        assert!(with(|s| s.set(&bad)).is_err());
        // JSON ingress rejects malformed records, not stores them.
        assert!(Cookie::from_json(&json!({"name": ""})).is_err());
        assert!(Cookie::from_json(&json!({
            "name": "n", "domain": "x.com", "sameSite": 9
        }))
        .is_err());
        assert!(Cookie::from_json(&json!({
            "name": "n", "domain": "x.com", "path": "p"
        }))
        .is_err());
        // A valid minimal record fills defaults.
        let ok = Cookie::from_json(&json!({
            "name": "n", "domain": "x.com"
        }))
        .unwrap();
        assert_eq!(ok.path, "/");
        assert!(!ok.host_only);
        assert_eq!(ok.same_site, SAMESITE_DEFAULT);
    }
}
