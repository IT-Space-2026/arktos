# Devin autonomous task list — Arktos MW-SHELL LANE (branch mw-shell, worktree ../arora-mwshell)

Thin-UI queue driven by `.devin/taskloop-shell.sh`. Goal: shrink the Qt layer
to a real thin shell — widgets paint and forward, facade owns decisions and
state; and formalize the engine adapter contract so every engine is consumed
through the same surface with capability-driven UI.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting, `done` only after verifying.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- NEVER edit `.devin/Arora-Task.md` or other lanes' tables.
- Cross-lane deps: `needs:task:X` may reference another lane — check that
  lane's table (`../arora-mwbridge/.devin/Arora-Task-Bridge.md`,
  `../arora-mwdomains/.devin/Arora-Task-Domains.md`) before starting. If the
  dependency is still pending/in_progress, leave YOUR row `pending` and pick
  the next runnable row — never burn an attempt on a blocked dep.
- Append a one-line summary to `.devin/WORKLOG.md` when a task completes.
- Table rows are one-line summaries; the FULL spec lives in `### <id>`
  under `## Task details`. Keep cells short.
- ALL writes to this file MUST go through `.devin/taskfile.sh`
  (flock-serialized): `set <ID> field=…`, `append-detail <ID>` (stdin),
  `with-lock -- <cmd>`.
