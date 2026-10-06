#!/bin/sh
# check-sanitize.sh — HARD01 ASan+UBSan harness
#
# Builds an instrumented copy of the tree (same throwaway-dir pattern
# as check-coverage.sh so instrumented objects never pollute the
# in-tree gcc build):
#   * qmake -spec linux-clang CONFIG+=sanitize — see sanitize.pri
#   * runs the full autotest suite plus the app's --*-smoke flags
#     under QT_QPA_PLATFORM=offscreen with an isolated HOME
#   * scans every log for sanitizer reports (ASan "ERROR:"/SUMMARY
#     lines, UBSan "runtime error:" lines, LSan if enabled) and
#     dedupes them into .devin/SANITIZER.md
#
# LeakSanitizer is deliberately OFF (detect_leaks=0): the leak hunt is
# MEM01's job, which reuses this harness with ARORA_SANITIZE_LEAKS=1.
#
# Env:
#   ARORA_SANITIZE_LEAKS=1   enable LSan (default off — MEM01 flips it)
#   ARORA_SANITIZE_STRICT=1  build CONFIG+=sanitize-strict (UBSan
#                            findings become fatal instead of logged)
#   ARORA_SANITIZE_JOBS=N    parallel make jobs (default: nproc, max 4)
#   ARORA_SANITIZE_KEEP=1    keep the instrumented build tree
#
# Exit status: 0 when the suite ran with zero sanitizer reports, 1 on
# build failure or any surviving report (the gate).
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPORT_MD="$ROOT/.devin/SANITIZER.md"

JOBS=${ARORA_SANITIZE_JOBS:-$(nproc 2>/dev/null || echo 2)}
[ "$JOBS" -gt 4 ] && JOBS=4
KEEP=${ARORA_SANITIZE_KEEP:-0}
LEAKS=${ARORA_SANITIZE_LEAKS:-0}
STRICT=${ARORA_SANITIZE_STRICT:-0}

die() { echo "check-sanitize: $*" >&2; exit 1; }

command -v clang++ >/dev/null 2>&1 || die "clang++ not found"
command -v git >/dev/null 2>&1 || die "git not found"

CONFIG=sanitize
[ "$STRICT" = "1" ] && CONFIG=sanitize-strict

BUILD=$(mktemp -d /tmp/arora-san.XXXXXX)
echo "check-sanitize: build tree $BUILD (CONFIG+=$CONFIG)"

cd "$ROOT" || die "cannot cd to $ROOT"
git ls-files -z -c -o --exclude-standard | tar --null -T - -cf - \
    | tar -x -C "$BUILD" || die "tree copy failed"

cd "$BUILD" || die "cannot cd to $BUILD"
# shellcheck disable=SC1090
. "$ROOT/.devin/qt-env.sh"

QMAKE=$(command -v qmake6 || command -v qmake) || die "qmake not found"

echo "check-sanitize: qmake ($("$QMAKE" -query QT_VERSION), linux-clang) + make -j$JOBS"
"$QMAKE" -spec linux-clang "CONFIG+=$CONFIG" arora.pro || die "qmake failed"
make -j"$JOBS" sub-src sub-autotests || die "build failed"

# Isolate HOME: tests/smokes write the real app profile dirs otherwise.
HOME="$BUILD/.home"
export HOME
mkdir -p "$HOME"

LOGDIR="$BUILD/sanlogs"
mkdir -p "$LOGDIR"

# ASan: abort per-binary on a memory error so the report lands in that
# test's log; leaks are MEM01's job.  The quarantine is capped (64 MiB
# vs the 256 MiB default) because the instrumented suite plus
# QtWebEngineProcess children can otherwise push the devin-run memory
# budget over its limit.  UBSan stays recoverable (unless
# sanitize-strict) so one report doesn't hide the rest of a test run.
export ASAN_OPTIONS="detect_leaks=$LEAKS:abort_on_error=1:print_stacktrace=1:handle_segv=1:quarantine_size_mb=64:thread_local_quarantine_size_kb=64"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=0:log_path=$LOGDIR/ubsan"
export QT_QPA_PLATFORM=offscreen

