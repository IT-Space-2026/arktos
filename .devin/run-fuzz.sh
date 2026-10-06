#!/bin/sh
# FUZZ01: build + run the libFuzzer harnesses for the hostile-input
# parsers.
#
#   .devin/run-fuzz.sh [seconds-per-target]
#
# Builds every fuzz/<target>/*.pro in a scratch dir with clang
# (-fsanitize=fuzzer,address,undefined) — gcc has no libFuzzer — then
# runs each binary over its committed seed corpus + dictionary.
# New coverage units are merged back into fuzz/corpus/<target>/ and
# crash artifacts land in fuzz/crashes/<target>/ (commit both).
#
# Env knobs:
#   FUZZ_SECONDS      per-target budget   (default 60; arg 1 wins)
#   ARORA_FUZZ_BUILD  build dir           (default /tmp/arora-fuzz-build)
#   JOBS              make parallelism    (default 2)

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
. "$ROOT/.devin/qt-env.sh"

BUILD=${ARORA_FUZZ_BUILD:-/tmp/arora-fuzz-build}
JOBS=${JOBS:-2}
BUDGET=${1:-${FUZZ_SECONDS:-60}}

TARGETS="adblockrule cookiejarstate historyformat htmltoxbel opensearchreader securestore streamingutils xbel"

dict_for() {
    case $1 in
        adblockrule)      echo adblock ;;
        opensearchreader) echo xml ;;
        xbel)             echo xml ;;
        htmltoxbel)       echo html ;;
        *)                echo "" ;;
    esac
}

mkdir -p "$BUILD"
cd "$BUILD" || exit 1
echo "== building fuzz targets (clang, fuzzer+asan+ubsan) =="
qmake -spec linux-clang "$ROOT/fuzz/fuzz.pro" || exit 1
make -j"$JOBS" || exit 1

status=0
for t in $TARGETS; do
    bin="$BUILD/$t/fuzz_$t"
    corpus="$ROOT/fuzz/corpus/$t"
    crashes="$ROOT/fuzz/crashes/$t"
    mkdir -p "$corpus" "$crashes"
    dict=$(dict_for "$t")
    dictarg=
    [ -n "$dict" ] && dictarg="-dict=$ROOT/fuzz/dict/$dict.dict"

    echo "== fuzz_$t ($BUDGET s) =="
    # shellcheck disable=SC2086
    "$bin" $dictarg -max_total_time="$BUDGET" -max_len=262144 \
        -artifact_prefix="$crashes/" -print_final_stats=1 \
        "$corpus" || status=1

    # Shrink the corpus back to its coverage-minimal set so the
    # committed corpus doesn't grow unboundedly across runs.
    minimized="$corpus.min"
    rm -rf "$minimized"
    mkdir -p "$minimized"
    "$bin" -merge=1 "$minimized" "$corpus" >/dev/null 2>&1 \
        && rm -rf "$corpus" && mv "$minimized" "$corpus" \
        || rm -rf "$minimized"
done

if [ "$status" -eq 0 ]; then
    echo "== all fuzz targets clean =="
else
    echo "== findings in $ROOT/fuzz/crashes/ =="
fi
exit $status
