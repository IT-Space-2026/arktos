#!/usr/bin/env bash
# wpe-deps.sh — WKB05 dependency-manifest tool for the WPE WebKit payload.
#
# Given a WPE prefix (e.g. ~/wpe, populated by WK00) or a set of Debian
# .deb files/dirs, computes what the webkit backend actually needs to
# ship and which parts target distros already provide. Read-only —
# never modifies the input prefix.
#
#   scan TARGET     TARGET = a WPE prefix dir, a dir containing .debs,
#                   or .deb file(s). Prints: the transitive DT_NEEDED
#                   closure seeded from libWPEWebKit + the WPE helper
#                   binaries; per-SONAME resolution (prefix / system /
#                   MISSING); dlopen-only payload candidates (gstreamer
#                   plugins etc. — invisible to the NEEDED closure);
#                   PT_INTERP checks; glibc/glibcxx floors; and a
#                   curated soname × target-distro matrix.
#   debs TARGET     scan + per-source-deb verdicts: which debs
#                   contribute ≥1 file to the closure (SHIP) vs none
#                   (DROP — headers/tools/etc).
#   floor TARGET    only the GLIBC_/GLIBCXX_/CXXABI_ floor report.
#   selftest        builds a tiny gcc fixture prefix + fixture debs and
#                   asserts the scan/debs output (no WPE needed).
#
# Resolution order mirrors the deployed layout: prefix first (RPATH
# points there), then the host's ldconfig cache, then MISSING.
#
# "Which target distros provide it" is reported as the owning package
# on THIS host (dpkg -S; this dev box tracks Ubuntu 24.04/noble) plus a
# small embedded matrix for the headline WPE SONAMEs. The matrix is
# curated — refresh per release; .devin/WEBKIT-PACKAGING.md carries the
# full analysis.
#
# Env: ARKTOS_PKG_KEEP=1 keeps scratch dirs (extracted debs, fixtures).

set -euo pipefail

fail() { echo "wpe-deps: FAIL: $*" >&2; exit 1; }
note() { echo "wpe-deps: $*" >&2; }

command -v readelf >/dev/null || fail "readelf not found (binutils)"
command -v objdump >/dev/null || fail "objdump not found (binutils)"

SCRATCH=""
scratch() {
    SCRATCH="$(mktemp -d /tmp/wpe-deps.XXXXXX)"
    [ -n "${ARKTOS_PKG_KEEP:-}" ] || trap 'rm -rf "$SCRATCH"' EXIT
}

# ---------------------------------------------------------------------------
# Input normalization: turn TARGET into a single merged prefix dir $PREFIX.
# Populates DEB_FILE[] (relpath → deb name) when the input was .debs.
# ---------------------------------------------------------------------------
PREFIX=""
declare -A DEB_FILE=()

reset_globals() {
    ELF=() SEEDED=()
    PMAP=() RES=() IN_CLOSURE=() SYSPATH=() DEB_FILE=()
}

