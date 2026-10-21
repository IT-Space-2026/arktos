# RUSTCORE-ARCH — Arktos Rust browser-core target architecture (RC01)

Audit of `src/rustcore` and the C++ service surface it must absorb, plus
the target module layout, ownership rules, per-store format decisions,
the `arktos_core.h` facade sketch, and the migration/parity strategy.

Status of record: this audit found the retired rust lane
(`.devin/Arora-Task-Rust.md`, RCORE01–05 + SEC/TRUST/ENG/OSE/UAG rows)
already landed far more than "credentials + urlstrip" — **the bookmark,
history, site-decision, request-policy, session, autofill, opensearch,
blocklist, tranco, UA and profile-vault stores are already Rust and
already consumed by the Qt shell under `CONFIG+=rustcore`** (default-on
per `.qmake.conf` RDEF01).  The genuinely missing stores are **settings**
(RC02) and the **profile registry** (RC03).  RC04/RC05/RC06 reduce to
compat-verification + facade routing, not new modules — the per-task
sections below say what each still owes.

---

## 1. What exists today

### 1.1 rustcore crate (`src/rustcore`, `arktos-rustcore` staticlib+cdylib)

C ABI in `include/rustcore.h` (~150 `rc_*` entry points).  Conventions,
uniform across every module (error.rs / notify.rs / store.rs):

- `RcStatus` enum result + `rc_last_error_message()` thread-local string.
- `RcBuffer` for byte results (`rc_buffer_free`), `char*` for strings
  (`rc_string_free`); UTF-8 in/out; no Qt types cross the boundary.
- Opaque integer handles (bookmarks) and process-global stores behind
  mutexes; `catch_unwind` on every entry point (ffi_surface.rs drives
  the whole surface in tests, COV05).
- One change callback — `rc_set_change_callback(cb, userdata)` with a
  topic string; `RustCoreBridge` re-emits `storeChanged(topic)` as a
  queued Qt signal (GUI-thread safe).
- Persistence: `store::atomic_write` = temp+fsync+rename, 0600 —
  the QSaveFile contract, byte-for-byte.
- Anchor: `rc_set_data_dir(path)` — one process-global data dir; every
  module's `DirCache` re-stamps on change and drops cached state
  (credential keys, decoded maps, memoized policy verdicts via
  `policy::bump_eval_generation`).

