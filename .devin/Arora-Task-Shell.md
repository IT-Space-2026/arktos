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
| MS03 | LocationBar thinning — suggestion/accept/navigate policy via facade | blocked | 5 | blocked:exceeded 5 restarts |
| MS04 | WebView thinning slice 1 — context-menu + link-hover policy to controller | done | 3 | webviewcontroller.{h,cpp}: contextMenuModel owns ctx-menu entry set/order/enabled+disabled-reason, hoverStatusText the status verdict; webview.cpp renders the model (menu build/exec/strings stay widget), engine-adapter hover sink consumes same fn; env legs thin-blocked MD02/MB04 documented; verify: full rust build + --browser-smoke PASS + tst_webview 29/29 incl 11 ctx-menu cases |
| MS05 | WebView thinning slice 2 — zoom/find/print/devtools gating by capability | pending | 4 | needs:task:MB04 (mw-bridge) — re-checked run 5: still in_progress (att 4/5) there, caps.rs + mw/ still uncommitted in that worktree, ac_engine_caps absent from this facade; row stays pending |
| MS06 | Capability-driven chrome — toolbar/menu/prefs items per engine caps | pending | 0 | needs:task:MB04; reader/devtools/extensions/tor/containers items enable-disable from caps; honest tooltips for unsupported |
| MS07 | Settings dialog thinning — sections read/write via facade handles only | pending | 0 | no direct QSettings/store pokes left in settings.cpp; each section's apply/load goes through Ac* handles |
| MS08 | Tab/session UI thinning — restore + per-tab state decisions via AcSession | pending | 0 | tab strip keeps painting; restore-order, pinned-state, per-tab zoom/session fields sourced via sessions facade |
| MS09 | Boundary doc + smoke parity — final thin-shell map + --*-smoke harness check | pending | 0 | .devin/MW-SHELL.md finalized: what stays Qt (paint/input/engine objects) vs moved; every --*-smoke leg still passes |
| MS10 | TabBar context-menu model — entry set/enabled to controller | pending | 0 | MS04-shaped: tabbar.cpp contextMenuRequested :538 entry set+enabled (reopen-enable, pin label, container rows, sleep/wake, group rows) decided by a model; inputs AcSession+ac_containers, widget keeps build/exec |

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