make_prefix() {
    local target="$1" debs=() d
    if [ -d "$target" ] && ! ls "$target"/*.deb >/dev/null 2>&1; then
        PREFIX="$(cd "$target" && pwd)"
        return
    fi
    if [ -d "$target" ]; then
        while IFS= read -r d; do debs+=("$d"); done \
            < <(find "$target" -maxdepth 1 -name '*.deb' | sort)
    else
        for d in "$@"; do debs+=("$d"); done
    fi
    [ "${#debs[@]}" -gt 0 ] \
        || fail "target is neither a prefix dir nor .deb(s): $target"
    command -v dpkg-deb >/dev/null \
        || fail "dpkg-deb needed to unpack .deb input"
    scratch
    PREFIX="$SCRATCH/root"
    mkdir -p "$PREFIX"
    for d in "${debs[@]}"; do
        local name
        name="$(dpkg-deb -f "$d" Package 2>/dev/null || basename "$d")"
        dpkg-deb -x "$d" "$PREFIX" \
            || fail "dpkg-deb -x failed: $d"
        while IFS= read -r p; do
            p="${p#./}"; p="${p#/}"
            [ -n "$p" ] && DEB_FILE["$p"]="$name"
        done < <(dpkg-deb --fsys-tarfile "$d" | tar -tf - 2>/dev/null \
                 | grep -v '/$')
        note "  unpacked $(basename "$d") [$name]"
    done
    note "extracted ${#debs[@]} deb(s) -> $PREFIX"
}

# ---------------------------------------------------------------------------
# ELF inventory + closure machinery.
#   ELF[]      all ELF files under PREFIX (relpaths)
#   PMAP       soname-or-basename → relpath (prefix resolution map)
#   RES        soname → "prefix:rel" | "system:abs" | "missing:"
#   SEEDED[]   seed files actually walked
# ---------------------------------------------------------------------------
declare -a ELF=() SEEDED=()
declare -A PMAP=() RES=() IN_CLOSURE=() SYSPATH=()

is_elf() { readelf -h "$1" >/dev/null 2>&1; }

collect_elves() {
    local f rel
    while IFS= read -r f; do
        is_elf "$PREFIX/$f" || continue
        ELF+=("$f")
    done < <(cd "$PREFIX" && find . -type f | sed 's|^\./||' | sort)
    [ "${#ELF[@]}" -gt 0 ] || fail "no ELF files under $PREFIX"
    for rel in "${ELF[@]}"; do
        local sn
        sn="$(readelf -d "$PREFIX/$rel" 2>/dev/null \
              | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p' | head -1)"
        [ -n "$sn" ] && PMAP["$sn"]="$rel"
        case "$(basename "$rel")" in
            *.so|*.so.*) PMAP["$(basename "$rel")"]="$rel" ;;
        esac
    done
    # SONAME symlink aliases (libfoo.so.1 -> libfoo.so.1.2.3)
    while IFS= read -r rel; do
        local tgt
        tgt="$(readlink -f "$PREFIX/$rel")"
        case "$tgt" in "$PREFIX"/*)
            tgt="${tgt#$PREFIX/}"
            [ -n "${PMAP[$(basename "$rel")]:-}" ] \
                || PMAP["$(basename "$rel")"]="$tgt"
        ;; esac
    done < <(cd "$PREFIX" && find . -type l -name '*.so*' | sed 's|^\./||')
}

pick_seeds() {
    local rel wpe=0
    for rel in "${ELF[@]}"; do
        case "$rel" in
            *libWPEWebKit-*.so.*|*/wpe-webkit-*/*)
                case "$rel" in
                    */MiniBrowser|*/WPEWebDriver) ;;  # upstream dev tools
                    *) SEEDED+=("$rel"); wpe=1 ;;
                esac ;;
        esac
    done
    if [ "$wpe" -eq 0 ]; then
        SEEDED=("${ELF[@]}")
        note "no libWPEWebKit signature — seeding every ELF file"
    fi
}

