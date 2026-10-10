#!/bin/sh
# Runs every autotest binary found below this directory.
#
# A directory is part of the suite when its .pro is a non-subdirs
# project that its parent project actually lists in SUBDIRS — orphan
# trees are reported but not treated as failures.
cd "$(dirname "$0")" || exit 1

# Isolate HOME so tests can't read or pollute the user's real
# ~/.config/Arora (QSettings) or ~/.local/share/Arora (WebEngine
# profiles, container storage).  Without this the suite's result
# depends on whatever settings the live browser last synced — e.g. a
# real `urlloading/searchEngineFallback=false` broke tst_TabWidget's
# omnibox expectations and the adblock-disabled state flipped
# tst_ContainerManager — and test-side writes land in the real
# profile.  The XDG vars are overridden explicitly in case the caller
# exports them (they would otherwise bypass the HOME redirect).
ARORA_TEST_HOME=$(mktemp -d "${TMPDIR:-/tmp}/arora-testhome.XXXXXX")
trap 'rm -rf "$ARORA_TEST_HOME"' EXIT
HOME="$ARORA_TEST_HOME"
XDG_CONFIG_HOME="$HOME/.config"
XDG_DATA_HOME="$HOME/.local/share"
XDG_CACHE_HOME="$HOME/.cache"
export HOME XDG_CONFIG_HOME XDG_DATA_HOME XDG_CACHE_HOME
mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME"

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
