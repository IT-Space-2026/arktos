# XSLEAK01 — xsinator.com XS-Leak baseline + classification
(Qt 6.12.0 / Chromium 140.0.7339.225)

Baseline + per-vector disposition for the XS-Leak test battery at
https://xsinator.com/testing.html, following the SEC15/ANON01
convention (app-fixable / app-mitigable / engine-side with named API
gap / test-stale).  This is the report-only split (XSLEAK02): no code
changes, no blind re-runs — the baseline below is the user-measured
result JSON pasted into the parent task spec.

## Baseline provenance + suite methodology

XSinator measures **state-difference oracles**: for each vector it
fetches the same cross-origin resource in server state `res0` and
`res1` (triggered by URL parameters, not cookies) and checks whether
a side channel distinguishes them.  A "leaking" verdict means the
oracle exists, not that user data actually leaked.  This matters for
interpretation: the app's third-party-cookie block cannot close
fixtures whose state rides the URL rather than credentials.

Measured baseline (user, live site, app profile with shipping
defaults):

- **LEAKING (~18 vectors):** EventHandler (Object / Stylesheet /
  Script), RequestMerging, URLMaxLength, MaxRedirect, HistoryLength,
  CSPRedirectDetection, WebSocket (GC), FrameCount, MediaDimensions,
  MediaDuration, Cache (CORS), IdAttribute, CSSProperty,
  ContentDocument-X-Frame, CORP, CORB, DownloadDetection,
  PerformanceAPI-DownloadDetection, PerformanceAPI-CORP
  (ambiguous — res0 = res1 = 1).
- **CLEAN:** PerfAPIError, CORSError, RedirectStart, DurationRedirect,
  FetchRedirect, CSPViolation, SRI, Cache (POST), XSSAuditor.
- **TIMED OUT:** CSPDirective, COOP, PerfAPI-X-Frame.

## App-profile caveats (defaults differ from vanilla Chromium)

The baseline was taken on the real application profile.  These
shipping defaults can alter suite outcomes vs a bare engine and are
the first suspects for the timed-out vectors:

| Default | Mechanism | Suite interaction |
|---|---|---|
| `websettings/blockPopupWindows` = **on** | `WebPage::createWindow` refuses `WebBrowserWindow`/`WebDialog` types (window.open *with* a feature string) and returns a `PopupProbePage` dead-end — the opener gets a live handle whose every navigation is refused | Tests that orchestrate `window.open(url, name, 'width=…,height=…')` stall: the opened "window" never loads.  Prime suspect for the COOP timeout and a possible skew on window-mediated vectors |
| `privacy/blockPings` = **on** (PING01) | `ResourceTypePing` **and** `ResourceTypeCspReport` requests are failed in `PrivacyRequestInterceptor` | Any test whose verdict signal rides `report-uri`/`Report-To` delivery to xsinator's collector never sees it → timeout.  Prime suspect for the CSPDirective timeout |
| `privacy/httpsOnly` + `httpsFirst` = **on** (SAFE01) | surviving `http:` main-frame navigations are refused with an interstitial | If any fixture navigates a frame/window to a plain-`http:` endpoint, the refusal substitutes our warning page — can read as a false leak difference or stall |
| `cookies/blockThirdPartyCookies` = **on** (PRIV01) | `QWebEngineCookieStore` filter drops third-party Set-Cookie/cookie sends | Kills credential-state oracles (login-state detection).  Neutral for the suite's URL-parameter fixtures |
| `privacy/blockPrefetch` = **on** | `ResourceTypePrefetch` + `Purpose: prefetch` dropped | Neutral for this suite |
| AdBlock (enabled by default) | `AdBlockRequestInterceptor` fails matched requests | No known xsinator rule hits, but an unmatched-list assumption is unaudited per-run |
| Referrer policy = **Trimmed** (REF01) | cross-site `Referer:` spoofed to the target origin, same-site reduced to origin | Reduces referrer-based state leaks; not one of the flagged vectors |

Two app-code findings from auditing the named suspects:

- **NAM/QNetworkDiskCache is NOT in the page-load path.**  The parent
  spec's Cache(CORS) suspect is a misattribution: `NetworkAccessManager`
  + `NetworkDiskCache` only serve app-initiated fetches (opensearch
  suggestions, adblock list downloads, source-viewer re-fetch).
  Page resources go through Chromium's own network stack and its
  HTTP cache — the app has no handle on it.