load_system_map() {
    local ldc archtag
    case "$(uname -m)" in
        x86_64)  archtag='x86-64' ;;
        aarch64) archtag='AArch64' ;;
        *)       archtag='' ;;
    esac
    for ldc in ldconfig /sbin/ldconfig /usr/sbin/ldconfig; do
        command -v "$ldc" >/dev/null 2>&1 || continue
        # ldconfig -p: "libfoo.so.1 (libc6,x86-64) => /path" — the cache
        # mixes arches; only accept entries matching our arch tag so a
        # 32-bit shadow can't win a 64-bit resolution.
        while read -r sn p; do
            [ -n "${p:-}" ] && SYSPATH["$sn"]="$p"
        done < <("$ldc" -p 2>/dev/null | awk -v tag="$archtag" '
            /^[ \t]/ {
                line=$0
                if (line !~ /=>/) next
                if (tag != "" && line !~ tag) next
                sub(/^[ \t]+/, "", line)
                split(line, a, / => /)
                split(a[1], b, " ")
                print b[1], a[2]
            }')
        [ "${#SYSPATH[@]}" -gt 0 ] && return
    done
    # Fallback: well-known dirs, no cache.
    local d s
    for d in /lib /usr/lib /lib64 /usr/lib64 /lib/*-linux-gnu \
             /usr/lib/*-linux-gnu /usr/local/lib; do
        [ -d "$d" ] || continue
        for s in "$d"/*.so.* "$d"/*.so; do
            [ -e "$s" ] || continue
            SYSPATH["$(basename "$s")"]="$s"
        done
    done
}

resolve() { # soname -> echoes resolution token
    local s="$1"
    case "$s" in linux-vdso*|ld-linux*|ld-musl*) echo "skip:"; return;; esac
    if [ -n "${PMAP[$s]:-}" ]; then echo "prefix:${PMAP[$s]}"; return; fi
    if [ -n "${SYSPATH[$s]:-}" ]; then echo "system:${SYSPATH[$s]}"; return; fi
    echo "missing:"
}

walk_closure() {
    local queue=("${SEEDED[@]}") cur n rel
    declare -A seen=()
    while [ "${#queue[@]}" -gt 0 ]; do
        cur="${queue[0]}"; queue=("${queue[@]:1}")
        [ -n "${seen[$cur]:-}" ] && continue
        seen["$cur"]=1; IN_CLOSURE["$cur"]=1
        while read -r n; do
            [ -n "$n" ] || continue
            if [ -z "${RES[$n]:-}" ]; then
                RES[$n]="$(resolve "$n")"
                case "${RES[$n]}" in
                    prefix:*)
                        rel="${RES[$n]#prefix:}"
                        [ -n "${seen[$rel]:-}" ] || queue+=("$rel") ;;
                esac
            fi
        done < <(readelf -d "$PREFIX/$cur" 2>/dev/null \
                 | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p')
    done
}

host_pkg() { # absolute path -> owning package or '-'
    command -v dpkg >/dev/null 2>&1 || { echo "-"; return; }
    local p
    p="$(dpkg -S "$(readlink -f "$1")" 2>/dev/null | head -1 | cut -d: -f1)"
    [ -n "$p" ] || p="$(dpkg -S "$1" 2>/dev/null | head -1 | cut -d: -f1)"
    echo "${p:--}"
}

# ---------------------------------------------------------------------------
# Version floors: max GLIBC_/GLIBCXX_/CXXABI_ among *undefined* symbols.
# ---------------------------------------------------------------------------
floor_of() { # file -> "GLIBC_x GLIBCXX_y CXXABI_z" (maxes; may be empty)
    objdump -T "$1" 2>/dev/null \
        | awk '/\*UND\*/{for(i=1;i<=NF;i++) if ($i ~ /^\((GLIBC|GLIBCXX|CXXABI)_[0-9.]/) { sub(/[()]/,"",$i); gsub(/[()]/,"",$i); print $i }}' \
        | sort -Vu | awk -F_ '{m[$1]=$0} END{for(k in m) printf "%s ", m[k]}'
}

cmd_floor() {
    reset_globals
    make_prefix "$@"; collect_elves
    local f fl maxall="" g="" gx="" c=""
    printf '%-72s %s\n' "file" "max undefined version refs"
    for f in "${ELF[@]}"; do
        fl="$(floor_of "$PREFIX/$f")"
        [ -n "$fl" ] && printf '%-72s %s\n' "$f" "$fl"
        for tok in $fl; do
            case "$tok" in
                GLIBC_*)    [ "$tok" \> "$g" ] && g="$tok" ;;
                GLIBCXX_*)  [ "$tok" \> "$gx" ] && gx="$tok" ;;
                CXXABI_*)   [ "$tok" \> "$c" ] && c="$tok" ;;
            esac
        done
    done
    echo "== floors: ${g:--} ${gx:--} ${c:--}"
    echo "== host glibc: $(ldd --version 2>/dev/null | grep -oE '[0-9.]+' | head -1)"
}

