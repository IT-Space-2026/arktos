# CEF-SPIKE — Chromium Embedded Framework as the Chromium backend (analysis only)

Date: 2026-10-22 · Status: spike, **no code** · Lane: rust-core (RC11) ·
Companion docs: `.devin/ENGINE.md` (live coupling map + backend matrix),
`.devin/RUSTCORE-ARCH.md`, `.devin/RUSTCORE-ABI.md`,
`.devin/GECKO-STUDY.md`, `.devin/SERVO-SPIKE.md`, `.devin/WEBKIT-STUDY.md`.

**Scope rule, stated once and repeated throughout: CEF replaces the
QtWebEngine/Chromium embed only. It does not serve the Servo or WPE
WebKit backends — they keep their own `Engine::Backend` implementations
regardless of what happens on the Chromium lane.**

Question answered: if QtWebEngine were swapped for raw CEF behind the
same `Engine::` boundary, how much of the app does `arktos_core.h`
already absorb, what must still be ported, what has no CEF equivalent,
and is it worth doing?

---

## (a) What a CEF backend would consume from `arktos_core.h`

The facade (`src/rustcore/include/arktos_core.h`, AC_API_VERSION 1) is
the service layer every backend shares. Mapping each module to its CEF
integration point proves the facade is engine-complete for everything
that is *data* — the remaining work is purely *engine*.

| Facade surface | CEF integration point | Verdict |
|---|---|---|
| `ac_init` / `ac_shutdown` / `ac_set_change_callback` (`AcCore`) | `CefBackend::initialize()` lifetime — one `AcCore` per process, minted beside `CefInitialize`, torn down before `CefShutdown`. Change topics ("settings", "profiles", "history", "sitedec", …) drive per-profile re-apply exactly as today. | covered |
| `ac_capabilities` | Backend startup asserts the module bits it needs before advertising itself. | covered |
| `ac_settings_*` (typed get/set/list/sync, `import_ini`) | Source for `CefSettings`/`CefBrowserSettings` population and the `Engine::Profile::setProfileAttribute` apply path — the same keys `BrowserProfile::applySettings` reads through `ProfileSettings` today (already facade-backed per RC02/RC08). | covered |
| `ac_profiles_*` (registry, active persona, `ac_profiles_dir`, `ac_profiles_engine_storage_dir`, `ac_profiles_engine_cache_dir`, `slugify`) | `CefRequestContextSettings::root_cache_path`/`cache_path` for every named persona and container context — the facade already owns the path layout (RC03) and the persona resolution (RC09's shared `profileId()`). OTR personas map to transient `CefRequestContext`s, containers to named ones — the same model `ContainerManager` runs on `QWebEngineProfile`. | covered |
| `ac_hist_*` (add/update_title/query/suggest/icons/clear/expire) | Fed from `CefLoadHandler::OnLoadEnd` + `CefDisplayHandler::OnTitleChange`/`OnFaviconURLChange` on the page adapter — replaces the `QWebEnginePage` signal plumbing, hits the same sqlite store HistoryManager owns. | covered |
| `ac_bm_*` (XBEL tree, undo-safe handles) | Nothing to wire — bookmarks are already engine-free UI-side. The strongest proof the facade model works: zero of the 1427 touchpoints are bookmark-driven. | covered |
| `ac_sitedec_*`, `ac_policy_evaluate`, `ac_policy_cookie_filter`, `ac_blocklist_check`, `ac_tranco_rank`, `ac_sitetrust_lookup`, `ac_lookalike_check`, `ac_ua_spoof*` | The verdict layer behind `Engine::NavigationPolicy` (RC06) — a `CefResourceRequestHandler`/`CefCookieAccessFilter` consults exactly the same calls the `PrivacyRequestInterceptor` marshals today. Permission grants persisted through `ac_sitedec` ("webperm" kind) survive the swap byte-for-byte. | covered |
| `ac_last_error`/`ac_string_free`/`ac_buffer_free`, `AcStatus`, `AcBuffer` | Error/ownership plumbing, unchanged. | covered |

**Deliberately engine-owned (the facade correctly does NOT cover):
navigation state, DOM, JS evaluation, downloads, cookies, the per-tab
session nav stack.** Cookies deserve the explicit call-out per RC15's
reality check: `QWebEngineProfile` owns Chromium's cookie jar today and
`CefCookieManager` would own it under CEF — the facade supplies the
*filter verdicts* (`ac_policy_cookie_filter`, sitedec cookie rules) but
never the jar. Same for downloads (`CefDownloadHandler` →
`Engine::DownloadRequest`) and extensions-adjacent surfaces — see (c).

Verdict: **the facade is service-complete for CEF.** Everything RC02–RC08
moved into `arktos_core.h` ports for free; nothing in the spike requires
a new store module. What the facade cannot do is by design — it is a
data layer, not an engine API.

---

## (b) QWebEngine touchpoint inventory

Measured 2026-10-22 on branch `rust-core`:
`grep -rn "QWebEngine" src --include='*.cpp' --include='*.h'` excluding
generated `.moc`/`.obj`:

- **1427 touchpoints across 102 files** (the spec's "~1423" — drift
  since the estimate is +4 refs).
- Of those: 165 lines are comment-only mentions (mostly in the
  droppable files, some inline notes inside must-port files), 15 are
  `#include` lines naming QWebEngine headers (most files reach the
  API via grouped `QtWebEngine*` includes or the 39 forward
  declarations), the rest are code.
- Wider net `QWebEngine|QtWebEngine` (module/process/`QtWebEngineProcess`
  names): 1544 refs / 118 files incl. `.pri` — same buckets.
- Autotests excluded from the headline number but counted: 697 refs /
  52 files — they follow the product files' buckets (chromium-lane
  tests keep working while backend-0 stays WebEngine; they only need
  re-authoring if WebEngine is *removed*).

