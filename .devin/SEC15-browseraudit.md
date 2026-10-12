# SEC15 — browseraudit.com full baseline (Qt 6.12.0 / Chromium 140.0.7339.225)

Baseline measurement of the complete browseraudit.com suite (431 tests,
42 leaf categories) against the real Arora build, with a bare-engine
control run for attribution.

## Method

`./arora --browseraudit-smoke` (new in `src/main.cpp`) loads
`https://browseraudit.com/test?categories=*&sendresults=false` on the
real application profile (named `arora` profile, CookieJar,
PrivacyRequestInterceptor + adblock, extensions, userscripts, WebView
with its `loadFinished` handlers). A `QWebEngineScript` injected at
DocumentCreation wraps `browserAuditTestFramework.start()` and records
every test's id/title/behaviour/outcome/duration/category path; on
suite end the JSON is written to `$ARORA_AUDIT_OUT`
(default `/tmp/browseraudit-app.json`).

`--browseraudit-bare` does the same on a throwaway off-the-record
profile + plain `QWebEngineView` — no CookieJar, interceptor, WebPage
navigation policy or WebView handlers. Divergence between the two
profiles attributes failures to app wiring. `ARORA_AUDIT_WIRE=<csv>`
(settings,cookies,interceptor,extensions,webpage,webview,autofill,
cosmetic,bar,contained,nofinish) re-applies individual pieces of the
app wiring onto the bare profile for bisection.

Runs used `QT_QPA_PLATFORM=offscreen` + `LIBGL_ALWAYS_SOFTWARE=1`
(the host iGPU intermittently traps `Chrome_InProcGp` — environmental,
reproduces on unmodified HEAD).

## Results

| profile | pass | warning | critical | skip | total |
|---------|------|---------|----------|------|-------|
| app (SEC16B re-run) | **397** | 34 | 0 | 0 | 431 |
| app (post-fix, final capture) | **383** | 48 | 0 | 0 | 431 |
| app (pre-fix capture) | 372 | 50 | 0 | 9 | 431 |
| bare engine (SEC16B re-run) | 399 | 32 | 0 | 0 | 431 |

The bare engine itself warns on 32 tests — the suite was written ~2015
against CSP2/early-fetch-spec semantics, so shared warnings are
spec drift, environment, or engine behavior, not app bugs.

## Per-category results (app profile, post-fix)

| Cat | Name | pass | warn | crit | skip | non-pass test IDs |
|-----|------|------|------|------|------|-------------------|
| 2 | DOM access | 24 | 0 | 0 | 0 | |
| 3-6 | DOM access (parent/child × origins) | 24 | 0 | 0 | 0 | |
| 7 | XMLHttpRequest | 28 | 0 | 0 | 0 | |
| 8-10 | Cookies (domain/illegal/path scope) | 19 | 0 | 0 | 0 | |
| 12 | stylesheets | 22 | 0 | 0 | 0 | |
| 13 | scripts | 18 | 0 | 0 | 0 | |
| 14 | 'unsafe-inline' | 6 | 2 | 0 | 0 | 229, 231 |
| 15 | 'unsafe-eval' | 8 | 0 | 0 | 0 | |
| 17 | images | 10 | 0 | 0 | 0 | |
| 18 | media | 20 | 0 | 0 | 0 | |
| 19 | frames | 30 | 0 | 0 | 0 | |
| 21-23 | CORS Allow-Origin/Methods/Headers | 34 | 0 | 0 | 0 | |
| 24 | Access-Control-Expose-Headers | 22 | 4 | 0 | 0 | 140, 149, 152, 153 |
| 26-27 | HttpOnly / Secure flags | 7 | 0 | 0 | 0 | |
| 29 | Referer | 1 | 0 | 0 | 0 | |
| 31 | X-Frame-Options | 8 | 0 | 0 | 0 | (was 9 skips pre-fix) |
| 32 | Strict-Transport-Security | 2 | 3 | 0 | 0 | 180, 182, 183 |
| 33 | fonts | 24 | 16 | 0 | 0 | 311-348 (see below) |
| 34 | report-uri | 2 | 1 | 0 | 0 | 464 |
| 35 | connect-src | 30 | 2 | 0 | 0 | 361, 364 |
| 36 | sandbox | 16 | 4 | 0 | 0 | 395, 397, 399, 401 |
| 38 | worker-src | 6 | 4 | 0 | 0 | 430, 432, 435, 437 |
| 39 | manifest-src | 6 | 4 | 0 | 0 | 440, 442, 444, 447 |
| 40 | Referrer-Policy | 6 | 3 | 0 | 0 | 450, 451, 455 |
| 41 | form-action | 3 | 2 | 0 | 0 | 459, 461 |
| 42 | frame-ancestors | 3 | 3 | 0 | 0 | 466, 467, 468 |
| 43 | Access-Control-Allow-Credentials | 4 | 0 | 0 | 0 | |

