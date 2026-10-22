# MW-DOMAINS audit — Qt-side decision logic inventory (MD01)

Contract for MD02-MD09. For each decision site: `file:line`, what it
decides, and the verdict:

- **move** — pure data/policy: the decision computes over url/host/
  scheme/settings/rules with no JS eval, widget, or engine surface.
  Belongs in rustcore behind an `rc_<dom>_*` FFI verdict; Qt keeps the
  execution.
- **keep** — needs a widget/dialog, page lifetime, JS eval, engine
  callback, or live-object graph. Qt owns it; a rustcore module may
  still supply the *inputs* (verdict/store) it consumes.
- **done** — already delegated to `Engine::NavigationPolicy` /
  `SiteDecisionStore` / another rustcore module (ENG02/SITED01/OMNI01/
  RC13/UAG01); listed so MD02-MD09 do not re-scope them.

## WebPage — src/webpage.cpp

| Site | Line | Decides | Verdict | Notes / consumer |
|------|------|---------|---------|------------------|
| `JavaScriptExternalObject::AddSearchProvider` | 97 | url validity + http/https-only + one-prompt-at-a-time before fetching a descriptor | move | scheme+length gate is pure policy (navpolicy ext surface); the QMessageBox consent and `ToolbarSearch::addEngine` exec stay Qt |
| qrc-channel arming (`urlChanged` → registerObject "arktos") | 365 | whether this commit is an internal qrc page | keep | channel wiring; the scheme test is trivial and page-lifetime bound |
| `TlsVerifier::probeUrl` trigger | 402 | probe on real-load commit only | keep | page-state gate (`m_documentLoadPending`); the probe's own tor/proxy/host gates are below |
| `permissionRequested` connect | 429 | route feature perms to manager | keep | wiring only — WebPermissionManager internals below |
| `certificateError` / `selectClientCertificate` connects | 443, 454 | route engine signals | keep | wiring only |
| History record on loadFinished | 462-485 | which schemes count as visited (excludes arktos-cert-error/http-warning/site-block + managed pdf staging) + OTR gate | move | small scheme-exclusion table belongs to navpolicy/history policy; `history->addHistoryEntry` exec stays |
| `isBrowserHandledScheme` | 615 | the browser-served scheme allowlist (else → external handoff) | move | MD02 core input — one allowlist shared with WPE decide-policy |
| `promptOpenExternalUrl` gate | 655 | subframe drop + one-prompt dedup before desktop handoff | move | verdict = external-handoff/ask/drop (MD02); dialog + `QDesktopServices` exec stay Qt |
| `confirmInsecureFormPost` + `m_insecureFormApprovedHosts` | 706 | once-per-(page,target host) POST approval | keep | modal exec + page-lifetime approvals set; the upstream `shouldWarnFormPost` verdict is already delegated — MD02 only consumes the verdict |
| `acceptNavigationRequest` — arktos-cert-error scheme | 739 | nonce-bound interstitial vs action-link resolution | keep | needs `m_certErrorPending`/nonce page state + `SchemeAccessHandler` registry |
| — arktos-http-warning scheme | 756 | same shape | keep | same reason |
| — arktos-site-block scheme | 772 | same shape | keep | same reason |
| — abp subscribe host rewrite | 784 | subscribe.adblockplus.org?location= → abp: prompt | move | pure url rule → navpolicy verdict `external`/rewrite; `AdBlockManager::promptSubscribe` exec stays |
| — unhandled scheme → external | 811 | block + (main-frame) prompt | move | MD02 external-handoff arm |
| — `torHoldNavigation` | 830 | hold page-initiated nav while daemon can't carry it | keep | needs TorManager/circuit state + view queue |
| — `PdfSupport::managedViewerNavAllowed` | 849, pdfsupport.cpp:176 | staged-viewer file-origin clamp | move | pure scheme/path policy (navpolicy) |
| — file: dir → arktos-file: reroute | 855 | directory detection → reroute | move | the isDir/url-rewrite decision is pure; the deferred `load()` stays. WPE backend mirrors this at webkitbackend.cpp:3283 |
| — `divertToContainerRule` | 878, 1083 | host→container rule + wrong-profile refusal + 8-in-5s rate cap | move | site-rule lookup already SiteDecisionStore-backed (containerIdForHost, containermanager.cpp:575); verdict `divert(container)` belongs in navpolicy; tab spawn + rate-limit state stay Qt |
| — `shouldBlockDomain` | 888 | SEC18 blocklisted host | done | already `NavigationPolicy::shouldBlockDomain` → rustcore policy |
| — https→http bounce detection | 913 | downgraded-mark bookkeeping on redirect hops | keep | delegates the predicates (`isUpgradeCandidate`, `markDowngraded`) to policy already; the `m_requestedUrl` compare is page state |
| — `shouldWarnHttp` (HTTPS-Only) | 940 | warn vs allow http: | done | delegated |
| — `shouldWarnFormPost` | 958 | warn vs allow insecure POST | done | delegated; prompt+approvals stay (above) |
| — `PdfSupport::shouldIntercept` | 990, pdfsupport.cpp:84 | sanitize-on-view claim (.pdf, http/https/file, non-POST, toggles on) | move | pure toggle+scheme+suffix verdict; `PdfSanitizeFetch` exec stays |
| — post-accept modifier divert | 1001 | LinkClicked + user modifiers → retarget tab/window | keep | gesture/modifier state + TabWidget chrome |
| — post-accept bookkeeping | 1014 | JS policy apply, script arming, pending-flag resets, popup tally reset | keep | page state; `applyJavaScriptPolicy`'s verdict input is below |
| `PopupProbePage` | 1151 | dead-end page for blocked popups | keep | probe mechanics; the gate that spawns it is the next row |
| `createWindow` popup gate | 1182 | popupShaped(window/dialog) + `PopupBlocker::shouldBlock(opener)` → probe vs real window | move | MD09 — verdict through navpolicy; WebView/window creation stays Qt |
| `noteBlockedPopupTarget` | 1223 | record open-once url (cap 20, dedup) | keep | per-page tally for the indicator |
| `handleLoadingChanged` — self-heal | 1248 | clear stale downgrade mark on real https commit | done | `clearDowngradedHost` delegated; phantom-commit bookkeeping stays Qt |
| — held-nav/blocked-nav swallow + failure classify | 1294-1401 | tor held-nav, takeBlocked{Domain,Http}Nav, `failureImpliesDowngrade`/`noteNavigationFailure` | done | all verdicts delegated; interstitial/error-page exec + tor retry machinery keep |
| cert-error session whitelist | 1718 | (host, leaf-sha256) → accept/ask; per-profile session set | move | decision data → permpolicy-style verdict store (in-memory profile scope is fine — nothing persists today) |
| `certificateErrorOverridden` / foreign override hosts | 1761, 1773 | cross-profile/cross-engine "host has an override" read | move | same store, host-granularity read |
| `handleCertificateError` | 1808 | main-frame+pending gate, whitelist consult, nonce publish, reject | mixed | verdict consult moves; nonce/interstitial plumbing + `error.rejectCertificate()` keep |
| `resolveCertificateErrorLink` | 1928 | proceed→whitelist+reload / back→history/start page | keep | exec; the whitelist write lands in the moved store |
| `handleClientCertificateSelection` | 1974 | empty-store decline + per-authority remembered choice | move | remembered-choice map is a decision store (per-authority, session); `QInputDialog` picker keeps Qt |
| `show{HttpWarning,DomainBlockWarning}` + resolves | 2041-2260 | interstitial publish/load, proceed/always/back | keep | plumbing; `allowHttpForHost`/`allowBlockedDomain` writes already delegate to policy |
| `isRateLimitInterstitialUrl` | 2269 | google /sorry/ host-shape heuristic | move | pure url heuristic — navpolicy/urlclean table |
| `showRateLimitNoticeIfNeeded` | 2305 | banner injection | keep | runJavaScript exec |
| `applyJavaScriptPolicy` | 2368 | per-site JS verdict → JavascriptEnabled attribute | move | the verdict (rules + tier + pref) — see ScriptControlManager below; attribute write stays |
| `schedulePageScripts` / `setInjectedScriptsEnabled` | 2394, 2412 | which per-page scripts arm | keep | engine script collection |

