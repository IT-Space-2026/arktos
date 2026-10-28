# MW-ARCH — Arktos middleware bridge architecture (MB01; finalized MB08)

The typed command/query/event surface between the Qt shell, the Rust
core (`src/rustcore`), and the engine adapters. This document is the
contract the MB02–MB07 rows implemented — §7's registry, §8's route
column and §9's cutover table now record the LANDED state; where it
and a code comment disagree, this file wins (same rule as
RUSTCORE-ABI.md).

Sources of record this doc was written against:
`src/rustcore/include/arktos_core.h` (facade ABI v9),
`src/rustcore/src/mw/mod.rs` (the frozen domain/op tables — the
`mw.describe` wire truth), `src/rustcore/src/notify.rs` (both event
lanes), `src/rustcore/src/caps.rs` (the static engine matrix),
`src/rustcore/include/rustcore.h` (legacy `rc_*`/`rv_*` surface),
`.devin/RUSTCORE-ABI.md`, `.devin/RUSTCORE-ARCH.md`,
`src/rustcorebridge.{h,cpp}` and `src/aceventnotifier.{h,cpp}`
(the Qt-side consumers).

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
/* Landed signature (arktos_core.h, ABI v7): */
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
   v7 added `ac_mw_dispatch`/`ac_mw_version`, v8 the `ac_events_*`
   lane, v9 `ac_engine_caps` (append-only, per RUSTCORE-ABI §2). A
   caller learns the mw_*/events/caps symbols exist from this alone.
2. **Envelope schema version** — the frame's `version` byte, starting
   at 1. `ac_mw_version()` reports the max the library accepts. A
   request with `version` newer than the library gets a `bad_version`
   error envelope; older-but-known versions stay dispatchable so a
   stale caller degrades, not dies.
3. **Per-domain schema** — reported by the `mw.describe` op
   (domain 0): a JSON self-description
   `{envelope_version, header_len, error_codes:[...],
   domains:[{id,name,ops:[{id,name,class,profile_scoped,since}]}],
   events:[topic,...]}` (landed shape; `events` is the flat-lane
   store-topic list — the namespaced registry is
   `ac_events_topics()`). This is what makes
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
| `sync` | sync engine | member add/remove/seen, local op append, import accepting new ops |
| `synctabs` | tab-presence adapter | a peer's presence doc replaced this device's newest stored view (the doc is in-memory; nothing durable changed — it is a view-update wake-up like `torctl`) |

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

**Landed (MB03):** the namespaced lane ships as specced —

- `ac_events_subscribe(core, filter, cb, userdata)` /
  `ac_events_unsubscribe` / `ac_events_topics` at ABI v8. Filter
  grammar: NULL/""/"*" = all, trailing '/' = the namespace,
  otherwise exact. Subscriptions are per-core, id-keyed, and die
  with `ac_shutdown`; one core cannot unsubscribe another's.
- Payload schema v1: `{"v":1,"topic":<t>,"profile":<bound id or
  null>, ...domain fields}` — the envelope keys are reserved
  (producers cannot overwrite them); `profile` is stamped from the
  same thread-local hint the flat lane uses.
- The topic registry (`notify::EVENT_TOPICS`, reported by
  `ac_events_topics()`; the `mw.describe` `events` array instead
  lists the 14 flat-lane store topics above) carries
  honesty bits per topic: `produced` = an emit site exists
  in-tree, `seen` = one actually fired this process. Declared
  today: `history/changed` (produced — the history mutation sites
  in lib.rs emit `op` = visit|title|remove|clear|clear_since|
  expire on BOTH lanes), `policy/changed`, `profile/changed`,
  `session/saved` (declared contract, not yet produced — the
  registry says so, never claims otherwise). `mw.emit` (domain 0)
  is the test/probe producer: it fires only registry-declared
  topics, so it cannot mint noise outside the contract.
- Qt side: `AcEventNotifier` (`src/aceventnotifier.{h,cpp}`) —
  subscribes with a topic filter and re-emits each delivery as a
  queued `storeChanged(QString topic, QByteArray payload)` signal
  on its own thread. Its userdata guard block is deliberately
  leaked: a Rust thread mid-callback while the destructor runs
  cannot be waited on, and the dead flag is the only ordering
  primitive that survives the free.

Legacy topics keep firing — subscribers opt into the namespaced
lane; nothing existing was re-routed.

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

Landed domain ids for the envelope header (frozen in
`src/rustcore/src/mw/mod.rs`'s `DOMAINS` table; `mw.describe`
reports the live table — this doc mirrors it):

| id | domain | covers |
|----|--------|--------|
| 0 | `mw` | `describe`, `version`, `ping`, `emit` — the meta ops |
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
| 17 | `tor` | tor control client — id reserved, zero ops registered |
| 18 | `bidi` | devtools BiDi client — id reserved, zero ops registered |
| 19 | `sync` | serverless sync engine core — Ed25519 device identity, group membership, signed op-log (SYNC01; see §8.19) |

Domains 11 and 14–18 cover rc-only surfaces: the envelope gives
them a facade spelling without an ABI bump per op. Domains 17/18
landed as reserved-but-empty — the tor control link and the BiDi
client are live socket handles, exactly the §2.3 keep-flat case;
the ids exist so `mw.describe` can say "registered, no ops" rather
than `unknown_domain`.

## 8. Export classification — the full surface

Legend: **Q**=query, **C**=command, **E**=event, **L**=lifecycle
(stays flat — §1). Route is the LANDED state: **envelope** = an op
is registered in the domain table and reachable through
`ac_mw_dispatch` today (the parenthetical names it);
**registered** = the op exists but no Qt caller dispatches it;
**flat** = typed FFI only, per §2's policy. `—` = not applicable.
`mw.describe` is the wire truth — these tables mirror it.

### 8.1 Facade housekeeping (`arktos_core.h`)

