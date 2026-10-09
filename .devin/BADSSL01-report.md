# BADSSL01 — badssl.com baseline (BADSSL02) + fix pass (BADSSL03)

Baseline measurement of the full badssl.com test matrix against the
current Arora build, followed by the BADSSL03 app-level fix pass.
BADSSL02 was measurement only; every non-pass disposition from the
baseline was re-visited below in "BADSSL03 fix pass".

- **Date:** 2026-10-09
- **Engine:** QtWebEngine / Chromium 140.0.7339.225 (Qt 6.12.0, gcc_64)
- **Method:** `./arora --badssl-smoke` — a headless harness
  (`QT_QPA_PLATFORM=offscreen`) that loads every case sequentially on
  the real browsing profile, observing the SEC06 certificate-error
  interstitial, the SAFE01 HTTPS-Only warning, load failures, the final
  committed URL, and a post-load DOM probe (title, footer text, image
  widths, body background).  Raw JSON: `/tmp/badssl-baseline.json`.
  BADSSL03 added `ARORA_CLIENTCERT_FILE`/`ARORA_CLIENTCERT_PASSPHRASE`
  support so the `client` case can run with a certificate installed
  (selection auto-armed for the headless run).
- **Result totals:** 72 cases — 23 loaded, 49 blocked, 0 other.

## Verdict legend

| Verdict | Meaning |
|---|---|
| `ok` | Behavior matches badssl's expected label. |
| `ok-stale-fixture` | Blocked, and correctly so — the test fixture itself is broken upstream (e.g. expired "good" certificate). badssl's label is stale. |
| `engine-gap` | Genuinely wrong behavior rooted in QtWebEngine/Chromium: no embedder API exists to fix it. Documented, not fixable app-side. |
| `test-obsolete` | The badssl test asserts a feature modern engines no longer have; behavior matches every current Chromium/Firefox. |
| `app-gap` | Wrong or suboptimal behavior that *is* fixable in Arora — input to BADSSL03. |
| `transient` | Passed on re-run; the first failure was network flake. |

## Summary of misses

| Case | Expected | Observed | Disposition |
|---|---|---|---|
| `revoked` | bad → blocked | loaded | **engine-gap** — see below; re-run in the fix pass still `loaded`, disposition confirmed |
| `pinning-test` | bad → blocked | loaded | **test-obsolete** — HPKP removed from Chromium in M72; loads in every modern browser |
| `mixed-script` | bad → blocked | loaded, but insecure script did **not** run | **ok** — Chromium's mixed-content blocker stopped the subresource (page bg stayed gray; the "script ran" red marker absent). The main frame loading is correct; badssl's binary pass/fail can't express subresource blocking. |
| `mozilla-old` | bad → blocked | loaded | **test-obsolete** — the fixture negotiates TLS 1.3 with a modern client; "supports old TLS" is a server-config property no client can observe by browsing. |
| `client` | good → loaded | HTTP 400 (cert-less) / loaded (with cert) | **FIXED (BADSSL03)** — see below |
| `sha384`, `sha512`, `1000-sans`, `extended-validation` | good → loaded | cert interstitial | **ok-stale-fixture** — upstream certs expired Apr 2022 / Oct 2021 / Aug 2022 (verified via openssl). Blocking is correct. |
| `rsa8192` | dubious | cert interstitial | **ok-stale-fixture** — cert expired Mar 2024. |
| `10000-sans` | good → loaded | ERR_CONNECTION_RESET | **ok-stale-fixture** — certificate message too large; fails in raw openssl too. Endpoint effectively broken. |
| `longextendedsubdomain` | good → loaded | ERR_CONNECTION_RESET (first pass) | **transient** — re-run loaded cleanly (`ARORA_BADSSL_ONLY=longextendedsubdomain` → loaded). |
| `http` family (6 cases) | bad → blocked | error page, ERR_FAILED → HTTPS-Only warning | **FIXED (BADSSL03)** — all six cases now surface the SAFE01 warning interstitial; see "BADSSL03 fix pass" |

### `revoked` — engine-gap detail