## WebView — src/webview.cpp

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `contextMenuEvent` composition | 305 | which entries exist/enabled | keep | UI; but the enable flags read `isUrlAllowedFromPageLink` (move) |
| `isUrlAllowedOnUntrustedInput` | 1160 | scheme != javascript: | move | THE untrusted-input floor — shared by dropEvent 1628, middle-click paste 1644, `BrowserApplication::openUrl` 1497, `TabWidget::loadStringFromUntrustedSource` 4433, `LocationBar::dropEvent` 455 |
| `isUrlAllowedFromPageLink` | 1165 | + data:/blob: refused for cross-context opens | move | same policy, stricter tier |
| open*In{Tab,Window,PrivateTab,PrivateWindow,TorWindow} | 670-800 | context + profile picking for page links | keep | view/persona graph exec; the url gate is the moved policy |
| `loadUrlInView` Referer rule | 811 | Referer header only same-profile http(s) | move | small policy bit (referer module surface); request exec stays |
| tor queue machinery | 1184-1435 | hold/queue/dispatch under bootstrap | keep | TorManager state + timers |
| `loadUrl` javascript: exec | 1437 | typed javascript: runs in page | keep | engine exec (intentional path) |
| dropEvent/dragEnter/dragMove | 1575-1633 | drop accept + javascript: refusal | mixed | mime/motion plumbing keep; scheme verdict = moved policy |
| `mouseReleaseEvent` middle-click | 1635 | X11 selection-as-url | keep | gate is the moved policy |
| `searchRequested` background pref | 875 | selection-search tab placement | keep | chrome placement pref — not a nav verdict |
| `allowScriptsOnThisSite` | 1686 | allow-once/always write | keep | UI path; write lands on ScriptControlManager (moved store) |
| canvas/img resolvers, poster, search-form probe | 421-571, 943-1055, 1710-1812 | in-page element resolution | keep | runJavaScriptLifted exec |
| `copyCleanLinkToClipboard` / page clean-link | 648, 590 | tracking-param strip on copy | done+move | uses `UrlCleaner::cleanedUrl`; MD06 moves the ruleset/policy to `urlclean` |
| download triggers (Save Link/Image, canvas Save) | 353, 400, 443-448, 631-639, 834-839 | route url → `m_page->download()` | keep | pure exec into the DownloadManager dispatch above — the policy lands at `handleDownloadRequested`, not here |

