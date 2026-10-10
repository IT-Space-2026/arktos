#!/bin/sh
# check-rustcoverage.sh — COV05 Rust line-coverage gate
#
# For every crate in the tree (any tracked/untracked Cargo.toml
# outside target/ — src/rustcore, src/rustdl, src/adblock/rust today;
# new crates are picked up automatically) runs:
#
#   cargo llvm-cov --workspace --json --summary-only
#
# and enforces a per-crate minimum on the reported total LINE coverage
# (user directive: test coverage minimum 85% on the rust-made code).
# Instrumented objects land in <crate>/target/llvm-cov-target (already
# gitignored) so the in-tree qmake build is never polluted.
#
# The per-crate results are also refreshed inside the marked
# `rustcov` section of .devin/COVERAGE.md (between the
# <!-- rustcov:start --> / <!-- rustcov:end --> markers — appended at
# the end when the markers are absent).
#
# Env:
#   ARORA_RUSTCOV_MIN=N   per-crate line-coverage gate (default 85;
#                        set 0 for report-only)
#   ARORA_RUSTCOV_JOBS=N cargo build jobs for the instrumented build
#                        (default 2 — the loop's memory watchdog)
#
# Exit status: 0 when every crate meets the gate, or when the Rust
# tools themselves are absent (the Rust surface is optional — a
# machine without cargo/cargo-llvm-cov builds the tree fine, so the
# gate skips rather than breaking `make check` there).  1 when any
# crate falls under the gate or the measurement fails.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPORT_MD="$ROOT/.devin/COVERAGE.md"
MIN=${ARORA_RUSTCOV_MIN:-85}
JOBS=${ARORA_RUSTCOV_JOBS:-2}

note() { echo "check-rustcoverage: $*"; }
die() { echo "check-rustcoverage: FAIL — $*" >&2; exit 1; }

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
# cargo locates cargo-* subcommand binaries on PATH — make sure the
# resolved toolchain's bin dir is searchable.
PATH="$(dirname "$CARGO"):$PATH"
export PATH

if ! "$CARGO" llvm-cov --version >/dev/null 2>&1; then
    note "SKIP — cargo-llvm-cov not installed for this toolchain"
    note "  install user-local: cargo install cargo-llvm-cov"
    exit 0
fi

# spikes/ is excluded on purpose — same convention as
# check-supplychain.sh: spike harnesses are throwaway experiments
# (servo-embed carries no tests), not shipped rust-made code; a spike
# that graduates into the tree moves out of spikes/ and gets gated.
CRATES=$(cd "$ROOT" && git ls-files -c -o --exclude-standard -- '*/Cargo.toml' 'Cargo.toml' \
    | grep -v '/target/' | grep -v '^spikes/' || true)
[ -n "$CRATES" ] || { note "no crates in tree — nothing to measure"; exit 0; }

command -v python3 >/dev/null 2>&1 || die "python3 not found"

ROWS=""
FAILED=""
for toml in $CRATES; do
    dir=$(dirname "$toml")
    note "measuring $dir (instrumented build, may take a while)"
    json=$(cd "$ROOT/$dir" \
        && CARGO_BUILD_JOBS=$JOBS "$CARGO" llvm-cov --workspace \
            --json --summary-only 2>/dev/null) \
        || die "cargo llvm-cov failed in $dir"
    pct=$(printf '%s' "$json" | python3 -c '
import json, sys
doc = json.load(sys.stdin)
pcts = [s["totals"]["lines"]["percent"] for s in doc.get("data", [])]
print(f"{max(pcts):.2f}" if pcts else "0.00")
') || die "could not parse llvm-cov JSON for $dir"
    note "  $dir lines = ${pct}% (gate ${MIN}%)"
    ROWS="$ROWS$dir|$pct
"
    python3 - "$pct" "$MIN" "$dir" <<'PYEOF' || FAILED="$FAILED $dir"
import sys
sys.exit(0 if float(sys.argv[1]) >= float(sys.argv[2]) else 1)
PYEOF
done

# Refresh the rustcov section of COVERAGE.md (in-place, idempotent —
# identical content leaves the tree clean).
if [ -f "$REPORT_MD" ]; then
    # Rows travel via env: stdin must stay free for the program heredoc.
    RUSTCOV_ROWS="$ROWS" python3 - "$REPORT_MD" "$(date -u '+%Y-%m-%d %H:%M UTC')" \
        "$(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)" \
        "$("$CARGO" llvm-cov --version 2>/dev/null || echo cargo-llvm-cov)" "$MIN" <<'PYEOF'
import os, sys

md, stamp, commit, tool, gate = sys.argv[1:6]
rows = [l.split("|") for l in os.environ["RUSTCOV_ROWS"].splitlines() if "|" in l]
start, end = "<!-- rustcov:start -->", "<!-- rustcov:end -->"

body = [start, "", "## Rust crates (COV05)", "",
        f"Generated: {stamp} on commit {commit}",
        f"Toolchain: {tool}", "",
        "Per-crate total LINE coverage from"
        " `cargo llvm-cov --workspace --json --summary-only`,"
        f" gated at {gate}% minimum per crate by"
        " `make check` (.devin/check-rustcoverage.sh;"
        " `ARORA_RUSTCOV_MIN` overrides).", "",
        "| Crate | Lines |", "|-------|-------|"]
for d, p in rows:
    body.append(f"| {d} | {p}% |")
body += ["", end]

with open(md) as f:
    lines = f.read().splitlines()
try:
    i, j = lines.index(start), lines.index(end)
    lines = lines[:i] + body + lines[j + 1:]
except ValueError:
    lines += ([""] if lines and lines[-1] else []) + body
with open(md, "w") as f:
    f.write("\n".join(lines) + "\n")
PYEOF
fi

if [ -n "$FAILED" ]; then
    die "line coverage under ${MIN}% in:$FAILED"
fi
note "PASS — all crates at or above ${MIN}% line coverage"
exit 0
