# SEC26 — Security re-audit of the new attack surface

Date: 2026-10-19 · lane: rust-work · scope: hosted `chrome.*` runtime,
WebKit backend injection paths, Servo shim ABI, cross-cutting IPC
gates. Prior context: WK21 (WebKit extension APIs are stubs, inspector
unavailable, `prefetch_dns()` resolves locally under proxy — embedder
gated, pages cannot reach it), SEC24 (isolated/main-world channel
split), ENG36 (per-view `SeInstance`), ER16/ER20, QUAL03.

## Findings

### SEC26-1 (fixed): extension chrome pages carried the ER20 JS lift without a navigation lock

`ExtensionActionBar::openPopup()` and the health-probe page in
`ExtensionManager::startNextHealthProbe()` used plain
`QWebEnginePage`. ER20 forces `JavascriptEnabled` on these extension
contexts, but nothing constrained where they could navigate — a popup
following a link/redirect, or a probe page reaching foreign content,
would run attacker JS under the extension-context exemption. The
hosted background page already carried QUAL03's lock; these two
surfaces did not.

**Fix:** new `ExtensionChromePage` (extensionbackgroundhost.h) —
main frame accepts only the extension's own
`chrome-extension://<id>` origin or `about:blank`; subframes accept
only the same extension origin. HTTPS, `file:`, `javascript:`,
`data:`, `about:blank` subframes, and foreign extension origins are
all refused. Applied to the action popup and the health probe.

**Test:** `extensionChromeNavLock` — full allow/deny matrix across
main-frame and subframe navigations for popup and probe pages.

### SEC26-2 (fixed): `relayPortConnect` trusted a caller-supplied `sender.url`

The relay built `MessageSender.url` from a string the untrusted page
side passed in, letting an extension context forge sender identity in
fan-out payloads. The fix derives `sender.url` from the bridge's
`contextPage()->url()` — the host-verified page URL — whenever a
context page is bound.

### SEC26-3 (fixed): unbounded relay bookkeeping + duplicate `mid` overwrite

Page-controlled message IDs fed `m_relayPending` with no bound and no
uniqueness check: re-sending a live `mid` silently replaced the
pending entry, orphaned its timeout, and created response races; the
map itself was a memory-growth DoS. `m_portEnds`/`m_portChans` had the
same unbounded shape. Fix: `kMaxRelayPending` = 512,
`kMaxPortChans` = 1024, `kMaxPortEndsPerChan` = 64; duplicate in-flight
`mid`s are rejected; port-end registrations deduplicate; a refused
connect never destroys existing valid endpoints.

**Test:** `extensionMessageRelay` gained legs for duplicate-mid
rejection, port-end dedup (`p1b` fresh registration then re-register),
and host-verified `sender.url`.

## Per-surface verdicts

### Hosted `chrome.*` runtime — PASS after fixes

- Host-permission scoping: ER16 gates scripting injection on manifest
  host permissions and frame coverage; tabs/windows payloads are
  scrubbed of URL-bearing fields unless `tabs` or a matching host
  permission exists. Verified intact.
- Consent honoring: navigation gate consults `RequestPolicy::decide`
  on every `decide-policy` dispatch; external-scheme consent (SEC02)
  is reached through the same path.
- Message validation: extension IDs are bound per `ChromeBridge`;
  relay ops re-verify `ownsRelayKey()`; cross-extension fanout is
  filtered on extension ID; `openOptionsPage` rejects absolute,
  scheme-bearing, and `..` paths. SEC26-2/3 close the remaining
  spoofing and resource-growth holes.
- ER20 JS exemption scope: now confined by `ExtensionChromePage`
  (SEC26-1) — the exemption cannot leak into normal pages because
  extension chrome pages cannot navigate off-origin.
- `syncPageBridge()` self-wires by scheme but `attachExtensionPage()`
  still validates profile, OTR state, extension ID, and profile
  identity — page adoption cannot bind a foreign context.

### WebKit backend — PASS, no new findings

- `evaluate_javascript` marshaling: `jscValueToVariant` dispatches on
  the actual JSCValue type tag (undefined→invalid, null→Nullptr
  metatype, bool/number/string scalars, objects→JSON text). No caller
  type is trusted; type confusion is not reachable.
- Custom schemes (`register_uri_scheme`): handlers serve fixed
  response bodies; `arktos-file:` is GET-only directory listing behind
  the SEC02 `FileAccessHandler`, marked Local/Secure explicitly;
  `file:` keeps no scheme handler.
- decide-policy veto: every navigation/response dispatch ends in an
  explicit `use`/`ignore`/`download`; `about:`/`data:`/`file:`/`blob:`
  fall through the same `RequestPolicy::decide` consult — no default
  accept path. Back-forward navigations bypass decide-policy by WebKit
  design (documented at line ~2112); the embedder policy client covers
  the gap.
- World isolation: `scriptWorldName` maps worldId 0 → main world,
  `ArktosChannelWorld` → a named world; `webkit.messageHandlers` only
  materializes in worlds the handler was registered into — the main
  world stays bare (SEC24).
- `arktosChannel` boundary: page-to-host messages arrive as JSCValue
  in the channel world only; replies marshal through the checked
  `variantToJsc` matrix.

### Servo shim ABI — PASS, no new findings

- Multi-view routing: every delegate call carries the view's
  `SeInstance` handle; `viewFor` resolves `webview.id` to the exact
  view — a view cannot reach a sibling's instance through the public
  API since handles are opaque and engine-owned.
- Pending-load queue (ENG36): `pending_load` is per-view
  `RefCell<Option<Url>>`; `se_spin` flushes every live view's queue
  explicitly — a queued load cannot flush into a sibling view or leak
  across `viewDestroy` (the queue dies with the instance).

### Cross-cutting — PASS

- Profile-data paths: `BrowserPaths` roots everything under
  `QStandardPaths::AppDataLocation`; persona dirs are minted only for
  non-OTR profiles; the shim's options-page path rejects `..` and
  absolute paths. No path escapes `$HOME`.
- Per-call gates: every new IPC seam (relay ops, shim invoke, channel
  messages, scheme handlers) re-checks its capability at dispatch —
  no ambient-authority paths found.
- Credentials/secrets: no new credential or secret handling anywhere
  in the audited surface.

## Verification

- `make check` 4 gates PASS: check-warnings (0 warnings, src+tools,
  no-rust config), check-supplychain (advisories/bans/licenses/sources
  clean, 3 crates), check-engine-audit (181 files documented),
  check-rustcoverage (94.11 / 91.78 / 89.25 % ≥ 85 % gate).
- `tst_extensionmanager`: 29/29 PASS offscreen, incl. new
  `extensionChromeNavLock` matrix and the three new relay legs.
- Full build: `qmake && make -j2` clean on Qt 6.12.
