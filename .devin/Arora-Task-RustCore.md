# Devin autonomous task list — Arktos RUST-CORE LANE (branch rust-core, worktree ../arora-rustcore)

Rust browser-core queue driven by `.devin/taskloop-rustcore.sh`. Goal: the
gradual Rust foundation agreed in the CEF discussion — move every
engine-agnostic browser service (settings, profiles, history, bookmarks,
cookie/storage policy, trust data) out of Qt/C++ into `src/rustcore` behind
one stable C ABI, then make each engine backend consume that core. CEF is
only a spike at the end — NOT a migration.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting, `done` only after verifying.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- NEVER edit `.devin/Arora-Task.md` — that is the main lane's queue.
- Cross-lane deps: a note may reference main-lane tasks via needs:task:X —
  check `/home/szefi/Documents/arora/.devin/Arora-Task.md` for that row's
  status before starting.
- Append a one-line summary to `.devin/WORKLOG.md` when a task completes.
- Format: the table holds a one-line summary per row; the FULL spec
  lives in the `### <id>` section under `## Task details`. Keep table
  cells short — long text belongs in the details section.
- ALL writes to this file MUST go through `.devin/taskfile.sh`
  (flock-serialized — direct edits race the loop's row updates and
  lose data): `set <ID> field=…` for row cells, `append-detail <ID>`
  (stdin) for the `### <id>` section, `with-lock -- <cmd>` otherwise.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| RC01 | Rust-core architecture audit + target design doc | done | 1 | audit landed .devin/RUSTCORE-ARCH.md — full store map w/ keep-move decisions + format choices (settings+profiles are the real gaps; RC04-06 stores already landed in retired lane); facade sketch + per-profile handle scoping spec; verified cargo test 270/270 green |
| RC02 | rustcore `settings` module — typed per-profile key-value store + FFI | done | 1 | settings store + ini import + rc_settings_* FFI + RustCoreSettings landed; verified cargo 282/282, real-QSettings ini via FFI 18/18, rust+no-rust make -j2 link clean; fixed pre-existing no-rust tst_cookiejar include bug |
| RC03 | rustcore `profiles` module — persona registry + roots under home dir + FFI | done | 0 | profiles.json registry in rustcore behind rc_profiles_* (byte-compatible QJsonDocument::Indented writer), RcProfiles handle + RustCoreProfiles wrapper, BrowserPaths layout mirrored; verified cargo 292/292, tst_rustprofiles 6/6, rust + no-rust make -j2 clean |
| RC04 | rustcore `history` module — visit/query/clear, compat with existing store | done | 0 | history module verified + completed: store was already landed (sqlite history.db, legacy QDataStream import, icons/hosts/suggest); added rc_hist_query (term/date-range/limit-offset) + rc_hist_expire_older_than bulk retention; verified cargo 294/294, tst_historymanager 49/49 + historyui 8/8 + filtermodel 37/37 offscreen, rust + no-rust make -j2 clean |
| RC05 | rustcore `bookmarks` module — tree ops + XBEL import/export compat | done | 0 | bookmarks store verified + XBEL export fixed: <desc> now emitted as child element on folders+bookmarks (XbelWriter shape; was bookmark-only attr C++ reader ignores, folder desc dropped); legacy attr form still parses; new tst_xbel rustStoreInterchange proves C++-write->rust-edit->rust-save->C++-read identical tree; cargo 296/296, xbel 16/16 + bookmark node/model/html/dialog tests all pass offscreen, rust+no-rust make -j2 clean |
| RC06 | rustcore `policy` module — cookie/storage/site-decision/trust data + FFI | done | 0 | audit: policy surface already landed end to end (sitedecisions + per-profile façade + legacy imports, blocklist/tranco/majestic read-path, sitetrust, policy verdicts); added FFI-parity legs — verified cargo 296/296, sitedecisions 9/9, trancolist 9/9, domainblock 14/14 + 6 consumer suites green offscreen, rust+no-rust compile clean |
| RC07 | Stable `arktos_core.h` C ABI facade v1 + RustCoreBridge v2 | done | 0 | arktos_core.h facade: AcCore + profile-bound ac_* handles over shared rc_* stores, (module,profile) cb lane, bridge v2 + ABI doc — cargo 299+1preexisting, acprobe OK, make clean, autotests green |
| RC08 | QtWebEngine adoption — services read/write through facade, parity proof | done | 0 | facade adoption across settings/profiles/history/bookmarks/policy consumers w/ rc_* fallback; fixed root-drift, bm UAF + stale-tree remint, profile_dir mkpath — 19 parity binaries 0-fail, rust+no-rust clean, cargo 299+1preexisting |
| RC09 | Servo/WebKit backends consume the same facade + capability reporting | done | 0 | Servo/WebKit profiles bound to window persona: shared profileId() resolver, EngineTab history->instanceFor(persona), servo persona-named profile cache, persona-scoped adblock/downloads; smoke quick_exit on pre-existing teardown segv — verified: --servo-smoke all legs PASS incl history-recorded rc=0, webkit=1 build clean, no-rust TUs clean |
| RC10 | Packaging: new crates linked + staged in deb/rpm/flatpak/dmg/exe legs | in_progress | 0 | needs:task:RC07 |
| RC11 | CEF spike — bounded adapter design + QWebEngine touchpoint audit + go/no-go | pending | 0 | needs:task:RC08 |
| RC12 | rustcore `reader` module — engine-agnostic article extraction (reader mode) | pending | 0 | needs:task:RC01 needs:tool:cargo |
| RC13 | rustcore `sessions` module — per-profile tab/window session store | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC14 | `torctl` client — Tor control protocol in Rust (NEWNYM, circuits, auth) | pending | 0 | needs:tool:cargo |
| RC15 | rustcore `cookies` module — per-profile cookie db for Servo/WPE (Chromium stays engine-owned) | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC16 | rustcore `icons` module — per-profile favicon/site-icon cache | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC17 | rustcore `autofill` module — address/contact profiles under secure custody | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC18 | rustcore `opensearch` module — engine XML parse/validate + registry CRUD | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC19 | rustcore `containers` module — container registry + storage namespace map | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC20 | rustcore `updates` module — update-check state + manifest/semver policy | pending | 0 | needs:task:RC07 needs:tool:cargo |
| RC21 | rustcore `notes` module — side-panel notes persistence | pending | 0 | needs:task:RC07 needs:tool:cargo |

