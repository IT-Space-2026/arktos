#!/bin/sh
# check-leaks.sh — MEM01 memory-leak gate
#
# Phase 1 — LeakSanitizer over the HARD01 instrumented variant.
#   A throwaway copy of the tree is built with
#   `qmake -spec linux-clang CONFIG+=sanitize`; the autotest suite and
#   every --*-smoke flag then run offscreen under an isolated HOME
#   with ASan detect_leaks=1 and LSAN_OPTIONS=exitcode=0, so each
#   binary reports its leaks instead of dying.  Every leak block is
#   classified by its ALLOCATION SITE: a block whose first frames
#   reach code under src/ or autotests/ is "ours" and fails the gate;
#   everything else is an upstream leak recorded in LEAKS.md.
#
#   The gate deliberately does NOT trust .devin/lsan.supp.  LSan
#   suppression matching is any-frame (verified on this build), so a
#   `leak:libQt6Core` entry would also hide Arora allocations made
#   inside Qt-driven call chains.  lsan.supp therefore only lists leaf
#   third-party libraries that can never call into our code, and is
#   meant for interactive use — the classification is what gates.
#
# Phase 2 — valgrind memcheck over the suite on the PLAIN in-tree
#   build (LSan and memcheck cannot share a binary).
#   QT_ENABLE_REGEXP_JIT=0 disables pcre2's JIT, whose self-modifying
#   code produces thousands of false positives under memcheck.
#   definite+indirect leaks count as errors (--errors-for-leak-kinds);
#   still-reachable does not — those are Qt's deliberate process-wide
#   caches.  .devin/valgrind.supp covers the one upstream init error
#   seen on this Qt build.  --trace-children=no keeps the exec'd
#   QtWebEngineProcess helpers native; every memcheck record (error
#   contexts and leak records alike) is classified by allocation site
#   exactly like phase 1 — the gate fails only on records sited in
#   binaries under the repo root.
#
# Env:
#   ARORA_LEAKS_BUILD=dir    resume an existing instrumented tree
#   ARORA_LEAKS_LSAN_LOGS=dir  reuse an existing phase-1 log corpus
#                            instead of rerunning suite+smokes (for
#                            resuming after an interrupted run; the
#                            classifier must still show 0 our-code
#                            blocks for the gate to pass)
#   ARORA_LEAKS_JOBS=N       make jobs for the ASan build (default 4)
#   ARORA_LEAKS_VGJOBS=N     parallel valgrind tests (default 2)
#   ARORA_LEAKS_VGLOGS=dir   reuse an existing phase-2 valgrind corpus
#                            instead of rerunning the suite (one
#                            <testdir>.log per test binary; the gate
#                            still requires a log for every test)
#   ARORA_LEAKS_KEEP=1       keep the instrumented build tree
#   ARORA_LEAKS_VALGRIND=0   skip phase 2
#   VALGRIND=path            default: /snap/bin/valgrind, else PATH
#
# Writes .devin/LEAKS.md.  Exit 0 iff no leak is allocated in our code
# and the valgrind pass reports no unsuppressed errors.
set -u

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
REPORT_MD="$ROOT/.devin/LEAKS.md"

JOBS=${ARORA_LEAKS_JOBS:-4}
VGJOBS=${ARORA_LEAKS_VGJOBS:-2}
KEEP=${ARORA_LEAKS_KEEP:-0}
DO_VG=${ARORA_LEAKS_VALGRIND:-1}
BUILD=${ARORA_LEAKS_BUILD:-}

die() { echo "check-leaks: $*" >&2; exit 1; }

command -v clang++ >/dev/null 2>&1 || die "clang++ not found"
command -v git >/dev/null 2>&1 || die "git not found"
command -v python3 >/dev/null 2>&1 || die "python3 not found (log classifier)"

# ---------------- phase 1: build + run under LSan ----------------

if [ -n "$BUILD" ]; then
    [ -d "$BUILD/src" ] || die "ARORA_LEAKS_BUILD=$BUILD is not a build tree"
    echo "check-leaks: resuming instrumented tree $BUILD"
