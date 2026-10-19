#!/bin/sh
# WK04 input+eval proof — a synthetic move+down+up pointer triple on the
# fixture.html link must navigate to fixture2.html (asserted via the
# WebKitWebView "uri" property), a Return keydown/keyup pair on the
# focused back-link must navigate home, and evaluate_javascript must
# round-trip typed JSCValues (number + string). Prints a PASS/FAIL line
# per leg and exits nonzero on any leg failure.
#   spikes/webkit/wpe-harness/run-input.sh [--no-sandbox]
set -u
cd "$(dirname "$0")"
. ../../../.devin/webkit-env.sh
. ../../../.devin/qt-env.sh   # QT_QPA_PLATFORM=offscreen

export WEBKIT_INJECTED_BUNDLE_PATH="$ARKTOS_WPE_LIBDIR/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so"
# Same raster contract as run-render.sh — buffers land on the widget.
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
# Keep WPE state out of the user's real XDG dirs.
export XDG_CACHE_HOME="$PWD/.xdg-cache" XDG_DATA_HOME="$PWD/.xdg-data" \
       XDG_CONFIG_HOME="$PWD/.xdg-config" GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

if [ "${1:-}" = "--no-sandbox" ]; then
    export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
    shift
fi

timeout 90 ./wpe-harness --input --timeout=45
rc=$?
[ "$rc" -eq 0 ] && echo "INPUT+EVAL PASS" || echo "INPUT+EVAL FAIL"
exit $rc
