//! MD02: navpolicy — the acceptNavigationRequest-time verdict core.
//!
//! `WebPage::acceptNavigationRequest` used to hard-code every "should
//! this navigation commit?" decision in C++.  The decisions are pure
//! policy — they compute over the url, the navigation type, frame
//! position and a handful of caller-observed flags — so they live
//! here behind one JSON manifest in / verdict out contract, the same
//! shape ENG02 gave the request interceptors:
//!
//!   manifest in -> {"url","type","is_main_frame","current_url",
//!                   "current_is_managed","target_is_dir",
//!                   "target_is_managed","otr","container",
//!                   "pdf_viewer","pdf_sanitize","gesture",
//!                   "phase":"pre"|"mid"|"claim"}
//!   verdict out -> {"action":"continue"}
//!                | {"action":"internal","scheme":..,"interstitial":b}
//!                | {"action":"block","reason":..}
//!                | {"action":"ask","url":..,"kind":..}
//!                | {"action":"external","url":..,"kind":..}
//!                | {"action":"rewrite","kind":"abp-subscribe",...}
//!                | {"action":"redirect","kind":"file-dir","url":..}
//!                | {"action":"divert","container":..}
//!                | {"action":"claim","kind":"pdf"}
//!
//! The phases preserve the old function's gate ordering exactly:
//! "pre" runs the gates ahead of the tor hold (interstitial schemes,
//! per-profile overrides, the abp-subscribe rewrite, the external-
//! scheme handoff), "mid" runs the gates between the tor hold and the
//! already-delegated blocklist/https-only/form gates (staged-viewer
//! clamp, file-dir reroute, container divert) and "claim" runs the
//! pdf sanitize-on-view claim last.  Everything not listed — nonce
//! resolution, tor holds, dialogs, tab spawning, fetch claiming —
//! stays Qt-side execution; this module only answers questions.
//!
//! State the verdict needs is pushed in, never pulled: whether the
//! target is a directory (QFileInfo), whether a file: url lives in
//! the managed pdf staging dir, whether the profile is off-the-record
//! and which container the page is bound to are all caller-observed
//! flags.  The ONE store the module reads itself is the bound
//! profile's sitedecisions.json — the caller anchors it per call like
//! every other store consumer — for the per-profile override kinds:
//!
//!   "nav-block"  host -> "block"   (a host the user never lets a
//!                                  navigation commit to, any frame)
//!   "nav-scheme" scheme -> rule    (per-scheme override of the
//!                                   external-handoff gate:
//!                                   "ask" | "external" | "block";
//!                                   on handled schemes only "block"
//!                                   is honored — the engine cannot
//!                                   render mailto: no matter what)
//!   "container"  host -> id        (the CONT04 "always open in
//!                                   <container>" rules ContainerManager
//!                                   mirrors into this store — the
//!                                   divert verdict reads them here)
//!
//! Mirroring rules (kept 1:1 with the pre-refactor C++):
//!   * the browser-handled scheme allowlist is WebPage::
//!     isBrowserHandledScheme's QSet verbatim — both engines consume
//!     it through the same helper now, so the lists can never drift;
//!   * interstitial carrier schemes classify before everything else
//!     and only on the main frame — subframe requests on them drop;
//!   * subscribe.adblockplus.org claims only https main-frame
//!     navigations, and a url without ?location= is refused silently
//!     (no prompt), exactly like the old branch;
//!   * the staged-pdf clamp only bites while the CURRENT document is
//!     a managed staging-dir file — http(s) targets and managed-dir
//!     siblings pass;
//!   * the google /sorry/ rate-limit heuristic keeps its label-
//!     position approximation (no public-suffix list) — a "google"
//!     label in second-to-last position, or third-to-last behind a
//!     plausible second-level public-suffix label.

use serde_json::{json, Value};
use std::collections::BTreeSet;
use std::sync::{LazyLock, Mutex};

use crate::error::{fail, RcResult, RcStatus};
use crate::sitedecisions;

// ---- inputs -------------------------------------------------------------

/// QWebEnginePage::NavigationType ordinals — the manifest carries the
/// raw engine value like policy.rs does for resource types.
const NAV_FORM_SUBMITTED: i64 = 2;

/// The nonce-backed interstitial carriers — browser-served schemes
/// whose navigations resolve page-pending decisions in Qt.  Listed in
/// decision order; the verdict names which one matched so the Qt
/// plumbing needs no second scheme test.
const INTERSTITIAL_SCHEMES: [&str; 3] = [
    "arktos-cert-error",
    "arktos-http-warning",
    "arktos-site-block",
];

/// Schemes the engine or the registered scheme handlers serve —
/// WebPage::isBrowserHandledScheme's set verbatim.  A navigation to
/// anything else can only be serviced by the desktop (SEC02).
const BROWSER_SCHEMES: [&str; 19] = [
    "about",
    "abp",
    "arktos-cert-error",
    "arktos-file",
    "arktos-http-warning",
    "arktos-resource",
    "arktos-site-block",
    "blob",
    "chrome",
    "chrome-extension",
    "data",
    "devtools",
    "file",
    "filesystem",
    "http",
    "https",
    "javascript",
    "qrc",
    "view-source",
];

/// Per-profile override kinds inside sitedecisions.json.
const KIND_NAV_SCHEME: &str = "nav-scheme";
const KIND_NAV_BLOCK: &str = "nav-block";
const KIND_CONTAINER: &str = "container";
/// MD09: the pop-up exception kind — the same persona-independent
/// rows PopupBlocker persists through SiteDecisionStore::KindPopup
/// (the caller anchors "default" exactly like that path does).
const KIND_POPUP: &str = "popup";

// ---- minimal URL decomposition ------------------------------------------
// The manifest carries QUrl::toEncoded() output — already normalized
// (lowercase scheme/host, default ports dropped).  Strings stay
// byte-verbatim; decoded pieces are produced only where the old code
// decoded them (paths, query items).

/// Scheme text before the first ':', lowercased.  "" when absent.
fn url_scheme(url: &str) -> String {
    match url.find(':') {
        Some(i) => url[..i].to_lowercase(),
        None => String::new(),
    }
}

/// authority = between "://" and the next '/', '?' or '#'.
fn url_authority(url: &str) -> &str {
    let start = match url.find("://") {
        Some(i) => i + 3,
        None => return "",
    };
    let rest = &url[start..];
    let end = rest
        .find(|c| c == '/' || c == '?' || c == '#')
        .unwrap_or(rest.len());
    &rest[..end]
}

