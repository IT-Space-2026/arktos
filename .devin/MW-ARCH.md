# MW-ARCH — Arktos middleware bridge architecture (MB01)

The typed command/query/event surface between the Qt shell, the Rust
core (`src/rustcore`), and the engine adapters. This document is the
design contract the MB02–MB07 rows implement; where it and a code
comment disagree, this file wins (same rule as RUSTCORE-ABI.md).

Sources of record this doc was written against:
`src/rustcore/include/arktos_core.h` (facade ABI v6),
`src/rustcore/include/rustcore.h` (legacy `rc_*`/`rv_*` surface),
`.devin/RUSTCORE-ABI.md`, `.devin/RUSTCORE-ARCH.md`,
`src/rustcore/src/notify.rs` (the event lane as it exists today),
`src/rustcorebridge.{h,cpp}` (the Qt-side consumer).

## 1. Op classes

Every export on the `ac_*`/`rc_*`/`rv_*` surface is exactly one of:

- **query** — a synchronous read or pure decision. No durable or
  shared-observable state changes; safe to call concurrently on any
  thread (stores serialize internally). A query that fails changes
  nothing. Examples: `ac_settings_get`, `ac_policy_evaluate`,
  `rc_classify_input`. Pure transforms of caller bytes (parse gates,
  sanitizers, codecs, scorers) are queries — they own no store.
- **command** — a state change or an action. Writes a store, mutates
  scoped in-memory state (reload/sync/mark), submits work to a live
  channel (tor control command, BiDi command), or performs an
  irreversible act (seal, wipe). A mutating op that also returns data
  (`ac_containers_create` → row JSON) is still a command — the class
  describes the effect, not the return shape.
- **event** — a Rust→UI notification. Covers the *producing* halves
  the ABI exposes: callback registration points and the pull-drain
  lanes (`rc_tor_take_*`) that consume what a notification announced.
  Events are never requested through the request/response path.
- **lifecycle** — the substrate the spec's three classes don't
  enumerate but the ABI plainly contains: core init/shutdown, module
  handle mint/close, returned-buffer destruction, the process-global
  data anchor, and the thread-local error read. Forcing
  `ac_string_free` or `ac_shutdown` into "command" would pollute the
  taxonomy with ops that have no domain and no payload. Lifecycle ops
  stay flat FFI and are never dispatched through the envelope (§2).

Two deliberate edge calls, stated for honesty:

- `ac_cookies_for_url` is a **query** that may lazily delete expired
  rows as a side effect (RFC6265 lazy eviction, matching the Qt jar).
  The class describes the caller-visible contract, not the sweep.
- `ac_policy_call`/`rc_policy_call` is a JSON-named dispatch of its
  own; its class is inherited from the inner op it resolves to.

## 2. Wire format: one envelope entry point — `ac_mw_dispatch()`

**Decision: a single versioned byte-buffer dispatch for domain ops.**
Rejected: per-domain typed calls as *the* bridge surface.

Rationale:

