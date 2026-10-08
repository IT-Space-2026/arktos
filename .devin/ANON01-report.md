# ANON01 — ipduh.com privacy-test audit (Qt 6.12.0 / Chromium 140.0.7339.225)

Measurement and disposition of every item the ipduh.com
`/privacy-test/` page reports against the real Arora build, following
the SEC15/BADSSL01 convention (app-side / engine-side / test-stale).

## Method

`./arora --anon-smoke` (new in `src/main.cpp`, modeled on
`--browseraudit-smoke`) loads `https://ipduh.com/privacy-test/` on the
real application profile (named `arora` profile, CookieJar,
PrivacyRequestInterceptor + adblock, WebView handlers), injects a
DocumentCreation `QWebEngineScript` that arms an RTCPeerConnection
ICE-gather probe, polls until the page's async probes settle (DNS
resolver readouts, header anomaly scan, font/storage checks), then
writes the rendered report sections + direct JS probes to
`$ARORA_ANON_OUT` (default `/tmp/anon-check.json`). The flag hits a live
site so it stays out of check-coverage's SMOKE_FLAGS, same as
`--browseraudit-smoke`.

Runs used `QT_QPA_PLATFORM=offscreen` + `LIBGL_ALWAYS_SOFTWARE=1`
(host iGPU traps `Chrome_InProcGp` under plain offscreen —
environmental, predates this task). Two profiles were measured:

- **default**: fresh `$HOME` — all privacy settings at compiled
  defaults (`/tmp/anon-check.json`)
- **hardened**: same plus `reportUtcTimezone`, `normalizeAcceptLanguage`
  seeded ON (`/tmp/anon-check3.json`) to verify the toggles reach the
  page on a live site

## Results

### PASS / verified-good items (default profile)

| item | observed | disposition |
|------|----------|-------------|
| User-Agent (wire) | `Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/155.0.0.0 Safari/537.36` | **pass** — UA01+UA03 verified live: no `QtWebEngine` token, presented Chrome/155 |
| User-Agent (JS) | `navigator.userAgent` identical to wire | **pass** — no split-identity signal |
| Client hints | `Sec-Ch-Ua: "Google Chrome";v="155", "Chromium";v="155", "Not=A?Brand";v="24"`, platform `"Linux"`, mobile `?0` | **pass** — UA02/UA03 verified live: brands agree with the UA's presented version |
| Accept-Language (wire) | `en-US, en;q=0.9`; `navigator.languages` = `["en-US","en"]` consistent | **pass** — wire and JS agree; on this en-US host the configured list already is `en-US, en` (the normalizeAcceptLanguage toggle covers non-English systems) |
| WebRTC local-IP leak | ICE gather completed, **0 candidates** (no IP literal, no mDNS) | **pass** — `privacy/webrtcIpProtection` (default ON) → `--force-webrtc-ip-handling-policy=disable_non_proxied_udp` verified armed on a live page; LEAK01 holds in situ |
| Header anomalies | `anmls_plchldr` section empty — the site detected none | **pass** — no `Via`/`X-Forwarded-For`/proxy headers; request headers are plain-Chrome shape (Accept, Accept-Encoding, Accept-Language, Sec-Fetch-*, Upgrade-Insecure-Requests) |
| `navigator.webdriver` | `false` | **pass** — no automation signal |
| Cookie/local/session storage | accepted + readable | **expected** — normal browsing state; the OTR private profile and the Tor window are the isolation answer |
| doa24 JS/UA consistency | site's own script threw (`null.addEventListener`) before populating the table; direct probes show UA/navigator.platform/languages all mutually consistent | **pass-by-probe** — no cross-signal mismatch; site-side script failure is the page's own bug under offscreen, not a leak |

### Flagged items with dispositions