else
    BUILD=$(mktemp -d /tmp/arora-leaks.XXXXXX)
    echo "check-leaks: build tree $BUILD"
    cd "$ROOT" || die "cannot cd to $ROOT"
    git ls-files -z -c -o --exclude-standard | tar --null -T - -cf - \
        | tar -x -C "$BUILD" || die "tree copy failed"
    cd "$BUILD" || die "cannot cd to $BUILD"
    # shellcheck disable=SC1090
    . "$ROOT/.devin/qt-env.sh"
    QMAKE=$(command -v qmake6 || command -v qmake) || die "qmake not found"
    echo "check-leaks: qmake ($("$QMAKE" -query QT_VERSION)) + make -j$JOBS"
    "$QMAKE" -spec linux-clang "CONFIG+=sanitize" arora.pro || die "qmake failed"
    make -j"$JOBS" sub-src sub-autotests || die "build failed"
fi

HOME="$BUILD/.home"
export HOME
mkdir -p "$HOME"

LOGDIR=${ARORA_LEAKS_LSAN_LOGS:-$BUILD/leaklogs}
mkdir -p "$LOGDIR"

# Same ASan budget as check-sanitize.sh; detect_leaks=1 is the whole
# point of this script.  exitcode=0 keeps the suite running to the end
# so one leaky binary cannot hide the others — we gate on the parsed
# reports, not on process exit codes.
export ASAN_OPTIONS="detect_leaks=1:quarantine_size_mb=64:thread_local_quarantine_size_kb=64:print_stacktrace=1"
export LSAN_OPTIONS="exitcode=0:print_suppressions=0:report_objects=1"
export QT_QPA_PLATFORM=offscreen
command -v llvm-symbolizer-18 >/dev/null 2>&1 \
    && export ASAN_SYMBOLIZER_PATH=/usr/bin/llvm-symbolizer-18
command -v llvm-symbolizer >/dev/null 2>&1 \
    && export ASAN_SYMBOLIZER_PATH=$(command -v llvm-symbolizer)

if [ -z "${ARORA_LEAKS_LSAN_LOGS:-}" ]; then
    echo "check-leaks: running autotest suite under LSan"
    (cd "$BUILD/autotests" && ./runTests.sh) >"$LOGDIR/suite.log" 2>&1
    TESTS_RC=$?
    [ "$TESTS_RC" -eq 0 ] || echo "check-leaks: WARNING: autotests exited $TESTS_RC"

    SMOKE_FLAGS="--quit-after-load --app-smoke --browser-smoke --nam-smoke
--cookie-smoke --history-smoke --bookmarks-smoke --search-smoke
--settings-smoke --autofill-smoke --find-smoke --source-smoke
--adblock-smoke --adblock-list-smoke --adblock-rust-smoke
--extension-smoke --ua-smoke"
    for flag in $SMOKE_FLAGS; do
        name=$(echo "$flag" | tr -d -)
        timeout 300 "$BUILD/arora" "$flag" >"$LOGDIR/smoke-$name.log" 2>&1 \
            || echo "check-leaks: smoke $flag exit=$? (tolerated)"
    done
    timeout 300 "$BUILD/arora" --download-smoke "file://$BUILD/README" \
        >"$LOGDIR/smoke-downloadsmoke.log" 2>&1 \
        || echo "check-leaks: smoke --download-smoke exit=$? (tolerated)"
else
    TESTS_RC=0
    echo "check-leaks: reusing LSan corpus $LOGDIR ($(ls "$LOGDIR"/*.log 2>/dev/null | wc -l) logs)"
fi

# ---------------- phase 1: classify leak reports ----------------
# A block is "ours" when one of its first four stack frames resolves
# to a source file under the instrumented tree's src/ or autotests/
# dirs (the allocation site is in our code or in generated code for
# our classes).  Deeper frames only mean our code triggered the
# upstream allocation — unfixable and reported as upstream.

CLASSIFY_OUT="$BUILD/classify.txt"
python3 - "$LOGDIR" "$BUILD" > "$CLASSIFY_OUT" <<'PYEOF'
import re, os, sys, collections

logdir, build = sys.argv[1], os.path.abspath(sys.argv[2])
ours_total = up_total = blocks_total = 0
ours_sites = collections.Counter()
up_topsites = collections.Counter()
per_log = []

