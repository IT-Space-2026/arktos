#!/usr/bin/env bash
# pkg-flatpak.sh — PKG02 flatpak packaging driver.
#
# Builds com.arktos_browser.Arktos.yaml with flatpak-builder inside org.flatpak.Builder
# (user-local, no host flatpak-builder needed), produces an installable
# single-file bundle dist/com.arktos_browser.Arktos_<ver>.flatpak, installs it into
# the user flatpak installation, and verifies it launches offscreen with
# the engine + bundled tor reachable.
#
#   build [--clean]   export sources + run flatpak-builder -> repo
#   bundle            repo -> dist/*.flatpak single-file bundle
#   install           install the app into flatpak --user (bundle first,
#                     falls back to a file:// remote over the build repo)
#   verify            manifest sanity + installed payload + offscreen
#                     --quit-after-load + bundled-tor smoke
#   flavors           build+bundle+install+verify BOTH flavors
#                     (com.arktos_browser.Arktos + …NoServo) side-by-side
#   clean             remove flatpak/.build (state, build dir, repo)
#
# Plain `./.devin/pkg-flatpak.sh` runs build+bundle+install+verify.
#
# Flavors (SRV01): ARKTOS_FLAVOR=servo (default) builds the canonical
# manifest com.arktos_browser.Arktos.yaml with the servo-embed module;
# ARKTOS_FLAVOR=noservo builds the generated com.arktos_browser.ArktosNoServo.yaml
# (flatpak/gen-flavor-manifest.py — QtWebEngine only).  The two carry
# distinct app-ids so they install side-by-side; export dir, build dir
# and output repo are per-flavor so both can build in parallel against
# the shared download/module state.
#
# Source export: the working tree is copied via `git ls-files` (tracked +
# uncommitted non-ignored files — .git/, dist/, target/ never enter the
# sandbox build). `--clean` instead exports `git archive HEAD`, proving
# the committed tree is what a flathub-style build would see.
#
# Env: ARKTOS_PKG_JOBS (default 2), ARKTOS_VERSION (default 1.0),
#      ARKTOS_FLAVOR (servo|noservo, default servo),
#      ARKTOS_PKG_STATE overrides the flatpak-builder state dir (download
#      + module cache — point it at a prior build's state to reuse it),
#      ARKTOS_PKG_KEEP=1 keeps the export dir, SKIP_TOR_SMOKE=1 skips the
#      real-tor bootstrap check in verify.

set -euo pipefail

SRCROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILDER=org.flatpak.Builder
VER="${ARKTOS_VERSION:-1.0}"
JOBS="${ARKTOS_PKG_JOBS:-2}"
BUILDTOP="$SRCROOT/flatpak/.build"
STATE="${ARKTOS_PKG_STATE:-$BUILDTOP/state}"  # flatpak-builder state (dl/module cache)
DIST="$SRCROOT/dist"

fail() { echo "pkg-flatpak: FAIL: $*" >&2; exit 1; }
note() { echo "pkg-flatpak: $*"; }

# Per-flavor identity + output paths.  Both flavors share STATE (the
# download cache is flavor-independent and the module build dirs are
# content-keyed) while export/build-dir/repo split so two flavors can
# build concurrently without clobbering each other.
set_flavor() {
    FLAVOR="$1"
    case "$FLAVOR" in
        servo)   APPID=com.arktos_browser.Arktos ;;
        noservo) APPID=com.arktos_browser.ArktosNoServo ;;
        *) fail "unknown ARKTOS_FLAVOR '$FLAVOR' (servo|noservo)" ;;
    esac
    MANIFEST=$APPID.yaml
    REPO="$BUILDTOP/repo-$FLAVOR"
    BUILDDIR="$BUILDTOP/build-dir-$FLAVOR"
    EXPORTDIR="$BUILDTOP/src-$FLAVOR"
}
set_flavor "${ARKTOS_FLAVOR:-servo}"

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
        # ls-files --cached also lists files deleted from the worktree
        # (unstaged removals) — subtract them or tar fails on the
        # missing paths.
        (cd "$SRCROOT" \
            && comm -z -23 \
                <(git ls-files -z --cached --others --exclude-standard | sort -z) \
                <(git ls-files -z --deleted | sort -z)) \
            | (cd "$SRCROOT" && tar --null -cf - -T -) \
            | tar -x -C "$dest"
    fi
    [ -f "$dest/$MANIFEST" ] || fail "export has no $MANIFEST"
    note "exported $mode tree -> $dest ($(du -sh "$dest" | cut -f1))"
}