/// Host portion of the authority: userinfo stripped, :port stripped
/// (bracket-aware for IPv6 literals), brackets dropped, lowercased.
fn url_host(url: &str) -> String {
    let authority = url_authority(url);
    let hostport = match authority.rfind('@') {
        Some(i) => &authority[i + 1..],
        None => authority,
    };
    let host = if hostport.starts_with('[') {
        match hostport.find(']') {
            Some(i) => &hostport[1..i],
            None => hostport,
        }
    } else {
        match hostport.rfind(':') {
            Some(i) => &hostport[..i],
            None => hostport,
        }
    };
    host.to_lowercase()
}

/// The path component, percent-decoded — QUrl::path(FullyDecoded).
/// For opaque schemes (data:, mailto:) the path is everything after
/// the scheme and an optional '//authority'.
fn url_path_decoded(url: &str) -> String {
    let after_scheme = match url.find(':') {
        Some(i) => &url[i + 1..],
        None => url,
    };
    let rest = if let Some(r) = after_scheme.strip_prefix("//") {
        // Skip the authority: to the next '/', '?' or '#'.  A
        // missing delimiter means the whole rest was authority.
        match r.find(|c| c == '/' || c == '?' || c == '#') {
            Some(i) => &r[i..],
            None => "",
        }
    } else {
        after_scheme
    };
    let path = match rest.find(|c| c == '?' || c == '#') {
        Some(i) => &rest[..i],
        None => rest,
    };
    percent_decode(path)
}

/// One query item's decoded value — QUrlQuery::queryItemValue.
/// Pairs split on '&'; the key match is on the decoded key (QUrlQuery
/// compares keys decoded too).  '+' stays literal, matching
/// QUrlQuery's strict percent-decoding (no form-urlencoding legacy).
fn query_item(url: &str, key: &str) -> Option<String> {
    let qmark = url.find('?')?;
    let query = &url[qmark + 1..];
    let query = match query.find('#') {
        Some(i) => &query[..i],
        None => query,
    };
    for pair in query.split('&') {
        let (k, v) = match pair.find('=') {
            Some(i) => (&pair[..i], &pair[i + 1..]),
            None => (pair, ""),
        };
        if percent_decode(k) == key {
            return Some(percent_decode(v));
        }
    }
    None
}

/// %XX -> byte.  Invalid sequences pass through byte-verbatim like
/// QUrl's tolerant decoder; UTF-8 is validated lossily — callers only
/// compare ASCII tails.
fn percent_decode(s: &str) -> String {
    let bytes = s.as_bytes();
    let mut out = Vec::with_capacity(bytes.len());
    let mut i = 0;
    let hex = |b: u8| -> Option<u8> {
        match b {
            b'0'..=b'9' => Some(b - b'0'),
            b'a'..=b'f' => Some(b - b'a' + 10),
            b'A'..=b'F' => Some(b - b'A' + 10),
            _ => None,
        }
    };
    while i < bytes.len() {
        let decoded = bytes[i] == b'%'
            && i + 2 < bytes.len()
            && matches!(
                (hex(bytes[i + 1]), hex(bytes[i + 2])),
                (Some(_), Some(_))
            );
        if decoded {
            out.push(
                hex(bytes[i + 1]).unwrap() * 16 + hex(bytes[i + 2]).unwrap(),
            );
            i += 3;
            continue;
        }
        out.push(bytes[i]);
        i += 1;
    }
    String::from_utf8_lossy(&out).into_owned()
}

// ---- the pure predicates --------------------------------------------------

/// isBrowserHandledScheme verbatim — the shared allowlist both
/// engines' navigation hooks consume.
fn is_browser_scheme(scheme: &str) -> bool {
    BROWSER_SCHEMES.contains(&scheme.to_lowercase().as_str())
}

fn is_interstitial_scheme(scheme: &str) -> bool {
    INTERSTITIAL_SCHEMES.contains(&scheme)
}

/// SEC02's untrusted-input floor (WebView::isUrlAllowedOnUntrustedInput):
/// everything but javascript: may load — script text must never run
/// from a drop/paste/external handoff.
fn untrusted_input_allowed(scheme: &str) -> bool {
    scheme != "javascript"
}

/// The stricter page-link tier (WebView::isUrlAllowedFromPageLink):
/// additionally refuses data:/blob: for cross-context opens.
fn page_link_allowed(scheme: &str) -> bool {
    untrusted_input_allowed(scheme)
        && scheme != "data"
        && scheme != "blob"
}

/// PdfSupport::managedViewerNavAllowed — while the committed document
/// is a managed staging-dir file, only http(s) targets and managed
/// siblings may navigate.  The isLocalFile/isManagedPath halves are
/// caller-observed flags (they need the temp dir path, which is Qt's).
fn staged_viewer_nav_allowed(
    current_is_managed: bool,
    target_is_managed: bool,
    target_scheme: &str,
) -> bool {
    if !current_is_managed {
        return true;
    }
    if target_is_managed {
        return true;
    }
    target_scheme == "http" || target_scheme == "https"
}

/// PdfSupport::shouldIntercept — the sanitize-on-view claim.  The
/// viewer/sanitize toggles and the managed-dir membership are
/// caller-observed; the type/scheme/suffix predicates live here.
fn pdf_claim(
    pdf_viewer: bool,
    pdf_sanitize: bool,
    nav_type: i64,
    target_is_managed: bool,
    scheme: &str,
    decoded_path: &str,
) -> bool {
    if !pdf_viewer || !pdf_sanitize {
        return false;
    }
    // A POST response cannot be re-fetched.
    if nav_type == NAV_FORM_SUBMITTED {
        return false;
    }
    if scheme != "http" && scheme != "https" && scheme != "file" {
        return false;
    }
    // The pipeline's own sanitized output passes through.
    if scheme == "file" && target_is_managed {
        return false;
    }
    decoded_path
        .to_lowercase()
        .ends_with(".pdf")
}

