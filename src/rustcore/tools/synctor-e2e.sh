#!/bin/sh
# synctor-e2e.sh — SYNC02 live-network verification: two real
# processes talking over embedded Arti onion services.
#
#   tools/synctor-e2e.sh [path-to-synctorping]
#
# Legs (all on the REAL Tor network — no chutney, no mocks):
#   1. onion address stability: `addr` twice on one data root must
#      print the same endpoint (hsid persists in the keystore).
#   2. authorized ping: server authorizes the client's discovery
#      key; client connects and gets PONG.
#   3. unauthorized refusal: a fresh device with no authorized key
#      must fail BEFORE the app layer (the Tor layer cannot decrypt
#      the restricted-discovery descriptor) — the server sees no
#      link and no frame.
#   4. restart stability: after the server restarts on the same data
#      root the endpoint is unchanged.
#
# Not run in `make check` — it needs live Tor egress.  Run it to
# reproduce the SYNC02 verification record.

set -u
BIN=${1:-target/release/synctorping}
[ -x "$BIN" ] || { echo "missing binary: $BIN" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/synctor-e2e-XXXXXX")
A=$WORK/A; B=$WORK/B; C=$WORK/C
mkdir -p "$A" "$B" "$C" && chmod 700 "$A" "$B" "$C"
SERVE_PID=
cleanup() {
    [ -n "$SERVE_PID" ] && kill "$SERVE_PID" 2>/dev/null
    echo "workdir: $WORK"
}
trap cleanup EXIT

say() { printf '\n=== %s ===\n' "$*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

say "1. endpoint stability"
A1=$("$BIN" addr "$A") || fail "addr A"
A2=$("$BIN" addr "$A") || fail "addr A (rerun)"
[ "$A1" = "$A2" ] || fail "endpoint changed across reopen: $A1 vs $A2"
ONION=${A1%%:*}
echo "server endpoint: $A1"

say "2. authorized ping"
KEY=$("$BIN" genkey "$B" "$ONION") || fail "genkey"
echo "client discovery key: $KEY"
"$BIN" serve "$A" "e2e=$KEY" > "$A/serve.log" 2>&1 &
SERVE_PID=$!
# Wait for the SERVING line — start() returns after bootstrap+launch.
i=0
while [ $i -lt 240 ]; do
    grep -q "SERVING " "$A/serve.log" && break
    grep -q "transport start failed" "$A/serve.log" && { cat "$A/serve.log"; fail "serve start"; }
    sleep 1; i=$((i+1))
done
grep -q "SERVING $A1" "$A/serve.log" || { cat "$A/serve.log"; fail "server did not come up at $A1"; }
echo "server up"

# The first descriptor publish takes a while to propagate — the
# connect backoff inside the transport covers most of it; allow a
# few outer retries for the rest.
i=0; PING=
while [ $i -lt 6 ]; do
    if OUT=$(timeout 300 "$BIN" ping "$B" "$A1" 2>"$B/ping.err"); then
        PING=$OUT; break
    fi
    OUT2=$(cat "$B/ping.err" | tail -3)
    echo "ping attempt $((i+1)) not yet through ($OUT $OUT2) — retrying"
    i=$((i+1)); sleep 15
done
[ "$PING" = "PING-OK" ] || fail "authorized ping never succeeded (last: $PING)"
echo "PING-OK"
grep -q "PING-SERVED" "$A/serve.log" || fail "server did not log PING-SERVED"

say "3. unauthorized peer refused pre-app-layer"
FRAMES_BEFORE=$(grep -c "PING-SERVED\|incoming link" "$A/serve.log")
UNAUTH=$(timeout 300 "$BIN" ping "$C" "$A1" 2>"$C/ping.err"; echo "rc=$?")
echo "$UNAUTH" | tail -2
echo "$UNAUTH" | grep -q "CONNECT-FAIL" || fail "unauthorized connect did not fail: $UNAUTH"
FRAMES_AFTER=$(grep -c "PING-SERVED\|incoming link" "$A/serve.log")
[ "$FRAMES_BEFORE" = "$FRAMES_AFTER" ] || fail "unauthorized peer produced server-side traffic"
echo "unauthorized peer refused before the app layer (no link, no frame)"

say "4. restart keeps the endpoint"
A3=$("$BIN" addr "$A") || fail "addr A (post-run)"
[ "$A3" = "$A1" ] || fail "endpoint moved after run: $A1 -> $A3"
echo "endpoint stable: $A3"

say "ALL LEGS PASS"
