# Devin autonomous task list — Arktos extension-runtime lane

This file is the task queue for the extension-runtime lane's Devin CLI
task loop (`.devin/taskloop-extruntime.sh`). Each run of the loop picks
the FIRST row whose Status is `pending` or `in_progress`
(crash-restart), works it to completion, updates the row, commits, and
stops.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting work, `done` only after
  building and verifying per the task's own verification hint.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- Never revert unrelated working-tree changes.

Context: repo is Arktos (ex-Arora), a Qt 6.12.0 / QtWebEngine browser.
Qt's embedder never registered the extension delegates for
`scripting`, `webNavigation`, `tabs` events, `contextMenus` callbacks,
`notifications`, `sidePanel`, `windows`, `idle`, `offscreen`, `webRequest`,
and more — the schemas are compiled into QtWebEngineCore but nothing
serves them (EXT11 measured the boundary; EXT21 grades extensions that
REQUIRE an unserved core API `NotSupported`). This lane does NOT fix
Chromium: it implements the missing `chrome.*` surfaces ourselves, in
the EXT14–19 hosted runtime — the hidden host page per extension +
the `chrome.*` shim + C++ dispatch in `extensionshimbackend.cpp`.
Bitwarden (installed, id `kkjpijbfkjghhkbbkmgmkdmfkipcfflk`, unpacked at
`~/.local/share/Arktos/Arktos/profiles/default/QtWebEngine/Extensions/bitwarden-2026.9.3_2QIFbk`)
is the benchmark workload — its measured call surface is in
`.devin/EXT-RUNTIME-STUDY.md` — READ IT FIRST. Target end state:
`Bitwarden → unlock → autofill a login form` works end to end, or a
measured `blocked` with the exact world-isolation finding.

Design study: `.devin/EXT-RUNTIME-STUDY.md`. Existing runtime:
`src/extensions/extensionbackgroundhost.{h,cpp}` (host pages),
`src/extensions/extensionshimbackend.{h,cpp}` (dispatch — 12
namespaces), `src/extensions/extensioneventhub.{h,cpp}` (tab/window
tracking), `src/extensions/extensionmanager.{h,cpp}` (health grade).

