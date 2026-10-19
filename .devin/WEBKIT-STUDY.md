# WEBKIT-STUDY — modern WebKit as a third engine backend (study only)

Date: 2026-10-18 · Status: study, no code · Scope: "keep everything, add
WebKit the way Servo was added" — i.e. a new `Engine::Backend`
implementation, registered in `EngineRegistry`, hosted by `EngineTab`,
capability-gated exactly like `ServoBackend`.

"Modern WebKit" on Linux means two shipping ports:
- **WPE WebKit** — the toolkit-free embedder port. No GTK dependency;
  the host supplies a *platform implementation* that receives rendered
  buffers and feeds input back. This is the port designed for exactly
  what we want to do.
- **WebKitGTK** — the GTK-widget port (`WebKitWebView` is a
  `GtkWidget`). Cannot be embedded into a Qt Widgets app (GTK4 removed
  foreign-window embedding; there is no live offscreen-render API —
  `get_snapshot` is single-shot thumbnails only). **Rejected for tab
  embedding**; see option C.

macOS-only aside: on macOS the same study slot is `WKWebView` — native
`NSView` embedding, trivially the right WebKit there, but it is a
different adapter, not this one. Note it for the cross-platform matrix.

## What the Servo path looks like (the template)

- `spikes/servo/servo-embed/` — Rust cdylib exposing a ~30-function C
  ABI (`se_init`, `se_load`, `se_frame_copy`, `se_mouse_button`,
  `se_set_load_gate`, ...), dlopen'd by the backend. Artifact absent →
  backend reports unavailable → chrome hides the menu entry.
- `src/engine/servobackend.{h,cpp}` (~2.1k LOC) — `ServoBackend`
  (factory + artifact resolution), `ServoProfile`, `ServoPage`,
  `ServoView` (QWidget: paints `se_frame_copy`'d `QImage`, forwards
  input, pumps `se_spin` on the GUI thread).
- `EngineTab` hosts `Engine::Page` + view in the tab strip; every
  missing surface (find, cert errors, downloads, devtools) degrades
  through `Capabilities` instead of faking it.
- `src/engine/engine.pri` — `qmake servo=1` gate + artifact detection
  + build-time warnings.

WebKit fits this shape *better* than Servo does, because the WebKit2
embedder API was designed for this: real profiles, real multiprocess
isolation, real JS results, real history objects.

## The five integration questions

1. **Which port/API level** — WPE, and specifically the **WPEPlatform
   API** (stable since WPE 2.54, Oct 2026; libwpe/WPEBackend-fdo is now
   the deprecated legacy path). WPEPlatform is built *into* WPE WebKit
   and ships three built-in platforms: `wayland`, `drm`, `headless`
   (selected via `WPE_PLATFORM`), plus the contract for custom ones —
   an integrator subclasses `WPEDisplay`/`WPEToplevel`/`WPEView`. Our
   options reduce to "use a built-in platform" vs "write a Qt
   platform" (options below).

2. **Getting pixels into a QWidget** — WPE hands the UI process a
   `WPEBuffer` per frame (DMA-BUF on GPU, `WPEBufferSHM` in software).
   The buffer API gives two importers: `wpe_buffer_import_to_pixels()`
   (CPU copy → `QImage`, the `se_frame_copy` equivalent) and
   `wpe_buffer_import_to_egl_image()` (zero-copy → GL texture). Raster
   copy first, dmabuf→texture as the perf upgrade — same staging as
   Servo.

3. **Event loop** — the WPE UI-process API runs on a `GMainContext`.
   Qt on Linux is usually built with the glib event dispatcher
   (`QEventDispatcherGlib`), which already pumps the *default* context
   — meaning WPE work posted to the default context likely Just Works
   inside our GUI thread. Verify in the spike; fallback is a dedicated
   GLib thread with `g_main_loop_run` + queued marshaling to Qt, which
   is strictly more work but fully deterministic.

4. **Packaging** — the real blocker for this machine: **Ubuntu 24.04
   ships no WPE package at all** (only `libwebkit2gtk-4.1` 2.52).
   Debian sid/testing carries `libwpewebkit-2.0-1` 2.54.1 + `-dev`;
   Fedora and Arch ship it too. Options: target distros that have it,
   build WPE ourselves (a real WebKit build — heavy, CI-scale), or
   bundle the `.so` set in our own packages. Until a distro package or
   bundled artifact resolves, the backend is compiled-in but absent —
   the same `artifactAvailable()` shape as Servo, so nothing regresses.

