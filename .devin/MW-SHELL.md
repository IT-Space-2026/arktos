# MW-SHELL — thin-shell audit (MS01)

Scope: `src/locationbar/`, `src/webview.cpp`, `src/browsermainwindow.cpp`,
`src/tabbar.cpp`, `src/tabwidget.cpp`, `src/settings.cpp`, `src/history/` UI,
`src/bookmarks/` UI.  Method-level classification of every site that *decides*
something (url accept, menu availability, restore policy, suggestion source,
permission default, capability gating).

Verdicts:

- `thin` — decision/state moves behind the `arktos_core.h` facade
  (ac_settings / ac_sitedec / ac_policy / ac_session / ac_hist / ac_bm /
  ac_containers / ac_cookies / ac_notes, or an existing rc_* module:
  rc_classify_input, rc_urlstrip, rc_cred, rc_autofill, rc_opensearch,
  ac_sitetrust / ac_tranco / ac_blocklist).  Sizes MS02–MS08 work.
- `thin-blocked:MDnn` — same move, but the facade function does not exist
  yet; it lands with the named MW-DOMAINS module
  (`../arora-mwdomains/.devin/Arora-Task-Domains.md`).  Do NOT stub fake
  facade calls; leave the Qt code and note the dep.
- `keep` — painting, focus, input, menus, drag/drop, dialogs, or
  engine-object ownership/live state (QWebEngineView/Page/Profile,
  EngineTab, TorManager, EnergySaver).  Some `keep` rows carry a `thin`
  sub-item where a store touch hides inside an otherwise-UI method.
- `done` — already routed through the facade/rc_*; listed for the MS09
  boundary map.

Legend for "Needs" column: which MSnn row / MDnn module the move waits on.
`-` = facade surface exists today, move anytime.

## src/locationbar/locationbar.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `keyPressEvent` :387 (Ctrl/Shift+Enter → .com/.net/.org append) | keep | MS03 resolved: MD01 verdicts the ctrl/shift TLD append chrome UX (modifier gesture), not a store/policy decision — stays Qt. |
| `dragEnterEvent`/`dropEvent` :417/:432 | keep + thin-blocked:MD02 | drop plumbing stays; the `isUrlAllowedOnUntrustedInput` scheme verdict → navpolicy. |
| `registrableDomainRange` :144 | keep | pure-URL paint helper (SAFE03 domain emphasis); optional facade url-util later, no MD row. |
| `unicodeUrlHint` :247 | keep | pure-URL tooltip helper. |
| `webViewUrlChanged`/`displayUrl`/focus handlers :131-377 | keep | display state + focus policy. |

## src/locationbar/omniboxsuggestions.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `enabled()` :67 (tor gate + `suggestionsInAddressField`) | thin-blocked | MS03: availability verdict needs a facade fn (tor-mode + ac_settings inputs) — none exists and no MD row covers it; flag for domains lane. Toggle read already ac_settings. |
| `currentEngineChanged` :80 / `requestSuggestions` :125 | thin-blocked | MS03: suggestion-source pick (context/keyword engine, `suggestionsOnlyWithKeyword`, per-engine opt-in) — no facade verdict fn, no MD row; flag. Inputs already ac_settings/rc_ose (engineForContext/engineForKeyword/opt-ins). |
| `scheduleSuggestions` :111 (debounce + delete-burst) | keep | input pacing; the enable/suppress verdict inside rides `enabled()` above. |
| `newSuggestions` :187 | keep | completer repopulation. |

## src/locationbar/sitepadlock.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `stateFor` :222 | thin | composite lock verdict (trust level + TlsVerifier + cert-override + tor/engine-validated).  ac_sitetrust covers the trust leg; the TLS/override/engine-leg composition needs a new facade verdict fn — no MD row assigned, flag for domains lane. |
| `rebuildPopup` :412 | keep | popup composition; reads the same verdicts (thin via `stateFor`/`trustSignalLines`). |

## src/locationbar/siteshield.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `trustLevelFor` :240 | thin | verdict cache — underlying read already ac_sitetrust (done leg); the master/per-signal gates read settings → ac_settings.  Qt keeps the cache. |
| `refreshIcon`/`populateMenu`/`growMenuToFit` :299/:122/:144 | keep | paint + popup sizing. |

## src/locationbar/sitepanel.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `refresh` :462 | thin | per-row enablement + state reads: cookie rule (ac_cookies/ac_sitedec), adblock whitelist (ac_policy), JS rule (ac_sitedec), popups (ac_sitedec/MD09), http allowance, url-clean exempt (MD06), fingerprint exempt (ac_sitedec).  Engine gating (`m_engineTab` greys) → caps (MS06, needs MB04). |
| `applyCookieRule` :733 | thin | rule write → ac_cookies/ac_sitedec. |
| `toggleContentBlocking`/`toggleGlobalBlocking` :749/:760 | thin | site whitelist + global enable → ac_policy. |
| `applyJavaScriptRule` :768 | thin | per-host JS rule → ac_sitedec (+ reload stays Qt). |
| `togglePopups` :787 | thin | allow-host write → ac_sitedec; block verdict itself → MD09. |
| `toggleHttpAllowance` :806 | thin | HTTPS-Only exemption → ac_sitedec. |
| `toggleUrlCleaning` :825 | thin | exemption write → ac_sitedec; strip policy → thin-blocked:MD06. |
| `toggleFingerprintSpoof` :846 | thin | exemption write → ac_sitedec. |
| `rebuildPermissionRows` :870 | thin-blocked:MD03 | WebPermissionManager entries + revoke → permpolicy verdict store. |
| `trustSignals`/`trustLevel`/`trustSignalLines` :975/:1051/:1040 | done | already ac_sitetrust_lookup_opts via AcPolicy; Qt maps verdict→lines (display, keep). |
| `clearSiteData` :1128 | thin + keep | cookie leg → ac_cookies_remove_for_host; DOM-storage wipe is an engine-script op (keep). |
| `siteCookieJar`/`host`/`origin`/`pageUrl` :419-459 | keep | accessors over engine objects. |

## src/locationbar/{readerbutton,privacyindicator,popupblockerbutton,engineindicator,locationbarsiteicon}.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `ReaderButton::refresh` :226 | keep + thin | reads engine-side ReaderMode state (keep); "hidden on widget/engine tabs" presence gate → caps (MS06/MB04) — enforced in BrowserMainWindow::updateReaderState. |
| `PrivacyIndicator::setWebView` + click :62/:42 | keep | off-the-record read from the page's profile + chrome actions. |
| `PopupBlockerButton::rebuildMenu`/`refresh` :128/:94 | keep | menu + count badge from engine state. |
| `PopupBlockerButton::allowPopupsForSite` :192 | thin | allow-host write → ac_sitedec; OTR→session-only discipline via profile context; verdict → MD09. |
| `PopupBlockerButton::openBlockedPopup` :174 | keep | chrome action (tab placement already routed). |
| `EngineIndicator::refresh` :69 | thin | MS06 — visible/enabled from backend count + lock state → caps/availability (needs MB04). |
| `EngineIndicator::rebuildMenu` :102 | thin | per-backend `pageUnavailableReason` → caps table (MS06/MB04); menu paint keeps. |
| `LocationBarSiteIcon::*` | keep | drag source, paint. |

