# SEC27 — Reader-mode sanitizer audit

Scope: `src/readersanitizer.cpp` (the C++ fragment filter — the real
trust boundary), `src/readermode.cpp` (both render paths), `src/data/
reader.js` + `reader-forward.js` (the isolated-world driver, overlay
and channel relay), `src/rustcore/src/readability.rs` (the Rust-path
pre-sanitize). Method: line-level adversarial review plus a live
QtWebEngine probe binary answering "what origin/CSP does the output
document actually get".

## Pipeline shape

extract (isolated world / Rust) → `ReaderSanitizer::sanitize` in C++
(raw-text-aware tokenizer, allowlist elements, attribute policy,
independent re-serialization — source markup is never copied through)
→ rendered either in a dedicated `ReaderOverlayPage` QWebEngineView
(`setHtml(doc, baseUrl)`, ARKTOS_RUSTCORE) or a sandboxed `<iframe
srcdoc>` inside a shadow host (no `allow-scripts`, Qt fallback).

## Findings

### F1 — The Rust-path reader document ran with the source page's own origin and full JavaScript — FIXED

Probe (Qt 6.12, offscreen): `QWebEngineView::setHtml(doc, baseUrl)`
commits a `data:text/html,…` navigation but the resulting document
reports `window.origin == "https://example.com"` — the *base url's real
origin*, not an opaque one — with `JavaScriptEnabled` on and no CSP.
An injected `<img onerror>` fired. So every byte the sanitizer missed
would have executed as first-party script on the source origin — and
on JSCTL/Safer pages where page script was deliberately blocked, the
reader surface would quietly re-enable it.

Fix: `readerDocument()` now emits `<meta http-equiv="Content-Security-
Policy" content="default-src 'none'; img-src * data:; media-src *
data:; font-src * data:; style-src 'unsafe-inline'; base-uri 'none';
form-action 'none'">`. Re-probed: inline `<script>`, inline event
handlers and `javascript:` navigations are all refused
(`window.__pwned` stays `undefined` through load and a scripted click),
while app-side `runJavaScript` still executes — the theme toggle's
live-apply is unaffected. `reader.js`'s `frameDoc()` carries the same
policy for the Qt fallback path (its sandbox already refuses scripts;
the CSP kills fetch/base/form surfaces too).

Residual, honest: `setHtml` offers no opaque-origin mode — the doc
*is* the source origin; the lockdown is sanitizer + CSP + the nav
gate, not an origin boundary. Same for the JS path's
`allow-same-origin` srcdoc (kept so the chrome can restyle).

### F2 — Navigable URL attributes allowed every non-js/data scheme — FIXED

`keepAttribute` only blocked `javascript:`/`vbscript:`/`data:` on
href-class attributes. A hostile article could therefore emit
`<a href="file:///etc/passwd">`, `<a href="arora-file:///…">`,
`<a href="qrc:/…">`, `<a href="chrome://…">`, `<a href="about:…">`,
`<a href="view-source:…">` and — worst — `<a href="blob:https://
victim/uuid">`: a blob: URL created by the source page's own JS still
resolves in the same profile, and clicking it opens stored HTML
*in the source origin* — script execution bought for the price of
markup. The Rust overlay's nav gate actively made `file:` worse by
routing it to the real view's `loadUrl`, which performs local file
navigations a renderer never would.

Fix: navigable attrs (`href`, `xlink:href`, `action`, `formaction`,
`longdesc`, `usemap`, plus `cite` now) run a scheme *allowlist* —
schemeless (relative/fragment/query, cannot mint a scheme) or
`http https mailto tel callto sms ftp ftps cid xmpp`. Resource attrs
(`src poster background dynsrc lowsrc`) allow schemeless + `http`,
`https`, `data`. `ReaderOverlayPage::acceptNavigationRequest` now
routes `file:` links only when the article itself was `file:`-based —
local pages keep working links, remote pages can't click into the
filesystem.

Residual, honest: mailto:/tel: links stay inert in the Rust overlay
(the gate routes only http/https/file-local); the Qt fallback path
reaches the external-app consent prompt through the source page's nav
hook. Chromium's implicit `noopener` covers `target=_blank` popups in
both.

### F3 — SMIL animation elements survived the allowlist — FIXED