Landed modules (src/rustcore/src/*.rs):

| Module | Store / format | rc_* surface | Qt consumer |
|--------|----------------|--------------|-------------|
| store + cred | `securestore.key` (0600), `securestore.kdf` ("ARKDF1" Argon2id 64MiB/t3/p4 + verifier), `credentials.dat` ("ARSEC1" AES-256-GCM, tag-first) — **byte-identical to the C++ it replaced** | `rc_open/unlock/lock/seal/key_file_*/kdf_*/cred_*` | `securestore.cpp` 1:1 |
| autofill | `autofill-store.dat` — sealed under the SAME custody as credentials | `rc_autofill_*` | `autofillmanager.cpp` |
| bookmarks | `bookmarks.xbel` per profile — canonical, quick-xml transliteration of XbelReader/Writer grammar | `rc_bm_*` (handle-addressed lazy tree, attach/detach for undo, HTML import/export) | `bookmarksmanager/node/model` |
| history | `history.db` per profile — rusqlite WAL: visits + per-host icon PNGs; legacy QDataStream one-shot import | `rc_hist_*` | `historymanager.cpp` |
| session | `session.dat` per profile — versioned ARSS binary + JSON manifest, opaque engine blobs | `rc_session_*` | `browserapplication.cpp`/`tabwidget` |
| sitedecisions | `sitedecisions.json` per profile — `{version, kinds{kind{key:val}}}`; kinds: webperm/js/container/popup/http-allow/cookie/adblock/uaspoof/urlclean | `rc_sitedec_*` | `sitedecisionstore.cpp` façade |
| policy | stateless verdict fns over JSON manifests (+ bounded downgrade/session registries) | `rc_policy_evaluate/cookie_filter/load_snapshot/call` | `privacyrequestinterceptor`, `cookiejar` |
| blocklist | vendored seed + `<datadir>/blocklist-domains.txt` override; exact+suffix host match | `rc_blocklist_*` | request policy |
| tranco | vendored top-100k seed + `<datadir>/tranco-list.dat` (zip) + `majestic-million.csv`, best-rank-wins merge | `rc_tranco_*` | `sitetrust`, `lookalike` |
| urlstrip | vendored ClearURLs rules + data-dir override | `rc_urlstrip_*` | request policy |
| opensearch | `searchengines.json` per profile — engines/keywords/order/removed-bundled | `rc_ose_*` | opensearch manager |
| useragent | stateless UA build + presets + per-site spoof (rides sitedecisions "uaspoof") | `rc_ua_*` | interceptor/profiles |
| profilevault | "ARV1" sealed profile archives — wrapped vault key, chunked AES-GCM / Threefish opt-in | `rc_pv_*` (via store/seal family) | `profilevault.cpp` |
| parsers | memory-safe gates: OpenSearch, gupdate manifests, suggestions, XBEL structural check | `rc_opensearch_parse`, `rc_updatemanifest_parse`, `rc_suggest_parse`, `rc_xbel_check` | feed/parser call sites |
| omnibox / palette / lookalike / sitetrust / bidi / clearplan / qrcode / readability / pdfdoc / pdfsanitize / tlsprobe / extverify / bookmarkhtml | stateless or read-only over the above | assorted | thin Qt shells |

### 1.2 The C++ service surface and its disposition

Per-store keep/move + format decisions (details + rationale in §4):

| C++ surface | File(s) today | Decision | Format |
|-------------|---------------|----------|--------|
| Per-profile settings | `profilesettings.*` — dual-store façade: classified keys → `profiles/<id>/settings.ini`, else global QSettings ini; read fallback profile→global; `migrateFromGlobal` marker | **MOVE → `settings` module (RC02)** | New `settings.json` (typed, versioned, atomic); one-shot READ import of both ini files — see §4.1 |
| Profile registry | `profileregistry.*` + `profilestore.*` (**duplicated pair** — ProfileStore is the MPROF05 lane-fallback implementing the same MPROF01 file contract) | **MOVE → `profiles` module (RC03)** — one Rust registry ends the duplication | Keep `profiles.json` byte-compatible (`{"version":1,"profiles":[{id,name,avatar,createdAt}]}`) |
| Profile identity/avatar | `profileidentity.*` | **SPLIT** — name/avatar fields ride the registry; PNG decode/scale/crop stays Qt (QPixmap pipeline); `profiles/<id>/profile-avatar.png` filename owned by registry | unchanged |
| Path authority | `browserpaths.*` | **STAYS C++** — Rust mirrors `dataRoot()/profiles/<id>/` + `engineStorageDir` layout; never invents a second one (RC03 verifies equality on Linux) | n/a |
| History | `history/historymanager.*` | **DONE (RCORE02b)** — manager is already a Qt adapter over `rc_hist_*`; RC04 owes facade routing + residual gap check only | `history.db` sqlite WAL canonical; legacy binary import once |
| Bookmarks | `bookmarks/*` + `xbel/` | **DONE (RCORE02a)** — tree/model adapters over `rc_bm_*`; C++ XbelReader/Writer remain the no-rust fallback | `bookmarks.xbel` stays canonical forever (user interchange format) |
| Site decisions | `sitedecisionstore.*` | **DONE (SITED01)** — already a façade over `rc_sitedec_*`; RC06 folds it into the `policy` facade | `sitedecisions.json` canonical |
| Domain blocklist | `network/domainblocklist.*` | **SPLIT — already so**: matching in `blocklist.rs`; **fetcher stays C++** (consent-gated HTTPS fetch via app NAM; Rust never downloads lists) | `blocklist-domains.txt` drop-file contract |
| Tranco/Majestic | `network/trancolist.*` | **SPLIT — already so**: rank table in `tranco.rs`; **fetcher stays C++** | `tranco-list.dat` + `majestic-million.csv` drop files |
| Cookie jar | `network/cookiejar/cookiejar.*` | **SPLIT — mostly done**: accept rules/exceptions already in sitedecisions "cookie" kind (per-profile); global policy keys (`cookies` group) move with RC02 settings; **jar storage stays engine-owned** (QWebEngineCookieStore inside `profiles/<id>/QtWebEngine`) — `rc_policy_cookie_filter` already evaluates attempts | mixed — see §4.6 |
| `networkcookiejar/` | vendored Qt demo jar | **STAYS C++** — uncompiled legacy for the app-side QNAM (MIG04 may revive); not a browser-store surface | n/a |
| FFI conventions | `rustcore.h` + `rustcorebridge.*` | **BASE** — RC07 grows `arktos_core.h` facade over this surface; bridge v2 consumes it | n/a |
| Downloads | `src/rustdl` (rustdownloadengine) | separate crate/lane — shares conventions, out of RC01 scope | n/a |
| Engine storage | `BrowserPaths::engineStorageDir(profileId)` = `profiles/<id>/QtWebEngine` (+ `cache/QtWebEngine`) | **STAYS per-engine forever** — cookies, LocalStorage/IndexedDB, service workers, visited links, HTTP cache are engine-private | engine's own |

---

## 2. Target module layout

Existing modules stay where they are (lib.rs §1.1 is the registry).
Additions:

```
src/rustcore/src/
  settings.rs   (RC02)  typed per-profile KV: string/bool/i64/f64/bytes;
                        get/set/remove/list-prefix; two scopes:
                        profile (profiles/<id>/settings.json) and
                        global (<data root>/settings.json) — the Qt
                        adapter keeps isProfileKey() routing.
                        Read-path imports the legacy ini pair once.
  profiles.rs   (RC03)  registry: list/create/rename/delete,
                        active-id tracking, avatar file reference;
                        profiles.json byte-compatible with MPROF01;
                        per-profile data/cache/settings root paths
                        mirror BrowserPaths exactly (Linux:
                        QStandardPaths::AppDataLocation/profiles/<id>).
  facade.rs     (RC07)  AcCore: owns per-profile module handles +
                        the change-callback dispatch; re-exports the
                        rc_* surface behind ac_* accessors.
```

Independence rule (hard constraint): each store module is usable alone
— `settings` must not depend on `profiles` compiling; the facade is the
only integration point.  Existing modules already follow this (shared
deps limited to error/store/notify/util).

## 3. Ownership rules

- **Rust owns the store**: file format, schema versioning, atomicity,
  corruption tolerance, concurrency.  Qt never opens a rustcore file.
- **Qt owns the view**: models, dialogs, pixmaps, QSettings emulation
  shims, engine objects.  Rust never sees a Qt type.
- **One writer per file**: once a store migrates, only Rust writes it;
  the legacy file is read-once import material, then inert.
- **No Qt types across the ABI** — strings `char*` UTF-8, bytes
  `RcBuffer`, structured results JSON or handle accessors (the lazy
  per-node `rc_bm_*` pattern for trees — never bulk marshaling).
- **Profile scoping is explicit**: today's single `rc_set_data_dir` +
  per-op `rustCoreEnsureDataDirFor(id)` re-anchoring is the wart the
  facade fixes — `ac_history(core, profile_id)` returns a handle bound
  at open time, so alternating personas stops being a global-state
  switch (and stops re-stamping every DirCache).  Legacy `rc_*` calls
  keep their process-global semantics until RC08 migrates callers.
- **Notifications**: one callback, topics `"<module>"` today; facade v1
  widens to `(module, profile_id)` so per-persona views invalidate only
  their own store.
- **Rust never does network I/O for lists** — DomainBlocklist/TrancoList
  fetchers stay Qt (app NAM + remoteListsAllowed consent + HTTPS rule);
  the data-dir drop files are the handoff.
- **Engines report, the shell stores**: backends emit visit/bookmark/
  decision intent through `EngineInterface`; no engine writes a
  rustcore file directly.  Engine-private storage (Chromium profile,
  Servo/WebKit equivalents) stays inside `engineStorageDir` and is
  sealed/unsealed with the profile vault as today.
- **Honest capabilities**: where a backend lacks a seam (e.g. cookie
  injection on Servo), `Backend::capabilities()` records the gap —
  never fake parity through the facade.

## 4. Per-store format decisions

Rule: reuse the existing on-disk format wherever the C++ fallback must
keep reading it or users interchange it; introduce a new format only
with a one-shot importer that never mutates the source until the copy
verifies.  User data survives the cutover byte-for-byte.

| Store | On-disk | Decision |
|-------|---------|----------|
| credentials/autofill | `securestore.key/.kdf`, `credentials.dat`, `autofill-store.dat` | **keep** — ARSEC1/ARKDF1 frozen byte-identical by RCORE01/05 |
| bookmarks | `profiles/<id>/bookmarks.xbel` | **keep XBEL** — interchange format; users export/import it |
| history | `profiles/<id>/history` (QDataStream legacy) → `history.db` | **sqlite canonical** (landed); legacy import once, source untouched until verified |
| session | `profiles/<id>/session.dat` | **keep ARSS v1** — opaque engine blobs survive engine swaps |
| site decisions | `profiles/<id>/sitedecisions.json` | **keep** — landed SITED01; kind strings are wire contract, never rename without migration |
| settings | `profiles/<id>/settings.ini` + global QSettings ini | **new `settings.json`** — see §4.1 |
| profiles registry | `<data root>/profiles.json` | **keep byte-compatible** — already the contract two C++ impls share; moving it to Rust retires the duplication |
| blocklist/tranco | `blocklist-domains.txt`, `tranco-list.dat`, `majestic-million.csv` | **keep drop-file contract** — fetchers stay C++ |
| searchengines | `profiles/<id>/searchengines.json` | **keep** |
| engine storage | `profiles/<id>/QtWebEngine/` | **stays engine-owned** |
| cookie jar | inside engine profile | **stays engine-owned** (see §4.6) |

### 4.1 settings format — why JSON, not ini-canonical

RC02 prescribes JSON + ini import; this audit confirms it:

- QSettings IniFormat serializes QVariant with type escapes
  (`@Variant(...)`, QByteArray `%xx`, array `prefix/size` pseudo-keys,
  `@Invalid()`).  A Rust *writer* reproducing that byte-exactly is
  fragile; a Rust *reader* for the subset the tree actually uses
  (strings, bools, ints, doubles, byte arrays, string lists, arrays) is
  tractable — so ini stays a one-way import source, not the format.
- The typed store wants real bool/int/double/bytes — ini's everything-
  is-a-string loses that under QVariant round-trips.
- Divergence caveat (recorded honestly): after first write, the no-rust
  build reads stale ini.  Accepted — RDEF01 makes rust the default
  path; the no-rust build is the degradation floor, not a co-equal
  writer.  The ini files are never deleted (import is copy-not-move,
  matching `migrateFromGlobal`'s precedent).
- Import merge order mirrors ProfileSettings read semantics:
  `settings.ini` (profile store) wins; the global ini contributes
  `isProfileKey()`-classified keys not already present — the
  classification table (`kProfileGroups`/`kProfileKeys`/
  `kGlobalExceptions` in profilesettings.cpp) moves into the importer
  verbatim so a future engine shares it.
- Scope: profile-scope keys migrate (the RC02 "per-profile" brief).
  App-global keys (crash guards, engine defaults, machine-global list
  stamps — the `kGlobalExceptions` set and unclassified keys) stay on
  the global QSettings file in v1; a `global` scope exists in the
  module API so a later wave can move them without an ABI change.

### 4.6 cookie surface split

- Exception lists (block/allow/allow-for-session) — already in
  sitedecisions `cookie` kind, per-profile (SITED01 + MPROF02).
- Accept/keep policies, third-party block, session length — ProfileSettings
  `cookies` group → moves with the settings module.
- The jar itself (cookie objects, expiry enforcement) is
  QWebEngineCookieStore inside the engine profile — engine-owned;
  `rc_policy_cookie_filter` already evaluates attempts against a
  caller-supplied rule snapshot.  For non-Chromium engines the same
  filter verdict applies through `Engine::Profile::setCookieFilter`;
  where the engine can't honor it (Servo), capabilities() says so.

## 5. Facade sketch — `include/arktos_core.h` v1 (RC07)

Thin ownership layer over the existing modules; does NOT replace the
`rc_*` surface (kept compiling until RC08 migrates callers).

```c
typedef struct AcCore AcCore;          // opaque
typedef struct AcSettings AcSettings;  // per-scope KV handle
typedef struct AcProfiles AcProfiles;  // registry handle
typedef struct AcHistory AcHistory;    // per-profile store handle
typedef struct AcBookmarks AcBookmarks;
typedef struct AcPolicy AcPolicy;      // sitedecisions + verdicts

typedef enum AcStatus { AC_OK=0, AC_INVALID=1, AC_IO=2, AC_LOCKED=3,
    AC_CORRUPT=7, AC_NOT_FOUND=11, ... } AcStatus;   // mirrors RcStatus

typedef struct AcConfig {
    const char *data_root;    // BrowserPaths::dataRoot()
    const char *app_name;     // path derivation for non-Qt consumers
    uint32_t    api_version;  // caller's expected ABI
} AcConfig;

uint32_t  ac_api_version(void);
uint64_t  ac_capabilities(void);       // per-module present bits
AcCore   *ac_init(const AcConfig *cfg);
void      ac_shutdown(AcCore *);
const char *ac_last_error(void);       // thread-local; ac_string_free

AcSettings  *ac_settings(AcCore *, const char *profile_id /* NULL=global */);
AcProfiles  *ac_profiles(AcCore *);
AcHistory   *ac_history(AcCore *, const char *profile_id);
AcBookmarks *ac_bookmarks(AcCore *, const char *profile_id);
AcPolicy    *ac_policy(AcCore *, const char *profile_id);