## LocationBar — src/locationbar/locationbar.cpp

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `setSanitizesUntrustedInput` + drop fallback | 59, 432 | strip javascript: from paste/drop text | move | the sanitize verdict; text-field mechanics keep |
| `registrableDomainRange` | 144 | SAFE03 domain-emphasis span | move | pure domain math over `twoleveldomains_p` — rustcore already owns similar tables (tranco/lookalike) |
| `unicodeUrlHint` | 247 | punycode→unicode tooltip | keep | display helper |
| Enter TLD-completion modifiers | 396 | ctrl/shift → .com/.org/.net | keep | chrome UX pref — not policy |
| accept path | — | Enter → `TabWidget::loadString` → `guessUrlFromString` (tabwidget.cpp:4575) | done | `rc_classify_input` (OMNI01) already owns url-vs-search; `looksLikeAddress`/`searchInAddressField` remain as the no-rust reference path |

## DownloadManager — src/downloadmanager.cpp

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `alwaysPromptForFileName` read | 203 | save-as vs auto-name per item | move | MD04 policy key (per-profile) |
| `sanitizeFileName` | 409 | untrusted suggested-name → safe basename | move | MD04 — pure name policy |
| `isDangerousExtension` / `isExecutableMimeType` | 447, 474 | warn-required suffix/mime tables | move | MD04 threat tables |
| `confirmSafeToSave` | 502 | warn vs plain save (name∩mime verdict) | move | verdict moves; `QMessageBox` exec keeps Qt |
| `saveFileName` "-N" dedup | 568 | collision rename | keep | needs live-item + fs state |
| `removeExecutableBit` | 534 | strip exec bits on completed file | keep | fs mutation |
| `externalDownload` | 1504 | external-handler enabled + scheme floor + argv | move | MD04 — enabled/path/scheme-gate is policy; `QProcess::startDetached` stays |
| `download()` rustdl dispatch | 1548 | `RustDownloadEngine::canHandle` claim | move | rustdownloadengine.cpp canHandle (~line 60-110): selector pref + scheme + tor/proxy rule = pure verdict |
| `handleDownloadRequested` | 1583 | retry re-attach, sanitize-fetch claim, external, rustdl | mixed | claim/attach plumbing keep; external+canHandle decisions move |
| `updateRow` removal | 1698-1718 | remove OTR-done rows / `removePolicy==SuccessFullDownload` | move | removal verdict is policy; model exec keeps |
| `removePolicy` / `downloadDirectory` | 1728, 1415 | per-profile prefs | move | MD04 per-profile rule store |
| `save`/`load` records | 1741, 1780 | download-history persistence (download_N_*) | move | MD04 — legacy QSettings schema imports into a per-profile rustcore store; OTR exclusion stays a write-time rule |
| `allowQuit` | 1489 | quit-confirm when downloads active | keep | dialog |
| `whenProfileDownloadsSettle` | 1345 | profile teardown gating | keep | live object graph |
| `gateCheck` (rustdl) | rustdownloadengine.cpp:74 | per-hop scheme/SSRF/blocklist/https/adblock | done | already composes the delegated predicates |

