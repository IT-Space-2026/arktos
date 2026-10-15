#!/usr/bin/env bash
# pkg-deb.sh — PKG01 .deb packaging driver.
#
# Produces dist/arora_<ver>_<arch>.deb from the qmake build. The package
# carries the same self-contained payload as BuildProcess/bundle-linux.sh
# (Qt 6.12 runtime + QtWebEngineProcess + plugins/resources, all under
# /usr/lib/arora) because the tree builds against a user-local Qt that is
# newer than any distro series — the deb therefore bundles Qt rather than
# depending on libqt6* packages. The optional Rust pieces (arora-rustdl,
# arora-rustcore) are STATICALLY linked into the binary via their .a
# archives (see src/rustdl/rustdl.pri, src/rustcore/rustcore.pri), so
# there are no rust .so artifacts to ship. Servo (libservo_embed) does
# not exist in this tree yet — the ENG03 spike gates it.
#
# Subcommands (also the targets debian/rules delegates to):
#   build    — qmake (rust-crate CONFIG auto-detected from built .a
#              artifacts, override via ARORA_QMAKE_CONFIG) + make -C src
#   stage D  — populate FHS package tree under D (usr/bin, usr/lib/arora,
#              usr/share/{arora,applications,icons,pixmaps,man,doc})
#   pack D   — shlibdeps -> substvars, gencontrol, md5sums, dpkg-deb
#   verify F — extract the .deb, ldd it with the dev Qt masked, and run
#              --quit-after-load from the installed layout headless
#   clean    — remove packaging artifacts (never distcleans src/)
#
# Plain `./.devin/pkg-deb.sh` runs build+stage+pack+verify.
# dpkg-buildpackage -b -us -uc -rfakeroot -d works too via debian/rules
# (artifacts then land in the parent dir as usual).
#
# Env: QTDIR, ARORA_PKG_JOBS (default 2), ARORA_QMAKE_CONFIG,
#      ARORA_PKG_KEEP=1 keeps the verify scratch dir.

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/.." && pwd)"
QTDIR="${QTDIR:-$HOME/Qt/6.12.0/gcc_64}"
JOBS="${ARORA_PKG_JOBS:-2}"
PKG=arora
DIST="$SRCROOT/dist"
LIBDIR=usr/lib/arora   # bundle payload home inside the package

fail() { echo "pkg-deb: FAIL: $*" >&2; exit 1; }
note() { echo "pkg-deb: $*"; }

[ -x "$QTDIR/bin/qmake" ] || fail "no qmake at $QTDIR/bin/qmake (set QTDIR)"

cmd_build() {
    # Enable a rust crate only when its release archive already exists —
    # mirrors what the in-tree build was configured with; use
    # ARORA_QMAKE_CONFIG to force a different set.
    local cfg=""
    [ -f "$SRCROOT/src/rustdl/target/release/libarora_rustdl.a" ] \
        && cfg="$cfg CONFIG+=rustdl"
    [ -f "$SRCROOT/src/rustcore/target/release/libarora_rustcore.a" ] \
        && cfg="$cfg CONFIG+=rustcore"
    cfg="${ARORA_QMAKE_CONFIG:-$cfg}"

    local need_qmake=0 tok
    [ -f "$SRCROOT/src/Makefile" ] || need_qmake=1
    for tok in $cfg; do
        grep -q -- "$tok" "$SRCROOT/src/Makefile" 2>/dev/null || need_qmake=1
    done
    if [ "$need_qmake" -eq 1 ]; then
        note "configuring: qmake src.pro$cfg"
        (cd "$SRCROOT/src" && "$QTDIR/bin/qmake" src.pro $cfg)
    fi
    (cd "$SRCROOT/src" && make -j"$JOBS")
    [ -x "$SRCROOT/arora" ] || fail "build produced no arora binary"
}

