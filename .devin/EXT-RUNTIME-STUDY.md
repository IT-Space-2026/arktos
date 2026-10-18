# EXT-RUNTIME-STUDY — an Arktos-hosted Chrome-extension runtime (study only)

Date: 2026-10-18 · Status: study, no code · Question: can Arktos implement
enough `chrome.*` surface *itself* — on engine-neutral primitives — that a
real-world MV3 extension (Bitwarden is the benchmark) actually works?

Motivation: Qt WebEngine's extension support is fixed at compile/registration
time. The namespace schemas are in the binary, but Qt's embedder never
registered the browser-side delegates for `scripting`, `webNavigation`,
`tabs` events, `contextMenus`, `notifications`, `sidePanel`, `windows`,
`idle`, `offscreen`, and more (EXT11 measured the boundary; EXT21 grades
extensions that require them `NotSupported`). We cannot turn them on.
**But Arktos owns the browser.** Every surface those delegates would have
fed — tab model, page JS injection, context menus, notifications, event
streams — is already ours.

## What already exists (EXT14–EXT19 built half of this)

The hosted runtime is not a hypothetical — it runs today:

- **Hidden host page** per enabled extension on each prepared profile
  (`ExtensionBackgroundRuntime`), which *is* the background context: the
  service-worker JS runs inside it.
- **JS shim preamble** injected into the host page and into sibling
  `chrome-extension://` pages (popup, options) — provides `chrome.runtime`,
  `chrome.storage.local/.session/.managed`, `chrome.alarms`,
  `chrome.tabs`/`chrome.windows` **query** snapshot, `chrome.action`,
  `chrome.i18n`, `chrome.permissions`, `chrome.contextMenus` *registry*
  (create/update/remove recorded; EXT16 renders contributed items),
  `chrome.sidePanel` option bookkeeping, `chrome.notifications`
  (typed-`unsupported` gap), `chrome.runtime` context bookkeeping.
- **Dispatch**: `invoke()` routes `ns.method` calls to C++ handlers in
  `extensionshimbackend.cpp` (~2k LOC); 12 namespaces wired today.
- **Page-side bridge**: `__ARKTOS_HOST_CONFIG` + QWebChannel transport
  self-wires on any extension page load (EXT19).
- **Honesty plumbing**: `ExtensionInfo.health`/`healthReason`,
  `shimGaps` (typed `unsupported`/`unimplemented` replies are recorded,
  not silently swallowed), `classifyHealth` — the degradation story is
  already built.

This is the skeleton. The study question is what it costs to go from
"popup renders" to "extension functions."

## Bitwarden's actual API surface (measured, not guessed)

`chrome.*`/`browser.*` call counts extracted from the installed bundle
(`background.js`, `popup/main.js`, `offscreen-document.js`, content
scripts), deduplicated by namespace:

