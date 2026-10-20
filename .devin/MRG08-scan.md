# MRG08 — post-merge leak sweep B: per-backend EngineTab churn

Generated: 2026-10-20 on f552e570 (the MRG08 fix+legs commit; the
instrumented tree is a `git ls-files` copy of that working state, so
the corpus covers exactly the committed code).  Focused ASan/LSan
churn over the engine backends on merged master:

- Per-backend `EngineTab` mint/destroy cycles for every registered
  backend — webengine, servo, webkit — including webengine, which the
  TabWidget swap path can never produce (`engineBackendChurn`, x4 each).
- Servo view create/destroy xN — ENG36's multi-view path: 3x3
  concurrent live views with reverse-order teardown plus 6 sequential
  cycles (`servoViewChurn`).
- WebKit render churn — 3x create → fixture load → rendered frame →
  destroy, exercising the WPE buffer/WebProcess path the old
  never-navigating churn leg missed (`webkitRenderChurn`).
- Cross-engine poison regression — hold a live webkit GL pipe through
  3 servo churn cycles, then require a fresh webkit view to still draw
  GL frames (`webkitGlSurvivesServoChurn`; covers the MRG06-filed
  SIGSEGV/shader-failure pair fixed in f552e570 — `eglBindAPI(ES)`
  before context creation, per-frame `eglInitialize` display revive,
  `wkGlReset` stale-pipe rebuild, `wkGlCleanup` makeCurrent gate).
- The pre-existing `engineTabChurn` TabWidget swap leg, as the
  sweep's baseline.

Toolchain: Qt 6.12.0 (~/Qt/6.12.0/gcc_64), clang 18.1.3,
`qmake6 -spec linux-clang CONFIG+=sanitize` (ASan+UBSan) — MEM11's
throwaway-tree recipe.  Flavor difference vs MRG07, deliberate: this
tree is the FULL three-engine flavor — `webkit=1` (WPE env via
webkit-env.sh) and the servo backend live (prebuilt
`libservo_embed.so` copied into the throwaway; it resolves from
`spikes/servo/servo-embed/target/release/`).  The rust `.so` and the
WPE libs are NOT instrumented — ASan still intercepts every malloc,
so leaks are caught, but their stacks can never resolve to `src/`:
uninstrumented-module allocations classify upstream by construction.
Per-cycle growth is the real churn signal and is what the tally
below checks.

## Instrumented tree

    /tmp/arktos-mrg08.oeP1H5        (leak corpus: ./leaklogs)

Throwaway-only measurement patch (never committed): the MEM09/MEM11
seam in `autotests/qtest_arktos.h` — `__lsan_do_leak_check()` before
the QUAL05 `_exit(rc)`.  tst_engineswitch uses `QTEST_MAIN`, so the
one harness patch covers it.

(A second copy, /tmp/arktos-mrg08.fjxqOo, is the abandoned attempt-2
tree — killed during its build before any corpus ran.)

## Method

Per-run `HOME=$BUILD/.home` (+ XDG_* under it),
`QT_QPA_PLATFORM=offscreen`, `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`,
`ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=64:thread_local_quarantine_size_kb=64:print_stacktrace=1`,
`LSAN_OPTIONS=exitcode=0:print_suppressions=0:report_objects=1`,
`ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18`, webkit-env.sh
sourced (WPE rpath/compat shim live — WPEWebProcess ran for real).
Classification is `.devin/check-leaks.sh`'s rule verbatim: a block is
"ours" iff one of its first four frames resolves to a source file
under the instrumented tree's `src/` or `autotests/`.

## Results

| run | qtest | ours | upstream records |
|-----|-------|-----:|---------:|
| engineTabChurn            | 3/3 PASS (436 ms) | 0 | 10 (38,268 B) |
| engineBackendChurn        | 3/3 PASS (164 ms) | 0 | 10 (38,268 B) |
| servoViewChurn            | 3/3 PASS (447 ms) | 0 | 10 (41,057 B) |
| webkitRenderChurn         | 3/3 PASS (903 ms) | 0 |  5 (1,063 B)  |
| webkitGlSurvivesServoChurn| 3/3 PASS (755 ms) | 0 | 16 (39,803 B) |
| full binary (-silent)     | 49 PASS / 0 fail / 0 skip (24.9 s) | 0 | 43 (73,709 B) |

Corpus total: 94 upstream records, **zero `src/`- or
`autotests/`-sited records** — the first-four-frames rule finds no
frame under the tree in any log.

Upstream tally (first-real-frame module):

| module | records | bytes | families |
|--------|--------:|------:|----------|
| libservo_embed.so (rust) | 40 | 197,861 | stylo TLS: `style::sharing` SharingCache, `selectors::bloom` BloomFilter; `rayon` worker threads; `script::dom::globalscope` script-execution globals; `ServoUrl`/`url` parse caches; `moz_xmalloc`, `__rust_alloc` |
| libdbus-1.so.3 | 21 | 28,107 | dbus connections/strings — process-lifetime |
| libQt6WebEngineCore.so.6 | 19 | 3,920 | Chromium teardown (full-binary log only) |
| libfontconfig.so.1 | 12 | 1,920 | `FcInit` — same family MEM11/MRG07 tallied |
| libWPEWebKit-2.0.so.1 | 2 | 360 | WPE UI-process init (full-binary log only) |

Churn-growth check (the point of the sweep): the servo-touching legs
each mint a different number of views — engineBackendChurn 4 cycles,
servoViewChurn 3x3 concurrent + 6 sequential, glsurvive 3 cycles —
yet all carry the same 8 rust-family records (~38–41 KB); the only
delta is larger object counts inside those same init families when
more concurrent views ran (TLS/rayon pools scale with live threads,
not leaked per cycle).  `webkitRenderChurn` — the only leg that never
initializes servo — carries zero rust records, 5 records / 1,063 B
total.  No record family scales with cycle count: cycles add nothing.

The MRG06-filed cross-engine kill is also gone under ASan:
`engineTabChurn`'s servo cycles followed by webkit GL work passed in
the full-binary run (49/0/0 — the same ordering that SIGSEGV'd the
MRG06 build), and `webkitGlSurvivesServoChurn` is the dedicated
regression leg for it.

## Caveats

- Rust/servo and WPE libs are uninstrumented: their allocations can
  never classify "ours".  A leak *inside* libservo_embed would appear
  (malloc is intercepted) but always as upstream; the stable
  record-count-across-churn evidence above is the mitigation.
- The attempt-1 full-binary corpus run was SIGTERM'd mid-leg when the
  whole session hit the memory cap (a session kill, not a test
  fault); the completed 49/0/0 log here is the attempt-3 re-run of
  the identical binary.

## Verdict

All four new churn carriers plus the baseline swap leg run green on
the plain webkit=1 build AND under ASan/LSan, and the whole
tst_engineswitch binary (49 legs incl. every webkit*/servo leg) runs
49 PASS / 0 fail / 0 skip under the same harness: **zero
`src/`-sited leak records** anywhere in the corpus — every recorded
block is an upstream servo/stylo/dbus/QtWebEngine/fontconfig/WPE
process-lifetime allocation in already-known families.  Nothing
required fixing; no suppression was added or needed.

Normal-tree gates: check-warnings PASS (0 diagnostics, fresh build),
check-supplychain PASS, check-engine-audit PASS (185 files),
check-rustcoverage PASS (94.11/91.83/89.25 >= 85, COVERAGE.md
regenerated on f552e570).
