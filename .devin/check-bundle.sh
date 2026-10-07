#!/usr/bin/env bash
# check-bundle.sh — PACK01 verification gate.
#
# 1. Builds the self-contained bundle via BuildProcess/bundle-linux.sh.
# 2. Proves self-containment: runs the bundle inside a bwrap sandbox where
#    $QTDIR is tmpfs-masked, with a scrubbed environment (env -i).
# 3. Runs --quit-after-load under the offscreen QPA.
# 4. Builds .devin/wayland-compositor.c (a minimal libwayland-server
#    compositor: wl_compositor/shm/subcompositor/seat/output/
#    data_device_manager + xdg_wm_base) into a scratch dir and runs the
#    bundled binary natively on Wayland — the user's real session is never
#    touched.
#
# Env overrides: QTDIR, ARORA_BUNDLE_DIR, ARORA_BUNDLE_KEEP=1 to keep the
# bundle for inspection.

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/.." && pwd)"
QTDIR="${QTDIR:-$HOME/Qt/6.11.3/gcc_64}"
BUNDLE="${ARORA_BUNDLE_DIR:-$(mktemp -d /tmp/arora-bundle.XXXXXX)/Arora}"
SCRATCH="$(mktemp -d /tmp/arora-check-bundle.XXXXXX)"
COMP_PID=""

cleanup() {
    [ -n "$COMP_PID" ] && kill "$COMP_PID" 2>/dev/null || true
    if [ -z "${ARORA_BUNDLE_KEEP:-}" ] && [ -z "${ARORA_BUNDLE_DIR:-}" ]; then
        rm -rf "$(dirname "$BUNDLE")"
    fi
    rm -rf "$SCRATCH"
}
trap cleanup EXIT

fail() { echo "check-bundle: FAIL: $*" >&2; exit 1; }
pass() { echo "check-bundle: PASS: $*"; }

echo "check-bundle: bundling into $BUNDLE"
"$SRCROOT/BuildProcess/bundle-linux.sh" "$BUNDLE"
[ -x "$BUNDLE/arora" ] || fail "no launcher in $BUNDLE"
[ -x "$BUNDLE/bin/arora" ] || fail "no binary in $BUNDLE/bin"
[ -f "$BUNDLE/bin/qt.conf" ] || fail "no qt.conf"

# -- 1. ldd: nothing resolves to $QTDIR or fails entirely -------------------
missing=$(bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
    env -i HOME="$SCRATCH/home" PATH=/usr/bin:/bin \
    LD_LIBRARY_PATH="$BUNDLE/lib" \
    ldd "$BUNDLE/bin/arora" 2>&1 | grep -cE 'not found' || true)
[ "$missing" -eq 0 ] || fail "$missing unresolved libs with $QTDIR hidden"
leaked=$(bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
    env -i HOME="$SCRATCH/home" PATH=/usr/bin:/bin \
    LD_LIBRARY_PATH="$BUNDLE/lib" \
    ldd "$BUNDLE/bin/arora" 2>&1 | grep -c "$QTDIR" || true)
[ "$leaked" -eq 0 ] || fail "$leaked libs still resolve into $QTDIR"
pass "self-containment: 0 missing libs, 0 references to $QTDIR"

sandbox() {
    bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
        env -i HOME="$SCRATCH/home" PATH=/usr/bin:/bin "$@"
}

# -- 2. offscreen smoke ------------------------------------------------------
out=$(sandbox QT_QPA_PLATFORM=offscreen \
      "$BUNDLE/arora" --quit-after-load 2>&1) || fail "offscreen run: $out"
echo "$out" | grep -q loadFinished || fail "offscreen: no loadFinished"
pass "offscreen: --quit-after-load exit 0"

# -- 3. native Wayland smoke -------------------------------------------------
mkdir -p "$SCRATCH/xdgrun"
chmod 700 "$SCRATCH/xdgrun"
XDG_XML=/usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
[ -f "$XDG_XML" ] || fail "xdg-shell.xml not found (wayland-protocols missing)"
wayland-scanner server-header "$XDG_XML" "$SCRATCH/xdg-shell-server.h"
wayland-scanner private-code "$XDG_XML" "$SCRATCH/xdg-shell-code.c"
cc -O1 -I"$SCRATCH" -o "$SCRATCH/wayland-compositor" \
    "$SRCROOT/.devin/wayland-compositor.c" "$SCRATCH/xdg-shell-code.c" \
    $(pkg-config --cflags --libs wayland-server) \
    || fail "compositor build"

XDG_RUNTIME_DIR="$SCRATCH/xdgrun" "$SCRATCH/wayland-compositor" \
    > "$SCRATCH/comp.log" 2>&1 &
COMP_PID=$!
sleep 0.5
kill -0 "$COMP_PID" || { cat "$SCRATCH/comp.log"; fail "compositor exited"; }

out=$(sandbox \
      XDG_RUNTIME_DIR="$SCRATCH/xdgrun" WAYLAND_DISPLAY=arora-smoke \
      QT_QPA_PLATFORM=wayland QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
      "$BUNDLE/arora" --quit-after-load 2>&1) || fail "wayland run: $out"
echo "$out" | grep -q loadFinished || fail "wayland: no loadFinished"
kill "$COMP_PID" 2>/dev/null || true
COMP_PID=""
pass "wayland: --quit-after-load exit 0 against bundled libqwayland"

echo "check-bundle: ALL CHECKS PASSED"