| namespace | calls | we have | gap |
|---|---|---|---|
| `runtime` | 201 | messaging, getURL, contexts | mostly OK |
| `tabs` | 90 | **query** snapshot | `sendMessage`, `create`, `update`, and **all events** (`onCreated`/`onUpdated`/`onRemoved`/`onActivated`) missing |
| `windows` | 38 | query snapshot | events missing; less critical than tabs |
| `storage` | 38 | local/session/managed | OK (sync not offered) |
| `scripting` | 30 | **nothing** | `executeScript` — the *whole product*; can be backed by our own `runJavaScript` per tab/frame |
| `webNavigation` | 26 | **nothing** | we have load signals; `onCommitted`/`onDOMContentLoaded`/`onCompleted` emittable |
| `privacy` | 24 | **nothing** | Bitwarden queries `network.webRTCIPHandlingPolicy` etc.; honest answers + settings mapping |
| `alarms` | 20 | host QTimers, onAlarm fires | OK |
| `offscreen` | 17 | **nothing** | Bitwarden keeps its crypto (Argon2 WASM) in an offscreen document; hostable as another hidden page — the runtime already knows how |
| `webRequest` | 14 | **nothing** | observe-only events mappable from `QWebEngineUrlRequestInterceptor` (we see request start/redirect); **blocking/mutation is NOT possible** — the interceptor can only skip, not rewrite headers; Bitwarden uses it mainly for auth-token refresh observation, not blocking |
| `permissions` | 13 | bookkeeping | OK |
| `notifications` | 12 | typed-`unsupported` | a native `QSystemTrayIcon`/freedesktop presenter is a small, self-contained piece |
| `extension` | 12 | not dispatched | MV2 legacy namespace — `getBackgroundPage`, `getViews`; composable over the host page |
| `sidePanel` | 9 | option bookkeeping | needs a real sidebar surface to matter |
| `i18n` | 8 | locale fallback | OK |
| `commands` | 8 | **nothing** | keyboard-shortcut registry; mappable to QShortcut |
| `management` | 6 | **nothing** | self-inspection (`getSelf`); trivially answerable |
| `contextMenus` | 6 | registry + rendering | the `onClicked`→`executeScript` fill path needs `scripting` to land first |
| `contentScripts` | 6 | **nothing** | dynamic content-script registration; same injection primitive as `scripting` |
| `browserAction`/`action` | 3+3 | popup wiring | OK |
| `idle` | 2 | **nothing** | `queryState`/`onStateChanged` — lock-on-idle; a QTimer + input watcher |
| `declarativeNetRequest`, `webRequestAuthProvider` | declared | n/a | Bitwarden survives without them — its request-filter use is observational |

### The two load-bearing primitives

Everything above reduces to **two capabilities we don't yet provide**:

1. **Script execution into web pages** (`scripting`, `contentScripts`,
   context-menu fill, autofill). Bitwarden's flow: content script on the
   page posts "fill this field" ← background decides ←
   `scripting.executeScript` pushes the fill payload into the page's
   isolated world. We can emulate this: the runtime already injects a
   preamble into *extension* pages; a page-side agent (declared
   `content_scripts` — **the engine already injects these natively**,
   EXT11 measured them live) plus `WebView::page()->runJavaScript` for
   the dynamic path. The hard part is **isolated-world fidelity** —
   `runJavaScript` evaluates in the main world; Bitwarden's
   `content-message-handler` runs isolated and talks to the page via
   `window.postMessage` + DOM events, which actually *works* across the
   boundary (postMessage crosses worlds). Doable; needs verification
   that Bitwarden's handshake doesn't check world identity.
2. **Event streams** (`tabs.on*`, `webNavigation.on*`, `windows.on*`,
   `idle.onStateChanged`, `runtime.onStartup/onInstalled`). Pure
   browser-chrome work — `TabWidget` signals, `WebView` load signals,
   a global input-idle watcher — marshaled into the host page as
   `chrome.events` dispatches. Unambiguous, bounded, testable.

`offscreen` is the third piece of the *mechanism* — Bitwarden's unlock
runs Argon2/WASM in an offscreen document; a hidden host page is a
document, so `createDocument({reasons:[...]})` maps onto "spawn another
host page, give it the shim." The runtime already knows how to make one.

## Design options

### Option A — extend the hosted runtime (recommended)

Keep the EXT14 architecture; grow the dispatch table and add the two
primitives:

- **Page-agent script** (`QWebEngineScript`, injection point
  `DocumentCreation`, world `MainWorld`+a compat world): provides
  `chrome.runtime.sendMessage`→host relay inside web pages so
  extension content scripts that *do* get injected (declared ones work
  natively!) can reach the background. This also unlocks
  `tabs.sendMessage`.
- **Dynamic injection handler**: `scripting.executeScript` →
  per-frame `runJavaScript` with a world-emulation preamble; results
  marshaled back over the channel (one-way values only — matches the
  MV3 contract).
- **Event hub v2**: `ExtensionEventHub` already tracks tab/window
  lifecycle for EXT16; extend to emit `tabs.on*`/`webNavigation.on*`
  payloads into each host.
- **`offscreen` host pages**: second-class host pages keyed by
  extension+document-URL, with `hasDocument`/`closeDocument`.