## Task details

### RC01

Audit what `src/rustcore` already provides (credentials store, urlstrip —
see the RCORE01 row in the retired rust lane) and map the C++ service
surface the core must absorb:

- `src/profilesettings.{h,cpp}` — per-profile QSettings wrapper
- `src/profilestore.{h,cpp}` / `profileregistry` / `profileidentity` —
  persona registry, avatar, lock state
- `src/history/historymanager.*` — history store format and API
- `src/bookmarks/bookmarksmanager.*` + `xbel*` — bookmark tree + XBEL
- `src/sitedecisionstore.*` — per-site cookie/js/permission decisions
- `src/network/domainblocklist.*` + trancolist — trust/blocklist data
- `src/network/cookiejar/*` — cookie policy seams
- `src/rustcore/include/rustcore.h` + `rustcorebridge.*` — existing FFI
  conventions (RcStatus, thread-local error strings, RcBuffer marshaling,
  catch_unwind entry points, change-callback → queued Qt signal)

Deliverable: `.devin/RUSTCORE-ARCH.md` — target module layout, which data
moves vs stays per-engine, ownership rules (Rust owns the store, Qt owns
the view), one-format-per-store decision (reuse existing on-disk formats
wherever possible — user data must survive the cutover byte-for-byte),
the facade's API sketch, and the migration/parity-test strategy.

Hard constraints: profile data stays under the per-OS home-directory
roots BrowserPaths already defines; no Qt types cross the ABI; every
module independently usable so backends adopt piecemeal.

Verify: doc exists, lists every store above with a keep/move decision +
format choice; `cargo test` in src/rustcore still green (no code change
expected).


Outcome (2026-10-21): `.devin/RUSTCORE-ARCH.md` written. Key findings:
retired rust lane landed far more than assumed — bookmarks/history/
sitedecisions/policy/session/autofill/opensearch/blocklist/tranco/UA/
profilevault are already Rust + consumed; RC04/05/06 are facade+
verify work, not new modules. Real gaps: settings (RC02, JSON +
ini-import decision) and the profiles.json registry (RC03, keeps the
byte-compatible MPROF01 format, retires the ProfileRegistry/ProfileStore
duplication). Design call recorded: ac_* module handles bind profile at
open, retiring rustCoreEnsureDataDirFor global re-anchoring. Verified:
cargo test 270 passed / 0 failed (1 dead-code warning, pre-existing).

### RC02

`settings` module in src/rustcore: a typed per-profile key-value store
covering what ProfileSettings persists — get/set/remove/list-prefix,
types string/bool/int/double/bytes, per-profile scoping (root path in,
namespace = profile id), JSON-on-disk under the profile data dir with
atomic write (temp+rename+fsync) and schema-version field. Read-path
MUST understand the existing QSettings ini files (import-on-first-open,
then own format going forward) so upgrading users keep their settings.

C ABI: rc_settings_open(root, profile_id) → handle, get/set/remove/
list/close, RcStatus + thread-local errors like the existing entries.
RustCoreBridge: thin Qt wrapper mirroring the API.

Verify: `cargo test` green incl. ini-import fixture (write a real
ProfileSettings ini from the C++ side, open via FFI, values match);
default build (`qmake` without rust) still links.


Outcome (2026-10-21): `settings.rs` — typed Value set
(Str/Bool/Int/Dbl/Bytes/List; List is one wider than the brief —
QSettings round-trips QStringList and dropping it would strand real
keys) + `inifmt.rs` — a read-only QSettings IniFormat parser whose
grammar was verified byte-for-byte against Qt 6.12's writer (first-'/'
section split, %XX latin1 key escapes, '\' literal-slash, \xNNNN value
escapes, top-level comma lists, @ByteArray/@@/@Invalid() type escapes,
[General]/[%General%] root mapping). FFI: `rc_settings_open(root,
profile_id)` binds scope at open — <root>/profiles/<id>/settings.json
or <root>/settings.json (global) — plus typed get/set, contains,
remove, list-prefix, sync, import_ini, close; RcStatus + thread-local
errors + catch_unwind + "settings" change topic, all per the existing
conventions.  Writes persist atomically via store::atomic_write
(temp+fsync+rename).  Import: profiles/<id>/settings.ini merges
wholesale on first profile open (copy-not-move, existing keys win, ini
never written back); the global ini goes through rc_settings_import_ini
filtered by the MPROF04 isProfileKey table ported verbatim into
inifmt::is_profile_key.  Qt side: RustCoreSettings in rustcorebridge
maps the store's typed JSON <-> QVariant.  Verified: cargo test 282/282
(12 new incl. a real-QSettings fixture import + ffi_surface ABI test);
independent C probe on a freshly QSettings-written ini — 18/18 typed
reads match over the C ABI and the ini is byte-identical afterwards;
`qmake && make -j2` (rust on — rustcorebridge.cpp compiles, arktos
links libarktos_rustcore.a) and `qmake CONFIG+=no-rust && make -j2` in
a copied tree (qmake anchors OBJECTS_DIR to the source dir, so a
shadow build would collide) both complete 0-error.  Drive-by fix:
tst_cookiejar.cpp used ProfileSettings unconditionally but included
profilesettings.h only under ARKTOS_RUSTCORE — broke every no-rust
autotests build (pre-existing on the branch, present in main lane too).