echo "check-sanitize: running autotest suite (offscreen, ASan+UBSan)"
(cd autotests && ./runTests.sh) >"$LOGDIR/suite.log" 2>&1
TESTS_RC=$?
[ "$TESTS_RC" -eq 0 ] || echo "check-sanitize: WARNING: autotests exited $TESTS_RC"

SMOKE_FLAGS="--quit-after-load --app-smoke --browser-smoke --nam-smoke
--cookie-smoke --history-smoke --bookmarks-smoke --search-smoke
--settings-smoke --autofill-smoke --find-smoke --source-smoke
--adblock-smoke --adblock-list-smoke --adblock-rust-smoke
--extension-smoke --ua-smoke"
for flag in $SMOKE_FLAGS; do
    name=$(echo "$flag" | tr -d -)
    timeout 240 ./arora "$flag" >"$LOGDIR/smoke-$name.log" 2>&1
    rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "check-sanitize: smoke $flag PASS"
    else
        echo "check-sanitize: smoke $flag exit=$rc (tolerated)"
    fi
done
timeout 240 ./arora --download-smoke "file://$BUILD/README" \
    >"$LOGDIR/smoke-downloadsmoke.log" 2>&1 \
    && echo "check-sanitize: smoke --download-smoke PASS" \
    || echo "check-sanitize: smoke --download-smoke exit=$? (tolerated)"

# --- collect findings ------------------------------------------------
# ASan prints "ERROR: AddressSanitizer: <kind>" + "SUMMARY:"; UBSan
# prints "file:line:col: runtime error: ..." (to stderr inline or, for
# reports mid-printf, to $LOGDIR/ubsan.<pid> via log_path); LSan prints
# "ERROR: LeakSanitizer".  UBSan dedupes by source location.
FINDINGS="$BUILD/findings.txt"
: > "$FINDINGS"
for log in "$LOGDIR"/*; do
    [ -f "$log" ] || continue
    grep -E "ERROR: (Address|Leak|Thread)Sanitizer|runtime error:|SUMMARY: (Address|Leak|Thread|UndefinedBehavior)Sanitizer|SEGV on unknown" \
        "$log" 2>/dev/null | sed "s|^|$(basename "$log"): |" >> "$FINDINGS" || true
done
sort -u -o "$FINDINGS" "$FINDINGS"

COUNT=$(grep -c . "$FINDINGS" 2>/dev/null || echo 0)

{
    echo "# Sanitizer sweep (HARD01)"
    echo
    echo "Generated: $(date -u '+%Y-%m-%d %H:%M UTC') on commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "Toolchain: $(clang++ --version | head -1), qmake -spec linux-clang CONFIG+=$CONFIG"
    echo
    echo "## Reproduce"
    echo
    echo '    source .devin/qt-env.sh && make check-sanitize'
    echo
    echo 'Instrumented copy of the tree is built in a throwaway dir with'
    echo '`-fsanitize=address,undefined`; the autotest suite and every'
    echo '`--*-smoke` flag run offscreen under an isolated HOME.'
    echo 'LeakSanitizer is off by default (MEM01); ARORA_SANITIZE_LEAKS=1'
    echo 'enables it, ARORA_SANITIZE_STRICT=1 makes UBSan findings fatal.'
    echo
    echo "## Result"
    echo
    echo "autotest suite exit: $TESTS_RC"
    echo "sanitizer reports: $COUNT"
    echo
    if [ "$COUNT" -gt 0 ]; then
        echo '```'
        cat "$FINDINGS"
        echo '```'
    else
        echo "No sanitizer reports."
    fi
} > "$REPORT_MD"

echo "check-sanitize: $COUNT deduped sanitizer report(s); suite rc=$TESTS_RC"
echo "check-sanitize: report written to ${REPORT_MD#$ROOT/}"

if [ "$KEEP" != "1" ]; then
    rm -rf "$BUILD"
else
    echo "check-sanitize: keeping $BUILD (logs in $LOGDIR)"
fi

[ "$COUNT" -eq 0 ] && [ "$TESTS_RC" -eq 0 ]