# ---------------------------------------------------------------------------
# Curated headline matrix — the sonames whose distro story actually varies.
# Format per row: soname|noble|sid|tumbleweed|fedora|fd-25.08-runtime
# "none" = not packaged; a name = the providing package; "runtime" = in the
# flatpak runtime. Verified 2026-10-18 (see .devin/WEBKIT-PACKAGING.md).
# ---------------------------------------------------------------------------
matrix_row() {
    case "$1" in
        libWPEWebKit-2.0.so.1) echo "none|libwpewebkit-2.0-1|libWPEWebKit-2_0-1|copr-only*|none" ;;
        libwpe-1.0.so.1)       echo "none|libwpe-1.0-1|libwpe-1_0-1|libwpe|runtime?*" ;;
        libsoup-3.0.so.0)      echo "libsoup-3.0-0|libsoup-3.0-0|libsoup-3_0-0|libsoup3|runtime" ;;
        libgstreamer-1.0.so.0) echo "libgstreamer1.0-0|libgstreamer1.0-0|libgstreamer-1_0-0|gstreamer1|runtime" ;;
        libicuuc.so.*)         echo "none(icu74)|libicu78|libicu*|icu|runtime" ;;
        libxml2.so.*)          echo "none(abi12)|libxml2-16|libxml2-2|libxml2|runtime" ;;
        libjpeg.so.*)          echo "none(so.8)|libjpeg62-turbo|libjpeg62|libjpeg-turbo|runtime" ;;
        libwebp.so.*)          echo "libwebp7|libwebp7|libwebp7|libwebp|runtime" ;;
        libgbm.so.1)           echo "libgbm1|libgbm1|libgbm1|mesa-libgbm|runtime" ;;
        libseccomp.so.2)       echo "libseccomp2|libseccomp2|libseccomp2|libseccomp|runtime" ;;
        *) return 1 ;;
    esac
}

