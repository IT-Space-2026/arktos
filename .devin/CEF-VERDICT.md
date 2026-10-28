# CEF-VERDICT — final synthesis: is prebuilt CEF a viable Arktos backend? (CS05)

Date: 2026-10-25 · Lane: `cef-spike` · Build under test throughout:
CEF `152.0.15+gb3d72fb` / Chromium `152.0.7977.169` linux64 (Spotify
prebuilt, chrome bootstrap; Alloy bootstrap removed since M128).

Inputs — all measured this lane, never re-derived:

| Doc | Task | What it proved |
|---|---|---|
| `CEF-SPIKE.md` | RC11 | facade is service-complete for CEF; touchpoint inventory (below); original NO-GO that *assumed* extensions absent (gap G3) |
| `CEF-EXT-SPIKE.md` | — | MV3 fixture + real Bitwarden 2026.10.0 CRX run on the **chrome runtime**: SW + popup + storage + messaging + scripting all live; OSR forces Alloy; Chrome-UI extension surfaces dead there, RequestContext-level functionality may still run (CEF#3859 maintainer) |
| `CEF-BW-VERDICT.md` | CS01 | our bundled BW 2026.9.3 zip verified end-to-end; per-API gap table measured |
| `CEF-INPUT-VERDICT.md` | CS02 | windowed chrome-style `SetAsChild` reparent into QWidget: 9/11 input legs PASS, both FAILs carry verified workarounds |
| `CEF-CAPS.md` | CS03 | the runtime-style split collapses to one Capabilities bit (`extensions`); one `cef` id + shape pinned at `initialize()` fits the contract unchanged |
| `CEF-SANDBOX.md` | CS04 | real Chromium userns sandbox runs (seccomp+pidns verified via /proc); bwrap wrap × CEF sandbox = FATAL in all variants; persona isolation verified, containers need flat keys |

---

## 1. Go / no-go vs the `Engine::Backend` contract

Judged against `ENGINE-ADAPTER.md` area by area. **GO on contract fit —
a `cef` backend can satisfy the adapter contract.** Every area has
either a real CEF surface measured in the probes or an honest false
bit the contract already accommodates (Servo reports a near-empty
table today; honest falses are the designed shape).

| Contract area (ENGINE-ADAPTER §) | CEF surface | Fit |
|---|---|---|
| 1 Identity/lifecycle | `initialize()` = `AcCore` mint + `CefInitialize` + scheme registration; self-registering `addProbe` probe; `pageUnavailableReason`/`engineNote` free surfaces | fits |
| 2 Navigation control | `CefBrowser` load/stop/reload + `CefNavigationEntry` stack (historyItems/goToHistoryEntry parity); zoom via `SetZoomLevel` | fits |
| 3 Load-state/page signals | the full §3 set maps onto `CefLoadHandler`/`CefDisplayHandler`/`CefRequestHandler`/`CefContextMenuHandler`/`CefFindHandler` | fits |
| 4 Profile binding | `CefRequestContext` per persona — isolation + persistence measured (CS04); **caveat**: `cache_path` must be a direct child of one process `root_cache_path` → flat-key indirection, containers can't nest | fits with design constraint |
| 5 Request policy | `CefResourceRequestHandler`/`GetResourceResponseFilter` — full pipeline, *stronger* than Qt's interceptor; `NavigationPolicy` consult maps 1:1 | fits (upgrade) |
| 6 Downloads | `CefDownloadHandler` → `Engine::DownloadRequest` incl. pause/resume | fits |
| 7 Permissions | `CefPermissionHandler` prompt set is **coarser** than `QWebEnginePermission` (G5) — covered types marshal into the neutral `handleRequest` overload | partial, honest |
| 8 Schemes | `CefSchemeHandlerFactory` + `CefResourceHandler` — full parity for arktos-file:/abp:/interstitials, reusing backend-0 serving halves | fits |
| 9 DevTools | CDP over `--remote-debugging-port` **verified** (CS01 drove it = the BiDi panel's protocol); embedded inspect pane absent — `ShowDevTools` is a separate window only | fits with degrade |
| 10 Extensions | **shape-conditional**: full MV3 surface verified on chrome-style windows (CS01); Chrome-UI surfaces (toolbars/popups, sidePanel, chrome://extensions) dead on OSR/Alloy; RequestContext-level functionality *should* work per maintainer — UNVERIFIED here (CEF#3859) | one conditional bit |
| 11 Actions/menus | mapped `StandardAction`s like webengine's; `CefContextMenuHandler` hit-test feeds `ContextMenuInfo` | fits (expected) |
| 12 Scripting | `ExecuteJavaScript` verified live (CS02 `__snap`); the **result channel** and isolated-world/declarative semantics are shim work (G1/G2) | partial, honest |
| 13 Lifecycle/memory | real `renderProcessId`; `lifecycleDiscard` has no CEF equivalent → honest false; OTR measured disk-clean | fits with honest false |

Contract verdict: **the adapter boundary is not the obstacle.** The
caps design is already drafted (CEF-CAPS.md §4, one `EngineRow`, no
schema bump, the split carried by `capabilities().extensions` live +
a `partial`+note facade row).

## 2. Extension fidelity — the deciding driver, measured

The lane existed because MV3 extensions were the only plausible
CEF-unique driver. Measured answer: **CEF's chrome runtime is a real
extension host, not a stub.**

- Fixture MV3: all six probes live (SW, storage, action, CS injection,
  CS↔SW messaging, `scripting.executeScript`).
- Real Bitwarden (fetched 2026.10.0 CRX **and** our bundled 2026.9.3
  zip): SW boots, WASM SDK loads, popup opens and routes interactively,
  `chrome.tabs.create` onboarding fires, cs↔sw echo round-trips.
- Full manifest permission set (16 perms incl. webRequest+
  webRequestAuthProvider, sidePanel, offscreen, clipboard) returns
  real values or correct Chrome-typed errors.

Measured gaps (verbatim from CS01):

| Gap | Status |
|---|---|
| `nativeMessaging` (`connectNative`/`sendNativeMessage`) | **absent** — BW desktop-app/biometric unlock dead on CEF — **CS12 correction**: works unpatched when the extension holds the permission (API hidden while ungranted, upstream gating); see CEF-GRAFT-NATIVEMSG.md |
| `privacy`, `identity` | absent in this config — optional/undeclared perms, unverified not broken |
| `sidePanel` inset rendering in host window | API accepts calls; inset unverified |
| MV2 legacy surface | gone — expected under MV3 |

**The hard condition**: extensions exist only on the chrome-style
windowed embed, which is X11 `SetAsChild` only. **Wayland has no
fully extension-capable CEF shape** — OSR is forced to Alloy
there. Per the maintainer in CEF#3859 (closed *not planned*, Dec
2024): "Extension functionality that works at the RequestContext
level (like network intercepts) should already work with Alloy
style browsers. Extension functionality that requires Chrome UI
(toolbars etc.) will not be supported." So on Wayland the *runtime*
half of an extension (background SW, storage, cs messaging,
webRequest) may still run under Alloy style, but every Chrome-UI
surface (action popup toolbar, sidePanel, chrome://extensions)
is dead by design — a partial story at best, UNVERIFIED here.
Note chrome-extension:// pages are not chrome:// UI — whether an
Alloy OSR view can render the popup inside our own action-bar host
is an open probe, not an assumption either way.

## 3. Integration cost — touchpoints + gap ledger

Inventory (CEF-SPIKE §b, measured 2026-10-22):

| Bucket | Files | Refs | Cost |
|---|---|---|---|
| must-port | 74 | 1176 | real engine-surface work (~100–130 lines are facade-absorbed persona plumbing inside these files) |
| facade-absorbed | 2 | 4 | free — `ac_*` already owns it |
| droppable | 26 | 247 | backend-0 internals replaced wholesale + comments |
| (autotests, excluded) | 52 | 697 | re-author only if WebEngine is *removed* |

Effort (RC11 baseline, updated by this lane's findings):

| Workstream | Est. | Delta from this lane |
|---|---|---|
| `cefbackend` adapter core incl. OSR/widget hosts | 4–7 wk | CS02 moved the windowed shape from unknown to **verified with a named embedder contract** (geometry mirroring, `SetFocus(true)` refocus, `force-device-scale-factor`, Tab-escape interception) — that contract is inside this estimate |
| Consumer un-coupling (74 files / 1176 refs) | 6–10 wk | unchanged — still the dominant cost |
| G1 script-collection shim + G2 bridge replacement | 3–5 wk | unchanged; `ExecuteJavaScript` delivery verified, semantics still to build |
| Extension runtime port | was "research project" → **~3–5 wk** | CS01 demoted it from unknown to a bounded port: Arktos's `src/extensions/*` (~260 refs on `QWebEngineExtensionManager`) needs a second implementation over Chrome's real extension system (load path, SW host, action popup wiring, permission grants). The host exists; our manager doesn't speak it |
| Sandbox decision + profile flat-key mapping | **+1–2 wk** (new row) | CS04: bwrap × CEF-sandbox measured FATAL in all variants — the port must pick one confinement layer or redesign the wrap; persona→ctx mapping needs adapter-owned flat keys under one `root_cache_path` |
| Packaging legs (5) + subprocess/sandbox staging | 3–5 wk | unchanged — ~150–300 MB per platform is new shipping weight |
| **bounded total** | **~20–34 wk** | to parity *including* extensions — on X11 only |

## 4. Risk register (all measured, none assumed)

- **Wayland**: no extension-capable shape exists. On Wayland sessions
  CEF degrades to an OSR engine where only the non-UI half of
  extensions may run (CEF#3859) — honest but the driver
  evaporates there.
- **Sandbox layering**: Chromium's real sandbox (verified: seccomp-BPF
  + nested pidns, zero privileges) cannot nest inside the bwrap
  filesystem wrap — measured FATAL, no degrade path. Choosing it
  un-protects the browser process; choosing the wrap loses renderer
  confinement. Unresolved design fork.
- **IME**: dead-key compose produced nothing under xvfb (no IM stack);
  real IBus/fcitx preedit/commit inside the reparented view is
  **unmeasured** — the largest residual input risk.
- **Focus asymmetry**: `SetFocus(false)` and X focus-yank do not blur
  the DOM (measured); programmatic blur needs a workaround.
- **Silent-transience trap**: non-direct-child `cache_path` is accepted
  and runs silently in-memory — the flat-keyspace rule MUST be enforced
  by the adapter (`GetCachePath()` verifies at runtime; the hook exists).
- ~~**nativeMessaging absent** on CEF's runtime: any feature needing the
  desktop-companion channel dies on the cef backend specifically.~~
  **CS12 correction**: present and functional on both runtimes when the
  extension declares/holds the permission; hidden while ungranted
  (upstream permission gating hid it in CS01's BW measurement). See
  CEF-GRAFT-NATIVEMSG.md.
- **Codecs**: open-codecs only (h264/hevc/aac absent — measured);
  matches QtWebEngine default builds, no regression, proprietary-codec
  parity requires a self-hosted ~100 GB Chromium build.

## 5. Recommendation

**REJECT the port today.** Between the three options:

- **Replace QtWebEngine** — negative delta on day one. CEF is the
  *same* Chromium Arktos already embeds, minus Qt's integration:
  ~20–34 wk of port work buys parity minus QWebChannel bridges,
  declarative `QWebEngineScript`, embedded devtools, Qt-side
  compositing, and permission granularity — plus new shipping weight
  (~150–300 MB) and the bwrap/sandbox fork. Nothing user-visible is
  gained.
- **Third engine** — the only defensible port *shape* (CEF-CAPS.md is
  the design), but no driver justifies it now: it duplicates the
  primary Chromium backend, its one differentiator (extensions) is
  parity with what backend-0 already ships and is X11-conditional, and
  `nativeMessaging` absent means CEF's extension story is not even
  strictly superior. **CS12 correction**: nativeMessaging works
  unpatched on CEF — the "not even strictly superior" clause is wrong;
  CEF's extension story is marginally more complete (the
  X11-conditional qualifier still stands).
- **Reject** — correct: the capability question is answered (viable,
  measured), and the answer to "is it worth doing" is no under current
  conditions.

What changed vs RC11's NO-GO: the extension blocker is **removed**
(G3's "assume extensions=false" was wrong — CS01 verified the full MV3
surface). What did not change: extensions are parity, not a
differentiator, and every other cost line stands.

**Re-entry criteria** — reopen only if one lands:

1. Qt drops/breaks WebEngine (licensing, cadence, API removal) → CEF
   becomes the *replacement* shape; this doc + CEF-SPIKE §b + CEF-CAPS
   are the standing map, ~20–34 wk bounded.
2. A real requirement for chrome.* surface **beyond** Qt's curated
   extension API appears (~~and nativeMessaging isn't it — CEF lacks
   it too~~ **CS12**: nativeMessaging works on CEF — it IS such a
   candidate requirement now) → third-engine shape per CEF-CAPS.md.
3. Chromium-embed maintenance burden inverts (QtWebEngine patches
   become costlier than a CEF adapter).

If reopened, sequence per CEF-SPIKE §e: finish escape-hatch
retirement → `cefbackend` behind `CONFIG+=cef` → G1/G2 shims →
extensions via the chrome runtime — and resolve the CS04 sandbox fork
(accept Chromium-sandbox-only, keep bwrap+`no_sandbox`, or redesign
the wrap) **before** packaging legs start.

*Reminder: none of this touches the Servo or WPE WebKit backends —
CEF only ever contends for the Chromium lane.*
