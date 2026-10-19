# Source this before building/running anything against the user-local
# WPE WebKit prefix (WK00 acquisition — see ~/wpe/ACQUIRED.md).
#   source .devin/webkit-env.sh
#
# What is installed (~/wpe, Debian sid .debs via dpkg -x, NO sudo):
#   libwpewebkit-2.0-1 + -dev 2.54.1-1  (libWPEWebKit-2.0.so, headers,
#     wpe-webkit-2.0.pc, WPEWebProcess/WPENetworkProcess/WPEGPUProcess)
#   wpe-platform-2.0 .pc + headers     (incl. headless platform)
#   libwpe-1.0-1 + -dev 1.16.3-2, libsoup-3.0-dev 3.8.0-3,
#   libsysprof-capture-4-dev, plus the sid runtime deps the host lacks
#   (icu78/jpeg62/xml2-16/jxl/avif/enchant/flite/webp/gstreamer/...).
#   glibc/ — Debian sid libc6 2.43-7 (host glibc 2.39 is too old).
#   compat/libwpe-host-compat.so — shim exporting g_sort_array
#     (GLib 2.82 API, absent from host glib 2.80).
#
# API level: WPEPlatform >= 2.54 STABLE — WPEDisplay/WPEToplevel/WPEView,
# headless platform, WPEBuffer + wpe_buffer_import_to_pixels. This is the
# modern API the WEBKIT-STUDY targets; NOT legacy libwpe/WPEBackend-fdo.
#
# glibc rule (ACQUIRED.md): the sid .so set floors at GLIBC_2.42/2.43, so
# any process loading libWPEWebKit must run under the sid ld.so. Binaries
# we link need (verified by spikes/webkit/probe/build-probe.sh):
#   -Wl,--dynamic-linker,$ARKTOS_WPE_DYNAMIC_LINKER
#   -Wl,--disable-new-dtags -Wl,-rpath,$ARKTOS_WPE_LIBDIR:$ARKTOS_WPE_GLIBC
#      (DT_RPATH is transitive — the WPE lib's OWN deps find sid libc/libm
#       and ~/wpe runtime deps; RUNPATH is not transitive and the host's
#       older libm then fails the GLIBC_2.4x version check)
#   -Wl,--no-as-needed $ARKTOS_WPE_COMPAT -Wl,--as-needed
#      (gcc's spec defaults --as-needed, which drops the shim; retained as
#       DT_NEEDED it satisfies libgstreamer's g_sort_array shlib check)
#   $ARKTOS_WPE_GLIBC/libc.so.6 $ARKTOS_WPE_GLIBC/libm.so.6 explicitly
#      on the link line for the GLIBC_2.4x versioned refs.
# The WPE subprocess binaries were already patchelf'd and run standalone.
# glibc/ must NEVER go in LD_LIBRARY_PATH — host-interp helpers (bwrap)
# would pick the foreign libc; keep it interp/rpath-scoped.

export ARKTOS_WPE_PREFIX="$HOME/wpe"
export ARKTOS_WPE_LIBDIR="$ARKTOS_WPE_PREFIX/usr/lib/x86_64-linux-gnu"
export ARKTOS_WPE_GLIBC="$ARKTOS_WPE_PREFIX/glibc/usr/lib/x86_64-linux-gnu"
export ARKTOS_WPE_DYNAMIC_LINKER="$ARKTOS_WPE_GLIBC/ld-linux-x86-64.so.2"
export ARKTOS_WPE_COMPAT="$ARKTOS_WPE_PREFIX/compat/libwpe-host-compat.so"

export PKG_CONFIG_PATH="$ARKTOS_WPE_LIBDIR/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
export C_INCLUDE_PATH="$ARKTOS_WPE_PREFIX/usr/include${C_INCLUDE_PATH:+:$C_INCLUDE_PATH}"

# Runtime: wpe libdir + compat dir (shim is dlopen'd via LD_PRELOAD but
# keeping the dir visible costs nothing). NOT the glibc dir — see above.
export LD_LIBRARY_PATH="$ARKTOS_WPE_LIBDIR:$ARKTOS_WPE_PREFIX/compat${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Baked into binaries linked under this env: old-style RPATH covers
# transitive deps of libWPEWebKit (sid runtime deps + sid glibc).
export LD_RUN_PATH="$ARKTOS_WPE_LIBDIR:$ARKTOS_WPE_GLIBC${LD_RUN_PATH:+:$LD_RUN_PATH}"

# g_sort_array shim — only DT_NEEDED is libc, safe to preload anywhere
# (host-glib processes simply resolve the forwarder to their own glib).
export LD_PRELOAD="$ARKTOS_WPE_COMPAT${LD_PRELOAD:+:$LD_PRELOAD}"
