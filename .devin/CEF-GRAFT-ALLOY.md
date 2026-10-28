# CEF-GRAFT-ALLOY — G1: extension tab model on Alloy surfaces (CS08)

CS06 measured what Alloy-style CEF actually lacks for extensions:
not the popup renderer (that already works) but the **extension tab
model** — `tabs.query`/`windows.*` enumerate nothing, `tab.windowId`
is `-1`, and no `tabs.*`/`windows.*` events fire. CS08's graft wires
the missing delegates. Built on the CS07 source build at pin
`152.0.15+gb3d72fb` / Chromium `152.0.7977.169`.

## Patches (spikes/cef/patches/ + BUILD.md)

| Patch | Target | Size | Contents |
|---|---|---|---|
| `0001-alloy-tab-model-cef.patch` | `cef/` repo | 587 ln | new `libcef/browser/chrome/extensions/alloy_window_controller.{h,cc}` — `cef::AlloyExtensionWindowController` (`extensions::WindowController` impl) + `cef::AlloyBaseWindow` (`ui::BaseWindow` impl), registered in `WindowControllerList`; `AlloyBrowserHostImpl` create/destroy/focus wiring; `cef/BUILD.gn` entry |
| `0002-alloy-tab-model-chromium.patch` | `chromium/src` | 532 ln | `#if BUILDFLAG(ENABLE_CEF)` hunks in `tabs_api.{cc,h}` (`BuildTabList` alloy enumeration + `MatchesAlloyWindow`/`MatchesAlloyTab` + current-window/last-focused fallback for WebContents-less senders; `WindowsGetAll`/`WindowsGetLastFocused`/`TabsGetSelected` alloy paths), `chrome_extension_function_details.cc` (null-safe BWI + alloy current-window fallback), `windows_event_router.cc` (alloy `windows.onCreated/onRemoved/onBoundsChanged`), `tabs_event_router.{h,cc}` (`TrackAlloyTab`/`UntrackAlloyTab`, TabEntry tolerates no `TabInterface`), `tabs_windows_api.cc` (bootstrap tracking for pre-existing alloy tabs when the lazily-created router starts) |

Design: alloy browsers are single-tab, not `BrowserWindowInterface`
objects, so the extension tab/window model can't see them. The graft
registers each `AlloyBrowserHostImpl` in `WindowControllerList` as a
single-tab window, sets a real `SessionTabHelper` window id, and adds
the small alloy-specific hunks the API code needs wherever it
previously required a `BrowserWindowInterface`. Same style as
upstream's own `cef::GetAlloyTabById` graft.

## Measured results (spikes/cef/cs08/, tabprobe fixture)

`run.sh` drives both shapes over CDP; per-leg JSON in
`~/cef-cache/cs08-alloy-{windowed,osr}.json`.

| Leg | Baseline (prebuilt, unpatched) | Patched alloy-windowed | Patched alloy-OSR |
|---|---|---|---|
| `--load-extension` install / SW boot | PASS | PASS | PASS |
| `sender.tab` in cs→sw message | PASS (windowId **-1**) | PASS (windowId real) | PASS (windowId real) |
| `tabs.query({})` | FAIL `[]` | **PASS** | **PASS** |
| `tabs.query({currentWindow,active})` | FAIL `[]` | **PASS** | **PASS** |
| `tabs.get(id)` / `tabs.getCurrent()` (in popup) | PASS / PASS | PASS / PASS | PASS / PASS |
| `scripting.executeScript(tabId)` | PASS | PASS | PASS |
| `tabs.update(tabId,{url})` | PASS | PASS | PASS |
| `windows.getAll/getCurrent/getLastFocused` | FAIL `[]`/`No current window` | **PASS** all | **PASS** all |
| `tabs.onUpdated` event | FAIL (no router tracking) | **PASS** ({updated:2}) | **PASS** ({updated:3}) |
| `chrome-extension://` page render + pixel witness | PASS | **PASS** | **PASS** |

**Verify hint met**: Bitwarden 2026.9.3 popup rendered inside the
patched alloy-OSR view — `popup/index.html` → `#/intro-carousel`,
`app-root` live, witness `~/cef-cache/cs08-bw-popup-alloy-osr.png`
(50KB, visually verified: logo, carousel, Create account/Log in).
tabprobe witness `~/cef-cache/cs08-popup-alloy-osr.png`.

## Runtime caveat learned (not graft-related)

The source build (non-official) enables Chromium's hardened stack
canary for zygote children: `base::ResetStackCanaryIfPossible`
re-randomizes each forked child's TLS canary, but embedder `main()`
is not `NO_STACK_PROTECTOR` — a subprocess returning through `main`'s
epilogue trips `__stack_chk_fail` ("stack smashing detected", ~5 per
run on child exits; browser unaffected). Measured: zero smashes on
the shipped Spotify binary under identical flow; zero on the source
build with `--change-stack-guard-on-fork=disable` (Chromium's own
escape hatch, crbug.com/1206626). The flag is now in `run.sh`'s
runtime flags alongside `UseBoringSSLForRandBytes`; record it in the
CS10 cost column as a non-official-build artifact only.

A second probe-side gotcha: the source build bundles Chromium's
*Contextual Tasks* component extension (`glbjnfim…`, absent from the
prebuilt dist) whose SW also surfaces under `chrome-extension://` —
CDP SW selection must match the fixture's path-derived id
(`expected_ext_id` in probe.py), not "first extension SW".

## Honest remaining gaps (measured, not assumed)

- Alloy windows are single-tab by construction: `tabs.query` returns
  the one tab, `tabs.onMoved/onAttached/onDetached/onActivated`,
  `windows.onCreated` for pre-existing windows, pin/group/lifecycle
  states have no upstream analogue — reported shapes are consistent
  (`index:0`, `active` filtered as true), not runtime bugs.
- `windows.onFocusChanged`/`onBoundsChanged` only fire where an alloy
  equivalent exists (`OnSetFocus` is wired; bounds changes are no-ops —
  no NOTIMPLEMENTED in the query path).
- `getLastFocused` resolves to the most recently `OnSetFocus`'d alloy
  window (per-controller), not a global activation ordering.
- Unchanged dead surfaces (by design, see CEF-ALLOY-EXT.md):
  `chrome://` pages, action toolbar, sidePanel inset; ~~G2 graft
  (nativeMessaging, CS12) still pending~~ CS12 closed G2 as a
  **zero-line graft** — nativeMessaging works unpatched on both
  runtimes (CEF-GRAFT-NATIVEMSG.md); identity/privacy remain
  undeclared-permission absent.

## Cost signal for CS10

~1.1k patch lines total, but ~330 of that is the new CEF-side file —
the chromium-side delta is ~200 lines of mostly mechanical
`IsAlloyWindowController` guards + two query helpers + an events
bootstrap, all behind `ENABLE_CEF` like upstream's own grafts. Rebase
risk concentrates in `tabs_api.cc` (`BuildTabList`, still churning
upstream) and the `WindowController` interface; the rest is
append-only hunks.
