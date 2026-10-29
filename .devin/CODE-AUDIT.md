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
| `MaximumKeywords`, `MaximumKeywordLength` | src/opensearch/opensearchreader.cpp:84-85 | `clang -Wunused-const-variable`; bounds now live in rustcore (comment at :82 says the constants exist to "match" the rust store, but nothing reads them) |
| `kHeaderSize` | src/securestore.cpp:65 | `clang -Wunused-const-variable`; nonce/tag sizes are used individually (:63-64), the combined constant isn't |

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
| src/webviewsearch.cpp:24 | `webpage.h` | file references only `WebView`/`pdfview.h`; no `WebPage` ident |
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
