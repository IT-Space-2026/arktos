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
- [Source structure](#source-structure)
- [Building](#building)
- [Testing & diagnostics](#testing--diagnostics)
- [Packaging](#packaging)
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
- **Fingerprint resistance** — opt-in countermeasures plus the honest
  caveat that they are noise, not anonymity.
- **Zero telemetry, verified** — launch produces no unsolicited
  connections; Chromium's background networking, component updates,
  domain reliability, metrics and sync are disabled, and a capture-
  proxy smoke proves it.
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
- **Hardened parsers** — OpenSearch descriptions are size-capped and
  DTD-free, download file names are fully sanitized, and page-controlled
  strings cannot inject markup into chrome.

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
no suggestions, no dedicated search box — the omnibox covers it).
The status bar shows the live circuit's hop chain
(guard -> middle -> exit, fingerprints on hover).

### Downloads

The download manager drives QWebEngineDownloadRequest: progress, speed
and ETA per item, Try Again (re-issues through the page or a hidden
page), open-folder/open-with actions, server-suggested names sanitized
and de-duplicated, and an external download-program handoff. A Rust
reqwest-based multi-connection engine is in development (DLACC tasks).

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

Arora uses the **qmake** build system against Qt 6 (developed on Qt
6.12, user-local install — see `.devin/qt-env.sh`):

```sh
qmake && make -j$(nproc)
```

Optional build flags:

- `CONFIG+=adblock_rust` — Brave adblock-rust filter engine (needs a
  user-local Rust toolchain: `cargo build --release` in
  `src/adblock/rust` first)
- `CONFIG+=rustcore` — Rust core crate: credential store backend,
  URL tracking-param stripper, domain blocklist and untrusted-format
  parsers (same toolchain; qmake runs cargo; C++ paths stay the
  default without it)
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
- `./arora --profile-startup` — millisecond startup timeline

GUI binaries always run headless in tests (`QT_QPA_PLATFORM=offscreen`
or `xvfb-run`).

## Packaging

`make bundle` produces a **self-contained relocatable directory**
(`dist/Arora-<ver>-linux-<arch>/`) packing the binary, the Qt/WebEngine
runtime, resources, plugins and freedesktop metadata — runs on a system
with no Qt installed. `make check-bundle` verifies self-containment in
a bubblewrap sandbox with the dev Qt hidden.

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