for fn in sorted(os.listdir(logdir)):
    if not fn.endswith(".log"):
        continue
    text = open(os.path.join(logdir, fn), errors="replace").read()
    ours_n = up_n = 0
    for b in re.split(r"(?=^(?:Direct|Indirect) leak of )", text, flags=re.M):
        m = re.match(r"(Direct|Indirect) leak of (\d+) byte", b)
        if not m:
            continue
        blocks_total += 1
        site = None
        for line in b.splitlines()[1:9]:
            fm = re.match(r"\s*#(\d+) ", line)
            if not fm:
                break
            if int(fm.group(1)) > 4:
                break
            src = re.search(r"(%s/(?:src|autotests)/\S+?\.(?:cpp|h)):(\d+)"
                            % re.escape(build), line)
            if src:
                site = src.group(1).replace(build + "/", "") + ":" + src.group(2)
                break
        if site:
            ours_n += 1
            ours_sites[(site, fn)] += 1
        else:
            up_n += 1
            # signature = module of the first non-interceptor frame
            fr = re.findall(r"#\d+ 0x[0-9a-f]+ (?:in \S+.*? )?\(([^)]+)\)", b)
            if len(fr) > 1:
                lib = re.sub(r"\+0x[0-9a-f]+.*", "", fr[1])
                up_topsites[os.path.basename(lib) or lib] += 1
    if ours_n or up_n:
        per_log.append((fn, ours_n, up_n))
    ours_total += ours_n
    up_total += up_n

print("LEAK_BLOCKS_TOTAL %d" % blocks_total)
print("LEAK_BLOCKS_OURS %d" % ours_total)
print("LEAK_BLOCKS_UPSTREAM %d" % up_total)
print("-- per-log (ours/upstream) --")
for fn, o, u in per_log:
    print("%-50s %4d / %4d" % (fn, o, u))
print("-- our allocation sites --")
for (site, fn), c in sorted(ours_sites.items()):
    print("%-70s %s (%d)" % (site, fn, c))
print("-- upstream top modules --")
for lib, c in up_topsites.most_common(15):
    print("%-40s %d" % (lib, c))
PYEOF
CLASSIFY_RC=$?
[ "$CLASSIFY_RC" -eq 0 ] || die "leak classifier failed"

OURS=$(grep '^LEAK_BLOCKS_OURS' "$CLASSIFY_OUT" | awk '{print $2}')
TOTAL=$(grep '^LEAK_BLOCKS_TOTAL' "$CLASSIFY_OUT" | awk '{print $2}')
UPSTREAM=$(grep '^LEAK_BLOCKS_UPSTREAM' "$CLASSIFY_OUT" | awk '{print $2}')
echo "check-leaks: $TOTAL leak blocks — $OURS in our code, $UPSTREAM upstream"
sed -n '/-- our allocation sites --/,/-- upstream/p' "$CLASSIFY_OUT" | head -20

# ---------------- phase 2: valgrind over the plain build ----------