/// WebPage::isRateLimitInterstitialUrl — Google serves its bot check
/// from <google host>/sorry/index; the host check is a label-position
/// approximation: a "google" label second-to-last (google.com,
/// www.google.de) or third-to-last behind a second-level public-
/// suffix label (google.co.uk, www.google.com.au).
fn rate_limit_interstitial(url: &str) -> bool {
    let path = {
        let after_scheme = match url.find(':') {
            Some(i) => &url[i + 1..],
            None => url,
        };
        let rest = if let Some(r) = after_scheme.strip_prefix("//") {
            match r.find(|c| c == '/' || c == '?' || c == '#') {
                Some(i) => &r[i..],
                None => "",
            }
        } else {
            after_scheme
        };
        match rest.find(|c| c == '?' || c == '#') {
            Some(i) => &rest[..i],
            None => rest,
        }
    };
    if !path.starts_with("/sorry/") {
        return false;
    }
    // The C++ QSet verbatim — plausible second-level public-suffix
    // labels (co.uk, com.au, …).
    const SECOND_LEVEL: [&str; 23] = [
        "ac", "ad", "co", "com", "edu", "gen", "go", "gov", "gouv", "id",
        "in", "ind", "me", "med", "mil", "ne", "net", "nom", "or", "org",
        "res", "sch", "web",
    ];
    let host = url_host(url);
    let labels: Vec<&str> = host.split('.').collect();
    let count = labels.len();
    for (i, label) in labels.iter().enumerate() {
        if *label != "google" {
            continue;
        }
        if i == count.wrapping_sub(2) && labels[i + 1].len() >= 2 {
            return true;
        }
        if i == count.wrapping_sub(3)
            && SECOND_LEVEL.contains(&labels[i + 1])
            && labels[i + 2].len() >= 2
        {
            return true;
        }
    }
    false
}

// ---- verdict emitters -----------------------------------------------------

fn cont() -> Value {
    json!({"action": "continue"})
}

fn block(reason: &str) -> Value {
    json!({"action": "block", "reason": reason})
}

// ---- per-phase pipelines --------------------------------------------------

fn field_str<'a>(v: &'a Value, key: &str) -> &'a str {
    v.get(key).and_then(|x| x.as_str()).unwrap_or("")
}

fn field_bool(v: &Value, key: &str) -> bool {
    v.get(key).and_then(|x| x.as_bool()).unwrap_or(false)
}

/// A host-level sitedecisions lookup that swallows store errors into
/// "no rule" — a broken preferences file must not wedge navigation.
fn store_lookup(kind: &str, host: &str) -> Option<String> {
    sitedecisions::lookup(kind, host)
        .ok()
        .flatten()
        .map(|(_, value)| value)
}

/// "pre" phase — ahead of the tor hold, in the old function's order:
/// interstitial carriers, per-profile nav-block, the abp-subscribe
/// rewrite, then the external-scheme gate with its per-profile
/// scheme-rule override.
fn decide_pre(m: &Value) -> Value {
    let url = field_str(m, "url");
    let scheme = url_scheme(url);
    let main = field_bool(m, "is_main_frame");

    // The nonce-backed interstitial carriers — the verdict names the
    // scheme; Qt resolves nonce/pending state and the "interstitial"
    // path branch.
    if is_interstitial_scheme(&scheme) {
        if !main {
            return block("interstitial-scheme-subframe");
        }
        return json!({
            "action": "internal",
            "scheme": scheme,
            "interstitial": url_path_decoded(url) == "interstitial",
        });
    }

    // Per-profile hard block — a host the user never lets a
    // navigation reach, whatever the frame.  Runs before every other
    // gate so even an otherwise-handled scheme cannot smuggle a nav
    // to it.
    let host = url_host(url);
    if !host.is_empty() {
        if let Some(v) = store_lookup(KIND_NAV_BLOCK, &host) {
            if v == "block" {
                return block("nav-block");
            }
        }
    }

    // ADB04: the https redirector form of the subscribe link —
    // https://subscribe.adblockplus.org/?location=… maps to the abp:
    // prompt.  No location query item means a silent refusal, exactly
    // like the old branch.
    if main && scheme == "https" && host == "subscribe.adblockplus.org" {
        match query_item(url, "location") {
            Some(location) if !location.is_empty() => {
                return json!({
                    "action": "rewrite",
                    "kind": "abp-subscribe",
                    "location": location,
                    "title": query_item(url, "title").unwrap_or_default(),
                });
            }
            _ => return block("abp-subscribe-no-location"),
        }
    }

    // The external-scheme gate — anything the browser cannot serve
    // goes to the desktop (main frame, with consent) or drops
    // silently (subframe).  The per-profile "nav-scheme" rule
    // overrides the default: "ask" keeps today's consent prompt,
    // "external" is the user's standing pre-consent, "block" drops
    // the navigation outright.  On handled schemes only "block" is
    // honored — "external" on http: would be nonsense.
    if !scheme.is_empty() {
        let rule = store_lookup(KIND_NAV_SCHEME, &scheme);
        if is_browser_scheme(&scheme) {
            if rule.as_deref() == Some("block") {
                return block("scheme-rule-block");
            }
            return cont();
        }
        match rule.as_deref() {
            Some("block") => return block("scheme-rule-block"),
            Some("external") => {
                return json!({
                    "action": "external",
                    "kind": "scheme-rule",
                    "url": url,
                });
            }
            _ => {
                if main {
                    return json!({
                        "action": "ask",
                        "kind": "external-scheme",
                        "url": url,
                    });
                }
                return block("external-scheme-subframe");
            }
        }
    }
    cont()
}

