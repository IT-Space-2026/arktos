# EXT13 — WebExtensions compatibility spike

Verdict: **Arch A — works** (hidden `QWebEngineView` hosting extension pages on the
extension's own profile; real `chrome.*`, native messaging, no package rewrite).
**Arch B — works for source compatibility only** (`deno_core` and `boa` both
evaluate a real 3.4 MB MV3 bundle end-to-end under a shim; `rquickjs` and `mozjs`
fail on it). A Rust JS host is viable as an *analysis/measurement* harness, not as
a practical browser extension runtime — every `chrome.*`/DOM/SW API would have to
be bridged natively.

Extensions therefore do **not** have to remain a hard Qt-only subset — but the
workable architecture keeps execution inside QtWebEngine (Chromium) and uses the
Rust lane for the pieces it is good at: package staging, the gapfill shim, and the
compat-surface measurements captured below.

## Architecture A — hidden QWebEngineView host

Harness: `spikes/ext/hostview/` (standalone qmake app, not wired into the
product build). Fixture: `spikes/ext/fixture-mv3/` — a real MV3 extension
(service worker, popup, content script, host page). Real extension: the vendored
Bitwarden 2026.9.3 package, staged into a temp dir with a 3-file bootstrap
(`shim-mode.js`, `chrome-shim.js`, `host-boot.js`). Full run log:
`spikes/ext/results/hostview-run.log`.

### Probe matrix

| Probe | Result | Detail |
|---|---|---|
| MV3 ext loads on named profile | PASS | `QWebEngineExtensionManager::loadUnpacked` + `setAllowed` |
| `chrome-extension://` page on own profile | PASS | `popup.html` loads, `chrome.runtime` real |
| Preload script on `chrome-extension://` | PASS | Profile `QWebEngineScript` (DocumentCreation, MainWorld) runs — `inj:42` reached the page |
| MV3 service worker runs | PASS | `sendMessage` answered `{"pong":true,"from":"real-sw"}` before any host page existed |
| `chrome-extension://` on a *foreign* profile | expected refuse | `loadOk=0` — ext pages must live on the installing profile |
| Override `chrome-extension` scheme handler | FAIL | `Cannot install a URL scheme handler overriding internal scheme: chrome-extension` — Qt owns the internal scheme; **not** an architectural blocker (ext pages load natively, no interception needed) |
| Hidden view hosts background script | PASS | `sw.js` ran inside a hidden hosted page via script-tag append (`__swRan = "hosted-page"`) |
| `eval()`/string compilation | CSP-blocked | MV3 CSP has no `unsafe-eval` — script-tag append is the correct mechanism, eval is not |
| `chrome.runtime.onMessage` in hosted page | PASS | real API, registered natively |
| `chrome.storage.local` in hosted page | PASS | real API |
| Popup → hosted page messaging | PASS | `{"pong":true,"from":"hosted-page","via":"popup2"}` — native routing, no relay needed |
| Content script → hosted page | PASS | `reply:hosted-page` |
| `fetch` + storage round trip | PASS | `storage:1,fetch:200` against a local loopback server |
| Hidden-view aliveness | PASS (throttled) | 2 timer ticks / 1.2 s — background timers are throttled but the page stays loaded and answers `runJavaScript` |
| **No-rewrite bootstrap** | PASS | A DocumentCreation script on the *unmodified* packaged `popup.html` appends `sw.js` (DOM append deferred to DOMContentLoaded — `documentElement` is null at creation time). Install-time package rewriting is **not** required → no conflict with SEC25 integrity pinning |
| Real Bitwarden `background.js` in hosted page | PASS | All bootstrap scripts loaded (`all-loaded`), `__report()` returned 1201 bytes, **0 uncaught errors** |
| Hosted page → hosted page messaging | no reply | `runtime.sendMessage` routes to the service-worker context, not between hosted extension pages — needs a QWebChannel/relay if an extension depends on it |

### Bitwarden compat surface under Arch A (gapfill mode)

The hosted page already had real `chrome.runtime`/`chrome.storage`/`chrome.i18n`,
so `chrome-shim.js` ran in `gapfill` mode — only the **missing** namespaces were
stubbed. 27 apiLog entries (`results/bw-host-report.json`) = the exact delta a
production bridge must provide:

`tabs.getCurrent`, `notifications.onButtonClicked/onClicked`,
`permissions.onAdded/contains`, `webNavigation.onCommitted/onErrorOccurred`,
`tabs.onActivated/onUpdated/onRemoved/onCreated`, `tabs.query`,
`commands.onCommand`, `contextMenus.onClicked`, `sidePanel.setOptions`.

`importScripts` does not exist in page scope — a one-line stub
(`self.importScripts = u => append <script src>`) was enough for Bitwarden's
webpack chunk loading to proceed.

## Architecture B — Rust-native JS host

Hosts: `spikes/ext/engines/{mozjs,deno,qjs,boa}-host/` sharing
`spikes/ext/engines/driver.rs` — evaluates `sw-globals.js` + `chrome-shim.js`
(full mode), then the payload, fires a `runtime.onMessage`, drains manual timers,
emits `__report()`. `EXT13_PARSE_ONLY=1` compiles without executing.

### Round-trip demo (all four engines, fixture `sw.js`)

Identical on every engine — shim installs, payload registers `onInstalled` +
`onMessage`, post-drain response:
`[{"pong":true,"from":"real-sw","via":"rust-host"}]` — one full `chrome.*`
round trip end-to-end. Captures: `results/fixture-{boa,qjs,deno,mozjs}.txt`.

### Real Bitwarden `background.js` (3,411,665 bytes, webpack MV3 bundle)

| Engine | Result | Detail |
|---|---|---|
| `deno_core` (V8) | **works** | eval OK, 402 timers drained, 66 apiLog entries, 7 `onMessage` listeners, 98 errors (dominated by "executing a cancelled action" timer noise + expected network/config stub failures — `api.bitwarden.com` unreachable, managed config absent). `results/bw-deno-report.txt` |
| `boa` | **works** | eval OK, 500 timers drained (hit the drain cap — a `permissions.contains` polling loop), 39 apiLog entries, 1 `onMessage` listener, 0 errors. `results/bw-boa-report.txt` |
| `rquickjs` (QuickJS) | **blocked by ES fidelity** | `InternalError: stack underflow (op=112, pc=145)` inside eval — QuickJS cannot run this production bundle at all. `results/bw-qjs-report.txt` |
| `mozjs` (SpiderMonkey, via servo) | **blocked by engine crash** | Full bundle *parses* cleanly (`parseOnly ok:true`) but execution SIGSEGVs (rc=139) — a native fault JS `try/catch` cannot contain. `results/bw-mozjs-{parseonly,exec-crash}.txt` |

### ES fidelity / build weight / bridge trade-offs

| Engine | Build weight | Fidelity on real bundle | Notes |
|---|---|---|---|
| deno_core | ~941 MB target, prebuilt V8 lib | best — full V8 + real async ops/event loop | heavy dep tree (needed a `timezone_provider` pin to build); in-process; a production version must bridge every `chrome.*` call out to the browser |
| boa | ~459 MB, pure Rust stable | good — completed the bundle | no native async I/O; the shim's manual timer queue carries async work; slowest but simplest to embed |
| rquickjs | ~59 MB | insufficient on real bundle | fine on the fixture; smallest and fastest to build |
| mozjs | shared servo-embed target (~GBs, longest build) | parse OK, execution crash | uncatchable → cannot ship as-is; already in-tree transitively but that does not make it usable |

Measured compat surface under a bare engine is *source compatibility* — the shim
records touches, it does not provide real behavior. e.g. `webRequest`,
`notifications`, `sidePanel`, `idle`, `browserAction`, `storage.managed`,
`alarms` were all touched by Bitwarden startup; a real Rust host would have to
implement or relay each one, plus the DOM/SW stubs (`fetch`, `caches`,
`indexedDB`, `Worker`, `DOMParser`, `TextEncoder`…). Deno/boa completing startup
proves the *JavaScript* runs; it does not prove the extension *works*.

## Recommendation

1. **Ship Arch A.** Hidden extension-page hosting on the extension profile works
   today: real `chrome.*`, native messaging from popup and content scripts, CSP
   respected, timers throttled but alive, and — critically — the profile script
   collection injects into `chrome-extension://` pages **without rewriting the
   package**, so SEC25 integrity pinning is unaffected.
2. Use `chrome-shim.js` in `gapfill` mode at DocumentCreation; the staged
   bootstrap needs only `shim-mode.js` → `chrome-shim.js` → payload, plus an
   `importScripts` stub. The measured gap delta is the 27-entry list above.
3. **Arch B is a measurement tool, not a runtime.** Keep the `deno_core` host
   (best fidelity) for offline compat profiling of candidate extensions; `boa`
   as the lightweight embeddable alternative. Do not invest in a Rust→Qt API
   bridge unless there is a use case for running extension JS with no page at
   all — the bridging surface is enormous.
4. Known limitations to track: `chrome-extension` scheme-handler refusal
   (harmless), hosted-page↔hosted-page messaging (relay needed if required),
   background timer throttling (use `chrome.alarms`-style wakeup or a keepalive
   if an extension misbehaves), and per-extension gapfill lists.

## Reproduce

```sh
# Arch A (Qt 6.12):
source .devin/qt-env.sh
cd spikes/ext/hostview && qmake && make -j2
QT_QPA_PLATFORM=offscreen ./hostview
# Arch B (user-local cargo):
cd spikes/ext/engines/deno-host && cargo build --release
./target/release/deno-host --script ../../fixture-mv3/sw.js --label fixture
unzip -o src/data/extensions/bitwarden-2026.9.3.zip -d spikes/ext/fixture-real
./target/release/deno-host --script ../../fixture-real/background.js --label bitwarden-bg
```
