# EXT11 — Extension-host diagnosis (Qt 6.12.0 / Chromium 140)

Platform-wide MV3 health check + Bitwarden root-cause + cheap-lever
sweep. Diagnosis-only slice; the only code that landed is the
`--mv3-health-smoke` diagnostic harness in `src/main.cpp`.
Reproduce: `cd src && make && ../arora --mv3-health-smoke`
(headless-safe; results are `mv3-health-smoke:` qInfo lines).

## Method

A purpose-built fixture extension (`arora-mv3-health`, MV3 manifest
carrying Bitwarden's full permission set) is installed + enabled on the
isolated smoke profile, then measured from inside its own JS contexts:

- content script on a loopback page — reports its API slice into DOM
  attributes (isolated world can't share `window`);
- the same page runs `navigator.serviceWorker.register` as the
  page-level control;
- a `sendMessage` round-trip content-script → background proves the SW
  alive (the SW's reply carries its own self-report — scope type, API
  matrix);
- the action popup runs a timed call battery (storage set/get,
  tabs.getCurrent/query, sendMessage→SW, runtime.getContexts);
- the bundled Bitwarden row is probed the same way, twice: as found,
  then after `setExtensionEnabled` + reload — separating "disabled
  context serves no `chrome.*`" from "enabled but the surface is
  missing".

## Platform verdict — measured matrix

MV3 fundamentals DO work on Qt 6.12.0 WebEngine. The fixture's
background runs as a real `ServiceWorkerGlobalScope`
(`isServiceWorkerScope:true`), answers `runtime.sendMessage` from both
the content script and the popup (`pong:true` carrying the SW's own
report), `storage.local` round-trips across contexts
(`set`→`get` returns the SW's stored report), content scripts inject,
and the action popup loads `chrome-extension://` with extension APIs.
Page-level `navigator.serviceWorker.register` also works — the control
is green.

Present in extension contexts: `chrome.runtime`
(id/getManifest/getURL/sendMessage/onMessage/onConnect/getContexts/
getBackgroundPage/openOptionsPage), `storage.local.*` AND
`storage.session`/`storage.managed` objects, `webRequest`
event objects (observe-only), `alarms.*`, `idle.*`, `offscreen.*`,
`i18n.getMessage`, a `chrome.tabs` object exposing `update` only.

Missing (measured `undefined`): `chrome.extension` (whole namespace),
`tabs.getCurrent`/`query`/`get`/`create`/`sendMessage`/`onUpdated`/
`onRemoved` (the tabs object exists but is nearly bare),
`webNavigation`, `scripting`, `contextMenus`, `sidePanel`,
`notifications`, `declarativeNetRequest`, `permissions`, `action`,
`windows`, `cookies`, `bookmarks`, `identity`, `gcm`,
`nativeMessaging`, `privacy`, `topSites`, `history`. In
content-script context even `chrome.tabs` is `undefined`.

Verdict: extensions whose needs fit that subset (content scripts,
storage.local, runtime messaging, alarms/idle/offscreen, popup pages)
are functional. Anything needing tabs lookup, navigation events,
scripting, notifications, context menus, declarativeNetRequest,
windows or the legacy `chrome.extension` bridge degrades or dies —
Bitwarden needs ~8 of the missing namespaces.

## Bitwarden root cause — measured

Fresh smoke profile: `installed=1 loaded=1 enabled=1` — the bundled
default-enable path works. Two independent breaks:

1. **Background SW dead.** `runtime.getContexts` from the enabled
   popup lists a `BACKGROUND` context, but `sendMessage` to it times
   out — no live `onMessage`. Matches the earlier live-log "Service
   worker registration failed. Status code: 15" = Chromium
   `kErrorScriptEvaluateFailed`: `background.js` throws during initial
   evaluation. Member-level probe of the SW context shows the exact
   kill chain: `chrome.webNavigation` is undefined, so Bitwarden's
   guarded `'webNavigation' in chrome` check takes its fallback —
   `EU.addListener(chrome.tabs.onUpdated, …)` — and `tabs.onUpdated`
   is ALSO undefined (the `tabs` object exists but is bare: no
   events, no get/query). Unguarded `chrome.notifications.*`,
   `chrome.windows.*`, `chrome.contextMenus`, `chrome.scripting` and
   `chrome.extension.getViews` accesses compound it — any one is a
   fatal TypeError at eval time. `storage.session`/`managed` and
   `runtime.onConnect` — which Bitwarden also uses — ARE present,
   so the eval-killers are the event/tabs/navigation surfaces.
2. **Popup bootstrap dies.** `polyfills.js` calls
   `chrome.extension.getBackgroundPage()` — `chrome.extension` is
   `undefined` (the legacy namespace isn't surfaced at all) — and
   `chrome.tabs.getCurrent` is missing. Angular never bootstraps:
   `bodyTextLength:0`, permanent spinner.

Bitwarden's content scripts do inject (manifest-declared injection is
engine-level and fixture-proven), but they bridge to the dead
background, so autofill cannot function end-to-end — the README claim
"the core autofill content scripts work" was optimistic.

**Disabled-vs-broken (measured side finding):** a DISABLED extension's
`chrome-extension://` pages serve HTML with **no `chrome.*` at all** —
run 2 of the smoke caught Bitwarden disabled (enabled state written by
`setExtensionEnabled` lives in Chromium ExtensionPrefs, flushed at
profile teardown; the smoke's `quick_exit` never flushes — a harness
artifact, clean-exit persistence is covered by tst_extensionmanager
legs). Any "the extension is dead" diagnosis must check
`info.enabled` first — a disabled row masquerades as total API
absence.

## EXT08 popup host — ruled out

The probe used EXT08's production shape verbatim: plain
`QWebEnginePage` on the browsing profile, no arora channel/page
policy. The popup loads (`loadFinished` PASS), serves
`chrome-extension://` correctly, and receives the enabled-extension
API context. All observed failures originate inside the extension's
own scripts/worker — the host is not the breakage.

## Cheap levers — tried, none exist

- `QTWEBENGINE_CHROMIUM_FLAGS="--enable-experimental-extension-apis"`:
  full re-run, byte-identical matrix.
- `QWebEngineSettings::WebAttribute`: all 40 attributes enumerated —
  none extension-related.
- `QTWEBENGINE_*` env vars (binary strings): no extension knob.
- `QWebEngineExtensionManager` public API: install/uninstall/
  enable/disable + installPath only — no surface options.
- API schemas for `webNavigation`, `contextMenus`,
  `declarativeNetRequest`, `tabs.create` etc. ARE compiled into
  `libQt6WebEngineCore.so` but unregistered by Qt's embedder — the
  missing surface is an implementation gap inside
  QtWebEngineCore, not a runtime gate. No app-side flag or setting
  can add a `chrome.*` namespace to extension contexts.

## Decision for EXT12

**No narrow stub set makes Bitwarden functional.** Its blocking gaps
(background `webNavigation`/`tabs` events + `notifications` +
`windows` + `scripting` + `contextMenus`/`action`, popup
`chrome.extension.*` + `tabs.getCurrent`/`query`, plus the entire
vault IPC through a live SW) live inside QtWebEngineCore — the app
cannot inject into an extension's service worker, and a popup-side
`chrome.extension` shim would produce a fake-alive shell over a dead
vault. Recommendation:

- **Honest degradation, not fake-alive**: detect never-ready MV3
  background (registered context but unanswered ping, or install/
  registration error), mark the row in Settings → Extensions with a
  "Limited"/"Not supported" state + reason (missing `chrome.*`
  surfaces), not naked "Enabled".
- **Popup honesty**: the engine's own spinner can't be intercepted,
  but the Preferences row + action-bar tooltip can carry "this
  extension needs APIs Qt WebEngine doesn't provide".
- **Same treatment for every extension failing the check**, not just
  Bitwarden — the fixture matrix gives the capability probe shape.
- `chrome.runtime.getBackgroundPage` IS present (popup-side) — a
  `chrome.extension → chrome.runtime` alias is the only genuinely
  cheap compatibility shim, worth doing as part of degradation UX if a
  polyfill point exists, but it does NOT unblock Bitwarden.

Platform-wide answer to "if Bitwarden doesn't work, will anything?":
yes — the MV3 subset above is real and exercised end-to-end, but the
missing surface is large enough that full-featured extensions
(password managers, tab managers, anything declarativeNetRequest)
cannot run until upstream Qt implements the embedder-side APIs.
