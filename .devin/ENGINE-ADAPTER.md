# ENGINE-ADAPTER.md — the EngineAdapter contract (MS02)

The formal contract every engine backend must satisfy to host a tab in
Arktos.  The live boundary is `src/engine/engineinterface.h`
(`Engine::Backend` / `Engine::Profile` / `Engine::Page` /
`Engine::DownloadRequest` / `Engine::RequestPolicy`), registered through
`src/engine/engineregistry.*` and hosted by `src/engine/enginetab.*`.
This doc states what each contract area *means*, maps it to the exact
interface surface, records how the capability path gates it, and gives
the honest per-engine matrix.  ENGINE.md remains the historical coupling
map; this file is the implementer's contract.

Two capability surfaces exist and MUST NOT diverge:

- `Engine::Backend::capabilities()` — the C++ struct the adapter itself
  reports; runtime-measured where the answer depends on the resolved
  artifact (Servo's `se_set_load_gate` probe, WebKit's dlsym-gated
  suspend/reply exports, the unlinked-TU all-false).
- `ac_engine_caps(engine_id)` — the rustcore facade table the thin
  shell consumes (bridge lane MB04, **pending** at time of writing —
  `../arora-mwbridge/.devin/Arora-Task-Bridge.md`).  Schema per MB04:
  per-feature `supported|partial|unsupported` + optional note over a
  fixed feature set.  MS05/MS06 rewire chrome gating to it.

Honesty rule: a backend reports only what it can actually do right now.
An unmapped `StandardAction` returns nullptr, an unserved scheme never
resolves, a missing export drops the bit.  Faking a surface is worse
than the gap — chrome degrades on the table, not on try-and-fail.

## Contract areas

### 1. Identity, availability, lifecycle (Backend)

| Requirement | Interface surface | Honest-degradation rule |
|---|---|---|
| Stable id + human name | `Backend::id()` (`"webengine"`, `"servo"`, `"webkit"`), `displayName()` | id is the caps/selector key; never rename shipped ids |
| Process/engine init | `Backend::initialize()` | false ⇒ probe drops the backend; the option never reaches chrome (Servo missing cdylib, WebKit unlinked TU) |
| Capability report | `Backend::capabilities()` | measured, not aspirational — see §9 |
| Pre-flight refusal reason | `Backend::pageUnavailableReason()` | non-empty ⇒ engine-switcher entries render disabled with this text; `createPage` may still hard-refuse (returns nullptr ⇒ `TabWidget::addEngineTab` shows the status message, never a doomed tab) |
| Selector caveat | `Backend::engineNote()` | one honest line (Servo: experimental/debug-artifact warning; WebKit: early-backend note) |
| Factory triple | `createProfile(ProfileOptions)` / `createPage(Profile)` / `createView(Page)` | the ONLY construction path `EngineTab` uses; a backend that cannot isolate profiles still returns an honest Profile object carrying persona identity (Servo's shared context) |

Registration is self-service: a static-init
`EngineRegistry::addProbe()` from the backend's own TU (servobackend.cpp
:1798, webkitbackend.cpp :5201) so subproject binaries linking
engineregistry.o never need the engine TU.  The registry lists, never
owns — backends parent to qApp.

### 2. Navigation control (Page)

| Requirement | Interface surface |
|---|---|
| Load/stop/reload/current url | `Page::load/stop/reload/url()` |
| History walk | `canGoBack/canGoForward/back/forward` |
| Session-history stack | `historyCount`, `currentHistoryIndex`, `historyItems`, `backItems(max)`, `forwardItems(max)`, `goToHistoryEntry` — linear stack, oldest-first, index-addressed (WebKit maps its BackForwardList; Servo keeps an adapter-side recordNavigation stack) |
| Zoom | `setZoomFactor/zoomFactor` |
| Scroll snapshot | `scrollPosition()` (undo-close restore; null point = nothing to restore) |
| View binding | `Page::view()` — reverse lookup to the painted widget; nullptr while headless/detached |

`NavigationType`/`LoadStatus` enums mirror the Chromium set; a coarser
engine reports the guaranteed subset (LinkClicked/Typed/Redirect).

### 3. Load-state + page events (Page signals)

