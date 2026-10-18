# Devin autonomous task list — Arktos WebKit engine lane

This file is the task queue for the WebKit lane's Devin CLI task loop
(`.devin/taskloop-webkit.sh`). Each run of the loop picks the FIRST row
whose Status is `pending` or `in_progress` (crash-restart), works it to
completion, updates the row, commits, and stops.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting work, `done` only after
  building and verifying per the task's own verification hint.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- Never revert unrelated working-tree changes.

Context: repo is Arktos (ex-Arora), a Qt 6.12.0 / QtWebEngine browser
with a pluggable engine layer: `Engine::Backend/Profile/Page`
(src/engine/engineinterface.h), hosted in tabs by EngineTab, listed by
EngineRegistry. Two backends exist today — WebEngineBackend (Chromium,
always present) and ServoBackend (dlopened servo-embed cdylib). This
lane adds the THIRD engine: modern WebKit via WPE WebKit's WPEPlatform
API (the toolkit-free embedder port — NOT QtWebKit, NOT WebKitGTK).
Design study + per-API mapping: `.devin/WEBKIT-STUDY.md` — READ IT
FIRST. The target end state is three selectable engines; nothing
existing may be removed or broken. All work happens on branch
`webkit-work` in this worktree; master/main-lane files stay surgical.

Qt toolchain: ~/Qt/6.12.0/gcc_64 — `source .devin/qt-env.sh` before
qmake/make. NO sudo, ever. Headless: QT_QPA_PLATFORM=offscreen or
xvfb-run — never touch the user's live display.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| WK00 | WPE toolchain acquisition — user-local WPE ≥2.50 with WPEPlatform + webkit-env.sh — details below | pending | 0 | needs:linux |
| WK01 | WPE embed spike — standalone harness proves loop+render+input → .devin/WEBKIT-SPIKE.md verdict — details below | pending | 0 | needs:task:WK00 |
| WK02 | WebkitBackend skeleton — Engine::Backend impl, pkg-config build gate, registry probe, engine list/switcher shows "WebKit <version>" — details below | pending | 0 | needs:task:WK01 |
| WK03 | Navigation + signals — load/stop/reload, url/title/progress, back/forward/history — details below | pending | 0 | needs:task:WK02 |
| WK04 | Profile storage — per-profile WebKitWebsiteDataManager rooted in profiles/<id>, OTR ephemeral session, clear-data — details below | pending | 0 | needs:task:WK03 |
| WK05 | Input + zoom + find — wpe event API from QWidget events, set_zoom_level, WebKitFindController — details below | pending | 0 | needs:task:WK03 |
| WK06 | JS bridge + script injection — evaluate_javascript with typed results, UserContentManager + isolated worlds, script-message host channel — details below | pending | 0 | needs:task:WK04 |
| WK07 | Policy layer — decide-policy nav veto, content-blocker adblock mapping, cookie accept-policy, custom uri-scheme handlers — details below | pending | 0 | needs:task:WK04 |
| WK08 | Capabilities + chrome polish — honest Capability table, engineNote, settings combo/cert-error/download wiring — details below | pending | 0 | needs:task:WK06 needs:task:WK07 |
| WK09 | Perf path — dmabuf/EGLImage → GL texture rendering (replaces raster copy) — details below | pending | 0 | needs:task:WK02 |
| WK10 | Stretch audit — SOCKS5/Tor leak check, WebExtensions availability probe, WebKitWebInspector embed — details below | pending | 0 | needs:task:WK08 |

## Task details

### WK00

