# arora-rustcore — the shared Rust core

Arora's engine-neutral application components live here behind a C ABI
(the Mozilla application-services pattern): the Qt shell consumes them
identically regardless of web engine, and the same sources build for
Linux/macOS/Windows/BSD — only the artifact name changes
(`libarora_rustcore.a/.so/.dylib/.dll`).

## Build

    qmake && make            # crate builds automatically (RDEF01 default)
    cd src/rustcore && cargo build --release   # or by hand

The crate is the DEFAULT code path: the project-root `.qmake.conf`
adds `CONFIG+=rustcore` whenever a cargo toolchain resolves (PATH,
then `~/.cargo/bin`).  The `rustcore` scope defines
`ARORA_RUSTCORE`, adds `include/` to the
include path, links `target/release/libarora_rustcore.a`, and wires a
Makefile rule so `make` rebuilds the crate when `src/*.rs` changes.
Opt out with `qmake CONFIG-=rustcore` (this crate) or
`CONFIG+=no-rust` (all crates); with no toolchain the flag is simply
never added and the tree builds the Qt fallback path — the same
convention as ADB02's `adblock_rust`.  The crate also emits a cdylib;
the static archive is what qmake links so the browser stays a single
self-contained binary.

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
directly (update/test seam).  In a no-rust build the strip
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
consent-gated like remote filter lists.  In a no-rust build the
whole feature is absent and nothing is ever blocked.

## Second-opinion TLS verification (SEC22)

`rc_tls_check(host, port, flags)` performs a real blocking TLS
handshake to the host (TLS 1.2/1.3, ALPN `http/1.1`, SNI = host,
5 s connect / 5 s per-io / 15 s total) and evaluates the presented
chain with rustls+webpki against the platform root store
(rustls-native-certs) plus any `rc_tls_add_root` anchors — a second,
independent verdict beside whatever the engine decided.  It answers
a JSON verdict, never an FFI error for a reachable-but-bad chain:

- `verified` — handshake done, chain validated.
- `warning` — chain evaluated and FAILED; `error_class` is one of
  `expired`, `not-yet-valid`, `bad-hostname`, `untrusted-root`,
  `broken-chain`, `weak-signature`, `revoked`.
- `unverified` — the chain was never evaluated: `network`
  (dns/connect/timeout), `protocol` (TLS-level failure before the
  chain), `root-store` (no usable anchors).  A network hiccup is not
  a bad chain — the Qt side must never render it as one.
- `refused` — the probe declined: `private-host` (loopback/private/
  LAN/.onion unless `RC_TLS_F_ALLOW_LOCAL`), `invalid-host`,
  `invalid-port`.

The chain the verifier actually evaluated is captured inside the
verify callback and summarized per cert (subject, issuer, validity
window, signature-algorithm OID, SANs) — a rejected chain is exactly
what the site panel needs to show.  Honest limits, spelled out in the
verdict: no OCSP/CRL revocation fetch (`"revocation":"not-checked"`),
and the probe is a *direct* connection — callers must not issue it
for tor-mode pages or while an application proxy is set (bypassing
SOCKS would de-anonymize the user), and private/loopback targets are
refused outright (SSRF discipline, same rule as the interceptors).
`data/testcerts/` holds the scratch-CA fixture set the unit tests
probe; `gen_testcerts.py` regenerates it.

## Extension package verification (EXT06)

`rc_ext_verify_package(bytes, expected_sha256?, pinned_pubkey?)` is the
untrusted-bytes boundary of the extension system: downloaded .zip/.crx
packages and self-hosted update payloads are verified before Qt's
installer or the review dialog sees a byte.  The FFI answers a JSON
verdict — `status: valid|rejected`, `format: zip|crx3`,
`signing: unsigned|signed`, `signature_check: none|structure-only`,
`expected_sha256: match|mismatch|not-declared`, archive stats, and the
embedded manifest's `rc_ext_manifest_check` verdict.

- CRX3: magic + version + header length are checked, the signed-header
  protobuf is scanned field-by-field on the wire format (length bounds
  enforced), and the embedded zip is re-verified.  A signature block is
  honestly reported as `structure-only` — v1 does no cryptographic
  signature verification, and a caller-supplied pinned key reports
  `no-pinning-configured`.  An unsigned package is a classification,
  not a rejection.
- Zip: EOCD + central-directory bounds, multi-disk and zip64 refusal,
  entry-count and total-uncompressed caps, per-member local-header
  checks, and member names rejecting `..`, absolute paths, drive
  letters and backslash tricks.  A missing/unreadable `manifest.json`
  rejects the package.
- `expected_sha256` carries the update manifest's declared
  `hash_sha256` — a tampered download fails before Qt writes it.

`rc_ext_manifest_check(json)` parses a manifest.json (≤ 1 MiB) and
returns the pre-classified fields the review dialog consumes: name,
version, update_url, key, permissions/host_permissions split into
`unsupported`, `unverified` and `dangerous` (nativeMessaging, debugger,
webRequestBlocking, cookies, browsingData, `<all_urls>`/`*://*/*`),
plus a non-fatal `errors` list — the Qt side never parses raw
package JSON itself.

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
