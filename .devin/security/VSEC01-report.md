# VSEC01 — Post-landing security audit: multi-profile + profile-vault stack

Method: source-level adversarial review of the merged MPROF01-03/05/06 +
PVAULT01/02/04 tree — every claim below was verified against the code,
not the task notes. Findings were fixed in place (marked **FIXED**) or
filed as follow-up rows.

## Threat model

The vault and profile machinery defend against a *local* adversary:
another process running as the same user (or an operator who walks away
from an unlocked session) that can create files, plant symlinks, and
edit anything under the data root, but cannot read process memory or
defeat file permissions. Renderer compromise and network adversaries
are covered by the SEC*/BADSSL* audits, not here.

## Findings

### F1 — Layout migration moved planted symlinks into the profile tree — FIXED

`BrowserPaths::ensureDataLayout()` enumerated pre-MPROF payload names at
the data root and `QFile::rename()`'d each into `profiles/default/`.
`QFile::exists()` follows links and `rename()` carries the link itself,
so a symlink planted at e.g. `<data>/history -> ~/.ssh/id_rsa` would
land inside the profile dir intact — every later `open()` of
`profiles/default/history` resolving to the attacker target. A link at
`profiles/` itself had the same escape via `mkpath` + rename.

Fix (`src/browserpaths.cpp`): `QFileInfo::isSymLink()` (lstat-based —
catches dangling links) is checked on the `profiles/` container, the
`profiles/default` destination, and every payload name before the move;
a hit fails the whole pass loudly with the path in the report. Nothing
moves, nothing follows. `profileDataDir()` additionally warns once per
path if an already-migrated `profiles/<id>` is a symlink — the path
contract can't fail there without breaking every caller, so the
treatment is a loud log rather than a silent follow.

Regression: `tst_profilecontext::migrationRefusesSymlinks` (both legs —
payload-named link and link-as-container).

### F2 — The engine's own storage is outside the sealed set — follow-up VSEC02

`BrowserProfile::profileFor(id)` creates a named
`QWebEngineProfile("arora-<id>")` and never calls
`setPersistentStoragePath()`/`setCachePath()`, so cookies, LocalStorage,
IndexedDB, service workers, the HTTP disk cache and visited-link state
persist under `QtWebEngine/arora[-<id>]/` at the data root — outside
`profiles/<id>/` and therefore outside anything `rv_seal` packs. A
"locked" profile still leaves every Chromium-side record on disk in
cleartext; sealing is an app-side-store guarantee, not a browsing-data
guarantee.

Too large for an audit fix — engine storage must relocate inside the
sealed tree (or gain a second sealed artifact), plus migration of
existing dirs and a `cache-hold/` semantics decision for the engine's
own cache. Filed as follow-up task **VSEC02**. `src/profilevault.h`'s
header doc now states the boundary plainly so it doesn't overpromise.

### F3 — OTR error pages could scrub the default profile's history — FIXED

`BrowserProfile::profileIdFor()` maps every unnamed (off-the-record)
profile to `"default"`. `WebPage::showErrorPage()` and
`showTorWarmupPage()` called
`HistoryManager::instanceFor(profileIdFor(profile()))->removeHistoryEntry(url)`
unconditionally — a private-mode page that failed to load deleted the
matching entry from the persistent default store it never wrote to.
Cross-context write against a store the context can't even append to.

Fix (`src/webpage.cpp`): both removals now gate on
`!profile()->isOffTheRecord()` — the same predicate the history feed
itself uses at page init.

Regression: `tst_privatebrowsing::otrErrorPageDoesNotScrubDefaultHistory`
seeds a real entry, drives an OTR navigation into a refused port, and
asserts the entry survives.

Every other `profileIdFor` call site was walked: the history feed is
already OTR-gated; downloads, autofill capture, searches, session state
and cookies all gate on `isOffTheRecord`; container-rule lookups and
adblock rule *reads* on an OTR page resolving to the owning persona are
read paths over non-secret config and are correct. The one remaining
OTR-adjacent write is listed under accepted risks.

### F4 — Auto-lock keyed to the ambient persona, not the closing window — FIXED

`BrowserApplication::windowDestroyed()` called
`ProfileVault::noteWindowClosed(ProfileStore::activeProfileId())` — the
focused profile, not the one whose window closed. Closing the last
"work" window while a "default" window held focus asked the vault to
auto-lock "default" (which still had windows → no-op) and never
evaluated "work". Worse, the check only ran when `m_mainWindows` was
completely empty, so the "last window of this profile" trigger could
not fire at all while any other persona had a window.

Fix (`src/browserapplication.{h,cpp}`): the `destroyed` connect is now a
lambda capturing the window's bound persona at construction;
`windowDestroyed(profileId)` runs `noteWindowClosed(profileId)` on
**every** window destruction, and the vault's `openWindowCount(id)`
(which already counts live windows by bound persona) decides whether
that profile's last window is gone.

### F5 — Windows could be opened on a sealed profile — FIXED

