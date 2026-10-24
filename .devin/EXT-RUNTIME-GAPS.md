# Extension-runtime residual gaps — the declared intentional set

ER18's contract: every typed `unimplemented`/`unsupported` (or domain
refusal) the real Bitwarden bundle emits through the hosted runtime
must either be implemented or listed here. The fenced `gaps` block
below is machine-read by `tst_extensionmanager` — anything the bundle
emits that is NOT covered is an unplanned regression and fails the
leg. Format: `chrome.<api>: <code>` exact, or `chrome.<ns>.*: <code>`
matching every member of the namespace (or a dotted sub-path such as
`chrome.storage.managed.*`).

## What the categories mean

- **Dead namespace** — Qt WebEngine never registers the binding and
  Arktos has no surface for it (no OS credential store, no native
  messaging host, no cookie jar we expose, no ChromeOS services).
  The shim's typed-error proxy answers every member `unsupported` —
  the honest reply, never a fake value.
- **Served namespace, unserved member** — the namespace works but the
  member needs infrastructure that does not exist (store update
  channel, runtime grant UI, icon pipeline, subframe addressing,
  FileSystemEntry handles). Each answers a typed error.
- **Domain refusal** — the call is implemented; the reply code is the
  correct answer for the state it ran in (dead tab id, uncovered
  origin, vault-locked window factory, duplicate menu id, managed
  storage write). These are listed so an emission is *attributed*,
  not hidden — if one fires where it shouldn't, that is still a bug.
- **Notes, not gaps** — `chrome.runtime.connectNative`,
  `chrome.offscreen`/`storage.onChanged` native-binding replacements,
  `chrome.extension.getBackgroundPage` (MV3 null), `chrome.privacy`,
  `chrome.idle` scope approximations are `shim.note` diagnostics; they
  are intentionally absent from the block.

## The declared set

