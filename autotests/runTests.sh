#!/bin/sh
# Runs every autotest binary found below this directory.
#
# A directory is part of the suite when its .pro is a non-subdirs
# project that its parent project actually lists in SUBDIRS — orphan
# trees are reported but not treated as failures.
cd "$(dirname "$0")" || exit 1

# The suite always runs headless, and Qt 6.12's WebEngine compositor
# traps (SIGTRAP) in the GPU path when the middle-click autoscroll
# overlay is exercised offscreen — force software compositing for every
# test process so `make check` stays green.
export QTWEBENGINE_CHROMIUM_FLAGS="--disable-gpu${QTWEBENGINE_CHROMIUM_FLAGS:+ $QTWEBENGINE_CHROMIUM_FLAGS}"

listed() {
    dir=$1
    parent=${dir%/*}
    name=${dir##*/}
    if [ "$parent" = "$dir" ] || [ "$parent" = "." ]; then
        pro=autotests.pro
    else
        pro=$(ls "$parent"/*.pro 2>/dev/null | head -n 1)
    fi
    [ -n "$pro" ] && grep -qw "$name" "$pro"
}

status=0
for pro in */*.pro */*/*.pro; do
    [ -f "$pro" ] || continue
    grep -q "^TEMPLATE *= *subdirs" "$pro" && continue
    dir=${pro%/*}
    name=${pro##*/}
    name=${name%.pro}
    if ! listed "$dir"; then
        printf "=== %s === (skipped: not in build tree)\n" "$dir"
        continue
    fi
    bin=
    for candidate in "$dir/tst_$name" "$dir/$name"; do
        if [ -x "$candidate" ] && [ ! -d "$candidate" ]; then
            bin=$candidate
            break
        fi
    done
    if [ -z "$bin" ]; then
        printf "%s: test binary is not compiled\n" "$dir"
        status=1
        continue
    fi
    printf "=== %s ===\n" "$dir"
    (cd "$dir" && "./${bin##*/}" -silent) || status=1
    printf "\n"
done
exit $status
