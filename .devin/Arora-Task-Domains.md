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
| MD01 | Domains audit — inventory Qt-side decision logic + move/keep table | done | 0 | MW-DOMAINS.md: full move/keep audit of webpage/webview/locationbar/downloadmanager/webpermissionmanager/network interceptors/GPC/popup/session/urlclean/scriptcontrol/extension surfaces; verified file:line against tree |
| MD02 | rustcore `navpolicy` — acceptNavigationRequest decisions + FFI + adoption | blocked | 5 | blocked:exceeded 5 restarts |
| MD03 | rustcore `permpolicy` — WebPermissionManager decisions per-profile + FFI | blocked | 5 | blocked:exceeded 5 restarts |
| MD04 | rustcore `dlwpolicy` — download interception policy (auto/save-as/block, mime) | blocked | 5 | blocked:exceeded 5 restarts |
| MD05 | rustcore `privacy` — GPC header decision + tracker-strip unified in policy store | blocked | 5 | blocked:exceeded 5 restarts |
| MD06 | rustcore `urlclean` — tracking-param strip / URL sanitize for nav + clipboard | blocked | 5 | blocked:exceeded 5 restarts |
| MD07 | Extension dispatch gaps — remaining chrome.* surfaces behind shim dispatch | blocked | 5 | blocked:exceeded 5 restarts |
| MD08 | Session snapshot policy — autosaver/dirty-tracking decisions into sessions module | blocked | 5 | blocked:exceeded 5 restarts |
| MD09 | Popup-block decision — WebView popup gate unified into navpolicy | done | 4 | popup gate unified behind navpolicy popup_decide + record ops via ac_nav_call/rc_nav_call (ABI v11); PopupBlocker consults facade w/ FFI-failure local fallback; verified cargo 431/431, make -j2 clean, tst_popupblocker 10/10 offscreen, acprobe OK incl popup legs, no-rust TU clean |
| MD10 | tabs core ops + tabObject honesty — remove/reload/duplicate/move/highlight/goBack/goForward/discard + real pinned/discarded/groupId + query filters | done | 0 | chrome.tabs ops (remove/reload/duplicate/move/highlight/goBack/goForward/discard + getAllInWindow/getSelected) dispatch onto TabWidget/WebView; tabObject+query report real pinned/audible/discarded/muted/autoDiscardable/groupId/highlighted/windowType — tst_extensionmanager extensionTabsOps + ShimBackend/ChromeNavLock/EventSurfaces/TabMessaging legs PASS offscreen |
| MD11 | tabs zoom + captureVisibleTab — get/setZoom(+Settings) + view grab -> dataUrl | done | 0 | get/setZoom via WebView::setTabZoomFactor + recorded zoomSettings (disabled enforced) + captureVisibleTab grab->png/jpeg dataUrl gated by seesUrls; tst_extensionmanager extensionTabsZoomCapture + 6 adjacent legs PASS offscreen |
| MD12 | tabs.connect — long-lived Port to a tab's page agent over ER02 relay | done | 0 | tabs.connect serves long-lived Ports to a tab page agent over the ER02 relay — invokeTabConnect + runtime tabPortConnect registers the caller end, ext-scoped __arktos.portConnect offer to the addressed agent, sender.tab on the connecting port, postMessage/onDisconnect both directions, sendMessage-identical gates (frameId!=0/documentId typed) — make -j2 clean, tst_extensionmanager extensionTabConnect + 6 adjacent legs PASS offscreen, no-rust TU clean |
| MD13 | windows fidelity — populate tabs array, real state, update/create geometry+drawAttention | done | 0 | populate=true tabs array + real state/alwaysOnTop/bounds on get/getCurrent/getLastFocused/getAll/create; update+create honour left/top/width/height/state/drawAttention via shared applyWindowProps; getInfo.windowTypes honest filter + get(WINDOW_ID_CURRENT); populated tabs shapedTab-scrubbed — make -j2 clean, tst_extensionmanager extensionWindowsFidelity + 7 adjacent legs PASS offscreen, no-rust TU clean |
| MD14 | action.setIcon — record bundle-path/imageData into ActionState for EXT08 strip | done | 0 | action/browserAction.setIcon live — path resolves bundle-relative under ext dir (openOptionsPage hygiene), imageData base64 RGBA decoded to QImage; recorded into ActionState.iconPaths/iconImages + ExtensionInfo.actionIcon*, EXT08 strip prefers overrides, setIcon({}) resets to manifest; JS stub removed; verify: make -j2 clean, extensionActionIcon+hostedBackgroundRuntime(e2e shim)+ShimBackend/WindowsFidelity PASS offscreen; chromePageChurn/extensionCommands fail identically on base; no-rust TU clean |
| MD15 | extension.isAllowedFileSchemeAccess/isAllowedIncognitoAccess — honest booleans | done | 0 | isAllowedFileSchemeAccess/isAllowedIncognitoAccess ride invokeExtension -> okReply(false) honest verdicts (regular-profile hosting, no file: grant); extshim installNs members + repair-path forceMember replace the fillNs typed stubs; 2 gap entries removed from EXT-RUNTIME-GAPS; verify: make -j2 clean, tst_extensionmanager extensionAllowedAccess + extensionIntrospection(e2e promise+callback)+ShimBackend/hostedBackgroundRuntime PASS offscreen, no-rust TU clean |
| MD16 | management read ops — getAll/get + permissionWarnings over ExtensionManager | done | 0 | getAll/get + permissionWarnings over ExtensionManager::extensions() sharing getSelf ExtensionInfo shape; management-gated; leg enumerates fixture — tst_extensionmanager extensionManagementRead+Introspection/ShimBackend/AllowedAccess/hostedBackgroundRuntime/ActionIcon/WindowsFidelity PASS offscreen, make -j2 clean, no-rust TU clean |
| MD17 | management mutations — setEnabled/uninstall/uninstallSelf, 'management'-gated | done | 0 | setEnabled/uninstall/uninstallSelf ride ExtensionManager::setExtensionEnabled/removeExtension management-gated; onEnabled/onDisabled/onUninstalled broadcast via runtime fanOut (management eventGate) + pushEvent fallback for bare backends; session removed-id marker stops self-fallback resurrection; verify: tst_extensionmanager extensionManagementMutations+Read/AllowedAccess/Introspection/hostedBackgroundRuntime/ActionIcon/WindowsFidelity 9/9 PASS offscreen, make -j2 clean, no-rust TU clean |
| MD18 | runtime lifecycle — reload, requestUpdateCheck, real setUninstallURL | done | 0 | runtime.reload->manager unload+load (consent re-grant, deferred dispatch); requestUpdateCheck->checkForUpdates/updateResultFor verdict pushed as __arktos.updateCheck in Chrome {status,version} vocab + onUpdateAvailable; setUninstallURL persisted per persona, removal opens it in a new tab (pending marker spares reload-unloads) — verify: make -j2 clean, tst_extensionmanager extensionRuntimeOps+5 adjacent legs 8/8 PASS offscreen, no-rust TU clean |
| MD19 | storage extras — getKeys, setAccessLevel, sync persistence, quota bounds | done | 0 | getKeys/setAccessLevel(TRUSTED/UNTRUSTED_CONTEXTS recorded)+sync persists like local w/ no-cloud-sync note+Chrome quota bounds (sync 8192/item,102400,512 items; local+session 10MB) enforced atomically; verify: extensionStorageExtras+ShimBackend+hostedBackgroundRuntime+Introspection PASS offscreen, make -j2 clean, no-rust TU clean |
| MD20 | alarms edge parity — MV3 30s period floor w/ note, unnamed defaults, getAll order | done | 0 | alarms edge parity: create() Chrome arg checks (when+delay / no-schedule / both name forms -> invalid-argument; period doubles as delay), unnamed alarms fully served under "" default (get/clear/onAlarm membership-keyed, replace parity), MV3 30s periodInMinutes floor clamps to 0.5 + recordNote, getAll creation-ordered via minted seq — verify: make -j2 clean, tst_extensionmanager extensionAlarmEdges+ShimBackend+StorageExtras+hostedBackgroundRuntime PASS offscreen, no-rust TU clean |
| MD21 | chrome.bookmarks.* over rustcore RC05 store | done | 1 | chrome.bookmarks.* over RC05 store via BookmarksManager/BookmarkNode facade lane: fixed 0/1/2 ids + handle-serial ids, get/getChildren/getRecent/getTree/getSubTree/search + create/move/update/remove/removeTree through undoable manager ops w/ Chrome verdict vocabulary, bookmarks-gated + eventGate on* fan-out, signal tap for UI-driven changes, removeOwner claim fixes detach/reattach ownership; verify: make -j2 clean, tst_extensionmanager extensionBookmarks+ShimBackend/hostedBackgroundRuntime/Introspection/Management*/RuntimeOps/inspectManifest/runtimeHealthRegrade PASS offscreen, no-rust TU clean; backgroundHealthProbe fails identically on base |
| MD22 | chrome.history.* over rustcore RC04 store | done | 0 | chrome.history.* over RC04 via HistoryManager: search aggregates per-url HistoryItems (AND terms, 24h default window, 100 cap), getVisits VisitItems w/ FNV serials (link/typedCount 0 noted), addUrl/deleteUrl/deleteRange/deleteAll on the retention path w/ aggregate onVisitRemoved + entryAdded/Removed/Cleared tap; history-gated + eventGate + runtimeServes; verify: make -j2 clean, tst_extensionmanager extensionHistory+10 adjacent legs PASS offscreen, no-rust TU clean |
| MD23 | chrome.cookies.* over RC15 store + engine cookie store | done | 1 | chrome.cookies over RC15 mirror + engine jar: CookieJar taps QWebEngineCookieStore cookieAdded/Removed into the AcCookies facade handle (MIRROR role, per-persona, OTR silent, clear() drops the partition); invokeCookies serves get/getAll/getAllCookieStores/set/remove — reads off the RC15 mirror (C++ jar fallback), set/remove write live store + mirror synchronously, Chrome Cookie shape (hostOnly dot, expires ms->expirationDate s, sameSite enum, storeId "0"), url+per-cookie-domain coverage via requestVisibleTo, verdicts no-permission/no-host-access/invalid-argument/scheme-refused/unsupported(partitionKey)/no-store/store-failed; onChanged rides the engine signal tap, eventGate cookies + shapedPayload per-domain withhold; extshim installs the namespace, runtimeServes gained cookies, EXT-RUNTIME-GAPS declared; fixed stale dead-perm legs (cookies->gcm) in inspectManifest/runtimeHealthRegrade — verify: make -j2 clean, extensionCookies+inspectManifest+runtimeHealthRegrade+History/Bookmarks/ShimBackend/Introspection/hostedBackgroundRuntime/Mgmt*/RuntimeOps/permissionLists/EventSurfaces/ActionIcon/AllowedAccess all PASS offscreen --disable-gpu, no-rust TU syntax clean on all touched TUs |
| MD24 | chrome.downloads.* subset over DownloadManager | done | 0 | chrome.downloads over DownloadManager records: download via manager policy path (retryPage, dlwpolicy verdict, rustdl gate) w/ deferred ticket->id __arktos.downloadCreated push + 30s bound; filename suggestion rides m_extSuggestedName into saveFileName (Chrome-relative subpaths, per-segment sanitize+uniquify); search/erase on DownloadsQuery id/url/filename/mime/state/error/time/size/regex filters; pause/resume/cancel/open/show/showDefaultFolder/removeFile/getFileIcon drive card actions w/ typed verdicts; shelf ops+POST/headers/overwrite typed unsupported; onCreated/onChanged/onErased taps (itemAdded, status/progress/isPausedChanged/finished, rowsAboutToBeRemoved), eventGate downloads + OTR rows invisible; page->download auto-accept quirk reconciled by downloadsFileFixup (completed file moved onto recorded name, engine partials cleaned on cancel); extshim installs namespace + deferred waiter; EXT-RUNTIME-GAPS dead entry swapped for refusal vocabulary — verify: make -j2 clean, tst_extensionmanager extensionDownloads(new leg: fixture download+ticket+onCreated+search+pause/resume deltas+cancel USER_CANCELED+erase/onErased+refusals+bare-backend data: flow+suggested name+icon+removeFile+gate)+ShimBackend/EventSurfaces/Cookies/History/Bookmarks/RuntimeOps PASS offscreen --disable-gpu, no-rust TU syntax clean on both touched TUs |
| MD25 | chrome.sessions minimal — getDevices honest empty, recentlyClosed/restore over undo stack | done | 0 | chrome.sessions over TabWidget TABS05 undo stack: ClosedTabRecord (serial+lastModified) projection + restoreRecentlyClosed(serial) reuse the menu/openLastTab undo path; invokeSessions serves getDevices (honest empty — no sync), getRecentlyClosed (per-window stacks merged newest-first, MAX_SESSION_RESULTS 25, filter.maxResults validated), restore (no-arg newest / serial sessionId -> live tab id); verdicts no-permission/invalid-argument/not-found/restore-failed/unimplemented; sessions.onChanged rides new recentlyClosedTabsChanged tap (close/restore/clear all emit), eventGate sessions + runtimeServes gained sessions; extshim installs namespace + MAX_SESSION_RESULTS, EXT-RUNTIME-GAPS dead-ns entry swapped for served vocabulary — verify: make -j2 clean (moc fix: struct in public, not slots), tst_extensionmanager extensionSessions+Downloads/Cookies/History/Bookmarks/ShimBackend/EventSurfaces/Introspection/TabsOps/WindowsFidelity/runtimeHealthRegrade/hostedBackgroundRuntime/inspectManifest all PASS offscreen --disable-gpu |
| MD26 | chrome.tabGroups.* over TABGRP machinery | done | 0 | chrome.tabGroups over TABGRP machinery: invokeTabGroups serves get/query/update/move/ungroup gated on the tabGroups grant, Chrome int ids ride the minted (window,string-id) map tabObject.groupId reports, colors map the enum onto the container palette; tabs.group/ungroup live under chrome.tabs like the real API (atomic multi-tab resolution, same-window enforced, createProperties.windowId/groupId honored); TabWidget gained tabGroupOf(view) + moveTabGroup(gid,index); the hub diffs per-window group snapshots on tabGroupsChanged to reconstruct onCreated/onUpdated/onMoved/onRemoved and diffs per-tab membership into tabs.onUpdated changeInfo.groupId — collapsed-group members stay tracked instead of misfiring onRemoved; extshim installs the namespace + TAB_GROUP_ID_NONE, eventGate + runtimeServes updated, EXT-RUNTIME-GAPS swapped for served vocabulary — verify: make -j2 clean, tst_extensionmanager extensionTabGroups PASS (hosted fixture: UI create/ungroup -> onCreated/onRemoved, tabs.group -> get/query/update/move/ungroup roundtrip incl title/color/collapsed + strip relocation, tabObject.groupId honesty, tabs.onUpdated groupId, typed no-permission/invalid-argument/not-found/unimplemented) + full binary 63p/4f — all 4 reproduce standalone as documented env flakes (updateCheckLoopback conn-refused, backgroundHealthProbe, chromePageChurn, extensionCommands per MD14/MD21/MRG10) — offscreen --disable-gpu, no-rust TU syntax clean on all touched TUs |
| MD27 | chrome.topSites.get — history most-visited | done | 0 | chrome.topSites.get over RC04 hosts rollup: History::top_hosts (visits desc, first_seen ties, newest-visit representative) via rc_history_top_hosts+ac_hist_top_hosts (AC_API_VERSION 12); invokeTopSites gated on declared topSites grant (Chrome-accurate — card no-gate line wrong) w/ C++ aggregate fallback; extshim installs ns, unsupportedNames/runtimeServes/EXT-RUNTIME-GAPS updated, stale dead-perm legs moved to gcm; verify: cargo test 432/432, make -j2 clean, extensionTopSites + inspectManifest/runtimeHealthRegrade + 11 adjacent legs PASS offscreen --disable-gpu, no-rust TU clean; backgroundHealthProbe env flake on base |
| MD28 | chrome.browsingData subset over ClearPrivateData | done | 0 | chrome.browsingData subset over CLR01 clear plan: settings/remove + 5 served remove* members mapped onto ClearPrivateData categories, secs:<n> exact-window vocab added (parse_period + planQt parity), 9 unserved dataTypes + per-origin/non-web scoping typed unsupported reported in-result; extshim installs ns, runtimeServes+GAPS updated; verify: cargo 433/433, make -j2 clean, tst_clearplan 133/133, extensionBrowsingData + 12 adjacent legs PASS offscreen --disable-gpu |
| MD29 | chrome.dns.resolve — QHostInfo | pending | 0 | async lookup -> {address[]} or typed failure |
| MD30 | chrome.system subset — memory/cpu/display/storage honest answers | pending | 0 | QSysInfo/QScreen/QStorageInfo; per-sub-ns permission gates |
| MD31 | chrome.privacy subset — map servable ChromeSetting members to real settings | pending | 0 | e.g. thirdPartyCookiesAllowed -> cookie policy; unserved stay typed |
| MD32 | chrome.userScripts.* over ER09 registry machinery | pending | 0 | register/unregister/getScripts/update/configureWorld |
| MD33 | chrome.fontSettings subset — getFontList/getFont/default sizes | pending | 0 | QFontDatabase + engine font settings; set* writes real defaults or typed |

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


