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
qmake/make. WPE toolchain: `source .devin/webkit-env.sh` once WK00/01
land it. NO sudo, ever. Headless: QT_QPA_PLATFORM=offscreen or
xvfb-run — never touch the user's live display.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| WK00 | WPE availability probe + user-local acquisition — get wpe-webkit-2.0 libs into ~/wpe without sudo — details below | pending | 0 | needs:linux |
| WK01 | webkit-env.sh + link/run probe — sourceable env file + a compiled webkit_get_*_version() smoke — details below | pending | 0 | needs:task:WK00 |
| WK02 | Spike: harness skeleton + event-loop verdict — which GMainContext pump model works inside Qt — details below | pending | 0 | needs:task:WK01 |
| WK03 | Spike: render path — WPEBuffer → import_to_pixels → QImage → QWidget paint + screenshot proof — details below | pending | 0 | needs:task:WK02 |
| WK04 | Spike: input + eval smoke — synthetic click navigates, JS eval returns a typed value — details below | pending | 0 | needs:task:WK03 |
| WK05 | Spike verdict doc — .devin/WEBKIT-SPIKE.md per-member table (the SERVO-SPIKE.md shape) — details below | pending | 0 | needs:task:WK04 |
| WK06 | Build gate — engine.pri `webkit=1` CONFIG, pkg-config detection, ARKTOS_WEBKIT, no-libs honesty, opt-outs — details below | pending | 0 | needs:task:WK01 |
| WK07 | WebkitBackend class + registry probe + "WebKit x.y.z" displayName + conservative capabilities — details below | pending | 0 | needs:task:WK06 needs:task:WK05 |
| WK08 | WebkitView/Page minimal — headless WPEView + raster paint in a QWidget + EngineTab hosts it unchanged — details below | pending | 0 | needs:task:WK07 |
| WK09 | Load signals — load-changed/progress/title/uri/favicon → Page signal contract; stop/reload — details below | pending | 0 | needs:task:WK08 |
| WK10 | History — WebKitBackForwardList → historyItems/backItems/goToHistoryEntry — details below | pending | 0 | needs:task:WK09 |
| WK11 | Profile storage — per-id WebsiteDataManager under profiles/<id>/WebKit + OTR ephemeral session — details below | pending | 0 | needs:task:WK09 |
| WK12 | clear(StorageAreas) + cookie accept-policy mapping — details below | pending | 0 | needs:task:WK11 |
| WK13 | Input — pointer/scroll/key events → wpe event API + resize — details below | pending | 0 | needs:task:WK08 |
| WK14 | Zoom + find + lifecycle — set_zoom_level, WebKitFindController, suspend/resume (symbol-gated) — details below | pending | 0 | needs:task:WK09 |
| WK15 | JS eval typed results — evaluate_javascript + JSCValue→QVariant marshaling — details below | pending | 0 | needs:task:WK09 |
| WK16 | Script injection + worlds + host channel — UserContentManager, world-name mapping, script-message reply — details below | pending | 0 | needs:task:WK11 needs:task:WK15 |
| WK17 | Navigation veto + custom schemes — decide-policy → RequestPolicy; register_uri_scheme → arktos-file:/abp: — details below | pending | 0 | needs:task:WK11 |
| WK18 | Content-blocker adblock — rule→JSON subset converter + capability honesty — details below | pending | 0 | needs:task:WK17 |
| WK19 | Capabilities + surfaces — cert errors, context menu, permissions, downloads, settings/About/"WebKit x.y.z" consistency — details below | pending | 0 | needs:task:WK16 needs:task:WK18 |
| WK20 | Perf — dmabuf/EGLImage → GL texture path (raster stays fallback) — details below | pending | 0 | needs:task:WK08 |
| WK21 | Stretch audit — SOCKS5/Tor leak check, WebExtensions probe, WebKitWebInspector embed — details below | pending | 0 | needs:task:WK19 |

## Task details

### WK00

WPE availability probe + acquisition — the lane's feasibility gate.
(a) Probe first: `pkg-config --exists wpe-webkit-2.0 wpe-platform-2.0`,
ldconfig scan for libWPEWebKit — the user may already have it.
(b) If absent, acquire WITHOUT sudo: preferred path = download Debian
sid `libwpewebkit-2.0-1` + `libwpewebkit-2.0-dev` .debs (plus missing
dep .debs — libsoup3/gstreamer mostly already present via the
installed libwebkit2gtk) and `dpkg -x` into `~/wpe`. BEFORE choosing
sid: check its binaries' symbol floors against host glibc 2.39/glib
2.80 (extract first, `LD_LIBRARY_PATH=~/wpe/usr/lib/... ldd -r` for
missing symbols). If sid is too new, try trixie's 2.48.x (note: that
is the legacy libwpe API — record the API-level consequence in the
env file header). If debs can't resolve, try a Flatpak SDK runtime
containing WPE. Building WebKit from source is LAST RESORT (hours,
~30GB) — only detached via nohup, and `blocked:` is a legitimate
outcome if nothing resolves.