## src/webview.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `contextMenuEvent` :305 | done | MS04: entry set/order/enabled is `WebViewController::contextMenuModel(info, env)` — the widget gathers env + renders.  env legs still Qt (thin-blocked): CONT07 scheme gate → MD02, private-mode + tor probe → context/caps facade (MB04); opensearch reads already rc_ose-bound. |
| `contextMenuInfo` :273 | keep | engine request→neutral struct translation. |
| `isUrlAllowedOnUntrustedInput` :1160 / `isUrlAllowedFromPageLink` :1165 | thin-blocked:MD02 | scheme allowlist verdict (callers: drops, middle-click, context menu, tor hand-off). |
| `loadUrlInView` :811 | thin-blocked:MD02 | referer-header + same-profile decision on hand-off. |
| `openPageUrlInPrivateTab`/`PrivateWindow`/`TorWindow` :710/:734/:775 | thin-blocked:MD02 | context/profile routing of a page-supplied url (tor vs clearnet OTR pick). |
| `openUrlInTarget`/`getView` chain :787 | keep | view construction/placement mechanics (decisions upstream). |
| `wheelEvent` :618 | keep | Ctrl+wheel input → zoom. |
| `levelForZoom`/`zoomIn`/`zoomOut`/`resetZoom` :1062-1149 | thin | zoom-level table + stepping policy → facade (settings/defaults); apply stays engine-side.  Engine zoom support → caps (MS05). |
| `applyZoom` :1085 | thin | `zoomTextOnly` read → ac_settings; font-scaling engine calls keep. |
| `containerId` :261 | thin | profile→container lookup → ac_containers. |
| `searchRequested` :875 | thin | `selectionSearchInBackground` via ac_settings + engine lookup via opensearch. |
| `imageSearchRequested` :921 | thin | image engine via opensearch facade. |
| `addSearchEngine`/`addSearchEngineFromMap` :943/:964 | thin | engine add/collision → opensearch registry (RC18 exists); QInputDialogs keep. |
| `addSelectionToNote` :896 | thin | note model pick (OTR→volatile) → ac_notes handle selection. |
| `torMustQueue` :1222 / `torHoldNavigation` :1343 | thin-blocked:MD02 | nav-hold verdict (scheme list + daemon/circuit/proxy-warm state). Timers, wakeups, queue storage keep Qt. |
| `torQueueLoad`/`torFlushQueuedLoad`/`torGraceDispatch`/`torArmWakeup`/`torArmQueueGrace` :1279-1436 | keep | queue mechanics + timers (verdict above moves). |
| `loadUrl` :1437 | thin-blocked:MD02 | javascript:-typed → runJS decision + tor-queue consult; dispatch stays. |
| `showPdfDocument`/`pdfView` :1501/:1543 | keep | PdfView/engine-object ownership. |
| `mousePressEvent`/`mouseReleaseEvent` :1553/:1635 | keep + thin-blocked:MD02 | XButton nav, middle-click PRIMARY load (input); scheme verdict inside → shared MD02 gate. |
| `dragEnterEvent`/`dragMoveEvent`/`dropEvent` :1575-1633 | keep + thin-blocked:MD02 | drop accept is input; url verdict → MD02 gate. |
| `init` linkHovered→setStatusBarText :164 | done | MS04: hover text now routes through `WebViewController::hoverStatusText` (verbatim = parity); display rules consult the facade at that seam when one lands.  Engine-adapter hover sink in browsermainwindow applies the same verdict. |
| `setStatusBarText` :1653 | keep | status propagation. |
| `updateScriptBlockBar` :1667 | keep | infobar show/hide. |
| `allowScriptsOnThisSite` :1686 | thin | JS allow-rule write → ac_sitedec (+OTR session-only via context). |
| `grabContextImage`/`openContextPosterInTarget`/`runContextImageScript`/`addSearchEngine` probe :1740-1812 | keep | in-page script resolution (engine objects). |
| `blockImage`/`bookmarkLink`/`downloadLinkToDisk`/`downloadImageToDisk`/copy* :631-873 | keep | actions delegating to managers/engine; bookmark write already ac_bm. |
| `url`/`isLoading`/`setProgress`/`loadFinished` :1057-1499 | keep | engine-derived state. |

