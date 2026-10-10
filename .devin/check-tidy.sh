#!/bin/sh
# check-tidy.sh — AUDIT01 clang-tidy pass
#
# Runs clang-tidy over every translation unit that the build compiles,
# reusing an already-generated build tree's Makefiles: `make -Bn` in
# each leaf dir re-prints the real compile lines (generated moc/ui/qrc
# files already exist there), g++ is swapped for clang-tidy, the
# -fanalyzer/object -o is stripped, and commands are deduplicated to
# one analysis per canonical source file (autotest Makefiles re-emit
# the shared src objects ~44x).
#
# clang-tidy comes from the user-local pip wheel
# (~/venvs/aqt/bin/clang-tidy — no sudo install possible); it is a
# build-time audit tool, not a shipped dependency.
#
# Env:
#   ARORA_TIDY_BUILD=dir   build tree with generated Makefiles +
#                          moc/ui/qrc artifacts (default:
#                          /tmp/arora-audit01/static-tree — the
#                          check-static pass-1 tree fits exactly)
#   ARORA_TIDY_DIRS="dirs" Makefile search roots under the build tree
#                          (default: "src tools")
#   ARORA_TIDY_JOBS=N      parallel clang-tidy jobs (default 4)
#   ARORA_TIDY_CHECKS=str  check glob list
#                          (default 'bugprone-*,cert-*,clang-analyzer-*')
#   CLANG_TIDY=path        clang-tidy binary
#                          (default ~/venvs/aqt/bin/clang-tidy, then
#                          PATH)
#
# Writes <build>/tidy-findings.txt — a report tool, not a gate:
# findings are triaged into .devin/AUDIT01-report.md by hand.
set -u

BUILD=${ARORA_TIDY_BUILD:-/tmp/arora-audit01/static-tree}
DIRS=${ARORA_TIDY_DIRS:-"src tools"}
JOBS=${ARORA_TIDY_JOBS:-4}
CHECKS=${ARORA_TIDY_CHECKS:-'bugprone-*,cert-*,clang-analyzer-*'}
TIDY=${CLANG_TIDY:-$HOME/venvs/aqt/bin/clang-tidy}
command -v "$TIDY" >/dev/null 2>&1 || TIDY=$(command -v clang-tidy || true)

die() { echo "check-tidy: $*" >&2; exit 1; }

[ -n "$TIDY" ] || die "clang-tidy not found (pip wheel expected in ~/venvs/aqt)"
[ -d "$BUILD/src" ] || die "ARORA_TIDY_BUILD=$BUILD is not a build tree"

cd "$BUILD" || die "cannot cd to $BUILD"
# shellcheck disable=SC1090
. "$(dirname -- "$0")/qt-env.sh"

LOGDIR="$BUILD/tidy-logs"
rm -rf "$LOGDIR"
mkdir -p "$LOGDIR"
CMDS="$BUILD/tidy-cmds.txt"
: > "$CMDS"

MFROOTS=""
for d in $DIRS; do MFROOTS="$MFROOTS $BUILD/$d"; done

# MAKE=true neuters the recursive $(MAKE) calls qmake emits for
# subdirs; keep only compile steps, drop -fanalyzer (clang rejects it)
# and the object -o.  The source path stays last on the line.
# shellcheck disable=SC2086
find $MFROOTS -name Makefile -type f | while read -r mf; do
    d=$(dirname "$mf")
    make -Bn MAKE=true -C "$d" 2>/dev/null \
        | grep '^g++ ' | grep ' -c ' \
        | sed -e 's|^g++ ||' \
              -e 's| -fanalyzer||g' \
              -e 's| -o [^ ]*| |' \
        | sed "s|^|cd '$d' \&\& |" >> "$CMDS"
done

# Collapse to one analysis per canonical source file, preferring the
# Makefile that owns the source (same dedupe rule as check-static).
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
echo "check-tidy: analyzing $N_TU translation units (-j$JOBS)"
echo "check-tidy: $TIDY $("$TIDY" --version | sed -n 's/.*version //p' | head -1) checks: $CHECKS"

# Each line: `cd dir && <compile flags> file.cpp` — clang-tidy takes
# the file positionally and the flags after `--`.  One numbered log
# per TU keeps parallel output from interleaving.
nl -ba "$CMDS" | while read -r n line; do
    printf '%s\n' "$line" > "$LOGDIR/$(printf '%04d' "$n").cmd"
done
export TIDY CHECKS LOGDIR
# The runner lives in a generated script file: embedding it as a
# single-quoted `sh -c` argument breaks on the \' sequences in the
# parameter expansions (sh: Syntax error: Missing '}').
cat > "$LOGDIR/run.sh" <<'RUNEOF'
#!/bin/sh
f=$1
n=$(basename "$f" .cmd)
# split "cd dir && flags file" back apart
line=$(cat "$f")
dir=${line#cd \'}
dir=${dir%%\' \&\&*}
rest=${line#*&& }
src=${rest##* }
flags=${rest% "$src"}
# The compile line shell-escapes -D values (PKGDATADIR=\"/path\").
# Word-splitting below bypasses shell unquoting, so strip the
# backslashes or clang sees a bare path token ("expected expression").
flags=$(printf '%s' "$flags" | sed 's/\\//g')
{
    echo "### $src"
    cd "$dir" && "$TIDY" "$src" --checks="$CHECKS" --quiet -- $flags
} > "$LOGDIR/$n.log" 2>&1
RUNEOF
chmod +x "$LOGDIR/run.sh"
ls "$LOGDIR"/*.cmd | xargs -P "$JOBS" -n1 "$LOGDIR/run.sh"

FINDINGS="$BUILD/tidy-findings.txt"
export BUILD FINDINGS
python3 - <<'PYEOF'
import re, os
BUILD = os.environ['BUILD']
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
        if os.path.exists(os.path.join(BUILD, cand)):
            return cand, 'tree'
    return p, 'tree'

tree, qt, sysl = set(), set(), set()
rx = re.compile(r'([^:]+):(\d+):(\d+):\s*(warning:.*\[[a-zA-Z][a-zA-Z0-9._-]+\])\s*$')
import glob
for logf in glob.glob(os.path.join(BUILD, 'tidy-logs', '*.log')):
    for line in open(logf, errors='replace'):
        m = rx.match(line.rstrip())
        if not m:
            continue
        canon, where = canonical(m.group(1))
        rec = '%s:%s:%s: %s' % (canon, m.group(2), m.group(3), m.group(4))
        (qt if where == 'qt' else sysl if where == 'sys' else tree).add(rec)

with open(os.environ['FINDINGS'], 'w') as f:
    f.write('== in-tree ==\n')
    f.write('\n'.join(sorted(tree)) + ('\n' if tree else ''))
    f.write('== qt-headers ==\n')
    f.write('\n'.join(sorted(qt)) + ('\n' if qt else ''))
    if sysl:
        f.write('== system/other ==\n')
        f.write('\n'.join(sorted(sysl)) + '\n')
print('check-tidy: %d in-tree findings, %d qt/system' % (len(tree), len(qt) + len(sysl)))
PYEOF

echo "check-tidy: findings written to $FINDINGS"
exit 0
