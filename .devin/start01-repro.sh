#!/bin/sh
# START01 regression harness — the qrc start page must always finish
# loading and get a working channel.
#
# Runs `arora --startpage-smoke` under an isolated HOME: fifty
# consecutive qrc:/startpage.html loads in one process with the
# adblock matcher armed by the custom subscription with the widened
# uBO rules that produced the original hang (*$script,redirect-rule
# =noopjs plus rules naming the page's own qrc subresources).  Each
# load must emit loadFinished within the smoke's per-load deadline
# AND resolve channel.objects.arora — proven by the page's own
# update() filling the title/search button plus an explicit
# searchUrl() round-trip — so both reported suspects (interceptor
# stall, urlChanged/registration ordering) are covered.
#
# Usage:  .devin/start01-repro.sh [runs]
# Pass:   every run exits 0 with the smoke's PASS line.

set -u
cd "$(dirname "$0")/.."
. ./.devin/qt-env.sh

ARORA=./arora
[ -x "$ARORA" ] || { echo "FAIL: ./arora not built" >&2; exit 2; }

RUNS=${1:-1}

HOME_DIR=$(mktemp -d /tmp/start01-home.XXXXXX)
trap 'rm -rf "$HOME_DIR"' EXIT

fail=0
pass=0
i=1
while [ "$i" -le "$RUNS" ]; do
    out=$(HOME="$HOME_DIR" QT_QPA_PLATFORM=offscreen \
          LIBGL_ALWAYS_SOFTWARE=1 \
          timeout 120 "$ARORA" --startpage-smoke 2>&1)
    rc=$?
    if [ $rc -eq 0 ] && printf '%s' "$out" | grep -q 'startpage-smoke: PASS'; then
        pass=$((pass + 1))
        printf 'run %2d ok\n' "$i"
    else
        fail=$((fail + 1))
        printf 'run %2d FAIL rc=%d\n' "$i" "$rc"
        printf '%s\n' "$out" | tail -10
    fi
    i=$((i + 1))
done

total=$((pass + fail))
echo "START01: $pass/$total smoke runs passed"
[ "$fail" -eq 0 ]