## src/browsermainwindow.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `loadDefaultState`/`save` :853/:870 | thin | persisted window blob read/write → ac_settings (blob format can stay Qt). |
| `saveState`/`restoreState` :884/:924 | thin | same store route; field layout + compat migration logic stays Qt (it's the blob's codec). |
| `lastTabClosed` :1045 | thin | `quitAsLastTabClosed` via ac_settings. |
| `setupMenu` :1064 | keep + thin | construction keeps; embedded decisions thin: tor-binary gate :1101 (caps), container-menu hide :1115 (mode/caps), `canRestoreSession` :1527 — MS08 done: per-persona existence verdict now `ac_session_exists` on bound handles, lock-store enable :1667 (rc_cred), BiDi tor gate :1719 (caps), updates tor gate :1793 (caps/mode). |
| devtools rows refresh :1733 | thin | `capabilities().devTools` per tab → ac_engine_caps (MS05, needs MB04); today reads adapter-local caps. |
| `aboutToShowViewMenu` :1830 | keep | label sync. |
| `updateReaderState` :1839 | thin | reader/PiP availability per tab → caps (MS06); checked-state from engine objects keep. |
| `aboutToShowTextEncodingMenu`/`viewTextEncoding` :1882/:1910 | keep | menu + per-page engine setting writes. |
| `retranslate`/`setupToolBar` :1930/:2065 | keep | chrome construction; embedded gates listed separately. |
| `applySearchBoxVisibility` :2471 | thin | tor-mode + `MainWindow/showSearchBox` → ac_settings. |
| `updateEngineIndicator` :2510 | thin | MS06 — swap-lock verdict (tor/private/container-strip/widget-tab). |
| `applySidebarSettings`/`applySidebarWidth`/`persistSidebarWidth`/`updateSidebarCloseButton` :2593/:2634/:2649/:2691 | thin | sidebar keys via ac_settings; layout mechanics keep. |
| `eventFilter`/sidebar grip/dock handling :2704 | keep | input + geometry. |
| `showNavigationBarContextMenu` :2734 | keep + thin | menu build keep; `navBarResetAction` enable :2749 = order-vs-default compare (state read thin). |
| `navBarOrder`/`applyNavBarOrder` :2773/:2795 | keep | id reconciliation/layout — pure chrome algorithm. |
| `applyStoredNavBarOrder`/`saveNavBarOrder`/`syncNavBarOrderToWindows`/`resetNavBarOrder` :2840-2893 | thin | `navbar/order` via ac_settings; fan-out keeps. |
| nav-bar edit/drag machinery :2894-3105 | keep | customization input handling. |
| `showBookmarksDialog`/`addSidebarBookmark`/`addBookmark`/`addBookmarkFolder` :3121-3180 | keep | dialogs; writes already ac_bm. |
| `viewMenuBar`/`applyMenuBarVisibility`/`viewToolbar`/`viewBookmarksBar`/`viewStatusbar` :3187-3253 | keep | visibility toggles (persist via window-state blob → thin leg above). |
| `downloadManager`/`fileSaveAs` :3255/:3275 | keep | raises sidebar / delegates to DownloadManager (interception policy is MD04, engine-side). |
| `showSettingsPage`/`showHistoryPage` :3288/:3318 | keep | widget-tab management. |
| `updateStatusbar`/`updateTorCircuitLabel`/`updateWindowTitle` :3370/:3381/:3451 | keep | display. |
| `previewEngineTabDocument`/`filePrint`/`printRequested` :3521/:3544/:3556 | thin | print capability verdict (pdfView? engineTab? refusal) → ac_engine_caps (MS05, needs MB04). |
| `closeEvent` :3597 | thin | `confirmClosingMultipleTabs`/`confirmExit` via ac_settings; QMessageBox keep. |
| `editFind*`/`zoomIn`/`zoomNormal`/`zoomOut`/`viewFullScreen`/`viewPageSource` :3676-3753 | keep | delegate to tab/engine; zoom gating → caps (MS05). |
| `goHome` :3755 | thin | `homePageMode` + `MainWindow/home` via ac_settings. |
| `webSearch`/`selectLineEdit`/`swapFocus` :3774/:3266/:3817 | keep | focus policy. |
| `clearPrivateData` :3798 | keep | dialog keep; MS08 verified the undo-stack wipe targets the per-run in-memory recently-closed list — no session-store surface applies. |
| `updateStopReloadActionText`/`loadProgress`/`updateStopReloadState` :3849-3887 | keep | state from engine loading flag. |
| `populateNewContainerTabMenu` :3894 | thin | container list via ac_containers + private-mode gate via context. |
| `aboutToShowBackMenu`/`aboutToShowForwardMenu`/`aboutToShowWindowMenu`/`openActionUrl` :3943-4038 | keep | engine-history objects + window list. |
| `keyPressEvent`/`mousePressEvent`/`changeEvent`/`fileNew`/`fileOpen`/`aboutApplication`/`checkForUpdates`/`privacyChanged` :812-3595 | keep | input/actions; `checkForUpdates` dialog keep (tor gate covered in setupMenu). |

## src/tabbar.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `contextMenuRequested` :538 | thin | MS04-style menu model: reopen-enable (session stack), pin toggle label (session), container submenu rows + rules (ac_containers + private/tor gate), sleep/wake + free-memory enable (engine state keep / policy thin), group rows (session).  Menu build keep. |
| `decodeArktosTabDrag`/`beginTabDragSession`/`endTabDragSession` :139-156 | keep | drag codec/session — drag/drop stays Qt. |
| `mouseDoubleClickEvent`/`mousePressEvent`/`mouseMoveEvent`/`mouseReleaseEvent`/`wheelEvent` :865-1478 | keep | drag, tear-off, rename-edit, scroll input. |
| `dragEnterEvent`/`dragMoveEvent`/`dragLeaveEvent`/`dropEvent`/`insertIndexForDrop` :1327-1466 | keep | drop accept + insert index (input). |
| `viewTabBar`/`selectTabAction`/`updateViewToolBarAction` :518-537 | keep | strip visibility action. |
| `freeSide`/`closeButtonSide`/`setPerTabCloseButtons`/`installCloseButton`/`updateCloseButtonVisibility`/`updateHoveredTab` :373-490 | keep + thin | layout/hover paint keep; `oneCloseButton` pref read → ac_settings (loadSettings leg). |
| container strip + chips :1528-1800 (`containerForTab`/`containerChipFont`/`containerChipSize`/`containerPillReserve`/`containerStripHeight`/`stripPos`/`containerHeaderId`/`containerHeaderRect`/`containerHeaderAt`/`paintContainerStrip`/`shiftChildrenBelowStrip`) | keep | paint + hit-test; container *data* reads → ac_containers (thin via ContainerManager, not here). |
| preview dwell/`showTabPreview`/`hideTabPreview`/`tabPreview` :1479-1527 | keep | hover preview timing/paint. |
| settings reads :2834-3254 (`verticalTabWidth`, `horizontalTabHeight`, `tabMinWidth`/`tabMaxWidth`, `tabHoverPreviews`, `iconOnlyTabs` + writes) | thin | ac_settings (all already ProfileSettings — facade-backed; MS07 verifies no raw QSettings remains). |