- **Download accept path is not client-observable.**  For ordinary
  files `DownloadItem::getFileName` reaches `accept()` immediately
  (no dialog unless `alwaysPromptForFileName` or a dangerous
  extension); Chromium's navigation-interrupt semantics (what the
  oracle actually measures) happen below the app.  A cross-origin
  page cannot observe our accept timing through any JS API.

## LEAKING vectors — dispositions

| # | Vector | Baseline | Mechanism | Disposition |
|---|--------|----------|-----------|-------------|
| 1 | EventHandler (Object) | leaking | `onload`/`onerror` on `<object>` distinguishes res0/res1 | **engine-side** — no embedder API to observe or uniformize subresource success/failure events. Credential-riding variants already blunted by the default third-party cookie block |
| 2 | EventHandler (Stylesheet) | leaking | same via `<link>` | **engine-side** — same named gap |
| 3 | EventHandler (Script) | leaking | same via `<script>` | **engine-side** — same named gap |
| 4 | RequestMerging | leaking | inflight identical requests coalesce; timing reveals prior/recent fetch | **engine-side** — no API for request coalescing in Chromium's network service |
| 5 | URLMaxLength | leaking | navigation to >max-length redirect target errors out detectably | **engine-side** — Chromium `url::kMaxURLChars`; no embedder control |
| 6 | MaxRedirect | leaking | redirect chain cut at limit, observable via nav outcome | **engine-side** — redirect limit is engine-internal |
| 7 | HistoryLength | leaking | `window.history.length` oracle | **engine-side** — no API to alter history-length reporting |
| 8 | CSPRedirectDetection | leaking | CSP `img-src` etc. reveals redirect targets | **engine-side** — note: our `ResourceTypeCspReport` block does NOT prevent the oracle (detection rides whether the resource loaded, not the report) |
| 9 | WebSocket (GC) | leaking | ws connect accept/refuse distinguishes endpoint state | **engine-side, app-mitigable** — the interceptor DOES see ws/wss upgrades (arrive as resourceType 254, `isWebRequestScheme` covers them; adblock `$websocket` option works). An opt-in "block third-party WebSockets" policy is reachable but breaks real-time sites; left as XSLEAK03 candidate, not a default |
| 10 | FrameCount | leaking | `window.frames.length` counts cross-origin frames | **engine-side** — no embedder hook for frame-tree visibility |
| 11 | MediaDimensions | leaking | `<video>` intrinsic size oracle | **engine-side** — media metadata surface, no API |
| 12 | MediaDuration | leaking | media duration oracle | **engine-side** — same surface |
| 13 | Cache (CORS) | leaking | cross-origin fetch timing reveals cached state | **engine-side** — Chromium HTTP cache, NOT our NAM (see caveat). Chrome 86+ partitions the HTTP cache by top-frame site, so this vector may be partially **test-stale** on Chromium 140 — re-measure in XSLEAK04 |
| 14 | IdAttribute | leaking | fragment/id scroll-oracle | **engine-side** — DOM-level oracle, no API |
| 15 | CSSProperty | leaking | CSS state oracle (applied-style side effects) | **engine-side** — same class |
| 16 | ContentDocument-X-Frame | leaking | `iframe.contentDocument` null-vs-accessible reveals XFO/frame-ancestors | **engine-side** — SOP enforcement internals |
| 17 | CORP | leaking | Cross-Origin-Resource-Policy presence detectable | **engine-side** — named gap: `QWebEngineUrlRequestInterceptor` sees request metadata only, **no response-header access** — CORP can neither be hidden nor enforced app-side |
| 18 | CORB | leaking | CORB vs non-CORB response detectable | **engine-side** — same named gap (no response-header access) |
| 19 | DownloadDetection | leaking | download-vs-render outcome on a popup/navigation observable | **engine-side** — interrupt semantics are Chromium's; app accept-path timing not client-observable (verified above). No named API to uniformize the outcome |
| 20 | PerfAPI-DownloadDetection | leaking | Resource Timing presence/shape reveals the download | **engine-side** — no embedder control over Resource Timing entries |
| 21 | PerfAPI-CORP | **ambiguous** (res0 = res1 = 1) | Resource Timing + CORP interaction | **engine-side / inconclusive** — equal results may be a true negative; XSLEAK04 re-measure decides |

