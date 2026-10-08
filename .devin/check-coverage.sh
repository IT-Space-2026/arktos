#!/bin/sh
# check-coverage.sh — clang source-based coverage harness (COV01)
#
# lcov is not available on this box, so coverage uses clang's
# source-based instrumentation:
#   * copies the tracked tree (git ls-files) into a throwaway build dir
#     so instrumented objects/moc files never pollute the in-tree build
#   * builds with `qmake -spec linux-clang CONFIG+=coverage`
#     (-fprofile-instr-generate -fcoverage-mapping — see coverage.pri)
#   * runs the full autotest suite plus the app's --*-smoke flags under
#     LLVM_PROFILE_FILE (continuous mode, so counts survive the known
#     exit-139 profile-teardown crashes and parallel writers)
#   * merges profraw with llvm-profdata and reports with llvm-cov
#   * rewrites .devin/COVERAGE.md with the src/ baseline
#
# Env:
#   ARORA_COVERAGE_MIN=N   exit non-zero when src/ line coverage < N
#                          (default 80 — the COV04 gate; set 0 for
#                          report-only)
#   ARORA_COVERAGE_JOBS=N  parallel make jobs (default: nproc, max 4)
#   ARORA_COVERAGE_KEEP=1  keep the instrumented build tree for
#                          inspection instead of deleting it

set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPORT_MD="$ROOT/.devin/COVERAGE.md"

JOBS=${ARORA_COVERAGE_JOBS:-$(nproc 2>/dev/null || echo 2)}
[ "$JOBS" -gt 4 ] && JOBS=4
MIN=${ARORA_COVERAGE_MIN:-80}
KEEP=${ARORA_COVERAGE_KEEP:-0}

die() { echo "check-coverage: $*" >&2; exit 1; }

command -v clang++ >/dev/null 2>&1 || die "clang++ not found"
LLVM_PROFDATA=$(command -v llvm-profdata-18 || command -v llvm-profdata) \
    || die "llvm-profdata(-18) not found"
LLVM_COV=$(command -v llvm-cov-18 || command -v llvm-cov) \
    || die "llvm-cov(-18) not found"
command -v python3 >/dev/null 2>&1 || die "python3 not found"
command -v git >/dev/null 2>&1 || die "git not found"

BUILD=$(mktemp -d /tmp/arora-cov.XXXXXX)
echo "check-coverage: build tree $BUILD"

# Copy tracked + untracked-nonignored files only: the source tree carries
# in-tree Makefiles/.obj/binaries that would confuse the fresh build.
cd "$ROOT" || die "cannot cd to $ROOT"
git ls-files -z -c -o --exclude-standard | tar --null -T - -cf - \
    | tar -x -C "$BUILD" || die "tree copy failed"

cd "$BUILD" || die "cannot cd to $BUILD"
# shellcheck disable=SC1090
. "$ROOT/.devin/qt-env.sh"

QMAKE=$(command -v qmake6 || command -v qmake) || die "qmake not found"

echo "check-coverage: qmake ($("$QMAKE" -query QT_VERSION), linux-clang) + make -j$JOBS"
"$QMAKE" -spec linux-clang CONFIG+=coverage arora.pro || die "qmake failed"
make -j"$JOBS" sub-src sub-autotests || die "build failed"

PROFDIR="$BUILD/.coverage"
mkdir -p "$PROFDIR"
# %m%c = one continuous-mode profraw per binary: counters stream to the
# file (SIGSEGV teardowns keep their counts) and distinct binaries get
# distinct files — a single %c file silently drops every binary but the
# first writer.
export LLVM_PROFILE_FILE="$PROFDIR/%m%c.profraw"

# Isolate HOME: the tests and smokes read/write the real app profile
# dirs (~/.local/share/Arora, ~/.qttest) — a scratch HOME keeps runs
# deterministic and leaves the user's data untouched.
HOME="$BUILD/.home"
export HOME
mkdir -p "$HOME"

echo "check-coverage: running autotest suite (offscreen)"
(cd autotests && ./runTests.sh)
TESTS_RC=$?
[ "$TESTS_RC" -eq 0 ] || echo "check-coverage: WARNING: autotests exited $TESTS_RC"

# --download-smoke takes a URL operand (a file:// URL works headless);
# run it separately, the rest take no argument.
SMOKE_FLAGS="--quit-after-load --app-smoke --browser-smoke --nam-smoke
--cookie-smoke --history-smoke --bookmarks-smoke --search-smoke
--settings-smoke --autofill-smoke --find-smoke --source-smoke
--adblock-smoke --adblock-list-smoke --extension-smoke
--extension-otr-smoke --extension-update-smoke --ua-smoke
--container-smoke
--icons-smoke --fingerprint-smoke --ping-smoke --httpsonly-smoke --telemetry-smoke
--tls-smoke --tls-off-smoke --webrtc-smoke --webrtc-off-smoke"
for flag in $SMOKE_FLAGS; do
    if timeout 240 ./arora "$flag" >/dev/null 2>&1; then
        echo "check-coverage: smoke $flag PASS"
    else
        echo "check-coverage: smoke $flag exit=$? (tolerated)"
    fi
