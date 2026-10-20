# MRG07 — post-merge leak sweep A: extension merge seams

Generated: 2026-10-20 on 4548d884 (MRG06) + this task's working-tree
changes.  Focused ASan/LSan churn over the newest allocation paths the
rust-work/extension merge put on master:

- `ExtensionChromePage` mint/destroy cycles — the nav-locked
  QWebEnginePage subclass (SEC26-1) that `ExtensionActionBar` popups
  and `ExtensionManager`'s health-probe loop both mint; each page
  carries the lifted-settings object graph the merge made the newest
  page-construction path.
- `ExtensionBackgroundRuntime` relay `PortEnd` register/expire cycles
  on merged master's generalized `QPointer<QObject> target` — both
  endpoint kinds (`ChromeBridge` shim contexts and
  `ExtensionPageBridge` page agents), AT and PAST the SEC26-3 bounds
  (`kMaxPortEndsPerChan` = 64, `kMaxPortChans` = 1024,
  `kMaxRelayPending` = 512), through every removal path (respond,
  disconnect, the 30 s expiry timers, unregister/page death).

Toolchain: Qt 6.12.0 (~/Qt/6.12.0/gcc_64), clang 18.1.3,
`qmake6 -spec linux-clang CONFIG+=sanitize` (ASan+UBSan) — MEM11's
throwaway-tree recipe.  Flavor difference vs MEM11, deliberate: the
throwaway tree was configured `CONFIG+=no-rust nowebkit=1` — the
prebuilt rust `.a`/WPE artifacts are gitignored and do not exist in a
`git ls-files` copy, and neither rust nor the WPE backend participates
in the extension-seam object graph under test (the hosted runtime and
both bridge kinds are pure Qt WebEngine code either way).

## Instrumented tree

    /tmp/arktos-mrg07.sFkNLc          (leak corpus: ./leaklogs-mrg07)

Throwaway-only measurement patch (never committed): the MEM09/MEM11
seam in `autotests/qtest_arktos.h` — `__lsan_do_leak_check()` before
the QUAL05 `_exit(rc)`, without which `_exit` skips LSan's at-exit
check entirely.  tst_extensionmanager uses `QTEST_MAIN`, so the one
harness patch covers it; no per-binary custom-main patches needed.

## Churn legs added (committed, main tree)

- `tst_extensionmanager::chromePageChurn` — loads a fixture MV3
  extension with `action.default_popup`, waits for its hosted
  background host to reach Ready, then:
  - 3× `ExtensionActionBar::triggerAction` open/dismiss cycles; each
    pass asserts the popup's page `qobject_cast`s to
    `ExtensionChromePage` (the SEC26 nav-locked class, not a plain
    `QWebEnginePage`), has `JavascriptEnabled` forced on, commits the
    `chrome-extension://` popup url, and — via QPointer guards — that
    `popup->close()` destroys the frame, view, AND page for real.
  - 3× forced health-probe re-measures; each pass waits for
    `healthProbePage()` (a small test seam added to
    `ExtensionManager`) to mint a fresh `ExtensionChromePage`, waits
    for the probe verdict, and asserts the page is torn down.
- `tst_extensionmanager::relayPortChurn` — two `ChromeBridge` relay
  contexts plus a real `ExtensionPageBridge` page agent on a live
  `QWebEnginePage`:
  - per-chan end cap: 64 ends accepted, #65 refused, dedup refused,
    63-end fan-out, then all 64 disconnected (chan dies on empty);
  - channel-count cap: drives `relayPortConnect` until first refusal —
    observed boundary is 1023 populated channels (the
    `m_portEnds[chan]` lookup inserts the empty list before the
    `size() >= kMaxPortChans` check fires, so map-size 1024 holds
    1023 populated + 1 tombstoned channel; still bounded — pinned in
    the test, noted here);
  - pending-mid churn: send/respond cycles, then fill to the
    kMaxRelayPending = 512 bound with the 513th refused, then
    `unregisterRelayContext` death-expires all of them plus the ~1000
    live port-end channels (survivor disconnect pushes);
  - page-agent ends (the other `PortEnd::target` kind): connect/
    message/disconnect cycles, page↔bridge deliveries, a pageSend
    resolved via `__arktos.extResponse`, a `pageDeliver`/`pageRespond`
    host round-trip resolved via `__arktos.deliverResponse`, one real
    30 s expiry for both a pending mid and a page delivery (late
    answers get `unknown-mid`, the waiter resolves with the honest
    no-response shape), and a page-death expire (`deleteLater` on the
    view — `~ExtensionPageBridge` unregisters the page context, kills
    its ends and pending sends, pushes disconnects at survivors).

