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
| `keyPressEvent` :387 (Ctrl/Shift+Enter → .com/.net/.org append) | thin | MS03 — typed-input→URL policy; belongs to the classify-input verdict surface (rc_classify_input exists, extend or keep as reference). |
| `dragEnterEvent`/`dropEvent` :417/:432 | keep + thin-blocked:MD02 | drop plumbing stays; the `isUrlAllowedOnUntrustedInput` scheme verdict → navpolicy. |
| `registrableDomainRange` :144 | keep | pure-URL paint helper (SAFE03 domain emphasis); optional facade url-util later, no MD row. |
| `unicodeUrlHint` :247 | keep | pure-URL tooltip helper. |
| `webViewUrlChanged`/`displayUrl`/focus handlers :131-377 | keep | display state + focus policy. |

## src/locationbar/omniboxsuggestions.cpp

| Site | Verdict | Notes / Needs |
|------|---------|---------------|
| `enabled()` :67 (tor gate + `suggestionsInAddressField`) | thin | MS03 — suggestion-availability verdict via facade (mode + ac_settings). |
| `currentEngineChanged` :80 / `requestSuggestions` :125 | thin | MS03 — engine pick (private-context engine, keyword engine, `suggestionsOnlyWithKeyword`, per-engine opt-in) = suggestion-source selection. |
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
| `contextMenuEvent` :305 | thin | MS04 — menu *model*: scheme gate per entry (MD02), private-entry hide (context via facade), tor-binary availability (caps), image-search presence (opensearch), canvas/media payload shape (keep).  Widget still builds/execs the QMenu. |
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
| `init` linkHovered→setStatusBarText :164 | keep + thin | pass-through today; MS04 hover-status *policy* (what text to show) may consult facade later. |
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
| `setupMenu` :1064 | keep + thin | construction keeps; embedded decisions thin: tor-binary gate :1101 (caps), container-menu hide :1115 (mode/caps), `canRestoreSession` :1527 (ac_session_exists/last_exit), lock-store enable :1667 (rc_cred), BiDi tor gate :1719 (caps), updates tor gate :1793 (caps/mode). |
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
| `clearPrivateData` :3798 | keep + thin | dialog keep; undo-stack wipe → session store (MS08). |
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
| `lineEditReturnPressed` :3281 | thin | MS03 — Alt→NewTab placement + typed-text accept chain entry. |
| `loadString` :4411 / `loadStringFromUntrustedSource` :4425 | thin / thin-blocked:MD02 | private-context pick + classify call → MS03; untrusted-source scheme gate → navpolicy. |
| `looksLikeAddress` :4447 | thin | superseded by rc_classify_input on rust builds; remains the no-rust reference leg. |
| `searchInAddressFieldEnabled` :4473 | thin | ac_settings + legacy-key migration (move migration into rust settings). |
| `guessUrlFromStringRust` :4488 | thin | MS03 — verdict consumption done; engine resolution still Qt (→ opensearch registry handles). |
| `guessUrlFromStringCpp` :4585 | thin | no-rust reference leg — MS03 keeps it as parity oracle only. |
| `modifyWithUserBehavior` :4774 | thin | modifier→placement policy + `selectNewTabs` → ac_settings (MS03; nav-placement is MD02-adjacent). |
| `loadUrl` :4807 | thin | decisions: branded-start-page re-pick, widget-tab→new-tab fallback, invalid refusal → facade; placement mechanics keep. |
| `getView` :4868 | keep + thin | construction keep; per-target profile pick (private/container) → ac_profiles/ac_containers. |
| `newTabPage`/`loadNewTabPageUrl`/`newPrivateTab`/`makeNewTab`/`makeNewTabLike`/`makeNewPrivateTab`/`makeNewTabInContainer`/`makeNewTabOnProfile` :582-1141 | thin | home-url pick (ac_settings), private/container profile routing (ac_profiles/ac_containers); construction keep. |
| `loadSettings` :4667 | thin | tab-strip prefs block → ac_settings. |
| `addEngineTab`/`reloadTabInEngine`/`tabEngineId`/`engineTab` :1180-1566 | thin | MS06 — swap-availability verdict (tor/private/container-strip guards + backend resolve) → caps/context; engine-object work keeps. |
| `reopenTabInContainer`/`loadUrlInContainer`/`manageContainers`/`manageContainersInteractive` :1567-1663 | thin | container moves + site-rule enforcement → ac_containers (+ MD02 for nav-time rule verdict). |
| tab groups :1663-2191 (`assignTabGroup`/`createTabGroup`/`moveTabIntoGroupRun`/`addTabToGroup`/`removeTabFromGroup`/`groupTabWith`/`renameTabGroup`/`setTabGroupColor`/`inheritTabGroup`/`forgetTabGroupIfEmpty`/`detachGroupMember`/`expandTabGroup`/`setTabGroupCollapsed`/`ungroupTabs`/`closeTabGroup`/`normalizeTabGroupMove`) | thin | MS08 — group membership/name/color/collapsed = per-tab session fields via AcSession; strip reflow keep.  BUG found: `closeTabGroup` reads raw `QSettings` :2107 (bypasses ProfileSettings) → MS07 fix list. |
| container strip :2309-2700 (`twoLevelStrip`/`activeContainerHeader`/`containerTabCount`/`containerHeaders`/`containerStripActive`/`applyContainerFilter`/`detachTabIntoContainerStore`/`restoreHiddenContainerTabs`/`setActiveContainerHeader`/`moveContainerHeader`/`syncContainerStrip`/`closeHiddenTab`/`closeHiddenPage`/`closeTabsInContainer`/`setTwoLevelStrip`) | thin + keep | active level + hidden stores → session/containers state; strip rebuild + painting keep. |
| `markTabActivity` :2714 | thin | activity stamp → session state (MS08). |
| `suspendIdleIntervalMs`/`applyEnergyCaps` :2724/:2731 | thin | saver-override + interval policy → ac_settings/energy verdict (no MD row assigned — flag for domains lane). |
| `updateSleepTimer`/`isTabSleeping`/`sleepingTabCount` :2749-2778 | keep | timers + view-state reads. |
| `sleepBlockReason` :2780 | keep | verdict over live engine state (loading/audible/download/form-dirty/pinned) — inputs are engine objects; revisit if a facade lifecycle-verdict lands (flag). |
| `suspendIdleTabs`/`sleepTab`/`beginTabSleep`/`finishTabSleepCapture`/`wakeTab`/`purgeInactiveTabs`/`updatePurgeTimer`/`freeTabMemory`/`freeBackgroundTabsMemory`/`flushCurrentTabMemory`/`reportFreedMemory`/`applySleepVisuals` :2812-3163 | keep + thin | engine lifecycle calls + capture keep; sweep cadence/eligibility thresholds → settings/policy (thin, no MD row — flag). |
| `captureTabThumbnail`/`scheduleThumbnailCapture`/`tabThumbnail`/`animationLabel` :3164-4072 | keep | thumbnails/spinners — paint. |
| `webViewLoadStarted`/`webViewLoadProgress`/`webViewLoadFinished`/`webViewIconChanged`/`webViewTitleChanged`/`webViewUrlChanged`/`currentChanged` :379-4233 | keep | engine→chrome forwarders. |
| `pushRecentlyClosedTab`/`restoreClosedTab`/`openLastTab`/`aboutToShowRecentTabsMenu`/`aboutToShowRecentTriggeredAction`/`hasRecentlyClosedTabs`/`clearRecentlyClosedTabs`/`historyCleared` :292-4362 | thin | MS08 — closed-tab stack → AcSession (private-exclusion is a policy decision → MD08-adjacent). |
| `isTabPendingRestore`/`pendingRestoreUrl`/`restorePendingTab`/`dressHiddenPendingTab` :4992-5074 | thin | MS08 — lazy-restore decisions via AcSession (dress/placeholder paint keep). |
| `collectSessionSnapshot` :5075 | thin | MS08 — what enters the snapshot (OTR-drop, pending substitution, scroll pick, group splice) → facade; dirty/interval policy → MD08. |
| `saveState`/`restoreState` :5191/:5230 | thin | MS08 — store via AcSession; blob codec can stay Qt initially. |
| `restoreSessionSnapshot` :5300 | thin | MS08 — restore order/policy (placeholder reuse, pinned-first ordering, container levels). |
| `sessionStateJson`/`restoreSessionState` :5579/:5638 | thin | MS08 — already the ARSS-facing JSON leg; route through ac_session_save/load fully. |
| `createTab` :5698 | keep | restore-time construction. |
| `isTabPinned`/`setTabPinned`/`normalizePinnedBlock`/`pinnedTabCount` :709-791 | thin | MS08 — pinned flag → session field; strip normalization keep. |
| `tabTitleOverride`/`tabDisplayTitle`/`setTabTitleOverride`/`setCustomTitleForPage`/`applyTabTitle` :792-892 | thin | MS08 — rename override → session field; display keep. |
| `closeTab`/`closeOtherTabs`/`cloneTab`/`windowCloseRequested` :3303-3513 | keep + thin | strip mechanics keep; private-no-reopen exclusion → session policy (thin). |
| `moveTabTo`/`transferTabTo`/`carryHiddenMemberTo`/`carryKeyedState`/`dropKeyedState`/`severTabBindings`/`adoptTransferredTab`/`tearTabToNewWindow` :3514-4024 | keep + thin | cross-window move mechanics keep; keyed-state carry → session fields (thin, MS08). |
| `orderedWebViews`/`orderedSessionPages` :2192/:2241 | thin | MS08 — session ordering rule → facade. |
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
| `HistoryCompletionModel::filterAcceptsRow`/`lessThan` :150/:169 | thin | MS03 — match + word-boundary rank via facade (RustHistorySuggestModel supersedes under rust; this is the fallback leg). |
| `RustHistorySuggestModel::*` :194-319 | done | ac_hist_suggest. |
| `OmniboxCompletionModel::setSearchText` :471 | thin | MS03 — scope-parse → which source rows. |
| `OmniboxCompletionModel::rebuildScopedRows`/`collectBookmarkRows` :531/:502 | thin | MS03 — bookmarks rows via ac_bm; tab rows via session/tab provider. |
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

- **MS03 LocationBar** (~10 sites): locationbar.cpp:387,
  omniboxsuggestions.cpp:67/80/111/125,
  tabwidget.cpp:3281/4411/4447/4473/4488/4585/4774/4807,
  historycompleter.cpp:150/169/471/502/531.  `rc_classify_input` already
  lands the verdict; remaining work = engine-table resolution + settings
  reads via facade + the no-rust leg as parity oracle.
- **MS04 WebView ctx menu + hover** (~8 sites): webview.cpp:305-616
  (entry set + per-entry enabled), hover-status policy (init:164);
  tabbar.cpp:538 menu model same shape.
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
