#!/bin/sh
# check-static.sh — STAT01 static-analysis sweep
#
# Two passes over a throwaway copy of the tree (same pattern as
# check-sanitize.sh so analyzer objects never pollute the in-tree
# build):
#
#   1. GCC -fanalyzer: full qmake build with CONFIG+=analyzer
#      (analyzer.pri adds -fanalyzer).  Warnings land in the build
#      log as  file:line: warning: ... [-Wanalyzer-*].
#
#   2. clang static analyzer: `clang++ --analyze` cannot share the
#      build pass (it emits no objects), so after the GCC build has
#      generated all moc/ui/qrc files the per-TU compile lines are
#      extracted with `make -Bn` in every leaf Makefile dir, g++ is
#      swapped for `clang++ --analyze` (-fanalyzer and the object -o
#      dropped), duplicated TUs are collapsed to one analysis per
#      source file, and the survivors are analyzed in parallel.
#      Diagnostics come back as file:line: warning: ... [checker.Name].
#
# Deduped findings from both passes are written to
# .devin/STAT01-report.md; a hand-maintained .devin/STAT01-triage.md
# (fix/suppress justification per finding) is appended verbatim when
# present so re-runs keep the triage.
#
# Env:
#   ARORA_STATIC_BUILD=DIR  reuse an existing analysis tree instead
#                           of copying the sources to a fresh mktemp
#                           dir — lets a killed run resume (gcc log is
#                           appended, objects already built are kept).
#                           Implies KEEP.
#   ARORA_STATIC_GCC_JOBS=N   parallel jobs for the -fanalyzer build
#                             (default: 1 — a single -fanalyzer TU
#                             peaks near 900MB, so >1 concurrent job
#                             risks the task-loop's 2048MB tree RSS
#                             cap; -j2 is fine on unconstrained runs)
#   ARORA_STATIC_CLANG_JOBS=N parallel jobs for clang --analyze
#                             (default: 3 — ~400MB peak per TU)
#   ARORA_STATIC_TARGETS="goals"  pass-1 make goals instead of the
#                             whole tree (e.g. "sub-src sub-tools" —
#                             autotest dirs recompile every src object
#                             ~44x, which dominates pass-1 cost)
#   ARORA_STATIC_DIRS="dirs"  pass-2 Makefile search roots relative to
#                             the build tree (e.g. "src tools") —
#                             analyses then cover only those dirs' TUs
#   ARORA_STATIC_KEEP=1       keep a fresh analysis build tree
#
# Exit status: 0 unless a build/analysis pass failed.  This is a
# report tool, not a gate — findings are triaged into fixes or
# documented suppressions, not counted toward a zero-warning bar.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPORT_MD="$ROOT/.devin/STAT01-report.md"
TRIAGE_MD="$ROOT/.devin/STAT01-triage.md"

GCC_JOBS=${ARORA_STATIC_GCC_JOBS:-1}
CLANG_JOBS=${ARORA_STATIC_CLANG_JOBS:-3}
KEEP=${ARORA_STATIC_KEEP:-0}
RESUME=${ARORA_STATIC_BUILD:-}
TARGETS=${ARORA_STATIC_TARGETS:-}
DIRS=${ARORA_STATIC_DIRS:-}

die() { echo "check-static: $*" >&2; exit 1; }

command -v g++ >/dev/null 2>&1 || die "g++ not found"
command -v clang++ >/dev/null 2>&1 || die "clang++ not found"
command -v git >/dev/null 2>&1 || die "git not found"

if [ -n "$RESUME" ]; then
    BUILD=$RESUME
    [ -d "$BUILD" ] || die "ARORA_STATIC_BUILD=$BUILD does not exist"
    KEEP=1
    echo "check-static: resuming build tree $BUILD"
else
    BUILD=$(mktemp -d /tmp/arora-stat.XXXXXX)
    echo "check-static: build tree $BUILD"

    cd "$ROOT" || die "cannot cd to $ROOT"
    git ls-files -z -c -o --exclude-standard | tar --null -T - -cf - \
        | tar -x -C "$BUILD" || die "tree copy failed"

    cd "$BUILD" || die "cannot cd to $BUILD"
    # shellcheck disable=SC1090
    . "$ROOT/.devin/qt-env.sh"

    QMAKE=$(command -v qmake6 || command -v qmake) || die "qmake not found"
    "$QMAKE" "CONFIG+=analyzer" arora.pro || die "qmake failed"
fi

cd "$BUILD" || die "cannot cd to $BUILD"
# shellcheck disable=SC1090
. "$ROOT/.devin/qt-env.sh"
QMAKE=$(command -v qmake6 || command -v qmake) || die "qmake not found"