/// "mid" phase — between the tor hold and the already-delegated
/// blocklist/https-only/form gates: the staged-pdf viewer clamp, the
/// file: directory reroute, the container divert.
fn decide_mid(m: &Value) -> Value {
    let url = field_str(m, "url");
    let scheme = url_scheme(url);
    let main = field_bool(m, "is_main_frame");

    // PDF01: while a staged viewer document is displayed, non-remote
    // targets outside the managed dir are refused — an embedded
    // file:///etc/passwd-style link must not ride the local-to-local
    // exemption.
    if main
        && !staged_viewer_nav_allowed(
            field_bool(m, "current_is_managed"),
            field_bool(m, "target_is_managed"),
            &scheme,
        )
    {
        return block("staged-viewer-clamp");
    }

    // file:// is built into Chromium and cannot take a scheme
    // handler; directories reroute to arktos-file: for the managed
    // listing.  The is-dir observation is Qt's (filesystem access
    // stays shell-side); the reroute verdict is policy.
    if main && scheme == "file" && field_bool(m, "target_is_dir") {
        // Rebuild the listing url: scheme swap on the encoded form,
        // authority dropped — FileAccessHandler::urlForLocalPath's
        // shape (scheme + path, no host).
        let path = {
            let after_scheme = match url.find(':') {
                Some(i) => &url[i + 1..],
                None => url,
            };
            match after_scheme.strip_prefix("//") {
                Some(r) => match r.find(|c| c == '/' || c == '?' || c == '#') {
                    Some(i) => &r[i..],
                    None => "/",
                },
                None => after_scheme,
            }
        };
        return json!({
            "action": "redirect",
            "kind": "file-dir",
            "url": format!("arktos-file://{}", path),
        });
    }

    // CONT04: "always open this site in <container>" — a main-frame
    // http(s) navigation whose host rules into a different container
    // than the page's cannot commit here.  The lookup reads the same
    // "container" kind ContainerManager mirrors; the id-membership
    // check and the spawn stay Qt-side (the verdict carries the
    // candidate, the caller confirms it is still a live container).
    if main
        && (scheme == "http" || scheme == "https")
        && !field_bool(m, "otr")
    {
        // The caller pushes ContainerManager::normalizeSiteHost's
        // canonical key as "container_host" — its www-fold and
        // trailing-dot trim need the public-suffix table that stays
        // Qt-side; an absent key falls back to the raw url host, an
        // explicit empty one means "unnormalizable" — no rule, same
        // as the empty candidate the local walk produces.
        let host = match m.get("container_host").and_then(|x| x.as_str()) {
            Some(h) => h.to_string(),
            None => url_host(url),
        };
        if !host.is_empty() {
            if let Some(id) = store_lookup(KIND_CONTAINER, &host) {
                let current = field_str(m, "container");
                if !id.is_empty() && id != current {
                    return json!({
                        "action": "divert",
                        "container": id,
                    });
                }
            }
        }
    }
    cont()
}

/// "claim" phase — the pdf sanitize-on-view claim, last gate before
/// the engine's own accept.
fn decide_claim(m: &Value) -> Value {
    let url = field_str(m, "url");
    let scheme = url_scheme(url);
    if field_bool(m, "is_main_frame")
        && pdf_claim(
            field_bool(m, "pdf_viewer"),
            field_bool(m, "pdf_sanitize"),
            m.get("type").and_then(|x| x.as_i64()).unwrap_or(-1),
            field_bool(m, "target_is_managed"),
            &scheme,
            &url_path_decoded(url),
        )
    {
        return json!({"action": "claim", "kind": "pdf"});
    }
    cont()
}

/// The manifest -> verdict entry point.  Unknown/missing phase runs
/// all three phases in order and reports the first non-continue
/// verdict — the parity-test seam for "what would the whole pipeline
/// say" without replaying the Qt consult points.
pub fn decide(m: &Value) -> Value {
    match field_str(m, "phase") {
        "pre" => decide_pre(m),
        "mid" => decide_mid(m),
        "claim" => decide_claim(m),
        _ => {
            for verdict in [decide_pre(m), decide_mid(m), decide_claim(m)] {
                if verdict.get("action").and_then(|a| a.as_str())
                    != Some("continue")
                {
                    return verdict;
                }
            }
            cont()
        }
    }
}

// ---- popup gate (MD09) ---------------------------------------------------
// WebPage::createWindow's decision half: PopupBlocker::shouldBlock
// consulted "is this opener's pop-up-shaped window refused?" — an
// enabled flag, a scheme floor and a two-layer host allowlist — all
// pure policy.  The persistent layer is sitedecisions.json's "popup"
// kind (the same rows the Qt class already keeps); the session
// overlay — PopupBlocker::m_sessionAllowed, where off-the-record
// "always allow" writes land — is process-global here exactly like
// permpolicy's session answers.  Mirroring rules (1:1 with
// popupblocker.cpp):
//   * enabled=false allows everything (the Settings checkbox is
//     pushed in, never read — the module stays store-independent
//     for settings);
//   * a non-pop-up-shaped consult allows — tab-typed requests are
//     the placement preference's business, not the blocker's;
//   * an explicit gesture=false refuses even allow-listed openers
//     (the upstream JavascriptCanOpenWindows binding withholds
//     gesture-less opens before the allow list could matter);
//   * only http/https openers gate — internal pages and host-less
//     urls keep their windows;
//   * allow-list matching is the suffix walk both the session set
//     and the persisted rows share ("example.com" covers
//     "www.example.com", never "notexample.com");
//   * the persisted rows are value-blind today — presence allows.
//     A row spelled "ask" is the reserved always-ask mode: the
//     verdict answers "ask" so a consult point can route it to the
//     blocked-pop-up indicator flow instead of silently allowing.
//
// The window/probe creation, the indicator tally and the "Open
// once" spawn stay Qt-side execution; this module only answers.

/// PopupBlocker::m_sessionAllowed's rust home — process-global like
/// the QSet it replaces; nothing here ever persists.
fn popup_session() -> &'static LazyLock<Mutex<BTreeSet<String>>> {
    static SET: LazyLock<Mutex<BTreeSet<String>>> =
        LazyLock::new(|| Mutex::new(BTreeSet::new()));
    &SET
}

/// PopupBlocker::normalizeHost verbatim: lowercase, every trailing
/// dot stripped, then every leading dot (a leading dot is already
/// covered by the suffix match).
fn popup_normalize_host(host: &str) -> String {
    let mut normalized = host.to_lowercase();
    while normalized.ends_with('.') {
        normalized.pop();
    }
    while normalized.starts_with('.') {
        normalized.remove(0);
    }
    normalized
}

/// matchesHost's parent walk over the session overlay — host itself,
/// then each leftmost-label strip.
fn popup_session_allowed(host: &str) -> bool {
    let set = popup_session().lock().unwrap();
    let mut suffix = host;
    loop {
        if set.contains(suffix) {
            return true;
        }
        match suffix.find('.') {
            Some(dot) => suffix = &suffix[dot + 1..],
            None => return false,
        }
    }
}

/// Whether the host resolves allowed — the session overlay first,
/// then the persisted rows.  An "ask"-spelled row is a rule too, so
/// it reports allowed here (the consult op is what surfaces the
/// distinction); parity with the value-blind Qt key check.
fn popup_is_allowed(host: &str) -> bool {
    popup_session_allowed(host) || store_lookup(KIND_POPUP, host).is_some()
}

