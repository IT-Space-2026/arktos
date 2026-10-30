#!/usr/bin/env python3
"""Generate flatpak-builder sources for Arktos's vendored Rust deps.

Writes two manifests fragments next to this script:

  flatpak/cargo-sources.json        — one archive source per registry
                                      crate in src/rustdl/Cargo.lock,
                                      src/rustcore/Cargo.lock and
                                      src/adblock/rust/Cargo.lock,
                                      unpacked under cargo/vendor/ in the
                                      module build dir.
  flatpak/cargo-sources-servo.json  — same treatment for
                                      spikes/servo/servo-embed/Cargo.lock
                                      (SRV01), plus its git deps: the
                                      lock pins libservo + 54 sibling
                                      crates to one servo.git commit, so
                                      the fragment clones the repo once
                                      (type: git, commit-pinned), copies
                                      each member crate into
                                      cargo/vendor/, and drops in the
                                      normalized Cargo.toml `cargo
                                      vendor` produces — workspace-member
                                      manifests are not standalone until
                                      their `.workspace = true` keys are
                                      resolved.

Crates that re-resolve their own manifest at build time (the mozjs_*
cbindgen shims — `cbindgen::generate(".")` in build.rs resolves the
crate's SHIPPED Cargo.lock against the vendor dir) get their nested
lock's registry packages vendored too; detection is any vendored crate
with a root Cargo.lock whose build.rs mentions cbindgen or
MetadataCommand.

Both fragments end with an inline source that writes .cargo/config.toml
into the module build dir: crates.io (and each git source) is redirected
to the vendored tree and, combined with CARGO_NET_OFFLINE=true, the build
is offline and lockfile-exact — cargo still verifies each registry
crate's embedded .cargo-checksum.json against Cargo.lock.

Regenerate after touching a Cargo.lock:

    ./flatpak/gen-cargo-sources.py

Needs cargo plus a warm dep cache (cargo metadata and cargo vendor are
run --offline): anyone touching a lock has just built the crate, so the
checkouts are already under ~/.cargo/{registry,git}.
"""

import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_LOCKS = [
    ROOT / "src/rustdl/Cargo.lock",
    ROOT / "src/rustcore/Cargo.lock",
    ROOT / "src/adblock/rust/Cargo.lock",
]
SERVO_LOCK = ROOT / "spikes/servo/servo-embed/Cargo.lock"
OUT = ROOT / "flatpak/cargo-sources.json"
SERVO_OUT = ROOT / "flatpak/cargo-sources-servo.json"

CRATE_URL = "https://static.crates.io/crates/{name}/{name}-{version}.crate"
DEST = "cargo/vendor"
GIT_CACHE = "flatpak-cargo/git"  # upstream flatpak-cargo-generator layout

PKG_RE = re.compile(
    r"\[\[package\]\]\n"
    r'name = "(?P<name>[^"]+)"\n'
    r'version = "(?P<version>[^"]+)"\n'
    r'source = "(?P<source>[^"]+)"\n'
    r'(?:checksum = "(?P<checksum>[0-9a-f]{64})"\n)?'
)


def lock_packages(lock):
    """Every sourced package in a Cargo.lock: (name, version, source, checksum)."""
    return [
        (m["name"], m["version"], m["source"], m["checksum"])
        for m in PKG_RE.finditer(lock.read_text())
    ]


def registry_sources(locks, extra=None):
    """Archive + checksum-stub pair per registry crate, deduped."""
    seen = dict(extra or {})
    for lock in locks:
        found = False
        for name, version, source, checksum in lock_packages(lock):
            if not source.startswith("registry+"):
                continue
            found = True
            if not checksum:
                raise RuntimeError(
                    f"registry crate {name} {version} in "
                    f"{lock} has no checksum"
                )
            seen[(name, version)] = checksum
        if not found:
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
    return sources, len(seen)


def git_packages(lock):
    """(name, version, repo_url, query, commit) per git-sourced package."""
    pkgs = []
    for name, version, source, _c in lock_packages(lock):
        if not source.startswith("git+"):
            continue
        ref, _, commit = source.partition("#")
        url, _, query = ref[4:].partition("?")
        pkgs.append((name, version, url, query, commit))
    return pkgs


def cargo(args, cwd):
    return subprocess.run(
        ["cargo", *args, "--offline"], cwd=cwd, check=True,
        stdout=subprocess.PIPE,
    ).stdout


def lock_support(lock_dir):
    """(members, commits, normalized, nested) for one lock's crate.

    members      — git package name -> member dir inside its repo checkout
    commits      — git package name -> locked commit
    normalized   — vendored dir name -> normalized Cargo.toml contents
                   (cargo vendor resolves .workspace = true so a
                   workspace member becomes standalone)
    nested       — {(name, version): checksum} from Cargo.locks shipped
                   inside crates that re-resolve their own manifest at
                   build time (cbindgen/cargo-metadata codegen — the
                   mozjs_* shim family runs `cbindgen::generate(".")`
                   from build.rs, resolving the crate's own lockfile
                   against the vendor dir; its versions must be vendored
                   too or the inner cargo metadata call fails offline)
    """
    meta = json.loads(cargo(
        ["metadata", "--format-version", "1",
         "--manifest-path", lock_dir / "Cargo.toml"], lock_dir))
    members, commits = {}, {}
    for pkg in meta["packages"]:
        source = pkg.get("source") or ""
        if not source.startswith("git+"):
            continue
        manifest = Path(pkg["manifest_path"]).resolve()
        # ~/.cargo/git/checkouts/<db>/<short-sha>/<member...>/Cargo.toml
        parts = manifest.parts
        i = parts.index("checkouts")
        members[pkg["name"]] = str(Path(*parts[i + 3:-1]))
        commits[pkg["name"]] = source.rpartition("#")[2]

    with tempfile.TemporaryDirectory() as tmp:
        cargo(["vendor", "--manifest-path", lock_dir / "Cargo.toml",
               f"{tmp}/vendor"], lock_dir)
        normalized = {
            d.name: (d / "Cargo.toml").read_text()
            for d in Path(f"{tmp}/vendor").iterdir()
            if (d / "Cargo.toml").exists()
        }
        nested = {}
        for d in Path(f"{tmp}/vendor").iterdir():
            build_rs, nested_lock = d / "build.rs", d / "Cargo.lock"
            if not (build_rs.is_file() and nested_lock.is_file()):
                continue
            if not re.search(r"cbindgen|MetadataCommand",
                             build_rs.read_text(errors="replace")):
                continue
            n = 0
            for name, version, source, checksum in lock_packages(
                    nested_lock):
                if source.startswith("registry+") and checksum:
                    nested[(name, version)] = checksum
                    n += 1
            print(f"nested lock {d.name}: {n} pkgs", file=sys.stderr)

    return members, commits, normalized, nested


