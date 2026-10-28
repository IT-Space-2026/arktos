# CS08 graft patches — Alloy tab-model for extensions

Two patches against the CS07 source build
(`~/cef-src/chromium/src`, pin `152.0.15+gb3d72fb` /
Chromium `152.0.7977.169`):

- `0001-alloy-tab-model-cef.patch` — applies to the `cef/` repo
  (`~/cef-src/chromium/src/cef`). Adds
  `libcef/browser/chrome/extensions/alloy_window_controller.{h,cc}`
  (`cef::AlloyExtensionWindowController` + `cef::AlloyBaseWindow` —
  an `extensions::WindowController`/`ui::BaseWindow` bridge for
  single-tab Alloy browsers, registered in `WindowControllerList`)
  plus `AlloyBrowserHostImpl` lifecycle/focus wiring and the
  `cef/BUILD.gn` entry.
- `0002-alloy-tab-model-chromium.patch` — applies to the Chromium
  tree (`~/cef-src/chromium/src`). Small `#if BUILDFLAG(ENABLE_CEF)`
  hunks, same style as upstream's existing `cef::GetAlloyTabById`
  graft:
  - `tabs_api.cc` — `TabsQueryFunction::BuildTabList` appends Alloy
    tabs via `MatchesAlloyWindow`/`MatchesAlloyTab`;
    `TabsGetSelectedFunction` handles Alloy controllers;
    `WindowsGetAllFunction`/`WindowsGetLastFocusedFunction` include
    Alloy windows. `currentWindow`/current-window-id matching falls
    back to the last-focused Alloy controller when the sender has no
    WebContents (service workers), mirroring upstream semantics.
  - `chrome_extension_function_details.cc` — null-safe BWI check +
    current-window fallback for Alloy-hosted senders.
  - `windows_event_router.cc` — `windows.onCreated/onRemoved/
    onBoundsChanged` for Alloy windows.
  - `tabs_event_router.{h,cc}` — `TrackAlloyTab`/`UntrackAlloyTab`
    (tabs.onCreated/onUpdated/onRemoved); `TabEntry` tolerates a
    WebContents with no `tabs::TabInterface`.
  - `tabs_windows_api.cc` — `InitTabsEventRouter` bootstraps tracking
    for already-live Alloy tabs (the router is created lazily on first
    event listener, so Alloy browsers that exist by then would never
    fire tab events otherwise; bootstrap does not fire onCreated,
    matching upstream pre-existing-tab behavior).

## Apply

```bash
cd ~/cef-src/chromium/src/cef && git apply \
  /path/to/repo/spikes/cef/patches/0001-alloy-tab-model-cef.patch
cd ~/cef-src/chromium/src && git apply \
  /path/to/repo/spikes/cef/patches/0002-alloy-tab-model-chromium.patch
```

## Build

Same flow as CS07 (`.devin/CEF-SOURCEBUILD.md`): GN regen is
automatic; then

```bash
cd ~/cef-src/chromium/src
third_party/ninja/ninja -C out/Release_GN_x64 -j2 \
    cefsimple chrome_sandbox
```

Run it detached under a memcg (the 2.8G `libcef.so` SOLINK needs it):

```bash
systemd-run --user --unit=cs08-build.service -p MemoryMax=4G \
    -p OOMPolicy=continue --collect -- \
    third_party/ninja/ninja -C out/Release_GN_x64 -j2 \
    cefsimple chrome_sandbox
```

## Runtime caveat (dcheck/PA-malloc build)

This source build is not `is_official_build`; launching it requires

```
--disable-features=UseBoringSSLForRandBytes --ozone-platform=x11 \
    --change-stack-guard-on-fork=disable
```

(see `.devin/CEF-SOURCEBUILD.md` "Required runtime flags"). The third
flag is needed because this build enables the hardened stack-canary
mitigation for zygote children (`base::ResetStackCanaryIfPossible`
re-randomizes each child's TLS canary); embedder `main()` is not
compiled `NO_STACK_PROTECTOR`, so a subprocess that returns through
`main`'s epilogue trips `__stack_chk_fail` ("stack smashing detected")
on every child exit. The shipped Spotify binary does not exhibit it —
measured zero smashes under identical flow; Chromium documents the
flag in `base/stack_canary_linux.cc` (crbug.com/1206626). It is an
artifact of running custom-embedder binaries against a non-official
build, unrelated to the graft.

## Verify

`spikes/cef/cs08/run.sh` — runs the tabprobe fixture
(`spikes/cef/fixtures/tabprobe/`) over CDP on alloy-windowed
(`cefsimple --use-alloy-style`, xvfb) and alloy-OSR (`qcefprobe`,
`LD_LIBRARY_PATH=<out dir>`) legs, writing per-leg PASS/FAIL JSON to
`~/cef-cache/cs08-*.json` plus popup pixel witnesses.
