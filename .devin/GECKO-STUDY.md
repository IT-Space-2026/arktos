# GECKO-STUDY — Gecko as a fourth engine backend (study only)

Date: 2026-10-20 · Status: study, no code · Scope: "keep everything,
add Gecko the way Servo and WebKit were added" — a new
`Engine::Backend` implementation, registered in `EngineRegistry`,
hosted by `EngineTab`, capability-gated exactly like `ServoBackend`
and `WebkitBackend`. Companion to `.devin/WEBKIT-STUDY.md` and the
engine matrix in `.devin/ENGINE.md`.

## What Gecko actually is today (verified, not remembered)

- mozilla-central (hg.mozilla.org/mozilla-central) is alive — daily
  commits, Gecko is still Firefox's engine. Servo never replaced it;
  instead Servo's pieces were upstreamed INTO it (Stylo/CSS in FF57
  Quantum, WebRender, assorted Rust components).
- There is **no supported desktop embedding API**. The old
  XULRunner/embedding surface was removed around 2016; Mozilla's
  documented position is "embedding is not supported." Internals are
  free to change every 6-week release train.
- `GeckoView` exists but is **Android-only** — it powers Fenix. A
  desktop GeckoView was prototyped; it is not a product surface.
- The monolithic artifact is `libxul.so` — the whole engine in one
  library, but with no stable C ABI and no promise of symbol
  stability between versions. Linking it directly is building
  Firefox, not embedding it.

So the honest framing: Gecko cannot be *used* as a library the way
WPE WebKit or libservo can — it can only be *wrapped*.

## The one live precedent: EmbedLite (Sailfish OS)

Jolla's Sailfish OS browser embeds Gecko in production phones —
the proof this shape works:

- **EmbedLiteComponents** — an out-of-tree embedder layer carried as
  a patchset/component inside mozilla-central. It exposes a narrow
  embedder API (EmbedLiteApp → EmbedLiteView): create views, get
  compositor surface callbacks (texture or shared-memory buffers),
  feed synthesized input events, receive navigation/title/security
  signals. Gecko's own multiprocess model runs underneath; the
  embedder layer rides its IPDL/compositor machinery.
- **qmozembed / embedlite-qt** — the Qt binding half: an embedlite
  view renders offscreen and the Qt side composites the delivered
  buffer into QML — the same "engine hands us pixels, our widget
  paints them" model our ServoView and WebkitView already use.
- **Cost shape**: they track ESR releases (~yearly), carry the
  embedder patchset forward each ESR bump, and own the maintenance.
  That is the price of no API stability — measurable, not fatal.

Our `servo-embed`/`wpe-harness` pattern maps directly: a
`gecko-embed` shim — a component + small C ABI built inside the
Gecko tree, dlopen'd by `GeckoBackend`; artifact absent → capability
hides.

## The five integration questions

1. **Which surface** — our own minimal embedder API modeled on
   EmbedLite (option A below). GeckoView-desktop and raw libxul are
   rejected; external control (Marionette) is not embedding.
2. **Pixels into QWidget** — EmbedLite-style compositor callback:
   offscreen layer manager hands texture ids (GL path) or shmem
   buffers (raster path) per frame — same staging as WebKit's
   `import_to_pixels`/`import_to_egl_image` (raster first, GL
   texture as the perf leg).
3. **Event loop** — Gecko wants its own event dispatch
   (nsAppShell + MessageLoop). EmbedLite drives it via an explicit
   start + pump calls; we'd host the pump on the GUI thread exactly
   like `se_spin`/the GMainContext verdict (WK02) proved works.
4. **Profiles/storage** — Gecko profiles are plain directories
   (profiles.ini + a profile dir: cookies, storage, cert db). A
   `GeckoProfile` = an engine-managed profile dir under
   `profiles/<id>/Gecko` — the same home-dir isolation contract as
   the other backends. Per-view OTR maps to Gecko private-browsing
   windows (a real per-context concept there).
5. **Build/dist** — the real gate. mozilla-central checkout ≈
   30–40GB; `mach bootstrap` + `mach build` = a Rust+C++ hybrid
   build measured in hours, not minutes; the artifact set is
   libxul + dependencies ~hundreds of MB. Strategy mirrors WK00:
   pin an ESR tag, user-local prefix (`~/gecko`), dlopen/probe, hide
   when absent. Distro packaging (flatpak/deb/rpm bundling) is a
   WKB05-scale question of its own — likely bundle-the-artifact.

## Capability mapping (Engine::Capabilities)