5. **The shim boundary** — unlike libservo, WPE is a real system
   library with a stable C ABI and pkg-config (`wpe-webkit-2.0`,
   `wpe-platform-2.0`). Two viable shapes:
   - **Direct link**: `webkitbackend.cpp` compiles against the
     headers, `CONFIG+=webkit` gates it; simplest, no shim artifact.
   - **Shim**: `spikes/webkit/webkit-embed/` C++ cdylib exporting
     `wk_*` over GLib calls, dlopen'd like servo-embed. Costs an extra
     translation layer but keeps the app binary dependency-free and
     lets the artifact be versioned/swapped. **Recommended**: direct
     link — a system library doesn't need the shim ceremony Servo did
     (Servo needed it because libservo has no stable C ABI and the
     artifact is huge/out-of-tree).

## Options

### Option A — WPEPlatform `headless` platform + raster copy (recommended start)

`WPEDisplayHeadless` renders offscreen and still delivers `WPEBuffer`s
— that is the entire missing piece between "WebKit" and "a QImage we
can paint". The backend:

- `WebkitBackend::initialize()` — verify glib dispatcher (or spawn
  the GLib thread), `WPEPlatform` init, `WebKitNetworkSession` setup.
- `WebkitProfile` — one `WebKitWebsiteDataManager`/`WebKitNetworkSession`
  per `storageName`, `base_directory` pointed inside
  `profiles/<id>/QtWebEngine`-adjacent storage (profile-rooted, per the
  VSEC02 layout); OTR → `webkit_network_session_new_ephemeral()`.
- `WebkitPage`/`WebkitView` — `WebKitWebView` on a `WPEView` under a
  headless toplevel sized to the widget; `buffer` callback →
  `wpe_buffer_import_to_pixels` → `QImage` → `paintEvent`. Input:
  `wpe_view` event API from `QWidget` events. Zoom/scroll keys map 1:1.

Same honest-degradation contract as Servo, but the capability table
flips mostly to true (see map below). Raster copy is CPU-bound —
acceptable for parity with Servo's current path, not for daily-driver
smoothness; that is what Option B/D upgrades.

### Option B — WPEPlatform `headless` + DMA-BUF/EGLImage → GL texture

Same as A but `import_to_egl_image()` → `QOpenGLWidget`/`QRhiWidget`
texture. Near-native scroll/video perf. Needs EGL context sharing
between WPE's rendering and Qt's — this is the work upstream's Qt6
binding does at scene-graph level. Defer until A proves the plumbing;
this is the "make it fast" phase, not the "make it work" phase.

### Option C — upstream WPE Qt6 binding (`ENABLE_WPE_QT_API`, `libqtwpe`)

WebKit's own tree contains `Source/WebKit/UIProcess/API/wpe/qt6/` —
`WPEQtView` is a `QQuickItem` doing the full WPEPlatform → scene-graph
integration (damage, vsync, dmabuf) properly. Two catches: **nobody
packages it** — you must build WebKit from source (multi-hour build,
huge tree, per-host toolchain), and it's Qt Quick, so `EngineTab`
would host a `QQuickWidget` (works, slight compositing cost) rather
than a plain `QWidget`. Right answer if we ever ship a vendored WPE;
wrong answer for "develop against system packages".

### Option D — WebKitGTK in-process — REJECTED

`WebKitWebView` is a `GtkWidget`: needs a GTK window hierarchy,
GTK4 dropped XEmbed/foreign-window hosting, Wayland has no such
mechanism at all, and there is no offscreen live-render API (snapshots
are async single-frames). Would only ever work as a separate window —
not a tab backend. Listed so nobody re-evaluates it.

### Option E — out-of-process compositing (separate browser process + frame pipe)

Run a WPE mini-browser in its own process and composite frames —
rejects itself: doubles the latency/IPC problem to get what
WPEPlatform already gives in-process, and breaks every page-facing
API. Rejected.

## Engine::Page/Profile mapping (what each capability lands as)

