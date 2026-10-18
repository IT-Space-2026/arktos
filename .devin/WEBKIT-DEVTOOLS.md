# WKB04 — WebKit backend devtools surfaces (research)

Feeds the webkit spine's WK19 `Capabilities.devTools` decision and the
WK21(c) stretch audit (`WebKitWebInspector` embed probe). Scope: WPE
WebKit 2.46+ on the WPEPlatform API — NOT QtWebKit, NOT WebKitGTK. Doc
only; no code changed.

## What we already have (the thing any WebKit answer must slot into)

- `DevToolsWindow` (src/devtoolswindow.*) — a top-level QMainWindow
  hosting a `QWebEnginePage` bound via `setDevToolsPage()`; Chromium
  DevTools inside our chrome, reached from Tools > Development Tools
  > "Chromium Dev Tool". This is the *separate-window* precedent.
- `BidiPanel` + `BidiClient` (src/bidipanel.*, src/bidiclient.*) — the
  engine-neutral dock. Qt side speaks WebDriver-BiDi-shaped JSON plus
  a vendor `arora.cdp` passthrough; `rustcore/src/bidi.rs` translates
  that surface onto CDP over Chromium's loopback remote-debugging
  socket (armed process-wide as `BrowserProfile::bidiDebugPort()`).
  The Qt side is protocol-agnostic — it ships opaque method+JSON
  through `rc_bidi_command` and renders whatever answers.
- Consumed command surface (enumerated from the panel): BiDi
  `session.new/subscribe/end`, `browsingContext.getTree/navigate/
  close`, `script.evaluate`, `storage.getCookies`,
  `network.setCacheBehavior`, event groups browsingContext / log /
  network / debugger — plus `arora.cdp` raw: `DOM.getDocument`,
  `DOM.requestNode`, `DOM.getAttributes`, `DOM.setAttributeValue`,
  `DOM.getBoxModel`, `CSS.enable/getComputedStyleForNode`,
  `Page.getResourceTree/getResourceContent` + `frameNavigated`,
  `Runtime.evaluate/getHeapUsage/releaseObjectGroup`,
  `Debugger.enable/pause/resume/step*/setBreakpointByUrl/
  removeBreakpoint/getScriptSource` + `scriptParsed/paused/resumed/
  breakpointResolved`, `Performance.getMetrics`, `Profiler.start/stop`,
  `HeapProfiler.startSampling/stopSampling`, `Security.enable`.
- Per-domain honesty is already the contract: the panel probes domains
  per context and honest-disables tabs whose domain doesn't answer —
  so a partial WebKit mapping degrades cleanly instead of faking.

## (a) `WebKitWebInspector` embed surface on WPE

**The GTK inspector API does not exist on WPE.** The whole
`WebKitWebInspector` object — `attach`/`detach`/`open-window` signals,
`webkit_web_inspector_get_web_view()`, attached-height — is absent from
the `wpe-webkit-2.0` API index (checked against the 2.52 reference).
GTK's attach hook worked because the handler returned a `GtkWidget` to
repack; WPE has no toolkit widgets, so none of that machinery is
exported. There is no GLib handle to the inspector's frontend view on
WPE.

**The only WPE entry point is `webkit_web_view_toggle_inspector()`**
(since 2.46, WPEPlatform API only — a warned no-op on the legacy
libwpe backend). Reading `WebInspectorUIProxyWPE.cpp` upstream:

- The frontend is a second `WKWPE::ViewPlatform` created on the
  inspected view's `WPEDisplay`, bound to its **own `WPEToplevel`**
  (reuses the view's toplevel when present, else
  `wpe_display_create_toplevel()`), titled "Web Inspector — <url>",
  torn down via the WPEView `closed` signal. Frontend data lives in a
  dedicated `wpewebkit-*/WebInspector` data store.
- `platformAttach`, `platformDetach`,
  `platformSetAttachedWindowHeight/Width` are all `notImplemented()`.
  **Attach is impossible on WPE — the local inspector is always a
  detached window.** Answering the task's question directly: it cannot
  attach to our view; it needs its own toplevel.
- Requires `WebKitSettings:enable-developer-extras=TRUE` on the
  inspected view.