### RC03

`profiles` module: persona registry — list/create/rename/delete profiles,
active-profile tracking, avatar file reference, per-profile data/cache/
settings roots computed identically to BrowserPaths for each OS
(~/.config/arktos/... on linux; read BrowserPaths for the canonical
layout and mirror it, do not invent a second one). Locked-profile
metadata (ProfileVault markers) is out of scope — vault stays C++.

C ABI same conventions as RC02; Qt wrapper in RustCoreBridge.

Verify: cargo test — registry round-trip, root paths equal
BrowserPaths::profileDataPath for a fixture profile on Linux; C++ probe
binary (or unit test via the bridge) confirms a profile created by Rust
is listed by ProfileStore and vice versa.

### RC04

`history` module: add-visit, remove, query (by url substring / date
range / limit-offset), title updates, expiry rules matching
HistoryManager's retention. Storage: first decide compat — read the
existing history file format HistoryManager writes; if it is a plain
documented format (sqlite/json/sql), reuse it; only introduce a new
format with a one-shot importer that never touches the source file
until the copy verifies.

C ABI + bridge same as before.

Verify: cargo test — round-trip + a fixture written by the C++
HistoryManager reads identically through FFI; no-rust build links.


Outcome (2026-10-21): audit confirmed RC01's call — the store was
already landed (rusqlite <profile>/history.db, WAL + busy_timeout,
legacy QDataStream file imported once by the existing HistoryParser,
per-profile rc_hist_open binding, icons + hosts rollup + suggest +
clear_since all wired into HistoryManager).  The two spec gaps were
closed rather than rebuilt:

- Query surface: `HistoryStore::query(spec_json)` + `rc_hist_query` —
  one JSON spec {"term","from_ms","until_ms","limit","offset"} in, a
  JSON array of {"url","title","ts"} rows out, same deduped
  newest-first order as the listing; term is a case-insensitive
  substring on url OR title (the history dialog's all-columns
  fixed-string proxy match); range bounds inclusive; offset+limit
  paginate after filtering; malformed specs error, never match-all.
  The dedup collapse was factored into `ranged_entries` so the
  listing and queries share it.
- Retention: `HistoryStore::expire_older_than(cutoff_ms)` +
  `rc_hist_expire_older_than(cutoff_ms, out_removed)` — bulk delete
  at-or-before the boundary, icons kept, hosts rollup reset for lazy
  rebuild, deleted count returned.  HistoryManager::checkForExpired
  now issues one ranged call instead of a per-entry rc_hist_remove:
  the cutoff reproduces the existing per-item check exactly
  (QDateTime(now.date - days, now.time) + 999ms, absorbing secsTo's
  truncation; DST-transition divergence bounded to an hour and noted
  in the comment).  Side benefit: duplicate raw rows of an expired
  visit no longer linger (per-entry remove deleted only the newest
  match) and the hosts table is wiped once, not per row.

Verified: cargo test 294/294 (+query_filters_ranges_and_paginates,
+expire_older_than_deletes_through_the_boundary, +ffi_surface legs
for both new symbols incl. NULL/malformed-spec and out_removed);
qmake && make -j2 (rust on) 0-error, arktos + all autotest binaries
link; tst_historymanager 49/49 offscreen --disable-gpu — incl.
daysToExpire (the bulk-expiry path), saveload round-trips,
legacyImport (a QDataStream file written by the C++ fixture path
reads identically through FFI); qmake CONFIG+=no-rust && make -j2
in a slim copied tree (fallback path, no ARKTOS_RUSTCORE) 0-error.

### RC05

`bookmarks` module: tree CRUD (add/remove/move/rename folder+leaf),
descriptions/keywords matching BookmarkNode fields, XBEL import AND
export byte-compatible with the existing xbel reader/writer (the file
is `bookmarks.xbel` per profile — keep the filename + format so
bookmarks survive).

C ABI + bridge same as before.

Verify: cargo test — XBEL round-trip: a fixture file produced by the
C++ writer parses, edits, re-exports, and the C++ reader accepts the
result with identical tree.


Outcome (2026-10-22): audit confirmed RC01's call — the module was
already landed end to end (handle-addressed tree, rc_bm_* CRUD +
detach/attach move + undo-aware destroy, per-profile
bookmarks.xbel load/save via BookmarksManager, BookmarkNode lazy
view, BKIMP01 url-scheme gate, "bookmarks" change topic).  The one
real spec violation found and fixed:

