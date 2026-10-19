#!/bin/sh
# WK01 probe build+run — link/run proof against the WK00 ~/wpe prefix.
#   spikes/webkit/probe/build-probe.sh
# Uses the glibc recipe from ~/wpe/ACQUIRED.md: sid ld.so as interp,
# rpath over wpe+glibc libdirs, explicit sid libc.so.6/libm.so.6 for the
# GLIBC_2.4x versioned refs libWPEWebKit carries.
set -e
cd "$(dirname "$0")"
. ../../../.devin/webkit-env.sh

echo "== pkg-config =="
pkg-config --modversion wpe-webkit-2.0

# Notes on the link recipe (hard-won — keep in sync with webkit-env.sh):
#  * --no-as-needed around the compat shim: gcc's spec passes --as-needed,
#    which would drop it (the probe refs no shim symbol directly) — and
#    ld DOES consult command-line shared libs for the shlib-undefined
#    check, so the retained shim is what satisfies libgstreamer's
#    g_sort_array ref.
#  * --disable-new-dtags: -rpath must emit DT_RPATH (transitive), not
#    RUNPATH — libWPEWebKit's own deps (sid libm/libc + ~/wpe runtime
#    deps) resolve through the binary's RPATH. With RUNPATH the host's
#    older libm loads and the GLIBC_2.4x version check fails.
cc -O2 -o webkit-probe webkit-probe.c \
    $(pkg-config --cflags wpe-webkit-2.0) \
    $(pkg-config --libs wpe-webkit-2.0) \
    -Wl,--no-as-needed "$ARKTOS_WPE_COMPAT" -Wl,--as-needed \
    -Wl,--dynamic-linker,"$ARKTOS_WPE_DYNAMIC_LINKER" \
    -Wl,--disable-new-dtags \
    -Wl,-rpath,"$ARKTOS_WPE_LIBDIR:$ARKTOS_WPE_GLIBC" \
    "$ARKTOS_WPE_GLIBC/libc.so.6" "$ARKTOS_WPE_GLIBC/libm.so.6"

echo "== run =="
./webkit-probe