## CLEAN vectors (no action)

PerfAPIError, CORSError, RedirectStart, DurationRedirect,
FetchRedirect, CSPViolation, SRI, Cache (POST), XSSAuditor — engine
already returns no detectable difference; consistent with
partitioned-cache (POST clean while CORS leaks fits the
partitioning model) and removed-XSSAuditor expectations.

## TIMED OUT vectors — dispositions + local-repro notes

| Vector | Disposition | Evidence / repro note |
|--------|-------------|------------------------|
| CSPDirective | **app-side explained — XSLEAK03 verified** (deliberate feature, not a defect) | PING01 fails `ResourceTypeCspReport` requests by default; a test whose verdict rides report delivery to the collector stalls forever. XSLEAK03 attribution run: `arora --ping-smoke` PASS — armed phase saw the CSP violation fire (`img-src 'none'` refusal logged) but `/csp` never reached the collector alongside `/beacon` and `/aping`; the disarmed control delivered all three. So the mechanism is confirmed: enforcement intact, report upload dropped, verdict stalls. XSLEAK04 should still re-run this vector with `privacy/blockPings=false` to capture the real verdict |
| COOP | **app-side explained — XSLEAK03 verified** (popup dead-end), else test-stale | `window.open` with a feature string arrives as `WebBrowserWindow`/`WebDialog` → the default-on popup blocker returns a `PopupProbePage` whose every navigation is refused — an orchestrated window never loads → timeout. XSLEAK03 attribution: `tst_webpage` `popupBlocking`/`popupClickBlocking`/`popupTargetCapture`/`popupStateReset` all PASS, confirming the dead-end behavior is live and default-on. The live-vector confirmation with `websettings/blockPopupWindows=false` remains with XSLEAK04 — COOP has no embedder hooks either way |
| PerfAPI-X-Frame | **test-stale / environmental** | XFO detection via Resource Timing rides iframes (not popup-gated) — no app mechanism explains a stall; likely orchestration/network flake under the remote run. Engine-side surface (Resource Timing + XFO internals) either way |

## Named missing APIs (engine-side summary)

Every engine-side row above traces to one of these gaps — each is
the specific hook a future Qt/Chromium upgrade could provide:

1. **No response-header access** in `QWebEngineUrlRequestInterceptor`
   (request metadata only) — CORP, CORB, CSPDirective enforcement
   headers are unobservable and unenforceable app-side.
2. **No subresource load/error uniformization hook** — the
   EventHandler family (success/failure events are renderer-internal).
3. **No frame-tree/DOM-visibility control** — FrameCount, IdAttribute,
   ContentDocument-X-Frame, MediaDimensions, MediaDuration,
   CSSProperty.
4. **No navigation-oracle controls** — URLMaxLength, MaxRedirect,
   HistoryLength, CSPRedirectDetection (limits and history reporting
   are engine-internal).
5. **No Performance/Resource-Timing API control** — the PerfAPI
   family.
6. **No request-coalescing hook** — RequestMerging.
7. **No renderer HTTP-cache partitioning API** — Cache (CORS);
   Chromium's partition is internal and fixed.
8. **No download-interrupt outcome hook** — DownloadDetection.

## XSLEAK03 ordered fix list (app-side only)

Ranked by value/effort; each item bounded ~30 min, checkpoint into
this report after each. **Outcome column added by XSLEAK03.**

1. **Attribute the two app-explained timeouts** — *done.*  No live
   xsinator run (reserved for XSLEAK04); attribution is local:
   `arora --ping-smoke` PASS shows `/csp` (CSP report-uri upload)
   dropped while the violation still fires — the CSPDirective stall
   mechanism is confirmed.  `tst_webpage` popup tests PASS show the
   default-on dead-end `PopupProbePage` for feature-string
   `window.open` — the COOP stall mechanism is confirmed.  See the
   TIMED OUT table.
