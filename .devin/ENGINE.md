# ENGINE.md — engine-portability coupling map (ENG01)

Groundwork for the selectable-engine effort (user directive: the Qt UI
shell stays; the web engine becomes selectable per tab —
QtWebEngine/Chromium vs Servo).  **No refactor yet** — this document is
the coupling audit + interface spec; `src/engine/engineinterface.h` is
the compiled-but-unused header sketch; ENG04 does the WebView refactor,
ENG05 the per-tab switcher.

Maintained by `.devin/check-engine-audit.sh` (wired into `make check`):
every C/C++ source file that names a `QWebEngine*`/`QtWebEngine*` symbol
must appear in the coupling map below or match a whitelisted path — add
an engine dependency without documenting it and the gate fails.

## Backends

| # | Engine | Status |
|---|--------|--------|
| 0 | QtWebEngine (Chromium 140.0.7339.225, Qt 6.12.0) | the only backend today |
| 1 | Servo | planned — ENG03 spike gates viability (interception, SOCKS5, cookie/storage, download, JS-eval hooks are the open questions) |

## Adapter boundary (where engine types are allowed to live)

The future `Engine::Backend` implementations wrap this layer.  Today
these files *are* the engine — ENG04 splits each into a neutral core +
a thin WebEngine adapter:

- `src/webpage.h`, `src/webpage.cpp` / `src/webview.h`, `src/webview.cpp` — the page/view pair.
- `src/browserprofile.h`, `src/browserprofile.cpp` — profile lifecycle + settings apply.
- `src/network/privacyrequestinterceptor.*`, `src/adblock/adblockrequestinterceptor.*`,
  `src/tor/torrequestinterceptor.*` — request-policy marshaling adapters
  (ENG02 done: every verdict comes from `Engine::NavigationPolicy` —
  rustcore `policy.rs` when built, an identical Qt fallback otherwise;
  `src/engine/navigationpolicy.*` is that engine-neutral seam and holds
  no WebEngine types).
- `src/network/schemeaccesshandler.*`, `src/network/fileaccesshandler.*`,
  `src/adblock/adblockresourcehandler.*`, `src/adblock/adblockschemeaccesshandler.*` —
  custom-scheme handlers.
- `src/network/cookiejar/cookiejar.h`, `src/network/cookiejar/cookiejar.cpp` — cookie-store wrap.
- `src/webpermissionmanager.h`, `src/webpermissionmanager.cpp` — permission broker.
- `src/webviewsearch.h`, `src/webviewsearch.cpp`, `src/webactionmapper.h`, `src/webactionmapper.cpp` — find /
  action plumbing.

## Coupling map — every `src/` file touching QtWebEngine

Verdicts: **wrap** = folds cleanly behind `Engine::Page/Profile` (most
signal/property usage); **redesign** = needs a neutral surface first
(history blobs, permission types, channel bridges); **engine-bound** =
inherently Chromium — capability-gated, degrades on Servo; **comment**
= prose-only mention, no dependency.