| Export | Class | Route | Notes |
|--------|-------|-------|-------|
| `ac_api_version` | Q | flat | ABI discovery — must precede the envelope's existence |
| `ac_capabilities` | Q | flat | module-presence bits |
| `ac_init` | L | flat | mints `AcCore`; the envelope's required first arg |
| `ac_shutdown` | L | flat | frees core + all minted handles + event subs |
| `ac_last_error` | Q | flat | TLS error read; envelope errors also arrive in-band |
| `ac_string_free` | L | flat | destructor |
| `ac_buffer_free` | L | flat | destructor — frees `ac_mw_dispatch` output |
| `ac_set_change_callback` | E | flat | flat-lane event registration |
| `ac_mw_dispatch` `ac_mw_version` | — | IS the envelope | the transport itself; status is transport-only |
| `ac_events_subscribe` `ac_events_unsubscribe` `ac_events_topics` | E | flat | namespaced event lane (§5) — registration, never dispatched |
| `ac_engine_caps` | Q | flat | static per-engine capability matrix (MB04) — static registry, no core |
| `ac_settings` `ac_profiles` `ac_history` `ac_bookmarks` `ac_policy` `ac_session` `ac_cookies` `ac_icons` `ac_containers` `ac_notes` | L | flat | profile-bound handle mints; envelope ops rebind per call instead |
| `ac_history_exists` | Q | envelope | `history.exists` — the probe Qt actually dispatches |
| `ac_settings_close` `ac_profiles_close` `ac_hist_close` `ac_bm_close` `ac_policy_close` `ac_session_close` `ac_cookies_close` `ac_icons_close` `ac_containers_close` `ac_notes_close` | L | flat | handle release |

### 8.2 settings (RC02)

| Export | Class | Route |
|--------|-------|-------|
| `ac_settings_contains` `ac_settings_get` `ac_settings_get_string` `ac_settings_list` | Q | envelope — settings.contains/get/get_string/list; batch reads are the envelope win |
| `ac_settings_get_bool` `ac_settings_get_int` `ac_settings_get_double` `ac_settings_get_bytes` `ac_settings_get_list` | Q | envelope via `settings.get` — the `{"t","v"}` typed doc covers every scalar getter; no per-type op registered |
| `ac_settings_set_string` `ac_settings_set_bool` `ac_settings_set_int` `ac_settings_set_double` `ac_settings_set_bytes` `ac_settings_set_list` | C | envelope via `settings.set` — same typed doc in |
| `ac_settings_remove` `ac_settings_sync` `ac_settings_import_ini` | C | envelope — settings.remove/sync/import_ini |
| `rc_settings_open` `rc_settings_close` | L | flat (legacy spelling; `ac_settings_close` is §8.1) |
| `rc_settings_contains` `rc_settings_get` `rc_settings_get_string` `rc_settings_get_bool` `rc_settings_get_int` `rc_settings_get_double` `rc_settings_get_bytes` `rc_settings_get_list` `rc_settings_list` | Q | envelope via same ops — also RustCoreSettings' no-bridge fallback |
| `rc_settings_set_string` `rc_settings_set_bool` `rc_settings_set_int` `rc_settings_set_double` `rc_settings_set_bytes` `rc_settings_set_list` `rc_settings_remove` `rc_settings_sync` `rc_settings_import_ini` | C | envelope via same ops |

### 8.3 profiles (RC03)

| Export | Class | Route |
|--------|-------|-------|
| `ac_profiles_list` `ac_profiles_get` | Q | envelope — profiles.list/get |
| `ac_profiles_reload` `ac_profiles_create` `ac_profiles_rename` `ac_profiles_set_avatar` `ac_profiles_delete` | C | envelope — profiles.reload/create/rename/set_avatar/delete |
| `ac_profiles_active` `ac_profiles_set_active` | Q/C | flat — the ambient persona is per-handle in-memory state; an envelope op would read a fresh handle's "default" and the set would die with the call (§9) |
| `ac_profiles_dir` `ac_profiles_cache_dir` `ac_profiles_engine_storage_dir` `ac_profiles_engine_cache_dir` `ac_profiles_settings_dir` `ac_profiles_avatar_path` `ac_profiles_file` `ac_profiles_slugify` | Q | flat — pure path math, no store I/O to batch |
| `rc_profiles_open` `rc_profiles_close` | L | flat |
| `rc_profiles_list` `rc_profiles_get` | Q | envelope via same ops — the explicit-fixture-root fallback |
| `rc_profiles_active` `rc_profiles_dir` `rc_profiles_cache_dir` `rc_profiles_engine_storage_dir` `rc_profiles_engine_cache_dir` `rc_profiles_settings_dir` `rc_profiles_avatar_path` `rc_profiles_file` `rc_profiles_slugify` | Q | flat |
| `rc_profiles_reload` `rc_profiles_create` `rc_profiles_rename` `rc_profiles_set_avatar` `rc_profiles_delete` | C | envelope via same ops |
| `rc_profiles_set_active` | C | flat |

### 8.4 history (RC04)

| Export | Class | Route |
|--------|-------|-------|
| `ac_hist_count` `ac_hist_query` `ac_hist_suggest` | Q | envelope — history.count/query/suggest (query is the batch listing that retired the entry_at loop) |
| `ac_hist_icon_get` | Q | flat — the legacy history.db icon lane, superseded by domain 8 (`icons.*`); no envelope op |
| `ac_hist_entry_at` | Q | flat — per-row cursor paging; §2 keep-flat |
| `ac_hist_add` `ac_hist_update_title` `ac_hist_remove` `ac_hist_clear` `ac_hist_clear_since` `ac_hist_expire_older_than` | C | envelope — history.add/update_title/remove/clear/clear_since/expire |
| `ac_hist_icon_set` `ac_hist_icon_clear` | C | flat — same legacy icon lane |
| `rc_hist_open` | L | flat |
| `rc_hist_count` `rc_hist_query` `rc_history_suggest` `rc_hist_exists` | Q | envelope via same ops — also the `histAnchor()` no-bridge fallback |
| `rc_hist_icon_get` | Q | flat |
| `rc_hist_entry_at` | Q | flat |
| `rc_hist_add` `rc_hist_update_title` `rc_hist_remove` `rc_hist_clear` `rc_hist_clear_since` `rc_hist_expire_older_than` | C | envelope via same ops |
| `rc_hist_icon_set` `rc_hist_icon_clear` | C | flat |

### 8.5 bookmarks (RC05)

| Export | Class | Route |
|--------|-------|-------|
| `ac_bm_get` | Q | registered (`bookmarks.get`) — a probe op; Qt's per-node walk stays flat on `m_store` (§9) |
| `ac_bm_root` `ac_bm_find` `ac_bm_parent` `ac_bm_child_count` `ac_bm_child_at` | Q | flat — lazy per-node walk is the tree's hot path; a `bookmarks.subtree` bulk query may be added later, it does not replace these |
| `ac_bm_create` | C | registered (`bookmarks.create`) — probe op; the model's call site is one flat call already |
| `ac_bm_reload` `ac_bm_load_mem` `ac_bm_save` `ac_bm_attach` `ac_bm_detach` `ac_bm_destroy` `ac_bm_set_title` `ac_bm_set_url` `ac_bm_set_desc` `ac_bm_set_expanded` `ac_bm_set_tags` `ac_bm_import_html` `ac_bm_export_html` | C | flat — opaque node handles cannot cross the frame (§2.3), and the bulk ops are one scalar FFI call each with nothing batchable (§9) |
| `rc_bm_root` `rc_bm_get` `rc_bm_find` `rc_bm_parent` `rc_bm_child_count` `rc_bm_child_at` | Q | flat |
| `rc_bm_load` `rc_bm_load_mem` `rc_bm_save` `rc_bm_create` `rc_bm_attach` `rc_bm_detach` `rc_bm_destroy` `rc_bm_set_title` `rc_bm_set_url` `rc_bm_set_desc` `rc_bm_set_expanded` `rc_bm_set_tags` `rc_bm_import_html` `rc_bm_export_html` | C | flat — as above |

