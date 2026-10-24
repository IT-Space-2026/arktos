#!/bin/sh
# CEF01 render proof — two legs:
#   leg 1 (OSR): QT_QPA_PLATFORM=offscreen + windowless rendering,
#        OnPaint -> QImage -> PNG witness analysis. The Wayland-safe
#        embed path; always Alloy-style per the CEF header contract.
#   leg 2 (windowed): xvfb-run + SetAsChild(winId) reparent with the
#        Chrome runtime style — the extension-capable shape. Proof =
#        load end + title + root-window grab PNG witness analysis.
#   ./run-render.sh
set -u
cd "$(dirname "$0")"
. ../../../.devin/qt-env.sh
: "${CEF_ROOT:?CEF_ROOT unset — see spikes/cef/README.md}"
# Standard dist splits binaries (Release/) from resources (Resources/).
export CEF_RESOURCES_DIR="$CEF_ROOT/Resources"
# MTML: CEF runs its own UI thread — verified config; STML is kept as
# the CEF_MTL-unset code path for future comparison only.
export CEF_MTL=1
unset WAYLAND_DISPLAY WAYLAND_SOCKET XDG_SESSION_TYPE

FIXTURE="file://$PWD/fixture.html"
rc=0

echo "==== leg 1: OSR / windowless_rendering (QT_QPA_PLATFORM=offscreen) ===="
CEF_OZONE=headless QT_QPA_PLATFORM=offscreen \
timeout 60 ./qcefprobe --mode=osr \
    --url="$FIXTURE" --shot="$PWD/cef-osr.png" \
    --profile-dir=/tmp/cefprobe-osr --timeout=25
l1=$?
[ "$l1" -eq 0 ] && echo "OSR PASS" || { echo "OSR FAIL"; rc=1; }

echo "==== leg 2: windowed SetAsChild + CHROME runtime style (xvfb) ===="
CEF_OZONE=x11 QT_QPA_PLATFORM=xcb \
timeout 60 xvfb-run -a -s "-screen 0 800x600x24" \
    ./qcefprobe --mode=windowed --rtstyle=chrome \
    --url="$FIXTURE" --shot="$PWD/cef-windowed-chrome.png" \
    --profile-dir=/tmp/cefprobe-win --timeout=25
l2=$?
[ "$l2" -eq 0 ] && echo "WINDOWED-CHROME PASS" \
               || { echo "WINDOWED-CHROME FAIL"; rc=1; }

echo "==== leg 3: windowed SetAsChild + ALLOY style (advisory) ===="
CEF_OZONE=x11 QT_QPA_PLATFORM=xcb \
timeout 60 xvfb-run -a -s "-screen 0 800x600x24" \
    ./qcefprobe --mode=windowed --rtstyle=alloy \
    --url="$FIXTURE" --shot="$PWD/cef-windowed-alloy.png" \
    --profile-dir=/tmp/cefprobe-win2 --timeout=25
l3=$?
[ "$l3" -eq 0 ] && echo "WINDOWED-ALLOY PASS" \
               || echo "WINDOWED-ALLOY FAIL (advisory)"

[ "$rc" -eq 0 ] && echo "RENDER PROOF PASS" || echo "RENDER PROOF FAIL"
exit $rc
