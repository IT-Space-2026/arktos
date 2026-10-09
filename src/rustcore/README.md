# arora-rustcore — the shared Rust core

Arora's engine-neutral application components live here behind a C ABI
(the Mozilla application-services pattern): the Qt shell consumes them
identically regardless of web engine, and the same sources build for
Linux/macOS/Windows/BSD — only the artifact name changes
(`libarora_rustcore.a/.so/.dylib/.dll`).

## Build

    cd src/rustcore && cargo build --release
    qmake CONFIG+=rustcore && make

`CONFIG+=rustcore` defines `ARORA_RUSTCORE`, adds `include/` to the
include path, links `target/release/libarora_rustcore.a`, and wires a
Makefile rule so `make` rebuilds the crate when `src/*.rs` changes.
Without the flag nothing references the crate — the tree builds with
no Rust toolchain installed at all (same convention as ADB02's
`adblock_rust`).  The crate also emits a cdylib; the static archive is
what qmake links so the browser stays a single self-contained binary.

Toolchain: user-local rustup (`~/.cargo/bin/cargo`), no sudo.  For
offline builds `cargo vendor` + a `.cargo/config.toml` source
replacement works unchanged; `Cargo.lock` is committed.

## ABI conventions (reused by every component)

- `enum RcStatus` is the only error channel; `rc_last_error_message()`
  carries the detail string per thread.
- Out-parameters use `RcBuffer` (`rc_buffer_free`) / `char *`
  (`rc_string_free`).  Null out-pointers are legal ("result unwanted").
- Every entry point is `catch_unwind`-guarded and treats pointers as
  untrusted.
- `rc_set_change_callback(cb, userdata)` registers one process-wide
  sink; components announce topic strings (`"credentials"` today).
  `RustCoreBridge` (src/rustcorebridge.*) re-emits them as queued Qt
  signals — Rust never runs on the GUI thread.

## Credentials (RCORE01 part B)

`securestore.key` / `securestore.kdf` / sealed `ARSEC1` blob formats are
byte-identical to the C++ implementation (AES-256-GCM, tag before
ciphertext, `ARKDF1` header with m/t/p u32le + salt16 + verifier), so
stores written by either side open on the other and no migration is
needed.  `credentials.dat` adds the `rc_cred_*` named-credential map
under the same custody.

## Post-quantum posture — read before "adding PQ"

At rest this store is **already post-quantum-sufficient**: AES-256-GCM
with a 256-bit key retains ~128-bit security under Grover, and Argon2id
is a memory-hard symmetric KDF with no quantum weakness.  PQC
(ML-KEM-768 hybrid) protects *key exchange* — there is no exchange on a
local file.  When a sync layer lands, ML-KEM belongs **there**, not in
local storage.  Do not add it here.

## Supply chain (SEC21)

RustCrypto crates are exact-pinned in `Cargo.toml`; `Cargo.lock` is
committed; `deny.toml` is the cargo-deny policy (license allowlist +
advisory gate).  Update workflow: bump deliberately, `cargo update`,
`cargo deny check` / `cargo audit`, commit both manifests.
