#!/bin/sh
# WK02 harness build — qmake + make against the WK00 ~/wpe prefix.
#   spikes/webkit/wpe-harness/build-harness.sh
# The .pro consumes ARKTOS_WPE_* for the sid-glibc link recipe
# (see ~/wpe/ACQUIRED.md + .devin/webkit-env.sh).
set -e
cd "$(dirname "$0")"
. ../../../.devin/webkit-env.sh
. ../../../.devin/qt-env.sh

qmake wpe-harness.pro
make -j2

echo "== ldd sanity (unresolved would print here) =="
ldd ./wpe-harness | grep 'not found' && exit 1 || true
echo "built ./wpe-harness"
