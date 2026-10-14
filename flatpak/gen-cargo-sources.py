#!/usr/bin/env python3
"""Generate flatpak-builder 'archive' sources for Arora's vendored Rust deps.

Reads src/rustdl/Cargo.lock and src/rustcore/Cargo.lock and emits
cargo-sources.json: one archive source per registry crate, unpacked under
cargo/vendor/ in the module build dir.  flatpak/cargo-config.toml (installed
as .cargo/config.toml by the manifest) then redirects crates.io to that
vendor directory, so `cargo build --release` runs fully offline with the
lockfile-pinned, checksum-verified crate set — the same policy the in-tree
build gets from Cargo.lock.

Regenerate after touching a Cargo.lock:

    ./flatpak/gen-cargo-sources.py > flatpak/cargo-sources.json
"""

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOCKS = [ROOT / "src/rustdl/Cargo.lock", ROOT / "src/rustcore/Cargo.lock"]
CRATE_URL = "https://static.crates.io/crates/{name}/{name}-{version}.crate"
DEST = "cargo/vendor"

PKG_RE = re.compile(
    r"\[\[package\]\]\n"
    r'name = "(?P<name>[^"]+)"\n'
    r'version = "(?P<version>[^"]+)"\n'
    r'source = "registry\+[^"]*"\n'
    r'checksum = "(?P<checksum>[0-9a-f]{64})"'
)


def main() -> int:
    seen = {}
    for lock in LOCKS:
        text = lock.read_text()
        for m in PKG_RE.finditer(text):
            seen[(m["name"], m["version"])] = m["checksum"]
        if not PKG_RE.search(text):
            print(f"warning: no registry crates parsed from {lock}",
                  file=sys.stderr)

    sources = []
    for (name, version), checksum in sorted(seen.items()):
        sources.append(
            {
                "type": "archive",
                # .crate is a gzipped tar; flatpak-builder's format
                # sniffer does not know the extension, so name it
                # explicitly
                "archive-type": "tar-gzip",
                "url": CRATE_URL.format(name=name, version=version),
                "sha256": checksum,
                # keep the .crate's top-level name-version/ dir — the
                # default strip-components=1 would flatten every crate
                # into one dir
                "strip-components": 0,
                "dest": DEST,
            }
        )
        # .crate archives do not carry .cargo-checksum.json — cargo
        # synthesizes it in `cargo vendor`.  The empty files map is the
        # canonical stub (same as flatpak-cargo-generator emits): the
        # package sha256 is already verified by flatpak-builder at
        # download time and cross-checked against Cargo.lock.
        sources.append(
            {
                "type": "inline",
                "contents": json.dumps(
                    {"package": checksum, "files": {}}
                ),
                "dest": f"{DEST}/{name}-{version}",
                "dest-filename": ".cargo-checksum.json",
            }
        )
    json.dump(sources, sys.stdout, indent=2)
    sys.stdout.write("\n")
    print(f"{len(seen)} crates -> {len(sources)} sources", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