/// The createWindow consult.  Manifest: {"op":"popup_decide",
/// "url":<opener toEncoded>, "shaped":b, "enabled":b, "gesture":b?}.
/// Verdict: {"action":"allow","reason":..} | {"action":"block",
/// "reason":..} | {"action":"ask"}.
fn popup_decide(m: &Value) -> Value {
    let allow = |reason: &str| json!({"action": "allow", "reason": reason});
    // Both flags default ON like their Qt counterparts
    // (blockPopupWindows's true default; the consult IS the
    // popup-shaped one) — a caller must pass false explicitly to
    // move the verdict, so an omitted key can never weaken the gate.
    if !m.get("enabled").and_then(|x| x.as_bool()).unwrap_or(true) {
        return allow("disabled");
    }
    if !m.get("shaped").and_then(|x| x.as_bool()).unwrap_or(true) {
        return allow("shape");
    }
    // Gesture-required is the default: engines with the
    // JavascriptCanOpenWindows-style binding withhold gesture-less
    // opens upstream; an adapter that can observe the flag pushes it
    // in and the same refusal applies — even to allow-listed hosts.
    // A missing key means "not observed", not "no gesture".
    if let Some(false) = m.get("gesture").and_then(|x| x.as_bool()) {
        return block("popup-no-gesture");
    }
    let url = field_str(m, "url");
    let scheme = url_scheme(url);
    if scheme != "http" && scheme != "https" {
        return allow("scheme");
    }
    let host = popup_normalize_host(&url_host(url));
    // An http(s) url with no host cannot match a rule — isAllowedHost
    // refused it early the same way — so the walk only runs on a
    // real host.
    if !host.is_empty() {
        if popup_session_allowed(&host) {
            return allow("allowed");
        }
        if let Some(value) = store_lookup(KIND_POPUP, &host) {
            if value == "ask" {
                return json!({"action": "ask", "kind": "popup-ask"});
            }
            return allow("allowed");
        }
    }
    block("popup")
}

