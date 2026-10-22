#!/bin/sh
# CEF01 probe build — builds libcef_dll_wrapper (if needed) then the
# qmake probe. Requires: source .devin/qt-env.sh; CEF_ROOT set to the
# extracted cef_binary_*_linux64 standard distribution.
#   ./build-probe.sh
set -eu
cd "$(dirname "$0")"
: "${CEF_ROOT:?CEF_ROOT unset — see spikes/cef/README.md}"
CMAKE="${CMAKE:-cmake}"
if ! command -v "$CMAKE" >/dev/null 2>&1; then
    CMAKE=/home/szefi/cef-cache/cmake/bin/cmake
fi
if [ ! -f "$CEF_ROOT/build/libcef_dll_wrapper/libcef_dll_wrapper.a" ]; then
    echo "== building libcef_dll_wrapper =="
    "$CMAKE" -S "$CEF_ROOT" -B "$CEF_ROOT/build" -DCMAKE_BUILD_TYPE=Release
    "$CMAKE" --build "$CEF_ROOT/build" --target libcef_dll_wrapper -j4
fi
# The standard dist ships binaries in Release/ and resources in
# Resources/, but Chromium resolves icudtl.dat/*.pak relative to
# libcef.so's own dir — the distro's test targets merge them into one
# app dir. Do the same (symlinks keep the 2 GB tree single-copy).
for f in icudtl.dat resources.pak chrome_100_percent.pak \
         chrome_200_percent.pak locales; do
    [ -e "$CEF_ROOT/Release/$f" ] || \
        ln -s "../Resources/$f" "$CEF_ROOT/Release/$f"
done
echo "== building qcefprobe =="
qmake6 qcefprobe.pro
make -j4
