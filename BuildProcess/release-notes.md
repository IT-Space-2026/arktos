Arktos 1.3 — two engine flavors on every desktop package plus the
tor 16.0 expert bundle.

Each platform now ships a servo build and a NoServo (QtWebEngine-only)
sibling that installs side-by-side: flatpak
com.arktos_browser.Arktos / com.arktos_browser.ArktosNoServo, Windows
"Arktos"/"Arktos NoServo" installers + portable zips, macOS
Arktos-1.3.dmg / Arktos-NoServo-1.3.dmg. The bundled tor expert bundle
is updated to 16.0.

Arktos 1.2 — full code audit, engine flavors on every desktop package, and the Servo backend reaching the flatpak build.

Engine flavors: every platform ships two side-by-side-installable builds — the default servo flavor and a generated NoServo (QtWebEngine-only) sibling. Flatpak carries distinct app-ids (com.arktos_browser.Arktos / com.arktos_browser.ArktosNoServo, "Arktos" / "Arktos (No Servo)" launchers, generated manifest + metainfo drift-checked in CI; the servo flavor builds libservo_embed.so inside the KDE 6.11 SDK from the vendored sha256-pinned crate set with llvm21 clang for mozjs-sys bindgen). Windows gets "Arktos"/"Arktos NoServo" NSIS installers (distinct product name, install dir and uninstall key) plus per-flavor portable zips. macOS gets Arktos-1.2.dmg / Arktos-NoServo-1.2.dmg with the NoServo bundle renamed ArktosNoServo.app. The engine selector lists webengine + Servo 0.7.0 in the servo flavor and webengine only in the NoServo one; the backend is dlopen'd at runtime so a missing cdylib degrades honestly.

Code audit (CA01–CA13, .devin/CODE-AUDIT.md): dead code removed, ~20 cross-file duplicate families consolidated, eight security findings fixed (extension bridge subframe isolation, PDF worker path traversal, NAM certificate persistence, DNR redirect re-gating, native-messaging host anchoring), three dying-object/lifetime crashes fixed, performance work (adblock rebuild coalescing, DNR candidate index, cold-start), and Windows/macOS platform remediations (pinned IniFormat QSettings, registry/AppData paths, NTFS-reserved filename screening, ASFW_ANY, longPathAware, entitlements, menubar/navbar-drag gates, DPR pixmaps).

Other changes: pastel-circle settings icons from Arktos-Mobile; packaging pipeline fixes (macOS IOKit link, arti-client vendoring, rpm tarball naming, MSVC far-macro, smoke-helper platform guards, NSIS license path + overridable PRODUCT_NAME, clang vexing-parse, generated-manifest drift check); bundled tor expert bundle 15.0.25 (16.0 ships in 1.3); the flatpak app-id is read from /.flatpak-info instead of hardcoded.

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
