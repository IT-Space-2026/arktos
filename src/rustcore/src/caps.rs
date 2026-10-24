//! caps (MB04): the static per-engine capability matrix behind
//! ac_engine_caps() — the honest feature registry the
//! self-describing bridge needs so Qt-side consumers (MS06's
//! engine gating, acprobe, the mw.describe cross-reference) stop
//! hardcoding `backend->id()` checks.
//!
//! Every grade was filled from the reporting code that already
//! exists, not from aspiration:
//!
//!   * `Engine::Capabilities` — the Qt-side honest-degradation
//!     struct (src/engine/engineinterface.h) and its three backend
//!     implementations: WebEngineBackend::capabilities()
//!     (all-true), ServoBackend::capabilities() (near-empty;
//!     requestInterception is runtime-conditional on the resolved
//!     servo-embed artifact exporting se_set_load_gate),
//!     WebkitBackend::capabilities() (the WK07 spike-verified
//!     table — devTools and extensions measured false on the WPE
//!     build, downloads true).
//!   * Feature plumbing — PdfSupport (PDF01 interception is
//!     QWebEnginePage-bound; PDF02's PdfView shell is
//!     engine-neutral but only the WebEngine fetch pipeline
//!     stages documents into it), ExtensionManager ("MV3 only;
//!     MV2 packages are rejected outright" + the declared
//!     residual-gap set in .devin/EXT-RUNTIME-GAPS.md —
//!     Bitwarden-class extensions run with typed gaps, never
//!     full parity), WebPermissionManager (SEC05 broker — the
//!     WebKit backend maps its permission-request signal into
//!     the same broker for the covered types, unmapped types
//!     deny outright), the arktosAutofill QWebChannel bridge
//!     (page side lives only on QWebEnginePage), ReaderMode
//!     (WebView-bound; engine tabs hide the control), ENG05
//!     (Tor stays Chromium; container/private/session-bound
//!     spawns never leave WebEngine), WK09 favicon tracking,
//!     WK11 ephemeral + path-rooted network sessions, the
//!     WK16 arktosChannel script-message bridge.
//!
//! The table is STATIC: it reports what each engine supports when
//! its backend is present, never whether the backend resolved this
//! run — presence stays a Qt-side runtime question (the servo
//! artifact probe, WPE linkage, the all-false unlinked-TU report)
//! and is deliberately not claimed here.
//!
//! Wire shape: ac_engine_caps(NULL, out) emits
//! {"v":1,"engines":[<row>...]}; ac_engine_caps(id, out) emits the
//! single row {"v":1,"engine","display","backend_ids","features"}.
//! Each feature is {"state":"supported"|"partial"|"unsupported"}
//! plus "note" when the grade needs the caveat — notes are the
//! honesty mechanism: a `partial` without the why would be a lie
//! of omission.

/// The emitted document's schema version — bump when the JSON
/// shape changes, never for a grade correction (a re-grade is new
/// truth, not a new schema).
pub const CAPS_SCHEMA_VERSION: u32 = 1;

#[derive(Clone, Copy)]
enum State {
    Supported,
    Partial,
    Unsupported,
}

impl State {
    fn as_str(self) -> &'static str {
        match self {
            State::Supported => "supported",
            State::Partial => "partial",
            State::Unsupported => "unsupported",
        }
    }
}

/// One capability row: feature key, grade, caveat.  An empty note
/// is omitted from the JSON — notes exist to qualify partials and
/// surprising unsupported, not to narrate the obvious.
type Feature = (&'static str, State, &'static str);

struct EngineRow {
    /// The canonical engine id emitted in `engine`.
    engine: &'static str,
    display: &'static str,
    /// Every accepted lookup spelling — the canonical id plus the
    /// backend->id() the Qt side reports ("webengine").
    backend_ids: &'static [&'static str],
    features: &'static [Feature],
}

/// The feature set the MB04 spec names as the minimum — the
/// schema test asserts every engine reports every one of these.
#[cfg(test)]
const REQUIRED_FEATURES: [&str; 15] = [
    "devtools",
    "pdf_viewer",
    "extensions_mv2",
    "extensions_mv3",
    "webchannel",
    "otr_profile",
    "tor",
    "containers",
    "reader_mode",
    "downloads",
    "sessions",
    "favicons",
    "opensearch",
    "site_permissions",
    "autofill",
];

