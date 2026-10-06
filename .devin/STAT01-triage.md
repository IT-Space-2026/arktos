## Triage (STAT01)

Every in-tree finding from both analyzers was examined individually.
**Zero actionable bugs were found.** All findings fall into five
false-positive families; each is justified below. The analyzers run
over the whole ported tree (src/, autotests/, tools/ — 294 unique
translation units on the clang pass, every TU on the GCC pass).

### GCC `-fanalyzer`

GCC 13's analyzer is not Qt-aware: it cannot see QObject parent
ownership, `QPointer` weak guards, `connect()` context-object
auto-disconnect, or implicitly-shared value semantics. That accounts
for every finding.

- **`use-of-uninitialized-value` (5,763 raw)** — the noisiest GCC 13
  checker. Every in-tree instance names the value `'<unknown>'` (the
  analyzer cannot attribute it — Qt implicitly-shared/QMetaType
  internals); every *named-variable* instance is inside Qt headers
  (`qhash.h`, `qmetacontainer.h`, `qarraydatapointer.h`, ...).
  Suppressed: no attributable in-tree site.

- **`possible-null-dereference` / `possible-null-argument` (1,092
  raw)** — all in-tree instances are `dereference of possibly-NULL
  'operator new(...)'` / `use of possibly-NULL 'operator new(...)'`.
  C++ `operator new` throws `std::bad_alloc` on failure and never
  returns null — GCC 13 models the non-throwing path anyway
  (documented FP, CWE-690). Suppressed.

- **`malloc-leak` (179 raw)** — the 3 unique in-tree sites are
  `std::function` type-erasure payloads handed to Qt async APIs:
  `browsermainwindow.cpp:1436` (`page->toHtml(callback)`),
  `webviewsearch.cpp:80` (`page->findText(..., callback)`),
  `tst_adblockrequestinterceptor.cpp:187` (WebEngine callback). Qt
  takes ownership of the callable and destroys it; the analyzer cannot
  see the sink. Suppressed.

- **`null-dereference` / `null-argument` (130 raw; 15 unique in-tree
  sites)** — all are the `QPointer`-guard pattern the analyzer cannot
  track through Qt's weak-pointer internals. Verified by hand:
  - `network/fileaccesshandler.cpp:126-132`,
    `adblock/adblockschemeaccesshandler.cpp:82` —
    `QPointer<QWebEngineUrlRequestJob> job`, guarded by
    `if (!job) return;` at function top.
  - `downloadmanager.cpp:554,589,622,628,638` — `m_download` QPointer,
    every use guarded (`m_download ?`, `if (m_download)`, `&&`).
  - `locationbar/locationbar.cpp:97,142` — `m_webView` guarded.
  - `sourceviewer.cpp:129,135,148` — `QPointer<SourceViewer> self`
    derefs: the lambdas are connected with `this` as the context
    object, so Qt drops them when the viewer dies; the `toHtml`
    callback (no context available) is explicitly `if (self)`-guarded.
  - `webview.h:84,99` — inlined `webPage()`/`progress()` bodies flagged
    at QPointer-guarded call sites (same guard-loss).

  Zero findings from the exploitable classes
  (`use-after-free`, `double-free`, `out-of-bounds`, `file`, `tainted`,
  `fd-*`, `unsafe-call-within-signal-handler`) anywhere in the build —
  those checkers were enabled and silent.

### clang `--analyze` (6 findings)

- `useragent/useragentmenu.cpp:90` `cplusplus.NewDeleteLeaks`
  `actionGroup` — `new QActionGroup(this)` is parented; Qt owns it. FP.
- `bookmarks/xbel/xbelreader.cpp:188` `cplusplus.NewDeleteLeaks`
  `folder` — `BookmarkNode(Folder, parent)`'s ctor calls
  `parent->add(this)`; ownership transfers into the node tree. FP.
- `network/cookiejar/cookiejar.cpp:102` `core.NonNullParamChecker` —
  `m_policy.block` is a plain `QStringList` member of a `this`-captured
  lambda; the member cannot be null and the jar is qApp-owned so it
  outlives the cookie store that invokes the filter (MIG03's QPointer
  design covers the opposite direction). FP.
- `autotests/dialogs/tst_dialogs.cpp:464` `core.CallAndMessage` —
  `custom->trigger()` sits behind `QVERIFY(custom)`; QVERIFY returns
  from the test on failure. FP.
- `autotests/downloadmanager/tst_downloadmanager.cpp:270`
  `core.CallAndMessage` — same QVERIFY guard pattern on
  `tryAgainButton`. FP.
- `autotests/adblock/adblockmanager/tst_adblockmanager.cpp:167`
  `deadcode.DeadStores` — the assigned value is intentionally unused;
  the `customRules()` call itself is the assertion (a second call must
  not re-emit signals). Benign.

### Environment notes

`clang-tidy`, `cppcheck`, and `scan-build` are not installed and cannot
be (no sudo, no network package installs) — noted per task
requirements. The sweep is therefore `g++ -fanalyzer` (GCC 13.3.0) and
`clang++ --analyze` (LLVM 18.x) only.

Hardening note for future runs: `make check-static` rebuilds pass 2
from scratch on every invocation (clang emits no reusable artifacts);
`ARORA_STATIC_BUILD=<dir>` resumes a pass-1 build after an
interruption, which matters here because a single `-fanalyzer` TU
peaks near 900 MB RSS and the task loop caps the tree at 2048 MB —
hence the `-j1` default for pass 1.
