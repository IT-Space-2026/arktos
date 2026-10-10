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
- **Per-site JavaScript control** — allow/block rules by host with a
  shield-panel toggle, a blocked-scripts bar with "Allow once"/"Always
  allow", and three Mullvad-style security tiers (Standard / Safer /
  Safest) for script and media handling on insecure pages.
- **Fingerprint resistance** — normalization (vanilla-Chrome user
  agent + client hints, UTC timezone, generic Accept-Language) plus an
  opt-in injected layer (Settings > Privacy, "Spoof canvas/WebGL
  fingerprints"; always on in Tor windows): canvas readouts get
  per-session noise, WebGL reports a generic vendor/renderer and
  navigator.hardwareConcurrency/deviceMemory/plugins are uniform.
  Honest caveat: this is noise, not anonymity — sites can detect the
  spoofing and canvas-heavy pages can break, so the shield panel has a
  per-site "Spoof fingerprints on this site" switch that exempts a
  misbehaving site on reload.
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
  Argon2id master passphrase; a Rust implementation (rustcore) is
  available behind `CONFIG+=rustcore`, byte-compatible with the C++
  store.
- **Tracking-parameter stripping** — ClearURLs-style rules strip
  tracking query params from navigations (vendored rules +
  data-dir override; rustcore backend, C++ fallback).
- **Anti-phishing/malware blocklist** — a vendored domain seed merged
  with a consent-gated URLhaus/OpenPhish feed; listed main-frame
  navigations stop at a warning interstitial with a session-only
  proceed.
- **Rust parsers for untrusted input** — OpenSearch descriptors,
  extension update manifests and suggestion replies are parsed in
  memory-safe Rust behind the FFI before Qt sees them.
- **Rust bookmark + history stores** — with `CONFIG+=rustcore` the
  crate is also the canonical bookmark + history store:
  bookmarks.xbel stays the on-disk format but is parsed and emitted
  in Rust (queried lazily, one node handle per call — no bulk tree
  marshal), and history lives in a SQLite history.db (visits plus
  per-host favicons that persist across restarts) that imports the
  legacy QDataStream file on first run. The crate also owns session
  save/restore serialization: a versioned session.dat written
  atomically and bounds-checked on decode, so a corrupt file is
  rejected whole instead of looping the crash-restore prompt —
  per-tab records carry url, container binding, tab group and an
  engine tag beside an opaque engine-state blob, keeping the format
  engine-neutral (a pre-Rust QSettings session still restores once
  through the legacy reader).
- **Hardened parsers** — OpenSearch descriptions are size-capped and
  DTD-free, download file names are fully sanitized, and page-controlled
  strings cannot inject markup into chrome.
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
  window.
- **Tab groups** — named, color-coded groups on the strip.
- **Sidebar dock** — optional Vivaldi-style side panel (bookmarks,
  history, downloads, notes), lazily built and off by default.
- **Tab search** — Ctrl+Shift+A fuzzy dropdown over all open tabs,
  with hover previews.
- **Sleeping tabs** — suspend tabs to reclaim memory without losing
  their place.
- **Reader mode** (Ctrl+Alt+R) and **Picture-in-Picture** on videos.
- **Command palette** — Ctrl+Shift+P fuzzy search over browser
  commands.
- **Sharing** — "Copy Clean Link" strips tracking parameters;
  "Show QR" renders a link as a QR code.
- **Preferences** — opens as a tab (one per window), Vivaldi-style
  vertical sidebar navigation with a filter field that finds settings
  by page title and control labels, scrollable pages capped to the
  screen so buttons never clip, dedicated Search and Containers pages.
- **History manager** — "Show All History" (Ctrl+H) opens in a tab
  instead of a floating window; entries open in new tabs.
- **Look & feel** — Theme selector (System default / Light / Dark,
  `ARORA_COLOR_SCHEME` override), selectable bundled icon sets
  (Adwaita/Breeze/Tabler, light+dark variants), status-bar load
  indicator and zoom control, domain-emphasized anti-phishing address
  bar with IDN punycode display.

### Search

- **Omnibox** — address-shaped input navigates, everything else
  searches the default engine (DuckDuckGo by default).
- **Engine management** — a dedicated Search settings page with an
  inline engine editor (name, nickname, search/suggest/image URLs, POST
  params), per-context engine choices (default / private-window /
  image search) and shortcut nicknames (@bookmarks, @history, @tabs).
- **Suggestions opt-in per engine** — nothing is sent to a suggest
  endpoint unless the user enables it for that engine.

### AdBlock

- **uBlock-Origin-tier rule engine** — resource types,
  third-party/domain restrictions, $important/$badfilter/$removeparam,
  $redirect to bundled stubs, cosmetic extended selectors and a
  scriptlet subset.
- **Optional Brave adblock-rust engine** — `qmake
  CONFIG+=adblock_rust` after `cargo build --release` in
  src/adblock/rust; the built-in matcher stays the default.
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
(guard -> middle -> exit, fingerprints on hover).

### Downloads

The download manager drives QWebEngineDownloadRequest: progress, speed
and ETA per item, Try Again (re-issues through the page or a hidden
page), open-folder/open-with actions, server-suggested names sanitized
and de-duplicated, and an external download-program handoff. An
optional Rust reqwest-based engine (`CONFIG+=rustdl`) accelerates
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
install always stops at a permission-review screen, `update_url`
checks are supported, and extensions are verified not to run in
private or Tor windows.

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
  by an injected script reporting over a hardened, token-gated
  WebChannel bridge. Honest deltas: only JS-observable submits are
  captured (the app cannot see POST bodies), iframe forms are not
  captured, and a submit racing an immediate navigation can be missed.
  The store moved from plaintext `autofill.dat` to AES-256-GCM sealed
  storage with an optional Argon2id master passphrase.
- **Caching** — page loads cache inside Chromium; the app's
  QNetworkAccessManager (with its own disk cache) only handles
  app-side fetches such as search suggestions and blocklist downloads.
- **Inspector** — WebKit's built-in inspector became a DevTools host
  window: the inspected page hands its `devToolsPage` to a second
  QWebEngineView on the same profile.  A second, engine-neutral
  panel (Tools → BiDi Dev Tools; `CONFIG+=rustcore` builds) talks
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
  icons/              bundled icon sets (adwaita/breeze/tabler ±dark)
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

- `CONFIG+=adblock_rust` — Brave adblock-rust filter engine (needs a
  user-local Rust toolchain: `cargo build --release` in
  `src/adblock/rust` first)
- `CONFIG+=rustcore` — Rust core crate: credential store backend,
  URL tracking-param stripper, domain blocklist, untrusted-format
  parsers, bookmark + history stores and the navigation/cookie
  request-policy core (same toolchain; qmake runs cargo; C++ paths
  stay the default without it)
- `CONFIG+=rustdl` — Rust accelerated downloader (reqwest +
  std::thread, segmented ranged downloads behind a C ABI)
- `CONFIG+=sanitize` — ASan+UBSan instrumented build for the test
  suite

On Windows `nmake`/`jom` replaces `make`; macOS uses `make` as usual.

## Testing & diagnostics

- `make check` — the qtest suite (offscreen-safe)
- `make check-coverage` / `check-static` / `check-leaks` — coverage,
  static analysis and sanitizer-leak gates
- `./arora --*-smoke` — a family of headless harnesses:
  `--adblock-smoke`, `--adblock-rust-smoke`, `--telemetry-smoke`,
  `--anon-smoke`, `--badssl-smoke`, `--browseraudit-smoke`,
  `--session-smoke`, `--perf-smoke`, `--sorry-smoke`, and more
- `./arora --sandbox-smoke` — end-to-end sandbox verification: wraps a
  probe child in bwrap, asserts the denylist hides files and
  directories, writes pass through, the engine runs inside the wrap,
  and a missing bwrap degrades gracefully
- `./arora --sandbox-status` — sandbox backend, whether this process
  is wrapped, and the resolved policy
- `./arora --write-sandbox-launcher` — print the `arora-sandbox`
  launcher script (used to regenerate src/sandbox/arora-sandbox)
- `./arora --profile-startup` — millisecond startup timeline
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
their authors:

**Framework & engine**

| Component | License | Home |
|---|---|---|
| Qt 6 (toolkit) | LGPL-3.0 / GPL-3.0 | https://www.qt.io |
| QtWebEngine → Chromium (Blink/V8) | BSD-3-Clause (and others — see `about:credits`) | https://www.chromium.org |
| Tabler icons (bundled) | MIT | https://github.com/tabler/tabler-icons |
| KDE Breeze icons (bundled) | LGPL-3.0 | https://github.com/KDE/breeze-icons |
| GNOME Adwaita icons (bundled) | CC-BY-SA-3.0 / LGPL-3.0 | https://gitlab.gnome.org/GNOME/adwaita-icon-theme |

**Rust crates** (`src/rustcore`, `src/rustdl`, `src/adblock/rust` — all
versions pinned, vetted by `cargo deny` per `src/rustcore/deny.toml`)

| Crate | License | Home |
|---|---|---|
| aes-gcm, argon2, zeroize, getrandom | MIT / Apache-2.0 | https://github.com/RustCrypto |
| quick-xml | MIT | https://github.com/tafia/quick-xml |
| serde_json | MIT / Apache-2.0 | https://github.com/serde-rs/json |
| rusqlite | MIT | https://github.com/rusqlite/rusqlite |
| SQLite (bundled via rusqlite) | Public domain | https://sqlite.org |
| reqwest | MIT / Apache-2.0 | https://github.com/seanmonstar/reqwest |
| rustls (TLS inside reqwest) | Apache-2.0 / MIT / ISC | https://github.com/rustls/rustls |
| tempfile | MIT / Apache-2.0 | https://github.com/Stebalien/tempfile |
| base64 | MIT / Apache-2.0 | https://github.com/marshallpierce/rust-base64 |
| adblock — Brave adblock-rust (optional matcher) | MPL-2.0 | https://github.com/brave/adblock-rust |

**Data feeds** (fetched at runtime, consent-gated)

| Feed | Terms | Home |
|---|---|---|
| URLhaus domain blocklist | abuse.ch usage terms | https://urlhaus.abuse.ch |
| OpenPhish feed | OpenPhish community feed terms | https://openphish.com |
| ClearURLs-derived tracking-param rules (`urlstrip-rules.json`, vendored) | LGPL-3.0 | https://github.com/ClearURLs/Addon |

**Planned** (queued tasks, not shipped): PDFium +
`pdfium-render` (BSD-3 / MIT-Apache — https://pdfium.googlesource.com,
https://github.com/ajrcarey/pdfium-render) for the shell-owned PDF
view, `lopdf` (MIT — https://github.com/J-F-Liu/lopdf) for the sanitize
pass, libservo (MPL-2.0 — https://servo.org) as the selectable engine.

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