const QTWEBENGINE_FEATURES: &[Feature] = &[
    ("devtools", State::Supported,
     "inspectable debug channel — the BiDi/CDP panel"),
    ("pdf_viewer", State::Supported,
     "PDFium viewer plus the sanitize-on-view pipeline and the \
      engine-neutral PdfView shell (PDF01/PDF02)"),
    ("extensions_mv2", State::Unsupported,
     "MV2 manifests are rejected outright at install"),
    ("extensions_mv3", State::Partial,
     "MV3 only; the QtWebEngine hosted runtime answers a subset — \
      residual gaps are typed and declared in EXT-RUNTIME-GAPS.md \
      (Bitwarden-class extensions run, never full parity)"),
    ("webchannel", State::Supported,
     "QWebChannel object transport; the autofill, PiP and \
      extension-shim bridges ride it"),
    ("otr_profile", State::Supported, ""),
    ("tor", State::Supported,
     "the only engine Tor is wired for (ENG05)"),
    ("containers", State::Supported,
     "per-container profile storage roots"),
    ("reader_mode", State::Supported,
     "readability pipeline plus the injected reader.js driver on \
      WebView tabs"),
    ("downloads", State::Supported, ""),
    ("sessions", State::Supported,
     "full per-tab state blob (serializePageHistory) plus scroll \
      capture"),
    ("favicons", State::Supported, ""),
    ("opensearch", State::Supported,
     "app-side opensearch registry"),
    ("site_permissions", State::Supported,
     "SEC05 WebPermissionManager broker plus remembered site \
      decisions"),
    ("autofill", State::Supported,
     "the arktosAutofill QWebChannel bridge"),
    // Engine primitives — the Engine::Capabilities bits verbatim.
    ("request_interception", State::Supported,
     "live per-request policy consult gates traffic"),
    ("declarative_blocking", State::Unsupported,
     "policy gates requests live; no compiled-filter lane exists \
      on this backend"),
    ("script_injection", State::Supported, ""),
    ("script_evaluation", State::Supported,
     "runJavaScript returns results"),
    ("cookie_filter", State::Supported, ""),
    ("per_page_settings", State::Supported, ""),
    ("lifecycle_discard", State::Supported, ""),
    ("context_menu_info", State::Supported, ""),
    ("certificate_override", State::Supported, ""),
];

const SERVO_FEATURES: &[Feature] = &[
    ("devtools", State::Unsupported,
     "servo's devtools crate speaks the Firefox remote protocol on \
      a TCP listener — the BiDi/CDP panel cannot read it; no fake \
      channel is claimed"),
    ("pdf_viewer", State::Unsupported,
     "libservo ships no viewer; the PdfView shell can host staged \
      documents but no fetch/interception path exists on a servo \
      tab"),
    ("extensions_mv2", State::Unsupported, ""),
    ("extensions_mv3", State::Unsupported, ""),
    ("webchannel", State::Unsupported, ""),
    ("otr_profile", State::Unsupported,
     "profiles are one shared browsing context per persona — OTR \
      tabs stay on WebEngine (ENG05)"),
    ("tor", State::Unsupported,
     "Tor stays Chromium (ENG05)"),
    ("containers", State::Unsupported,
     "one shared browsing context per persona — per-container \
      isolation would be faked"),
    ("reader_mode", State::Unsupported,
     "ReaderMode is WebView-bound; engine tabs hide the control"),
    ("downloads", State::Unsupported,
     "no download delegate in the shim (capabilities().downloads \
      is false)"),
    ("sessions", State::Partial,
     "url and engine tag round-trip; the per-tab state blob only \
      understands WebEngine's serialize format, so a servo tab \
      restores at its flat url"),
    ("favicons", State::Unsupported,
     "no icon-delivery surface in the shim"),
    ("opensearch", State::Supported,
     "app-side opensearch registry"),
    ("site_permissions", State::Unsupported,
     "no permission-request surface"),
    ("autofill", State::Unsupported,
     "the arktosAutofill bridge is QWebChannel-bound"),
    // Engine primitives.
    ("request_interception", State::Partial,
     "runtime-conditional: true only when the resolved servo-embed \
      artifact exports se_set_load_gate"),
    ("declarative_blocking", State::Unsupported, ""),
    ("script_injection", State::Unsupported,
     "the resolved shim exposes no script-injection API"),
    ("script_evaluation", State::Unsupported, ""),
    ("cookie_filter", State::Unsupported, ""),
    ("per_page_settings", State::Unsupported, ""),
    ("lifecycle_discard", State::Unsupported, ""),
    ("context_menu_info", State::Unsupported, ""),
    ("certificate_override", State::Unsupported, ""),
];

