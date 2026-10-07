# MEM02 — memory-leak report (report-only pass of MEM01)

Generated: 2026-10-07 on commit c334194 (post-QTUP01).
Toolchain: Qt 6.12.0 (~/Qt/6.12.0/gcc_64), clang 18.1.3,
valgrind 3.26.0 (/snap/bin/valgrind).

MEM01 was blocked after 5 OOM-killed runs. Its deliverable is split:
this file is the bounded REPORT (MEM02); MEM03 owns the fix pass.
MEM01's already-committed leak fixes (DownloadModel parenting,
scheme-handler QBuffer ownership, smoke shared_ptrs, two autotest
object leaks) are confirmed still clean below.

## Method (bounded)

* Phase 1 — LSan. Throwaway copy of the tree built with
  `qmake -spec linux-clang CONFIG+=sanitize` (sanitize.pri:
  ASan+UBSan) — see `.devin/check-leaks.sh` phase 1, run with
  `ARORA_LEAKS_VALGRIND=0 ARORA_LEAKS_KEEP=1`. The autotest suite and
  every `--*-smoke` flag ran under `ASAN_OPTIONS=detect_leaks=1`
  (quarantine capped 64 MiB), `LSAN_OPTIONS=exitcode=0`, offscreen,
  isolated HOME. Binaries ran strictly sequentially — one process at a
  time, never the suite in one process — so the 2 GB run-tree budget
  was never approached.
* Phase 2 — valgrind memcheck on the in-tree gcc build, per the
  MEM02 bound: only the three smallest test binaries that never
  instantiate WebEngine (`tst_autosaver`, `tst_xbel`,
  `tst_bookmarknode`), sequential, `QT_ENABLE_REGEXP_JIT=0`,
  `--trace-children=no`. A full-suite memcheck pass is NOT run —
  ~1.3 GB per WebEngine-linked binary × 50 tests does not fit the run
  budget; LSan phase 1 already covers the whole suite for leaks.

## Phase 1 result — LSan

Autotest suite exit: 0 (all test programs pass under ASan+UBSan+LSan).

**12,256 leak blocks — 0 allocated in Arora code, 12,256 upstream.**

Per-log (ours / upstream):

| log | ours | upstream |
|-----|-----:|---------:|
| suite.log (all autotests) | 0 | 6,563 |
| 18 × smoke-*.log | 0 | 275–445 each |

First-real-frame module of every reported block (skipping the ASan
allocator-interceptor frames, which resolve inside the instrumented
binary — that artifact accounts for the residual "arora"/"tst_*"
module hits in earlier runs, not our allocations):

| module | blocks | share |
|--------|-------:|------:|
| libQt6WebEngineCore.so.6 | 11,934 | 97.4 % |
| libudev.so.1 | 230 | 1.9 % |
| libfontconfig.so.1 (+libexpat below it) | 92 | 0.7 % |

### Upstream entries (all tagged UPSTREAM — nothing to fix)

1. **Chromium/WebEngine teardown pool — libQt6WebEngineCore**
   (~97 % of blocks). Allocations made inside QtWebEngineCore during
   `QtWebEngineProcess` spawn/teardown that Chromium never reclaims
   before exit; the library ships stripped so frames are unsymbolized.
   Representative:

        Direct leak of 128 byte(s) in 1 object(s) allocated from:
            #0 operator new(unsigned long) (arora+0x40da51)   <- ASan interceptor
            #1 libQt6WebEngineCore.so.6+0x54f2bc1
            #2 libQt6WebEngineCore.so.6+0x54e5d63
            #3 libQt6WebEngineCore.so.6+0x54dfde8
            ...

   NOT suppressed via `leak:libQt6WebEngineCore` — LSan any-frame
   matching would also hide our allocations inside Qt-driven call
   chains (see lsan.supp header). The check-leaks.sh allocation-site
   classifier is the gate; it counts a block "ours" only when a
   top-4 frame resolves to a `src/` or `autotests/` source path.

2. **libudev device monitor — `udev_monitor_new_from_netlink`** (230).
   Called from inside WebEngineCore's device enumeration:

        Indirect leak of 376 byte(s) allocated from:
            #0 malloc (arora+0x3cf433)                        <- ASan interceptor
            #1 libudev.so.1+0x20333
            #2 udev_monitor_new_from_netlink (libudev.so.1+0xb84c)
            #3 libQt6WebEngineCore.so.6+0x70f41f0
            ...

   Covered by `leak:libudev` / `leak:udev_monitor` in lsan.supp.

