# Arktos core C ABI — stability contract (`arktos_core.h`) — v2

Doc v2 (MB08): revised for ABI v9 — the mw envelope lane, the
namespaced event lane and the engine capability matrix joined the
facade under MB02–MB04, and §9 now lists every export with its
signature, thread rule and ownership rule verbatim from the header.

RC07. `src/rustcore/include/arktos_core.h` is the single stable ABI
every engine backend (QtWebEngine now, Servo/WebKit next, CEF only
as the RC11 spike) programs against. This document is the contract
both sides — the Rust implementation and its consumers — are bound
by. Where it and a code comment disagree, this file wins.

## 1. Scope

The facade is a thin ownership layer over the RC02–RC06 modules:

- settings (`AcSettings` — `RcSettings` verbatim), RC02
- profiles (`AcProfiles` — `RcProfiles` verbatim), RC03
- history (`AcHistory`), RC04
- bookmarks (`AcBookmarks`), RC05
- policy: site decisions, verdict evaluation, blocklist/Tranco,
  site-trust, lookalike, UA-spoof tables (`AcPolicy`), RC06
- sessions: per-profile window/tab session store + lifecycle
  marker (`AcSession`), RC13 — added at ABI v2
- cookies: per-profile cookie database (`AcCookies`), RC15 —
  added at ABI v3
- icons: per-profile favicon/site-icon cache (`AcIcons`), RC16 —
  added at ABI v4
- containers: per-profile container registry + storage-namespace
  map (`AcContainers`), RC19 — added at ABI v5
- notes: per-profile sidebar note list (`AcNotes` — `RcNotes`
  verbatim), RC21 — added at ABI v6
- mw envelope: the versioned command/query dispatch
  (`ac_mw_dispatch`, `ac_mw_version`), MB02 — added at ABI v7;
  the wire format and domain registry live in MW-ARCH §2/§7
- namespaced events: per-topic subscriber lane
  (`ac_events_subscribe`/`ac_events_unsubscribe`/`ac_events_topics`),
  MB03 — added at ABI v8
- engine capability matrix (`ac_engine_caps`), MB04 — added at
  ABI v9

It deliberately does NOT wrap the other rc_* surfaces (credentials,
autofill, opensearch, readability, vault, …). Those keep
their rc_* spellings until their own adoption rows land.

## 2. Versioning and growth rules

- **Append-only.** New functions and new `AcConfig` fields may only
  be appended. Existing signatures, struct field order/type, and
  enum values are frozen for the life of the ABI.
- **Never repurpose an enum value.** `AcStatus` (alias of
  `RcStatus`) values are frozen with the `RcStatus` enum in
  `rustcore.h`: `RC_OK=0`, `RC_INVALID_ARGUMENT=1`, `RC_IO=2`,
  `RC_LOCKED=3`, `RC_WRONG_PASSPHRASE=4`, `RC_CRYPTO=5`,
  `RC_NOT_INITIALIZED=6`, `RC_CORRUPT=7`, `RC_ALREADY_ENABLED=8`,
  `RC_NOT_ENABLED=9`, `RC_UNAVAILABLE=10`, `RC_NOT_FOUND=11`,
  `RC_EMPTY_PASSPHRASE=12`, `RC_WRONG_PASSWORD=13`,
  `RC_TAMPERED=14`, `RC_UNSUPPORTED_ALGO=15`. The `AC_ERR_*`
  aliases in the header resolve onto these values (`RC_CRYPTO`
  doubles as the internal/panic fallback). New failures get new
  numbers; retiring a value leaves a gap.
- **`ac_api_version()`** returns the library ABI version (`9`).
  It bumps only on ABI-visible additions — the probe/autotests pin
  it per release. (v2: `AcSession` + `ac_session_*`, RC13.
  v3: `AcCookies` + `ac_cookies_*`, RC15.
  v4: `AcIcons` + `ac_icons_*`, RC16.
  v5: `AcContainers` + `ac_containers_*`, RC19.
  v6: `AcNotes` + `ac_notes_*`, RC21.
  v7: `ac_mw_dispatch` + `ac_mw_version`, MB02.
  v8: `ac_events_*`, MB03.
  v9: `ac_engine_caps`, MB04.)
  The mw *envelope* schema version (`ac_mw_version()`, the frame's
  `version` byte) is a separate number — MW-ARCH §4 keeps the
  three-version story straight.
