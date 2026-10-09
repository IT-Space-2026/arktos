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

## URL stripping (SEC17)

`rc_urlstrip` is the ClearURLs-style tracking-parameter stripper the
request interceptors redirect through.  The ruleset is a vendored JSON
file (`data/urlstrip-rules.json`, embedded via `include_str!`):
`params` is a list of query-parameter names to remove — a trailing
`*` makes it a prefix rule (`utm_*`); `exceptions` lists hosts where
stripping is skipped entirely, or partially via a `keep` list, for
sites whose auth flow breaks when a listed name disappears.  Matching
is case-insensitive and decodes percent-escapes in names; kept query
segments pass through byte-for-byte.

Updates land like filter lists: drop a newer `urlstrip-rules.json`
into the app data dir and call `rc_urlstrip_reload()` — the override
wins over the builtin, a malformed override keeps the previous set.
`rc_urlstrip_load_rules` is the same swap with the JSON handed over
directly (update/test seam).  Without `CONFIG+=rustcore` the strip
stage simply isn't there — the interceptors degrade to no-strip.

## Domain blocklist (SEC18)

`rc_blocklist_check(host)` is the local anti-phishing/malware domain
probe the interceptors consult before a main-frame navigation leaves —
the "poor man's Safe Browsing": every answer comes from a shipped,
updatable list, so no URL or hash ever leaves the machine for a lookup.
The list parser accepts every common feed row shape verbatim (bare
domains, `ip domain` hostfile rows like URLhaus, `scheme://host/path`
URL rows like OpenPhish, `||dom^`/ `*.dom` decorations); comments and
junk are skipped, entries are validated as conservative DNS names and
stored lowercase.

Matching is exact + suffix: a listed `evil.example` blocks itself and
every subdomain, a listed subdomain never reaches up, and label-boundary
matching means `notevil.example` is never caught; IP literals only
exact-match.  The active set is the union of the vendored seed
(`data/blocklist-domains.txt`, embedded via `include_str!`) and
`<data dir>/blocklist-domains.txt` written by the Qt-side
DomainBlocklist updater, which then calls `rc_blocklist_reload()` —
updates can only *add* coverage, a malformed or missing override falls
back to the seed.  `rc_blocklist_load` swaps in a caller-supplied body
(update/test seam); `rc_blocklist_count` reports the merged size.  The
Rust side never touches the network — fetching is entirely Qt-side and
consent-gated like remote filter lists.  Without `CONFIG+=rustcore` the
whole feature is absent and nothing is ever blocked.

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
