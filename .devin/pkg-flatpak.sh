#!/usr/bin/env bash
# pkg-flatpak.sh — PKG02 flatpak packaging driver.
#
# Builds io.arora.Arora.yaml with flatpak-builder inside org.flatpak.Builder
# (user-local, no host flatpak-builder needed), produces an installable
# single-file bundle dist/io.arora.Arora_<ver>.flatpak, installs it into
# the user flatpak installation, and verifies it launches offscreen with
# the engine + bundled tor reachable.
#
#   build [--clean]   export sources + run flatpak-builder -> repo
#   bundle            repo -> dist/*.flatpak single-file bundle
#   install           install the app into flatpak --user (bundle first,
#                     falls back to a file:// remote over the build repo)
#   verify            manifest sanity + installed payload + offscreen
#                     --quit-after-load + bundled-tor smoke
#   clean             remove flatpak/.build (state, build dir, repo)
#
# Plain `./.devin/pkg-flatpak.sh` runs build+bundle+install+verify.
#
# Source export: the working tree is copied via `git ls-files` (tracked +
# uncommitted non-ignored files — .git/, dist/, target/ never enter the
# sandbox build). `--clean` instead exports `git archive HEAD`, proving
# the committed tree is what a flathub-style build would see.
#
# Env: ARORA_PKG_JOBS (default 2), ARORA_VERSION (default 0.2),
#      ARORA_PKG_KEEP=1 keeps the export dir, SKIP_TOR_SMOKE=1 skips the
#      real-tor bootstrap check in verify.

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/.." && pwd)"
APPID=io.arora.Arora
MANIFEST=$APPID.yaml
BUILDER=org.flatpak.Builder
VER="${ARORA_VERSION:-0.2}"
JOBS="${ARORA_PKG_JOBS:-2}"
BUILDTOP="$SRCROOT/flatpak/.build"
STATE="$BUILDTOP/state"          # persistent flatpak-builder state (dl cache)
REPO="$BUILDTOP/repo"
BUILDDIR="$BUILDTOP/build-dir"
DIST="$SRCROOT/dist"

fail() { echo "pkg-flatpak: FAIL: $*" >&2; exit 1; }
note() { echo "pkg-flatpak: $*"; }

builder() {
    flatpak run --file-forwarding "$BUILDER" "$@"
}

# Copy the sources the dir-module sees into a clean export dir. Without
# this the manifest's `type: dir path: .` would drag the ~2.8G worktree
# (.git aside: dist/, cargo target/, .obj) into every build.
cmd_export() {
    local dest="$1" mode="${2:-dirty}"
    mkdir -p "$dest"
    if [ "$mode" = clean ]; then
        git -C "$SRCROOT" archive HEAD | tar -x -C "$dest"
    else
        (cd "$SRCROOT" \
            && git ls-files -z --cached --others --exclude-standard) \
            | (cd "$SRCROOT" && tar --null -cf - -T -) \
            | tar -x -C "$dest"
    fi
    [ -f "$dest/$MANIFEST" ] || fail "export has no $MANIFEST"
    note "exported $mode tree -> $dest ($(du -sh "$dest" | cut -f1))"
}

cmd_build() {
    local mode=dirty
    [ "${1:-}" = "--clean" ] && mode=clean
    local export="$BUILDTOP/src"
    rm -rf "$export" "$BUILDDIR"
    cmd_export "$export" "$mode"
    mkdir -p "$STATE" "$REPO"

    note "flatpak-builder (via $BUILDER) — jobs=$JOBS"
    builder --force-clean --jobs="$JOBS" \
        --state-dir="$STATE" --repo="$REPO" \
        "$BUILDDIR" "$export/$MANIFEST"
    [ -d "$REPO/refs" ] || ls "$REPO" >/dev/null
    note "build done; repo at $REPO"
}

cmd_bundle() {
    mkdir -p "$DIST"
    local out="$DIST/${APPID}_${VER}_x86_64.flatpak"
    flatpak build-bundle "$REPO" "$out" "$APPID" >/dev/null
    note "bundle: $out ($(du -h "$out" | cut -f1))"
}

