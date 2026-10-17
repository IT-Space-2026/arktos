#!/bin/sh
# AUDIT02-resume.sh — (re)launch the STAT02 check-static sweep detached
# from the devin process tree (the taskloop watchdog kills the whole
# tree at 2048MB RSS; a single -fanalyzer TU peaks ~900MB, so -j3 must
# run outside it — a systemd --user transient unit does exactly that).
#
# Idempotent: refuses to launch a second unit while arora-stat02 runs.
# Check progress:  systemctl --user status arora-stat02
#                  tail -f /tmp/audit02-static.log
set -u

BUILD=/tmp/arora-stat.un2m6A
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

if systemctl --user is-active --quiet arora-stat02; then
    echo "arora-stat02 already running — nothing to do" >&2
    exit 0
fi

[ -d "$BUILD" ] || { echo "build tree $BUILD gone — fresh sweep needed" >&2; exit 1; }

# -j1 for pass 1: the remaining TUs are the huge ones (tabwidget/tabbar/
# settings, ~2.5GB each under -fanalyzer) — -j3 on those OOMs the box.
exec systemd-run --user --unit=arora-stat02 --collect \
    --description="STAT02 check-static resume (AUDIT02 slice A, capped)" \
    -p MemoryMax=5G -p MemoryLow=2G \
    --setenv=ARORA_STATIC_BUILD="$BUILD" \
    --setenv=ARORA_STATIC_TARGETS="sub-src sub-tools" \
    --setenv=ARORA_STATIC_DIRS="src tools" \
    --setenv=ARORA_STATIC_GCC_JOBS=1 \
    --setenv=ARORA_STATIC_CLANG_JOBS=3 \
    --setenv=ARORA_STATIC_REPORT="$ROOT/.devin/STAT02-report.md" \
    --setenv=ARORA_STATIC_TRIAGE="$BUILD/no-triage-yet" \
    --setenv=ARORA_STATIC_TITLE=STAT02 \
    /bin/sh -c "exec '$ROOT/.devin/check-static.sh' >> /tmp/audit02-static.log 2>&1"
