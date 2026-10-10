#!/bin/sh
# check-warnings.sh — WRN01 zero-warning gate
#
# Builds a throwaway copy of the tree (same pattern as
# check-coverage.sh/check-static.sh so objects never pollute the
# in-tree build) and fails if the compiler emits ANY warning or error
# while compiling the shipping code (src/ + tools/).
#
# This gate deliberately configures the NO-RUST build
# (`CONFIG+=no-rust`): RDEF01 made the Rust crates the default code
# path, so this clean copy stays the gate that proves the tree still
# configures and compiles warning-free without any Rust toolchain —
# while the in-tree `make check` build exercises the Rust default.
#
# The autotest tree is deliberately not gated here: `make check`
# already rebuilds those TUs in-tree where warnings are visible in the
# build output, and gating the ~44x recompiled shared sources would
# only multiply identical diagnostics.
#
# Env:
#   ARORA_WARN_BUILD=DIR  reuse an existing warnings tree instead of
#                         copying the sources to a fresh mktemp dir —
#                         lets a killed run resume (objects already
#                         built are kept, the log is appended).
#                         Implies KEEP.
#   ARORA_WARN_JOBS=N     parallel make jobs (default: 2, capped at 4)
#   ARORA_WARN_KEEP=1     keep a fresh warnings build tree
#
# Exit status: 0 when the build produced zero warnings/errors, 1 when
# diagnostics were found (they are printed deduplicated), or when the
# build itself failed.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

JOBS=${ARORA_WARN_JOBS:-2}
case $JOBS in *[!0-9]*|'') JOBS=2;; esac
[ "$JOBS" -gt 4 ] && JOBS=4
KEEP=${ARORA_WARN_KEEP:-0}
RESUME=${ARORA_WARN_BUILD:-}

die() { echo "check-warnings: $*" >&2; exit 1; }

command -v g++ >/dev/null 2>&1 || die "g++ not found"
command -v git >/dev/null 2>&1 || die "git not found"

if [ -n "$RESUME" ]; then
    BUILD=$RESUME
    KEEP=1
    [ -d "$BUILD" ] || die "ARORA_WARN_BUILD dir $BUILD does not exist"
else
    BUILD=$(mktemp -d /tmp/arora-warn.XXXXXX)
    # Copy tracked + untracked-nonignored files only: the source tree
    # carries in-tree Makefiles/.obj/binaries that would confuse the
    # fresh build.
    cd "$ROOT" || die "cannot cd to $ROOT"
    git ls-files -z -c -o --exclude-standard | tar --null -T - -cf - \
        | tar -x -C "$BUILD" || die "tree copy failed"
fi
echo "check-warnings: build tree $BUILD"

cd "$BUILD" || die "cannot cd to $BUILD"
# shellcheck disable=SC1090
. "$ROOT/.devin/qt-env.sh"

QMAKE=$(command -v qmake6 || command -v qmake) || die "qmake not found"

LOG="$BUILD/warnings-build.log"
echo "check-warnings: qmake ($("$QMAKE" -query QT_VERSION), CONFIG+=no-rust) + make -j$JOBS (src + tools)"
"$QMAKE" "CONFIG+=no-rust" arora.pro >>"$LOG" 2>&1 || die "qmake failed (see $LOG)"
# One make invocation per subdir: tools/ shares src/'s OBJECTS_DIR and
# MOC_DIR (src.pri anchors them at $$PWD), and passing multiple subdir
# goals at once bypasses the CONFIG+=ordered serialization so two
# makes race on the same generated moc/object files.
for sub in sub-src sub-tools; do
    make -j"$JOBS" "$sub" >>"$LOG" 2>&1 \
        || die "build of $sub failed (see $LOG)"
done

# gcc/clang diagnostics look like  file:line:col: warning|error: ...
# uic/rcc/moc noise uses different phrasing; count only compiler
# diagnostics so the gate tracks the source baseline.
DIAGS=$(grep -E ':(warning|error):' "$LOG" | sort -u || true)
COUNT=$(printf '%s\n' "$DIAGS" | grep -c . || true)

if [ "$COUNT" -gt 0 ]; then
    echo "check-warnings: FAIL — $COUNT unique diagnostic(s):" >&2
    printf '%s\n' "$DIAGS" >&2
    echo "check-warnings: full log at $LOG" >&2
    exit 1
fi

echo "check-warnings: PASS — zero compiler warnings/errors in src/ + tools/"

if [ "$KEEP" = 0 ]; then
    rm -rf "$BUILD"
else
    echo "check-warnings: kept build tree $BUILD"
fi
exit 0
