# WEBKIT-TOR — SOCKS5/Tor on WPE: proxy surface, leak vectors, test method (research only)

Date: 2026-10-18 · Status: study, no code · Feeds the webkit spine's
WK21 "(a) SOCKS5 leak audit" leg — WK21's job is to *execute* the
checklist in §(c) on the resolved WPE build and record verdicts in
`.devin/WEBKIT-SPIKE.md`. Companion to `.devin/WEBKIT-STUDY.md` (the
"proxy / SOCKS5" capability row) and `.devin/WEBKIT-EXTENSIONS.md`.

Facts below were checked against the published 2.54.1 (latest stable,
Oct 2026) WebKitGTK API reference — same API surface WPE exposes under
`wpe-webkit-2.0` — plus upstream commits/bugs cited inline. Anything
marked *(measure)* is build-dependent and only the checklist answers it.

## (a) The proxy surface — `WebKitNetworkProxySettings`

### API shape and version floor

```c
WebKitNetworkProxySettings *s =
    webkit_network_proxy_settings_new("socks://127.0.0.1:9050", NULL);
webkit_network_session_set_proxy_settings(session,
    WEBKIT_NETWORK_PROXY_MODE_CUSTOM, s);
webkit_network_proxy_settings_free(s);
```

- `WebKitNetworkProxySettings` exists since **2.16**
  (`webkit_network_proxy_settings_new(default_proxy_uri, ignore_hosts)`,
  `add_proxy_for_scheme()`, `copy`, `free`).
- **Since 2.40** the settings attach to `WebKitNetworkSession` via
  `webkit_network_session_set_proxy_settings(session, mode, settings)`.
  Pre-2.40 they attached to `WebKitWebContext`
  (`webkit_web_context_set_proxy_settings`, now deprecated). Our floor
  is WPE 2.54 (per WEBKIT-STUDY), so only the session API matters.
- `WebKitNetworkProxyMode`: `DEFAULT` (=`g_proxy_resolver_get_default()`
  — reads env vars `http_proxy`/`all_proxy`/… and desktop proxy
  settings, including GNOME "automatic" PAC/WPAD), `NO_PROXY`,
  `CUSTOM` (our `WebKitNetworkProxySettings`).

### socks:// semantics — the one line that matters

Per the reference docs (and identical wording in `GSimpleProxyResolver`
whose parser this is): a `default_proxy_uri` starting with `socks://`
"will be treated as referring to all three of the socks5, socks4a, and
socks4 proxy types" — GIO tries **socks5 first**.

DNS resolution side: GIO's `GSocks5Proxy` implements
`GProxy::supports_hostname = TRUE`, meaning the resolver hands the
*hostname* to the proxy and the SOCKS5 request goes out with
`ATYP=DOMAINNAME` — **remote DNS at the proxy** (the `socks5h` behavior
in curl terms). SOCKS4 plain resolves locally; `socks4a`/socks5 resolve
remotely — which is why `socks://` ordering (5→4a→4) is the right form
to send. *(measure: confirm ATYP=domainname on the wire — checklist C1.)*

### Scoping — this is the good news for per-profile Tor

Proxy settings live on the **network session**, and a
`WebKitNetworkSession` is created per `WebKitWebsiteDataManager` —
i.e. **per profile** in our WK11 layout (`profiles/<id>/WebKit` storage
→ its own data manager → its own network session → its own proxy).
OTR gets `webkit_network_session_new_ephemeral()`, which takes the same
proxy call. So **per-profile Tor — the Chromium-isolation shape — is
architecturally supported**: Tor profile gets a CUSTOM socks:// session,
normal profiles get NO_PROXY/DEFAULT sessions, all in one process.

What is *not* available: **per-tab or per-view proxying**. Views sharing
a network session share the proxy. A "Tor tab next to normal tabs in
one profile" would need either separate (possibly ephemeral) sessions
per Tor tab — viable but a session-per-tab cost — or the Tor-mode
profile split we already use (`torTitleBarTitle`/`setTorMode` window
model maps cleanly: a Tor window = a Tor-bound profile/network session).

### Traps in the surface itself

1. **`ignore_hosts` is a direct-connect list.** Entries (hostname /
   `*.domain` / IP / IP:port / CIDR) bypass the proxy entirely. For Tor
   mode it must be `NULL` — every bypass is a leak by definition.
   *(The fixture uses it as the positive control.)*
2. **Never use `DEFAULT` mode for a Tor context.** DEFAULT defers to
   `g_proxy_resolver_get_default()` → env vars + GNOME settings +
   auto-detect (PAC/WPAD fetch over the local net = metadata leak).
   Tor mode is `CUSTOM` or nothing.