- **`ac_capabilities()`** is a per-module bitmask
  (`AC_CAP_SETTINGS=0x01` … `AC_CAP_POLICY=0x10`,
  `AC_CAP_SESSION=0x20`, `AC_CAP_COOKIES=0x40`,
  `AC_CAP_ICONS=0x80`, `AC_CAP_CONTAINERS=0x100`,
  `AC_CAP_NOTES=0x200`). A module built
  out of the library reports `0` — callers must test bits, not
  assume presence.
- **`AcConfig.struct_size`** is the caller's `sizeof(AcConfig)`.
  `ac_init` refuses a size smaller than the version it understands
  and refuses `api_version` newer than the library's own. New
  fields append only; a v1 caller zeros them implicitly by passing
  the smaller size once the struct grows (the library reads only
  what `struct_size` covers).

## 3. Ownership

- `AcCore *ac_init(const AcConfig *)` — single owner handle; free
  with `ac_shutdown`. `ac_shutdown` also frees every module handle
  the core minted and unregisters its change callback.
- Module handles come from the accessors
  (`ac_settings`, `ac_profiles`, `ac_history`, `ac_bookmarks`,
  `ac_policy`, `ac_session`, `ac_cookies`, `ac_icons`,
  `ac_containers`, `ac_notes`), are released
  individually with the matching
  `ac_*_close`, and **die with their core** — using a handle after
  `ac_shutdown` is a use-after-free.
- `ac_*_close(NULL)` is a no-op. `AcSettings`/`AcProfiles`/`AcNotes`
  handles may also be closed by
  `rc_settings_close`/`rc_profiles_close`/`rc_notes_close` —
  same types, same stores.
- Returned `char *` and `AcBuffer` are caller-owned: free with
  `ac_string_free` / `ac_buffer_free` (aliases of the rc_*
  destructors — the allocators are one pool). The `ac_mw_dispatch`
  and `ac_engine_caps` `*out` buffers and the `ac_events_topics`
  registry doc follow the same rule.
- `ac_last_error()` returns a NEW thread-local message copy each
  call — free it too.
- Event subscription ids (v8) are uint64 tokens, not pointers:
  released by `ac_events_unsubscribe` or wholesale by
  `ac_shutdown`; an id is only meaningful to the core that minted
  it — the token check keeps one core from dropping another's
  registration.
- No handle crosses `ac_mw_dispatch`: scoped payloads carry
  `{"profile":"<id>"|null}` and the dispatcher resolves the bound
  store per call (MW-ARCH §2.3).

## 4. Error model

Every fallible call returns `AcStatus`, or NULL/0/-1 where the
signature has no status channel (documented per function). On any
non-`AC_OK` return the thread-local error string is populated —
`ac_last_error()` fetches it; an empty string means "no detail".

## 5. Profile-bound handles

- `ac_settings(core, NULL)` binds the *global* settings scope;
  `"default"` or a slug binds `profiles/<id>/settings.json`. A
  non-slug id binds `"default"` (the
  `BrowserPaths::profileDataDir` normalization).
- `ac_history`, `ac_bookmarks`, `ac_policy` bind a profile at open
  — `NULL` or non-slug resolves to `"default"` — and **re-assert
  that binding on every operation**. The underlying stores are
  process-global today; the re-assert is a no-op when the binding
  already matches and a re-open/re-load otherwise. Callers never
  run `rc_set_data_dir` for facade handles.
- `ac_bookmarks` nuance: the bookmark tree is one process-global
  document. Re-anchoring flushes a displaced tree's pending edits
  to the file it was bound to (if that directory still exists) and
  then loads the bound file — in-memory edits follow their profile,
  never cross into another. `ac_bm_save(h, NULL)` writes the bound
  file; `ac_bm_load_mem` installs an anonymous tree that stays live
  until a foreign load displaces it.
- `ac_policy` nuance: `ac_sitetrust_*` calls also re-open the bound
  profile's `history.db` — the first-seen input can't leak another
  persona's visits into a verdict.
- `ac_session` nuance (v2): the handle carries only the resolved
  `profiles/<id>/` dir — every op is path-explicit under it
  (`session.dat`, `session.state`), so unlike the re-asserted
  stores above there is no process-global anchor to race at all.
  `NULL`/non-slug binds `"default"`. Session ops emit no change
  notifications — per-run scratch state, not shared service data.
