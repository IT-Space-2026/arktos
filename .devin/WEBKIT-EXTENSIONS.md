# WEBKIT-EXTENSIONS — what "WebExtensions" means on WPE (research only)

Date: 2026-10-18 · Status: study, no code · Feeds the webkit spine's
WK21 "WebExtensions probe" row and the extruntime lane's Option B
(`.devin/EXT-RUNTIME-STUDY.md`). Scope question: does WPE 2.52+/2.54
give us a *native* WebExtension (manifest.json + JS) runtime, and if
not, what does our hosted `chrome.*` runtime need from WebKit
primitives?

Answer checked against the published 2.52.0 and 2.54.1 (latest stable,
Oct 2026) API references and upstream bugs/PRs — not assumptions.

## Three different things called "extension" — disambiguate first

WebKit GLib APIs have **three** distinct extension surfaces; conflating
them is the easy mistake:

1. **Web process extensions** — `wpe-web-process-extension-2.0`
   (pkg-config), namespace `WPEWebProcessExtension`. A *native* GObject
   `.so` the embedder compiles and drops into a directory set via
   `webkit_web_context_set_web_process_extensions_directory()`; it runs
   **inside the WebProcess** with DOM/world access (`WebKitWebPage`,
   `WebKitFrame`, `WebKitScriptWorld`, `WebKitWebFormManager`) and talks
   to the UI process over `WebKitUserMessage`. This is the OLD
   `WebKitWebExtension` — **renamed `WebKitWebProcessExtension` in
   2.40** precisely to free the name. Still fully supported in 2.54.
   NOT a manifest.json loader.
   Sources: wpe-webkit.org `wpe-web-extension-2.0` reference index;
   WebContext methods `set_web_process_extensions_directory`,
   `send_message_to_all_extensions`, signal `initialize-web-process-extensions`.

2. **`WebKitWebExtension` (UI process, since 2.52)** — the new class in
   `wpe-webkit-2.0` itself. See (a) below. Despite the recycled name it
   is unrelated to (1): different library, different process, different
   purpose.

3. **The embedder-built runtime** — what Epiphany does (below). The app
   parses manifests, injects a `browser.*` JS shim, hosts background
   content in hidden web views marked with the `web-extension-mode`
   property, and bridges content scripts via a (1)-style web process
   extension. No WebKit "extension runtime" is involved.

## (a) What `WebKitWebExtension` in WPE 2.52+/2.54 actually is

`WebKit.WebExtension` — `final class`, `GObject`, implements
`GInitable`, since 2.52, in `wpe-webkit-2.0` (the UI-process lib — NOT
the web-process-extension lib). Per its own docs: "encapsulates a web
extension's resources that are defined by a `manifest.json` file ...
handles the reading and parsing of the manifest file along with the
supporting resources like icons and localizations."

It is a **manifest.json parser + resource accessor**, and in 2.52/2.54
that is ALL it is. The full exported surface:

- `webkit_web_extension_new(path)` — parse a folder containing
  `manifest.json`; `GError` on invalid/missing manifest.
- metadata getters: `display-name/-short-name/-description/
  -action-label/-version`, `default-locale`, `version`,
  `manifest-version`, `supports_manifest_version` (both MV2 and MV3
  manifests parse).
- capability queries: `has-background-content`,
  `has-persistent-background-content`, `has-injected-content`,
  `has-content-modification-rules`, `has-commands`,
  `has-options-page`, `has-override-new-tab-page`.
- permission introspection: `requested-permissions`,
  `requested-permission-match-patterns`,
  `optional-permissions`, `optional-permission-match-patterns`,
  `all-requested-match-patterns` (typed
  `WebKitWebExtensionMatchPattern`, since 2.52, with
  `WebExtensionMatchPatternOptions` flags and its own error quark).
- `icon`/`action_icon` rasterization at a requested size.

Adjacent public API:
- `WebKitWebView:web-extension-mode` (construct-only property, enum
  `WebKitWebExtensionMode`: NONE/MANIFESTV2/MANIFESTV3, **since 2.38**)
  — marks a view as extension content; per the binding docs "in
  practice this limits the Content-Security-Policies allowed" (the
  MV2/MV3 extension-CSP relaxation). This is the pre-existing hook
  Epiphany's runtime uses for background/popup/options pages.
- `WebKitWebExtensionError` quark — parse errors only.

**What is NOT there**: grepped the full wpe-webkit-2.0 class/method
indexes for both 2.52.0 and 2.54.1 — there is no
`WebExtensionContext`, no `WebExtensionManager`, no
`WebExtensionController`, no extension `Tab`/`Window`/`Action`
objects, and `WebKitWebContext` has no `load`/`unload`/`get` extension
methods (its only extension-named APIs are the *web-process* ones from
surface (1)). Same result on the webkitgtk-6.0 index at 2.54.1 — the
GTK port does not have the runtime half exported either.