Zero `critical` results anywhere — nothing the suite considers an
actual security failure fired on either profile.

## Classification of every non-pass item

### APP-SIDE — found and FIXED in this task

- **171, 175, 176, 412, 416, 417, 465, 469, 470 (9 skips, X-Frame-Options
  + frame-ancestors "timed out")** — `WebPage::init` disabled
  `QWebEngineSettings::ErrorPageEnabled` (a MIG02 choice). With error
  pages off, failed SUBFRAME loads produce no error commit and no
  `load` event for the frame; browseraudit gates its result check on
  `iframe.load` (including failed loads), so every test whose frame was
  *supposed* to be refused (XFO DENY, frame-ancestors mismatch) timed
  out at 10s. This also affects real pages that gate on iframe load
  events — a genuine bug. Re-enabled error pages: the main-frame
  `LoadFailed` signal still fires so `handleLoadingChanged` still
  substitutes `notfound.html`; subframe errors now commit Chromium's
  error page and fire `load` normally. Verified end-to-end on the live
  suite: **0 skips** in the post-fix capture. `tst_csp` 15/15,
  `tst_certerror` 9/9, `tst_webpage` 40/40, `tst_webview` 11/11.

### APP-SIDE — remaining, test-timing latency (no security impact)

- **229, 231 ('unsafe-inline') + 311-348 ×16 (fonts)** — all are
  `behaviour:"allow"` tests where a paint-gated fetch (webfont or
  `background:` image in the iframe) must reach the server within the
  suite's 300 ms result-check window. Verified on a local probe server:
  the font fetch **does** land in app mode (~20 ms after the other
  requests); it is delayed, never blocked. Bisection (`ARORA_AUDIT_WIRE`,
  sequential runs): a plain `QWebEngineView` + `WebPage` + interceptor +
  cookies + settings + both `loadFinished` callees → 40/40 pass;
  `wire=webview` reproduces all 16 warnings; disconnecting
  `QWebEngineView::loadFinished` on the WebView clears them. The work is
  WebView's per-load JS (`AutoFillManager::attachToPage` qwebchannel.js +
  autofill.js injection, adblock cosmetic pass) running inside the same
  turn in which Chromium schedules the paint-gated fetch — it lands
  after the suite's deadline on a real WAN. The individual callees do
  not reproduce on a plain view, so the trigger is the WebView-context
  `loadFinished` path, not one callee. Adblock logged zero blocks
  for these runs (ARORA_DEBUG_BLOCK instrumentation); cookie rotation,
  profile storage, GPU sandboxing all ruled out by the bisect matrix.

  **RESOLVED by SEC16A:** the per-load JS moved off `loadFinished`
  entirely — `WebPage::schedulePageScripts` now arms the autofill
  bundle and the adblock cosmetic pass as per-page `QWebEngineScript`s
  at `DocumentReady` (armed on `urlChanged` while a document load is
  pending), so the injected work runs at DOMContentLoaded — before the
  `load` event whose turn schedules the paint-gated fetches.
  `WebView::loadFinished` retains only GUI-thread bookkeeping
  (progress reset, icon/badge refresh, history entry). Re-run on the
  affected categories (14 + 33, 48 tests): app profile 48/48 pass,
  bare control 48/48 — all 18 latency warnings gone, app now
  indistinguishable from the bare engine on these categories
  (`sec15/browseraudit-sec16a-app.json`,
  `sec15/browseraudit-sec16a-bare.json`).

