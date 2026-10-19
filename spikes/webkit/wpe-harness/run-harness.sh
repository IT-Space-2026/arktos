#!/bin/sh
# WK02 harness run — exercises all three GMainContext pump models in
# order and prints the verdict table. Usage:
#   spikes/webkit/wpe-harness/run-harness.sh [--no-sandbox] [uri]
# --no-sandbox: set WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1 (fallback
# when the bwrap sandbox can't stand the view up in this environment).
set -u
cd "$(dirname "$0")"
. ../../../.devin/webkit-env.sh
. ../../../.devin/qt-env.sh   # QT_QPA_PLATFORM=offscreen

export WEBKIT_INJECTED_BUNDLE_PATH="$ARKTOS_WPE_LIBDIR/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so"
# Keep WPE state out of the user's real XDG dirs.
export XDG_CACHE_HOME="$PWD/.xdg-cache" XDG_DATA_HOME="$PWD/.xdg-data" \
       XDG_CONFIG_HOME="$PWD/.xdg-config" GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

if [ "${1:-}" = "--no-sandbox" ]; then
    export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
    shift
fi
URI="${1:-}"

overall=0
for mode in glib timer thread; do
    echo "==================== loop=$mode ===================="
    timeout 90 ./wpe-harness --loop="$mode" --timeout=30 ${URI:+"$URI"}
    rc=$?
    echo "---- loop=$mode rc=$rc ----"
    [ "$rc" -eq 0 ] || overall=1
done
echo "==================== verdict ===================="
[ "$overall" -eq 0 ] && echo "ALL MODELS PASS" || echo "some models failed — see per-loop lines above"
exit $overall
