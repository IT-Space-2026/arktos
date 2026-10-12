# UIAUDIT03 — Post-queue code-quality sweep (ui-work)

Scope: the ui-work delta since UIAUDIT02 (`f4adff2..HEAD` — 22 feature
commits, 44 src files, +4498/−429) covering TABS04/05, CONT07/08,
DLACC06, PREFUI04–08, ADDR01/02, THEME02, TPREV01/02, ADBLK01, SIDE03,
BKEX01, TRUST06/02, LANG01.

## (a) DEAD CODE — clean, nothing removable

- settings.ui: 64 new objectNames since UIAUDIT02 — every unreferenced
  one is a layout, spacer, titled groupbox, or static label (same
  verdict classes as UIAUDIT02's census). `alternatingRowColors` and
  `standardButtons` are .ui *property* names, not widgets.
  addressdialog.ui: `buttonBox` is .ui-connection-wired and read in
  `AddressDialog::updateSaveButton()`; all other unreferenced names are
  labels/layouts.
- New headers: every added `m_*` member has ≥2 uses outside its
  declaration; every new public method has a caller (AddressDialog,
  EnergySaver, HtmlBookmarksWriter, SidebarPositionPicker, autofill
  `maybeSaveAddress`/`markAddressUsed`/`promptToSaveAddress` —
  internally wired; tabwidget pinned/undo APIs — used + tested).
- Named suspects verified live:
  - BookmarksDialog (`Ctrl+Shift+B`, `m_bookmarksShowAllAction`) is
    still reachable by design — HIST02's host mechanism only converted
    History; the bookmarks popup remains the tracked follow-on.
  - TPREV01/02 preview plumbing (`tabPreviewsEnabled`,
    `reloadTabHoverPreviews`, `hideTabPreview`, `m_previewTimer`) all
    wired.
  - downloaditem.ui card widgets all referenced (DOWN01/DOWN02).
  - No superseded nav-list sizing left post-PREFUI06/07/08 —
    `chromeIconExtent`/`navIconExtent` are the only extent sources.
- `BrowserApplication::siteTrustEnabled/Lookalike/ThreatlistEnabled`
  have no in-tree consumer *by design*: they are the canonical gates
  pre-wired for the main lane's TRUST03 lookups (TRUST02 notes).  Kept —
  removing them would strand the prefs surface; documented under (d).

## (b) DUPLICATION — fixed

- `TabPositionPicker` (278 lines) vs `SidebarPositionPicker` (270
  lines): ~85% identical — tile chrome (selection wash/frame, check
  badge, label, focus rect), mouse/key/focus handling, and the whole
  picker shell (setCurrentIndex / tileActivated / stepTileFocus /
  layout / focus proxy) were copy-pasted.
  - Extracted shared `PositionPicker` + file-local `PositionPickerTile`
    (new `src/positionpicker.{h,cpp}`); each concrete picker now only
    supplies a `PreviewPainter` for its miniature interior
    (`paintTabStripPreview`, `paintDockPanelPreview`) plus labels,
    display-order mapping, and accessible description.
  - Public surface preserved exactly: `currentIndex()`, `count()`,
    `setCurrentIndex` slot, `positionChanged` signal (all inherited),
    persisted index semantics unchanged (tab display order {0,2,3,1},
    sidebar identity {0,1}).
  - Net: −548 → +330 lines across the three pairs; pickers shrink to
    ~80 lines each.
- Settings read/write blocks: the PREFUI04/05/06/TRUST/SIDE03/LANG01
  additions each read distinct keys with distinct widgets — no
  copy-pasted blocks found that would collapse without inventing a
  framework; left as-is.

## (c) PERFORMANCE — fixed

- `TabBar::initStyleOption` and `TabBar::tabSizeHint` each ran
  `qobject_cast<TabWidget*>(parentWidget())` twice per call when a tab
  was pinned (once inside `isTabPinned()`, once explicitly) —
  initStyleOption runs per tab per repaint.
  Hoisted to a single cast with identical semantics (verified: null
  parent ⇒ not pinned in both old and new code paths).
- Audit checked the named hot paths: recently-closed stack is a
  bounded QList (cap 25) with O(1) pin checks via `m_pinnedTabs`
  QSet; `normalizePinnedBlock`/`applyEnergyCaps` iterate tabs only on
  structural changes; EnergySaver hooks are singleton lookups on
  event paths, not paint paths; downloads sidebar list rides
  QSortFilterProxyModel (no manual row rebuilds); download progress
  keeps the 200ms per-item throttle.  No model-reset or
  sizeHint-in-paint offenders found.

## (d) SECURITY — verified, one documented gap (by design)

- BKEX01 export: `HtmlBookmarksWriter` HTML-escapes every interpolated
  string (title, url, desc, folder name) — no injection into the
  Netscape HTML output.  Export path is a user-driven file dialog.
- TRUST06/TRUST02: the three toggles persist correctly
  (`trust/enabled`, `trust/lookalike`, `trust/threatlist`, all default
  on) and the accessor gates exist — **but no consumer exists in this
  tree yet** (TRUST03's lookalike/threatlist lookups are main-lane
  work).  The toggles are currently inert-by-design; when TRUST03 lands
  it MUST route through the accessors or the checkboxes will lie.
  Reported, not fixable from this lane.
- ADBLK01: `storedEngine()` defaults to rust but `engine()` falls back
  to `NativeEngine` when `rustEngineAvailable()` is false — a saved
  'rust' pick on a no-rust build degrades to the native matcher, no
  dead option.  `setEngine` emits `rulesChanged` for live rebuild.
- ADDR01/02: autofill script injection is JSON-encoded
  (FORMS_JSON/ADDRESS_JSON placeholders — no string interpolation);
  only ONE MRU address enters the page world; `formSubmitted` re-verifies
  the reported URL against the bridge's tagged URL and drops forged
  reports; `maybeSaveAddress` whitelists member names, requires ≥2
  fields + postal evidence, dedups on name+street+zip, and only ever
  *prompts* (prefilled AddressDialog) — never silently stores.
  `usedAddressId` can only mark the address the page already received.
- CONT07/08: covered by CONT08's own adversarial audit commit
  (scheme gates, same-profile Referer, argv-only tor hand-off).

## Committed fixes

- positionpicker.{h,cpp} added; tabpositionpicker +
  sidebarpositionpicker rebased onto it ((b)).
- tabbar.cpp: single parent cast in initStyleOption + tabSizeHint ((c)).

## Verification

- `make -C src -j2` rc=0 (positionpicker + both pickers recompiled and
  linked; moc_positionpicker generated).
- `tst_settingsdialog tabPositionPicker sidebarPositionPicker` — PASS
  (tile counts, accessible names, click-to-commit, signal spies).
- `make check` — see task row / WORKLOG for final result
  (QT_QPA_PLATFORM=offscreen, QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu).
