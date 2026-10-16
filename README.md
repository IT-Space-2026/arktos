# Arora — a Qt 6 / QtWebEngine web browser (experimental)

**Arora** is an experimental cross-platform web browser built on **Qt 6
and QtWebEngine** (Chromium). It continues the 2009 Qt/WebKit Arora as a
maintained fork: the original WebKit stack is fully replaced by
WebEngine, and the shell has been rebuilt around privacy hardening,
containers, Tor support and a modernized UI.

**Status: experimental.** Arora is a work in progress and an
experimental tool — it is provided **as is, without warranty of any
kind**, under the terms of the **GNU General Public License v2 or
later** (see COPYING). It is not yet a daily-driver replacement: expect
rough edges, unfinished features and occasional crashes, and do not
trust it with anything you cannot afford to lose.

## Table of contents

- [Feature tour](#feature-tour)
  - [Privacy & security](#privacy--security)
  - [Tabs, chrome & UI](#tabs-chrome--ui)
  - [Search](#search)
  - [AdBlock](#adblock)
  - [Containers](#containers)
  - [Tor](#tor)
  - [Downloads](#downloads)
  - [Extensions](#extensions)
  - [Credentials & autofill](#credentials--autofill)
- [Differences from Qt 4 Arora](#differences-from-qt-4-arora)
- [Source structure](#source-structure)
- [Building](#building)
- [Testing & diagnostics](#testing--diagnostics)
- [Packaging](#packaging)
- [Third-party components](#third-party-components)
- [License](#license)

## Feature tour

### Privacy & security

The application layer is hardened around a request interceptor on the
browsing profile:

- **HTTPS-First / HTTPS-Only** — public http: navigations upgrade to
  https automatically; in HTTPS-Only mode a plain-HTTP destination is
  refused and swapped for a warning interstitial (with per-site
  exceptions and "proceed anyway"). Downgraded hosts are remembered per
  session only after a real TLS failure — aborted or blocked loads
  never poison a site.
- **Third-party cookie blocking**, plus a "Connections & Storage"
  group on the Privacy page: referrer levels, prefetch blocking,
  remote-font blocking, tracking ping/beacon blocking, third-party
  WebSocket blocking (opt-in) and per-origin storage grants.
- **Site trust (opt-out)** — a "Site Trust" group on the Privacy page
  holds the master switch for the toolbar trust indicator plus
  per-signal toggles for the lookalike-domain warning and the
  threat-list check; disabling the master hides the badge and the
  popover's Trust section and skips the lookups entirely, and each
  signal can be skipped individually, all without a restart. The
  lock/shield popover carries a Trust section with one line per real
  signal from the verdict — known/little-known site (Tranco rank),
  "resembles <domain>" lookalike hits, threat-list hits and first
  visits — refreshed per navigation. A small padlock inside the left
  end of the address bar renders the verdict at a glance (green /
  amber / red, locked on https and unlocked on http), while the
  per-site privacy shield sits on the toolbar just left of the bar
  and always reflects the active tab. On Tor windows the
  second-opinion TLS probe cannot run (a direct handshake would
  bypass Tor), so the engine's own end-to-end chain validation stands
  in for it; anywhere, a page committed via "proceed anyway" on a
  certificate error shows red for the rest of the session. The group also has an "Update
  Now" button that refreshes both remote trust lists (Tranco ranks,
  threat blocklist) on demand, with a last-updated status line; it
  stays disabled until remote list downloads are consented to.
- **Per-site JavaScript control** — allow/block rules by host with a
  shield-panel toggle, a blocked-scripts bar with "Allow once"/"Always
  allow", and three Mullvad-style security tiers (Standard / Safer /
  Safest) for script and media handling on insecure pages.
- **Fingerprint resistance** — normalization (vanilla-Chrome user
  agent + client hints, UTC timezone, generic Accept-Language) plus an
  injected layer that is on by default (Settings > Privacy, "Spoof
  canvas/WebGL fingerprints"; always on in Tor windows): canvas
  readouts get per-session noise, WebGL reports a generic
  vendor/renderer and navigator.hardwareConcurrency/deviceMemory/
  plugins are uniform. Honest caveat: this is noise, not anonymity —
  sites can detect the spoofing and canvas-heavy pages can break, so
  the shield panel has a per-site "Spoof fingerprints on this site"
  switch that exempts a misbehaving site on reload.
- **Zero telemetry, verified** — launch *and* real browsing sessions
  produce no unsolicited connections; Chromium's background networking,
  component updates, domain reliability, metrics and sync are disabled,
  and a capture-proxy smoke proves it (idle launch plus a browsing run
  covering navigation, search and a download). DNS prefetch is off by
  default — linked hostnames are no longer leaked to the resolver.
- **Consent gates** — external protocol launches (mailto:, magnet:,
  custom schemes) ask first; adblock-list downloads are opt-in;
  command-line URLs are treated as untrusted input.
- **TLS hardening** — weak cipher suites removed from the ClientHello,
  a proper certificate-error interstitial, and verified WebRTC/DNS leak
  behavior.
- **Encrypted credential storage** — AES-256-GCM store with optional
  Argon2id master passphrase; a Rust implementation (rustcore) is the
  default backend — the crates are default-on, see Build flags —
  byte-compatible with the C++ fallback used in no-rust builds.
- **Tracking-parameter stripping** — ClearURLs-style rules clean
  top-level navigations: global + domain-scoped tracking params
  (vendored rules + data-dir override; rustcore backend, C++
  fallback) and redirect unwrapping that follows known redirector
  links (google /url, facebook l.php, DDG /l, safelinks…) straight
  to the real destination. Per-site opt-out + a "N trackers removed"
  count on the shield panel; master toggle in Preferences → Privacy.
- **Anti-phishing/malware blocklist** — a vendored domain seed merged
  with a consent-gated URLhaus/OpenPhish feed; listed main-frame
  navigations stop at a warning interstitial with a session-only
  proceed.
- **Tranco site-rank table** — a vendored top-100k seed merged with a
  consent-gated top-1M download; the merge is poison-safe (an update
  can only add entries, never un-rank or re-rank a seed domain).
  `rc_tranco_rank` backs the site-trust signals; lookups never leave
  the machine.
- **IDN/homograph lookalike probe** — `rc_lookalike_check` consults
  the rank table: the host is punycode-decoded, case-folded and mapped
  through the UTS #39 confusables table, then matched against a
  skeleton index of the rank table — it flags only when the visual
  twin is a *different* popular domain.
- **Site-trust composite verdict** — `rc_sitetrust_lookup` folds
  blocklist membership, Tranco rank, the lookalike verdict, https
  state and a first-seen signal from the history store into one
  advisory green/amber/red verdict with per-signal reporting — a
  lock-icon badge input that never blocks a page.
- **Rust parsers for untrusted input** — OpenSearch descriptors,
  extension update manifests and suggestion replies are parsed in
  memory-safe Rust behind the FFI before Qt sees them.
- **Rust bookmark + history stores** — in default (rust-enabled)
  builds the crate is also the canonical bookmark + history store:
  bookmarks.xbel stays the on-disk format but is parsed and emitted
  in Rust (queried lazily, one node handle per call — no bulk tree
  marshal), and history lives in a SQLite history.db (visits plus
  per-host favicons that persist across restarts) that imports the
  legacy QDataStream file on first run. The crate also owns session
  save/restore serialization: a versioned session.dat written
  atomically and bounds-checked on decode, so a corrupt file is
  rejected whole instead of looping the crash-restore prompt — and a
  restore that still yields zero windows (empty, corrupt or declined)
  falls back to a normal window so the app never runs windowless;
  per-tab records carry url, container binding, tab group and an
  engine tag beside an opaque engine-state blob, keeping the format
  engine-neutral (a pre-Rust QSettings session still restores once
  through the legacy reader). The crate also owns
  the omnibox: the location bar's url-or-search routing decision
  (rc_classify_input — a ported QUrl::fromUserInput heuristic plus
  keyword/search-fallback handling, verdict diffed against the old
  C++ routing in autotests/omnibox) and the frecency-ranked history
  suggestions the completion dropdown shows (rc_history_suggest
  queries history.db directly so only the visible rows cross the
  FFI). The autofill form store follows the credentials into the
  same encrypted custody: saved field name/value records
  (including typed passwords and card numbers) live in a dedicated
  sealed autofill-store.dat under the shared unlock/passphrase
  lifecycle, with a one-shot import of the old autofill.dat —
  sealed or plaintext — on first run. The crate also carries the
  second-opinion TLS verifier: on https page commits a worker
  performs a real handshake to the host and re-evaluates the
  certificate chain with rustls+webpki against the platform
  roots — the site panel shows a "TLS verified by Arora" chip,
  or a warning line naming the failure class when the
  independent verdict disagrees. The check only informs the
  panel (it never blocks a page), skips Tor windows, proxied
  sessions and private/loopback hosts entirely, and does no
  OCSP/CRL fetch — an unreachable host reports "not verified",
  never a false alarm.
- **Hardened parsers** — OpenSearch descriptions are size-capped and
  DTD-free, download file names are fully sanitized, and page-controlled
  strings cannot inject markup into chrome.
- **In-browser PDFs, sanitized before view** — PDFs open in a
  shell-owned document view (Preferences → Privacy → PDF Documents):
  pages are rasterized by a confined `arora --pdf-worker` subprocess
  running the pure-Rust `hayro` renderer under a bubblewrap allowlist
  (staged file read-only, one writable work dir, no network, no
  $HOME) — the same widget every engine backend hosts, so the view
  doesn't depend on WebEngine's embedded PDFium. The worker speaks a
  single-purpose JSONL protocol and the Qt view provides
  scroll/zoom/page-nav/find-in-document/print handoff. In
  rust-enabled builds the bytes are first fetched through the page's
  own profile (cookies, Tor SOCKS routing and container isolation
  ride along) and rewritten by a Rust policy pass that strips
  JavaScript, /Launch and auto-actions, embedded files and
  XFA/AcroForm submit remnants before the renderer ever sees them;
  embedded links to local files are refused. Chromium's built-in
  PDFium viewer remains an opt-in Settings fallback. Saving a PDF to
  disk still stores the original, unsanitized bytes — sanitization
  applies to in-browser viewing only.
- **Filesystem sandbox** — browsing sessions re-exec inside a
  permissive bubblewrap sandbox on Linux: credential stores (`~/.ssh`,
  `~/.gnupg`, keyrings, CLI/cloud config) and other browsers' profile
  data are masked (a tmpfs over directories, a `/dev/null` bind over
  files), `/etc` `/usr` `/boot` are remounted read-only, and the rest
  of the filesystem — profile and `~/Downloads` included — stays
  writable. One declarative `SandboxPolicy` drives pure per-platform
  generators: the bwrap argv + `arora-sandbox` launcher script for
  Linux, a Seatbelt profile for macOS, an AppContainer manifest for
  Windows, and an unveil/pledge sequence for OpenBSD (those apply
  paths are compiled but untested off-Linux; FreeBSD has no Capsicum
  apply path yet and warns accordingly). Escape hatches:
  `arora --no-sandbox`, `ARORA_NO_SANDBOX=1`, or `sandbox/enabled=false`
  in settings. `arora --sandbox-status` prints the backend and the
  live policy; a missing sandbox tool warns once and continues
  unsandboxed rather than failing.

### Tabs, chrome & UI

- **Tab bar positions** — Top/Bottom/Left/Right, live-applied per
  window and picked from clickable preview tiles in Preferences.
  A Vivaldi-style **'+' button** sits right after the last tab in
  every orientation (an appended row on vertical rails).
- **Tab groups** — named, color-coded groups on the strip.
- **Sidebar dock** — optional Vivaldi-style side panel (bookmarks,
  history, downloads, notes), lazily built and off by default; the
  bookmarks rail doubles as the manager — "Show All Bookmarks"
  (Ctrl+Shift+B) opens it with Add/Remove right on the page.  The
  notes panel manages multiple named notes (list, editor, autosave);
  notes persist per browsing profile — private/Tor sessions keep
  them in memory only.
- **Tab search** — Ctrl+Shift+A fuzzy dropdown over all open tabs,
  with hover previews.
- **Sleeping tabs** — suspend tabs to reclaim memory without losing
  their place; the tab context menu also offers "Free Memory" (a
  discard+reload flush on the current tab, plain discard on
  background tabs) and "Free Memory in Background Tabs", reporting
  the measured freed figure on the status bar.
- **Per-tab engine switcher** — an engine button in the navigation
  toolbar, immediately right of the URL bar, names the active tab's
  engine and offers "Reload in …" on the other backends; Servo
  (`qmake servo=1` + the servo-embed artifact) is an experimental
  degraded co-engine — Tor windows stay Chromium-locked and private
  tabs refuse the swap, and its selector entries carry a note that
  page loads are slower than Chromium (running a debug servo-embed
  artifact marks the name "(debug build)" and warns about extreme
  slowness instead of passing off as engine speed). libservo
  supports one web view at a time, so only one Servo tab can exist;
  further swaps are refused instead of crashing. A crash inside the
  engine degrades that tab to a crash notice instead of taking the
  browser down, and the notice offers "Try Servo Again" — the
  crashed web view is torn down and a fresh one is created on the
  still-running engine (switching the tab to Chromium and back
  recovers the same way). Branded start pages follow the tab's
  engine: a new tab with Servo as the default engine lands on the
  Servo-branded start page, and a start page carried through an
  engine swap (or reached via the Home button) re-resolves to the
  destination engine's own variant. Servo renders in-process, so its
  status-bar memory readout shows the shared whole-app figure
  marked "(shared)" rather than a per-tab number.
- **Reader mode** (Ctrl+Alt+R) and **Picture-in-Picture** on videos.
- **Command palette** — Ctrl+Shift+P fuzzy search over browser
  commands.
- **Sharing** — "Copy Clean Link" strips tracking parameters;
  "Show QR" renders a link as a QR code.
- **Preferences** — opens as a tab (one per window), Vivaldi-style
  vertical sidebar navigation with a filter field that finds settings
  by page title and control labels, scrollable pages capped to the
  screen so buttons never clip, dedicated Search and Containers pages.
  General page has an inline Language selector (System default /
  installed translations / English) that applies immediately.
- **History manager** — "Show All History" (Ctrl+H) opens in a tab
  instead of a floating window; entries open in new tabs.
- **Look & feel** — Theme selector (System default / Light / Dark,
  `ARORA_COLOR_SCHEME` override), bundled icon sets
  (Adwaita/Breeze/Tabler/Lucide, light+dark variants — Tabler is the default
  so every menu glyph ships in the bundle), flat palette-aware chrome with a consistent
  spacing rhythm, status-bar load
  indicator, zoom control and per-tab memory/bandwidth readouts
  (Linux), domain-emphasized anti-phishing address
  bar with IDN punycode display.

### Search

- **Omnibox** — address-shaped input navigates, everything else
  searches the default engine (DuckDuckGo by default).
- **Engine management** — a dedicated Search settings page with an
  inline engine editor (name, nickname, search/suggest/image URLs, POST
  params), per-context engine choices (default / private-window /
  image search) and shortcut nicknames (@bookmarks, @history, @tabs).
- **Bundled engines** — DuckDuckGo (default), Google, Bing, Qwant,
  Startpage, Ecosia, Yahoo, Wikipedia, YouTube, Reddit and Google
  I'm Feeling Lucky, each with its own icon and a default keyword
  nickname where declared ("b arora" searches Bing, "sp" Startpage,
  "q" Qwant, "e" Ecosia); nicknames you rebind or remove stay yours.
- **Suggestions opt-in per engine** — nothing is sent to a suggest
  endpoint unless the user enables it for that engine.

### AdBlock

- **uBlock-Origin-tier rule engine** — resource types,
  third-party/domain restrictions, $important/$badfilter/$removeparam,
  $redirect to bundled stubs, cosmetic extended selectors and a
  scriptlet subset.
- **Brave adblock-rust engine** — compiled in by default (qmake runs
  cargo; opt out with `CONFIG-=adblock_rust` or `CONFIG+=no-rust`).
  With the engine compiled in it is the default matcher. Preferences
  → Privacy → Content Blocking lets you switch between Brave
  adblock-rust and the built-in matcher at runtime (no restart);
  builds without the engine behave exactly like the built-in matcher.
- **Subscription-centric UI** — preset filter-list catalog, per-list
  enable/update, custom rules editor, toolbar blocker button with
  per-page counts and per-site toggle.

### Containers

Firefox-style isolated containers: each container is a separate
WebEngine profile with its own cookies, storage and cache. Tabs bind
to a container (new tabs inherit their opener's), a Settings page
manages them, sites can be pinned to always open in a given container,
and the isolation boundary is audited end to end.

### Tor

File → "New Tor Window" (or `arora --tor`) opens a Tor-browsing window
backed by a managed tor daemon (bundled or system): SOCKS5 egress
only, a dedicated off-the-record profile, .onion reachability, and
Tor-aware rules (no direct-connection fallbacks, no extension loads,
no suggestions, no dedicated search box — the omnibox covers it, no
local DNS resolver: names resolve remotely through SOCKS5 and a
configured DNS-over-HTTPS mode is ignored).
The status bar shows the live circuit's hop chain
(guard -> middle -> exit, each hop tagged with its country code,
fingerprints on hover), and a small onion button next to the
reader-mode icon opens a dropdown of the same circuit — one row per
hop with country flag and name, fingerprints and OR addresses on
hover. Its "New Tor circuit for this site" action sends SIGNAL
NEWNYM and reloads the site's tab, so the page's traffic lands on a
fresh circuit and the displayed chain rotates; requests inside tor's
rate limit report the wait instead of pretending to rotate, and the
button explains itself when the daemon cannot take one. New tabs
land on a Tor-branded start page: the usual Arora
header and search box on a purple background with the official Tor
onion centered — bundled in the binary, so the page never touches the
network; a configured custom homepage still takes precedence.
Navigations typed while the daemon is still starting are held rather
than dropped: the window waits for the SOCKS listener and the first
stream-ridable circuit (with a status-bar note explaining the wait),
and a page that still races a cold circuit retries itself inside a
bounded warm-up window before showing an honest error.

### Downloads

The download manager drives QWebEngineDownloadRequest: progress, speed
and ETA per item, Try Again (re-issues through the page or a hidden
page), reveal-in-file-manager (with the file selected where the DE
supports it) and open-with actions, server-suggested names sanitized
and de-duplicated, and an external download-program handoff. Downloads
surface in the sidebar's Downloads panel (Ctrl+Y or Tools →
Downloads) — filterable, sortable, each selected row expands into a
detail card in the panel's bottom pane, and a compact action row
offers pause/resume, retry, remove and show-in-folder for the
selected download; a header button clears all finished entries
(in-progress downloads and files on disk are kept).
Preferences → Downloads hosts
a download-engine selector — Built-in (Chromium) or Accelerated (Rust,
compiled in by default and the default engine — a saved Built-in
pick is honored) — a connections-per-download
field, a cleanup policy for the finished-downloads list, and a
"Parallel segments" toggle (default on) that has the built-in
Chromium engine fetch range-capable downloads over multiple
concurrent byte-range requests; it takes effect at restart. A
Rust reqwest-based engine (rustdl, compiled in by default) accelerates
HTTP(S) downloads as parallel ranged GETs and holds the same privacy
guarantees as the built-in engine: every request and each redirect hop
is vetted by the same interceptor pipeline (HTTPS-first upgrade,
HTTPS-Only veto, tracking-parameter strip, domain blocklist, adblock
rules, public-to-private redirect refusal), cookies and Authorization
never ride a cross-domain hop, the profile's User-Agent and Referer
policy apply, the application proxy is honored, and inside a tor
window a download exits only via the managed SOCKS listener —
otherwise it falls back to the built-in engine rather than ever
opening a direct connection.  Authenticated downloads are fed by a
scoped cookie export: only rows matching the target URL are written
to a private 0600 file under the temp root (itself 0700), fsynced
before the engine starts and deleted when the download ends;
in-flight part/staging files stay 0600, stale dl-*/cookies-* debris
is swept when the engine arms, and debug output redacts URL queries,
fragments and userinfo.

On Linux the accelerated engine runs inside `arora
--download-worker`, a confined subprocess under a restrictive
bubblewrap wrap: the mount namespace exposes only the download's
work dir and the destination directory — `$HOME` and the profile
are never mounted — so a compromised downloader cannot touch
credentials, keys or cookies.  The browser keeps answering the
policy gate over a stdin/stdout protocol, making the sandboxed path
policy-identical to in-process; a failed wrap falls back to
in-process.  Disable via `ARORA_DL_NO_SANDBOX=1` or
`downloadmanager/sandboxedWorker=false`.

Rust supply chain: `make check-supplychain` (also a `make check`
prerequisite) runs cargo deny + cargo audit over every crate's
committed Cargo.lock — RustSec advisories, yanked releases, the
license allow-list and the crates.io-only source policy in each
crate's deny.toml all fail the build.  Bumping a dependency is
`cargo update` in the crate directory, re-run the gate, commit
Cargo.toml and Cargo.lock together.  Machines without
cargo-deny/cargo-audit skip the gate cleanly.

### Extensions

Chrome Manifest-V3 extensions via Qt WebEngine's tech-preview API:
install always stops at a permission-review screen with an extra
per-permission consent gate on the high-risk set (native messaging,
debugger, blocking webRequest, cookies, browsing data, all-URLs host
access), and extensions are verified not to run in private or Tor
windows. `update_url` checks are supported — fetches are HTTPS-only,
downloaded packages are hash/structure-verified before they touch
disk, and applying an update is opt-in: nothing replaces installed
extension code without a fresh review. CRX3 signature blocks are
structure-checked only; there is no Chrome Web Store signature
verification. Each install records an integrity pin — a SHA-256 over
the extension's on-disk files — re-verified at every startup: files
changed outside the engine mark the row **Tampered** rather than
loading silently. Update reviews call out permissions and host
patterns the installed version never declared, and whole-scheme host
grants (`<all_urls>`, `*://*/*`, `http://*/*` …) get a warning row of
their own. Each enabled extension is also health-checked once per
session: a hidden extension-origin page pings its background service
worker and counts live background contexts, and the result is folded
together with the manifest's declared-but-missing `chrome.*` APIs
into the Settings → Extensions status — **Limited** when the worker
runs but some declared APIs cannot be served, **Not supported** when
the background worker never became ready — with the reason shown in
the details pane instead of a naked "Enabled". Clicking a
Not-supported extension's toolbar action opens a short explanation
rather than a popup that spins on a dead worker. The page lists each
extension's manifest description and, for installed ones, the
on-disk folder — installed extensions live under the browsing
profile's `QtWebEngine/arora/Extensions/` directory (inside the
Arora data location, e.g. `~/.local/share/Arora/Arora/` on Linux)
and can be removed with the page's Remove button or by deleting
their folder while Arora is closed.

Arora bundles the **Bitwarden password-manager extension** (official
Chrome build, `src/data/extensions/` — provenance and checksums in
`PROVENANCE.txt` there). On the first run of a profile that has no
extensions yet it is installed and enabled by default; on a profile
that already carries extensions it is installed but left disabled —
flip it on from Settings → Extensions. It installs exactly once:
uninstalling is never undone, and it is never loaded into private
or Tor profiles. The bundled build cannot self-update (no
`update_url`); updates arrive with Arora releases. Qt WebEngine's
extension API is a tech preview — measured on 6.12: MV3 service
workers, content scripts, action popups, `storage`, `alarms`,
`idle`, `offscreen`, `i18n`, observe-only `webRequest` and runtime
messaging all work, but `chrome.extension`, `tabs.getCurrent/query`
and tab events, `webNavigation`, `scripting`, `contextMenus`,
`notifications`, `windows`, `action` and `declarativeNetRequest`
are not implemented. Bitwarden's background worker needs several of
the missing namespaces, fails registration
(`kErrorScriptEvaluateFailed`), and its popup dies on
`chrome.extension.getBackgroundPage` — so natively it cannot
function; the health check above therefore reports it **Not
supported** with the missing APIs listed. See
`.devin/EXT11-report.md` for the full matrix. Under the hosted
runtime below, though, the Bitwarden popup runs its real app —
extension pages (popup, options, extension links in tabs) get the
same shim injection and chromeBridge channel as the background, and
the popup reaches its genuine vault login/unlock gate. Actual
sign-in needs a real Bitwarden account; a few surfaces it touches
(`chrome.tabs.sendMessage` to content scripts,
`chrome.notifications`) answer typed errors that show up in its
Settings → Extensions health row instead of failing silently.

Enabled extensions that declare a background script additionally get
a hosted background context: a hidden page on the extension's own
profile at its real `chrome-extension://` origin runs a generated
bootstrap (shim preamble, then the real background scripts in
manifest order — the installed package is never modified). This
works around Qt WebEngine's MV3 service-worker subset, which cannot
run most background bundles. The hosted page keeps real
`chrome.runtime`/`chrome.storage`, is kept answerable under
background throttling, and bridges to the shell over a dedicated
`chromeBridge` WebChannel — calls into chrome.* surfaces that are
not implemented yet answer a typed error that shows up in the
extension's health state on the Settings page rather than silently
no-op'ing. Set `ARORA_EXTHOST_OFF=1` to disable hosted backgrounds.

### Credentials & autofill

Saved-password prompts, per-site "never" rules and a form-passwords
dialog; an injected script fills stored credentials after page load
and reports submits over a hardened WebChannel bridge. The store is
encrypted at rest and can be master-passphrase protected.

## Differences from Qt 4 Arora

The 2009 codebase was rebuilt on Qt 6 / QtWebEngine (Chromium). What
that swap means in practice:

- **Engine** — QtWebKit is gone; page content renders in
  out-of-process Chromium renderers. Everything that used QWebFrame's
  synchronous DOM API now goes through injected scripts and
  asynchronous `runJavaScript` — there is no synchronous JS bridge.
- **Plugins removed** — the NPAPI/Flash machinery (QWebPluginFactory,
  ClickToFlash) was deleted outright; modern Chromium has no plugin
  API to port. The remaining "Enable Plugins" setting maps to
  WebEngine's own `PluginsEnabled`.
- **Autofill reimplemented** — WebKit's frame-level form hooks have no
  WebEngine equivalent, so form detection, fill and capture are done
  by an injected script reporting over a token-gated WebChannel
  bridge that lives in a script world page code cannot enter.
  Honest deltas: only JS-observable submits are
  captured (the app cannot see POST bodies), iframe forms are not
  captured, and a submit racing an immediate navigation can be missed.
  The store moved from plaintext `autofill.dat` to AES-256-GCM sealed
  storage with an optional Argon2id master passphrase.
- **Caching** — page loads cache inside Chromium; the app's
  QNetworkAccessManager (with its own disk cache) only handles
  app-side fetches such as search suggestions and blocklist downloads.
- **Inspector** — WebKit's built-in inspector became a DevTools host
  window (Tools → Development Tools → Chromium Dev Tool): the
  inspected page hands its `devToolsPage` to a second
  QWebEngineView on the same profile.  A second, engine-neutral
  panel (Tools → BiDi Dev Tools; rust-enabled builds) talks
  WebDriver-BiDi-shaped commands to the engine — console log + JS
  eval, DOM tree, network request list, cookies/localStorage — over
  the loopback-only, Origin-token-gated remote-debugging socket
  (never a bare port; off in Tor windows and via
  `devtools/bidiBackend=0`).
- **file:// directory listings** — WebKit rendered them natively; they
  are now served by a registered `arora-file:` URL scheme handler.
- **QtScript gone** — OpenSearch suggestions parse JSON natively;
  user-script, autofill and fingerprint injection use
  QWebEngineScript. Qt5Compat remains only as a migration bridge.
- **Self-contained installs** — `make bundle` produces a relocatable
  directory shipping the Qt/WebEngine runtime, so Arora runs on a
  system with no Qt installed (see [Packaging](#packaging)).
- **Privacy-first direction** — everything in the
  [feature tour](#feature-tour) is new since the Qt 4 series:
  zero-telemetry launch, HTTPS-Only, per-site JS/cookie/permission
  controls, containers, a managed-Tor window and an opt-in filesystem
  sandbox.

## Source structure

```
src/                  application sources (flat layout + subdirs)
  browserapplication/browsermainwindow — app object, windows, menus
  webpage, webview, tabwidget, tabbar — the browsing stack
  adblock/            rule engine + optional adblock-rust FFI crate
  network/            request interceptor, proxy, NAM
  tor/                managed tor daemon, control, SOCKS5 plumbing
  containers/         container manager + profile isolation
  opensearch/         OpenSearch engine store + editor backend
  bookmarks/ history/ stores, models, dialogs
  locationbar/        omnibox, shield panel, domain emphasis
  extensions/         MV3 plumbing
  sandbox/            declarative policy + per-platform generators
                      (bwrap / Seatbelt / AppContainer / unveil+pledge),
                      runtime re-exec, arora-sandbox launcher script
  icons/              bundled icon sets (adwaita/breeze/tabler/lucide ±dark)
  utils/              shared helpers (SafeText, scope shortcuts, ...)
  rustcore/           shared Rust core crate (credential store, ...)
  data/ htmls/ locale/ icons and bundled pages, translations
autotests/            qtest suite (make check)
tools/                bundled CLI tools + sanitizer/coverage scripts
BuildProcess/         bundling + fetch scripts (bundle-linux.sh, ...)
.devin/               task queue, task loops, reports (dev tooling)
```

## Building

Arora uses the **qmake** build system against **Qt 6**. The reference
toolchain is **Qt 6.12** — features such as the extension API and the
DNS-mode control rely on recent Qt releases, so prefer the newest
Qt 6 you can install.

### Getting Qt user-locally (no sudo)

A from-source Qt build is not needed — the official binary packages
install into your home directory with
[aqtinstall](https://github.com/miurahr/aqtinstall):

```sh
python3 -m venv ~/venvs/aqt
~/venvs/aqt/bin/pip install aqtinstall
~/venvs/aqt/bin/aqt install-qt linux desktop 6.12.0 gcc_64 -O ~/Qt \
    -m qtwebengine qtwebchannel qtpositioning qtdeclarative qt5compat \
       qtsvg qttools qtwayland qttranslations
```

That lands Qt under `~/Qt/6.12.0/gcc_64`. Then either source the
env helper and build:

```sh
source .devin/qt-env.sh   # QTDIR/PATH/LD_LIBRARY_PATH + offscreen default
qmake && make -j$(nproc)
```

or put a `qmake6` symlink onto `~/.local/bin` pointing at
`$QTDIR/bin/qmake` (what this tree's tooling expects on PATH).

A system-packaged Qt 6 with the same modules works too — make sure
the distro's *webengine*, *webchannel*, *core5compat*, *svg* and
*tools/linguist* dev packages are present before running `qmake`.

### Build flags

- **Rust crates — ON by default (RDEF01).** `.qmake.conf` adds
  `CONFIG+=rustcore rustdl adblock_rust` whenever a cargo toolchain
  resolves (PATH first, then the user-local rustup at
  `~/.cargo/bin`; no sudo, no system packages) and qmake runs cargo
  itself as part of `make`. Opt out per crate with
  `CONFIG-=rustcore` / `CONFIG-=rustdl` / `CONFIG-=adblock_rust`, or
  `CONFIG+=no-rust` for all three. With no toolchain at all qmake
  warns loudly and builds the Qt fallback paths, so the tree always
  configures. The crates:
    - `rustcore` — credential store backend, URL tracking-param
      stripper, domain blocklist, untrusted-format parsers,
      bookmark + history stores and the navigation/cookie
      request-policy core (the C++ paths are the no-rust fallback)
    - `rustdl` — accelerated downloader (reqwest + std::thread,
      segmented ranged downloads behind a C ABI)
    - `adblock_rust` — Brave adblock-rust filter engine
- `servo=1` — experimental Servo co-engine (ENG05): compiles the
  Servo backend against `spikes/servo/servo-embed/include`, then
  dlopens the prebuilt `libservo_embed.so` at runtime. The artifact
  is an out-of-tree build (pinned libservo v0.7.0, see
  `.devin/SERVO-SPIKE.md`); when it is absent the Servo option simply
  never appears — nothing links or loads against it. While a
  `libservo_embed.so` exists under `spikes/servo/servo-embed/target/`,
  `.qmake.conf` auto-adds `CONFIG+=servo` so a plain `qmake && make`
  cannot silently drop the backend; opt out with `qmake CONFIG-=servo`,
  `noservo=1` or `servo=0`. The canonical artifact step is
  `cargo build --release` in `spikes/servo/servo-embed/` — the
  resolver prefers `target/release` over `target/debug` at every
  candidate depth, and a debug-only artifact set warns at configure
  time. If a debug cdylib does end up in use, the app logs a loud
  warning once and marks the engine name "(debug build)" with an
  extreme-slowness note on the engine surfaces — an unoptimized
  libservo is drastically slower than the release build, which is
  itself slower than Chromium on real pages (Servo is an
  experimental engine, not a performance option)
- `CONFIG+=sanitize` — ASan+UBSan instrumented build for the test
  suite

On Windows `nmake`/`jom` replaces `make`; macOS uses `make` as usual.

## Testing & diagnostics

- `make check` — the qtest suite (offscreen-safe); each test binary
  is capped at 15 minutes (`ARORA_TEST_TIMEOUT` overrides) so a hung
  test can't stall the run
- `make check-rustcoverage` — per-crate cargo-llvm-cov line-coverage
  gate for the shipped Rust crates (>=85% each;
  `ARORA_RUST_COVERAGE_MIN` overrides; also a `make check`
  prerequisite, skipped where the Rust tools are absent)
- `make check-coverage` / `check-static` / `check-leaks` — coverage
  (src/ >=85% line gate by default; `ARORA_COVERAGE_MIN` overrides),
  static analysis and sanitizer-leak gates
- `./arora --*-smoke` — a family of headless harnesses:
  `--adblock-smoke`, `--adblock-rust-smoke`, `--telemetry-smoke`,
  `--anon-smoke`, `--badssl-smoke`, `--browseraudit-smoke`,
  `--session-smoke`, `--perf-smoke`, `--sorry-smoke`,
  `--engine-list`, `--engine-smoke` (backend matrix: scripted
  navigation/zoom/find across every registered engine), and more.
  Every smoke (plus `--quit-after-load`) runs against a throwaway
  per-run config/data store, so test writes never touch the real
  profile — even when a run is killed mid-flight
- `./arora --sandbox-smoke` — end-to-end sandbox verification: wraps a
  probe child in bwrap, asserts the denylist hides files and
  directories, writes pass through, the engine runs inside the wrap,
  and a missing bwrap degrades gracefully
- `./arora --sandbox-status` — sandbox backend, whether this process
  is wrapped, and the resolved policy
- `./arora --write-sandbox-launcher` — print the `arora-sandbox`
  launcher script (used to regenerate src/sandbox/arora-sandbox)
- `./arora --profile-startup` — millisecond startup timeline
- `ARORA_DISABLE_GPU_COMPOSITING=1` — GPU-crash workaround: forces
  Chromium's `--disable-gpu-compositing` (software page compositing;
  WebGL/canvas acceleration stays on). The same mitigation lives in
  Preferences and engages automatically after two consecutive unclean
  exits; `=0` overrides even a stored "on" for a run.
- `make doc` — doxygen API reference into `doc/html` (needs `doxygen`
  + `dot`; both optional, nothing else uses them)

GUI binaries always run headless in tests (`QT_QPA_PLATFORM=offscreen`
or `xvfb-run`).

## Packaging

`make bundle` produces a **self-contained relocatable directory**
(`dist/Arora-<ver>-linux-<arch>/`) packing the binary, the Qt/WebEngine
runtime, resources, plugins, freedesktop metadata and the
`arora-sandbox` launcher — runs on a system with no Qt installed.
`make check-bundle` verifies self-containment in a bubblewrap sandbox
with the dev Qt hidden.

## Third-party components

Arora is built on, bundles, or talks to these projects — thank you to
their authors. `THIRD-PARTY-NOTICES` is the authoritative, complete
inventory (including the full transitive Rust crate list); the tables
below are the user-facing summary.

**Framework & engine**

| Component | License | Home |
|---|---|---|
| Qt 6 (toolkit) | LGPL-3.0 / GPL-3.0 | https://www.qt.io |
| QtWebEngine → Chromium (Blink/V8) | BSD-3-Clause (and others — see `about:credits`) | https://www.chromium.org |
| OpenSSL libcrypto (runtime-resolved by `securestore`, not vendored) | Apache-2.0 (3.x) / OpenSSL+SSLeay (1.x) | https://openssl-library.org |
| Tabler icons (bundled) | MIT | https://github.com/tabler/tabler-icons |
| KDE Breeze icons (bundled) | LGPL-3.0 | https://github.com/KDE/breeze-icons |
| GNOME Adwaita icons (bundled) | CC-BY-SA-3.0 / LGPL-3.0 | https://gitlab.gnome.org/GNOME/adwaita-icon-theme |

**Bundled & derived code**

| Component | License | Where |
|---|---|---|
| Qt "demo browser" heritage — ~50 core files | GPL-2.0 / GPL-3.0 + GPL Exception 1.2 | core of `src/` (© Trolltech) |
| Mozilla/Arc90 Readability 0.6.0 (Reader Mode) | Apache-2.0 | `src/data/Readability*.js` |
| networkcookiejar (Torch Mobile/Linden) | MPL-1.1 / GPL-2.0 / LGPL-2.1 | `src/network/cookiejar/networkcookiejar/` |
| BSD utility code (utils, adblockrule, useragentmenu) | BSD-3-Clause | `src/utils/` + named files |
| qrcodegen — QR generator (Nayuki, C++ and Rust crate) | MIT | `src/qrcodegen.*`, crate `qrcodegen` |
| Arora icon artwork (Jens Bache-Wiig, 2008) | GPL-2.0 | `src/data/arora.svg` + renderings |
| OpenSearch descriptors (service names are their owners' marks) | GPL-2.0+ (Arora-authored XML) | `src/data/searchengines/` |
| Bitwarden password-manager extension 2026.9.3 (bundled default) | GPL-3.0 | `src/data/extensions/` |
| Community translations | GPL-2.0+ (project license) | `src/locale/` |
| modeltest + browser-export fixtures | GPL-2.0 / test data — never shipped | `autotests/`, `tools/htmlToXBel/tests/` |

**Rust crates** (`src/rustcore`, `src/rustdl`, `src/adblock/rust` — all
versions pinned, vetted by `cargo deny` per `src/rustcore/deny.toml`;
the full transitive list lives in `THIRD-PARTY-NOTICES` §7–9)

| Crate | License | Home |
|---|---|---|
| aes-gcm, argon2, sha2, blake2 and friends (RustCrypto) | MIT / Apache-2.0 | https://github.com/RustCrypto |
| zeroize, getrandom, libc, tempfile, base64, miniz_oxide | MIT / Apache-2.0 (+Zlib for miniz_oxide) | crates.io |
| quick-xml | MIT | https://github.com/tafia/quick-xml |
| serde_json | MIT / Apache-2.0 | https://github.com/serde-rs/json |
| rusqlite | MIT | https://github.com/rusqlite/rusqlite |
| SQLite (bundled via rusqlite) | Public domain | https://sqlite.org |
| reqwest, hyper, tokio, quinn (HTTP/1.1–3 download engine) | MIT / Apache-2.0 | https://github.com/seanmonstar/reqwest |
| rustls + rustls-native-certs + rustls-webpki | Apache-2.0 / ISC / MIT | https://github.com/rustls |
| ring | Apache-2.0 / ISC | https://github.com/briansmith/ring |
| aws-lc-rs / aws-lc-sys (TLS crypto inside reqwest) | ISC / Apache-2.0 / MIT / BSD-3-Clause | https://github.com/aws/aws-lc-rs |
| webpki-root-certs (Mozilla CA store) | CDLA-Permissive-2.0 | https://github.com/rustls/webpki-roots |
| x509-parser (Rusticata) | MIT / Apache-2.0 | https://github.com/rusticata/x509-parser |
| dom_smoothie + dom_query (reader-mode extraction) | MIT | https://github.com/niklak/dom_smoothie |
| html5ever, markup5ever; cssparser, selectors (Servo) | MIT / Apache-2.0; MPL-2.0 | https://github.com/servo |
| adblock — Brave adblock-rust (optional matcher) | MPL-2.0 | https://github.com/brave/adblock-rust |
| hayro (PDF rasterizer for the shell document view) | Apache-2.0 / MIT | https://github.com/LaurenzV/hayro |
| lopdf (PDF rewrite/text extraction) | MIT | https://github.com/J-F-Liu/lopdf |

**Data feeds** (fetched at runtime, consent-gated — nothing ships in the repo)

| Feed | Terms | Home |
|---|---|---|
| EasyList / EasyPrivacy / Fanboy & regional EasyList lists (adblock defaults + presets) | GPL-3.0 / CC-BY-SA-3.0 | https://easylist.to |
| uBlock filters (uAssets) | GPL-3.0 | https://github.com/uBlockOrigin/uAssets |
| URLhaus domain blocklist | abuse.ch usage terms | https://urlhaus.abuse.ch |
| OpenPhish feed | OpenPhish community feed terms | https://openphish.com |
| Tranco top-site list (top-1M) | Tranco list terms | https://tranco-list.eu |
| ClearURLs-derived tracking-param rules (`urlstrip-rules.json`, vendored) | LGPL-3.0 | https://github.com/ClearURLs/Addon |

**Experimental** (in-tree, not shipped): libservo (MPL-2.0 —
https://servo.org) behind the spikes/servo embedding harness and the
planned selectable engine.

`make pkg-deb` (driver: `./.devin/pkg-deb.sh`) builds a **Debian
package** at `dist/arora_0.2_amd64.deb`. The same payload is laid out
on the FHS — `/usr/bin/arora` launcher, bundled Qt 6.12 + QtWebEngine
under `/usr/lib/arora` (the tree builds against a newer Qt than the
distro series ships), app data in `/usr/share/arora`, desktop file,
hicolor icons and man page under `/usr/share`. The optional Rust crates
(arora-rustdl, arora-rustcore) are statically linked, so the package
has no rust artifacts to ship. Runtime `Depends` are computed by
`dpkg-shlibdeps` — real versioned system deps only (libnss3, libdbus,
libxcb-cursor0, ...). `debian/` carries hand-rolled metadata (no
debhelper needed): `dpkg-buildpackage -b -us -uc -rfakeroot -d` works
too and drops the artifacts in the parent dir as usual. The driver
verifies the package after building: a real `dpkg -i` into a scratch
instdir, `ldd` clean with the dev Qt masked, and a headless
`--quit-after-load` launch from the installed layout.

`make pkg-flatpak` (driver: `./.devin/pkg-flatpak.sh`, manifest:
`io.arora.Arora.yaml`) builds an **installable flatpak** at
`dist/io.arora.Arora_0.2_x86_64.flatpak` and installs it into the user
flatpak. The app compiles inside the org.kde 6.11 SDK (newest Qt 6
runtime on flathub) with QtWebEngine provided by the
`io.qt.qtwebengine.BaseApp` and cargo by the rust-stable SDK extension —
both rust crates build in-sandbox from the vendored, sha256-pinned crate
set in `flatpak/cargo-sources.json` (regenerate it with
`flatpak/gen-cargo-sources.py` after touching a Cargo.lock). The tor
expert bundle ships under `/app/lib/tor` so tor windows bootstrap inside
the sandbox (no host tor needed). Permissions stay honest: ipc, network,
wayland + fallback-x11, dri, pulseaudio, cups, xdg-download — no session
bus, no host filesystem, no flatpak-spawn host access. Building needs
flatpak plus `org.flatpak.Builder` installed `--user` (no root); the
driver verifies the bundle by installing it and running the offscreen
`--quit-after-load` + `--tor --tor-window-smoke` paths inside the
sandbox.

`make pkg-rpm` (driver: `./.devin/pkg-rpm.sh`, spec: `rpm/arora.spec`)
builds an **openSUSE-compatible RPM** at `dist/arora_0.2-0.*.rpm`.
Same artifact policy as the deb — identical staged payload
(`/usr/bin/arora` + private Qt/QtWebEngine under `/usr/lib/arora`,
statically-linked rust crates), so the spec disables `AutoReqProv` and
carries an explicit `Requires:` list in SONAME-capability form that the
driver regenerates and diffs against the staged tree. The `%{qtdir}`
macro selects the relocatable Qt the payload bundles (defaults to
`$QTDIR`, then `~/Qt/6.12.0/gcc_64`). Building the rpm itself needs
`rpmbuild`; on a host without rpm tooling `./.devin/pkg-rpm.sh
container` prints the openSUSE container command. The driver's verify
leg checks spec sanity, `%files` coverage and the `Requires` surface
locally, and — given a built .rpm plus rpm tooling — extracts it and
runs the same masked-Qt `ldd` + headless `--quit-after-load` checks as
the deb verifier.

## License

Arora is **free software under the GNU General Public License,
version 2 or later** — see COPYING for the full text.

Arora is **experimental software provided "as is"** — the license text
governs, but in plain terms: there is **no warranty**, no guarantee of
fitness for any purpose, and no liability accepted for data loss,
security failures or any other damage. Use it for testing, hacking and
learning; treat it as a research browser, not a promise.

Original Arora: Benjamin C. Meyer (icefox) and contributors, 2007–2011.
Qt 6 port + continued development: see AUTHORS and git history.
