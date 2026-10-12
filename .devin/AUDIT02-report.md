# AUDIT02 — Post-queue code-quality sweep on master

Date: 2026-10-12 · Scope: `src/` (C++ + rustcore surface), master after the
TRUST chain, SENG02, ENG12, SESS02, READ03/04, REFER01 landed.

Method: targeted reads on the recently-landed surface plus mechanical
sweeps — `#if 0` / commented-code grep, qrc↔disk both-direction check,
forms (.pri FORMS)↔disk check, header-declaration↔call-site count across
`src/` + `autotests/`, timer/EventLoop/process-wait inventory.

## (a) Dead code

### Fixed

| Item | Evidence |
|------|----------|
| `AutoFillManager::attachToPage` | Declared `autofillmanager.h`, defined `autofillmanager.cpp` — zero callers anywhere. Superseded by `scheduleOnPage` (named per-page QWebEngineScript at DocumentReady), the only injection path `webpage.cpp` uses. Removed decl + impl. |
| `AdBlockRequestInterceptor::clearBlockedRequestCounts` | Marked "// test hook" but no test calls it — the interceptor test diffs `blockedRequestCount` before/after instead of resetting. Removed decl + impl. |
| `FingerprintProtector::exceptionHosts` | Dead getter — zero callers. Removed decl + impl. |
| `PrivacyRequestInterceptor::isPingTelemetryRequest` | Static wrapper over `NavigationPolicy::isPingTelemetry` — both real call sites (rust + fallback pipelines) consult `NavigationPolicy` directly. Removed decl + impl. |

### Reviewed, kept (intentional)

- `networkcookiejar.cpp:235` — `#if 0`-disabled cookie path check. Upstream
  Qt WebKit code kept with the Firefox-parity comment that explains *why*
  the check is off; removing it loses the rationale next to the decision.
- No commented-out code blocks found (grep pass over all `src/*.cpp`).
- No orphaned `.ui` files — `FORMS` in .pri files matches disk exactly.
- No orphaned qrc assets — every `<file>` entry resolves, and every file
  under the qrc-covered dirs is listed (the `data/*.png` hits are covered
  by `data/graphics/graphics.qrc` / `data/searchengines/searchengines.qrc`;
  `arora.1`/`.xpm`/install icons are desktop-install assets, not resources).
- MIG06/MIG07-flagged statics (`s_historyManager`, `s_bookmarksManager`)
  already cleaned — confirmed gone.

## (b) Duplication — C++ vs rustcore twins

| Twin pair | Verdict |
|-----------|---------|
| `NavigationPolicy::rewrittenReferer` (C++ fallback) vs `policy.rs rewritten_referer` | **Aligned.** Both post-REFER01: EngineDefault verbatim, Never empty, https→http empty, cross-site empty at Strict, else the SOURCE's bare origin. `referer_origin` in Rust replicates QUrl's default-port drop (test `referer_origin_drops_default_ports`). The C++ copy is the documented `!ARORA_RUSTCORE` fallback — an intentional twin, not drift. |
| `refererForRequest` vs `referer_directive` | **Aligned.** Same gate order: max(policy, minLevel) → level 0 keep → non-http(s) keep → empty header keep → non-http referer keep → set. |
| `DomainBlocklist` / `TrancoList` | **No twin drift.** C++ side is fetch+commit only (HTTPS guard, one redirect hop, 64MB cap); matching/probing lives entirely in rustcore (`blocklist::check`, `rc_tranco_rank`). |
| XBEL read/write vs `bookmarks.rs` | **No twin drift.** Store load/save is Rust (`rc_bm_load`/`rc_bm_save`/`rc_bm_load_mem`). The C++ `XbelReader`/`XbelWriter` only handle the drag-and-drop mime fragment path (`application/bookmarks.xbel`) — a subtree-fragment shape the store FFI doesn't expose. `xbelreader.cpp` already defers to `rc_xbel_check` for validation. |
| OpenSearch parse vs `opensearch.rs`/`parsers.rs` | **Aligned.** Rust build: `rc_opensearch_parse` is canonical (Qt sees only JSON). C++ fallback: DTD rejected outright, 1MB input cap, `<Alias>` bounds (64×128) identical to the store's `MAX_KEYWORDS`/`MAX_KEYWORD_LEN`. |

## (c) Performance