- Where that toplevel lands is *our* problem: on a real wayland/drm
  WPEDisplay it pops a free OS window (DevToolsWindow UX parity); on a
  headless/custom display the frontend exists but nobody paints it
  unless our display glue surfaces it. One real embed hook exists: the
  toplevel is created through `wpe_display_create_toplevel()` **on our
  display object**, so a custom `WPEDisplay` subclass observes that
  vfunc and can route the frontend view's buffers into a Qt dock
  instead of an OS window. Verify against the resolved WPE version in
  the WK21 spike.

**`inspector://` is the *remote* inspector client scheme, not the
local one.** Setting `WEBKIT_INSPECTOR_SERVER=ip:port` (env, before
web-context init — same arming pattern as `bidiDebugPort`) makes the
UI process listen; opening `inspector://ip:port` in any WebKit view
loads the bundled remote-inspector frontend, which connects to the
socket, lists all debuggable targets, and navigates into the full
inspector for the picked target. The scheme is resolved internally by
WebKit — it is not a `register_uri_scheme` surface and likely bypasses
`decide-policy`. The local frontend itself is
`resource:///org/webkit/inspector/UserInterface/Main.html` (GResource).
`WEBKIT_INSPECTOR_HTTP_SERVER` is the http:// variant for non-WebKit
clients.

**Cheap embedded variant worth a spike leg:** since `inspector://`
runs in any WebKit view, once the server is armed a plain WebkitView
hosted in a DevToolsWindow-shaped window (or dock) pointed at
`inspector://127.0.0.1:PORT` gives the *full* WebKit inspector inside
our chrome with zero platform glue and no version-skew risk (the
client is our own lib). Caveats to audit: the target list is
process-wide — every webkit view including OTR ones is a debuggable;
the env var is process-wide on/off, so arm it only when the user asks
for devtools (and never in Tor windows, which stay webkit-forbidden
pending WK21a anyway).

## (b) Protocol delta vs our BiDi/CDP panel

Two deltas, and they land in different places:

**Transport — differs.** Chromium gives a `ws://` endpoint on
`--remote-debugging-port`; WebKit's remote inspector is a *custom
socket protocol* on the `WEBKIT_INSPECTOR_SERVER` socket — a
length-framed JSON channel with a target-list handshake
(`SetupInspectorClient` → `SetTargetList` → `Setup`/`FrontendConnected`
→ `SendMessageToBackend/Frontend`), moved off D-Bus circa 2.30. It is
an internal, version-coupled contract (MiniBrowser is the reference
client) — not raw CDP-over-WebSocket, and not public API. Verify the
wire format against the resolved WPE version at spike time; do not
build on assumptions here.

**Schema — close but drifted.** The WebKit inspector protocol is
CDP's ancestor (Blink forked WebKit's inspector in 2013): same
`{"id":N,"method":"Domain.command","params":{}}` envelope and
`Domain.event` notifications, and a large fraction of our consumed
command names survived verbatim:

| Our surface (BiDi / arora.cdp) | WebKit inspector protocol | Verdict |
|---|---|---|
| `script.evaluate` | `Runtime.evaluate` | maps — minor param drift |
| `browsingContext.getTree` | remote-inspector `SetTargetList` | maps — the handshake *is* the tree |
| `browsingContext.navigate` | `Page.navigate` | maps |
| `network.*` event group | `Network.requestWillBeSent / responseReceived / loadingFinished / loadingFailed / requestServedFromCache` | maps — same event names, payload drift |
| `log.entryAdded` | `Console.messageAdded` / `Runtime.consoleAPICalled` | maps |
| `network.setCacheBehavior` | `Network.setResourceCachingDisabled` | maps |
| `DOM.getDocument/requestNode/getAttributes/setAttributeValue/getBoxModel` | `DOM.*` same names | maps — inherited names |
| `CSS.getComputedStyleForNode` | `CSS.getComputedStyleForNode` | maps |
| `Page.getResourceTree/getResourceContent` + `frameNavigated` | same names | maps |
| `Debugger.*` + `scriptParsed/paused/resumed/breakpointResolved` | `Debugger.*` same names | maps ~1:1, param drift |
| `Runtime.getHeapUsage` | no direct match — `Heap` GC stats or eval | translate or degrade |
| `HeapProfiler.startSampling/stopSampling` | `Heap.startSampling/stopSampling/getSamplingProfile` | maps — clean analog |
| `Profiler.start/stop` | `ScriptProfiler.startTracking/stopTracking` | maps — different sample shape |
| `Performance.getMetrics` | **none** (Timeline domain is a different thing) | honest-disable |
| `Security.enable` | **none** | honest-disable — our Security tab is Arora-sourced (TlsVerifier) anyway |
| `storage.getCookies` | `Page.getCookies`-era leftover or skip the protocol — `WebKitCookieManager` answers it embedder-side | trivial either way |