Qt toolchain: ~/Qt/6.12.0/gcc_64 — `source .devin/qt-env.sh` before
qmake/make. NO sudo, ever. Headless: QT_QPA_PLATFORM=offscreen or
xvfb-run — never touch the user's live display.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| ER00 | Bitwarden gap harness — env-gated test leg that loads the real unpacked bundle on an isolated profile and dumps every typed shim gap it produces — details below | done | 1 | needs:linux · bitwardenGapBaseline leg PASS: real bundle installs+hosts ready, popup bridges, 0 typed gaps at boot — baseline + touched-map in .devin/EXT-RUNTIME-BASELINE.md |
| ER01 | Page agent — profile-wide QWebEngineScript giving web pages a chrome.runtime sendMessage bridge into the extension host — details below | done | 1 | needs:task:ER00 · extensionPageAgent leg PASS: agent+declared cs inject in shared world 0x40, cs sendMessage echoes via hosted bg, sender/url/extid honest, typed denials, main world clean — full suite 32/32 (2 env skips); data: gets profile scripts but no page-level channel so coverage rejects it; Bitwarden popup SIGTRAP reproduced on clean ER00 tree too — pre-existing env issue, not this diff |
| ER02 | tabs.sendMessage + runtime.onMessage delivery into web pages — details below | done | 1 | needs:task:ER01 · extensionTabMessaging leg PASS: host→page round-trip via __arktos.deliver/respond, Chrome sender shape, no-listener→"Receiving end does not exist", typed not-found/invalid-mid/unsupported(frameId)/no-host-access; full suite 33/33 (2 env skips) |
| ER03 | Tab events — tabs.onCreated/onUpdated/onRemoved/onActivated dispatched into host pages — details below | done | 1 | needs:task:ER01 · extensionEventSurfaces leg PASS: fixed onActivated to Chrome's single activeInfo object (was 2 scalar args), added causal-order + shape assertions; tabs events stay ungated-but-url-scrubbed (Chrome parity, not tabs-permission-gated — EXT16's documented model); full suite 33/33 (2 env skips); Bitwarden env leg SIGTRAPs at popup load — pre-existing env issue per ER01 notes |
| ER04 | webNavigation events — onCommitted/onDOMContentLoaded/onCompleted/onErrorOccurred from WebView load signals — details below | done | 0 | needs:task:ER03 · extensionEventSurfaces PASS: onDOMContentLoaded via DocumentCreation probe (extnavdcl.js) → navDomReady bridge op, forge-gated on in-flight load + once-per-nav; data:/about: fallback at loadFinished; committed/dcl/completed ordering asserted on data: + real http origin (probe counter); refused connection emits onErrorOccurred + generated/isErrorPage error-commit cycles suppressed — full suite 33/33 (2 env skips) |
| ER05 | windows.on* + runtime.onStartup/onInstalled dispatches — details below | done | 1 | needs:task:ER03 · extensionRuntimeLifecycle leg PASS: host-Ready dispatch reads ProfileSettings extensions/runtime/<id> → install/update(+previousVersion)/browser_update, onStartup on relaunch only (per-session dedupe + restartLifecycleSession seam), uninstall clears the record so reinstall is fresh; windows.onCreated fixed to single {window} arg; shim dual-registers runtime.onInstalled/onStartup + windows.on* — full suite 34/34 (2 env skips) |
| ER06 | DECISION GATE — Bitwarden unlock + content-script handshake proven or measured-blocked with world-isolation finding — details below | done | 1 | needs:task:ER02 needs:task:ER04 · bitwardenHandshakeGate leg PASS: real bundle's cs↔bg handshake crossed the emulated-world boundary both directions (triggerAutofillScriptInjection + autofill-injected-script-port in with sender.tab; tabs.sendMessage bgUnlockPopoutOpened out + real cs forward-back reply) — bg dispatched chrome.scripting.executeScript → typed unsupported gap; verdict PASS, evidence .devin/EXT-RUNTIME-SPIKE.md; full suite 34/34 (3 env skips) |
| ER07 | scripting.executeScript — per-frame runJavaScript, host-permission check, one-shot result marshaling — details below | done | 0 | needs:task:ER06 · extensionScripting leg PASS: per-frame QWebEngineFrame::runJavaScript fan-out (allFrames/frameIds over DFS tree ids) in shared agent world — agent's scoped chrome bound via the compat closure; thenables parked on __arktosExecResults + polled (runJavaScript never awaits); func/args/files forms; 'scripting' + host-coverage gates run BEFORE page write — no-permission/no-host-access/not-found/bad-path/no-such-file typed + ledgered, documentIds unsupported; MAIN = unprivileged page world; full suite 35/35 (3 env skips) |
| ER08 | scripting.insertCSS/removeCSS + executeScript file/frames/args forms — details below | done | 1 | needs:task:ER07 · extensionScriptCss leg PASS: insertCSS/removeCSS share the executeScript gates + per-frame fan-out, agent-world <style> ledger keyed by css unit+origin (upsert/remove/no-op-on-miss, dies with document); css/files/origin validated, USER origin = author-level approximation recorded; allFrames+frameIds target real subframes; multi-file files[] order verified; no-permission/no-host-access/bad-path/no-such-file/invalid-argument/unsupported/not-found typed + ledgered; full suite 36/36 (3 env skips) |
| ER09 | contentScripts dynamic registration — register/unregister/getScripts over our injection set — details below | done | 1 | needs:task:ER07 · extensionContentScriptRegistration leg PASS: shared runtime registry behind BOTH surfaces (MV3 scripting.register/unregister/getRegistered/update + Firefox contentScripts.register/unregister/getScripts w/ unregister-handle) arms profile scripts into the shared agent world on next matching nav; per-match host-coverage implication checked BEFORE arming (no-host-access), 'scripting' permission gates only the MV3 surface; persisted specs re-arm across host respawn via ProfileSettings extensions/regscripts/<id>, session-only die with load, all drop on uninstall; bad-path/no-such-file/invalid-argument/not-found/unsupported typed; full suite 37/37 (3 env skips) + real Bitwarden legs all PASS incl. handshake gate (bg dispatched 4 executeScript — executed, no gap) |
| ER10 | offscreen.createDocument/hasDocument/closeDocument → second-class host page — details below | done | 0 | hidden bridge-backed doc page; typed denials + verbatim single-document msg; getContexts OFFSCREEN_DOCUMENT; extensionOffscreen + full suite 38/38 + Bitwarden legs pass |
| ER11 | webRequest observe-only events (onBeforeRequest/onCompleted/onErrorOccurred) via QWebEngineUrlRequestInterceptor — details below | done | 2 | needs:task:ER04 · extensionWebRequest leg PASS: interceptor→watcher feed (OTR/tor excluded) serves onBefore*/onSendHeaders/onHeadersReceived/onCompleted/onErrorOccurred w/ per-listener filters + per-event host-coverage re-proof; blocking/requestBody→typed unsupported+gap, unservable events refuse at subscribe; orphan tabId-adoption fixed fast-fail nav race; full suite 39/39 + real Bitwarden legs PASS (bg registered onHeadersReceived — note, not gap) |
| ER12 | notifications.create/update/clear/getAll → native presenter — details below | done | 0 | needs:task:ER06 · extensionNotifications leg PASS: record registry + freedesktop Notify over session D-Bus; onClicked/onButtonClicked/onClosed(byUser)/onShown route back through the runtime fan-out; 'notifications' permission gates the ns; extension-host-smoke PASS + full suite 40/40 (3 env skips) + real Bitwarden legs PASS (gap baseline 0 typed gaps — notifications no longer one) |
| ER13 | idle.queryState/onStateChanged — input-activity watcher, configurable threshold — details below | done | 1 | needs:task:ER05 · extensionIdle leg PASS: app-wide Qt input filter→precise-timer watcher (in-app activity only, 'locked' never reported — honest note once/context); listener subscribe arms it; queryState's own threshold answers vs clock independent of declared state; 'idle' permission gates calls + fan-out; pre-bridge diagnostics rerouted through the call queue (was dropping gaps); full suite 41/41 (3 env skips) + real Bitwarden legs PASS offscreen --disable-gpu (bg drove idle.setDetectionInterval/subscribe, 0 typed gaps) |
| ER14 | commands — manifest command parsing + QShortcut registry + onCommand dispatch — details below | pending | 0 | needs:task:ER05 |
| ER15 | management.getSelf + chrome.extension.getBackgroundPage/getViews + sidebarAction sugar — details below | pending | 0 | needs:task:ER06 |
| ER16 | Security pass — host_permissions scoping on every injection path; per-call consent honoring EXT05 grants — details below | pending | 0 | needs:task:ER07 needs:task:ER09 |
| ER17 | Bitwarden E2E — install→unlock→fill a login form on a test page, harness-driven, recorded — details below | pending | 0 | needs:task:ER08 needs:task:ER10 needs:task:ER15 |
| ER18 | Residual gap sweep — drive Bitwarden until shimGaps hold zero typed unimplemented for its call set — details below | pending | 0 | needs:task:ER17 |
| ER19 | Health regrade — EXT21 fatal check subtracts runtime-provided APIs; Bitwarden leaves NotSupported ONLY when it verifiably works — details below | pending | 0 | needs:task:ER18 |
| ER20 | Chrome polish + docs — settings copy, action tooltip, README extension-support section, ChangeLog — details below | pending | 0 | needs:task:ER19 |

