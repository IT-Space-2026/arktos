# CEF-EXT-SPIKE — does modern CEF's Chrome runtime host MV3 extensions? (CEF01 outcome)

Gate question (AMENDMENT to CEF01): RC11's analysis concluded NO-GO but
explicitly excluded extension research, and extension fidelity is the
only plausible CEF-unique driver. This spike answers, empirically:

> Does `cef_binary`'s chrome runtime actually host MV3 extensions in
> the Bitwarden shape — background service worker, action popup,
> `chrome.storage`, content-script messaging, `chrome.scripting`?

**Verdict: YES — extensions work. The RC11 no-go is overridden on the
extension-fidelity driver.** Per AMENDMENT 2, CEF02–CEF15 remain
`blocked: parked` — any port decision is the user's after reading this.

## Build under test

- CEF `152.0.15+gb3d72fb` / Chromium `152.0.7977.169`, stable channel,
  linux64. Pinning scheme: `cef_binary_<chromium_major>.<branch>.<patch>
  +g<cef_sha>+chromium-<full_version>_<platform>` — leading component is
  the Chromium major, `+g<sha>` pins the exact CEF commit
  (`b3d72fb752b200a48d19db1682500a4b9051afac`, built 2026-10-21).
- Host binary: prebuilt `cefsimple` from the `_client` distribution —
  chrome bootstrap is the only bootstrap shipped since M128 removed
  Alloy (see citations).
- Cache outside the repo: `~/cef-cache/`.

## Method

1. Deterministic MV3 fixture (`~/cef-cache/fixtures/mv3ext`): service
   worker probes `chrome.storage.local` + `chrome.action`, a content
   script probes messaging to the SW, the SW probes
   `chrome.scripting.executeScript`. Each success fires an HTTP beacon
   to a local probe server (`127.0.0.1:8877`) for unambiguous headless
   verification.
2. `cefsimple --use-native --load-extension=<fixture> --user-data-dir=
   <tmp> --remote-debugging-port` under `xvfb-run` (x11 ozone). CDP
   enumerates live targets (service workers, popups, Chrome UI).
3. Same procedure with a real Bitwarden `.crx` (2026.10.0, 24.9 MB):
   MV3, `background.service_worker`, `action.default_popup`, demanding
   permission set (scripting, storage, tabs, webRequest+
   webRequestAuthProvider, sidePanel, offscreen, contextMenus, alarms,
   idle, clipboard, notifications, webNavigation).

## Results — fixture MV3

All six beacons fired:

```
sw_started  storage_ok  action_api_ok  cs_started  msg_ok  scripting_ok
```

= service worker ran, `chrome.storage.local` round-tripped,
`chrome.action` present, content script injected, content-script→SW
messaging worked, `chrome.scripting.executeScript` executed. CDP also
showed a live Chrome browser UI (`chrome://omnibox-popup.top-chrome/`).

## Results — Bitwarden 2026.10.0 (real CRX)

- CDP exposed a live `service_worker` target
  `chrome-extension://<id>/background.js`.
- Its WASM SDK loaded (`WASM SDK loaded in 18ms`), managed-storage read,
  internal background services started.
- The extension opened its onboarding tab itself
  (`chrome.tabs.create` → bitwarden.com/browser-start).
- Loading `chrome-extension://<id>/popup/index.html` directly showed
  the popup app run: routed to its default-password-manager flow,
  `main.js` loaded the WASM SDK and messaged the service worker.

That is the full Bitwarden shape: SW + popup + storage + messaging +
scripting. Not claimed: `sidePanel` chrome surface inside a CEF host
window (CEF Chrome-style windows do show Chrome UI; whether side-panel
insets render there is a CEF08-scale question, unverified).

## Runtime flavor — the constraint that matters

Per `cef_window_info.h` and CEF issue #3685:

- `CefWindowInfo::SetAsWindowless` (OSR) **forces**
  `CEF_RUNTIME_STYLE_ALLOY` — the extension API is **not** supported on
  Alloy windows.
- Windowed `SetAsChild` accepts `CEF_RUNTIME_STYLE_CHROME` — the
  extension-capable shape.
- Since M128 the Alloy *bootstrap* is gone; the whole process always
  runs the Chrome bootstrap — style is per-window.

Consequence for Arktos: an extension-capable CEF backend must embed a
**windowed chrome-style** browser (X11 `winId` reparent — verified
below), while the Wayland-safe OSR path is necessarily extension-less.
Both shapes are real; capability reporting must stay honest about that
split — this is exactly CEF08's scope.