# --- pass 1: gcc -fanalyzer full build --------------------------------
# On a resumed tree objects already exist, so only the tail recompiles;
# the log is appended so earlier warnings are not lost.
echo "check-static: pass 1/2 — make -j$GCC_JOBS $TARGETS (gcc -fanalyzer)"
GCC_LOG="$BUILD/gcc-analyzer.log"
if ! make -j"$GCC_JOBS" $TARGETS >>"$GCC_LOG" 2>&1; then
    tail -50 "$GCC_LOG" >&2
    die "analyzer build failed (see $GCC_LOG)"
fi

# --- pass 2: clang --analyze per TU ------------------------------------
# `make -Bn` re-prints every recipe line unconditionally; MAKE=true
# neuters the recursive $(MAKE) calls qmake emits for subdirs (they
# WOULD run even under -n and re-print child compile lines that then
# get the wrong directory prefix).  Keep only compile steps, swap the
# driver, drop -fanalyzer (clang rejects it) and the object -o.
# Generated files (moc/ui/qrc) already exist from pass 1 so the clang
# pass resolves the same includes.
echo "check-static: pass 2/2 — clang++ --analyze per TU (-j$CLANG_JOBS)"
CLANG_LOGDIR="$BUILD/clang-logs"
rm -rf "$CLANG_LOGDIR"
mkdir -p "$CLANG_LOGDIR"
CMDS="$BUILD/clang-cmds.txt"
: > "$CMDS"
if [ -n "$DIRS" ]; then
    MFROOTS=""
    for d in $DIRS; do MFROOTS="$MFROOTS $BUILD/$d"; done
else
    MFROOTS="$BUILD"
fi
# shellcheck disable=SC2086
find $MFROOTS -name Makefile -type f | while read -r mf; do
    d=$(dirname "$mf")
    make -Bn MAKE=true -C "$d" 2>/dev/null \
        | grep '^g++ ' | grep ' -c ' \
        | sed -e 's|^g++ |clang++ --analyze -Xanalyzer -analyzer-output=text |' \
              -e 's| -fanalyzer||g' \
              -e 's| -o [^ ]*| |' \
        | sed "s|^|cd '$d' \&\& |" >> "$CMDS"
done

# Every autotest Makefile re-prints compile rules for the shared src
# objects, so each src TU arrives ~50x.  Collapse to one analysis per
# canonical source file, preferring the Makefile that owns the source
# (its own flags — no -DAUTOTESTS etc.).
python3 - "$CMDS" <<'PYEOF'
import re, os, sys
lines = open(sys.argv[1]).read().splitlines()
best = {}
for ln in lines:
    m = re.match(r"cd '([^']+)' && (.*) (\S+)$", ln)
    if not m:
        continue
    d, _, src = m.groups()
    apath = os.path.normpath(os.path.join(d, src))
    score = 0 if apath.startswith(os.path.normpath(d) + os.sep) else 1
    cur = best.get(apath)
    if cur is None or score < cur[0]:
        best[apath] = (score, ln)
uniq = [v[1] for _, v in sorted(best.items())]
open(sys.argv[1] + '.final', 'w').write('\n'.join(uniq) + '\n')
print('unique TUs:', len(uniq))
PYEOF
mv "$CMDS.final" "$CMDS"

N_TU=$(grep -c . "$CMDS" 2>/dev/null || echo 0)
echo "check-static: analyzing $N_TU translation units"
# Each line is a self-contained `cd dir && clang++ --analyze ...`; one
# numbered log per TU keeps parallel output from interleaving.
nl -ba "$CMDS" | while read -r n line; do
    printf '%s\n' "$line" > "$CLANG_LOGDIR/$(printf '%04d' "$n").cmd"