void ac_set_change_callback(AcCore *,
    void (*cb)(const char *module, const char *profile_id, void *ud),
    void *userdata);
```

Key semantic change vs today: **module handles are profile-bound at
open**, retiring `rustCoreEnsureDataDirFor` re-anchoring for facade
callers.  Handles own (or snapshot) their anchor; `ac_shutdown` drops
all.  The legacy `rc_*` entry points forward to the same underlying
stores so the two surfaces never diverge on disk.

Stability rules (full contract lands in `.devin/RUSTCORE-ABI.md` at RC07):
append-only symbols; enum values never repurposed; `AcConfig` is
size/version-tagged; every entry point panic-safe.

## 6. Migration & parity-test strategy

Per store, the same five steps (proven by RCORE01–05/SITED01):

1. Rust module + `rc_*`/`ac_*` entry points; `ffi_surface.rs` gains
   coverage for the new symbols.
2. Qt adapter keeps its public API 1:1 (ProfileSettings, ProfileStore/
   Registry, HistoryManager, BookmarksManager, SiteDecisionStore
   precedents) — callers never learn the store moved.
3. Format bridge: fixture written by the C++ implementation → read via
   FFI → values identical; for one-format stores, byte-level round-trip.
4. Both builds verified: `qmake` default (rust on, RDEF01) and
   `CONFIG+=no-rust` — the Qt fallback path must still compile+link.
5. Existing autotests are the parity bar — run twice: rust path and
   no-rust path (tst_historymanager, tst_bookmarksmodel,
   tst_securestore, tst_autofill, tst_profile*, tst_sitepanel,
   tst_settings* per RC08's gate) plus `make check` end to end.

RC-order implications of this audit:

- **RC02 (settings)** — real new module; the ini-import fixture is the
  critical test (write a real ProfileSettings ini pair from C++, open
  via FFI, values match incl. the group/array forms).
- **RC03 (profiles)** — real new module; must produce identical
  `profileDataDir` roots to BrowserPaths on Linux/macOS/Windows and
  read the existing profiles.json untouched (MPROF01 contract).
- **RC04/RC05/RC06** — stores + FFI + Qt adapters already landed;
  residual work is (a) audit residual Qt-side persistence
  (history icon dirs, bookmark autosave topics), (b) port onto the
  facade handles, (c) the spec's verify fixtures — most already exist
  as landed tests (legacyImport, XBEL round-trip, sitedecisions
  fixture); rerun + cite rather than rewrite.
- **RC08** — adoption gate: the parity suite decides, run twice as
  specced.  Nothing here relaxes it.
- **RC09** — servo/webkit consume the same `AcCore`; capability gaps
  recorded honestly.
- **RC11** — CEF spike audits ~1398 QWebEngine touchpoints (measured
  this run) against the facade; CEF replaces the Chromium embed only.

## 7. Hard-constraint checklist

- [x] Profile data stays under BrowserPaths' per-OS roots — every store
  anchors `profiles/<id>/`; the registry is the only data-root file.
- [x] No Qt types cross the ABI — holds today, enforced for new modules.
- [x] Every module independently usable — keep it that way; the facade
  is composition, not a dependency hub.
- [x] User data sacred — every move is keep-format or
  import-once-then-verify; nothing is a clean-room rewrite.
- [x] No-rust build stays a full browser — every migrated store keeps
  its Qt fallback path behind `!no-rust` gating in the .pri files.
