# MEM10 — leak re-audit B: lifecycle churn legs + fix pass

Generated: 2026-10-18 on the MEM10 working tree (branch rust-work).
Toolchain: Qt 6.12.0 (~/Qt/6.12.0/gcc_64), clang 18 (linux-clang
sanitize spec), valgrind 3.26.0 (/snap/bin/valgrind).

MEM09's handoff: one Arktos-code leak found, sited in test code
(`autotests/browsermainwindow/tst_browsermainwindow.cpp` —
`stopReloadState()` allocated `new QByteArray` per TCP connection and
never freed it for the `/held` and `/abort` legs).  This pass adds the
lifecycle-churn coverage MEM09 lacked — persona create → window →
close → delete, N-persona `instanceFor` churn, vault
seal/unseal/lock/dirty-reseal cycles and OTR teardown — fixes the
fixture leak, and fixes the product-side lifecycle gap the churn legs
exist to catch.

## Method

* Phase 1 — LSan over MEM09's kept instrumented tree
  (`/tmp/arktos-leaks.mA8jKi`, `qmake -spec linux-clang
  CONFIG+=sanitize`, plus its throwaway `qtest_arktos.h`
  `__lsan_do_leak_check()` seam — never committed).  The MEM10 diff was
  synced into that tree and rebuilt; the lifecycle-focused binaries ran
  offscreen under isolated HOMEs with
  `ASAN_OPTIONS=detect_leaks=1`, `LSAN_OPTIONS=exitcode=0`.  Every leak
  block is classified by allocation site exactly as check-leaks.sh
  does: a top-4 frame under `src/` or `autotests/` is "ours".
* Phase 2 — bounded valgrind memcheck on the plain in-tree gcc build,
  MEM02 convention: the two smallest binaries carrying the new churn
  legs (`tst_profilemanagers`, `tst_vaultengine`), sequential,
  `QT_ENABLE_REGEXP_JIT=0`, `--trace-children=no`, leak kinds
  definite+indirect only.  `.devin/valgrind.supp` unchanged.

## New coverage (the churn legs)

* `tst_profilemanagers` (new legs): `releasePersonaDestroysManagers`
  materializes a persona's whole world — the named
  `QWebEngineProfile`, History/Bookmarks/AutoFill/AdBlock/Container/
  Extension/Download managers, NotesModel, ProfileIdentity, CookieJar —
  releases it, and asserts every QPointer dies; `releasePersonaChurn`
  runs 8 personas × 2 rounds; `releasePersonaRefusesUnsafe` pins
  "default"/non-slug refusal; `deleteProfileReleasesPersona` drives
  `ProfileStore::deleteProfile` end-to-end; `offTheRecordRelease` kills
  a stack-owned OTR profile and asserts its per-object notes model and
  jar die with it.
* `tst_profilewindow` (new legs): `releaseRefusesOpenWindowThenSucceeds`
  — create → window → release refused while the window lives → close →
  release succeeds; `privateWindowTeardownKeepsSharedOtr` — closing a
  private window leaves the shared OTR overlay and its stores alive.
* `tst_vaultengine` (new leg): `churnCycles` — 3×
  seal → unseal → lockProfile (dirty re-seal via cached custody) →
  unseal; asserts custody drops on lock (`sessionPassword` empty) and
  lands on unseal, plaintext round-trips byte-for-byte, and no
  `pre-unlock`/`cache-hold`/`profiles-hold` residue survives a cycle.

## Findings and fixes

### Ours — fixed

