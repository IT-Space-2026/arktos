# MD07 — chrome.* dispatch feasibility audit

Scope: every surface that answers a typed `unsupported`/`unimplemented`
today (the `.devin/EXT-RUNTIME-GAPS.md` declared set + the JS-side
`failCall`/`fillNs` stubs), classified `queued` (feasible behind
dispatch/state only — no new engine JS APIs) or `stays` (needs a UI
surface, an engine capability we do not have, or an external service).

## Dispatch architecture (as measured)

- `ChromeBridge::invoke` (src/extensions/extensionbackgroundhost.cpp:138)
  owns the context-scoped ops BEFORE the backend sees them: `relay.*`,
  `tabs.getCurrent` (:150), `tabs.sendMessage` (:157), `scripting.*`
  (:164), `contentScripts.*` (:169), `offscreen.*` (:173),
  `webRequest.*` (:178), `storage.changed` (:182).
- `ExtensionShimBackend::invoke` (src/extensions/extensionshimbackend.cpp:206)
  routes 18 namespaces; fallthrough answers `unimplemented` (:248).
- extshim.js fills native-subset namespaces with typed stubs
  (`fillNs`, src/data/extshim.js:3675-3901) and wraps absent
  namespaces in the typed-error proxy (`unsupportedNs`, :3908-3993).
- Typed refusals in the gaps block's "domain refusals" section are
  correct replies for bad state — not gaps; nothing queued for them.

## Per-namespace findings

### chrome.storage — backend invokeStorage (:364-487)
Served: local/session/sync/managed get/set/remove/clear/getBytesInUse,
changed fan-out.  Gaps worth queuing: `getKeys`, `setAccessLevel`
(record-only), `sync` persistence (today `storeFor` aliases session's
in-memory map, :274-285 — Chrome's sync roams; honest local persistence
+ note is better than volatile), quota constants/limits.  `managed`
read-only stays (correct).  → **MD19**

### chrome.alarms — invokeAlarms (:534-605)
Served: create/get/getAll/clear/clearAll + onAlarm.  Edge parity only:
Chrome's MV3 30s `periodInMinutes` floor with a recorded note,
unnamed-alarm defaulting, getAll ordering.  → **MD20**

### chrome.tabs — invokeTabs (:662-887) + JS proxy (:2320-2463)
Served: query/get/create/update/sendMessage + events.  Unserved
methods currently typed-stubbed by fillNs (:3772-3780):
- Feasible ops: `remove` (closeTab), `reload`, `duplicate`,
  `highlight`, `move` (moveTabTo / strip reorder), `goBack`/`goForward`,
  `discard` (SLEEP01 sleepTab, tabwidget.h:397), deprecated
  `getAllInWindow`/`getSelected` aliases.  → **MD10**
- State fidelity: tabObject hardcodes pinned/audible/discarded/
  mutedInfo (:626-636) while `TabWidget::isTabPinned`
  (tabwidget.cpp:712), sleep state and `tabGroupId` (tabwidget.h:290)
  are real — report them + extend query filters (pinned, audible,
  muted, discarded, highlighted, autoDiscardable, groupId, windowType).
  → **MD10** (same row: the query surface these ops report through)
- `getZoom`/`setZoom` (QWebEngineView::zoomFactor),
  `get/setZoomSettings` (recorded scope) and `captureVisibleTab`
  (view grab → data:image/png, gated by the seesUrls host-coverage
  check shapedTab already uses).  → **MD11**
- `tabs.connect` — long-lived Port into a tab's page agent over the
  ER02 relay.  → **MD12**
- Stays: `detectLanguage` (no engine language API), `executeScript`/
  `insertCSS`/`removeCSS`/`sendRequest` (deprecated MV2 spellings —
  MD10 may alias them to the scripting ops, low value),
  `print`/`toggleReaderMode` (no surface).

### chrome.windows — invokeWindows (:971-1106)
Served: get/getCurrent/getLastFocused/getAll/create/update/remove.
Feasible: `populate` query option (create() already returns the tabs
array :1058-1067), real `state` reporting (currently hardcoded
"normal", :964), update `width/height/left/top/drawAttention`, create
geometry/state props.  → **MD13**