`revoked.badssl.com` presents a fresh Let's Encrypt leaf (notBefore
2026-09-15, notAfter 2026-12-14) whose revocation is asserted via
CRL/OCSP.  Chromium's revocation enforcement is CRLSets-push plus
optional OCSP stapling/hard-fail for EV — an LE DV cert appears in
neither, so the connection commits.  QtWebEngine exposes no
`QWebEngineCertificateError` for revocation and no profile setting to
enable OCSP/CRLSet enforcement, so there is no embedder-level fix.
Same as stock QtWebEngine and close to default Chrome for non-EV certs.

### `client` — FIXED (BADSSL03)

`client.badssl.com` requires a TLS client certificate; without one the
server returns HTTP 400 — which is also what Chrome does with no client
cert installed.  Qt 6.12 exposes `QWebEnginePage::selectClientCertificate`
(and `QWebEngineClientCertificateStore`).

BADSSL03 wired both ends:

- `BrowserProfile::addClientCertificateFile()` loads a PEM or
  PKCS#12/PFX file (encrypted PEM keys and .p12 bundles honour
  `ARORA_CLIENTCERT_PASSPHRASE`) into
  `QWebEngineProfile::clientCertificateStore()`.
- `WebPage` now connects `selectClientCertificate`: it picks a
  certificate from the offered list (auto-selecting when
  `ARORA_CLIENTCERT_AUTOSELECT=1`, used by the harness) or
  `selectNone()` when the store is empty — matching Chromium's
  continue-without-cert behaviour.
- Verified: `./arora --clientcert-smoke` with the badssl fixture
  (badssl.com-client.pem + passphrase `badssl.com`) → **PASS**, page
  title `client.badssl.com`; and `ARORA_BADSSL_ONLY=client
  ARORA_CLIENTCERT_FILE=... ./arora --badssl-smoke` → `client: loaded`,
  `client-cert-missing` still blocked, `revoked` still the engine gap.
  Cert-less Chrome parity (HTTP 400) is preserved when no cert is
  configured.

## Full matrix

Legend for *Observed*: `cert-interstitial` = SEC06 arora-cert-error
page; `error-page` = generic failure page (net error noted); `loaded`
= page committed.

### certificate

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| expired | bad | cert-interstitial | ok |
| wrong.host | bad | cert-interstitial | ok |
| self-signed | bad | cert-interstitial | ok |
| untrusted-root | bad | cert-interstitial | ok |
| revoked | bad | loaded | engine-gap |
| pinning-test | bad | loaded | test-obsolete |
| no-common-name | dubious | cert-interstitial | ok |
| no-subject | dubious | cert-interstitial | ok |
| incomplete-chain | dubious | loaded (Chromium AIA fetching completes the chain) | ok |
| sha256 | good | loaded | ok |
| sha384 | good | cert-interstitial | ok-stale-fixture (upstream cert expired Apr 2022) |
| sha512 | good | cert-interstitial | ok-stale-fixture (expired Apr 2022) |
| 1000-sans | good | cert-interstitial | ok-stale-fixture (expired Oct 2021) |
| 10000-sans | good | error-page: ERR_CONNECTION_RESET | ok-stale-fixture (cert too large; fails in openssl too) |
| ecc256 | good | loaded | ok |
| ecc384 | good | loaded | ok |
| rsa2048 | good | loaded | ok |
| rsa4096 | good | loaded | ok |
| rsa8192 | dubious | cert-interstitial | ok-stale-fixture (expired Mar 2024) |
| extended-validation | good | cert-interstitial | ok-stale-fixture (expired Aug 2022) |

### client-certificate

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| client | good | error-page: HTTP 400 | app-gap candidate (no `selectClientCertificate` handler; outcome matches cert-less Chrome) |
| client-cert-missing | bad | error-page: HTTP 400 | ok |

### mixed-content

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| mixed-script | bad | loaded; insecure script blocked (bg gray, script-did-run marker absent) | ok |
| very | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| mixed | dubious | loaded; http image auto-upgraded (256px rendered) | ok |
| mixed-favicon | dubious | loaded | ok |
| mixed-form | dubious | loaded | ok |

### http

