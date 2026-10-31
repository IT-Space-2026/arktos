Arktos 1.4.2 — LAN sync fast path and pastel URL-bar indicators.

Sync pairing now starts its transport with backend "auto": the desktop announces over mDNS and listens on QUIC while the pair dialog is open, so same-network devices discover and dial it in a second or two with no Tor in the loop. Tor remains the fallback for remote networks — previously the dialog was tor-only, so LAN discovery never ran at all.

The URL-bar state indicators (site padlock, trust shield, reader button) adopt the pastel-circle icon style introduced in 1.4.1, with the disc color following the security/trust state. Toolbar and URL-bar icons are sized up so the discs stay readable, the pairing code field now fits the full code (no clipping/overflow), and the domain-highlight repaint no longer leaves a mismatched patch in the url bar. Bundled tor expert bundle: 16.0.

Experimental software, provided as is.

Arktos 1.4.1 — pastel-circle chrome icons.

The nav bar (back/forward/stop/reload/home), tab-strip buttons and the popup-blocker URL-bar button now use the same pastel-circle icon style as the preferences sidebar. The app logo, profile button and the state-painted indicators (site padlock, trust shield, reader, engine, privacy) keep their existing visuals; tor mode keeps its violet identity tint. Bundled tor expert bundle: 16.0.

Experimental software, provided as is.

Arktos 1.4 — sync pairing fixes.

The pairing code now displays in a read-only, copyable monospace field with a Copy button (previously a clipped, unselectable label under the QR). Closing the pairing dialog can no longer freeze the app — the worker thread previously blocked the GUI on destruction while a tor call was in flight — and the 800ms pair-poll no longer piles queued calls onto a hung worker. Bundled tor expert bundle: 16.0.

Experimental software, provided as is.

Arktos 1.3 — two engine flavors on every desktop package plus the
tor 16.0 expert bundle.

Each platform now ships a servo build and a NoServo (QtWebEngine-only)
sibling that installs side-by-side: flatpak
com.arktos_browser.Arktos / com.arktos_browser.ArktosNoServo, Windows
"Arktos"/"Arktos NoServo" installers + portable zips, macOS
Arktos-1.3.dmg / Arktos-NoServo-1.3.dmg. The bundled tor expert bundle
is updated to 16.0.

Arktos 1.2 — a maintained continuation of the 2009 Qt/WebKit Arora, rebuilt on Qt 6 + QtWebEngine (Chromium) around privacy hardening, per-site containers, optional Tor windows and a Vivaldi-style UI. This release lands the full CA01–CA13 code audit and ships two engine flavors on every desktop package.

## Features

- **Two engine flavors on every platform.** The default servo build and a generated NoServo (QtWebEngine-only) sibling install side-by-side:
  - **Flatpak** — `com.arktos_browser.Arktos` and `com.arktos_browser.ArktosNoServo`, distinct app-ids and launchers ("Arktos" / "Arktos (No Servo)"); the NoServo manifest + metainfo are generated from the canonical servo manifest by `flatpak/gen-flavor-manifest.py` and drift-checked in CI.
  - **Windows** — "Arktos" and "Arktos NoServo" NSIS installers (distinct product name, install directory and uninstall registry key) plus per-flavor portable zips.
  - **macOS** — `Arktos-1.2.dmg` and `Arktos-NoServo-1.2.dmg`; the NoServo bundle is renamed `ArktosNoServo.app` so both can live in /Applications.
- **Servo backend reaches the flatpak build.** `libservo_embed.so` now compiles inside the KDE 6.11 SDK as its own flatpak-builder module — ~700 crates plus the 55 libservo member crates vendored offline-exact from `flatpak/cargo-sources-servo.json` (commit-pinned servo.git, cargo-vendor-normalized manifests, nested mozjs_* cbindgen locks), with clang/libclang from the llvm21 SDK extension feeding mozjs-sys bindgen. The engine selector lists `webengine` + `Servo 0.7.0`; the backend is dlopen'd at runtime so a missing cdylib degrades honestly rather than showing a dead entry.
- **Per-flavor CI matrix.** flatpak, windows and macos jobs each build both flavors with per-flavor cache keys, artifact names and payload assertions (servo cdylib present iff servo flavor).
- **Pastel-circle settings icons** ported from Arktos-Mobile — one colored disc + monochrome glyph per settings category, including the Sync section icon fix.
- **Flatpak Wayland association** reads the app-id from `/.flatpak-info`, so engine flavors get the correct desktop-file match automatically.

## Fixes

- **Security (CA02):** extension chromeBridge confined to its own subframe; PDF worker filename pinned against path traversal; NAM session cert whitelist host+fingerprint scoped (no CA-store planting); DNR redirect re-gated with file/ftp coverage removed; native-messaging host name anchored to an absolute allow-list path; header value validation hardened.
- **Lifetime (CA03):** CookieJar mirror is now a QPointer (store can die during WebEngine teardown — real segfault), event-hub destroyed-lambda member access, removeMainWindow downcast at window-manager entry; 13-leg ASan/UBSan/LSan smoke run shows zero our-code blocks.
- **Windows (CA12 + CI):** IniFormat QSettings pinned globally, nativeMessaging registry+Filesystem discovery, profile-vault rename-with-open-handles retry, NTFS reserved-name and invalid-char screening, win32 console attach + NOMINMAX/LEAN, ASFW_ANY + self-raise, case-insensitive suffix checks, NSIS StartMenuInternet + VCREDIST warn + GPL page, longPathAware manifest, MSVC far-macro collision, smoke-helper platform guards, NSIS license path, overridable PRODUCT_NAME.
- **macOS (CA12 + CI):** App Support + homebrew tor fetch paths, arktos.entitlements, menubar/navbar-drag gates + shortcuts, locale/useragents bundle data, DPR pixmaps, 19px icon cap removed, IOKit framework link for rustcore consumers, clang vexing-parse in tst_settingsdialog.
- **Packaging:** flatpak cargo-sources regenerated (371→666 crates, arti-client 0.47.0 tree), rpm tarball name matches spec Version, generated NoServo manifest drift-check, `*.zip` + per-flavor artifacts in the release glob, `git ls-files` deleted-paths export fix.