Buckets (dominant verdict per file — split files annotated below):

| Bucket | Files | Touchpoints | Meaning for a CEF port |
|---|---|---|---|
| **must-port** | 74 | 1176 | Real engine-surface dependency — needs a CEF/`Engine::` equivalent |
| **facade-absorbed** | 2 | 4 | Touchpoint exists only to key on persona/profile identity the facade now owns — satisfied by `ac_*`, nothing CEF-specific to write |
| **droppable** | 26 | 247 | Comment-only refs, or backend-0 adapter internals *replaced wholesale* by `cefbackend` (not ported) |
| **total** | **102** | **1427** | |

The absorbed slice is mostly *inside* must-port files, not whole files:
persona/storage-dir plumbing (`BrowserProfile::profileIdFor`,
`storageName()` parsing, `arktosProfileId` tags,
`ac_profiles_engine_storage_dir` consumers in `browserprofile.*`,
`containermanager.*`, `webenginebackend`, `webpage.cpp`,
`cookiejar`/`downloadmanager` per-profile keying) — roughly 100–130 of
the 1176 must-port lines are plumbing that resolves through the facade
rather than through a CEF API. The table below marks these files "↔".

### must-port (74 files, 1176 refs)

Adapter-boundary core — the files whose classes ARE engine types today:

| File(s) | Refs | CEF equivalent |
|---|---|---|
| `webpage.cpp`(75) `webpage.h`(14) ↔ | 89 | `WebPage : QWebEnginePage` must un-inherit → `Engine::Page` consumer; cert error, `createWindow`, `selectClientCertificate`, permission broker hook → `CefRequestHandler`/`CefPermissionHandler`/`OnSelectClientCertificate` |
| `webview.cpp`(32) `webview.h`(4) | 36 | `WebView : QWebEngineView` → `Engine::View` over an OSR/windowed CefBrowser host widget |
| `browserprofile.cpp`(99) `browserprofile.h`(32) ↔ | 131 | `CefRequestContext` lifecycle; `applySettings` → `CefBrowserSettings`; script installs → see gap G1; DNS mode/client certs/client hints → `CefSettings`/context options or dropped |
| `browserapplication.cpp`(24) `.h`(8) | 32 | `prepareProfile` → `CefRequestContextHandler` install set |
| `containermanager.cpp`(15) `.h`(7) ↔ | 22 | per-container `CefRequestContext` with facade dirs |
| `main.cpp`(197) | 197 | app-init (scheme registration, DNS, flags) → `CefBackend::initialize()`; ~40+ refs are `--*-smoke` probe harnesses driving raw WebEngine — re-author against `Engine::` or leave backend-0-only |
| `settings.cpp`(15) | 15 | settings-dialog reads of engine defaults → backend-supplied |

Request policy / schemes / cookies:

