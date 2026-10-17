## Triage (STAT02)

Every NEW finding from the STAT02 deduped list
(`.devin/STAT02-findings.txt` — 21 GCC `-fanalyzer` + 1 clang
`--analyze`) was examined individually. **Zero crashes/leaks were
confirmed.** One honest hardening gap was found during the
mandated priority-site review and fixed (UpdateChecker, below);
every analyzer finding itself is a false positive in the same two
Qt-blindness families STAT01 established.

### GCC `-fanalyzer` — `null-argument` / `null-dereference` (20 sites)

All twenty are the QPointer guard-loss family: the analyzer cannot
see that `QPointer` zeroes itself, so a null-check upstream is
"forgotten" at the deref. Verified by hand:

- `engine/enginetab.h:57` (`EngineTab::page`) — flagged from
  `statusbarwidgets.cpp:323`, where the call is
  `m_tab ? m_tab->page() : nullptr` behind a `QPointer<EngineTab>`.
- `engine/webenginebackend.cpp:593,599,609` — `livePage` is a
  `QPointer<QWebEnginePage>`; every flagged call sits behind
  `if (!livePage) return;` / `if (livePage)` inside the lambdas.
- `locationbar/sitepadlock.cpp:299,344` — `m_webView` (QPointer)
  ternary-guarded: `m_webView ? m_webView->url() : QUrl()`.
- `pdfview.cpp:796` + `pdfview.h:94` — `m_client->findText` sits
  behind `if (!m_client || !m_client->isReady() ...) return;`;
  the header finding is the inlined `isReady()` flagged at that
  same guarded call site.
- `printpreviewdialog.cpp:189` — `m_page->printToPdf` is past the
  `if (!m_page) { ...; return; }` early-out.
- `sourceviewer.cpp:171,173,281,302` — `self` is a
  `QPointer<SourceViewer>`; all four `self->showSource` calls are
  behind `if (!self ...)` / `if (self && ...)` guards.
- `tabwidget.cpp:2619-2622,2840-2841,2854-2856` — `guard` is a
  `QPointer<WebView>`/`QPointer<QWebEnginePage>`; every use is
  behind `guard &&` / `if (!guard) return;` /
  `if (!guard || !guard->enginePage()) return;`.

### GCC `-fanalyzer` — `malloc-leak` (1 site)

- `vaultdialogs.cpp:357` — `std::function<int()>` type-erasure
  payload. `VaultUi::runWithProgress` takes the callable by const
  reference and invokes it **synchronously** (`const int status =
  op();`); the functor's storage is stack-owned by the caller and
  destroyed at end of statement. Same family as STAT01's
  `toHtml`/`findText` callback reports — the analyzer cannot see
  the sink. FP.

### clang `--analyze` (1 finding)

- `webview.cpp:824` `core.CallAndMessage` — `m_page->profile()`.
  `m_page` is a raw `WebPage*` assigned once in every WebView
  constructor (`new WebPage(this)` / `new WebPage(profile, this)`,
  parented to the view, never reassigned, `new` cannot return
  null). The analyzer explores the branch where the file's own
  defensive `m_page ? ... : nullptr` at line 748 implies
  reachability of a null field; it cannot. FP. (The inconsistent
  defensive guards elsewhere in the file are style, not evidence.)

### Priority-site review (per task spec — read, not analyzer-driven)

- **profilevault `rv_*` FFI boundaries** — clean: `rv_inspect`'s
  returned `char *` is `rc_string_free`d at both call sites
  (seal conf capture, `inspect()`); password `QByteArray`s pass
  `constData()`/`size()` pairs, never outlive the call;
  cached-password custody is `secureZero`d on drop; the progress
  trampoline context is a stack `Ctx` valid for the synchronous
  FFI duration; unseal is two-pass (MAC before plaintext) per the
  crate contract; `engine-hold` restore is lstat-guarded and keeps
  the hold on rename failure.
- **VSEC02 `ensureEngineStorageLayout`** — clean: entry-level
  `isSymLink` refusal (lstat, dangling links included via
  `Dirs|Files|Hidden|System` enumeration), `pathChainHasLink` on
  the destination chain, both-sides collision abort, empty
  Qt-materialized residue removed rather than migrated, orphaned
  persona dirs left in place loudly, cache-side links unlinked not
  descended.
- **ProfileSettings routing** — clean: group/root classification
  table with explicit `kGlobalExceptions`; `remove()` and
  `clear()` drop both stores so the read fallback cannot resurrect
  a key; array reads require the `/size` marker before binding the
  profile store.