/// Generic granular endpoint — {"op":..., params} -> JSON result.
/// Covers the standalone predicates the Qt surfaces consult outside
/// the decide pipeline (the shared scheme allowlist, the untrusted-
/// input floors, the rate-limit heuristic), the override-store
/// reads the settings UI needs, and the MD09 pop-up gate (decide
/// consult + exception-list record ops — the mutation results carry
/// "mutated" for the change-notify emit).
pub fn call_json(v: &Value) -> RcResult<Value> {
    let op = v.get("op").and_then(|x| x.as_str()).unwrap_or("");
    let arg_str = |key: &str| -> String {
        v.get(key)
            .and_then(|x| x.as_str())
            .unwrap_or("")
            .to_string()
    };
    match op {
        "decide" => Ok(decide(v)),
        "browser_scheme" => Ok(json!(is_browser_scheme(&arg_str("scheme")))),
        "scheme_class" => {
            let scheme = arg_str("scheme").to_lowercase();
            let class = if is_interstitial_scheme(&scheme) {
                "internal"
            } else if is_browser_scheme(&scheme) {
                "handled"
            } else {
                "unhandled"
            };
            Ok(json!(class))
        }
        "rate_limit_interstitial" => {
            Ok(json!(rate_limit_interstitial(&arg_str("url"))))
        }
        "untrusted_input" => Ok(json!(untrusted_input_allowed(
            &arg_str("scheme").to_lowercase()
        ))),
        "page_link" => Ok(json!(page_link_allowed(
            &arg_str("scheme").to_lowercase()
        ))),
        "staged_viewer" => Ok(json!(staged_viewer_nav_allowed(
            v.get("current_is_managed")
                .and_then(|x| x.as_bool())
                .unwrap_or(false),
            v.get("target_is_managed")
                .and_then(|x| x.as_bool())
                .unwrap_or(false),
            &arg_str("scheme").to_lowercase(),
        ))),
        "nav_rule" => {
            // The override a host/scheme resolves to — the settings
            // UI's "what governs this?" read.  {"kind","key"} ->
            // {"key","value"} or null.
            let kind = arg_str("kind");
            let key = arg_str("key");
            if kind != KIND_NAV_SCHEME && kind != KIND_NAV_BLOCK {
                return fail(RcStatus::InvalidArgument, "unknown nav kind");
            }
            match sitedecisions::lookup(&kind, &key)? {
                Some((k, val)) => Ok(json!({"key": k, "value": val})),
                None => Ok(Value::Null),
            }
        }
        // ---- MD09 popup-gate ops ----
        "popup_decide" => Ok(popup_decide(v)),
        "popup_is_allowed" => {
            let host = popup_normalize_host(&arg_str("host"));
            Ok(json!({
                "allowed": !host.is_empty() && popup_is_allowed(&host),
            }))
        }
        "popup_allow" => {
            // allowHost: persistent rows write the store; session
            // rules stay in the overlay.  An unnormalizable host is
            // the Qt no-op, not an error.
            let host = popup_normalize_host(&arg_str("host"));
            if host.is_empty() {
                return Ok(json!({"recorded": false}));
            }
            if field_bool(v, "persistent") {
                let mutated = sitedecisions::set(KIND_POPUP, &host, "allow")?;
                Ok(json!({
                    "recorded": true,
                    "persisted": true,
                    "mutated": mutated,
                }))
            } else {
                popup_session().lock().unwrap().insert(host);
                Ok(json!({"recorded": true, "persisted": false}))
            }
        }
        "popup_remove" => {
            // removeAllowedHost scrubs both layers.
            let host = popup_normalize_host(&arg_str("host"));
            if host.is_empty() {
                return Ok(json!({
                    "removed_persistent": false,
                    "removed_session": false,
                }));
            }
            let rp = sitedecisions::remove(KIND_POPUP, &host)?;
            let rs = popup_session().lock().unwrap().remove(&host);
            Ok(json!({
                "removed_persistent": rp,
                "removed_session": rs,
                "mutated": rp,
            }))
        }
        "popup_list" => {
            // The persisted keys — the Settings audit table's
            // allowedHosts() read.  Value-blind like the Qt list.
            let entries = sitedecisions::list_json(KIND_POPUP)?;
            let rows: serde_json::Map<String, Value> =
                serde_json::from_str(&entries).unwrap_or_default();
            Ok(json!({
                "hosts": rows.keys().cloned().collect::<Vec<String>>(),
            }))
        }
        "popup_clear" => {
            // clearAllowedHosts: the persistent list only — the
            // session overlay survives (parity).
            let cleared = sitedecisions::clear_kind(KIND_POPUP)?;
            Ok(json!({"cleared": cleared, "mutated": cleared}))
        }
        "popup_clear_session" => {
            // The test-hook overlay drop — reports whether anything
            // was held, like the Qt early-out.
            let had = !popup_session().lock().unwrap().is_empty();
            popup_session().lock().unwrap().clear();
            Ok(json!({"cleared": had}))
        }
        _ => fail(RcStatus::InvalidArgument, "unknown navpolicy op"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn with_store(f: impl FnOnce()) {
        let _guard = crate::store::test_lock();
        let dir = std::env::temp_dir().join(format!(
            "arktos-nav-test-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).unwrap();
        crate::store::lock()
            .set_data_dir(dir.to_str().unwrap())
            .unwrap();
        let _ = sitedecisions::reset();
        popup_session().lock().unwrap().clear();
        f();
        let _ = sitedecisions::reset();
        popup_session().lock().unwrap().clear();
        let _ = std::fs::remove_dir_all(&dir);
    }

    fn pre(url: &str, main: bool) -> Value {
        decide(&json!({"phase": "pre", "url": url, "is_main_frame": main}))
    }

    #[test]
    fn internal_schemes_classify() {
        // Real carrier urls are opaque-form — scheme:path?n=nonce
        // (webpage.cpp:1839/2051/2168); the "//" spelling would put
        // "interstitial" in the authority, where QUrl::path() is ""
        // and the C++ check reads false too.
        for s in INTERSTITIAL_SCHEMES {
            let v = pre(&format!("{s}:interstitial?n=abc"), true);
            assert_eq!(v["action"], "internal");
            assert_eq!(v["scheme"], s);
            assert_eq!(v["interstitial"], true);
            // An action link (any other path) is still internal.
            let v = pre(&format!("{s}:proceed?n=abc"), true);
            assert_eq!(v["interstitial"], false);
            // Subframes on the carrier scheme drop.
            assert_eq!(pre(&format!("{s}:interstitial?n=x"), false)["action"],
                       "block");
        }
    }

    #[test]
    fn handled_schemes_continue() {
        for u in [
            "https://example.com/",
            "http://example.com/",
            "about:blank",
            "data:text/html,x",
            "file:///tmp/x",
            "javascript:alert(1)",
            "qrc:///start.html",
            "arktos-file:///tmp/",
            "view-source:https://example.com/",
        ] {
            assert_eq!(pre(u, true)["action"], "continue", "{u}");
            assert_eq!(pre(u, false)["action"], "continue", "{u}");
        }
    }

    #[test]
    fn unhandled_scheme_gate() {
        // Main frame: the consent-prompt verdict.
        let v = pre("mailto:a@b.c", true);
        assert_eq!(v["action"], "ask");
        assert_eq!(v["kind"], "external-scheme");
        // Subframe: silent drop.
        assert_eq!(pre("mailto:a@b.c", false)["action"], "block");
        assert_eq!(pre("magnet:?xt=urn:x", true)["action"], "ask");
        assert_eq!(pre("tel:+1234", true)["action"], "ask");
    }

    #[test]
    fn abp_subscribe_rewrite() {
        let v = pre(
            "https://subscribe.adblockplus.org/?location=https%3A%2F%2Fe.example%2Flist.txt&title=MyList",
            true,
        );
        assert_eq!(v["action"], "rewrite");
        assert_eq!(v["kind"], "abp-subscribe");
        assert_eq!(v["location"], "https://e.example/list.txt");
        assert_eq!(v["title"], "MyList");
        // No location -> silent refuse, same as the old branch.
        assert_eq!(
            pre("https://subscribe.adblockplus.org/?title=x", true)["action"],
            "block"
        );
        // Subframe or http -> not the rewrite path.
        assert_eq!(
            pre("https://subscribe.adblockplus.org/?location=x", false)["action"],
            "continue"
        );
    }

    #[test]
    fn scheme_and_host_overrides() {
        with_store(|| {
            sitedecisions::set("nav-scheme", "magnet", "external").unwrap();
            sitedecisions::set("nav-scheme", "tel", "block").unwrap();
            sitedecisions::set("nav-block", "evil.example", "block").unwrap();
            sitedecisions::set("nav-block", "deep.evil.example", "block")
                .unwrap();
            let v = pre("magnet:?xt=urn:x", true);
            assert_eq!(v["action"], "external");
            assert_eq!(pre("tel:+1", true)["action"], "block");
            assert_eq!(pre("tel:+1", false)["action"], "block");
            // Suffix walk: a sub.host under the rule host blocks too.
            assert_eq!(
                pre("https://www.evil.example/", true)["action"],
                "block"
            );
            assert_eq!(
                pre("https://evil.example/", false)["action"],
                "block"
            );
            // Unrelated host unaffected.
            assert_eq!(
                pre("https://ok.example/", true)["action"],
                "continue"
            );
        });
    }

    #[test]
    fn staged_viewer_clamp() {
        let mid = |current_managed: bool, target_managed: bool, url: &str| {
            decide(&json!({
                "phase": "mid", "url": url, "is_main_frame": true,
                "current_is_managed": current_managed,
                "target_is_managed": target_managed,
            }))
        };
        // No staged doc -> nothing clamps.
        assert_eq!(
            mid(false, false, "file:///etc/passwd")["action"],
            "continue"
        );
        // Staged doc: remote targets pass, unmanaged file: refuses.
        assert_eq!(
            mid(true, false, "https://e.example/")["action"],
            "continue"
        );
        assert_eq!(mid(true, false, "file:///etc/passwd")["action"], "block");
        assert_eq!(mid(true, true, "file:///staged/x.pdf")["action"], "continue");
        // Subframes never hit the clamp.
        assert_eq!(
            decide(&json!({
                "phase": "mid", "url": "file:///etc/passwd",
                "is_main_frame": false, "current_is_managed": true,
            }))["action"],
            "continue"
        );
    }

    #[test]
    fn file_dir_reroute() {
        let v = decide(&json!({
            "phase": "mid", "url": "file:///home/x%20y/",
            "is_main_frame": true, "target_is_dir": true,
        }));
        assert_eq!(v["action"], "redirect");
        assert_eq!(v["kind"], "file-dir");
        assert_eq!(v["url"], "arktos-file:///home/x%20y/");
        // A plain file (not a dir) continues.
        assert_eq!(
            decide(&json!({
                "phase": "mid", "url": "file:///x.png",
                "is_main_frame": true, "target_is_dir": false,
            }))["action"],
            "continue"
        );
        // file://host/path drops the authority like urlForLocalPath.
        let v = decide(&json!({
            "phase": "mid", "url": "file://localhost/etc/",
            "is_main_frame": true, "target_is_dir": true,
        }));
        assert_eq!(v["url"], "arktos-file:///etc/");
    }

    #[test]
    fn container_divert() {
        with_store(|| {
            sitedecisions::set("container", "bank.example", "uuid-a")
                .unwrap();
            let mid = |container: &str, otr: bool, url: &str| {
                decide(&json!({
                    "phase": "mid", "url": url, "is_main_frame": true,
                    "container": container, "otr": otr,
                }))
            };
            let v = mid("uuid-b", false, "https://bank.example/");
            assert_eq!(v["action"], "divert");
            assert_eq!(v["container"], "uuid-a");
            // Same container -> continue; OTR never diverts;
            // subframes never divert; non-web schemes never divert.
            assert_eq!(
                mid("uuid-a", false, "https://bank.example/")["action"],
                "continue"
            );
            assert_eq!(
                mid("uuid-b", true, "https://bank.example/")["action"],
                "continue"
            );
            assert_eq!(
                decide(&json!({
                    "phase": "mid", "url": "https://bank.example/",
                    "is_main_frame": false, "container": "uuid-b",
                }))["action"],
                "continue"
            );
            // Suffix: a rule on the parent covers subdomains.
            assert_eq!(
                mid("uuid-b", false, "https://www.bank.example/")["action"],
                "divert"
            );
            // The caller-pushed canonical key wins over the raw url
            // host — a www-folded lookup reaches the rule the raw
            // labels cannot, and an explicit "" spells "unnormalizable"
            // (the local walk produces an empty candidate, never a
            // raw-host probe).
            sitedecisions::set("container", "fold.example", "uuid-a")
                .unwrap();
            assert_eq!(
                decide(&json!({
                    "phase": "mid", "url": "https://www.fold.example./",
                    "is_main_frame": true, "container": "uuid-b",
                    "container_host": "fold.example",
                }))["action"],
                "divert"
            );
            assert_eq!(
                decide(&json!({
                    "phase": "mid", "url": "https://www.fold.example./",
                    "is_main_frame": true, "container": "uuid-b",
                }))["action"],
                "continue"
            );
            assert_eq!(
                decide(&json!({
                    "phase": "mid", "url": "https://fold.example/",
                    "is_main_frame": true, "container": "uuid-b",
                    "container_host": "",
                }))["action"],
                "continue"
            );
        });
    }

    #[test]
    fn pdf_claim_gate() {
        let claim = |url: &str, ty: i64| {
            decide(&json!({
                "phase": "claim", "url": url, "is_main_frame": true,
                "type": ty, "pdf_viewer": true, "pdf_sanitize": true,
            }))
        };
        assert_eq!(claim("https://e.example/a.pdf", 0)["action"], "claim");
        assert_eq!(claim("https://e.example/a.PDF?x=1", 0)["action"], "claim");
        assert_eq!(claim("file:///tmp/a.pdf", 1)["action"], "claim");
        // Encoded suffix decodes like QUrl::path(FullyDecoded).
        assert_eq!(claim("https://e.example/a.p%64f", 0)["action"], "claim");
        assert_eq!(claim("https://e.example/a.pdfx", 0)["action"], "continue");
        assert_eq!(claim("https://e.example/a.pdf", 2)["action"], "continue");
        // Toggles off -> no claim.
        assert_eq!(
            decide(&json!({
                "phase": "claim", "url": "https://e.example/a.pdf",
                "is_main_frame": true, "type": 0,
                "pdf_viewer": false, "pdf_sanitize": true,
            }))["action"],
            "continue"
        );
        // Managed staging output passes through.
        assert_eq!(
            decide(&json!({
                "phase": "claim", "url": "file:///staged/a.pdf",
                "is_main_frame": true, "type": 0,
                "pdf_viewer": true, "pdf_sanitize": true,
                "target_is_managed": true,
            }))["action"],
            "continue"
        );
    }

    #[test]
    fn rate_limit_heuristic() {
        for u in [
            "https://google.com/sorry/index",
            "https://www.google.de/sorry/index?q=x",
            "https://google.co.uk/sorry/",
            "https://www.google.com.au/sorry/index",
        ] {
            assert!(rate_limit_interstitial(u), "{u}");
        }
        for u in [
            "https://google.com/search",
            "https://google.example.com/sorry/",
            "https://google.com.evil.example/sorry/",
            "https://example.com/sorry/",
        ] {
            assert!(!rate_limit_interstitial(u), "{u}");
        }
    }

    #[test]
    fn untrusted_input_floors() {
        assert!(!untrusted_input_allowed("javascript"));
        assert!(untrusted_input_allowed("https"));
        assert!(untrusted_input_allowed("data"));
        assert!(!page_link_allowed("javascript"));
        assert!(!page_link_allowed("data"));
        assert!(!page_link_allowed("blob"));
        assert!(page_link_allowed("https"));
        assert!(page_link_allowed("ftp"));
    }

    #[test]
    fn whole_pipeline_first_verdict() {
        // No phase: pre -> mid -> claim, first claim wins.
        let v = decide(&json!({
            "url": "mailto:a@b.c", "is_main_frame": true,
        }));
        assert_eq!(v["action"], "ask");
        let v = decide(&json!({
            "url": "https://e.example/a.pdf", "is_main_frame": true,
            "type": 0, "pdf_viewer": true, "pdf_sanitize": true,
        }));
        assert_eq!(v["action"], "claim");
        assert_eq!(
            decide(&json!({
                "url": "https://e.example/", "is_main_frame": true,
            }))["action"],
            "continue"
        );
    }

    // ---- MD09 popup gate — the tst_popupblocker matrix verbatim ----

    fn popup(url: &str) -> Value {
        popup_decide(&json!({
            "url": url, "shaped": true, "enabled": true,
        }))
    }

    #[test]
    fn popup_enabled_and_shape_gates() {
        // enabled=false allows everything — the Settings checkbox.
        let v = popup_decide(&json!({
            "url": "http://example.com/", "shaped": true,
            "enabled": false,
        }));
        assert_eq!(v["action"], "allow");
        assert_eq!(v["reason"], "disabled");
        // A non-shaped consult allows — the placement preference's
        // business, not the blocker's.
        let v = popup_decide(&json!({
            "url": "http://example.com/", "shaped": false,
            "enabled": true,
        }));
        assert_eq!(v["action"], "allow");
        assert_eq!(v["reason"], "shape");
        // Enabled + shaped + http(s) gates.
        assert_eq!(popup("http://example.com/")["action"], "block");
        assert_eq!(popup("https://example.com/")["action"], "block");
    }

    #[test]
    fn popup_scheme_floor() {
        // Only http/https openers gate — internal pages and
        // host-less urls keep their windows.
        for u in [
            "file:///tmp/x.html",
            "about:blank",
            "data:text/html,<p>x</p>",
            "qrc:/startpage.html",
            "mailto:a@b.c",
            "",
        ] {
            let v = popup(u);
            assert_eq!(v["action"], "allow", "{u}");
            assert_eq!(v["reason"], "scheme", "{u}");
        }
    }

    #[test]
    fn popup_gesture_required() {
        // An explicit gesture=false refuses even allow-listed
        // openers; a missing key means "not observed".
        let m = |gesture: Value| json!({
            "url": "http://example.com/", "shaped": true,
            "enabled": true, "gesture": gesture,
        });
        // Every read runs under the store guard too — parallel tests
        // flip the process-global store mid-run, and an unguarded
        // popup_decide can transiently observe a sibling's "allow".
        with_store(|| {
            assert_eq!(popup_decide(&m(json!(false)))["action"], "block");
            assert_eq!(
                popup_decide(&m(json!(false)))["reason"],
                "popup-no-gesture"
            );
            assert_eq!(popup_decide(&m(json!(true)))["action"], "block");
            sitedecisions::set("popup", "example.com", "allow").unwrap();
            assert_eq!(
                popup_decide(&m(json!(false)))["action"],
                "block"
            );
            assert_eq!(
                popup_decide(&m(json!(true)))["action"],
                "allow"
            );
        });
    }

    #[test]
    fn popup_host_exceptions() {
        with_store(|| {
            sitedecisions::set("popup", "example.com", "allow").unwrap();
            // The parent rule covers subdomains — Firefox's
            // Exceptions semantics — never siblings or partials.
            assert_eq!(popup("http://example.com/")["action"], "allow");
            assert_eq!(popup("http://www.example.com/x")["action"], "allow");
            assert_eq!(
                popup("http://deep.sub.example.com/")["action"],
                "allow"
            );
            assert_eq!(popup("http://notexample.com/")["action"], "block");
            assert_eq!(popup("http://ample.com/")["action"], "block");
            assert_eq!(popup("http://example.org/")["action"], "block");
            assert_eq!(popup("http://other.org/")["action"], "block");
        });
    }

    #[test]
    fn popup_session_overlay() {
        with_store(|| {
            let allow = |host: &str, persistent: bool| {
                call_json(&json!({
                    "op": "popup_allow", "host": host,
                    "persistent": persistent,
                }))
                .unwrap()
            };
            // Session exceptions match like persistent ones...
            allow("session.test", false);
            assert_eq!(popup("http://session.test/")["action"], "allow");
            assert_eq!(
                popup("http://www.session.test/")["action"],
                "allow"
            );
            // ...but never land in the persisted list.
            let l = call_json(&json!({"op": "popup_list"})).unwrap();
            assert_eq!(l["hosts"], json!([]));
            // clear_session drops only the overlay.
            call_json(&json!({"op": "popup_clear_session"})).unwrap();
            assert_eq!(popup("http://session.test/")["action"], "block");
            // popup_remove scrubs both layers.
            allow("session.test", false);
            let r = call_json(&json!({
                "op": "popup_remove", "host": "session.test",
            }))
            .unwrap();
            assert_eq!(r["removed_session"], true);
            assert_eq!(r["removed_persistent"], false);
            assert_eq!(popup("http://session.test/")["action"], "block");
        });
    }

    #[test]
    fn popup_normalization_and_list() {
        with_store(|| {
            // Case and surrounding dots fold to the same rule;
            // re-adding under another spelling does not stack.
            for h in [".Example.COM.", "example.com"] {
                call_json(&json!({
                    "op": "popup_allow", "host": h,
                    "persistent": true,
                }))
                .unwrap();
            }
            let l = call_json(&json!({"op": "popup_list"})).unwrap();
            assert_eq!(l["hosts"], json!(["example.com"]));
            // Empty and punctuation-only hosts are refused outright.
            for h in ["", "..."] {
                let r = call_json(&json!({
                    "op": "popup_allow", "host": h,
                    "persistent": true,
                }))
                .unwrap();
                assert_eq!(r["recorded"], false);
            }
            let l = call_json(&json!({"op": "popup_list"})).unwrap();
            assert_eq!(l["hosts"], json!(["example.com"]));
            // is_allowed normalizes its input the same way.
            let r = call_json(&json!({
                "op": "popup_is_allowed", "host": "EXAMPLE.COM.",
            }))
            .unwrap();
            assert_eq!(r["allowed"], true);
            let r = call_json(&json!({
                "op": "popup_is_allowed", "host": "",
            }))
            .unwrap();
            assert_eq!(r["allowed"], false);
        });
    }

    #[test]
    fn popup_clear_and_ask_row() {
        with_store(|| {
            sitedecisions::set("popup", "a.example", "allow").unwrap();
            sitedecisions::set("popup", "b.example", "allow").unwrap();
            let r = call_json(&json!({"op": "popup_clear"})).unwrap();
            assert_eq!(r["cleared"], true);
            assert_eq!(r["mutated"], true);
            assert_eq!(popup("http://a.example/")["action"], "block");
            // A second clear reports nothing moved.
            let r = call_json(&json!({"op": "popup_clear"})).unwrap();
            assert_eq!(r["cleared"], false);
            // The reserved ask-mode row answers the ask verdict —
            // nothing writes it today, so parity is untouched.
            sitedecisions::set("popup", "ask.example", "ask").unwrap();
            assert_eq!(popup("http://ask.example/")["action"], "ask");
            // ...but the plain allowed check stays value-blind.
            let r = call_json(&json!({
                "op": "popup_is_allowed", "host": "ask.example",
            }))
            .unwrap();
            assert_eq!(r["allowed"], true);
        });
    }
}