cmd_scan() {
    reset_globals
    make_prefix "$@"; collect_elves; pick_seeds; load_system_map; walk_closure

    echo "== wpe-deps scan =="
    echo "prefix: $PREFIX"
    printf 'seeds: %s\n' "${SEEDED[*]}"
    echo "(upstream dev tools MiniBrowser/WPEWebDriver are not seeded —"
    echo " they are not part of the embed payload; MiniBrowser alone drags"
    echo " in the legacy libWPEBackend-fdo)"

    echo; echo "-- payload: prefix files the NEEDED closure requires --"
    local rel n=0 sz
    for rel in $(printf '%s\n' "${!IN_CLOSURE[@]}" | sort); do
        sz="$(du -h "$PREFIX/$rel" 2>/dev/null | cut -f1)"
        printf '  %-66s %s\n' "$rel" "$sz"
        n=$((n+1))
    done
    echo "  ($n files)"

    echo; echo "-- soname resolution --"
    printf '  %-28s %-8s %s\n' SONAME VERDICT DETAIL
    local s res
    local nmis=0 nsys=0 npre=0
    for s in $(printf '%s\n' "${!RES[@]}" | sort); do
        res="${RES[$s]}"
        case "$res" in
            prefix:*) npre=$((npre+1))
                printf '  %-28s %-8s %s\n' "$s" bundle "${res#prefix:}" ;;
            system:*) nsys=$((nsys+1))
                printf '  %-28s %-8s %s (%s)\n' "$s" dep \
                    "${res#system:}" "$(host_pkg "${res#system:}")" ;;
            missing:*) nmis=$((nmis+1))
                printf '  %-28s %-8s\n' "$s" MISSING ;;
            skip:*) ;;
        esac
    done

    echo; echo "-- dlopen-only payload candidates (in prefix, outside NEEDED closure) --"
    local any=0
    for rel in "${ELF[@]}"; do
        [ -n "${IN_CLOSURE[$rel]:-}" ] && continue
        case "$rel" in
            *.so|*.so.*)
                printf '  %s\n' "$rel"; any=$((any+1)) ;;
        esac
    done
    [ "$any" -eq 0 ] && echo "  (none)"
    echo "  ($any files — plugin/module dirs are dlopened, not NEEDED; decide per feature)"

    echo; echo "-- PT_INTERP of prefix executables --"
    for rel in "${ELF[@]}"; do
        local interp
        interp="$(readelf -l "$PREFIX/$rel" 2>/dev/null \
                  | awk '/INTERP/{f=1} f&&/Requesting/{print;exit}' \
                  | grep -oE '/[^]]+' || true)"
        [ -n "$interp" ] || continue
        case "$interp" in
            "$PREFIX"|"$PREFIX"/*)
                [ -e "$interp" ] && echo "  $rel -> $interp [exists]" \
                                 || echo "  $rel -> $interp [ABSENT!]" ;;
            *)  if [ -e "$PREFIX$interp" ]; then
                    echo "  $rel -> $interp [in-prefix]"
                elif [ -e "$interp" ]; then
                    echo "  $rel -> $interp [host]"
                else
                    echo "  $rel -> $interp [ABSENT on host!]"
                fi ;;
        esac
    done

    echo; echo "-- floors --"
    local f fl g="" gx="" c="" tok
    for rel in $(printf '%s\n' "${!IN_CLOSURE[@]}"); do
        for tok in $(floor_of "$PREFIX/$rel"); do
            case "$tok" in
                GLIBC_*)    [ "$tok" \> "$g" ] && g="$tok" ;;
                GLIBCXX_*)  [ "$tok" \> "$gx" ] && gx="$tok" ;;
                CXXABI_*)   [ "$tok" \> "$c" ] && c="$tok" ;;
            esac
        done
    done
    echo "  payload max undefined refs: ${g:--} ${gx:--} ${c:--}"
    echo "  host glibc: $(ldd --version 2>/dev/null | grep -oE '[0-9.]+' | head -1)"

    echo; echo "-- headline soname x target-distro matrix (curated; verify per release) --"
    printf '  %-24s %-14s %-20s %-20s %-14s %s\n' \
        soname noble-24.04 debian-sid opensuse-tw fedora fd-25.08-rt
    for s in $(printf '%s\n' "${!RES[@]}" | sort); do
        local row
        row="$(matrix_row "$s" 2>/dev/null)" || continue
        IFS='|' read -r a b c2 dd e <<<"$row"
        printf '  %-24s %-14s %-20s %-20s %-14s %s\n' "$s" "$a" "$b" "$c2" "$dd" "$e"
    done

    echo; echo "== summary: $npre bundled, $nsys system-resolved, $nmis missing =="
    [ "$nmis" -eq 0 ] || note "$nmis SONAMEs unresolved — inspect before shipping"
    return 0
}

cmd_debs() {
    cmd_scan "$@"
    [ "${#DEB_FILE[@]}" -gt 0 ] || {
        note "input was not .debs — no per-deb verdicts"; return 0; }

    echo; echo "-- deb ship/drop verdicts --"
    declare -A USE=()
    local rel deb
    for rel in "${!IN_CLOSURE[@]}"; do
        deb="${DEB_FILE[$rel]:-}"
        [ -n "$deb" ] && USE["$deb"]="ship"
    done
    for rel in "${ELF[@]}"; do
        [ -n "${IN_CLOSURE[$rel]:-}" ] && continue
        deb="${DEB_FILE[$rel]:-}"
        [ -n "$deb" ] && [ -z "${USE[$deb]:-}" ] && USE["$deb"]="dlopen-only"
    done
    declare -A ALL=()
    for rel in "${!DEB_FILE[@]}"; do ALL["${DEB_FILE[$rel]}"]=1; done
    for deb in $(printf '%s\n' "${!ALL[@]}" | sort); do
        echo "  ${USE[$deb]:-DROP}  $deb"
    done
    echo "  (DROP = nothing in the runtime closure — headers, dev tools,"
    echo "   or deps already system-provided; dlopen-only = plugin payload)"
}

# ---------------------------------------------------------------------------
cmd_selftest() {
    command -v gcc >/dev/null || fail "selftest needs gcc"
    scratch
    local F="$SCRATCH/fixture"
    mkdir -p "$F/lib/wpetest-2.0/plugins" "$F/DEBIAN"

    cat > "$F/dep.c" <<'EOF'
double wpe_dep(double x);
double wpe_dep(double x) { extern double cos(double); return cos(x) + 1.0; }
EOF
    cat > "$F/core.c" <<'EOF'
double wpe_dep(double); double wpe_core(double);
double wpe_core(double x) { return wpe_dep(x) * 2.0; }
EOF
    cat > "$F/main.c" <<'EOF'
double wpe_core(double);
int main(void) { return (int)wpe_core(0.5) & 0; }
EOF
    cat > "$F/plug.c" <<'EOF'
int wpe_plugin(void); int wpe_plugin(void) { return 42; }
EOF
    cat > "$F/DEBIAN/control" <<'EOF'
Package: libwpetest1
Version: 1.0
Architecture: amd64
Maintainer: wpe-deps selftest
Description: fixture
EOF

    gcc -shared -fPIC -Wl,-soname,libwpetest-dep.so.1 \
        -o "$F/lib/libwpetest-dep.so.1" "$F/dep.c" -lm
    ln -s libwpetest-dep.so.1 "$F/lib/libwpetest-dep.so"
    gcc -shared -fPIC -Wl,-soname,libwpetest-core.so.1 \
        -o "$F/lib/libwpetest-core.so.1" "$F/core.c" -L"$F/lib" -lwpetest-dep
    ln -s libwpetest-core.so.1 "$F/lib/libwpetest-core.so"
    gcc -o "$F/lib/wpetest-2.0/WPETestProcess" "$F/main.c" \
        -L"$F/lib" -lwpetest-core -Wl,-rpath-link,"$F/lib"
    gcc -shared -fPIC -o "$F/lib/wpetest-2.0/plugins/libwpetest-plugin.so" \
        "$F/plug.c"
    rm -f "$F"/{dep,core,main,plug}.c
    if command -v dpkg-deb >/dev/null; then
        (cd "$F" && dpkg-deb --root-owner-group --build . \
            "$SCRATCH/libwpetest1.deb" >/dev/null 2>&1) || true
    fi

    local out
    out="$(cmd_scan "$F")"
    echo "$out" | grep -q 'libwpetest-core.so.1' \
        || fail "selftest: core lib not in payload"
    echo "$out" | grep -q 'libwpetest-dep.so.1' \
        || fail "selftest: dep lib not resolved in-prefix"
    echo "$out" | grep -qE 'libc\.so\.6 +dep' \
        || fail "selftest: libc not system-resolved"
    echo "$out" | grep -q 'plugins/libwpetest-plugin.so' \
        || fail "selftest: plugin not flagged dlopen-only"
    echo "$out" | grep -q 'WPETestProcess' \
        || fail "selftest: subprocess not seeded"
    note "selftest scan legs PASS"

    if [ -f "$SCRATCH/libwpetest1.deb" ]; then
        out="$(cmd_debs "$SCRATCH/libwpetest1.deb")"
        echo "$out" | grep -qE 'ship +libwpetest1|ship.*libwpetest1' \
            || fail "selftest: deb not judged ship"
        note "selftest debs leg PASS"
    fi
    note "selftest PASS"
}

# cmd_scan/cmd_debs share globals via subshell isolation in selftest —
# run them in the main shell; for selftest we exec a fresh instance.
if [ "${1:-}" = selftest-inner ]; then
    shift; "cmd_$1" "$@"; exit $?
fi

case "${1:-}" in
    scan)     shift; cmd_scan "$@" ;;
    debs)     shift; cmd_debs "$@" ;;
    floor)    shift; cmd_floor "$1" ;;
    selftest) exec env ARKTOS_PKG_KEEP="${ARKTOS_PKG_KEEP:-}" \
                  bash "$0" selftest-inner selftest ;;
    *) echo "usage: $0 [scan TARGET|debs TARGET|.debs...|floor TARGET|selftest]" >&2
       exit 2 ;;
esac