| Interface member | WebKitGLib/WPE surface | Verdict |
|---|---|---|
| load/stop/reload/url | `webkit_web_view_load_uri/stop_loading/reload`, `g_uri` | works |
| title/url/load signals | `notify::title`, `notify::uri`, `load-changed` (has progress via `notify::estimated-load-progress`!) | works — better than Servo (no progress there) |
| history nav + items | `WebKitBackForwardList` (get_list, back_item, go_to_back_forward_list_item) | works — real objects, not just can-go |
| zoom | `webkit_web_view_set_zoom_level` | works |
| `runJavaScript` + results | `webkit_web_view_evaluate_javascript` → `JSCValue` (typed return!) | works — fixes Servo's fire-and-forget |
| script injection + worlds | `WebKitUserContentManager` `add_script` with `world_name`; script-message handlers → two-way JS↔host channel | works — better than Servo (real isolated worlds, real replies; covers ArktosChannelWorld, autofill, fingerprint protector) |
| per-page settings | `WebKitSettings` per view: javascript-enabled, loads-images, media, UA | works — fixes Servo's "global prefs only" |
| `findText` | `WebKitFindController` | works |
| `setLifecycleState` | `webkit_web_view_suspend`/`resume` (2.48+) | works (throttle-suspend) |
| `renderProcessId` | multiprocess model is real; `web-process-terminated` for crash signal | works (pid surfacing TBD in spike) |
| `createWindow` (popups) | `create` signal + `WebKitNavigationAction` | works |
| nav-policy gate | `decide-policy` signal (navigation + new-window actions) | works for navigation veto |
| **request interception** | **No general UI-process request interceptor.** Custom schemes: `webkit_web_context_register_uri_scheme` (covers arktos-file/abp/arktos-resource — real scheme handlers). Subresource blocking: `WebKitUserContentFilterStore` — compiled JSON content-blocker rules (block / block-cookies / css-display-none / make-https). | **partial — the one real gap**: adblock maps onto content-blocker conversion (rule-format translation needed, no `$redirect` stub serving, no per-request verdict callback, no referer override). `Capabilities.requestInterception` degrades or we ship a filter-compilation path. |
| cookie filter | `WebKitCookieManager` accept-policy enum (always/never/no-third-party) + `cookies-changed` | partial — third-party gate yes; our per-attempt custom verdict callback has no equivalent → capability-gate honestly |
| downloads | `WebKitDownload` + `download-started` | works |
| cert errors | `load-failed-with-tls-errors` + `allow_tls_certificate_for_host` + `get_tls_info` | works — fixes Servo's biggest security gap |
| permissions broker | `WebKitPermissionRequest` signals (geolocation, media, notification…) | works |
| context menu | `context-menu` signal + `WebKitContextMenu` + hit-test | works |
| custom schemes | `register_uri_scheme` → `WebKitURISchemeRequest` callback | works |
| proxy / SOCKS5 | `WebKitNetworkProxySettings` — accepts `socks://` per session | **works — Tor on WebKit is conceivable** (unlike Servo). Needs a leak audit before claiming it (DNS, WebRTC, captive checks) — Tor stays Chromium-locked until audited. |
| devtools | `WebKitWebInspector` (attachable/detached window) + WPE WebDriver (`wpewebkit-webdriver`) | works — better than Servo's dead FRP socket; inspector embedding into our window TBD |
| extensions | `WebKitWebExtension` GLib API — landed 2.52 (GTK), WPE port in progress | partial/emerging — `extensions` capability stays false at first, flips when WPE ships it |
| session restore | `WebKitWebViewSessionState` serialize/restore | works — covers RCORE03 opaque-blob need |
| print/PDF | `WebKitPrintOperation` is GTK-only; WPE has no print API | gap — reuse shell `PdfView` path (already the EngineTab story) or leave capability off |
| media | full via GStreamer (needs gst plugins installed) | works (host dependency) |

Net: WebKit lands ~85-90% of the interface vs Servo's ~40%. The two
honest gaps are **request-level interception** (content-blocker rules
instead of per-request verdicts) and **printing**. Everything else is
equal or better than backend-0's surface.

## What would be needed — work breakdown