- `ac_cookies` nuance (v3): the handle re-asserts
  `profiles/<id>/cookies.db` on every op like the history model —
  `NULL`/non-slug binds `"default"`. Mutations emit the `"cookies"`
  change topic stamped with the bound profile.
- **Cookie role honesty (v3).** `ac_cookies_role` /
  `ac_cookies_set_role` record whether the database is
  `AC_COOKIES_AUTHORITATIVE` (this file IS the jar — the Servo/WPE
  arrangement) or `AC_COOKIES_MIRROR` (an engine owns the real jar
  and this db is its durable shadow — the Chromium arrangement,
  where `QWebEngineCookieStore` cannot be displaced). The flag is
  persisted in the db's own meta table, the ops are identical
  either way, and callers MUST report which mode they actually run
  — never claim authoritative storage behind an engine-owned jar.
- **Cookie schema.** One row per `(container, domain, name, path)`;
  `container` is the persona-container partition (`""` = the
  default context, ContainerManager uuid-hex otherwise). Columns
  carry every field cookie policy and send-matching key on:
  `value`, `path`, `expires` (ms-epoch, NULL = session),
  `secure`, `httponly`, `samesite` (`QNetworkCookie::SameSite`
  ordinals 0–3), `host_only` (Qt-verbatim: leading-dot `Domain`
  imports as a domain cookie, no-dot or absent as host-exact),
  `created`. Session rows are swept at open (a persistent db can
  only have inherited them from a dead run) and via
  `ac_cookies_purge_session` at exit. Setting a row whose expiry
  already passed deletes it — the RFC6265 §5.3 eviction rule.
- **Cookie send-match.** `ac_cookies_for_url` answers with the
  Qt-jar-verified rules: host-only vs domain reach, RFC6265
  §5.1.4 boundary-aware paths, secure gating, expiry filtering
  (lazy deletion), the optional first-party SameSite filter, and
  §5.4 send order (longest path, then earliest created).
- **Cookie interchange.** `ac_cookies_import` /
  `ac_cookies_export` speak `QNetworkCookie::toRawForm()` — one
  raw line per cookie, the shape the vendored `NetworkCookieJar`'s
  saveState streams (testdata: `src/rustcore/testdata/cookies/
  export.txt`, emitted by a real Qt 6.12 build). Import skips
  lines lacking a `Domain` attribute — there is no request host
  to key them on.
- **Cookie gate.** `ac_cookies_gate` resolves a cookie attempt
  against the bound profile's `sitedecisions.json` `"cookie"`
  kind (block > allow > session > accept-policy) plus the
  caller-supplied third-party/keep/session-length inputs — the
  same precedence `CookieJar`'s IO-thread filter applies to
  Chromium, exposed for backends whose jar this store is.
- `ac_icons` nuance (v4): path-bound like `ac_session` — the
  handle carries the resolved `profiles/<id>/` dir and every op
  works under its `cache/icons/` lane (the disposable lane the
  profile vault's "cache" exclusion already holds unsealed —
  icon files never land in the data root proper). `NULL`/non-slug
  binds `"default"`. Ops take the PAGE url; the store derives the
  `historyIconKey` (lowercase host, or the whole url when
  hostless) so every page of a host shares one PNG — that is the
  dedup-by-host rule. Fetch stays engine-side (the backend
  already has the bytes); the store owns the index
  (`icons.json`), the file lifecycle, and the expiry sweep
  (`ac_icons_sweep(cutoff_ms)` — HistoryManager passes its
  history retention boundary). A profile's first touch folds the
  legacy sources in once — the pre-rustcore
  `<profile>/icons/<pct-host>.png` files and the `history.db`
  `icons` table — read-only, gated by a `cache/icons.done` stamp
  that survives an explicit `ac_icons_clear` so cleared icons
  cannot resurrect. Mutations emit the `"icons"` change topic
  stamped with the bound profile.
- `ac_containers` nuance (v5): the registry is a typed
  specialization over the bound profile's settings scope — every
  op opens that scope's `settings.json` fresh (a mid-process
  C++ mirror write is honored) and stamps `"containers"` with the
  bound profile.
