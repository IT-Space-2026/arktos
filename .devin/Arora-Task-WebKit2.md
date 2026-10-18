# Devin autonomous task list — Arktos WebKit lane B (parallel research/tools)

This file is the task queue for the SECOND WebKit lane's Devin CLI task
loop (`.devin/taskloop-webkit2.sh`), running in parallel with the main
WebKit lane (`arora-webkit`, branch `webkit-work`, tasks WK00–WK21).
This lane carries the work that does NOT need the WPE backend skeleton
or even the WPE libraries: pure-code converters, research/audit docs,
packaging scripts, and test fixtures the spine will consume later.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting work, `done` only after
  building/verifying per the task's own verification hint.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- Never revert unrelated working-tree changes.
- Everything lands on branch `webkit2-work` in this worktree
  (/home/szefi/Documents/arora-webkit2). The spine lane merges it later;
  keep all new code self-contained (new files/dirs, minimal edits to
  shared sources) so the merge is clean.

Context: Arktos is Qt 6.12.0 / QtWebEngine. The WebKit engine being
added on the spine lane is WPE WebKit (wpe-webkit-2.0, WPEPlatform API)
— NOT QtWebKit, NOT WebKitGTK. Design study:
`.devin/WEBKIT-STUDY.md` — READ IT FIRST. This lane's outputs feed the
spine's WK17/WK18 (adblock), WK19 (devtools flag), WK20 (perf), and
WK21 (Tor/WebExtensions/inspector audit).

Qt toolchain: ~/Qt/6.12.0/gcc_64 — `source .devin/qt-env.sh` before
qmake/make. NO sudo, ever. Headless: QT_QPA_PLATFORM=offscreen —
never touch the user's live display.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| WB01 | EasyList→content-blocker JSON converter — standalone filter-rule→WebKit JSON converter + unit test; the hard half of spine WK18 — details below | pending | 0 | needs:linux |
| WB02 | WebExtensions research doc — what WebKitWebExtension (2.52+) actually offers vs chrome.*, vs WK21's probe — details below | pending | 0 | — |
| WB03 | SOCKS5/Tor plumbing audit doc — WebKitNetworkProxySettings surface + leak vectors + test methodology for WK21 — details below | pending | 0 | — |
| WB04 | Inspector/devtools research doc — WebKitWebInspector embed options + protocol delta vs our CDP devtools window — details below | pending | 0 | — |
| WB05 | WPE packaging strategy — how the .so set ships inside our deb/rpm/flatpak + dependency manifest scripts — details below | pending | 0 | needs:linux |
| WB06 | Spike fixture set — the static test pages and input/eval fixtures WK03/WK04 will consume — details below | pending | 0 | — |

## Task details