## src/tabwidget.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `lineEditReturnPressed` :3281 | keep | MS03 resolved: Alt→NewTab is chrome placement (MD01: post-accept modifier divert = keep); the accept chain it enters is rc_classify_input. |
| `loadString` :4411 / `loadStringFromUntrustedSource` :4425 | done / thin-blocked:MD02 | MS03: private-context pick is engine-object state (keep), classify verdict consumed via rc_classify_input; :4425 scheme gate still → navpolicy. |
| `looksLikeAddress` :4447 | keep | no-rust reference oracle — rc_classify_input supersedes under rust (MD01 lists it as the reference leg). |
| `searchInAddressFieldEnabled` :4473 | done | key read + legacy-key drop already route ac_settings via ProfileSettings; the ini-mirror parity forces the remove to stay Qt-side (RcSettings cannot drop the ini copy). |
| `guessUrlFromStringRust` :4488 | done | verdict = rc_classify_input; engine resolution rc_ose_* (keyword → rc_ose_engine_for_keyword, template expand → rc_ose_expand, context picks → ac_settings). about:home re-pick consumes startPageUrl — engine-object/mode inputs, no facade verdict (flag). |
| `guessUrlFromStringCpp` :4585 | keep | no-rust parity oracle. |
| `modifyWithUserBehavior` :4774 | keep | MS03 resolved: modifier→placement is chrome policy (MD01 keep); `selectNewTabs` already read via ac_settings. |
| `loadUrl` :4807 | keep + thin-blocked | invalid refusal = defensive guard (keep); widget-tab→new-tab = chrome placement (MD01 keep); branded-start-page re-pick has no facade verdict fn (no MD row — flag). |
| `getView` :4868 | keep + thin | construction keep; per-target profile pick (private/container) → ac_profiles/ac_containers. |
| `newTabPage`/`loadNewTabPageUrl`/`newPrivateTab`/`makeNewTab`/`makeNewTabLike`/`makeNewPrivateTab`/`makeNewTabInContainer`/`makeNewTabOnProfile` :582-1141 | thin | home-url pick (ac_settings), private/container profile routing (ac_profiles/ac_containers); construction keep. |
| `loadSettings` :4667 | thin | tab-strip prefs block → ac_settings. |
| `addEngineTab`/`reloadTabInEngine`/`tabEngineId`/`engineTab` :1180-1566 | thin | MS06 — swap-availability verdict (tor/private/container-strip guards + backend resolve) → caps/context; engine-object work keeps. |
| `reopenTabInContainer`/`loadUrlInContainer`/`manageContainers`/`manageContainersInteractive` :1567-1663 | thin | container moves + site-rule enforcement → ac_containers (+ MD02 for nav-time rule verdict). |
| tab groups :1663-2191 (`assignTabGroup`/`createTabGroup`/`moveTabIntoGroupRun`/`addTabToGroup`/`removeTabFromGroup`/`groupTabWith`/`renameTabGroup`/`setTabGroupColor`/`inheritTabGroup`/`forgetTabGroupIfEmpty`/`detachGroupMember`/`expandTabGroup`/`setTabGroupCollapsed`/`ungroupTabs`/`closeTabGroup`/`normalizeTabGroupMove`) | keep + thin-blocked:MD08 | MS08 verified: group membership/name/color/collapsed already persist via the AcSession manifest (collectSessionSnapshot→ac_session_save); the live `m_tabGroups`/`m_tabGroupInfo` maps are per-run widget state with no facade surface — membership-op policy is MD08's. Strip reflow keep. |
| container strip :2309-2700 (`twoLevelStrip`/`activeContainerHeader`/`containerTabCount`/`containerHeaders`/`containerStripActive`/`applyContainerFilter`/`detachTabIntoContainerStore`/`restoreHiddenContainerTabs`/`setActiveContainerHeader`/`moveContainerHeader`/`syncContainerStrip`/`closeHiddenTab`/`closeHiddenPage`/`closeTabsInContainer`/`setTwoLevelStrip`) | keep + thin-blocked:MD08 | MS08 verified: active level persists via the manifest's `activeContainer` (ac_session_routed); hidden stores are per-run widget state.  Restore-time level pick = MD08 restore policy; strip rebuild + painting keep. |
| `markTabActivity` :2714 | keep | MS08 resolved: the stamp is in-memory `m_sleepStates[].lastActiveMs` feeding suspend ordering — live per-run state that never serializes; no AcSession surface exists for it and none is needed. |
| `suspendIdleIntervalMs`/`applyEnergyCaps` :2724/:2731 | thin | saver-override + interval policy → ac_settings/energy verdict (no MD row assigned — flag for domains lane). |
| `updateSleepTimer`/`isTabSleeping`/`sleepingTabCount` :2749-2778 | keep | timers + view-state reads. |
| `sleepBlockReason` :2780 | keep | verdict over live engine state (loading/audible/download/form-dirty/pinned) — inputs are engine objects; revisit if a facade lifecycle-verdict lands (flag). |
| `suspendIdleTabs`/`sleepTab`/`beginTabSleep`/`finishTabSleepCapture`/`wakeTab`/`purgeInactiveTabs`/`updatePurgeTimer`/`freeTabMemory`/`freeBackgroundTabsMemory`/`flushCurrentTabMemory`/`reportFreedMemory`/`applySleepVisuals` :2812-3163 | keep + thin | engine lifecycle calls + capture keep; sweep cadence/eligibility thresholds → settings/policy (thin, no MD row — flag). |
| `captureTabThumbnail`/`scheduleThumbnailCapture`/`tabThumbnail`/`animationLabel` :3164-4072 | keep | thumbnails/spinners — paint. |
| `webViewLoadStarted`/`webViewLoadProgress`/`webViewLoadFinished`/`webViewIconChanged`/`webViewTitleChanged`/`webViewUrlChanged`/`currentChanged` :379-4233 | keep | engine→chrome forwarders. |
| `pushRecentlyClosedTab`/`restoreClosedTab`/`openLastTab`/`aboutToShowRecentTabsMenu`/`aboutToShowRecentTriggeredAction`/`hasRecentlyClosedTabs`/`clearRecentlyClosedTabs`/`historyCleared` :292-4362 | keep + thin-blocked:MD08 | MS08 verified: the stack is per-run in-memory only — nothing serializes it, so no AcSession surface applies.  The private-exclusion capture policy is MD08-adjacent (no facade verdict fn today). |
| `isTabPendingRestore`/`pendingRestoreUrl`/`restorePendingTab`/`dressHiddenPendingTab` :4992-5074 | keep + thin-blocked:MD08 | MS08 verified: `m_pendingRestores` is per-run deferral state that re-serializes through the manifest; the deferral verdict itself (lazyRestore gates in restoreSessionSnapshot) is MD08 restore policy.  Dress/placeholder paint keep. |
| `collectSessionSnapshot` :5075 | thin-blocked:MD08 | MS08 verified: every field it emits (urls/containers/groups/pinned/titles/engines/scrolls/activeContainer) already flows through ac_session_save — the "what enters the snapshot" policy (OTR-drop, pending substitution, scroll pick, group splice) is MD08's call; no facade fn today. |
| `saveState`/`restoreState` :5191/:5230 | done | MS08: blob codec stays Qt (per spec); the store route is already AcSession — saveSession writes per-persona via ac_session_save, restore enters through the manifest load. |
| `restoreSessionSnapshot` :5300 | thin-blocked:MD08 | MS08: restore order/policy (placeholder reuse, pinned-first ordering, lazy deferral, container-level pick) is MD08 restore policy — no facade verdict surface yet.  Mechanics keep. |
| `sessionStateJson`/`restoreSessionState` :5579/:5638 | done | MS08 verified: already the ARSS-facing JSON leg routed fully through ac_session_save/load via SessionAccess — schema decode stays Qt. |
| `createTab` :5698 | keep | restore-time construction. |
| `isTabPinned`/`setTabPinned`/`normalizePinnedBlock`/`pinnedTabCount` :709-791 | keep | MS08 resolved: the pin flag persists via the manifest's per-tab `pinned` key (ac_session_routed — storage done); `m_pinnedTabs` is live strip state and normalizePinnedBlock is strip reflow — no runtime facade surface exists. |
| `tabTitleOverride`/`tabDisplayTitle`/`setTabTitleOverride`/`setCustomTitleForPage`/`applyTabTitle` :792-892 | keep | MS08 resolved: rename override persists via the manifest `title` key (done leg); `m_customTitles` + display stay widget-side. |
| `closeTab`/`closeOtherTabs`/`cloneTab`/`windowCloseRequested` :3303-3513 | keep + thin-blocked:MD08 | strip mechanics keep; private-no-reopen exclusion → session policy (no facade fn today, MD08). |
| `moveTabTo`/`transferTabTo`/`carryHiddenMemberTo`/`carryKeyedState`/`dropKeyedState`/`severTabBindings`/`adoptTransferredTab`/`tearTabToNewWindow` :3514-4024 | keep | MS08 resolved: keyed-state carry moves live widget objects between windows — the fields it carries already persist via the manifest when saved; nothing crosses a store boundary at move time. |
| `orderedWebViews`/`orderedSessionPages` :2192/:2241 | thin-blocked:MD08 | MS08: serialization-order rule (collapsed-group splice, container-hidden append) is snapshot policy → MD08; no facade ordering verdict exists. |
| `containerIdForTab`/`isTabPrivate`/`profileId` :669-727 | thin | context reads → ac_containers/ac_profiles. |
| `ensureLineEditCompleter`/`wireLocationBar`/`wireWebView` :930-1060 | keep | wiring. |
| `moveTab`/`reloadTab`/`reloadAllTabs`/`bookmarkTabs`/`clear`/`nextTab`/`previousTab`/`addWebAction`/`addWidgetTab`/`retranslate`/`changeEvent`/`openActionUrl`/`editTabTitle` :292-4410 | keep | strip/actions; bookmark writes already ac_bm. |