- **One seam to version, secure, and observe.** Version check,
  correlation, error shaping, and the `catch_unwind` boundary live in
  exactly one place instead of ~470 entry points. New ops ship by
  extending the domain/op tables — the ABI stays append-only at
  dispatch granularity, so callers stop paying a symbol per feature.
  This is the mechanism that makes the surface *self-describing*
  (§4's `mw.describe`).
- **The stores already speak JSON.** Sitedecisions, cookies, policy
  verdicts, session manifests, container rows, opensearch — nearly
  every domain's non-trivial type is already JSON on the wire today.
  The envelope formalizes what the flat surface improvised.
- **C consumers stay thin.** `tools/acprobe.c` (and any future
  non-Qt consumer — the CEF spike's glue, headless harnesses) walks
  one symbol instead of linking the whole surface.
- **Honest capability reporting.** A dispatcher can refuse an op a
  build lacks with a structured `unknown_op` instead of a link-time
  absence the caller discovers the hard way.

The cost is real and recorded: a JSON decode + dispatch per call, and
no compile-time argument checking. Two mitigations, both first-class
policy rather than accidents:

- **Hot iterative paths keep flat calls.** Per-row tree/cursor walks
  (`ac_bm_child_at`, `ac_hist_entry_at` paging, icon fetch per host)
  stay on the typed `ac_*` spelling where the envelope buys nothing.
  MB06/MB07 record the per-call keep/envelope decision in §8.
- **Batch ops become the envelope's idiom.** `settings.get`,
  `history.query`, `bookmarks.subtree`, `cookies.list` are already
  bulk-shaped; the envelope pushes callers toward them, which is the
  direction the stores want anyway.

### 2.1 Frame layout

Request and response share one fixed little-endian header followed by
a UTF-8 JSON payload:

```
off  size  field
 0    2    magic  "AM" (0x41 0x4D)            — cheap non-frame reject
 2    1    version                           — envelope schema ver (v1)
 3    1    msg_type  request: 1=query 2=command
                    response: 3=ok 4=error
 4    8    corr_id                           — caller-chosen, echoes
12    1    domain                            — registry in §7
13    1    flags                             — v1: 0 (reserved)
14    2    op                                — per-domain op id
16    4    payload_len                       — bytes following
20    N    payload                           — JSON; may be empty
```

- `msg_type` on the request encodes the class: queries and commands
  travel the same frame, but the type byte lets a future restricted
  context (read-only probe, sandboxed helper) accept queries only.
- `corr_id` echoes verbatim in the response — the only ordering/
  matching key, since dispatch is synchronous today but the frame
  must not preclude an async transport later.
- Responses echo `domain`/`op`; `type=4` carries the error object.

```c
/* Proposed signature (MB02 freezes it in arktos_core.h): */
AcStatus ac_mw_dispatch(AcCore *core, const uint8_t *req,
                      size_t req_len, AcBuffer *out);
uint32_t ac_mw_version(void);
```

- The `AcStatus` return is *transport-level only*: NULL args,
  unreadable buffer, allocation failure. A request whose header
  parses at all — even a truncated payload or bad version — still
  yields a `type=4` response envelope in `*out` (with `corr_id=0`
  when the header itself didn't parse). Malformed input produces a
  structured error, never a panic and never a bare status.
- `*out` is caller-owned, freed with `ac_buffer_free` — the existing
  ownership pattern, no new destructor.

### 2.2 Error object

```json
{"code":"unknown_op","status":1,"message":"policy: op 0x1f not registered"}
```

`code` is a stable string enum owned by MB02's table:
`bad_version`, `malformed`, `unknown_domain`, `unknown_op`,
`bad_payload`, `op_failed`. `status` carries the underlying
`RcStatus` value for `op_failed` (0 for pure envelope faults), so the
store's error space survives through the envelope unmapped.
`message` mirrors what `ac_last_error()` would have stashed.

### 2.3 Handles do not cross the envelope

Envelope payloads carry `{"profile":"<id>"|null, ...args}`; the
dispatcher resolves the bound store per call — the same store a flat
`ac_*` handle would reach, whose re-assert-on-every-call contract the
facade already guarantees. Reasons:

- Handle lifetime over a byte boundary can't express
  use-after-close; profile binding makes every envelope op
  self-contained and replayable.
- The stores are process-global behind profile re-asserts, so
  per-call binding costs a no-op check when the binding matches.

Consequence: ops where the handle *is* the resource — the pdfdoc
cursor, the BiDi client, the tor control link, an unlocked custody —
stay flat. That is not a hole in the envelope; it's the keep-flat
list (§8) doing its job.

## 3. Command/query asymmetry

- **Queries** may run on any thread, may overlap, and must not emit
  change events. A query that wants to emit is a command.
- **Commands** serialize on their store's lock (existing behavior),
  then emit their domain's change topic stamped with the bound
  profile — *after* releasing the lock, synchronous on the mutating
  thread, per the established notify contract. The envelope must not
  change this ordering guarantee.
- Events never flow through `ac_mw_dispatch` — the envelope is
  request/response only. The event lane is §5.

## 4. Versioning and discovery

Three independent versions, none may be conflated:

1. **`AC_API_VERSION`** (`ac_api_version()`) — the C ABI revision.
   Bumps to v7 when `ac_mw_dispatch`/`ac_mw_version` land
   (append-only, per RUSTCORE-ABI §2). A caller learns the mw_*
   symbols exist from this alone.
2. **Envelope schema version** — the frame's `version` byte, starting
   at 1. `ac_mw_version()` reports the max the library accepts. A
   request with `version` newer than the library gets a `bad_version`
   error envelope; older-but-known versions stay dispatchable so a
   stale caller degrades, not dies.
3. **Per-domain schema** — reported by the `mw.describe` op
   (domain 0): a JSON self-description
   `{envelope_version, domains:[{id,name,ops:[{id,name,class,
   profile_scoped,since}]}], events:[topic,...]}`. This is what makes
   the bridge self-describing: `acprobe --bridge` (MB05) walks this
   table instead of hardcoding ops, and the MB04 caps table can
   cross-reference it.

Growth rules mirror the facade's: append-only domain/op ids, never
renumber or repurpose, unknown ids → `unknown_domain`/`unknown_op`
error envelopes. Removing an op is forbidden; retiring leaves a gap.

## 5. Event lane

The event class is **the existing callback lane, formalized** — not
a new transport.

Today (`notify.rs`, verified against emit sites): one legacy
topic-only callback (`rc_set_change_callback`) and one facade
(module, profile_id) callback per `AcCore`
(`ac_set_change_callback`), both fired synchronously on the mutating
thread outside all store locks. `RustCoreBridge` re-emits them as
queued `storeChanged(topic)` / `storeChangedFor(topic, profileId)`
signals on the GUI thread.

Topics actually emitted today:

| Topic | Producer | Fires on |
|-------|----------|----------|
| `settings` | settings store | set/remove/import/sync-worthy mutation |
| `profiles` | profile registry | create/rename/delete/active/avatar |
| `history` | history store | add/remove/clear/expire/title/icon |
| `bookmarks` | bookmark tree | create/attach/detach/destroy/set_*/load |
| `sitedecisions` | sitedec store | set/remove/clear/replace/reset (incl. the `uaspoof` kind — there is no separate "uaspoof" emit; the header's example list is aspirational) |
| `searchengines` | opensearch registry | put/remove/rename/reorder/bundled ops |
| `cookies` | cookie db | set/remove/clear/purge/import/header-store |
| `icons` | icon cache | put/remove/clear/sweep |
| `containers` | container registry | create/update/remove |
| `notes` | notes store | insert/set/remove/sync |
| `credentials` | credential map | put/remove/reseal/passphrase |
| `autofill` | autofill store | set_forms/set_addresses/reseal |
| `updates` | update-check state | record_check/snooze/set_* |
| `torctl` | tor control link | reply/event/error queued for drain (a wake-up — no store mutated) |

Session ops deliberately emit nothing — per-run scratch state, not
shared-observable data (arktos_core.h documents this; MB03 must not
"fix" it).

Two non-notify event lanes also exist and stay as-is:

- **Tor drain** — the `torctl` topic is a wake-up; payloads come off
  `rc_tor_take_reply`/`take_event`/`take_error`.
- **BiDi** — `RcBidiCallback` delivers `response`/`event`/`state`
  kinds on an internal thread; its `state` emit is the client's own
  callback, not a notify topic.
- **Vault progress** — `RvProgressCallback`, synchronous on the
  caller's thread.

**MB03's job, as specced here:** namespaced topics
(`history/changed`, `policy/changed`, `profile/changed`,
`session/saved`), a payload schema (v1: `{"topic","profile",…}`
JSON), subscriber registration per topic, and a Qt-side
`AcEventNotifier` QObject converting the callback into typed
QSignals. Legacy topics keep firing — subscribers opt into the
namespaced lane; nothing existing is re-routed.

## 6. Keep-out list — Qt-side forever

What the bridge must NOT swallow (carried from RUSTCORE-ARCH §3 and
the task spec, restated as the honest boundary):

- **JS execution** — `runJavaScript`/`evaluateJavaScript` and any
  page-context script path. The render world's language stays the
  engine's.
- **WebChannel** — `QWebChannel` objects, `qwebchannel.js`, the
  page↔C++ object bridge.
- **`QWebEngineProfile` and engine-private storage** — the Chromium
  cookie jar (`ac_cookies_*` is its durable *mirror* on QtWebEngine —
  `AC_COOKIES_MIRROR` honesty contract), LocalStorage/IndexedDB, HTTP
  cache, service workers, visited links, everything under
  `engineStorageDir`.
- **Request-interception *execution*** — `QWebEngineUrlRequestInterceptor`
  stays Qt; the *decision* is already a Rust query
  (`policy.evaluate`, `policy.cookie_filter`, `sitedec.lookup`,
  `blocklist.check`, `urlstrip`). Same split for redirect/block
  enforcement.
- **Rendering and the view tree** — compositing, `QWebEngineView`/
  page lifecycle, input, focus.
- **OS file dialogs and native chrome** — file pickers, print UI,
  native menus.
- **List fetching / bulk network I/O** — blocklist/Tranco/Majestic/
  opensearch fetchers stay on the app NAM behind the
  remoteListsAllowed consent + HTTPS rule; Rust consumes drop files.
  Honest exceptions where the protocol *is* the feature: the tor
  control client (loopback TCP) and the second-opinion TLS probe.
- **Image decode/scale** — the avatar/icon PNG pipeline (QPixmap);
  Rust stores bytes, Qt renders them.
- **Path authority** — `BrowserPaths` owns the layout; Rust mirrors
  `profiles/<id>/` derivation and never invents a second root.
- **Volatile OTR bindings** — `rc_notes_open*(…, NULL)`-style
  off-the-record stores never bind through the facade or envelope.
- **Downloads engine** — `src/rustdl` is a separate crate/lane.
- **Qt composition guards** — e.g. `releasePersona` window-binding
  checks and `ProfileVault::wipeVault` around profile deletion; Rust
  deletes the registry row + dir, Qt owns the live-object teardown.

## 7. Domain registry

Proposed domain ids for the envelope header (MB02 freezes them in
code; `mw.describe` reports the live table):

| id | domain | covers |
|----|--------|--------|
| 0 | `mw` | `describe`, `version`, `ping` — the meta ops |
| 1 | `settings` | typed KV, both scopes |
| 2 | `profiles` | persona registry |
| 3 | `history` | visits + embedded icon lane |
| 4 | `bookmarks` | tree ops + HTML import/export |
| 5 | `policy` | sitedec, verdicts, blocklist, tranco, sitetrust, lookalike, uaspoof |
| 6 | `session` | session.dat + lifecycle marker |
| 7 | `cookies` | cookie db |
| 8 | `icons` | favicon cache |
| 9 | `containers` | container registry + namespace map |
| 10 | `notes` | sidebar notes |
| 11 | `custody` | seal/open/unlock + credential/autofill maps (rc-only today) |
| 12 | `opensearch` | search-engine registry |
| 13 | `updates` | update-check policy/state |
| 14 | `services` | stateless queries: urlstrip, omnibox, palette, qrcode, readability, parsers, tls, ua build, clearplan, pdfdoc sanitize |
| 15 | `extverify` | extension package/manifest verification |
| 16 | `vault` | rv_* archive ops |
| 17 | `tor` | tor control client |
| 18 | `bidi` | devtools BiDi client |

Domains 11 and 14–18 are rc-only today: the envelope gives them a
facade spelling without an ABI bump per op.

## 8. Export classification — the full surface

Legend: **Q**=query, **C**=command, **E**=event, **L**=lifecycle
(stays flat — §1). "En route" = slated for the envelope; "flat" =
kept on typed FFI per §2's policy. `—` = not applicable.

### 8.1 Facade housekeeping (`arktos_core.h`)

| Export | Class | Route | Notes |
|--------|-------|-------|-------|
| `ac_api_version` | Q | flat | ABI discovery — must precede the envelope's existence |
| `ac_capabilities` | Q | flat | module-presence bits |
| `ac_init` | L | flat | mints `AcCore`; the envelope's required first arg |
| `ac_shutdown` | L | flat | frees core + all minted handles |
| `ac_last_error` | Q | flat | TLS error read; envelope errors also arrive in-band |
| `ac_string_free` | L | flat | destructor |
| `ac_buffer_free` | L | flat | destructor — frees `ac_mw_dispatch` output |
| `ac_set_change_callback` | E | flat | event-lane registration |
| `ac_settings` `ac_profiles` `ac_history` `ac_bookmarks` `ac_policy` `ac_session` `ac_cookies` `ac_icons` `ac_containers` `ac_notes` | L | flat | profile-bound handle mints; envelope ops rebind per call instead |
| `ac_history_exists` | Q | en route | existence probe without minting — `history.exists` |
| `ac_settings_close` `ac_profiles_close` `ac_hist_close` `ac_bm_close` `ac_policy_close` `ac_session_close` `ac_cookies_close` `ac_icons_close` `ac_containers_close` `ac_notes_close` | L | flat | handle release |

### 8.2 settings (RC02)

| Export | Class | Route |
|--------|-------|-------|
| `ac_settings_contains` `ac_settings_get` `ac_settings_get_string` `ac_settings_get_bool` `ac_settings_get_int` `ac_settings_get_double` `ac_settings_get_bytes` `ac_settings_get_list` `ac_settings_list` | Q | en route — batch reads are the envelope win |
| `ac_settings_set_string` `ac_settings_set_bool` `ac_settings_set_int` `ac_settings_set_double` `ac_settings_set_bytes` `ac_settings_set_list` `ac_settings_remove` `ac_settings_sync` `ac_settings_import_ini` | C | en route |
| `rc_settings_open` `rc_settings_close` | L | flat (legacy spelling; `ac_settings_close` is §8.1) |
| `rc_settings_contains` `rc_settings_get` `rc_settings_get_string` `rc_settings_get_bool` `rc_settings_get_int` `rc_settings_get_double` `rc_settings_get_bytes` `rc_settings_get_list` `rc_settings_list` | Q | en route via same ops |
| `rc_settings_set_string` `rc_settings_set_bool` `rc_settings_set_int` `rc_settings_set_double` `rc_settings_set_bytes` `rc_settings_set_list` `rc_settings_remove` `rc_settings_sync` `rc_settings_import_ini` | C | en route via same ops |

### 8.3 profiles (RC03)

| Export | Class | Route |
|--------|-------|-------|
| `ac_profiles_list` `ac_profiles_get` `ac_profiles_active` `ac_profiles_dir` `ac_profiles_cache_dir` `ac_profiles_engine_storage_dir` `ac_profiles_engine_cache_dir` `ac_profiles_settings_dir` `ac_profiles_avatar_path` `ac_profiles_file` `ac_profiles_slugify` | Q | en route |
| `ac_profiles_reload` `ac_profiles_create` `ac_profiles_rename` `ac_profiles_set_avatar` `ac_profiles_delete` `ac_profiles_set_active` | C | en route |
| `rc_profiles_open` `rc_profiles_close` | L | flat |
| `rc_profiles_list` `rc_profiles_get` `rc_profiles_active` `rc_profiles_dir` `rc_profiles_cache_dir` `rc_profiles_engine_storage_dir` `rc_profiles_engine_cache_dir` `rc_profiles_settings_dir` `rc_profiles_avatar_path` `rc_profiles_file` `rc_profiles_slugify` | Q | en route |
| `rc_profiles_reload` `rc_profiles_create` `rc_profiles_rename` `rc_profiles_set_avatar` `rc_profiles_delete` `rc_profiles_set_active` | C | en route |

### 8.4 history (RC04)

| Export | Class | Route |
|--------|-------|-------|
| `ac_hist_count` `ac_hist_query` `ac_hist_suggest` `ac_hist_icon_get` | Q | en route |
| `ac_hist_entry_at` | Q | flat — per-row cursor paging; §2 keep-flat |
| `ac_hist_add` `ac_hist_update_title` `ac_hist_remove` `ac_hist_clear` `ac_hist_clear_since` `ac_hist_expire_older_than` `ac_hist_icon_set` `ac_hist_icon_clear` | C | en route |
| `rc_hist_open` | L | flat |
| `rc_hist_count` `rc_hist_query` `rc_history_suggest` `rc_hist_icon_get` `rc_hist_exists` | Q | en route |
| `rc_hist_entry_at` | Q | flat |
| `rc_hist_add` `rc_hist_update_title` `rc_hist_remove` `rc_hist_clear` `rc_hist_clear_since` `rc_hist_expire_older_than` `rc_hist_icon_set` `rc_hist_icon_clear` | C | en route |

### 8.5 bookmarks (RC05)

| Export | Class | Route |
|--------|-------|-------|
| `ac_bm_root` `ac_bm_get` `ac_bm_find` `ac_bm_parent` `ac_bm_child_count` `ac_bm_child_at` | Q | flat — lazy per-node walk is the tree's hot path; a `bookmarks.subtree` bulk query may be added later, it does not replace these |
| `ac_bm_reload` `ac_bm_load_mem` `ac_bm_save` `ac_bm_create` `ac_bm_attach` `ac_bm_detach` `ac_bm_destroy` `ac_bm_set_title` `ac_bm_set_url` `ac_bm_set_desc` `ac_bm_set_expanded` `ac_bm_set_tags` `ac_bm_import_html` `ac_bm_export_html` | C | en route except `load_mem`/`import_html` bulk-buffer ops — decide in MB06 |
| `rc_bm_root` `rc_bm_get` `rc_bm_find` `rc_bm_parent` `rc_bm_child_count` `rc_bm_child_at` | Q | flat |
| `rc_bm_load` `rc_bm_load_mem` `rc_bm_save` `rc_bm_create` `rc_bm_attach` `rc_bm_detach` `rc_bm_destroy` `rc_bm_set_title` `rc_bm_set_url` `rc_bm_set_desc` `rc_bm_set_expanded` `rc_bm_set_tags` `rc_bm_import_html` `rc_bm_export_html` | C | as above |

### 8.6 policy (RC06: sitedec + verdicts + lists + trust + spoof)

| Export | Class | Route |
|--------|-------|-------|
| `ac_sitedec_get` `ac_sitedec_list` `ac_sitedec_lookup` `ac_sitedec_snapshot` `ac_sitedec_store_present` | Q | en route — decision feeds the Qt-side interceptor (§6) |
| `ac_sitedec_set` `ac_sitedec_remove` `ac_sitedec_clear` `ac_sitedec_replace` `ac_sitedec_reload` `ac_sitedec_reset` | C | en route |
| `ac_policy_evaluate` `ac_policy_cookie_filter` `ac_policy_eval_generation` | Q | en route |
| `ac_policy_call` | Q/C | en route — class inherited from the inner op |
| `ac_policy_load_snapshot` | C | en route |
| `ac_blocklist_check` `ac_blocklist_count` | Q | en route |
| `ac_blocklist_load` `ac_blocklist_reload` | C | en route |
| `ac_tranco_rank` `ac_tranco_count` `ac_tranco_file_entries` | Q | en route |
| `ac_tranco_load` `ac_tranco_reload` | C | en route |
| `ac_sitetrust_lookup` `ac_sitetrust_lookup_opts` `ac_lookalike_check` | Q | en route |
| `ac_sitetrust_reload` | C | en route |
| `ac_ua_spoof` `ac_ua_spoof_list` | Q | en route |
| `ac_ua_spoof_set` `ac_ua_spoof_remove` | C | en route |
| `rc_sitedec_get` `rc_sitedec_list` `rc_sitedec_lookup` `rc_sitedec_snapshot` `rc_sitedec_store_present` | Q | en route |
| `rc_sitedec_set` `rc_sitedec_remove` `rc_sitedec_clear` `rc_sitedec_replace` `rc_sitedec_reload` `rc_sitedec_reset` | C | en route |
| `rc_policy_evaluate` `rc_policy_cookie_filter` `rc_policy_eval_generation` | Q | en route |
| `rc_policy_call` | Q/C | en route |
| `rc_policy_load_snapshot` | C | en route |
| `rc_blocklist_check` `rc_blocklist_count` | Q | en route |
| `rc_blocklist_load` `rc_blocklist_reload` | C | en route |
| `rc_tranco_rank` `rc_tranco_count` `rc_tranco_file_entries` | Q | en route |
| `rc_tranco_load` `rc_tranco_reload` | C | en route |
| `rc_sitetrust_lookup` `rc_sitetrust_lookup_opts` `rc_lookalike_check` | Q | en route |
| `rc_sitetrust_reload` | C | en route |
| `rc_ua_spoof` `rc_ua_spoof_list` | Q | en route |
| `rc_ua_spoof_set` `rc_ua_spoof_remove` | C | en route |

### 8.7 session (RC13)

Session ops emit no change events by contract — commands here are
event-silent.

| Export | Class | Route |
|--------|-------|-------|
| `ac_session_load` `ac_session_exists` `ac_session_dirty` `ac_session_crash_count` `ac_session_last_exit` `ac_session_restoring` | Q | en route |
| `ac_session_save` `ac_session_clear` `ac_session_mark_start` `ac_session_mark_exit` `ac_session_mark_terminated` `ac_session_set_restoring` | C | en route — `mark_start` returns the prev-dirty flag *and* writes the marker, so command |
| `rc_session_load` `rc_session_exists` `rc_session_dirty` `rc_session_crash_count` `rc_session_last_exit` `rc_session_restoring` | Q | en route |
| `rc_session_save` `rc_session_clear` `rc_session_mark_start` `rc_session_mark_exit` `rc_session_mark_terminated` `rc_session_set_restoring` | C | en route |
| `rc_session_encode` `rc_session_decode` | Q | en route — stateless ARSS codec, no store |

### 8.8 cookies (RC15)

| Export | Class | Route |
|--------|-------|-------|
| `ac_cookies_role` `ac_cookies_get` `ac_cookies_list` `ac_cookies_count` `ac_cookies_gate` | Q | en route |
| `ac_cookies_for_url` | Q | en route — lazy-expiry eviction footnote, §1 |
| `ac_cookies_set_role` `ac_cookies_set` `ac_cookies_remove` `ac_cookies_remove_for_host` `ac_cookies_clear` `ac_cookies_purge_expired` `ac_cookies_purge_session` `ac_cookies_store_header` `ac_cookies_import` `ac_cookies_export` | C | en route |
| `rc_cookies_open` | L | flat |
| `rc_cookies_is_open` `rc_cookies_role` `rc_cookies_get` `rc_cookies_list` `rc_cookies_count` `rc_cookies_gate` `rc_cookies_for_url` | Q | en route |
| `rc_cookies_set_role` `rc_cookies_set` `rc_cookies_remove` `rc_cookies_remove_for_host` `rc_cookies_clear` `rc_cookies_purge_expired` `rc_cookies_purge_session` `rc_cookies_store_header` `rc_cookies_import` `rc_cookies_export` | C | en route |

### 8.9 icons (RC16)

| Export | Class | Route |
|--------|-------|-------|
| `ac_icons_get` `ac_icons_key` `ac_icons_list` `ac_icons_count` | Q | en route |
| `ac_icons_put` `ac_icons_remove` `ac_icons_clear` `ac_icons_sweep` | C | en route |
| `rc_icons_get` `rc_icons_key` `rc_icons_list` `rc_icons_count` | Q | en route |
| `rc_icons_put` `rc_icons_remove` `rc_icons_clear` `rc_icons_sweep` | C | en route |

### 8.10 containers (RC19)

| Export | Class | Route |
|--------|-------|-------|
| `ac_containers_list` `ac_containers_get` `ac_containers_namespace` | Q | en route |
| `ac_containers_create` `ac_containers_update` `ac_containers_remove` | C | en route |
| `rc_containers_list` `rc_containers_get` `rc_containers_namespace` | Q | en route |
| `rc_containers_create` `rc_containers_update` `rc_containers_remove` | C | en route |

### 8.11 notes (RC21)

| Export | Class | Route |
|--------|-------|-------|
| `ac_notes_count` `ac_notes_get` `ac_notes_list` `ac_notes_is_dirty` `ac_notes_is_persistent` | Q | en route |
| `ac_notes_insert` `ac_notes_set_title` `ac_notes_set_body` `ac_notes_set_markdown` `ac_notes_remove` `ac_notes_sync` | C | en route |
| `rc_notes_open` `rc_notes_open_file` `rc_notes_close` | L | flat — incl. the volatile NULL-path OTR bind, which the envelope must never mint (§6) |
| `rc_notes_count` `rc_notes_get` `rc_notes_list` `rc_notes_is_dirty` `rc_notes_is_persistent` | Q | en route |
| `rc_notes_insert` `rc_notes_set_title` `rc_notes_set_body` `rc_notes_set_markdown` `rc_notes_remove` `rc_notes_sync` | C | en route |

### 8.12 custody + credentials + autofill (rc-only)

| Export | Class | Route |
|--------|-------|-------|
| `rc_open` `rc_open_with_key` `rc_seal` `rc_seal_with_key` `rc_unlock` `rc_lock` `rc_kdf_create` `rc_kdf_file_delete` `rc_key_file_create` `rc_key_file_copy` `rc_key_file_delete` `rc_derived_key_copy` | C | flat — custody state is a session resource, not a per-call bind (§2.3); revisit only if a headless consumer needs it |
| `rc_is_available` `rc_is_sealed` `rc_is_unlocked` `rc_passphrase_enabled` | Q | flat |
| `rc_cred_get` `rc_cred_list` | Q | en route |
| `rc_cred_put` `rc_cred_remove` `rc_cred_change_passphrase` `rc_cred_reseal` | C | en route |
| `rc_autofill_forms` `rc_autofill_addresses` `rc_autofill_store_present` | Q | en route |
| `rc_autofill_set_forms` `rc_autofill_set_addresses` `rc_autofill_reseal` | C | en route |
| `rc_set_data_dir` | L | flat — process anchor; envelope binds profiles per call so facade consumers never need it |
| `rc_set_change_callback` | E | flat — legacy event registration |
| `rc_last_error_message` | Q | flat |
| `rc_string_free` `rc_buffer_free` | L | flat |

### 8.13 opensearch registry (rc-only, OSE01)

| Export | Class | Route |
|--------|-------|-------|
| `rc_ose_list` `rc_ose_get` `rc_ose_keywords` `rc_ose_keyword_url` `rc_ose_engine_for_keyword` `rc_ose_expand` `rc_ose_url` `rc_ose_bundled_names` `rc_ose_removed_bundled` `rc_ose_store_present` | Q | en route |
| `rc_ose_put` `rc_ose_remove` `rc_ose_rename` `rc_ose_reorder` `rc_ose_set_keywords` `rc_ose_block_bundled` `rc_ose_unblock_bundled` `rc_ose_restore_bundled` `rc_ose_reset` `rc_ose_seed_bundled` `rc_ose_import` | C | en route |

### 8.14 update-check policy (rc-only, RC20)

| Export | Class | Route |
|--------|-------|-------|
| `rc_updates_state` `rc_updates_channel_accepts` `rc_updates_compare_versions` `rc_updates_is_trusted_url` `rc_updates_normalize_version` `rc_updates_parse_manifest` `rc_updates_platform_pick` `rc_updates_should_notify` | Q | en route |
| `rc_updates_record_check` `rc_updates_set_channel` `rc_updates_set_skipped` `rc_updates_snooze` | C | en route |

### 8.15 stateless services (rc-only)

All pure queries — no store, verdict/transform in → JSON/bytes out;
the `services` domain's natural citizens. Flat-call candidates only
where a hot loop justifies it (none known today).

| Export | Class | Route |
|--------|-------|-------|
| `rc_urlstrip` | Q | en route |
| `rc_urlstrip_load_rules` `rc_urlstrip_reload` | C | en route — swap/reload the ruleset |
| `rc_opensearch_parse` `rc_updatemanifest_parse` `rc_suggest_parse` `rc_xbel_check` | Q | en route — untrusted-doc gates |
| `rc_pdf_sanitize` | Q | en route |
| `rc_pdfdoc_open` `rc_pdfdoc_close` | L | flat — cursor handle (§2.3) |
| `rc_pdfdoc_page_count` `rc_pdfdoc_page_size` `rc_pdfdoc_title` `rc_pdfdoc_find` `rc_pdfdoc_render_png` | Q | flat — handle-bound |
| `rc_readability_extract` `rc_readability_probe` | Q | en route |
| `rc_ext_manifest_check` `rc_ext_verify_package` | Q | en route |
| `rc_classify_input` `rc_frecency_score` `rc_history_suggest` | Q | en route — `rc_history_suggest` reaches the history store but takes no handle; its op lands in domain 3 |
| `rc_pal_match` `rc_pal_score` | Q | en route |
| `rc_qr_encode` | Q | en route |
| `rc_clearplan` | Q | en route — returns the plan; Qt executes the wipe |
| `rc_tls_check` | Q | en route — does network I/O by design (§6 exception) |
| `rc_tls_add_root` `rc_tls_clear_roots` | C | en route |
| `rc_ua_build` `rc_ua_presets` `rc_ua_brand_version` | Q | en route — spoof table ops are in §8.6 (they ride sitedecisions) |

### 8.16 profile vault (rv_*)

| Export | Class | Route |
|--------|-------|-------|
| `rv_verify` `rv_inspect` | Q | en route |
| `rv_seal` `rv_unseal` `rv_rewrap` | C | flat for now — long-running ops with a progress callback don't fit a synchronous envelope; the `RvProgressCallback` lane is the event channel (§5) |

### 8.17 tor control (RC14)

| Export | Class | Route |
|--------|-------|-------|
| `rc_tor_open` `rc_tor_close` | L | flat — live socket handle |
| `rc_tor_disconnect` `rc_tor_send` `rc_tor_auth_cookie` `rc_tor_auth_cookie_file` `rc_tor_auth_password` `rc_tor_getinfo` `rc_tor_setevents` `rc_tor_takeownership` `rc_tor_signal` | C | flat — commands target the handle's connection; replies arrive by seq on the drain lane |
| `rc_tor_state` `rc_tor_error` | Q | flat |
| `rc_tor_take_reply` `rc_tor_take_event` `rc_tor_take_error` | E | flat — pull-drain after the `torctl` wake-up topic |

### 8.18 devtools BiDi (DEVT03)

| Export | Class | Route |
|--------|-------|-------|
| `rc_bidi_connect` `rc_bidi_free` | L | flat — live client handle |
| `rc_bidi_command` | C | flat — async submit; correlation is the BiDi id |
| `rc_bidi_is_alive` | Q | flat |
| `RcBidiCallback` | E | flat — `response`/`event`/`state` kinds |

## 9. Cutover decision table (MB06/MB07 fill in)

Rule restated: an `ac_*` caller moves to `ac_mw_dispatch` only where
the envelope shrinks code — batch reads, list/tree queries, JSON-in/
JSON-out verdicts. Single scalar reads in hot paths and every §1
lifecycle op stay flat. Per-call outcomes get recorded here as the
cutover lands; the verify bar is unchanged behavior + unchanged
autotest counts.

| Caller site | Flat today | Decision (pending MB06/MB07) |
|-------------|-----------|------------------------------|
| settings/profiles/history/bookmarks callers | ac_* typed | TBD — MB06 |
| policy/sessions/cookies/icons/opensearch callers | ac_* + rc_* mixed | TBD — MB07 |

## 10. What this doc deliberately did not decide

- Concrete op ids per domain — MB02 owns the tables; this doc owns
  the frame, the classes, and the boundary.
- Payload field names per op — derived mechanically from the flat
  signatures during MB02; `mw.describe` is where they must agree.
- Whether a binary (non-JSON) payload lane is ever needed — deferred;
  `flags` byte reserves the space. Session blob passthrough and icon
  PNGs are the candidates if JSON b64 overhead ever measures hot.