- **`notifications`** → native presenter; **`idle`** → input watcher;
  **`commands`** → QShortcut registry; **`management`/`extension`** →
  introspection sugar.

Cost shape: incremental, each namespace independently shippable, each
verifiable against Bitwarden's next failing call (the shim's typed-gap
recording *tells us* the next unimplemented method live).

### Option B — full custom runtime, engine-neutral

Same as A but the dispatch talks only `Engine::Page` primitives, so a
future WebKit/Servo backend could host extensions too. More upfront
abstraction (page-injection, popup hosting, and request observation must
be expressed through the engine interface); the honest sequencing is
**A first** — prove Bitwarden works on QtWebEngine where primitives are
richest — then generalize to B when the surface is proven, so we don't
design abstractions against an untested contract.

### Option C — patch QtWebEngine to register the real delegates

The extension schemas exist in `QtWebEngineCore`; the missing namespaces
are unregistered *delegates*, not absent code. Registering them means
maintaining a patched QtWebEngine build — a Chromium-scale CI artifact
we'd have to build, ship, and security-track forever. **Rejected** — same
reason we don't fork QtWebEngine elsewhere.

### Option D — native Bitwarden adapter (alternative, not runtime)

Skip the extension problem entirely: implement the *function* (unlock,
sync, fill) natively via Bitwarden's API/`bw` CLI + our own page
injection + autofill UI. ~85–90% of the user-facing value
(vault+fill+gen+TOTP), no MV3 security-boundary burden, works on **every
engine**. Loses: passkey provider, biometric/nativeMessaging unlock,
org/enterprise edge cases, and it serves exactly one extension.

## The honest caveat — we own the security boundary

Today, whatever Chromium *does* enforce for extensions comes free. A
hosted runtime inherits:

- **Host-permission scoping**: `host_permissions: ["*://*/*"]` must
  actually scope which pages the extension may inject into — our
  injection path must check it; Chromium's is gone.
- **World isolation**: the main-world `runJavaScript` path leaks
  extension function into page scope; a malicious page could potentially
  detect or interfere with the fill agent. Acceptable for a first pass
  (Bitwarden fills into *fields the user asked it to fill*), must be
  documented and ideally moved to a real `QWebEngineScript` world.
- **Permission consent**: EXT05's review flow exists; the runtime must
  honor it per-namespace at call time, not just at install.
- **Storage isolation**: `chrome.storage` must stay per-extension and
  per-profile — the current shim already scopes it; keep it that way.

None of these are blockers; all of them are "we are now the security
implementation" — the thing a study must say plainly.

## Recommendation

**Option A**, sliced as a lane like the WebKit one:

| phase | work | verifiable by |
|---|---|---|
| E1 | page-agent script + `tabs.sendMessage`/`onMessage` relay into web pages | Bitwarden content script reaches background |
| E2 | `scripting.executeScript` (one-shot, world-emulated) | fill payload lands in a test form |
| E3 | event streams: `tabs.on*`, `webNavigation.on*`, `runtime.onStartup` | unlock→fill flow drives a real login form |
| E4 | `offscreen` host page | Argon2 unlock succeeds in-session |
| E5 | `notifications`, `idle`, `commands`, `management`, `extension` | Bitwarden stops emitting typed `unimplemented` gaps |
| E6 | security pass: host-permission scoping on injection, per-call consent | hostile-fixture extension denied |
| E7 | Bitwarden end-to-end: install→unlock→autofill on a live login | the actual product works |
| E8 | *(later)* generalize to `Engine::Page` primitives for WebKit (Option B) | same runtime over a second backend |

Decision gate sits after E3: if the content-script handshake survives
world emulation and events drive the fill path, the remaining work is
mechanical. If Bitwarden's messaging requires true isolated-world
identity, that surfaces at E1–E2 early and cheaply — before E4–E6 is
spent.

Cross-reference: `.devin/WEBKIT-STUDY.md` (engine side),
`.devin/EXT11-report.md` (measured support boundary),
ChangeLog EXT14–EXT21 (what the hosted runtime already does).