| File | QtWebEngine surface used | Verdict | Migration note |
|------|--------------------------|---------|----------------|
| `src/webpage.cpp` / `src/webpage.h` | `QWebEnginePage` subclass — `acceptNavigationRequest`, `createWindow`, `certificateError`, `selectClientCertificate`, `loadingChanged`, `QWebEnginePermission` broker hook, `QWebChannel` objects, `QWebEngineScript(Collection)`, `QWebEngineSettings`, `QWebEngineUrlSchemeHandlers` | redesign | The single hardest touchpoint: the class IS the backend-0 page. Splits into the policy/interstitial core (engine-neutral — cert/HTTPS-only/domain-block pages, popup gating, JS policy) + a `WebEnginePage` adapter. WebChannel bridging is engine-specific but the JS-side contract (arora objects) stays stable. |
| `src/webview.cpp` / `src/webview.h` | `QWebEngineView` subclass — zoom, `print`, `triggerPageAction`, `QWebEngineContextMenuRequest`, `QWebEngineHttpRequest` load, drag/drop URLs, `iconChanged` | wrap | Becomes the backend-0 `Engine::View`; context-menu payload maps to `Engine::ContextMenuInfo`; `print`/`HttpRequest` post-load go behind optional capabilities. |
| `src/webviewsearch.h`, `src/webviewsearch.cpp` | `QWebEnginePage::findText`, `QWebEngineFindTextResult` | wrap | `Engine::Page::findText` + `FindResult`. |
| `src/webactionmapper.h`, `src/webactionmapper.cpp` | `QWebEnginePage::WebAction` enum → `QAction` mapping | wrap | Enum becomes `Engine::WebAction`; the mapper itself is engine-neutral. |
| `src/webpermissionmanager.h`, `src/webpermissionmanager.cpp` | `QWebEnginePage::permissionRequested`, `QWebEnginePermission` types, `QWebEngineSettings` deny-pins, `persistentPermissionsPolicy` | redesign | Permission types need `Engine::PermissionType` (already sketched); the persisted grant store is engine-neutral. Qt's persistent-permissions policy is engine-bound → capability-gated. |
| `src/browserprofile.h`, `src/browserprofile.cpp` | profile creation (`normalProfile`/`privateProfile`/`torProfile`), `QWebEngineSettings` apply, `QWebEngineGlobalSettings::setDnsMode`, `applyChromiumFlags` (`QTWEBENGINE_CHROMIUM_FLAGS`), client hints, client certs, `persistentStoragePath` wipes, `QWebEngineScript` install | mixed | Profile lifecycle → `Engine::Backend::createProfile` (wrap). `applyChromiumFlags`, `setDnsMode`, client hints/certs, `TZ` tricks are engine-bound → move behind `Backend::initialize`/capability gates. Settings apply itself is engine-neutral semantics. |
| `src/containermanager.h`, `src/containermanager.cpp` | per-container `QWebEngineProfile` objects + storage paths, `QtWebEngineCore` version probe | wrap | Containers are `Engine::Profile`s with `ProfileOptions::dataPath`; site rules + registry are already engine-neutral. |
| `src/browserapplication.h`, `src/browserapplication.cpp` | `webEngineProfile()`/`privateWebEngineProfile()`, `prepareProfile` (CookieJar + interceptors + schemes + downloads + extensions install), `QWebEngineSettings` tweaks, extension manager | wrap | `prepareProfile` becomes `Backend::prepareProfile` — the *set* of services installed per profile is app policy; the install calls adapt. Extension install is capability-gated. |
| `src/browsermainwindow.h`, `src/browsermainwindow.cpp` | `QWebEngineHistory`/`QWebEngineHistoryItem` (menus + session serialize), `QWebEnginePage` action enums + devtools/inspect, `QWebEngineView::print` | redesign | History items need `Engine::HistoryItem` (title/url/time + opaque per-tab state blob — RCORE03's design). The rest wraps. |
| `src/tabwidget.h`, `src/tabwidget.cpp` | per-tab profiles, `QWebEngineHistory` serialization (`serializePageHistory`), `QWebEnginePage` for OpenUrlIn plumbing, OTR checks | redesign | Session blobs are engine-serialized history — becomes the opaque per-tab engine-state blob the interface stores verbatim (RCORE03). Profile binding per tab wraps. |
| `src/downloadmanager.h`, `src/downloadmanager.cpp` | `QWebEngineProfile::downloadRequested`, `QWebEngineDownloadRequest` lifecycle (state/received/total/cancel/pause/suggestedFileName) | wrap | `Profile::downloadRequested` + `Engine::DownloadRequest` cover the surface 1:1. |
| `src/rustdownloadengine.h`, `src/rustdownloadengine.cpp` | wraps `QWebEngineDownloadRequest` to accept into the Rust engine; reads page/profile for policy parity | wrap | Consumes `Engine::DownloadRequest` + `NavigationRequest` policy context. |
| `src/downloadworker.h` | comment only — the sandboxed worker never touches QtWebEngine | comment | Already engine-neutral by design. |
| `src/devtoolswindow.h`, `src/devtoolswindow.cpp` | `QWebEnginePage::setDevToolsPage` + a second `QWebEngineView` on the same profile | engine-bound | Chromium DevTools have no Servo equivalent — capability `devTools=false` hides the feature; DEVT02's BiDi panel is the portable path. |
| `src/extensions/extensionmanager.h`, `src/extensions/extensionmanager.cpp`, `src/extensions/extensionreviewdialog.h` | `QWebEngineExtensionManager`/`ExtensionInfo` (MV3 install/enable/uninstall/manifest inspect) + user `QWebEngineScriptCollection` | engine-bound | Chrome extensions are Chromium-only — capability `extensions`; the user-script half maps onto `Profile::insertScript` (wrap). |
| `src/network/privacyrequestinterceptor.h`, `src/network/privacyrequestinterceptor.cpp` | `QWebEngineUrlRequestInterceptor`, `QWebEngineUrlRequestInfo`, `QWebEngineLoadingInfo`, `QWebEngineProfile`, `QWebEngineSettings` read | wrap | Done (ENG02): decisions moved into rustcore `policy.rs` behind `Engine::NavigationPolicy`; this file only marshals `QWebEngineUrlRequestInfo` into the neutral manifest and applies the verdict (adblock matcher stays the tail stage). |
| `src/tor/torrequestinterceptor.h`, `src/tor/torrequestinterceptor.cpp` | `QWebEngineUrlRequestInterceptor` + `UrlRequestInfo` | wrap | Done (ENG02): same thin-adapter shape as the privacy interceptor — tor mode is a manifest flag the shared policy evaluates; Tor stays Chromium per ENG05, so it only ever needs the backend-0 form. |
| `src/adblock/adblockrequestinterceptor.h`, `src/adblock/adblockrequestinterceptor.cpp` | `QWebEngineUrlRequestInterceptor` + `UrlRequestInfo` | wrap | Same adapter shape; the matcher is already engine-free. |
| `src/adblock/adblocknetwork.h`, `src/adblock/adblocknetwork.cpp` | `QWebEngineUrlRequestInfo::ResourceType` + `ResourceTypeWebSocket` constants feeding rule type masks | redesign | Rule type masks key on the engine enum — decouple to `Engine::ResourceType` (the values were copied deliberately so the table already matches). |
| `src/adblock/adblockrule.h`, `src/adblock/adblockrule.cpp` | `QWebEngineUrlRequestInfo::ResourceType` for `$script`/`$image`/etc option masks | redesign | Same enum decoupling as adblocknetwork. |
| `src/adblock/adblockrustengine.cpp` | maps `QWebEngineUrlRequestInfo::ResourceType` numerics to webRequest type strings for the Rust engine | wrap | One switch on the enum — retarget to `Engine::ResourceType`. |
| `src/adblock/adblockpage.h`, `src/adblock/adblockpage.cpp` | cosmetic `QWebEngineScript`/`ScriptCollection` injection on `QWebEnginePage` | wrap | `Profile::insertScript` / `Page::runJavaScript`. |
| `src/adblock/adblockmanager.h`, `src/adblock/adblockmanager.cpp` | `QWebEngineProfile::setUrlRequestInterceptor` install point | wrap | `Profile::setRequestPolicy`. |
| `src/adblock/adblockresourcehandler.h`, `src/adblock/adblockresourcehandler.cpp` | `QWebEngineUrlSchemeHandler`/`UrlRequestJob`/`UrlScheme` — serves stub resources | engine-bound | Custom schemes need a per-backend story (Servo: embedder fetch interception — ENG03 open question #1). |
| `src/adblock/adblockschemeaccesshandler.h`, `src/adblock/adblockschemeaccesshandler.cpp` | `QWebEngineUrlSchemeHandler`/`UrlRequestJob`/`UrlScheme` — `abp:` subscribe endpoint | engine-bound | Same custom-scheme story. |
| `src/network/schemeaccesshandler.h`, `src/network/schemeaccesshandler.cpp` | `QWebEngineUrlSchemeHandler`/`UrlRequestJob`/`UrlScheme`/`UrlSchemeHandlers` — registered `arora-file:` etc. | engine-bound | Same custom-scheme story. |
| `src/network/fileaccesshandler.h`, `src/network/fileaccesshandler.cpp` | `QWebEngineUrlSchemeHandler`/`UrlRequestJob` serving `arora-file:` listings + `QWebEngineSettings` read | engine-bound | Same custom-scheme story. |
| `src/network/cookiejar/cookiejar.h`, `src/network/cookiejar/cookiejar.cpp` | `QWebEngineCookieStore` — `setCookieFilter`, `cookieAdded/Removed` mirror, `deleteAllCookies`, `setCookie` | wrap | Accept/reject decisions delegated to `Engine::NavigationPolicy::cookieFilter` (ENG02) — only storage/persistence stays here. Port is `Profile::setCookieFilter` + the storage-policy surface; the mirror/cookie model is engine-neutral. |
| `src/network/cookiejar/cookiejar.pri` | comment referencing `QWebEngineCookieStore` | comment | — |
| `src/network/networkaccessmanager.cpp` | comments only (cookie store, accept-language, interceptor notes) | comment | The app-side QNAM is already engine-free; comments update at ENG04. |
| `src/network/networkdiskcache.h` | comment only | comment | — |
| `src/webpermissionmanager` covered above | | | |
| `src/autofillmanager.h`, `src/autofillmanager.cpp` | `QWebEngineScript`/`ScriptCollection` injection, `QWebEnginePage` attach | wrap | `Profile::insertScript` + per-page attach through the adapter. The WebChannel bridge is inside webpage.cpp. |
| `src/fingerprintprotector.h`, `src/fingerprintprotector.cpp` | `QWebEngineScript` source + `QWebEngineProfile` install/uninstall | wrap | `Profile::insertScript`/`removeScript`; the JS itself is engine-agnostic. |
| `src/scriptcontrolmanager.h`, `src/scriptcontrolmanager.cpp` | `QWebEngineSettings::JavascriptEnabled` attribute, `QWebEnginePermission` comment, `UrlRequestInfo` resource types | wrap | `Page::setPageAttribute("javascript.enabled")` + `Engine::ResourceType`. |
| `src/popupblocker.h` | comment only (`JavascriptCanOpenWindows` note) | comment | — |
| `src/clearprivatedata.cpp` | `QWebEngineProfile` `clearHttpCache`/`clearAllVisitedLinks`/cookie store + per-`QWebEngineView` JS sweep | wrap | `Profile::clear(StorageAreas)` + `Page::runJavaScript`. |
| `src/settings.cpp` | `QWebEnginePermission` type list (Site Permissions audit page), `QWebEngineSettings` defaults, `QWebEngineProfile` | wrap | Maps `Engine::PermissionType` names; defaults come from the backend. |
| `src/statusbarwidgets.h`, `src/statusbarwidgets.cpp` | `QWebEngineView` load signals (loading indicator); memory widget reads `renderProcessPid` via the view; `.h` only names `QtWebEngineProcess` in comments | wrap | `Engine::View` signals + `Page::renderProcessId` (returns -1 when unsupported → '—'). |
| `src/locationbar/locationbar.cpp` | `QWebEngineView` `urlChanged`/`loadProgress` connects | wrap | `Engine::View` signals. |
| `src/locationbar/locationbarsiteicon.cpp` | `QWebEngineView` `loadFinished`/`iconChanged` | wrap | — |
| `src/locationbar/popupblockerbutton.cpp` | `QWebEngineView` `urlChanged` | wrap | — |
| `src/locationbar/privacyindicator.cpp` | `QWebEnginePage` profile OTR read | wrap | `Profile::isOffTheRecord`. |
| `src/locationbar/sitepanel.cpp` | `QWebEngineView` load/url signals | wrap | — |
| `src/locationbar/siteshield.cpp` | `QWebEngineView` load signals | wrap | — |
| `src/toolbarsearch.cpp` | `QWebEnginePage` OTR-profile read | wrap | `Profile::isOffTheRecord`. |
| `src/sourceviewer.cpp` | probe `QWebEnginePage` for DOM-compare + `toHtml` | wrap | `Page::runJavaScript`/`toHtml` behind a capability. |
| `src/readermode.cpp` | `QWebEnginePage` load signals + `JavascriptEnabled` lift + `runJavaScript` | wrap | `Page` surface covers all of it. |
| `src/pictureinpicture.h`, `src/pictureinpicture.cpp` | `QWebEnginePage`/`Profile`/`Settings`/`Permission` — pop-out video on the source profile | redesign | Needs a second page on the source profile — portable iff the interface exposes profile→page creation outside tabs; the video JS is engine-neutral. |
| `src/pipwindow.h`, `src/pipwindow.cpp` | second `QWebEngineView`/`Page`/`Profile` hosting `pip-player.html` | redesign | Same second-view surface as PiP. |
| `src/aboutdialog.cpp` | comment + `qWebEngineVersion`-style version string | engine-bound | Backend supplies `Backend::displayName`/version string. |
| `src/history/historymanager.h` | comment only (Chromium's internal history note) | comment | App-side store — already engine-neutral. |
| `src/tor/tormanager.h` | comment only (no per-profile proxy API note) | comment | Tor forces backend-0 anyway (ENG05 lock). |
| `src/main.cpp` | app init (scheme registration, `QtWebEngineQuick` init note) + every `--*-smoke` harness driving raw `QWebEngineView`/`Profile`/`Script`/interceptors | mixed | App-init lines → `Backend::initialize`. The smoke harnesses are engine-coupled test probes by design — ENG06's engine-matrix work absorbs/replaces them. |
| `src/src.pri` | `QT += webenginewidgets webchannel quickwidgets` | engine-bound | Backend-0 module deps; a Servo build swaps these for the spike's GL surface deps. |
| `src/rustdl/include/rustdl.h` | comment only (gate mirrors interceptor semantics) | comment | C FFI header — engine-neutral by design. |

## Engine-neutral interface surface (`src/engine/engineinterface.h`)

Sketched (compiled, unused) from the audit above.  `Engine::` namespace:

- `Backend` — id/displayName/`capabilities()`, `initialize()`, profile/
  page/view factories.
- `Profile` — OTR flag, storage name, user agent, `setRequestPolicy`
  (the ENG02 `NavigationPolicy` hook), `setCookieFilter`, script
  collection, `clear(StorageAreas)`, download handoff signal.
- `Page` — load/stop/reload/url, history nav, zoom, async `findText`,
  `runJavaScript`, per-page attributes, `setLifecycleState`
  (SLEEP01), `renderProcessId` (SBAR01 — -1 = unsupported),
  `createWindow` (POPUP01), and the signal set every consumer already
  connects to.
- `RequestPolicy`/`NavigationRequest`/`RequestDecision` — the
  interceptor boundary: url + resource type + navigation type +
  first-party + initiator → allow/block/redirect (+referer override).
- `DownloadRequest`, `Script`, `FindResult`, `CertificateErrorInfo`,
  `PermissionType`, `ContextMenuInfo` — the value types crossing the
  boundary.
- `Capabilities` — per-backend truth table (interception, injection,
  cookie filter, per-page settings, lifecycle discard, context-menu
  payload, cert override, devtools, extensions, downloads) so a Servo
  tab degrades honestly instead of faking surfaces.

Deliberately NOT in the interface (named engine gaps, per SEC16C's
bar): response-header access, sandbox/frame attribution in request
info, DOM access, per-profile proxy (why Tor is a separate process —
TOR02), a PiP negotiation API, OTR extension support.

## Settings keys — engine-bound vs engine-neutral

`webengine/` is the proposed namespace prefix for keys whose value is
meaningless to another engine (ENG04 migrates; listed for the audit):

| Key | Bound to | Note |
|-----|----------|------|
| `privacy/secureDns`, `secureDnsMode`, `secureDnsServer` | `QWebEngineGlobalSettings::setDnsMode` | → `webengine/dns*` |
| `privacy/tlsStrictCiphers` | `--cipher-suite-blacklist` Chromium flag | → `webengine/tlsStrictCiphers` |
| `privacy/webrtcIpProtection` | `--force-webrtc-ip-handling-policy` + `WebRTCPublicInterfacesOnly` | WebRTC is engine-bound today → `webengine/` |
| `privacy/dnsPrefetch` | `QWebEngineSettings::DnsPrefetchEnabled` | → `webengine/dnsPrefetch` |
| `websettings/forceDarkMode` | `QWebEngineSettings::ForceDarkMode` (Chromium auto-invert) | semantic is portable, implementation engine-bound — keep shared, backends interpret best-effort |
| `websettings/middleClickAutoscroll` | `MiddleClickAutoscroll` Blink feature | → `webengine/` |
| `extensions/*` | `QWebEngineExtensionManager` | → `webengine/extensions/*` |
| `websettings/*` remainder (fonts, enableJavascript, blockPopupWindows, enableImages, enableLocalStorage, enablePlugins, userAgent, cache) | conceptually engine-neutral | stay shared — every engine maps them best-effort |
| `privacy/fingerprintProtection`(+`Exceptions`), `reportUtcTimezone`, `normalizeAcceptLanguage`, `refererPolicy`, `httpsFirst`, `securityLevel`, `blockPings`/`blockPrefetch`/`blockRemoteFonts`/`blockThirdPartyCookies`, `clearOnExit`, `domainBlock*` | app-layer policy (interceptor/env/JS) | engine-neutral — implemented on our side of the boundary |
| `sessions/lastSession` | contains engine-serialized per-tab history | stays engine-opaque (RCORE03 blob design) |

## Non-code references (documented, not gated)

- `src/data/*.js` (`autofill.js`, `fingerprint.js`, `reader.js`,
  `pip-shim.js`, `arora-channel.js`) — mention QtWebEngine in comments;
  the JS itself is engine-agnostic page code.
- `fuzz/adblockrule/fuzz_adblockrule.cpp` — comment naming the
  ResourceType enum it mirrors.
- `tools/placesimport` — drives `HistoryManager` only; inherits the
  engine dep through `src.pri` transitively.
- `autotests/` — test harnesses legitimately drive engine APIs
  directly (they test the engine-coupled classes); whitelisted below.
- Generated dirs (`.moc/.obj/.rcc/.ui/`, `target/`, Makefiles,
  binaries) are excluded from the scan by extension filters + the
  whitelist.

## Audit whitelist (`check-engine-audit.sh`)

Entries below are path *substrings*: a file whose path contains one is
exempt from the per-file documentation requirement.

<!-- engine-audit-whitelist
.moc/       generated Qt meta-object sources
.obj/       intermediate objects
.rcc/       generated resources
.ui/        generated uic headers
/target/    cargo build output
autotests/  test harnesses drive engine APIs directly — the audit maps shipping-code coupling, not test internals
tools/      dev utilities
fuzz/       libFuzzer harnesses
manualtests/ manual fixtures
src/engine/ the adapter boundary itself — engine types live here by design once backends land
-->