- XBEL export shape: write_item emitted a bookmark's description as
  a `desc=` ATTRIBUTE and never wrote folder descriptions at all,
  while XbelWriter's on-disk convention — the only form the C++
  XbelReader binds — is a `<desc>` child element on both folders and
  bookmarks (title, desc, then children).  Rust-written saves were
  silently losing folder descriptions outright and writing bookmark
  descriptions a no-rust build or foreign XBEL tool could not read
  back.  The writer now emits the element form in XbelWriter's exact
  positions; the reader still tolerates the interim attribute shape
  (element wins when both exist) so files written by the broken
  revision keep their data.  The tags= extension attribute stays —
  foreign readers ignore it by design.

Verify legs: new tst_xbel rustStoreInterchange does the spec's exact
round-trip — XbelWriter-produced fixture (folder+bookmark desc,
entity url, separator) -> rc_bm_load -> rc_bm_* edits (rename,
describe, detach+attach move, create) -> rc_bm_save -> C++
XbelReader re-reads a tree QCOMPARE-identical to the expected one,
and the emitted bytes carry <desc> elements, no desc= attribute.
Cargo 296/296 (+xbel_writer_emits_desc_elements_like_xbelwriter,
+xbel_reader_tolerates_legacy_desc_attributes); tst_xbel 16/16,
tst_bookmarknode 20/20, tst_bookmarksmodel 12/12 (incl.
rustPersistence + rustHtmlImportExport), tst_htmlbookmarks 6/6,
tst_addbookmarkdialog 6/6 — all offscreen --disable-gpu; qmake &&
make -j2 (rust on) 0-error, and a full qmake CONFIG+=no-rust &&
make -j2 in a copied tree links clean (stale-Makefile/Object carry
from the source tree excluded).

### RC06

`policy` module: the per-site decision data —
SiteDecisionStore (cookie/js/permission allow|deny|ask per origin+type),
domain allow/block lists feeding the shield/adblock presets, and the
Majestic+Tranco rank cache (read-path only if the list files already
exist; never re-download in Rust). Typed get/set/remove/list per
profile; persistence under the same profile data dir the C++ uses.

C ABI + bridge same as before.

Verify: cargo test — decision CRUD + a SiteDecisionStore-written
fixture reads identically via FFI; blocklist/rank lookups match the
C++ results for a fixed host set.


Outcome (2026-10-22): audit confirmed RC01's call — the whole policy
surface was already landed and consumed end to end; nothing needed
rebuilding, so the spec gaps closed were the two missing explicit
parity legs.

- Site decisions: `sitedecisions.rs` is the consolidated
  kind->key->value store (webperm/js/container/popup/http-allow/
  cookie/adblock/uaspoof/urlclean kinds), sitedecisions.json under
  the profile data dir, atomic write, corrupt-row tolerance,
  generation counter, "sitedecisions" change topic.  `rc_sitedec_*`
  (get/set/remove/clear/replace/list/snapshot/lookup/store_present/
  reload/reset) is the typed CRUD+list surface; SiteDecisionStore is
  the Qt façade whose profile-scoped overloads re-anchor the store
  at BrowserPaths::profileDataDir(<id>) so cookie/container kinds
  stay per-profile while persona-independent kinds pin the default
  store (MPROF02).  Every consumer keeps a one-shot legacy
  QSettings import (webpermissions/scriptcontrol/popupExceptions/
  containers/cookies exceptions/httpsOnlyExceptions/adblock
  whitelist) — user decisions survive the cutover.
- Domain allow/block: `blocklist.rs` = vendored seed u
  <datadir>/blocklist-domains.txt; the Qt DomainBlocklist fetches
  (consent-gated) and calls rc_blocklist_reload — Rust never
  downloads.  Consumed via NavigationPolicy::isDomainBlocked /
  shouldBlockDomain -> the WebPage navigation veto + interstitial.
- Rank/trust: `tranco.rs` merges the vendored top-100k seed with
  tranco-list.dat (zip) and majestic-million.csv, best-rank-wins,
  read-path only; `sitetrust.rs` folds blocklist + rank + lookalike
  + https + first-seen into the advisory verdict the site panel
  shows via rc_sitetrust_lookup_opts.  rc_tranco_file_entries is
  the updater's per-file validation probe.
- Request/cookie policy: `policy.rs` verdicts (rc_policy_evaluate /
  rc_policy_cookie_filter / rc_policy_call / load_snapshot /
  eval_generation) are consumed through navigationpolicy.cpp; the
  cookie jar ships its rule lists per call.

Added the spec's missing verify legs:

- tst_sitedecisions ffiWrittenDataParity — façade-written rows read
  identically through raw rc_sitedec_get/lookup/list/snapshot, a
  raw rc_sitedec_set surfaces through the façade, and the
  profile-scoped anchoring proves store isolation (raw calls read
  profiles/<id>/ after the scoped write; the default store is
  untouched).
- tst_domainblock ffiParity — for a fixed host set (listed, deep
  subdomain, listed-parent, subdomain-of-listed, parent-of-listed-
  subdomain, unlisted, substring lookalikes, empty)
  PrivacyRequestInterceptor::isDomainBlocked == rc_blocklist_check
  on every entry.