| Path | Assessment |
|------|------------|
| Per-request JSON FFI (`NavigationPolicy::evaluate` → `rc_policy_evaluate`) | **Design item — filed PERF01.** Every interceptor-visible request marshals a JSON manifest and parses a JSON verdict on the IO thread. That is the ENG02 architecture (engine-neutral policy), not an accident — batching/caching identical (url, firstParty, rtype) verdicts within a page load changes semantics and needs design. Flagged, not fixed. |
| `rc_ua_spoof` per allowed request | Cheap hashmap lookup in the crate; allocation-light. OK. |
| `isSiteWhitelisted` → `subscriptions()` on `refreshIcon` | `load()` memoizes after first call; subsequent calls are a SiteDecisionStore hash read. OK. |
| Sync I/O on UI thread | Bounded and deliberate: print drain QEventLoop (15s cap, post-dialog), `ContainerManager::removeStorageTree` retry loop (5s cap, user-initiated container deletion, documented Chromium-teardown race), `PdfDocumentClient::probeWorker` (test seam, bounded). |
| Repaint storms | `SiteShieldButton::refreshIcon` on loadStart/progress/finished does a pixmap wash per call — progress ticks are coarse, trust badge is cached per host+scheme+probes. OK. |
| Timer storms | Inventory clean: 500ms tab-preview debounce, 200ms omnibox debounce, 15s sleep-timer, singleShots in harness paths. No high-frequency periodic timers. |
| O(n²) host matching | Exception lists are small user-defined sets (linear scan fine); the adblock matcher is snapshot+index based (`matchLinear` over pageRules index), domain blocklist is a Rust set. No O(n²) on the nav path. |

## (d) Security — recently-landed surface

| Area | Assessment |
|------|------------|
| REFER01 referer semantics | Verified in both impls: strict-origin-when-cross-origin — the SOURCE's bare origin cross-site at Trimmed, nothing at Strict, https→http always stripped, level 0 verbatim. No target-origin spoof left. |
| TRUST03 gating | **Verified.** `SitePanel::siteTrustVerdict` and `SiteShieldButton::trustLevelFor` both early-out on `!siteTrustEnabled()` *before* `rc_sitetrust_lookup_opts` — feature-off makes zero FFI calls (counter seam proves it). Per-signal toggles ride a live-read probe bitmask; badge cache key carries the mask so flips re-evaluate. |
| SENG01/02 descriptor parsing | **Verified hardened.** 1MB input cap, DTD rejected outright (kills the whole entity-expansion surface), `<Alias>` bounded 64×128, Rust parse canonical in rust builds. Image payloads capped 1MB on both paths (`data:` decode and `reply->size()` gate before `readAll`). |
| READ03/04 reader plumbing | Title/byline/siteName `toHtmlEscaped`; article body arrives sanitized from the Rust extractor (no script eval in the page world — serialized-DOM → `rc_readability_probe`); overlay view shares the page's profile (interceptor context preserved); JS-disabled lift is bounded with a 5s fallback restore. |
| ENG08–10 engine plumbing | **One finding, fixed** (below). |

### Fixed: Servo callbacks unguarded teardown + cross-thread writes

`ENG12` gated only `cbWake` on `g_liveView` — the other four callbacks
(`cbFrameReady`, `cbUrlChanged`, `cbTitleChanged`, `cbLoadStatus`)
dereferenced the raw `this` userdata unguarded and wrote members
(`m_image`, `m_url`, `m_title`, `m_loading`) from a Servo thread while
the GUI thread read them. A callback racing `se_destroy` was the same
UAF class ENG12 fixed; the member writes were additionally plain data
races (e.g. `grabFrame`'s `m_image` write vs `paintEvent`).

Fix: all four now take `servoMutex()`, gate on `g_liveView == view`
identically to `cbWake`, and bounce the real work through
`QMetaObject::invokeMethod(..., Qt::QueuedConnection)` — the queued
call is dropped if the receiver dies between gate and delivery, and
FIFO ordering preserves the signal sequence. `cbConsole`/`cbNavigation`/
`cbResourceLoad` need no gate (userdata unused).

### Noted, not fixed

- **PERF01 (filed):** per-request JSON verdict marshaling on the IO
  thread — architectural, needs design (verdict memoization per page
  load). See new task row.
- **Bundled-icon name collision (cosmetic):** `OpenSearchEngine::loadImage`
  keys the bundled favicon on the engine *name* — a user-added engine
  named e.g. "Bing" gets the bundled Bing icon instead of its own
  `imageUrl`. Confusing, not dangerous; icon is cosmetic only.
- **Image-fetch blind SSRF:** a malicious descriptor's `imageUrl` could
  probe internal hosts — strictly weaker than what the same descriptor
  already permits via its search URL; the feature is consented install.
  Documented here, no action.

## Fixes shipped

1. `dead-code`: removed 4 uncalled functions (autofill attach, adblock
   test hook, fingerprint getter, ping-telemetry wrapper).
2. `security`: gated the 4 remaining Servo callbacks on the live-view
   slot + GUI-thread bounce.

## Verify

`make check` — see WORKLOG entry for the gate results.
