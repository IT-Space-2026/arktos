# CEF-SOURCEBUILD — CS07 artifact verification + pin record (CS11)

Verification-only companion to CS07 (the source build itself). Nothing
here was rebuilt — this file records what the build produced, that the
artifacts actually run, and how to reproduce.

## Pin

| Piece | Value | Evidence |
|-------|-------|----------|
| CEF | 152.0.15 @ `b3d72fb752b200a48d19db1682500a4b9051afac` | `git -C ~/cef-src/chromium/src/cef rev-parse HEAD` |
| Chromium | 152.0.7977.169 (src @ `92a6f15143569cf59b340472d0b5030465d95722`, shallow `--no-history`) | `chrome/VERSION` MAJOR=152 BUILD=7977 PATCH=169 |
| Combined stamp | `152.0.15+gb3d72fb+chromium-152.0.7977.169` | embedded string in built `libcef.so` |

This is the **same pin** as the Spotify prebuilt
(`cef_binary_152.0.15+gb3d72fb+chromium-152.0.7977.169_linux64` in
`~/cef-cache`) that CS01–CS06 ran every verdict against — source and
binary lanes are version-identical.

## Verified artifacts (measured 2026-10-28)

Out dir: `~/cef-src/chromium/src/out/Release_GN_x64` (24G on disk;
`~/cef-src` total 30G).

| Artifact | Bytes | Note |
|----------|-------|------|
| `libcef.so` | 2,973,944,480 (2.8G) | `symbol_level=1` — line info kept for CS08 graft debugging |
| `cefsimple` | 4,699,608 (4.5M) | runs — see verify below |
| `chrome_sandbox` | 37,632 | setuid helper binary (not installed setuid) |

Runtime check (`spikes/cef/cs07-verify.sh`, re-run this session):
`xvfb-run cefsimple --ozone-platform=x11
--disable-features=UseBoringSSLForRandBytes --remote-debugging-port=9337
<data-URL>` → CDP `/json/version` reports **Chrome/152.0.7977.169**,
`/json/list` shows **1 page target** (renderer path live, not just
process boot). Result JSON: `~/cef-cache/cs07-result.json`.

## GN_DEFINES actually used

Script env (feeds `cef/tools/gn_args.py` via `gclient_hook.py`):

```
GN_DEFINES="is_component_build=false symbol_level=1 use_cups=false use_sysroot=true"
GN_OUT_CONFIGS="Release_GN_x64"   CEF_INSTALL_SYSROOT="x64"
```

Resolved `out/Release_GN_x64/args.gn` (full file, verbatim):

```
blink_heap_inside_shared_library=true
clang_use_chrome_plugins=false
enable_background_mode=false
enable_downgrade_processing=false
enable_linux_installer=false
enable_precompiled_headers=false
enable_resource_allowlist_generation=false
enable_widevine=true
forbid_non_component_debug_builds=false
is_component_build=false
is_debug=false
optimize_webui=true
symbol_level=1
target_cpu="x64"
use_cups=false
use_qt5=false
use_qt6=false
use_sysroot=true
```

Why these: `use_cups=false` — `cups_config_helper` execs the host
`cups-config` binary, absent on this sudo-less box (only printer
enumeration is lost; print-to-PDF unaffected). `use_sysroot=true` —
builds against the downloaded bullseye sysroot instead of missing host
-dev packages. `symbol_level=1` keeps link RAM survivable under the 4G
memcg cap while retaining line info. `proprietary_codecs` left at the
Chromium default (false) = open-codec set, same as the Spotify binary.

## Wall-clock (from `~/cef-build.log` timestamps)

| Stage | Span | Time |
|-------|------|------|
| gclient config + `gclient sync --no-history` | 10-26 21:07 → 21:25 | ~18 min |
| `gclient runhooks` + `gclient_hook.py` (GN gen) | 21:25 → 21:35 | ~10 min |
| `ninja -C out/Release_GN_x64 cefsimple chrome_sandbox` (26,634 edges) | 10-26 21:37 → 10-28 03:30 | **~29h53m** |
| Total pipeline | 10-26 21:07 → 10-28 03:31 | **~30h24m** |

Ninja ran detached as `cs07-build.service` (systemd --user, MemoryMax=4G,
OOMPolicy=continue) with jobs stepped down 5→4→2→1 as the memcg cap
bit; several OOM kills recovered by rerun/resume — `-j1` finished the
tail. Service CPU consumed (last leg): 11h49m; memory peak 4.0G.

## Required runtime flags (caveat, measured)

This is a **dcheck_always_on + PartitionAlloc-as-malloc** build —
chromium-checkout defaults, NOT the shipped `is_official_build=true`
config. Launch requires:

```
--disable-features=UseBoringSSLForRandBytes --ozone-platform=x11
```

Without the feature-disable, each thread's first `base::Rand*` call
lazily mallocs BoringSSL's per-thread rand state inside a PA
thread-cache op → `PA_REENTRANCY_GUARD` FATAL (full chain in
`cs07-verify.sh` header). The flag routes `base::RandBytes` to
getrandom(2) — a supported runtime config, zero rebuild. The shipped
Spotify binary is official-build and can never hit this. Consequence
for downstream rows: any probe running the source build must pass the
flag; prebuilt comparisons remain unaffected.

## Reproduce

```bash
# prerequisites: depot_tools at ~/depot_tools, ~35G free under $HOME,
# xvfb for the verify leg. No sudo anywhere.
spikes/cef/cs07-source-build.sh   # resumable; self-detaches into
                                  # cs07-build.service under a 4G memcg
spikes/cef/cs07-verify.sh         # writes ~/cef-cache/cs07-result.json
```

Stage markers under `~/cef-src/.cs07/` make the build script
idempotent — rerun to resume. Monitor: `tail -f ~/cef-build.log`.

## Verdict

Source build at the tested pin is **real and runnable**: artifacts
verified byte-size + version-stamped, cefsimple renders a page over CDP.
Cost on this box: ~30h wall (28min fetch/sync/hooks, ~30h compile at
-j1..5 under a 4G memcg), 30G disk. CS08/CS09/CS12 grafts can build
against this tree.