WPE toolchain acquisition — the feasibility gate for the whole lane.
This host (Ubuntu 24.04) ships NO wpewebkit package; Debian sid has
libwpewebkit-2.0-1 2.54.1. Spec: (a) probe first —
`pkg-config --exists wpe-webkit-2.0 wpe-platform-2.0` may already hold
if the user installed something; also check ldconfig. (b) If absent,
acquire user-locally WITHOUT sudo: preferred path is downloading the
Debian sid .debs (libwpewebkit-2.0-1 + libwpewebkit-2.0-dev, plus any
missing dep .debs — libsoup3, gstreamer pieces are shared with the
installed libwebkit2gtk so most deps may already hold) and `dpkg -x`
extracting into a prefix like ~/wpe. Check glibc/glib version floors
of the sid binaries against the host (ldd -r style missing-symbol
check) — if sid's binaries don't resolve on 24.04's glibc/glib, fall
back to Debian stable (trixie) 2.48.x (legacy libwpe API — note the
compromise in the doc and adjust the WK01 plan), or lastly try a
Flatpak SDK runtime that ships WPE. Source-building WebKit is the
last resort — a real WebKit build is hours and ~30GB; only attempt it
detached via nohup and mark blocked if it can't fit the run. (c)
Whatever path lands, write `.devin/webkit-env.sh` — sourceable, sets
PKG_CONFIG_PATH/LD_LIBRARY_PATH/C_INCLUDE paths so qmake and harness
builds resolve WPE exactly like qt-env.sh resolves Qt. (d) Document
what was installed, version, and which API level (WPEPlatform vs
legacy libwpe) in the env file header comment.

Verify: `source .devin/webkit-env.sh && pkg-config --modversion
wpe-webkit-2.0` prints a version ≥2.44 AND `gcc $(pkg-config --cflags
--libs wpe-webkit-2.0) -o /tmp/wkprobe probe.c` links + runs a trivial
`webkit_get_major_version()` probe. If no path produces working WPE
binaries on this host, mark blocked: with the exact dep list that
failed — do NOT fake it.

### WK01

WPE embeddability spike — mirror of ENG03's SERVO-SPIKE. Standalone
harness at `spikes/webkit/wpe-harness/` (own qmake/cmake project —
NOT wired into the top-level SUBDIRS, `make check` untouched).

Spec: (a) start a WPEPlatform `headless` display (WPE_PLATFORM env or
wpe_display_headless_new), create WebKitWebView + WPEView + toplevel
sized to the widget, (b) ANSWER THE EVENT-LOOP QUESTION FIRST — the
study's open issue: does Qt's glib event dispatcher pump WPE's
GMainContext? Test: create the view on the Qt GUI thread, load a real
page, see if load-finished arrives without an explicit glib pump; if
not, drive g_main_context_iteration from a QBasicTimer, and if THAT
deadlocks/misses, use a dedicated std::thread running g_main_loop_run
with queued marshaling into Qt. Record which model the harness uses —
it becomes the backend's threading contract. (c) render path: grab the
view's WPEBuffer, `wpe_buffer_import_to_pixels()` → QImage → paint in
a QWidget. (d) input: translate a QMouseEvent into the wpe event API
and click a link — prove nav happens. (e) per-member verdict pass over
the STUDY's mapping table: runJavaScript result types, history list
objects, find controller, suspend/resume, decide-policy, uri-scheme
handler, cookie manager, WebKitDownload, tls error signal, proxy
settings — each graded works/partial/absent with evidence, exactly the
SERVO-SPIKE.md shape. Deliverable: `.devin/WEBKIT-SPIKE.md` verdict doc
+ the harness.

Verify: harness binary loads `https://example.com` (or a local file://
fixture if offscreen has no net), paints non-empty pixels into a saved
PNG, a synthetic click navigates, JS eval returns a typed value.
Blocked = which member failed, never a green-painted blank page.

### WK02

WebkitBackend skeleton — needs:task:WK01 (spike verdict + harness
exist). New `src/engine/webkitbackend.{h,cpp}` implementing
Engine::Backend/Profile/Page/View adapted from servobackend's shape —
NOT a copy: WebKit has real API where Servo stubbed.

Spec: (a) build gate — engine.pri `webkit=1` CONFIG block mirroring
the servo=1 stanza: pkg-config detection of wpe-webkit-2.0 +
wpe-platform-2.0 (PKGCONFIG +=), -DARKTOS_WEBKIT, build-time message
"Arktos engine backends: webengine + webkit" listing; absent libs →
backend compiled but `isSupported()`/initialize() false → chrome
hides it (mirror artifactAvailable()). qmake opt-out `webkit=0` /
`nowebkit=1` symmetric with noservo. (b) `WebkitBackend`: id() =
"webkit"; displayName() = `WebKit <major>.<minor>.<micro>` from
webkit_get_*_version() — the user requirement is the engine switcher
showing the version exactly like "Servo 0.7.0" does; engineNote() =
honest experimental note; capabilities() returns the spike-verified
table (start conservative — only what WK01 proved); pageUnavailable-
Reason() for missing-lib display. (c) `WebkitProfile`/`WebkitPage`/
`WebkitView` minimal but real: view = QWidget painting the imported
buffer (raster path from the spike), page wraps WebKitWebView+WPEView,
profile wraps the session/data-manager. Loads can be minimal — WK03
fleshes out. (d) registration: static-init probe installing into
EngineRegistry (the addProbe seam — engineregistry.cpp needs NO
edit); EngineTab must host it with zero changes — VERIFY that claim
by compiling. (e) threading contract from WK01 applies here.

