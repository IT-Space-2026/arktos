# CODE-AUDIT — dead code + duplication (CA01)

Generated 2026-10-29 on commit c2fa40c2 (+ uncommitted task-table rows).

## Method / tool inventory

| Detector | Tool | Coverage |
|----------|------|----------|
| unreachable code, unused consts, unused lambda captures | `clang++ -fsyntax-only -Wall -Wextra -Wunreachable-code` replayed over all ~197 non-generated TUs (flags extracted from `make -Bn`) | src/ C++ |
| dead stores (`-Wunused-but-set-variable` et al.) | `g++ -fsyntax-only` same TU set | src/ C++ |
| unused members/signals/fields/whole classes/free functions | custom name-reference scan (decl-parse of all 218 headers + word-index over src/, autotests/, tools/, .js/.html for webchannel-invoked names) | src/ C++ |
| `#if 0` / never-true `#ifdef` branches | macro test-vs-defined scan (in-tree `#define`s, `-D` flags, test-build DEFINES, Qt/compiler builtins) | src/ C++ |
| unreferenced files | .pro/.pri SOURCES/HEADERS/FORMS vs on-disk + `#include` graph | src/ |
| dead includes | IWYU-lite: declared-ident sets per project header vs including-file words, then hand-verified | src/ |
| duplicate blocks | normalized-line rolling-hash windows (8 lines), merged into clone regions | src/ C++ |
| dead_code in rustcore/rustdl/adblock-rust | `cargo clippy --offline --all-targets` | Rust |

Known blind spots: Qt polymorphic calls are invisible to text search
(overrides were never flagged as dead); `connect`/`Q_INVOKABLE`/webchannel
names were cross-checked against `src/data/*.js|*.html`; `#define`d API
contracts in FFI headers are counted separately from internal macros.
Prior bug-hunts (AUDIT01/02, STAT01/02) covered memory/UB findings —
this pass is dead code + clones only.

---

## DEAD CODE

### D1. Dead member functions — declared + defined, zero call sites

| Site | Evidence | Why dead |
|------|----------|----------|
| `RustCoreProfiles::avatarPath(const QString &)` | decl src/rustcorebridge.h:439, def src/rustcorebridge.cpp:966 | no textual reference anywhere in src/, autotests/, tools/, *.js — getter was never wired to a consumer |
| `WebPage::noteCertErrorOverride(const QString &)` | decl src/webpage.h:168, def src/webpage.cpp:2064 | zero callers; the cert-error override bookkeeping it was meant to feed is never invoked — flag for CA09 review in case the *call* was lost rather than the method |
| `ExtensionIdleWatcher::detectionInterval()` | inline getter src/extensions/extensionidlewatcher.h:81 | no callers; `m_intervalSeconds` is read directly inside the class (:48,:152) |

### D2. Dead constants (compiler-verified)

| Site | Evidence | Why dead |
|------|----------|----------|
| `MaximumKeywords`, `MaximumKeywordLength` | src/opensearch/opensearchreader.cpp:84-85 | `clang -Wunused-const-variable` **— CA07: FALSE POSITIVE, kept.** Both are read at :246-247 (the `<Alias>` parse), which sits in the `#else` non-rustcore arm — dead only under the default build, live under `no-rust` (D4-class). |
| `kHeaderSize` | src/securestore.cpp:65 | `clang -Wunused-const-variable` **— CA07: FALSE POSITIVE, kept.** Read at :492/626/644/652, all inside the `#else !ARKTOS_RUSTCORE` OpenSSL-EVP arm — live under `no-rust` (D4-class). |

### D3. Dead preprocessor blocks

| Site | Evidence | Why dead |
|------|----------|----------|
| `#if 0` path-check block | src/network/cookiejar/networkcookiejar/networkcookiejar.cpp:236-244 | unconditionally disabled `matchingPath` rejection arm; the comment above documents Firefox-parity intent — dead by design, kept as documentation. Flag for CA07 to delete or leave with a comment (the rationale is already in prose at :233-235). |

Never-true-`#ifdef` scan: **no findings.** Every tested macro is
either defined in-tree, passed via `-D`, defined in autotest scope
(`AUTOTESTS` autotests.pri:11, `ACPROBE_BRIDGE_DEFAULT`
autotests/acprobe/acprobe.pro:41), a system macro (`SIGHUP`), or a
`*_DEBUG` dev toggle compiled in only on demand (18 families,
e.g. `ADBLOCKSUBSCRIPTION_DEBUG` — conditional logging, not dead).

### D4. Dead code under the shipped build config (rust lanes ON)

`#ifdef ARKTOS_RUSTCORE/#ifdef ARKTOS_RUSTDL/#ifdef ARKTOS_SERVO`
blocks that end in `return;`/`return x;` leave everything after the
matching `#endif` **compiled but unreachable** in the default build
(alive under `CONFIG+=no-rust` — these are intentional dual lanes,
not deletable code). clang `-Wunreachable-code` proves three
in-function cases; a source scan counts ~50 sites total.

| Site | What is unreachable under default build |
|------|------------------------------------------|
| src/history/historymanager.cpp:869-880+ | the whole legacy SQL `history.db` flush tail of `save()` — clang: "code will never be executed" |
| src/readermode.cpp:756 | `m_busy = true` + entire JS `extract()` two-phase path after the rustcore early-return in `enter()` |
| src/readermode.cpp:843 | `runDriver(__arktosReader.exit())` tail of `exit()` |
| ~50 more sites | same `#ifdef ARKTOS_* early-return` shape: autofillmanager.cpp:286, downloadmanager.cpp:836/1221/1362/1373/1384/1395/1407/1764/1880, updatechecker.cpp:233/263/299/406/433/523, readermode.cpp:47/312/485/545/692, securestore.cpp:700, pdfview.cpp:358/398/431, profilesettings.cpp:277/314/339/408/564, enginecaps.cpp:25, profilevault.cpp:94/180/346, browserapplication.cpp:1040/1293/1443/1514, main.cpp:4317/4855/5278/5460/5821/11079/11278/24177, tabwidget.cpp:4621/4715/5780, extensionshimbackend.cpp:368/916/952/4682/5121/7431, bookmarknode.cpp:66/140, bookmarksmanager.cpp:235/465/736, historycompleter.cpp:152/179/210, historymanager.cpp:87/432/814 |
| Treatment | **not deletion candidates** — they are the `no-rust` build lanes. Listed so CA07 knows why the compiler's unreachable-code hits must NOT be "fixed" by deleting the tails. If the lanes are ever dropped, all of these become dead. |

### D5. Dead includes (project-local; hand-verified)

| Site | Include | Why dead |
|------|---------|----------|
| src/browsermainwindow.cpp:93 | `networkaccessmanager.h` | zero `NetworkAccessManager` references in the TU |
| src/adblock/adblockmanager.cpp:44 | `networkaccessmanager.h` | uses `AdBlockNetwork` (adblockmanager.h:133), not NAM — leftover |
| src/webviewsearch.cpp:24 | `webpage.h` | **CA07: FALSE POSITIVE, kept.** `m_webView->webPage()->showErrorPage(...)` at :167 needs the complete `WebPage` type — removal breaks the build. |
| src/bookmarks/bookmarkstoolbar.cpp:25 | `bookmarknode.h` | `BookmarkNode` never named; calls go through `BookmarksManager`/`model->index(BookmarkNode*)` — a fwd decl suffices |
| src/locationbar/sitepanel.cpp:27 | `browsertheme.h` | no `BrowserTheme`/`ColorScheme`/`isDarkPalette` reference in the file |

Checked and **live** (would be CA07 churn if reported): `clearbutton.h`
in toolbarsearch.cpp (`clearButton()->hide()` needs the complete type),
`downloadworker.h` in main.cpp (`downloadWorkerMain()` called :5830),
three `twoleveldomains_p.h` includes (`twoLevelDomains[]` iterated at
containermanager.cpp:498, locationbar.cpp:230, networkcookiejar.cpp).

### D6. Unused lambda captures — 47 sites (compiler-verified class)

`clang -Wunused-lambda-capture`: lambdas that close over objects they
never use. Mostly smoke-leg boilerplate in main.cpp — the recurring
`[&application]` and `[host]` captures in `finish`/`report` lambdas.
Full site list: `application` ×21 (main.cpp:2023, 2122, 10906-ish…
see /tmp/ca01/clang_warn.txt), `host` ×7 (main.cpp:13986, 14168,
14406, 15021, 16480, 16675, 17891), `this` ×7 (browsermainwindow.cpp:2424,
readermode.cpp:617/623/629, bidipanel.cpp:1246/3470/5571,
extensionmanager.cpp:414, extensionshimbackend.cpp:2119 [`tab`],
sidebarpanel.cpp:1330), `serveDelayMs` ×2 (main.cpp:10867, 10906),
`blobSize` ×2 (main.cpp:7230, 7308), `poll` (main.cpp:8444),
`writeAndExit` (main.cpp:1775), `view` (main.cpp:3592),
`iterationCount` (main.cpp:3642), `heavy2` (main.cpp:13129),
`server` (main.cpp:24020), `docPage` (main.cpp:16527).
Harmless individually; a shared smoke-lambda shape would remove the
class (see C14).

**CA07 application:** 43 of 46 flagged captures removed. 3 stayed —
clang's "not required to be captured" verdict is over-lenient vs GCC:
`blobSize` (main.cpp:7229, 7308 lambdas) and `serveDelayMs` (:10866)
are odr-used in ways GCC still requires a capture for (verified by
build error). `iterationCount` (:3642) and `serveDelayMs` (:10906)
removed cleanly under both compilers.

### D7. Rust dead code

| Site | Evidence | Why dead |
|------|----------|----------|
| `SRCS_TRANCO` | src/rustcore/src/tranco.rs:544 | `cargo clippy`: "constant is never used"; `#[cfg(test)]` re-export of `SRC_TRANCO` — only `SRCS_MAJESTIC` (:548) is consumed by sitetrust tests |

FFI-contract constants (`AC_CAP_*` ×13, `AC_ERR_*` ×5,
`RC_SITETRUST_PROBE_ALL`, `RC_SESSION_STATE_FILE` —
src/rustcore/include/arktos_core.h:73-85/99-104,
rustcore.h:403/752) are unreferenced by every C++ consumer —
`ac_capabilities()` is never called; only `AC_ERR_NOT_FOUND` is used
(autotests/rustevents:217). They exist for parity with
rustcore/src/facade.rs:53-58 — keep, but note the C++ side never
inspects the capability bitmask.

### D8. Searched, nothing found (honest negatives)

- **Dead stores**: `g++ -fsyntax-only` produced zero warnings over all
  TUs — `-Wunused-but-set-variable` clean.
- **Whole dead classes**: none — every class is instantiated or
  referenced outside its own decl block (RAII guards like `TorGuard`,
  `PaletteGuard` are used via trailing-declarator `} guard;`).
- **Dead signals**: every declared signal is emitted or connected.
- **Unreferenced files**: only `src/rustcore/tools/gen_cookie_export.cpp`
  — a documented one-shot testdata generator (built manually per its
  header comment); not dead, intentionally outside the build.
- **Free functions**: zero unreferenced file-scope functions found.

### D9. Verified live (so CA07 doesn't re-chase them)

`ChromeBridge::reportError` (invoked from src/data/extshim.js +
exthost.js), `PipPlayerBridge::notifyReady/notifyError`
(src/data/pip-player.html), `ReaderBridge::setPreference`
(src/data/reader-forward.js), `SchemeAccessHandler::publishInterstitialPage`
(webpage.cpp:2127/2344/2461), `SourceHighlighter::highlightBlock`,
`TabBar::tabRemoved`, `FlowLayout::*` (Qt polymorphic overrides),
`AdBlockNetwork::matchLinear/matchLinearUnlocked` (differential-test
reference impl — called by autotests/adblock/adblocknetwork:381/399/513).

---

## DUPLICATE BLOCKS

127 clone groups detected (8-line normalized windows). Grouped below:
**A** = cross-file verbatim clones (real dedup targets),
**B** = within-file clones,
**C** = excluded families (config-gated parallel arms — not dups).

### A. Cross-file clones

