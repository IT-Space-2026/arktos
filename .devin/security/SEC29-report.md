# SEC29 — PDF sanitize-pipeline audit

Scope: `src/rustcore/src/pdfsanitize.rs` (the structural rewrite —
the real trust boundary), `src/pdfsupport.cpp` (fetch → sanitize →
stage → view), `src/pdfworker.cpp` + `src/pdfview.cpp` (confined
rasterizer and its host), `src/webpage.cpp` (navigation
interception), `src/webview.cpp`/`webviewsearch.cpp` (shell-view
integration), `src/rustcore/src/pdfdoc.rs` (hayro raster layer).
Method: line-level adversarial review plus probe fixtures exercised
through `cargo test pdfsanitize` and the headless `--pdf-smoke` /
`--pdfview-smoke` harnesses.

## Pipeline shape

Main-frame `.pdf` navigation (http/https/file) → refused by
`WebPage::acceptNavigationRequest` → `PdfSanitizeFetch` downloads the
body through the page's own profile (cookies/Tor SOCKS/container
ride along) → `PdfSupport::sanitize()` runs the Rust lopdf rewrite →
sanitized bytes written to the managed temp dir → either the
shell-owned `PdfView` (bwrap-confined `arktos --pdf-worker`, hayro
raster → PNG tiles + text search) or, with `privacy/pdfShellView`
off, a `file://` load into Chromium's PDFium.

Structural rewrite confirmed, not raster/flatten: `Document::load_mem`
parses the object graph, dictionaries are recursively rewritten with
`MAX_DEPTH = 128`, `Document::save_to` reserializes a standalone file.
Page content/structure survives; the worker rasterizes *sanitized*
bytes and never sees the original (input cap 256 MiB; corrupt or
oversize input degrades to a refusal code, never unbounded work).

## Findings

### F1 — Pipeline failed open: unsanitizable files were re-fetched raw — FIXED

`PdfSanitizeFetch::serveBytes`/`fail()` recorded a one-shot bypass
and reloaded the original URL, so a sanitize failure — and worse,
fetch interruption/cancellation/timeout — handed the raw body to
PDFium. A hostile endpoint could *force* this: abort the staging
download mid-body, then serve clean bytes to the second fetch.
Split: `refuse()` is now the terminal path for sanitize/staging/
transfer failures (drops staged bytes, shows the standard error
page, no bypass); `fail()`'s raw reload survives only for bodies
that provably are not PDFs, which render as their true type.

### F2 — External-document actions survived the rewrite — FIXED

`/S` values `GoToR`, `GoToE`, `GoToDp`, `Thread`, `RichMediaExecute`
and `GoTo3DView` passed through untouched — remote-document fetch,
embedded-document jump and rich-media execution surfaces. Added to
`DANGEROUS_ACTIONS` (action dict emptied in place).

### F3 — Embedded-file payloads reachable off the name tree — FIXED

`EmbeddedFiles` inside `/Names` was stripped, but `/EF` payload maps
inside FileSpec dictionaries were not — a `FileAttachment`
annotation carries its own `/FS` spec independent of the document
name tree, so embedded bodies survived. `EF` joined `STRIP_KEYS`;
`FileAttachment`, `RichMedia`, `Screen`, `Sound`, `Movie`, `3D`
annot subtypes now drop the whole annot dict (markup annots stay).

### F4 — Orphan/dead objects reserialized into output — FIXED

lopdf keeps every parsed object; a payload referenced only through a
just-stripped key — or an outright orphan / stale incremental-update
body — was written into the sanitized file byte-for-byte. A trailer-
rooted reachability sweep (`reachable_ids`) now drops every object
the Root/Info/Encrypt chain cannot follow before `save_to`.

## Verified clean

- Pre-existing strips hold: `JS`/`JavaScript`/`AA`/`OpenAction`/
  `EmbeddedFiles`/`XFA` keys; `JavaScript`/`Launch`/`SubmitForm`/
  `ImportData`/`Movie`/`Sound`/`Rendition` action types; trailer-
  level copies of the same keys.
- Fetch rides the page profile (`fetch went through profile network`);
  viewer-side `file:`/`dir:`/`arktos-file:` links refused,
  `https:` allowed; sanitize-off and non-PDF-`.pdf` fallback legs
  unchanged.
- Shell view confined: worker probe shows `$HOME` and the data dir
  hidden, `/usr` read-only, work dir mounted; staged file read-only.
- `PdfView::documentFailed` (a staged doc hayro cannot open) now
  also fails closed — the overlay is dropped and the refusal error
  page shows instead of a silent blank view (both `WebView` and
  `WebViewWithSearch` hosts).

## Residual limits (documented, not fixed)

- PDF served at a non-`.pdf` URL is not intercepted — the embedder
  API exposes no response headers pre-body; MIME-sniffed PDFs keep
  the stock path.
- POST PDF responses are not intercepted — the body cannot be
  replayed safely.
- Download/save-as intentionally preserves raw bytes — sanitization
  is a viewing control.
- `looksLikePdfBytes` scans the first 1 KiB; lopdf scans the whole
  buffer. A body whose `%PDF-` header sits deeper takes the true-type
  fallback — PDFium's own header search is comparably bounded, so it
  still does not render as a PDF.
- The sanitizer removes active/embedded surfaces structurally; it
  does not flatten. Static content (ink, fonts, images, form values)
  renders as authored — a cosmetic overlay attack on page content is
  out of scope, as is any hayro parser bug (mitigated by the bwrap
  confinement, not eliminated).

## Verification

- `cargo test pdfsanitize`: 5/5 — including new adversarial legs
  (`strips_external_and_embedded_surface`,
  `drops_unreachable_objects`).
- `--pdf-smoke`: PASS — new legs `viewed pdf strips GoToR /
  FileAttachment / RichMedia / FileSpec / EmbeddedFile / evil`,
  `unsanitizable pdf refused`, `no raw reload on sanitize fail`
  (single server hit on the corrupt body).
- `--pdfview-smoke`: PASS (also pinned `privacy/pdfShellView=true`
  in-harness so a stale `--pdf-smoke` setting cannot silently route
  the e2e leg through the engine fallback; `::_exit` on finish
  sidesteps a pre-existing offscreen RWHV-delegate teardown crash,
  and the ctrl-F leg now drives `pressKey` like the Escape leg —
  the window-level Find shortcut eats `sendEvent` before the widget
  sees it).
