# TELEM02 — During-browsing telemetry sweep + DNS prefetch

Follow-up to TELEM01 (which zeroed the *idle-launch* surface). This
task measured Arora's outbound traffic **while a user actually browses**
and closed the DNS-prefetch leak.

## Harness — `--telemetry-browse-smoke`

TELEM01's capture proxy (`src/main.cpp`) was extended from
record-and-502 into a real forwarding proxy (`s_telemetryForward`):
CONNECT gets a `200 Connection Established` tunnel, plain-HTTP
requests are relayed verbatim, and every normalized `host:port` target
is recorded in `s_telemetryHosts`.  Idle `--telemetry-smoke` keeps the
fail-fast 502 semantics unchanged.

The smoke:

1. Pins `privacy/dnsPrefetch` and `privacy/secureDnsMode` to known
   values so the measurement is independent of the operator's settings
   (`ARORA_TELEMETRY_PREFETCH=1` arms prefetch instead — used by the
   detector-differential, restored afterwards).
2. Drives the real browsing session through the tap: navigation to
   example.com, a DuckDuckGo HTML search query, and a file download
   (`ARORA_TELEMETRY_URLS` overrides the leg list;
   `ARORA_TELEMETRY_SETTLE_MS` the post-activity watch window).
3. Polls `/proc/net` the whole session for process-tree sockets that
   bypass the proxy (QUIC, direct connects, stray DNS).
4. Classifies every observed endpoint:

   | Verdict | Rule |
   |---------|------|
   | `content` | host is, is a subdomain of, or shares the registrable domain of a browsed host (plus `ARORA_TELEMETRY_CONTENT_HOSTS` extras) |
   | `essential:cert-validation` | engine-managed OCSP/CRL/CT endpoints (`*ocsp*`, `*crl*`, `*.pki.goog`) |
   | `proxied-upstream` | socket-scan endpoint that is one of the capture proxy's own upstream dials |
   | `essential:system-dns` | socket-scan endpoint on :53/:853 — the forwarding proxy's own resolver traffic |
   | `NON-CONTENT` | anything else — **fails the run** |

5. Runs the DNS-prefetch differential (below).
6. Restores the pinned settings and the proxy switch.

## Measured results

Reference browsing session (example.com navigation, html.duckduckgo.com
search query, example.com download, 15 s settle):

| Endpoint | Class |
|----------|-------|
| `example.com:443` | content |
| `html.duckduckgo.com:443` | content |
| `duckduckgo.com:443` | content |
| `external-content.duckduckgo.com:443` | content (search-result image proxy) |
| direct sockets | none beyond the proxy's own upstreams/DNS |

No connectivity checks (`clients3.google.com/generate_204`), no
variations/field-trial fetches, no component updater, no OCSP/CRL
fetches were observed in this session — TELEM01's kill-flags and the
missing `chrome_branded` build bits keep QtWebEngine quiet.  OCSP/CRL
endpoints are classified `essential:cert-validation` when they appear:
they are the engine validating TLS revocation on the user's behalf and
silencing them would weaken certificate validation — documented as
unavoidable-by-design, never blocked.

## DNS prefetch — `privacy/dnsPrefetch` (default OFF)

Chromium's DNS prefetch resolves every hostname linked by a rendered
page before the user clicks — an unsolicited stream of
browsing-interest hints to the resolver.  `browserprofile.cpp` now
applies the attribute from `privacy/dnsPrefetch`, default **off**;
Settings > Privacy gains a "Prefetch DNS for linked sites" checkbox.

Tor hardening: `DnsPrefetchEnabled` is force-disabled on the tor
profile in `BrowserApplication::prepareProfile()` and re-pinned in
`loadSettings()` — engine DNS would resolve via the *system* resolver
outside the SOCKS tunnel, a proxy-bypass class bug.  `--tor-window-smoke`
asserts the attribute reads back off.

### End-to-end proof (`--telemetry-prefetch-child-smoke`)

The parent browse smoke spawns two child runs; each serves a fixture
page whose only reference to a unique `telem02-<pid>.invalid` hostname
is a `<link rel="dns-prefetch">` plus a plain anchor — traffic a real
browser generates for a page the user never navigates to.  Each child
runs with `--log-net-log`, and the parent greps the child's netlog for
a resolver event naming the probe host:

| `privacy/dnsPrefetch` | netlog contains probe host | verdict |
|-----------------------|----------------------------|---------|
| off | absent | PASS |
| on (control) | present | PASS — proves the detector would catch a regression |

The ON control matters: an OFF run with no observation could just mean
a broken netlog; requiring the control to resolve proves a negative
result is real.

## Regression guard

`--telemetry-browse-smoke` exits non-zero if any endpoint fails to
classify as content/essential, and is listed in
`.devin/check-coverage.sh`'s smoke sweep alongside the original idle
`--telemetry-smoke`.