const WEBKIT_FEATURES: &[Feature] = &[
    ("devtools", State::Unsupported,
     "WK21 measured: the WPE build exports no WebKitWebInspector \
      API and the inspector gresource is not installed"),
    ("pdf_viewer", State::Unsupported,
     "the PdfView shell can host staged documents but remote-PDF \
      interception is QWebEnginePage-bound and WPE has no viewer"),
    ("extensions_mv2", State::Unsupported, ""),
    ("extensions_mv3", State::Unsupported,
     "the WebKitWebExtension entry points are compiled-out stubs \
      on this build (WK21)"),
    ("webchannel", State::Partial,
     "the WK16 arktosChannel is a script-message bridge with reply \
      in an isolated world — a real two-way channel, not \
      QWebChannel object transport"),
    ("otr_profile", State::Supported,
     "ephemeral WebKitNetworkSession (WK11)"),
    ("tor", State::Unsupported,
     "no Tor lane is wired; the SOCKS5 surface exists engine-side \
      (WEBKIT-TOR study) but nothing consumes it"),
    ("containers", State::Partial,
     "dataPath-rooted sessions implement the storage-isolation \
      contract (WK11) but container tabs never spawn on this \
      backend — context-bound spawns stay on WebEngine (ENG05)"),
    ("reader_mode", State::Unsupported,
     "ReaderMode is WebView-bound; engine tabs hide the control \
      (the backend's eval/toHtml primitives exist but nothing \
      drives them for this)"),
    ("downloads", State::Partial,
     "the DownloadRequest handoff works — decide-destination is \
      WPE's only request point and pause/resume stay honest \
      no-ops (WK19)"),
    ("sessions", State::Partial,
     "url and engine tag round-trip; the per-tab state blob only \
      understands WebEngine's serialize format, so a WebKit tab \
      restores at its flat url"),
    ("favicons", State::Supported,
     "WK09 favicon tracking feeds iconChanged"),
    ("opensearch", State::Supported,
     "app-side opensearch registry"),
    ("site_permissions", State::Partial,
     "the SEC05 broker covers the mapped request types \
      (geolocation, notifications, mouse lock, media deny-list); \
      unmapped WebKit request types deny outright"),
    ("autofill", State::Unsupported,
     "the arktosAutofill bridge is QWebChannel-bound"),
    // Engine primitives.
    ("request_interception", State::Supported,
     "the live decide consult gates navigations"),
    ("declarative_blocking", State::Supported,
     "WK18 rules compile into a WebKitUserContentFilterStore \
      filter; inexpressible rules are counted, not faked"),
    ("script_injection", State::Supported, ""),
    ("script_evaluation", State::Supported, ""),
    ("cookie_filter", State::Unsupported,
     "WebKitCookieManager offers only a 3-state accept policy — no \
      per-attempt CookieAttempt functor"),
    ("per_page_settings", State::Supported, ""),
    ("lifecycle_discard", State::Supported, ""),
    ("context_menu_info", State::Supported, ""),
    ("certificate_override", State::Supported, ""),
];

const ENGINES: &[EngineRow] = &[
    EngineRow {
        engine: "qtwebengine",
        display: "Chromium (QtWebEngine)",
        // "webengine" is the id EngineRegistry::backends()
        // reports — callers holding backend->id() must resolve.
        backend_ids: &["qtwebengine", "webengine"],
        features: QTWEBENGINE_FEATURES,
    },
    EngineRow {
        engine: "servo",
        display: "Servo (servo-embed shim)",
        backend_ids: &["servo"],
        features: SERVO_FEATURES,
    },
    EngineRow {
        engine: "webkit",
        display: "WebKit (WPE)",
        backend_ids: &["webkit"],
        features: WEBKIT_FEATURES,
    },
];

fn engine_row(engine_id: &str) -> Option<&'static EngineRow> {
    ENGINES
        .iter()
        .find(|e| e.backend_ids.contains(&engine_id))
}

fn row_json(row: &EngineRow) -> serde_json::Value {
    let mut features = serde_json::Map::new();
    for (name, state, note) in row.features {
        let mut f = serde_json::Map::new();
        f.insert("state".into(), state.as_str().into());
        if !note.is_empty() {
            f.insert("note".into(), (*note).into());
        }
        features.insert((*name).into(), f.into());
    }
    serde_json::json!({
        "v": CAPS_SCHEMA_VERSION,
        "engine": row.engine,
        "display": row.display,
        "backend_ids": row.backend_ids,
        "features": features,
    })
}