2. **E2E third-party-cookie rejection check** — *done.*  New
   `tst_privacy::thirdPartyCookieEndToEnd`: a localhost page embeds a
   `127.0.0.1` iframe (cross-site) whose `Set-Cookie: xsleak_third=1;
   SameSite=None; Secure` is refused by the profile's cookie-store
   filter when armed — `cookieAdded` never fires for it and the cookie
   is never sent back; the disarmed control both stores and sends it.
   `SameSite=None;Secure` is required in the fixture — without it
   Chromium drops the cookie itself and the armed phase proves
   nothing.
3. **Opt-in third-party WebSocket block (evaluate)** — *implemented.*
   `privacy/blockThirdPartyWebSockets` (default OFF — legitimate
   cross-site chat/feed/realtime sockets would break):
   `PrivacyRequestInterceptor::shouldBlockWebSocket()` refuses a
   `ResourceTypeWebSocket` upgrade whose request host is not same-site
   with the first-party host (same last-two-label approximation the
   other policies use, not a full PSL).  Applies to normal and Tor
   profiles; decision is pure/readable from the IO thread.  Covered
   by `webSocketBlockDecision` (pure function matrix) and
   `thirdPartyWebSocketEndToEnd` (real page on a loopback fixture:
   armed cross-site ws never reaches the wire, same-site and disarmed
   connect).  Engine quirk found while testing: a ws upgrade refused
   *during initial document parse* leaves the handshake pending and
   the `load` event never fires — a blocker for Chromium, cosmetic
   for users (the page renders; only the load-finished signal waits).
4. **Document the Cache(CORS) verdict** — *done.*  Recorded in the
   leaking table (row 13): Chromium's renderer/network-service HTTP
   cache partition — Arora's `QNetworkAccessManager`/
   `QNetworkDiskCache` serve only app-initiated fetches, never the
   page path.
5. **Document download-timing non-observability** — *done.*  Rows
   19-21 carry it: ordinary-file `accept()` is immediate and the
   interrupt outcome isn't exposed through client-observable app
   behavior.
6. **Scheme-handler leak audit — verified clean, recorded** — *done.*
   `FileAccessHandler` denies remote initiators (SEC02),
   `arora-resource:` serves fixed inert stubs, `abp:` prompts before
   acting and fails the job afterwards, interstitials are
   nonce-gated.  No scheme-handler XS-Leak surface identified.
7. **Baseline caveat for XSLEAK04** — *done.*  See the XSLEAK04 note
   below; the sensitive vectors are called out there.

## XSLEAK03 results summary

| Item | Result |
|------|--------|
| New mitigation | `privacy/blockThirdPartyWebSockets` — opt-in, off by default, normal + Tor profiles, Settings > Privacy checkbox |
| New tests | `webSocketBlockDecision`, `thirdPartyCookieEndToEnd`, `thirdPartyWebSocketEndToEnd` in `tst_privacy` |
| Timeout attribution | CSPDirective and COOP verified app-explained locally (`--ping-smoke`, `tst_webpage` popup tests); live confirmation deferred to XSLEAK04 |
| Documentation | Cache(CORS), DownloadDetection, scheme-handler audit verdicts recorded in the tables above |

## XSLEAK04 note

Re-run guidance: measure with app defaults AND with
`blockPopupWindows`/`blockPings` disabled to attribute timeouts;
every vector must end with a fixed / app-mitigated /
engine-side-plus-named-API / test-stale disposition.

Arora's defaults skew several vectors relative to vanilla Chromium —
read deltas against this list, not against upstream:

- `websettings/blockPopupWindows` = on → popup-mediated vectors
  (COOP, DownloadDetection, anything `window.open`-orchestrated).
- `privacy/blockPings` = on → `ResourceTypePing` and
  `ResourceTypeCspReport` uploads dropped (CSPDirective verdict
  channel).
- `privacy/httpsFirst` + `privacy/httpsOnly` → http: probes are
  upgraded/refused before they hit the network (any vector whose
  fixtures live on http: endpoints).
- `cookies/blockThirdPartyCookies` = on → third-party
  storage/state oracles collapse to clean.
- `privacy/blockPrefetch` = on → prefetch/prerender probes dropped.
- `privacy/blockThirdPartyWebSockets` (opt-in, default off) → when
  armed, WebSocket(GC)-family oracles collapse to clean.
- Adblock subscriptions → any vector whose endpoints match rules.