Verify: build with webkit=1, `--engine-list` (or settings combo) shows
all three engines with "WebKit x.y.z"; creating an EngineTab on the
webkit backend paints the spike's page load; build withOUT the libs
still compiles+links (gate off) and the menu hides the entry. Tests:
REQUIRED — a small autotest leg (backend registers, id/name/caps,
absent-libs honesty) is enough at this stage.

### WK03

Navigation + signals — needs:task:WK02. Wire WebKit's real load
signals onto Engine::Page's contract: load-changed →
loadStarted/loadFinished/loadProgress (estimated-load-progress notify
gives real percentages — better than Servo); notify::uri →
urlChanged; notify::title → titleChanged; notify::favicon →
iconChanged; stop_loading/reload. History: WebKitBackForwardList →
historyItems/backItems/forwardItems/canGoBack/canGoForward/
goToHistoryEntry — these are real objects, unlike Servo's synthetic
list; currentHistoryIndex tracked. NavigationType mapping from
WebKitNavigationType where available.

Verify: autotest legs — typed load completes with progress>0 signals
in order, title arrives, back/forward round-trips a two-page session.
Offscreen headless only.

### WK04

Profile storage — needs:task:WK03. Per-profile WebKitWebsiteDataManager
(or NetworkSession on 2.4x API) with base_directory rooted inside the
profile's storage dir — SAME contract as ServoProfile::configDir and
VSEC02's profile-rooted layout: profiles/<id>/WebKit (name the dir so
it can't collide with QtWebEngine/). OTR profile → ephemeral session
(webkit_network_session_new_ephemeral or the is-ephemeral manager) —
verify NOTHING persists to disk for OTR. Profile::clear(StorageAreas)
maps onto webkit_website_data_manager_clear type flags; cookie policy
→ WebKitCookieManager set_accept_policy. storageName plumbing per
ProfileOptions.

Verify: tst leg — profile A sets a cookie/localStorage via a real page
load, profile B can't see it; clear(CookiesArea) empties A's store;
OTR session leaves zero files. Data must land under the profile tree —
assert the path.

### WK05

Input + zoom + find — needs:task:WK03. QWidget input events → wpe
event API (pointer move/button, scroll/wheel with line+pixel delta,
key press/release with keysym translation — reuse or mirror whatever
mapping the spike harness proved). set_zoom_level ←
setZoomFactor/zoomFactor. WebKitFindController → findText +
FindResult signals (found/did-fail). Suspend/resume →
setLifecycleState (webkit_web_view_suspend when the resolved version
has it — symbol-gate like servo's optional exports; degrade honestly
to Active-only otherwise).

Verify: harness or autotest legs — synthetic click follows a link,
Ctrl+F-equivalent findText reports found, zoom changes rendered
scale. Wheel pacing: WebKit delivers frames on its own schedule —
no Servo-style coalescing needed, but note it if input floods.

### WK06

JS bridge + script injection — needs:task:WK04. runJavaScript →
webkit_web_view_evaluate_javascript with JSCValue→QVariant marshaling
(string/number/bool/null; object→JSON-stringify document the limit).
insertScript → WebKitUserContentManager add_script with injection-time
and world-name mapping — CRITICAL: Engine::ArktosChannelWorld (0xa0)
and isolated worlds in general map onto WebKit's named script worlds
(default vs named string world); implement the mapping so the app's
private-world contract (SEC24) holds on this engine too.
runJavaScriptLifted must actually run in the named world. Two-way
host channel: webkit_user_content_manager_register_script_message_
handler(_with_reply on 2.5x) → the native arktos-channel transport
equivalent for autofill/reader/etc — evaluate which existing JS assets
(autofill.js, arktos-channel.js) can run on this backend and what the
bridge surface needs; don't fake the channel — if a consumer needs a
real two-way reply and the API version lacks it, capability-gate that
consumer honestly.