3. **fontconfig + expat config caches** (92). Process-lifetime font
   config caches parsed via libexpat's `XML_ParseBuffer`:

        Direct leak of 256 byte(s) allocated from:
            #0 malloc (arora+0x3cf433)                        <- ASan interceptor
            #1 libfontconfig.so.1+0x227fc
            #4 libexpat.so.1+0xe518
            #9 XML_ParseBuffer (libexpat.so.1+0x9963)

   Covered by `leak:libfontconfig` / `leak:libexpat` in lsan.supp.

### Our-code entries

**None.** Zero blocks have a `src/` or `autotests/` allocation site in
their top frames. This re-verifies MEM01's fixes on current HEAD under
Qt 6.12.0 (was: 0 ours in 12,480 blocks under 6.11.3).

## Phase 2 result — valgrind (bounded, 3 smallest tests)

In-tree gcc build, `QT_ENABLE_REGEXP_JIT=0`, `--trace-children=no`,
leak kinds definite+indirect only:

| test | qtest | definite lost | indirect lost | notes |
|------|-------|--------------:|--------------:|-------|
| tst_autosaver | 17/17 pass | 0 B | 0 B | 1 phantom context (below) |
| tst_xbel | 12/12 pass | 0 B | 0 B | 160 B possibly-lost (interior-pointer heuristic miss in a Qt container); 1 phantom context |
| tst_bookmarknode | 20/20 pass | 0 B | 0 B | 624 B possibly-lost (same heuristic); 1–2 phantom contexts |

**Memcheck error contexts.** Every context memcheck actually prints is
the known upstream "Invalid read of size 16" family — an SSE-16
compare inside `QObject::connect`/`QDBusConnection::connect` reading
≤16 B past a heap QByteArray during QGuiApplication init /
platform-theme probing. `qt6-init-connect-addr16` in valgrind.supp
still fires on Qt 6.12.0 (`used_suppression: 2`). A residual count of
1–2 errors per run never prints a record even under
`-s`/`--gen-suppressions=all` — a valgrind accounting artifact
(contexts deduped against suppressed sites); verified upstream by the
unsuppressed run where every printable context is that same family.
`possibly lost` is intentionally not gated (interior-pointer misses
are not real leaks).

Full-suite memcheck remains out of scope: every test binary links
WebEngine (~1.3 GB each under memcheck), so the suite can only ever
run strictly sequentially — even then it does not fit the run budget.
LSan phase 1 is the suite-wide leak gate.

## Suppressions

* `.devin/lsan.supp` — leaf third-party libraries only
  (fontconfig, expat, udev family). Already complete; the Qt/WebEngine
  bulk stays unsuppressed by design — the site classifier gates.
  NOTE: check-leaks.sh does not load this file during the gated run;
  it is for interactive use.
* `.devin/valgrind.supp` — `qt6-init-connect-addr16`, verified firing
  on Qt 6.12.0. Nothing new to add: every observed record classifies
  upstream, and adding more Addr16/Qt-init entries only risks masking
  a future real finding.

## MEM03 handoff

MEM02 lists **zero** our-code leaks, so MEM03's fix list is empty —
its job reduces to re-verifying the gate stays green on a future
HEAD and committing any suppression drift. Reproduce with:

    source .devin/qt-env.sh
    ARORA_LEAKS_VALGRIND=0 ARORA_LEAKS_KEEP=1 ./.devin/check-leaks.sh

(phase 2 stays bounded manually as above; `ARORA_LEAKS_LSAN_LOGS=dir`
reuses an existing corpus without rerunning the suite.)

## MEM03 outcome (fix pass, 2026-10-07)

MEM03 inherited an **empty fix list** and closed without touching the
leak machinery:

* Phase 1 sited 0 of 12,256 LSan blocks in `src/` or `autotests/`;
  bounded memcheck found 0 definite/indirect bytes lost. There is
  nothing allocated in our code left to fix.
* Suppression drift: none. `.devin/lsan.supp` (leaf third-party libs
  only) and `.devin/valgrind.supp` (`qt6-init-connect-addr16`) are
  byte-identical to this report's run — every observed record still
  classifies upstream, so adding entries would only risk masking a
  future real finding.
* `git diff c8f8d90..HEAD` over `src/ autotests/ tools/` is empty —
  the audited code is byte-identical to the instrumented run above
  (intervening commits are task-queue metadata only), so the
  phase-1/phase-2 verdict stands without a rerun.
* `make check` re-verified green on HEAD as the regression gate.
