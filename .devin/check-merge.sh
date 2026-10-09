#!/usr/bin/env bash
# check-merge.sh — is it time to merge rust-work into master?
#
# "Merge due" when EITHER:
#   1. the rust lane's queue is drained (no pending/in_progress rows in
#      Arora-Task-Rust.md), or
#   2. a pending/in_progress main-lane task references the rust lane's
#      work (rustcore / rc_* / the rust-lane task ids) — meaning main
#      needs code that only lives on the rust-work branch.
#
# Exit 0 = merge recommended, 1 = keep lanes separate, 2 = can't tell.
# Usage: .devin/check-merge.sh [--quiet]

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUST_WT="$(dirname "$REPO")/arora-rust"
MAIN_TASKS="$REPO/.devin/Arora-Task.md"
RUST_TASKS="$RUST_WT/.devin/Arora-Task-Rust.md"
QUIET=0
[ "${1:-}" = "--quiet" ] && QUIET=1
say() { [ "$QUIET" = 0 ] && echo "$*"; return 0; }

[ -f "$RUST_TASKS" ] || { say "rust lane file missing ($RUST_TASKS)"; exit 2; }
[ -f "$MAIN_TASKS" ] || { say "main task file missing ($MAIN_TASKS)"; exit 2; }

# --- 1. rust queue drained? ---------------------------------------------
rust_open=$(awk -F'|' '/^\|/ && NF>5 {
		s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s);
		if (s=="pending"||s=="in_progress") n++
	} END{print n+0}' "$RUST_TASKS")

rust_active=$(awk -F'|' '/^\|/ && NF>5 {
		s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s);
		if (s=="in_progress") n++
	} END{print n+0}' "$RUST_TASKS")

rust_done=$(awk -F'|' '/^\|/ && NF>5 {
		s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s);
		if (s=="done") n++
	} END{print n+0}' "$RUST_TASKS")

say "rust lane: $rust_done done, $rust_open open ($rust_active in-flight)"

# already merged? then nothing on the lane is outstanding regardless
# of the task-file states below.
if git -C "$REPO" rev-parse --verify rust-work >/dev/null 2>&1 \
	&& git -C "$REPO" merge-base --is-ancestor rust-work master 2>/dev/null; then
	ahead=$(git -C "$REPO" rev-list --count master..rust-work 2>/dev/null || echo 0)
	if [ "$ahead" -eq 0 ]; then
		say "rust-work is fully merged into master — no merge needed"
		exit 1
	fi
fi

if [ "$rust_open" -eq 0 ] && [ "$rust_done" -gt 0 ]; then
	say "MERGE DUE: rust lane queue drained — $rust_done task(s) ready on rust-work"
	exit 0
fi

# --- 2. main lane waiting on rust code? ---------------------------------
# only a real needs:task:<rustlane-id> dependency counts — a passing
# mention (like DEVT02's 'if rustcore landed, else standalone' fallback)
# is not a merge trigger. rust-lane ids are read live from the rust file.
rust_ids=$(awk -F'|' '/^\|/ && NF>5 {id=$2; gsub(/^[ \t]+|[ \t]+$/,"",id); print id}' "$RUST_TASKS" | tr '\n' ' ')
hits=""
for rid in $rust_ids; do
	m=$(awk -F'|' -v rid="$rid" '/^\|/ && NF>5 {
			s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s);
			if ((s=="pending"||s=="in_progress") && $0 ~ ("needs:task:" rid "[^0-9A-Za-z]")) {
				id=$2; gsub(/^[ \t]+|[ \t]+$/,"",id); print id
			}
		}' "$MAIN_TASKS")
	[ -n "$m" ] && hits="$hits $m(needs:$rid)"
done

# 'due' only when the needed rust row is DONE on rust-work (merge would
# unblock the main task now); still-open rust deps are a watch, not a due.
if [ -n "$hits" ]; then
	due=""; watch=""
	for h in $hits; do
		dep="${h##*needs:}"; dep="${dep%)}"; dep="${dep##*(:}"
		st=$(awk -F'|' -v rid="$dep" '/^\|/ && NF>5 {
				id=$2; gsub(/^[ \t]+|[ \t]+$/,"",id);
				if (id==rid) { s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s); print s; exit }
			}' "$RUST_TASKS")
		if [ "$st" = "done" ]; then due="$due $h"; else watch="$watch $h($dep=$st)"; fi
	done
	if [ -n "$due" ]; then
		say "MERGE DUE: main-lane task(s) blocked on landed rust work:$due"
		exit 0
	fi
	[ -n "$watch" ] && say "watch (deps still open on rust lane):$watch"
fi

# --- 3. drift check (informational) --------------------------------------
if git -C "$REPO" rev-parse --verify rust-work >/dev/null 2>&1; then
	ahead=$(git -C "$REPO" rev-list --count master..rust-work 2>/dev/null || echo '?')
	say "rust-work is $ahead commit(s) ahead of master — lanes independent, no merge needed"
fi

# --- 4. ui lane: frequent-merge policy -----------------------------------
# ui-work merges EARLY AND OFTEN — its commits are small UI edits that
# drift-conflict with main if they pile up. Any commits ahead = due.
UI_WT="$(dirname "$REPO")/arora-ui"
UI_TASKS="$UI_WT/.devin/Arora-Task-UI.md"
if git -C "$REPO" rev-parse --verify ui-work >/dev/null 2>&1; then
	if [ -f "$UI_TASKS" ]; then
		ui_done=$(awk -F'|' '/^\|/ && NF>5 {s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s); if (s=="done") n++} END{print n+0}' "$UI_TASKS")
		ui_open=$(awk -F'|' '/^\|/ && NF>5 {s=$4; gsub(/^[ \t]+|[ \t]+$/,"",s); if (s=="pending"||s=="in_progress") n++} END{print n+0}' "$UI_TASKS")
		say "ui lane: $ui_done done, $ui_open open"
	fi
	uiahead=$(git -C "$REPO" rev-list --count master..ui-work 2>/dev/null || echo 0)
	if [ "$uiahead" -gt 0 ]; then
		say "MERGE DUE (frequent-merge lane): ui-work is $uiahead commit(s) ahead of master"
		exit 0
	fi
	say "ui-work is fully merged into master"
fi

say "no merge needed"
exit 1