### 8.6 policy (RC06: sitedec + verdicts + lists + trust + spoof)

| Export | Class | Route |
|--------|-------|-------|
| `ac_sitedec_get` `ac_sitedec_list` `ac_sitedec_lookup` `ac_sitedec_snapshot` `ac_sitedec_store_present` | Q | envelope — policy.sitedec_*; the decision still feeds the Qt-side interceptor (§6) |
| `ac_sitedec_set` `ac_sitedec_remove` `ac_sitedec_clear` `ac_sitedec_replace` `ac_sitedec_reload` `ac_sitedec_reset` | C | envelope — policy.sitedec_* |
| `ac_policy_evaluate` `ac_policy_cookie_filter` `ac_policy_eval_generation` | Q | flat — per-navigation/per-cookie scalar verdicts are the hot path §1 keeps flat |
| `ac_policy_call` | Q/C | flat — its own JSON dispatch; no envelope twin needed |
| `ac_policy_load_snapshot` | C | flat |
| `ac_blocklist_check` `ac_blocklist_count` | Q | flat — per-request verdict probe |
| `ac_blocklist_load` | C | flat |
| `ac_blocklist_reload` | C | envelope — policy.blocklist_reload (the DomainBlocklist commit hook) |
| `ac_tranco_rank` `ac_tranco_count` | Q | flat — per-request verdict probe |
| `ac_tranco_file_entries` | Q | envelope — policy.tranco_file_entries (TrancoList validation) |
| `ac_tranco_load` | C | flat |
| `ac_tranco_reload` | C | envelope — policy.tranco_reload |
| `ac_sitetrust_lookup` | Q | envelope — policy.sitetrust_lookup (SitePanel verdict) |
| `ac_sitetrust_lookup_opts` `ac_lookalike_check` | Q | flat — per-request probes |
| `ac_sitetrust_reload` | C | flat |
| `ac_ua_spoof` `ac_ua_spoof_list` | Q | flat — per-request probe |
| `ac_ua_spoof_set` `ac_ua_spoof_remove` | C | flat |
| `rc_sitedec_get` `rc_sitedec_list` `rc_sitedec_lookup` `rc_sitedec_snapshot` `rc_sitedec_store_present` | Q | envelope via same ops — also the SiteDecisionStore no-bridge fallback |
| `rc_sitedec_set` `rc_sitedec_remove` `rc_sitedec_clear` `rc_sitedec_replace` `rc_sitedec_reload` `rc_sitedec_reset` | C | envelope via same ops |
| `rc_policy_evaluate` `rc_policy_cookie_filter` `rc_policy_eval_generation` | Q | flat |
| `rc_policy_call` | Q/C | flat |
| `rc_policy_load_snapshot` | C | flat |
| `rc_blocklist_check` `rc_blocklist_count` | Q | flat |
| `rc_blocklist_load` `rc_blocklist_reload` | C | flat (`reload` has an envelope twin; `load` does not) |
| `rc_tranco_rank` `rc_tranco_count` `rc_tranco_file_entries` | Q | flat / envelope — `file_entries` is registered, the verdict probes stay flat |
| `rc_tranco_load` `rc_tranco_reload` | C | flat / envelope — `reload` registered, `load` flat |
| `rc_sitetrust_lookup` `rc_sitetrust_lookup_opts` `rc_lookalike_check` | Q | flat — `sitetrust_lookup`'s envelope twin serves SitePanel; the hot-path probes stay flat |
| `rc_sitetrust_reload` | C | flat |
| `rc_ua_spoof` `rc_ua_spoof_list` | Q | flat |
| `rc_ua_spoof_set` `rc_ua_spoof_remove` | C | flat |

### 8.7 session (RC13)

Session ops emit no change events by contract — commands here are
event-silent.

| Export | Class | Route |
|--------|-------|-------|
| `ac_session_load` `ac_session_exists` | Q | envelope — session.load/exists (the manifest crosses as a JSON doc) |
| `ac_session_dirty` `ac_session_crash_count` `ac_session_last_exit` `ac_session_restoring` | Q | flat — single-scalar marker reads; main.cpp's probe legs keep them |
| `ac_session_save` `ac_session_clear` `ac_session_mark_start` `ac_session_mark_exit` `ac_session_mark_terminated` | C | envelope — session.save/clear/mark_start/mark_exit/mark_terminated (`mark_start` returns the prev-dirty flag *and* writes the marker, so command) |
| `ac_session_set_restoring` | C | flat — restore-loop guard; internal to the Qt restore flow |
| `rc_session_load` `rc_session_exists` `rc_session_dirty` `rc_session_crash_count` `rc_session_last_exit` `rc_session_restoring` | Q | flat — the load/exists twins exist on the envelope; the scalar marker reads stay flat |
| `rc_session_save` `rc_session_clear` `rc_session_mark_start` `rc_session_mark_exit` `rc_session_mark_terminated` | C | envelope via same ops — also the main.cpp probe-leg flat spelling |
| `rc_session_set_restoring` | C | flat |
| `rc_session_encode` `rc_session_decode` | Q | flat — stateless ARSS codec; the self-test legs exercise it directly |

### 8.8 cookies (RC15)