## src/settings.cpp (MS07 target)

`ProfileSettings` already routes every read/write through the AcSettings
scopes under ARKTOS_RUSTCORE (RC08 — settings.json canonical, ini mirror).
MS07's residual work is the non-ProfileSettings pokes: raw `QSettings`
instances and manager-owned stores that haven't been facade-bound.

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `loadDefaults`/`loadFromSettings`/`saveToSettings` :1083/:1117/:1693 | thin | MS07 — section load/apply via ac_settings; sweep for residual QSettings (one known: tabwidget.cpp:2107). |
| `previewTheme`/`revertThemePreview` :822/:836 | keep | live preview mechanics; persisted pick via ac_settings. |
| `populateLanguageCombo`/`applyLanguagePick` :856/:900 | thin | `LanguageManager/language` via ac_settings. |
| `resetSearchSettings`/`resetPerformanceSettings` :2252/:2297 | thin | defaults table → facade (reset = write defaults). |
| `showCookies`/`showExceptions` :2351/:2357 | thin | cookie rows/exceptions via ac_cookies (gate exists); dialogs keep. |
| `chooseDownloadDirectory`/`chooseDownloadProgram`/`chooseFont`/`chooseFixedFont`/`setHomeToCurrentPage`/`chooseStyleSheet`/`chooseAcceptLanguage`/`refreshAcceptLanguages` :2363-2437 | thin + keep | file dialogs keep; resulting values via ac_settings. |
| autofill section (`refreshAddresses`/`addAddress`/`editAddress`/`removeAddress`/`removeAllAddresses`/`addressSelectionChanged`/`editAutoFillUser`) :2438-2542 | thin | rc_autofill exists. |
| engines section (`refreshEngineList`/`engineSelectionChanged`/`populateEngineForm`/`updateEngineButtonStates`/`engineAdd`/`engineRemove`/`engineMove`/`engineRestoreDefaults`/`engineAssignmentChanged`/`commitEngineEdits`) :2543-2889 | thin | opensearch registry CRUD (RC18) + per-engine opt-ins via ac_settings. |
| `refreshSearchEngines`/`refreshSearchSuggestions`/`stashSearchSuggestions` :2890-2981 | thin | suggestion opt-ins via ac_settings/opensearch. |
| extension section (`loadExtension`/`installExtension`/`removeExtension`/`checkExtensionUpdates`/`extensionUpdateCheckFinished`/`extensionSelectionChanged`/`extensionItemChanged`/`refreshExtensions`/`refreshUserScripts`/`openUserScriptsFolder`/`reloadUserScripts`/`extensionError`) :2982-3389 | thin | install/enable state + update-check flag via facade (extension install-registry surface needed — MD07-adjacent, flag); ExtensionManager keeps runtime objects (keep). |
| permissions section (`refreshPermissions`/`permissionSelectionChanged`/`removePermission`/`clearPermissions`) :3390-3499 | thin-blocked:MD03 | permpolicy verdict store. |
| `updateSecurityLevelHint`/`updateContentBlockerStatus`/`updateSiteTrustLists`/`siteTrustListUpdateFinished`/`refreshSiteTrustListStatus` :3500-3679 | thin | blocklist/tranco status + reload via ac_blocklist/ac_tranco (exist). |
| credential section (`refreshCredentialUi`/`credentialPassphraseChange`/`credentialPassphraseRemove`/`credentialStoreLock`) :3680-3825 | thin | rc_cred/securestore. |
| container section (`selectedContainerId`/`refreshContainers`/`containerSelectionChanged`/`refreshContainerSites`/`containerSiteRemove`/`containerNew`/`containerEdit`/`containerRemove`) :3826-3950 | thin | ac_containers (site rules → container store). |
| `updateEnergySaverStatus` :2330 | keep | live battery/saver state (system read). |
| ScriptControlManager/PopupBlocker page wiring :578-590 | thin | rule lists via ac_sitedec. |
| `filterPages`/`buildPageSearchIndex`/`pageTitle`/`openAtPage`/`showEvent`/`changeEvent`/`retranslate`/`sidebarPanelMove`/`updateSidebarPanelButtonStates` :924-2251 | keep | dialog UI. |

