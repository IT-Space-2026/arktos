# Devin autonomous task list — Arktos MW-BRIDGE LANE (branch mw-bridge, worktree ../arora-mwbridge)

Middleware queue driven by `.devin/taskloop-bridge.sh`. Goal: formalize the
Rust middleware layer — the typed command/query/event surface between Qt UI,
Rust logic (`src/rustcore`), and engine adapters. Everything lands on top of
the RC01-RC21 stores and the `arktos_core.h` facade already merged into
`rust-core`; this lane turns the ad-hoc FFI surface into one versioned,
self-describing bridge.

Status values: `pending` | `in_progress` | `done` | `blocked`

Rules for the agent working this file:
- Mark the row `in_progress` before starting, `done` only after verifying.
- `blocked:<reason>` in Notes when genuinely stuck — never fake done.
- One task per run: after committing, STOP.
- NEVER edit `.devin/Arora-Task.md` or other lanes' tables.
- Cross-lane deps: `needs:task:X` may reference another lane — check that
  lane's table (`../arora-mwdomains/.devin/Arora-Task-Domains.md`,
  `../arora-mwshell/.devin/Arora-Task-Shell.md`) before starting. If the
  dependency is still pending/in_progress, leave YOUR row `pending` and pick
  the next runnable row — never burn an attempt on a blocked dep.
- Append a one-line summary to `.devin/WORKLOG.md` when a task completes.
- Table rows are one-line summaries; the FULL spec lives in `### <id>`
  under `## Task details`. Keep cells short.
- ALL writes to this file MUST go through `.devin/taskfile.sh`
  (flock-serialized): `set <ID> field=…`, `append-detail <ID>` (stdin),
  `with-lock -- <cmd>`.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| MB01 | Middleware architecture doc — typed cmd/query/event surface over arktos_core.h | done | 0 | MW-ARCH.md written: envelope dispatch chosen (16B header + JSON payload, corr-id echo); all 468 ac_*/rc_*/rv_* exports classified Q/C/E/L; event lane + topics inventoried; keep-out list; verify: every export present (script-checked) |query|event classes; define ABI versioning + envelope wire format; keep-out list (JS exec, WebChannel, render stay Qt-side) |
| MB02 | rustcore `mw` envelope — versioned request/response framing over the C ABI | done | 0 | mw envelope already landed via MB04 commit (in-flight lane); verified in-tree: v1 frame + bad_version/malformed/unknown_op structured error envelopes, corr echo, ac_buffer_free ownership, 18-domain registry — cargo mw 36/36, full suite 417/417 |
| MB03 | Event bus — Rust->Qt notification channel + AcEventNotifier QObject bridge | pending | 0 | retry: prior 5 runs killed by 2GB cap — keep builds small: make -j1, CARGO_BUILD_JOBS=1 or detached cargo, avoid concurrent autotest links |
| MB04 | Engine capability table in rustcore + ac_engine_caps() FFI | done | 4 | caps.rs static per-engine matrix (24 features, honest partial+note grades filled from Engine::Capabilities + plumbing code) + ac_engine_caps FFI at ABI v9 — cargo 394/394, acprobe prints matrix rc=0, make -j2 clean, tst_rustevents 6/6 |servo|webkit; consumed later by MS06 |
| MB05 | Bridge harness — extend acprobe to exercise envelope+events+caps end-to-end | done | 0 | acprobe --bridge leg dispatches Q+C per domain via the envelope, exercises the namespaced event round-trip and caps matrix; wired as autotests/acprobe (no-rust stub) — cargo 410/410, probe rc=0, make -j2 clean, tst_acprobe green |
| MB06 | Cutover slice 1 — settings/profiles/history/bookmarks callers through envelope | done | 0 | envelope for settings/profiles/history callers + new mw ops; bookmarks flat (opaque handles); cargo mw 31/31, make clean, affected autotests all pass offscreen |
| MB07 | Cutover slice 2 — policy/sessions/cookies/icons/opensearch through envelope | done | 0 | envelope for policy/session/icons/opensearch callers + extended mw ops; cookies registered (zero Qt callers), navigationpolicy+probe legs flat; cargo mw 36/36, qmake+make clean, affected autotests green offscreen, tst_* 102 unchanged (trancolist fixture-fetch + memcycle teardown SIGSEGV reproduce at HEAD) |
| MB08 | Docs — RUSTCORE-ABI.md v2 + MW-ARCH.md finalize + honest boundary doc | done | 0 | RUSTCORE-ABI.md v2: all 188 ac_* exports incl mw_*/ac_events_*/ac_engine_caps with verbatim signatures + thread + ownership rules, ABI v9; MW-ARCH finalized: §7 landed registry (17/18 reserved-empty), §8 routes = landed state, §9 landed cutover, honest boundary list kept — verify: script-checked every header export appears in the ABI table, §8 dropped 0 symbols, tables well-formed |