| item | observed (default) | observed (hardened) | disposition |
|------|--------------------|---------------------|-------------|
| Timezone disclosure | `Europe/Budapest`, `getTimezoneOffset()=-120` | `UTC`, `0` | **app-side: mitigated by existing opt-in** — `privacy/reportUtcTimezone` verified live end-to-end. Kept opt-in (not defaulted on) per PRIV02's design: forcing UTC breaks sites' local-time display for ordinary users. README documents the bound |
| Caching-DNS-server visibility | resolvers exposed: `172.68.224.115`, `2400:cb00:123:1024::ac44:e073` (Cloudflare egress), `213.163.69.200` (ISP-side) | same class | **documented bound** — with `secureDnsMode=0` (default) lookups follow the system resolver, which is what the site's unique-subdomain trick reports. Covered by `privacy/secureDnsMode` (automatic / custom+fallback / strict DoH, DOH01) or the Tor window (SOCKS5 tunnels DNS). README already says this plainly |
| Font enumeration | "Guessed 53 of your system fonts / 462" | same | **engine-side — no API** — the page measures text-metric differences across `font-family` fallbacks; QtWebEngine exposes no font-visibility restriction and Chromium has no switch that survives (FontSrcLocalMatching only covers `local()` src). Mitigation would need renderer-level caps — not reachable |
| `navigator.hardwareConcurrency` | `12` (real core count) | same | **engine-side — no API** — QtWebEngine exposes no navigator-property override; script-injection spoofing is itself detectable (getter `toString`, worker inconsistency, timing) and was rejected as a fake fix. Tor-window/normalized-UA story documented |
| `navigator.deviceMemory` | `8` | same | **engine-side — no API** — same reasoning; Chromium clamps to {0.25,0.5,1,2,4,8} already |
| `navigator.platform` | `Linux x86_64` | same | **consistent-by-design** — matches the UA/Sec-CH-UA-Platform triad; spoofing it would *create* a mismatch signal. No Qt API regardless |
| `navigator.plugins`/`mimeTypes` | 5 / 2 | same | **engine-side** — Chromium's built-in PDF/viewer registrations; no API to strip |
| Screen geometry | `screen` 800×800, dpr 1 | same | **environment artifact** — offscreen QPA surface size; on a real display it reports the real screen (engine-side surface either way, no API) |
| Public IP / ASN / geolocation | `78.92.202.162`, AS5483 Magyar Telekom, city-level geoloc | same | **inherent, not a browser bug** — the exit IP is the connection endpoint; only a proxy/VPN/Tor changes it. TOR02's Tor window is the in-app answer |
| Reverse-DNS hostname | `4E5CCAA2.dsl.pool.telekom.hu` | same | **inherent** — PTR of the exit IP; same answer |
| `doNotTrack` | `null` | same | **engine-side** — Chromium removed DNT entirely (no header sent); nothing to restore |

## Summary

Every item the page displays was captured and classified. The results
land in three buckets:

1. **Verified good on a live site** — UA/client-hints consistency
   (UA01–UA03), WebRTC lock-down (LEAK01), no header anomalies. The
   prior hardening work survives contact with a real check page.
2. **Covered by existing app-side toggles** — timezone
   (`reportUtcTimezone`, verified flipping JS reads to UTC on the live
   site), language list (`normalizeAcceptLanguage`), DNS
   (`secureDnsMode` modes or the Tor window). These stay opt-in per
   their tasks' deliberate compat decisions; no new code was needed.
3. **Engine-side with named missing APIs** — font enumeration (no
   font-visibility API), hardwareConcurrency/deviceMemory/plugins
   (no navigator-property override; injection spoofing is detectable
   and was rejected), screen geometry (no API), and the exit-IP
   disclosures that are network-inherent rather than browser behavior.

No unexplained flags. No app-side defects found — the check's flags
are either already-mitigated-by-toggle or documented engine bounds, all
of which are already spelled out in README's privacy section.

## Notes

- The site needs a refresh to fully populate some sections on a first
  visit; the poll waits for DNS readouts + header scan + font/storage
  probes and then extracts — settled both runs (`dns=3`).
- The page's own `doa()` script errors under offscreen
  (`null.getExtension` — WebGL blocklisted, `null.addEventListener`),
  so its system-info table renders only the header. Direct probes in
  the harness capture the same values.
- First hardened attempt seeded `$XDG_CONFIG_HOME/Arora/Arora.conf`
  but the app stores settings under `~/.qttest/` test-mode paths —
  capture showed all `null`. Re-seeded there; hardened run shows the
  toggles latched (`"reportUtcTimezone": "true"` etc.) and timezone
  flipped to UTC.