cmd_build() {
    local mode=dirty
    [ "${1:-}" = "--clean" ] && mode=clean
    local export="$EXPORTDIR"
    rm -rf "$export" "$BUILDDIR"
    cmd_export "$export" "$mode"
    mkdir -p "$STATE" "$REPO"

    note "flatpak-builder (via $BUILDER) — flavor=$FLAVOR app=$APPID jobs=$JOBS"
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
            arktos-pkg-local "file://$REPO"
        flatpak install --user -y arktos-pkg-local "$APPID"
        flatpak remote-delete --user --force arktos-pkg-local || true
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
    # the noservo manifest + metainfo are generated — refuse to verify
    # against a stale copy of the canonical servo manifest
    python3 "$SRCROOT/flatpak/gen-flavor-manifest.py" --check \
        || fail "generated NoServo flavor files drifted — re-run flatpak/gen-flavor-manifest.py"
    if command -v appstreamcli >/dev/null 2>&1; then
        appstreamcli validate --no-net \
            "$SRCROOT/flatpak/$APPID.metainfo.xml" \
            || note "metainfo validate reported issues (see above)"
    fi

    flatpak info "$APPID" >/dev/null 2>&1 \
        || fail "$APPID not installed — run install first"

    # -- icons: rename-icon must have produced app-id-named icons ---------
    fp_run --command=/bin/sh "$APPID" -c \
        "ls /app/share/icons/hicolor/*/apps/$APPID.* >/dev/null 2>&1" \
        || fail "no $APPID icons in the flatpak (rename-icon failed)"
    fp_run --command=/bin/sh "$APPID" -c \
        "grep -q '^Icon=$APPID\$' /app/share/applications/$APPID.desktop" \
        || fail "desktop file Icon= not rewritten to $APPID"
    note "icons + desktop Icon= present (renamed to $APPID)"

    # -- rust link-proof (RC10): the facade must be inside the binary ------
    # The manifest passes CONFIG+=rustcore explicitly, so a missing ac_init
    # means the rust SDK ext was lost and the no-rust fallback shipped —
    # the launch smoke below cannot tell the difference.  grep over the
    # binary's symtab works inside the runtime (no nm/binutils needed).
    fp_run --command=/bin/sh "$APPID" -c \
        'grep -q ac_init /app/bin/arktos' \
        || fail "facade symbol ac_init missing from the flatpak binary"
    note "facade linked: ac_init present in /app/bin/arktos"

    # -- flavor payload: servo-embed present iff the servo flavor ----------
    # ServoBackend dlopens the cdylib at runtime — the servo flavor must
    # ship it, the noservo flavor must not.
    if [ "$FLAVOR" = servo ]; then
        fp_run --command=/bin/sh "$APPID" -c \
            'test -f /app/lib/libservo_embed.so' \
            || fail "servo flavor missing /app/lib/libservo_embed.so"
        note "servo payload present: /app/lib/libservo_embed.so"
    else
        fp_run --command=/bin/sh "$APPID" -c \
            'test ! -e /app/lib/libservo_embed.so' \
            || fail "noservo flavor unexpectedly ships libservo_embed.so"
        note "noservo payload clean: no libservo_embed.so"
    fi

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
    export)  cmd_export "${2:-$EXPORTDIR}" "${3:-dirty}" ;;
    build)   cmd_build "${2:-}" ;;
    bundle)  cmd_bundle ;;
    install) cmd_install ;;
    verify)  cmd_verify ;;
    clean)   cmd_clean ;;
    all)     cmd_build "${2:-}"; cmd_bundle; cmd_install; cmd_verify ;;
    flavors)
        for f in servo noservo; do
            set_flavor "$f"
            note "=== flavor: $FLAVOR ($APPID) ==="
            cmd_build "${2:-}"; cmd_bundle; cmd_install; cmd_verify
        done ;;
    *) echo "usage: $0 [export [DIR [dirty|clean]]|build [--clean]|bundle|install|verify|clean|flavors|all]" >&2
       exit 2 ;;
esac