done
if timeout 240 ./arora --download-smoke "file://$BUILD/README" >/dev/null 2>&1; then
    echo "check-coverage: smoke --download-smoke PASS"
else
    echo "check-coverage: smoke --download-smoke exit=$? (tolerated)"
fi

ls "$PROFDIR"/*.profraw >/dev/null 2>&1 || die "no profraw written"
"$LLVM_PROFDATA" merge -sparse -o "$BUILD/coverage.profdata" \
    "$PROFDIR"/*.profraw || die "llvm-profdata merge failed"

OBJECTS="-object ./arora"
for b in $(find autotests -type f -perm -u+x -name 'tst_*' ! -name '*.*'); do
    OBJECTS="$OBJECTS -object $b"
done

# llvm-cov positional args are extra object files, not source filters,
# so take the full report and filter to src/ rows in post-processing.
# That also excludes generated moc/ui/rcc code and the tst_* drivers.
$LLVM_COV report $OBJECTS -instr-profile="$BUILD/coverage.profdata" \
    > "$BUILD/report-full.txt" || die "llvm-cov report failed"

read LINES_PCT REGIONS_PCT FUNCS_PCT <<EOF
$(python3 - "$BUILD" "$BUILD/report-full.txt" "$BUILD/report-src.txt" <<'PYEOF'
import sys
build, report, out = sys.argv[1], sys.argv[2], sys.argv[3]
prefix = build.lstrip("/") + "/src/"
# llvm-cov report columns after the filename:
# regions reg_missed reg% funcs fn_missed fn% lines ln_missed ln% br br_missed br%
IDX = [1, 2, 4, 5, 7, 8, 10, 11]
tot = [0] * 8
rows = []
with open(report) as f:
    for line in f:
        parts = line.split()
        if len(parts) != 13 or not parts[0].startswith(prefix):
            continue
        # generated code lives in src/.moc/.ui/.rcc — drop dot-dir rows
        if parts[0][len(prefix):].startswith("."):
            continue
        for i, idx in enumerate(IDX):
            t = parts[idx]
            tot[i] += int(t) if t != "-" else 0
        # re-root the filename at src/ but keep llvm-cov's alignment
        rows.append("src/" + parts[0][len(prefix):] + line[len(parts[0]):].rstrip())

def pct(covered, total):
    return 100.0 * covered / total if total else 0.0

with open(out, "w") as f:
    for r in sorted(rows):
        f.write(r + "\n")
    f.write(f"TOTAL regions {pct(tot[0]-tot[1], tot[0]):.2f}% "
            f"({tot[0]-tot[1]}/{tot[0]})  functions {pct(tot[2]-tot[3], tot[2]):.2f}% "
            f"({tot[2]-tot[3]}/{tot[2]})  lines {pct(tot[4]-tot[5], tot[4]):.2f}% "
            f"({tot[4]-tot[5]}/{tot[4]})\n")

print(f"{pct(tot[4]-tot[5], tot[4]):.2f} "
      f"{pct(tot[0]-tot[1], tot[0]):.2f} "
      f"{pct(tot[2]-tot[3], tot[2]):.2f}")
PYEOF
)
EOF
[ -n "$LINES_PCT" ] || die "could not parse coverage summary"

cp "$BUILD/report-src.txt" "$BUILD/report-rel.txt"

{
    echo "# Coverage baseline (COV01)"
    echo
    echo "Generated: $(date -u '+%Y-%m-%d %H:%M UTC') on commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "Toolchain: $(clang++ --version | head -1) + $LLVM_COV"
    echo
    echo "## Reproduce"
    echo
    echo '    source .devin/qt-env.sh && make check-coverage'
    echo
    echo 'Instrumented copy of the tree is built with `qmake -spec'
    echo 'linux-clang CONFIG+=coverage`, the autotest suite and the app'
    echo '`--*-smoke` flags run under `LLVM_PROFILE_FILE`, and llvm-cov'
    echo 'reports over `src/` only (generated moc/ui/rcc and test drivers'
    echo 'excluded). `ARORA_COVERAGE_MIN=NN` turns it into a gate.'
    echo
    echo "## Totals (src/ only)"
    echo
    echo "| Metric | Coverage |"
    echo "|--------|----------|"
    echo "| Lines | ${LINES_PCT}% |"
    echo "| Regions | ${REGIONS_PCT}% |"
    echo "| Functions | ${FUNCS_PCT}% |"
    echo
    echo "## Per-file"
    echo
    echo '```'
    cat "$BUILD/report-rel.txt"
    echo '```'
} > "$REPORT_MD"

echo "check-coverage: src/ line coverage = ${LINES_PCT}% (gate ${MIN}%)"
echo "check-coverage: baseline written to ${REPORT_MD#$ROOT/}"

if [ "$KEEP" != "1" ]; then
    rm -rf "$BUILD"
else
    echo "check-coverage: keeping $BUILD"
fi

python3 - "$LINES_PCT" "$MIN" <<'PYEOF' || exit 1
import sys
sys.exit(0 if float(sys.argv[1]) >= float(sys.argv[2]) else 1)
PYEOF
exit 0