| Export | Class | Route |
|--------|-------|-------|
| `ac_cookies_list` `ac_cookies_count` | Q | registered — cookies.list/count; zero Qt callers exist yet (§9) |
| `ac_cookies_role` `ac_cookies_get` `ac_cookies_gate` | Q | flat |
| `ac_cookies_for_url` | Q | flat — the send-match is the per-request hot path (lazy-expiry eviction footnote, §1) |
| `ac_cookies_set` | C | registered — cookies.set |
| `ac_cookies_set_role` `ac_cookies_remove` `ac_cookies_remove_for_host` `ac_cookies_clear` `ac_cookies_purge_expired` `ac_cookies_purge_session` `ac_cookies_store_header` `ac_cookies_import` `ac_cookies_export` | C | flat |
| `rc_cookies_open` | L | flat |
| `rc_cookies_is_open` `rc_cookies_role` `rc_cookies_get` `rc_cookies_gate` `rc_cookies_for_url` | Q | flat |
| `rc_cookies_list` `rc_cookies_count` | Q | registered via same ops |
| `rc_cookies_set_role` `rc_cookies_remove` `rc_cookies_remove_for_host` `rc_cookies_clear` `rc_cookies_purge_expired` `rc_cookies_purge_session` `rc_cookies_store_header` `rc_cookies_import` `rc_cookies_export` | C | flat |
| `rc_cookies_set` | C | registered via `cookies.set` |

### 8.9 icons (RC16)