- tst_trancolist ffiParity — the rank embedded in the
  rc_sitetrust_lookup composite (what the panel displays) equals
  rc_tranco_rank for a fixed host set.

Verified: cargo test 296/296; qmake && make -j2 (rust on) 0-error;
tst_sitedecisions 9/9, tst_trancolist 9/9, tst_domainblock 14/14,
tst_cookiejar 51/51, tst_scriptcontrol 23/23, tst_popupblocker
10/10, tst_httponly 25/25, tst_containermanager 37/37, tst_sitepanel
26/26 — all offscreen --disable-gpu.  No-rust compile check: the
three touched test TUs compile with the rust defines stripped
(fresh moc — ffiWrittenDataParity is rust-only; the ffiParity slots
compile to empty bodies, matching existing test style).

### RC07

Stable facade: `src/rustcore/include/arktos_core.h` — the single entry
surface all engines program against:

- `ac_init(const AcConfig*) -> AcCore*`, `ac_shutdown(AcCore*)` —
  opaque core handle owning all module handles
- version query `ac_api_version()` + per-module capability bits
- unified error model: AcStatus + `ac_last_error()` thread-local
- module accessors: `ac_settings(core, profile_id)`, `ac_profiles(core)`,
  `ac_history(core, profile_id)`, `ac_bookmarks(core, profile_id)`,
  `ac_policy(core, profile_id)` — all returning the RC02–06 handles
- change-notification callback set once, dispatched per module+profile

Re-export the RC02–06 entry points behind it (thin forwarding is fine),
write `.devin/RUSTCORE-ABI.md` documenting the contract + stability
rules (append-only symbols, never repurpose an enum value), and grow
RustCoreBridge into the facade's Qt consumer (RustCoreBridge v2 —
keep the old rc_* surface compiling until RC08 migrates callers).

Verify: cargo test + a C probe binary compiled against the header
exercises every accessor; existing autotests still pass unmodified.


Verification evidence (2026-10-22):
- `cargo test --release`: 299/300 — the one failure,
  `profilevault::tests::key_material_zeroized_on_drop`, fails
  identically on the base commit (release-mode stack-slot probe;
  pre-existing, unrelated).
- `tools/acprobe.c` compiled with `cc -Iinclude` against
  `libarktos_rustcore.a` and ran green: every accessor minted,
  settings/profiles/history/bookmarks/policy ops exercised,
  (module,profile) callback stamped "work".
- `qmake && make -j2`: app + all autotest binaries, 0 warnings on
  touched TUs; `clearprivatedata.cpp` (a rustcorebridge.h consumer)
  syntax-checks under no-rust.
- Autotests offscreen: rustprofiles 6/6, sitedecisions 9/9,
  trancolist 9/9, domainblock 14/14, cookiejar 51/51, scriptcontrol
  23/23, popupblocker 10/10, httponly 25/25, containermanager 37,
  sitepanel 26/26, bookmarksmodel 12/12, bookmarknode 20/20,
  profilesettings 12/12, securestore 20/20. `xbel` (2) and
  `historymanager` (16) fail identically on the base commit —
  pre-existing, verified by stash+rebuild.
- ffi_surface gained 4 facade tests: init/accessor smoke,
  cross-profile isolation over the process-global stores,
  (module,profile) callback stamping incl. NULL for rc_* traffic,
  NULL-handle failure spellings.

Design notes:
- Handles re-assert their binding per op (rc_hist_open /
  Bookmarks::ensure_path / rc_set_data_dir no-ops on match), so
  facade callers never run the rustCoreEnsureDataDirFor dance.
- Bookmarks store gained loaded_path + epoch + dirty flag:
  re-anchoring flushes a displaced dirty tree to ITS file first
  (the in-store twin of saveIfNeccessary) when the directory still
  exists — profile edits can't cross or vanish on a switch.

### RC08

QtWebEngine adoption — flip the real services onto the facade behind
`CONFIG+=rustcore` (already exists; keep the no-rust build 100%
functional as today):

- ProfileSettings reads/writes → ac_settings (with the RC02 ini import)
- HistoryManager store ops → ac_history (keep the public API identical)
- BookmarksManager persistence → ac_bookmarks (keep API identical)
- SiteDecisionStore → ac_policy

Parity bar: the existing autotests MUST pass on the Rust path —
tst_history*, tst_bookmarks*, tst_settings*, tst_sitepanel,
tst_profile* — plus `make check` suite overall. Run the suite twice:
default flags and with rust disabled. A real-profile smoke: launch
offscreen, visit pages, confirm history/bookmark writes land in the
same files a C++ build would write (byte-compat where the format is
shared).

Any parity failure blocks done — this is the gate everything else
stands on.


DONE (2026-10-22): all facade-covered consumers now dispatch ac_*-first
with rc_* fallback when RustCoreBridge::serving() can't serve (no
QCoreApplication, non-GUI first touch, ac_init failure).

- rustcorebridge: exists()/serving()/rootDrifted() — serving() refuses
  to first-mint the QObject singleton off the app thread and refuses
  once BrowserPaths::dataRoot() moved under a pinned AcCore root
  (test-mode renames); RustCoreSettings/RustCoreProfiles get
  facade-first ctors + RustCoreSettings::clear().
- sitedecisionstore: per-op PolicyAccess RAII mints a profile-bound
  AcPolicy (empty id -> default, persona-independent kinds); legacy
  path keeps rustCoreEnsureDataDirFor anchoring.
