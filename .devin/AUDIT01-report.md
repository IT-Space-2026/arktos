# AUDIT01 — post-landing hardening sweep

Post-queue-drain audit of the whole tree (user directive: "there were a
lot of changes"). Bounded triage: real findings get fixed, Qt-blindness
noise gets per-class verdicts. No reformatting.

Generated: 2026-10-10 on commit a645851 (+ in-flight audit commits).

## Method

All heavy phases run serialized inside a detached systemd user unit
(`/tmp/arora-audit01`, `arora-seq.service`) because the task-loop's
per-run tree cap (2048 MB RSS) kills any in-shell build: a single
`g++ -fanalyzer` TU peaks near 900 MB and `src/main.cpp` exceeds 11 GB
(see below). Phases: check-static → check-sanitize (+ key smokes) →
check-leaks → bounded valgrind → no-rust build → check-tidy →
`make check` on the real tree (`arora-finalcheck.service`, gated on
`seq.done`).

Both audit trees were re-synced to post-merge HEAD (31 changed files —
the rust-work and ui-work merges landed while the pipeline was staged,
so the audit covers merged rustcore/engine/container code, not a stale
snapshot).

## Tool inventory

| Tool | Status |
|------|--------|
| g++ -fanalyzer (GCC 13.3.0) | ran — pass 1 |
| clang++ --analyze (LLVM 18.1.3) | ran — pass 2, 253 unique TUs |
| clang-tidy 22.1.8 | user-local pip wheel (~/venvs/aqt) — new `.devin/check-tidy.sh` harness |
| clazy | NOT available (no prebuilt binary, no sudo) — documented gap, same as STAT01 |
| ASan+UBSan (clang, CONFIG+=sanitize) | via check-sanitize.sh |
| LSan | via check-leaks.sh (ARORA_SANITIZE_LEAKS) |
| valgrind memcheck | bounded: 3 smallest non-WebEngine tests |

## (a) Static analysis

**gcc -fanalyzer:** 402 in-tree findings (was 353 at STAT01 — growth is
new code, not new bug classes). Six warning families, all previously
classified Qt-blindness FPs:

- 228 `-Wanalyzer-possible-null-argument` — `operator new` modeled as
  possibly-NULL; C++ new throws, never returns null. FP.
- 188 `-Wanalyzer-use-of-uninitialized-value` — every instance names
  `'<unknown>'` (Qt implicitly-shared-class modeling). No attributable
  in-tree site. FP.
- 53 `-Wanalyzer-null-argument` — nullptr passed to Qt APIs that
  accept it by contract (disconnect, setDevToolsPage, etc.). FP.
- 34 `-Wanalyzer-possible-null-dereference` — same null-new modeling. FP.
- 22 `-Wanalyzer-malloc-leak` — std::function/Qt-parented ownership the
  analyzer can't see (async runJavaScript/findText/toHtml sinks,
  BookmarkNode tree ownership). FP.
- 13 `-Wanalyzer-null-dereference` — QPointer/connect-context guard
  loss; every site hand-verified guarded (see below).

**Exclusion:** `src/main.cpp` (11,996 lines) exceeds `-fanalyzer`
memory bounds — a single cc1plus passed 11 GB RSS and was OOM-killed on
four attempts. Its object is built without the flag; the TU is still
covered by clang --analyze, clang-tidy, and the sanitizer runtime
passes. Noted honestly rather than silently skipped.

**New-code sites hand-verified (all FP):**
- `devtoolswindow.cpp:60` — `window->m_view` deref; m_view set in ctor,
  s_window only assigned post-construction. `page` null-guarded at :48.
- `readermode.cpp:268` — `self->` after `if (!self) return;` — QPointer
  guard-loss. :230/:284 std::function leaks — Qt-owned async sinks.
- `rustdownloadengine.h:104-106` — inline member getters; `this`-null
  modeling.
- `bookmarknode.cpp:211` — `new BookmarkNode` in materializeChildren is
  appended to `m_children` the same iteration; nothing can bail. FP.
- `browsertheme.cpp:214`, `toolbarsearch.cpp:532` — possibly-null-new.
- `qrcodegen.hpp:46` — vendored Nayuki QR lib; `__dest` memcpy dest
  modeling. FP (also upstream code, not ours to patch).
- `webview.h:91/109/119`, `webview.cpp:964-965` — `this`-null +
  QPointer-guarded lambda sinks. FP.
- `sandboxmanager.cpp:74/105`, `rustdownloadengine.cpp:152-153`,
  `adblockrustengine.cpp:176/179` — `'<unknown>'` uninit family. FP.

**clang --analyze:** 8 in-tree findings + 1 Qt-header:

- `main.cpp:934` `cplusplus.NewDeleteLeaks` — **REAL (benign)**:
  ARORA_AUDIT_WIRE smoke harness allocated a QWebEngineView
  unconditionally, then abandoned it when the "webview" wire path built
  a real WebView instead. FIXED (a645851 — allocate lazily in the else
  branch).