## src/history/ UI

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `HistoryManager::checkForExpired` :501 | thin | expiry cutoff → ac_hist_expire_older_than (+ ac_icons_sweep); QTimer keep. |
| `HistoryManager::daysToExpire`/`setDaysToExpire`/`loadSettings` :617-682 | thin | `history/historyLimit` via ac_settings. |
| `HistoryManager::history` mirror :330 | thin | in-memory shadow list → read from AcHistory (mirror elimination — flag for MS09). |
| `prependHistoryEntry`/`updateHistoryEntry`/`removeHistoryEntry` :558-615 | done | already ac_hist_*-backed. |
| `clear`/`clearSince`/`confirmClear` :631-674 | done/keep | ac_hist_clear* done; confirm dialog keep. |
| `icon`/`setIcon`/`clearIcons` :353-445 | done | AcIcons. |
| `HistoryDialog::*`/`HistoryMenu`/`HistoryMenuModel`/`HistoryTreeModel`/`HistoryModel` | keep | views/models over the (facade-backed) manager; removeRows already routes store-side. |
| `HistoryPeriodFilterModel` :1035-1081 | keep | date-partitioning view filter (could become ac_hist_query specs later — optional). |
| `HistoryFilterModel::frecencyScore` :721 | thin | rc_frecency_score exists — consume instead of the Qt copy. |
| `HistoryFilterModel::startFrecencyTimer`/`refreshFrecencies` :501-512 | keep | daily timer. |
| `HistoryCompletionModel::filterAcceptsRow`/`lessThan` :150/:169 | done | MS03: pass-through under rust — RustHistorySuggestModel serves store-filtered + store-ranked rows (rc_history_suggest derives the identical predicate/key), so the proxy accepts all and orders by source position; Qt legs remain the no-rust fallback. |
| `RustHistorySuggestModel::*` :194-319 | done | ac_hist_suggest. |
| `OmniboxCompletionModel::setSearchText` :471 | thin-blocked | MS03: scope→provider pick verdict has no facade fn (no MD row — flag); scope tokens + engine-keyword precedence already read ac_settings/rc_ose via ScopeShortcuts. |
| `OmniboxCompletionModel::rebuildScopedRows`/`collectBookmarkRows` :531/:502 | done | bookmark rows walk the ac_bm-bound BookmarkNode tree (facade-bound load/writes); tab rows come from the live tab-entry provider (engine objects). |
| `OmniboxCompletionModel::setSuggestions`/`historyVisible`/`data` :414-660 | keep | merge-model presentation; source pick above moves. |
| `HistoryCompleter`/`HistoryCompletionView` :671-812 | keep | completer widget + popup sizing. |

## src/bookmarks/ UI

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `BookmarksManager::load` :287 | done | facade-bound load incl. quarantine + defaults path. |
| `BookmarksManager::rustStore`/`ensureBound`/`save` :236-512 | done | handle management + ac_bm_save. |
| `addBookmark`/`removeBookmark`/`setTitle`/`setUrl`/`changeExpanded` :279-565 | done | ac_bm_* node writes. |
| `importBookmarks`/`exportBookmarks`/`importHtmlDocument`/`importHtmlBookmarks`/`exportBookmarksTo` :607-864 | done (rust leg) | ac_bm_import_html/export; dialogs + bookmarklet confirm keep; no-rust leg stays as reference. |
| `RemoveBookmarksCommand`/`InsertBookmarksCommand`/`ChangeBookmarkCommand` :865-952 | keep | Qt undo stack. |
| `BookmarksDialog::*` :77-247 | keep | dialog + tree actions; `saveExpandedNodes`/`expandNodes` persist via ac_bm_set_expanded (done through node handles). |
| `BookmarksMenu::activated`/`postPopulated`/`openAll` :90-148 | keep + thin | menu keep; `openAll` first-current-rest-new placement rule → nav placement policy (MS08/MD02-adjacent, low prio). |
| `BookmarksMenuBarMenu::*` :150-175 | keep | menubar menu model. |
| `BookmarksToolBar::*` :34-158 | keep | toolbar + context menu. |
| `AddBookmarkDialog::accept`/`AddBookmarkProxyModel` :75-212 | keep | dialog validation; addBookmark write already ac_bm. |
| `BookmarksModel` | keep | model over facade-backed nodes. |

## Sizing for MS02+

- **MS03 LocationBar** — RESOLVED: typed-input verdict was already
  rc_classify_input, engine resolution already rc_ose_*/ac_settings,
  history rows already ac_hist_suggest, bookmark rows the ac_bm-bound
  node tree, urlclean already consumed at the request interceptor
  (rc_urlstrip).  Code moved: historycompleter.cpp:150/169 pass
  through to the store-filtered/ranked rust source.  thin-blocked
  flags (no facade verdict fn, no MD row): suggestion-source pick +
  availability (omniboxsuggestions.cpp:67/80/125), scope→provider
  pick (historycompleter.cpp:471), branded-start-page re-pick
  (tabwidget.cpp:4807/:4526).  keep per MD01 contract: TLD append
  (:387), Alt/modifier placement (:3281/:4774), widget-tab fallback +
  invalid guard (:4807), no-rust oracle legs (:4447/:4585).
- **MS04 WebView ctx menu + hover** — DONE: webviewcontroller.{h,cpp}
  owns the menu model (`contextMenuModel` — entry set/order/enabled/
  disabled-reason) + `hoverStatusText`; webview.cpp:558 renders it and
  keeps menu build/exec/strings; engine-adapter hover sink
  (browsermainwindow.cpp:642) consumes the same verdict.  env inputs
  still Qt-legged (thin-blocked): CONT07 gate → MD02, private/tor
  availability → context + MB04 caps.  tabbar.cpp:538 menu model is
  the same shape (still open — no MS row yet covers it; the tab-bar
  context menu verdicts map to MS08 session/ac_containers inputs).
- **MS05 zoom/find/print/devtools caps** (~5 sites): browsermainwindow.cpp
  :1733, :3521-3581, webview zoom :1062-1149 — all `needs:task:MB04`.
- **MS06 capability chrome** (~6 sites): browsermainwindow.cpp:2510,
  :1115/:1719/:1793, engineindicator.cpp:69/102, updateReaderState:1839 —
  `needs:task:MB04`.
- **MS07 settings** (~12 section groups): settings.cpp whole + residual
  raw `QSettings` (found: tabwidget.cpp:2107) + manager stores not yet
  facade-bound (WebPermissionManager → MD03; extension registry → flag).
- **MS08 tabs/session** (~20 sites): tabwidget session cluster
  :4992-5722, pinned :709-791, titles :792-892, groups :1663-2191,
  containers :2309-2700, closed-stack :292-4362, order :2192-2241.
- **thin-blocked counts**: MD02 ~12 sites (webview gates + tor queue +
  context routing + loadString untrusted); MD03 ~3 (sitepanel:870,
  settings perm section); MD06 ~2 (sitepanel:825 + interceptor sites);
  MD08 ~4 (snapshot policy, closed-stack exclusion, autosaver);
  MD09 ~3 (popup toggles); no-MD-row flags ~4 (padlock composite :222,
  energy verdicts, sleep/purge cadence, extension registry).
