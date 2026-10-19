#!/bin/sh
# WK03 render-path proof — loads a page, consumes WPEBuffers via
# "buffer-rendered" -> wpe_buffer_import_to_pixels -> QImage -> QWidget
# paintEvent, saves PNG evidence and a post-resize PNG.
#   spikes/webkit/wpe-harness/run-render.sh [--no-sandbox] [uri]
# Default uri: bundled render-fixture.html (deterministic witness
# colors) — THE GATE. Artifacts land next to this script as wk03-*.png.
# A second, advisory leg tries https://example.com when reachable
# (witness checks off; density/distinct/dominant gates still apply).
set -u
cd "$(dirname "$0")"
. ../../../.devin/webkit-env.sh
. ../../../.devin/qt-env.sh   # QT_QPA_PLATFORM=offscreen

export WEBKIT_INJECTED_BUNDLE_PATH="$ARKTOS_WPE_LIBDIR/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so"
# Headless/no-GPU sandbox: the web process must raster on CPU or it
# produces dmabuf/EGL buffers import_to_pixels cannot read back here.
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
# Keep WPE state out of the user's real XDG dirs.
export XDG_CACHE_HOME="$PWD/.xdg-cache" XDG_DATA_HOME="$PWD/.xdg-data" \
       XDG_CONFIG_HOME="$PWD/.xdg-config" GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"

if [ "${1:-}" = "--no-sandbox" ]; then
    export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
    shift
fi
URI="${1:-}"

timeout 90 ./wpe-harness --shot="$PWD/wk03-render.png" \
    --resize=640x480 --resize-shot="$PWD/wk03-resize.png" \
    --timeout=45 ${URI:+"$URI"}
rc=$?
echo "---- render-path rc=$rc ----"

# Advisory live leg — example.com over real HTTPS through the sandbox.
if [ -z "$URI" ]; then
    echo "==================== advisory: https://example.com ===================="
    timeout 90 ./wpe-harness --shot="$PWD/wk03-example.png" \
        --resize=640x480 --resize-shot="$PWD/wk03-example-resize.png" \
        --witness=none --timeout=30 https://example.com
    arc=$?
    [ "$arc" -eq 0 ] && echo "example.com advisory PASS" \
                     || echo "example.com advisory FAIL (non-fatal — no network?)"
fi

[ "$rc" -eq 0 ] && echo "RENDER PASS" || echo "RENDER FAIL"
exit $rc
