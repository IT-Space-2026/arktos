# CEF-INPUT-VERDICT — windowed-embed depth on the reparented chrome view (CS02)

Companion: `CEF-SPIKE.md` (G4 embed shapes), `CEF-BW-VERDICT.md` (CS01
extension runtime). Where CS01 proved the *extension* story lives on the
chrome runtime, CS02 measures the *embed* story of the same shape: a
chrome-style CEF browser reparented into a plain QWidget via
`CefWindowInfo::SetAsChild` — does real X input/focus/geometry behave
like a native view?

## Method

- Harness: `spikes/cef/qcefprobe` extended with `--mode=input`
  (windowed, `CEF_RUNTIME_STYLE_CHROME`, MTML, `--no-sandbox`), fixture
  `input-fixture.html`, runner `run-input.sh`. CEF
  `152.0.15+gb3d72fb` / Chromium `152.0.7977.169` linux64, xvfb
  (`xvfb-run -a`, 1024x768 and 2048x1536 screens), `CEF_OZONE=x11`,
  `QT_QPA_PLATFORM=xcb`.
- Input is **real X-server input**: XTest fake motion/button/key events
  over the probe's own `Display*`, the same event class a physical
  keyboard/mouse produces (no `send_event` shortcuts). Non-latin keys
  arrive by remapping the keyboard map's spare keycode (xdotool's
  `key U+4E2D` trick).
- Readback channel: the fixture mirrors DOM events to `console.log`
  (`PROBEEVT`/`PROBESNAP`) → `CefDisplayHandler::OnConsoleMessage`; the
  probe also posts `__snap(n)` via `CefFrame::ExecuteJavaScript` for
  synchronous state reads (`document.hasFocus()`, `activeElement`,
  input values, `scrollTop`, `devicePixelRatio`, `innerWidth/Height`).
- The host is deliberately realistic: a container QWidget holding a
  sibling `QLineEdit` (Qt focus target) + the `RasterWidget` the CEF
  window is reparented into.

## Leg table (measured 2026-10-25)

| Leg | Verdict | Evidence | Reading |
|---|---|---|---|
| focus-in | **PASS** | XTest click on `#i1` → `hasFocus=1 active=i1`, X focus `0x200003` (the CEF window) | Clicking the reparented view focuses it like a native window — CEF grabs X input focus itself on ButtonPress. |
| focus-out | **PASS** (with nuance) | `qtclick: hasFocus=0` — clicking a Qt sibling **blurs the page**; `XSetInputFocus(PointerRoot): hasFocus=1` (no blur); `host->SetFocus(false): hasFocus=1` (no blur) | Only a real click elsewhere reliably blurs. `SetFocus(false)` and X focus-yank both leave the DOM focused — the embedder cannot force blur via the API on this path. |
| focus-embedder | **PASS** | `host->SetFocus(true)` → `hasFocus=1`, `active=i1` (element restored) | The embedder CAN re-focus the view programmatically — the Qt `focusInEvent` bridge has a working endpoint. |
| key-ascii | **PASS** | XTest `h e l l o` → `i1v='hello'` (keydown+input events fired) | Plain typing reaches the page unmodified. |
| key-nonlatin | **PASS** | `dead_acute+e → 'e'` (dead key swallowed, no compose); `U+4E2D keysym → ''`; `XK_eacute → 'é'` | Direct Unicode/Latin-1 keysyms deliver correct chars. Dead-key compose is **absent** in this env (no XIM/IBus under xvfb) — the key event itself is eaten with no preedit; on a desktop with ibus/fcitx this path is expected to work but is **unverified** — real IME preedit/commit surfaces were not exercised. |
| wheel | **PASS** | 3×button5 over `#scroller` → `scrollTop 0→360` (scroll events fired) | Wheel over the view scrolls normally. |
| tab-cycle | **PASS** (with caveat) | `Tab×6` trail `i2>lnk>b1>scroller>i1>i2`, `hasFocus=1` throughout | Tab advances through in-page focusables and **wraps inside the document — it never escapes** to Qt chrome or browser chrome (`escaped-to-chrome=no`). An embedder that needs Tab-out must implement it (key filter + focus handoff). |
| resize-auto | **FAIL** | host widget `→640x480`, CEF child stayed `820x590` | Nothing propagates Qt resizes to the foreign X child — **the embedder owns child geometry**, same contract as any alien-window embed. |
| resize-embedder | **PASS** | `XMoveResizeWindow(cef,640,480)` + `NotifyMoveOrResizeStarted` → child `640x480`, `innerWidth=640` | The supported path works: resize the child X window, notify CEF, the page viewport follows. |
| hidpi | **FAIL** | `QT_SCALE_FACTOR=2`: host `1640x1180` phys, child `820x590` (naive logical-px `SetAsChild` rect = quadrant coverage), page `dpr=1 iw=820`; after `XMoveResizeWindow` to `1640x1180` → `iw=820 dpr=1` still | Out of the box: child covers a quarter of the host AND the page never adopts a scale factor. Manual resize fixes coverage but not dpr. |
| hidpi-fdsf | **PASS** | same run + `force-device-scale-factor=2`: child `1640x1180` at create (CEF treats `SetAsChild` rect as DIP), `dpr=2 iw=820` | With the switch the chrome runtime does the DIP→physical conversion itself — the naive embed is then *correct*. |

`run-input.sh` prints this table; `legs: 11 pass=9 fail=2`. Both FAILs
are capability gaps with a **verified workaround** measured in the same
run — they are design inputs for the port, not blockers.

## Embedder-contract findings (feeds CS03/CS05)

1. **Geometry is embedder-owned.** `SetAsChild` sets initial bounds only;
   Qt layouts never touch the foreign child. Port must mirror widget
   `resizeEvent` → `XMoveResizeWindow(child)` (+ `NotifyMoveOrResizeStarted`).
2. **HiDPI = one global switch.** `force-device-scale-factor` makes CEF
   interpret `SetAsChild` bounds as DIP and reports `dpr=2`. It is
   process-global — a per-monitor mixed-DPI setup cannot be expressed;
   multi-monitor HiDPI needs the display-scale path Chrome reads from
   Xft/XSETTINGS (unverified under xvfb).
3. **Focus is bridged asymmetrically.** Click-in and `SetFocus(true)`
   work; blur on Qt-focus-loss is not API-driven (`SetFocus(false)` does
   nothing visible on this path — measured, not doc'd). In practice blur
   arrives when the user clicks another window; programmatic blur (tab
   switch, popup grab) may need an X focus event or hiding the window —
   flagged for the port.
4. **Qt focus model never sees the view.** After click-in,
   `qApp->focusWidget()` still reported the Qt sibling — Qt and X focus
   are decoupled for foreign windows; tab-order/focus-chain integration
   is embedder work (findings 3+7).
5. **IME surface:** key injection for non-latin chars works; dead-key
   compose produced nothing (no IM stack under xvfb); real IBus/fcitx
   preedit/commit inside the reparented view is unmeasured — biggest
   residual input risk.
6. **Tab never escapes.** In-page tab order wraps; `escaped-to-chrome=no`.
   If Arktos chrome participates in a Tab chain it must intercept at the
   Qt layer before delivery.
7. Noise: CEF logs `browser_info_manager.cc:858 Timeout of new browser
   info response` around window ops — benign on this config, noted for
   the port's log filtering.