### TEST-STALE — the suite predates the spec / its own site config

- **361, 364 (wss under `'self'`)** — CSP3's `'self'` matching covers
  the `wss:` variant of an https origin (Chromium ≥ 70,
  crbug.com/815142); the 2015 suite expects a block. SEC14 verified the
  allow is spec-correct end-to-end locally.
- **149, 152, 153 (Content-Length exposed without ACEH)** — Fetch made
  `Content-Length` a CORS-safelisted response header (post-dates the
  suite); exposure is spec-correct.
- **140 (Content-Type not exposed)** — the `/cors/exposed-headers`
  endpoint serves a 0-byte body with **no Content-Type header at all**
  (verified via curl), so `getResponseHeader` correctly returns null.
  Server-config artifact, not a browser miss.
- **180, 182, 183 (HSTS)** — every `http://*.browseraudit.com` URL now
  301s to https at the site's own nginx edge AND Chromium auto-upgrades
  mixed-content images on the https suite page (mixed-content
  auto-upgrade). No request can be observed over plain http, so the
  protocol check always reads "https". Unmeasurable test environment.
- **391, 393 (sandbox + allow-same-origin cookie access)** — SEC14's
  local suite proved spec-correct behavior (access allowed under
  allow-same-origin, denied opaque); flaky on the live site — both
  passed in the final app capture.
- **395, 397, 399, 401 (sandbox allow-scripts / allow-forms)** —
  local probe: the sandboxed iframe's script fetch and form navigation
  both reach the server; the suite checks results at the outer iframe's
  `load` (+300 ms for some) and the doubly-nested iframe lands later on
  a WAN. Timing artifact — mechanism verified working.
- **430-437 (worker-src)** — `new Worker`/`SharedWorker` script fetch
  verified landing locally; worker startup + WAN latency exceeds the
  300 ms window. Timing.
- **459, 461 (form-action)** — in-iframe form submission verified
  landing locally; outer-`load` check races it. Timing.
- **466-468 (frame-ancestors allows)** — nested allowed framing
  verified working; same check-window race.
- **450, 451, 455 (Referrer-Policy)** — local probe: `<img
  referrerpolicy>` honored exactly per spec (no-referrer → header
  stripped; origin → `https://host/`; unsafe-url → full URL). The
  suite's `/set_referer` + `/get_referer_policy` endpoints are
  session-global mutable state — the observed mismatch is a
  store/correlation artifact of the 2015 harness, not engine behavior.

### ENGINE-SIDE — real Chromium/QtWebEngine behavior, named gap

- **440-447 (manifest-src)** — QtWebEngine never issues the
  `<link rel="manifest">` fetch at all (local probe: zero requests,
  bare and app profiles). Upstream Chromium fetches manifests via the
  installability/PWA pipeline, which Qt WebEngine does not run.
  `manifest-src` enforcement is moot when manifests never load.
  Missing API: no `QWebEngine` hook to trigger or observe manifest
  fetches. Not a security regression — an absent feature.
- **464 (report-to)** — Chromium delivers CSP reports asynchronously
  through the Reporting API (batched/deferred); nothing can arrive
  inside the suite's 300 ms window. In-app these requests are
  additionally blocked by design (PING01 blocks
  `ResourceTypeCspReport`); the bare-profile warning shows the engine
  behavior independent of that policy.

## SEC16B — app-side fix pass (fresh full-suite captures)

Re-ran the complete suite on current HEAD
(`sec15/browseraudit-sec16b-app.json`,
`sec15/browseraudit-sec16b-bare.json`):