| File(s) | Refs | CEF equivalent |
|---|---|---|
| `privacyrequestinterceptor.*`(29) ↔ | 29 | `CefResourceRequestHandler::OnBeforeResourceLoad` + `GetResourceResponseFilter` — verdict consult already lands in rustcore `policy.rs` via `NavigationPolicy` |
| `adblockrequestinterceptor.*`(5) | 5 | same handler lane |
| `torrequestinterceptor.*`(11) | 11 | same (Tor stays Chromium-lane; a CEF swap changes *which* Chromium, not the model) |
| `schemeaccesshandler.*`(31), `fileaccesshandler.*`(15), `adblockschemeaccesshandler.*`(12), `adblockresourcehandler.*`(12) | 70 | `CefSchemeHandlerFactory` + `CefResourceHandler` — full parity for `arktos-file:`, `abp:`, interstitials |
| `cookiejar.*`(27) ↔ | 27 | `CefCookieManager` + filter via `ac_policy_cookie_filter` verdicts; jar stays engine-owned |
| `webpermissionmanager.*`(59) ↔ | 59 | `CefPermissionHandler` — **coarser type set** (gap G5); decisions still persist via `ac_sitedec` |

Downloads / PDF / print / devtools:

| File(s) | Refs | CEF equivalent |
|---|---|---|
| `downloadmanager.*`(18) ↔, `rustdownloadengine.*`(33) | 51 | `CefDownloadHandler` → `Engine::DownloadRequest` — parity incl. pause/resume |
| `pdfsupport.*`(17) | 17 | `CefBrowserHost::PrintToPDF`; the built-in-PDF-viewer staging is Chromium-builtin — CEF ships PDFium, parity likely |
| `printpreviewdialog.*`(6) | 6 | `PrintToPDF` parity |
| `devtoolswindow.*`(15) | 15 | `CefBrowserHost::ShowDevTools` is a *separate* window — embedded devtools pane needs the remote-debugging frontend hosted in a page (gap G4) |

Extension runtime (all must-port; the biggest single gap — G3):

| File(s) | Refs |
|---|---|
| `extensionmanager.*`(99), `extensionbackgroundhost.*`(117), `extensioneventhub.cpp`(16), `extensionpagebridge.*`(9), `extensionactionbar.*`(7+1 hdr), `extensionrequestwatcher.*`(7), `extensionshimbackend.*`(5) | ~260 |

Page-adjacent chrome (small, mechanical — `Engine::Page`/`View` already
abstracts most of it):

`readermode.cpp`(21 — `ReaderOverlayPage` second page), `pipwindow.*`
(11 — second view on source profile), `pictureinpicture` (hdr comments
only — droppable; .cpp 1 comment), `sourceviewer.cpp`(3),
`autofillmanager.cpp`(4 — JS bridge, gap G2),
`fingerprintprotector.*`(6 — script install, gap G1),
`clearprivatedata.cpp`(11 — `CefCookieManager`/storage clears ↔),
`tabwidget.cpp`(8 — lifecycle discard ↔, session blobs are already
`Engine::`-neutral), `webviewsearch.cpp`(1), `toolbarsearch.cpp`(1),
`scriptcontrolmanager.cpp`(1), `adblockmanager.*`(5 — install point),
`locationbar/*` (7 files, 22 — signal connects + OTR/profile reads ↔),
`webkitbackend.cpp`(10 — **real dep**: `QWebEnginePermission::
PermissionType` is the shared permission-type currency; needs
`Engine::PermissionType` retarget if QtWebEngine is removed entirely).

### facade-absorbed (2 files, 4 refs)

`notesmodel.*` (4) — per-profile model keyed on `QWebEngineProfile*`;
becomes the persona id the facade resolves. Every other absorbed
touchpoint is the ↔ plumbing slice inside must-port files above.

### droppable (26 files, 247 refs)

- **Backend-0 adapter internals — replaced, not ported** (193):
  `webenginebackend.cpp`(173), `webenginebackend.h`(20). A CEF swap
  deletes these and writes `cefbackend.*` fresh (section d) — none of
  the 193 refs is consumer work.
- **Doc-mirror comments** (19): `engineinterface.h` — "Mirrors
  `QWebEngineX`" provenance notes; the interface itself is
  engine-free.
- **Comment-only** (35): `servobackend.cpp`(1), `navigationpolicy.*`(4),
  `browserpaths.h`(2), `rustdl.h`(1), `profilevault.cpp`(1),
  `popupblocker.h`(1), `networkaccessmanager.cpp`(4),
  `networkdiskcache.h`(1), `adblockrule.*`(4 — enum already severed to
  numerics deliberately), `adblocknetwork.*`(3),
  `adblockrustengine.cpp`(1), `adblockpage.h`(1),
  `browsermainwindow.h`(1), `autofillmanager.h`(1),
  `pictureinpicture.*`(3), `extensionreviewdialog.h`(1),
  `extensionactionbar.h`(1), `scriptcontrolmanager.h`(2),
  `webkitbackend.h`(2).

---

## (c) CEF-specific gaps the facade does NOT cover

