#!/usr/bin/env bash
# mergewatch.sh — automated merge watchdog for the arora lanes.
#
# Polls the work lanes (rust-work, ui-work) every INTERVAL seconds and
# merges landed work into master when a lane is ahead. Runs the same
# checks as check-merge.sh plus an explicit dirty-tree guard.
#
# Safety rails:
#   - append-only files (.devin/WORKLOG.md, ChangeLog, task tables)
#     carry `merge=union` in .gitattributes — both sides survive
#   - if the main worktree's uncommitted files overlap the merge set
#     (a taskloop agent mid-run), the cycle is SKIPPED, never forced
#   - any remaining conflict: merge --abort + log; a human resolves
#
# Usage:
#   nohup .devin/mergewatch.sh &     # background
#   touch .devin/STOP_MERGEWATCH     # graceful stop
#   INTERVAL=600 .devin/mergewatch.sh

set -u
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="$REPO/.devin/mergewatch.log"
STOPFILE="$REPO/.devin/STOP_MERGEWATCH"
INTERVAL="${INTERVAL:-300}"
LANES="${LANES:-rust-work ui-work}"

LOCK="$REPO/.devin/mergewatch.lock"
exec 9>"$LOCK" || exit 1
flock -n 9 || { echo "another mergewatch is running" >&2; exit 1; }

ts()  { date '+%F %T'; }
log() { echo "[$(ts)] $*" >>"$LOG"; [ -t 1 ] && echo "[$(ts)] $*"; return 0; }

cd "$REPO" || exit 1
log "mergewatch starting: lanes='$LANES' interval=${INTERVAL}s"

while true; do
	[ -f "$STOPFILE" ] && { log "stop file found — exiting"; break; }

	for lane in $LANES; do
		git rev-parse --verify "$lane" >/dev/null 2>&1 || continue
		ahead=$(git rev-list --count "master..$lane" 2>/dev/null || echo 0)
		[ "$ahead" -eq 0 ] && continue

		# files the merge would touch vs files the main lane has dirty
		git diff --name-only "master...$lane" 2>/dev/null | sort > /tmp/mw_merge.$$
		git status --porcelain | awk '{print $2}' | sort > /tmp/mw_dirty.$$
		if comm -12 /tmp/mw_merge.$$ /tmp/mw_dirty.$$ | grep -q .; then
			log "skip $lane: merge overlaps in-flight work ($(comm -12 /tmp/mw_merge.$$ /tmp/mw_dirty.$$ | tr '\n' ' '))"
			rm -f /tmp/mw_merge.$$ /tmp/mw_dirty.$$
			continue
		fi
		rm -f /tmp/mw_merge.$$ /tmp/mw_dirty.$$

		log "merging $lane ($ahead commit(s) ahead)"
		if git merge "$lane" --no-edit -m "merge $lane: watchdog auto-merge" >>"$LOG" 2>&1; then
			git push >>"$LOG" 2>&1 && log "merged + pushed $lane" \
				|| log "merged $lane locally; push failed (next cycle retries state)"
		else
			log "CONFLICT merging $lane — aborting, needs human review"
			git merge --abort 2>/dev/null
		fi
	done

	sleep "$INTERVAL"
done
