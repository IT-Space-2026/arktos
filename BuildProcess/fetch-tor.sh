#!/bin/sh
# fetch-tor.sh — download the Tor expert bundle (the `tor` daemon +
# pluggable transports + geoip data, BSD-3-Clause licensed) and
# install it where TorManager's binary search finds it:
# the per-user application data directory (no root needed).
#
#   BuildProcess/fetch-tor.sh [version] [destdir]
#
# destdir defaults to ${XDG_DATA_HOME:-~/.local/share}/Arora/Arora/tor
# so the binary lands at <AppDataLocation>/tor/tor.  Set
# ARORA_TOR_PREFIX to override.  The download is verified against the
# sha256sums-unsigned-build.txt published on dist.torproject.org.
set -eu

VERSION=${1:-15.0.24}

case "$(uname -s)" in
    Linux)  os=linux ;;
    Darwin) os=macos ;;
    *)      echo "unsupported OS: $(uname -s)" >&2; exit 1 ;;
esac
case "$(uname -m)" in
    x86_64|amd64)   arch=x86_64 ;;
    aarch64|arm64)  arch=aarch64 ;;
    i?86)           arch=i686 ;;
    *)              echo "unsupported arch: $(uname -m)" >&2; exit 1 ;;
esac

DEST=${ARORA_TOR_PREFIX:-${XDG_DATA_HOME:-$HOME/.local/share}/Arora/Arora}/tor
TARBALL="tor-expert-bundle-${os}-${arch}-${VERSION}.tar.gz"
BASE="https://dist.torproject.org/torbrowser/${VERSION}"
SUMS="sha256sums-unsigned-build.txt"

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

echo "fetching $BASE/$TARBALL"
curl -fSL --progress-bar -o "$work/$TARBALL" "$BASE/$TARBALL"
curl -fsSL -o "$work/$SUMS" "$BASE/$SUMS"

expected=$(grep " $TARBALL\$" "$work/$SUMS" | awk '{print $1}')
if [ -z "$expected" ]; then
    echo "no sha256 entry for $TARBALL in $SUMS" >&2
    exit 1
fi
actual=$(sha256sum "$work/$TARBALL" | awk '{print $1}')
if [ "$expected" != "$actual" ]; then
    echo "checksum mismatch: expected $expected got $actual" >&2
    exit 1
fi
echo "sha256 verified: $actual"

mkdir -p "$DEST"
tar -xzf "$work/$TARBALL" -C "$work"
cp -R "$work/tor/." "$DEST/"
mkdir -p "$DEST/../data"
cp -R "$work/data/." "$DEST/../data/" 2>/dev/null || true
cp -R "$work/docs" "$DEST/../docs" 2>/dev/null || true
chmod 700 "$DEST" "$DEST/.." 2>/dev/null || true

echo "installed tor to $DEST/tor"
"$DEST/tor" --version | head -1