Verify: libraries physically present under ~/wpe (or proven already
system-installed). Do NOT proceed to WK01 in the same run — the
env-file task is separate.

### WK01

webkit-env.sh + link/run probe — needs:task:WK00. Write
`.devin/webkit-env.sh` — sourceable, mirrors qt-env.sh's shape: sets
PKG_CONFIG_PATH/LD_LIBRARY_PATH/LD_RUN_PATH (+C_INCLUDE_PATH if
headers live in the prefix) so `pkg-config --modversion
wpe-webkit-2.0` resolves. Header comment documents what was installed
and which API level (WPEPlatform ≥2.54 / transitional 2.4x-2.5x /
legacy libwpe). Then compile+run a trivial probe:
`webkit_get_major_version()/minor/micro` printed — proves headers,
lib, and runtime loading all work.

Verify: `source .devin/webkit-env.sh && pkg-config --modversion
wpe-webkit-2.0` prints ≥2.44 and the compiled probe binary runs and
prints a version triple.

### WK02

Spike: harness skeleton + event-loop verdict — needs:task:WK01.
Standalone project at `spikes/webkit/wpe-harness/` (own qmake project,
NOT in top-level SUBDIRS — `make check` untouched). (a) Create a
WPEDisplayHeadless display + WPEToplevel + WPEView + WebKitWebView
sized 800x600. (b) THE EVENT-LOOP QUESTION, first-class: attempt 1 —
view on Qt GUI thread, rely on Qt's glib event dispatcher
(QEventDispatcherGlib is default in most Qt builds); if
load-finished never arrives, attempt 2 — QBasicTimer driving
g_main_context_iteration(false) ~60Hz; attempt 3 — dedicated
std::thread running g_main_loop_run + QMetaObject::invokeMethod
queued marshaling into Qt. Record which model works and WHY (the
answer becomes the backend's threading contract — document it in the
harness header comment).

Verify: harness binary starts, creates the view, and a `load uri`
call is issued without deadlock/crash; the loop-model finding is
written into the source comment AND carried forward to WK05's doc.

### WK03

Spike: render path — needs:task:WK02. Take the view's produced
WPEBuffer, `wpe_buffer_import_to_pixels()` → QImage → QWidget
paintEvent; resize handling (re-size the WPEView on resizeEvent).
Prove with an actual rendered page: `https://example.com`, or a
bundled file:// fixture if the sandbox has no network.

Verify: harness saves a non-empty PNG (assert >N non-background
pixels, not just file-exists) — screenshot artifact committed under
spikes/webkit/ as evidence. A green-painted blank page does NOT
count as done.

### WK04

Spike: input + eval smoke — needs:task:WK03. (a) translate a
QMouseEvent click into the wpe pointer-event API and click a link in
a two-page fixture → assert navigation happened (uri property
changed). (b) `webkit_web_view_evaluate_javascript` → assert the
JSCValue result round-trips a typed value (number + string).
(c) If a wpe keyboard API was exercised, note the keysym mapping
shape in a comment.

Verify: click navigates, eval returns correct typed values — print
PASS/FAIL lines and exit nonzero on failure.

### WK05

Spike verdict doc — needs:task:WK04. Write `.devin/WEBKIT-SPIKE.md`
in the SERVO-SPIKE.md format: per-member verdict table over the
Engine interface (works/partial/absent + evidence), the five gating
questions answered (event-loop model decision documented, buffer
path, packaging status from WK00, interception story, version floor),
and the recording of which WebKit version/API level the findings
apply to. Quick source-survey members (decide-policy, uri-scheme,
cookie manager, download, tls-error, suspend, proxy settings) get
source/API-doc evidence — members needing runtime proof get it.

Verify: doc committed; every Engine::Page/Profile member has a
verdict row; the capability recommendations feed WK07.

### WK06