## Task details

### ER00
Baseline measurement. Add an env-gated test leg (follow
`realExtensionPopup`'s ARKTOS_EXTTEST_DIR pattern) that installs the
real unpacked Bitwarden onto an isolated test profile, forces the hosted
runtime on, then exercises it: open the popup context, wait for the
background host Ready, and record EVERY typed shim gap (`shimGaps`,
`unimplemented`/`unsupported` replies) the bundle emits during boot.
Output: a committed baseline list under `.devin/` or the test log —
this list is the lane's work order and the later tasks' acceptance
input. Do NOT use the user's live profile; fixture-copy the bundle to a
temp dir.
Verify: leg prints the gap list; baseline committed.

### ER01
The page agent — the lane's spine. A profile-wide `QWebEngineScript`
(DocumentCreation, every web page — NOT extension pages) that installs
a minimal `chrome.runtime` bridge inside web pages: `sendMessage` /
`onMessage` surface reaching the extension host, replies marshaled back.
Transport options to evaluate in code order: (a) QWebChannel to the host
page, (b) console/log bridge, (c) DOM-event relay through a declared
extension content script. Pick what survives `document_start` timing
honestly — never fake a bridge that races the page. Per-host scoping:
the bridge answers ONLY for extensions whose host_permissions cover
the page origin; everything else gets the typed absent-API reply.
Verify: fixture extension's declared content script round-trips a
message with its background through the agent on a data:/local page.

Run notes (2026-10-18): transport picked (a) — the existing
`arktos:channel` QWebChannel, reached via a whitelisted postMessage
forwarder (`src/data/arktos-channel.js` now admits only
`arktosExt.invoke`/`arktosExt.extEvent`); no bridge races the page
because the agent queues calls until its `hello` handshake completes.
One shared isolated world (`AgentWorldId = 0x40`) serves every
extension agent — Qt caps world ids at 256 — with per-extension
namespacing under `window.__arktosExtAgents[id]` and the scoped
`chrome` object passed to each content script by closure, so no
global `chrome` binding is installed anywhere. The C++ boundary is
`ExtensionPageBridge` (`arktosExt` on each WebPage's channel):
per-call coverage checks against the page's *committed* url, typed
`invalid-mid`/`invalid-chan`/`no-host-access`/`no-storage-permission`
denials, sender stamped from real tab metadata. Engine findings
encoded as behavior, not worked around: page-level `scripts()` never
inject on `data:`/`about:` documents (profile-level scripts do), so
the covered-leg test uses loopback http and `agentCovers`/`hostCovers`
reject originless schemes exactly like Chrome does; Chromium's own
empty `window.chrome` persists in the main world (checked: no keys,
no `runtime`, no `__arktosExtAgents`). `world:"MAIN"` content scripts
run in the shared isolated realm (recorded approximation — more
isolation, same scoped binding). Field check: the real Bitwarden
popup leg (`realExtensionPopup`, `bitwardenGapBaseline`) SIGTRAPs
during popup load in this environment — reproduced identically on the
clean ER00 tree, with the agent disabled, on a fresh extension store
and with WASM off, while a minimal extension popup passes — a
pre-existing engine-level issue in Bitwarden's blessed-context app
code, NOT this diff; deferred for separate investigation.

### ER02
`tabs.sendMessage(tabId, message)` + `runtime.onMessage` delivery INTO
web pages — the background→content direction. Resolve tabId→WebView via
the event hub's registry, marshal the call into the page agent, deliver
to content-script listeners, return the callback/promise reply.
Verify: host→page round-trip on a fixture page; wrong-tabId gets a
typed error.

### ER03
Tab events into host pages: `tabs.onCreated/onUpdated/onRemoved/
onActivated`. Source: `ExtensionEventHub` already tracks tab widgets —
extend it to emit subscription-filtered events per extension host
(only extensions with `tabs` permission get them). Payload shape must
match Chrome's (tab object parity with the existing query snapshot).
Verify: fixture extension logging events sees create/update/activate/
remove in order during scripted tab ops.

### ER04
`webNavigation.onCommitted/onDOMContentLoaded/onCompleted/
onErrorOccurred` — map WebView/Page load signals + URL changes into
Chrome-shaped navigation events (frameId 0 only — subframes are a
documented gap). Permission-gated to `webNavigation` declarants.
Verify: fixture sees all four events in order across a scripted
navigation; a failing load emits onErrorOccurred not silence.

### ER05
`windows.onCreated/onRemoved/onFocusChanged` (from window lifecycle —
the hub tracks windows already) plus `runtime.onStartup` and
`runtime.onInstalled` (reason `install`/`update`/`browser_update` from
the version record in ProfileSettings).
Verify: fixture sees onInstalled once on first load, onStartup on
relaunch, window events on open/close.

### ER06
DECISION GATE — the study's E3 checkpoint made executable. Drive real
Bitwarden: host boots, unlock-init path runs (expect offscreen gaps —
typed errors are acceptable here), content-script handshake into a
page is attempted through the ER01/02 bridge. Verdict either way is
honest: PASS = the content script receives the background's handshake
and replies; BLOCK = a measured isolation finding (e.g. handshake
requires true isolated-world identity that world-emulation can't
fake) — write the finding into `.devin/EXT-RUNTIME-SPIKE.md` and mark
the lane blocked; do NOT paper over it.
Verify: leg asserts handshake bytes crossed the boundary, or the
blocking finding is documented with the exact failing primitive.

### ER07
`chrome.scripting.executeScript` — the product API. Map onto
`WebView::page()->runJavaScript` per target (tabId, frameIds where
resolvable, all_frames honest-false when we can't address subframes).
Enforce host_permissions BEFORE injecting — a miss is a typed error,
not a warning. `func` form + `files` form (read the bundle file,
wrap, inject); `args` marshaled into the call. One-shot JSON values
back — the MV3 contract permits exactly that. World emulation:
prepend the compat preamble so the injected code sees the same
`chrome.*` the declared content scripts see.
Verify: fixture executes `() => document.title` on a test tab and
gets the value; a host-permission miss is refused.

### ER08
`scripting.insertCSS/removeCSS` (Bitwarden's overlay css) + the
remaining executeScript forms (files list, per-frame targeting, args
marshaling — whatever ER07 left partial).
Verify: insertCSS changes a measurable style on the fixture page and
removeCSS restores it.

### ER09
`chrome.contentScripts.register/unregister/getScripts` — Bitwarden
registers its fill handler dynamically on some flows. Map onto our own
injection set: registered specs join the page-agent injection list for
matching pages. Persist per-extension in ProfileSettings so
registrations survive restarts like Chrome's.
Verify: a registered script fires on the next matching navigation and
survives a runtime restart.

Result (done): one runtime-owned registry
(`m_regScripts`, extId→regId→normalized spec) behind both front doors
— `invokeScriptRegistration` for the MV3 set (gated on the declared
'scripting' permission, like every chrome.scripting member) and
`invokeContentScripts` for the Firefox shape the bundled
webextension-polyfill resolves `browser.contentScripts.*` to (no API
permission exists there; the gate is the same per-match implication
check Chrome's "must have host permission" rule requires). A
normalized spec arms as a profile-level QWebEngineScript
(`arktos-extdcs-<extId>-<sanitized-regId>`) in AgentWorldId with the
same wrapper/gate shape as a declared content script — the wrapper
waits for this extension's agent, re-checks matches/excludeMatches
against the committed url, and runs the code in a closure bound to
the scoped chrome/browser object. Registration validates the whole
batch before arming anything (Chrome's atomic contract): match
patterns parse + every `matches[]` must be implied by the
host-permissions ∪ declared-cs-matches coverage union
(`hostPermCoversPattern` — scheme, host incl. `*.` subdomains, path
granularity; fails closed) else typed `no-host-access`; js/css file
paths re-proven against the bundle (traversal/absolute/scheme →
`bad-path`, missing/oversize → `no-such-file`), inline `code:` is
Firefox-only (typed `invalid-argument` on MV3), globs/cookieStoreId →
`unsupported`, dup id / missing caller id / bad runAt or world →
`invalid-argument`, unregister+update on a missing id → `not-found`.
`world:'MAIN'` registers with the recorded same approximation
declared scripts carry; `matchAboutBlank`/`matchOriginAsFallback`
accept with a recorded inert note. MV2 callers get a minted
`mv2cs-N` id and an `unregister()` handle. Persisted specs
(`persistAcrossSessions`, the default) serialize to ProfileSettings
`extensions/regscripts/<extId>` and merge back in
`restoreRegisteredScripts()` on every host respawn; session-only
entries die when the extension unloads, and uninstall drops the
record + registry + armed scripts (`dropRegisteredScripts` from the
uninstall path). The stale bitwardenHandshakeGate assertion was
updated to post-ER07 reality (dispatch proof = touched delta; a typed
gap is still reported when the real call degrades) — it now measures
4 real executeScript dispatches from Bitwarden's background, all
executed with no gap. The realExtensionPopup SIGTRAP remains the
documented pre-existing blessed-context engine issue, reproduced on
the clean tree — unrelated to this diff.

### ER10
`chrome.offscreen.createDocument/hasDocument/closeDocument` — spawn a
second-class host page (no toolbar wiring) loaded at the declared
offscreen URL with the full shim. Bitwarden's Argon2/WASM unlock runs
there. `getContexts`/`createDocument` reasons recorded honestly.
Verify: createDocument loads the bundle's offscreen-document.html,
hasDocument true, a JS call inside it resolves, closeDocument
releases the page.

### ER11
`chrome.webRequest` observe-only: `onBeforeRequest/onBeforeSendHeaders/
onCompleted/onErrorOccurred` from `QWebEngineUrlRequestInterceptor` —
we see requests already. Chrome-shaped detail objects, per-listener
`urls` filters honored. NOT blocking: `blocking` extraInfoSpec gets a
typed `unsupported` (the interceptor cannot mutate) — recorded in
shimGaps, never silently ignored.
Verify: fixture's onCompleted listener sees a scripted fetch; a
blocking registration replies unsupported and is logged as a gap.

Result (done): `ExtensionRequestWatcher` (singleton) is fed by
`PrivacyRequestInterceptor::interceptRequest` — observeRequest()
snapshots on the IO thread and queued-delivers before the privacy
verdict applies; off-the-record profiles never report (tor has its
own interceptor). The shim holds per-listener (fn, filter,
extraInfoSpec) triples and registers one record per addListener via
`webRequest.subscribe`; the bridge gates the whole namespace on the
'webRequest' manifest permission and the watcher re-proves
host_permissions coverage per emitted event in C++ (`requestVisibleTo`,
the 'tabs' shortcut deliberately excluded). Servable:
onBeforeRequest/onBeforeSendHeaders/onSendHeaders from the
interception point, onHeadersReceived/onCompleted/onErrorOccurred
from main-frame milestones (loadingChanged LoadSucceededStatus
hands the real responseHeaders; loadFinished pairs completion) plus
the channel-world resource-timing probe (`extresprobe.js` → page
bridge 'resourceTiming') for subresource/fetch completions —
probe reports only ever pair requests the interceptor announced,
so forged ops cannot invent events. Unservable
(onBeforeRedirect/onResponseStarted/onAuthRequired) and
'blocking'/'requestBody' extraInfoSpec answer typed `unsupported`
into shimGaps — blocking listeners still register observe-only per
Chrome MV3. Race machinery: the interceptor's queued start can
lose to engine milestones, so commits/failures/dones buffer per
tab until the start drains them in Chrome order, and records whose
queued start ran before the tab announced its pending url park
under tabId -1 and get adopted by the resolving milestone feed
(measured: a refused-connection nav opened under -1 and lost its
onErrorOccurred until adoption landed). Verified: extensionWebRequest
PASS (real page load + scripted fetch over loopback, headers on
request only to registrations that asked, before/completed share a
requestId, filtered listener silent, blocking degrade + both typed
denials in shimGaps, refused-port net::ERR_, fixture B host-scope
refusal), full suite 39/39 (3 env skips), real Bitwarden legs PASS —
its background's live onHeadersReceived subscription registered and
surfaced the documented fidelity note, not a gap.

### ER12
`chrome.notifications.create/update/clear/getAll` → freedesktop
notifications (`QSystemTrayIcon::showMessage` or libnotify via
the existing platform integration — check what Tor/browser already
uses for system notifications first). onClicked/onButtonClicked
dispatch back into the host.
Verify: create produces a visible/inspectable notification object and
click routes to the listener.

Result (2026-10-19): landed. `ExtensionNotificationCenter` holds the
per-extension record registry (Chrome-side semantics — records outlive
a host respawn, die with the extension) and presents over
`org.freedesktop.Notifications.Notify` on the session bus, chosen over
QSystemTrayIcon because the browser already speaks D-Bus
(downloadmanager's FileManager1) and the daemon gives us real action/
close signals. `ExtensionShimBackend::invokeNotifications` gates on the
manifest's `notifications` permission (typed `no-permission`) then
delegates; validation answers `invalid-argument` for bad type/missing
title/>2 buttons; iconUrl/imageUrl resolve strictly inside the bundle.
Daemon `ActionInvoked` maps 'default'→onClicked and 'btn-N'→
onButtonClicked(index); `NotificationClosed` maps reason 2→byUser=true
(a programmatic clear emits byUser=false). `notifications.*` events
join the runtime's eventGate permission fan-out. When no service is
reachable the record still exists (getAll/update/clear work) but
nothing claims it was shown: no onShown, one-time health-visible note.
`ARKTOS_EXTNOTIFY_STUB=1` selects a synthetic presenter for headless
runs — tests and the real-Bitwarden legs use it so no banner reaches
the desktop. Verified: extensionNotifications leg PASS (hosted fixture
creates/sees/updates/clears through the real page listeners, daemon
signal slots drive click/button/close, no-permission fixture denied +
gap-ledgered), extension-host-smoke PASS (`notifications.create
presented`), full suite 40/40 (3 env skips), real Bitwarden legs PASS
— its typed-gap list is now empty of notifications entirely.

### ER13
`chrome.idle.queryState(state, detectionInterval)` +
`onStateChanged` — X11/Wayland idle detection is deep water; the
honest v1 is a QTimer reset by input events inside our windows
(documented limitation: only in-app activity resets it, matching
"screen-saver idle" only loosely). Threshold configurable per call.
Verify: queryState answers active under synthetic input and idle
after threshold with input stopped.

Result (2026-10-19): landed. `ExtensionIdleWatcher` (lazy singleton)
is the browser-global clock: an application-wide Qt event filter
(key/button/motion/wheel/touch/tablet) feeds a single-shot precise
QTimer at the configured detection interval — Chrome's global
semantics, last setDetectionInterval wins for every extension.
Both the interval and each queryState threshold clamp at Chrome's
15s floor (ARKTOS_EXT_IDLE_MIN_S lowers it for the autotest leg).
queryState answers against its own argument, so a long threshold
returns 'active' while the declared state is already 'idle';
transitions broadcast `idle.onStateChanged` through the runtime's
permission-gated eventGate ('idle' gates the namespace like
Chrome). The shim's onStateChanged onAdd posts `idle.subscribe` —
a listener registration alone arms the watcher — and records the
honest scope once per context: input is measured inside Arktos
windows only and 'locked' is never reported.
`idle.getAutoLockDelay` is typed `unsupported` (ChromeOS-only),
unknown members `unimplemented`, bad arguments `invalid-argument`,
no 'idle' permission `no-permission` — all ledgered into shimGaps.
Two infrastructure findings fixed en route: QTimer's default
CoarseTimer fires up to 5% early and raced the elapsed-clock answer
(precise timer + re-check on timeout), and the shim's
`note()`/`recordGap()` invoked the bridge directly so diagnostics
recorded before the handshake were silently dropped host-side —
both now ride the `call()` queue that flushes on bridge-up.
Verified: extensionIdle leg PASS (real QKeyEvent through the app
filter drives active→idle→active into the hosted page, per-call
threshold independence, invalid-argument/unsupported/no-permission
all ledgered, fan-out never reaches a permission-less listener),
full tst_extensionmanager 41/41 (3 env skips), real Bitwarden legs
all PASS offscreen + QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu (its
background calls idle.setDetectionInterval(300) and subscribes
onStateChanged — served, gap baseline still 0 typed gaps; the
popup legs SIGTRAP without --disable-gpu, the documented
environmental blessed-context issue).

### ER14
`chrome.commands`: parse `commands` from the manifest (the Rust/Qt
parsers gain the field), register shortcuts as QShortcut on the main
window, dispatch `onCommand` into hosts. `_execute_action` maps to
the action click.
Verify: fixture command `Ctrl+Shift+Y`-style binding fires onCommand.

### ER15
Introspection sugar: `chrome.management.getSelf` (id/name/version/
enabled from our records), `chrome.extension.getBackgroundPage`
(proxy object over the host — Window can't cross the channel, so
return a typed capability handle or the documented MV3 answer),
`chrome.extension.getViews` (context list), `chrome.sidebarAction`
minimal (alias to sidePanel bookkeeping).
Verify: getSelf returns the fixture's real id/version; getViews lists
host+popup contexts.

### ER16
Security pass — the lane's honesty checkpoint. Every injection path
(ER01 agent, ER07 executeScript, ER09 contentScripts, context-menu
fill) must consult the extension's host_permissions + the page origin
BEFORE touching the page; per-call permission checks for runtime APIs
(tabs permission for tab queries/events, etc.); a hostile fixture
extension (manifest over-claiming, injection into a non-granted
origin) is refused and logged.
Verify: hostile-fixture legs assert the refusals; a clean extension
still passes its flow.

### ER17
The E2E that matters: Bitwarden install→unlock→fill, harness-driven
(no human clicks). Test fixture: a login page (local HTML with a
form), Bitwarden bundle, recorded unlock path (test vault or the
unlocked-cache path Bitwarden exposes for automation — find the
shortest honest route). Assert the fill payload lands in the form
fields. If any step hits an immovable wall, the output is a measured
gap, not a pass claim.
Verify: the test asserts `input.value` became the fixture credential.

### ER18
Residual sweep: replay the ER00 baseline; every Bitwarden-emitted
typed `unimplemented`/`unsupported` gap must either be implemented or
documented as intentional in `.devin/EXT-RUNTIME-GAPS.md`. Zero
unplanned gaps.
Verify: gap list shrinks to the documented intentional set.

### ER19
Health regrade — EXT21's fatal check currently condemns Bitwarden
because `scripting`/`webRequest` are unservable *by Qt*. Once the
runtime serves them, `fatalMissing` must subtract the
runtime-provided set (keep a `runtimeServes()` list next to the fatal
table — required permission in fatal set AND runtime serves it → not
fatal; still lands in `unverified`/missing for honest wording).
Bitwarden moves OFF NotSupported ONLY when ER17/18 proved the flow —
the regrade is earned, not declared.
Verify: Bitwarden classifies Limited-or-better after E2E; a fixture
requiring scripting WITHOUT the runtime path still grades
NotSupported.

### ER20
Chrome polish + docs: Preferences row/details wording when the runtime
serves an extension (status "Hosted runtime", not bare "Enabled");
action-bar tooltip gains the runtime note; README's extension section
rewritten from "Qt provides X" to "Qt provides X, Arktos hosts Y";
ChangeLog EXT22 entry summarizing the lane.
Verify: settings shows the new status string; docs committed.
