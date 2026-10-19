# WK01–WK04 — WPE WebKit embeddability spike (verdict doc)

Date: 2026-10-19 · WPE WebKit pinned at Debian sid `libwpewebkit-2.0-1`
**2.54.1-1** (libWPEWebKit-2.0.so.1.11.4, `dpkg -x` into `~/wpe` — no
sudo, no repo commit; inventory + repairs in `~/wpe/ACQUIRED.md`) · API
level: **WPEPlatform ≥ 2.54 stable** (`WPEDisplay`/`WPEToplevel`/
`WPEView`, `WPEBuffer` + `wpe_buffer_import_to_pixels` — NOT the legacy
libwpe/WPEBackend-fdo path) · findings verified against the installed
headers in `~/wpe/usr/include/wpe-webkit-2.0/wpe/` AND the live .so,
not upstream docs.

Spike artifacts: `spikes/webkit/probe/` (link/run smoke) +
`spikes/webkit/wpe-harness/` (Qt Widgets harness, standalone qmake
project — NOT in the top-level SUBDIRS, so `make check` is unaffected
by design). Runtime evidence PNGs: `wpe-harness/wk03-*.png`.

Method note: WPE WebKit 2.54's surface is RICHER than the task premise
assumed in several places (real load-progress fractions, per-view
`WebKitSettings`, `WebKitNetworkSession` as a true per-profile object,
a Safari-format content-blocker pipeline that plugs straight into
WKB01's EasyList→JSON converter, a `WebKitWebExtension` MV2/MV3 API) —
and poorer in two (no suspend/resume, no OS-level renderer PID).
Verdicts below are per `Engine::` interface member
(`src/engine/engineinterface.h`, `.devin/ENGINE.md` §"Engine-neutral
interface surface"), graded `works` / `partial` / `absent` /
`unproven`. "Works" means runtime-proven by the harness where a spike
task demanded it, header+symbol verified otherwise.

## Per-member verdict

| Interface member | Verdict | Evidence / bound |
|------------------|---------|------------------|
| `Backend::initialize` + event loop | works | WK02 measured all three pump models: **(1) `QPAEventDispatcherGlib` pumps WebKit's GSources natively on the Qt GUI thread — THE BACKEND MODEL** (WebKit binds `g_main_context_get_thread_default()` = global default on the GUI thread; zero extra code, file:// page to FINISHED in ~160 ms sandboxed). (2) QT_NO_GLIB + 60 Hz `g_main_context_iteration` drain works (fallback only). (3) private ctx + `g_main_loop_run` on a std::thread works but has two measured traps (g_timeout_add-family attach global-default not thread-default; worker permanently becomes WebKit's "main thread" → normal exit self-SIGABRTs in static teardown, needs `_exit`/coordinated shutdown). Full contract in `wpe-harness.cpp` header. |
| `Page::load/stop/reload/url` | works | `webkit_web_view_load_uri` proven to STARTED→COMMITTED→FINISHED in all loop models; `webkit_web_view_stop_loading`, `webkit_web_view_reload` (+`_bypass_cache` for ReloadAndBypassCache), `webkit_web_view_get_uri`. |
| `Page` signals (load/url/title/icon/hover/close/progress) | works | `load-changed` (STARTED/REDIRECTED/COMMITTED/FINISHED) + `load-failed` → loadStarted/loadFinished; **`estimated-load-progress` property + notify → REAL fractions** (QtWebEngine-grade; better than Servo's none); `notify::uri`→urlChanged, `notify::title`→titleChanged; favicon via `WebKitFaviconDatabase::changed` + `webkit_favicon_database_get_favicon`/`webkit_web_view_get_page_icons`→iconChanged; `mouse-target-changed` + `WebKitHitTestResult`→linkHovered; `close` signal→windowCloseRequested; `web-process-terminated`/`get_is_web_process_responsive`→crash surface. |
| `Page` history nav | works | `WebKitBackForwardList` full API: `get_current_item`/`get_back_item`/`get_forward_item`/`get_nth_item`/`get_length`/`get_back_list_with_limit`/`get_forward_list_with_limit` → historyCount/currentHistoryIndex/historyItems/backItems/forwardItems; `webkit_web_view_go_to_back_forward_list_item` → goToHistoryEntry; `can_go_back/forward`. Entry objects carry uri+title (WebKitBackForwardListItem) — 1:1 with `HistoryEntry`. |
| `Page` zoom | works | `webkit_web_view_set_zoom_level`/`get_zoom_level` (text zoom); `set_magnification`/`get_magnification` for page-scale. |
| `Page::scrollPosition` | partial | No scroll-offset getter on `WebKitWebView`; the proven `evaluate_javascript` path answers `window.scrollY`/`scrollX` on demand (async — undo-close snapshots need caching like the last-known-url pattern). |
| `Page` printing (`printRequested`) | absent | `WebKitPrintOperation` is only a forward typedef in 2.54 — no print functions exported (`nm -D` confirms). Print path stays WebEngine-only; adapter reports the entry disabled. |
| `Page::findText` | works | `webkit_web_view_get_find_controller` → `WebKitFindController`: `search` with `WebKitFindOptions` (CASE_INSENSITIVE/BACKWARDS/WRAP_AROUND/AT_WORD_STARTS), `search_next`/`search_previous`, `count_matches`, `found-text`/`failed-to-find-text`/`counted-matches` signals → async FindResult exactly like Qt. |
| `Page::runJavaScript` | works | **WK04 runtime-proven**: `webkit_web_view_evaluate_javascript` → `JSCValue*` round-trips typed values (is_number/to_double = 42, is_string/to_string). `world_name` parameter → per-world eval (runJavaScriptLifted's worldId maps to named `WebKitScriptWorld`s — 0→NULL default world). `call_async_javascript_function` with GVariant args also exists. |
| `Page::toHtml` | works | `webkit_web_view_get_main_resource` → `webkit_web_resource_get_data` (async GBytes = serialized doc), or evaluate `document.documentElement.outerHTML`. |
| `Page` standard actions | partial | No generic WebAction enum, but the pieces exist: `execute_editing_command`(+`_with_argument`)/`can_execute_editing_command` cover Cut/Copy/Paste/Undo/Redo/SelectAll; Back/Forward/Stop/Reload are the direct calls above; `webkit_web_view_save`/`_to_file` → MHTML SavePage; ViewSource→toHtml; InspectElement→`toggle_inspector`. The backend synthesizes QActions itself. |
| `Page::isLoading` / `recentlyAudible` | works | `webkit_web_view_is_loading`; `webkit_web_view_is_playing_audio` + `set_is_muted`/`get_is_muted` (mute toggle is a bonus). |
| `Page::isOffTheRecord` | works | `WebKitNetworkSession` per view: `webkit_network_session_new_ephemeral` / `is_ephemeral` — real OTR sessions. |
| `Page::setPageAttribute` (JSCTL JavascriptEnabled, popups) | works | **`WebKitSettings` is PER-VIEW** (`webkit_web_view_set_settings`) — `enable-javascript` + `enable-javascript-markup` (inline-script split: markup off leaves eval API working — ideal for JSCTL+runJavaScriptLifted), `javascript-can-open-windows-automatically`, `enable-media`, `auto-load-images`, etc. |
| `Page::setLifecycleState` | partial | **No suspend/resume/throttle API exists in 2.54** (absent from every header — verified). Discarded IS expressible: `webkit_web_view_get_session_state` → `WebKitWebViewSessionState` (serializable) → destroy the view → re-create + `restore_session_state` on wake = real unload/reload of process resources. Frozen has no primitive (closest: unmap the WPEView / unfocus — stops presenting, doesn't freeze JS). Honest `lifecycleDiscard`=true-possible, Frozen n/a. |
| `Page::renderProcessId` / `residentMemoryKb` | partial→absent | `webkit_web_view_get_page_id` is WebKit's internal PageID, not an OS pid; no renderer-PID API in the headers. `is-web-process-responsive` exists (crash/health, not identity). Report -1 honest — unless a later probe finds a /proc trick; memoryIsShared then =false-with-nothing. |
| `Page` popups (`createWindow`) | works | `WebKitWebView::create` signal returns a new WebKitWebView (`ready-to-show`, `WebKitWindowProperties` for geometry/toolbars) — POPUP01-style popup gate expressible via decide-policy NEW_WINDOW_ACTION too. |
| `Page::download(url)` / `DownloadRequest` | works (minus pause) | `webkit_web_view_download_uri` + `webkit_network_session_download_uri`; content-disposition downloads surface via decide-policy RESPONSE + `webkit_policy_decision_download` → **`WebKitNetworkSession::download-started`** announces `WebKitDownload` (get_request/get_destination/set_destination/get_response/cancel/`get_estimated_progress`/`get_received_data_length`/get_web_view + `decide-destination`/`received-data`/`finished`/`failed` signals). **Gap: no pause/resume** — DownloadRequest::pause/resume unimplementable (honest no-op); state byte counts are engine-side. |
| `RequestPolicy` — navigation gate | works (minus bf) | `decide-policy` signal: NAVIGATION_ACTION → `WebKitNavigationPolicyDecision` (`WebKitNavigationAction`: request/uri, navigation type LINK_CLICKED/FORM_SUBMITTED/BACK_FORWARD/RELOAD/FORM_RESUBMITTED/OTHER, mouse button+modifiers, is-user-gestured) → `webkit_policy_decision_use`/`ignore`/`download`. NEW_WINDOW_ACTION covers target=_blank. **Runtime caveat (WK10-verified): back/forward navigations emit NO decide-policy at all on 2.54** — the history-item path never consults the policy client (no NAVIGATION_ACTION, no RESPONSE), cacheable or not — so bf hops cannot be vetoed or type-sensed through this hook; `back()`/`forward()`/`goToHistoryEntry()` stamp NavigationType::BackOrForward at the call site instead. |
| `RequestPolicy` — subresource interception | partial | RESPONSE policy decisions DO fire for every resource incl. subresources (WebKitURIRequest+WebKitURIResponse — and unlike QtWebEngine we CAN SEE response headers/status here) → block = `ignore`, divert-to-download = `download`. **But no redirect/serve-stub for http(s)**: `register_uri_scheme` covers custom schemes only. The redirect/stub path is the **content-blocker pipeline** (below) or the injected-bundle web-process extension (`WebKitWebPage::send-request` — heavier, needs the bundle API). So: Allow/Block = full coverage; Redirect = content-blocker `redirect` rules only (no arbitrary rewrite). |
| Content blockers (adblock) | works | `WebKitUserContentFilterStore` (save/load compiled filters by id) + `WebKitUserContentManager::add_filter` — **Safari content-blocker JSON, exactly the format WKB01's rust converter already emits** (arktos_adblock_ffi `contentblocker.rs`). `block`/`block-cookies`/`css-display-none`/`make-https`/`redirect`-to-extension-resource actions → adblock + cosmetic filtering with zero engine patches. |
| `Profile` model (containers, OTR, storage) | works | **`WebKitNetworkSession` IS the profile object** (since 2.40): `webkit_network_session_new(data_dir, cache_dir)` per container/storage name, `new_ephemeral` for OTR, `get_website_data_manager`/`get_cookie_manager`/`set_itp_enabled`/`set_persistent_credential_storage_enabled`. Per-session state; WebKitWebContext holds process-global config. Per-profile proxy supported natively (below). |
| `Profile::setUserAgent` | works | `WebKitSettings` `user-agent` (per-view) + `set_user_agent_with_application_details`. |
| `Profile::setCookieFilter` | partial | `WebKitCookieManager` (`set_accept_policy`: ALWAYS/NEVER/**NO_THIRD_PARTY** — the standard third-party gate maps cleanly) + `set_persistent_storage`(txt/sqlite) + add/get/delete/replace/get_all cookies + `cookies-changed` signal. **No per-attempt callback** — the fine-grained `CookieAttempt{firstParty,origin,thirdParty}` functor cannot be honored 1:1 (site exceptions per-domain need content-blocker `block-cookies` triggers or JS-side gates). |
| `Profile` scripts (`insertScript`/`removeScript`/`scripts`) | works | `WebKitUserContentManager` per session/view: `webkit_user_script_new(source, frames MAIN/ALL, injection_time START/END, allows, blocklist, world_name)` + `add_script`/`remove_script`/`remove_all_scripts`, `add_style_sheet`. Named `WebKitScriptWorld`s = real isolated worlds incl. the SEC24 channel world (register_script_message_handler(_with_reply) per world → the arktos-channel story carries over). |
| `Profile::clear(StorageAreas)` | works (minus visited-links) | `WebKitWebsiteDataManager::clear` with `WEBKIT_WEBSITE_DATA_*` bitmask: MEMORY/DISK_CACHE, LOCAL/SESSION_STORAGE, INDEXEDDB, COOKIES, SERVICE_WORKER_REGISTRATIONS, HSTS_CACHE, DOM_CACHE, ALL — near 1:1 with our flags. No "visited links" type (WebKit tracks :visited internally; honest unmapped bit). |
| `CertificateErrorInfo` / cert override | works | `load-failed-with-tls-errors` signal (failing_uri, `GTlsCertificate`, `GTlsCertificateFlags`) → return TRUE to proceed; `get_tls_info` reads the live chain; `WebKitNetworkSession::set_tls_errors_policy` FAIL/IGNORE + `allow_tls_certificate_for_host` (session-scoped exception — maps AcceptOnce if we simply don't persist it; it's per-session anyway). GTlsCertificate→DER→QSslCertificate fills the chain list. SEC06-grade interstitial IS drivable (unlike Servo). |
| `ContextMenuInfo` | works | `context-menu` + `context-menu-dismissed` signals, `WebKitContextMenu`/`WebKitContextMenuItem` embedder-owned, `WebKitHitTestResult` (is_link/image/media/editable/selection/scrollbar + link_uri/title/label/image_uri/media_uri) — every field of `ContextMenuInfo` producible (isCanvas = editable+hit-test combo). |
| Permissions (SEC05 broker) | works | `permission-request` + `query_permission_state` signals; concrete request classes: Geolocation, UserMedia, MediaKeySystem, Notification, DeviceInfo, PointerLock, XR, WebsiteDataAccess. |
| Authentication / dialogs / file chooser | works | `authenticate` → `WebKitAuthenticationRequest`; `script-dialog` → `WebKitScriptDialog` (alert/confirm/prompt/before-unload); `run_file_chooser` → `WebKitFileChooserRequest`; `show_option_menu` (select popups); `show_notification`; `enter_fullscreen`/`leave_fullscreen`; `submit_form`; `insecure_content_detected`. |
| Proxy / SOCKS5 | works — verify for Tor | **`WebKitNetworkProxySettings` is PER-SESSION** (`webkit_network_session_set_proxy_settings(mode, settings)` — per-profile proxy, better than QtWebEngine's process-global scheme). Proxy URIs route through libsoup3/GIO (`GProxyResolver` supports `socks5://`/`socks5h://`-family + ignore_hosts). Tor windows are NOT permanently Chromium-locked like Servo — BUT the DNS-leak question (local vs remote resolution under socks5) is exactly what WK21's stretch audit must verify before Tor can leave WebEngine. |
| Devtools / BiDi | partial | `webkit_web_view_toggle_inspector` + `enable-developer-extras` + INSPECT_ELEMENT context action: the built-in WebKit inspector exists end-user-facing. **No WebKitWebInspector public header exported** → no embedder attach/socket/channel API on this build — the BiDi panel's CDP client has nothing to talk to. Conservative `devTools=false`. |
| Extensions | unproven→partial | `WebKitWebExtension` + `WebKitWebExtensionMatchPattern` + `web-extension-mode` MV2/MV3 DO exist in 2.54 (install path, manifest v2/v3 probing, permission/request match patterns, icons). Genuinely new API — our extension surface (Bitwarden host etc.) maps how is exactly WK21's stretch probe. |
| Scheme handlers (arktos-file/abp/arktos-resource) | works | `webkit_web_context_register_uri_scheme` + `WebKitURISchemeRequest` (uri/method/headers/body/web_view) → `finish` (GInputStream+length+mime) / `finish_with_response` (`WebKitURISchemeResponse`: status, content-type, arbitrary http headers — full response control, better than QWebEngineUrlSchemeHandler's body-only) / `finish_error`. |
| `Backend` rendering surface | works | **WK03 runtime-proven**: `WPEView::buffer-rendered` signal → `wpe_buffer_import_to_pixels` (borrowed buffer-owned GBytes — never unref, deep-copy inside handler) → ARGB8888-premul ≡ `QImage::Format_ARGB32_Premultiplied` (SHM stride honored) → QWidget paintEvent blit. Buffers arrive as WPEBufferDMABuf under `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1` and still import. Resize: QWidget::resizeEvent→`wpe_toplevel_resize`→next buffer carries new dims (measured 640×480 frame #2). WK20's dmabuf→GL-texture path is the perf upgrade; raster is the proven fallback. |
| Input | works | **WK04 runtime-proven**: `wpe_event_pointer_move/button_new` → `wpe_view_event` hit-tests and navigates (coords are doubles in view CSS-px space; buttons WPE_BUTTON_PRIMARY=1/MIDDLE=2/SECONDARY=3; press_count down-only; `wpe_view_focus_in` required post-map or input drops). Keyboard: `wpe_event_keyboard_new` — **keyval = XKB/X11 keysym space** (WPE_KEY_Return=0xff0d; printable ASCII in low range; Shift level is the embedder's job — shift+a wants XK_A), **keycode = X/XKB keycode = evdev+8** (WK13 measured: raw evdev maps 8 slots early — 'a'/30 reports KeyU); Qt::Key needs a mapping table — WK13's contract, done. Wheel: `wpe_event_scroll_new` — **WK13 runtime-proven**: non-precise deltas are wheel TICKS scaled by `pixelsPerLineStep(view dims)` (1 tick ≈ 71px@600h), precise deltas are px ×2.5 for mouse/touchpad sources (divide back out for pixel-faithful touchpads); sign is positive=scroll-UP like Qt (WPEWaylandSeat negates wl_pointer.axis), so Qt deltas pass through un-negated. |
| Sandboxing / multiprocess | works | Real WebProcess/NetworkProcess/GPUProcess split; bwrap sandbox runs — **embedder must `webkit_web_context_add_path_to_sandbox`** the ~/wpe prefix dirs (subprocess binary-patched PKGLIBEXECDIR lives outside standard binds; done in createViewStack) or `WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1` for dev. |

## The five gating questions

1. **Event-loop model — DECIDED.** `QPAEventDispatcherGlib` (Qt6
   default) pumps WebKit's global-default-context GSources natively on
   the GUI thread; WebKit objects live on the GUI thread with zero
   extra code. Fallback: QT_NO_GLIB + 60 Hz
   `g_main_context_iteration` drain. The private-context worker model
   works but is strictly worse (embedder-source attach trap + fatal
   exit-teardown trap — both measured, documented in the harness
   header). **Backend contract: GUI-thread WPE objects + glib
   dispatcher; never a worker GMainLoop.**

2. **Buffer path — DECIDED.** Observe `WPEView::buffer-rendered` →
   `wpe_buffer_import_to_pixels` (borrowed GBytes, deep-copy in
   handler) → `QImage::Format_ARGB32_Premultiplied` → QWidget
   paintEvent; resize via `wpe_toplevel_resize` (next produced buffer
   carries new dims). Headless requires
   `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1`; buffers still arrive as
   DMABuf and import fine. WK20 upgrades this to a zero-copy
   dmabuf→EGLImage→GL-texture path; raster remains the proven
   fallback.

3. **Packaging status (WK00) — landed, with sharp edges documented.**
   User-local `~/wpe` prefix from Debian sid .debs via `dpkg -x`
   (NO sudo); sid glibc 2.43 is interp/rpath-scoped only
   (`-Wl,--dynamic-linker` + DT_RPATH link recipe — glibc must NEVER
   enter LD_LIBRARY_PATH or host-interp tools pick a foreign libc);
   `compat/libwpe-host-compat.so` (g_sort_array forwarder) is
   LD_PRELOAD'd + `--no-as-needed`-linked; PKGLIBEXECDIR +
   injected-bundle strings were binary-patched in libWPEWebKit
   (pristine copy in `~/wpe/backup/`); the bwrap sandbox needs the
   prefix whitelisted via `add_path_to_sandbox`. All of this lives
   outside the repo; `.devin/webkit-env.sh` + `ACQUIRED.md` are the
   contract. Distribution story is a later problem (packaging task,
   not a spike gate).

4. **Interception story — layered, not a single hook.** (a)
   `decide-policy`: navigation veto + new-window veto + per-resource
   allow/block/download — and the RESPONSE decision carries
   WebKitURIResponse, so **we can observe response headers/status,
   something QtWebEngine AND Servo both deny us** (named SEC16C gap —
   closed here for reads, though still request-decision-shaped for
   writes). (b) Safari content-blocker JSON via
   `WebKitUserContentFilterStore`+`add_filter`: block/block-cookies/
   css-display-none/make-https/redirect — **WKB01's converter already
   emits this format**, so adblock+cosmetic land with zero engine
   patching; `redirect` only reaches bundled/extension resources,
   not arbitrary URLs. (c) `register_uri_scheme` handlers cover the
   custom schemes with full response control. (d) What does NOT
   exist: an arbitrary http(s) request rewriter/stub-server (the
   Servo `load_web_resource` equivalent) — REF01-style referer/UA
   rewrites must ride decide-policy's request object
   (`WebKitURIRequest` headers are NOT mutable there → honest gap:
   referer/UA/strip-param policies that need mid-flight rewrites
   degrade to content-blocker `make-https`+`redirect` subset or
   block; the injected-bundle `send-request` hook is the heavier
   escape hatch, unprobed). Verdict: **sufficient for the
   NavigationPolicy adapter with named gaps — BETTER than premised
   on adblock (native content-blocker), worse than Servo on
   arbitrary request rewriting.**

5. **Version floor — ≥ 2.44 asked, 2.54.1 pinned, findings apply to
   WPEPlatform ≥ 2.54 stable.** `WebKitNetworkSession` needs ≥2.40;
   `URISchemeResponse` ≥2.36; `WebKitWebExtension` ≥2.38;
   `buffer-rendered`/`wpe_buffer_import_to_pixels` + headless
   WPEDisplay are the 2.54 WPEPlatform API (trixie's 2.48 lacks
   wpe-platform entirely — the version floor is real, not
   conservative bookkeeping).

## Overall verdict: **viable — the strongest non-Chromium candidate yet**

Unlike Servo, WPE WebKit 2.54 clears every hard gate WK00 was worried
about: full load-signal set incl. real progress fractions, per-view
JS toggles, per-profile sessions with per-session proxy (Tor not
permanently locked — pending the WK21 DNS-leak audit), a real
download surface, cert-error interstitials, history/find/zoom/
context-menus/permissions/dialogs all present. The runtime-proven
spine (loop → create → render → input → eval) is already demonstrated
end-to-end in `wpe-harness`.

Named gaps in severity order:

1. **No arbitrary http(s) interception** — block yes, redirect/stub
   no (content-blocker `redirect` subset + scheme handlers only).
   Referer/UA/tracking-param rewrites needing mid-flight header
   edits have no hook outside the injected bundle (unprobed).
2. **No suspend/resume** — Frozen impossible; Discarded =
   session-state + destroy + restore round-trip.
3. **No renderer PID** — SBAR01 per-tab memory shows '—'.
4. **No download pause/resume** and no "visited links" clear type —
   honest no-ops in the adapter.
5. **No programmatic inspector channel** — built-in UI exists;
   BiDi panel stays disabled (same as Servo verdict, different
   reason).
6. **sideloaded-glibc link recipe** — every binary touching
   libWPEWebKit needs the dynamic-linker+RPATH+preload recipe;
   `webkit=0` builds must stay clean of it.
7. **Extensions** — `WebKitWebExtension` MV2/MV3 exists but is brand
   new; unproven against our Bitwarden-host requirements.

### `Capabilities` recommendations for WK07 (conservative — only what
WK01–WK04 proved or headers pin down)

| Capability | Value | Why |
|------------|-------|-----|
| requestInterception | **true** | decide-policy navigation veto + subresource block + content-blocker pipeline (WKB01 format); honest doc-note that redirect/stub is subset-only. |
| scriptInjection | **true** | UCM scripts + stylesheets + named worlds, per-session and per-view. |
| scriptEvaluation | **true** | evaluate_javascript JSCValue round-trip runtime-proven (WK04). |
| cookieFilter | **false** | only coarse 3-state accept policy; our per-attempt `CookieAttempt` functor is not expressible. |
| perPageSettings | **true** | WebKitSettings is per-view (enable-javascript et al). |
| lifecycleDiscard | **true** | Discarded via WebKitWebViewSessionState destroy/restore round-trip (Frozen unimplementable — document in adapter). |
| contextMenuInfo | **true** | context-menu + HitTestResult covers every field. |
| certificateOverride | **true** | load-failed-with-tls-errors + allow_tls_certificate_for_host. |
| devTools | **false** | toggle_inspector exists but no embedder channel API exported. |
| extensions | **false** | WebKitWebExtension API present but unprobed — honest until WK21. |
| downloads | **true** | download-started + WebKitDownload (pause/resume absent — adapter no-ops honestly). |

## Re-evaluation triggers

- A `suspend`/`resume` or background-throttle API landing upstream
  (unblocks SLEEP01 Frozen parity).
- The `WebKitWebInspector`/remote-inspector surface becoming public
  (unblocks the BiDi panel).
- `WebKitWebExtension` maturing past MV2/MV3 probe stage (unblocks
  extension parity — WK21's job to check first).
- Any upstream per-request header-rewrite hook on the UI-process API
  (would close the interception story's biggest gap without the
  injected bundle).
- A distro shipping wpe-webkit ≥2.54 on the host's glibc floor
  (retires the sideloaded-glibc recipe).

## Build/harness status

`spikes/webkit/probe` (webkit-probe.c + build-probe.sh — compiles+
runs under the WK00 recipe, prints `WPE WebKit 2.54.1`) +
`spikes/webkit/wpe-harness` (standalone qmake project: ViewStack
creation incl. sandbox whitelisting, three loop models, RasterWidget
render consumer, input/eval legs). Gates: `run-harness.sh` (all 3
loop models), `run-render.sh` (fixture shot + advisory live leg),
`run-input.sh` (pointer/keyboard/eval). All sandboxed, offscreen,
headless — zero display-server touch.

### Smoke results (sandboxed, `QT_QPA_PLATFORM=offscreen`, CPU raster)

| Case | Result |
|------|--------|
| loop=glib load→FINISHED | PASS — ~149–160 ms, QPAEventDispatcherGlib natively pumps WebKit sources |
| loop=timer (QT_NO_GLIB) | PASS — ~230 ms via 60 Hz drain |
| loop=thread (private ctx) | PASS — delivers, but two documented traps (exit teardown, source attach) |
| render fixture 800×600 | PASS — non-bg=480000/480000, 738 distinct colors, dominant #eef4ff (60.3%), witness-miss=none |
| resize→640×480 | PASS — next buffer arrives at new dims |
| live https://example.com | PASS (advisory leg) — real page shot, 243 colors |
| pointer click → navigate | PASS — uri property reaches fixture2.html |
| Return key → activate link | PASS — navigates home |
| eval typed round-trip | PASS — JSCValue number(42) + string |

Evidence PNGs committed under `spikes/webkit/wpe-harness/wk03-*.png`.

### Embedder gotchas found en route (contract notes for WK07+)

- **`wpe_view_focus_in` once post-map or ALL input drops** — no
  error, input just evaporates.
- `wpe_buffer_import_to_pixels` returns a **borrowed** GBytes —
  never unref; copy inside the `buffer-rendered` handler only
  (rendered/released bookkeeping is WebKit's).
- `press_count` belongs on the pointer-button DOWN event only — the
  ctor asserts otherwise.
- Qt::Key does NOT coincide with XKB keysyms — WK13 needs a
  Qt::Key→WPE_KEY_* table (or `xkb_keysym_from_name`); keycode is
  the X/XKB keycode (evdev+8 — WK13 measured DOM code off by 8 on
  raw evdev) feeding DOM `.code`.
- `WebKitWebView` owns its WPEView in 2.54 — construct via the
  `"display"` construct property, then `wpe_view_set_toplevel` +
  `wpe_toplevel_resize` + `wpe_view_set_visible` + `wpe_view_map`.
- Sandbox whitelisting is per-WebContext and mandatory under this
  prefix (`add_path_to_sandbox` ×4 — see createViewStack).
- Binaries linking libWPEWebKit need the FULL link recipe
  (dynamic-linker + `--disable-new-dtags` RPATH + `--no-as-needed`
  compat + explicit sid libc/libm objects) — see
  `webkit-env.sh`/`build-probe.sh`/`wpe-harness.pro`.