- Hard rule: UI thinning = move decision/state code behind facade; widgets
  keep painting, focus, input, menus, and engine-object ownership
  (QWebEngineView/Page stay on the engine side).

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| MS01 | Shell audit — business logic inside widgets + keep/thin table | done | 0 | audit: .devin/MW-SHELL.md — method-level thin/keep/thin-blocked:MDnn map for locationbar/webview/browsermainwindow/tabbar/tabwidget/settings/history/bookmarks; verified vs facade handles in arktos_core.h |
| MS02 | EngineAdapter contract doc — formalize EngineInterface surface + caps path | done | 0 | .devin/ENGINE-ADAPTER.md: 13-area contract mapped to EngineInterface/EngineTab surface, MB04 feature→adapter mapping table (MB04 pending in mw-bridge), measured 3-engine caps matrix + new-backend checklist; doc-only |
| MS03 | LocationBar thinning — suggestion/accept/navigate policy via facade | done | 0 | audit-verified: suggest/accept/navigate path already facade-routed — rc_classify_input verdict, ac_hist_suggest store-ranked rows (proxy pass-through), rc_ose engine pick/expand, ac_settings opt-ins (edits landed via MS04 commit); residual verdicts thin-blocked per MW-SHELL.md: MD02 scheme gate + no-MD-row flags for suggest-source pick/availability, scope->provider pick, branded start-page re-pick. verify: make -j1 clean no-op; tst_omnibox 88/88, locationbar 57+1skip, toolbarsearch 12, opensearchmanager 24, historyfiltermodel 37, searchlineedit 4, tabwidget 46+9skip; --search-smoke + --search-guess-smoke PASS offscreen |
| MS04 | WebView thinning slice 1 — context-menu + link-hover policy to controller | done | 3 | webviewcontroller.{h,cpp}: contextMenuModel owns ctx-menu entry set/order/enabled+disabled-reason, hoverStatusText the status verdict; webview.cpp renders the model (menu build/exec/strings stay widget), engine-adapter hover sink consumes same fn; env legs thin-blocked MD02/MB04 documented; verify: full rust build + --browser-smoke PASS + tst_webview 29/29 incl 11 ctx-menu cases |
| MS05 | WebView thinning slice 2 — zoom/find/print/devtools gating by capability | done | 0 | caps-driven chrome gating: ported MB04 caps slice (caps.rs+ac_engine_caps FFI, header v7, ffi tests, acprobe leg) + new EngineCaps Qt cache (facade row wins under rust, adapter flag fallback); devtools rows query "devtools" grade + re-arm on tabsChanged (engine-swap fix); engine-tab print refusal sourced from "pdf_viewer" grade, still refuses honestly; zoom stepping thin-blocked (no MD domain/facade surface), find needs no check (EngineTab lacks find verb). Verify: make -j1 clean; cargo caps tests 8/8; acprobe caps ok; browser/find/printpreview smokes PASS; tst_webview 29/29; devToolsSubmenu/printPaths/zoomAndFind PASS; chromeSlots SIGSEGV pre-existing (repros on HEAD file); engineswitch 1 fail pre-exists MS07 |
| MS06 | Capability-driven chrome — toolbar/menu/prefs items per engine caps | done | 0 | MB04 landed (bridge) — caps-driven chrome: reader gate via EngineCaps reader_mode in updateReaderState (parity-identical); new EngineCaps::capabilitySummary rollup on engine-indicator swap rows + settings renderEngineCombo tooltips + extensionsHintLabel scope line ("Extensions run on %1 tabs only"). Resolved keep/thin-blocked: pageUnavailableReason+swap locks (runtime presence/mode, caps cannot grade), sitepanel greys (WebEngine-bound managers, not engine caps). Verify: make -j2 clean; tst_engineswitch capsAnnotations PASS (28 pass/1 pre-existing indicatorToolbarSlot fail); tst_settingsdialog 59/59; reader legs 8/8; browser/settings/engine smokes PASS offscreen |
| MS07 | Settings dialog thinning — sections read/write via facade handles only | done | 0 | settings.cpp already ProfileSettings-routed end to end; swept residual raw stores: tabwidget close-group confirm, engineregistry defaultEngine, tranco/majestic/domainblock stamps now via ProfileSettings (majesticListUpdated added to global exceptions — SEC19 missed it). Tests that seeded profile-tier keys via raw QSettings rerouted to the facade. Verify: build OK, --settings-smoke PASS, settingsdialog 58/58, profilesettings 12/12, tabwidget 46+9skip, tabbar 31/31, trancolist 9/9, domainblock 14/14 (engineswitch indicatorToolbarSlot fail is pre-existing since 0f183058 moved ext strip out of splitter) |
| MS08 | Tab/session UI thinning — restore + per-tab state decisions via AcSession | done | 0 | restoring crash-guard moved to session.state via ac_session_*_restoring (BrowserProfile::setSessionRestoring/sessionRestoring, legacy-key upgrade read + retire, tor/no-rust keep ini); sessionStoreProfileIds + canRestoreSession verdict via ac_session_exists per bound handle; per-tab fields verified already ac_session-routed via manifest — remaining decision sites (snapshot content, restore order, lazy deferral, closed-tab stack) marked thin-blocked:MD08 in MW-SHELL.md; fixed pre-existing VSEC01 fallback-window sprouting in standalone runs (floor-smoke leg1). Verify: build clean; --session-smoke 8/8, --restore-smoke PASS, --session-floor-smoke 29/29, --sigterm-smoke 15/15 (all w/ ARKTOS_BUNDLED_EXTENSIONS_DIR=empty — bundled bitwarden welcome tab otherwise pollutes fresh-profile fixtures, pre-existing); tst_browserapp 11/11, profilewindow 14/14, profilee2e 23/23, tabwidget 46+9skip, profilesettings 12/12 |
| MS09 | Boundary doc + smoke parity — final thin-shell map + --*-smoke harness check | done | 2 | boundary map finalized in MW-SHELL.md (75 keep / ~67 thin / ~18 done rows; thin-blocked MD02/03/06/08/09 verdicts + MB04 caps flagged); all 85 standalone --*-smoke legs re-run headless, per-leg fresh HOME, rc+verdict recorded — harness fixes: adblock about:blank one-retry for the page-script snapshot race, download legs pin engine=builtin (rustdl default otherwise pre-empts the Chromium path they verify), autofill shim dedup; FAILs recorded honestly: extension-health (probe settle timeout — pre-existing/env, no lane diff on that path), xsleak (upstream fixture dead); crash=139 by design, download-refused tolerated, servo SKIP |
| MS10 | TabBar context-menu model — entry set/enabled to controller | done | 0 | tabbarcontroller.{h,cpp}: contextMenuModel(env) owns strip-menu entry set/order/enabled/checked/label-variant (reopen-enable, pin label, container rows+rules+private/tor gate, sleep/wake+free-memory, group rows); tabbar.cpp renders via addContextMenuEntry — build/exec/strings keep. env legs documented (rc_containers-backed registry; MS08-manifest per-run state; mode legs). Verify: build clean, tst_tabbar 32/32 incl new contextMenuModel matrix leg, tst_tabwidget 46+9skip, --browser-smoke PASS, --container-smoke PASS |