All six: `https→http` downgrade redirect vetoed, error page shown
(ERR_FAILED attributed to the https source URL).

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| http | bad | error-page: ERR_FAILED | ok (app-gap polish: HTTPS-Only warning page not surfaced) |
| http-textarea | bad | error-page: ERR_FAILED | ok |
| http-password | bad | error-page: ERR_FAILED | ok |
| http-login | bad | error-page: ERR_FAILED | ok |
| http-dynamic-login | bad | error-page: ERR_FAILED | ok |
| http-credit-card | bad | error-page: ERR_FAILED | ok |

### cipher-suite

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| cbc | dubious | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| rc4-md5 | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| rc4 | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| 3des | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| null | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| mozilla-old | bad | loaded (TLS 1.3 negotiated; old-config unobservable) | test-obsolete |
| mozilla-intermediate | dubious | loaded | ok |
| mozilla-modern | good | loaded | ok |

### key-exchange

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| dh480 | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| dh512 | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| dh1024 | bad | error-page: ERR_CONNECTION_RESET | ok |
| dh2048 | dubious | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| dh-small-subgroup | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| dh-composite | bad | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| static-rsa | dubious | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |

### protocol

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| tls-v1-0 | dubious | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| tls-v1-1 | dubious | error-page: ERR_SSL_VERSION_OR_CIPHER_MISMATCH | ok |
| tls-v1-2 | good | loaded | ok |

### certificate-transparency

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| no-sct | bad | cert-interstitial | ok |

### upgrade

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| hsts | good | loaded | ok |
| upgrade | good | loaded | ok |
| preloaded-hsts | good | loaded | ok |
| subdomain.preloaded-hsts | bad | cert-interstitial | ok |
| https-everywhere | good | loaded | ok |

### ui

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| spoofed-favicon | dubious | loaded | ok |
| lock-title | dubious | loaded | ok |
| long-extended-subdomain-name | good | loaded | ok |
| longextendedsubdomain | good | loaded (retry; first pass hit transient ERR_CONNECTION_RESET) | ok |

### known-bad

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| superfish | bad | cert-interstitial | ok |
| edellroot | bad | cert-interstitial | ok |
| dsdtestprovider | bad | cert-interstitial | ok |
| preact-cli | bad | cert-interstitial | ok |
| webpack-dev-server | bad | cert-interstitial | ok |

### chrome

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| captive-portal | bad | cert-interstitial | ok |
| mitm-software | bad | cert-interstitial (untrusted private CA) | ok |

### defunct

| Case | Expected | Observed | Verdict |
|---|---|---|---|
| sha1-2016 | dubious | error-page: ERR_CONNECTION_RESET | ok |
| sha1-2017 | bad | error-page: ERR_CONNECTION_RESET | ok |
| sha1-intermediate | bad | cert-interstitial | ok |
| invalid-expected-sct | bad | cert-interstitial | ok |

## Environmental notes

- `Fontconfig error: Cannot load default config file` appears during
  offscreen rendering; cosmetic, does not affect results.
- badssl.com rate-limits aggressive sequential fetching; a small
  per-case settle delay is built into the harness, and one transient
  reset (`longextendedsubdomain`) cleared on retry.

## Input to BADSSL03

1. **app-gap:** surface the SAFE01 HTTPS-Only warning page (or at least
   its wording) when an `https→http` redirect hop is vetoed — currently
   falls through to the generic error page because the failure is
   attributed to the https source URL while `s_blockedHttpNavs` keys on
   the http URL.  → **DONE**, see below.
2. **app-gap:** consider handling `QWebEnginePage::selectClientCertificate`
   so client-cert flows can work with a user-installed cert.
   → **DONE**, see `client` section above.
3. **engine-gap (document only):** revocation checking — no QtWebEngine
   API; matches stock engine behavior.  → confirmed, still `loaded`.
4. **test-obsolete (document only):** `pinning-test` (HPKP gone),
   `mozilla-old` (server-config assertion).  → confirmed.
5. Everything else is correct or blocked by stale upstream fixtures.

## BADSSL03 fix pass