Verify: autotest legs — eval returns typed QVariant, injected script
runs at DocumentCreation in the right world, a script-message
round-trip works, main-world page JS cannot see the private-world
objects (mirror the SEC24 assertion shape).

### WK07

Policy layer — needs:task:WK04. The interception story — WebKit's
only real gap vs QtWebEngine/Servo: no per-request veto API. (a)
Navigation gate: decide-policy signal → Engine::RequestPolicy::decide
for navigation actions (main-frame allow/deny covers SEC02-class
blocking + external-scheme prompts). (b) Subresource blocking:
WebKitUserContentFilterStore — compile our adblock rules into WebKit
content-blocker JSON. Scope honestly: write a converter for the
subset that maps (url-filter + resource-type + block action);
$redirect/stub-serving has NO equivalent — those rules degrade to
block (document it) or get dropped with a count logged. Decide with
the spike findings whether requestInterception stays false or becomes
true-for-block-subset — the honest answer is probably a new/adjusted
capability rather than faking parity; update engineinterface.h docs if
the capability needs a granularity split (e.g.
requestInterception=navigation-only, declarativeBlocking=true) — but
keep the change additive and reviewable, never fake. (c) Custom
schemes: webkit_web_context_register_uri_scheme → serve arktos-file:/
abp:/arktos-resource: handlers — same adapters as backend-0 where the
serving side is engine-agnostic. (d) cookieFilter: map the achievable
subset (accept-policy + cookies-changed observation); report
capability honestly.

Verify: autotest legs — blocked navigation never loads, an adblock
fixture URL is blocked via the compiled filter, arktos-file: serves a
listing, third-party-cookie policy respected. Document the rule-
coverage count (rules converted / rules dropped) in the test output.

### WK08

Capabilities + chrome polish — needs:task:WK06+WK07. Fill the
Capabilities table with the verified truth (the spike table +
whatever landed since). engineNote(): honest caveat line. Wire the
remaining surfaces: certificate errors (load-failed-with-tls-errors +
CertificateInfo → our cert-error interstitial contract — check how
WebEngineBackend surfaces it and match), WebKitDownload →
DownloadManager handoff (rustdl path if it accepts a generic
download-source interface — else plain save), context-menu →
ContextMenuInfo, permission requests → WebPermissionManager types,
renderProcessId (real WebProcess pid if the API exposes it, else -1).
Make sure the settings "Default engine" combo, the indicator tooltip,
About dialog engine line, and --engine-list all render "WebKit x.y.z"
consistently — the explicit user requirement. Tor honesty: WebKit
must NOT be offered for Tor tabs until WK10's audit (the existing
lock mechanism applies — verify it's consulted).

Verify: full `make check` green (excluding documented env flakes);
a fresh profile launches on webkit backend and browses a fixture
page end-to-end through the real chrome.

### WK09

Perf path — needs:task:WK02 (independent of 03-08; schedulable early
if raster copy proves painful). WPEBuffer →
wpe_buffer_import_to_egl_image → QOpenGLWidget/QRhiWidget texture
instead of CPU copy. Requires EGL/dmabuf plumbing and fence handling
(renderingFence/releaseFence) — the heavy-lifting task. Keep the
raster path as fallback when EGL import fails.

Verify: same autotest legs pass; scroll/paint throughput measured
vs raster path (a rough frame-time log is enough — this is about
proving the path works, not benchmarking).

### WK10

Stretch audit — needs:task:WK08. (a) SOCKS5: WebKitNetworkProxySettings
accepts socks:// — set a session proxy and audit for leaks (DNS via
socks5h-equivalent remote resolution? WebRTC host candidates? captive-
portal checks bypassing the proxy?). Honest verdict doc; only if clean
does the Tor-lock get a "webkit allowed" capability — default stays
Chromium-locked. (b) WebExtensions: probe whether the resolved WPE
version exposes the WebKitWebExtension GLib API (2.52+ GTK, WPE status
varies) — set caps.extensions accordingly, never assume. (c)
WebKitWebInspector: can it embed as a view in our devtools window or
is it separate-window only? Verify: verdict entries in
.devin/WEBKIT-SPIKE.md addendum + capability flags matching reality.