Backing: the class wraps `WebKit::WebExtension` from
`Source/WebKit/Shared/Extensions/` — the same cross-platform C++
implementation that powers Safari's `_WKWebExtension`. The engine is
real and complete upstream; only the GLib binding is partial.

The missing half exists upstream but **unmerged**: WebKit bug 300631 /
PR webkit/webkit#52242 "[GTK] Create WebKitWebExtensionContext"
(opened 2025-10-13, still open + labeled `merging-blocked` as of
2026-09; ~4.3k LOC adding `webkit_web_extension_context_*`,
context→settings/webview hooks, and a GLib `WebExtensionContext` impl
gated on `ENABLE(WK_WEB_EXTENSIONS) && ENABLE(2022_GLIB_API)`; uses a
`webkit-extension://` base URI). Follow-up PR #69832 (options-page
API, 2026-07) splits the work further. Both patches touch
`SourcesWPE.txt`/`PlatformWPE.cmake` — WPE is in scope for the same
code once it lands, and WPE 2.5x always builds the 2022 GLib API, so
the gate is satisfied on WPE — but nothing is committed.

- 2.52.0 release notes: "Added initial API for web extensions."
- 2.54.0 release notes: no further extension API (highlights are the
  Skia compositor, favicon API, per-navigation UA, magnification).
- Sources: wpewebkit.org/reference/2.52.0 + /stable (2.54.1)
  `class.WebExtension.html`, `enum.WebExtensionMode.html`,
  `class.WebContext.html`; webkitgtk.org 2.52/2.54 release notes;
  github.com/WebKit/WebKit/pull/52242, /pull/69832;
  bugs.webkit.org/show_bug.cgi?id=300631.

## (b) Can any manifest.json + JS extension actually load on WPE today?

**Via the native API: no.** With only the parser exported, an
extension can be validated and inspected but never *loaded* — there is
no API to create its extension context, host its background content,
inject its content scripts, or vend its action. Expected answer
confirmed, with a sharper edge: it is not "MV2 only" or "subset", it
is *parse-only* on both GLib ports through 2.54.1.