1. **WK01 spike** — mirror of ENG03: a `spikes/webkit/webkit-embed`
   harness (standalone, outside SUBDIRS) that: starts a headless
   `WPEDisplay`, loads a URL, imports a `WPEBuffer` to pixels, paints
   it in a `QWidget`. Answers: does Qt's glib dispatcher pump WPE's
   context? (Decides thread model.) Input forward smoke.
   Deliverable: `.devin/WEBKIT-SPIKE.md` with the same per-member
   verdict table as SERVO-SPIKE.
2. **WK02 backend skeleton** — `src/engine/webkitbackend.{h,cpp}`:
   `WebkitBackend`/`WebkitProfile`/`WebkitPage`/`WebkitView` adapted
   from the servobackend shape (~2k LOC, mostly mechanical); pkg-config
   `wpe-webkit-2.0` + `wpe-platform-2.0`; `engine.pri` `webkit=1`
   gate + `isSupported()` probe (`.so` resolvable); `EngineRegistry`
   probe. EngineTab unchanged.
3. **WK03 navigation + signals** — load/uri/title/progress/back-forward.
4. **WK04 profile storage** — data manager rooted in the profile dir;
   OTR ephemeral session; `ClearPrivateData` → `WebsiteDataManager::clear`.
5. **WK05 input + zoom + find**.
6. **WK06 JS bridge** — evaluate_javascript + script-message world
   channel (this replaces the whole QWebChannel-adjacent path for this
   backend: autofill, reader, arktos-channel.js get a native transport).
7. **WK07 policy mapping** — `decide-policy` for navigation veto;
   content-blocker store for adblock rules (needs the filter-JSON
   converter — honest scope question: reuse an existing ABP→CB
   translator or write a subset); cookie accept-policy mapping;
   `insertScript` → UserContentManager.
8. **WK08 capabilities + chrome honesty** — fill the Capabilities
   table, wire the switcher menu, badges, settings pages that degrade.
9. **WK09 perf** — dmabuf→GL texture path (Option B).
10. **WK10 stretch** — SOCKS5/Tor leak audit; WebExtensions once WPE
    ships the 2.52+ API broadly; inspector embedding.

## Honest risks

- **Distribution**: biggest real-world risk — WPE 2.54 only exists in
  Debian sid/testing, Fedora, Arch today; this machine's Ubuntu 24.04
  has none. Browser can't ship a hard dep on it until packages spread
  or we bundle — hence the "compiled-in, dlopen/probe, hide-when-absent"
  pattern is the right one regardless.
- **Media stack**: GStreamer plugins are an implicit host dep.
- **Interception gap**: our adblock/privacy layer is interceptor-shaped;
  WebKit wants content-blocker-shaped. This is a *policy-format*
  translation problem, not a veto problem — decide whether the
  NavigationPolicy contract gains a "compile to declarative rules" leg
  or the backend reports `requestInterception=false` honestly (Servo
  actually beats WebKit here — `load_web_resource` is a full
  intercept-or-replace; WebKit's is a compiled filter store).
- **Two GLib threads + Qt**: low risk, but the spike must prove the
  dispatcher assumption before building on it.
- **Multi-view (checked 2026-02-20, user report)**: Servo's one-tab
  constraint is our servo-embed shim's design — NOT something to
  copy here. WPE has no single-view limit: one `WebKitWebContext`
  hosts N `WPEView`s/`WebKitWebView`s natively (multi-view is the
  normal embedding model, and each view can carry its own
  WebKitWebsiteDataManager if we want per-tab stores — though our
  profiles make that unnecessary). The backend MUST keep per-page
  state per-view and allow `createPage` unconditionally — do NOT
  port Servo's g_servoSlot single-live-instance gate.
- **Version floor**: target WPE 2.54+ (stable WPEPlatform); earlier
  releases exist in Debian stable (2.48, legacy API) — set the floor
  deliberately rather than chasing both APIs.

## Recommendation

Option A (headless WPEPlatform + raster copy), direct-linked against
the system `wpe-webkit-2.0`, registered exactly like ServoBackend.
It reuses *every* piece of the Servo integration story — EngineTab,
Capabilities honesty, the registry probe, profile-rooted storage — and
lands a backend that is *more* capable than backend-0 in two places
immediately (typed JS results, real isolated script worlds) and opens
two doors Servo can never (SOCKS5 → potential Tor offload, real
devtools). The one structural concession is adblock/privacy moving
from per-request veto to compiled content-blocker rules on this
engine — feasible, but it's the work item to scope honestly in WK01.