- `xbelreader.cpp:210` NewDeleteLeaks — `folder` is `new BookmarkNode
  (Folder, parent)`; the ctor registers with the parent — Qt tree
  ownership the analyzer can't see. FP.
- `useragentmenu.cpp:90` NewDeleteLeaks — `new QActionGroup(this)` is
  QObject-parented. FP.
- `cookiejar.cpp:108` NullDereference — `this` deref inside the
  cookieFilter lambda; store-invoked while the jar lives. FP.
- `adblockpresetsdialog.cpp:77`, `tabbar.cpp:421`,
  `tabwidget.cpp:1990/2813` CallAndMessage — possibly-null-new /
  container-singleton / guarded-pointer modeling. All hand-checked: the
  pointers are constructor- or factory-sourced and non-null on every
  path (makeNewTab* always returns a fresh WebView; currentTab on a
  fresh window is never null). FP.
- qt6 qsharedpointer_impl.h `cplusplus.NewDelete` — upstream Qt header
  noise, not ours. Documented.

**clang-tidy:** results appended when the pipeline reaches it (runs
last, serialized — .devin/check-tidy.sh, checks
`bugprone-*,cert-*,clang-analyzer-*`, deduped one run per canonical
source file).

## (b) Memory/UB — ASan+UBSan+LSan

Instrumented build `/tmp/arora-san.2G69yK` (no-rust config, post-merge
sources), clang ASan+UBSan, isolated HOME, offscreen:

**Suite:** ~67 test programs ran; 3 aborted on ONE upstream crash —
`AddressSanitizer:DEADLYSIGNAL SEGV getenv()` inside
libexpat→libfontconfig→libQt6WebEngineCore on a Chromium
ThreadPoolForeg worker during `QWebEngineProfile` construction
(tst_engineadapter, tst_ToolbarSearch, tst_TorManager). Read address
0x0029000076de is a torn `environ` entry — Chromium rewrites environ
during profile/zygote setup while fontconfig parses on the worker
thread; glibc getenv-vs-concurrent-setenv is a documented POSIX UB
class, exposed here by ASan's interceptor. UPSTREAM — not Arora-owned
(18 frames deep in uninstrumented system libs); same fontconfig/expat
family MEM02 already tags upstream for leaks. All other programs that
completed reported `0 failed`.

**Real finding (fixed):** `tst_tormanager.cpp` circuitInfo() —
heap-use-after-free: `const TorCircuit *built` pointed into the
`QList<TorCircuit>` temporary `manager.circuits()` returned; the temp
dies at range-for scope end and QCOMPAREs then read a freed QString
member. Fixed by copying the TorCircuit value (5b05025); test passes
uninstrumented.

**Smokes (18 run):** 15 PASS (incl. download, cookie, history,
bookmarks, search, settings, autofill, find, adblock, extension-otr,
browser, app, quit-after-load). Tolerated non-zero:
- nam-smoke + extension-smoke: exit 134 = the same upstream getenv
  SEGV (ASan abort) — profile-init path.
- session-smoke: FAIL "tab-state blob header" — STALE smoke, not a
  bug: TabWidget::saveState writes format v4 (container/group tails);
  the smoke asserted tversion==1. Fixed to accept v1..v4 (5b05025);
  re-verified PASS on the uninstrumented build.
- source-smoke: FAIL raw compare — Qt 6.12 toHtml() injects
  `<head><meta name="referrer" content="strict-origin"></head>` into
  serialized DOM; smoke expected verbatim fixture bytes. Fixed by
  tolerating an engine-injected head (5b05025); re-verified PASS
  (highlighter/raw/fallback all green).
- ua-smoke live-check SKIP (offline).
- adblock-rust-smoke SKIP (no-rust config by design).

LSan phase + bounded valgrind: results appended when phases complete.

## (c) Harness re-runs

check-sanitize.sh result: suite rc=1 (three upstream aborts above),
10 deduped sanitizer reports written to `.devin/SANITIZER.md` —
breakdown: 4× getenv SEGV (upstream), 1× tormanager UAF (fixed), rest
same-class dedupes. check-leaks.sh / valgrind pending in pipeline.

## (d) Regression

PENDING — `arora-finalcheck.service` runs `make -j4 check` on the real
tree after seq.done; the no-rust tree build+suite is driver phase 5.

## Fixes landed this task

- `50b9e73` — omnibox: restore SRCH07 no-usable-engine degradation on
  the rust path (the audit's regression gate caught a real merge
  regression: `guessUrlFromStringRust` emitted `http://word/` instead
  of falling back to the DuckDuckGo engine).
- `a645851` — bareView leak fix (clang NewDeleteLeaks, real benign).
- `ce7f653` — .devin/check-tidy.sh harness (clang-tidy 22.1.8 pip
  wheel; STAT01's "unavailable" note resolved).