Every window-creation path (session restore over manifest personas,
File>New Window, the profile menu's per-persona entries, container
spawns, link-open-in-new-window, the extension shim, the profile
manager's Open button) funnels through
`BrowserApplication::newMainWindow()`, which bound the window to the
requested persona with no vault-state check. A window on a Locked
profile browses on empty stores *and* mints fresh plaintext under
`profiles/<id>/` — which the next `unseal()` then refuses to overwrite
(`PlaintextPresent`), wedging the profile until manual cleanup.

Fix: `newMainWindow()` fails closed (`nullptr`) when
`ProfileVault::isLocked(bound)`. Prompt ownership sits with the
contexts that can show one: session restore runs
`VaultUi::ensureUnlocked` once per sealed manifest persona (decline →
window skipped, remembered so N windows prompt once), the profile menu
and profile manager prompt on demand, and the postLaunch bring-up
re-prompts the active persona and creates its window on success.
`postLaunch()` also runs restore windowless when the launch placeholder
was refused — otherwise a sealed active profile silently skipped the
whole session restore. Every caller of `newMainWindow`/
`newMainWindowInContainer`/`mainWindow()` that dereferenced the result
now handles null.

Regression: `tst_profilee2e::sealedPersonaRefusesWindow` — a bare
`vault.lock` marker makes the persona Locked (state is marker-derived)
and `newMainWindow(id)` must return null while a clean persona still
opens.

### F6 — Registry `avatar` field was a path-traversal primitive — FIXED

`ProfileContext::avatar` is read verbatim from `profiles.json` and
joined under `profiles/<id>/` for `avatarPixmap()` reads, the
`QSaveFile` PNG write in `setAvatarFile()`, and the `QFile::remove` in
`removeAvatar()`. A crafted `"avatar": "../../victim"` steered reads —
and, sharper, deletion — to attacker-chosen paths.

Fix (`src/profileregistry.cpp`, `src/profilestore.cpp`): a
`sanitizedAvatarName()` accepts only a plain basename (non-empty, ≤255,
no `/` `\` `.` `..`) applied on `load()` AND `updateProfile()`, so a
traversal value can neither arrive from disk nor be written to it.

Regression: `tst_profilecontext::registryDropsTraversalAvatar` —
crafted-file load drop, update-time strip before write, plain basename
surviving both directions.

### F7 — `securestore.kdf` derive parameters were unbounded — FIXED

`store.rs::parse_kdf()` read `memory_kib`, `passes` and `lanes` from
`securestore.kdf` — an unauthenticated, user-writable file — and fed
them straight into Argon2. A 4-byte edit to `memory_kib` asks the
allocator for arbitrary memory; allocation failure is an abort, not a
recoverable error — a planted file was a kill-switch for the process.
`profilevault.rs`'s ARV1 header already declares an envelope;
`store.rs` now applies the same bounds *before* any derive and rejects
zero/oversize passes and lanes as `Corrupt`.

Regression: rustcore `kdf_params_from_untrusted_file_are_bounded` — a
real `.kdf` is byte-edited at each parameter offset (u32::MAX and 0)
and `rc_unlock` must return `RC_CORRUPT` rather than hang or abort; the
honest file still unlocks.

## Verified clean (sign-offs)

**(a) Profile isolation.** `BrowserPaths::isProfileId()` is the single
slug contract (`[a-z0-9_-]`, bounded, no `.`/`..`/separators), enforced
at the registry boundary (`isValidProfileId` on
`load()`/`updateProfile()`) and the path boundary (`profileDataDir`
falls back to `default` + warns on non-slug).
`normalizedWindowProfileId()` additionally requires registry membership
— a session manifest cannot invent a persona; unknown/garbage ids
degrade to `"default"` after one reload. `default` is refused at the
`ProfileStore::deleteProfile` manager layer, not just the UI.

**(b) Vault crypto.** `profilevault.rs` reads as built: fresh
`VaultKey::random()` + fresh salt per seal — key+nonce reuse across
re-seals is structurally impossible; chunk nonce =
`salt[0..8]‖chunk_index`; Threefish XEX tweak =
`salt[0..4]‖chunk‖block` — unique per (salt, chunk, block); `algo_id`
is inside the MAC'd header bytes, so a downgrade edit fails the header
MAC; two-pass unseal verifies the whole-payload MAC before any
plaintext exists, then re-verifies per-chunk tags while decrypting into
a staging dir that renames onto the destination only on success;
KEK-unwrap failure → `WrongPassword`, payload-MAC failure → `Tampered`
— the split the UI surfaces is faithful; key material rides
`Zeroizing` throughout (`VaultKey`, the 128-byte Threefish expansion,
chunk buffers).

**(c) Plaintext lifecycle.** `secureEraseTree()` is lstat-aware —
links are unlinked, never followed into the zero-overwrite; the packer
enumerates the same tree with `follow_symlinks(false)`, so every file
read is a file erased. `cache/` exclusion is spec'd and documented —
`cache-hold/` contents stay plaintext while locked. Dirty-boot markers
(`unlocked.dirty`) are recovery UX, not a security boundary — deleting
the marker merely suppresses the re-seal offer. Tar extraction
whitelists file/dir/symlink entries, caps entry count, and relies on
tar-rs `unpack_in` containment — `..`/absolute/symlink-target escapes
are refused (`tar_traversal_entries_refused`).

**(d) Migration + registry.** Registry writes go through `QSaveFile`
(temp + rename); a corrupt `profiles.json` yields the synthesized
default and is *not* clobbered (verified by `registryBasics`). F1
closes the symlink hole; the migration marker itself is `QSaveFile`
atomic and the pass is resumable (`resumeAfterPartialMove`).

**(e) UI/FFI boundary.** Password custody is `QByteArray` end-to-end;
the session cache wipes with `secureZero()` on lock/shutdown;
`WrongPassword` vs `Tampered` never conflates (unwrap vs payload MAC).
Wrong-password timing is bounded by Argon2 cost — no per-attempt
oracle beyond the inherent "was it right". Auto-lock's 30 s `QTimer`
poll can be *delayed* by a saturated event loop but never permanently
starved (timers always dispatch once their deadline passes), and the
idle clock only counts real input events (key/mouse/wheel/touch) —
posted-event floods can't fake activity. Lock while windows are open
is refused (`HasOpenWindows`) *and* F5 now makes the converse
impossible: no path opens a window on a sealed profile.

**(f) Supply chain.** Vault crates are exact-pinned (`=x.y.z`):
`aes-gcm 0.10.3`, `argon2 0.5.3`, `zeroize 1.8.1`, `sha2 0.11.0`,
`hmac 0.13.0`, `threefish 0.6.0`, `skein 0.2.0`, `tar 0.4.46` —
`Cargo.lock` matches `Cargo.toml` and `check-supplychain` (cargo-deny
advisories/licenses/bans/sources + cargo-audit) passes. The
RustCrypto threefish/skein unaudited-upstream caveat is still
documented at `profilevault.rs` and is why Threefish is opt-in rather
than default.

**(g) FFI surface.** `ffi_surface.rs` exercises null pointers,
unsupported algorithms, invalid destinations, truncated/malformed
blobs and wrong passwords through `catch_unwind` — every `rv_*` entry
converts panics to status codes. Length/pointer validation precedes
any Rust-side deref.

## Accepted risks (documented, not fixed)

- **`cache/` plaintext while locked** — spec'd exclusion; the sealed
  set deliberately skips it, the hold dirs sit next to the vault.
  Anyone reading the disk sees a locked profile's cache. VSEC02 may
  subsume this with the engine-cache decision.
- **SSD/CoW erase limits** — secure erase is one zero overwrite +
  fsync + unlink; wear-leveling and snapshots can retain old blocks.
  The guarantee is "no readable plaintext at the path", stated in
  `profilevault.h` — no fake multi-pass claim.
- **`QString` residual copies** — passwords cross Qt as `QByteArray`
  and are wiped via `secureZero()`, but `QString`/`QLineEdit` copies
  made inside the dialogs (and inside Qt's own line-edit internals)
  have no guaranteed zeroize — Qt can't promise it. Documented
  honestly; the FFI boundary itself is `QByteArray`→`&[u8]`.
- **Explicit adblock rule added from an OTR context menu** — "Block
  Image" writes the image URL into the owning persona's persistent
  custom-rules file. User-initiated persistence, but the URL of a
  privately-viewed resource does reach disk. Accepted: the user asked
  for the rule; silently not saving it would be worse.
- **`key_material_zeroized_on_drop` fails under `--release`** — the
  test probes a dropped stack slot via `black_box`; the observation
  technique is compiler-sensitive (debug passes; the heap `Zeroizing`
  leg proves the mechanism). Pre-existing on this toolchain,
  independent of these changes — recorded rather than masked.
- **Symlinked `profiles/<id>` post-migration** — the migration pass
  refuses links, but a link planted *after* migration is still
  followed by stores (the path contract can't fail). Mitigated to a
  once-per-path `qWarning`; a same-uid attacker who can write the
  data root can already write the files directly, so the residual is
  detection, not prevention.

## Verification

- `qmake && make -j2` — clean build, zero new warnings.
- `tst_profilecontext` 12/12, `tst_privatebrowsing` 12/12,
  `tst_profilee2e` 17/17, `tst_vaultengine` 16/16 — all PASS offscreen.
- `cargo test --release kdf_params_from_untrusted_file_are_bounded` — PASS.
- `cargo test --release` (full): 259/260 — the one failure is
  `key_material_zeroized_on_drop`, pre-existing and
  toolchain-dependent (accepted-risk list above).
- `make check`: check-warnings PASS (no-rust build clean),
  check-supplychain PASS, engine-audit gate satisfied by the new
  ENGINE.md row for `profilevault.h`, check-rustcoverage + autotest
  suite per below.