## Improvements

- **Performance (CA04):** adblock rule rebuilds coalesced (11/13/12 → 3/5/3 rebuilds per session), DNR gram/domain candidate index (~4400x faster per eval), site whitelist folded into the matcher snapshot, AutoSaver policy memoized, process-global NetIndicator pid scan, history url-membership fast path.
- **Maintainability (CA01/CA07/CA08/CA13):** dead functions/constants/includes/lambda captures removed, ~20 cross-file duplicate families consolidated (shim FFI vocabulary, cookie record JSON, session/tab/sanitize/opensearch/bidi/statusbar helpers), clippy dead_code at zero.

## Notes

- Bundled tor expert bundle: **15.0.25** on this tag (16.0 ships in 1.3).
- Assets: debs for ubuntu-24.04 + ubuntu-26.04, an openSUSE-compatible rpm, both flatpak flavors, both Windows installers + portable zips, both macOS dmgs.

Experimental software, provided as is.

Arktos 1.0 — first release. An experimental cross-platform web browser
built on Qt 6 and QtWebEngine (Chromium), continuing the 2009 Arora
browser as a maintained fork — rebuilt around privacy hardening,
containers, Tor support and a modernized UI.

## Highlights

- **Three rendering engines** — Chromium (Qt WebEngine) default, plus
  experimental Servo and WPE WebKit backends with a per-tab engine
  switcher and honest capability reporting per engine.
- **Tor browsing windows** — managed bundled tor daemon, SOCKS5-only
  egress, dedicated off-the-record profile, .onion reachability, live
  circuit display and "new circuit for this site", fail-closed startup
  proxying.
- **Private tabs** — true off-the-record browsing (Chromium OTR
  profile), private state torn down when the last private page closes,
  excluded from history/session/recently-closed/download records.
- **Profiles, personas and containers** — isolated per-window browsing
  profiles under `profiles/<id>/`, site-bound container tabs with
  "always open this site in…" rules.
- **Privacy controls** — HTTPS-first upgrade + HTTPS-Only mode,
  Global Privacy Control (`Sec-GPC`), tracking-parameter stripping,
  domain blocklist, per-site JavaScript control with security tiers,
  site-trust panel (Tranco/Majestic rank, lookalike domains, threat
  list), second-opinion Rust TLS verification.
- **Ad blocking** — network-level blocking, `$redirect` stub serving
  and declarative cosmetic filtering, per-profile policy.
- **Extensions** — Chrome MV3 support: consent-gated install,
  integrity pinning, opt-in verified updates, per-session health
  check, and a hosted `chrome.*` runtime for APIs Qt WebEngine lacks.
  Bitwarden bundled — limited support, documented honestly.
- **Tabs** — groups with collapse, pinning, drag between windows,
  clone, recently-closed restore, idle suspend/discard and
  memory-pressure purge, command palette.
- **Downloads** — parallel-segment accelerated engine (Rust) running
  in a bubblewrap-sandboxed worker on Linux, sidebar panel with
  detail cards, external program handoff.
- **Rust core** — an engine-neutral browser core behind one stable C
  ABI (`arktos_core.h`): settings, profiles/personas, history,
  bookmarks and per-site policy stores consumed identically by every
  engine backend, plus session/omnibox stores, credential vault and
  sealed autofill store, parsers for untrusted input,
  cargo-deny/audit supply-chain gates.
- **Reader mode, Picture-in-Picture, PDF viewer** — PDFs sanitized
  before view; in-browser PDF tooling.
- **Security hardening** — extension navigation locks, verified
  message-relay senders, nonce-bound interstitials, sandbox policy
  (bwrap / Seatbelt / AppContainer / unveil+pledge), scheme-handler
  initiator gates, untrusted-input URL filtering.

## Packages

- **arktos_1.0_amd64.deb** — Debian/Ubuntu (`apt install ./arktos_1.0_amd64.deb`)
- **arktos-1.0-1.x86_64.rpm** — Fedora/openSUSE (`dnf install`/`zypper install`)
- **com.arktos_browser.Arktos.flatpak** — flatpak bundle (KDE 6.11
  runtime + QtWebEngine baseapp; Servo backend not included)
- **Arktos.dmg** — macOS, drag to Applications
- **Arktos 1.0 Installer.exe** — Windows 11 x64 installer

## Caveats

- Experimental software — expect rough edges; not yet a daily-driver
  replacement.
- Binaries are unsigned: macOS Gatekeeper and Windows SmartScreen will
  warn; verify via the source tag.
- Bundled Bitwarden is limited to tested flows (unlock, autofill) —
  see the README for the honest capability matrix.

Source: https://github.com/IT-Space-2026/arktos — website: https://arktos-browser.com