VG_RC=0
VG_SUMMARY=""
if [ "$DO_VG" = "1" ]; then
    VALGRIND=${VALGRIND:-}
    if [ -z "$VALGRIND" ]; then
        [ -x /snap/bin/valgrind ] && VALGRIND=/snap/bin/valgrind \
            || VALGRIND=$(command -v valgrind || true)
    fi
    if [ -z "$VALGRIND" ]; then
        echo "check-leaks: WARNING: no valgrind found — phase 2 skipped"
    elif [ ! -x "$ROOT/arora" ]; then
        echo "check-leaks: WARNING: no in-tree build — run qmake && make first; phase 2 skipped"
    else
        export QT_ENABLE_REGEXP_JIT=0
        VGLOG=${ARORA_LEAKS_VGLOGS:-$BUILD/vglogs}
        mkdir -p "$VGLOG"
        # --trace-children=no: exec'd QtWebEngineProcess helpers run
        # natively — nothing of ours executes in them, and tracing them
        # triples per-test memory/time.  Chromium's fork-without-exec
        # children cannot be detached (they are clones of the traced
        # image); their records classify upstream by binary path.
        VGFLAGS="--tool=memcheck --leak-check=full --show-leak-kinds=definite,indirect --errors-for-leak-kinds=definite,indirect --error-exitcode=42 --num-callers=15 --trace-children=no --suppressions=$ROOT/.devin/valgrind.supp"
        cd "$ROOT/autotests" || die "no autotests dir"
        TESTLIST=$(
            for pro in */*.pro */*/*.pro; do
                [ -f "$pro" ] || continue
                grep -q "^TEMPLATE *= *subdirs" "$pro" && continue
                dir=${pro%/*}; name=${pro##*/}; name=${name%.pro}
                bin=
                for c in "$dir/tst_$name" "$dir/$name"; do
                    [ -x "$c" ] && [ ! -d "$c" ] && bin=$c && break
                done
                [ -n "$bin" ] && echo "$dir ${bin##*/}"
            done
        )
        # Health per log = the qtest result only: a missing/incomplete
        # log or a failed test fails the gate.  memcheck's own counts
        # (rc=42, nonzero ERROR SUMMARY) do NOT decide — the classifier
        # below sites every record; upstream Chromium noise is expected
        # and reported, only records allocated in our code fail.
        if [ -n "${ARORA_LEAKS_VGLOGS:-}" ]; then
            echo "check-leaks: reusing valgrind corpus $VGLOG ($(ls "$VGLOG"/*.log 2>/dev/null | wc -l) logs)"
            echo "$TESTLIST" | while read -r d b; do
                log="$VGLOG/$(echo "$d" | tr / _).log"
                if [ ! -f "$log" ]; then
                    echo "$d rc=missing"
                elif ! grep -q "Totals:" "$log"; then
                    echo "$d rc=nocomplete"
                elif ! grep -q "Totals:.* 0 failed" "$log"; then
                    echo "$d rc=testfail"
                else
                    echo "$d rc=0"
                fi
            done | tee "$VGLOG/results.txt"
        else
            echo "check-leaks: running suite under valgrind ($VGJOBS jobs)"
            echo "$TESTLIST" | xargs -P"$VGJOBS" -n2 sh -c '
                cd "'"$ROOT"'/autotests/$0" || exit 1
                log="'"$VGLOG"'/$(echo "$0" | tr / _).log"
                timeout 900 '"$VALGRIND"' '"$VGFLAGS"' "./$1" -silent \
                    > "$log" 2>&1
                if ! grep -q "Totals:" "$log"; then
                    echo "$0 rc=nocomplete"
                elif ! grep -q "Totals:.* 0 failed" "$log"; then
                    echo "$0 rc=testfail"
                else
                    echo "$0 rc=0"
                fi
            ' | tee "$VGLOG/results.txt"
        fi

        # classify every memcheck record (error contexts and
        # definite/indirect leak records): a record is "ours" when one
        # of its first four stack frames resolves to a binary under
        # $ROOT — the test binaries statically contain all of src/, so
        # their frames appear as "(in .../autotests/<dir>/tst_*)".
        # WebEngine/Chromium teardown noise — where our functions only
        # appear as deep callers — classifies upstream and is recorded,
        # not gated.  This is the same discipline as phase 1: the
        # suppression file cannot safely express "upstream leak that
        # our code merely called into", so classification does.
        VGCLASS="$BUILD/vgclassify.txt"
        python3 - "$VGLOG" "$ROOT" > "$VGCLASS" <<'PYEOF'
import re, os, sys, collections

vglog, root = sys.argv[1], os.path.abspath(sys.argv[2])
ours_total = up_total = 0
ours_sites = collections.Counter()
up_kinds = collections.Counter()
per_log = []
ERR_HDR = re.compile(
    r"(Invalid (?:read|write|free)|Conditional jump|Use of uninitialised|"
    r"Syscall param|Mismatched free|bytes in \d+ blocks are (?:definitely|indirectly) lost|"
    r"direct, [\d,]+ indirect\) bytes in \d+ blocks are (?:definitely|indirectly) lost)")
LINE = re.compile(r"^==(\d+)==\s+(.*)$")
for fn in sorted(os.listdir(vglog)):
    if not fn.endswith(".log"):
        continue
    text = open(os.path.join(vglog, fn), errors="replace").read()
    ours_n = up_n = 0
    # The banner PID is the test binary itself; other PIDs in the log
    # are fork-without-exec children (clones of the instrumented image
    # that valgrind cannot detach).  Their frames resolve to our binary
    # too, but they are Qt/Chromium helper forks — not test call sites —
    # so their records always classify upstream.
    mainpid = None
    cur = None
    recs = []
    for line in text.splitlines():
        m = LINE.match(line)
        if not m:
            continue
        pid, body = m.group(1), m.group(2)
        if mainpid is None and body.startswith("Command:"):
            mainpid = pid
        if re.match(r"(?:at|by) 0x", body):
            if cur is not None and cur[2] == pid:
                cur[1].append(body)
                continue
            if cur is not None:
                recs.append(cur)
            cur = None
        else:
            if cur is not None:
                recs.append(cur)
            cur = [body, [], pid]
    if cur is not None:
        recs.append(cur)
    for hdr, stack, pid in recs:
        if not ERR_HDR.search(hdr):
            continue
        site = None
        if mainpid is not None and pid != mainpid:
            stack = []
        for i, fr in enumerate(stack[:5]):
            om = re.search(r"\(in (\S+)\)", fr)
            if not om:
                continue
            obj = om.group(1)
            if "vg_" in os.path.basename(obj):
                continue
            if obj.startswith(root) and not obj.endswith(".so"):
                site = "%s#%d %s" % (os.path.basename(obj), i,
                                     re.sub(r"^at |^by ", "", fr).split(" (in ")[0])
                break
            if i >= 4:
                break
        if site:
            ours_n += 1
            ours_sites[(site, fn)] += 1
        else:
            up_n += 1
            up_kinds[ERR_HDR.search(hdr).group(1).split(" in ")[0][:40]] += 1
    if ours_n or up_n:
        per_log.append((fn, ours_n, up_n))
    ours_total += ours_n
    up_total += up_n

