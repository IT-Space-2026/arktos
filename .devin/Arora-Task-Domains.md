# Devin autonomous task list — Arktos MW-DOMAINS LANE (branch mw-domains, worktree ../arora-mwdomains)

Logic-layer queue driven by `.devin/taskloop-domains.sh`. Goal: move every
remaining engine-agnostic *decision* still living in Qt C++ into
`src/rustcore` behind the `arktos_core.h` facade — continuing where RC01-RC21
left off. The domains here are the ones the RC lane never touched:
navigation policy, site permissions, download policy, privacy/GPC, extension
dispatch gaps, popup blocking, autosaver/session snapshot policy, URL
sanitization.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting, `done` only after verifying.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- NEVER edit `.devin/Arora-Task.md` or other lanes' tables.
- Cross-lane deps: `needs:task:X` may reference another lane — check that
  lane's table (`../arora-mwbridge/.devin/Arora-Task-Bridge.md`,
  `../arora-mwshell/.devin/Arora-Task-Shell.md`) before starting. If the
  dependency is still pending/in_progress, leave YOUR row `pending` and pick
  the next runnable row — never burn an attempt on a blocked dep.
- Append a one-line summary to `.devin/WORKLOG.md` when a task completes.
- Table rows are one-line summaries; the FULL spec lives in `### <id>`
  under `## Task details`. Keep cells short.
- ALL writes to this file MUST go through `.devin/taskfile.sh`
  (flock-serialized): `set <ID> field=…`, `append-detail <ID>` (stdin),
  `with-lock -- <cmd>`.
- Every module follows the established pattern: per-profile store under the
  persona root, `rc_<dom>_*` FFI, `Ac<Dom>` facade handle, legacy-format
  import where a Qt store exists, `make rust=1` AND `no-rust` builds clean.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| MD01 | Domains audit — inventory Qt-side decision logic + move/keep table | pending | 0 | .devin/MW-DOMAINS.md: every if/switch in WebPage/WebView/LocationBar/DownloadManager/permissions/GPC code paths classified move-to-rustcore vs stays-Qt (render/JS/surface-dependent) |
| MD02 | rustcore `navpolicy` — acceptNavigationRequest decisions + FFI + adoption | pending | 0 | scheme allowlist, blocked-nav, external-protocol handoff, target=_blank/popup gate; WebPage::acceptNavigationRequest becomes decision consumer; parity autotest vs current Qt behavior |
| MD03 | rustcore `permpolicy` — WebPermissionManager decisions per-profile + FFI | pending | 0 | notification/geo/clipboard/pointerlock/media etc verdict store + defaults; Qt permission dialogs keep UI, decisions via facade; tst_parity green |
| MD04 | rustcore `dlwpolicy` — download interception policy (auto/save-as/block, mime) | pending | 0 | verdict from request headers+suggested name+mime rules; DownloadManager adopts via facade; per-profile rules store |
| MD05 | rustcore `privacy` — GPC header decision + tracker-strip unified in policy store | pending | 0 | GPC enabled per profile -> Sec-GPC decision via facade at request-intercept time; unify w/ existing tracking/adblock policy reads |
| MD06 | rustcore `urlclean` — tracking-param strip / URL sanitize for nav + clipboard | pending | 0 | ruleset-driven param stripping (utm_* etc); consumed on navigation + copy-link; per-profile on/off in policy store |
| MD07 | Extension dispatch gaps — remaining chrome.* surfaces behind shim dispatch | pending | 0 | audit EXT-RUNTIME-GAPS.md first; move unimplemented-but-feasible APIs behind ExtensionShimBackend dispatch; honest unsupported list updated |
| MD08 | Session snapshot policy — autosaver/dirty-tracking decisions into sessions module | pending | 0 | snapshot interval, dirty thresholds, restore-policy decisions via AcSession; Qt timers keep firing, policy from facade |
| MD09 | Popup-block decision — WebView popup gate unified into navpolicy | pending | 0 | gesture-required / allowlist / always-ask policy through navpolicy facade; parity with current WebPage popup behavior |

## Task details

### MD01
Grep-driven audit. For each of:
`src/webpage.cpp` (acceptNavigationRequest, certificateError, feature
permissions), `src/webview.cpp` (popup paths, context menus, download
triggers), `src/network/` interceptors, `src/webpermissionmanager*`,
`src/downloadmanager.cpp` policy branches, GPC/DNT injection,
`src/locationbar/` accept policy — list each decision site with
file:line, what it decides, and verdict `move` (pure data/policy ->
rustcore) or `keep` (needs JS exec, widget, or engine surface). Output
`.devin/MW-DOMAINS.md`. This table is the contract for MD02-MD09 — be
exhaustive.

### MD02
New `navpolicy` module. Inputs: url, navigation type (typed/link/
redirect/backforward/reload/form), is_main_frame, gesture state.
Output: allow | block | external-handoff | ask. Persist per-profile
overrides (scheme rules, blocked hosts) in the persona store. WebPage::
acceptNavigationRequest consults facade first; identical decision to
today for all cases not covered by new rules. Autotest: feed the
current decision matrix through FFI, assert parity.

### MD03
Per-profile permission verdict store: feature enum x host -> allow/
deny/ask (+expiry optional). WebPermissionManager keeps the dialog UI;
the remember/default logic moves behind rc_perm_*. Legacy QSettings
permission keys imported once.

### MD04
Download policy: given response headers (content-type,
content-disposition), url, mime -> download | display | ask |
block(extensions list). DownloadManager asks facade; user rules store
per profile (always-save mime, block ext list) with settings UI reading
the same facade keys.

### MD05
GPC: per-profile toggle already exists somewhere — find it; the
injection decision at request time goes through rc_privacy_gpc(profile)
so engine adapters just ask. Also unify: tracker-strip and adblock
policy reads share the privacy module's verdict helper.

### MD06
URL sanitize: default ruleset stripping utm_*, fbclid, gclid, igshid,
mc_cid/eid etc on commit + on copy-link; ruleset JSON in profile store,
editable. Honest default: ON for copy, config-gated for nav (breaking
sites risk documented).

### MD07
Read `.devin/EXT-RUNTIME-GAPS.md` + ExtensionShimBackend dispatch table.
For each chrome.* surface marked unimplemented but feasible (storage
quota ops, alarms edges, tabs.query extras, notifications minimal,
contextMenus basics): implement in shim dispatch; update the honest
unsupported list. NO new engine JS APIs — dispatch/state only.

### MD08
AcSession gains policy queries: snapshot interval, dirty-on-what,
max age/count, restore defaults. BrowserApplication/TabWidget timers
read policy via facade instead of hardcoded/QSettings-scattered values.

### MD09
Popup gate: unify WebPage createWindow policy + WebView popup handling
through navpolicy verdict (gesture-required default, per-host allowlist
in permpolicy store, ask-mode). Parity autotest.