## Task details

### MS01
Audit `src/locationbar/`, `src/webview.cpp`, `src/browsermainwindow.cpp`,
`src/tabbar/`, `src/settings.cpp`, `src/history/` UI, `src/bookmarks/` UI.
For each method that *decides* something (URL accept, menu availability,
restore policy, suggestion source pick, permission default) record
file:line + verdict `thin` (move behind facade — domains lane provides
the modules; if the module doesn't exist yet mark `thin-blocked:MDnn`)
or `keep`. Output `.devin/MW-SHELL.md`.


AMENDMENT (slicing): if the audit finds thin-able decision sites not
covered by MS03-MS08, append one new pending row per widget cluster
(MS10+) via taskfile.sh with-lock/append-detail — one widget/method
group per row. Sites blocked on a pending domains module get
`needs:task:MDnn` in Notes.

### MS02
Document the adapter contract every engine backend must satisfy:
navigation control, load state events, devtools presence, profile
binding, download hooks, permission surfacing, scheme handling,
extension runtime surface (or honest none), caps reporting. Map each
to existing EngineInterface/EngineTab methods, list gaps per engine.
Doc only: `.devin/ENGINE-ADAPTER.md`.

### MS03
LocationBar: keep completer painting/input handling. Move: suggestion
source selection and ordering rules, typed-input -> navigate/search/
internal-scheme resolution behind facade query (settings+opensearch+
history+bookmarks read through handles, no direct store).

### MS04
Context menu: action set + enabled state decided by a controller
method fed by facade (permissions, capabilities) — widget just
receives the menu model. Hover-link status text policy same.

### MS05
After MB04 lands: replace any engine #ifdef / hardcoded capability
checks in zoom/find-in-page/print/devtools action setup with
ac_engine_caps reads (cached per engine switch).

### MS06
After MB04: main-window chrome (menus, toolbar buttons, preferences
sections visibility) driven by the caps table — e.g. no Servo devtools
item, WebKit extensions row marked unsupported with tooltip, reader
button hidden where unsupported.

### MS07
settings.cpp: every section's load/apply flows through Ac* facade
handles — hunt remaining QSettings / direct-store writes and route
them. Parity: --settings-smoke passes, round-trip unchanged.


AMENDMENT (slicing): if remaining direct-store pokes span more than ~6
preference sections, route only the first half this run and leave the
row in_progress — the loop will resume it next run; do not force
partial work into done.

### MS08
Tab restore order/pinned/per-tab fields sourced from AcSession; the
tab widget keeps paint/drag/close-button logic only.

### MS09
Finalize MW-SHELL.md boundary map. Verify every --*-smoke leg that
ran before still runs (list them, run each, record rc).

### MS10
TabBar `contextMenuRequested` (tabbar.cpp:538): same shape as MS04 —
extract the entry set + enabled/label decisions into a menu model fed
by facade inputs (closed-stack reopen enable → AcSession, pin toggle
label → session state, container submenu rows + private/tor gate →
ac_containers, sleep/wake + free-memory enable → engine state,
group rows → session).  The widget keeps building/execing the QMenu
and owning the strings; MS04's WebViewController is the pattern to
mirror (WebViewController was intentionally WebView-scoped — add a
sibling controller rather than widening it).