cmd_install() {
    # Try the single-file bundle first — that is the artifact users get.
    # Unsigned bundles may refuse on some flatpak versions; fall back to
    # installing from the build repo via a throwaway file:// remote.
    local bundle
    bundle="$(ls -t "$DIST"/${APPID}_*.flatpak 2>/dev/null | head -1 || true)"
    if [ -n "$bundle" ] \
        && flatpak install --user -y "$bundle" >/dev/null 2>&1; then
        note "installed from bundle $bundle"
    else
        [ -d "$REPO" ] || fail "no repo at $REPO — run build first"
        note "bundle install unavailable — installing via file:// remote"
        flatpak remote-add --user --if-not-exists --no-gpg-verify \
            arora-pkg-local "file://$REPO"
        flatpak install --user -y arora-pkg-local "$APPID"
        flatpak remote-delete --user --force arora-pkg-local || true
    fi
    flatpak info "$APPID" >/dev/null || fail "app did not install"
    note "installed: $(flatpak info "$APPID" | grep -E 'ID:|Branch:|Origin:|Commit:' | tr -s ' ' | tr '\n' ' ')"
}

fp_run() {
    flatpak run --env=QT_QPA_PLATFORM=offscreen \
        --env=QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu "$@"
}

cmd_verify() {
    # -- manifest sanity --------------------------------------------------
    builder --show-manifest "$SRCROOT/$MANIFEST" >/dev/null
    note "manifest parses"
    if command -v appstreamcli >/dev/null 2>&1; then
        appstreamcli validate --no-net \
            "$SRCROOT/flatpak/$APPID.metainfo.xml" \
            || note "metainfo validate reported issues (see above)"
    fi

    flatpak info "$APPID" >/dev/null 2>&1 \
        || fail "$APPID not installed — run install first"

    # -- icons: rename-icon must have produced app-id-named icons ---------
    fp_run --command=/bin/sh "$APPID" -c \
        'ls /app/share/icons/hicolor/*/apps/io.arora.Arora.* >/dev/null 2>&1' \
        || fail "no io.arora.Arora icons in the flatpak (rename-icon failed)"
    fp_run --command=/bin/sh "$APPID" -c \
        'grep -q "^Icon=io.arora.Arora$" /app/share/applications/io.arora.Arora.desktop' \
        || fail "desktop file Icon= not rewritten to io.arora.Arora"
    note "icons + desktop Icon= present (renamed to $APPID)"

    # -- payload: bundled tor present + runs in the sandbox ----------------
    fp_run --command=/bin/sh "$APPID" -c \
        'test -x /app/lib/tor/tor && test -f /app/lib/data/geoip' \
        || fail "tor payload missing (/app/lib/tor, /app/lib/data)"
    fp_run --env=LD_LIBRARY_PATH=/app/lib/tor --command=/app/lib/tor/tor \
        "$APPID" --version | grep -q "^Tor version" \
        || fail "bundled tor does not execute in the sandbox"
    note "bundled tor executes: $(fp_run --env=LD_LIBRARY_PATH=/app/lib/tor --command=/app/lib/tor/tor "$APPID" --version | head -1)"

    # -- engine: offscreen launch of the installed app ----------------------
    local log="$BUILDTOP/launch.log"
    mkdir -p "$BUILDTOP"
    fp_run "$APPID" --quit-after-load >"$log" 2>&1 \
        || { cat "$log" >&2; fail "--quit-after-load failed in flatpak"; }
    note "--quit-after-load PASS (QtWebEngine up inside the sandbox)"

    # -- tor path: real daemon bootstrap + tor-window assertions ------------
    if [ -n "${SKIP_TOR_SMOKE:-}" ]; then
        note "SKIP_TOR_SMOKE set — skipping real-tor bootstrap smoke"
    else
        log="$BUILDTOP/tor-smoke.log"
        if ! timeout 300 flatpak run \
                --env=QT_QPA_PLATFORM=offscreen \
                --env=QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
                "$APPID" --tor --tor-window-smoke >"$log" 2>&1; then
            cat "$log" >&2
            fail "tor-window smoke failed inside flatpak"
        fi
        tail -5 "$log" | sed 's/^/pkg-flatpak:   /'
        note "--tor --tor-window-smoke PASS inside flatpak"
    fi

    note "verify PASS: $APPID"
}

cmd_clean() {
    rm -rf "$BUILDTOP"
}

case "${1:-all}" in
    export)  cmd_export "${2:-$BUILDTOP/src}" "${3:-dirty}" ;;
    build)   cmd_build "${2:-}" ;;
    bundle)  cmd_bundle ;;
    install) cmd_install ;;
    verify)  cmd_verify ;;
    clean)   cmd_clean ;;
    all)     cmd_build "${2:-}"; cmd_bundle; cmd_install; cmd_verify ;;
    *) echo "usage: $0 [export [DIR [dirty|clean]]|build [--clean]|bundle|install|verify|clean|all]" >&2
       exit 2 ;;
esac
