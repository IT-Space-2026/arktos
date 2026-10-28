# CEF-GRAFT-AUTH — G3: webRequest.onAuthRequired on CEF (CS09)

**Verdict: G3 is a zero-line graft — the auth-answer delegate the task
assumed CEF lacks is already wired.** The upstream chain
`ChromeContentBrowserClientCef::CreateLoginDelegate` →
`ChromeContentBrowserClient::CreateLoginDelegate` →
`HttpAuthCoordinator::Flow::ForwardToExtension` →
`WebRequestAPI::MaybeProxyAuthRequest` → `ProxySet` →
`WebRequestEventRouter` → extension listener is intact and *fires* on
the unmodified chrome runtime — verified both by runtime legs and by a
log-only instrumentation build (`spikes/cef/cs09/cs09dbg-instrumentation.patch`
applied over the CS07 tree; `CS09DBG` trace lines in
`~/cef-cache/cs09-*.stderr.log`). Subresource challenges do not even
need that path: `MaybeProxyURLLoaderFactory` proxies the per-request
URLLoaderFactory once a webRequest extension is loaded
(`MPLF wr=1 → proxy=1` for every factory type), and the proxied
loader dispatches `onAuthRequired` itself.

**Verify hint satisfied**: `leg_nav` — `Page.navigate` to a
basic-auth URL answered by the extension, page commits `AUTH-OK` —
PASSes on src-chrome, src-alloy, and the stock Spotify prebuilt.

## Measured results (spikes/cef/cs09/)

`probe.py` + `isolate.py` drive `fixtures/wrprobe` (MV3, `webRequest`
+ `webRequestAuthProvider`) over CDP against a real basic-auth HTTP
server (`fixtures/wrprobe/auth_server.py`, 127.0.0.1:8899, per-path
realms + per-request Authorization logging). `isolate.py` runs each
mode on a fresh profile so every leg issues a real 401 challenge.

### Authoritative per-mode legs (fresh profile each — `~/cef-cache/cs09-iso-*`)

| Leg | src-chrome | src-alloy | prebuilt-chrome |
|---|---|---|---|
| `blocking` sync `{authCredentials}` return → 200 | PASS | PASS | PASS |
| `asyncBlocking` delayed cb (600ms) → 200 | PASS | PASS | PASS |
| `asyncBlocking` xhr immediate cb → 200 | PASS | — | PASS |
| `asyncBlocking` `{cancel:true}` → 401 delivered | PASS | PASS | PASS |
| wrong creds → re-challenge → cancel → 401, 2 events | PASS | PASS | PASS |

All legs server-log-verified: answered legs carried the exact
`Authorization: Basic <spikeuser:spikepass>` header, cancel legs never
did, and `leg-wrong` carried the fixture's `bad` creds (proof the
extension — nothing else knows them — answered the first challenge),
then cancelled the re-challenge. `leg_nav` additionally verified the
event details: `type=main_frame`, `scheme=basic`, `isProxy=false`,
correct `challenger`/`realm` fields.

`leg-wrong` initially *looked* flaky (fetch resolved 401 but the event
record read back empty): MV3 rotates service-worker instances and the
challenge can be dispatched to a different instance than the one the
CDP session attached to. Fixture now mirrors every event to
`chrome.storage.session` (browser-process state) — all three runtimes
PASS with the full 2-event record.

### Combined-run legs — preemptive-cache mask, upstream-correct

The single-profile `probe.py` run shows `leg_nav` PASS then every
subresource leg returning `200 AUTH-OK` with **zero** events and zero
challenges on the wire. This is not a defect: after the first
extension answer succeeds, `HttpAuthController::SelectPreemptiveAuth`
serves subsequent requests via `HttpAuthCache::LookupByPath` —
**path-scoped, realm-agnostic** (RFC 2617 protection space) — so no
401 is ever issued and `onAuthRequired` cannot fire. Identical on
stock Chrome. Reclassified INFO in the JSONs; the iso-* legs above are
the real measurements.

### The unanswered-challenge shapes (no extension listener matches)

- **Subresource** (`/protected` fetch): on the chrome runtime the
  proxied loader forwards to the login-delegate path, which falls to
  the Chrome views login prompt — a real `views::Textfield` dialog
  appears in stderr and the fetch stays pending (recorded `HUNG`).
  With `--disable-chrome-login-prompt` CEF falls back to
  `net_service::LoginDelegate` → `GetAuthCredentials` (embedder hook
  `CefRequestHandler::GetAuthCredentials`, unimplemented in cefsimple)
  → deterministic committed 401.
- **Main frame** (wrprobe-noperm baseline nav): normal flags →
  committed `chrome-error://chromewebdata` page (dialog path fails
  over to error under bare Xvfb); `--disable-chrome-login-prompt` →
  committed 401 `UNAUTHORIZED`. Both recorded as INFO shapes in
  `cs09-prebuilt-noperm-{prompt,noprompt}.json`.
- **API gating**: without the permission, `chrome.webRequest` is
  `undefined` (upstream permission hiding — same mechanism as CS12's
  `connectNative` correction). `api_surface` PASS on
  `wrprobe-noperm`.

### What the instrumentation showed (src build, unpatched + CS09DBG)

- `WRAPI ctor`, `OnExtLoaded wr=1`, `OnListenerAdded`/`AddListener`
  (lazy + non-lazy) all fire for the fixture's two listeners — full
  upstream registration path, including the MV3 lazy-wake listener
  copy (`is_lazy=1`).
- `MPLF wr=1 → proxy=1` for **all** URLLoaderFactory types after the
  extension loads (navigations *and* subresources).
- `cefCreateLoginDelegate` fires for the navigation 401;
  `FwdToExt proxied=1` → `ProxySetAuth` → extension answers.
- No `cefCreateLoginDelegate` for subresource challenges — they are
  serviced inside `WebRequestProxyingURLLoader` (no delegate needed).

## Implications

- **CS10 cost column: G3 patch lines = 0.** Like G2 (CS12), this graft
  existed only on paper — upstream linked the whole path into libcef.
- `webRequestAuthProvider` + the `asyncBlocking`/`blocking`
  `onAuthRequired` contract are **fully functional** on both runtimes
  and the shipped prebuilt: answer, cancel, re-challenge, lazy-SW
  dispatch all behave upstream-identically.
- The only real gap is the **non-extension** auth path: unanswered
  challenges reach the views login prompt (pend under a bare WM) or,
  with the prompt disabled, `CefRequestHandler::GetAuthCredentials`.
  An embedder needing basic-auth fills for non-extension requests
  implements that existing CEF hook — no graft required either.
- Corrections: CEF-BW-VERDICT's `onAuthRequired` row upgrades from
  "listener accepted" to "answers end-to-end (CS09)"; nothing in
  CEF-VERDICT claimed it dead, so no retraction needed.

## Repro

```bash
spikes/cef/cs09/run.sh all            # 3 runtimes + noperm + iso modes
spikes/cef/cs09/run-remaining.sh      # leg-wrong x3 + combined re-runs
# artifacts: ~/cef-cache/cs09-*.json + cs09-authserver.log (server rows)
```
