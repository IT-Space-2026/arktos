#!/usr/bin/env bash
# pkg-rpm.sh — PKG03 RPM packaging driver (openSUSE-compatible spec).
#
# Produces dist/arktos-<ver>-<rel>.<arch>.rpm from rpm/arktos.spec. The
# package carries the SAME payload as the .deb (PKG01): the
# self-contained bundle from BuildProcess/bundle-linux.sh mapped onto
# the FHS — /usr/bin/arktos launcher + private Qt 6.12 / QtWebEngine
# under /usr/lib/arktos. The rust crates link statically (.a) so there
# are no rust .so artifacts to ship; the optional libservo_embed.so
# Servo backend rides along in $LIBDIR/lib when the tree built one.
#
# Subcommands:
#   build        cargo for missing rust .a archives (optional), then the
#                shared qmake+make leg via .devin/pkg-deb.sh build
#   stage DIR    populate the FHS tree under DIR — delegates the payload
#                layout to .devin/pkg-deb.sh stage so deb and rpm can
#                never drift apart
#   requires DIR print the Requires: capability lines the staged tree
#                actually needs (NEEDED minus bundled SONAMEs)
#   verify [S|R] [DIR] — spec sanity + %files/Requires coverage against
#                the staged tree (DIR defaults to rpm/buildroot); with
#                an .rpm argument, extract + ldd + headless launch when
#                rpm tooling exists
#   srctar       dist/arktos-<ver>.tar.gz — git ls-files export like
#                pkg-flatpak.sh (dirty tree; --clean uses git archive)
#   pack         rpmbuild -bb into dist/ (requires rpmbuild)
#   container    print the openSUSE container build command for hosts
#                without rpm tooling
#   clean        remove packaging artifacts (never distcleans src/)
#
# Plain `./.devin/pkg-rpm.sh` runs build + stage + verify, then pack
# when rpmbuild is installed. This dev host has no rpm toolchain, so
# the local verification is the spec+stage legs; the binary rpm build
# is verified via the container path.
#
# Env: QTDIR, ARKTOS_PKG_JOBS (default 2), ARKTOS_VERSION (default: the
#      spec's Version field),
#      ARKTOS_QMAKE_CONFIG, ARKTOS_PKG_KEEP=1.

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/.." && pwd)"
QTDIR="${QTDIR:-$HOME/Qt/6.12.0/gcc_64}"
JOBS="${ARKTOS_PKG_JOBS:-2}"
PKG=arktos
SPEC="$SRCROOT/rpm/arktos.spec"
# The source tarball name MUST equal Source0 (%{name}-%{version}.tar.gz),
# so the default comes from the spec itself — a stale fallback here is
# what produced `arktos-1.2.tar.gz: No such file or directory` in %prep
# on the v1.2 tag. ARKTOS_VERSION stays an explicit override.
VER="${ARKTOS_VERSION:-$(awk '/^Version:[[:space:]]/{print $2; exit}' "$SPEC")}"
BUILDROOT="$SRCROOT/rpm/buildroot"
DIST="$SRCROOT/dist"
LIBDIR=usr/lib/arktos   # bundle payload home inside the package

fail() { echo "pkg-rpm: FAIL: $*" >&2; exit 1; }
note() { echo "pkg-rpm: $*"; }

cmd_build() {
    # The rust crates are statically linked — build their .a archives
    # when the sources are here and cargo exists (mirrors the deb's
    # autodetect: no .a -> the qmake CONFIG is simply left off).
    local c d
    for c in rustdl rustcore; do
        d="$SRCROOT/src/$c"
        [ -f "$d/Cargo.toml" ] || continue
        [ -f "$d/target/release/libarktos_$c.a" ] && continue
        command -v cargo >/dev/null || continue
        (cd "$d" && cargo build --release --locked) \
            || fail "cargo build failed: $c"
    done
    QTDIR="$QTDIR" ARKTOS_PKG_JOBS="$JOBS" \
        "$SRCROOT/.devin/pkg-deb.sh" build
}

