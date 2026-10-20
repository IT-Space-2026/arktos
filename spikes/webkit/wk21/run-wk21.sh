#!/bin/sh
# WK21 stretch-audit runner — builds the shim + probe, raises the
# SOCKS5/HTTP fixture, runs the leak legs, prints the evidence.
#   spikes/webkit/wk21/run-wk21.sh
# Sandboxed-off (WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1) so the
# LD_PRELOAD shim reaches WPEWebProcess/WPENetworkProcess — the audit
# is meaningless if the children can't be observed.
set -u
cd "$(dirname "$0")"
. ../../../.devin/webkit-env.sh

echo "== build dns-shim =="
cc -O2 -fPIC -shared -o dns-shim.so dns-shim.c -ldl

echo "== build probe =="
pkg-config --modversion wpe-webkit-2.0
cc -O2 -o wk21-probe wk21-probe.c \
    $(pkg-config --cflags wpe-webkit-2.0) \
    $(pkg-config --libs wpe-webkit-2.0) \
    -Wl,--no-as-needed "$ARKTOS_WPE_COMPAT" -Wl,--as-needed \
    -Wl,--dynamic-linker,"$ARKTOS_WPE_DYNAMIC_LINKER" \
    -Wl,--disable-new-dtags \
    -Wl,-rpath,"$ARKTOS_WPE_LIBDIR:$ARKTOS_WPE_GLIBC" \
    "$ARKTOS_WPE_GLIBC/libc.so.6" "$ARKTOS_WPE_GLIBC/libm.so.6" \
    -ldl

echo "== ABI checks (headers + exported symbols) =="
HDR="$ARKTOS_WPE_PREFIX/usr/include/wpe-webkit-2.0/wpe"
SO="$ARKTOS_WPE_LIBDIR/libWPEWebKit-2.0.so.1"
echo "-- inspector API:"
nm -D "$SO" | grep -E " T webkit_.*inspector" || echo "  (none exported)"
ls "$HDR" | grep -i inspector || echo "  (no header)"
echo "-- extension manager/load API:"
nm -D "$SO" | grep -E " T webkit_" | grep -E "extension_manager|extension_context|_web_extensions_dir|web_extension_mode" \
    || echo "  (none exported)"
nm -D "$SO" | grep -E " T webkit_web_extension" | wc -l \
    | sed 's/^/  webkit_web_extension_* exported count: /'
echo "-- webrtc settings ABI:"
nm -D "$SO" | grep -E " T webkit_settings_.*webrtc" || echo "  (none)"
echo "-- network proxy ABI:"
nm -D "$SO" | grep -E " T webkit_network_.*proxy" || echo "  (none)"

echo "== fixture up =="
WORK="$PWD/.wk21-run"
rm -rf "$WORK"; mkdir -p "$WORK"
RUN="r$$"
export WK21_RUN="$RUN"
# fixture reads WK21_PF/WK21_IMG at request time — export before launch
export WK21_PF="wk21pf-page-$RUN.invalid"
export WK21_IMG="wk21img-$RUN.test"
python3 fixture.py "$WORK" 2>"$WORK/fixture.err" &
FIX=$!
sleep 0.4
. "$WORK/ports.env"
echo "socks=$SOCKS_PORT http=$HTTP_PORT"

export WK21_DNSLOG="$WORK/dns.log"
export WK21_SOCKS_URI="socks5://localhost:$SOCKS_PORT"
export WK21_SOCKS_PORT="$SOCKS_PORT"
export WK21_TARGET="http://wk21main-$RUN.test/"
export WK21_EXT="$PWD/ext-fixture"

: > "$WK21_DNSLOG"
export WEBKIT_INJECTED_BUNDLE_PATH="$ARKTOS_WPE_LIBDIR/wpe-webkit-2.0/injected-bundle/libWPEInjectedBundle.so"
export XDG_CACHE_HOME="$WORK/xdg-cache" XDG_DATA_HOME="$WORK/xdg-data" \
       XDG_CONFIG_HOME="$WORK/xdg-config" GSETTINGS_BACKEND=memory
mkdir -p "$XDG_CACHE_HOME" "$XDG_DATA_HOME" "$XDG_CONFIG_HOME"
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
export WEBKIT_SKIA_ENABLE_CPU_RENDERING=1
export LD_PRELOAD="$PWD/dns-shim.so${LD_PRELOAD:+:$LD_PRELOAD}"

echo "== probe run =="
timeout 180 ./wk21-probe "$WORK" 2>&1 | tee "$WORK/probe.log"
echo "---- probe rc=$? ----"

kill $FIX 2>/dev/null
wait $FIX 2>/dev/null

echo
echo "== shim log (RESOLVE = local lookup, CONNECT/SENDTO = peer) =="
cat "$WK21_DNSLOG" 2>/dev/null | sed 's/^/  /' || echo "  (empty)"
echo "== socks log =="
cat "$WORK/socks.log" 2>/dev/null | sed 's/^/  /' || echo "  (empty)"
echo "== http fixture log =="
cat "$WORK/http.log" 2>/dev/null | sed 's/^/  /' || echo "  (empty)"
echo "== fixture stderr =="
cat "$WORK/fixture.err" | sed 's/^/  /'

echo
echo "== leak tally =="
echo "RESOLVE of test names (local lookup of a proxied host = LEAK):"
grep -E "RESOLVE.*(wk21|\.invalid|\.test)" "$WK21_DNSLOG" \
    | grep -v wk21selfctl || echo "  none"
echo "non-loopback CONNECT/SENDTO (bypass candidates):"
grep -E "CONNECT|SENDTO|SENDMSG" "$WK21_DNSLOG" \
    | grep -vE "127\.0\.0\.1|::1" || echo "  none"
echo "DNS-to-53 or mDNS 5353 sends:"
grep -E ":(53|5353)\b" "$WK21_DNSLOG" || echo "  none"