### WB01
EasyList→WebKit content-blocker JSON converter — the hard half of spine
task WK18, fully testable without WPE. WebKit's adblock path is
`WebKitUserContentFilterStore` + a JSON rule list (Safari content-
blocker format: `trigger.url-filter` regex + `resource-type`,
actions `block`/`block-cookies`/`css-display-none`). Write a converter
`src/adblock/contentblocker/` (new dir, self-contained .h/.cpp +
qmake .pri) that takes our adblock rule model (read
`src/adblock/adblockrule.{h,cpp}` for the existing parser's output
shape — reuse it, don't re-parse EasyList) and emits the JSON:

- Covered: `||domain^` url-filter patterns → Safari `url-filter`
  regexes; resource-type restrictions (script/image/stylesheet…);
  `@@` exception rules → `ignore-previous-rules`; `##selector`
  cosmetic rules → `css-display-none` with `if-domain`.
- Dropped (counted, logged, listed in output metadata): `$redirect`,
  `$rewrite`, `$csp`, `$removeparam`, `elemhide` exceptions needing
  scriptlet injection, regex filters Safari can't express. Never
  silently drop — the converter returns {rules, dropped:[{rule,
  why}]}.
- Unit test: `autotests/contentblocker/` — a fixture .txt of real
  EasyList patterns; assert converted counts, a known-good conversion
  per rule class, and the drop list contents.

Verify: `make -C autotests/contentblocker` + run — legs pass;
converter prints rule/drop counts for a real EasyList fixture.

### WB02
WebExtensions research doc → `.devin/WEBKIT-EXTENSIONS.md`. Answer
precisely: (a) what `WebKitWebExtension` in WPE 2.52+/2.54 actually is
(native GObject API vs manifest.json loader — verify against upstream
docs/source, not assumptions); (b) can ANY manifest.json+JS-worker
extension load on WPE today (expected: no); (c) the delta between
WebKit's model and what the extruntime lane's hosted runtime would
need to run over WebKit primitives — a cross-lane note for Option B
of the extension study; (d) honest verdict for WK21's
"WebExtensions probe" row. Sources: WebKitGTK/WPE API docs, upstream
NEWS/release notes, the WebKit source tree if reachable.
Verify: doc committed; the four questions each have a sourced answer
or an explicit "unknown — requires source build to verify".

### WB03
SOCKS5/Tor audit doc → `.devin/WEBKIT-TOR.md`. For WK21's leak audit:
(a) `WebKitNetworkProxySettings`/`webkit_network_proxy_settings_new`
surface — socks:// support, per-context vs per-WebContext scoping,
whether proxy is settable per session/profile (needed for per-profile
Tor like Chromium's isolation); (b) leak vectors to enumerate and how
to test each: DNS (does WPE resolve via the proxy or libc?), WebRTC
(does WPE even ship WebRTC? state it), captive-portal/GIO proxy
auto-detect leaks, fetch/XHR vs subresource paths, extension/host
traffic escaping the proxy; (c) the concrete test methodology the
spine will run (local SOCKS5 fixture + packet/connection assertions);
(d) verdict: is Tor-on-WebKit even a candidate, or documented out.
Verify: doc committed with a checklist WK21 can execute verbatim.

### WB04
Inspector/devtools research doc → `.devin/WEBKIT-DEVTOOLS.md`. (a)
`WebKitWebInspector` embed surface: can it attach to a WPE view, does
it need its own toplevel, `inspector://` scheme handling; (b) protocol
delta: WPE's inspector speaks the WebKit protocol — map against what
our existing devtools window (CDP-based) consumes — is it pluggable
or a second implementation; (c) WPE WebDriver (WebKitWebDriver) as an
alternative surface — what it gives for free; (d) recommendation for
the `devTools` capability flag on the WebKit backend: false-first vs
inspector-embed path.
Verify: doc committed; capability recommendation stated plainly.

### WB05
Packaging strategy — `.devin/WEBKIT-PACKAGING.md` + concrete scripts.
The deployment blocker (Ubuntu 24.04 ships no WPE) needs an answer per
package format. Deliver: (a) a dependency-manifest script
(`.devin/wpe-deps.sh`) that given a WPE prefix or Debian .deb set lists
the required .so closure and which of them our target distros already
provide; (b) per-format analysis: deb (bundled private libs under
/usr/lib/arktos/wpe vs Depends-on-distro-packages — tradeoffs,
SONAME risk), rpm (same question Fedora-side where WPE is packaged),
flatpak (runtime-extension or bundled modules in our manifest — the
flatpak dir already exists); (c) recommended strategy + the concrete
manifest/script changes staged but not applied to live packaging
(live changes land on the spine lane after the backend exists).
Verify: script runs and emits a sane dep list against a local
fixture dir; doc committed.

### WB06
Spike fixture set — `spikes/webkit/fixtures/` committed: the static
pages the spine's spike tasks (WK03/WK04) will drive —
`blank.html` (minimal, stable paint), `paint-proof.html`
(deterministic rendered content — colored blocks + text whose pixels
are assertable in a QImage screenshot), `input-click.html` (a link
that navigates on click + a button that records event coordinates),
`eval-proof.html` (page exposing a known value via JS for the typed
eval leg), `nav-target.html` (click destination with marker
title/URL). Each file self-contained, no external resources, with a
comment header naming the WK task that consumes it.
Verify: files committed; each is valid self-contained HTML (checked
by parsing, not by eyeballing).
