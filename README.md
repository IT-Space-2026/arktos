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
- **Site trust (opt-out)** — a "Site Trust" group on the Privacy page
  holds the master switch for the toolbar trust indicator plus
  per-signal toggles for the lookalike-domain warning and the
  threat-list check; disabling the master hides the badge and each
  signal can be skipped individually, all without a restart.
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
  window and picked from clickable preview tiles in Preferences.
  A Vivaldi-style **'+' button** sits right after the last tab in
  every orientation (an appended row on vertical rails).
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
  General page has an inline Language selector (System default /
  installed translations / English) that applies immediately.
- **History manager** — "Show All History" (Ctrl+H) opens in a tab
  instead of a floating window; entries open in new tabs.
- **Look & feel** — Theme selector (System default / Light / Dark,
  `ARORA_COLOR_SCHEME` override), bundled icon sets
  (Adwaita/Breeze/Tabler, light+dark variants — Tabler is the default
  so every menu glyph ships in the bundle; Native still follows the
  desktop theme and falls back to bundled dark glyphs when it can't
  serve light-on-dark art), flat palette-aware chrome with a consistent
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
- **Suggestions opt-in per engine** — nothing is sent to a suggest
  endpoint unless the user enables it for that engine.

### AdBlock

- **uBlock-Origin-tier rule engine** — resource types,
  third-party/domain restrictions, $important/$badfilter/$removeparam,
  $redirect to bundled stubs, cosmetic extended selectors and a
  scriptlet subset.
- **Brave adblock-rust engine** — `qmake
  CONFIG+=adblock_rust` after `cargo build --release` in
  src/adblock/rust; with the engine compiled in it is the default
  matcher. Preferences → Privacy → Content Blocking lets
  you switch between Brave adblock-rust and the built-in matcher at
  runtime (no restart); builds without the engine behave exactly like
  the built-in matcher.
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
(guard -> middle -> exit, each hop tagged with its country code,
fingerprints on hover), and a small onion button next to the
reader-mode icon opens a dropdown of the same circuit — one row per
hop with country flag and name, fingerprints and OR addresses on
hover. New tabs land on a Tor-branded start page: the usual Arora
header and search box on a purple background with the official Tor
onion centered — bundled in the binary, so the page never touches the
network; a configured custom homepage still takes precedence.

### Downloads

The download manager drives QWebEngineDownloadRequest: progress, speed
and ETA per item, Try Again (re-issues through the page or a hidden
page), open-folder/open-with actions, server-suggested names sanitized
and de-duplicated, and an external download-program handoff. Downloads
surface in the sidebar's Downloads panel (Ctrl+Y or Tools →
Downloads) — filterable, sortable, and each selected row expands into a
detail card in the panel's bottom pane. Preferences → Downloads hosts
a download-engine selector — Built-in (Chromium) or Accelerated (Rust,
enabled in `CONFIG+=rustdl` builds; Accelerated is the default there
and a saved Built-in pick is honored) — a connections-per-download
field, a cleanup policy for the finished-downloads list, and a
"Parallel segments" toggle (default on) that has Chromium fetch
range-capable downloads over multiple concurrent byte-range requests;
it takes effect at restart. The Accelerated engine is a Rust/reqwest
multi-connection fetcher behind a C ABI (`src/rustdl`); it claims
http(s) downloads only, and never runs when an application proxy is
configured — Tor windows and other proxied transfers always stay on
the built-in engine (DLACC06).

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