| Capability | Gecko outlook |
|---|---|
| Navigation/history/zoom/find | all exist in the embedder layer (EmbedLite proves) |
| Per-profile storage | native — profile dirs, our strongest story after WebKit |
| JS eval | via embedder evaluate calls — typed results possible |
| Script injection/worlds | content-script machinery exists (the WebExtension internals); exposing it = embedder-layer work |
| Request interception | WebRequest exists for extensions; embedder-side network veto needs the same embedder API work — likely `requestIntervention` partial |
| **WebExtensions** | **THE unique win — native runtime.** Gecko ships the complete WebExtensions implementation (MV2 complete, MV3 landing). Bitwarden officially supports Firefox. `moz-extension://` schemes, real background pages, real content scripts — no hosted-runtime needed for Firefox-targeting extensions (manifest deltas exist but the core is shared lineage with Chrome's). Only engine where extensions are engine-native. |
| **Tor** | **The other unique win — Tor Browser IS Firefox ESR.** SOCKS5 + remote DNS (`network.proxy.socks_remote_dns`) is the most-audited proxy path in any shipping engine; proxy prefs are per-profile (matches our Tor-window-per-profile model); First-Party Isolation exists as a pref. No audit checklist needed — the lineage is the audit. |
| Devtools | Firefox Remote Debugging Protocol — and WebDriver BiDi (shipping since ~FF120) means our BidiPanel may work nearly as-is |
| PDF | pdf.js is in-tree; embeddability in a chrome-free view = verify |
| Media | Linux Firefox uses ffmpeg — works, adds a runtime dep |

## Options

### Option A — our own `gecko-embed` layer (EmbedLite-modeled) — recommended if ever

A component in the Gecko tree + a small stable C ABI shim (the
`servo-embed` pattern at 10x scale): view create/destroy, frame
callbacks, input injection, nav signals, profile handle, eval.
Proven by Sailfish; sized by "port EmbedLite's approach to our
contract" not "invent embedding."

### Option B — wait on desktop GeckoView — rejected as a plan

A desktop GeckoView may or may not ever ship; building on an
unshipped API is not a lane.

### Option C — link libxul directly — REJECTED

No stable symbols, ABI churn per release, XPCOM lifecycle rules
we'd violate accidentally. This is "build Firefox," not embed an
engine.

### Option D — external control (Marionette/WebDriver BiDi over a
stock Firefox) — REJECTED for tabs

Control ≠ embedding: no pixels in our widgets, no per-tab views.

## Honest risks

- **API stability**: the embedder patchset rides internal IPDL/
  compositor machinery — every ESR bump can break it; Jolla visibly
  pays this cost. Budget it as recurring per-ESR work, not a
  one-time port.
- **Build weight**: 30–40GB tree, hours-long builds, mach/Rust
  toolchain quirks vs our qmake flow — CI and dev-machine cost is
  the largest of any engine by an order of magnitude.
- **Distribution**: no distro ships an embeddable Gecko; we bundle
  libxul+deps — biggest artifact of the three alternative engines.
- **XPCOM ergonomics**: comPtr/interface plumbing in C++ is uglier
  than WebKit's glib API — the shim absorbs it, but it is real work.
- **NSS, not OpenSSL**: certs live in per-profile cert9.db — fine
  for us, different from both other backends' TLS model.

## Work breakdown sketch (GK series, if ever queued)

- GK00: acquire — ESR-pinned mozilla-central checkout, `mach
  bootstrap` + `mach build`, user-local `~/gecko` prefix, version
  probe. The WK00 equivalent at ~10x the cost.
- GK01–05: embedder-layer spike — minimal `gecko-embed` API
  (EmbedLite-modeled), event-loop verdict, render→QWidget raster
  proof, input+eval smoke, verdict doc (the WK02–05 arc).
- GK06+: backend build-out — same Engine::Backend surface the
  WK06–19 spine filled.
- Stretch: WebExtensions runtime wiring (engine-native Bitwarden),
  Tor profile mode (the strongest Tor story of any engine), BiDi
  devtools binding.

## Recommendation

**Feasible, proven, and the most expensive engine option.** The
EmbedLite precedent makes it *proven* rather than speculative —
real phones ship it. Its two unique wins are real: only Gecko gives
engine-native WebExtensions and the Tor-Browser proxy pedigree.

Sequenced last for honest reasons: the two wins are partially
covered already — our hosted `chrome.*` runtime runs Bitwarden E2E
today, and Tor stays functional on Chromium. Gecko would be the
*upgrade* on both axes, not the unblock. If the project ever wants
a fourth engine, this study is the map; until then it stays a
document.