done
ls "$CLANG_LOGDIR"/*.cmd | xargs -P "$CLANG_JOBS" -I {} sh -c '
    f={}
    n=$(basename "$f" .cmd)
    {
        echo "### $(sed "s|^cd \(.*\) \&\& .* \([^ ]*\)$|\2|" "$f")"
        sh "$f"
    } > "'"$CLANG_LOGDIR"'/$n.log" 2>&1
'
CLANG_LOG="$BUILD/clang-analyzer.log"
cat "$CLANG_LOGDIR"/*.log > "$CLANG_LOG" 2>/dev/null || : > "$CLANG_LOG"

# --- collect findings ---------------------------------------------------
# GCC analyzer: "path/file.cpp:L:C: warning: ... [-Wanalyzer-name]"
# clang analyzer: "path/file.cpp:L:C: warning: ... [checker.name]"
# Normalization: the same src file is reached from the src/ build dir
# (adblock/x.cpp), from autotest dirs (../../../src/adblock/x.cpp) and
# from the build root — collapse every path spelling to repo-relative
# and dedupe, then split in-tree findings from Qt/system headers.
GCC_FINDINGS="$BUILD/gcc-findings.txt"
CLANG_FINDINGS="$BUILD/clang-findings.txt"
export ROOT BUILD GCC_LOG CLANG_LOG GCC_FINDINGS CLANG_FINDINGS
python3 - <<'PYEOF'
import re, os
ROOT = os.environ['ROOT']; BUILD = os.environ['BUILD']
QTDIR = os.environ.get('QTDIR', os.path.expanduser('~/Qt/6.12.0/gcc_64'))

def canonical(path):
    p = path
    if p.startswith(BUILD + '/'):
        p = p[len(BUILD) + 1:]
    while p.startswith('../') or p.startswith('./'):
        p = p[p.index('/') + 1:]
    if p.startswith('/'):
        if p.startswith(QTDIR):
            return 'qt6/' + os.path.relpath(p, QTDIR), 'qt'
        return p, 'sys'
    for cand in (p, 'src/' + p, 'autotests/' + p, 'tools/' + p, 'fuzz/' + p):
        if os.path.exists(os.path.join(ROOT, cand)):
            return cand, 'tree'
    return p, 'tree'

def collect(log, pattern, out):
    tree, qt, sysl = set(), set(), set()
    rx = re.compile(pattern)
    for line in open(log, errors='replace'):
        if not rx.search(line) or 'warning:' not in line:
            continue
        m = re.match(r'([^:]+):(\d+):(\d+):\s*(warning:.*)', line.rstrip())
        if not m:
            continue
        canon, where = canonical(m.group(1))
        rec = '%s:%s:%s: %s' % (canon, m.group(2), m.group(3), m.group(4))
        (qt if where == 'qt' else sysl if where == 'sys' else tree).add(rec)
    with open(out, 'w') as f:
        f.write('== in-tree ==\n')
        f.write('\n'.join(sorted(tree)) + ('\n' if tree else ''))
        f.write('== qt-headers ==\n')
        f.write('\n'.join(sorted(qt)) + ('\n' if qt else ''))
        if sysl:
            f.write('== system/other ==\n')
            f.write('\n'.join(sorted(sysl)) + '\n')
    return len(tree), len(qt) + len(sysl)

gcc_tree, gcc_ext = collect(os.environ['GCC_LOG'],
                          r'\[-Wanalyzer', os.environ['GCC_FINDINGS'])
clang_tree, clang_ext = collect(os.environ['CLANG_LOG'],
                              r'\[[a-zA-Z]+\.[a-zA-Z.]+\]\s*$', os.environ['CLANG_FINDINGS'])
open(os.path.join(BUILD, 'counts.env'), 'w').write(
    'GCC_TREE=%d\nGCC_EXT=%d\nCLANG_TREE=%d\nCLANG_EXT=%d\n'
    % (gcc_tree, gcc_ext, clang_tree, clang_ext))
PYEOF
# shellcheck disable=SC1090
. "$BUILD/counts.env"
GCC_COUNT=$GCC_TREE
CLANG_COUNT=$CLANG_TREE

{
    echo "# Static-analysis sweep (STAT01)"
    echo
    echo "Generated: $(date -u '+%Y-%m-%d %H:%M UTC') on commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "Toolchains: $(g++ --version | head -1) / $(clang++ --version | head -1)"
    echo
    echo "clang-tidy, cppcheck and scan-build are NOT installed on this"
    echo "box and cannot be (no sudo) — the sweep is limited to"
    echo "\`g++ -fanalyzer\` and the clang static analyzer"
    echo "(\`clang++ --analyze\`)."
    echo
    echo "## Reproduce"
    echo
    echo '    source .devin/qt-env.sh && make check-static'
    echo
    echo 'A copy of the tree is built in a throwaway dir with'
    echo '`CONFIG+=analyzer` (-fanalyzer on every TU); then each compile'
    echo 'line is replayed as `clang++ --analyze` (deduplicated to one'
    echo 'analysis per source file).  See analyzer.pri.'
    echo
    echo "Counts are deduplicated per (file:line:col, diagnostic) and"
    echo "split into in-tree findings vs Qt/system-header noise."
    echo
    echo "## GCC -fanalyzer findings ($GCC_COUNT in-tree, $GCC_EXT in Qt/system headers)"
    echo
    if [ "$GCC_COUNT" -gt 0 ] || [ "$GCC_EXT" -gt 0 ]; then
        echo '```'
        cat "$GCC_FINDINGS"
        echo '```'
    else
        echo "No findings."
    fi
    echo
    echo "## clang --analyze findings ($CLANG_COUNT in-tree, $CLANG_EXT in Qt/system headers)"
    echo
    if [ "$CLANG_COUNT" -gt 0 ] || [ "$CLANG_EXT" -gt 0 ]; then
        echo '```'
        cat "$CLANG_FINDINGS"
        echo '```'
    else
        echo "No findings."
    fi
    echo
    if [ -f "$TRIAGE_MD" ]; then
        cat "$TRIAGE_MD"
    fi
} > "$REPORT_MD"

echo "check-static: gcc findings $GCC_COUNT, clang findings $CLANG_COUNT"
echo "check-static: report written to ${REPORT_MD#$ROOT/}"

if [ "$KEEP" != "1" ]; then
    rm -rf "$BUILD"
else
    echo "check-static: keeping $BUILD"
fi

exit 0