- `ac_notes` nuance (v6): the handle IS an `RcNotes` like
  `AcSettings`/`AcProfiles` — minted bound to
  `profiles/<id>/notes.json` (NULL/non-slug resolves `"default"`),
  closed by `ac_notes_close` or `rc_notes_close`, and the rc/ac
  spellings mix freely on one handle. Two contract points differ
  from the write-through stores: mutations dirty the decoded list
  and `ac_notes_sync` (plus `ac_notes_close`) is what persists —
  the Qt AutoSaver debounce carried across the ABI — and the
  VOLATILE off-the-record binding lives only on the rc_*
  spelling (`rc_notes_open*(.., NULL)`); `ac_notes` always binds a
  persistent persona file. Mutations emit `"notes"` stamped with
  the bound profile — file-bound `RcNotes` handles stamp NULL.

## 6. Notifications

`ac_set_change_callback(core, cb, userdata)` registers at most one
callback per core (NULL `cb` unregisters; `ac_shutdown` clears it).
Signature: `cb(module_topic, profile_id_or_null, userdata)`.

- `module` is the store topic (`"settings"`, `"profiles"`,
  `"history"`, `"bookmarks"`, `"sitedec"`, `"uaspoof"`, …) — the
  same strings the legacy `rc_set_change_callback` emits.
- `profile_id` is the profile the handle was bound to, or NULL for
  global-scope and unscoped mutations. Legacy `rc_*` callers have
  no binding to report, so facade subscribers see a NULL profile
  for their traffic — this is how the two surfaces stay
  distinguishable.
- Invoked **synchronously on the mutating thread, outside every
  store lock** — re-entry into the library (reads, writes, another
  `ac_set_change_callback`, even `ac_shutdown` from a different
  thread) cannot deadlock. Both string arguments are borrowed:
  valid only for the call's duration; consumers that hop threads
  must copy them first (RustCoreBridge queues QString copies).
- Qt consumer: `RustCoreBridge::storeChangedFor(topic, profileId)`
  is the queued re-emit; the legacy `storeChanged(topic)` stays
  wired to the legacy callback so existing listeners see unchanged
  traffic.

### 6.1 Namespaced event lane (v8)

`ac_events_subscribe(core, topic_filter, cb, userdata)` registers a
per-core `AcEventCallback(topic, payload_json, userdata)` — the
opt-in typed sibling of the change callback. Filter grammar:
NULL/""/"*" receives everything, a trailing '/' ("history/") takes
a namespace, anything else is an exact topic. The payload is a v1
JSON doc `{"v":1,"topic":<t>,"profile":<id-or-null>, ...domain
fields}` — `v`/`topic`/`profile` are reserved envelope keys.
`ac_events_topics()` returns the declared registry as a JSON array
of `{topic,summary,fields,produced,seen}` — `produced` means an
emit site exists in-tree, `seen` means one fired this process
(both honesty bits, never claimed unverified). Same thread
contract: synchronous on the mutating thread, outside all locks,
borrowed strings. Qt consumer: `AcEventNotifier` (src/
aceventnotifier.*) queues each delivery into a
`storeChanged(QString topic, QByteArray payload)` signal.
Today `history/changed` is the produced topic; `policy/changed`,
`profile/changed`, `session/saved` are declared contract. The two
lanes are additive — a mutation that wants both audiences emits on
both; nothing existing was re-routed.

## 7. Panic safety

No Rust panic crosses the boundary — every entry point is wrapped
in `catch_unwind`; a panic degrades to NULL/`AC_ERR_INTERNAL` plus
a stashed `ac_last_error()` message. Bad pointers and malformed
input return errors, never UB — but pointers that ARE passed must
still satisfy their documented contract (NUL-terminated UTF-8,
`len`-sized readable buffers, writable out-params).

## 8. Legacy `rc_*` surface

The entire `rustcore.h` surface keeps compiling and stays
byte-compatible on disk — `ac_*` ops forward to the same `rc_*`
entry points, so mixing spellings is supported:

- `ac_settings`/`ac_profiles` return `RcSettings`/`RcProfiles`
  handles directly — `rc_settings_*`/`rc_profiles_*` calls on them
  are legal (and vice versa: `ac_*` ops on rc-opened handles work).
- `ac_*` writes are immediately visible to `rc_*` reads and the
  reverse — one store, two spellings, no divergence.
- `ac_settings_close`/`ac_profiles_close` accept rc-opened handles
  (they become owner-less gracefully); `rc_*_close` accepts
  ac-opened handles.