## QWidget render proof (bootstrap, built ahead of descope)

`spikes/cef/qcefprobe/` — standalone qmake probe, not in SUBDIRS.
`./run-render.sh` result on this host, all three legs PASS:

- **OSR** (`QT_QPA_PLATFORM=offscreen`, `windowless_rendering`,
  ozone headless): `OnPaint` BGRA→QImage, 800x600, load http=200, title
  delivered; pixel analysis `non-sentinel=480000/480000 distinct=317
  witness-miss=none`.
- **Windowed + CHROME style** (`xvfb-run`, xcb, `SetAsChild(winId)`):
  reparented into a plain QWidget, root-window grab 800x600,
  `non-sentinel=478601/480000 witness-miss=none`.
- **Windowed + ALLOY style** (advisory): same, `witness-miss=none`.

`libcef_dll_wrapper.a` built from the standard dist with a user-local
cmake 3.31.8 (host has no system cmake; no sudo). Build recipe in
`spikes/cef/README.md`.

### Embedding gotchas verified (CEF02 notes)

- `CefInitialize` on a **fresh profile hangs indefinitely** unless
  `--no-first-run` + `--no-default-browser-check` are appended — the
  Chrome bootstrap runs first-run tasks that block context init.
  `root_cache_path`/`cache_path` themselves are fine.
- Init **before** `QApplication` works (as does after, once first-run
  is suppressed — the original stall diagnosis was muddied by it).
- `multi_threaded_message_loop=1` (CEF's own UI thread): handlers
  fire off-GUI-thread → marshal via `QMetaObject::invokeMethod`
  (QueuedConnection). `external_message_pump` deadlocks
  `CefInitialize` on this host (ContentMainRun exit 28); STML
  (`CefDoMessageLoopWork` pumping) is kept as an env-switchable path.
- `CreateBrowserSync` is UI-thread-only → returned null under MTML;
  `CreateBrowser` (any thread, async) is the correct call.
- CEF re-execs the app binary for `--type=<renderer|gpu-process|
  utility>` — the `CefExecuteProcess` fast-path must precede Qt.

## Codec verdict — open codecs only (verified at runtime)

`canPlayType` probe executed inside the verified browser:

```
h264=no  hevc=no  aac=no  |  vp9=probably  av1=probably
mp3=probably  opus=probably  vorbis=probably
```

Spotify-builds binaries carry no proprietary codecs. **Decision:
open-codecs-only is the baseline** — it matches QtWebEngine's default
builds, so nothing regresses vs what Arktos ships today. A proprietary
codec build is a self-hosted Chromium compile (~100 GB depot_tools +
hours) and belongs — if ever — to CEF14's packaging row. MP3 decodes
(patents expired, bundled).

## Sources

- CEF issue #3685 — Alloy bootstrap removed in M128; *"The Chrome
  extension API is supported with Chrome style browsers/windows only."*
  https://github.com/chromiumembedded/cef/issues/3685
- CEF issue #3450 — Chrome-runtime extension install/management via
  `chrome://extensions` + `--load-extension`; old `CefExtension` API
  removed. https://github.com/chromiumembedded/cef/issues/3450
- `include/cef_window_info.h` (M152) — `SetAsWindowless` ⇒
  `CEF_RUNTIME_STYLE_ALLOY`; `SetAsChild` + `runtime_style` field.
- `libcef/common/resource_util.cc` @ b3d72fb — user-data-dir
  resolution: `root_cache_path` (browser process) vs `--user-data-dir`
  switch (subprocesses) vs `$XDG_CONFIG_HOME/cef_user_data` default.
- Spotify builds index: `https://cef-builds.spotifycdn.com/index.json`.
- CefSharp `RequestContext.LoadExtension` docs (supporting context for
  extension loading + background/popup behavior).

## What's left open for CEF02+ (if un-parked)

- Chrome-style windowed embedding is the only extension-capable shape;
  OSR is extension-less by construction (CEF08 must design honestly).
- `--use-native` / native parenting details under real X11/Qt-chrome —
  the probe proves reparent, not full integration (focus, IME, resize,
  HiDPI: CEF03).
- Extension install UX: `chrome://extensions` inside a chrome-style
  window vs `--load-extension` unpacked vs CRX sideload (CEF08).
- Sandbox: `no_sandbox=1` used everywhere here — real policy is CEF13.
