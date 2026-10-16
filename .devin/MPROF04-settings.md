# MPROF04 — QSettings keyspace audit (global vs per-profile)

Audit of every `QSettings` read/write site in `src/` for the multi-profile
settings split. The table below is the classification `ProfileSettings::
isProfileKey()` implements — keep the two in sync.

## Storage model

- PER-PROFILE keys live in `profiles/<id>/settings.ini` (IniFormat file under
  `QStandardPaths::AppDataLocation/profiles/<id>/`, resolved by
  `ProfilePaths::dirFor()` — the MPROF04 standalone copy of MPROF01's layout
  contract; whoever merges second deletes the duplicate helper).
- GLOBAL keys stay on the plain `QSettings` (org `Arora`, app `Arora`) file.
- `ProfileSettings` is a QSettings-compatible facade owning one IniFormat
  object per store. It routes each key by full path: classified-profile keys
  read profile-store-first and fall back to the global file (the "old keys
  stay as harmless fallback" half of the copy-not-move migration), writes go
  to the profile store; global-classified keys always hit the global file.
  Each facade owns private `QSettings` objects rather than sharing a qApp map
  entry — `beginGroup()` mutates the object's group stack, so a shared object
  would leak group state between call sites that forget `endGroup()`.
- `ProfileSettings::currentId()` resolves the *calling* profile — `"default"`
  until MPROF03 lands window->profile binding. The SettingsDialog binds
  `currentId()` at construction; MPROF03 retargets the accessor (or swaps
  call sites to an explicit window id) without touching this file.
- Migration: `ProfileSettings::migrateFromGlobal()` runs once in the
  BrowserApplication ctor — copy-not-move of every classified key present in
  the global file into `profiles/<id>/settings.ini`, marked by
  `meta/settingsMigrated=1` inside the profile ini.

## Classification

### PER-PROFILE top-level groups (every key beneath)

| Group | Contents (representative keys) | Rationale |
|-------|--------------------------------|-----------|
| `AdBlock` | `enabled`, `subscriptions`, `engine`, `remoteListsConsent` | per-profile adblock config (MPROF02 scopes AdBlockManager per profile) |
| `BrowserMainWindow` | `defaultState` | window chrome state is persona layout |
| `LanguageManager` | `language` | UI pref a user may want per persona |
| `MainWindow` | `home`, `homePageMode`, `startupMode`, `startupPages`, `startupBehavior` (legacy), `home`, `showSearchBox`, `showSidebar`, `sidebarDockArea`, `sidebarWidth`, `iconTheme`, `windows`, `shell` (legacy session blob) | homepage/startup/UI prefs — the spec's headline persona keys |
| `Settings` | `currentTab` | settings-page UI state |
| `SourceViewer` | `size` | dialog UI state |
| `autofill` | `passwordForms`, `ignoreAutoCompleteOff` | persona autofill prefs |
| `browser` | `colorScheme` | theme is persona chrome (see exceptions below) |
| `clearprivatedata` | `browsingHistory`, `downloadHistory`, `searchHistory`, `autofill`, `cookies`, `siteData`, `cache`, `favIcons` | persona hygiene prefs |
| `commandPalette` | `mru` | persona usage state |
| `containers` | registry `order`, `<id>/name|color|sites` | containers are persona (MPROF02) |
| `cookies` | `blockCookies`, `filterTrackingCookies`, `blockThirdPartyCookies`, `keepCookiesUntil`, `acceptCookies`, `allow`, `block`, `allowForSession`, `exceptions/*` | cookie policy — spec call-out |
| `downloadmanager` | `alwaysPromptForFileName`, `downloadDirectory`, `external`, `externalPath`, `engine`, `connections`, `parallelSegments`, `removeDownloadsPolicy`, `url/location/done/started/finished/size` (history) | downloads dir + engine — spec call-out; download history is persona data |
| `extensions` | `autoUpdateCheck`, `lastUpdateCheck`, `bundled/<storage>`, `integrity/<storage>/<id>` | extension set is persona (MPROF02) |
| `history` | `historyLimit` | persona retention pref |
| `navbar` | `order` | toolbar layout — persona |
| `network` | `cacheEnabled`, `maximumCacheSize`, `acceptLanguages` | network/storage prefs — persona (see exceptions) |
| `openSearch` | `engine`, `privateEngine`, `imageEngine`, `fieldEngine`, `keepFieldEngine`, `suggest*`, `keywords/*`, `engineOrder`, `removedBundledEngines`, `suggestions` | search-engine choice — spec call-out |
| `performance` | `hardwareAcceleration`, `lazyRestoreTabs`, `alwaysLoadPinnedTabs`, `energySaver` | persona perf prefs |
| `popupExceptions` | `allowed` | per-site popup grants — persona |
| `privacy` | `securityLevel`, `httpsFirst`, `httpsOnly`, `httpsOnlyExceptions`, `refererPolicy`, `trimReferer`, `webrtcIpProtection`, `secureDns*`, `tlsStrictCiphers`, `blockPings`, `blockPrefetch`, `dnsPrefetch`, `blockRemoteFonts`, `blockThirdPartyWebSockets`, `pdfViewer`, `pdfSanitize`, `pdfShellView`, `clearOnExit`, `reportUtcTimezone`, `normalizeAcceptLanguage`, `fingerprintProtection`, `fingerprintExceptions`, `fingerprintMigrationV1`, `stripTrackingParams`, `domainBlocklist`, `doNotTrack`, `siteData`… | privacy toggles — spec call-out (see exceptions) |
| `profile` | `name` | profile identity itself |
| `proxy` | `enabled`, `type`, `hostName`, `port`, `userName`, `password` | per-persona routing (work proxy vs direct) |
| `reader` | `fontSize`, `theme` | reader-mode persona prefs |
| `scriptcontrol` | per-host rules | persona script policy |
| `searchShortcuts` | per-scope token/enabled | scoped-completion nicknames — persona |
| `sessions` | `lastSession` (legacy blob) | session data is persona |
| `sidebar` | `currentTab`, `panelOrder`, `panels/<id>`, `notes`, `historyPeriod`, `showPanelToggle`, `showCloseButton`, `autoCloseInactive`, `folderCounters`, `lazyLoad` | sidebar prefs/state — persona |
| `tabs` | `tabBarPosition`, `selectNewTabs`, `confirmClosingMultipleTabs`, `confirmExit`, `oneCloseButton`, `quitAsLastTabClosed`, `openTargetBlankLinksIn`, `openLinksFromAppsIn`, `containerDisplay`, `tabHoverPreviews`, `tabMinWidth`, `tabMaxWidth`, `verticalTabWidth`, `horizontalTabHeight`, `iconOnlyTabs`, `suspendTabs`, `suspendTabsMinutes`, `newTabButtonInRightCorner`, `newTab` | tab prefs — persona |
| `toolbarsearch` | `recentSearches`, `maximumSaved`, `alwaysNewTab`, `keepTypedText`, `showEngineNickname`, `showEngineIcon` | search field prefs + recent list — persona |
| `trust` | `enabled`, `lookalike`, `threatlist` | feature toggles are persona prefs — the site *verdict* store (SiteDecisionStore JSON, `trust` list downloads) stays machine-global; MPROF03 documents the same split |
| `urlcleaner` | `extraParams` | user's extra strip params — persona |
| `urlloading` | `searchInAddressField`, `searchEngineFallback` (retired), `selectionSearchInBackground` | omnibox behavior — persona |
| `webpermissions` | `<origin>/grant` | per-site permission grants — persona (Chrome/Firefox scope these per profile) |
| `websettings` | `standardFont`, `fixedFont`, `minimumFontSize`, `blockPopupWindows`, `enableJavascript`, `enablePlugins`, `enableImages`, `enableLocalStorage`, `forceDarkMode`, `middleClickAutoscroll`, `userStyleSheet` | page-appearance/web prefs — persona |

### PER-PROFILE top-level keys (no group)

| Key | Contents | Rationale |
|-----|----------|-----------|
| `userAgent` | UA01 override string | persona UA override |

### GLOBAL — exceptions inside profile-owned groups

| Key | Contents | Rationale |
|-----|----------|-----------|
| `MainWindow/defaultEngine` | render-backend pick | spec: "engine defaults" are app-level |
| `MainWindow/restoring` | session-restore crash sentinel | crash-guard state — app-level machinery |
| `browser/gpuCrashCount` | GPU safe-mode counter | crash state — app-level |
| `browser/lastExit` | clean/dirty exit stamp | crash state — app-level |
| `browser/sessionActive` | unclean-exit sentinel | crash-guard state — app-level |
| `browser/sessionActiveTor` | unclean-exit sentinel (tor) | crash-guard state — app-level |
| `network/CaCertificates` | user-accepted CA/exception certs | trust decision — shared trust store (MPROF03 recommends global verdicts) |
| `privacy/trancoListUpdated` | last-fetch stamp | metadata for the machine-global Tranco list |
| `privacy/domainBlocklistUpdated` | last-fetch stamp | metadata for the machine-global blocklist |
| `meta/settingsMigrated` | migration marker (profile ini only) | written directly to the profile ini by migrateFromGlobal, never routed |

### GLOBAL — groups that never moved

| Group | Contents | Rationale |
|-------|----------|-----------|
| `sandbox` | `enabled` | app-level sandbox posture |
| `crashreporter`/consent keys | telemetry/crash consent | spec: "crash-consent" is app-level |
| tor prefs | tor binary/bootstrap prefs | spec: "tor binary prefs" are app-level (TorManager keeps runtime state in files, not QSettings) |
| profile registry | `profiles.json` at data root | spec: the registry itself is global (MPROF01 owns it) |
| SiteDecisionStore | per-site trust verdicts (JSON file, not QSettings) | shared trust store per MPROF03 recommendation |

## Call-site treatment

Every site that touches ≥1 classified key constructs `ProfileSettings`
instead of `QSettings`; routing happens inside the object, so mixed sites
(the SettingsDialog writes both `MainWindow/defaultEngine` and
`MainWindow/home` in one function) need no split. Sites touching only
global keys keep plain `QSettings` (`engineregistry.cpp`,
`sandboxmanager.cpp`, `trancolist.cpp`, `domainblocklist.cpp`).

Read fallback is load-bearing for compatibility: pre-migration installs and
the ~200 autotest/smoke legs that seed the global file keep working
(profile-store miss -> global hit). Writes never touch the legacy file for
classified keys, so a second profile's toggles cannot leak into another
profile's ini — or into the shared global file.