Ordered by severity for this codebase. (a) proves the *services* are
portable; these are the *engine* surfaces with no facade analogue.

- **G1 — Content/user scripts (the ~100 `QWebEngineScript` refs).**
  Autofill bundle, fingerprint protector, referrer-policy meta, GPC,
  adblock cosmetic filters, reader forwarder, PiP shim, and the
  `arktos-channel` bootstrap all ride `QWebEngineScript` on named
  worlds (`MainWorld`, `ApplicationWorld`, `ArktosChannelWorld` 0xa0,
  `ReaderWorldId`). CEF has **no declarative script collection**: the
  equivalent is hand-run `CefFrame::ExecuteJavaScript` from
  `CefRenderProcessHandler::OnContextCreated`/`OnFocusedNodeChanged`
  timing, and isolated "worlds" exist only inside the extension
  system — which leads to G3. An adapter shim must reimplement
  injection-point + world + subframe semantics per frame. Doable, not
  free — this is the seam where most silent behavior loss would hide.
- **G2 — JS↔C++ bridges (QWebChannel, 17+ files).** Autofill, arktos
  objects, reader and PiP bridges are `QWebChannel` objects in a
  private world. CEF has no equivalent: the path is
  `CefV8Handler` + `CefProcessMessage` browser↔renderer IPC (or the
  community message-router pattern) — every bridge object needs manual
  dispatch, and the "private world the page cannot enumerate" security
  property (SEC24) must be re-argued, not assumed.
- **G3 — Extension runtime.** ~260 must-port refs in `src/extensions/*`
  on `QWebEngineExtensionManager/Info`. CEF's extension story is the
  Alloy runtime only — partial, build-flag dependent, and **not**
  WebExtension-parity (no MV3 service-worker `chrome.*` surface
  comparable to Qt's). Honest capability: `extensions=false`, and the
  whole `ExtensionManager`/background-host/page-bridge/watcher lane
  degrades or becomes a CEF-specific research project. The single
  largest functional regression vs QtWebEngine.
- **G4 — Compositor/input + embedded devtools.** `QWebEngineView`
  composites via Qt's RHI. CEF gives either a *windowed* native child
  (X11-reparent — Wayland is second-class) or *OSR*
  (`CefRenderHandler::OnPaint` → texture + synthesized input) — OSR is
  the Wayland-safe choice and re-opens the exact render-widget/input
  plumbing the Servo spike bought. Embedded DevTools
  (`setInspectedPage`) also has no equivalent — `ShowDevTools` opens a
  separate window.
- **G5 — Permission-type granularity + lifecycle discard.**
  `QWebEnginePermission`'s type set (LocalFontsAccess, clipboard,
  desktop-capture splits) is richer than
  `CefPermissionHandler`'s prompt set; the SEC05 broker keeps working
  through `ac_sitedec` but some kinds have no CEF prompt to hang on.
  `setLifecycleState(Discarded)` has no CEF equivalent — capability
  reports false, the sleep-gate degrades honestly.
- **G6 — Packaging/runtime shape.** QtWebEngine arrives inside Qt.
  CEF must be *shipped*: `libcef.so` + a separate subprocess
  executable + `icudtl`/`.pak`/`locales/` (~150–300 MB per platform),
  multi-process mandatory, new sandbox packaging. All five RC10 legs
  (deb×2, rpm, flatpak, dmg, exe) need CEF staging — flatpak
  sandboxing and macOS `.app` subprocess-bundles are the nontrivial
  legs.
- **Non-gaps, verified rather than assumed:** session history is
  already `Engine::HistoryEntry`-neutral (`serializePageHistory` —
  `tabwidget.cpp:3412`); navigation/find/zoom/downloads/cookies/cert-
  errors/client-certs/scheme-handlers all have direct CEF handlers;
  request interception is *stronger* on CEF (full resource-handler
  pipeline, not just an interceptor) — the `NavigationPolicy` consults
  map 1:1.

---

## (d) Bounded adapter sketch — `src/engine/cefbackend`

Same shape as `servobackend.*`/`webkitbackend.*`: register via
`EngineRegistry::addProbe` → `registerBackend`, report honest
`Capabilities`, consume the facade — nothing else.

