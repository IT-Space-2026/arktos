# SEC23 — External-URL ingress audit

Audit + regression coverage for every path an untrusted URL can enter a
running Arora process.  Companion to the ui-lane CONT07/CONT08 work;
this task owns the process-boundary side.

## Entry-point matrix

### (a) Positional argv URL

`arora <url>` resolves through two sinks, both already gated:

- **Standalone harness** (`main.cpp`): the `--quit-after-load`-style
  path refuses `!WebView::isUrlAllowedOnUntrustedInput(firstUrl)`
  before any `loadUrl()` — prints "Ignoring untrusted argv url".
- **Normal launch** (`BrowserApplication::postLaunch()`): feeds the
  operand through `TabWidget::loadStringFromUntrustedSource()`, which
  resolves via `guessUrlFromString()` (Rust `rc_classify_input` with
  the C++ fallback) and gates the *resolved* `QUrl` — normalization
  tricks (leading whitespace, mixed case) are caught post-resolution.

The gate itself (`WebView::isUrlAllowedOnUntrustedInput`) refuses
`javascript:` — `WebView::loadUrl()` would otherwise run the payload
via `runJavaScript` in the current page.  `data:` and `file:` remain
allowed: `data:` navigates an opaque origin (equivalent to typing it)
and `file:` is the SEC09 decision (local file operands are a feature).

`vbscript:` / `aroramessage://` / other non-browser schemes pass the
scheme gate but dead-end: a programmatic `load()` of an unknown scheme
never becomes a Chromium navigation request — no prompt, no external
handoff, no script.  The consent prompt that backs real
(renderer-initiated) external-protocol navigations stays hook-level
covered by `tst_webpage` (SAFE05 magnet/intent/vnc rows).

**Coverage** (`--ingress-smoke`, in-process lane):
`javascript:` refused (plain / leading-space padded / `JaVaScRiPt:`
case-mangled — `QUrl` lowercases the scheme), `vbscript:` and
`aroramessage://` operands dead-end without navigation or script,
`data:`/`file:` gate-allowed, and a `file://` fixture proves the lane
still navigates permitted URLs.

**Coverage** (end-to-end child): `arora --quit-after-load
javascript:alert(1)` exits 0 with "Ignoring untrusted argv url" on
stderr — the gate fires before the stub window's first load.

### (b) Single-instance socket forward

`SingleApplication::newConnection()` reaps clients on `readyRead`;
`BrowserApplication::messageReceived()` tokenizes the payload and
branches on the **first whitespace-delimited token only**:

- `aroramessage://getwinid` → control lane: raises the window and
  answers `aroramessage://winid/` — never touches navigation.
- `aroramessage://<anything-else>` → control lane: ignored entirely.
- anything else → navigation lane:
  `loadStringFromUntrustedSource(message, openLinksFromAppsIn)` — the
  same gate as argv, plus the 10s `m_lastAskedUrl` dedup.

Namespace confusion is closed by construction: a control token that
carries a URL payload stays in the control lane (first token wins,
leftover tokens are discarded with the QTextStream), and a URL-shaped
payload can never reach the control handlers because the
`aroramessage://` prefix is checked on the raw wire text, not a parsed
URL.

**Coverage**: a real `QLocalServer`/`QLocalSocket` pair feeds
`messageReceived()` directly — getwinid answers on the wire with no
navigation, an unknown control token is ignored, a control token
carrying a `javascript:` payload navigates nothing and executes
nothing, a bare `javascript:` payload is refused by the gate, and the
`file://` fixture navigates through the lane.

### (c) `QDesktopServices` `http` handler → `openUrl()`

Was the one real gap: the slot called
`mainWindow()->tabWidget()->loadUrl()` **directly** — no untrusted
gate.  Only `http` is registered today, but the slot is public surface
and the registered-scheme set can grow (or be invoked by in-app code
holding a `QUrl`).

**Fix** (`browserapplication.cpp`): `openUrl()` now refuses
`!WebView::isUrlAllowedOnUntrustedInput(url)` with a warning before
loading — same policy object as argv and IPC.

**Coverage**: `openUrl(javascript:…)` invoked through the meta-object
leaves the page untouched with no script execution;
`openUrl(file://fixture)` navigates.