- **UpdateChecker/UpdateDialog** — one gap found and **fixed**:
  the reply body was `readAll()`ed and JSON-parsed with no size
  bound. A hostile/wedged endpoint could grow the reply buffer
  unboundedly (only the 15 s transfer timeout limited it).
  `check()` now sets an 8 MB `setReadBufferSize` and
  `replyFinished()` rejects payloads over 1 MB before parsing;
  regression leg `oversizedBody` added to
  `autotests/updatechecker` (1.2 MB body → `Status::Error`).
- **vaultdialogs.cpp:357 area** — the runWithProgress modal wait
  is synchronous; cancel only dismisses the dialog (the op still
  lands) — documented in the function comment; no bug.

### clang-tidy 22.1.8 (slice b)

331 unique TUs analyzed against the STAT02 build tree
(`ARORA_TIDY_BUILD=/tmp/arora-stat.un2m6A`, checks
`bugprone-*,cert-*,clang-analyzer-*`): 773 in-tree findings —
218 in generated `.moc`/`.ui` sources (excluded), 555 in real
sources. The bulk re-confirms AUDIT01's documented idiom families
(narrowing-conversions, swappable-parameters, implicit-widening,
missing-default-case, enum-init consistency, shadowing,
unused-lambda-capture, macro-parentheses, multi-level-pointer-
conversion, copy-constructor-init, branch-clone, cert-oop54,
return-const-ref, dcl50/dcl54/dcl59, forwarding-reference,
inc-dec-in-conditions, crtp-accessibility, Padding) — no new
defects in any of them. Focused triage of everything else:

**Fixed (real, small):**

- `main.cpp:390/399` `unix.StdCLibraryFunctions` — the tor/webdriver
  socket pump passed `fds[1-i].fd` to `::shutdown`/`::send` without
  checking whether the peer direction had already closed (fd = -1).
  Behavior was benign (EBADF fell out as flow control) but the
  analyzer was right — now guarded (`to >= 0` shutdown, `to < 0`
  ends the pump).
- `profilevault.cpp:71` `bugprone-casting-through-void` —
  `secureZero` took the two-hop `static_cast<void *>` route;
  switched to a direct `reinterpret_cast<volatile unsigned char *>`
  (the cv-add is legal there) — same semantics, cleaner cast.
- `updatechecker.cpp` — see the UpdateChecker entry above
  (response-size bound + `oversizedBody` autotest leg).

**False positives / documented idiom (per-site):**

- `cookiejar.cpp:114` `core.NullDereference` — `this` captured by
  the IO-thread cookie filter; `this` is a constructed qApp-owned
  singleton, never null. FP.
- `adblockpresetsdialog.cpp:77` `core.CallAndMessage` —
  `group->layout()`; `group` is null only when a preset's category
  equals the empty `currentCategory` sentinel — impossible: every
  category in `AdBlockPresets::all()` is a non-empty `tr()`
  literal. FP.
- `tabbar.cpp:639` `core.CallAndMessage` — `tabView->url()` is
  behind the transitive guard (`tabScheme` is non-empty only when
  `tabView` was non-null). FP.
- `tabwidget.cpp:3096,4434` `core.CallAndMessage` — `makeNewTab*`/
  `getView` factory results; AUDIT01's same-file family — the
  factories always return a fresh WebView. FP.
- `webview.cpp:824` `core.CallAndMessage` — same finding as the
  clang `--analyze` hit above; ctor-initialized `m_page`. FP.
