# ENG03 — Servo embeddability spike (verdict doc)

Date: 2026-10-10 · libservo pinned at git tag `v0.7.0` (`aac43a3f3`,
blobless clone at /tmp/servo-src — API surveyed against source, not docs)
· spike artifacts: `spikes/servo/servo-embed/` (cdylib C ABI) +
`spikes/servo/harness/` (Qt widget harness, standalone qmake project —
not in the top-level SUBDIRS, so `make check` is unaffected by design).

Method note: v0.7.0's real API is RICHER than the task premise assumed —
there is a full request-interception delegate hook, a user-content
(script/stylesheet) manager, a site-data manager, and a protocol
registry. Verdicts below are per ENG01 interface member
(`.devin/ENGINE.md` §"Engine-neutral interface surface"), graded
`works` / `partial` / `absent` / `blocked` (blocked = gating gap).

## Per-member verdict

| Interface member | Verdict | Evidence / bound |
|------------------|---------|------------------|
| `Backend::initialize` + event loop | works | `ServoBuilder` + `EventLoopWaker` (one `wake()` callback the embedder marshals to its GUI thread → `Servo::spin_event_loop()`). Single-process mode (`opts.multiprocess=false`) needs no re-exec. Spike crate implements this. |
| `Page::load/stop/reload/url` | works | `WebView::load(Url)`/`load_request(UrlRequest)`/`reload()`/`url()`. `stop` not found on the WebView surface — cancel via interceptor instead (partial, see below). |
| `Page` signals (title/url/load) | partial | `notify_url_changed`, `notify_page_title_changed`, `notify_status_text_changed`, `notify_load_status_changed` (Started/HeadParsed/Complete — **no progress fraction**, so a percent bar is impossible; spinner/indeterminate only), `notify_favicon_changed`, `notify_crashed`. |
| `Page` history nav | works | `go_back/go_forward/can_go_back/can_go_forward`, `TraversalId`, `notify_history_changed(Vec<Url>, current)` — richer than QtWebEngine's (full entry list delivered). `clear_session_history()`. |
| `Page` zoom | works | `set_page_zoom/page_zoom`, `adjust_pinch_zoom/pinch_zoom`, `set_hidpi_scale_factor`. |
| `Page::runJavaScript` | partial | `evaluate_javascript(script, FnOnce(Result<JSValue, JavaScriptEvaluationError>))` exists — **but JSValue has no public marshaller** (no Display/serialization on the embedder surface). Scripts fire; typed return values are effectively fire-and-forget. Autofill-style two-way channel needs eval-and-observe-side-effects or a patched libservo. |
| User content injection (autofill/fingerprint scripts) | works | `UserContentManager` (`add_script(UserScript)`, `add_stylesheet(UserStyleSheet)`) on `WebViewBuilder`, shareable across webviews — direct analog of `QWebEngineScriptCollection`. |
| `Page` per-page settings (JS on/off, images) | absent | `Preferences` is whole-instance (ServoBuilder) — `WebViewBuilder` takes delegate/url/hidpi/content+clipboard+gamepad delegates only. No per-webview `JavascriptEnabled` → JSCTL/SECLVL per-tab toggles have NO equivalent. Mitigations: interceptor-side script blocking (resource-type decisions exist), or fork-patched per-pipeline pref. |
| `Page` `findText` | absent | No find-in-page API on `WebView` (v0.7.0). Would need JS-eval find (runJavaScript exists) or fork patch. |
| `Page` `setLifecycleState` (SLEEP01 discard/freeze) | absent | No suspend/discard/throttle API on `WebView`. Sleeping tabs not portable to a Servo backend today. |
| `Page::renderProcessId` (SBAR01) | absent | `opts.multiprocess` exists but no renderer-PID surface on `WebView`. Interface already defines `-1 = unsupported` — honest. |
| `Page::createWindow` (popups) | works | `request_create_new(parent_webview, CreateNewWebViewRequest)` delegate — popup decision hook exists. |
| `RequestPolicy` — navigation gate | works | `request_navigation(webview, NavigationRequest)` with `allow()`/`deny()` — main-frame scheme/policy gating covered (SEC02/SAFE01 analog). `request_protocol_handler` for external-scheme prompts (SAFE05 analog). |
| `RequestPolicy` — request interception | partial | **`load_web_resource` is a real full interceptor**: fires for every fetch-pipeline request, carries `WebResourceRequest{method,url,headers,destination,referrer_url,is_for_main_frame,is_redirect}`, and `intercept(WebResourceResponse)` yields an `InterceptedWebResourceLoad` (`send_body_data`/`finish`/`cancel`) — embedder can serve synthetic responses → adblock stubs, redirects, blocks all expressible. **Gaps:** (a) WebSocket handshakes bypass it — `net/websocket_loader.rs` composes its own HTTP upgrade outside the fetch interceptor (same hole QtWebEngine has, but at least parity); (b) request-side only — cannot OBSERVE the real response headers/status, only veto or replace (same named gap as SEC16C); (c) fires on the net thread with an unbounded_channel — verdicts must be fast, no GUI-thread round-trips without care (same discipline as our IO-thread snapshot). |
| `Profile` model (containers, OTR, per-profile state) | partial | No profile object: one `Opts::config_dir` per `Servo` instance. `Opts::temporary_storage` = OTR equivalent (no disk persistence). Multiple `Servo` instances per process are not documented/tested — per-tab container profiles (CONT01-05) likely mean one Servo instance per container, heavy but conceivable; needs a follow-up probe. `SiteDataManager` gives `cookies_for_url/set_cookie_for_url/clear_cookies/clear_session_cookies/clear_site_data(site, StorageType)` — cookie/storage policy surface exists. |
| Cookie filter (third-party gate) | partial | `SiteDataManager` is imperative (set/get/clear), not a per-request filter callback. A third-party-cookie gate would ride the request interceptor (deny the request) — blocking, not cookie-only stripping. Named gap for ENG02's `setCookieFilter` semantic parity. |
| `DownloadRequest` | absent | No download delegate anywhere in `components/servo/`. The only path: intercept the document-level `load_web_resource`, detect non-renderable responses (embedder can't see Content-Disposition — response-side!), and hand the URL to our own downloader (rustdl). Works but loses engine-side download UX; also means the engine can't decide "download vs render" for us. |
| `CertificateErrorInfo` | absent | No cert-error delegate: `NetToEmbedderMsg` = SelectFiles / WebResourceRequested / RequestAuthentication / cookie-op responses only. `Opts::ignore_certificate_errors` is a global kill-switch; the internal `CertificateErrorOverrideManager` (connector.rs) has no embedder notification path. SEC06-style interstitial is NOT drivable — a cert failure surfaces as a generic load failure with no chain detail. **Gating gap for security parity.** |
| Permissions (SEC05 broker) | works | `request_permission(webview, PermissionRequest{feature, allow/deny})` delegate exists. |
| `ContextMenuInfo` | works | `show_embedder_control(EmbedderControl::{ContextMenu{element_info, items}, SelectElement, ColorPicker, FilePicker, InputMethod, SimpleDialog(Alert/Confirm/Prompt)})` — embedder-owned chrome like today. |
| Authentication | works | `request_authentication(url, for_proxy, …)` — incl. proxy auth. |
| Proxy / SOCKS5 | **blocked** | Prefs expose `network_http_proxy_uri` / `network_https_proxy_uri` / `network_http_no_proxy` (hyper-util matcher syntax) + env `http_proxy`/`HTTPS_PROXY` — **HTTP CONNECT proxies only, zero SOCKS support**. Tor windows can never run on Servo → ENG05's Chromium-lock for Tor is permanent, not "unproven". |
| Devtools / BiDi | partial | `devtools_server_enabled` + `devtools_server_listen_address` prefs exist — a devtools server (CDP-flavored, TCP socket). DEVT02's pipe-vs-port constraint applies equally here; WebDriver BiDi status on v0.7.0 not verified — servo has a webdriver component but its BiDi coverage is partial upstream. |
| Extensions | absent | No WebExtension surface at all. |
| Scheme handlers (arora-file/abp/arora-resource) | works | `ProtocolRegistry` on `ServoBuilder` (`protocol_handler` module — `ProtocolHandler` trait + `ProtocolRegistry::merge` + `with_internal_protocols`). Custom schemes fully supported. |
| Notifications / media session / fullscreen | works | `show_notification`, `notify_media_session_event` + `notify_media_session_action_event`, `notify_fullscreen_state_changed`/`exit_fullscreen`. |
| Accessibility | works | `notify_accessibility_tree_update`, `set_accessibility_active`, accesskit tree ids (better than QtWebEngine's AT-SPI surface). |
| `Backend` rendering surface | works | `RenderingContext` trait — `SoftwareRenderingContext::new(PhysicalSize)` (used by the spike; headless/no-GL path exists) + `OffscreenRenderingContext` for GL. `webview.paint()` + `read_to_image(rect)` → `RgbaImage` — the spike's whole display path. |

## The five gating questions

1. **Request interception** — BETTER than premised: `load_web_resource`
   is a full intercept-or-replace hook, not just observation. Option
   ranking resolved: no fork needed for allow/block/redirect/serve-stub
   on fetch traffic. Residual holes: WebSocket upgrades bypass it;
   response headers unobservable (veto/replace only); verdict must be
   net-thread-cheap. Verdict: **sufficient for ENG02's NavigationPolicy
   adapter with documented gaps.**

2. **SOCKS5 proxy** — ABSENT. `network_http(s)_proxy_uri` is
   http-proxy-only. Tor stays Chromium-locked permanently. Verdict:
   **confirmed gap (was predicted).**

3. **Cookie/storage persistence** — `config_dir` persists cookies,
   HSTS, HTTP auth (JSON). `temporary_storage` = OTR. SiteDataManager
   covers clear+query. Per-profile model absent → containers need one
   Servo instance per profile (unproven multi-instance). Verdict:
   **works for single-profile + OTR; containers need a probe.**

4. **Download callbacks** — absent; embedder-owned via interception.
   Verdict: **workable, degraded.**

5. **JS evaluation** — fires, but `JSValue` doesn't marshal results to
   the embedder. One-way injection (autofill/fingerprint) is fine;
   two-way needs side-channel (console/URL scheme round-trip) or a
   fork. Verdict: **partial.**

## Overall verdict: **viable-in-parts — 'partial backend', not yet a swap candidate**

Servo v0.7.0 can host real tabs in the Qt shell (event loop, input,
paint, navigation, interception, user scripts, protocol schemes,
permission/notification/auth delegates all exist). What blocks a
drop-in backend today, in severity order:

1. **No SOCKS5** — Tor windows locked to Chromium permanently.
2. **No cert-error surface** — SEC06-grade TLS UX impossible; a cert
   failure is indistinguishable from a network failure.
3. **No per-webview preferences** — JSCTL/SECLVL per-tab JS control
   loses its mechanism (interceptor can block script *resources*, but
   can't disable inline `<script>`/handlers — same bound our own
   SAFER tier already documents for http pages).
4. **No find-in-page, no lifecycle discard, no renderer PID** —
   MIG12/SLEEP01/SBAR01 features simply don't exist on the backend.
5. **One-way JS only** — autofill/reader/fingerprint scripts that
   report back need the console/scheme side-channel or a fork.
6. **No download surface** — embedder owns it via interception
   (acceptable: rustdl already exists).
7. **Container/per-profile state** — no profile objects; multi-instance
   per container unproven.

`Engine::Capabilities` (ENG01) was designed for exactly this: a Servo
backend would register {interception: yes, injection: yes, cookie
filter: partial, per-page settings: no, lifecycle: no, cert override:
no, downloads: no, devtools: partial, extensions: no} and degrade
honestly.

## Re-evaluation trigger

Watch for these upstream (servo/servo, ~monthly releases):
- SOCKS5 support landing in `network_*_proxy_uri` prefs or net prefs.
- A certificate-error delegate/EmbedderMsg (the internal
  `CertificateErrorOverrideManager` is 90% of the machinery already —
  it only needs a notification path upstream).
- Per-webview `Preferences` on `WebViewBuilder`.
- `JSValue` → string/DOM marshaling on `evaluate_javascript`.
- A `find_text`/search API on `WebView`.
- Multi-`Servo`-instance support documented (for containers).
Next sensible re-probe: one release after ANY of {SOCKS5, cert-error
delegate, per-webview prefs} lands — those are the three hard gates.

## Build/harness status

`spikes/servo/servo-embed` (cdylib over libservo v0.7.0,
`default-features=false` + `bundled`/`js_jit`; ~700 crates incl.
mozjs/webrender, ~20min cold build on this box, ~6s incremental;
debug .so ~1.15 GB — link via lld, GNU ld is too memory-hungry) +
`spikes/servo/harness` (Qt offscreen harness: wake→queued spin,
painted-frame grab, input forwarding, request-log dump).

### Smoke results (offscreen, SoftwareRenderingContext)

| Case | Result |
|------|--------|
| `about:blank` | PASS — LoadStatus::Complete, 800x600 frame grabbed |
| `http://127.0.0.1:18089/page.html` | PASS — url + title ("Servo Spike Fixture") delegates fired, frame painted; **all 3 fetches observed through `load_web_resource`** (document + png + js) |
| `file:///tmp/servo-fixture/page.html` | PASS — same, 3 requests observed |
| `data:text/html,...` | PASS — title "dataurl", fetch observed |

So the harness itself is the smoke test: **page renders, input path
exists, navigation + interception + JS + delegates all work** on
v0.7.0. `servospike --smoke <secs> <url>` is the `--servo-smoke`
stand-in (detached — the debug .so alone maps >1 GB).

### Embedder gotcha found en route (matters for ENG05)

`WebView::load()` before the constellation finishes registering the
webview's browsing context is **silently dropped** ("LoadUrl for
unknown browsing context", warn-level only). The first `se_load` in
the harness raced registration and never navigated. `WebView::url()`
returns `None` until the first history entry lands, so the shim
queues early loads into `pending_load` and flushes them from
`se_spin` once registration reports. Passing the initial URL through
`WebViewBuilder::url()` rides inside `NewWebView` and can't race —
do that for any per-tab construction in ENG05.

Second gotcha: an `if let` scrutinee's `borrow_mut()` temporary lives
through the whole block — `RefCell already borrowed` panic on the
re-borrow. Hoist `take()` into a `let`.