## WebPermissionManager — src/webpermissionmanager.cpp

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `isDenied` | 89 | hard-deny feature table (capture, screen, clipboard, fonts, unsupported) | move | MD03 — verdict table |
| `isPromptable` | 106 | ask-capable set (notifications/geo/mouselock) | move | MD03 |
| `keyFor` | 173 | origin→normalized key | move | pure normalization |
| persistent entries (set/get/remove/clear/entries) | 187-279 | durable origin×type store | done | SiteDecisionStore KindWebPermission + legacy import |
| `m_sessionDecisions` / `sessionKeyFor` | 281-313 | session-scoped verdicts, OTR-prefixed | move | MD03 session layer in the same verdict store |
| `handleRequest` order | 336 | deny-first → stored → prompt | move | MD03 verdict pipeline; `resolve` callback keeps Qt |
| `showNextPrompt` | 367 | serialized modal queue | keep | dialog machinery |

## Network interceptors + handlers — src/network/

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `PrivacyRequestInterceptor::interceptRequest` | privacyrequestinterceptor.cpp:230 | per-request verdict (block/redirect/referer/GPC) | done | thin adapter over `NavigationPolicy::evaluate` (ENG02) |
| clean-tally tables | 69-127 | per-host "N trackers removed" bookkeeping | keep | UI bookkeeping state — decision already emitted by the verdict; optionally movable as policy stats |
| `loadSettings` snapshot | 264 | privacy/* QSettings → policy snapshot | move | MD05 — the QSettings reads become rustcore policy-store reads (httpsFirst, httpsOnly, referer, securityLevel, pings, fonts, prefetch, ws, urlCleaning, domainBlocklist, gpcSignal, exception lists) |
| `applyUserAgentSpoof` | 744 | per-site UA header | done | rc_ua_spoof |
| `TorRequestInterceptor::interceptRequest` | torrequestinterceptor.cpp:55 | tor pipeline verdict | done | same evaluate(), torMode arm |
| `FileAccessHandler::initiatorAllowed` | fileaccesshandler.cpp:64 | which initiator schemes may read listings | move | initiator-scheme policy table |
| `InterstitialSchemeHandler::requestStarted` | schemeaccesshandler.cpp:63 | nonce→page serve | keep | scheme serving |
| `DomainBlocklist` fetch/update cadence | domainblocklist.cpp:95-186 | https-only sources, 7-day cadence, 64MB cap, redirect gate | keep | network machinery; verdict membership already rustcore (ac_blocklist_reload) |
| `NetworkAccessManager::loadSettings` proxy | networkaccessmanager.cpp:131 | proxy type/host/port from settings | keep | Qt network plumbing (proxy factory is Qt-owned); password via SecureStore |
| `NetworkAccessManager::sslErrors` / auth dialogs | 194-349 | app-side CA accept + credential prompts | keep | modal + QSslConfiguration writes |
| `CookieJar` gate | cookiejar/cookiejar.cpp:111, 458 | accept/block/session cookie verdict | done | `NavigationPolicy::cookieFilter` |
| `TlsVerifier::probeUrl` gates | tlsverifier.cpp:54 | https-only + tor-off + proxy-off + not-private-host | move | pure gate predicates ahead of the rustcore `rc_tls_check` exec |

## GPC / privacy surfaces (MD05)

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| Sec-GPC verdict bit | privacyrequestinterceptor.cpp:211 | send header on Allow | done | folded in `PolicyVerdict::sendGpc` (tor unconditional) |
| `gpcSignal` toggle storage | settings.cpp:1516,1978; privacyrequestinterceptor.cpp:295,440 | per-profile on/off (default on) | move | MD05 — the pref becomes a rustcore privacy-store read |
| DOM twin install | browserprofile.cpp:685-701, ~890 | install `arktos:gpc` script when toggle on | mixed | toggle read moves (same store); `scripts().insert` exec keeps |
| referer/securityLevel/etc | above | — | move | same MD05 privacy store |

## Popup gate (MD09)

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `PopupBlocker::shouldBlock` | popupblocker.cpp:125 | enabled + http(s) opener + not-allowed | move | unify into navpolicy verdict; `isEnabled` reads `websettings/blockPopupWindows` → policy store |
| allowed/session host sets | 136-194 | suffix-match verdict + session overlay | move | persistent list already SiteDecisionStore KindPopup; session set + match rules move with it |
| `WebPage::createWindow` consume | webpage.cpp:1182 | verdict → probe/window | keep | creation exec; MD09 wires the verdict in |

## Script control (JSCTL) — src/scriptcontrolmanager.cpp

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `ruleForHost` + `bestMatchLength` | 177-231 | longest-suffix rule resolution, session-over-persistent, deny-on-tie | move | pure rule engine → permpolicy/navpolicy verdict |
| `isJavaScriptEnabledFor` | 343 | rules → internal-scheme → Safest tier → http-insecure → global pref | move | verdict composition; the `JavascriptEnabled` attribute write stays (webpage.cpp:2368) |
| `isAllowedHostSnapshot` | 315 | IO-thread allow read | keep | lock-guarded snapshot is the required engine-side visibility — the store behind it moves (already KindJavaScript) |

## Container divert — src/containermanager.cpp

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `normalizeSiteHost` | 516 | rule-key canonicalization (www-fold, public-suffix guard) | move | pure normalization |
| `containerIdForHost` | 575 | host→container rule lookup | move | already reads SiteDecisionStore KindContainer rows via saveSiteRules |
| divert exec + rate cap | webpage.cpp:1083-1130 | spawn tab on ruled profile, 8-in-5s bound | keep | TabWidget/persona exec |

## Session snapshot / autosaver (MD08)

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `AutoSaver` cadence | autosaver.cpp:72-73 | 3s debounce / 15s max-wait constants | move | MD08 — interval policy via AcSession |
| `BrowserMainWindow` changeOccurred hooks | browsermainwindow.cpp:651,695,802,3191-3252 | what marks a session dirty | move | dirty-event set is policy; signal wiring keeps |
| `saveSession` / `restoreLastSession` | browserapplication.cpp:1079,1279 | manifest schema + per-persona routing | done | RC13 rc_session_* store |
| `lazyRestoreTabs`, `suspendTabs(Minutes)`, `memoryPurge` | tabwidget.cpp:4741-4763 | restore/suspend/purge prefs | move | per-profile policy reads via facade |
| `m_pendingRestores` lazy-load | tabwidget.cpp:4982+ | defer first load until activation | keep | timer/tab exec |

## URL cleaning (MD06)

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `kBuiltInTrackingParams` + `isTrackingParameter` | urlcleaner.cpp:32-92 | strip set + prefix-match rules | move | ruleset JSON in profile store |
| `UrlCleaner::cleanedUrl` | 94 | scheme floor + strip exec | move | pure transform — `rc_urlclean_clean` |
| `urlcleaner/extraParams`, `setAdditionalParameters` | 65-75 | user extension point | move | rides the same ruleset store |
| copy-link consumers | webview.cpp:588,648 | apply on copy | keep | clipboard exec |
| nav-time strip | — | verdict Redirect via policy | done | `strippedUrl`/cleaning stage already in evaluate() (POL04) |

## Extension dispatch (MD07 input)

| Site | Line | Decides | Verdict | Notes |
|------|------|---------|---------|-------|
| `ExtensionShimBackend::invoke` dispatch | extensionshimbackend.cpp (~1975 LOC; ns table in .h:199-238) | chrome.* ns→method routing + typed unsupported | keep | host-side dispatch over Qt objects — MD07 is coverage work (see .devin/EXT-RUNTIME-GAPS.md declared set), not a rustcore verdict |
| `extensionNavUrl` | .h:169 | url an extension may put a tab on | move-able | scheme/origin gate — could read the same untrusted-input policy; low value, flag for MD07 |
| `ExtensionRequestWatcher` observe | privacyrequestinterceptor.cpp:251 | feed webRequest listeners | keep | event plumbing |

## Cross-backend mirrors (adoption surfaces for MD02)

- `WebkitPage` decide-policy path — webkitbackend.cpp:3164-3311 (cert scheme, RequestPolicy consult, external scheme, file: dir reroute). Any navpolicy verdict MD02 ships must be consumable here too — it already runs `policy->decide`.
- `ServoProfile` policy consult — servobackend.cpp:~489-530 (decide() → allow/block/stub).
- `BrowserApplication::openUrl` — browserapplication.cpp:1488 (QDesktopServices-registered handler; untrusted gate).
- `BrowserApplication::askDesktopToOpenUrl` — 1481 (records the handed-off url for loop detection).
- `TabWidget::torStartup` re-gate — argv/single-instance urls re-checked in the tor process (webview.cpp:775 comment).

## Priority map for MD02-MD09

- **MD02 navpolicy**: acceptNavigationRequest scheme/interstitial/external branches (739-817), isBrowserHandledScheme (615), file-dir reroute (855), managedViewerNavAllowed (849), shouldIntercept (990), abp rewrite (788), divert verdict (874/1083), isRateLimitInterstitialUrl (2269). Keep: all nonce/interstitial plumbing, tor holds, modifier divert, bookkeeping.
- **MD03 permpolicy**: isDenied/isPromptable/keyFor (webpermissionmanager.cpp:89-185), sessionDecisions (281-313), handleRequest order (336); cert whitelist (webpage.cpp:1718) + client-cert memory (1974) are the same verdict-store shape.
- **MD04 dlwpolicy**: sanitizeFileName (409), dangerous/ext+mime tables (447-500), alwaysPrompt (199), externalDownload gate (1504), canHandle claim, removePolicy/updateRow removal (1713-1739), download records store (1741-1819).
- **MD05 privacy**: privacy/* snapshot reads (privacyrequestinterceptor.cpp:264-337), gpcSignal (settings.cpp:1978), DOM-twin trigger (browserprofile.cpp:685), TlsVerifier gates (tlsverifier.cpp:54).
- **MD06 urlclean**: UrlCleaner tables + cleanedUrl (urlcleaner.cpp), extraParams store; copy-path consumers stay put.
- **MD07 extdispatch**: EXT-RUNTIME-GAPS.md declared set vs invoke dispatch — Qt-side coverage work.
- **MD08 session**: AutoSaver constants, dirty-trigger set, lazy/suspend/purge prefs.
- **MD09 popup**: createWindow gate (webpage.cpp:1182) + PopupBlocker verdict (popupblocker.cpp:125-194) folded into navpolicy.
