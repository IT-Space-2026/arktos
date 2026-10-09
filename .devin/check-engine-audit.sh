#!/bin/sh
# check-engine-audit.sh — ENG01 engine-coupling audit gate
#
# Keeps .devin/ENGINE.md honest: every C/C++ source file (and qmake
# project file) that names a QWebEngine*/QtWebEngine* symbol must
# appear verbatim in ENGINE.md's coupling map, or match a whitelisted
# path substring from the doc's `engine-audit-whitelist` block.  A patch
# that adds an engine dependency without documenting it fails the gate.
#
# Runs against the source tree itself (no build needed) — cheap enough
# to sit in `make check`'s depends.

cd "$(dirname "$0")/.." || exit 1
DOC=.devin/ENGINE.md

if [ ! -f "$DOC" ]; then
    echo "engine-audit: $DOC missing" >&2
    exit 1
fi

# Whitelist = first token of each line inside the
# <!-- engine-audit-whitelist ... --> block in ENGINE.md.
whitelist=$(sed -n '/engine-audit-whitelist/,/-->/p' "$DOC" | sed '1d;$d' | awk 'NF {print $1}')

files=$(grep -rlE 'QWebEngine|QtWebEngine' \
    src tools autotests fuzz manualtests \
    --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.c' \
    --include='*.pro' --include='*.pri' 2>/dev/null | sort)

missing=0
count=0
whitelisted=0
for f in $files; do
    count=$((count + 1))
    w=0
    for p in $whitelist; do
        case "$f" in *"$p"*) w=1; break ;; esac
    done
    if [ "$w" -eq 1 ]; then
        whitelisted=$((whitelisted + 1))
        continue
    fi
    if ! grep -qF "$f" "$DOC"; then
        echo "engine-audit: $f references QtWebEngine but is not documented in $DOC" >&2
        missing=$((missing + 1))
    fi
done

if [ "$missing" -gt 0 ]; then
    echo "engine-audit: FAIL — $missing undocumented QtWebEngine touchpoint(s)" >&2
    exit 1
fi
echo "engine-audit: PASS — $count files reference QtWebEngine ($whitelisted whitelisted, rest documented)"
exit 0
