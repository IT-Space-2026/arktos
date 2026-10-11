#!/usr/bin/env bash
# Validate task-queue files against the taskloop parser contract.
# Usage: taskcheck.sh [taskfile ...]   (defaults to the .devin task file of cwd)
#
# The loop parses rows with awk -F'|' and assumes exactly 5 columns:
#   | ID | description | status | attempts | notes |
# Literal '|' inside description/notes shifts columns and silently
# misreads status/attempts — this script fails on any such row.
set -u
files=("$@")
if [ ${#files[@]} -eq 0 ]; then
    files=(".devin/Arora-Task.md" ".devin/Arora-Task-UI.md" ".devin/Arora-Task-Rust.md" ".devin/Arora-Task-Lang.md" ".devin/TASKS.md")
fi
rc=0
for f in "${files[@]}"; do
    [ -f "$f" ] || continue
    awk -F'|' -v file="$f" '
        /^\|[[:space:]]*[A-Z]+[0-9]+[[:space:]]*\|/ {
            id=$2; st=$4; att=$5
            gsub(/^[ \t]+|[ \t]+$/,"",id); gsub(/^[ \t]+|[ \t]+$/,"",st); gsub(/^[ \t]+|[ \t]+$/,"",att)
            bad=""
            if (NF != 7) bad = bad " columns=" NF-2 " (expected 5 — literal | inside a field?)"
            if (st !~ /^(pending|in_progress|done|blocked)$/) bad = bad " status=<" st ">"
            if (att !~ /^[0-9]+$/) bad = bad " attempts=<" att ">"
            if (bad != "") { printf "%s:%d: %s :%s\n", file, NR, id, bad; err=1 }
            seen[id]++
        }
        END { for (i in seen) if (seen[i] > 1) printf "%s: duplicate task id %s (%d rows)\n", file, i, seen[i]; exit err }
    ' "$f" || rc=1
done
exit $rc