cmd_stage() {
    local D="$1"
    # One shared payload implementation: identical FHS tree for both
    # package formats, byte-for-byte consistent with the deb.
    QTDIR="$QTDIR" ARKTOS_PKG_JOBS="$JOBS" \
        "$SRCROOT/.devin/pkg-deb.sh" stage "$D"
    note "staged $D"
}

# rpm capability suffix for ELF SONAMEs on this arch.
soname_tag() {
    case "$(uname -m)" in
        *64*) echo "()(64bit)" ;;
        *)    echo "" ;;
    esac
}

# NEEDED entries of all ELF files in $1 minus the SONAMEs the payload
# itself provides and minus the interpreter — the rpm Requires surface.
scan_needs() {
    local D="$1" f
    local tmp; tmp="$(mktemp -d)"
    while IFS= read -r f; do
        file "$f" | grep -q 'ELF' || continue
        readelf -d "$f" 2>/dev/null \
            | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p' >>"$tmp/needed"
        readelf -d "$f" 2>/dev/null \
            | sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p' >>"$tmp/provided"
    done < <(find "$D" -type f -o -type l)
    sort -u "$tmp/provided" -o "$tmp/provided" 2>/dev/null || touch "$tmp/provided"
    touch "$tmp/needed"
    sort -u "$tmp/needed" | comm -23 - "$tmp/provided" \
        | grep -v '^ld-linux' || true
    rm -rf "$tmp"
}

cmd_requires() {
    local D="$1" tag s
    [ -d "$D/$LIBDIR" ] || fail "stage $D first"
    tag="$(soname_tag)"
    scan_needs "$D" | while IFS= read -r s; do
        echo "Requires:       $s$tag"
    done
}

# Strip the known rpm macros from a %files path for filesystem checks.
expand_path() {
    local p="$1"
    p="${p//\%{_mandir\}//usr/share/man}"
    p="${p//\%{_datadir\}//usr/share}"
    p="${p//\%{_bindir\}//usr/bin}"
    p="${p//\%{_docdir\}//usr/share/doc}"
    p="${p//\%{name\}/$PKG}"
    p="${p//\%{version\}/$VER}"
    echo "$p"
}

# Emit the resolved %files entries (one path per line) from the spec.
spec_files() {
    local S="$1" line tok
    awk '/^%files/{f=1;next}
         /^%(changelog|description|package|pre|post|preun|postun|check|trigger)/{f=0}
         f{print}' "$S" \
    | while IFS= read -r line; do
        for tok in $line; do
            case "$tok" in
                %defattr*|%attr*|%doc|%license|%dir|%config*|%ghost) ;;
                /%*|%*) expand_path "$tok" ;;
                *)  expand_path "$tok" ;;
            esac
        done
    done | grep -v '^$' | sed 's:/*$::' | sort -u
}

# Emit the spec's Requires values (capability text only).
spec_requires() {
    awk '/^Requires:/{sub(/^Requires:[ \t]*/,"");print}' "$1" | sort -u
}