```
src/engine/cefbackend.h / .cpp        (~4–6k LOC, cf. webkitbackend 5.2k)

CefBackend : Engine::Backend
  initialize()      // AcCore mint (facade), CefSettings from ac_settings,
                    // CefInitialize(multiprocess, external subproc),
                    // CefRegisterSchemeHandlerFactory for the app's schemes
  id() "cef"        // displayName "Chromium (CEF <ver>)"
  capabilities()    // requestInterception=T, scriptInjection=T(shim),
                    // declarativeBlocking=F, cookieFilter=T, devTools=T(separate
                    // window), extensions=F (G3), lifecycleDiscard=F (G5)

CefProfileAdapter : Engine::Profile   // wraps CefRefPtr<CefRequestContext>
  createProfile:    // ProfileOptions::dataPath/cachePath :=
                    //   ac_profiles_engine_storage_dir/cache_dir(persona)
                    // OTR → transient context; containers → named contexts
  setRequestPolicy  // CefRequestContextHandler::GetResourceRequestHandler
                    //   → CefResourceRequestHandler marshaling to
                    //   NavigationRequest → NavigationPolicy/ac_policy_*
  setCookieFilter   // via resource handler / CefCookieAccessFilter →
                    //   ac_policy_cookie_filter-shaped consult
  insertScript      // per-frame ExecuteJavaScript shim (G1) — the
                    //   InjectionPoint/world/subframe semantics live here
  profileId()       // inherited RC09 resolver — persona tag, not re-parsed

CefPageAdapter : Engine::Page         // wraps CefRefPtr<CefBrowser>
  load/back/forward/reload/findText   // direct CefBrowser calls
  runJavaScript(+Lifted)              // frame->ExecuteJavaScript (+ V8
                                      //   route for values; main-world
                                      //   only unless G1 shim lands)
  historyItems/goToHistoryEntry       // CefNavigationEntry visits
  certificateError                    // OnCertificateError →
                                      //   CertificateErrorInfo
  contextMenuRequested                // CefContextMenuHandler →
                                      //   ContextMenuInfo
  action(StandardAction)              // mapped QActions like webengine's
  renderProcessId/lifecycle           // -1 / Active-only (G5 honest)

CefViewWidget : QWidget               // OSR CefRenderHandler host
                                      // (OnPaint → QImage/GL texture,
                                      // synthesized input) — Wayland-safe;
                                      // windowed X11 fallback

CefDownloadAdapter                    // CefDownloadHandler →
                                      // Engine::DownloadRequest incl.
                                      // pause/resume/totals
```

Bounded means: the adapter consumes `arktos_core.h` for every service
and `Engine::` for every consumer contract — no facade changes, no new
stores, no consumer edits beyond finishing the escape-hatch retirement
already underway.

---

## (e) Effort estimate + go/no-go

**Effort (order-of-magnitude, engineer-weeks):**

| Workstream | Est. | Notes |
|---|---|---|
| `cefbackend` adapter core (G4 OSR widget incl.) | 4–7 wk | CEF's C++ API is friendlier than WPE/glib; webkitbackend-scale |
| Consumer un-coupling — finish `Engine::` migration for the 74 must-port files | 6–10 wk | The real cost. `WebPage`/`WebView` un-inheriting + ~1176 touchpoints; escape hatches (`webEnginePage()`) retire or go engine-gated |
| G1 script-injection shim + G2 bridge replacement | 3–5 wk | Includes re-arguing SEC24's private-world property |
| Extension runtime | research project | G3 — assume `extensions=false` in any bounded scope |
| Packaging legs (5) + subprocess/sandbox | 3–5 wk | RC10's link-proof pattern extends; binary distribution is new weight |
| **bounded scope total** | **~16–27 wk** | to parity *minus* extensions |

**Go / no-go: NO-GO for a production swap today.**

- CEF buys the *same* Chromium engine Arktos already embeds, minus Qt's
  integration — the delta is negative on day one (lose extensions,
  QWebChannel bridges, embedded devtools, declarative scripts; gain
  nothing user-visible).
- The rustcore migration de-risks the swap either way and is the point
  of this exercise: sections (a)+(b) prove a future CEF backend writes
  **only the engine half** — every store it needs is already behind
  `ac_*`. That property is worth keeping (and was the design goal), not
  a reason to swap.
- The `Engine::` boundary already hosts three backends; CEF is the
  fourth *only if* a driver appears: Qt drops/changes WebEngine
  licensing or cadence, a CEF-unique capability is required, or the
  Chromium-embed maintenance burden inverts. Revisit then — the
  inventory above is the standing map.
- If a driver does appear, sequence: (1) finish escape-hatch retirement
  so consumers compile engine-free, (2) `cefbackend` behind
  `CONFIG+=cef`, (3) G1/G2 shims, (4) extensions stay off — never
  claim parity the runtime does not verify.

*Reminder: CEF replaces the Chromium embed. Servo and WPE WebKit keep
their own backends and their own capability rows; nothing here gates
them.*
