# MEM09 — leak re-audit A: instrumented build + focused scan

Generated: 2026-10-18 on merge commit 3004079e (rust-work +
origin/master — MPROF01-06 / PVAULT01-04 all present).
Toolchain: Qt 6.12.0 (~/Qt/6.12.0/gcc_64), clang 18.1.3,
`qmake -spec linux-clang CONFIG+=sanitize` (ASan+UBSan, sanitize.pri).

## Instrumented tree (MEM10 handoff)

Built by `.devin/check-leaks.sh` phase 1 (`ARKTOS_LEAKS_KEEP=1`,
`ARKTOS_LEAKS_VALGRIND=0`, `ARKTOS_LEAKS_JOBS=2`,
`ARKTOS_LEAKS_LSAN_LOGS=<empty>` so only the build ran — no tooling
rebuilt):

    /tmp/arktos-leaks.mA8jKi          (kept; logs in ./leaklogs-mem09)

**The kept tree carries a measurement patch** (throwaway copy only —
never committed): `autotests/qtest_arktos.h` calls
`__lsan_do_leak_check()` immediately before the QUAL05 `_exit(rc)`
in `QTEST_MAIN`.  Required because `_exit` skips atexit handlers,
which also skips LSan's at-exit check — every QTEST_MAIN binary
(78 of them, including all 13 focused ones) silently reported zero
leaks.  QUAL05's stability property is preserved: destructors still
don't run.  Consequence: the check sees the heap *before* static /
app teardown — blocks reachable from live roots are not reported, so
teardown-time orphans (e.g. the Chromium teardown pool that dominated
MEM02's full-exit counts) are under-counted here.  Mid-run orphans —
the profile/vault lifecycle bug class MEM10 hunts — report
identically.

## Method

13 binaries — every `tst_profile*`/`tst_vault*` plus the touched-core
`tst_browsermainwindow`/`tst_tabwidget` — run strictly sequentially,
`-silent`, per-binary fresh HOME + XDG dirs,
`ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=64:
thread_local_quarantine_size_kb=64:print_stacktrace=1`,
`LSAN_OPTIONS=exitcode=0:print_suppressions=0:report_objects=1`,
`QT_QPA_PLATFORM=offscreen`,
`QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`.  Classification is
check-leaks.sh's allocation-site rule verbatim: a block is "ours"
when a top-4 frame resolves under the kept tree's `src/` or
`autotests/`; everything else is upstream by first-real-frame module.

No leaks were fixed (MEM10's job).

## Result — all 13 binaries ran to completion

| binary | qtest | ours | upstream | leaked bytes |
|--------|-------|-----:|---------:|-------------:|
| tst_profileavatar   | 17/17 pass | 0 |  2 |       320 |
| tst_profilecontext  | 12/12 pass | 0 |  2 |       320 |
| tst_profilee2e      | 23/23 pass | 0 |  3 |       336 |
| tst_profilemanager  |  8/8 pass  | 0 |  2 |       320 |
| tst_profilemanagers | 15/15 pass | 0 |  2 |       320 |
| tst_profilemenu     | 14/14 pass | 0 |  2 |       320 |
| tst_profilesettings | 12/12 pass | 0 |  2 |       320 |
| tst_profilevault    | 11/11 pass | 0 |  2 |       320 |
| tst_profilewindow   | 12/12 pass | 0 |  2 |       336 |
| tst_vaulte2e        |  9/9 pass  | 0 |  2 |       320 |
| tst_vaultengine     | 16/16 pass | 0 |  2 |       320 |
| tst_browsermainwindow | 77/77 pass | **1** | 15 | 1,115,461 |
| tst_tabwidget       | 36/36 pass | 0 | 32 |   221,302 |
| **total** | — | **1** | **70** | ~1.30 MB |

(Block = one report line; each line may cover N objects.  Per-binary
SUMMARY totals shown at right include those objects.)

## Our-code block (feeds MEM10's fix pass)

**`autotests/browsermainwindow/tst_browsermainwindow.cpp:7595` —
Direct 336 B in 14 objects.**  `stopReloadState()`'s fixture
`QTcpServer` lambda does `QByteArray *request = new QByteArray;` per
connection and never frees it (the lambda only deletes via the socket
on normal close — the held/aborted connections outlive it).  Autotest
code, not product code — but the classifier counts it ours.
Representative stack:

    #0 operator new (tst_browsermainwindow)
    #1 tst_BrowserMainWindow::stopReloadState()::$_0::operator()()
       autotests/browsermainwindow/tst_browsermainwindow.cpp:7595
    #7 Qt6Core  #8 QTcpServerPrivate::readNotification (Qt6Network)

**Zero leak blocks sited in `src/`** — no profile/vault/product-code
leak observed under LSan on these legs.  (Caveat: reachable-but-
unreleased singletons and pre-teardown state are invisible to this
check; MEM10's churn legs + valgrind cover that surface.)

## Upstream tally (70 blocks, all tagged upstream)

| first-real-frame module | blocks |
|-------------------------|-------:|
| libexpat.so.1           | 26 |
| libfontconfig.so.1      | 20 |
| libstdc++.so.6          |  2 |
| libQt6Core.so.6         |  2 |
| libQt6Widgets.so.6      |  1 |
| libQt6WebEngineCore.so.6|  1 |
| (unsymbolized/other)    | 18 |

Same MEM02 families (fontconfig/expat config caches, WebEngineCore,
libstdc++ locale facets).  The small per-test counts are consistent
with the pre-teardown check — the large Chromium teardown pool only
appears at real exit (a control run of `./arktos --quit-after-load`
on this tree reported 2,916 allocs / 409 KB, all upstream, exactly
MEM02's family).

## Observations (not leaks — recorded for MEM10)

- `tst_browsermainwindow` emits one recoverable UBSan report:
  `qpointer.h:76 downcast ... to 'QFrame'` (QWidget vs QFrame vptr) —
  Qt-header class-hierarchy UB triggered from our qpointer usage;
  same family MEM02-era sanitize runs saw.
- First pass on a *shared* HOME flaked two legs
  (`writeShadowsFallback`, `torStartPage`) from cross-binary
  QSettings bleed; fresh-per-binary HOME removed both — final run
  above is clean.  Raw logs: `$BUILD/leaklogs-mem09/`.
- No `tst_vaultui`/`profileregistry` binary exists — vault dialogs
  and the registry are covered inside tst_vaultengine /
  tst_profilecontext / tst_profilee2e.