```gaps
# --- dead namespaces: no Arktos surface exists ---------------------
# MD07 triage (.devin/EXT-DISPATCH-AUDIT.md): several "dead"
# namespaces turned out to be servable through dispatch/state over
# stores this tree already has — each queue entry left this block
# when its row landed the implementation (downloads left with MD24,
# sessions with MD25, tabGroups with MD26, topSites with MD27,
# browsingData with MD28, dns with MD29, system with MD30, privacy
# with MD31, userScripts with MD32, fontSettings with MD33).
# MD07 re-audit (close-out): two more dead-namespace members were
# found servable and queued — declarativeNetRequest -> MD34 (the
# interceptor DOES block/redirect; the audit's observe-only premise
# was stale) and identity.getRedirectURL -> MD35 (derived origin).
# Entries stay until the row lands.
chrome.accessibilityFeatures.*: unsupported
chrome.audio.*: unsupported
chrome.autofill.*: unsupported
chrome.certificateProvider.*: unsupported
chrome.chromeOs.*: unsupported
chrome.contentSettings.*: unsupported
chrome.debugger.*: unsupported
chrome.declarativeContent.*: unsupported
# declarativeNetRequest left the dead list with MD34 — see the
# served vocabulary under "served namespaces" below.
chrome.desktopCapture.*: unsupported
chrome.devtools.*: unsupported
chrome.displaySource.*: unsupported
chrome.documentScan.*: unsupported
chrome.enterprise.*: unsupported
chrome.favicon.*: unsupported
chrome.fileBrowserHandler.*: unsupported
chrome.fileSystemProvider.*: unsupported
# fontSettings left the dead list with MD33 — see the served
# vocabulary under "served namespaces" below.
chrome.gcm.*: unsupported
chrome.identity.*: unsupported
chrome.input.*: unsupported
chrome.loginState.*: unsupported
chrome.mediaPerception.*: unsupported
chrome.nativeMessaging.*: unsupported
chrome.omnibox.*: unsupported
chrome.platformKeys.*: unsupported
chrome.power.*: unsupported
chrome.printerProvider.*: unsupported
chrome.printing.*: unsupported
chrome.processes.*: unsupported
chrome.readingList.*: unsupported
chrome.search.*: unsupported
chrome.signedInDevices.*: unsupported
chrome.systemIndicator.*: unsupported
chrome.tabCapture.*: unsupported
chrome.tts.*: unsupported
chrome.vpnProvider.*: unsupported
chrome.wallpaper.*: unsupported
# --- served namespaces, deliberately unserved members --------------
# MD07's carve-outs all landed: tabs ops/state MD10, zoom+capture
# MD11, connect MD12; management read MD16 + mutations MD17; alarms
# edges MD20; extension.isAllowed* MD15; storage getKeys/
# setAccessLevel/sync-persistence/quota MD19.  What remains typed:
# tabs members beyond the implemented set (deprecated MV2 spellings
# executeScript/insertCSS/removeCSS/sendRequest, detectLanguage,
# print/toggleReaderMode) — sendMessage frameId!=0 is unsupported
# (page agent lives in the main frame only).  Queued second wave:
# webNavigation.onCreatedNavigationTarget -> MD36, action
# getUserSettings honesty -> MD37.
chrome.tabs.*: unsupported
chrome.idle.getAutoLockDelay: unsupported
chrome.notifications.*: unsupported
chrome.permissions.remove: unsupported
chrome.permissions.request: unsupported
chrome.runtime.getPackageDirectoryEntry: unsupported
chrome.runtime.restartAfterDelay: unsupported
chrome.runtime.sendNativeMessage: unsupported
chrome.sidePanel.open: unsupported
chrome.sidebarAction.close: unsupported
chrome.sidebarAction.open: unsupported
chrome.sidebarAction.toggle: unsupported
# management gates every member but getSelf on the declared
# 'management' permission — no-permission undeclared (Bitwarden does
# not declare it), unsupported when declared and still unserved.
# MD16 serves getAll/get + the permissionWarnings pair when declared;
# MD17 serves setEnabled/uninstall/uninstallSelf over the manager's
# engine calls — only the launch members (launchApp/
# generateAppForLink/setLaunchType) stay typed.
chrome.management.*: no-permission
chrome.management.*: unsupported
# webRequest is observe-only: blocking/requestBody extraInfoSpec and
# unservable events (onBeforeRedirect, onResponseStarted,
# onAuthRequired) are typed unsupported at subscribe time.
chrome.webRequest.*: unsupported
# scripting: documentIds targets are never minted; registration
# constraints (globs, cookieStoreId) refuse the same way.
chrome.scripting.*: unsupported
# --- domain refusals — correct replies, kept visible ---------------
chrome.alarms.create: invalid-argument
# MD21: chrome.bookmarks is served over the RC05 store — 'bookmarks'
# permission gates every member (no-permission undeclared); the rest
# are the op's correct verdicts (bad ids, fixed roots, leaf parents,
# the store's url-scheme gate, empty-folder-only remove).
chrome.bookmarks.*: no-permission
chrome.bookmarks.*: invalid-argument
chrome.bookmarks.*: not-found
chrome.bookmarks.*: cannot-modify
chrome.bookmarks.*: invalid-parent
chrome.bookmarks.*: scheme-refused
chrome.bookmarks.*: create-refused
chrome.bookmarks.*: folder-not-empty
chrome.bookmarks.*: no-store
# MD22: chrome.history is served over the RC04 store — 'history'
# permission gates every member (no-permission undeclared); the rest
# are the op's correct verdicts (bad/missing args, script-or-data
# schemes refused at addUrl, store unreachable).
chrome.history.*: no-permission
chrome.history.*: invalid-argument
chrome.history.*: scheme-refused
chrome.history.*: no-store
# MD27: chrome.topSites is served over the RC04 hosts rollup —
# 'topSites' gates the namespace like Chrome (no-permission
# undeclared); get() is the only published member, so a bridge call
# naming anything else is the typed unreachable-member verdict.
chrome.topSites.*: no-permission
chrome.topSites.*: unimplemented
# MD28: chrome.browsingData is served over the CLR01 clear-plan
# machinery — 'browsingData' gates the namespace (no-permission
# undeclared).  settings/remove plus removeHistory/removeCookies/
# removeDownloads/removeCache/removeFormData are live; the other
# nine Chrome dataTypes (appcache, cacheStorage, fileSystems,
# indexedDB, localStorage, passwords, pluginData, serviceWorkers,
# webSQL) and per-origin/non-web RemovalOptions scoping are typed
# unsupported — inside a mixed remove() they report in the result,
# never silently skipped.  The rest are the calls' correct
# verdicts (missing/mistyped args, unreachable members).
chrome.browsingData.*: no-permission
chrome.browsingData.*: invalid-argument
chrome.browsingData.*: unsupported
chrome.browsingData.*: unimplemented
# MD29: chrome.dns is served over QHostInfo — 'dns' gates the
# namespace like Chrome's declared-grant requirement (no-permission
# undeclared).  resolve is the only published member; the lookup is
# deferred — the invoke answers a ticket and the verdict lands on
# '__arktos.dnsResolved' as {addresses:[...]} or a typed failure.
# The rest are the call's correct verdicts (missing/mistyped or
# oversized hostname, unreachable members, resolver error or the
# bounded wait expiring).
chrome.dns.*: no-permission
chrome.dns.*: invalid-argument
chrome.dns.*: resolve-failed
chrome.dns.*: unimplemented
# MD30: chrome.system is served over measured machine facts —
# each sub-namespace gates on its own 'system.<sub>' declared grant
# (no-permission undeclared).  memory/cpu/display/storage getInfo
# answer real values plus storage.getAvailableCapacity;
# storage.ejectDevice, the ChromeOS-only display mutators and the
# declared-but-unserved subs (network, powerSource, ...) stay typed
# 'unsupported'.  The rest are the calls' correct verdicts
# (mistyped args, an unknown storage unit id, unreachable members,
# an unmeasurable capacity).
chrome.system.*: no-permission
chrome.system.*: invalid-argument
chrome.system.*: not-found
chrome.system.*: unsupported
chrome.system.*: unimplemented
# MD31: chrome.privacy is served over the browser's real settings —
# 'privacy' gates the whole namespace like Chrome's declared grant
# (no-permission undeclared).  Every sub-namespace member answers
# get/set/clear in the ChromeSetting vocabulary: the eight mapped
# members (websites.thirdPartyCookiesAllowed/hyperlinkAuditing
# Enabled/referrersEnabled/doNotTrackEnabled, network.
# networkPredictionEnabled/webRTCIPHandlingPolicy, services.
# passwordSavingEnabled/searchSuggestEnabled) read/write the persona's
# persisted toggles and re-apply live; set() claims control (last
# writer wins — levelOfControl resolves per extension) and the
# controller's clear() restores the pre-extension value.  The rest
# of Chrome's published vocabulary (safeBrowsing*, autofill*,
# translationEnabled, the Privacy Sandbox family, ChromeOS members)
# stays typed 'unsupported'; the two webRTCIPHandlingPolicy values
# the bool+process-flag pair cannot express and non-'regular'
# scopes are typed 'unsupported' too.  The rest are the calls'
# correct verdicts (mistyped/missing args, unreachable members).
chrome.privacy.*: no-permission
chrome.privacy.*: invalid-argument
chrome.privacy.*: unsupported
chrome.privacy.*: unimplemented
# MD32: chrome.userScripts is served over the ER09 registration
# machinery — 'userScripts' gates the namespace like Chrome's
# declared grant (no-permission undeclared).  register/unregister/
# getScripts/update plus configureWorld/getWorldConfigurations/
# resetWorldConfiguration are live; RegisteredUserScript.js is
# ExtensionFileSource[] ({file}|{code} objects — inline code
# allowed), world:'USER_SCRIPT'/'MAIN' and worldId collapse onto
# the shared agent realm (a recorded note, not a gap), and the
# includeGlobs/excludeGlobs matchers Chrome declares stay typed
# 'unsupported' (a different matcher).  The rest are the ops'
# correct verdicts (missing/mistyped args, uncovered match patterns,
# bad/missing bundle paths, unknown ids, unreachable members).
chrome.userScripts.*: no-permission
chrome.userScripts.*: invalid-argument
chrome.userScripts.*: no-host-access
chrome.userScripts.*: bad-path
chrome.userScripts.*: no-such-file
chrome.userScripts.*: not-found
chrome.userScripts.*: unsupported
chrome.userScripts.*: unimplemented
# MD33: chrome.fontSettings is served over the real engine font
# defaults — 'fontSettings' gates the whole namespace like Chrome's
# declared grant (no-permission undeclared).  getFontList answers
# the system family list; getFont/setFont/clearFont map Chrome's
# servable generic families (standard/serif/sansserif/fixed/
# cursive/fantasy) onto the persisted websettings rows and
# re-apply live; the three get/set/clear*FontSize triples ride the
# composite standard/fixed font rows and minimumFontSize.  Chrome's
# 'math' generic family (no Qt family) and per-script (non-'Zyyy')
# fonts stay typed 'unsupported'.  The rest are the ops' correct
# verdicts (mistyped details/fontId/pixelSize, an unpublished
# genericFamily, an unknown fontId, unreachable members).
chrome.fontSettings.*: no-permission
chrome.fontSettings.*: invalid-argument
chrome.fontSettings.*: unsupported
chrome.fontSettings.*: unimplemented
# MD23: chrome.cookies is served over the RC15 persistent mirror
# (reads) + QWebEngineCookieStore live jar (writes land both).
# 'cookies' gates the namespace; url-bearing ops additionally need
# host coverage (no-host-access); the rest are the ops' correct
# verdicts (bad/missing args, non-http(s) set urls refused,
# partitionKey unsupported — no CHIPS, mirror write failure).
chrome.cookies.*: no-permission
chrome.cookies.*: invalid-argument
chrome.cookies.*: no-host-access
chrome.cookies.*: scheme-refused
chrome.cookies.*: unsupported
chrome.cookies.*: no-store
chrome.cookies.*: store-failed
# MD24: chrome.downloads is served over DownloadManager's record
# list — 'downloads' gates the namespace (no-permission
# undeclared).  The shelf surface (acceptDanger/drag/setShelfEnabled/
# setUiOptions), POST/headers/body download options and
# conflictAction 'overwrite' are typed unsupported; the deferred id
# push can reject 'download-failed' when the engine never mints an
# item.  The rest are the ops' correct verdicts (bad ids, wrong
# state for the action, missing files, platform reveals refused,
# icon pipeline empty, store/manager unreachable).
chrome.downloads.*: no-permission
chrome.downloads.*: invalid-argument
chrome.downloads.*: unsupported
chrome.downloads.*: unimplemented
chrome.downloads.*: scheme-refused
chrome.downloads.*: not-found
chrome.downloads.*: not-pausable
chrome.downloads.*: not-resumable
chrome.downloads.*: not-in-progress
chrome.downloads.*: not-complete
chrome.downloads.*: file-missing
chrome.downloads.*: reveal-failed
chrome.downloads.*: no-icon
chrome.downloads.*: no-store
chrome.downloads.*: start-failed
chrome.downloads.*: download-failed
# MD25: chrome.sessions is served over TabWidget's TABS05 closed-tab
# undo stack — 'sessions' gates the namespace (no-permission
# undeclared).  getDevices answers the honest empty list (no sync);
# getRecentlyClosed/restore enumerate/rebuild the menu's records.
# The rest are the ops' correct verdicts (bad maxResults or
# non-numeric sessionId, unknown/stale serial, empty stack,
# rebuild failure) plus unreachable members.
chrome.sessions.*: no-permission
chrome.sessions.*: invalid-argument
chrome.sessions.*: not-found
chrome.sessions.*: restore-failed
chrome.sessions.*: unimplemented
# MD26: chrome.tabGroups is served over the TABGRP machinery —
# 'tabGroups' gates the namespace (no-permission undeclared); get/
# query/update/move/ungroup are all live, creation rides
# chrome.tabs.group like the real API.  The rest are the ops'
# correct verdicts (bad/mistyped args, unknown or stale groupId,
# cross-window move has no atomic machinery, the strip refusing a
# move, unreachable members).  tabs.group/ungroup verdicts land
# under chrome.tabs.* below.
chrome.tabGroups.*: no-permission
chrome.tabGroups.*: invalid-argument
chrome.tabGroups.*: not-found
chrome.tabGroups.*: unsupported
chrome.tabGroups.*: move-refused
chrome.tabGroups.*: unimplemented
# MD34: chrome.declarativeNetRequest is served over
# DeclarativeNetRequest's registry + the live interceptor's
# block/redirect/setHttpHeader stage — 'declarativeNetRequest' or
# 'declarativeNetRequestWithHostAccess' gates the rule surface
# (WithHostAccess additionally scopes matching to host-permission
# coverage), 'declarativeNetRequestFeedback' gates getMatchedRules
# and onRuleMatchedDebug.  Response-side behavior the layer cannot
# reach (responseHeaders conditions, response header ops), tabIds
# conditions, RE2-incompatible regex constructs and
# setExtensionActionOptions stay typed 'unsupported'; the rest are
# the ops' correct verdicts (bad rule/options shapes, duplicate or
# missing rule ids, undeclared ruleset ids, session/dynamic/static
# quota caps, the 20-calls-per-10min getMatchedRules window,
# unreachable members).
chrome.declarativeNetRequest.*: no-permission
chrome.declarativeNetRequest.*: invalid-argument
chrome.declarativeNetRequest.*: duplicate-rule-id
chrome.declarativeNetRequest.*: rule-not-found
chrome.declarativeNetRequest.*: ruleset-not-found
chrome.declarativeNetRequest.*: quota-exceeded
chrome.declarativeNetRequest.*: unsupported
chrome.declarativeNetRequest.*: unimplemented
chrome.action.setIcon: invalid-image-data
chrome.action.setIcon: invalid-path
chrome.browserAction.setIcon: invalid-image-data
chrome.browserAction.setIcon: invalid-path
chrome.contextMenus.create: duplicate-id
chrome.contextMenus.remove: not-found
chrome.contextMenus.update: not-found
chrome.idle.queryState: invalid-argument
chrome.idle.setDetectionInterval: invalid-argument
chrome.management.get: invalid-argument
chrome.management.get: not-found
chrome.management.getPermissionWarningsById: invalid-argument
chrome.management.getPermissionWarningsById: not-found
chrome.management.getPermissionWarningsByManifest: invalid-argument
chrome.management.setEnabled: cannot-modify
chrome.management.setEnabled: invalid-argument
chrome.management.setEnabled: not-found
chrome.management.uninstall: cannot-modify
chrome.management.uninstall: invalid-argument
chrome.management.uninstall: not-found
chrome.notifications.clear: not-found
chrome.notifications.update: not-found
chrome.offscreen.closeDocument: not-hosted
chrome.offscreen.createDocument: invalid-argument
chrome.offscreen.createDocument: single-document
chrome.runtime.openOptionsPage: no-options-page
chrome.runtime.sendMessage: native-failed
chrome.scripting.*: bad-path
chrome.scripting.*: invalid-argument
chrome.scripting.*: no-frame
chrome.scripting.*: no-host-access
chrome.scripting.*: no-permission
chrome.scripting.*: no-such-file
chrome.scripting.*: not-found
chrome.storage.managed.*: read-only
chrome.storage.*: invalid-area
chrome.storage.*: invalid-argument
chrome.storage.*: quota-exceeded
chrome.tabs.captureVisibleTab: capture-failed
chrome.tabs.captureVisibleTab: invalid-argument
chrome.tabs.captureVisibleTab: no-permission
chrome.tabs.captureVisibleTab: no-window
chrome.tabs.captureVisibleTab: not-found
chrome.tabs.connect: invalid-argument
chrome.tabs.connect: no-host-access
chrome.tabs.connect: no-receiver
chrome.tabs.connect: not-found
chrome.tabs.connect: too-many-ends
chrome.tabs.connect: unsupported
chrome.tabs.create: no-tab
chrome.tabs.create: no-window
chrome.tabs.create: url-refused
chrome.tabs.discard: not-found
chrome.tabs.duplicate: invalid-argument
chrome.tabs.duplicate: no-tab
chrome.tabs.duplicate: not-found
chrome.tabs.get: not-found
chrome.tabs.getZoom: not-found
chrome.tabs.getZoomSettings: not-found
chrome.tabs.goBack: no-history
chrome.tabs.goBack: not-found
chrome.tabs.goForward: no-history
chrome.tabs.goForward: not-found
chrome.tabs.group: invalid-argument
chrome.tabs.group: not-found
chrome.tabs.group: no-window
chrome.tabs.group: group-refused
chrome.tabs.highlight: invalid-argument
chrome.tabs.highlight: not-found
chrome.tabs.highlight: no-window
chrome.tabs.move: invalid-argument
chrome.tabs.move: move-refused
chrome.tabs.move: not-found
chrome.tabs.move: no-window
chrome.tabs.reload: not-found
chrome.tabs.remove: invalid-argument
chrome.tabs.remove: not-found
chrome.tabs.sendMessage: no-host-access
chrome.tabs.sendMessage: no-receiver
chrome.tabs.sendMessage: not-found
chrome.tabs.setZoom: invalid-argument
chrome.tabs.setZoom: not-found
chrome.tabs.setZoom: zoom-disabled
chrome.tabs.setZoomSettings: invalid-argument
chrome.tabs.setZoomSettings: not-found
chrome.tabs.ungroup: invalid-argument
chrome.tabs.ungroup: not-found
chrome.tabs.update: not-found
chrome.webNavigation.getAllFrames: no-tab
chrome.webNavigation.getFrame: no-frame
chrome.webNavigation.getFrame: no-tab
chrome.webRequest.subscribe: invalid-argument
chrome.webRequest.subscribe: no-webRequest-permission
chrome.windows.create: no-window
chrome.windows.create: url-refused
chrome.windows.get: not-found
chrome.windows.getCurrent: no-window
chrome.windows.getLastFocused: no-window
chrome.windows.remove: not-found
chrome.windows.update: not-found
```