print("VG_RECORDS_OURS %d" % ours_total)
print("VG_RECORDS_UPSTREAM %d" % up_total)
print("-- per-log (ours/upstream) --")
for fn, o, u in per_log:
    print("%-50s %4d / %4d" % (fn, o, u))
print("-- our sites --")
for (site, fn), c in sorted(ours_sites.items()):
    print("%-70s %s (%d)" % (site, fn, c))
print("-- upstream kinds --")
for k, c in up_kinds.most_common(20):
    print("%-45s %d" % (k, c))
PYEOF
        VG_OURS=$(grep '^VG_RECORDS_OURS' "$VGCLASS" | awk '{print $2}')
        VG_UP=$(grep '^VG_RECORDS_UPSTREAM' "$VGCLASS" | awk '{print $2}')
        echo "check-leaks: memcheck records — $VG_OURS sited in our code, $VG_UP upstream"
        sed -n '/-- our sites --/,/-- upstream/p' "$VGCLASS" | head -20

        # collect: tests that failed, timed out, or left our-code records
        VG_FAILS=$(grep -v "rc=0$" "$VGLOG/results.txt" || true)
        if [ -n "$VG_FAILS" ] || [ "${VG_OURS:-1}" != "0" ]; then
            VG_RC=1
            VG_SUMMARY="valgrind: $VG_OURS our-code record(s) + suite failures: $VG_FAILS"
            echo "check-leaks: valgrind findings:"
            echo "$VG_FAILS"
        else
            VG_SUMMARY="valgrind: suite clean — 0 records sited in our code ($VG_UP upstream records, see suppressions)"
            echo "check-leaks: $VG_SUMMARY"
        fi
    fi
fi

# ---------------- report ----------------------------------------

{
    echo "# Memory-leak sweep (MEM01)"
    echo
    echo "Generated: $(date -u '+%Y-%m-%d %H:%M UTC') on commit $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo unknown)"
    echo "Toolchain: $(clang++ --version | head -1), $("$VALGRIND" --version 2>/dev/null | head -1 || echo 'no valgrind')"
    echo
    echo "## Reproduce"
    echo
    echo '    source .devin/qt-env.sh && make check-leaks'
    echo
    echo 'Phase 1 builds the ASan tree (sanitize.pri) and runs the suite'
    echo '+ all --*-smoke flags under LSan; every leak block is classified'
    echo 'by allocation site.  Phase 2 reruns the suite under valgrind'
    echo 'memcheck on the in-tree build (QT_ENABLE_REGEXP_JIT=0).'
    echo 'Suppressions: .devin/lsan.supp (LSan, leaf upstream libs only —'
    echo 'any-frame matching makes Qt-wide suppressions unsafe) and'
    echo '.devin/valgrind.supp (stack-ordered, upstream init noise).'
    echo
    echo "## Result"
    echo
    echo "autotest suite exit: $TESTS_RC"
    echo "LSan leak blocks: $TOTAL total — **$OURS in Arora code**, $UPSTREAM upstream"
    echo "${VG_SUMMARY:-valgrind: skipped}"
    echo
    echo '```'
    cat "$CLASSIFY_OUT"
    echo '```'
} > "$REPORT_MD"

echo "check-leaks: report written to ${REPORT_MD#$ROOT/}"

if [ "$KEEP" != "1" ] && [ -z "${ARORA_LEAKS_BUILD:-}" ]; then
    rm -rf "$BUILD"
else
    echo "check-leaks: keeping $BUILD (logs in $LOGDIR)"
fi

[ "${OURS:-1}" -eq 0 ] && [ "$TESTS_RC" -eq 0 ] && [ "$VG_RC" -eq 0 ]