- **app 397 pass / 34 warning / 0 critical / 0 skip**; bare 399 / 32 /
  0 / 0. All 18 SEC16A latency warnings (229, 231, 311–348) stay
  cleared on the full suite. Zero skips.
- The only app-vs-bare outcome divergences are **405, 454, 456, 458**
  (app-only warns) and **451, 461** (bare-only warns — flip-flops).

### Per-item resolution of the app-side divergences

- **405 (report-uri) — APP-SIDE, by design.** Behaviour `block`, warn =
  "report not received". Local probe (loopback HTTP fixture
  `sec15/csp-report-probe.py` serving
  `Content-Security-Policy: default-src 'none'; report-uri /r`):
  with `privacy/blockPings=false` two `application/csp-report` POSTs
  arrive; with the shipping default they never leave the browser —
  PING01's `ResourceTypeCspReport` block drops the report-uri upload
  deterministically. The SEC15 app capture passed this test, so the
  block was presumably not active in that run's profile; the mechanism
  is now verified deterministic. This is the privacy feature working —
  **resolved as intentional divergence**, not fixed. (Whether the
  newer Reporting-API `report-to` path also reaches the interceptor is
  PING02's residual question.)
- **454, 456, 458 (+450/451/455 shared) — Referrer-Policy — NOT
  app-caused.** Isolated re-run of category 40 twice on the app
  profile, once with `privacy/refererPolicy=0` (engine default) and
  once with the shipping Trimmed default (`sec15/referer-probe.py`
  captured the on-wire Referer for the interceptor-visibility check):
  **identical warn sets**
  (454/455/456/458 warn both ways). REF01's rewrite never reaches
  these requests — the tests observe redirect follow-up legs, whose
  Referer Chromium recomputes after the interceptor ran (already noted
  in the interceptor header). The warn subset shifts run to run on
  both profiles — the suite's session-global `/set_referer` /
  `/get_referer_policy` endpoints are shared mutable state. Reclassify
  as harness artifact (was listed under test-stale; now verified
  policy-independent).

### SEC16 interceptor-lever evaluation — none reach a real item

- **ws: blocking (wss-only).** The only websocket warnings (361/364)
  are wss-under-`'self'` spec drift — the suite wants a spec-correct
  CSP3 allow blocked; satisfying it means violating CSP3. ws: on an
  https: page is already mixed-content-blocked by the engine; ws: on
  an http: page is legitimate (SAFE07's downgrade/exception machinery
  exists precisely for http-only sites). Third-party ws: is already
  covered opt-in (XSLEAK03 `blockThirdPartyWebSockets`). A blanket ws:
  block reaches zero flagged items and would break real sites — **not
  applied**.
- **https-only subrequest enforcement.** Engine already enforces this:
  Chromium's mixed-content auto-upgrade is the documented reason the
  HSTS probes (180/182/183) can observe no plain-http request.
  Interceptor-upgrading http: subresources on SAFE07-excepted http
  hosts would break exactly the sites the exception exists for.
  **Not applied** — no flagged item is reachable through it.
- **Additive CSP injection.** No remaining warning is an app-owned
  "policy not enforced". Injection cannot conjure the manifest fetches
  (440–447 — the engine never issues the request) and cannot fix
  report scheduling (464 — in-app already blocked). **Not applied** —
  nothing to inject for.
- **Cookie-store filtering for sandbox cases.** SEC14 verified the
  sandbox cookie behavior spec-correct locally; 391/393 flip-flop
  between runs on *both* profiles (timing), and
  `QWebEngineCookieStore::FilterRequest` exposes no sandbox/opaque-
  origin flag to attribute a frame context anyway — that observation
  gap is engine-side material for SEC16C, not an app fix. **Not
  applied**.

### App-side column: final state

**Zero defect items.** The remaining app warnings decompose as: shared
test-stale/engine warnings identical to the bare control, one
intentional privacy divergence (405 — PING01), and the referrer
harness flake (454/456/458 this run — verified policy-independent).
Engine-side remainder (440–447 manifest-src, 464 report-to) carries
over to SEC16C for the named-missing-API write-ups.

## Notes for SEC16 (fix pass)

- The only app-side non-pass group is the loadFinished-latency one
  (18 tests). Nothing else in the app profile diverges from the bare
  engine — the interceptor, CookieJar, scheme handlers, permission
  broker, JSCTL tiers and extension layer neither weaken nor break any
  audited behavior.
- Engine-side items are QtWebEngine-level (manifest fetch) or Chromium
  report scheduling — app-layer mitigations per the SEC16 lever list
  don't apply (manifest fetch has no request to intercept; report-to
  is already privacy-blocked in app mode).