AMENDMENT (slicing): if the audit finds decision sites not covered by
MD02-MD09 rows, append one new pending row per uncovered cluster
(MD10+) via taskfile.sh with-lock/append-detail — one site-cluster per
row, never bundle.

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


AMENDMENT (slicing): MD07 performs the audit ONLY — read
EXT-RUNTIME-GAPS.md + ExtensionShimBackend dispatch, produce the
feasible/unsupported classification, then append ONE new pending row per
feasible surface (IDs MD10, MD11, …) via
`.devin/taskfile.sh Arora-Task-Domains.md with-lock -- sed -i …` row
insert + `append-detail` spec, each spec = implement that single
surface + parity test. MD07 itself implements NO surfaces — it just
creates the queue. Update the honest unsupported list in the same run.

### MD08
AcSession gains policy queries: snapshot interval, dirty-on-what,
max age/count, restore defaults. BrowserApplication/TabWidget timers
read policy via facade instead of hardcoded/QSettings-scattered values.

### MD09
Popup gate: unify WebPage createWindow policy + WebView popup handling
through navpolicy verdict (gesture-required default, per-host allowlist
in permpolicy store, ask-mode). Parity autotest.

### MD10
chrome.tabs core ops + state honesty.  Implement in invokeTabs
(extensionshimbackend.cpp:662) + tabsNs (extshim.js:2320): remove
(TabWidget::closeTab), reload, duplicate, highlight, move
(moveTabTo / strip reorder), goBack/goForward, discard (SLEEP01
sleepTab — sleeping IS Chrome's discarded state), deprecated
getAllInWindow/getSelected aliases.  tabObject stops hardcoding
pinned/audible/discarded/mutedInfo/groupId (:626-636) — read
isTabPinned (tabwidget.cpp:712), sleep state, tabGroupId
(tabwidget.h:290); extend query filters (pinned, audible, muted,
discarded, highlighted, autoDiscardable, groupId, windowType).
Verify: tst_extensionmanager leg driving each op on a temp-profile
window + gap-list entries removed for served members.

### MD11
chrome.tabs zoom + captureVisibleTab.  getZoom/setZoom via
WebView/QWebEngineView zoomFactor per tabId; get/setZoomSettings
record scope/mode honestly.  captureVisibleTab grabs the view to
data:image/png;base64 — gated by the same seesUrls host-coverage
check shapedTab applies (extension must hold 'tabs'/activeTab or
host coverage, else typed no-permission).  Verify: leg asserts zoom
roundtrip and a non-empty dataUrl for a covered tab, refusal for an
uncovered one.

### MD12
chrome.tabs.connect — long-lived Port between the extension context
and a tab's page agent over the ER02 relay machinery
tabs.sendMessage already uses (extensionbackgroundhost.cpp:157).
Port needs postMessage/onMessage/onDisconnect both directions and
host-coverage gating identical to sendMessage.  Verify: parity leg —
open port, exchange messages, disconnect propagates.

### MD13
chrome.windows fidelity.  invokeWindows (extensionshimbackend.cpp:
971): populate=true returns the tabs array on get/getCurrent/
getLastFocused/getAll (create already does, :1058-1067); real
window.state (isMinimized/isMaximized/isFullScreen instead of
hardcoded "normal" :964); update accepts width/height/left/top/
drawAttention/state; create honours the same geometry props.
Verify: leg asserts populate shape + a geometry roundtrip.

### MD14
chrome.action/browserAction.setIcon.  Path details resolve to
bundle-relative files under m_extensionPath (same hygiene
openOptionsPage applies); imageData is raw RGBA.  Record into
ActionState + ExtensionInfo so the EXT08 action strip can render;
remove the JS typed stub (extshim.js:2261).  Removes the two
`*.setIcon` gap entries when landed.  Verify: leg asserts ActionState
carries the icon and getIcon readers see it.

### MD15
chrome.extension.isAllowedFileSchemeAccess /
isAllowedIncognitoAccess — answer honest booleans (Arktos hosts
extensions on the normal profile and never grants file: scheme, so
both answer false today — that IS the truth; wire through invoke so
a future capability surfaces honestly).  Replaces fillNs typed stubs
(extshim.js:3760-3762); removes two gap entries.  Verify: leg asserts
{ok:true,false} for both.

### MD16
chrome.management read ops.  invokeManagement gains getAll/get over
ExtensionManager::extensions() (extensionmanager.h:268) shaping the
same ExtensionInfo getSelf already builds (:1626-1772), plus
getPermissionWarningsById/getPermissionWarningsByManifest answering
honest warning strings derived from the permission list.  Gated:
'management' declared -> served; undeclared -> no-permission
(unchanged).  Verify: leg enumerates the fixture extension.

### MD17
chrome.management mutations.  setEnabled/uninstall/uninstallSelf via
ExtensionManager::setExtensionEnabled/removeExtension
(extensionmanager.h:283-284), 'management'-permission gated; fire
management.onEnabled/onDisabled/onUninstalled through pushEvent.
Verify: leg toggles the fixture extension off and on.

### MD18
chrome.runtime lifecycle ops.  reload -> ExtensionManager unload+load
of the calling extension (replaces typed unsupported at
extensionshimbackend.cpp:1942); requestUpdateCheck ->
ExtensionManager::checkForUpdates/updateResultFor answering Chrome's
{status,version} shape; setUninstallURL persists the url and
removeExtension opens it.  Verify: leg asserts requestUpdateCheck
status vocabulary and uninstall-url roundtrip on a temp profile.

### MD19
chrome.storage extras.  invokeStorage (:364): <area>.getKeys,
setAccessLevel recorded (TRUSTED/UNTRUSTED_CONTEXTS note), 'sync'
stops aliasing the in-memory session store (storeFor :274-285) and
persists like 'local' with a no-cloud-sync note; QUOTA_BYTES /
QUOTA_BYTES_PER_ITEM constants + enforcement bounds matching Chrome.
Verify: leg asserts sync survives a backend respawn on temp profile.

### MD20
chrome.alarms edge parity.  invokeAlarms (:534): enforce Chrome's MV3
30s periodInMinutes floor with a recorded note (today any period
spins), unnamed alarms get Chrome's generated name, getAll answers
in a deterministic order, create() on an existing name replaces like
Chrome.  Verify: legs assert floor-clamp note + order stability.

### MD21
chrome.bookmarks.* served over the rustcore RC05 bookmarks store
(src/rustcore/src/bookmarks.rs) through the facade/FFI lane the C++
BookmarkNode wrappers already use: get/getChildren/getRecent/
getTree/getSubTree/search/create/move/update/remove/removeTree +
onCreated/onRemoved/onChanged/onMoved events via pushEvent.
'bookmarks' manifest permission gates the namespace.  Removes the
dead-namespace entry when landed.  Verify: parity leg against a
temp-profile bookmark tree.

### MD22
chrome.history.* over the RC04 store (history.rs) +
HistoryManager signals: search/getVisits/addUrl/deleteUrl/
deleteRange/deleteAll + onVisited/onVisitRemoved events.
'history' permission gates.  Deletes must run the same retention/
icons path HistoryManager::checkForExpired uses.  Verify: leg
records a visit, searches it, deletes it.

### MD23
chrome.cookies.* over the RC15 store (cookies.rs) as the persistent
mirror + QWebEngineCookieStore for live set/remove: get/getAll/
getAllCookieStores/set/remove + onChanged.  'cookies' permission +
host coverage for the url gate.  Verify: leg sets + gets a cookie on
a covered host in a temp profile.

### MD24
chrome.downloads.* subset over DownloadManager records: download
(route through the same policy path DownloadManager uses), search,
pause/resume/cancel/open/show/showDefaultFolder/erase/removeFile +
onCreated/onChanged/onErased events.  getFileIcon answers a type
icon or typed unsupported; setShelfEnabled/drag typed (no shelf).
'downloads' permission gates.  Verify: leg downloads a fixture url,
searches it, cancels one.

### MD25
chrome.sessions minimal.  getDevices answers the honest empty list
(no sync), MAX_SESSION_RESULTS constant; getRecentlyClosed/restore
over TabWidget's per-window closed-tab undo stack — expose a read
API (hasRecentlyClosedTabs/openLastTab exist; need enumerated
records + restore-by-index).  'sessions' permission gates.  Verify:
leg closes a tab, sees it in getRecentlyClosed, restores it.

### MD26
chrome.tabGroups.* over the TABGRP machinery (tabwidget.h:284-326):
get/query/update/move/ungroup, tabObject.groupId filled (rides
MD10's honesty change — land a fallback if MD10 is pending), the
four on* events via pushEvent.  'tabGroups' permission gates.
Verify: leg creates a group via update(), queries it, ungroups.

### MD27
chrome.topSites.get — history most-visited hosts from the RC04
store (same host/suggest table the location completer ranks).
No permission gate in Chrome.  Verify: leg asserts a visited host
appears.

### MD28
chrome.browsingData subset.  remove() + removeHistory/removeCookies/
removeDownloads/removeCache/removeFormData/settings mapped onto the
ClearPrivateData machinery — 'browsingData' permission gated.
Unserved dataTypes keep typed unsupported inside the removal set,
never silently skipped.  Verify: leg asserts removeHistory empties a
temp-profile history.

### MD29
chrome.dns.resolve — QHostInfo::lookupHost, 'dns' permission gated;
answers {addresses:[...]} or a typed resolution failure.  Verify:
leg resolves localhost on a temp profile.

### MD30
chrome.system subset.  system.memory.getInfo (QSysInfo capacity),
system.cpu.getInfo (arch/numProcessors — no per-core time if not
honestly measurable), system.display.getInfo (QScreen geometry/depth),
system.storage.getInfo (QStorageInfo on the profile root).  Each
sub-namespace gated by its 'system.*' permission like Chrome.
Verify: legs assert honest non-empty shapes.

### MD31
chrome.privacy subset — ChromeSetting-shaped objects for members
with a real mapping (e.g. websites.thirdPartyCookiesAllowed ->
cookie-filter policy, network.networkPredictionEnabled -> prefetch
setting if one exists); get/set respect 'privacy' permission;
unservable members keep typed unsupported.  Verify: leg roundtrips
one mapped setting on a temp profile.

### MD32
chrome.userScripts.* over the ER09 registration machinery:
register/unregister/getScripts/update answering the MV3 shapes
(ids, matches, js, runAt) + configureWorld recorded honestly.
'userScripts' permission gated.  Verify: leg registers a script and
lists it.

### MD33
chrome.fontSettings subset.  getFontList via QFontDatabase, getFont/
getDefaultFontSize/getDefaultFixedFontSize answering real engine
defaults; set*/clear* write the real default settings where an
honest mapping exists, typed unsupported otherwise.  'fontSettings'
permission gates.  Verify: leg asserts getFontList non-empty and a
default-size read.
