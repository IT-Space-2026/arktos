#!/bin/sh
# check-supplychain.sh — SEC21 Rust supply-chain vetting gate
#
# For every crate in the tree (any tracked/untracked Cargo.toml
# outside target/ — src/rustcore, src/rustdl, src/adblock/rust today;
# new crates are picked up automatically) runs:
#
#   cargo deny --locked check   advisories (yanked = deny), license
#                               allow-list, bans (no wildcard deps,
#                               duplicate versions flagged), sources
#                               (crates.io only).  --locked asserts the
#                               committed Cargo.lock still matches
#                               Cargo.toml — a dep bump must commit
#                               the regenerated lockfile.
#   cargo audit                 RustSec advisory DB scan of that same
#                               committed Cargo.lock.  Any unfixed
#                               vulnerability fails the build.
#
# Policy lives in <crate>/deny.toml; all crates share the same file.
# Cargo.lock is committed for every crate so what the gate scans is
# exactly what ships.
#
# Dep-update workflow (SEC21): in the crate dir run `cargo update`
# (or edit Cargo.toml then `cargo update`), re-run this script until
# green — advisories/licenses are re-vetted — then commit Cargo.toml
# AND Cargo.lock together.
#
# Env:
#   ARORA_SUPPLYCHAIN_OFFLINE=1  no-network mode: cargo deny --frozen
#                                and cargo audit -n --stale run purely
#                                against the cached advisory DB +
#                                registry index under ~/.cargo.  Fails
#                                if nothing is cached yet.
#
# Exit status: 0 when every crate is clean, or when the Rust tools
# themselves are absent (the Rust surface is optional — a machine
# without cargo/cargo-deny/cargo-audit builds the tree fine, so the
# gate skips rather than breaking `make check` there).  1 on any
# policy violation, advisory, or tool failure.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
OFFLINE=${ARORA_SUPPLYCHAIN_OFFLINE:-0}

note() { echo "check-supplychain: $*"; }
die() { echo "check-supplychain: FAIL — $*" >&2; exit 1; }

# Same toolchain resolution as the .pri files: PATH first, then the
# user-local rustup default.  No sudo, no system packages.
CARGO=$(command -v cargo || true)
if [ -z "$CARGO" ] && [ -x "$HOME/.cargo/bin/cargo" ]; then
    CARGO=$HOME/.cargo/bin/cargo
fi
if [ -z "$CARGO" ] || ! "$CARGO" --version >/dev/null 2>&1; then
    note "SKIP — no cargo toolchain (Rust surface is optional)"
    exit 0
fi

if ! "$CARGO" deny --version >/dev/null 2>&1 \
    || ! "$CARGO" audit --version >/dev/null 2>&1; then
    note "SKIP — cargo-deny/cargo-audit not installed for this toolchain"
    note "  install user-local: cargo install cargo-deny cargo-audit"
    exit 0
fi

DENY_FLAGS="--locked"
AUDIT_FLAGS=""
if [ "$OFFLINE" = 1 ]; then
    DENY_FLAGS="--frozen"
    AUDIT_FLAGS="-n --stale"
    [ -d "$HOME/.cargo/advisory-db" ] \
        || die "ARORA_SUPPLYCHAIN_OFFLINE=1 but no cached advisory DB at ~/.cargo/advisory-db"
fi

CRATES=$(cd "$ROOT" && git ls-files -c -o --exclude-standard -- '*/Cargo.toml' 'Cargo.toml' \
    | grep -v '/target/' || true)
[ -n "$CRATES" ] || { note "no crates in tree — nothing to vet"; exit 0; }

FAILED=""
for toml in $CRATES; do
    dir=$(dirname "$toml")
    note "vetting $dir"
    ( cd "$ROOT/$dir" && "$CARGO" deny $DENY_FLAGS check ) \
        || FAILED="$FAILED $dir(deny)"
    ( cd "$ROOT/$dir" && "$CARGO" audit $AUDIT_FLAGS ) \
        || FAILED="$FAILED $dir(audit)"
done

if [ -n "$FAILED" ]; then
    die "supply-chain violations in:$FAILED"
fi
note "PASS — all crates clean (advisories, licenses, bans, sources)"
exit 0