**Via an embedder-built runtime: yes — Epiphany proves the pattern.**
GNOME Web has shipped optional WebExtension support since Epiphany 43
(2022), built entirely app-side: its own manifest handling, a
`browser.*` JS polyfill injected into extension contexts, hidden
background views (`web-extension-mode`), and a web-process extension
(`.so`, surface 1) bridging content scripts. Modeled on Firefox MV2;
supports a meaningful subset — `alarms`, `commands`, `cookies`,
`downloads`, `menus`, `notifications`, `storage`, `tabs`, `windows` —
with `webRequest` notably absent and MV3 unfinished. It is
developer-only (off by default; Catanzaro: "arbitrary extensions are
not going to work... not being actively developed"). Sources:
blog.tingping.se/2022/06/29/WebExtensions-Epiphany.html;
discourse.gnome.org/t/18024 (Catanzaro, 2023-11).

So the honest statement: **WPE can host real extensions only if the
embedder supplies the runtime — which is exactly the architecture our
EXT14+ hosted runtime already uses.** Nothing about WPE blocks it; the
blocker is the same as on QtWebEngine — the runtime must be ours.

## (c) Delta — our hosted chrome.* runtime over WebKit primitives (Option B)

EXT-RUNTIME-STUDY's Option B asks whether the EXT14 runtime could run
on `Engine::Page` primitives. Mapped onto WPE's actual surface:

| hosted-runtime need | WebKit primitive | verdict |
|---|---|---|
| hidden background host page (MV3 SW stand-in) | headless `WPEView`/`WebKitWebView` + `web-extension-mode=manifestv3` (CSP handling free) | OK — equivalent to our hidden `QWebEnginePage`; WebView suspend/resume may even model SW lifetime better |
| `chrome.*` shim into extension pages | `WebKitUserContentManager` `add_script`/`add_script_in_world` at DocumentStart | OK — and **named script worlds** give real isolated worlds; `webkit_web_view_evaluate_javascript_in_world` evaluates into them (stronger than main-world `runJavaScript`) |
| content-script ↔ background channel | `register_script_message_handler_in_world` + **`_with_reply` since 2.40** (`script-message-with-reply-received` → `WebKitScriptMessageReply`, async-capable) | OK — a better fit than our QWebChannel bridge; maps onto the arktos-channel transport (WK16 already assumes this) |
| `scripting.executeScript` / dynamic injection | `evaluate_javascript(_in_world)` | Partial — JS string eval only (no file/func form), main-frame scoped per call; per-frame fan-out needs the web-process side (`WebKitFrame` JS contexts in a `.so`) or per-frame message relay |
| tabs/windows/webNavigation/idle events | chrome-side signals (`load-changed`, `notify::uri/title`, `create`/`ready-to-show`) marshaled by us | OK — engine-agnostic work, identical cost profile to QtWebEngine |
| `webRequest` observe/mutate | `decide-policy` (navigation observe), content-blocker JSON store (declarative block subset — WK18) | Gap — same as QtWebEngine: no per-request mutate; keep honest observe-only/`declarativeBlocking` story |
| extension storage/cookies/downloads/notifications/alarms/contextMenus | `WebKitWebsiteDataManager`, `WebKitCookieManager`, `WebKitDownload`, `WebKitNotification`, our own registry/QTimer | OK — same shape as the Chromium-side handlers |
| extension page scheme | `webkit_web_context_register_uri_scheme` (upstream native impl uses `webkit-extension://`; our compat layer can keep `chrome-extension://`) | OK |
| MV3 service-worker semantics | none native; a persistent hidden view is the approximation (Epiphany does the same) | Documented limitation, same as today |

Net delta vs Option A-on-WebEngine: **near-parity, tilted positive.**
Two primitives are *better* on WebKit (named worlds +
promise-returning script messages); two are *worse* (eval granularity:
string-only, and real per-frame injection needs a web-process `.so` we
don't currently plan to ship). The load-bearing conclusion of the
study — "the runtime must be ours either way" — is reinforced, not
revised: even when upstream `WebKitWebExtensionContext` lands it will
be a *different* runtime (Safari-parity surface, not `chrome.*`
parity), so Option B's value stays "our runtime, engine-neutral
primitives."

One freebie to reuse: if wpe ≥2.52 is resolved, `WebKitWebExtension`
can serve as a *manifest validator/permissions source-of-truth* in the
webkit backend — optional sugar; our own manifest handling stays the
portable path.

## (d) Verdict for WK21's probe row

- `caps.extensions` on the webkit backend: **native WebExtension =
  not available** at any released WPE version (parse-only API since
  2.52, runtime API unmerged upstream). Record it as a measured fact,
  not a guess.
- The *probe* itself is cheap: `dlsym`/`nm` for
  `webkit_web_extension_new` (present ≥2.52 → parser available) and
  confirm the absence of `webkit_web_extension_context_*` symbols →
  native runtime absent. Add a `WEBKIT-EXTENSIONS.md`-verifying
  autotest leg only if a cheap symbol check exists; otherwise version
  gate + doc reference is enough.
- Capability honesty: distinguish "no native extension runtime" from
  "hosted runtime can port here" — the second is a real, favorable
  property of this engine (worlds + script-message-reply + extension
  CSP mode). If the Capabilities table gains an extension field, the
  honest value is `HostedRuntime` (portable), not `None`.
- Watch item: upstream PR #52242 / bug 300631 — if the
  `WebKitWebExtensionContext` API lands in a future WPE (2.56+),
  re-evaluate whether native MV2/MV3 hosting beats maintaining our
  runtime on this backend. Until merged+released it is vapor for
  planning purposes.

## Sources

- WPE API refs: wpewebkit.org/reference/2.52.0/wpe-webkit-2.0/ and
  /stable/ (2.54.1) — `class.WebExtension.html`,
  `enum.WebExtensionMode.html`, `struct.WebExtensionMatchPattern.html`,
  `class.WebContext.html`, `error.WebExtensionError.html`; web-process
  side: wpewebkit.org/reference/stable/wpe-web-extension-2.0/ (namespace
  `WPEWebProcessExtension`).
- WebKitGTK 2.52.0 / 2.54.0 release notes — webkitgtk.org
  (2026-03-18, 2026-09-16).
- Upstream runtime work: github.com/WebKit/WebKit/pull/52242
  ("[GTK] Create WebKitWebExtensionContext", open, merging-blocked),
  /pull/69832 (options-page split), bugs.webkit.org/show_bug.cgi?id=300631;
  context impl gated on `ENABLE(WK_WEB_EXTENSIONS) &&
  ENABLE(2022_GLIB_API)` and uses `webkit-extension://` base URIs.
- Embedder-built precedent: blog.tingping.se "WebExtension Support in
  Epiphany" (2022-06-29); GNOME Discourse t/18024 (support is
  developer-only, partial, not actively developed).
- Script message reply: `register_script_message_handler_with_reply`
  since 2.40 (webkitgtk.org webkit2gtk reference; also corrects the
  "2.5x" note in the WK16 row).
- Cross-refs: `.devin/WEBKIT-STUDY.md`, `.devin/EXT-RUNTIME-STUDY.md`
  (Option B), `.devin/EXT11-report.md`, task rows WK16/WK18/WK21.