- Raw captures: `/tmp/browseraudit-app-final.json`,
  `/tmp/browseraudit-bare.json`; bisection JSONs `/tmp/audit-f33-*`,
  `/tmp/audit-c14-*`, `/tmp/audit-c3142-app.json`.

## SEC16C — engine-side named-missing-API register

Every warning that remains ENGINE-SIDE after SEC16B's mitigation pass,
with the specific embedder-API gap, the bundled-engine version
(Qt 6.12.0 / Chromium 140.0.7339.225) and an upstream reference.
App-layer reachability was evaluated in SEC16B — these are the items
the QtWebEngine API surface genuinely cannot observe or drive.

### 440–447 (manifest-src) — engine never issues the manifest fetch

- **Missing API.** QtWebEngine exposes no manifest machinery at all:
  no `QWebEngine*` class or signal reports a page's declared manifest,
  there is no request type for it on `QWebEngineUrlRequestInfo`, and
  no embedder hook exists to trigger the fetch. In upstream Chromium
  the `<link rel="manifest">` fetch is driven browser-side by
  `content::ManifestManagerHost` (implements
  `blink::mojom::ManifestManager`, see
  `content/browser/manifest/manifest_manager_host.cc`) as part of the
  installability/PWA pipeline — the Qt port does not wire this path,
  so the request is never created and `manifest-src` has nothing to
  enforce against. Verified locally: zero manifest requests reach the
  server on either profile (bare + app).
- **Disposition.** Engine-side, absent feature (not a security
  regression — an unfetched manifest cannot be abused). Re-check on
  Qt upgrade: if a future QtWebEngine version surfaces manifest
  loading, the interceptor automatically sees it as a normal request
  and CSP enforcement falls out of Chromium itself.
- **Upstream reference.** `content/browser/manifest/manifest_manager_host.cc`,
  `third_party/blink/public/mojom/manifest/manifest_manager.mojom`
  (Chromium 140 tree); the fetch initiator lives in the
  `chrome/browser/web_applications` installability pipeline, which is
  outside the `//content` layer QtWebEngine embeds.

### 464 (report-to) — Reporting-API delivery is engine-scheduled

- **Missing API.** CSP `report-to` / `Reporting-Endpoints` reports are
  delivered by Chromium's `network::ReportingService`
  (`net/reporting/README.md` in the Chromium tree): reports are
  cached and sent out-of-band by `ReportingDeliveryAgent` with
  batching + backoff — Chrome's own documentation states delivery can
  be delayed "up to a minute" and that there is no way to control the
  timing. Nothing can arrive inside the suite's 300 ms observation
  window, and no QtWebEngine API exists to flush or observe queued
  reports. A Chromium debug switch (`--short-reporting-delay`)
  shortens the delivery interval, but changing it would not alter the
  app outcome — PING01 blocks `ResourceTypeCspReport` uploads by
  privacy policy anyway, so the warning is intentional in-app.
- **Disposition.** Engine-side (scheduling) + app-intentional
  (blocked delivery). Whether Reporting-API deliveries reach the
  interceptor under a different resource type than
  `ResourceTypeCspReport` is PING02's residual question — tracked
  there, not here.