`animate`, `animateMotion`, `animateTransform`, `set`, `mpath` were
kept with unchecked `attributeName`/`to`/`from`/`values`/`by`
attributes. SMIL can rewrite a sibling's `href`/`xlink:href` to
`javascript:` or `data:` *after* the attribute pass — a runtime
payload the static check could never see. Removed from the kept set;
they now fall to transparent (tag dropped, children — if any — pass
the same filter). Articles carry no legitimate SMIL.

### F4 — `xml:base` kept — FIXED

`xml:base` on an SVG subtree rewrites relative-URL resolution inside
it — the same base-hijack class as `<base href>` (already a dropped
element). Now blocked by name alongside `ping`/`srcdoc`.

## Verified-clean items (no change needed)

- `meta refresh`, `base href`: `meta`, `base`, `link` are already
  void-drops; `iframe`, `object`, `embed`, `form`-adjacent wrappers are
  raw-text/container drops or transparent.
- mXSS: `annotation-xml` (the MathML integration point) is not in the
  kept set → transparent → its children get filtered as markup; the
  foreign-content self-close tracking (`isForeignElement`) only ever
  *under*-produces structure (breakout direction is always benign:
  kept elements re-parented, never unfiltered).
- Attribute pipeline: names validated to a safe charset before
  emission; all `<`-input is dispatched (comments, doctypes, PIs,
  bogus closes); emitted text runs cannot contain `<` by construction;
  attribute values re-escape `"` `<` `>` and keep entities verbatim so
  decode is identical on reparse; duplicate attributes resolve the
  same direction a browser takes (first wins — a dropped first leaves
  only the safe one).
- URL normalization: entity-decoded (`&colon;`, `&Tab;`, numeric refs
  incl. overflow → U+FFFD), ASCII whitespace+controls squashed
  (superset of what browsers strip), lowercased — then the scheme
  allowlist runs. `javascript:` cannot be spelled around it: any
  token with a valid scheme gets checked, anything else is a relative
  URL that inherits the base scheme.
- `data:` on navigable attrs was already dead and stays dead; `data:`
  on resource attrs is image/media-context only (SVG-in-`<img>` is
  script-inert).
- `reader-forward.js` relay: page-forged `arktos:reader:*` events are
  cosmetic (exit hint + two clamped preferences).
- Rust `readability.rs::sanitize` is a weaker blocklist by design —
  its output always re-passes the strict C++ pass before display
  (READ05 invariant re-verified in `ReaderMode::enter`).
- Title/byline/siteName are escaped (`toHtmlEscaped` / `esc()`); theme
  is a closed enum; font size is clamped numeric.

## Documented residual limits

- `style` attributes are kept (matches DOMPurify's default): Blink
  cannot execute JS via CSS; worst case is article-bounded UI
  redress/phishing — the attacker already controls all rendered
  content, and the real chrome bar is Qt widgets outside the document.
- `srcset`/`sizes` pass unchecked: they can only name image fetches —
  no script surface in Blink.
- `target=_blank` in the Rust overlay is a dead link
  (`createWindow` default) — pre-existing behavior.
- Foreign elements emitted outside their proper parent (e.g.
  `<circle/>` at top level) can absorb siblings into inert unknown
  elements on reparse — a content-hiding quirk, not a script surface.
- File: articles' absolute `file:`/`blob:` URLs are dropped (relative
  paths still resolve) — edge-case image loss, chosen direction.
- `readerDocument` CSP leaves `img/media/font-src *` open — articles
  need cross-origin figures; `default-src 'none'` still pins every
  script, fetch, frame, worker, object and connect surface.

## Verification

- Probe (`/tmp/sec27probe`): setHtml origin = base origin confirmed;
  with the shipped CSP meta, inline script + inline event handlers +
  javascript: click all refused; app `runJavaScript` unaffected.
- `tst_browsermainwindow readerSanitizer` — PASS incl. new legs:
  scheme-allowlist (file/arora-file/qrc/chrome/blob/about/view-source
  dropped; mailto/tel/ftp/relative kept), resource allowlist
  (file/blob dropped; data/https kept), SMIL drop, xml:base/cite.
- `tst_browsermainwindow readerModeContent` — all 5 legs (form, thin,
  danger, tamper, hostile) PASS offscreen under ARKTOS_RUSTCORE.
- `qmake && make -j2` clean, no new warnings.