## Task details

### MB01
Read `src/rustcore/` and `arktos_core.h` + `.devin/RUSTCORE-ABI.md` +
`.devin/RUSTCORE-ARCH.md` (already in-tree). Produce
`.devin/MW-ARCH.md` answering:
- Which existing exports classify as **query** (sync decision/read),
  **command** (async state change), **event** (Rust->UI notification)?
- Wire format: single `ac_mw_dispatch()` byte-buffer entry point vs
  per-domain typed calls — pick one, justify.
- Versioning: how a Qt-side caller discovers ABI version + schema.
- Keep-out list: JS execution, WebChannel objects, QWebEngineProfile,
  request interception *execution* (decision may be Rust, execution
  stays Qt), rendering, OS file dialogs.
Verify: doc exists, every current export appears in the classification
table; no code changes required.

### MB02
Implement the envelope from MB01's decision in `src/rustcore/src/mw/`
(or wherever MB01 says). Requirements: version byte checked; malformed
input returns structured error, never panics; correlation id echoes;
response buffers caller-freed via existing rc_free_* pattern. Rust
`#[test]` coverage: round-trip each domain op class, truncated buffer,
bad version, unknown op. Verify: `cargo test` count grows, all green.

### MB03
Facade already has a `(module,profile)` callback lane (see RC07 work).
Formalize: topic registry (string topics, JSON payloads), subscriber
registration FFI, Qt-side `AcEventNotifier` QObject that converts the
callback into QSignals (`storeChanged(QString topic, QByteArray
payload)`). Add one real producer (e.g., history visit write emits
`history/changed`). Offscreen autotest: write via FFI, signal observed
in Qt test. Verify: new autotest green, cargo tests green.

### MB04
`src/rustcore/src/caps.rs`: static per-engine feature matrix +
`ac_engine_caps(engine_id, out_json)` FFI. Engines: qtwebengine, servo,
webkit. Features at minimum: devtools, pdf_viewer, extensions_mv2,
extensions_mv3, webchannel, otr_profile, tor, containers, reader_mode,
downloads, sessions, favicons, opensearch, site_permissions,
autofill. Values: supported|partial|unsupported + optional note.
HONEST: consult existing capability reporting code before filling;
Bitwarden-class extension support is `partial` at best on every engine.
Verify: probe prints the matrix; unit test asserts schema.

### MB05
Extend `src/rustcore/tools/acprobe` (or wherever acprobe lives) with a
`--bridge` leg: dispatch at least one query+command per domain through
the MB02 envelope, subscribe to MB03 topics and provoke one event,
dump MB04 caps. Wire as an autotest target. Verify: probe exits 0 in
build tree, make target green.


AMENDMENT (slicing): if envelope coverage per domain exceeds one run's
budget, land the harness + a core-domain subset and leave in_progress —
the loop resumes; never mark done with untested domains.

### MB06
Move settings/profiles/history/bookmarks Qt callers onto the envelope
WHERE it shrinks code (batch reads, tree queries). Where a flat ac_*
call is simpler, keep it and record the decision in MW-ARCH.md's
cutover table. Hard requirement: no behavior change — existing
autotests must pass unmodified count-wise.

### MB07
Same cutover for policy/sessions/cookies/icons/opensearch.

### MB08
Update `.devin/RUSTCORE-ABI.md` to v2: every export incl mw_* with
signature, thread rules, ownership rules. Finalize MW-ARCH.md with the
landed cutover table + honest boundary list.
