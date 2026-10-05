#!/usr/bin/env bash
# taskloop.sh — drive .devin/TASKS.md through the Devin CLI.
#
# Each iteration launches one non-interactive `devin -p` run that works
# ONE task (the first pending/in_progress row), marks it done and
# commits. If the run crashes, times out, or exits without finishing
# the task, the loop bumps the row's Attempts counter and restarts it —
# the next run sees the row still in_progress and continues the work
# (state lives in TASKS.md + git, so a fresh session is more robust
# than resuming a possibly-corrupted one).
#
# After MAX_ATTEMPTS crashes on the same task the row is marked
# `blocked` and the loop moves on.
#
# Usage:
#   .devin/taskloop.sh                 # run in foreground
#   nohup .devin/taskloop.sh &         # background
#   touch .devin/STOP_TASKLOOP         # graceful stop at next check
#   RUN_TIMEOUT=3600 MAX_ATTEMPTS=5 .devin/taskloop.sh   # tuneables
#
# Log: .devin/taskloop.log

set -u

# Strip IDE/ACP session env — when taskloop is launched from inside a Devin
# desktop session, ACP_BACKEND makes the CLI refuse to read local
# credentials.toml ("ACP host is the sole source of credentials").
unset ACP_BACKEND WINDSURF_IDE_TYPE WINDSURF_EXT_HOST_PID \
      VSCODE_ESM_ENTRYPOINT VSCODE_CODE_CACHE_PATH VSCODE_IPC_HOOK \
      VSCODE_PID VSCODE_CWD VSCODE_CRASH_REPORTER_PROCESS_TYPE \
      VSCODE_NLS_CONFIG VSCODE_HANDLES_UNCAUGHT_ERRORS 2>/dev/null || true

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TASKS="$REPO/.devin/TASKS.md"
PROMPT="$REPO/.devin/RUNNER_PROMPT.md"
LOG="$REPO/.devin/taskloop.log"
STOPFILE="$REPO/.devin/STOP_TASKLOOP"
MAX_ATTEMPTS="${MAX_ATTEMPTS:-5}"
RUN_TIMEOUT="${RUN_TIMEOUT:-3600}"
COOLDOWN="${COOLDOWN:-10}"
MEM_LIMIT_MB="${MEM_LIMIT_MB:-2048}"   # RSS cap for the devin child; kill+restart beyond this
DEVIN_MODEL="${DEVIN_MODEL:-}"          # e.g. swe-2-high; empty = account default

DEVIN="${DEVIN_BIN:-$(command -v devin 2>/dev/null || true)}"
if [ -z "${DEVIN}" ]; then
	DEVIN=/usr/share/devin-desktop/resources/app/extensions/windsurf/devin/bin/devin
fi

ts()  { date '+%F %T'; }
log() { echo "[$(ts)] $*" >>"$LOG"; [ -t 1 ] && echo "[$(ts)] $*"; return 0; }

# single instance — concurrent loops race on credentials.toml and the task table
LOCK="$REPO/.devin/taskloop.lock"
exec 9>"$LOCK" || exit 1
flock -n 9 || { echo "[$(ts)] another taskloop is already running (lock: $LOCK)" >&2; exit 1; }

# A task row may declare environment requirements in Notes via needs: tags,
# e.g. `needs:tool:xvfb-run` or `needs:macos`/`needs:windows`/`needs:freebsd`.
# Rows whose requirements are not met on this host are SKIPPED (not blocked —
# they may run elsewhere); the loop picks the next runnable task instead.
task_runnable() {
	local notes="$1" req
	while read -r req; do
		[ -n "$req" ] || continue
		case "$req" in
			tool:*) command -v "${req#tool:}" >/dev/null 2>&1 || return 1 ;;
			macos)   [ "$(uname -s)" = Darwin ] || return 1 ;;
			windows) case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*|Windows*) ;; *) return 1 ;; esac ;;
			freebsd) [ "$(uname -s)" = FreeBSD ] || return 1 ;;
			linux)   [ "$(uname -s)" = Linux ] || return 1 ;;
		esac
	done < <(echo "$notes" | grep -oE 'needs:[a-zA-Z0-9:_-]+' | sed 's/^needs://')
	return 0
}

# id of first task row with status pending|in_progress whose needs: tags
# are satisfiable on this host ("" when none)
next_task() {
	local id status notes
	while IFS=$'\x1f' read -r id status notes; do
		id="${id//[[:space:]]/}"
		status="${status//[[:space:]]/}"
		case "$status" in
			pending|in_progress) ;;
			*) continue ;;
		esac
		if task_runnable "$notes"; then
			echo "$id"
			return 0
		else
			echo "[$(ts)] skipping $id — needs: requirement not met on this host" >>"$LOG"
		fi
	done < <(awk -F'|' '/^\|[[:space:]]*[A-Z]+[0-9]+[[:space:]]*\|/ {
		print $2 "\x1f" $4 "\x1f" $6
	}' "$TASKS")
	return 0
}

