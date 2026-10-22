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
| MS01 | Shell audit — business logic inside widgets + keep/thin table | pending | 0 | .devin/MW-SHELL.md: LocationBar/WebView/TabBar/dialogs decision sites classified thin-now vs keep (paint/input/engine-object); sizes MS02+ work |
| MS02 | EngineAdapter contract doc — formalize EngineInterface surface + caps path | pending | 0 | .devin/ENGINE-ADAPTER.md: what every backend must implement, how caps (bridge MB04) map to adapter methods, honest per-engine matrix |
| MS03 | LocationBar thinning — suggestion/accept/navigate policy via facade | pending | 0 | widget keeps painting+input; suggestion sources + accept decisions (search-vs-url, urlclean hook) move behind facade queries |
| MS04 | WebView thinning slice 1 — context-menu + link-hover policy to controller | pending | 0 | menu action availability/enabledness + hover-status policy via facade decisions, not inline widget code |
| MS05 | WebView thinning slice 2 — zoom/find/print/devtools gating by capability | pending | 0 | needs:task:MB04 (mw-bridge lane); actions gated by ac_engine_caps not #ifdef/hardcode |
| MS06 | Capability-driven chrome — toolbar/menu/prefs items per engine caps | pending | 0 | needs:task:MB04; reader/devtools/extensions/tor/containers items enable-disable from caps; honest tooltips for unsupported |
| MS07 | Settings dialog thinning — sections read/write via facade handles only | pending | 0 | no direct QSettings/store pokes left in settings.cpp; each section's apply/load goes through Ac* handles |
| MS08 | Tab/session UI thinning — restore + per-tab state decisions via AcSession | pending | 0 | tab strip keeps painting; restore-order, pinned-state, per-tab zoom/session fields sourced via sessions facade |
| MS09 | Boundary doc + smoke parity — final thin-shell map + --*-smoke harness check | pending | 0 | .devin/MW-SHELL.md finalized: what stays Qt (paint/input/engine objects) vs moved; every --*-smoke leg still passes |

## Task details

### MS01
Audit `src/locationbar/`, `src/webview.cpp`, `src/browsermainwindow.cpp`,
`src/tabbar/`, `src/settings.cpp`, `src/history/` UI, `src/bookmarks/` UI.
For each method that *decides* something (URL accept, menu availability,
restore policy, suggestion source pick, permission default) record
file:line + verdict `thin` (move behind facade — domains lane provides
the modules; if the module doesn't exist yet mark `thin-blocked:MDnn`)
or `keep`. Output `.devin/MW-SHELL.md`.

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

### MS08
Tab restore order/pinned/per-tab fields sourced from AcSession; the
tab widget keeps paint/drag/close-button logic only.

### MS09
Finalize MW-SHELL.md boundary map. Verify every --*-smoke leg that
ran before still runs (list them, run each, record rc).
