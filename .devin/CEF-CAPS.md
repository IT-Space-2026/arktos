# CEF-CAPS — capability-split design for a `cef` backend (CS03)

Status: **design only — no code.** This doc drafts how a CEF backend
reports its two embed shapes honestly through the capability contract.

Reviewed against the real signatures (2026-10-25, this tree):

- `Engine::Capabilities` — 11 bits, `engineinterface.h:595-607`
- `Backend::capabilities()/initialize()/pageUnavailableReason()/
  engineNote()` — `engineinterface.h:612-650`
- Servo's runtime-conditional precedent —
  `servobackend.cpp:1717` (`caps.requestInterception = resolve() &&
  api().setLoadGate`, symbol-probed at resolve time)
- `ac_engine_caps` facade row schema — `rustcore/src/caps.rs`
  (`{"v":1,"engine","display","backend_ids","features"}` with
  per-feature `{state,note}`; `partial` without a note is a test
  failure — the note IS the honesty mechanism)
- Qt-side consumption — `enginecaps.cpp` (`featureUsable`: facade row
  wins under rust, adapter bit is the fallback),
  `engineindicator.cpp:104-159` (menu rows: `pageUnavailableReason` +
  `engineNote` + `capabilitySummary`),
  `browsermainwindow.cpp:2547` (`updateEngineIndicator`: engine id +
  context locks only — it never reads caps bits)

Measured inputs (never re-derived here):

- `CEF-EXT-SPIKE.md` — `SetAsWindowless` forces
  `CEF_RUNTIME_STYLE_ALLOY`; the extension API is supported on
  chrome-style windows only (CEF issue 3685). `SetAsChild` accepts
  `CEF_RUNTIME_STYLE_CHROME`.
- `CEF-BW-VERDICT.md` (CS01) — bundled Bitwarden 2026.9.3 verified on
  the chrome runtime: SW + popup + storage + messaging + scripting.
  Gaps: `nativeMessaging` absent *(CS12 corrected: present,
  permission-gated)*, `privacy`/`identity` unverified,
  `sidePanel` inset unverified.
- `CEF-INPUT-VERDICT.md` (CS02) — windowed chrome shape embeds via X11
  `SetAsChild` reparent; 9/11 input legs PASS with embedder-owned
  geometry, asymmetric focus bridging, `force-device-scale-factor`
  for HiDPI, and Tab never escaping the view.

## 1. The split, stated precisely

Runtime style is a **per-window** property (`CefWindowInfo`), not a
process property — since M128 the bootstrap is always Chrome. But a
backend's *embed choice* is constrained by the display server:

| Shape | `runtime_style` | Platform reach | Extension runtime |
|---|---|---|---|
| **windowed** `SetAsChild` | `CHROME` | X11 reparent verified (CS02); **no Wayland path** — a foreign window cannot be reparented there | Full MV3 surface verified (CS01) |
| **OSR** `SetAsWindowless` | `ALLOY` — **forced by CEF, not chosen** | Every platform incl. Wayland, offscreen | **Chrome-UI surfaces dead** (toolbars, sidePanel, chrome://extensions); RequestContext-level ext functionality *should* work per maintainer — unverified (CEF#3859) |

Consequence: `extensions` is a **shape property**, not an engine
property. On an X11 session the backend can produce either shape; on
Wayland it can only produce OSR, so `extensions=false` is forced from
outside. A single flat `Engine::Capabilities` cannot express "true for
some of this backend's pages" — the bits are backend-global and are
consulted before `createView` ever runs.

The rest of the capability surface is shape-independent: request
interception, cookies, downloads, cert errors, devtools, scheme
handlers all live on the browser/context handlers, not on the render
mode (CEF-SPIKE §c non-gaps).

## 2. `Engine::Capabilities` for `cef`, per shape

Grades marked **[m]** = measured in this lane's probes; **[e]** =
expected from the CEF handler surface, unverified at runtime here.

| Bit | windowed (chrome-style) | OSR (alloy) | Ground |
|---|---|---|---|
| `requestInterception` | ✔ [e] | ✔ [e] | `CefResourceRequestHandler` — stronger than Qt's interceptor (CEF-SPIKE §c) |
| `scriptInjection` | ✔ [m] | ✔ [m] | `CefFrame::ExecuteJavaScript` verified live (CS02 `__snap`); the declarative-collection semantics are shim work (gap G1), but the bit certifies "scripts reach pages" — they do |
| `scriptEvaluation` | ✔ [e] | ✔ [e] | result channel needs the V8/process-message round-trip (G2 plumbing); `ExecuteJavaScript` itself is fire-and-forget — graded ✔ as a port target, flagged G2 |
| `cookieFilter` | ✔ [e] | ✔ [e] | `CefCookieAccessFilter` / resource-handler consult |
| `perPageSettings` | ✔ [e] | ✔ [e] | `CefBrowserSettings` is per-browser at create time; runtime flips unverified |
| `lifecycleDiscard` | ✘ [e] | ✘ [e] | no CEF equivalent (gap G5) |
| `contextMenuInfo` | ✔ [e] | ✔ [e] | `CefContextMenuHandler` hit-test params; OSR menus are embedder-presented anyway |
| `certificateOverride` | ✔ [e] | ✔ [e] | `CefRequestHandler::OnCertificateError` → continue |
| `devTools` | ✔ [m] | ✔ [m] | CDP over `--remote-debugging-port` verified (CS01 drove CDP) — the BiDi panel's protocol; embedded inspect is separate-window only (`ShowDevTools`, gap G4) |
| `extensions` | **✔ [m]** | **✘ [m]** | **the split bit** — CS01 verified on chrome-style; on alloy the UI surfaces die by design while RequestContext-level may still run (CEF#3859, unverified) — cap reporting should reflect the UI-surface verdict |
| `downloads` | ✔ [e] | ✔ [e] | `CefDownloadHandler` → `Engine::DownloadRequest` parity |

**Headline: exactly one bit differs between shapes.** The other 10 are
identical — the split collapses to `extensions` plus the embed-burden
differences (CS02's geometry/focus/HiDPI contract), which are
integration cost, not capability bits.

## 3. Recommended reporting design — one `cef` id, shape pinned per session

The simplest honest shape that fits the existing contract untouched:

1. `CefBackend::initialize()` resolves `m_shape` **once** from the real
   session: `QGuiApplication::platformName() == "xcb"` →
   `Shape::ChromeWindowed`; `wayland`/headless/else → `Shape::Offscreen`.
   An explicit env override (`ARKTOS_CEF_SHAPE`) pins either for
   testing. This is the `resolve() && api().setLoadGate` pattern
   applied to a different runtime question: measured at init, never
   assumed.
2. `capabilities()` returns the per-shape matrix above with
   `caps.extensions = (m_shape == Shape::ChromeWindowed)`.
   `--engine-list` (`main.cpp:6454`) then prints the live shape verdict
   for free — the same line that today exposes Servo's absent gate.
3. `displayName()`/`engineNote()` carry the shape — e.g.
   `"Chromium (CEF 152)"` + note `"offscreen shape — extensions
   unavailable (CEF runtime limit)"` on OSR, or `"X11 windowed embed —
   extensions supported"` on the chrome shape. `engineNote` is already
   surfaced in the indicator tooltip and menu rows (ENG29 plumbing,
   `engineindicator.cpp:81-86,132-138`) — zero chrome work.
4. `pageUnavailableReason()` stays shape-independent (empty unless
   `CefInitialize` failed — a dead backend refuses the page outright,
   ENG19 contract).
5. **`updateEngineIndicator` needs no change** — it reads `tabEngineId`
   (`"cef"`) and context locks (tor/private/container), never caps
   bits. The shape caveat reaches the user through `engineNote` in the
   tooltip and the caps summary line in menu rows. The indicator's
   job is engine identity + swap availability, both per-backend.

### The consumer rule — the one place the split could lie

`EngineCaps::featureUsable()` lets the **facade row win** when rust is
present (`enginecaps.cpp:103-110`), and a `partial` grade answers
*usable*. If `cef.extensions_mv3` grades `partial` in caps.rs (correct
— the ceiling is real), a facade-reading consumer would light
extension chrome on a **Wayland** session where the adapter bit is
`false`. Rule for the port:

> Extension-chrome gating (`caps.extensions` — the §10/MS06 surface)
> MUST read the **adapter bit** `capabilities().extensions`, not the
> static facade row. The caps.rs row states the ceiling; the bit
> states this session's shape. Same discipline Servo's
> `request_interception` consumers already follow: the static
> `partial` row never overrides the adapter's live `false`.

That is a consumer-site constraint, not a contract change — the two
surfaces already coexist by design ("the two capability surfaces MUST
NOT diverge" means neither overstates the other, and a `partial` +
note honestly describes exactly this conditional).

## 4. caps.rs row (draft)

One `EngineRow`, engine id `"cef"`, `backend_ids: &["cef"]`, display
`"Chromium (CEF)"`. The 15 required features plus the primitives, with
the split encoded in notes — no schema bump needed, the `note` field
is the designed honesty channel:

```rust
const CEF_FEATURES: &[Feature] = &[
    ("devtools", State::Supported,
     "CDP over --remote-debugging-port verified on this lane; \
      embedded inspect is a separate ShowDevTools window, no \
      in-pane devtools"),
    ("pdf_viewer", State::Partial,
     "PrintToPDF parity expected but unverified; the engine-neutral \
      PdfView shell hosts staged documents on any backend"),
    ("extensions_mv2", State::Unsupported,
     "MV2 rejected at install, same as the qtwebengine row"),
    ("extensions_mv3", State::Partial,
     "runtime-conditional on the resolved view shape: chrome-style \
      windowed embeds (X11 SetAsChild reparent) host the full MV3 \
      surface — Bitwarden 2026.9.3 SW+popup+storage+messaging+\
      scripting verified; nativeMessaging present (CS12 corrected \
      "absent" → permission-gated hide; works unpatched), \
      privacy/identity \
      unverified, sidePanel inset unverified. OSR/alloy shape \
      (forced on Wayland) loses all Chrome-UI extension surfaces \
      (CEF#3685+#3859; RequestContext-level may survive, unverified) \
      — the adapter's capabilities().extensions \
      carries this session's live verdict"),
    ("webchannel", State::Unsupported,
     "no QWebChannel; bridges need CefV8Handler + process-message \
      plumbing (CEF-SPIKE gap G2)"),
    ("otr_profile", State::Supported,
     "transient CefRequestContext; the private-tab engine lock in \
      updateEngineIndicator is chrome policy, not an engine limit"),
    ("tor", State::Unsupported,
     "the Tor lane is WebEngine-bound by wiring (ENG05); CEF can \
      carry SOCKS5 via CefRequestContextSettings.proxy — unwired"),
    ("containers", State::Supported,
     "per-container CefRequestContext on ac_profiles_* paths — the \
      facade owns the layout (CS04 verifies)"),
    ("reader_mode", State::Unsupported,
     "reader driver rides runJavaScriptLifted + the private channel \
      world — no isolated-world surface outside the extension \
      system (G1/G2)"),
    ("downloads", State::Supported,
     "CefDownloadHandler → DownloadRequest, pause/resume parity"),
    ("sessions", State::Partial,
     "CefNavigationEntry stack exists; the serialized blob format \
      is WebEngine-bound — cef tabs restore at flat url until the \
      port maps it"),
    ("favicons", State::Supported,
     "CefDisplayHandler::OnFaviconURLChange"),
    ("opensearch", State::Supported,
     "app-side opensearch registry"),
    ("site_permissions", State::Partial,
     "CefPermissionHandler's prompt set is coarser than \
      QWebEnginePermission's (CEF-SPIKE gap G5); covered types \
      marshal into the same WebPermissionManager broker"),
    ("autofill", State::Unsupported,
     "the arktosAutofill bridge is QWebChannel-bound (G2)"),
    // Engine primitives — the Engine::Capabilities bits verbatim.
    ("request_interception", State::Supported,
     "CefResourceRequestHandler consults the profile RequestPolicy \
      — a full resource-handler pipeline, stronger than Qt's \
      interceptor"),
    ("declarative_blocking", State::Unsupported,
     "policy gates requests live; no compiled-filter lane on this \
      backend either"),
    ("script_injection", State::Partial,
     "per-frame ExecuteJavaScript verified; the declarative \
      collection's injection-point/world/subframe semantics are \
      shim work (CEF-SPIKE gap G1)"),
    ("script_evaluation", State::Partial,
     "ExecuteJavaScript is fire-and-forget; the result channel \
      needs the V8/process-message round-trip (G2)"),
    ("cookie_filter", State::Supported, ""),
    ("per_page_settings", State::Partial,
     "CefBrowserSettings is create-time per browser; runtime flips \
      unverified"),
    ("lifecycle_discard", State::Unsupported,
     "no CEF equivalent (CEF-SPIKE gap G5)"),
    ("context_menu_info", State::Supported,
     "CefContextMenuHandler hit-test params — expected, unverified \
      in probes"),
    ("certificate_override", State::Supported,
     "OnCertificateError continue path — expected, unverified in \
      probes"),
];

// ENGINES gains:
EngineRow {
    engine: "cef",
    display: "Chromium (CEF)",
    backend_ids: &["cef"],
    features: CEF_FEATURES,
},
```

Honesty note on the grades: every `[e]` bit from §2 lands here as
either `Partial` or a `Supported` row whose note says *unverified in
probes* — nothing is graded up on the strength of CEF documentation.

## 5. Alternatives considered

- **Two backend ids, `cef` + `cef-osr`, sharing one `CefContext`** —
  two `addProbe` registrations, `initialize()` idempotent behind
  `CefInitialize`-already-done. Caps rows become trivially static and
  the selector *is* the shape picker. Rejected as the default: it
  doubles switcher rows for a choice almost nobody wants, and the
  session-pinned design already tells the truth. It stays the clean
  upgrade path if per-session shape mixing is ever required — no
  contract change, just a second registration.
- **Per-`Page` capabilities** — would let one process mix chrome-style
  ext tabs and OSR tabs. Rejected: caps plumbing is backend-keyed
  end to end (caps.rs rows, `EngineCaps` cache, indicator, MS06
  consumers); threading a per-page verdict through `EngineTab` →
  chrome gating is contract churn a spike must not pretend is free.
  Flagged as the design prerequisite if mixed-shape sessions become a
  requirement.
- **Pin one shape forever** — always-windowed loses every Wayland
  session (and Wayland is where the embed is cleanest to *not* fake);
  always-OSR throws away the extension runtime, the only CEF-unique
  driver that justified this lane. The conditional shape is the honest
  middle.

## 6. Carried into CS05 (verdict inputs)

- The split is real but narrow: **one bit** (`extensions`) plus
  embed-side burdens. The capability contract absorbs it without
  changes — the honest surfaces are `capabilities()` (live shape
  verdict), `engineNote` (user-facing caveat), and the caps.rs `note`
  (the static ceiling).
- Verified gaps stay verbatim in the row's notes: `nativeMessaging`
  ~~absent~~ **present** (CS12: permission-gated hide misread — full
  stack verified unpatched on prebuilt, both runtimes);
  `privacy`/`identity`/`sidePanel`-inset unverified;
  `scriptEvaluation`'s result channel and `perPageSettings` runtime
  flips unverified — none of these gets claimed.
- CS04 (sandbox/personas) doesn't change this design; the
  `containers`/`otr_profile` grades assume the `CefRequestContext`
  model CS04 verifies.
- If the port ever mixes shapes in one session, per-page caps (§5)
  becomes a prerequisite — record it in the integration-cost table.