cmd_stage() {
    local D="$1" B
    [ -x "$SRCROOT/arora" ] || cmd_build
    rm -rf "$D"
    B="$(mktemp -d /tmp/arora-deb.XXXXXX)/bundle"
    ARORA_BUNDLE_JOBS="$JOBS" QTDIR="$QTDIR" \
        "$SRCROOT/BuildProcess/bundle-linux.sh" "$B" >/dev/null
    [ -x "$B/bin/arora" ] || fail "bundle produced no binary"

    # /usr/lib/arora keeps the bundle's relative layout verbatim
    # (bin/../{lib,libexec,plugins,resources,translations} all resolve).
    install -d "$D/$LIBDIR" "$D/usr/bin"
    cp -a "$B/bin" "$B/lib" "$B/libexec" "$B/plugins" "$B/resources" \
          "$B/translations" "$D/$LIBDIR/"
    # Arora has no QtSql usage — the optional SQL driver plugins would
    # only pull undeclarable deps (libmysqlclient/libpq/oci/...).
    rm -rf "$D/$LIBDIR/plugins/sqldrivers"

    # App data is FHS /usr/share/arora; the binary's bundle-first lookup
    # (<bindir>/../share/arora) reaches it through this symlink.
    install -d "$D/usr/share/arora" "$D/$LIBDIR/share"
    cp -a "$B/share/arora/." "$D/usr/share/arora/"
    ln -s ../../../share/arora "$D/$LIBDIR/share/arora"

    # Bundled tor expert bundle -> $LIBDIR/lib/tor (+ lib/data geoip),
    # matching TorManager's <bindir>/../lib/tor and binaryDir/../data
    # searches.  ARORA_SKIP_TOR_BUNDLE=1 opts out (offline builds).
    if [ -z "${ARORA_SKIP_TOR_BUNDLE:-}" ]; then
        T="$(mktemp -d /tmp/arora-tor.XXXXXX)"
        ARORA_TOR_PREFIX="$T" sh "$SRCROOT/BuildProcess/fetch-tor.sh" >/dev/null \
            || fail "tor expert bundle fetch failed"
        install -d "$D/$LIBDIR/lib"
        cp -a "$T/tor" "$D/$LIBDIR/lib/tor"
        cp -a "$T/data" "$D/$LIBDIR/lib/data" 2>/dev/null || true
        rm -rf "$T"
        [ -x "$D/$LIBDIR/lib/tor/tor" ] || fail "staged tor missing"
    fi

    # freedesktop + doc payload
    install -d "$D/usr/share/applications" "$D/usr/share/pixmaps" \
               "$D/usr/share/man/man1" "$D/usr/share/doc/$PKG"
    cp -a "$B/share/applications/." "$D/usr/share/applications/"
    cp -a "$B/share/pixmaps/." "$D/usr/share/pixmaps/"
    cp -a "$B/share/icons" "$D/usr/share/"
    cp -a "$B/share/man/." "$D/usr/share/man/"
    install -m0644 "$SRCROOT/debian/copyright" "$D/usr/share/doc/$PKG/copyright"
    install -m0644 "$SRCROOT/LICENSE.GPL2" "$SRCROOT/LICENSE.GPL3" \
                   "$SRCROOT/THIRD-PARTY-NOTICES" "$D/usr/share/doc/$PKG/"
    gzip -9 -c "$SRCROOT/debian/changelog" \
        > "$D/usr/share/doc/$PKG/changelog.gz"

    # /usr/bin/arora — fixed-path launcher for the installed layout.
    cat > "$D/usr/bin/arora" <<'EOF'
#!/bin/sh
# Arora launcher — the package ships its Qt/WebEngine runtime privately.
ARORA_HOME=/usr/lib/arora
export LD_LIBRARY_PATH="$ARORA_HOME/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$ARORA_HOME/bin/arora" "$@"
EOF
    chmod 0755 "$D/usr/bin/arora"

    rm -rf "$(dirname "$B")"
    note "staged $D ($(du -sh "$D" | cut -f1))"
}

# SONAMEs that may be absent on the build host but are still real
# runtime deps (libxcb-cursor is required by Qt 6's xcb platform plugin,
# yet not installed here). Anything unmapped fails loudly so Depends
# never silently misses one.
soname_package() {
    case "$1" in
        libxcb-cursor.so.0) echo libxcb-cursor0 ;;
        libevent-2.1.so.7) echo libevent-2.1-7 ;;
        *) return 1 ;;
    esac
}

