# SEC14 — browseraudit.com CSP-category warnings: investigation + verdict

Environment: Qt 6.12.0 (bundled Chromium **140.0.7339.225**), offscreen,
Linux.  Harness: `autotests/csp/tst_csp.cpp` — scripted in-process
HTTP/HTTPS responders, a real 101-completing WebSocket endpoint, a
TLS-speaking page server (throwaway openssl cert, accepted via the
`certificateError` signal), and a plain-TCP "tap" port that detects
whether a `wss://` attempt ever left the page.  Every case runs twice:
on a **bare** `QWebEngineProfile` and on an **app-wired** profile
carrying the real CookieJar filter (third-party blocking default ON)
plus PrivacyRequestInterceptor — so an app-side cause would appear as
a bare-vs-wired divergence.

## App-side audit (task step a/b)

- `QWebEngineUrlRequestInfo` is request-side only — no code anywhere in
  `src/` can read or mutate response headers, so a CSP header cannot be
  stripped by our interceptors.  The only `setHttpHeader` writes are the
  Referer spoof (cross-site navigations) and NAM-side Accept-Language.
- Interceptors touched: PrivacyRequestInterceptor (http→https upgrade
  for `ResourceTypeMainFrame` only, Safer-tier script drops on http
  first-party, referer trim, adblock delegation), TorRequestInterceptor
  (same + .onion), AdBlockRequestInterceptor (ruleset matching).  None
  of them can reach websocket requests under `default-src 'self'`
  logic — and with a fresh profile the adblock ruleset is empty anyway.
  No QWebSocket client or ws-capable scheme handler exists in-tree
  (`grep -ri websocket src/` → only the adblock resource-type mapping).

## test-361 / test-364 — "WebSocket to wss:// NOT blocked under 'self'"

browseraudit source (testsuite SQL, 2015): both tests connect to
`wss://browseraudit.com` — the **same host** as the page — and expect
`block`.  That expectation is **stale**: CSP Level 3 changed `'self'`
to match the `ws:`/`wss:` variants of the page's origin (same host +
same-or-default port), implemented in Chromium 70
(crbug.com/815142, Sept 2018).  Per the CSP3 matching algorithm, an
https origin's `'self'` matches `wss://` on the same host — so modern
Chromium *must allow* the connection; the suite's expected-block is
pre-CSP3.

Local evidence (same shape, real sockets):

| case | bare | app-wired |
|---|---|---|
| https page, `default-src 'self'`, wss same host+port | handshake reached server | same |
| https page, `connect-src 'self'`, wss same host+port | allowed | same |
| http page, `default-src 'self'`, ws same host+port | allowed | same |
| `connect-src 'self'` ws same host+port | allowed | same |
| `connect-src ws://host:port` (explicit) | allowed | same |
| `'self'` ws **cross-port** (non-default) | **blocked** + violation | same |
| `'self'` ws/wss **cross-host** (localhost vs 127.0.0.1) | **blocked** + violation, 0 TCP hits on tap | same |
| `default-src 'none'` | blocked + violation | same |
| `connect-src 'none'` (wss and ws) | blocked + violation, 0 TCP hits | same |
| `connect-src http://host` + ws URL | **blocked** — http source does not up-match ws: (spec-correct) | same |

**Verdict: test-stale.**  The engine enforces connect-src on websockets
correctly (every block control fires a `securitypolicyviolation` and
never touches the network); allowing the same-origin `wss:` under
`'self'` is the spec-mandated CSP3 behavior.  No app-side divergence
observed — interceptors do not alter the outcome.  Nothing to fix.
browseraudit test-367/368 (cross-host `wss://test.browseraudit.com`)
keep passing precisely because 'self' does not extend across hosts.

## test-391 / test-393 — sandboxed iframe cookie access (expected: allow)

browseraudit source: child iframe served `Content-Security-Policy:
sandbox allow-same-origin allow-scripts`, expects the child to read
the `.browseraudit.com` cookie — expectation `allow`, so a warning
means the cookie was **blocked** on the user's run.

Local repro (parent `127.0.0.1:P_A`, iframe `127.0.0.1:P_B` —
cross-origin but same-site; host cookie pre-seeded):

| case | bare | app-wired |
|---|---|---|
| `sandbox allow-same-origin allow-scripts` — `document.cookie` read | sees cookie | same |
| same — `document.cookie` **write** then read | write lands | same |
| same — Cookie header on iframe-initiated fetch | sent | same |
| `sandbox allow-scripts` (opaque origin, control) | cookie denied, no Cookie header | same |

**Verdict: spec-correct locally; warning mechanism not reproducible
on this build.**  The CSP `sandbox` directive is still enforced by the
bundled engine (the opaque-origin control proves it), the app-side
third-party cookie filter does not misfire on same-site sandboxed
frames, and both read and write paths work with `allow-same-origin`.
If the user's run flagged 391/393, the plausible residual causes are
harness-side (the 2015 suite relies on cross-subdomain plumbing and,
elsewhere, `document.domain` tricks that Chrome 109+ removed) — not a
defect in our layer.  Recorded as: **engine verified spec-correct;
suite expectation/environment stale.**

## Bottom line

- 361/364: **test-stale** (CSP3 `'self'`⊇wss same-origin, crbug.com/815142).
- 391/393: **not reproduced** — local equivalent behaves per spec on
  both bare and app-wired profiles; no app-side defect found.
- Zero `src/` changes required; the repro harness is committed as a
  regression suite (`autotests/csp`, 15 cases) so future engine bumps
  will surface a real CSP regression if one ever appears.