- The legacy callback (`rc_set_change_callback`, topic-only) is a
  separate registration from the facade lane — both fire for every
  mutation.

The RC08 row migrates Qt callers onto the ac_* spellings; rc_*
remains the compatibility floor afterwards — it is not being
deleted.

## 9. Export reference — every `ac_*` symbol

All 188 exports of `arktos_core.h` at ABI v9, verbatim signatures.
Column conventions:

- **Threads** — every export may be called from any thread; the
  entry further qualifies *what* is serialized. "serializes on the
  bound store's lock" means calls on one bound handle (or the
  envelope's per-call bind) queue internally — safe, not lock-free.
  Callback registrations are any-thread, but the *callback itself*
  always fires synchronously on the mutating thread (§6).
- **Returns / ownership** — what the caller must do with the
  result. `char *` → `ac_string_free`; `AcBuffer`/`*out` buffers →
  `ac_buffer_free`; handles → their `ac_*_close` (or `ac_shutdown`).
  A `—` means the scalar/`AcStatus` return is the whole answer.
- Args not listed as out-params are borrowed inputs — `const char *`
  is NUL-terminated UTF-8, `len`-sized buffers must be readable for
  exactly `len` bytes.

**housekeeping + version**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `uint32_t ac_api_version(void)` | any — no handle needed | — |
| `uint64_t ac_capabilities(void)` | any — no handle needed | — |
| `AcCore *ac_init(const AcConfig *cfg)` | any | caller owns — ac_shutdown |
| `void ac_shutdown(AcCore *core)` | any — frees the core, its handles + event subs | — |
| `char *ac_last_error(void)` | any — reads the calling thread's TLS slot | caller frees — ac_string_free |
| `void ac_string_free(char *s)` | any | — |
| `void ac_buffer_free(AcBuffer buf)` | any | — |
| `void ac_set_change_callback(AcCore *core, AcChangeCallback cb, void *userdata)` | any — cb fires on the mutating thread | — |
| `uint64_t ac_events_subscribe(AcCore *core, const char *topic_filter, AcEventCallback cb, void *userdata)` | any — cb fires on the mutating thread | sub id (0 = fail); ac_events_unsubscribe / dies with core |
| `AcStatus ac_events_unsubscribe(AcCore *core, uint64_t subscription_id)` | any — cb fires on the mutating thread | — |
| `char *ac_events_topics(void)` | any — no handle needed | caller frees — ac_string_free |
| `uint64_t ac_policy_eval_generation(void)` | any — no handle needed | — |