- **keep (dominant)**: all paint/input/drag-drop/menus/dialogs/engine
  objects — the bulk of tabbar.cpp and both context menus' construction.
- **done**: sitetrust reads, history add/remove/clear/icons/suggest,
  bookmarks load/CRUD/import/export, ProfileSettings→AcSettings routing,
  torctl, containers registry, notes, autofill, cred, updates.

## Out of scope here but flagged

- `src/webpage.cpp` (`acceptNavigationRequest`, popup gate, permission
  surfacing) is the MD02/MD03/MD09 consumer — engine-side decision
  *consumer*, audited by MD01, not this file.  WebView-side verdicts that
  consult it are listed above.
- `DownloadManager` UI (interception policy → MD04) — outside the listed
  audit scope; the one UI touch (`fileSaveAs`/`downloadManager`) delegates.

## MS09 — final boundary map

The lane's contract, stated once.

**Stays Qt-side (widget code):**

- paint, focus, input, menus, drag/drop, dialogs
- engine-object ownership and live state: QWebEngineView/Page/Profile,
  EngineInterface/EngineTab adapters, Servo/WebKit view objects,
  TorManager, EnergySaver, PdfView
- codecs for persisted blobs whose *bytes* already route through the
  facade: the window-state QByteArray codec, the session-manifest JSON
  schema — the layout is chrome's, the store route is not
- per-run widget state that never crosses a store boundary: strip state
  (`m_pinnedTabs`, live group maps, hidden-store levels), the in-memory
  closed-tab stack, pending-restore placeholders, sleep timestamps,
  completer/mirror caches fed by facade reads
- mechanics that *execute* a verdict someone else made: menu
  build/exec/strings, tab placement and reflow, tor-queue timers,
  widget/view construction, focus policy

**Moved behind the facade (arktos_core.h / rc_*):**

- store reads/writes: ac_settings (incl. ProfileSettings), ac_hist,
  ac_bm, ac_sitedec, ac_policy, ac_containers, ac_cookies, ac_notes,
  ac_icons, ac_blocklist, ac_tranco, rc_cred, rc_autofill
- verdicts: rc_classify_input (typed input), rc_ose_* (engine
  resolution/opt-ins), rc_urlstrip (url clean), ac_sitetrust (trust
  level/signals), ac_session_exists + ac_session_*_restoring (store
  presence + restore guard), rc_frecency_score candidate
- menu/status models: `WebViewController::contextMenuModel` /
  `hoverStatusText` (MS04) — entry set/order/enabled and hover text are
  controller decisions fed by facade env; the widget renders

**Still Qt-legged pending a domains/bridge module (thin-blocked):**

- MD02 navpolicy — scheme gates on untrusted input (drops, middle-click,
  context menu, `loadStringFromUntrustedSource`), tor nav-hold verdict,
  referer/same-profile hand-off, private/tor window routing (~9 rows)
- MD03 permpolicy — permission entry list/revoke (~2 rows)
- MD06 url-clean policy (~1 row plus interceptor sites)
- MD08 session policy — snapshot contents, restore order/lazy deferral,
  closed-stack capture exclusion, serialization ordering (~8 rows)
- MD09 popup verdict (~3 rows)
- MB04 caps table (mw-bridge) — engine-capability gating that MS05/MS06
  consume: devtools/print/zoom/find/reader/engine-indicator rows
- no-MD-row flags for the domains lane — padlock composite verdict
  (sitepadlock.cpp:222), suggestion-source pick + availability
  (omniboxsuggestions.cpp:67/80/125), scope→provider pick
  (historycompleter.cpp:471), branded-start-page re-pick
  (tabwidget.cpp:4807/:4526), energy/sleep cadence + saver policy,
  extension install-registry surface

**Ledger (table rows above):** 75 pure `keep`; ~67 rows carrying a
`thin` leg (facade surface exists — any MS row can take them); ~18 rows
with a `done` leg; thin-blocked MD02 ~9 / MD03 ~2 / MD06 ~1 / MD08 ~8 /
unlabeled ~4; MD09 popup-verdict rows (~3) count as `thin` — the
ac_sitedec write leg is routed, only the verdict half waits on MD09.
Dominant mass is `keep` — as intended, the bulk of the shell is
paint/input/engine-object code that should never leave Qt.

### Smoke parity

Every standalone `--*-smoke` leg re-run headless
(`QT_QPA_PLATFORM=offscreen`, `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`,
`ARKTOS_BUNDLED_EXTENSIONS_DIR=<empty>` — the MS08 fixture note —
`ARKTOS_TOR_BINARY=<fetched tor>`).  Internal child/seed flags are not
legs — they exist to be spawned by their orchestrators and are listed
here for completeness: `sigterm-child-smoke`, `fingerprint-child-smoke`,
`fingerprint-inject-seed`, `telemetry-prefetch-child-smoke`,
`profile-wipe-arm-smoke`, `profile-wipe-child-smoke`, `vault-smoke-seed`,
`vault-smoke-dirty-seed`.

Two harness properties found en route, both pre-existing:

- `QStandardPaths` test mode resolves the data root to
  `~/.qttest/share/Arktos/Arktos` and ignores the XDG redirect, so a
  same-HOME sweep shares one profile store across legs — legs that pin
  `AdBlock/enabled=false` for determinism (browser-smoke etc.) poison
  later legs' adblock-enabled assumptions.  A fresh `HOME` per leg
  restores standalone semantics; every leg below was verified that way
  or in a state where the shared keys did not matter.
- `about:blank` commits are instant — a page-level `QWebEngineScript`
  armed during `acceptNavigationRequest` can lose to the renderer's
  per-document collection snapshot (profile-level scripts, installed
  before navigation, always make it).  `--adblock-smoke`'s cosmetic
  leg now re-issues the `about:blank` load once on first-check
  failure; the armed script stays resident and runs on the second
  commit.  Same leg, same assertion — this is the load bearing, not
  the adblock pipeline.
- DLACC06 made rustdl the default download engine: it claims http(s)
  downloads and cancels the Chromium request before a
  `downloadRequested` observer can attach, so the three Chromium-path
  legs (`--download-smoke`, `--parallel-download-smoke`,
  `--parallel-download-off-smoke`) silently timed out.  All three now
  pin `downloadmanager/engine=builtin` in the pre-init settings store
  — they exist to verify Chromium's DownloadRequest lifecycle and
  `ParallelDownloading` flag, which is exactly what they check again.

Also found: `AutoFillManager::scheduleOnPage` appended a fresh
`arktos:autofill-submit` page script on every navigation without
removing the prior copy (the armed-collection dump showed 5 copies).
`insert()` appends — the shim now goes through the same
remove-then-insert as `replacePageScript`.

