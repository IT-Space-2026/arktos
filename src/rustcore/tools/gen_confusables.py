#!/usr/bin/env python3
"""Generate src/confusables_table.rs from Unicode confusables.txt (UTS #39).

Usage:
    python3 tools/gen_confusables.py confusables.txt > src/confusables_table.rs

The skeleton the lookalike detector compares is always matched against
Tranco domain names, so only mappings whose FULLY RESOLVED target is
domain-legal ASCII [a-z0-9_-] are usable — every other row is dropped
(a non-ASCII target can never converge on an ASCII Tranco entry).
Targets are resolved transitively (fixpoint, cycle-safe) so chains like
X -> LATIN NON-ASCII -> ASCII still land, and ASCII-lowercased so both
sides of the comparison meet in lowercase space.
"""

import sys

ALLOWED = frozenset(b"abcdefghijklmnopqrstuvwxyz0123456789_-")


def parse(path):
    table = {}
    version = "unknown"
    for line in open(path, encoding="utf-8"):
        if "Version:" in line:
            version = line.split("Version:", 1)[1].split()[0]
            continue
        line = line.split("#")[0].strip()
        if not line:
            continue
        parts = [p.strip() for p in line.split(";")]
        if len(parts) < 2:
            continue
        try:
            src = int(parts[0], 16)
            tgt = tuple(int(t, 16) for t in parts[1].split())
        except ValueError:
            continue
        if src not in table:  # first row wins; sources are unique upstream
            table[src] = tgt
    return table, version


def resolve(table, tgt, depth=0):
    """Apply the table to every codepoint in tgt until fixpoint.

    Returns the resolved codepoint tuple, or None when the chain exceeds
    the hop cap (a cycle can only churn — never converge)."""
    if depth > 8:
        return None
    out = []
    changed = False
    for cp in tgt:
        sub = table.get(cp)
        if sub is None or sub == (cp,):
            out.append(cp)
        else:
            changed = True
            out.extend(sub)
    if not changed:
        return tuple(out)
    return resolve(table, tuple(out), depth + 1)


def main():
    table, version = parse(sys.argv[1])
    rows = []
    for src in sorted(table):
        resolved = resolve(table, table[src])
        if resolved is None or not resolved:
            continue
        if any(cp > 0x7F for cp in resolved):
            continue  # non-ASCII target can never match a Tranco key
        lowered = bytes(
            cp + 32 if 0x41 <= cp <= 0x5A else cp for cp in resolved
        )
        if len(lowered) > 16:
            continue  # no domain char needs a longer expansion
        if all(b in ALLOWED for b in lowered):
            rows.append((src, lowered))

    w = sys.stdout.write
    w("//! GENERATED FILE — do not edit by hand.\n")
    w("//! Source: Unicode confusables.txt %s (UTS #39 security data,\n" % version)
    w("//! https://www.unicode.org/Public/security/latest/confusables.txt)\n")
    w("//! Regenerate: python3 tools/gen_confusables.py confusables.txt")
    w(" > src/confusables_table.rs\n")
    w("//!\n")
    w("//! (source codepoint -> domain-legal ASCII skeleton bytes).\n")
    w("//! Only mappings whose transitively-resolved target is entirely\n")
    w("//! [a-z0-9_-] are kept — a skeleton is only ever compared against\n")
    w("//! ASCII Tranco entries, so rows resolving to anything else can\n")
    w("//! never converge.  Sorted by source for binary search.\n\n")
    w("pub(crate) static CONFUSABLES: &[(u32, &[u8])] = &[\n")
    for src, tgt in rows:
        w("    (0x%04X, b\"%s\"),\n" % (src, tgt.decode("ascii")))
    w("];\n")
    sys.stderr.write("kept %d of %d mappings\n" % (len(rows), len(table)))


if __name__ == "__main__":
    main()