### chrome.action / chrome.browserAction — invokeAction (:1113-1189)
Served: title/badge/popup/enabled set+get, onClicked.  Feasible:
`setIcon` — path entries are bundle-relative files, imageData is raw
RGBA; record into ActionState → ExtensionInfo for the EXT08 strip.
Currently typed-stubbed in JS (:2261-2264) and declared in gaps.
→ **MD14**

### chrome.i18n — invokeI18n + JS wraps (:2051-2136)
Served: getMessage (locale map), getUILanguage, getAcceptLanguages.
Stays: `detectLanguage` (no CLD).  Nothing queued.

### chrome.permissions — invokePermissions (:1329-1368)
Served: contains/getAll.  `request`/`remove` need a runtime grant UI —
no surface exists; stays unsupported (would be an MW-SHELL dep).

### chrome.contextMenus — invokeContextMenus (:1420-1482)
Served: create/update/remove/removeAll + onClicked via EXT16 render.
Complete; `ACTION_MENU_TOP_LEVEL_LIMIT` constant can ride any later
row.  Nothing queued.

### chrome.sidePanel / chrome.sidebarAction — (:1489-1510, :1803-1855)
setOptions/getOptions/setPanelBehavior/getPanelBehavior and the
sidebar bookkeeping served.  `open`/`close`/`toggle` need a sidebar UI
surface Arktos does not have — stays unsupported (MW-SHELL territory).

### chrome.notifications — invokeNotifications (:1529-1552)
Served: create/update/clear/getAll + getPermissionLevel (JS :3351) +
all four events.  Complete.  Nothing queued.

### chrome.commands — invokeCommands (:1564-1572).  Complete.
### chrome.idle — invokeIdle (:1585-1614)
Served: queryState/setDetectionInterval/onStateChanged.  Stays:
`getAutoLockDelay` (ChromeOS-only — correctly typed).

### chrome.management — invokeManagement (:1626-1772)
Served: getSelf.  Feasible over ExtensionManager
(extensions()/setExtensionEnabled()/removeExtension(),
extensionmanager.h:268-284):
- read ops `getAll`/`get` + `getPermissionWarningsById`/
  `getPermissionWarningsByManifest` (honest strings derived from the
  permission list).  → **MD16**
- mutations `setEnabled`/`uninstall`/`uninstallSelf`, gated by the
  'management' manifest permission like Chrome.  → **MD17**
Stays: `launchApp` (no apps), `generateAppForLink` (ChromeOS).

### chrome.extension — invokeExtension (:1781-1793)
Served: reloadView/getViews/getBackgroundPage/getURL/
inIncognitoContext.  Feasible: `isAllowedFileSchemeAccess` /
`isAllowedIncognitoAccess` — honest booleans instead of typed stubs
(JS fillNs :3760-3762).  → **MD15**

### chrome.runtime — invokeRuntime (:1863-1945) + JS (:1300+)
Served: id/getURL/getManifest/sendMessage/connect/getViews/
getContexts/getPlatformInfo/openOptionsPage/on* lifecycle.
Feasible: `reload` (ExtensionManager unload+load of own extension —
currently typed :1942), `requestUpdateCheck` (real checker exists:
checkForUpdates/updateResultFor, extensionmanager.h:331-334),
`setUninstallURL` (persist + open on removeExtension — currently a
recorded note).  → **MD18**
Stays: `getPackageDirectoryEntry` (no FileSystemEntry handles),
`sendNativeMessage`/`connectNative`/`restartAfterDelay` (no native
host / ChromeOS), `getFrameId` (no subframe addressing).

### chrome.webNavigation — invokeWebNavigation (:922-952)
getFrame/getAllFrames answer the main frame honestly.  Anything deeper
needs a subframe map the engine does not expose — stays.

### chrome.webRequest — invokeWebRequest (observe-only)
Blocking/requestBody interception is not servable at our layer —
stays unsupported (documented in gaps).

### chrome.scripting / chrome.contentScripts / chrome.offscreen
ExecuteScript/CSS/registry ops and the offscreen document lifecycle
are already bridge-served (ER07-ER10) — nothing left that is
dispatch-only.  `documentIds` targets stay unminted (no frame ids).

