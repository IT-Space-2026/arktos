#!/bin/sh
# ENG03 spike link wrapper — gcc driver forced onto lld.
#
# A debug cdylib over libservo (~700 crates incl mozjs/webrender)
# makes GNU ld want multiple GB of memory; the rustup toolchain's
# bundled rust-lld does the same link in a fraction. rust-lld is a
# multi-call binary: invoked under the name ld.lld it selects the
# ELF/GNU driver itself, so all we need is a dir where ld.lld resolves
# and a -B prefix that lets gcc's -fuse-ld=lld find it.
#
# Referenced from .cargo/config.toml as target.*.linker — a link-time
# only setting, so it does NOT dirty cargo's dep fingerprints.
set -eu

SYSROOT="$(rustc --print sysroot)"
LLD_DIR="$SYSROOT/lib/rustlib/x86_64-unknown-linux-gnu/bin"
CACHE="$HOME/.cache/arora-spike-lld"
mkdir -p "$CACHE"
ln -sf "$LLD_DIR/rust-lld" "$CACHE/ld.lld"
exec gcc -B"$CACHE" -fuse-ld=lld "$@"