| # | Canonical | Clone(s) | What |
|---|-----------|----------|------|
| A1 | src/tabwidget.cpp:4804-4838 ≡ src/autosaver.cpp:102-136 | (either — both are file-local `static` helpers; natural home is rustcorebridge) | `sessionPolicyCall()` — 35-line rust FFI roundtrip helper, **byte-identical** including comments |
| A2 | src/profileregistry.cpp (MPROF01, canonical) | src/profilestore.cpp (MPROF05 lane clone — its own header comment admits "the main lane's ProfileRegistry has not merged into this branch yet") | `isValidProfileId` reg:61≡store:89 (verbatim incl. comment), `slugify` 93≡122, `load` 114≡192, `updateProfile` 190≡254, `save` 217≡514; plus `avatarPixmap` store:480≡profileidentity.cpp:207 |
| A3 | src/scriptcontrolmanager.cpp:151-160 | src/popupblocker.cpp:131-140 | `normalizeHost` host-canonicalizer, verbatim incl. the leading-dot comment |
| A4 | src/locationbar/siteshield.cpp:145-153 | src/tor/torcircuitbutton.cpp:547-555 | "fit menu to panel sizeHint" helper, verbatim; related: siteshield.cpp:401-412 ≡ popupblockerbutton.cpp:110-121 |
| A5 | src/locationbar/sitepanel.cpp (canonical) | src/locationbar/sitepadlock.cpp | ctor 147-156≡115-124, `setWebView` 331-343≡154-166, `setEngineTab` 367-387≡183-203 — button wiring boilerplate cloned verbatim |
| A6 | src/browserpaths.cpp:508-516 | src/profilevault.cpp:112-120 | `pathChainHasLink()` symlink-walk helper, verbatim |
| A7 | src/autofillmanager.cpp:790-800 | src/adblock/adblockpage.cpp:418-427 | `replacePageScript()` — same name, near-identical body; **comments diverge on injection world** (ArktosChannelWorld vs main world) — a shared helper must keep the worldId parameter explicit |
| A8 | src/tabwidget.cpp:5440-5448 | src/network/cookiejar/networkcookiejar/networkcookiejar.cpp:177-184 | QDataStream session-state prologue (`sd`/`marker`/`v` decode) |
| A9 | src/network/privacyrequestinterceptor.cpp | src/tor/torrequestinterceptor.cpp | `toHeaderList()` 136-143≡40-47 verbatim; script-resource-type switch 160-170≡70-80 verbatim |
| A10 | src/extensions/declarativenetrequest.cpp:285-300 | src/extensions/extensionrequestwatcher.cpp:108-123 | Chrome match-pattern parser (`<all_urls>`/`scheme://host/path` decomposition), verbatim — **two copies of security-relevant URL matching; drift risk** |
| A11 | src/pdfworker.cpp / src/downloadworker.cpp | (mutual) | `emitLine` JSON-line emit 49-56≡57-64; `/proc` child-scan 87-103≡193-209 |
| A12 | src/engine/navigationpolicy.cpp:1589-1599 | src/network/cookiejar/cookiejar.cpp:797-808 | domain-suffix rule-match loop (`withoutDot`/`endsWith('.'+rule)`/`rule==domain`), verbatim |
| A13 | src/extensions/extensionshimbackend.cpp:105-125 | src/extensions/extensionnotificationcenter.cpp:37-54 | `{ok,result}`/`{ok:false,error}` reply-shape helpers |
| A14 | src/extensions/extensionbackgroundhost.h:350-374 | src/extensions/extensionbackgroundhost.cpp:769-791 | extension `QWebEnginePage` ctor boilerplate (PRIV07 tracking + permission-request lambda + clipboard attribute) — comments acknowledge "same rule as ExtensionChromePage" |
| A15 | src/network/domainblocklist.cpp | src/network/trancolist.cpp | list-loader skeletons: include preamble 29-42≡30-43, file-load 98-112≡134-148, reload guard 159-168≡199-208 |
| A16 | src/clearprivatedata.cpp:337-345 | src/locationbar/sitepanel.cpp:1153-1161 | per-site storage-clear JS snippet (localStorage/sessionStorage/indexedDB wipe) — two copies of a privacy-critical payload |
| A17 | src/locationbar/readerbutton.cpp:61-78 | src/utils/arktosicon.cpp:147-164 | badge overlay painting onto icon pixmap |
| A18 | src/modelmenu.cpp:372-382 | src/modeltoolbar.cpp:224-234 | ModelMenu/ModelToolBar — Qt-demo sibling pair; `updateActions`-style walk (upstream-parallel by origin, lowest priority) |
| A19 | src/containermanager.cpp:497-504 | src/locationbar/locationbar.cpp:229-236 | `twoLevelDomains[]` linear-scan loop |
| A20 | src/extensions/extensionshimbackend.cpp:1348-1355 | src/extensions/extensionbackgroundhost.cpp:2147-2154 + extensionshimbackend.cpp:2885 | "walk every browsing window → tabWidget → trackable webView" enumeration |

### B. Within-file clones (same logic, two arms)