| Export | Class | Route |
|--------|-------|-------|
| `ac_icons_get` `ac_icons_count` | Q | envelope — icons.get/count (PNGs cross base64'd) |
| `ac_icons_key` `ac_icons_list` | Q | flat — key derivation + index dump; no Qt caller needed the batch |
| `ac_icons_put` `ac_icons_clear` `ac_icons_sweep` | C | envelope — icons.put/clear/sweep (HistoryManager's icon lane) |
| `ac_icons_remove` | C | flat |
| `rc_icons_get` `rc_icons_count` | Q | envelope via same ops |
| `rc_icons_key` `rc_icons_list` | Q | flat |
| `rc_icons_put` `rc_icons_clear` `rc_icons_sweep` | C | envelope via same ops |
| `rc_icons_remove` | C | flat |

### 8.10 containers (RC19)

| Export | Class | Route |
|--------|-------|-------|
| `ac_containers_list` | Q | registered — containers.list |
| `ac_containers_get` `ac_containers_namespace` | Q | flat |
| `ac_containers_create` | C | registered — containers.create |
| `ac_containers_update` `ac_containers_remove` | C | flat — the storage-tree teardown stays Qt's job regardless |
| `rc_containers_list` | Q | registered via same op |
| `rc_containers_get` `rc_containers_namespace` | Q | flat |
| `rc_containers_create` | C | registered via same op |
| `rc_containers_update` `rc_containers_remove` | C | flat |

### 8.11 notes (RC21)

| Export | Class | Route |
|--------|-------|-------|
| `ac_notes_count` `ac_notes_list` | Q | registered — notes.count/list |
| `ac_notes_get` `ac_notes_is_dirty` `ac_notes_is_persistent` | Q | flat |
| `ac_notes_insert` | C | registered — notes.insert |
| `ac_notes_set_title` `ac_notes_set_body` `ac_notes_set_markdown` `ac_notes_remove` `ac_notes_sync` | C | flat — the sidebar's per-field edit path is already one flat call; the debounce/sync semantics ride the handle |
| `rc_notes_open` `rc_notes_open_file` `rc_notes_close` | L | flat — incl. the volatile NULL-path OTR bind, which the envelope must never mint (§6) |
| `rc_notes_count` `rc_notes_list` | Q | registered via same ops |
| `rc_notes_get` `rc_notes_is_dirty` `rc_notes_is_persistent` | Q | flat |
| `rc_notes_insert` | C | registered via same op |
| `rc_notes_set_title` `rc_notes_set_body` `rc_notes_set_markdown` `rc_notes_remove` `rc_notes_sync` | C | flat |

### 8.12 custody + credentials + autofill (rc-only)

| Export | Class | Route |
|--------|-------|-------|
| `rc_open` `rc_open_with_key` `rc_seal` `rc_seal_with_key` `rc_unlock` `rc_lock` `rc_kdf_create` `rc_kdf_file_delete` `rc_key_file_create` `rc_key_file_copy` `rc_key_file_delete` `rc_derived_key_copy` | C | flat — custody state is a session resource, not a per-call bind (§2.3); revisit only if a headless consumer needs it |
| `rc_is_available` `rc_is_sealed` `rc_is_unlocked` `rc_passphrase_enabled` | Q | flat — `custody.state` answers the first/third/fourth in one doc (sealed isn't carried); the flat calls stay |
| `rc_cred_list` | Q | registered — `custody.cred_names` returns the names array |
| `rc_cred_get` | Q | flat |
| `rc_cred_put` | C | registered — `custody.cred_put` (value crosses as `value_b64`) |
| `rc_cred_remove` `rc_cred_change_passphrase` `rc_cred_reseal` | C | flat |
| `rc_autofill_forms` `rc_autofill_addresses` `rc_autofill_store_present` | Q | flat |
| `rc_autofill_set_forms` `rc_autofill_set_addresses` `rc_autofill_reseal` | C | flat |
| `rc_set_data_dir` | L | flat — process anchor; envelope binds profiles per call so facade consumers never need it |
| `rc_set_change_callback` | E | flat — legacy event registration |
| `rc_last_error_message` | Q | flat |
| `rc_string_free` `rc_buffer_free` | L | flat |

### 8.13 opensearch registry (rc-only, OSE01)

| Export | Class | Route |
|--------|-------|-------|
| `rc_ose_list` `rc_ose_get` `rc_ose_keywords` `rc_ose_keyword_url` `rc_ose_engine_for_keyword` `rc_ose_expand` `rc_ose_url` `rc_ose_bundled_names` `rc_ose_removed_bundled` `rc_ose_store_present` | Q | envelope — opensearch.* (all 10 queries landed; OpenSearchManager dispatches them) |
| `rc_ose_put` `rc_ose_remove` `rc_ose_rename` `rc_ose_reorder` `rc_ose_set_keywords` `rc_ose_block_bundled` `rc_ose_unblock_bundled` `rc_ose_restore_bundled` `rc_ose_reset` `rc_ose_seed_bundled` `rc_ose_import` | C | envelope — opensearch.* (all 11 commands landed; raw XML descriptors cross as `xml_b64`) |

### 8.14 update-check policy (rc-only, RC20)

| Export | Class | Route |
|--------|-------|-------|
| `rc_updates_state` | Q | registered — updates.state |
| `rc_updates_channel_accepts` `rc_updates_compare_versions` `rc_updates_is_trusted_url` `rc_updates_normalize_version` `rc_updates_parse_manifest` `rc_updates_platform_pick` `rc_updates_should_notify` | Q | flat — stateless decision helpers; single-call sites don't shrink |
| `rc_updates_record_check` | C | registered — updates.record_check |
| `rc_updates_set_channel` `rc_updates_set_skipped` `rc_updates_snooze` | C | flat |

### 8.15 stateless services (rc-only)

All pure queries — no store, verdict/transform in → JSON/bytes out;
the `services` domain's natural citizens. Flat-call candidates only
where a hot loop justifies it (none known today).

| Export | Class | Route |
|--------|-------|-------|
| `rc_classify_input` | Q | registered — services.classify_input (the envelope's omnibox-entry smoke op) |
| `rc_urlstrip` | Q | flat |
| `rc_urlstrip_load_rules` `rc_urlstrip_reload` | C | flat — swap/reload the ruleset |
| `rc_opensearch_parse` `rc_updatemanifest_parse` `rc_suggest_parse` `rc_xbel_check` | Q | flat — untrusted-doc gates |
| `rc_pdf_sanitize` | Q | flat |
| `rc_pdfdoc_open` `rc_pdfdoc_close` | L | flat — cursor handle (§2.3) |
| `rc_pdfdoc_page_count` `rc_pdfdoc_page_size` `rc_pdfdoc_title` `rc_pdfdoc_find` `rc_pdfdoc_render_png` | Q | flat — handle-bound |
| `rc_readability_extract` `rc_readability_probe` | Q | flat |
| `rc_ext_manifest_check` | Q | registered — extverify.manifest_check (domain 15) |
| `rc_ext_verify_package` | Q | flat — signature verification stays a direct call |
| `rc_frecency_score` `rc_history_suggest` | Q | flat — `history.suggest` covers the suggest path; frecency is a pure scorer with no caller asking for the envelope |
| `rc_pal_match` `rc_pal_score` | Q | flat |
| `rc_qr_encode` | Q | flat |
| `rc_clearplan` | Q | flat — returns the plan; Qt executes the wipe |
| `rc_tls_check` | Q | registered — services.tls_probe; lands as a *command* (it does network I/O by design, §6 exception — a query must not have observable side effects) |
| `rc_tls_add_root` `rc_tls_clear_roots` | C | flat |
| `rc_ua_build` `rc_ua_presets` `rc_ua_brand_version` | Q | flat — spoof table ops are in §8.6 (they ride sitedecisions) |

### 8.16 profile vault (rv_*)

| Export | Class | Route |
|--------|-------|-------|
| `rv_verify` `rv_inspect` | Q | landed (MB05) |
| `rv_seal` | C | landed (MB05) — synchronous with a null progress callback; unseal/rewrap stay flat until a progress lane exists |
| `rv_unseal` `rv_rewrap` | C | flat for now — long-running ops with a progress callback don't fit a synchronous envelope; the `RvProgressCallback` lane is the event channel (§5) |

### 8.17 tor control (RC14)

| Export | Class | Route |
|--------|-------|-------|
| `rc_tor_open` `rc_tor_close` | L | flat — live socket handle |
| `rc_tor_disconnect` `rc_tor_send` `rc_tor_auth_cookie` `rc_tor_auth_cookie_file` `rc_tor_auth_password` `rc_tor_getinfo` `rc_tor_setevents` `rc_tor_takeownership` `rc_tor_signal` | C | flat — commands target the handle's connection; replies arrive by seq on the drain lane |
| `rc_tor_state` `rc_tor_error` | Q | flat |
| `rc_tor_take_reply` `rc_tor_take_event` `rc_tor_take_error` | E | flat — pull-drain after the `torctl` wake-up topic |

Domain 17 (`tor`) is registered with zero ops — the whole surface
is the §2.3 keep-flat case (a live socket handle that cannot cross
a self-contained frame).

### 8.18 devtools BiDi (DEVT03)

| Export | Class | Route |
|--------|-------|-------|
| `rc_bidi_connect` `rc_bidi_free` | L | flat — live client handle |
| `rc_bidi_command` | C | flat — async submit; correlation is the BiDi id |
| `rc_bidi_is_alive` | Q | flat |
| `RcBidiCallback` | E | flat — `response`/`event`/`state` kinds |

Domain 18 (`bidi`) likewise: registered, zero ops.

### 8.19 sync engine (SYNC01)

The serverless sync core is envelope-only — there is no flat
`rc_*`/`ac_*` surface for it.  The sync group is device-global, so
every op is `profile_scoped=false` and the engine opens on the
core's data root (`<root>/sync/` holds `device.key`, the Ed25519
PKCS#8 identity at 0600, and `sync.db`, the members + oplog
SQLite store).  Wire ops are JSON docs (`v:1`) with raw bytes on
the `_b64` convention; `op_id` is hex-SHA-256 over the signed
content frame, making dedup content-addressed and
transport-agnostic.  Import verifies each op against the signer's
member pubkey (or the local key for loopback), dedups by op_id,
then applies the batch in canonical (lamport, device_id, op_id)
order through per-store appliers registered via
`sync::register_applier` — the seam SYNC04–SYNC08 plug into; ops
for stores with no applier stay durable in the log and report as
`deferred`.

| Op | Class | Notes |
|----|-------|-------|
| `sync.identity` | Q | `{device_id, pubkey_b64}` — device id = hex(ed25519 pubkey) |
| `sync.members_list` | Q | paired devices: pubkey, onion addr, last-seen, added |
| `sync.member_add` | C | pair `{pubkey_b64, onion_addr?}`; upserts; self-pair rejected |
| `sync.member_remove` | C | unpair; honest `removed` bool |
| `sync.member_seen` | C | bump last-seen; unknown id → `op_failed` (NotFound) |
| `sync.append` | C | the op-log tailing seam — sign + log a CRDT op for one synced store |
| `sync.export` | Q | ops after `since_seq`, optional `store` filter — the transport drain |
| `sync.import` | C | verify/dedup/apply a remote batch; per-op rejections, never a batch abort |
| `sync.stats` | Q | device id, lamport clock, member/op counts, per-store heads |
| `sync.drain` | C | retry ops a registered applier hasn't consumed yet |

The bookmarks adapter (SYNC04, `syncbookmarks.rs`) rides the same
domain — no new ops.  Local mutations on the canonical bookmark
store record semantic events; `bookmarks::with` flushes them into
signed ops scoped to the bound profile (`<root>/profiles/<id>/
bookmarks.xbel` ⇒ scope `<id>`).  The registered applier binds that
scope's xbel on the peer, applies, and persists the merge.
Semantics: one OR-set element per node guid (XBEL `id` attr; random
at create, deterministic content fingerprint for foreign-loaded
nodes so pre-sync trees dedup), tombstoned deletes that record the
remover's observed place stamps (un-observed placements survive),
`link` ops that add+move by (lamport, device) LWW with fractional-
index `pos` sibling order, per-field LWW `set` ops, causal parking
for not-yet-known parents, deterministic stamp-ordered move-cycle
resolution.  The op-log itself is the adapter's durable state —
tombstones, stamps and parked ops rebuild by replaying on first
use.

The history adapter (SYNC05, `synchistory.rs`) rides the same
domain — no new ops.  `add`/`update_title` on the canonical store
record outbox events that `history::with` flushes into signed ops
scoped to the bound profile (`<root>/profiles/<id>/history.db` ⇒
scope `<id>`).  Semantics: one union-set element per visit keyed by
a visit-id deterministic over (url, visited_at) — identical
pre-sync visits on two devices name the same element and dedup to
one row.  `visit` is the only op type — append-only union; a known
element merges its title LWW by (lamport, device), an empty remote
title never erasing.  Dedup is two-layered: the replayed op-log
stamp frontier plus a row probe on (url, ts) for elements the
local table holds pre-sync.  Deletes and retention NEVER emit —
remove/clear/clear_since/expire are device-local by contract.
Per-device retention applies locally after merge:
expire_older_than records the widest cutoff in the store's meta
table and the applier re-runs it on every merged op, so a peer's
longer history cannot resurrect rows this device already expired.
The first flush after hydrate also advertises every local row
whose visit-id is still unknown — pre-sync history flows to the
group once.

The open-tabs presence adapter (SYNC06, `synctabs.rs`) adds the
domain's first new ops since pairing — the "tabs" store's doc is
EPHEMERAL per-device presence, not a durable CRDT.  `tabs_publish`
signs the caller's whole tab set as one `presence` body
(`windows:[{current, tabs:[{url, title?, pinned?, container?}]}]`,
the Qt side snapshots each window via
`TabWidget::presenceWindowJson` — off-the-record tabs dropped like
the session snapshot drops them); an empty windows array is a real
publish — the "everything closed" wipe.  Apply keeps only the
newest doc per device — LWW on the op's (lamport, device_id) stamp,
never a merge — held in MEMORY ONLY: the lane writes to no store,
so a peer's open tabs are a read-only view and receiving presence
can never mint a history row.  `tabs_peers` reports the newest doc
every OTHER device published for the scope (this device's own doc
is never listed — its live tabs are already on screen).  The
op-log's durability gives the view a warm restart — last-known
state is shown, honestly stale-capable (no TTL yet).  The Qt read
side is the History menu's "Tabs from Other Devices" submenu
(`SyncTabsMenu`): opening a listed tab is an ordinary local
navigation — its history entry is local, exactly like typing the
url.

| Op | Class | Notes |
|----|-------|-------|
| `sync.tabs_publish` | C | `{profile?, windows:[…]}` — publish this device's open tabs as one presence doc; `since: 4` |
| `sync.tabs_peers` | Q | `{profile?}` → `{devices:[{device_id, updated_ms, windows}]}` — newest presence doc per OTHER device |

The settings adapter (SYNC07, `syncsettings.rs`) rides the same
domain — the only new op is the exclusion read model; mutations
emit by themselves.  `RcSettings::set`/`remove` — the funnel every
mutation lane already shares — sign `{t:"set"|"del", key, val?}`
ops onto the profile scope's log post-persist, best-effort (a
failed append leaves a durable local change the next advertise
re-offers).  Semantics: a per-key LWW map — strictly newer
(lamport, device) moves the key, stale drops, concurrent writes
converge to the higher stamp.  The device-local exclusion list is
raw key-prefix rules that never emit AND never apply: built-ins
cover `sync` (engine config + the list itself), `sessions` and
`BrowserMainWindow` (window geometry/session blobs), `proxy`
(per-machine network config), and the downloadmanager path/
in-flight-record leaves; the profile's own `sync/localExclude`
string-list extends it — under the excluded `sync` prefix, so the
list can never sync into changing itself.  An excluded remote op
is consumed-and-dropped (counts applied, writes nothing); lifting
a rule lets the next op through, never resurrects skipped ones.
The first emit or apply per scope also advertises every present
non-excluded key the frontier doesn't know — pre-sync prefs (incl.
ini-imported values, which never emit per-key) flow to the group
once.  Global-scope settings never sync.  Apply writes through
`set_applied`/`remove_applied` so a merged op can't echo, and a
real change emits `settings` + `settings/changed` for the Qt side.

| Op | Class | Notes |
|----|-------|-------|
| `sync.settings_excludes` | Q | `{profile?}` → `{defaults:[…], local:[…]}` — the effective LWW-map exclusion prefixes; `since: 5` |

The passwords adapter (SYNC08, `syncpasswords.rs`) is strictly
opt-in and end-to-end encrypted: saved-login records (the autofill
store's `has_password` forms in `<root>/profiles/<id>/
autofill-store.dat` — plain form fills and addresses never leave
the device) travel as per-entry ciphertext under a vault key
Argon2id-derived from the user's sync passphrase and salted by
`SHA-256("arktos-pwvault-salt/v1" || group_secret)` — the SYNC03
group secret every member already holds, so key material is never
an op payload and the salt is bound to the paired group.
`passwords_enable` is the passphrase gate: it requires the
profile's group secret (no group → `NotEnabled`), probes the
newest `pwv` verifier op in the log (the first enabler plants it —
`seal(vault_key, VERIFIER)`, a wrong passphrase fails the GCM tag
→ `WrongPassphrase`), then drains pending ciphertext and
advertises the local entries.  Wire ops carry `{t:"pwv"}`,
`{t:"set","e":eid,"f":[blobs],"r":[blobs]}` and
`{t:"del","e":eid}` — `eid` = hex-SHA-256 of the record url, every
other byte sealed (set blobs bind `eid|field|value`, `r` blobs
tombstone a field).  Semantics: field-level LWW on the op's
(lamport, device_id) stamp — the op-log's total order makes each
field's latest stamp its version — with whole-entry `del`
tombstones; stale fields drop, a later set resurrects.  A locked
vault defers apply (`Locked` → applied=0 → `drain` retries after
unlock): a peer that never enables still holds and forwards the
ciphertext.  Emission diffs `Store::autofill_set` against the
decrypted synced image; apply merges through
`autofill_set_applied` without clobbering local fields the group
never mentioned, and a real change emits `autofill` +
`autofill/changed`.  Honest edges: two passphrases in one group
fork the vault (each side holds ops it cannot read), and SYNC09's
group-secret rotation re-bases the salt — its re-wrap must re-seal
password fields under the re-derived key.

| Op | Class | Notes |
|----|-------|-------|
| `sync.passwords_enable` | C | `{profile?, passphrase}` — opt in + unlock; verifier probe, drain, advertise; `since: 6` |
| `sync.passwords_lock` | C | `{profile?}` — drop vault key + decrypted image; `since: 6` |
| `sync.passwords_status` | Q | `{profile?}` → `{unlocked, enabled_ever, ops, pending_apply}`; `since: 6` |

The transport half (SYNC02) shares the same envelope and the
`synctransport` notify topic — a pull-drain wake-up shaped like
`torctl`'s: inbound links and frames land in a queue drained by
`transport_poll`.  The `tor` backend is embedded Arti (`synctor`
cargo feature, default-on): a per-device v3 onion service whose hsid
lives in the Arti keystore under `<root>/sync/tor/state` (stable
address across restarts), gated by restricted discovery — x25519
descriptor encryption, so unpaired peers are refused at the Tor
layer before a single app byte.  Grants persist in
`<root>/sync/tor/authorized_clients.json` and are replayed into the
service config at launch and on every live `reconfigure`.  A `mem`
loopback backend implements the same seam for hermetic tests and
pins the contract SYNC10's LAN path must satisfy.  Outbound links
retry transient failures on a bounded exponential backoff; a peer
without our authorized key is refused immediately (the failure is
Tor-layer, never retryable).  Discovery keys are per-peer-hsid
keystore entries — `transport_keygen` is idempotent.

| Op | Class | Notes |
|----|-------|-------|
| `sync.transport_start` | C | `{backend:"tor"|"mem", name?}` — boot the listener; idempotent; `since: 2` |
| `sync.transport_stop` | C | tear down; idempotent |
| `sync.transport_status` | Q | `{running, backend, endpoint, authorized[], links}` |
| `sync.transport_authorize` | C | `{nickname, discovery_key}` — Tor-layer grant |
| `sync.transport_deauthorize` | C | `{nickname}` — revoke |
| `sync.transport_keygen` | C | `{onion}` → our public discovery key for a peer svc |
| `sync.transport_connect` | C | `{endpoint, nickname?}` → `{link_id}` |
| `sync.transport_send` | C | `{link_id, payload_b64}` — one frame |
| `sync.transport_close` | C | `{link_id}` |
| `sync.transport_poll` | Q | drain events: ready/bootstrap/incoming/frame/closed/failed |

Pairing (SYNC03) turns two devices into one profile's sync group.
The inviter mints a one-time payload — QR text `arktos-pair1.<b64>`
or a grouped-base32 manual code — carrying its endpoint, a 256-bit
invite token, and an 8-byte fragment of its device-key hash.  The
token is doubly the Noise prologue and the derived one-time
restricted-discovery key, so an unpaired joiner reaches the gated
onion exactly once.  Noise_XX (`noisexx.rs`) then authenticates
both x25519 statics; signed payloads bind each static to the
Ed25519 device identity; the transcript hash renders as the
6-word/6-emoji fingerprint both users compare — only after BOTH
`pair_confirm` does the profile's group secret + the inviter's
permanent discovery key cross inside the channel, and the peer
lands in `member_scopes` bound to that profile.  `pair_poll` drives
the state machine (waiting → handshaking → fingerprint →
committing → done/failed) off transport events; the UI polls it on
a timer.

| Op | Class | Notes |
|----|-------|-------|
| `sync.pair_begin` | C | `{profile?}` → `{pair_id, qr, code, endpoint, scope}`; `since: 3` |
| `sync.pair_join` | C | `{payload}` (QR text or manual code) → `{pair_id}` |
| `sync.pair_poll` | C | `{pair_id?}` — advance sessions, report state + `fingerprint` |
| `sync.pair_confirm` | C | `{pair_id, ok}` — the fingerprint decision |
| `sync.pair_cancel` | C | `{pair_id}` — best-effort abort |
| `sync.pair_list` | Q | `{sessions}` — live pairing sessions |

Device management (SYNC09, `syncmeta.rs`) adds the sixth synced
store, `"syncmeta"` — group-management ops, never user data — and
the revocation flow.  `device_revoke` un-pairs a device from one
profile's group: it rotates the scope's group secret
(`group_secret_rotate` bumps a per-scope generation and keeps the
displaced key one deep in `gkey_prev:`), ships the new secret to
the surviving members inside a signed `{"t":"rekey","v":1,"gen",
"revoked":[…],"wraps":[…]}` op, drops the member's scope binding
(the whole member row goes when that was its last scope — a
bare-scoped member would silently become device-global), revokes
its `member-<id16>` onion client-auth grant, drops its tab
presence, and re-wraps the password vault when unlocked.  The
wrap is an ephemeral x25519 DH against the recipient's Ed25519
device pubkey converted to Montgomery form — recipients derive
their own x25519 static from the device.key seed
(SHA-512(seed)[:32]), so no extra key material or transport state
is needed and the same path works on tor and mem.  Applying a
rekey installs the secret only when its canonical stamp
(gen, lamport, device_id, op_id) passes the stored rotation
marker — concurrent rotations converge on the canonically-later
one — while `revoked` lists always land: every member drops the
device's binding and grant, so the cut propagates group-wide, and
a device finding ITSELF revoked tears the pair down
symmetrically.  Revoked devices are then refused at the Tor layer
(deauthorized), and any op they still sign is rejected as
unknown-signer.  The vault re-wrap re-seals only fields the
device itself last wrote — republishing a peer's field at a fresh
stamp would manufacture LWW recency.  Members that were locked
through a rotation keep reading: the passphrase probe falls back
to the one-deep previous-generation key.  Honest edges: a
re-pairing into the same profile after revocation still hits
`group_secret_install`'s loud conflict (the stale stored secret
is deliberate proof of old membership — clearing sync state is
the opt-out), and wraps the revoker did not include (asymmetric
pairing) leave that member diverged, exactly like a device that
was never in the group.

| Op | Class | Notes |
|----|-------|-------|
| `sync.devices_list` | Q | `{profile?}` → `{device_id, fingerprint, devices:[{device_id, fingerprint{words,emoji}, last_seen_ms, added_ms, onion_addr, device_global}]}` — this profile's paired devices only; `since: 7` |
| `sync.device_revoke` | C | `{profile?, device_id}` → `{ok, gen, member_removed, grant_revoked, wrapped_for, vault}` — rotate + revoke + report; `since: 7` |

## 9. Cutover decision table (landed — MB06/MB07)

Rule restated: an `ac_*` caller moves to `ac_mw_dispatch` only where
the envelope shrinks code — batch reads, list/tree queries, JSON-in/
JSON-out verdicts. Single scalar reads in hot paths and every §1
lifecycle op stay flat. The verify bar held: unchanged behavior +
unchanged autotest counts on both slices.

| Caller site | Flat today | Decision (MB06/MB07 landed) |
|-------------|-----------|------------------------------|
| `RustCoreSettings` (all ops: value/setValue/remove/contains/allKeys/clear/sync/importLegacyIni) | ac_* typed | **envelope** — settings.get/set/remove/contains/list/sync/import_ini; the handle is gone entirely, the payload's `"profile"` binds the scope per call; lazy `rc_settings_open` fallback when the bridge cannot serve |
| `RustCoreProfiles` registry ops (list/get/create/rename/setAvatar/delete/reload) | ac_* typed | **envelope** — profiles.list/get/create/rename/set_avatar/delete/reload on the shared-root spelling; explicit fixture roots keep the flat lane |
| `RustCoreProfiles::activeProfileId`/`setActiveProfileId` | rc_* | **flat** — the ambient persona is per-handle in-memory state; an envelope op would read a fresh handle's "default" and `set_active` would die with the call |
| `RustCoreProfiles` path derivations (profileDir/avatarPath/…) | rc_* | **flat** — pure path math, no store I/O to batch |
| `HistoryManager` mutations (add/updateTitle/remove/clear/clearSince/expire) | ac_hist_* | **envelope** — history.add/update_title/remove/clear/clear_since/expire; `m_hist` handle deleted, `"profile"` binds per call, `histAnchor()` + `rc_hist_*` stays the no-bridge fallback |
| `HistoryManager::load` (exists probe + full listing) | ac_history_exists + count/entry_at loop | **envelope** — history.exists stat + one history.query returns the whole deduped listing; the per-row entry_at loop is exactly what the batch read replaces |
| `RustHistorySuggestModel::refresh` (HistoryCompleter) | ac_hist_suggest on a minted-then-closed handle | **envelope** — history.suggest; the per-refresh handle mint disappears |
| `BookmarkNode`/`BookmarksManager` per-node ops (get/find/parent/child_*/attach/detach/destroy/set_*) | ac_bm_* | **flat** — opaque node handles cannot cross the frame (§2.3); every call site is already a single flat call on the model's `m_store`, JSON framing would only add code |
| `BookmarksManager` bulk ops (load/load_mem/save/import_html/export_html) | ac_bm_* | **flat** — one scalar FFI call each with nothing batchable on the Qt side; no envelope ops registered for them and none would shrink the call site |
| `SiteDecisionStore` all ops (get/set/remove/lookup/entries/snapshot/storePresent/reload/reset) | ac_sitedec_* on a minted AcPolicy | **envelope** — policy.sitedec_* (MB07 landed) — the handle is gone, `"profile"` binds per call; empty profileId still pins "default", `rc_set_data_dir` + `rc_sitedec_*` stays the no-bridge fallback |
| `DomainBlocklist::maybeCommit` reload | ac_blocklist_reload | **envelope** — policy.blocklist_reload (MB07 landed) |
| `TrancoList::maybeCommit` validation + reload | ac_tranco_file_entries + ac_tranco_reload | **envelope** — policy.tranco_file_entries/tranco_reload (MB07 landed) — binds the ambient persona's dir, matching the flat `dataFilePath` anchor |
| `SitePanel::siteTrustVerdict` | ac_sitetrust_lookup_opts | **envelope** — policy.sitetrust_lookup (MB07 landed) — host/https/probes in, verdict doc out |
| `NavigationPolicy` verdict probes (rc_policy_evaluate/cookie_filter/eval_generation, rc_blocklist_check, rc_tranco_rank, rc_lookalike_check, rc_ua_spoof*) | rc_* | **flat** — per-navigation/per-cookie scalar verdicts are the hot-path single reads §1 keeps flat; nothing to batch |
| `BrowserApplication` sessionSaveFor/sessionLoadFor/sessionClearFor | ac_session_* on a minted AcSession | **envelope** — session.save/load/clear (MB07 landed) — the manifest crosses as a JSON doc in `"session"`; load maps the flat lane's RC_NOT_FOUND/RC_CORRUPT onto the envelope error status |
| `BrowserProfile` markSessionStart/markSessionCleanExit/markSessionTerminated | ac_session_mark_* | **envelope** — session.mark_start/mark_exit/mark_terminated (MB07 landed) — `"profile":"default"` pins the marker's persona |
| `ClearPrivateData` session clear | rc_session_clear | **envelope** — session.clear (MB07 landed) |
| `main.cpp` self-test/probe legs (rc_session_clear/exists/save/decode + mark*/saveSession/restoreLastSession) | rc_* | **flat** — wire-level probes asserting the flat lane stays correct; the application-level calls inside them ride the envelope through the public API |
| cookies callers | none | **envelope-registered, flat Qt side** — zero Qt callers exist (RC15 ships no consumer yet); cookies.count/list/set are described+dispatched and probe-tested, no call site to cut over |
| `HistoryManager` iconPut/iconGet/iconClear/iconSweep | ac_icons_* on a cached AcIcons | **envelope** — icons.put/get/clear/sweep (MB07 landed) — PNGs cross base64'd, `"profile"` binds per call, `m_iconStore` deleted |
| `OpenSearchManager` registry ops (put/get/remove/rename/reorder/set_keywords/import/seed/restore/block/unblock/list/keywords/bindings) + `OpenSearchEngine::buildUrl` | rc_ose_* | **envelope** — opensearch.* (MB07 landed) — store is bound to `profiles/<active>/` (the flat `rustCoreEnsureDataDir` anchor), not the data root; raw XML descriptors cross as `"xml_b64"` |

## 10. Remaining open questions

Decided since MB01: the domain/op id tables (frozen in
`src/rustcore/src/mw/mod.rs`; `mw.describe` is the wire truth) and
the per-op payload field names (the mw handlers; `{"profile"}` is
the one universal field). Still deliberately open:

- Whether a binary (non-JSON) payload lane is ever needed — deferred;
  `flags` byte reserves the space. Session blob passthrough and icon
  PNGs are the candidates if JSON b64 overhead ever measures hot.
- Whether domains 17/18 ever grow ops — their resources are
  connection handles; an op would have to be self-contained to fit.
- Whether the declared-but-unproduced event topics
  (`policy/changed`, `profile/changed`, `session/saved`) get real
  producers — the registry reports `produced:false` until the emit
  sites exist; adding one is appending a topic field's worth of
  work, not an ABI event.