- historymanager + historycompleter: AcHistory member/suggest handles;
  all hist ops dispatch ac_hist_* else rc_* with ensureRustStore().
- bookmarksmanager + bookmarknode: manager owns AcBookmarks; every
  node carries the store that minted its handle.  Two real fixes from
  parity runs: (a) ~BookmarksManager must clear the undo stack BEFORE
  ac_bm_close — commands' stray nodes destroyed subtrees through a
  freed handle (UAF caught by valgrind); (b) ac_bookmarks ensure_path
  skips reload on same-path rebind, so a re-entering manager inherits
  a tree emptied by the previous one's teardown — load() now forces
  ac_bm_reload when the file exists (rc_bm_load semantics).
- facade.rs profile_dir now create_dir_all's the profile dir at mint —
  BrowserPaths::profileDataDir mkpaths; without it first-touch writes
  under a fresh root failed ENOENT (sitedecisions set() FALSE).
- profilesettings: Rust JSON is canonical (profile + global scopes via
  cached shared handles — separate handles decode stale docs), ini
  mirrors stay byte-compatible; read order json->profile-ini->global
  ini preserves copy-not-move; arrays flatten to path/1/key.
- sitepanel/trancolist/domainblocklist: sitetrust lookup + tranco
  entries/reload + blocklist reload through AcPolicy when serving.

Intentionally still rc_* (outside this slice): navigationpolicy.cpp
policy-eval engine calls, browserapplication session encode/decode,
tabwidget rc_classify_input, and autotests that probe the legacy ABI
directly — the rc_* surface stays compiled by design until later rows.