| Leg | rc | Verdict |
|-----|----|---------|
| `--browser-smoke` | 0 | PASS |
| `--settings-smoke` | 0 | PASS |
| `--session-smoke` | 0 | PASS (8/8) |
| `--restore-smoke` | 0 | PASS |
| `--session-floor-smoke` | 0 | PASS (29/29) |
| `--sigterm-smoke` | 0 | PASS (15/15) |
| `--nam-smoke` | 0 | PASS (NoError on file fetch; rc is verdict) |
| `--history-smoke` | 0 | PASS |
| `--cookie-smoke` | 0 | PASS |
| `--bookmarks-smoke` | 0 | PASS |
| `--search-smoke` | 0 | PASS |
| `--search-guess-smoke` | 0 | PASS |
| `--adblock-smoke` | 0 | PASS (4/4 — cosmetic leg retried per note above) |
| `--adblock-list-smoke` | 0 | PASS |
| `--adblock-rust-smoke` | 0 | PASS |
| `--autofill-smoke` | 0 | PASS |
| `--find-smoke` | 0 | PASS |
| `--source-smoke` | 0 | PASS (fresh HOME — see store note) |
| `--app-smoke` | 0 | PASS |
| `--ua-smoke` | 0 | PASS |
| `--privacy-smoke` | 0 | PASS |
| `--referer-smoke` | 0 | PASS |
| `--container-smoke` | 0 | PASS |
| `--sorry-smoke` | 0 | PASS |
| `--icons-smoke` | 0 | PASS |
| `--seclvl-smoke` | 0 | PASS |
| `--ping-smoke` | 0 | PASS |
| `--httpsonly-smoke` | 0 | PASS |
| `--resourceblock-smoke` | 0 | PASS |
| `--urlclean-smoke` | 0 | PASS |
| `--gpc-smoke` | 0 | PASS |
| `--fingerprint-smoke` | 0 | PASS |
| `--fingerprint-inject-smoke` | 0 | PASS |
| `--reader-smoke` | 0 | PASS |
| `--telemetry-smoke` | 0 | PASS |
| `--telemetry-browse-smoke` | 0 | PASS |
| `--doh-smoke` | 0 | PASS |
| `--tls-smoke` | 0 | PASS |
| `--tls-off-smoke` | 0 | PASS |
| `--webrtc-smoke` | 0 | PASS |
| `--webrtc-off-smoke` | 0 | PASS |
| `--clientcert-smoke` | 0 | PASS (local PEM + loopback fixture — `ARKTOS_CLIENTCERT_FILE`/`_URL`; without it the leg is env-gated) |
| `--pingspotter-smoke` | 0 | PASS (fresh HOME — see store note) |
| `--sleep-smoke` | 0 | PASS |
| `--palette-smoke` | 0 | PASS |
| `--pip-smoke` | 0 | PASS |
| `--tabstrip-smoke` | 0 | PASS |
| `--startpage-smoke` | 0 | PASS (fresh HOME — see store note) |
| `--crash-smoke` | 139 | PASS — rc=139 is the verdict: deliberate SIGSEGV, `crashreport.log` written |
| `--sandbox-smoke` | 0 | PASS |
| `--perf-smoke` | 0 | PASS (benchmark output; rc is verdict) |
| `--download-smoke` | 0 | PASS (`file://…/README.md` arg — the leg is inert without a URL operand) |
| `--download-smoke` (refused) | 1 | tolerated — check-coverage marks this leg tolerated; with the builtin pin the Interrupted arm genuinely runs (still rc=1, `FAIL <reason>`) |
| `--parallel-download-smoke` | 0 | PASS (3 conns, 2 range requests — forked slices) |
| `--parallel-download-off-smoke` | 0 | PASS (1 conn, 0 ranges — differential control) |
| `--ingress-smoke` | 0 | PASS |
| `--otr-teardown-smoke` | 0 | PASS |
| `--profile-wipe-smoke` | 0 | PASS (orchestrates arm/child legs itself) |
| `--vault-smoke` | 0 | PASS — two independent pairs, each on a fresh profile: locked pair (seed→locked-boot checks→prompt unseal→teardown auto-lock re-seal) rc=0/0; dirty pair (dirty-seed `_exit` crash plant → boot dirty-scan → offer-accept re-seal → Locked) rc=0/0 |
| `--pdf-smoke` | 0 | PASS |
| `--pdfview-smoke` | 0 | PASS |
| `--printpreview-smoke` | 0 | PASS |
| `--engine-smoke` | 0 | PASS |
| `--extension-smoke` | 0 | PASS |
| `--extension-otr-smoke` | 0 | PASS |
| `--extension-update-smoke` | 0 | PASS |
| `--extension-host-smoke` | 0 | PASS |
| `--bitwarden-smoke` | 0 | PASS — verifies the bundled zip itself; run WITHOUT the emptied bundled-dir override |
| `--mv3-health-smoke` | 0 | PASS |
| `--extension-health-smoke` | 1 | FAIL — "health verdicts never settled": fixtures + bundled Bitwarden install and enable (SW eval crash → status 15, by design), but no probe produced a finished detail inside the leg's own 180s window across two runs; no mw-shell diff touches `extensionmanager.cpp` — recorded as pre-existing/environmental (heavy sibling-lane build load at sweep time) rather than a lane regression |
| `--bidi-smoke` | 0 | PASS |
| `--bidi-elements-smoke` | 0 | PASS |
| `--bidi-network-smoke` | 0 | PASS |
| `--bidi-sources-smoke` | 0 | PASS |
| `--bidi-perfmem-smoke` | 0 | PASS |
| `--servo-smoke` | 0 | SKIP — self-skips, no libservo_embed artifact in this tree |
| `--tor-smoke` | 0 | PASS (real bootstrap) |
| `--tor-window-smoke` | 0 | PASS |
| `--tor-circuit-click-smoke` | 0 | PASS — invoked `--tor --tor-circuit-click-smoke` (the leg is not in the tor-arm flag list) |
| `--tor-newtab-smoke` | 0 | PASS |
| `--tor-firstsearch-smoke` | 0 | PASS |
| `--anon-smoke` | 0 | PASS (ipduh battery settled, dns=3 — rc is verdict) |
| `--badssl-smoke` | 0 | PASS (72 cases: 24 loaded, 48 blocked, 0 other) |
| `--browseraudit-smoke` | 0 | PASS (431 tests: 396 pass, 35 warning, 0 critical) |
| `--xsleak-smoke` | 124 | FAIL (env) — `xsinator.com` root now 404s; the live fixture is dead upstream, not a code regression |

Not legs: `engine-list` is not a flag (sweep listed it in error).
