# Arktos core C ABI — stability contract (`arktos_core.h`)

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

It deliberately does NOT wrap the other rc_* surfaces (credentials,
autofill, opensearch, readability, vault, …). Those keep
their rc_* spellings until their own adoption rows land.

## 2. Versioning and growth rules

- **Append-only.** New functions and new `AcConfig` fields may only
  be appended. Existing signatures, struct field order/type, and
  enum values are frozen for the life of the ABI.
- **Never repurpose an enum value.** `AcStatus` (alias of
  `RcStatus`) values are stable: `RC_OK=0`, `RC_ERR_GENERIC=1`,
  `RC_ERR_NOT_FOUND=2`, `RC_ERR_INVALID=3`, `RC_ERR_IO=4`,
  `RC_ERR_WRONG_PASSWORD=5`, `RC_ERR_TAMPERED=6`,
  `RC_ERR_INTERNAL=7`. New failures get new numbers; retiring a
  value leaves a gap.
- **`ac_api_version()`** returns the library ABI version (`4`).
  It bumps only on ABI-visible additions — the probe/autotests pin
  it per release. (v2: `AcSession` + `ac_session_*`, RC13.
  v3: `AcCookies` + `ac_cookies_*`, RC15.
  v4: `AcIcons` + `ac_icons_*`, RC16.)
- **`ac_capabilities()`** is a per-module bitmask
  (`AC_CAP_SETTINGS=0x01` … `AC_CAP_POLICY=0x10`,
  `AC_CAP_SESSION=0x20`, `AC_CAP_COOKIES=0x40`,
  `AC_CAP_ICONS=0x80`). A module built
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
  `ac_policy`, `ac_session`, `ac_cookies`, `ac_icons`), are released
  individually with the matching
  `ac_*_close`, and **die with their core** — using a handle after
  `ac_shutdown` is a use-after-free.
- `ac_*_close(NULL)` is a no-op. `AcSettings`/`AcProfiles` handles
  may also be closed by `rc_settings_close`/`rc_profiles_close` —
  same types, same stores.
- Returned `char *` and `AcBuffer` are caller-owned: free with
  `ac_string_free` / `ac_buffer_free` (aliases of the rc_*
  destructors — the allocators are one pool).
- `ac_last_error()` returns a NEW thread-local message copy each
  call — free it too.

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

## 9. Toolchain/build

The facade needs nothing beyond the RC02–06 crate: same staticlib
(`libarktos_rustcore.a`), same `CONFIG+=rustcore` /
`CONFIG+=no-rust` gating in `rustcore.pri` — a no-rust build simply
compiles none of this. `tools/acprobe.c` is the freestanding C
consumer probe: `cc -Iinclude tools/acprobe.c
target/release/libarktos_rustcore.a -ldl -lpthread -lm`.