Build gate — needs:task:WK01. engine.pri `webkit` CONFIG block
mirroring the servo stanza exactly: pkg-config detect
wpe-webkit-2.0 + wpe-platform-2.0, `PKGCONFIG +=` them, -DARKTOS_WEBKIT,
message "Arktos engine backends: webengine + servo? + webkit" style
listing, opt-outs `webkit=0`/`nowebkit=1`/`CONFIG-=webkit` symmetric
with noservo. When libs absent: backend still compiles (file is in
the build but initialize()/isSupported() report unavailable — match
the servo pattern of compile-but-inert, NOT conditional source
exclusion — check servobackend's approach and mirror it). No edit to
engineregistry.cpp (the addProbe seam exists for this).

Verify: `qmake webkit=1 && make` links with libs present;
`qmake webkit=0` and `qmake CONFIG-=webkit` build identical binaries
without webkit symbols; default `qmake` picks sane auto-detect.

### WK07

WebkitBackend class — needs:task:WK06 + WK05 (capabilities informed
by the verdict doc). `src/engine/webkitbackend.{h,cpp}`:
id()="webkit"; displayName()=`WebKit %1.%2.%3` from
webkit_get_*_version() — the user's explicit ask is the switcher
showing the version like "Servo 0.7.0"; engineNote() honest
experimental line; capabilities() = the spike-verified table,
conservative (only what WK01-04 proved); initialize()=lib-loadable
probe; pageUnavailableReason() when libs absent. Skeleton
WebkitProfile/WebkitPage/WebkitView classes compiling. Registration
via a static-init addProbe — no engineregistry.cpp edit.

Verify: `--engine-list` shows three entries with real version;
settings combo lists it; WITHOUT libs the binary builds, lists it
unavailable, menu hides it (or shows disabled-with-reason — match
what Servo does). Small autotest leg: registry contains "webkit",
displayName parses a version triple, caps honesty.

### WK08

WebkitView/Page minimal — needs:task:WK07. Real (if minimal)
WebkitView = QWidget subclass painting imported buffers (WK03 code,
productized); WebkitPage wraps WebKitWebView+WPEView created on the
WK02-decided loop model; WebkitProfile minimal session object. A
load() issued on the page must actually render into the widget.
EngineTab hosts it with ZERO changes — verify by instantiating.

Verify: autotest leg — EngineTab on the webkit backend loads a
fixture URL and the view paints non-blank pixels (QImage grab, or
the loadFinished signal at minimum). Offscreen only.

### WK09

Load signals — needs:task:WK08. load-changed →
loadStarted/loadFinished(bool); notify::estimated-load-progress →
loadProgress (real fractions — Servo can't, we can);
notify::uri → urlChanged; notify::title → titleChanged;
notify::favicon → iconChanged. stop()/reload() wired.
NavigationType from WebKitNavigationType where the enum surfaces.

Verify: autotest — load a fixture, assert signal order
(started→progress→finished) and title/url/favicon arrival.

### WK10

History — needs:task:WK09. WebKitBackForwardList →
historyItems/backItems/forwardItems/canGoBack/canGoForward/
goToHistoryEntry/currentHistoryIndex. These are REAL history objects
(get_list/back_list/forward_list) — map them directly, unlike
Servo's synthetic tracking.

Verify: autotest — two-page session, back/forward round-trip, item
urls/titles correct, currentHistoryIndex tracks.

### WK11

Profile storage — needs:task:WK09. Per-profile
WebKitWebsiteDataManager/WebKitNetworkSession with base_directory
rooted in the profile's storage tree (profiles/<id>/WebKit or the
VSEC02-approved equivalent — pick a name that can't collide with the
QtWebEngine dir; document the choice). OTR → ephemeral session —
assert nothing persists. storageName plumbing from ProfileOptions.

Verify: autotest — cookie/localStorage set on profile A invisible
to profile B; artifacts land under the profile dir (assert path);
OTR session writes zero files.

### WK12

clear() + cookie policy — needs:task:WK11. Profile::clear(StorageAreas)
→ webkit_website_data_manager_clear flag mapping (cache/cookies/
dom-storage/serviceworker flags). Cookie accept-policy →
WebKitCookieManager set_accept_policy + set_persistent_storage for
the named profile. KeepPolicy mapping honest.

Verify: autotest — clear(CookiesArea) empties the store
(cookie-manager read-back), clear(AllAreas) drops storage.

### WK13

Input — needs:task:WK08. QWidget events → wpe event API: pointer
move/button, wheel (line+pixel delta handling — check whether wpe
takes pixel or step units), key press/release with keysym
translation, resize → wpe_view size. WebKit drives its own frame
schedule — no Servo-style coalescing needed.

Verify: spike-harness-style or autotest — synthetic click navigates,
key events reach the page (eval reads a keydown log), resize re-renders.

### WK14

Zoom + find + lifecycle — needs:task:WK09. setZoomFactor/zoomFactor →
webkit_web_view_set_zoom_level. findText → WebKitFindController
(search + found/did-fail → FindResult). setLifecycleState →
webkit_web_view_suspend/resume IF the resolved WPE exports it —
symbol/version-gate like servo's optional exports; absent → honest
Active-only capability note.

Verify: autotest — zoom changes render scale (screenshot diff or
zoom_factor read-back), findText reports found, suspend/resume if
available.

### WK15

JS eval typed results — needs:task:WK09. runJavaScript →
evaluate_javascript with JSCValue→QVariant marshaling
(string/number/bool/null; object → JSON.stringify documented limit;
undefined→invalid). Error propagation to the result callback.
toHtml → document.documentElement.outerHTML eval.

Verify: autotest — eval returns typed QVariant per type; syntax
error surfaces as error, not crash.

### WK16

Script injection + worlds + host channel — needs:task:WK11 + WK15.
insertScript → WebKitUserContentManager add_script: injection-time
mapping (DocumentCreation→start, Deferred→end), world name mapping —
Engine::ArktosChannelWorld (0xa0) onto WebKit named script worlds so
the SEC24 private-world contract holds. runJavaScriptLifted actually
runs in the named world. Host channel:
webkit_user_content_manager_register_script_message_handler
(_with_reply on 2.5x — version-gate) → the native arktos-channel
transport; evaluate which JS assets (autofill.js, arktos-channel.js,
reader) can run — capability-gate whatever needs a missing reply
surface, never fake.

Verify: autotest — injected script at DocumentCreation in named
world; message round-trip; main-world page JS cannot enumerate the
private-world objects (SEC24 shape).

### WK17

Navigation veto + custom schemes — needs:task:WK11. decide-policy
signal → Engine::RequestPolicy::decide for navigation + new-window
actions (main-frame allow/deny; external-scheme prompt analog).
webkit_web_context_register_uri_scheme → serve arktos-file:, abp:,
arktos-resource:, arktos-cert-error:, arktos-http-warning:,
arktos-site-block: through the same engine-agnostic serving adapters
as backend-0 where separable.

Verify: autotest — blocked nav never loads (fixture policy denying
a marker URL); arktos-file: serves a real listing; abp: subscribe
flow reaches the app.

### WK18

Content-blocker adblock — needs:task:WK17. THE interception answer:
WebKit has no per-request veto — compile adblock rules into WebKit
content-blocker JSON via WebKitUserContentFilterStore. Scope
honestly: converter covers url-filter + resource-type + block/
block-cookies/css-display-none actions; $redirect/stub-serving has
NO equivalent → degrade to block, count + log the drops. Then the
capability decision: is requestInterception true (for the block
subset)? If the honest answer needs a new flag
(declarativeBlocking), add it additively to engineinterface.h with a
doc comment — never fake parity. Cookie blocking rules map to the
filter's block-cookies action.

Verify: autotest — an adblock fixture URL blocked via compiled
filter; cosmetic rule hides an element; test output prints the
converted/dropped rule counts.

### WK19

Capabilities + surfaces — needs:task:WK16 + WK18. Fill the
Capabilities table with verified truth (update as WK18 settled the
interception question). Wire: cert errors
(load-failed-with-tls-errors + CertificateInfo → our interstitial
contract — match WebEngineBackend's surface), context-menu →
ContextMenuInfo, WebKitPermissionRequest → WebPermissionManager,
WebKitDownload → DownloadManager/rustdl handoff (whatever generic
interface exists), renderProcessId (real WebProcess pid if exposed,
else -1). CONSISTENCY PASS — the user's requirement: "WebKit x.y.z"
renders identically in the engine indicator tooltip + menu, the
settings default-engine combo, the About dialog engine line, and
--engine-list. Verify the Tor lock is consulted (WebKit must not be
offered for Tor tabs until WK21 clears it).

Verify: `make check` green (minus documented env flakes); a fresh
profile browses a fixture page end-to-end on the webkit backend
through real chrome.

### WK20

Perf path — needs:task:WK08. wpe_buffer_import_to_egl_image →
QOpenGLWidget/QRhiWidget texture — dmabuf import + fence handling
(renderingFence/releaseFence). Keep raster copy as fallback when EGL
import fails. This is the heavy task; it exists to prove the path,
not to benchmark.

Verify: autotest legs pass on the GL path; a rough frame-time log
shows the difference; raster fallback still works when import is
unavailable.

### WK21

Stretch audit — needs:task:WK19. (a) SOCKS5: set a session
WebKitNetworkProxySettings socks:// proxy, audit leaks — does DNS
resolve remotely? WebRTC host candidates leak? Captive-portal/aux
requests bypass? Only if clean does a "webkit allowed for Tor"
capability exist — default stays Chromium-locked. (b) WebExtensions:
does the resolved WPE version expose the WebKitWebExtension GLib API
(2.52+ GTK, WPE status varies)? Set caps.extensions to the verified
answer. (c) WebKitWebInspector: embeddable view or separate-window
only — affects the devTools capability.

Verify: verdict entries appended to .devin/WEBKIT-SPIKE.md;
capability flags match measured reality.