- `xbelreader.cpp:210` `cplusplus.NewDeleteLeaks` — `folder` is a
  `BookmarkNode(Folder, parent)` whose ctor re-parents into the
  node tree (same site as STAT01's :188 report, shifted). FP.
- `useragentmenu.cpp:98` `cplusplus.NewDeleteLeaks` —
  `new QActionGroup(this)` is parented (STAT01 same site). FP.
- `history.cpp:496,744` `optin.cplusplus.VirtualCall` —
  `setSourceModel` in the ctor calls the class's own override by
  design (the override then chains to
  `QAbstractProxyModel::setSourceModel`). Intentional.
- `searchbutton.cpp:35` `optin.cplusplus.VirtualCall` — calls its
  own `sizeHint()` deliberately for the minimum size. Intentional.
- `flowlayout.cpp:43` `optin.cplusplus.VirtualCall` — `takeAt` in
  the dtor drains the layout's own items. Intentional.
- `bidipanel.cpp:324` `incorrect-roundings` — `int(ms + 0.5)` is a
  deliberate round-half-up on a non-negative display value.
- `bidipanel.cpp:977` `security.FloatLoopCounter` — waterfall tick
  positions are inherently double; the step is chosen so the loop
  count is bounded by design.
- `browsermainwindow.cpp:2937` `assignment-in-if-condition` —
  `!(widget = qobject_cast<...>(object))` is a deliberate
  short-circuit capture. Idiom.
- `main.cpp:18791,19129` `suspicious-missing-comma` — intentional
  adjacent-literal concatenation building multi-line PDF fixture
  objects (AUDIT01 same family).
- `main.cpp:390,399` — FIXED above (was real).
- `main.cpp:4713` `exception-escape` — `main` may propagate
  `std::bad_alloc`; standard. Idiom.
- `cert-err33-c` (35 sites) — unchecked `fflush`/`fwrite`/`fprintf`/
  `signal` returns in smoke-harness output, the pdf/download
  worker's stdout pipe, and crash/termination signal handlers.
  Every site is best-effort output or handler-restore where the
  return value cannot change the outcome. Family document.
- `opensearchreader.cpp:83,84` + `securestore.cpp:65`
  `unused-const-variable` — the constants are used (SENG02's Alias
  bounds at opensearchreader.cpp:245-246, kHeaderSize later in the
  same TU); stale-AST artifacts identical to AUDIT01's finding. FP.
- `clang-diagnostic-unused-const-variable`/`unused-lambda-capture`
  — same dead-capture family AUDIT01 documented in the smoke
  harnesses.

### Dead-code sweep (slice c)

Method: header-declaration↔call-site count across `src/` +
`autotests/` + `tools/` (every non-virtual member name resolved
against the whole tree — `&Class::name` connects, `emit name`,
string connects and JS-driven `Q_INVOKABLE`s included), `#if 0`/
commented-block grep, settings setValue↔value literal audit,
profile/vault menu-action wiring review.

**Removed (confirmed dead):**

| Item | Evidence |
|------|----------|
| `ProfileVault::hasSessionPassword` | decl+impl, zero callers — `sessionPassword()` covers the query |
| `AdBlockRule::setException` | decl+impl, zero callers — exception state is parse-time only; `setEnabled` (its sibling) IS used by autotests |
| `ExtensionBackgroundHost::handshakeDone` + `m_handshakeDone` | getter never called AND the member was never assigned — vestigial both directions |
| `TorManager::setNewNymWindowForTesting` | `#ifdef AUTOTESTS` seam no test calls (siblings `setStateForTesting`/`setCircuitsForTesting` are used, kept) |

**Reviewed, kept (with reason):**

- Signals emitted but currently unsubscribed —
  `ProfileVault::profileLocked/profileUnlocked`,
  `ExtensionShimBackend::actionStateChanged/noteRecorded` —
  meta-object surface (QSignalSpy/moc consumers, public notification
  API), emitted correctly.
- `Q_INVOKABLE` bridges reached from injected JS, invisible to a
  C++-only scan: `ChromeBridge::reportError` (exthost.js/extshim.js),
  `PipPlayerBridge::notifyReady/notifyError/reportState`,
  `JavaScriptAroraObject::currentEngineNameChanged`
  (startpage.js channel).
- Overrides/framework callbacks: `TabBar::tabRemoved`,
  `SourceHighlighter::highlightBlock`, `FlowLayout` size-hint
  overrides, `ReaderMode::setPreference` (moc member).
- `rustcore.h` `rc_*` decls without C++ callers — the header mirrors
  the crate ABI; unused entries are FFI export surface for sibling
  lanes/tests, not debris.
- `trie_p.h::walkTo` — template code used inside its own header
  (scan artifact).
- `#if 0` blocks: `networkcookiejar.cpp:235` (upstream
  Firefox-parity rationale, kept per AUDIT02) and
  `tst_languagemanager.cpp:150` (intentional QSKIP stub).
- Stale settings keys: **none** — every same-literal "written,
  never read" key resolves through group prefixes
  (`privacy/fingerprintProtection`, `MainWindow/iconTheme`,
  `downloadmanager/*`, `tabs/*`, etc.).
- Dead menu actions: **none** — profile/vault/update menu actions
  all carry `triggered` connects.

### Environment

`clang-tidy 22.1.8` (user-local `~/venvs/aqt`) ran per slice (b);
`cppcheck`/`scan-build` remain unavailable (no sudo).
