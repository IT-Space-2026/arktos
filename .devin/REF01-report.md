# REF01 — Referer hardening report

Scope: `https://www.darklaunch.com/tools/test-referer` showed "not
everything is good".  PRIV01's `trimReferer` only rewrote cross-site
*navigation* referers; subresource referers fell back to Chromium's
built-in `strict-origin-when-cross-origin`, which still reveals the
referring site's origin to every third party.

## Mechanism (what ships)

`privacy/refererPolicy` level selector (replaces the PRIV01
`trimReferer` bool — migration: `false`→EngineDefault, else→Trimmed;
the bool is still written in sync so older builds map sensibly):

| Level | cross-site | same-site |
|-------|-----------|-----------|
| Chromium default | engine policy (source origin) | engine policy (full URL!) |
| Trimmed (default) | **target's own origin** (uBO referrer-spoof) | source origin only |
| Strict | nothing | source origin only |
| Never | nothing | nothing |

Two enforcement layers:

1. `PrivacyRequestInterceptor::applyRefererPolicy()` — rewrites the
   renderer-computed `Referer` header visible in `httpHeaders()` for
   EVERY request class.  Contrary to the stale PRIV01 comment,
   `setHttpHeader("Referer")` DOES reach the wire for subresources on
   Qt 6.12 (verified on the wire, loopback two-site matrix).  An
   absent header stays absent — rel=noreferrer links, stricter
   page policies and https→http downgrades are never handed a referer
   the sender asked to withhold.
2. `BrowserProfile::installReferrerPolicy()` — a named
   `QWebEngineScript` (DocumentCreation, ApplicationWorld, all
   frames) injecting `<meta name="referrer">` into pages that don't
   set their own (page's own meta parses later and wins).  Covers the
   legs the header rewrite cannot reach: **redirect follow-ups** —
   Chromium recomputes the Referer for a 30x follow-up from the
   redirect chain's stored referrer AFTER the interceptor ran, so a
   `setHttpHeader` on that leg is silently dropped (verified: the
   rewrite is applied, the wire still carries the engine value).
   Meta values: Trimmed→`strict-origin`, Strict→`same-origin`,
   Never→`no-referrer`.

Tor profile floors at Trimmed (`applyRefererPolicy(info,
RefererTrimmed)` + the meta floor in `installReferrerPolicy`), even
when the user picked EngineDefault for normal profiles.

## Per-scenario results (`--referer-smoke`, two loopback "sites" 127.0.0.1 / 127.0.0.2)

Trimmed level — wire-observed Referer:

| Scenario | Before (engine default) | Now |
|---|---|---|
| direct nav / link click | full page URL | `http://target/` (target origin) |
| cross-site img/script/iframe/XHR subresource | source origin | `http://target/` (spoof) |
| same-site subresource | full page URL (path+query leak) | source origin only |
| 302 chain end, cross-site | source origin | source origin — **residual, engine-bound** (see below) |
| 302 chain end, same-site | full page URL | source origin (meta-covered) |
| meta refresh cross-site | source origin | target origin |
| form POST nav cross-site | source origin | target origin |
| rel=noreferrer link | none | none (sender intent respected) |
| page meta `no-referrer` | none | none (page policy wins) |
| `Referrer-Policy: no-referrer` header | none | none |

Never level: every request — nav, subresource, redirect follow-up —
arrives with no Referer at all (meta `no-referrer` covers redirect
legs where the rewrite can't reach).

## Decisions & honest bounds

- **(d) same-site full path**: darklaunch flags full-path
  same-site referers (query-param leaks to your own analytics/CDN
  endpoints).  All hardened levels now send only the origin
  same-site — decided as part of the feature, not left to Chromium.
- **Residual (documented)**: at Trimmed, a cross-site *redirect
  follow-up* still reveals the source's ORIGIN (never path/query) —
  Chromium recomputes that leg past the interceptor and `same-origin`
  is the only meta value that would suppress it, which would also
  kill the target-origin spoof on the legs we can write.  Strict/Never
  close it fully.  Missing API: a way to mutate the carried referrer
  of a redirect chain (Qt's `QWebEngineUrlRequestInfo` exposes the leg
  but header writes are dropped on it).
- Pages with an explicit referrer policy keep it — we floor, not
  override.
- Hotlink-check compat: Trimmed keeps sending *a* Referer
  (target-origin) unlike strict blockers, so referer-checking image
  CDNs keep working.

## Harness notes (fixed en route)

- SIGTRAP during smoke: GPU-thread CHECK under offscreen+i915 —
  unrelated to referer logic (reproduced with rewriting disabled);
  `LIBGL_ALWAYS_SOFTWARE=1` avoids it.  Never ran the bare-binary
  repro path again — it was already shown clean.
- Hermeticity bug: the user's real EasyList subscriptions block the
  fixture's bare `/img` paths — both referer-smoke and the older
  privacy-smoke now neuter adblock subscriptions for the run
  (`restoreAdBlockStateOnExit` restores them).
- `document.appendChild(meta)` at DocumentCreation (before `<html>`
  exists) creates a second document element and breaks parsing —
  attach only to head/documentElement, MutationObserver otherwise.

## Verification

- `--referer-smoke`: phase-1 trimmed PASS (15 checks), phase-2 never
  PASS (incl. n-redir→n-final redirect chain) — offscreen,
  `LIBGL_ALWAYS_SOFTWARE=1`.
- `--privacy-smoke`: PASS (img subresource now arrives and carries
  target-origin referer).
- `tst_privacy` 48/48 (new: refererPolicyMigration,
  refererRewriteMatrix incl. meta mapping, downgrade rules).
- `make check` — see commit notes.