3. **Timing**: the docs say settings apply to "connections started in
   session" — keep-alive connections already open aren't migrated.
   Set proxy settings before the session's first load; don't toggle on
   a live session and call it a mode switch.
4. **`add_proxy_for_scheme()`** exists for scheme-specific proxies —
   irrelevant for Tor (we want one default proxy covering everything),
   listed so nobody reaches for it.
5. **Auth**: `socks://user:pass@host:port` userinfo maps to SOCKS5
   username/password auth (method 0x02) — Tor needs none; noted for the
   generic-proxy use case.

## (b) Leak vectors — enumerate, and how to test each

Everything below hinges on one architectural fact: in modern
WebKit2/WPE, **all HTTP(S)/WS(S) fetches run through `SoupNetworkSession`
in the NetworkProcess**, and the proxy settings configure the
`GProxyResolver` on *that* session. Subresources, fetch/XHR, WebSocket,
favicons, downloads, service-worker traffic, and (since 2019) media
fragments are all inside it. The leak vectors are therefore exactly the
channels that **don't** traverse `SoupNetworkSession`:

| # | Vector | Why it can leak | Expected status | Test |
|---|--------|-----------------|-----------------|------|
| L1 | DNS for proxied connections | If anything resolves the hostname locally before connecting (the socks4 trap), the ISP resolver sees the target | **Expected clean** — GIO socks5 sends ATYP=domainname | C1: fixture logs ATYP on the CONNECT; `.leakcheck.test` only resolves *at* the proxy |
| L2 | **DNS prefetch** | `<link rel=dns-prefetch>`/speculative resolution does `getaddrinfo` in-process — outside `SoupNetworkSession`, outside the proxy | **Leaks unless disabled** — `WebKitSettings:enable-dns-prefetching` (default TRUE) must be FALSE on a Tor profile | C3: prefetch-positive page + resolver-shim log |
| L3 | **WebRTC ICE/STUN/TURN** | WPE ships GstWebRTC. Since WPE **2.52** the ICE agent moved into the NetworkProcess (librice via `USE_LIBRICE`; libnice fallback via `WEBKIT_GST_DISABLE_WEBRTC_NETWORK_SANDBOX=1`), and upstream PR #55978 anonymizes host candidates to mDNS in SDP — **but ICE is UDP and GIO socks5 has no UDP ASSOCIATE**: STUN/TURN packets can only leave directly. mDNS hides the IP *in the SDP*, not the STUN traffic *on the wire* | **Structural bypass — must be disabled for Tor.** `WebKitSettings:enable-webrtc=FALSE` (since 2.38, default TRUE; setting it also gates media-stream) | C5: STUN fixture counts datagrams; C6: `typeof RTCPeerConnection` after disable |
| L4 | GIO proxy auto-detect | `DEFAULT` mode may fetch PAC/WPAD or follow desktop config | **Avoided by construction** — CUSTOM only | C12 (control): DEFAULT+`ALL_PROXY` env behaves same as CUSTOM; proves env path exists |
| L5 | Media / HLS / DASH | Historical **CVE-2019-11070**: `souphttpsrc` fetched fragments in the WebProcess ignoring proxy settings. Fixed by r243197 — `webkitwebsrc` routes *all* WebKit media loads through `SoupNetworkSession` | **Expected clean** on 2.54 | C8: `<video>`/`adaptivedemux` source → CONNECT in socks log |
| L6 | GIO connectivity / captive-portal probes | `GNetworkMonitor` `can_reach`-style checks and GNOME connectivity polling happen outside the proxy context; libsoup itself only reads `network-available` (no request) | **Expected low risk** — but it's an *aux-request* class the checklist sweeps for | C10: catch-all — any non-fixture destination in any log = finding |
| L7 | HTTP/3 / QUIC (Alt-Svc) | libsoup3 h3 needs nghttp3 (rare in distro builds); QUIC is UDP → cannot traverse GIO socks5 → either fail-closed (fallback to TCP, safe) or a direct-UDP attempt (leak) | *(measure)* build-dependent | C14: Alt-Svc h3 header → resolver-shim must show no UDP/443 socket |
| L8 | WebTransport | Same UDP problem as QUIC; WebKit support is experimental | *(measure)* likely absent in WPE 2.54 | C13: `typeof WebTransport` probe |
| L9 | Geolocation (Geoclue) | `WebKitGeolocationManager` delegates to the *system* location provider — Geoclue does its own network I/O off-proxy | **Contained by policy** — Tor profile denies geolocation `WebKitPermissionRequest` (WK19's permission broker) | C10/catch-all + deny-by-default wiring |
| L10 | `<a ping>` hyperlink auditing | Goes through NetworkProcess → proxied, but pings are a privacy cost regardless | Off for Tor: `enable-hyperlink-auditing=FALSE` | C9 subresource sweep covers it |
| L11 | TLS aux fetches (OCSP/CRL) | GLib TLS backend does not fetch revocation online by default | Expected clean | C10 catch-all |
| L12 | Service worker / push / favicon / download | All inside `SoupNetworkSession` | Expected clean | C7/C9 subresource coverage |

**Tor-profile settings baseline** (what WK21 should set before any
verdict): `enable-webrtc=FALSE`, `enable-dns-prefetching=FALSE`,
`enable-hyperlink-auditing=FALSE`, geolocation permission denied,
proxy `CUSTOM socks://` with `ignore_hosts=NULL`.

## (c) Test methodology — the fixture WK21 runs

Everything is loopback-only and rootless. The design trick: **`*.leakcheck.test`
is non-resolvable by construction** (`.test` is reserved; nothing local
answers it), so a successful load *proves* the SOCKS proxy did remote
DNS, and the SOCKS fixture itself decides where the connection lands.

### Fixture components (4 pieces, all in one process tree OK)

1. **`tord-socks.py`** — ~120-line asyncio SOCKS5 server on
   `127.0.0.1:10808`. Per CONNECT logs `version atyp host port` to
   `socks.log`. If `host` ends in `.leakcheck.test` → relay to the HTTP
   fixture at `127.0.0.1:18080`; else reject (rep=host-unreachable).
   The log line's `atyp` field is the L1 verdict: `0x03` domainname =
   remote DNS; `0x01/0x04` = client resolved locally = L1 FAIL.
2. **`tord-http.py`** — HTTP server on `127.0.0.1:18080` serving the
   probe page tree (below) and a WS echo; logs every request line to
   `http.log`. Any `http.log` hit for a host with **no preceding
   `socks.log` CONNECT** = direct-connection leak (L-anything FAIL).
3. **`tord-stun.py`** — UDP listener on `127.0.0.1:13478` logging every
   datagram to `stun.log`. The probe page runs
   `new RTCPeerConnection({iceServers:[{urls:'stun:127.0.0.1:13478'}]})`.
   Any datagram = L3 direct-UDP confirmation.
4. **`resolver-log.c`** — ~60-line `LD_PRELOAD` shim wrapping
   `getaddrinfo`, `gethostbyname*`, and `socket(AF_INET*, SOCK_DGRAM,…)`:
   appends `pid comm call arg` to `$TORD_RESOLVER_LOG`. Run the WPE
   harness with `LD_PRELOAD=resolver-log.so` — catches DNS prefetch
   (L2), any UDP socket creation (L3/L7/mDNS), in *every* WebKit
   process. Distinguish by `comm=` (NetworkProcess vs WebProcess).

### Probe page (`http://index.leakcheck.test/`)

Pulls, on load, from distinct sibling hosts so each appears as its own
socks CONNECT: `<img src=//img.leakcheck.test>`, `<script
src=//js.leakcheck.test>`, `<link css=//css.leakcheck.test>`,
`fetch('//api.leakcheck.test')`, `new XMLHttpRequest`,
`new WebSocket('ws://ws.leakcheck.test')`, `<video
src=//media.leakcheck.test/v.mp4>`, `<a ping=//ping.leakcheck.test>`,
`<link rel=dns-prefetch href=//pf.leakcheck.test>`, the ICE gather
above, and `Alt-Svc: h3=":443"` on responses (L7 probe).

### Assertion model

- **socks.log** = what the proxy was asked for. Required: one CONNECT
  per page host, all `atyp=domainname`.
- **http.log** = what actually arrived. Required: every hit preceded by
  a socks CONNECT (the socks fixture is the *only* path in).
- **resolver-log** = what the libc resolver saw / what UDP was opened.
  Required: zero `getaddrinfo` for any `*.leakcheck.test`, zero UDP
  sockets (beyond the fixture's own) during load+idle.
- **stun.log** = ICE reality. Required for Tor verdict: zero datagrams
  — achievable only with `enable-webrtc=FALSE` (C6 confirms the knob
  took: `typeof RTCPeerConnection === 'undefined'` in-page).
- **Positive controls** (must FAIL, proving the fixtures see both
  paths): C11 rerun with `ignore_hosts={"leakcheck.test"}` → http.log
  hits with no socks CONNECT; C3b rerun with prefetch left ON →
  resolver-log shows the `pf.*` lookup. If the positive controls don't
  trip, the checklist proves nothing.

### WK21 executable checklist

Run on the resolved WPE build via the WK01 spike harness (or a minimal
GLib loader) with `LD_PRELOAD=resolver-log.so`, session = CUSTOM
`socks://127.0.0.1:10808`, `ignore_hosts=NULL`, Tor settings baseline
applied. "PASS" criteria are verbatim; each row → one verdict line in
`.devin/WEBKIT-SPIKE.md`.

| # | Step | PASS iff |
|---|------|----------|
| C1 | Load `http://index.leakcheck.test` | Page renders AND socks.log shows `CONNECT index.leakcheck.test:80 atyp=domainname` |
| C2 | Full load's subresources | socks.log has domainname CONNECTs for img/js/css/api/ws/ping hosts; http.log hits all preceded |
| C3 | `<link rel=dns-prefetch>` leg (prefetch OFF) | No `pf.leakcheck.test` in resolver-log |
| C3b | Positive control: prefetch ON | `pf.leakcheck.test` appears in resolver-log (else fixture broken) |
| C4 | Idle 30s post-load | No new entries in any log |
| C5 | ICE leg (webrtc OFF) | stun.log empty; resolver-log shows no non-fixture UDP socket |
| C6 | In-page `typeof RTCPeerConnection` | `'undefined'` under `enable-webrtc=FALSE` (knob verified, not assumed) |
| C6b | Positive control: webrtc ON | ICE datagrams reach stun.log — records the true upstream behavior of the resolved build (librice vs libnice; mDNS candidates vs literals in SDP) |
| C7 | WebSocket leg `ws://ws.leakcheck.test` | `CONNECT ws.leakcheck.test` in socks.log; echo works |
| C8 | `<video>` leg incl. one byte-range request | `CONNECT media.leakcheck.test` in socks.log; no direct http.log hit (L5 regression check) |
| C9 | `<a ping>` leg | CONNECT in socks.log (proxied) — and confirms why we set the pref off |
| C10 | Catch-all sweep | Union of all logs contains only fixture destinations — flags GIO connectivity / Geoclue / anything unlisted |
| C11 | Positive control: `ignore_hosts={"leakcheck.test"}` | http.log hits with NO matching socks CONNECT — proves direct-path detection works |
| C12 | Control: mode=DEFAULT + `ALL_PROXY=socks://127.0.0.1:10808` env | Same coverage as CUSTOM — documents the env path; NOT for shipping |
| C13 | `typeof WebTransport`, `typeof WebSocket` probes | WebTransport undefined OR marked (measure) if present → must be OFF for Tor |
| C14 | Alt-Svc h3 offered; force reload | resolver-log shows no UDP/443 socket from NetworkProcess (else L7 finding) |
| C15 | Two sessions: A=CUSTOM socks, B=NO_PROXY, same page | A through fixture, B direct — proves per-profile Tor scoping (the Chromium-equivalent claim) |

Preamble probes on the resolved build (feeds C-interpretation): WPE
version string; `gst-inspect-1.0 webrtcbin` present?; `strings
libWPEWebKit | grep WEBKIT_GST_DISABLE_WEBRTC_NETWORK_SANDBOX` (librice
build indicator); libsoup HTTP/3 support (`soup_session` h3 props /
build strings).

## (d) Verdict — is Tor-on-WebKit a candidate?

**Conditional candidate — not documented out, not cleared.** The honest
split:

- **In favor (verified by API shape, pending wire confirmation):**
  per-`WebKitNetworkSession` proxying gives real per-profile Tor scope;
  `socks://` via GIO does remote DNS (ATYP=domainname) by design; every
  HTTP(S)/WS(S)/media/download path is inside the proxied
  `SoupNetworkSession` — including the media path that was itself a
  CVE-class bypass until 2019 and is now fixed.
- **The deciding risks are exactly two**, both with named mitigations:
  (i) WebRTC — UDP ICE cannot traverse the proxy, upstream's mDNS SDP
  anonymization doesn't stop on-wire STUN, so `enable-webrtc=FALSE` on
  the Tor profile is *mandatory* and C5/C6 must prove the knob actually
  removes `RTCPeerConnection` (if a build exposes ICE despite the pref,
  that's a veto); (ii) DNS prefetch — `enable-dns-prefetching=FALSE`
  plus C3 wire proof.
- **What would veto it:** any C-row failing without a settings fix —
  specifically direct-UDP ICE traffic surviving `enable-webrtc=FALSE`,
  `getaddrinfo` firing for page hosts with prefetch disabled, or QUIC
  UDP attempts under a proxied session.

WK19's Tor lock stays exactly as specified — WebKit is not offered for
Tor tabs — until WK21's checklist comes back clean enough that a
"webkit allowed for Tor" capability is a measured fact rather than an
API-shape argument. Tor remains Chromium-locked in the interim.