def git_sources(pkgs, members, commits, normalized):
    """Vendor-shape sources for every git package in a lock.

    Mirrors upstream flatpak-cargo-generator: one git source per
    repo+commit into flatpak-cargo/git/<repo>-<sha7>, then per crate a
    shell copy of its member dir into cargo/vendor/<name> plus the
    normalized Cargo.toml and an unverified checksum stub (the git
    commit itself is the integrity pin).
    """
    sources = []
    seen_repos = set()
    made_vendor = False
    for name, version, url, query, commit in pkgs:
        repo_key = (url, commit)
        repo_name = f"{Path(url).stem}-{commit[:7]}"
        if repo_key not in seen_repos:
            seen_repos.add(repo_key)
            sources.append(
                {
                    "type": "git",
                    "url": url,
                    "commit": commit,
                    "dest": f"{GIT_CACHE}/{repo_name}",
                }
            )
        if name not in members or commits[name] != commit:
            raise RuntimeError(
                f"cargo metadata has no git checkout for {name}@{commit} "
                "(cargo metadata/vendor --offline need the dep cached)"
            )
        if name not in normalized \
                or f'version = "{version}"' not in normalized[name]:
            raise RuntimeError(
                f"cargo vendor produced no {name} {version}/")
        member = members[name]
        if not made_vendor:
            made_vendor = True
            # the shell copies below run before any archive source can
            # create the vendor tree — make it first
            sources.append(
                {"type": "shell", "commands": [f"mkdir -p {DEST}"]}
            )
        sources.extend(
            [
                {
                    "type": "shell",
                    "commands": [
                        f'cp -r --reflink=auto '
                        f'"{GIT_CACHE}/{repo_name}/{member}" '
                        f'"{DEST}/{name}"'
                    ],
                },
                {
                    "type": "inline",
                    "contents": normalized[name],
                    "dest": f"{DEST}/{name}",
                    "dest-filename": "Cargo.toml",
                },
                {
                    "type": "inline",
                    "contents": json.dumps({"package": None, "files": {}}),
                    "dest": f"{DEST}/{name}",
                    "dest-filename": ".cargo-checksum.json",
                },
            ]
        )
    return sources


def cargo_config(locks):
    """The .cargo/config.toml redirect, as `cargo vendor` would emit."""
    lines = [
        '[source.crates-io]',
        'replace-with = "vendored-sources"',
        '',
    ]
    seen = set()
    for lock in locks:
        for _name, _ver, url, query, commit in git_packages(lock):
            key = f"git+{url}" + (f"?{query}" if query else "")
            if key in seen:
                continue
            seen.add(key)
            lines.append(f'[source."{key}"]')
            lines.append(f'git = "{url}"')
            for field in ("tag", "rev", "branch"):
                m = re.search(rf"(?:^|&){field}=([^&]+)", query)
                if m:
                    lines.append(f'{field} = "{m.group(1)}"')
                    break
            lines.append('replace-with = "vendored-sources"')
            lines.append("")
    lines += [
        "[source.vendored-sources]",
        f'directory = "{DEST}"',
        "",
    ]
    return {
        "type": "inline",
        "contents": "\n".join(lines),
        "dest": ".cargo/",
        "dest-filename": "config.toml",
    }


def main() -> int:
    nested = {}
    for lock in APP_LOCKS:
        _m, _c, _n, lock_nested = lock_support(lock.parent)
        nested.update(lock_nested)
    if [p for lock in APP_LOCKS for p in git_packages(lock)]:
        raise RuntimeError(
            "app locks gained git deps — extend the generator like "
            "SERVO_LOCK's path"
        )
    sources, nreg = registry_sources(APP_LOCKS, nested)
    sources.append(cargo_config(APP_LOCKS))
    OUT.write_text(json.dumps(sources, indent=2) + "\n")

    members, commits, normalized, nested = lock_support(SERVO_LOCK.parent)
    servo_sources, nreg_servo = registry_sources([SERVO_LOCK], nested)
    pkgs = git_packages(SERVO_LOCK)
    nservo_git = 0
    if pkgs:
        # git checkouts must land before the shell copies that use them
        servo_sources = git_sources(pkgs, members, commits, normalized) \
            + servo_sources
        nservo_git = len(pkgs)
    servo_sources.append(cargo_config([SERVO_LOCK]))
    SERVO_OUT.write_text(json.dumps(servo_sources, indent=2) + "\n")

    print(f"{nreg} app crates + config -> {OUT}", file=sys.stderr)
    print(f"{nreg_servo} servo crates + {nservo_git} git crates + config "
          f"-> {SERVO_OUT}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