### HTTPS-Only warning on downgrade bounce (http family)

Deeper tracing changed the root cause: the http hop is **not** vetoed
by the interceptor at all.  The sequence is:

1. `http://http.badssl.com/` → https-first interceptor upgrades to https.
2. The https endpoint 301s back to the *same* http URL (real server
   behaviour, confirmed with curl).
3. `acceptNavigationRequest` sees the redirect hop with
   `NavigationTypeRedirect` and `m_requestedUrl` = the https URL; the
   interceptor re-upgrades.
4. Chromium's redirect-loop guard aborts with `ERR_FAILED` attributed
   to the https URL → `takeBlockedHttpNav` misses → generic error page.

Fix, all in `src/webpage.cpp` + `src/network/privacyrequestinterceptor.*`:

- `PrivacyRequestInterceptor::markDowngraded(host, scope)` records the
  host in the per-scope downgraded-hosts set (`s_downgradedHosts`,
  session-scoped + TTL'd like the connection-failure marks), so the
  https-first upgrade stops re-claiming the hop.
- `WebPage::acceptNavigationRequest` detects the bounce — a main-frame
  `NavigationTypeRedirect` hop to `http:` whose https form equals
  `m_requestedUrl` — calls `markDowngraded`, then lets the ordinary
  SAFE01 decision
  below run: HTTPS-Only vetoes and warns, plain https-first loads the
  plaintext page (Chrome parity).  Gated on `isUpgradeCandidate`, so a
  host already skipped by the upgrade (private/local, already marked)
  is untouched.
- `handleLoadingChanged` also gained an https-attribution fallback:
  when a vetoed/bounced http hop's failure is reported under the
  https: redirect source, the downgraded `http:` spelling is consulted
  too (`takeBlockedHttpNav` + the `m_httpWarningPending` suppression
  check).
- `WebPage::handleLoadingChanged` then consumes the recorded URL and
  swaps in the nonce-bound `arora-http-warning:` interstitial (Back to
  safety / Proceed once / Always allow for host).

Verified: `ARORA_BADSSL_ONLY=http ./arora --badssl-smoke` → all six
http cases (`http`, `http-textarea`, `http-password`, `http-login`,
`http-dynamic-login`, `http-credit-card`) report
`blocked:http-warning` (`ok-blocked`); `https-everywhere` still
`loaded`.  `./arora --httpsonly-smoke` → PASS (upgrade, downgrade
warning, proceed, redirect-hop warning, https unaffected, loopback
exempt).

### Certificate-error coverage (SEC06)

Re-verified during the fix pass: every untrusted-cert case in the
matrix lands on the SEC06 `arora-cert-error:` interstitial
(`handleCertificateError`), which honours `error.isOverridable()` —
non-overridable errors (e.g. `CertificateKnownRevoked`-class failures
upstream) get only "Back to safety".  Main-frame errors route through
the interstitial; subresource certificate failures are rejected
without a page swap, matching Chromium.

### Re-run results (2026-10-09)

| Check | Result |
|---|---|
| `badssl-smoke` http family ×6 | `blocked:http-warning` ×6, `ok-blocked` |
| `badssl-smoke` `client` (with fixture cert) | `loaded` |
| `badssl-smoke` `client-cert-missing` | `blocked` |
| `badssl-smoke` `revoked` | `loaded` — engine gap, documented |
| `clientcert-smoke` | PASS (title `client.badssl.com`) |
| `httpsonly-smoke` | PASS (all stages) |
| clean qmake/make build | PASS |
| `make check` | PASS |

### New env surface (internal smoke tooling)

| Variable | Purpose |
|---|---|
| `ARORA_CLIENTCERT_FILE` | PEM or PKCS#12/PFX file installed into the profile cert store |
| `ARORA_CLIENTCERT_PASSPHRASE` | Passphrase for encrypted PEM keys / .p12 bundles |
| `ARORA_CLIENTCERT_URL` | Override the `--clientcert-smoke` target (default `https://client.badssl.com/`) |
| `ARORA_CLIENTCERT_AUTOSELECT` | `1` → auto-pick a client cert without a UI prompt (headless runs) |