**Verdict: a second implementation, not a pluggable transport.** The
Qt side needs zero changes — it already emits BiDi-shaped JSON and
`arora.cdp` blobs blindly. The work is a second backend inside
rustcore behind the same `rc_bidi_*` contract, speaking the
remote-inspector socket framing + WebKit inspector protocol, mapping
`arora.cdp` CDP names onto the mostly-homonymous WebKit domains and
erroring honestly on the gaps (the panel renders those as
honest-disabled already). Bounded, but a real task of its own — same
class of work as the CDP translator, discounted by the shared
ancestry.

## (c) WPE WebDriver (`webkit-webdriver`)

Ships with WPE WebKit. Browser side: `webkit_web_context_set_
automation_allowed(ctx, TRUE)` + `WebKitWebContext::automation-started`
→ `WebKitAutomationSession` (embedder sets capabilities; `will-close`
cleanup signal since 2.46). The driver talks to the browser over the
*same* remote-inspector socket infrastructure — automation rides the
RemoteInspector channel, which is why `WEBKIT_INSPECTOR_SERVER` and
the automation allowance are siblings.

What it gives for free: full W3C WebDriver — sessions, navigate,
element find/click/keys, `execute script`, screenshots, cookies,
window/timeout management. That is already enough to drive
WK04-style input+eval legs and any external QA tooling — worth
adopting as the webkit backend's autotest driver regardless of the
devtools decision. It is **not** a devtools channel: classic WebDriver
has no DOM-inspection / network-event / debugger surface.

WebDriver-BiDi upstream is mid-landing (WPE `BidiBrowserAgent` enabled
Apr 2025; single-session; module coverage still growing against wpt).
If the resolved build's webdriver advertises `webSocketUrl`, rustcore's
BiDi client could attach **with no translator at all** — but only the
spec-BiDi slice would answer (browsingContext / script / log /
partial network); the `arora.cdp` tabs (Elements tree, Sources
debugger, Performance/Memory) stay honest-disabled because BiDi has no
DOM/CSS/Debugger domains. It also adds a managed webdriver process to
the lifecycle. Spike-leg material, not a WK19 dependency.

## (d) Recommendation: `devTools = false` first, inspector-embed path second

**WK19 should report `caps.devTools = false`.** Today the flag is only
surfaced by `--engine-list` and the honest-degradation contract — no
consumer would light up (the Chromium action is WebEngine-adapter-bound;
the BiDi panel would list a WebKit tab as "no debug target" anyway) —
but `true` would claim a channel that nothing can open. That is
precisely the lie the capability table exists to prevent.

Then, in order:

1. **WK21 spike — `inspector://` loopback (cheapest real win).** Arm
   `WEBKIT_INSPECTOR_SERVER=127.0.0.1:PORT` +
   `enable-developer-extras`; point a WebkitView inside a
   DevToolsWindow-shaped host at `inspector://127.0.0.1:PORT`. If the
   loopback frontend works we have the full WebKit inspector inside our
   chrome for ~zero protocol work — flip the flag then (or an additive
   one per the house rule if the honest answer needs a finer name).
2. **WK21 spike — local toggle.** `webkit_web_view_toggle_inspector()`:
   confirm where the frontend toplevel lands under our `WPEDisplay`
   (free window vs. needs the `create_toplevel` hook routing buffers
   into a dock). This is the in-process variant of (1) without the
   extra socket, same detached-window UX as `DevToolsWindow`.
3. **BidiPanel integration — separate task, do not gate the flag on
   it.** The rustcore WebKit-inspector-protocol backend in (b) is the
   "right" long-term answer (our UI, context selector, our Security
   tab) but is a task of its own after the wire format is verified.
4. **WebDriver** lands as the autotest driver on its own merits;
   BiDi-over-webdriver stays a stretch probe.

Caveats the audit must carry: the inspector server is process-wide and
lists *every* webkit view including OTR — gate arming on user action
like `bidiDebugPort`, never in Tor windows; the socket wire format is
internal/version-coupled; remote-inspector clients want same-or-newer
WebKit (the loopback self-client in step 1 sidesteps skew entirely).