The tab chrome (TabWidget labels, progress, status bar, padlock,
EngineTab's re-emitted `titleChanged/urlChanged/iconChanged/
loadStarted/loadProgress/loadFinished`) consumes exactly these signals —
a backend that never emits one leaves that chrome dark, which is the
honest shape for a missing surface:

`loadStarted`, `loadProgress(int)`, `loadFinished(bool)`,
`urlChanged(QUrl)`, `titleChanged(QString)`, `iconChanged(QIcon)`,
`linkHovered(QString)`, `windowCloseRequested()`, `printRequested()`,
`renderProcessIdChanged(qint64)`, `findTextFinished(FindResult)`,
`certificateError(CertificateErrorInfo, bool*)`,
`contextMenuRequested(ContextMenuInfo)`.

Failure pairing rule (WebKit WK09 is the model): a failed load claims
its error before `loadFinished(false)` so the engine's own error page
can never emit a fake-success pair behind it; progress notifies are
gated on the live load so the cancel-time reset never lands after the
finish.

### 4. Profile binding (Profile)

A Profile is the engine's browsing context: cookie jar, cache, storage,
settings, the request-policy hook, the download handoff.  Containers
(CONT01) are just Profiles with `ProfileOptions::dataPath`.

| Requirement | Interface surface | Per-engine reality |
|---|---|---|
| Identity | `storageName()`, `isOffTheRecord()` | Servo: name carries persona only — engine state is process-global, `isOffTheRecord` honest-false (swap path refuses private tabs).  WebKit: ephemeral session for OTR, named data/cache dirs per WK11 |
| Persona binding | `Profile::profileId()` (RC09 — `arktosProfileId` tag → storageName parse → ambient) | every profile-bound facade store keys on this; a foreign name must resolve to ambient, never crash |
| User agent | `setUserAgent` | |
| Request policy hook | `setRequestPolicy/requestPolicy` | only live when `requestInterception`/`declarativeBlocking` is true — see §5 |
| Cookie gate | `setCookieFilter(CookieAttempt)` | per-attempt functor; must be thread-safe (WebEngine invokes on the IO thread).  WebKit: no per-attempt API — policy fixed at session materialization, bit stays false |
| Script collection | `insertScript/removeScript/scripts` | `scriptInjection` bit says whether they reach pages (Servo records but never injects — honest) |
| Storage wipes | `clear(StorageAreas)` | map onto what the engine persists; unsupported areas are explicit no-ops (WebKit: VisitedLinksArea) |
| Attribute bag | `setProfileAttribute(name, value)` | free-form, namespaced per backend; unknown keys ignored |

Profile creation ownership: `createProfile` may return shared objects
(Servo's per-name cache, WebEngine's defaultProfile) — `addEngineTab`
parents only unparented profiles (tabwidget.cpp :1297).

### 5. Request policy + declarative blocking (RequestPolicy)

`Profile::setRequestPolicy` installs the app's policy engine
(`Engine::NavigationPolicy` — rustcore policy.rs with Qt fallback).  Two
honest lanes:

- **Live consult** (`caps.requestInterception`): the backend marshals
  each request into `Engine::NavigationRequest` and calls
  `RequestPolicy::decide()`.  `RequestDecision` = Allow | Block |
  Redirect (+ `referer` override, + `stubBody/stubMime` for engines
  that serve responses but cannot re-issue — Servo).
- **Declarative lane** (`caps.declarativeBlocking`, WebKit WK18):
  `RequestPolicy::contentBlockerJson()` renders the rules to Safari
  content-blocker JSON compiled into a WebKitUserContentFilterStore
  filter.  Narrower by nature: `$redirect` degrades to block,
  inexpressible rules are dropped-and-counted in `meta`, never faked.

Degradation rules a backend MUST honor: Redirect on an engine that can
only serve (no re-issue) takes the prefilled `stubBody` or degrades to
Allow; WebKit's NEW_WINDOW redirect degrades to refusal; Servo's gate
answers deny with an empty 403 reply.

### 6. Download hooks (Profile::downloadRequested + DownloadRequest)

The handoff: backend emits `Profile::downloadRequested(
Engine::DownloadRequest*)` at the one point the engine knows the
suggested name + response MIME (WebKit: `decide-destination`; refusing
there must *cancel*, not return false — a default-dir write is worse).
`TabWidget::addEngineTab` binds `DownloadManager` only when
`caps.downloads` (tabwidget.cpp :1290).

`DownloadRequest` surface: `url/suggestedFileName/mimeType`,
`accept(filePath)/cancel/pause/resume/isPaused`,
`receivedBytes/totalBytes/state/isFinished/interruptReasonString`,
split `setDownloadDirectory/setDownloadFileName` (+ getters — the name
dialog resolves before accept), `page()` (nullable; WebEngine keeps a
guarded snapshot — PRIV10), signals `stateChanged/receivedBytesChanged/
totalBytesChanged`.  Honest no-ops are required where the engine lacks
the verb (WebKit pause/resume).  `Page::download(url)` is the
chrome-initiated leg.

### 7. Permission surfacing

No `Engine::PermissionRequest` type exists yet — the surface today is:

- WebEngine: `QWebEnginePage::permissionRequested` →
  `WebPermissionManager::handleRequest(QWebEnginePermission)` (webpage.cpp :431).
- Portable path: `WebPermissionManager::handleRequest(parent, origin,
  type, resolve)` — the engine-neutral overload WebKit WK19 calls after
  mapping `WebKitPermissionRequest` subtypes (geolocation/notifications/
  pointer-lock prompt; media/device-info deny-list; EME/data-access/XR
  deny outright).
- Servo: no permission delegate exists in the shim — nothing surfaces,
  which is the honest "none".

A new backend MUST marshal into the neutral `handleRequest` overload
(or declare no-permission-surfacing); it MUST NOT grow a second
engine-typed broker.  The deny-list/prompt policy stays in
WebPermissionManager — the adapter only translates request types.

### 8. Scheme handling

The app's custom schemes are engine-serving surface, not chrome:
`arktos-file:` (directory listings + SEC02 initiator rules), `abp:`
(subscribe prompt → persona AdBlockManager), `arktos-resource:` (the
compiled `$redirect` stub table), `arktos-cert-error:` /
`arktos-http-warning:` / `arktos-site-block:` (nonce-bound
interstitials).  Contract: a backend that wants these pages registers
its engine's scheme-serving mechanism in `initialize()`/context setup
and reuses the SAME serving halves backend-0's handlers expose — never
a parallel implementation.  WebKit WK17 does this via
`webkit_web_context_register_uri_scheme` on the shared context with the
same security flags.  Servo: no scheme-serving hook → interstitial/file
flows simply never occur there (caps stay honest).

External schemes (mailto:, intent:, …): the SEC02 consent prompt is the
contract — refuse the navigation, queue the dialog, hand consent to the
desktop; iframe navs drop silently.  `file:` directory main-frame navs
reroute onto `arktos-file:`.

### 9. DevTools presence

`caps.devTools` = "an inspectable debug channel the shell can drive
exists".  Consumers: `refreshDevToolsRows` (browsermainwindow.cpp :1733)
disables both the Chromium-inspector entry and the BiDi panel entry with
a `devtools unavailable on <engine>` tooltip when the CURRENT tab's
backend reports false.

- WebEngine: true — `DevToolsWindow::inspectElement` reaches the page
  through `WebEnginePageAdapter::of()` (the escape hatch; caps.gated).
- Servo: false — servo's devtools crate speaks the Firefox remote
  protocol on a TCP listener; the BiDi panel's CDP client cannot read
  it, so no fake channel is wired (ENG35).
- WebKit: false — WK21 measured no WebKitWebInspector API export and no
  inspector gresource in the resolved build.

### 10. Extension runtime surface

`caps.extensions` — WebExtension-style add-ons.  WebEngine-only today:
the whole `src/extensions/` chain (manager, action bar, background host,
chrome.* shim, event hub, page bridge, request watcher) is engine-bound
per ENGINE.md and rides this bit.  WebKit WK21 measured
`webkit_web_extension_new` as a compiled-out `return nullptr` stub ⇒
false.  Servo: none ⇒ false.  A backend reporting false yields zero
popup-bearing extensions and every extension chrome surface stays
hidden — honest "none", not a degraded install path.

The user-script half is NOT extensions — it rides
`Profile::insertScript` behind `scriptInjection` instead.

### 11. Standard chrome actions + context menus

`Page::action(StandardAction)` returns the engine-owned QAction chrome
menus/toolbars mirror — nullptr = disabled entry, always.  Coverage is
per-backend: WebEngine maps the full set; Servo only Back + Reload;
WebKit currently none (its context menu presents WebKit's own item
model as a Qt popup via the `contextMenuRequested` marshal — WK19).
`caps.contextMenuInfo` says whether a full `ContextMenuInfo` hit-test
payload can be produced — Servo emits no `contextMenuRequested` at all.

### 12. Scripting surface

- `runJavaScript(source, cb)` — async eval.  `scriptEvaluation` bit =
  the result channel is real.  Servo's is one-way: the shim reports via
  console only, so cb is invoked with an invalid `QVariant` and the bit
  stays false.
- `runJavaScriptLifted(source, cb, worldId)` — trusted chrome-script
  injection past the JSCTL page-script gate; `worldId` selects an
  isolated world (`Engine::ArktosChannelWorld` = SEC24's private world).
  Servo: single world, id ignored.  WebKit: named worlds real, but an
  unanchored name gets a transient context — global state does not
  survive between evals.
- `toHtml(cb)` — serialized source for View Source.  Servo: empty
  callback.
- `insertScript/removeScript/scripts` per-page and per-profile —
  `InjectionPoint` maps DocumentCreation→start / DocumentReady+
  Deferred→end on two-point engines (WebKit collapses DocumentReady and
  Deferred honestly); `runsOnSubFrames`→all-frames; `worldId`→named
  world where supported.

### 13. Lifecycle, memory, misc

| Surface | Contract |
|---|---|
| `setLifecycleState/lifecycleState` (Active/Frozen/Discarded) | Frozen = "loaded but not compositing"; Discarded frees resources.  `lifecycleDiscard` bit says Discard actually frees.  WebKit: suspend/resume are dlsym-optional exports — absent ⇒ Frozen degrades to unmap+focus_out with JS timers running (honest); Discarded = terminate WebProcess + reload on wake (bf list survives).  Servo: state recorded, engine-side no-op |
| `isLoading/recentlyAudible/isOffTheRecord` | SLEEP01's suspend gate + privacy gates; no-concept answers false |
| `renderProcessId` | per-tab process identity for the memory widget; -1 = '—' (WebKit WPE 2.54 exposes no WebProcess pid, Servo is in-process) |
| `residentMemoryKb/memoryIsShared` | ENG26: in-process engines report the HOST figure and set memoryIsShared so the UI labels it instead of faking per-tab |
| `setPageAttribute(name, on)` | free-form per-page attribute bag; `perPageSettings` bit says the engine has per-page settings objects at all.  Unknown keys are ignored — WebKit's impl is deliberately inert while its WebKitSettings live per webview |
| `findText/FindResult` | async everywhere; WebKit synthesizes `activeMatch` locally (count authoritative, ordinal best-effort); Servo emits an empty result |
| `certificateError` + `CertificateErrorInfo` | SEC06: signal + AcceptOnce = session-scoped, never persisted.  `certificateOverride` bit = the engine accepts the decision.  WebKit WK19: load-failed-with-tls-errors → nonce interstitial → `allow_tls_certificate_for_host`.  Servo: no delegate — the bit stays false and the interstitial flow never runs |

## The caps path (backend → facade → chrome)

`Engine::Capabilities` fields today
(engineinterface.h :610-632 — `downloads` defaults true, everything else
false):

| Bit | Gates (adapter surface it certifies) | Chrome consumers today |
|---|---|---|
| `requestInterception` | `Profile::setRequestPolicy` live decide() consult | servo-smoke gate install (main.cpp :5495); adblock installOnProfile |
| `declarativeBlocking` | `RequestPolicy::contentBlockerJson` → compiled filter store | (WK18 lane; subresource coverage honest-shape flag) |
| `scriptInjection` | `Profile/Page::insertScript` reaches pages | — (install paths assume it; consumers degrade on the bit) |
| `scriptEvaluation` | `runJavaScript` result channel is real | engine-smoke asserts the 1+1 round-trip only when set (main.cpp :4195) |
| `cookieFilter` | `Profile::setCookieFilter` per-attempt honored | cookie-policy install paths |
| `perPageSettings` | per-page settings objects exist | JSCTL/script-control gates |
| `lifecycleDiscard` | Discarded frees resources | SLEEP01 tab-suspend decisions |
| `contextMenuInfo` | full ContextMenuInfo producible | context-menu model path |
| `certificateOverride` | cert-error AcceptOnce honored | SEC06 interstitial/padlock path |
| `devTools` | inspectable debug channel exists | refreshDevToolsRows (browsermainwindow.cpp :1733), servo-smoke (main.cpp :5633) |
| `extensions` | WebExtension runtime exists | extension chrome visibility (MS06 consumer) |
| `downloads` | `downloadRequested` handoff exists | DownloadManager install (tabwidget.cpp :1290) |

**MB04 mapping** (bridge lane — `ac_engine_caps(engine_id)` JSON,
pending).  MB04's feature names are FINER than the Capabilities bits —
the rustcore table is what MS05/MS06 chrome gating reads, so the doc
maps each MB04 feature to the adapter surface that must deliver it and
the C++ bit that certifies it today:

| MB04 feature | Adapter surface that delivers it | C++ certifier |
|---|---|---|
| `devtools` | DevToolsWindow inspect / BiDi channel | `devTools` |
| `pdf_viewer` | built-in viewer (Chromium PDFium) OR shell PdfView stack-over (EngineTab::showPdfDocument — shell-owned, every backend gets it) | partial-on-non-Chromium by design |
| `extensions_mv2` / `extensions_mv3` | src/extensions/* chain | `extensions` (MV3 partial even on Chromium — Bitwarden-class is `partial` at best per MB04 spec) |
| `webchannel` | QWebChannel transport the arktos-channel.js/autofill-capture/reader assets need | WebKit WK16: message-handler transport exists, QWebChannel wire shim does NOT — page assets inject inertly ⇒ partial |
| `otr_profile` | `ProfileOptions::offTheRecord` real isolation | Servo false (shared context); WebKit ephemeral session true |
| `tor` | SOCKS5 + tor profile path | Chromium only (ENG05 — swap path refuses) |
| `containers` | per-ProfileOptions::dataPath isolation | Servo false |
| `reader_mode` | reader driver via runJavaScriptLifted + channel world | needs scriptEvaluation+webchannel legs |
| `downloads` | DownloadRequest handoff | `downloads` |
| `sessions` | per-tab history serialize/restore (AcSession facade) | engine-serialized blobs are WebEngine-bound — partial elsewhere |
| `favicons` | `iconChanged` emission | WebKit opt-in favicon db; Servo none |
| `opensearch` | provider install/search URLs | engine-neutral (app-side) ⇒ supported |
| `site_permissions` | §7 surfacing path | WebEngine full / WebKit partial / Servo none |
| `autofill` | autofill.js DOM fill + capture bridge | partial on WebKit (DOM fill runs, capture path is WebEngine-bridge-gated); none on Servo |

Until MB04 lands, chrome keeps reading `Backend::capabilities()`
directly — MS05/MS06 are gated on `needs:task:MB04` for exactly this
reason.  The rustcore table MUST be generated from the same honest
source (consult each backend's `capabilities()` + the spike-verified
notes above), never a hand-copied matrix that can drift.

## Per-engine matrix (measured, not aspirational)

`Engine::Capabilities` as each `capabilities()` reports it today:

| Bit | webengine (Chromium 140) | servo | webkit (WPE 2.54) |
|---|---|---|---|
| requestInterception | ✔ | ◐ only if artifact exports `se_set_load_gate` | ✔ navigation-level veto (NEW_WINDOW redirect→refuse; response lane observe-only) |
| declarativeBlocking | ✘ | ✘ | ✔ WK18 filter-store; $redirect→block, inexpressible dropped+counted |
| scriptInjection | ✔ | ✘ (recorded, never injected) | ✔ 2 injection points, named worlds |
| scriptEvaluation | ✔ | ✘ one-way, cb gets invalid QVariant | ✔ `_with_reply` dlsym-gated (one-way without it) |
| cookieFilter | ✔ | ✘ | ✘ 3-state session policy only (WK12); per-host exceptions ungated |
| perPageSettings | ✔ | ✘ | ✔ (WebKitSettings per webview; free-form key bag inert) |
| lifecycleDiscard | ✔ | ✘ (state stored only) | ✔ Frozen=suspend iff export present else unmap+focus_out; Discard=kill+reload |
| contextMenuInfo | ✔ | ✘ | ✔ WK19 hit-test marshal + WebKit-item popup |
| certificateOverride | ✔ | ✘ | ✔ WK19 session-scoped `allow_tls_certificate_for_host` |
| devTools | ✔ | ✘ (Firefox-remote-proto only) | ✘ WK21: no inspector API/gresource |
| extensions | ✔ | ✘ | ✘ WK21: extension API is a compiled-out stub |
| downloads | ✔ | ✘ (no delegate) | ✔ decide-destination handoff; pause/resume no-ops |

Contract-area notes per engine:

- **webengine** — reference backend; every bit true.  Escape hatches
  `WebEnginePageAdapter::of/forPage`, `WebEngineProfileAdapter::of`,
  `WebEngineDownloadRequest::of` carry the migration tail.
- **servo** — near-empty table by design (ENG03 spike verdict).  Adds:
  Tor refusal (no SOCKS5 — swap path refuses), shared single context
  (`isOffTheRecord` always false), in-process memory reporting with
  `memoryIsShared`, poisoned-view recovery via `restartRequested` →
  `EngineTab::restartEngine()`, `pageUnavailableReason` on dead engine,
  `engineNote` (experimental/debug-artifact warning), N concurrent
  views post-ENG36.  Standard actions: Back+Reload only.
- **webkit** — compile-but-inert when unlinked (initialize() false ⇒
  never registers; caps all-false).  Linked: the WK08-WK19 stack above.
  Gaps beyond the bits: `renderProcessId`=-1, `action()` all-nullptr,
  `findTextFinished` count-authoritative/ordinal-best-effort,
  response-policy observe-only, unanchored world eval transient,
  QWebChannel-dependent assets (arktos-channel.js, autofill capture,
  reader) inert until a transport shim lands.  Cookie policy edge:
  KeepUntilTimeLimit's 90-day cap inexpressible (persists like
  KeepUntilExpire); VisitedLinksArea wipe is a no-op.

## Rules for a new backend (checklist)

1. New TU, `Engine::Backend` subclass + self-registering `addProbe`
   static — never edit engineregistry.cpp for a conditional engine.
2. `initialize()` = the loadable probe; false ⇒ hide, never fake.
3. `capabilities()` reports measured reality; artifact-dependent bits
   resolve through symbol probes (dlsym/dlopen pattern), not
   compile-time assumptions.
4. `createPage` returns nullptr rather than manufacturing a doomed tab;
   `pageUnavailableReason` carries the human text.
5. Implement Page/Profile/DownloadRequest virtuals fully — honest
   no-op + false bit beats a faked half-surface.  Unmapped
   StandardActions return nullptr.
6. Marshal permission requests into
   `WebPermissionManager::handleRequest` (neutral overload); unknown
   types deny, never blanket-grant.
7. Reuse the backend-0 serving halves for custom schemes; no parallel
   implementations.
8. Emit the §3 signal set for what the engine can report; a missing
   surface stays silent, never fakes a pair.
9. `profileId()` persona binding must resolve per RC09 — facade stores
   (history/settings/decisions) key on it; a broken binding cross-
   contaminates personas.
10. Update this doc's matrix in the same commit that lands the backend.

Verify (this task): doc-only — `src/engine/engineinterface.h`,
`enginetab.h`, `engineregistry.h`, all three backends' headers and
`capabilities()` bodies, plus caps consumers (browsermainwindow.cpp
:1733, tabwidget.cpp :1290, main.cpp --engine-list/--engine-smoke)
read for the mapping; no code changed.