## Feasibility triage (MD07)

`.devin/EXT-DISPATCH-AUDIT.md` classifies every entry above:
`queued -> MDnn` means the surface is servable through shim dispatch
plus Qt/rustcore state we already have (no new engine JS APIs); the
rest stay unsupported with reasons (ChromeOS surfaces, external
services, blocking interception, missing UI surfaces like
sidePanel/omnibox).  A queued entry is removed from the fenced block
only by the run that lands it — until then the typed error is the
honest answer and the test contract stands.

Close-out re-audit (MD07): every first-wave row (MD10-MD33) landed;
each surviving emit site in the dispatch was re-traced to a declared
entry.  Second-wave queue: declarativeNetRequest subset -> MD34
(blocking/redirect verdicts already execute in
PrivacyRequestInterceptor), identity.getRedirectURL -> MD35,
webNavigation.onCreatedNavigationTarget -> MD36, action
getUserSettings/onUserSettingsChanged -> MD37.  Unchanged stays:
runtime grant UI (permissions.request/remove, sidePanel/sidebarAction
open) is MW-SHELL territory; ChromeOS/device namespaces; MV2-deprecated
spellings (tabs.executeScript family, declarativeContent); surfaces
with no engine hook (webRequest response-side + blocking-family
events, subframe addressing, i18n/tabs detectLanguage).

## Out of scope by design

- **chrome.runtime.connectNative** — returns a dead port with a
  `no-native-messaging` note, not a gap entry; there is no native
  messaging host to bridge to.
- **chrome.extension.getBackgroundPage / getViews Window objects** —
  MV3 semantics answer null/context-counts; a Window cannot cross
  the channel, and faking one is worse than the honest null.
- **chrome.idle 'locked' state and OS-wide idle** — measured inside
  Arktos windows only; the shim notes the approximation once per
  context rather than emitting a gap on every query.
- **chrome.webRequest blocking** — Qt WebEngine's interceptor is
  observe-only at our layer; blocking subscriptions fail typed at
  `subscribe`, covered by the wildcard above.