1. **Persona singletons never released** (the lifecycle-churn gap the
   audit exists to find; latent since MPROF02).  Every per-persona
   `instanceFor()` used a process-lifetime
   `static QHash<QString, QPointer<T>>` of qApp-owned objects and
   `BrowserProfile::s_personaProfiles` never released a named
   `QWebEngineProfile` — deleting a profile freed the disk tree but
   left its live world allocated until process exit.
   Fix: a `releaseFor(id)` on every persona-keyed manager, a
   `ContainerManager::releaseFor` that also destroys the container
   profiles it materialized, a `CookieJar::releaseFor` covering both
   the named profile's jar and every container jar, and a central
   `BrowserProfile::releasePersona(id)` that orders the teardown —
   cookie jars before their profiles, managers before the engine
   profile last — refuses "default", non-slug ids and personas with a
   live bound window, and is invoked from `ProfileStore::deleteProfile`
   before the directory removal.  Companion fixes for the same
   dead-key hazard: `prepareProfile()`'s `s_preparedProfiles` set now
   self-cleans on profile destruction, `SchemeAccessHandler`s are
   parented to their profile instead of qApp, the OTR `NotesModel` map
   drops its row when the OTR profile dies, and a jar keyed on a
   destroyed profile is released (the `~CookieJar` path only touches
   the QPointer-guarded store, so it is teardown-safe).  `SecureStore::
   lock(id)` drops the deleted persona's unlocked key custody.
   `deleteProfile` also reloads the MPROF01 `ProfileRegistry` so the
   deleted persona stops resolving through the shared
   `profiles.json` row.

2. **stopReloadState fixture leak** (MEM09's finding, 336 B / 14
   objects).  The per-connection `new QByteArray` is now freed on
   socket `destroyed` — "request complete" is not a reliable free
   point for the held/aborted legs.

### Ours — remaining

**None.** After the fix pass, zero LSan blocks and zero memcheck
records site in `src/` or `autotests/`.

## Phase 1 result — LSan (focused lifecycle corpus)

12 binaries, all tests pass:

| log | ours | upstream |
|-----|-----:|---------:|
| tst_profilemanagers | 0 | 2 |
| tst_profilewindow | 0 | 3 |
| tst_profilecontext | 0 | 2 |
| tst_profilemanager | 0 | 2 |
| tst_profilevault | 0 | 2 |
| tst_vaultengine | 0 | 2 |
| tst_vaulte2e | 0 | 2 |
| tst_profilee2e | 0 | 3 |
| tst_containermanager | 0 | 0 |
| tst_notesmodel | 0 | 0 |
| tst_cookiejar | 0 | 94 |
| tst_browsermainwindow | 0 | 33 |

Total: **0 ours, 145 upstream** — the same fontconfig/expat/udev/
WebEngine-teardown families MEM02/MEM09 classified (cookiejar's 94 are
the fontconfig corpus pulled in through its settings paths;
browsermainwindow's 33 are WebEngine teardown pools — the MEM09 fixture
block at tst_browsermainwindow.cpp:7595 is gone).

One flake worth noting honestly: `tst_browsermainwindow::torStartPage`
failed once in the full-suite run (compared the URL before the page
title landed) and passes standalone under the same binary — a
title-timing flake, not a leak or a regression of this change.  The
`qpointer.h:76` UBSan note appears identically in MEM09's corpus.

## Phase 2 result — valgrind (bounded, MEM02 convention)

| binary | qtest | our records | upstream records |
|--------|-------|------------:|-----------------:|
| tst_profilemanagers | 20/20 pass | 0 | 45 |
| tst_vaultengine | 17/17 pass | 0 | 45 |

Each log's single definite+indirect record is the same 320 B
fontconfig↔expat config-parse allocation (malloc inside
`XML_ParseBuffer`) — upstream, unsuppressed by design.  The ~18,700
"ERROR SUMMARY" contexts dedupe to the upstream Qt-init/DBus families
(`qt6-init-connect-addr16` suppressed: 2–3 hits) — none site in the
test binary's own frames.

## Verdict

LSan lifecycle corpus: **0 leak blocks allocated in Arktos code**.
Bounded memcheck: **0 definite/indirect records sited in Arktos code**.
Both MEM10 fix classes (the fixture `QByteArray` and the persona-world
release path) are covered by committed regression legs that fail if the
maps ever retain a released persona again.

`runTests.sh` suite: rc=1 with 4 failures, all outside the MEM10
surface and reproducible standalone — `tst_tlsverify` initTestCase
(`rc_tls_add_root` rejects the testcerts fixture CA) and
`tst_sitepanel::padlockStateMatrix` (same TlsFixture root),
`tst_dialogs::aboutDialog` (author string), `tst_qrcode::
encodeMatchesReferenceMatrix`.  None touch profile lifecycle; the
worklog records the recurring env-flake class these fall in.