## Dead namespaces — feasible to serve

| surface | verdict | backing |
|---------|---------|---------|
| chrome.bookmarks.* | **MD21** | rustcore bookmarks store (RC05, src/rustcore/src/bookmarks.rs) + BookmarkNode tree |
| chrome.history.* | **MD22** | rustcore history store (RC04, history.rs) + HistoryManager visit/remove signals for onVisited/onVisitRemoved |
| chrome.cookies.* | **MD23** | rustcore cookies store (RC15, cookies.rs) mirror + QWebEngineCookieStore for live set/remove |
| chrome.downloads.* | **MD24** | DownloadManager records; DownloadItem pause/resume/cancel/open/showInFolder (downloadmanager.h:141-180) |
| chrome.sessions.* | **MD25** | getDevices honest []; getRecentlyClosed/restore over TabWidget's per-window closed-tab stack (hasRecentlyClosedTabs/openLastTab) |
| chrome.tabGroups.* | **MD26** | TABGRP ops (tabwidget.h:284-326): get/query/update/move/ungroup + groupId reporting |
| chrome.topSites.get | **MD27** | history most-visited hosts |
| chrome.browsingData.* subset | **MD28** | remove/removeHistory/removeCookies/removeDownloads/removeCache/removeFormData/settings via ClearPrivateData paths |
| chrome.dns.resolve | **MD29** | QHostInfo lookup; typed errors on NXDOMAIN/timeout |
| chrome.system.* subset | **MD30** | memory (QSysInfo), cpu (arch/cores), display (QScreen), storage (QStorageInfo on profile root) — per-sub-ns permission gates |
| chrome.privacy.* subset | **MD31** | map servable ChromeSetting members (e.g. websites.thirdPartyCookiesAllowed → cookie policy) to real settings; rest stay typed |
| chrome.userScripts.* | **MD32** | register/unregister/getScripts/update over the ER09 registry machinery |
| chrome.fontSettings.* subset | **MD33** | getFontList (QFontDatabase), getFont/getDefaultFontSize/getDefaultFixedFontSize via engine font settings; set* writes real defaults or stays typed |

## Dead namespaces — stay unsupported (with reason)

- **chrome.omnibox.*** — needs LocationBar keyword-input routing (UI
  lane work, not dispatch/state).
- **chrome.identity.*, chrome.gcm.*** — Google-account/FCM services.
- **chrome.enterprise.*, platformKeys, certificateProvider,
  printerProvider, documentScan, fileSystemProvider, vpnProvider,
  wallpaper, loginState, chromeOs, audio, mediaPerception,
  systemIndicator, accessibilityFeatures, autofill, input.*** —
  ChromeOS/device-management surfaces.
- **chrome.debugger, devtools, processes** — no CDP/introspection
  surface; devtools is a widget, not an extension API.
- **chrome.desktopCapture, displaySource, tabCapture** — no capture
  pipeline for extension streams.
- **chrome.declarativeNetRequest, declarativeContent** — declarative
  rules need a blocking interception engine the observe-only
  interceptor cannot provide.
- **chrome.contentSettings** — Chrome's per-site content matrix; our
  permission model lives in WebPermissionManager/rustcore
  permpolicy, not this schema.
- **chrome.favicon** — chrome://favicon internals.
- **chrome.power** — keep-awake semantics we do not manage.
- **chrome.readingList** — no reading-list store exists.
- **chrome.search** — Chrome restricts the API to allowlisted
  extensions; our opensearch registry is not the same contract.
- **chrome.tts** — no speech synthesis.
- **chrome.signedInDevices** — no sync account.
- **chrome.downloads.setShelfEnabled/getFileIcon** — no shelf UI;
  covered by MD24's typed subset.
- **chrome.printing, nativeMessaging, fontSettings.set*-only edges** —
  printing needs a job pipeline; nativeMessaging a native host.

## Honesty invariants kept

- The gaps fenced block stays machine-accurate: entries are NOT
  removed until the queued row lands the implementation.  Feasible
  entries gain `# queued -> MDnn` annotations only.
- No new engine JS APIs anywhere in the queue — every row is
  dispatch + Qt/rustcore state.