- **Upstream reference.** `net/reporting/README.md` +
  `net/reporting/reporting_delivery_agent.cc` (Chromium 140 tree);
  developer.chrome.com Reporting-API docs ("the browser controls when
  they're sent … up to a minute").

### 391/393 context — no sandbox/opaque-origin attribution on the request surface

- **Missing API.** `QWebEngineCookieStore::FilterRequest` exposes
  exactly `firstPartyUrl`, `origin` and `thirdParty`
  (doc.qt.io/qt-6/qwebenginecookiestore-filterrequest.html), and
  `QWebEngineUrlRequestInfo` exposes URL / resource type /
  navigation type / first-party URL / headers — neither carries the
  requesting frame's sandbox flags or an opaque-origin indicator, so
  the SEC16 cookie-filter lever cannot distinguish a sandboxed iframe's
  cookie access from a normal subresource one. The attribution data
  exists engine-side (`network::ResourceRequest` trusted params /
  render-frame context) but is not plumbed to the embedder.
- **Disposition.** Engine-side observation gap. SEC14 already proved
  the underlying behavior spec-correct end-to-end (cookie access
  allowed under `allow-same-origin`, denied for opaque origins); the
  391/393 flip-flops are the suite's outer-`load` timing race, so
  there is additionally no defect to mitigate — the gap only limits
  *optional stricter* filtering.
- **Upstream reference.** Qt 6.12 API surface (FilterRequest /
  UrlRequestInfo docs, above); internally Chromium tracks this via
  `network::mojom::CookieAccessObserver`-adjacent frame context, not
  exposed to `QWebEngineUrlRequestInterceptor` implementations.

### 454/456/458 (+450/451/455 shared) — redirect-leg Referer recompute

- **Missing API.** `QWebEngineUrlRequestInfo::setHttpHeader` can
  rewrite a Referer the request already carries at intercept time, but
  on redirect hops Chromium recomputes the referrer for the follow-up
  request inside the network stack
  (`services/network/redirect_util.cc` — `RedirectUtil::UpdateHttpRequest`
  re-derives `referer`/`referer_policy` from the redirect response),
  and the embedder API exposes no per-request referrer-policy override
  or redirect-attribution hook. This is also why REF01's documented
  residual (a cross-site redirect *response* leg can reveal the
  original full URL) cannot be closed from the interceptor.
- **Disposition.** The 454/456/458 warnings themselves are a harness
  artifact — verified policy-independent in SEC16B (identical warn
  sets under `refererPolicy=0` vs Trimmed; the warn subset flip-flops
  on the bare profile too, since `/set_referer`/`get_referer_policy`
  are session-global). The redirect-leg blind spot is the real
  engine-side bound; noted for a future Qt upgrade that surfaces
  referrer-policy control.
- **Upstream reference.** `services/network/redirect_util.cc`
  (`RedirectUtil::UpdateHttpRequest`, Chromium 140 tree);
  `QWebEngineUrlRequestInfo` API (no referrerPolicy field, Qt 6.12).

### Register summary

| tests | item | missing embedder API | upstream ref |
|-------|------|----------------------|--------------|
| 440–447 | manifest-src never evaluated — fetch never issued | manifest fetch/observe hook (`QWebEngine*` manifest API) | `content/browser/manifest/manifest_manager_host.cc`, `blink.mojom.ManifestManager` |
| 464 | report-to never arrives in-window | Reporting-API flush/observe control | `net/reporting/README.md`, `reporting_delivery_agent.cc` |
| 391/393 ctx | sandbox cookie attribution unreachable | frame sandbox/opaque-origin flag on `FilterRequest`/`UrlRequestInfo` | Qt 6.12 FilterRequest docs (3 fields only) |
| 454–458 ctx | redirect-leg Referer recompute past interceptor | per-request referrer policy / redirect-attribution hook | `services/network/redirect_util.cc` `UpdateHttpRequest` |

No engine-side item lacks a specific citation; all four are Qt-6.12 /
Chromium-140.0.7339.225 API-surface bounds, not generic "can't fix".
