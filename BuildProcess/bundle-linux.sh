#!/usr/bin/env bash
# bundle-linux.sh — build a self-contained, relocatable Linux bundle of Arora.
#
# Produces:
#   dist/Arora-<ver>-linux-<arch>/
#     arora                     wrapper (sets LD_LIBRARY_PATH, execs bin/arora)
#     bin/arora + bin/qt.conf   real binary; qt.conf anchors Qt paths at ".."
#     lib/                      every libQt6*/libicu* dep (ldd-resolved)
#     libexec/QtWebEngineProcess
#     plugins/                  platform + wayland + tls + imageformat plugins
#     resources/                icudtl.dat, qtwebengine_*.pak, v8 snapshot
#     translations/             Qt .qm + qtwebengine_locales/
#     share/                    app .qm, useragents.xml, desktop file, icons, man
#
# Usage:  ./BuildProcess/bundle-linux.sh [output-dir]
# Env:    QTDIR (defaults to ~/Qt/6.11.3/gcc_64), ARORA_VERSION (default 0.2),
#         ARORA_BUNDLE_JOBS (make parallelism when a build is needed).
#
# The bundle is verified by .devin/check-bundle.sh.

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/.." && pwd)"
QTDIR="${QTDIR:-$HOME/Qt/6.11.3/gcc_64}"
ARCH="$(uname -m)"
VER="${ARORA_VERSION:-0.2}"
OUT="${1:-$SRCROOT/dist/Arora-$VER-linux-$ARCH}"

[ -x "$QTDIR/bin/qmake" ] || { echo "error: no qmake at $QTDIR/bin/qmake (set QTDIR)" >&2; exit 1; }

if [ ! -x "$SRCROOT/arora" ]; then
    echo "bundle-linux: arora binary missing — building first"
    (cd "$SRCROOT/src" && "$QTDIR/bin/qmake" src.pro && make -j"${ARORA_BUNDLE_JOBS:-2}")
fi

rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib" "$OUT/libexec" "$OUT/plugins" "$OUT/resources" \
         "$OUT/translations" "$OUT/share/arora/locale" "$OUT/share/applications" \
         "$OUT/share/pixmaps" "$OUT/share/man/man1" \
         "$OUT/share/icons/hicolor/scalable/apps"

# --- binary -------------------------------------------------------------
install -m0755 "$SRCROOT/arora" "$OUT/bin/arora"

# --- plugins (allowlist — no designer/help/qml tooling) ------------------
PLUGIN_DIRS="platforms platforminputcontexts platformthemes imageformats \
iconengines tls networkinformation xcbglintegrations generic sqldrivers \
printsupport wayland-decoration-client wayland-graphics-integration-client \
wayland-shell-integration"
for d in $PLUGIN_DIRS; do
    [ -d "$QTDIR/plugins/$d" ] || continue
    mkdir -p "$OUT/plugins/$d"
    cp -a "$QTDIR/plugins/$d/"*.so "$OUT/plugins/$d/" 2>/dev/null || true
done

# --- QtWebEngine runtime --------------------------------------------------
install -m0755 "$QTDIR/libexec/QtWebEngineProcess" "$OUT/libexec/QtWebEngineProcess"
cp -a "$QTDIR/resources/." "$OUT/resources/"

# --- shared libraries -----------------------------------------------------
# Resolve every Qt/icu dependency of the binary, QtWebEngineProcess and the
# shipped plugins to a fixpoint, keyed by SONAME.
declare -A HAVE
collect() {
    local f soname target
    for f in "$@"; do
        while read -r soname target; do
            [ -n "$soname" ] || continue
            target="${target//libexec\/..\//}"          # normalize $QTDIR/libexec/../lib
            target="$(cd "$(dirname "$target")" && pwd)/$(basename "$target")"
            case "$target" in "$QTDIR"/lib/*) ;; *) continue;; esac
            [ -n "${HAVE[$soname]:-}" ] && continue
            HAVE[$soname]=1
            cp -L "$target" "$OUT/lib/$soname"
        done < <(LD_LIBRARY_PATH="$QTDIR/lib" ldd "$f" 2>/dev/null \
                 | awk '/=> \//{print $1, $3}')
    done
}
collect "$OUT/bin/arora" "$OUT/libexec/QtWebEngineProcess" "$OUT"/plugins/*/*.so
# Second pass over newly-copied libs for transitive deps (e.g. libQt6XcbQpa).
collect "$OUT"/lib/*.so

# --- qt.conf: anchor QLibraryInfo at the bundle root -----------------------
# Defaults resolve from Prefix: lib/, libexec/, plugins/, translations/ and
# <Data>/resources — exactly this layout.
cat > "$OUT/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
EOF

# --- Qt translations (qt_*.qm etc. + webengine locales) --------------------
cp -a "$QTDIR/translations/"qt_*.qm "$QTDIR/translations/"qtbase_*.qm \
      "$QTDIR/translations/"qtwebengine*.qm "$OUT/translations/" 2>/dev/null || true
if [ -d "$QTDIR/translations/qtwebengine_locales" ]; then
    mkdir -p "$OUT/translations/qtwebengine_locales"
    cp -a "$QTDIR/translations/qtwebengine_locales/." "$OUT/translations/qtwebengine_locales/"
fi

# --- app data (relocatable: installedDataDirectory prefers ../share/arora) -
cp -a "$SRCROOT/src/.qm/locale/." "$OUT/share/arora/locale/" 2>/dev/null || true
install -m0644 "$SRCROOT/src/useragent/useragents.xml" "$OUT/share/arora/"

# --- freedesktop metadata --------------------------------------------------
install -m0644 "$SRCROOT/BuildProcess/arora.desktop" "$OUT/share/applications/"
install -m0644 "$SRCROOT/src/data/arora.xpm" "$OUT/share/pixmaps/"
install -m0644 "$SRCROOT/src/data/arora.svg" "$OUT/share/icons/hicolor/scalable/apps/"
for sz in 16x16 32x32 128x128 512x512; do
    mkdir -p "$OUT/share/icons/hicolor/$sz/apps"
    install -m0644 "$SRCROOT/src/data/$sz/arora.png" "$OUT/share/icons/hicolor/$sz/apps/"
done
gzip -9 -c "$SRCROOT/src/data/arora.1" > "$OUT/share/man/man1/arora.1.gz"

# --- licenses ---------------------------------------------------------------
install -m0644 "$SRCROOT/LICENSE.GPL2" "$SRCROOT/LICENSE.GPL3" "$SRCROOT/THIRD-PARTY-NOTICES" "$OUT/"

# --- wrapper ----------------------------------------------------------------
cat > "$OUT/arora" <<'EOF'
#!/bin/sh
# Arora bundle launcher — uses only the bundled Qt/WebEngine runtime.
DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
export LD_LIBRARY_PATH="$DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$DIR/bin/arora" "$@"
EOF
chmod 0755 "$OUT/arora"

echo "bundle: $OUT ($(du -sh "$OUT" | cut -f1), $(find "$OUT" -type f | wc -l) files)"