### (d) `--tor <url>` operand

- `argumentUrl()` picks the last non-dash argument — the operand is
  positional; `--tor` is a flag (`QCommandLineOption` with no value
  name), so `arora --tor <url>` lands `<url>` in
  `positionalArguments()` and `--tor=<anything>` is a hard parser
  error.  Nothing passes through a shell anywhere on the path
  (`QProcess` argv forms only; the tor daemon spawn is
  `ARORA_TOR_BINARY`/path + fixed args).
- `torStartup()` funnels the operand through
  `loadStringFromUntrustedSource()` — same gate.
- Any `--` argument sets `m_standalone` before the single-instance
  handshake, so a `--tor` process never connects, never listens, and
  can never receive forwarded URLs — the process-boundary isolation
  this task needed to prove.

**Coverage**: parser-level assertions (`--tor` operand lands
positional, `javascript:` operand is positional *and* gated
downstream, `--tor=<value>` is a parse error) plus live children:
`arora --tor` (fake daemon via `ARORA_TOR_BINARY`) spawns **no**
listener in a private `XDG_RUNTIME_DIR`, and a following `arora
about:blank` cannot forward into it — it must create its own socket,
proving nothing was delivered to the tor process.  `arora
--tor=javascript:x` exits non-zero on the parser error.

### (e) Single-instance socket hardening

`SingleApplication::startSingleServer()` sets
`QLocalServer::UserAccessOption` **at bind time** (no create-then-
chmod race), then forces the socket file to `0600`.
`serverAddress()` binds inside `QStandardPaths::RuntimeLocation`
(`$XDG_RUNTIME_DIR`, user-private by definition) with a
`QDir::tempPath()/arora-<uid>` fallback; either way the containing
directory is forced `0700` and the server name is
`Arora_<uid>_<gid>` — not a predictable shared `/tmp` name, and
invisible to other users.

**Verified live**: the child instance's socket lands at
`$XDG_RUNTIME_DIR/Arora_1000_1000` — directory `0700`, socket `0600`.

**Honest threat note**: the endpoint is reachable by *any process
running as the same OS user* — an unauthenticated local open-URL
primitive exists by design (that is what the single-instance protocol
is).  The damage is bounded by the untrusted-source gate: a same-uid
attacker can navigate the browser and tickle the consent prompt, but
cannot turn the socket into script execution or a silent external-app
launch.  Cross-user access is denied by the `0700` directory + `0600`
socket + `UserAccessOption` at bind.

### (f) Forwarded URL into a private instance

A forwarded operand resolves `mainWindow()` → `tabWidget()` →
`loadStringFromUntrustedSource(url, openLinksFromAppsIn)`.  With
private browsing on:

- `NewSelectedTab`: the new tab is created via `makeNewTabLike()`
  from the OTR source tab → lands on the **OTR profile**.
- `NewWindow`: `newMainWindowInContainer()` sees
  `BrowserApplication::isPrivate()` → the spawned window is OTR.

The forward can never downgrade onto the persistent profile — the
receiving context decides, and it is at least as private as the
sender's intent.

**Coverage**: `setPrivate(true)` + forward `about:blank` lands an OTR
tab; `openLinksFromAppsIn=NewWindow` spawns an OTR window.

## Changes

- `src/browserapplication.cpp`: `openUrl()` gated by
  `WebView::isUrlAllowedOnUntrustedInput()` (item c).
- `src/main.cpp`: `--ingress-smoke` — 29 assertions over argv,
  socket, openUrl, tor-standalone, parser, socket-permission and
  private-forward lanes; registered as an internal option and auto
  picked up by the SMOKE01 settings-isolation (`--*-smoke` suffix).
- `.devin/check-coverage.sh`: `--ingress-smoke` added to
  `SMOKE_FLAGS`.

## Verification

- `QT_QPA_PLATFORM=offscreen ./arora --ingress-smoke` → 29/29 PASS,
  exit 0.
- Children run with hermetic `HOME`/`XDG_*` — the real profile and the
  real runtime socket are untouched (SMOKE01 store guard also
  enforced in-process).
- `make check` result recorded in the task row.