Verified: rust build clean; CONFIG+=no-rust clean-tree build links
(shadow builds can't share OBJECTS_DIR — stale-object link trap).
19 parity binaries green offscreen: profilesettings 12, rustprofiles 6,
historymanager 49, sitedecisions 9, xbel 16, bookmarknode 20,
bookmarksmodel 12, omnibox 87, trancolist 9, domainblock 14,
sitepanel 26, historyfiltermodel 37, historyui 8, htmlbookmarks 6,
navigationpolicy 16, cookiejar 51, cookiemodel 11, networkcookiejar
33, httponly 25. cargo test 299/300 (1 pre-existing profilevault
release-mode zeroize-probe failure, untouched file).

### RC09

Servo + WebKit backend adoption: ServoBackend and WebkitBackend resolve
the same AcCore for profile data (history writes from servo tabs land
in the same store as chromium tabs; settings/policies apply
uniformly). Where a backend lacks the seam (e.g. cookie injection on
servo), record the capability gap in EngineInterface::capabilities()
honestly instead of faking parity.

Verify: build with the servo artifact present — `--servo-smoke` legs
pass and a servo visit appears in the shared history; webkit backend
compiles the facade path (WPE env per .devin/webkit-env.sh if present,
otherwise document the build-only verification).

Implementation (2026-10-22):
- Engine::Profile::profileId() (engineinterface.{h,cpp}): persona
  resolution shared by all backends — "arktosProfileId" property tag
  wins (the only channel OTR/container profiles can carry), then the
  storageName parse ("arktos"->default, "arktos-<id>"-><id>,
  "arktos-container-*" never parses), ambient fallback for foreign
  names (Servo's shared context).
- EngineTab::wirePage: records loadFinished/title/icon into
  HistoryManager::instanceFor(profile->profileId()) — exact mirror
  of WebPage's MIG06 wiring incl. OTR gate and
  arktos-cert-error/-http-warning/-site-block + PdfSupport::isManagedPath
  exclusions. Facade-backed: lands in profiles/<id>/history.db via
  ac_history when rustcore is linked, C++ store under no-rust.
- ServoBackend::createProfile: QHash<QString,QPointer<ServoProfile>>
  — one shared profile per persona storageName (libservo has no
  profile objects; named profiles carry persona identity only, never
  fake engine-side isolation). Empty name keeps legacy "servo"
  context -> ambient persona.
- TabWidget::addEngineTab: passes ProfileOptions.storageName =
  "arktos"/"arktos-<id>" per window persona; installs
  AdBlockManager::instanceFor(personaId) on servo/webkit profiles
  (was ambient instance()) and DownloadManager::instanceFor(personaId)
  when capabilities().downloads (was ambient).
- main.cpp: new servo-smoke "history-recorded" leg asserts the
  committed servo probe URL is in HistoryManager::instance() (the
  smoke store's default-persona history.db); servo-smoke dispatch
  switched return->std::quick_exit — A/B-verified pre-existing
  static-teardown SIGSEGV on this lane (base build w/o this diff
  also rc=139 after PASS verdict; likely rust-core facade + linked
  WPE teardown ordering past shutdownEngine()). Verdict printed
  before exit either way; exit code now reflects it (rc=0).
Verification:
- Build: qmake servo=1 webkit=1 + WPE env, make -j2 clean;
  webkitbackend.o compiles the facade path against WPE headers.
- --servo-smoke (ARKTOS_SERVO_EMBED_LIB=main-lane release artifact,
  offscreen, isolated ARKTOS_SMOKE_STORE): every leg PASS incl.
  history-recorded, gate-*/multi-view-*/bidi-*, verdict PASS, rc=0,
  no crash.
- --engine-list: Chromium all-true; WebKit 2.54.1 cookieFilter/
  devtools/extensions=false (honest); servo listed w/ artifact.
- No-rust: all 5 touched TUs compile 0 warnings under
  CONFIG+=no-rust servo=1 (shadow build).

### RC10

Packaging audit for the new crates: confirm every package leg links and
ships the rustcore static/cdylib the facade needs — deb (both ubuntu
tags), rpm, flatpak manifest, macOS dmg (macdeployqt doesn't deploy
rust libs — check the staging block), Windows NSIS payload list.
Update BuildProcess/release-notes.md wording if service set changed.

Verify: CI packages run green on this branch (push + watch the run);
each artifact's file list contains the rust lib where the platform
stages one.


Audit + outcome (2026-10-22):
- rustcore is staticlib+cdylib but qmake links ONLY the staticlib into
  the binary (rustcore.pri LIBS += libarktos_rustcore.a); the cdylib is
  for out-of-tree consumers, never dlopened — no package leg ships a
  rust .so for the facade; the binary itself is the carrier. Same for
  rustdl and adblock/rust.
- Per-leg audit: deb (both ubuntu tags) relies on .qmake.conf
  auto-CONFIG when cargo resolves (GH ubuntu images carry rustup) —
  pkg-deb.sh cmd_build also force-passes CONFIG+= per existing .a;
  rpm's cmd_build pre-builds missing .a archives via cargo --locked;
  flatpak passes CONFIG+=rustdl+rustcore explicitly under the
  rust-stable SDK ext with flatpak/cargo-sources.json vendored deps —
  regenerated and confirmed IN SYNC with all three Cargo.locks
  (371 crates); dmg/exe legs get cargo via the servo-embed build step
  (hard-gated), then .qmake.conf auto-CONFIG; macdeployqt/windeployqt
  deploy no rust libs because none exist as files — the one dlopen'd
  rust artifact is libservo_embed, already hand-staged in every leg.
- Gap closed — silent no-rust fallback: every leg passed ldd + launch
  without proving the facade linked. New asserts: src/Makefile names
  libarktos_rustcore only while CONFIG+=rustcore is on (a deliberate
  no-rust re-qmake drops it, so no-rust packages still verify), and
  "ac_init" bytes only enter the binary via rustcore objects ->
  byte-grep the staged binary in pkg-deb verify, pkg-rpm (spec+stage
  AND extracted-rpm paths), flatpak build-command + pkg-flatpak
  in-sandbox verify, macOS dmg step. Windows PE has no symtab ->
  findstr on "panic inside rustcore" (catch_unwind rodata string only
  linked crate objects provide) + Test-Path arktos_rustcore.lib.
  servo_embed.* staged-file asserts: conditional when-built on
  deb/rpm, unconditional on dmg/exe (CI servo step hard-gates it).
- Fixed two pre-existing pkg-deb verify breakages on this host: a bare
  `grep "depends on" dpkg.out | sed` diagnostics line killed the
  script under set -e BEFORE the --force-depends retry whenever the
  failure text differed, and dpkg's ldconfig/start-stop-daemon lookup
  needs the sbin dirs a non-root user's PATH lacks.
- packages.yml push trigger now covers rust-core — the only mechanism
  for a green run on this branch (no gh CLI for workflow_dispatch) and
  the right permanent gate for the integration lane (per-ref
  concurrency, lane bookkeeping already paths-ignored).
- Wording: stale "libservo_embed does not exist yet (ENG03)" claims
  corrected in pkg-deb/pkg-rpm/arktos.spec/manifest; debian/control +
  spec descriptions updated; release-notes "Rust core" bullet now
  names the facade service set + flatpak added to Packages; README
  servo-shipped/verify/crate-count lines corrected.
- Honest gap recorded in the manifest: flatpak does not build/ship
  servo-embed (its clang/mozjs sysdeps + own vendored crate set are
  beyond the three locks cargo-sources.json covers); ServoBackend
  reports the engine unavailable at runtime.
- Local caveat: this worktree's binary was built webkit=1, so rpm
  spec+stage verify's Requires-coverage leg drifts on libWPEWebKit/
  libsoup/libGLESv2 — a local env artifact; CI builds webengine-only.
  assert_rust_payload's predicates verified directly on the staged
  tree.

### RC11

CEF spike — analysis only, NO port: write `.devin/CEF-SPIKE.md`
covering (a) what a CEF backend would consume from arktos_core.h
(prove the facade is engine-complete: navigation, downloads, cookies,
extensions-adjacent surfaces), (b) the ~1423 QWebEngine touchpoints
inventoried into must-port / facade-absorbed / droppable buckets with
file counts, (c) the CEF-specific gaps the facade does NOT cover
(content scripts, extension JS bridges, compositor/input), (d) a
bounded adapter sketch: new src/engine/cefbackend implementing
EngineInterface against the facade, (e) effort estimate + go/no-go
recommendation. Explicitly note: CEF replaces the Chromium embed, it
does not serve Servo/WebKit.

Verify: doc exists with the numbered inventory; no code changes beyond
the doc (any exploratory branch work stays out of rust-core).

### RC12

`reader` module: Readability-style article extraction — HTML+base-url in,
sanitized article fragment + metadata (title, byline, lang, excerpt,
length) out. Powers readersanitizer/readermode for ALL engines: the C++
side keeps only the trigger+overlay, the transform lives in Rust.

Stateless (no profile data), so it only needs RC01's conventions — not
the facade. Rust has mature crates for this (e.g. a readability port);
vendoring policy per existing cargo-deny rules, no telemetry deps.

Verify: cargo test on a fixture corpus (save real article pages of
different shapes); extraction parity-or-better vs the current C++
readersanitizer on the same fixtures; `rc_reader_extract` FFI returns
identical output on repeat calls.

### RC13

`sessions` module: per-profile session store — open windows, tab list
(url, title, engine id, scroll position, container id), clean-shutdown
flag + crash-dirty marker so "restore previous session?" works the
same across engines. Private-tab data must never persist — same rule
the C++ session writer follows (audit how private/otr tabs are
excluded today and mirror it).

Atomic writes, versioned schema. If today's restore data lives in
QSettings/session files, the RC02 ini-import rule applies.

Verify: cargo test — write/read round-trip incl. private-exclusion
fixture; a session written by the C++ path restores identically via FFI.

### RC14

`torctl` client: standalone Tor control-protocol port of torcontrol.cpp —
connect, authenticate (COOKIE + HASHEDPASSWORD), NEWNYM, GETINFO for
circuit/stream status, async event subscription (CIRC/STREAM). Pure
socket protocol, zero profile data — independent of the facade, the
only RC task that does not wait on RC07.

Qt side keeps the UI (circuit button/panel); the crate exposes the
protocol surface via FFI and a change-callback like the existing
rustcore entries. Must honor the no-leak rule: control connection is
localhost-only, auth secrets zeroed on drop.

Verify: cargo test against a mock control-port fixture (scripted
protocol transcript); integration leg skipped gracefully when no tor
expert bundle is present.

### RC15

`cookies` module: per-profile cookie database — name/value/domain/path/
expires/secure/httponly/samesite plus container-id partitioning. The
store is engine-owned reality check: QWebEngineProfile owns Chromium's
cookie jar and CANNOT be displaced — for chromium the module is an
export/sync surface at most; for Servo/WPE it is the authoritative
store the backend applies. Design the API so both modes are honest
(capability flag: authoritative vs mirror).

First audit cookiejar/* + BrowserProfile for the authoritative list of
fields and the container partitioning model — the schema must carry
what SiteDecisionStore policy needs (RC06 interplay).

Verify: cargo test — CRUD + expiry/samesite semantics; import fixture
from a C++ cookie export; chromium-mode mirror semantics documented.

### RC16

`icons` module: per-profile site-icon/favicon cache — page-url/origin
keyed records pointing at icon files under the profile CACHE dir (not
the data dir — it is disposable). Fetch is Qt/engine-side (the page
already has the bytes); Rust owns index, dedup-by-host, expiry, and
file lifecycle. Match the existing favicon behavior LocationBarSiteIcon
relies on — audit it first.

Verify: cargo test — index round-trip, dedup, expiry sweep; cache
files land under the BrowserPaths cache root, never the data root.

### RC17

`autofill` module: address/contact profiles (name, org, address lines,
phone, email, custom fields) — personal data, so storage must meet the
SecureStore custody bar: prefer reusing the existing rc_cred_*
encrypted map (audit whether it fits structured records; a dedicated
0600 JSON store keyed per profile is the fallback, decided in the
implementation note). AutofillManager's field semantics are the API
contract — audit it first.

Verify: cargo test — CRUD round-trip; file permissions 0600/0700;
a record written via FFI reads back in AutofillDialog.

### RC18

`opensearch` module: OpenSearch description-document parse+validate
(search URL templates, suggestion endpoints, icons, mime types) and
the engine registry CRUD (add/remove/reorder/set-default, per profile).
Qt keeps the network fetch and the dropdown UI; Rust owns the parsed
model + registry persistence. Must read the existing searchengines.xml
storage the C++ writer produces — audit opensearchreader/writer.

Verify: cargo test — parse the bundled engine XMLs in
data/searchengines, round-trip the registry file the C++ side reads.

### RC19

`containers` module: container registry (id/name/color/icon) + the
storage-namespace mapping (which container partitions cookies/cache/
site data). FIRST audit containermanager.* — container metadata may
already persist inside profile settings; if so this module is a typed
specialization over RC02's store, not a new file format. The mapping
rules feed RC15's cookie partitioning.

Verify: cargo test — registry CRUD + namespace derivation matches
ContainerManager's paths; a registry written via FFI is honored by the
C++ side on next launch.

### RC20

`updates` module: update-check policy + parse half — last-check
timestamp/snooze state (per profile or global — decide per the
existing updatechecker keys), manifest parsing, semver comparison,
channel rules. Network fetch stays Qt/rustdl-side. Thin module — if
the state is just a few keys it may live as typed helpers over
RC02 settings; decide in the implementation note and record why.

Verify: cargo test — semver compare matrix + manifest parse fixtures;
no regression in updatechecker behavior (its existing tests pass).

### RC21

`notes` module: side-panel notes persistence — per-profile notes CRUD
with the ordering/format notesmodel uses today (audit first; if it is
already a plain file under the profile dir this is a read-compat
module, if it lives in settings it folds into RC02's typed API).
Smallest module in the queue — good first adoption-proof task after
RC07 lands.

Verify: cargo test — CRUD round-trip; notes written by C++ load via
FFI and vice versa.