| File | Sites | What |
|------|-------|------|
| src/extensions/extensionshimbackend.cpp | 4439-4452/4469-4480/4889-4900; 4700-4709/4771-4778/4818-4825; 915-922/951-958; 927-939/5140-5152; 2079-2092/8392-8402; 2345-2355/2476-2486; 2430-2445/2814-2829; 2502-2510/2626-2634; 4154-4162/4205-4213; 4276-4284/4308-4316; 7251-7271/7458-7478; 7431-7438/7514-7521; 8453-8462/8487-8496; 476-490/628-642 | `chrome.*` handler boilerplate: window-target resolution, arg validation→`errReply`, event payload assembly, URL scheme-safety check — the handler table repeats a ~10-shape vocabulary; extractable into helpers |
| src/extensionbackgroundhost.cpp | 634-647/2192-2205 (`ok` reply lambda), 911-920/956-965 (JS source assembly), 2344-2353/2657-2666 (port-disconnect emission) | page-source / reply / port-teardown boilerplate |
| src/adblock/adblocknetwork.cpp | matchNativeUnlocked 288-411 vs matchLinearUnlocked 414-535 (five shared regions) | native-indexed vs linear-scan arms — **intentional differential pair** (autotests assert identical decisions); keep both, but the shared rule-eval inner blocks could be factored |
| src/tabwidget.cpp | 1205-1214/1492-1500; 2005-2022/2516-2529 (HiddenGroupTab snapshot capture); 2290-2299/2345-2354 (collapsed-group walk); 5293-5302/5914-5923 (orderedSessionPages walk) | session save/restore arms |
| src/securestore.cpp | 230-238/632-640, 706-715/981-990, 726-739/995-1008, 774-783/1051-1060, 805-817/1102-1114, 819-829/1120-1130, 840-858/951-966 | **NOT dups** — `#ifdef ARKTOS_RUSTCORE`/`#else` arms of the same functions (see C1) |
| src/engine/navigationpolicy.cpp | 1467-1482/1553-1568 | Referer-header scan duplicated across two policy arms |
| src/statusbarwidgets.cpp | 420-440/473-490 | `/proc/<pid>/stat`+`cmdline` child-scan loop cloned into two counters |
| src/readersanitizer.cpp | 303-339/406-442 | HTML tag whitelist table copied into `isForeignElement`'s exclusion set — second copy could reuse the first `QSet` |
| src/readermode.cpp | 567-575/636-644 | reader-bar stylesheet string + theme arms |
| src/webview.cpp | 764-776/809-817 | detached-OTR profile selection block |
| src/browserapplication.cpp | 1559-1576/1647-1664 | session window-restore persona resolution |
| src/downloadmanager.cpp | 2153-2163/2204-2214 | download-item widget state update (stop/tryAgain/progressBar) |
| src/tabbar.cpp | 2213-2222/2255-2264 | tab-shape paint arms |
| src/history/historycompleter.cpp | 628-638/661-671 | `Qt::FontRole` light-weight font arms in two models' `data()` |
| src/sidebarpanel.cpp | 957-967/1239-1249 | DPR-aware avatar pixmap construction |
| src/bidipanel.cpp | 4961-4974/5000-5013 | CDP `sendEval` DOM-props boilerplate |
| src/tor/torcontrol.cpp | 155-168/370-382 | control-port command round-trip arms |
| src/network/networkaccessmanager.cpp | 228-235/283-290 | `NAM_DEBUG`-gated `createRequest` prologue |
| src/opensearch/opensearchmanager.cpp | 992-1001/1221-1230; 1017-1035/1102-1120 | mwQuery prologue + engine-settings restore blocks |
| src/opensearch/opensearchwriter.cpp | 103-112/125-134/148-157 | `<Url>` element emission per engine shape |
| src/extensions/extensionmanager.cpp | 798-806/833-841 | unload-error early-returns |
| src/network/cookiejar/cookiemodel.cpp:158-166 | src/network/cookiejar/cookieexceptionsmodel.cpp:137-144 | `data()` role arms (Qt::FontRole→QApplication::font()) |
| src/main.cpp | ~30 groups: 631-653/750-772 (verbatim `listen-on-loopback` socket helper), 2810-2817/3019-3026/3276-3283 (`finish` lambda shape), 2819-2833/3285-3299, 2838-2846/3053-3061, 2853-2864/3069-3080, 3087-3097/3327-3337, 3121-3154/3366-3396, 8209-8217/8577-8585, 11053-11061/11485-11492, 11063-11074/11788-11799, 14809-14819/15232-15242, 14829-14839/15262-15272, 15281-15289/15644-15652, 15370-15377/15663-15670, 15453-15461/15482-15490, 16227-16237/16275-16288, 16528-16536/16572-16580, 16806-16815/16957-16966, 16880-16890/17114-17124, 17357-17364/17490-17497, 17384-17392/17517-17525, 17395-17403/17527-17535, 18214-18224/18309-18319, 20620-20628/21055-21063, 20693-20702/20976-20985, 20749-20759/21006-21016, 23848-23867/24233-24252, 25001-25015/25483-25497 | smoke-harness leg boilerplate: every `--*-smoke` leg re-implements the same serve/poll/report skeleton. One shared `SmokeLeg` helper would collapse the class (and fix D6's dead captures for free) |

### C. Excluded — parallel-by-design, not dedup targets

| Family | Why excluded |
|--------|--------------|
| `engine/webenginebackend.h` vs `engine/servobackend.h` (66-84≡276-294, 96-107≡307-316, 195-213≡362-375) | both classes must declare the same `EngineInterface` overrides — interface conformance, not copy-paste |
| `securestore.cpp` rustcore vs OpenSSL-EVP arms | `#if/#else` config alternates — only one arm compiles per build |
| `adblocknetwork` match*Unlocked pair | differential-test reference impl (see D9) |
| `domainblocklist`/`trancolist` include preambles | shared include vocabulary, no logic |
| `moc_*`/`qrc_*`/`ui_*` generated files | generated |

---

## Summary counts

- Confirmed dead member functions: **3** (D1)
- Dead constants: **3** (D2, +1 Rust D7)
- Dead `#if 0` block: **1** (D3)
- Dead includes: **5** (D5)
- Unused lambda captures: **47** sites (D6)
- Config-gated unreachable tails: **~50** sites — informational only,
  they back the `no-rust` build lane (D4)
- Dead stores / dead classes / dead signals / unreferenced sources:
  **0** (D8 — compiler + scan clean)
- Clone groups: **127** → ~20 cross-file families (A) + ~35 within-file
  families (B) + smoke-harness boilerplate class + excluded parallels (C)

---

## SECURITY (CA02)

Generated 2026-10-29. Method: adversarial source review of the
untrusted-input and trust-boundary surfaces — URL scheme/host
validation, cert/TLS, proxy trust, nativeMessaging manifest+spawn,
extension shim/bridge boundary, stored secrets, JS-injection
callers, exec/spawn paths — cross-checked against the SEC01-29,
VSEC01/02, BADSSL and XSLEAK01 reports so only *open* findings are
listed. Prior audits fixed the big classes; what remains is below.
Severity: **high / med / low**. Findings only — no code changes.

### S1 — Tab-hosted extension pages hand `chromeBridge` to foreign subframes — **HIGH**

`ExtensionBackgroundRuntime::attachExtensionPage()`
(src/extensions/extensionbackgroundhost.cpp:1970-2021) binds a fresh
`QWebChannel` with `chromeBridge` in `MainWorld` on *any* WebPage whose
committed url is `chrome-extension://<id>` — reached from
`WebView::init`'s `aboutToLoadUrl` hook (src/webview.cpp:234-244) and
the EXT19 view watcher (:2094-2112). A `setWebChannel` is a *page*-
level property: Qt exposes the transport + objects to **every frame of
the page in that world**, subframes included.

The hosted background page and popup/probe pages carry the QUAL03 /
SEC26 navigation locks (`HostedBackgroundPage`, `ExtensionChromePage`
— extensionbackgroundhost.h:341-400, :759+; main frame locked to the
extension's own origin, subframes confined to it). A `WebPage` in a
real tab has **no equivalent rule** — `WebPage::acceptNavigationRequest`
(webpage.cpp:817+) imposes no subframe confinement for chrome-extension
documents, and `attachExtensionPage` only checks the *top* url.

**Exploit sketch:** an extension whose options/help page loads remote
content in an iframe (oauth embeds, dashboards — a real-world pattern
Chromium's model makes safe) opens it in a tab. The remote subframe
sees `qt.webChannelTransport` in its MainWorld and can speak the
channel protocol (it needs nothing from us — the client protocol is
re-implementable) to call `chromeBridge.invoke(...)` as the extension:
- `invoke("native","connect",…)` — spawn a registered native host
  (nativeMessaging grant permitting, extensionbackgroundhost.cpp:632-705);
- `invoke("scripting","executeScript",…)` — inject into any tab whose
  url the extension's grants cover (:3578-3820);
- `invoke("storage",…)` — read the extension's storage (a password
  manager's vault metadata);
- `invoke("tabs","sendMessage"/"connect")` / `invoke("relay",…)` —
  reach the extension's other contexts and page agents.

Chrome exposes **no** extension surface to a web iframe; here the web
frame inherits the extension's entire bridge. Needs: a hosted
extension document iframing remote content. Likely fix direction:
subframe confinement on WebPage for chrome-extension main frames
(mirror `ExtensionChromePage::navigationAllowed`), or a per-frame
channel world the bridge binds to only for the top document.

### S2 — PDF worker's `file` field is a parent-side path — traversal to arbitrary delete — **HIGH**

`PdfDocumentClient::handleWorkerOutput()` (src/pdfview.cpp:318-331)
takes the worker's `"page"` event `file` string and does:

    const QString path = m_workDir->path() + '/' + file;
    QImage image(path, "PNG");
    QFile::remove(path);

No normalization or confinement. The worker is the component bwrap
exists to confine (`SandboxManager::bwrapPath()`, pdfview.cpp:207-216)
— and where bwrap is absent it runs **unconfined** (`worker` backend).
A compromised/crashed worker emitting `"file": "../../.bashrc"` gets
the parent to `QFile::remove()` outside the work dir — escaping the
sandbox into the parent's fs authority. A PNG-parseable target is also
read and shown as a page image (info disclosure of renderable files).

Secondary: `m_stdoutBuffer` (:295) grows unbounded — a hostile worker
emitting newline-free output grows RSS without limit (stderr is capped
at 8192, :237-238 — the asymmetry shows the intent).

**Exploit sketch:** memory-safety bug in the hayro rasterizer (or in
the unconfined `worker` backend) → controlled JSON on the worker's
stdout → parent deletes/reads arbitrary user files. Fix direction:
hard-confine `file` to the work dir (`QDir(m_workDir->path())
.filePath(file)` + `canonicalFilePath()` prefix check, refuse `..`),
cap `m_stdoutBuffer`.

### S3 — App-side `sslErrors` handler persists attacker certs into the CA store — **HIGH**

`NetworkAccessManager::sslErrors()` (src/network/networkaccessmanager.
cpp:282-349), still the Qt-demo shape:

- On "Yes, ignore" it **appends the presented certificates to
  `QSslConfiguration::defaultConfiguration()`'s CA list** (:334-338)
  and persists the merged set to `settings["CaCertificates"]`
  (:341-344) — a one-time "ignore these errors" click installs the
  peer's certs as **trust anchors** for every app-side fetch forever
  (update manifest, adblock lists, opensearch icons, suggestions,
  sync traffic — all ride this NAM).
- The skip rule `ca_merge.contains(error.at(i).certificate())`
  (:297-304) then silently ignores *any* error type for a stored
  cert — including `HostNameMismatch` when a stored cert is served
  on a different host → auto-`ignoreSslErrors()` (:305-307), no
  prompt.
- `reply->ignoreSslErrors()` (:306, :347) is called with no error
  list → ignores *all* errors on the reply.

**Exploit sketch:** a MITM/proxy serving a self-signed or attacker CA
chain; user clicks through once → the CA is a permanent trust anchor;
all subsequent app-side TLS to any host validates under it. The page-
load plane is unaffected (WebEngine's cert path is the SEC06
session-scoped host+fingerprint whitelist — correct), which makes this
easy to overlook. Fix direction: never add peer certs to the CA store;
scope the accepted exception to (host, fingerprint) in memory like the
WebPage path, and don't auto-ignore mismatches.

### S4 — DNR `transform`/`regexSubstitution` redirect outputs are unvalidated — **MED**

`DeclarativeNetRequest`'s literal-`url` redirect is scheme-gated to
http/https/chrome-extension (declarativenetrequest.cpp:724-733) — but
`resolveRedirect()` (:1370-1447) re-validates nothing for:

- `transform` (:1379-1437): `setScheme(t["scheme"])` accepts **any**
  scheme — a rule can rewrite an https request into `file:///etc/passwd`,
  `data:`, `javascript:`, or `chrome-extension://other-ext/…` (the
  extension-id scoping applied to `extensionPath` kind 2 doesn't apply
  to literal/transform targets);
- `regexSubstitution` (:1439-1443): `out.replace(re, sub)` can produce
  any string → `QUrl(out)`.

`info.redirect()` then issues the navigation. A `file:` main-frame
redirect lands on local content; combined with `dnrPatternCovers(
"<all_urls>")` covering `file:` (:288-291) and `hostPermCoversPattern`
granting `file` coverage under `<all_urls>` (extensionbackgroundhost.
cpp:4165-4170) — **with no Chrome-parity "allow access to file URLs"
opt-in gate found anywhere in the runtime** — an extension declaring
`<all_urls>` + `declarativeNetRequest` (+ `scripting`) can redirect a
victim navigation to a local file and script-read it.

**Exploit sketch:** extension with DNR permission registers
`{regexFilter:"(.*)", transform:{scheme:"file", path:"/etc/passwd"}}`
on main_frame → user navigation lands on the local file; an
`executeScript` on the covered `file:` url exfiltrates it. Chrome's
DNR only permits http/https/extension-resource redirect targets, and
file: access is a separate user toggle. Fix direction: re-gate the
resolved redirect url (scheme ∈ http/https/chrome-extension-with-own-
id), and gate `file:`/`ftp:` out of the coverage grammar or behind a
file-access setting.

### S5 — nativeMessaging host-name regex is unanchored — **MED**

`NativeMessagingSession::resolve()` (src/extensions/extensionbackground
host.cpp:400-404):

    static const QRegularExpression nameRe(
        "[a-z0-9_-]+(\\.[a-z0-9_-]+)*");
    if (appName.isEmpty() || appName.size() > 128
            || !nameRe.match(appName).hasMatch()   // substring match
            || appName.contains("..")) ...

`hasMatch()` accepts a name containing a matching *substring*, so
`a/b`, `HOST.Name`, `x y`, `.z` all pass the Chrome host-name rule —
`fileName = "a/b.json"` then resolves **subdirectories** of the search
dirs, which Chrome's flat `NativeMessagingHosts/` layout never consults
(the `..` check still blocks parent traversal; `QDir::filePath`
concatenates so an absolute `appName` can't escape either — the real
escape is subdir + non-Chrome name shapes).

Secondary, same function: manifest `path` is honored relative to the
manifest dir (:448-450) — Chrome requires an absolute path on POSIX,
so a manifest can carry `"path": "../payload"`; and `type` absent is
accepted (:437-442, Chrome requires `"stdio"`). No ownership/
permissions check on the manifest file either (a world-writable
manifest in a compat dir would be honored).

**Exploit sketch:** same-UID attacker able to write under
`~/.config/chromium/` (a compromised desktop app, a flatpak with
config access) plants `NativeMessagingHosts/sub/evil.json` +
`../payload` — an extension's `connectNative("sub/evil")` resolves it.
Fix direction: `anchoredMatch()` on the full string; refuse relative
`path` on POSIX; require `type=="stdio"`.

### S6 — DNR `modifyHeaders` values unchecked for control characters — **LOW**

`parseRule` validates header *names* against the token grammar
(declarativenetrequest.cpp:792-818) but `hop.value = value.toUtf8()`
(:836) accepts anything — CR/LF included. `setHttpHeader` on the
request then writes it (privacyrequestinterceptor.cpp:294-302).
Chromium's header layer likely normalizes or refuses, but a CRLF-
carrying value reaching the wire is header-splitting/request smuggling.
Extension-local (needs `declarativeNetRequest`), low severity — flag
for a `value`-side token validation pass.

### S7 — `randomBytes()` leaves tail bytes uninitialized — **LOW (latent)**

src/securestore.cpp:431-439 fills `count` in 8-byte strides:
`for (i = 0; i + 8 <= count; i += 8)` — any `count % 8` tail stays
`Qt::Uninitialized`. Only called with `kKeySize == 32` today, so no
live impact; a future caller with a non-multiple-of-8 count (e.g. a
nonce) would mint key material with uninitialized tail bytes.

### S8 — native-messaging inbox has a per-frame cap but no total cap — **LOW**

`NativeMessagingSession::drainStdout()` (extensionbackgroundhost.cpp:
578-609) bounds each frame at `kMaxNativeHostMessage` (64 MiB) but
`m_inbox` accumulates unboundedly — a hostile host streaming many
sub-cap frames (or a partial giant frame) grows memory without limit.
Chrome's 1MB total-message model never got ported; the per-frame
bound exists only against the corrupt-length case per the :363-366
comment. DoS-shaped, same-UID host process only.

### Checked, no findings (honest negatives)

- **Cert-error / HTTPS-Only / site-block interstitials**: nonce-bound
  action links, session-scoped (host,fingerprint) whitelist per
  profile, subframe errors hard-denied, `isOverridable` honored,
  registry bounded at 64 (webpage.cpp:2008-2260, schemeaccesshandler.
  cpp:100-117). The `arktos-cert-error`/`arktos-http-warning`/
  `arktos-site-block` schemes serve only published nonces.
- **file/dir listings**: `arktos-file:` refuses remote initiators
  (fileaccesshandler.cpp:64-86); not SecureScheme/CorsEnabled.
- **External scheme handoff**: non-browser schemes → consent prompt
  with `Qt::PlainText` rendering + modal-stacking guards
  (webpage.cpp:737-781, 978-984); `javascript:` refused on every
  untrusted path (argv, single-instance socket, QDesktopServices
  handler, tor-window argv — SEC23 + spot-verified).
- **Single-instance IPC**: `messageReceived` feeds
  `loadStringFromUntrustedSource` (browserapplication.cpp:598-667,
  tabwidget.cpp:4563-4576) — gated, `file:` allowed by SEC09 design.
- **Update checker**: trusted-URL gate on assets (https + github/
  api-host only, no userinfo), 1MB manifest cap, `releaseTitle`
  rendered PlainText, manual download only — no auto-install
  (updatechecker.cpp, updatedialog.cpp).
- **Spawn/exec paths**: all argv, no shells — tor daemon (fixed args,
  env override `ARKTOS_TOR_BINARY` documented), external download
  handler (scheme allowlist http/https/ftp + quoted-split argv,
  downloadmanager.cpp:1715-1754), tor-window respawn, profile-wipe
  respawn, pdf worker (bwrap when present).
- **Download filenames**: both separators stripped, per-segment
  sanitize for Chrome-relative subdir names, `.`/`..`/control
  chars/drive-letters/`~` rejected, dedup vs on-disk + in-flight
  (downloadmanager.cpp:494-529, 686-786).
- **Extension boundary**: page-agent `agentCovers` re-proved per call;
  sender.url taken from committed page url never caller input;
  mid/chan namespaced `<extId>#`; scripting.executeScript gated on
  permission + per-frame coverage incl. blob:/about: inheritance
  (extensionbackgroundhost.cpp:3234-3820, extensionpagebridge.cpp).
- **Secrets**: sealed stores + bounded KDF parse (SEC03/SEC28 already
  covered — re-verified the bounds are live, securestore.cpp:506-512).
- **Proxy config**: settings-only, tor mode ignores clearnet proxy
  (networkaccessmanager.cpp:140-165); proxy password sealed (SEC03).

## MEMORY / LEAKS (CA03)

Generated 2026-10-29. Method: static ownership review over src/
(~198 TUs) with a purpose-built scanner — every `new` of a
QObject-derived class without an inline parent was resolved to its
owning call (parent arg, addWidget/insertTab/setDefaultWidget,
WA_DeleteOnClose, owner-map with delete, or documented
process-lifetime registry) — plus targeted passes for contextless
`connect()`, QNetworkReply lifetimes, raw `new`/`delete` of
non-QObject project types, `QThread` worker teardown,
scheme-handler/request-interceptor ownership, container-profile
teardown, and the Rust FFI boundary (handle open/close pairing,
`char *` returns vs `rc_string_free`/`ac_string_free`/`rc_buffer_free`
and the `*Take` wrappers). Dynamic leg: a throwaway
`linux-clang CONFIG+=sanitize` (ASan+UBSan+LSan) build ran 13 smoke
flags; because EXTEXIT01 makes every exit lane `std::quick_exit`
(which skips the atexit LSan pass), the throwaway tree registered
`__lsan_do_recoverable_leak_check` via `std::at_quick_exit` — the
check then reports genuinely unreachable blocks while live objects
still count as reachable. Leak blocks were classified by ALLOCATION
SITE exactly as check-leaks.sh does — "ours" only when an early
frame resolves under src/ or autotests/. Cross-checked against the
QUAL02 + MEM01/02/07/09/10/11 and PRIV09 baselines so only *open*
findings are listed. Severity: **high / med / low**. Findings only —
no code changes.

### Result: 0 leaked heap blocks in our code; 3 lifetime (dying-object) findings

The leak half of the sweep is clean — matching MEM01's gate verdict
(0 our-code blocks of 21618) and the MRG07/08 sweeps. But the same
instrumented leg produced three *lifetime* findings: sanitizer-proven
member calls on partially-destructed objects — the "dead objects
kept alive" class the task covers. All three are UBSan/ASan
observations on shipped code paths, not speculation.

### L1 — `CookieJar::save()` dereferences a mid-destruction profile — quit-path SIGSEGV — **HIGH**

`CookieJar::instance(profile)` parents each jar to `qApp` and
registers `profile, &QObject::destroyed → delete cookieJars().take(profile)`
(cookiejar.cpp:153-162). When a profile dies, the jar is deleted
*inside the profile's own `destroyed` emission* — `~CookieJar` calls
`m_saveTimer->saveIfNeccessary()`, which lands in `CookieJar::save()`,
whose first statement is `m_profile->isOffTheRecord()`
(cookiejar.cpp:479). At that instant `m_profile` is the object being
destroyed: its dynamic type has already reverted past
QWebEngineProfile, so the member call is undefined behavior — and in
practice the profile's private pointer is already torn down, so the
call crashes.

Evidence — `--settings-smoke` on the instrumented build exits 139:

    network/cookiejar/cookiejar.cpp:479:20: runtime error: member call on
    address 0x5030004f8910 which does not point to an object of type
    'QWebEngineProfile'  (object has invalid vptr)
    ARKTOS CRASH pid=… signal=11 (SIGSEGV) addr=0x28
    stack: QObjectPrivate::deleteChildren → ~QObject →
           QMetaMethodInvoker::invokeImpl (destroyed emission) →
           … → QWebEngineProfile::isOffTheRecord

Any profile death with a live jar hits this — container teardown,
profile release, app quit with a secondary profile. The fix belongs
to CA10: guard `m_profile`/`save()` with a `QPointer`, or check
"profile is dying" before the save flush.

### L2 — `ExtensionEventHub` removed-tab sweep dereferences a dying window — **MED**

`onWindowDestroyed()` calls `syncNow()` while the window is
mid-destruction (extensioneventhub.cpp:860-865). The live-scan loop
skips `m_dyingWindows` windows (:888-890), so the dying window's tabs
fail the `seen` filter and reach the removed-tab sweep, which runs:

    WebView *live = tab.view.data();
    BrowserMainWindow *home = tab.window.data();   // :1017 — UB downcast
    TabWidget *homeTabs = home ? home->tabWidget() : nullptr;  // :1018

`QPointer<BrowserMainWindow>::data()` static-downcasts to
BrowserMainWindow — but during ~BrowserMainWindow the object's
dynamic type has already reverted to QWidget, so `data()`, the
`->tabWidget()` member call (extensioneventhub.cpp:1018), and the
`m_tabWidget` member access (browsermainwindow.cpp:3907) are all UB.
The code HAS the safe identity key — two lines later the `closing`
calc compares `tab.windowObject` against `m_dyingWindows`
(extensioneventhub.cpp:1026), and `windowObject` exists for exactly
this ("usable in dead-object compares where QPointer::data()'s
checked cast would be UB", extensioneventhub.h:246-249) — but the
collapsed-group probe dereferences `tab.window` first anyway.
UBSan fired on this path in the browser-smoke, container-smoke and
tabstrip-smoke legs (same three SUMMARY lines each run). In practice
`home->tabWidget()` reads the `m_tabWidget` member of a
partially-destructed window — garbage `TabWidget*` whose
`tabGroupOf` is then called; today it happens not to crash, but it
is a real torn-read. Fix (CA10): test the dying-window membership on
`windowObject` before touching `tab.window` (a dying window's
collapsed-group question is moot anyway).

### L3 — `LocationBar`-destroyed lambda touches a partially-destructed `TabWidget` — **LOW**

`TabWidget::wireLocationBar` connects `bar, &QObject::destroyed` to a
lambda that reads `m_lineEditCompleter` (tabwidget.cpp:1049-1050).
Qt emits child `destroyed` signals while the parent is already
inside `~QObject` (children die before connections are severed), so
when a location bar dies during `TabWidget` teardown the lambda runs
on a `this` whose dynamic type is already QWidget — member access is
UB, and `m_lineEditCompleter` may already be torn down. UBSan fired
in the container-smoke leg (member access on address "of type
QWidget" at tabwidget.cpp:1049:13/36 and :1050:13). Benign in
practice — the guard checks `widget() == bar` on possibly-dead but
mapped memory — but the same class as L1/L2; fix by early-returning
when `this` is being destroyed (e.g. a `m_destroying` flag, or
disconnecting in ~TabWidget before members die).

### Dynamic leg detail (LSan)

13 smoke legs on the ASan+UBSan+LSan build, leak check inside the
quick-exit handler: **28 leak blocks total, 0 allocated in our code**
(all fontconfig/expat upstream leaves). Per-leg:

    leg                                   rc    blocks  ours
    --quit-after-load                      0      2      0
    --app-smoke                            0      2      0
    --browser-smoke                        0      3      0
    --otr-teardown-smoke                   0      3      0
    --container-smoke                      0      3      0
    --tabstrip-smoke                       0      3      0
    --extension-smoke                      0      2      0
    --extension-otr-smoke                  0      2      0
    --cookie-smoke                         0      2      0
    --history-smoke                        0      2      0
    --settings-smoke                     139      —      —  (L1 crash)
    --search-smoke                         0      2      0
    --download-smoke                       0      2      0

Note on scope: `at_quick_exit` fires before the app's own
quick_exit body, so process-lifetime singletons and live app objects
correctly report reachable — the check measures *unreachable at
exit*, i.e. true leaks. Blocks leaked by skipped destructors cannot
exist by construction (quick_exit skips them on purpose — the
EXTEXIT01 trade-off). Logs: /tmp/ca03-leaklogs (not committed).

### Checked, no findings (honest negatives)

- **Unparented `new QObject`** — 141 scanner-flagged sites all
  resolve: detached `WebView`/`BrowserMainWindow`/`SourceViewer`/
  `SettingsDialog` windows carry `WA_DeleteOnClose`
  (webpage.cpp:1404,1508; webview.cpp:772,813,840;
  browserapplication.cpp; browsermainwindow.cpp:350,3816);
  `WebViewWithSearch` reparents its `WebView`
  (webviewsearch.cpp:122); tab teardown `deleteLater`s both the
  location bar and the page widget (tabwidget.cpp:3567,3584;
  hidden-group members 2674/2679); `main.cpp` smoke objects are
  parented to `&application`.
- **Per-profile registries** — `DownloadManager::instanceFor`
  (static QPointer map + `releaseFor` delete, downloadmanager.cpp:1475),
  `NotesModel::instance` (parented to the profile, QPointer row +
  destroyed-connect cleanup, notesmodel.cpp:95-105),
  `ContainerManager::m_profiles` (qApp-parented AND explicitly
  deleted in teardown + take paths, containermanager.cpp:121,219,445),
  `OpenSearchManager::m_engines` (`qDeleteAll` in dtor,
  opensearchmanager.cpp:157) — all bounded, all owned.
- **QNetworkReply** — every `NAM::get()` site parents the reply and
  `deleteLater`s it in the finished handler
  (extensionmanager.cpp:3059-3065,3111; opensearchengine.cpp:570-585;
  opensearchmanager.cpp:370+1339; adblocksubscription.cpp:289-311).
- **Contextless `connect()`** — zero `connect(sender, sig, functor)`
  without a context object in src/; every lambda connection names a
  receiver context.
- **Non-QObject raw `new`** — 22 sites all resolve: `QUndoStack`
  owns pushed bookmark commands (bookmarksmanager.h:146);
  `QIcon(QIconEngine*)`/`QApplication::setStyle` take ownership;
  `BookmarkNode` is a self-owning tree (dtor unlinks + deletes
  children, bookmarknode.cpp:121); delegates/layouts parent to the
  view.
- **QThread workers** — `SyncPairDialog` parents `m_thread`, quits
  it on `destroyed`, deletes the worker on `finished`
  (syncpairdialog.cpp:130-136).
- **QWebEngine seam** — every `QWebEngineUrlRequestInterceptor`,
  `QWebEngineUrlSchemeHandler`, `WebEnginePageAdapter` and
  download-request wrapper is parented to profile/page/request
  (webenginebackend.cpp:391,820,983; schemeaccesshandler.cpp:131-140
  including the `installed`-set destroyed cleanup).
- **Extension page bridges** — `attachExtensionPage` parents the
  `QWebChannel` and `ChromeBridge` to the page and detaches on
  `destroyed` (extensionbackgroundhost.cpp:2008-2038).
- **Rust FFI** — every `rc_*_open`/`ac_*_open` handle has a paired
  close (notesmodel.cpp:82-85, pdfview.cpp:88, pdfworker.cpp:226,
  historymanager.cpp, torcontrol.cpp:85-99, rustcorebridge.cpp:443-733,
  extensionshimbackend.cpp guards); 99 `rc_string_free` + 12
  `ac_string_free` + 25 `rc_buffer_free` sites cover the `char *`
  API surface; `char *` results flow through `*Take` wrappers.
- **Deliberate still-reachable caches** (documented, not bugs):
  `SyncTabPresence` static instance, `recently-closed` value
  snapshots (no live widget refs — tabwidget.cpp:4323+), the
  QWidgetAction default-widget hand-off in profilemenuactions.cpp:220.

## PERFORMANCE (CA04)

Generated 2026-10-29. Method: source review of the startup path (main()
-> BrowserApplication ctor -> newMainWindow -> postLaunch), per-tab
allocation, per-request hot paths (request interceptor, adblock/DNR
matchers, cookie filter), completion path, sync op-log, store
read/write cadence, and repaint/polling loops — cross-checked against
the PERF01/PERF03 work already in tree so only *open* findings are
listed. Dynamic leg: `StartupProfile` timeline via `--profile-startup`
(offscreen, real profile — 3 runs) and wall-time of the existing smoke
legs. Severity by expected impact: **high / med / low**. Findings only
— no code changes.

Measured baseline (offscreen build @HEAD, warm fs cache):

    --profile-startup marks (3 runs):
      ctor: browsing profile + services prepared   94-165 ms
      BrowserApplication ctor done                 152-237 ms
      history load                                  +6-22 ms
      opensearch engines parse                      +2-9 ms
      bookmarks xbel load                          +19-68 ms  (at ~640-806 ms)
      first window shown                            782-954 ms
      postLaunch: begin                           1043-1646 ms
      deferred warmup: adblock rules snapshot     +138-185 ms
      (cold run: NINE further snapshots, see P1)
    smoke legs (wall):  --quit-after-load 1.05-1.36 s
      --app-smoke 1.49 s   --browser-smoke 4.38 s   --history-smoke 1.05 s
      --settings-smoke 1.08 s   --search-smoke 0.55 s

### P1 — `rulesChanged -> rebuildRules` is direct-connected, un-coalesced, and double-fired per list event — **HIGH**

`AdBlockNetwork::rebuildRules()` rebuilds the WHOLE matcher snapshot —
copies every rule of every enabled subscription into `SubscriptionRules`
records and rebuilds four n-gram indexes per subscription
(adblocknetwork.cpp:546-619).  Measured **138-185 ms per call** on the
GUI thread (StartupProfile marks above).  Three coupling defects make
that cost fire far more often than once:

- Both `AdBlockSubscription::rulesChanged` AND
  `AdBlockSubscription::changed` forward to `AdBlockManager::rulesChanged`
  (adblockmanager.cpp:522-523, 719-720), which is direct-connected to
  `rebuildRules` (adblockmanager.cpp:291-292, 308-309).  One finished
  list download emits both: `rulesDownloaded()` calls `loadRules()`
  (emits `rulesChanged`, adblocksubscription.cpp:241) and then
  `emit changed()` (:350) — **two full corpus rebuilds per download**.
- `setEnabled`/`setLocation`/`setTitle`/`addRule`/`removeRule`/
  `setRules` all emit `changed` or `rulesChanged` per mutation
  (adblocksubscription.cpp:131,144,158,431,441,453) — every single-rule
  edit through the dialog or `AdBlockManager::addSiteWhitelistRule`
  (adblockmanager.cpp:845) rebuilds the whole corpus.  `addRule` also
  re-derives all per-subscription caches via `populateCache()`
  (:456-490, O(all rules)) before the rebuild — K adds = K x (O(sub) +
  O(corpus)).
- Measured cold-start impact: the first `--profile-startup` run hit
  NINE extra rebuilds after the deferred warmup — five stale remote
  lists finishing download in pairs (rulesChanged + changed), ~1.5 s of
  GUI-thread CPU between 2281-4345 ms, serialized against the first
  page's layout.  A mid-browse weekly refresh hits the same wall: a
  ~160-330 ms UI freeze per finishing list.

Fix direction (CA11): coalesce — a dirty flag + `QTimer::singleShot(0)`
(or a generation counter checked lazily in `match()`, which already
read-locks `m_lock`) collapses N emissions into one rebuild; disconnect
the metadata-only `changed` signal from the rebuild path; keep
`populateCache` incremental (append/remove single rule) or batch it
behind the same flag.

### P2 — DNR `evaluate()` linear-scans every registered rule per request — **HIGH (conditional)**

`DeclarativeNetRequest::evaluate()` walks `s_rules` — the flat
concatenation of session + dynamic + **enabled static rulesets** across
all extensions (rebuildSnapshot, declarativenetrequest.cpp:1877-1910) —
on every request (declarativenetrequest.cpp:1480).  Per rule it runs
`ruleMatchesRequest` (:1323-1367): regex `urlRe.match(urlStr)` where
present, domain-list suffix scans, type/method checks — plus
`dnrPatternCovers` per host pattern per host-access extension
(:1484-1491) and a `s_exemptLock` mutex on every allowAllRequests
match (:1504/:1513).  All under `QReadLocker(&s_lock)` on the IO
thread, so a `chrome.declarativeNetRequest` update (which takes the
write lock) stalls in-flight matching, and vice versa.

The only gate is `s_ruleCount > 0` (:1455) — one installed extension
with DNR rules turns the linear scan on for **every request on every
profile**.  Chrome's static-ruleset quota is tens of thousands of
rules per extension (kStaticGuaranteedRules), so enabling a
uBlock-scale ruleset degrades the intercept path from O(url length)
to O(total rules) per request — the same class of problem the
adblock network's n-gram index already solved
(adblocknetwork.cpp:228-285).  Fix direction: index candidates by
domain/urlFilter token like `buildIndex`, and hoist per-extension
hostPattern coverage into a per-(ext, first-party) memo.

### P3 — `SiteDecisionStore::lookup` FFI envelope per request in the adblock match — **MED**

`match()` and `matchNativeUnlocked()` call
`siteWhitelistedDocument(documentHost)` on EVERY request
(adblocknetwork.cpp:155, 304) — before touching the rule indexes.
That goes through `SiteDecisionStore::lookup`
(sitedecisionstore.cpp:251-278): a full `mwQuery` JSON envelope
(serialize request -> `ac_mw_dispatch` -> parse response) per request
on the IO thread.  The Rust side itself is an in-memory suffix walk
over a HashMap (sitedecisions.rs:387-410) — the cost is all marshal +
FFI overhead, paid per request even when the whitelist store is
empty.  `sendGpcForProfile` already shows the in-tree fix
(privacyrequestinterceptor.cpp:527-558): cache the answer keyed on
`ac_policy_eval_generation()` (or a sitedec generation), re-querying
at most once per store mutation.  Alternatively fold the whitelist
set into the `rebuildRules` snapshot — it is GUI-side state the
matcher's read lock already guards.

### P4 — `AutoSaver::changeOccurredFor` re-queries the session policy per dirty signal — **MED**

Every `changeOccurred`/`changeOccurredFor` runs `sessionPolicyCall`
(autosaver.cpp:162-205, 102-135): mints an `AcSession` handle (or opens
`rc_settings` outright), marshals a JSON request, FFI round-trip,
parses the reply — for a policy document that only changes when
`session/*` settings are written.  `tabsChanged` fires from ~13 sites
in tabwidget.cpp (:774,854,1180,1218,1403,1504,2501,2685,3591,3796,
3842,4062,4315) — url set, title apply, add/remove/move, strip syncs —
i.e. several FFI round-trips per navigation, plus one per sidebar /
chrome / lifecycle event (browsermainwindow.cpp:706,813,1318,3247+).
The comment at autosaver.cpp:95-101 acknowledges the mint-per-change
trade for settings freshness — but `ac_policy_eval_generation()` gives
freshness without the round-trip (same pattern as P3's fix).  Each
call is small; the count per navigation burst is not.

### P5 — Window-construction is the largest un-decomposed startup bucket — **MED**

`newMainWindow` spans "BrowserApplication ctor" (152-237 ms) to "first
window shown" (782-954 ms): ~600-720 ms, of which **~430-510 ms sits
between the "opensearch engines parse" and "bookmarks xbel load" marks
with no instrumentation** — the `setupMenu()` + `setupToolBar()` +
dock/statusbar wiring region (browsermainwindow.cpp:355-536).  The
tail (newTab at :769 + show) accounts for the last ~140-150 ms —
QWebEngineView/page construction and the async renderer bring-up.
Menus already populate lazily (modelmenu.cpp:202-238), the sidebar
panel defers to first show (:700-703), and the heavy stores are
queued post-show (browserapplication.cpp:846-862) — so the remaining
bulk is widget/action/dock construction and stylesheet application.
Fix direction: `StartupProfile::Scope` marks inside setupMenu /
setupToolBar / dock wiring to decompose the region before optimizing;
deferred construction of the non-visible docks (bidi panel,
download/graph docks) behind first-show.

### P6 — `NetIndicator` sweeps all of /proc every 5 s, per window — **LOW-MED**

`NetIndicator` samples every 1 s (statusbarwidgets.cpp:387-388); every
5th sample `rescanEnginePids()` re-scans **every** /proc entry —
`stat` + `cmdline` per process (:415-450), i.e. ~2 file opens x N
system processes (~hundreds-thousands) per rescan, for the life of the
window.  Each `BrowserMainWindow` owns its own indicator (ctor :337,
statusbar :673) so the sweep multiplies per window, and it runs while
the window is hidden/occluded.  The standalone `engineIoTotals`
(:459-515) repeats the same full sweep without even the pid cache.
Fix: one process-global sampler shared by all windows (the renderer
pid set is process-wide anyway); rescan only when a cached pid dies;
pause the timer when the window is not visible.  `MemIndicator`'s
single `/proc/<pid>/status` read per 2 s (:255-256,321-341) is fine
as-is.

### P7 — `updateHistoryEntry` linear-scans all of history per title update — **LOW-MED**

`HistoryManager::updateHistoryEntry` walks `m_history` front-to-back
comparing `url` per entry (historymanager.cpp:605-621).  Pages set
their title at least once per load (sometimes several times as JS
retitles), so each navigation pays an O(history-size) QString-compare
scan on the GUI thread until the match — misses (OTR-filtered urls,
title-before-add races) scan the whole list; a large history (tens of
thousands of rows) makes every title update a noticeable stall.
`m_atomicStringHash` already interns url strings — a url -> index
side map (rebuilt with the list) turns this O(1).  Related one-time
cost: `importLegacyHistory` / `rustReplaceAll` issue one `histAdd`
envelope round-trip per row (:828-844) — fine for a migration, but a
bulk-insert op would collapse it.

### P8 — Sync op-log: per-op statements + batch-critical sections — **LOW-MED**

`SyncState::append` (rustcore/src/sync.rs:648-691): per op —
`next_lamport` (SELECT + UPSERT into meta, :1253-1262), canonical
serialize, sha256+ed25519 sign, INSERT — ~4 autocommit statements
per local op (WAL+synchronous=NORMAL, so each is its own commit).
`import` (:781-900) holds `state.lock` for the WHOLE batch while doing
per-op: member_pubkey query, member_scope_allows query, ed25519
verify (~us-tens each, serialized under the global sync mutex),
`bump_lamport`, `canon`, `INSERT OR IGNORE` — then `mark_applied`
re-locks and commits a separate UPDATE per applied op (:885-911;
`drain_pending` same).  A first-pair bulk import of N ops is ~4N
transactions + N serialized signature verifies under one lock; LAN
drains hit the same shape.  Fix: one `conn.transaction()` around the
batch, hoist member key/scope reads (or the verifies) out of the
lock, batch the applied-marking.

### P9 — Redundant writes: history save(), AutoSaver shape — **LOW**

`HistoryManager::save()` constructs `ProfileSettings` and rewrites
`history/historyLimit` on EVERY debounced flush
(historymanager.cpp:857-867), though under rustcore the entries write
through per mutation and the limit only changes via `setDaysToExpire`
— a QSettings open+write per flush cycle for a quasi-static key.
Similarly the history `AutoSaver` still arms on every mutation
(:614,656,809) to reach a save() that is a near-no-op under the rust
store — the dirty plumbing outlives its purpose.  Minor per event;
cheap to gate on `m_daysToExpire` actually changing.

### Checked, no findings (honest negatives)

- **Per-request policy verdict** — PERF01's memoization is sound:
  generation-keyed, bounded at 4096, whole-header fingerprint in the
  key, main frames excluded for the consume-once side effects
  (navigationpolicy.cpp:1626-1689+).
- **Completion path** — 150 ms type-debounce (historycompleter.cpp
  :740-757), suggest model bounded at 500 rows per refresh
  (:237-277), suggestions debounced + gated (omniboxsuggestions.cpp);
  no per-keystroke store hits beyond the one debounced query.
- **Extension request observation** — `observeRequest` fast-paths on
  a zero-subscriber atomic before any JSON work
  (extensionrequestwatcher.cpp:243-244).
- **Per-tab allocation** — the eager per-tab objects are a handful of
  small QObjects (ReaderMode + PiP + ScriptBlockInfoBar,
  webview.cpp:250-268); Readability bundle is a static
  (readermode.cpp:276-297); the per-tab `reader-forward.js` read is
  one small qrc file (:232-242).  Lazy-creating ReaderMode/PiP on
  first use would shave this further — LOW value.
- **Menus** — lazy + dirty-flag rebuild (modelmenu.cpp:202-238).
- **Store writes on hot paths** — no per-request `QSettings`/`setValue`
  found in the interceptor/cookie paths; privacy exceptions write on
  user actions only (privacyrequestinterceptor.cpp:816-899); session
  manifest writes ride the debounced AutoSaver with a MAXWAIT cap
  (autosaver.cpp:197-204).
- **AdBlock native matcher** — n-gram token index bounds per-request
  candidates (adblocknetwork.cpp:228-285); the unindexed
  `matchLinearUnlocked` arm is the documented differential reference.
- **MemIndicator** — one small /proc read per 2 s
  (statusbarwidgets.cpp:321-341).

## WINDOWS (CA05)

Generated 2026-10-29. Method: read-only source audit on this Linux
tree — there is no Windows host and no MSVC/MinGW toolchain here, so
every finding is a static-evidence porting gap, not a compiled
diagnostic (WIN01 stays `needs:windows`). Sweeps: POSIX-API inventory
(unistd/sockets/proc/signals vs Q_OS_* guards), path-separator and
drive-letter handling, case-sensitive filename checks,
QSettings/registry assumptions, spawn/command lines, wchar/UTF-8
marshaling at the C/FFI boundary, rename/remove semantics,
reserved-name and illegal-char paths, MSVC-only language constructs,
installer/manifest/CRT packaging, sync-LAN socket+firewall review
(C++ and rustcore), and the smoke/test harness's POSIX dependencies.
Severity by impact on a real Win10/11 port: **high / med / low**.

The tree is far more Windows-aware than a typical Linux-first app —
Q_OS_WIN/Q_OS_LINUX branches already exist in most of the right
places — so the findings below are the residual class, not a flood.
Findings only — no code changes.

### W1 — `QSettings` NativeFormat→registry collides with `fileName()`-as-path and smoke isolation — **HIGH**

The app never pins `QSettings::setDefaultFormat` outside one macOS
smoke block (main.cpp:6022-6029), so on Windows `QSettings()` resolves
to `NativeFormat` = the registry (`HKCU\Software\<org>\<app>`).  Code
in five places treats `QSettings().fileName()` as a filesystem path:

- `browserpaths.cpp:331-345` — legacy-identity settings migration
  moves the ini *file*;
- `profilemenuactions.cpp:405-410` — profile wipe `settings.clear()`
  (works on the registry) then `QFile::remove(fileName)` (no-op);
- `profilesettings.cpp:261-263` — `importLegacyIni(QSettings().
  fileName())` hands a `\\HKEY_...` string to the Rust ini importer;
- `main.cpp:6034-6041` — the smoke-isolation guard resolves
  `fileName()` and refuses to continue when it isn't under the store
  root — **every `--*-smoke` leg aborts on Windows**;
- `main.cpp:6630-6637` — wipe verification reads the file directly; an
  unopenable path yields empty content → the "key is gone" check
  *passes vacuously* (false negative).

Root cause is double: (a) NativeFormat on Windows is a registry
backend, not a file; (b) `QSettings::setPath` does **not** redirect
the native backend on Windows (same limitation the existing macOS
comment documents for CFPreferences — main.cpp:6023-6027).  Fix
direction (CA12/WIN01): call `QSettings::setDefaultFormat(
QSettings::IniFormat)` unconditionally early in `main()` — the
settings code assumes ini semantics everywhere — and extend the
existing macOS smoke guard to `Q_OS_WIN`.  QSettings::setPath then
applies and the smoke store confines everything.

### W2 — NativeMessaging host discovery is filesystem-only; Windows uses the registry — **HIGH**

`NativeMessagingSession::searchDirs()` (extensionbackgroundhost.cpp:
376-390) scans `~/.config/{arktos,chromium,google-chrome}/`
`NativeMessagingHosts` plus `/etc/{arktos,chromium,opt/chrome}` —
Chrome's *Unix* layout.  On Windows, Chrome-family hosts register a
default value under
`HKCU\Software\<vendor>\NativeMessagingHosts\<name>` (and the HKLM
variant) whose data is the manifest path — there is no directory
convention, so `resolve()` returns `not-found` for every host and
`chrome.runtime.connectNative`/`sendNativeMessage` (the whole BW08
surface, extensionbackgroundhost.cpp:624-705) is dead on Windows.
The arktos-side registration path (`userHostsDir()`, used by the
native-messaging smoke at main.cpp:15740) must likewise write the
registry value instead of/in addition to a manifest file.  Fix:
`QSettings("HKEY_CURRENT_USER\\Software\\...", QSettings::
NativeFormat)` (or winreg) lookup feeding the same manifest JSON
parser — the manifest validation itself is platform-neutral.

### W3 — Sync LAN needs a Windows Firewall rule; silent pairing failure otherwise — **MED**

The LAN transport binds a QUIC listener on `0.0.0.0:14431` UDP
(rustcore/src/synctransport/lan.rs:60,466) and the mDNS responder on a
second UDP socket with multicast membership (mdns.rs:385-405).  On
Windows, first bind of an inbound-capable UDP listener raises the
firewall consent dialog — and if the user dismisses it (or it appears
behind a fullscreen window), *inbound* packets stay dropped:
discovery never sees peers and pairing times out with no diagnostic
surfaced to the user.  On networks typed "Public" inbound stays
blocked even after consent in some profiles.  The NSIS installer
(BuildProcess/windowsinstaller.nsi) creates no firewall exception —
add a private-profile inbound UDP rule for arktos.exe at install
(via the NSIS Firewall or SimpleFC plugin / `netsh advfirewall` in a
post step) or at minimum document+surface the pending-prompt state.
Secondary: mdns.rs relies on `SO_REUSEADDR` alone on Windows
(`#[cfg(unix)] set_reuse_port`, mdns.rs:390-391) — Windows permits a
second process to bind the same port under REUSEADDR, so a hostile
or careless local app can squat the mDNS port; SO_EXCLUSIVEADDRUSE
is the tighter knob if coexistence isn't needed.

### W4 — ProfileVault seal/unseal parks the live profile dir via rename — relies on POSIX open-handle semantics — **MED**

The vault flows move the *live* profile directory with
`QDir().rename()`: `live → held` and `dir → park` (profilevault.cpp:
445, 457, 738, 750, 768, 809).  POSIX rename succeeds while files
inside the tree are still open; on Windows **any** open handle in the
tree — a QtWebEngineProcess not yet dead, a sqlite WAL, Windows
Defender/Search-indexer scanning the dir, an AV real-time touch —
fails the whole seal with "cannot move … aside".  The code fails
loudly (correct), but on Windows the transient-lock case is routine,
not exceptional: seal/unseal needs the engine profile verifiably
torndown *and* bounded retry/backoff for AV/indexer locks before
failing.  Related uniform-behavior note: `QFile::rename` refuses to
overwrite on all platforms, and two callers don't check a
pre-existing target or the return — `notesmodel.cpp:152` (legacy
notes rename silently skipped → legacy notes never migrate) and
`opensearchmanager.cpp:933` (`.migrated` retire can collide).  Low,
but same class.

### W5 — Windows filename legality: reserved device names + NTFS-forbidden chars — **MED**

- **Profile dirs**: `ProfileStore::slugify` (profilestore.cpp:121-137)
  and the mirrored `profiles.rs::slugify` (rustcore/src/profiles.rs:
  101-116) emit `[a-z0-9-]{≤64}` — so a display name of "Con", "Nul",
  "Aux", "PRN", "COM1"…"COM9", "LPT1"…"LPT9" slugs to a Windows-
  reserved device name and `mkpath` fails (reserved even with an
  extension — `con.txt` is illegal too).  `isProfileId`/
  `check_name` don't screen them.  Denylist the ~22 stems or suffix
  the slug on Windows.
- **Downloads**: `DownloadItem::sanitizeFileName`
  (downloadmanager.cpp:494-541) strips control chars, trailing dots,
  and `.`/`..` — but not the NTFS-forbidden set `< > : " / \ | ? *`
  and not reserved stems.  `:` is the sharp edge: `report.pdf` is
  fine, but a suggested `invoice.pdf:payload.exe` becomes an NTFS
  **alternate data stream** — content lands on an invisible stream,
  Explorer shows 0-byte/unexpected files.  Any of `?`/`"`/`<`/`>`/`|`
  just fails the open → download dies with a filesystem error rather
  than a clean name.  Filter the set (and reserved stems) under
  `Q_OS_WIN`.
- Trailing spaces: Windows silently trims — the dedup `-N` logic sees
  a different on-disk name than requested.  Low.

### W6 — GUI-subsystem release builds have no console; every `--*-smoke` leg is stdout-driven — **MED**

`src.pri:15` sets `win32:Debug:CONFIG += console` — a Release build
links `/SUBSYSTEM:WINDOWS`, so the ~40 `qInfo`-reporting `--*-smoke`
legs, `--help`, `--version`, and all `qWarning` diagnostics print to
a console that isn't there.  The whole in-tree verification harness
(smokes + their `PASS/FAIL` parsing) is unobservable on Windows
release builds.  Fix options: keep `CONFIG += console` for the test
build profile, or `AttachConsole(ATTACH_PARENT_PROCESS)`/`AllocConsole`
early in `main()` when argv carries a `--*-smoke`/`--help` flag.

### W7 — `file://` assembly + custom-scheme path round-trip on drive letters — **LOW-MED**

- `main.cpp:9863` (search-smoke): `"file://" + fixturePath` — under
  `QDir::temp()` = `C:\…` this produces `file://C:/…` which parses
  `C:` as the URL *host* → the fixture leg fails on Windows.  Use
  `QUrl::fromLocalFile` + `QUrlQuery` for `q={searchTerms}`.
- `FileAccessHandler::urlForLocalPath` (fileaccesshandler.cpp:56-61)
  `setPath`s the raw local path — `arktos-file:C:/foo` round-trips
  through the handler's own `requestUrl().path()` fine, but anything
  emitting `arktos-file:///C:/foo` yields `/C:/foo`, which `QDir`
  resolves against the *current* drive — silent wrong-drive access.
  Normalize leading-slash-on-letter in the handler's path intake.

### W8 — No-rust `SecureStore` fallback needs dlopen'd OpenSSL by SONAME — **LOW (config-gated)**

`securestore.cpp:361-405` resolves libcrypto via
`QLibrary::setFileNameAndVersion("crypto", {3, 1.1, 1.0.2, ""})` —
Unix SONAME probing.  On Windows OpenSSL 3 is `libcrypto-3-x64.dll`
(different naming scheme QLibrary won't generate), and Qt6's default
Windows TLS backend is Schannel so no OpenSSL is bundled —
`evp()==nullptr` → sealed secrets (autofill payload, stored proxy
password, legacy migrate path) silently unopenable and
`enablePassphraseProtection` fails "crypto backend unavailable" —
but **only under `CONFIG-=rustcore`/`no-rust` builds**; the shipped
default uses the aes-gcm crate and is unaffected.  Disposition:
either declare rustcore mandatory on Windows or give the fallback a
BCrypt (CNG) path; don't silently ship no-rust on Windows.

### W9 — `SetForegroundWindow` single-instance raise usually no-ops — **LOW**

browserapplication.cpp:634-663: the second-instance `arktosmessage://
winid` handshake calls `SetForegroundWindow(hwnd)` — modern Windows
foreground-lock (SPI_GETFOREGROUNDLOCKTIMEOUT) refuses it whenever
the launching process isn't the foreground owner, so a second
`arktos.exe url` launch can appear to do nothing.  Standard remedy:
the already-running instance self-grants via
`AllowSetForegroundWindow` or `SetForegroundWindow` after
`AttachThreadInput`/`SwitchToThisWindow` fallback.  Behavior is
cosmetic, not data loss.

### W10 — Raw `<windows.h>` includes lack NOMINMAX/WIN32_LEAN_AND_MEAN — **LOW (latent)**

profilemenuactions.cpp:58, browserapplication.cpp:141,
singleapplication.cpp:43 include `windows.h` bare.  That leaks
`min`/`max` macros and `winsock.h` v1 into the TU — the classic MSVC
breakage when a later include uses `std::min`/`std::max`/
`numeric_limits::max` or `winsock2.h`.  No breakage *today* (those
TUs use neither), but it's a landmine — prefer `<qt_windows.h>` or
add `DEFINES += NOMINMAX WIN32_LEAN_AND_MEAN` to the win32 scope of
src.pri.

### W11 — Case-sensitive filename/extension checks — **LOW**

File-extension tests that ignore case (cross-platform correctness,
most likely *hit* on Windows where `FILE.ZIP`-style names are
common): `extensionmanager.cpp:1215` & `:1726`
(`info.suffix() == "zip"` — a `FOO.ZIP` is refused by the install
flow even though other entries accept case-insensitively, e.g.
`:996`,`:1045`,`:3209`), `bookmarksmanager.cpp:620`
(`endsWith(".html")` — `BOOKMARKS.HTML` misroutes to XbelReader; the
`:694`/`:856` siblings already use CaseInsensitive),
`languagemanager.cpp:338` (`.qm`), `tabwidget.cpp:587` (`.html`),
`adblockresourcehandler.cpp:265-275` (stub suffix map).  Also
`extensionshimbackend.cpp:7847` compares canonicalized paths with
`actual != wanted` — case-aliased spellings of the same file trigger
a pointless rename (harmless).

### W12 — Smoke/test/packaging legs assume POSIX tools and env — **LOW**

Not release code, but "suite green on Windows" needs skips or ports:
`sandbox-smoke` execs bwrap + a `#!/bin/sh` launcher +
`QFile::link(program,…)` (symlink creation needs
SeCreateSymbolicLinkPrivilege or developer mode on Windows);
tor-smoke plants a `#!/bin/sh` fake tor + exec bits and exports
`HOME`/`XDG_*` (all ignored); `--telemetry-smoke`'s raw-socket proxy
is `Q_OS_UNIX`-gated already (skips by design); SLEEP01 `/proc` RSS
counters return -1 → skip by design.  All `.devin/check-*.sh` gates
are bash → Windows CI needs git-bash/WSL or native ports.  And
smoke store isolation: `XDG_{CONFIG,DATA,CACHE}_HOME` redirects are
inert on Windows — `QStandardPaths` follows %APPDATA%; test-mode
`qttest` namespacing (main.cpp:5985) keeps smoke writes out of the
real profile tree but they're **not** confined to the temp store and
not cleaned up.  Fold the QSettings half into W1.

### W13 — Installer is Qt6-era but misses Default-Programs + has a silent-CRT risk — **LOW**

`BuildProcess/windowsinstaller.nsi` is already the windeployqt-era
rewrite (recursive `deploy\` payload, MultiUser HKLM/HKCU modes, no
Qt4 file list — the WIN01 "rewrite" comment is already satisfied).
Gaps: (a) no `HKLM\SOFTWARE\Clients\StartMenuInternet\Arktos` +
`RegisteredApplications` capabilities → Windows' "default browser"
setting can never list Arktos; (b) the App Paths key lacks the
`Path` value; (c) `File /nonfatal "${VCREDIST}\*.dll"` (:96-103)
silently ships a bundle missing vcruntime/msvcp when the caller
forgets `/DVCREDIST` — the exe then fails at launch on a clean
machine with the useless "missing VCRUNTIME140.dll" dialog; consider
warning at makensis time or shipping the `vc_redist.x64.exe` and
running it on install; (d) no GPL license page (cosmetic).  The
windeployqt note (:87-91) is correct — Qt 6 windeployqt
intentionally does not stage the CRT.

### W14 — No `longPathAware` manifest → >260-char paths fail in Rust fs and spawned tools — **LOW**

No application manifest ships (browser.rc carries only the icon;
MSVC qmake specs auto-embed supportedOS GUIDs but not
longPathAware).  Qt's own file APIs prefix `\\?\` internally, but
Rust `std::fs` (rustcore stores under the profile dir — deep
%LOCALAPPDATA% + long leaf names can exceed MAX_PATH) and any
spawned child process hit the 260-char cap unless the manifest
declares `longPathAware` *and* the host enables
`LongPathsEnabled`.  Add the manifest element in WIN01 (it is a
no-op where the policy is off) — via a `.manifest` file +
`RC_FILE`/`mt.exe` or `QMAKE_MANIFEST`.  DPI is honest-negative:
Qt 6 sets Per-Monitor-V2 itself at startup.

### W15 — synctor (arti-client) Windows build is unverified — **LOW**

`rustcore` features `synctor` pulls arti-client 0.47 + tokio stack —
arti's Windows support exists but is the least-exercised target
(fs-mistrust's permission checks degrade to no-ops, arti's state-dir
permission enforcement is Unix-shaped); `synclan`'s
quinn/socket2/rustls-native-certs stack is Windows-capable
(native-certs reads the Windows cert store — good).  Nothing here
*blocks* WIN01 but `cargo check --target
x86_64-pc-windows-msvc` for both crates is the cheap first gate
before the qmake port; if synctor fails to build, ship Windows with
`synclan` only and mark tor-sync `needs:arti-windows`.

### Honest negatives (verified clean — do not re-audit)

- **POSIX includes**: every `unistd.h`/`sys/socket.h`/`arpa/inet.h`/
  `execinfo.h`/`poll.h`/`netdb.h` include site is `Q_OS_UNIX`-gated
  (main.cpp:228-241, tormanager.cpp:36, profilemenuactions.cpp:53-58,
  extensionshimbackend.cpp:82, sandboxmanager.cpp:32,
  rustdownloadengine.cpp:58, crashreporter.cpp:26,
  terminationsignalhandler.cpp:22, profilevault.cpp:39,
  singleapplication.cpp:40).  Signal handling, `/proc` readers,
  `geteuid`, `kill(0)`, `fsync`, `QFile::link` and the raw-socket
  telemetry proxy are all inside those guards with graceful
  degradation on the `#else` side.
- **`/proc` consumers** all degrade: NetIndicator/MemIndicator
  (statusbarwidgets.cpp:395-599 `#else` zeros), SLEEP01 counters
  (-1 → caller skips, main.cpp:3658-3663), extension `system.cpu`/
  `system.memory` (empty/-1 shapes, extensionshimbackend.cpp:
  5700-5868), `Page::processMemoryKb` (-1, engineinterface.cpp:35).
- **Spawn surface**: all runtime launches are argv lists —
  `startDetached(exe, args)` everywhere; no `system()`/`sh -c`/string
  command lines in runtime paths (downloadmanager.cpp:1710-1754,
  browserapplication.cpp:2226-2245, profilemenuactions.cpp:484).
  Reveal-in-folder has the correct `explorer.exe /select` +
  `toNativeSeparators` branch (downloadmanager.cpp:929-936).
- **FFI path marshaling**: every path into rustcore leaves as
  `.toUtf8().constData()` and lands in `Path::new(&str)` (Rust
  converts UTF-8→UTF-16 for Win32) — Unicode paths are safe end to
  end.  The lone `fopen` in rustcore/tools/acprobe.c:231 is a dev
  tool, ANSI-codepage-bound on MSVC — non-ASCII args only.
- **Rename discipline**: the overwrite-capable cases remove or
  existence-check the destination first (historymanager.cpp:917-920,
  autofillmanager.cpp:496-498, bookmarksmanager.cpp:333-336,
  extensionshimbackend.cpp:7847-7852); history's QTemporaryFile
  deliberately lives in the target dir (cross-device note at
  historymanager.cpp:887-890).
- **Toolchain/build wiring already Windows-aware**: `.qmake.conf`
  resolves `%USERPROFILE%\.cargo\bin\cargo.exe`; `rustcore.pri`
  win32-scope `.lib` + `advapi32 bcrypt ntdll userenv ws2_32`;
  `src.pri` win32 `RC_FILE`+`user32 advapi32`; `tor` resolves
  `tor.exe` (tormanager.cpp:96-108); `SingleApplication` uses a
  named-pipe `serverName` with `GetUserNameW` (singleapplication.cpp:
  166-205); `languagemanager` covers `appDir/locale` (:74-76);
  `installedDataDirectory()` falls back to `applicationDirPath()`
  (browserapplication.cpp:1950-1974); `updatechecker` prefers the
  `*Installer*.exe` asset (:582-585); download reveal uses
  `explorer /select`.
- **Rust portability**: all Unix-only Rust is `#[cfg(unix)]` with
  documented fallbacks (mdns `set_reuse_port`, localtime_r day math,
  0o600/0o700 modes in store/bookmarkhtml/tor/sync).  No
  `std::os::unix` escapes a cfg gate; rusqlite is `bundled`
  (MSVC-clang cc path works); `argon2id.c` is plain C99
  (stdint/UINT64_C, no extensions) — MSVC-clean.
- **Path separators**: Qt paths are joined with `/` everywhere —
  Windows APIs accept it; no `split('/')` on *native* filesystem
  paths found (all the `'/'`-splits are URL/extension-resource
  paths by design).  `QUrl::fromUserInput`/`toLocalFile` handle
  drive letters correctly at every call site checked.
- **`QFile::setPermissions(0600)` caveat, not bug**: on Windows it
  maps to the read-only attribute — the real guard on
  `securestore.key`/vault blobs is the %LOCALAPPDATA% per-user ACL
  (same model as every browser on Windows); DPAPI would be the
  platform-idiomatic extra layer but nothing is *broken*.
- **argv→URL**: drive-letter/bare inputs route through
  `fromUserInput`/`guessUrlFromString` which treats `C:\…` and
  `C:/…` as local files before host-guessing (tabwidget.cpp:
  4578-4598's address-shape check only governs *search*, and
  `QDir::isAbsolutePath` catches them earlier).

## MACOS (CA06)

Generated 2026-10-29. Method: read-only source audit on this Linux
tree — there is no macOS host or SDK here, so every finding is a
static-evidence porting gap, not an on-target diagnostic (MAC01 stays
`needs:macos`; the MAC02–17 dmg fixes are all still "blind fixes —
runtime verify owed", which constrains how strongly the recurring
classes below can be stated).  Sweeps: every Q_OS_MACOS/Q_OS_MAC block
in src/, .app-bundle path probes (applicationDirPath-relative),
Retina/DPR in pixmap and measure paths, unified-NSToolbar item
visibility (the MAC03/14 class), style-metric-dependent sizing (the
MAC04/10/13 class), Info_mac.plist key inventory vs LaunchServices/
TCC requirements, hardened-runtime/notarization constraints on every
spawned helper (tor, native-messaging hosts, self-respawns,
QtWebEngineProcess, servo cdylib), shortcut/menu-role mapping, and the
fetch/install scripts' POSIX assumptions.  Severity by impact on a
real macOS 13+ port: **high / med / low**.

The tree is already far more macOS-aware than a straight Qt4 port —
unified toolbar, menu-role discipline, lifecycle (no quit-on-last-
window + activate-respawn), `open -R` reveal, Mach VM stats, per-
platform autoscroll default — so the findings are the residual class,
plus three instances proving the MAC04 "style-painted check
indicator" class is NOT closed (one was reintroduced after the fix).
Findings only — no code changes.

### M1 — NativeMessaging host discovery uses the Linux directory layout — **HIGH**

`NativeMessagingSession::searchDirs()` (extensionbackgroundhost.cpp:
376-390) builds on `QStandardPaths::GenericConfigLocation` — which on
macOS resolves to `~/Library/Preferences`, not `~/.config` — plus
`/etc/{arktos,chromium,opt/chrome}` which simply do not exist there.
Chrome-family browsers on macOS register hosts under
`~/Library/Application Support/{Google/Chrome,Chromium}/
NativeMessagingHosts` (user scope) and `/Library/{Google/Chrome,
Chromium}/NativeMessagingHosts` (system scope).  Result: every host a
Chrome/Chromium-targeted app installs is unresolvable —
`resolve()` returns `not-found` and `chrome.runtime.connectNative` /
`sendNativeMessage` (extensionbackgroundhost.cpp:624+) is dead on
macOS, same end-state as W2 on Windows but with a filesystem fix, not
a registry one.  `userHostsDir()` (line 369-374) also lands at
`~/Library/Preferences/arktos/NativeMessagingHosts` — functional but
wrong-family; AppConfigLocation (`~/Library/Application Support/
Arktos`) is the idiomatic root.  Fix direction: `#ifdef Q_OS_MACOS`
dir list using `~/Library/Application Support/{arktos,chromium,
google-chrome}` + `/Library/...` roots, same manifest parser.

### M2 — Info_mac.plist: no URL-scheme registration, no version keys — **HIGH**

`BuildProcess/Info_mac.plist` carries icon/identifier/executable/
NSPrincipalClass/NSHighResolutionCapable/LSMinimumSystemVersion and
one document type — but:

- **No `CFBundleURLTypes`** for http/https.  LaunchServices decides
  default-browser candidates from the declared schemes, so Arktos can
  never appear in the default-web-browser picker or receive
  `open -a`/link activations.  `QDesktopServices::setUrlHandler`
  (browserapplication.cpp:328) only routes URLs opened *from inside*
  the app — the external direction has no plumbing at all.
- **No `CFBundleShortVersionString` / `CFBundleVersion` /
  `CFBundleName` / `CFBundleInfoDictionaryVersion`.**  A custom
  QMAKE_INFO_PLIST is copied verbatim with only @ICON@/@EXECUTABLE@
  substitution — qmake injects nothing — so the dmg shows no version
  in Finder/Get Info and any future notarization/Sparkle check has
  nothing to compare.
- Document types use only the legacy `CFBundleTypeOSTypes`/`Viewer`
  shape — no `LSItemContentTypes` (public.html UTI) and no
  `LSHandlerRank`, which is what modern macOS consults for the
  "Open With" ranking.
- Conditional-low: no `NSCameraUsageDescription` /
  `NSMicrophoneUsageDescription`.  Today this is *safe* — SEC05's
  WebPermissionManager hard-denies media before Chromium asks TCC —
  but it is a trap: the first future code path that lets a getUserMedia
  request reach AVFoundation gets the process SIGKILLed (not denied —
  killed) for a missing usage string.  Either add the keys with honest
  strings or pin a comment in the plist saying media is intentionally
  un-grantable.

### M3 — Style-painted check indicators: the MAC04/10 class is NOT closed — **MED-HIGH**

MAC04 (extensions list) and MAC10 (checkable QGroupBoxes) converted
their controls to real widgets because the style-painted check path
(sheet → ChromeProxyStyle → QMacStyle → PE_Indicator*Check) was the
one unverifiable-on-Linux render step failing on the dmg.  Three live
instances of the identical class remain — including one introduced
*after* the fix, which is how the class recurs:

- `adblock/adblockdialog.cpp:290-293` — subscription rows are
  `ItemIsUserCheckable` tree items; enable/disable rides
  `setCheckState(0, …)` on the painted indicator.
- `settings.cpp:553-556` — `sidebarPanelsList` checkable
  QListWidgetItems gate every sidebar panel's visibility.
- `opensearch/opensearchenginemodel.cpp:92,149-171` — the SEC11
  per-engine "Suggestions" column (column 2, CheckStateRole +
  `ItemIsUserCheckable`) — added after MAC04, proving the defect
  family keeps re-entering through new features.

If the MAC04 hypothesis holds on target all three show invisible or
inert toggles.  The CA12 fix should be systemic — one shared
index-widget-checkbox helper (the MAC04 ExtensionEnableBoxDelegate
pattern generalized) applied to every CheckStateRole-bearing view —
not three more per-site conversions.

### M4 — fetch-tor.sh broken on macOS twice; `/opt/homebrew/bin` unsearched — **MED**

- `BuildProcess/fetch-tor.sh:43` uses `sha256sum`, which does not
  exist on macOS (`shasum -a 256` is the BSD tool) — the script dies
  at the checksum step on the very platform that supports it
  (`Darwin` is mapped at line 17).
- Its default DEST is `${XDG_DATA_HOME:-~/.local/share}/Arktos/
  Arktos/tor`, but `QStandardPaths::AppDataLocation` on macOS is
  `~/Library/Application Support/Arktos/Arktos` — so even a
  successful fetch installs tor where `TorManager::binarySearchPaths`
  (tormanager.cpp:65-88) never looks.  Tor mode on macOS is then
  silent-dead unless ARKTOS_TOR_PREFIX is hand-set.
- `binarySearchPaths` enumerates /usr/bin, /usr/local/bin, /usr/sbin
  and the bundle/data dirs — but a Finder-launched .app gets the
  minimal launchd PATH, so `findExecutable` covers nothing extra, and
  `/opt/homebrew/bin` (the Apple-Silicon Homebrew prefix; /usr/local
  covers Intel only) is absent — a Homebrew `tor` on an M-series Mac
  is invisible.  Also note the .app ships no bundled tor at all: the
  only QMAKE_BUNDLE_DATA entries (src.pro:15-24) are the Bitwarden
  files, so `Contents/{Resources,lib}/tor` candidates can never hit.

### M5 — `QSettings()` resolves to CFPreferences on macOS; the fileName()-as-path uses misfire — **MED**

Sibling of W1, different failure shape: `QSettings::setDefaultFormat`
is forced to IniFormat only inside the smoke-isolation block
(main.cpp:6022-6029), so a normal macOS run writes the app-level
`QSettings()` keys into the CFPreferences domain
(`~/Library/Preferences/com.arktos-browser.Arktos.plist`) — while
ProfileSettings already pins IniFormat (profilesettings.cpp:181) and
the codebase broadly assumes ini semantics.  The places that treat
`QSettings().fileName()` as an owned file misbehave there:

- `browserpaths.cpp:331-345` — identity migration moves the plist,
  racing cfprefsd's cached copy (the moved content can be dropped or
  clobbered back).
- `profilemenuactions.cpp:405-410` — profile wipe `clear()` +
  `QFile::remove(fileName)` — removing the plist under a live
  cfprefsd lets the daemon resurrect cached keys (wipe can undo
  itself).
- `profilesettings.cpp:261-263` — `importLegacyIni(QSettings().
  fileName())` hands a binary/XML plist to the Rust ini importer —
  parses nothing; legacy keys silently never fold.
- `main.cpp:6630-6637` — wipe verification reads the plist file
  directly; cfprefsd write-back timing makes the "key is gone" check
  unreliable.

Fix: force `QSettings::setDefaultFormat(QSettings::IniFormat)`
unconditionally early in `main()` on macOS too — same recommendation
as W1 for Windows — so every plain `QSettings()` lands on the ini
backend the surrounding code was written against.

### M6 — Hardened-runtime / notarization constraints on spawned helpers — **MED**

No `.entitlements` file, signing script, or notarization note exists
anywhere in the tree — the whole recipe is owed to MAC01.  Specific
spawned-helper surfaces that break or silently no-op once the app is
hardened-runtime signed:

- **tor child gets `DYLD_FALLBACK_LIBRARY_PATH`** (tormanager.cpp:
  760-767, the TOR17 fix).  dyld strips `DYLD_*` for a
  hardened-runtime target lacking
  `com.apple.security.cs.allow-dyld-environment-variables` — if the
  shipped tor binary is signed that way the fix no-ops and tor dies at
  exec exactly as before.  Verify the expert-bundle tor's entitlements
  on target; the honest fallback is `@rpath`/`install_name_tool` on
  the shipped binary, not env.
- **`libservo_embed.dylib` is dlopen'd** (servobackend.cpp:1602+).
  In-process dlopen of a foreign-team or unsigned dylib needs
  `com.apple.security.cs.disable-library-validation` *on the arktos
  binary* — a notarized build without it loads nothing and the servo
  backend silently vanishes.
- **QtWebEngineProcess** under hardened runtime needs the Chromium JIT
  entitlements (`allow-unsigned-executable-memory` /
  `disable-executable-memory-protection` depending on macOS version);
  macdeployqt does not add these automatically — the entitlements
  file must exist for MAC01's sign step.
- **Native-messaging hosts** spawn via plain `QProcess::start`
  (extensionbackgroundhost.cpp:483+) which survives hardened runtime
  (exec is not library-validated), but a quarantined host binary
  (e.g., inside a user-unzipped download) fails at exec with a
  Gatekeeper denial surfaced only as `spawn-failed` — worth one line
  of user-facing guidance.
- Self-respawns (`applicationFilePath()` — pdfview.cpp:192/481,
  browserapplication.cpp:2243 tor windows, profilemenuactions.cpp:484
  relaunch, plus the ~8 smoke children in main.cpp) are the same
  signed binary — safe under Gatekeeper — but a detached sibling never
  registers with LaunchServices: `open`-document events and Dock
  routing always land on the bundle's primary instance.  Fine for
  worker roles; for the tor *window* it means the second process has
  no LaunchServices identity (low).

### M7 — "Hide Menu Bar" is meaningless on macOS and its ⌘M collides with Minimize — **MED**

`applyMenuBarVisibility` (browsermainwindow.cpp:3255-3267) calls
`menuBar()->setVisible(m_menuBarVisible)` on non-tor windows — on
macOS the QMenuBar IS the global system menubar, so the toggle either
empties the app's menus for every window or no-ops entirely; there is
no per-window menu row to hide (the tor branch's removeAction walk is
the only honest variant, and it would still blank the shared bar).
Its shortcut `Ctrl+M` (browsermainwindow.cpp:1297) maps to ⌘M —
macOS's own Minimize accelerator — so the menu item shadows/conflicts
with window minimize.  Gate the action off on Q_OS_MACOS like the
icon discipline already does, or rebind to a non-reserved key.

### M8 — Nav-bar drag reorder runs on Qt-side geometry under a native NSToolbar — **MED-LOW**

`applyNavBarOrder`, `commitNavBarDrag` and the drop-indicator path
(browsermainwindow.cpp:2860-3070) drive the custom reorder through
`QToolBar::actionGeometry()` + `removeAction`/`addAction` reinsert.
Under `setUnifiedTitleAndToolBarOnMac` the items are native
NSToolbarItems — actionGeometry reflects the hidden Qt layout, not
the native bar, so drop geometry can be meaningless; and re-adding a
QWidgetAction has a long Qt-history of detaching the embedded widget
from its native item view (the reader-button/tor-badge/splitter/
profile items are all widget items).  Same "Qt-side bookkeeping vs
native bar" family as MAC03/14.  Needs on-target verify; if broken,
reorder should rebuild the bar's item list wholesale on macOS or be
disabled there.

### M9 — Translations, useragents.xml and the rest of the payload never enter the .app — **MED-LOW**

`src/src.pro:42` gates the whole INSTALLS payload (translations/*.qm,
useragents.xml, icons, man, sandbox launcher) behind `!mac`;
QMAKE_BUNDLE_DATA ships only the four Bitwarden entries.  So the dmg
has no compiled translations at all — and LanguageManager
(languagemanager.cpp:67-78) probes `appDir/src/.qm/locale`,
`appDir/../share/arktos/locale` (→ `Contents/share/…`, the wrong
root) and `appDir/locale`, but never `Contents/Resources/share/
arktos/locale` — so even a hand-bundled locale dir wouldn't be found.
useragents.xml has the compiled-in qrc fallback (useragentmenu.cpp:
76-80) so it degrades gracefully; translations degrade to
English-only.  Fix: QMAKE_BUNDLE_DATA (or macdeployqt install-name
step) for `.qm/locale` + useragents.xml under Contents/Resources/
share/arktos, plus the Resources probe in LanguageManager.

### M10 — 19px hard cap on the macOS bookmarks toolbar — **LOW**

`m_bookmarksToolbar->setMaximumHeight(19)` (browsermainwindow.cpp:
568) is a Qt4-era metric clamp — 19 DIP against current qmacstyle
item metrics + 16px icons clips bookmark text at normal font sizes.
This is the MAC13 under-measure class in reverse (the widget is
force-shrunk rather than the style under-measuring) and the
`w->setMaximumHeight(0)` spacer beside it (line 571) is a documented
OS X style hack with no remaining justification recorded.

### M11 — Retina stragglers in composite pixmaps — **LOW**

`privateBadgedPixmap` (tabwidget.cpp:4260-4272): the null-icon
fallback `QPixmap(16,16)` carries no devicePixelRatio, and both
`icon.pixmap(16,16)` and the `:graphics/private.png` badge are
requested without a paint-device DPR context — on Retina the
composite renders at 1x (the device-pixel math itself is consistent,
so position is right; sharpness is what's lost).  Same class,
smaller: `fileaccesshandler.cpp:121-129` favicon composites for
file:// listings, `tabwidget.cpp:1484/2543` favicon labels
`.pixmap(16,16)` (non-mac paths anyway).  All cosmetic.

### M12 — macOS shortcut/feature gaps — **LOW**

- "Show All History" has NO binding on macOS — `history.cpp:381-383`
  compiles the Ctrl+H out with no substitute (Safari convention is
  ⌘Y / ⌘⇧H).
- `Qt::Key_F11` fullscreen (browsermainwindow.cpp:1449) — F11 is
  Mission Control's Show-Desktop default and ⌃⌘F is the platform
  fullscreen convention; the action should carry both on macOS.
- `sandbox-exec` seatbelt apply (sandboxmanager.cpp:205-243) is opt-in
  with an explicit UNTESTED warning — Apple deprecated the interface
  years ago but still ships it; fine as recorded, noted for
  completeness.

### Honest negatives — checked, not findings

- **All `Qt::ControlModifier` / `Ctrl+…` shortcuts are CORRECT** —
  Qt maps `Qt::CTRL` to Command on macOS automatically; the ~25
  hardcoded bindings (Quit ⌘Q, Close Window, zoom, bookmarks,
  downloads, view-source) resolve properly.  Only the ⌘M conflict
  (M7) and the mac-missing bindings (M12) are real.
- Menu roles: `PreferencesRole` explicit (browsermainwindow.cpp:
  1189); About/Quit relocate to the app menu via TextHeuristicRole
  ("About %1" / "&Quit" match the heuristics).  `setIconVisibleInMenu
  (false)` applied consistently on mac.
- Lifecycle: `setQuitOnLastWindowClosed(false)` + ApplicationActivate
  respawn (browserapplication.cpp:323,1673+), unified-title toggled
  off in fullscreen (3789), `setWindowIcon(QIcon())`, tor-window title
  without app-name duplication — all correct conventions.
- Bundle path probes: `installedDataDirectory()` checks Resources
  then Contents/share (browserapplication.cpp:1954-1965);
  `bundledExtensionsPath()` probes both roots + dev layouts
  (extensionmanager.cpp:969-986); servo dylib probes Frameworks/+lib
  (servobackend.cpp:1615-1621).  The remaining probe gap is M9's
  locale dir.
- `/usr/bin/open -R` for reveal-in-Finder (downloadmanager.cpp:938+);
  Mach `hw.memsize`+HOST_VM_INFO64 memory stats (extensionshimbackend
  .cpp:5726+); `chrome.runtime.getPlatformInfo` reports "mac"
  (9120-9131); fetch-tor.sh does have a Darwin branch.
- MAC02–17 fixes hold up as the right shape on re-read — action-vs-
  widget visibility for the reader button, real-widget checkboxes,
  doc-fit measurement, popup anchor lock, tab close-side styleHint,
  first-show size bounce, completer rebinding.  The residual
  instances of each class are M3/M7/M8/M10 above.
- `privateBadgedPixmap` device-pixel geometry is internally
  consistent (only the 1x fallback is soft — M11); pdfview renders at
  zoom × devicePixelRatioF; ArktosIcon/searchbutton/sidebarpanel/
  tortitlebar all resolve painter DPR correctly.

## PLATFORM REMEDIATION (CA12)

Generated 2026-10-29. Dispositions for the CA05 WINDOWS and CA06
MACOS findings — all fixes are static (no Windows/macOS host here);
runtime verification stays owed to WIN01/MAC01.

### Windows (W*)

- W1 FIXED: `QSettings::setDefaultFormat(IniFormat)` is now pinned
  unconditionally in main() (all platforms — ini semantics are what
  the codebase assumes; registry/CFPreferences honor no setPath).
  The macOS-only smoke-guard workaround was removed; the smoke-store
  setPath redirect + guard now works identically everywhere.
- W2 FIXED: `nativeHostManifestRegistryPath()` looks hosts up in
  HKCU/HKLM `Software\{Arktos,Chromium,Google\Chrome}\
  NativeMessagingHosts` registry default values after the filesystem
  dirs; our own userHostsDir() stays a dir for local installs/smokes.
- W3 FIXED (installer half): the .nsi adds a private-profile inbound
  UDP rule (program-scoped, ports 14431+5353) at install and deletes
  it at uninstall.  mdns REUSEADDR kept deliberately (documented —
  EXCLUSIVEADDRUSE would bar a legitimate coexistent responder).
- W4 FIXED: profilevault dir moves go through renameTree() —
  bounded ~2.75 s backoff retry on Windows for transient AV/indexer/
  engine-handle locks.  notesmodel legacy-store rename gained a
  copy+remove fallback; opensearch .migrated retire clears a stale
  target and warns loudly instead of silently skipping.
- W5 FIXED: reserved device stems (con/nul/aux/prn/com1-9/lpt1-9)
  screened in BrowserPaths::isReservedDeviceStem — isProfileId +
  ProfileRegistry::isValidProfileId + profiles.rs::is_profile_id all
  refuse them, both slugify impls suffix "-profile".  Download
  sanitizeFileName + dlwpolicy.rs sanitize_name map the NTFS-
  forbidden set to '_', strip trailing spaces, and prefix reserved
  stems — all under Q_OS_WIN / cfg!(windows).
- W6 FIXED: --*-smoke/--help/--version argv triggers
  AttachConsole(ATTACH_PARENT_PROCESS) (+AllocConsole fallback and
  CONIN$/CONOUT$ rebinds) at the top of main().
- W7 FIXED: search-smoke file:// URL built via QUrl::fromLocalFile +
  QUrlQuery; arktos-file handler strips the "/" prefix off
  "/C:/…" intake on Windows.
- W8 FIXED (policy): rustcore.pri errors out on win32 without
  rustcore — the SONAME-probing Qt fallback has no crypto backend
  there; a BCrypt port is the sanctioned alternative.
- W9 FIXED: second instance grants ASFW_ANY before
  SetForegroundWindow; on failure it asks the running instance to
  self-raise via the arktosmessage://raise handshake.
- W10 FIXED: src.pri win32 scope adds DEFINES += NOMINMAX
  WIN32_LEAN_AND_MEAN.
- W11 FIXED: zip/html/qm suffix checks and the adblock stub suffix
  map are case-insensitive; the downloads fixup path compare is
  case-insensitive on Windows.
- W12 NOTED-DEFERRED: the POSIX-shaped smoke/test harness (bwrap,
  #!/bin/sh fixtures, XDG env redirects) can only be ported/skipped
  on a Windows host — WIN01 scope.
- W13 FIXED: StartMenuInternet capabilities + RegisteredApplications,
  App Paths "Path" value, makensis-time !warning when VCREDIST is
  absent, GPL2 license page.
- W14 FIXED: BuildProcess/arktos.manifest declares longPathAware;
  wired via QMAKE_MANIFEST in src.pri's win32 scope.
- W15 NOTED-DEFERRED: needs an x86_64-pc-windows-msvc cargo check on
  a Windows-capable host — WIN01 scope.

### macOS (M*)

- M1 FIXED: searchDirs() resolves the macOS layout — ~/{Library/
  Application Support}/{arktos,Chromium,Google/Chrome} +
  /Library/{Application Support/arktos,Chromium,Google/Chrome};
  userHostsDir() re-rooted at GenericDataLocation.
- M2 FIXED: Info_mac.plist declares CFBundleURLTypes (http/https),
  CFBundleName/DisplayName/ShortVersionString/Version/
  InfoDictionaryVersion/NSHumanReadableCopyright, LSItemContentTypes
  (public.html) + LSHandlerRank, and records the deliberate absence
  of camera/mic usage strings (SEC05 hard-deny — a future media path
  must declare BOTH first or die at SIGKILL).
- M3 FIXED SYSTEMIC: utils/indexcheckbox.h — IndexCheckBoxDelegate
  (the MAC11 paint-suppression lesson) + installIndexCheckBox()
  (CheckStateRole mirrored both ways, selection-palette cue,
  dataChanged sync) now backs EVERY CheckStateRole-bearing view:
  extensionsTree (MAC04 refactored onto the shared helper), adblock
  subscriptionsTree, sidebarPanelsList (reinstall walk after
  takeItem/insertItem reorders), and the OpenSearch Suggestions
  column (reinstall on modelReset/rowsInserted).
- M4 FIXED: fetch-tor.sh uses shasum -a 256 when sha256sum is absent
  and installs under ~/Library/Application Support on Darwin;
  TorManager probes /opt/homebrew/bin and Contents/Resources/tor.
- M5 FIXED: same as W1 — IniFormat is now global; the CFPreferences
  failure modes listed here no longer apply.
- M6 FIXED (recipe): BuildProcess/arktos.entitlements carries the
  hardened-runtime keys each shipped path needs (unsigned-executable-
  memory for the WebEngine JIT, dyld-env for the tor spawn,
  disable-library-validation for the servo cdylib) with the
  deliberately-absent set documented; the MAC01 sign step consumes it.
- M7 FIXED: the Hide/Show-Menu-Bar action is never added or bound on
  macOS (no per-window menubar exists; ⌘M is Minimize).
- M8 GATED: nav-bar drag edit mode refuses to engage on macOS —
  actionGeometry is meaningless under native NSToolbarItems and
  widget-action re-add can detach embedded widgets; programmatic
  order apply is unaffected.  On-target verify owed to MAC01.
- M9 FIXED: src.pro loops TRANSLATIONS into QMAKE_BUNDLE_DATA under
  Contents/Resources/share/arktos/locale + ships useragents.xml;
  LanguageManager probes the Resources path on macOS.
- M10 FIXED: the Qt4-era 19px bookmarks-toolbar cap and the
  undocumented 0px spacer hack are removed — the bar sizes itself.
- M11 FIXED: privateBadgedPixmap + its callers fetch pixmaps at the
  paint device's devicePixelRatioF; the transparent fallback carries
  the DPR too.
- M12 FIXED: Show All History binds ⌘Y on macOS; fullscreen carries
  ⌃⌘F alongside F11.

---

## CA13 CLOSE-OUT — detector re-run + final dispositions

CA01's scanners re-run on the post-CA12 tree:

| Detector | CA01 | CA13 | Verdict |
|----------|------|------|---------|
| Dead-symbol scan (member/signal/field) | 13 candidates | 10 | D1's three members gone; remainder is the D9 verified-live set (Q_INVOKABLE/override/webchannel-invoked) |
| Whole-class / free-function / #ifdef / unref-file scans | 0 actionable | 0 actionable | unchanged — same documented live categories |
| Dead includes | 5 fixed + 5 live | 2 new dead (autosaver.cpp/tabwidget.cpp `browserpaths.h` — CA08/CA12 refactor leftovers, removed) | fixed; 16 other candidates are scanner artifacts (inline/multi-line decls the ident regex misses — chromecommon/sitebinding_p/modeldnd_p/twoleveldomains_p/downloadworker all live) |
| Clone groups (8-line normalized windows) | **127** | **65** | all confirmed families consolidated; remainder documented below |
| `clang -Wunused-lambda-capture` | 47 | ~3 GCC-required keepers (blobSize×2, serveDelayMs — documented D6) | applied class stays clean |
| cargo clippy dead_code (rustcore/rustdl/adblock-rust) | 1 (SRCS_TRANCO) | **0** | clean |

### Dead-code dispositions (D1–D9)

- **D1** (3 dead members) — FIXED in CA07.
- **D2** (2 "dead" consts) — WONTFIX-as-audit-FP: live under the `no-rust` lane; annotated in CA07.
- **D3** (`#if 0` cookie-path arm) — FIXED in CA07 (deleted; Firefox-parity rationale already in prose).
- **D4** (~50 `#ifdef ARKTOS_*` unreachable tails) — WONTFIX by design: they are the `no-rust` build lanes; live in that flavor, unreachable only under the shipped config. Deleting them deletes the fallback build.
- **D5** (dead includes) — FIXED (CA07×4 + CA13×2); webpage.h in webviewsearch.cpp is a verified false positive (complete type needed).
- **D6** (47 lambda captures) — FIXED 44; 3 remain deliberately — GCC odr-uses what clang calls unneeded (documented).
- **D7** (SRCS_TRANCO + FFI parity consts) — FIXED (const deleted); FFI `AC_*` consts WONTFIX — ABI parity with facade.rs, documented.
- **D8** (stores/classes/signals/files) — nothing found, stays nothing found.
- **D9** — verified-live list re-confirmed on re-scan.

### Duplicate dispositions (A/B/C)

- **A1–A20** — all 20 cross-file families FIXED in CA08 (shared homes: hostmatch/menufit/chromecommon/sitebinding_p/workerio/remotelistfetch/modeldnd_p/policyrequestmarshal/twoleveldomains_p + member helpers).
- **A-carry: cookie record JSON** — FIXED in CA13: cookiejar.cpp `rustCookieJson` ≡ extensionshimbackend `cookieRecordJson` merged into the new shared header `network/cookiejar/cookierecord.h`.
- **B within-file** — FIXED in CA08+CA13: applyRefererVerdict, dialogParentWindow, readerBarStyleSheet, detachedPrivateView, GlyphCanvas, showRestoredCard, drawShiftedBandTab, finishTabInsert/detachHiddenTab, fanOutEvent + 13 ok/err pairs, emitPortDisconnects, hostShimSource, listenOnLoopback (CA08); then CA13: `emitUnsupported`/`emitNotLoaded` (extensionmanager ×8 sites), `lightFontForColumn`, `keptHtmlTags` (readersanitizer — the foreign-exclusion extras br/img/etc. were unreachable entries and dropped), `sendDomPropsFallback`, `engineChildProcs`, `oseEngineRecord` + `restoreEnginePrefs`, `writeUrlElement`, `windowForSessionSlot`, `sessionPages` + `orderedWebViews`-via-`orderedSessionPages` (tabwidget), and the extensionshimbackend vocabulary (`cookiesHandle`, `historyHandle`, `ffiTake`, `urlSchemeToken`, `windowIdQueryMatch`, `tabIdsArg`, `windowTargetFor`, `cookieTarget`, `findExtensionRecord`, `appendManifestHostPerms`/`addUniqueHostPerm`, `groupTarget` lambda, `bmMutableNode`).
- **B smoke-harness class (main.cpp)** — PARTIAL: CA13 extracted `smokeFinish` (3 verbatim legs), `smokeRespond` (2 defs + all call sites), `smokeServer` (3 legs), `smokePollUntil` (3 legs, bounds 200/240/400). The remaining ~25 main.cpp clone groups are leg-specific *variants* — the serve/poll/report skeleton recurs but always with different fixtures, captures, and verdict logic; further consolidation means parameterizing real behavior, not dedup. Documented WONTFIX-beyond-vocabulary.
- **C excluded families** — WONTFIX as designed: webenginebackend/servobackend header conformance (same interface, not copy-paste), securestore + torcontrol `#if/#else` config lanes (only one arm compiles per build), adblocknetwork match*Unlocked differential pair (autotests assert identical decisions), domainblocklist/trancolist preambles (shared include vocabulary), cookiemodel/cookieexceptionsmodel FontRole arm (single-expression arm), managementWarnings' QStringList apiPerms/hostPerms collector (different container type than appendManifestHostPerms), servobackend.cpp 1079-1129 leg pair, webpage.cpp cert-interstitial arms, sitepanel.cpp small pair — all verified live variants or interface-required.
- **Re-run residue** — of the 65 remaining groups: ~30 are the documented smoke-variant class, ~15 the C exclusions, ~10 shim handler shapes whose shared prefix is now 1-3 lines post-extraction (below the useful threshold), rest are new small cross-file pairs (profileregistry↔browserpaths avatar-name sanitize 9-10 lines — deferred, needs a deliberate home under rustcorebridge like A1 got).

### Security dispositions (CA02, S1–S8)

All 8 FIXED in CA09 (subframe confinement, pdf-worker pin+cap, session cert whitelist, DNR redirect re-gate + file/ftp refusal, nativeMessaging anchoring/stdio/abs-path + inbox bound, header-value validation, randomBytes tail, drainStdout bound). Carried-forward D1 resolved — `foreignCertOverrideHosts` removed; a future foreign proceed path must reintroduce the writer (WK19 comment).

### Leak dispositions (CA03, L1–L3)

All 3 FIXED in CA10 (+1 same-class `mainWindows()` downcast found on re-verification). ASan/LSan re-run clean — see CA03's leg table; the settings-smoke atexit report's 4 teardown-orphan blocks are the methodology-excluded statics class.

### Performance dispositions (CA04, P1–P9)

- **P1–P7, P9** — FIXED in CA11, measured: rebuild coalescing (~750 ms/pair → 1 pass), DNR index (~9.8 ms → ~2.4 µs/eval), whitelist-in-snapshot (2 FFI → 0), policy memoization (1 → 0 envelopes/signal), ctor instrumentation marks, shared /proc sweep, url-membership hash, conditional settings write.
- **P8** (sync oplog batching) — DEFERRED: bounded-scope cut; rust-side batching needs its own design pass.

### Platform dispositions (CA05/CA06, W*/M*)

All 15 Windows + 12 macOS findings FIXED in CA12 (see PLATFORM REMEDIATION above) except **W12** (POSIX-only smoke harness) and **W15** (arti windows cargo check) — DEFERRED, both need a Windows host.

### Bottom line

Dead code: fully remediated — every confirmed finding is fixed, a documented config-lane keeper, or an annotated false positive. Duplication: 127 → 65 clone groups, every *confirmed* family consolidated; the residue is the documented variant/exclusion set plus one deferred 10-line pair. Security/leaks/perf/platform: every finding dispositioned fixed or deferred-with-reason.
