# MEM11 — leak re-audit C: post-spine surfaces

Generated: 2026-10-19 on 9129b576 (WK21) + this task's working-tree
changes.  Third sweep after MEM02/MEM09/MEM10; covers the new code
landed since: `src/engine/webkitbackend.*` (WPE backend, WK01–WK21),
`src/extensions/*` (hosted chrome.* runtime), multi-view servo shim
(ENG36), and the new autotest fixtures.
Toolchain: Qt 6.12.0 (~/Qt/6.12.0/gcc_64), clang 18.1.3,
`qmake -spec linux-clang CONFIG+=sanitize` (ASan+UBSan), servo /
webkit-linked / rustcore / rustdl / adblock_rust all enabled.

## Instrumented tree

    /tmp/arktos-mem11.z5sUXp          (kept; corpora in
                                       ./leaklogs-mem11 [sweep 1]
                                       ./leaklogs-mem11-r2 [re-verify])

Throwaway-only measurement patches (never committed):

- `autotests/qtest_arktos.h`: `__lsan_do_leak_check()` before the
  QUAL05 `_exit(rc)` — the MEM09 seam; without it `_exit` skips
  LSan's at-exit check entirely.
- `autotests/webview/tst_webview.cpp`, `certerror/tst_certerror.cpp`,
  `schemehandlers/tst_schemehandlers.cpp`: the same
  `__lsan_do_leak_check()` + `_exit` added to their custom `main()`s.
  Required this sweep: a normal `return` runs static destructors
  (destroying `DownloadManager`'s static `downloadManagers()` map)
  before LSan's at-exit check, which reported the deliberate
  process-lifetime DM singleton as leaked (18 blocks / 3 binaries in
  sweep 1).  The pre-teardown check is the project's own MEM09
  methodology; the DM blocks vanished and the same upstream family
  remained.

## Churn legs added (committed, main tree)

- `tst_engineswitch::engineTabChurn` — 4× newTab→reloadTabInEngine→
  closeTab per live foreign backend (fake/webkit/servo).  Each cycle
  mints and tears down the backend's whole profile/page/view trio;
  QPointer guards assert real destruction.
- `tst_extensionmanager::hostedHostChurn` — 3× enable→Ready→disable
  on a fixture extension; each cycle mints a full
  ExtensionBackgroundHost (hidden view+page+channel+bridge+timers)
  and the guards assert host/view/page/bridge all die.

## Method

20 focused binaries — every `tst_extension*`, `tst_engine*`,
profile/vault churn carriers, and the touched-core
browsermainwindow/tabwidget/webview/webpage/certerror/
contentblocker/schemehandlers/devtools/navigationpolicy/
scriptcontrol/sitedecisions/containermanager — run sequentially,
`-silent`, per-binary fresh HOME+XDG,
`ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=64:
thread_local_quarantine_size_kb=64:print_stacktrace=1`,
`LSAN_OPTIONS=exitcode=0:print_suppressions=0:report_objects=1`,
`QT_QPA_PLATFORM=offscreen`,
`QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`, WPE env sourced.
Classification is check-leaks.sh's rule verbatim: a block is "ours"
when a top-4 frame resolves under the tree's `src/` or `autotests/`;
else upstream by first-real-frame module.

## Sweep 1 — before fixes (leaklogs-mem11)

| binary | qtest | ours | upstream |
|--------|-------|-----:|---------:|
| tst_browsermainwindow | ASan ABORT mid-suite | 0* | n/a* |
| tst_certerror    | 10/10 | 6 | 547 |
| tst_containermanager | 3/3 | 0 | 35 |
| tst_contentblocker | 7/7 | 0 | 2 |
| tst_devtools     | 20/20 (1 skip) | 0 | 21 |
| tst_engineadapter | 13/13 | 0 | 2 |
| tst_engineswitch | 44/44 (1 skip) | 2 | 40 |
| tst_extensionmanager | 28/28 | 0 | 2 |
| tst_extverify    | 10/10 | 0 | 2 |
| tst_navigationpolicy | 16/16 | 0 | 2 |
| tst_profilemanagers | 20/20 | 0 | 2 |
| tst_profilewindow | 14/14 | 0 | 3 |
| tst_schemehandlers | 24/25 (1 flake) | 6 | 308 |
| tst_scriptcontrol | 23/23 | 0 | 2 |
| tst_sitedecisions | 8/8 | 0 | 94 |
| tst_tabwidget    | 36/36 (9 skip) | 0 | 32 |
| tst_vaultengine  | 17/17 | 0 | 2 |
| tst_webpage      | 73/73 (2 skip) | 0 | 2 |
| tst_webpermissions | 22/22 | 0 | 2 |
| tst_webview      | 29/29 (1 skip) | 6 | 337 |
| **total** | | **20** | **1437** |

*tst_browsermainwindow aborted on the ASan error below — its leak
tally is void; see "non-leak finding".

## Our-code blocks (sweep 1)

**1. `src/engine/webkitbackend.cpp:4667` — FIXED.**  In
`WebkitProfile::cbContentFilterLoad` (WK18 content-filter fallback
compile), `g_bytes_new(...)` was passed inline to
`webkit_user_content_filter_store_save()` — transfer-none, so the
async task refs the GBytes itself and our ref was never released.
One leaked GBytes (+40 B data + ~490 B indirect) per compile.
Fixed by holding the pointer and `g_bytes_unref`ing after the call.
Re-verified: gone in sweep 2.

**2. `src/downloadmanager.cpp:1297/1317/1318` — measurement artifact,
classified not fixed.**  `DownloadManager::instanceFor` mints a
parentless process-lifetime singleton held by a static
`QHash<QString,QPointer>` (the code comment already calls the
pattern out).  In tst_webview/certerror/schemehandlers — the only
three binaries with a custom `main()` — static destruction at
normal `return` orphaned the DM island before LSan's at-exit check;
every QTEST_MAIN binary (destructors skipped via `_exit`) reports
nothing.  This is exactly the reachable-but-unreleased singleton
class MEM09 scoped out; `releaseFor()` exists for persona cleanup.
Resolved in-tree by extending the pre-teardown seam to those mains
(throwaway patch) — sweep 2 reports the same binaries with only the
standard upstream family.

## Non-leak finding — ASan heap-buffer-overflow, FIXED

`tst_browsermainwindow::servoStartPage` aborted the whole binary
mid-suite: `EngineTab::page()` (enginetab.h:57) called through
`firstEngine`, a raw `EngineTab*` captured before `closeWindow`
freed it — invalid-vptr UAF at tst_browsermainwindow.cpp:4323.
Silently benign on plain builds (freed memory reads back the stale
m_page).  Fixed test-side: the page-liveness check now runs before
`closeWindow`, preserving intent.  The abort had also voided the
binary's leak tally — the fixed binary was re-run in sweep 2.

## Sweep 2 — re-verify (leaklogs-mem11-r2)

| binary | qtest | ours | upstream |
|--------|-------|-----:|---------:|
| tst_certerror    | 10/10 | 0 | 2 |
| tst_engineswitch | 44/44 (1 skip) | 0 | 40 |
| tst_schemehandlers | 24/25 (same flake) | 0 | 2 |
| tst_webview      | 29/29 (1 skip) | 0 | 27 |
| tst_browsermainwindow (UAF fix) | 77/77 | 0 | 25 |
| **total** | | **0** | **96** |

Focused churn-leg runs under detect_leaks=1:
`tst_engineswitch engineTabChurn` 3/3 PASS — 10 blocks / 38 KB, all
Servo engine internals (style TLS caches, rayon pool, servo_url,
moz_xmalloc) + fontconfig; `tst_extensionmanager hostedHostChurn`
3/3 PASS — 2 blocks / 320 B fontconfig+expat.  Zero src/-sited.

## Upstream tally (sweep 1, 1437 blocks)

| first-real-frame module | blocks |
|-------------------------|-------:|
| libQt6Core.so.6         | 662 |
| libQt6WebEngineCore.so.6 | 243 |
| libexpat.so.1           | 42 |
| libudev.so.1            | 20 |
| libdbus-1.so.3          | 12 |
| libfontconfig.so.1      | 10 |
| libQt6Gui/Widgets       | 12 |
| libstdc++.so.6          | 2 |
| unsymbolized (servo cdylib, other) | ~434 |

Same MEM02/09 families plus Servo engine-internal TLS/pool
allocations reachable at check time from libservo's global state —
classified upstream (allocation sites inside the cdylib, not our
shim code; the shim-side view create/destroy itself leaked nothing).

## Observations (not leaks)

- `tst_schemehandlers::fileSchemeErrors` is an intermittent
  ASan-environment flake: the leg's `loadAndFail` waitFor burned its
  15 s timeout in both sweep runs (~2/5 observed full-suite runs)
  but passes standalone on the instrumented binary (235 ms), passes
  on the plain build (2.5 s), and passed on a post-fix full-suite
  re-run (25/25, 2.7 s).  Timing edge in the file:// error path
  under instrumentation, not a leak; not introduced by this task.
- tst_webview/certerror/schemehandlers carry the pre-teardown seam
  only in the throwaway tree; their in-tree custom mains are
  unchanged.

## Verdict

Two real issues found and fixed this task: the WK18 GBytes
transfer-none leak (src/) and the servoStartPage UAF (autotests/).
The DownloadManager island is a deliberate process-lifetime
singleton exposed by custom-main teardown order — classified, not a
churn leak.  All churn surfaces (EngineTab create/destroy across
backends incl. multi-view servo, extension host enable/disable,
profile/page churn inside the 20-binary corpus) report **zero
src/-sited leak blocks** post-fix.