/// The single-engine document for `engine_id` — canonical id or
/// any accepted backend-id spelling.  None for an unknown id.
pub fn engine_json(engine_id: &str) -> Option<String> {
    engine_row(engine_id)
        .map(|row| serde_json::to_string(&row_json(row)).unwrap_or_default())
}

/// The full matrix — every engine row under one document.
pub fn matrix_json() -> String {
    let engines: Vec<serde_json::Value> =
        ENGINES.iter().map(row_json).collect();
    serde_json::to_string(&serde_json::json!({
        "v": CAPS_SCHEMA_VERSION,
        "engines": engines,
    }))
    .unwrap_or_default()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn every_engine_reports_every_required_feature() {
        for row in ENGINES {
            let mut names: Vec<&str> =
                row.features.iter().map(|f| f.0).collect();
            names.sort_unstable();
            let mut seen = std::collections::HashSet::new();
            for n in &names {
                assert!(seen.insert(*n), "{} duplicates {}", row.engine, n);
            }
            for req in REQUIRED_FEATURES {
                assert!(
                    names.contains(&req),
                    "{} misses required feature {}",
                    row.engine,
                    req
                );
            }
        }
    }

    #[test]
    fn schema_shape_is_stable() {
        let doc: serde_json::Value =
            serde_json::from_str(&engine_json("qtwebengine").unwrap())
                .unwrap();
        assert_eq!(doc["v"], CAPS_SCHEMA_VERSION);
        assert_eq!(doc["engine"], "qtwebengine");
        assert_eq!(doc["backend_ids"], serde_json::json!(["qtwebengine", "webengine"]));
        let features = doc["features"].as_object().unwrap();
        assert!(features.len() >= REQUIRED_FEATURES.len());
        for (name, f) in features {
            let state = f["state"].as_str().unwrap();
            assert!(
                ["supported", "partial", "unsupported"].contains(&state),
                "{} has bad state {}",
                name,
                state
            );
            // A partial without a note is a lie of omission.
            if state == "partial" {
                assert!(f["note"].as_str().map_or(false, |n| !n.is_empty()),
                        "partial {} carries no note", name);
            }
        }
    }

    #[test]
    fn aliases_and_unknowns() {
        // The Qt backend id and the canonical id resolve to the
        // same row.
        assert_eq!(engine_json("webengine"), engine_json("qtwebengine"));
        assert!(engine_json("servo").is_some());
        assert!(engine_json("webkit").is_some());
        assert!(engine_json("gecko").is_none());
        assert!(engine_json("").is_none());
    }

    #[test]
    fn honest_spot_checks() {
        let qe: serde_json::Value =
            serde_json::from_str(&engine_json("qtwebengine").unwrap())
                .unwrap();
        assert_eq!(qe["features"]["devtools"]["state"], "supported");
        // Bitwarden-class extension support is partial at best on
        // every engine — the task's own honesty rule.
        assert_eq!(qe["features"]["extensions_mv3"]["state"], "partial");
        assert_eq!(qe["features"]["extensions_mv2"]["state"], "unsupported");
        let servo: serde_json::Value =
            serde_json::from_str(&engine_json("servo").unwrap()).unwrap();
        assert_eq!(servo["features"]["devtools"]["state"], "unsupported");
        assert_eq!(servo["features"]["downloads"]["state"], "unsupported");
        assert_eq!(servo["features"]["extensions_mv3"]["state"], "unsupported");
        let wk: serde_json::Value =
            serde_json::from_str(&engine_json("webkit").unwrap()).unwrap();
        assert_eq!(wk["features"]["devtools"]["state"], "unsupported");
        assert_eq!(wk["features"]["declarative_blocking"]["state"], "supported");
    }

    #[test]
    fn matrix_covers_all_engines() {
        let doc: serde_json::Value =
            serde_json::from_str(&matrix_json()).unwrap();
        assert_eq!(doc["v"], CAPS_SCHEMA_VERSION);
        let engines = doc["engines"].as_array().unwrap();
        assert_eq!(engines.len(), 3);
        let ids: Vec<&str> =
            engines.iter().map(|e| e["engine"].as_str().unwrap()).collect();
        assert_eq!(ids, ["qtwebengine", "servo", "webkit"]);
    }
}
