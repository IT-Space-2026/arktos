#!/usr/bin/env bash
# mergewatch.sh — merge watchdog for the arora lanes.
#
# Polls the work lanes (rust-work, ui-work) every INTERVAL seconds.
# When a lane has commits master lacks, it spawns ONE `devin` run
# (MERGE_PROMPT.md) that performs the merge, resolves conflicts,
# verifies the build and pushes. The watchdog itself never merges —
# a real agent handles conflicts instead of blind aborts.
#
# Usage:
#   nohup .devin/mergewatch.sh &     # background
#   touch .devin/STOP_MERGEWATCH     # graceful stop at next check
#   INTERVAL=600 .devin/mergewatch.sh

set -u

# Strip IDE/ACP session env — same reason as taskloop.sh.
unset ACP_BACKEND WINDSURF_IDE_TYPE WINDSURF_EXT_HOST_PID \
      VSCODE_ESM_ENTRYPOINT VSCODE_CODE_CACHE_PATH VSCODE_IPC_HOOK \
      VSCODE_PID VSCODE_CWD VSCODE_CRASH_REPORTER_PROCESS_TYPE \
      VSCODE_NLS_CONFIG VSCODE_HANDLES_UNCAUGHT_ERRORS 2>/dev/null || true

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOG="$REPO/.devin/mergewatch.log"
STOPFILE="$REPO/.devin/STOP_MERGEWATCH"
PROMPT="$REPO/.devin/MERGE_PROMPT.md"
INTERVAL="${INTERVAL:-300}"
LANES="${LANES:-rust-work ui-work}"
MEM_LIMIT_MB="${MEM_LIMIT_MB:-1536}"   # merge runs are light; still cap
DEVIN_MODEL="${DEVIN_MODEL:-swe-2-high}"

DEVIN="${DEVIN_BIN:-$(command -v devin 2>/dev/null || true)}"
if [ -z "$DEVIN" ]; then
	DEVIN=/usr/share/devin-desktop/resources/app/extensions/windsurf/devin/bin/devin
fi

ts()  { date '+%F %T'; }
log() { echo "[$(ts)] $*" >>"$LOG"; [ -t 1 ] && echo "[$(ts)] $*"; return 0; }

LOCK="$REPO/.devin/mergewatch.lock"
exec 9>"$LOCK" || exit 1
flock -n 9 || { echo "another mergewatch is running" >&2; exit 1; }

cd "$REPO" || exit 1
log "mergewatch starting: lanes='$LANES' interval=${INTERVAL}s model=${DEVIN_MODEL:-default}"

while true; do
	[ -f "$STOPFILE" ] && { log "stop file found — exiting"; break; }

	git fetch origin --quiet 2>/dev/null

	due=""
	for lane in $LANES; do
		git rev-parse --verify "$lane" >/dev/null 2>&1 || continue
		ahead=$(git rev-list --count "master..$lane" 2>/dev/null || echo 0)
		[ "$ahead" -gt 0 ] && due="$due $lane($ahead)"
	done

	# nothing to merge, but local master may hold an unpushed merge
	# (previous push raced a lane push) — nudge it up.
	if [ -z "$due" ]; then
		unpushed=$(git rev-list --count origin/master..master 2>/dev/null || echo 0)
		if [ "$unpushed" -gt 0 ]; then
			git push >>"$LOG" 2>&1 && log "pushed $unpushed unpushed commit(s)"
		fi
		sleep "$INTERVAL"; continue
	fi

	log "merge due:$due — spawning devin merge run"
	( ulimit -v $((MEM_LIMIT_MB * 1024)) 2>/dev/null || true
	  timeout 1800 "$DEVIN" --permission-mode dangerous \
		--respect-workspace-trust false \
		${DEVIN_MODEL:+--model "$DEVIN_MODEL"} \
		-p "$(cat "$PROMPT")" >>"$LOG" 2>&1 )
	rc=$?
	log "merge run exited rc=$rc"

	# next cycle re-checks drift — no blind retries needed
	sleep "$INTERVAL"
done