# ldd-resolved system libs -> owning packages, for the ELF files that
# dpkg-shlibdeps can't analyze (they NEED a lib absent on this host).
extra_deps() {
    local D="$1"; shift
    local f lib missing pkgs=""
    for f in "$@"; do
        while read -r lib; do
            [ -n "$lib" ] || continue
            case "$lib" in "$D"/*|"$QTDIR"/*) continue;; esac
            pkgs="$pkgs $(dpkg -S "$lib" 2>/dev/null \
                | cut -d: -f1 | tr -d ' ' | tr ',' '\n')"
        done < <(LD_LIBRARY_PATH="$D/$LIBDIR/lib" ldd "$f" 2>/dev/null \
                 | awk '/=> \//{print $3}')
        while read -r soname; do
            [ -n "$soname" ] || continue
            local p
            p="$(soname_package "$soname")" \
                || fail "no package mapping for missing SONAME $soname ($f)"
            pkgs="$pkgs $p"
        done < <(LD_LIBRARY_PATH="$D/$LIBDIR/lib" ldd "$f" 2>/dev/null \
                 | awk '/not found/{print $1}')
    done
    echo "$pkgs" | tr ' ' '\n' | sed '/^$/d' | sort -u | paste -sd, - \
        | sed 's/,/, /g'
}

cmd_pack() {
    local D="$1" OUT="${2:-$DIST}"
    [ -d "$D/$LIBDIR" ] || fail "stage $D first"
    cd "$SRCROOT"
    install -d "$D/DEBIAN" "$OUT"

    # Split the staged ELF files: dpkg-shlibdeps analyzes the ones whose
    # NEEDED set fully resolves; the rest (e.g. libqxcb needing
    # libxcb-cursor, absent on this host) get their deps computed by the
    # ldd/dpkg -S scan so nothing is silently dropped. --ignore-missing-
    # info covers the Qt libs the binary's dev RUNPATH resolves to
    # ~/Qt — their SONAMEs are satisfied by the bundled copies.
    local ok_elfs=() bad_elfs=() f miss
    while read -r f; do
        miss=$(LD_LIBRARY_PATH="$SRCROOT/$D/$LIBDIR/lib" ldd "$f" 2>/dev/null \
               | grep -c 'not found' || true)
        if [ "$miss" -eq 0 ]; then ok_elfs+=("$f"); else bad_elfs+=("$f"); fi
    done < <(find "$D" -type f -exec file {} + | awk -F': ' '/ELF/{print $1}')
    [ "${#ok_elfs[@]}" -gt 0 ] || fail "no resolvable ELF files staged"
    [ "${#bad_elfs[@]}" -eq 0 ] \
        || note "shlibdeps-excluded (missing SONAME on build host): ${bad_elfs[*]#$D/}"

    rm -f debian/substvars
    dpkg-shlibdeps --ignore-missing-info -Tdebian/substvars \
        -l"$SRCROOT/$D/$LIBDIR/lib" "${ok_elfs[@]}" 2>&1 \
        | grep -vE "could not be identified|useless|similar" || true

    local extra=""
    [ "${#bad_elfs[@]}" -eq 0 ] || extra=$(extra_deps "$D" "${bad_elfs[@]}")
    if [ -n "$extra" ]; then
        if grep -q "^shlibs:Depends=" debian/substvars; then
            sed -i "s|^shlibs:Depends=\(.*\)|shlibs:Depends=\1, $extra|" \
                debian/substvars
        else
            echo "shlibs:Depends=$extra" >> debian/substvars
        fi
    fi
    grep -q "^shlibs:Depends=" debian/substvars \
        || fail "no Depends computed"

    (cd "$D" && find . -type f ! -path './DEBIAN/*' -printf '%P\n' \
        | sort | xargs md5sum) > "$D/DEBIAN/md5sums"
    dpkg-gencontrol -p"$PKG" -P"$D"
    dpkg-deb --root-owner-group --build "$D" "$OUT"
    note "built $(ls -t "$OUT"/arora_*_*.deb | head -1)"
}

cmd_verify() {
    local deb="$1" R missing
    [ -f "$deb" ] || fail "no .deb at $deb"
    R="$(mktemp -d /tmp/arora-debverify.XXXXXX)"
    [ -n "${ARORA_PKG_KEEP:-}" ] || trap "rm -rf '$R'" EXIT

    # -- control sanity -------------------------------------------------
    dpkg-deb -f "$deb" Package Version Architecture >/dev/null
    dpkg-deb -f "$deb" Depends | grep -q . || fail "empty Depends"
    dpkg-deb -f "$deb" Installed-Size | grep -q . || fail "no Installed-Size"

    # -- real dpkg install into a scratch instdir ------------------------
    # Seeded with the host's dpkg status so dependency resolution sees
    # exactly what this machine provides. No maintainer scripts ship, so
    # the unpack is side-effect free; --force-depends completes the
    # configure so we can assert installed-state even when the host lacks
    # a declared dep (libxcb-cursor0 today — apt would pull it on a real
    # install).
    local db="$R/db" root="$R/root"
    mkdir -p "$db/info" "$db/updates" "$db/triggers" "$root"
    cp /var/lib/dpkg/status "$db/status"
    if ! dpkg --force-not-root --instdir="$root" --admindir="$db" \
            --log="$R/dpkg.log" -i "$deb" >"$R/dpkg.out" 2>&1; then
        note "dpkg -i reported dep problems on this host (expected —" \
             "scratch db cannot apt-fetch missing deps); retrying" \
             "with --force-depends"
        grep "depends on" "$R/dpkg.out" | sed 's/^/pkg-deb:   /' >&2
        dpkg --force-not-root --force-depends --instdir="$root" \
            --admindir="$db" --log="$R/dpkg.log" -i "$deb" \
            >>"$R/dpkg.out" 2>&1 || {
            cat "$R/dpkg.out" >&2; fail "dpkg install failed"; }
    fi
    dpkg --admindir="$db" -s "$PKG" 2>/dev/null \
        | grep -q "Status: install ok installed" \
        || fail "package did not reach installed state"
    [ -x "$root/usr/bin/arora" ] || fail "no usr/bin/arora installed"
    [ -f "$root/$LIBDIR/bin/qt.conf" ] || fail "no qt.conf installed"
    [ -f "$root/usr/share/applications/arora.desktop" ] \
        || fail "no desktop file installed"
    [ -f "$root/usr/share/icons/hicolor/128x128/apps/arora.png" ] \
        || fail "no hicolor icon installed"
    [ -f "$root/usr/share/icons/hicolor/scalable/apps/arora.svg" ] \
        || fail "no scalable icon installed"
    [ -f "$root/usr/share/pixmaps/arora.xpm" ] \
        || fail "no pixmap icon installed"
    [ -f "$root/usr/share/doc/$PKG/copyright" ] \
        || fail "no copyright installed"
    if [ -x "$root/$LIBDIR/lib/tor/tor" ]; then
        "$root/$LIBDIR/lib/tor/tor" --version | grep -q "^Tor version" \
            || fail "bundled tor does not execute"
        note "bundled tor: $("$root/$LIBDIR/lib/tor/tor" --version | head -1)"
    fi
    note "dpkg install: $(dpkg --admindir="$db" -s "$PKG" | grep '^Status:')"

    # -- ldd clean with the dev Qt hidden --------------------------------
    lddout=$(bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
        env -i PATH=/usr/bin:/bin \
        LD_LIBRARY_PATH="$root/$LIBDIR/lib" \
        ldd "$root/$LIBDIR/bin/arora" \
            "$root/$LIBDIR/libexec/QtWebEngineProcess" 2>&1)
    missing=$(printf '%s\n' "$lddout" | grep -cE 'not found' || true)
    if [ "$missing" -ne 0 ]; then
        printf '%s\n' "$lddout" | grep -E 'not found' >&2
        fail "$missing unresolved libs in package"
    fi

    # -- launch from the installed layout, headless ----------------------
    # dpkg -i into / needs root, so the wrapper's /usr/lib/arora prefix is
    # rebased into the scratch root — same payload, same layout.
    mkdir -p "$R/home"
    sed "s|/usr/lib/arora|$root/usr/lib/arora|g" \
        "$root/usr/bin/arora" > "$R/arora-rebased"
    chmod 0755 "$R/arora-rebased"
    bwrap --dev-bind / / --tmpfs "$QTDIR" -- \
        env -i HOME="$R/home" PATH=/usr/bin:/bin \
        QT_QPA_PLATFORM=offscreen \
        QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
        "$R/arora-rebased" --quit-after-load \
        >"$R/launch.log" 2>&1 || {
            cat "$R/launch.log" >&2
            fail "installed binary did not launch (log above)"
        }
    note "verify PASS: $deb"
    note "  dpkg install ok, ldd clean, --quit-after-load ok" \
         "(rebased prefix, QTDIR masked)"
}

cmd_clean() {
    rm -rf "$SRCROOT/debian/arora" "$SRCROOT/debian/files" \
           "$SRCROOT"/debian/substvars "$SRCROOT"/debian/*.substvars
}

case "${1:-all}" in
    build)   cmd_build ;;
    stage)   cmd_stage "$2" ;;
    pack)    cmd_pack "$2" ${3:+"$3"} ;;
    verify)  cmd_verify "$2" ;;
    clean)   cmd_clean ;;
    all)
        cmd_build
        cmd_stage debian/arora
        cmd_pack debian/arora
        cmd_verify "$(ls -t "$DIST"/arora_*_*.deb | head -1)"
        ;;
    *) echo "usage: $0 [build|stage DIR|pack DIR [OUT]|verify DEB|clean|all]" >&2
       exit 2 ;;
esac
