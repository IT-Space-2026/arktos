#!/bin/sh
# STALL01 regression harness — every page load must finish with
# adblock enabled.
#
# Reproduces the original failure conditions deterministically: a
# seeded file: subscription carries the uBO-syntax rules that used to
# hang every load:
#   *$script,redirect-rule=noopjs,from=...,to=...
#     — from=/to= were silently ignored, globalizing the rule into a
#       redirect-every-script filter whose arora-resource: target then
#       re-entered the same matcher and spun the redirect chain.
#   *$script,redirect-rule=noopjs  (deliberately global, legal syntax)
#     — still exercises the arora-resource: re-entry guard on every
#       script fetch of every test page.
#   $image,3p  /  *$3p,xhr
#     — 3p/1p spellings were ignored, dropping the party constraint.
#
# Pass: 3 real pages x 10 iterations, each --quit-after-load exits 0
# within 10 s.  Any timeout, nonzero exit or missing loadFinished is
# a failure.  Requires network access to the test sites.
#
# Usage:  .devin/stall01-repro.sh [iterations]

set -u
cd "$(dirname "$0")/.."
. ./.devin/qt-env.sh

ARORA=./arora
[ -x "$ARORA" ] || { echo "FAIL: ./arora not built" >&2; exit 2; }

ITERATIONS=${1:-10}
LOAD_TIMEOUT=10

HOME_DIR=$(mktemp -d /tmp/stall01-home.XXXXXX)
trap 'rm -rf "$HOME_DIR"' EXIT

LIST="$HOME_DIR/stall01-filters.txt"
cat > "$LIST" <<'EOF'
! STALL01 regression list — see .devin/stall01-repro.sh
*$script,redirect-rule=noopjs
*$script,redirect-rule=noopjs,from=dutchycorp.*,to=~sentry-cdn.com
*$script,3p,from=ovagames.com,to=~facebook.net|~fbcdn.net
$image,3p,denyallow=cdn77.org|gstatic.com,from=pussyspace.com|pussyspace.net
*$image,3p
||ads.example.com^
example.com##.never-matches-anything
EOF

mkdir -p "$HOME_DIR/.config/Arora" "$HOME_DIR/.local/share/Arora/Arora"
cat > "$HOME_DIR/.config/Arora/Arora.conf" <<EOF
[AdBlock]
enabled=true
remoteListsConsent=1
subscriptions="abp:subscribe?location=file://$LIST&title=STALL01"
EOF

URLS="https://en.wikipedia.org/ https://example.com/ https://news.ycombinator.com/"

fail=0
pass=0
i=1
while [ "$i" -le "$ITERATIONS" ]; do
    for url in $URLS; do
        out=$(HOME="$HOME_DIR" QT_QPA_PLATFORM=offscreen \
              LIBGL_ALWAYS_SOFTWARE=1 \
              timeout "$LOAD_TIMEOUT" "$ARORA" --quit-after-load "$url" \
              2>&1)
        rc=$?
        if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q 'loadFinished'; then
            pass=$((pass + 1))
            printf 'iter %2d ok   %s\n' "$i" "$url"
        else
            fail=$((fail + 1))
            printf 'iter %2d FAIL rc=%d %s\n' "$i" "$rc" "$url"
            printf '%s\n' "$out" | tail -5
        fi
    done
    i=$((i + 1))
done

total=$((pass + fail))
echo "STALL01: $pass/$total page loads finished within ${LOAD_TIMEOUT}s"
[ "$fail" -eq 0 ]