# RC10 rust link-proof, shared by both verify paths (staged tree and
# extracted rpm).  A cargo-less build silently degrades to the no-rust
# fallback and still passes ldd + launch — assert the facade actually
# linked when the tree was configured for it.  src/Makefile records the
# qmake intent: it names libarktos_rustcore.a only while CONFIG+=rustcore
# is on, so a deliberate no-rust re-qmake skips this instead of failing.
# libservo_embed.so is the one rust artifact shipped as a file (dlopen'd
# by ServoBackend) — when the tree built it, it must be inside.
assert_rust_payload() {
    local D="$1"
    if grep -q 'libarktos_rustcore' "$SRCROOT/src/Makefile" 2>/dev/null; then
        # Byte-grep, not nm|grep -q: grep -q's early exit SIGPIPEs nm
        # mid-dump and pipefail reads that as failure.  "ac_init" only
        # enters the binary via the rustcore objects.
        grep -q 'ac_init' "$D/$LIBDIR/bin/arktos" \
            || fail "configured for rustcore but ac_init missing" \
                    "from $D/$LIBDIR/bin/arktos"
        note "facade linked: ac_init present in the payload binary"
    fi
    if ls "$SRCROOT"/spikes/servo/servo-embed/target/*/libservo_embed.so \
            >/dev/null 2>&1; then
        [ -f "$D/$LIBDIR/lib/libservo_embed.so" ] \
            || fail "libservo_embed.so built but missing from $D"
        note "servo-embed staged: $LIBDIR/lib/libservo_embed.so"
    fi
}

cmd_verify() {
    local arg="${1:-}"
    if [ -n "$arg" ] && [ "${arg%.rpm}" != "$arg" ]; then
        verify_rpm "$arg"; return
    fi
    local S="${arg:-$SPEC}" D="${2:-$BUILDROOT}"
    [ -f "$S" ] || fail "no spec at $S"
    [ -d "$D/$LIBDIR" ] || fail "no staged tree at $D (run: $0 stage $D)"

    # -- header / section sanity ---------------------------------------
    local tag
    for tag in Name Version Release Summary License Group URL Source0 \
               AutoReqProv; do
        grep -q "^$tag:" "$S" || fail "spec missing $tag:"
    done
    for tag in %prep %build %install %files %changelog; do
        grep -q "^$tag" "$S" || fail "spec missing section $tag"
    done
    grep -q '^BuildRequires:.*qt6-webengine-devel' "$S" \
        || fail "spec missing qt6-webengine-devel BuildRequires"
    # Bundled-runtime policy must hold: auto dep scanning off, an
    # explicit Requires surface present.
    grep -q '^AutoReqProv:[[:space:]]*no' "$S" \
        || fail "AutoReqProv must be off (bundled Qt would corrupt deps)"
    spec_requires "$S" | grep -q . || fail "spec has no Requires:"

    # -- %files coverage ------------------------------------------------
    # Every spec entry must resolve against the staged tree, and every
    # staged file must be owned by some entry (or sit under an owned dir).
    local entry rel missing=0
    while IFS= read -r entry; do
        rel="$D$entry"
        if ls -d $rel >/dev/null 2>&1 || compgen -G "$rel" >/dev/null; then
            :
        else
            note "  %files entry has no staged match: $entry"
            missing=$((missing+1))
        fi
    done < <(spec_files "$S")
    [ "$missing" -eq 0 ] || fail "$missing %files entries unmatched"

    local unowned=0 dirs f entries e
    entries="$(spec_files "$S")"
    dirs="$(echo "$entries" | while IFS= read -r f; do
        [ -d "$D$f" ] && echo "$f"; done; true)"
    while IFS= read -r f; do
        f="${f#$D}"
        local owned="" d
        for e in $entries; do
            # unquoted RHS: glob %files entries (arktos.1*) match too
            [[ "$f" == $e ]] && owned=1 && break
        done
        for d in $dirs; do
            case "$f" in "$d"/*) owned=1;; esac
        done
        [ -n "$owned" ] && continue
        note "  staged file not owned by %files: $f"
        unowned=$((unowned+1))
    done < <(find "$D" -type f -o -type l)
    [ "$unowned" -eq 0 ] || fail "$unowned staged files not covered"

    # -- Requires coverage ----------------------------------------------
    local computed="$D.requires.actual" spec_req="$D.requires.spec"
    scan_needs "$D" | sed "s/$/$(soname_tag)/" | sort -u >"$computed"
    spec_requires "$S" >"$spec_req"
    local drift
    drift=$(comm -23 "$computed" "$spec_req")
    [ -z "$drift" ] || {
        echo "$drift" | sed 's/^/pkg-rpm:   missing Requires: /' >&2
        rm -f "$computed" "$spec_req"
        fail "spec Requires does not cover the payload's NEEDED set"
    }
    drift=$(comm -13 "$computed" "$spec_req")
    [ -z "$drift" ] || {
        echo "$drift" | sed 's/^/pkg-rpm:   stale Requires (not NEEDED): /' >&2
        rm -f "$computed" "$spec_req"
        fail "spec Requires lists capabilities nothing needs"
    }
    local nreq; nreq=$(wc -l <"$spec_req")
    rm -f "$computed" "$spec_req"

    assert_rust_payload "$D"

    command -v rpmlint >/dev/null \
        && rpmlint "$S" \
        || note "rpmlint not installed — static checks only"

    note "verify PASS (spec+stage): $S vs $D"
    note "  header/sections ok, %files covers staged tree," \
         "Requires == NEEDED-minus-bundled ($nreq caps)"
}

verify_rpm() {
    local rpm="$1" R
    [ -f "$rpm" ] || fail "no .rpm at $rpm"
    command -v rpm2cpio >/dev/null \
        || fail "no rpm tooling on this host — run 'pkg-rpm.sh container'"
    R="$(mktemp -d /tmp/arktos-rpmverify.XXXXXX)"
    [ -n "${ARKTOS_PKG_KEEP:-}" ] || trap "rm -rf '$R'" EXIT
    (cd "$R" && rpm2cpio "$rpm" | cpio -idm --quiet)

    # payload coverage: the spec's %files paths must all be in the rpm
    local entry missing=0
    while IFS= read -r entry; do
        if ls -d "$R$entry" >/dev/null 2>&1 \
            || compgen -G "$R$entry" >/dev/null; then :; else
            note "  rpm payload missing: $entry"; missing=$((missing+1))
        fi
    done < <(spec_files "$SPEC")
    [ "$missing" -eq 0 ] || fail "$missing %files entries missing from rpm"

    # ldd clean with the dev Qt masked, then a headless launch from the
    # installed layout (same legs as pkg-deb.sh verify).
    local m
    m=$(bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
        env -i PATH=/usr/bin:/bin \
        LD_LIBRARY_PATH="$R/$LIBDIR/lib" \
        ldd "$R/$LIBDIR/bin/arktos" \
            "$R/$LIBDIR/libexec/QtWebEngineProcess" 2>&1 \
        | grep -cE 'not found' || true)
    [ "$m" -eq 0 ] || fail "$m unresolved libs in rpm payload"

    assert_rust_payload "$R"

    mkdir -p "$R/home"
    sed "s|/usr/lib/arktos|$R/usr/lib/arktos|g" \
        "$R/usr/bin/arktos" > "$R/arktos-rebased"
    chmod 0755 "$R/arktos-rebased"
    bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
        env -i HOME="$R/home" PATH=/usr/bin:/bin \
        QT_QPA_PLATFORM=offscreen \
        QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
        "$R/arktos-rebased" --quit-after-load >"$R/launch.log" 2>&1 || {
            cat "$R/launch.log" >&2
            fail "installed binary did not launch (log above)"
        }
    note "verify PASS: $rpm"
    note "  payload complete, ldd clean, --quit-after-load ok" \
         "(rebased prefix, QTDIR masked)"
}

cmd_srctar() {
    local out="$DIST/$PKG-$VER.tar.gz" tmp mode="${1:-dirty}"
    mkdir -p "$DIST"
    tmp="$(mktemp -d /tmp/arktos-srctar.XXXXXX)"
    [ -n "${ARKTOS_PKG_KEEP:-}" ] || trap "rm -rf '$tmp'" EXIT
    mkdir -p "$tmp/$PKG-$VER"
    if [ "$mode" = clean ]; then
        git -C "$SRCROOT" archive HEAD | tar -x -C "$tmp/$PKG-$VER"
    else
        (cd "$SRCROOT" \
            && git ls-files -z --cached --others --exclude-standard) \
            | (cd "$SRCROOT" && tar --null -cf - -T -) \
            | tar -x -C "$tmp/$PKG-$VER"
    fi
    [ -f "$tmp/$PKG-$VER/rpm/arktos.spec" ] \
        || fail "export has no rpm/arktos.spec"
    tar -czf "$out" -C "$tmp" "$PKG-$VER"
    note "wrote $out"
}

cmd_pack() {
    if ! command -v rpmbuild >/dev/null; then
        cmd_container
        fail "rpmbuild not installed on this host"
    fi
    [ -x "$QTDIR/bin/qmake" ] || fail "no qmake at $QTDIR/bin/qmake (set QTDIR)"
    cmd_srctar
    local T="$DIST/rpmbuild"
    mkdir -p "$T/BUILD" "$T/RPMS" "$T/SOURCES" "$T/SPECS" "$T/SRPMS"
    cp "$DIST/$PKG-$VER.tar.gz" "$T/SOURCES/"
    cp "$SPEC" "$T/SPECS/$PKG.spec"
    # --nodeps: the spec's BuildRequires are system-Qt/Fedora package names
    # kept for documentation/OBS; the payload uses $QTDIR (aqt-installed Qt),
    # so on hosts like Ubuntu CI none of them can be satisfied.
    (cd "$SRCROOT" && rpmbuild -bb --nodeps \
        --define "_topdir $T" \
        --define "qtdir $QTDIR" \
        "$T/SPECS/$PKG.spec")
    cp "$T"/RPMS/*/"$PKG"-*.rpm "$DIST/"
    note "built $(ls -t "$DIST"/"$PKG"-*.rpm | head -1)"
}

