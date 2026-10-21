# SEC28 — Vault crypto coverage check: credential/autofill stores

Verdict on VSEC01's coverage, what it missed, and what this task
changed.  Date: 2026-10-21.  Tree: Qt 6.12.0 / QtWebEngine port,
rustcore the default backend, `CONFIG+=no-rust` the supported
fallback.

## Verdict: VSEC01 audited the crypto core — of the Rust backend only

VSEC01's (b)/(c)/(e)/(f)/(g) sign-offs genuinely covered the
cryptographic core: `profilevault.rs`'s ARV1 envelope (salt-bound
chunk nonces, MAC-before-decrypt, `Zeroizing` key custody,
downgrade-proof MAC'd header), `store.rs`'s AEAD framing (random
96-bit GCM nonce per seal, the ARSEC1 tag-before-ct compatibility
layout), the Argon2id KDF (m=64 MiB / t=3 / p=4 / 16-byte fresh salt —
RFC 9106's memory-constrained profile), the verifier-blob
wrong-passphrase path (GCM tag failure — no oracle), and F7 bounded
the *Rust* `securestore.kdf` parser.

The gaps: the **non-rustcore C++ backend** in `securestore.cpp` — a
supported build (`CONFIG+=no-rust`, or any host without cargo) that
implements the *same* ARKDF1/ARSEC1 on-disk contract — was never
audited, and the legacy `autofill.dat` retirement was only checked
for the sealed case.  Every finding below sat outside the FFI seam
VSEC01 focused on.

## Findings (all FIXED)

### G1 — no-rust `parseKdfFile`: unbounded KDF parameters — FIXED

`securestore.kdf` is unauthenticated, user-writable input.  F7
bounded `store.rs::parse_kdf` but the C++ `parseKdfFile()` fed
memoryKiB / passes / lanes straight into the vendored
`arktos_argon2id`, whose only guard is `memoryKiB >= 8*lanes`.  A
planted `.kdf` gave three primitives:

- `memoryKiB = 0xFFFFFFFF` → ~4 TiB `malloc` — an OOM kill or a hard
  lock-out;
- `passes = 0xFFFFFFFF` → an effectively endless KDF loop pinning
  the UI thread;
- `lanes = 0x20000001` → `8U*lanes` and `lanes*ARGON2_SYNC_POINTS`
  wrap to small values while `segment_length`/`lane_length` stay
  huge, so the lane-seed loop writes 1024 bytes per lane at
  `lane * lane_length` offsets — **out-of-bounds heap writes**, a
  memory-corruption primitive, not just a kill-switch.

Fix: the same envelope the Rust parser enforces is applied to the
parsed words before the KDF sees them — memoryKiB ∈ [8 MiB, 4 GiB],
passes ∈ [1, 16], lanes ∈ [1, 16].  Regression:
`tst_securestore::hostileKdfFileRejected` byte-edits each parameter
word of a real `.kdf` (plus zero/over-max lanes that overflow the
vendored arithmetic) and must be refused on both backends, while the
honest file still unlocks.

### G2 — no-rust `enablePassphraseProtection`: cached key zeroed but left registered — FIXED

MPROF02's per-profile hash refactor replaced `secureZero(newKey)`
(wiping the local copy — correct) with
`secureZero(s_derivedKeys[id])`.  `secureZero` detaches the shared
`QByteArray` and zeroes the detached hash entry, but the entry stays
in `s_derivedKeys`: the store then reported `isUnlocked() == true`
while holding an **all-zero key**.  Every subsequent seal produced
ciphertext under a publicly-known key — plaintext-equivalent for
anyone who can read the file — every open of the just-resealed
stores failed outright, and the real derived key survived unwiped in
the freed heap copy.

Fix: wipe the local `newKey` copy only; the hash entry keeps the
real derived key, matching the rustcore backend (`kdf_create` leaves
`derived_key` set) and pre-MPROF02 semantics.  Regression:
`tst_securestore::unlockReopensPostEnableSeals` — blobs sealed in
the same session as enable must open under the real key after a
lock/re-unlock; the pre-existing `autofillDatResealed` and
`passphraseCustody` legs also now run (and pass) under
`CONFIG+=no-rust`.

### G3 — plaintext legacy `autofill.dat` kept forever as `.migrated` — FIXED

RCORE05 retired an imported legacy file by renaming it to
`autofill.dat.migrated` — correct for a sealed blob (a recoverable
ciphertext backup), wrong for a Qt4-era plaintext file: the renamed
copy kept passwords and card numbers at rest in the clear
indefinitely, defeating the point of the sealed store.

Fix: `retireLegacyFile` peeks at the ARSEC1 magic — sealed → rename
backup (unchanged); anything else → `secureEraseFile` (one zero
overwrite + remove, the same honest "no readable plaintext at the
path" guarantee `secureEraseTree` makes; wear-leveling/CoW caveats
apply equally).  `loadFormData` additionally sweeps a stale
plaintext `.migrated` left behind by a pre-fix import.  Regression:
`tst_autofill::legacyPlaintextImport` (no retired copy survives),
`plaintextRetiredCopyErased` (stale plaintext scrubbed on load, a
sealed backup kept).

### G4 — `sealString` fail-open wrote plaintext credentials — FIXED

`sealString()` returned its input verbatim when the seal was
refused — a locked store (declined or non-interactive unlock) or a
missing crypto backend — and the proxy-password write stored it
unconditionally.  A user who enabled passphrase protection could
silently downgrade the stored proxy password to plaintext in
QSettings just by saving the dialog while locked.

Fix: `sealString` returns empty on failure (contract documented in
the header); the settings write keeps the previous stored value when
the seal is refused — an explicitly cleared field still writes
empty.  Regression: `tst_securestore::sealStringFailsClosed`.

### G5 — no-rust locked-store save downgraded `autofill.dat` — FIXED

`saveFormData()`'s plaintext fallback exists for a crypto-less
system — but a failed seal on a passphrase-protected store means the
unlock was *declined*, and the fallback overwrote the sealed file
with raw plaintext, non-atomically and without owner-only
permissions.

Fix: the fallback refuses when `passphraseProtectionEnabled()` — a
sealed store exists, so the data stays in memory with a warning,
matching the rustcore path — and the crypto-less case now writes
through `QSaveFile` at 0600.  Regression:
`tst_autofill::lockedStoreKeepsSealedFile` (no-rust leg: a locked
store leaves the sealed file untouched).

### G6 — decoded autofill cache left plaintext in freed heap — FIXED

The Rust `Store::autofill` cache held decoded `FormRecord` /
`AddressRecord` values — passwords included — dropped unwiped on
`lock()` or a data-dir switch.  Same residual class as VSEC01's
documented `QString` caveat, but the store's own cache is cheap to
fix: `AutofillData` now zeroizes every string field on `Drop`.
(zeroize's derive feature would add `zeroize_derive` to the pinned
supply chain; a hand-rolled `Drop` covers the same fields.)

## Verified clean (sign-offs)

- **KDF (both backends):** Argon2id v1.3, m=64 MiB / t=3 / p=4,
  16-byte random salt per enable/change; parameters ride the `.kdf`
  header for forward compatibility and are now bounded identically
  on parse in both parsers.
- **Seal/unseal:** AES-256-GCM, fresh random 96-bit nonce per seal,
  tag verified before plaintext is released (EVP `GCM_SET_TAG`
  before `DecryptUpdate` on the C++ side, RustCrypto on the Rust
  side — identical ARSEC1 framing, stores written by either backend
  open on the other).
- **Wrong passphrase:** the verifier blob fails its GCM tag → flat
  `false` / `RC_WRONG_PASSPHRASE` — no oracle beyond Argon2's
  inherent per-attempt cost; the interactive prompt is a bounded
  3-attempt loop.
- **Key custody:** passphrase mode never writes key material to disk
  (`.kdf` holds params+salt+verifier only); the derived key lives in
  `s_derivedKeys` / `Store::derived_key` (`Zeroizing`) and is wiped
  on `lock()`, per-profile `lock(id)`, data-dir switch and test
  reset; FFI transition copies are wiped by the caller.
- **Concurrent unlock prompts:** `s_unlockPromptActive` fails a
  second requester closed rather than stacking dialogs.
- **`credentials.dat` / `autofill-store.dat`:** sealed whole-map
  blobs under the shared custody key, bounds-checked decoders,
  atomic temp+rename writes at 0600.
- **Custody transitions:** crash-safe ordering — new artifact
  committed, every consumer re-sealed under explicit old/new keys,
  old artifact retired last; unreadable blobs warn-and-continue.

## Verification

- `qmake && make -j2` — clean build, zero new warnings.
- `.devin/check-warnings.sh` (`CONFIG+=no-rust` full build): PASS,
  zero warnings — the C++ backend compiles clean.
- `tst_securestore` 20/20 and `tst_autofill` 10/10 (+1 skip) under
  **both** the default rustcore build and `CONFIG+=no-rust`
  (offscreen, `--disable-gpu`).
- `tst_settingsdialog` 58/58, `tst_networkaccessmanager` 9/9
  (proxy-password consumers of the fail-closed `sealString`).
- `--autofill-smoke` PASS — fill / capture / OTR-drop / persistence
  / sealed-at-rest / securestore legs.
- `cargo test --release`: 267/268 — the single failure is
  `key_material_zeroized_on_drop`, the pre-existing
  toolchain-sensitive test already documented in VSEC01's accepted
  risks.
