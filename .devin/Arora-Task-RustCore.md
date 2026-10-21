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
| RC01 | Rust-core architecture audit + target design doc | pending | 0 | needs:tool:cargo |
| RC02 | rustcore `settings` module — typed per-profile key-value store + FFI | pending | 0 | needs:task:RC01 needs:tool:cargo |
| RC03 | rustcore `profiles` module — persona registry + roots under home dir + FFI | pending | 0 | needs:task:RC01 needs:tool:cargo |
| RC04 | rustcore `history` module — visit/query/clear, compat with existing store | pending | 0 | needs:task:RC02 needs:tool:cargo |
| RC05 | rustcore `bookmarks` module — tree ops + XBEL import/export compat | pending | 0 | needs:task:RC02 needs:tool:cargo |
| RC06 | rustcore `policy` module — cookie/storage/site-decision/trust data + FFI | pending | 0 | needs:task:RC01 needs:tool:cargo |
| RC07 | Stable `arktos_core.h` C ABI facade v1 + RustCoreBridge v2 | pending | 0 | needs:task:RC02 needs:task:RC03 needs:task:RC04 needs:task:RC05 needs:task:RC06 needs:tool:cargo |
| RC08 | QtWebEngine adoption — services read/write through facade, parity proof | pending | 0 | needs:task:RC07 |
| RC09 | Servo/WebKit backends consume the same facade + capability reporting | pending | 0 | needs:task:RC08 |
| RC10 | Packaging: new crates linked + staged in deb/rpm/flatpak/dmg/exe legs | pending | 0 | needs:task:RC07 |
| RC11 | CEF spike — bounded adapter design + QWebEngine touchpoint audit + go/no-go | pending | 0 | needs:task:RC08 |

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

### RC10

Packaging audit for the new crates: confirm every package leg links and
ships the rustcore static/cdylib the facade needs — deb (both ubuntu
tags), rpm, flatpak manifest, macOS dmg (macdeployqt doesn't deploy
rust libs — check the staging block), Windows NSIS payload list.
Update BuildProcess/release-notes.md wording if service set changed.

Verify: CI packages run green on this branch (push + watch the run);
each artifact's file list contains the rust lib where the platform
stages one.

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