cmd_container() {
    cat <<EOF
pkg-rpm: no rpm toolchain on this host. Build inside an openSUSE
container — bind-mount the tree and the relocatable Qt:

  podman run --rm -v "$SRCROOT:/src" -v "$HOME/Qt:/qt:ro" \\
      opensuse/tumbleweed bash -lc '
        zypper --non-interactive install -y rpm-build gcc-c++ make git binutils file cargo rust &&
        cd /src && QTDIR=/qt/6.12.0/gcc_64 ./.devin/pkg-rpm.sh pack &&
        ./.devin/pkg-rpm.sh verify dist/arktos-*.rpm'

(docker works identically.) zypper and the cargo crate fetch in
%build need network inside the container; the qt6-*-devel
BuildRequires are the system-Qt names for documentation/OBS — the
bundled payload uses the mounted Qt so they need not be installed for
this local rpmbuild. Result: dist/arktos-$VER-0.*.rpm
EOF
}

cmd_clean() {
    rm -rf "$BUILDROOT" "$DIST/rpmbuild"
}

case "${1:-all}" in
    build)     cmd_build ;;
    stage)     cmd_stage "$2" ;;
    requires)  cmd_requires "$2" ;;
    verify)    cmd_verify "${2:-}" ${3:+"$3"} ;;
    srctar)    cmd_srctar "${2:-dirty}" ;;
    pack)      cmd_pack ;;
    container) cmd_container ;;
    clean)     cmd_clean ;;
    all)
        cmd_build
        cmd_stage "$BUILDROOT"
        cmd_verify "$SPEC" "$BUILDROOT"
        if command -v rpmbuild >/dev/null; then
            cmd_pack
        else
            cmd_container | sed 's/^/  /'
            note "spec + staged payload verified; rpm build needs the" \
                 "container path above (no rpmbuild on this host)"
        fi
        ;;
    *) echo "usage: $0 [build|stage DIR|requires DIR|verify [SPEC|RPM] [DIR]|srctar [--clean]|pack|container|clean|all]" >&2
       exit 2 ;;
esac