# numeric Attempts column of a task row
attempts_of() {
	awk -v id="$1" 'BEGIN{FS="|"}
		$0 ~ ("^\\|[[:space:]]*" id "[[:space:]]*\\|") {
			a=$5; gsub(/[^0-9]/,"",a); print a+0; exit
		}' "$TASKS"
}

# bump Attempts of row $id by 1 (task rows are `| ID | Task | Status | Attempts | Notes |`)
bump_attempts() {
	local tmp
	tmp=$(mktemp) || return 1
	awk -v id="$1" 'BEGIN{FS="|";OFS="|"}
		$0 ~ ("^\\|[[:space:]]*" id "[[:space:]]*\\|") {
			a=$5; gsub(/[^0-9]/,"",a); a++;
			$5=" " a " "; print; next
		} {print}' "$TASKS" > "$tmp" && mv "$tmp" "$TASKS"
}

# mark row $id blocked with a reason in Notes
mark_blocked() {
	local tmp
	tmp=$(mktemp) || return 1
	awk -v id="$1" -v r="$2" 'BEGIN{FS="|";OFS="|"}
		$0 ~ ("^\\|[[:space:]]*" id "[[:space:]]*\\|") {
			$4=" blocked ";
			$6=" blocked:" r " "; print; next
		} {print}' "$TASKS" > "$tmp" && mv "$tmp" "$TASKS"
}

[ -x "$DEVIN" ] || { log "FATAL: devin CLI not found ($DEVIN)"; exit 1; }
[ -f "$TASKS" ] || { log "FATAL: $TASKS missing"; exit 1; }
[ -f "$PROMPT" ] || { log "FATAL: $PROMPT missing"; exit 1; }

log "taskloop starting: repo=$REPO devin=$DEVIN timeout=${RUN_TIMEOUT}s max_attempts=$MAX_ATTEMPTS"

while :; do
	if [ -f "$STOPFILE" ]; then
		log "STOP_TASKLOOP present — exiting cleanly"
		exit 0
	fi

	tid="$(next_task)"
	if [ -z "$tid" ]; then
		log "no pending/in_progress tasks remain — all done"
		exit 0
	fi

	att="$(attempts_of "$tid")"
	if [ "$att" -ge "$MAX_ATTEMPTS" ]; then
		log "$tid exceeded $MAX_ATTEMPTS attempts — marking blocked"
		mark_blocked "$tid" "exceeded $MAX_ATTEMPTS restarts"
		continue
	fi

	# pre-flight: don't burn attempts while auth is broken
	if "$DEVIN" auth status 2>&1 | grep -q "Not logged in"; then
		log "devin auth status failed — waiting 60s for login (run: devin auth login)"
		sleep 60
		continue
	fi

	log ">>> starting run for task $tid (attempt $((att+1)), tree mem cap ${MEM_LIMIT_MB}MB, timeout ${RUN_TIMEOUT}s)"
	(
		cd "$REPO" || exit 1
		"$DEVIN" --permission-mode dangerous \
			--respect-workspace-trust false \
			${DEVIN_MODEL:+--model "$DEVIN_MODEL"} \
			-p "$(cat "$PROMPT")" &
		child=$!
		elapsed=0
		# watchdog: kill the whole tree on total RSS over limit or timeout
		while kill -0 "$child" 2>/dev/null; do
			# sum RSS of $child + all descendants
			pids="$child"
			queue="$child"
			while [ -n "$queue" ]; do
				set -- $queue          # word-split on IFS whitespace
				p=$1; shift
				queue="$*"
				kids=$(pgrep -P "$p" 2>/dev/null | tr '\n' ' ')
				if [ -n "$kids" ]; then
					pids="$pids $kids"
					queue="${queue:+$queue }$kids"
				fi
			done
			rss=0
			for p in $pids; do
				k=$(awk '/VmRSS/{print $2}' "/proc/$p/status" 2>/dev/null)
				rss=$((rss + ${k:-0}))
			done
			if [ "$rss" -gt $((MEM_LIMIT_MB * 1024)) ]; then
				echo "[$(ts)] devin tree (pid $child) exceeded ${MEM_LIMIT_MB}MB RSS (total ${rss}KB) — killing" >>"$LOG"
				kill -TERM $pids 2>/dev/null; sleep 5; kill -KILL $pids 2>/dev/null; break
			fi
			if [ "$elapsed" -ge "$RUN_TIMEOUT" ]; then
				echo "[$(ts)] devin pid $child hit ${RUN_TIMEOUT}s timeout — killing" >>"$LOG"
				kill -TERM $pids 2>/dev/null; sleep 30; kill -KILL $pids 2>/dev/null; break
			fi
			sleep 5; elapsed=$((elapsed+5))
		done
		wait "$child"
	) >>"$LOG" 2>&1 9>&-
	rc=$?
	log "<<< devin exited rc=$rc (task $tid)"

	cur="$(next_task)"
	if [ "$cur" = "$tid" ]; then
		bump_attempts "$tid"
		log "task $tid still unfinished (rc=$rc) — attempt bumped, restarting in ${COOLDOWN}s"
		sleep "$COOLDOWN"
	else
		log "task $tid finished (next: ${cur:-none})"
		sleep 2
	fi
done