**module accessors + close**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcSettings *ac_settings(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcProfiles *ac_profiles(AcCore *core)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcHistory *ac_history(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `int ac_history_exists(AcCore *core, const char *profile_id)` | any | — |
| `AcBookmarks *ac_bookmarks(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcPolicy *ac_policy(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcSession *ac_session(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcCookies *ac_cookies(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcIcons *ac_icons(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcContainers *ac_containers(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `AcNotes *ac_notes(AcCore *core, const char *profile_id)` | any — mints a core-owned handle | caller closes — matching ac_*_close; dies with core |
| `void ac_settings_close(AcSettings *h)` | any — NULL is a no-op | — |
| `void ac_profiles_close(AcProfiles *h)` | any — NULL is a no-op | — |
| `void ac_hist_close(AcHistory *h)` | any — NULL is a no-op | — |
| `void ac_bm_close(AcBookmarks *h)` | any — NULL is a no-op | — |
| `void ac_policy_close(AcPolicy *h)` | any — NULL is a no-op | — |
| `void ac_session_close(AcSession *h)` | any — NULL is a no-op | — |
| `void ac_cookies_close(AcCookies *h)` | any — NULL is a no-op | — |
| `void ac_icons_close(AcIcons *h)` | any — NULL is a no-op | — |
| `void ac_containers_close(AcContainers *h)` | any — NULL is a no-op | — |
| `void ac_notes_close(AcNotes *h)` | any — NULL is a no-op | — |

**settings (RC02)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_settings_import_ini(AcSettings *h, const char *path)` | any — serializes on the bound store's lock | — |
| `int ac_settings_contains(AcSettings *h, const char *key)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_get(AcSettings *h, const char *key, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_settings_get_string(AcSettings *h, const char *key, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_settings_get_bool(AcSettings *h, const char *key, int *out)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `AcStatus ac_settings_get_int(AcSettings *h, const char *key, int64_t *out)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `AcStatus ac_settings_get_double(AcSettings *h, const char *key, double *out)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `AcStatus ac_settings_get_bytes(AcSettings *h, const char *key, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_settings_get_list(AcSettings *h, const char *key, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_settings_set_string(AcSettings *h, const char *key, const char *value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_set_bool(AcSettings *h, const char *key, int value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_set_int(AcSettings *h, const char *key, int64_t value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_set_double(AcSettings *h, const char *key, double value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_set_bytes(AcSettings *h, const char *key, const uint8_t *value, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_set_list(AcSettings *h, const char *key, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_settings_remove(AcSettings *h, const char *key)` | any — serializes on the bound store's lock | — |
| `char *ac_settings_list(AcSettings *h, const char *prefix)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_settings_sync(AcSettings *h)` | any — serializes on the bound store's lock | — |

**profiles (RC03)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_profiles_reload(AcProfiles *h)` | any — serializes on the bound store's lock | — |
| `char *ac_profiles_list(AcProfiles *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_get(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_create(AcProfiles *h, const char *display_name)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_profiles_rename(AcProfiles *h, const char *id, const char *name)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_profiles_set_avatar(AcProfiles *h, const char *id, const char *name_or_null)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_profiles_delete(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | — |
| `char *ac_profiles_active(AcProfiles *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_profiles_set_active(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | — |
| `char *ac_profiles_dir(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_cache_dir(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_engine_storage_dir(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_engine_cache_dir(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_settings_dir(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_avatar_path(AcProfiles *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_file(AcProfiles *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_profiles_slugify(const char *display_name)` | any — no handle needed | caller frees — ac_string_free |

**history (RC04)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `int64_t ac_hist_count(const AcHistory *h)` | any — serializes on the bound store's lock | — |
| `char *ac_hist_entry_at(const AcHistory *h, int64_t row)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_hist_add(const AcHistory *h, const char *url, const char *title, int64_t ts_ms)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_hist_update_title(const AcHistory *h, const char *url, const char *title)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_hist_remove(const AcHistory *h, const char *url, const char *title, int64_t ts_ms)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_hist_clear(const AcHistory *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_hist_clear_since(const AcHistory *h, int64_t since_ms)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_hist_expire_older_than(const AcHistory *h, int64_t cutoff_ms, int64_t *out_removed)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `char *ac_hist_query(const AcHistory *h, const char *spec_json)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_hist_icon_set(const AcHistory *h, const char *host, const uint8_t *png, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_hist_icon_get(const AcHistory *h, const char *host, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_hist_icon_clear(const AcHistory *h)` | any — serializes on the bound store's lock | — |
| `char *ac_hist_suggest(const AcHistory *h, const char *term, int64_t limit)` | any — serializes on the bound store's lock | caller frees — ac_string_free |

**bookmarks (RC05)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `uint64_t ac_bm_root(const AcBookmarks *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_reload(const AcBookmarks *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_load_mem(const AcBookmarks *h, const uint8_t *xbel, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_save(const AcBookmarks *h, const char *path_or_null)` | any — serializes on the bound store's lock | — |
| `int64_t ac_bm_child_count(const AcBookmarks *h, uint64_t node)` | any — serializes on the bound store's lock | — |
| `uint64_t ac_bm_child_at(const AcBookmarks *h, uint64_t node, int64_t row)` | any — serializes on the bound store's lock | — |
| `uint64_t ac_bm_parent(const AcBookmarks *h, uint64_t node)` | any — serializes on the bound store's lock | — |
| `char *ac_bm_get(const AcBookmarks *h, uint64_t node)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `uint64_t ac_bm_create(const AcBookmarks *h, uint64_t parent, int64_t row, const char *json)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_attach(const AcBookmarks *h, uint64_t parent, int64_t row, uint64_t node)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_detach(const AcBookmarks *h, uint64_t node)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_destroy(const AcBookmarks *h, uint64_t node)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_set_title(const AcBookmarks *h, uint64_t node, const char *value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_set_url(const AcBookmarks *h, uint64_t node, const char *value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_set_desc(const AcBookmarks *h, uint64_t node, const char *value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_set_expanded(const AcBookmarks *h, uint64_t node, int expanded)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_bm_set_tags(const AcBookmarks *h, uint64_t node, const char *json)` | any — serializes on the bound store's lock | — |
| `uint64_t ac_bm_find(const AcBookmarks *h, const char *url)` | any — serializes on the bound store's lock | — |
| `char *ac_bm_import_html(const AcBookmarks *h, uint64_t parent, const uint8_t *html, size_t len, int flags)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_bm_export_html(const AcBookmarks *h, const char *path)` | any — serializes on the bound store's lock | — |

**policy (RC06)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_sitedec_get(const AcPolicy *h, const char *kind, const char *key, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_sitedec_set(const AcPolicy *h, const char *kind, const char *key, const char *value)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_sitedec_remove(const AcPolicy *h, const char *kind, const char *key)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_sitedec_clear(const AcPolicy *h, const char *kind)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_sitedec_replace(const AcPolicy *h, const char *kind, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `char *ac_sitedec_list(const AcPolicy *h, const char *kind)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_sitedec_snapshot(const AcPolicy *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_sitedec_lookup(const AcPolicy *h, const char *kind, const char *host)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `int ac_sitedec_store_present(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_sitedec_reload(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_sitedec_reset(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_policy_load_snapshot(const AcPolicy *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `char *ac_policy_evaluate(const AcPolicy *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `int ac_policy_cookie_filter(const AcPolicy *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `char *ac_policy_call(const AcPolicy *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `int ac_blocklist_check(const AcPolicy *h, const char *host)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_blocklist_load(const AcPolicy *h, const uint8_t *text, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_blocklist_reload(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `size_t ac_blocklist_count(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `uint32_t ac_tranco_rank(const AcPolicy *h, const char *host)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_tranco_load(const AcPolicy *h, const uint8_t *text, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_tranco_reload(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `size_t ac_tranco_count(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `size_t ac_tranco_file_entries(const AcPolicy *h, const char *name)` | any — serializes on the bound store's lock | — |
| `char *ac_sitetrust_lookup(const AcPolicy *h, const char *host, int is_https)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_sitetrust_lookup_opts(const AcPolicy *h, const char *host, int is_https, uint32_t probes)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_sitetrust_reload(const AcPolicy *h)` | any — serializes on the bound store's lock | — |
| `char *ac_lookalike_check(const AcPolicy *h, const char *host)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_ua_spoof(const AcPolicy *h, const char *host)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_ua_spoof_set(const AcPolicy *h, const char *host, const char *ua)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_ua_spoof_remove(const AcPolicy *h, const char *host)` | any — serializes on the bound store's lock | — |
| `char *ac_ua_spoof_list(const AcPolicy *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |

**session (RC13)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_session_save(AcSession *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_session_load(AcSession *h, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `int ac_session_exists(const AcSession *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_session_clear(AcSession *h)` | any — serializes on the bound store's lock | — |
| `int ac_session_mark_start(AcSession *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_session_mark_exit(AcSession *h, int32_t code)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_session_mark_terminated(AcSession *h, int32_t signal)` | any — serializes on the bound store's lock | — |
| `int ac_session_dirty(const AcSession *h)` | any — serializes on the bound store's lock | — |
| `int ac_session_crash_count(const AcSession *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_session_last_exit(AcSession *h, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_session_set_restoring(AcSession *h, int on)` | any — serializes on the bound store's lock | — |
| `int ac_session_restoring(const AcSession *h)` | any — serializes on the bound store's lock | — |

**cookies (RC15)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_cookies_set_role(AcCookies *h, int64_t role)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_cookies_set(AcCookies *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_cookies_get(const AcCookies *h, const char *domain, const char *name, const char *path, const char *container_or_null, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_cookies_remove(const AcCookies *h, const char *domain, const char *name, const char *path, const char *container_or_null)` | any — serializes on the bound store's lock | — |
| `char *ac_cookies_list(const AcCookies *h, const char *container_or_null)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_cookies_for_url(const AcCookies *h, const uint8_t *request, size_t len)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_cookies_store_header(AcCookies *h, const char *url, const char *header, const char *container_or_null)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_cookies_gate(const AcCookies *h, const uint8_t *input, size_t len)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_cookies_import(AcCookies *h, const char *path, const char *container_or_null)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_cookies_export(const AcCookies *h, const char *path, const char *container_or_null)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_cookies_clear(AcCookies *h, const char *container_or_null)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_cookies_remove_for_host(AcCookies *h, const char *host, const char *container_or_null)` | any — serializes on the bound store's lock | — |
| `int64_t ac_cookies_count(const AcCookies *h, const char *container_or_null)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_cookies_purge_expired(AcCookies *h, int64_t now_ms)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_cookies_purge_session(AcCookies *h)` | any — serializes on the bound store's lock | — |

**icons (RC16)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_icons_put(const AcIcons *h, const char *url, const uint8_t *png, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_icons_get(const AcIcons *h, const char *url, AcBuffer *out)` | any — serializes on the bound store's lock | *out caller-owned — ac_buffer_free |
| `AcStatus ac_icons_remove(const AcIcons *h, const char *url)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_icons_clear(const AcIcons *h)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_icons_sweep(const AcIcons *h, int64_t cutoff_ms, int64_t *out_removed)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `int64_t ac_icons_count(const AcIcons *h)` | any — serializes on the bound store's lock | — |
| `char *ac_icons_key(const AcIcons *h, const char *url)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_icons_list(const AcIcons *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |

**containers (RC19)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `char *ac_containers_list(const AcContainers *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_containers_get(const AcContainers *h, const char *id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_containers_create(const AcContainers *h, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `AcStatus ac_containers_update(const AcContainers *h, const char *id, const uint8_t *json, size_t len)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_containers_remove(const AcContainers *h, const char *id)` | any — serializes on the bound store's lock | — |
| `char *ac_containers_namespace(const AcContainers *h, const char *container_id)` | any — serializes on the bound store's lock | caller frees — ac_string_free |

**notes (RC21)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `int64_t ac_notes_count(const AcNotes *h)` | any — serializes on the bound store's lock | — |
| `char *ac_notes_get(const AcNotes *h, int64_t row)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `char *ac_notes_list(const AcNotes *h)` | any — serializes on the bound store's lock | caller frees — ac_string_free |
| `int64_t ac_notes_insert(AcNotes *h, int64_t row, const char *title, const char *body)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_notes_set_title(AcNotes *h, int64_t row, const char *value, int *out_changed)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `AcStatus ac_notes_set_body(AcNotes *h, int64_t row, const char *value, int *out_changed)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `AcStatus ac_notes_set_markdown(AcNotes *h, int64_t row, int on, int *out_changed)` | any — serializes on the bound store's lock | scalar out-param(s) |
| `AcStatus ac_notes_remove(AcNotes *h, int64_t row, int64_t count)` | any — serializes on the bound store's lock | — |
| `AcStatus ac_notes_sync(AcNotes *h)` | any — serializes on the bound store's lock | — |
| `int ac_notes_is_persistent(const AcNotes *h)` | any — serializes on the bound store's lock | — |
| `int ac_notes_is_dirty(const AcNotes *h)` | any — serializes on the bound store's lock | — |

**mw envelope (v7)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `uint32_t ac_mw_version(void)` | any — no handle needed | — |
| `AcStatus ac_mw_dispatch(AcCore *core, const uint8_t *req, size_t req_len, AcBuffer *out)` | any — synchronous; ops serialize internally | *out caller-owned — ac_buffer_free |

The dispatch contract (transport status vs in-band error envelope,
frame layout, error-code vocabulary, per-call profile binding) is
documented in MW-ARCH §2 and mirrored on the declarations in
`arktos_core.h`; `mw.describe` (domain 0) is the live op registry.

**engine caps (v9)**

| Export | Threads | Returns / ownership |
|--------|---------|---------------------|
| `AcStatus ac_engine_caps(const char *engine_id, AcBuffer *out)` | any — no handle needed | *out caller-owned — ac_buffer_free |

`engine_id` is `"qtwebengine"` (alias `"webengine"`), `"servo"` or
`"webkit"`; NULL/"" emits the whole matrix. The table is static —
it reports what each engine supports when its backend is present,
never whether the backend resolved this run.

## 10. Toolchain/build

The facade needs nothing beyond the RC02–06 crate: same staticlib
(`libarktos_rustcore.a`), same `CONFIG+=rustcore` /
`CONFIG+=no-rust` gating in `rustcore.pri` — a no-rust build simply
compiles none of this. `tools/acprobe.c` is the freestanding C
consumer probe: `cc -Iinclude tools/acprobe.c
target/release/libarktos_rustcore.a -ldl -lpthread -lm`.
