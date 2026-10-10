# CRASH01 — QtWebEngine GPU-process CHECK (SIGTRAP) investigation

## Symptom

`arora` repeatedly dies while browsing. Kernel journal shows a fixed
signature, identical on every occurrence (10+ hits on 2026-10-10,
including bursts during the user's live Wayland session and during
test runs):

```
traps: Chrome_InProcGp[pid] trap int3 ip:XXXXe6481 ... in libQt6WebEngineCore.so.6.12.0[76e6481, ...]
```

- `int3` = deliberate CHECK/trap, not a memory fault.
- `Chrome_InProcGp` = Chromium's in-process GPU thread (QtWebEngine
  always runs GPU in-process — the name alone is not a flag symptom).
- The base-relative offset `0x76e6481` never varies → ONE CHECK site.

## What was captured

- `--enable-logging=stderr --log-level=0 --vmodule=gpu*=3,viz*=3` gets
  GPU init to the point of "Vulkan disabled or failed to initialize /
  VizNullHypothesis is disabled", then the process traps with **no
  CHECK text** — this is a silent `IMMEDIATE_CRASH` site in a release
  build; no file:line is emitted.
- A ptrace stack capture places the trap on a
  `Compositor::bind`-adjacent path (viz display compositor binding).
  View-less tests pass; every test that shows a `QWebEngineView`
  traps.

## Root-cause triage (flag bisect on `tst_webview`, offscreen)

| Flag                                   | Result            |
|----------------------------------------|-------------------|
| (none)                                 | SIGTRAP (rc=133)  |
| `--disable-gpu`                        | PASS              |
| `--disable-gpu-compositing`            | PASS              |
| `--disable-vulkan`                     | SIGTRAP           |
| `--disable-features=Vulkan`            | SIGTRAP           |
| `--disable-gpu-sandbox`                | SIGTRAP           |
| `--in-process-gpu`                     | SIGTRAP           |
| `--disable-gpu-watchdog`               | SIGTRAP           |
| `--ozone-platform=headless`            | SIGTRAP           |
| `--use-gl=swiftshader`                 | abort (rc=134)    |
| `--use-angle=gl-egl` / `swiftshader`   | SIGTRAP           |
| `--enable-unsafe-swiftshader` + angle  | SIGTRAP           |
| `--disable-software-rasterizer`        | SIGTRAP           |

Ruled out: Vulkan init/teardown, the Chromium GPU sandbox, the
in-process-GPU process model, the Ozone backend pick, and the ANGLE/GL
backend selection. The trigger is specifically the **viz GPU display
compositor** — disabling GPU compositing avoids the CHECK while
keeping the GPU process (and therefore WebGL/canvas acceleration)
alive. `--disable-gpu` also works but was explicitly ruled out as too
broad (it strips WebGL entirely).

## Sandboxed vs unsandboxed

- The crash reproduces in a plain unsandboxed process (`tst_webview`
  run directly) — sandboxing is not the trigger.
- The bwrap policy (`src/sandbox/bwrapgenerator.cpp`) uses
  `--dev-bind / /`, so `/dev/dri` (card1/renderD128, Intel UHD 770) is
  already visible inside the sandbox — no device denial.
- The AppArmor `unpriv_bwrap DENIED cap_sys_admin` line affects
  Chromium's *renderer* namespace sandbox, not the in-process GPU
  thread where this CHECK fires.

## Environment observations

- Host: Intel Alder Lake-S UHD 770, Wayland session
  (`XDG_SESSION_TYPE=wayland`), Qt 6.12.0.
- Under `QT_QPA_PLATFORM=offscreen` the crash is 100% reproducible —
  the offscreen QPA gives the viz compositor no real surface. The
  shipping browser already forces `--disable-gpu` under offscreen
  (POL02 in `applyChromiumFlags`), so only raw test binaries hit it
  headless — that is the source of the recurring journal bursts during
  `make check`-adjacent runs.
- Under Xvfb/llvmpipe (GLX available) the browser runs clean —
  `xvfb-run ./arora --quit-after-load https://example.com/` rc=0.
- The user's live Wayland crashes carry the SAME fixed offset
  `0x76e6481`, so they are the same CHECK site — an upstream Qt
  6.12.0/Chromium defect in the GPU display-compositor path on this
  driver/stack, not app code.

## Containment (what shipped)

`--disable-gpu-compositing` — surgical: moves the display compositor
to software while the GPU process, WebGL and canvas acceleration stay
functional (verified: `webgl2` context creation succeeds under the
flag on Xvfb). Three arming paths in
`BrowserProfile::applyChromiumFlags()`:

1. `websettings/disableGpuCompositing` — a new Preferences checkbox
   ("Disable GPU compositing (crash workaround)"), restart-latched.
2. `ARORA_DISABLE_GPU_COMPOSITING=1` env override (`=0` overrides even
   a stored "on", for one-shot verification runs).
3. Crash-loop safe mode: `BrowserProfile::markSessionStart()` /
   `markSessionCleanExit()` keep a `browser/sessionActive` sentinel +
   `browser/gpuCrashCount`. A fatal CHECK never unwinds, so an
   uncleared sentinel at next start means the previous run crashed.
   Two consecutive unclean exits auto-engage the flag AND persist the
   settings toggle so the dialog reflects it; any clean run resets
   the counter. Tor windows get their own sentinel
   (`sessionActiveTor`).

This contains an upstream bug honestly: after the second GPU-compositor
crash, the browser self-stabilizes instead of crash-looping; users on
unaffected stacks see no behavior change.

## Verification

- `tst_privacy::chromiumFlags` extended: flag off by default; armed by
  the setting, the env var (incl. `=0` override), and the crash
  counter (with toggle persistence) — PASS.
- E2E (scratch HOME): kill -9 -> `sessionActive=true`; second kill -9
  -> `gpuCrashCount=1`; next start -> `disableGpuCompositing=true`
  persisted; clean SIGTERM exit -> sentinel/count cleared, pref kept.
- WebGL probe under `ARORA_DISABLE_GPU_COMPOSITING=1` on Xvfb:
  `webgl2` context creation succeeds.
- No `Chrome_InProcGp int3` journal entries from any post-change run.
- `make check` — see WORKLOG line for result.

## Known limitations / honest caveat

- The exact CHECK source location could not be recovered: the release
  build emits no log text and no debug info ships; ptrace stack +
  flag bisect is the identification. If upstream ships a fix, the
  mitigation can be retired (it is opt-out via the checkbox/env).
- The crash was not reproducible on the live Wayland display (testing
  there is off-limits); identity is inferred from the identical trap
  offset rather than re-triggered interactively.