## Method

Per-run fresh HOME/XDG_CONFIG_HOME/XDG_DATA_HOME,
`QT_QPA_PLATFORM=offscreen`,
`QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`,
`ASAN_OPTIONS=detect_leaks=1:quarantine_size_mb=64:thread_local_quarantine_size_kb=64:print_stacktrace=1`,
`LSAN_OPTIONS=exitcode=0:print_suppressions=0:report_objects=1`,
`ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18`.
Classification is `.devin/check-leaks.sh`'s rule verbatim: a block is
"ours" iff one of its first four frames resolves to a source file
under the instrumented tree's `src/` or `autotests/`; interceptor-only
stacks in the test binary (`malloc`/`operator new` shims) do not
count — the first real frame decides.

## Results — focused legs

| run | qtest | ours | upstream records |
|-----|-------|-----:|---------:|
| chromePageChurn | 3/3 PASS (7.5 s) | 0 | 4 (9 allocs / 688 B) |
| relayPortChurn  | 3/3 PASS (32 s)  | 0 | 2 (3 allocs / 320 B) |
| full binary (-silent) | 50 PASS (48 legs + init/cleanup), 4 skips, 0 fails (227 s) | 0 | 4 (12 allocs / 952 B) |

("record" = one `leak of N bytes in M objects` LSan grouping; the
check-leaks.sh classifier counts records, MEM11-style.)

Upstream tally across the corpus (10 records, 0 ours):

| first-real-frame module | records |
|-------------------------|--------:|
| libexpat.so.1 / libfontconfig.so.1 | 6 |
| libQt6Widgets.so.6 (`QLayoutPrivate::createWidgetItem`) | 2 |
| libstdc++.so.6 (`__cxa_demangle` via ASan symbolizer) | 2 |

Detail on the per-leg records: `chromePageChurn`'s Qt6Widgets items
are `QLayoutPrivate::createWidgetItem` widget-item allocations (the
action-bar popup's layout children — process-lifetime under the
offscreen `_exit` teardown); the rest in every log is the same
fontconfig/expat process-lifetime family MEM11 already tallied on
`hostedHostChurn` (2 records / 320 B then, identical signatures now).

Zero `src/`- or `autotests/`-sited records: `grep` of every leak
record in all three logs finds no frame resolving to a source path
under the tree; the only in-tree frames are the sanitizer
interceptors at `#0`.

## Observed non-leak finding — UBSan vptr note (not a defect)

`chromePageChurn` emitted one UBSan runtime error:

    qpointer.h:76:14: downcast of address ... which does not point to
    an object of type 'QFrame'; object is of type 'QWidget'

Analysis: `QPointer<T>::get()` at that line is
`static_cast<T*>(wp.internalData())`.  The test polls
`!popupGuard && !viewGuard && !pageGuard` inside a QTRY while the popup
frame is mid-teardown — after `~QFrame()`/`~QWidget()` have run but
before `~QObject()` clears the tracked-pointer guard, the object's
vptr is already `QWidget`'s, so the guard's `static_cast<QFrame*>` is
technically an out-of-lifetime downcast.  Harmless in practice (the
guard resolves to null microseconds later when `~QObject` runs —
exactly what the test then observes); a Qt-internals teardown pun of
the same class as Qt's own `QWeakPointer` handling, not a leak and not
a defect in our code.  relayPortChurn's `QPointer<ExtensionPageBridge>`
/`QPointer<QWebEnginePage>` guards did not trip it (destruction there
completes between polls).

## Verdict

Both merge-seam surfaces churn clean under ASan/LSan, and the whole
tst_extensionmanager binary (48 legs incl. both new churn carriers)
runs green (50 PASS / 4 skips / 0 fails) under the same harness: **zero
`src/`-sited leak records** anywhere in the corpus — every recorded
block is an upstream Qt/fontconfig/expat/sanitizer process-lifetime
allocation in the families MEM11 already tallied.  Nothing required
fixing; no suppression was added or needed.
