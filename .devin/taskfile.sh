#!/usr/bin/env bash
# taskfile.sh — serialized read-modify-write for taskloop task files
# (Arora-Task*.md).  INFRA01: the loop's bump_attempts/mark_blocked and
# runner agents' row updates used to rewrite the file unlocked — any
# concurrent edit between a writer's read and write was silently
# clobbered (lost an appended bullet and row updates twice in one day).
#
# Every mutation takes a blocking flock on <file>.lock (a sibling of the
# task file), rewrites via awk into a temp file in the same directory,
# then mv -f into place — atomic install, so readers never see a torn
# file and cooperating writers never lose each other's edits.
#
# Usage:
#   taskfile.sh <file> set <ID> [status=…] [attempts=N|+N] [task=…]
#                               [notes=…] [notes+=…]
#       Update table-row fields.  notes= replaces the cell, notes+=
#       appends to it.  Row layout: | ID | Task | Status | Attempts | Notes |
#   taskfile.sh <file> append-detail <ID>
#       Append stdin lines at the end of the row's '### <ID>' section
#       under '## Task details' (before the next '#' heading or EOF).
#   taskfile.sh <file> with-lock -- cmd [args…]
#       Run cmd while holding the lock — escape hatch for edits the
#       subcommands don't cover (e.g. adding a whole row via sed -i).
#       The child inherits the lock fd, so the lock is held for the
#       command's whole lifetime.
#
# Rules enforced: status ∈ pending|in_progress|done|blocked; field
# values may not contain '|' or newlines (they break the table and the
# loop's awk parser — see taskcheck.sh).
set -u

die() { echo "taskfile.sh: $*" >&2; exit 2; }

[ $# -ge 3 ] || die "usage: taskfile.sh <file> <set|append-detail|with-lock> …"
dir="$(cd "$(dirname "$1")" && pwd)" || die "bad directory for $1"
FILE="$dir/$(basename "$1")"; shift
CMD="$1"; shift
[ -f "$FILE" ] || die "$FILE: no such file"

# serialise all writers on the task file's lock (blocking — the lock IS
# the fix; a stale flock can't survive its holder's death)
exec 8>>"$FILE.lock" || die "cannot open $FILE.lock"
flock 8 || die "cannot flock $FILE.lock"

tmp=""
cleanup() { [ -n "$tmp" ] && rm -f "$tmp"; }
trap cleanup EXIT

install_tmp() {
	# keep the file's existing mode across the atomic rename
	chmod --reference="$FILE" "$tmp" 2>/dev/null || chmod 664 "$tmp"
	mv -f "$tmp" "$FILE"
}

case "$CMD" in

set)
	[ $# -ge 1 ] || die "set: missing task id"
	TF_ID="$1"; shift
	TF_STATUS="" TF_ATTEMPTS="" TF_TASK="" TF_NOTES="" TF_NOTES_ADD=""
	for kv in "$@"; do
		case "$kv" in
			status=*)   TF_STATUS="${kv#status=}" ;;
			attempts=*) TF_ATTEMPTS="${kv#attempts=}" ;;
			task=*)     TF_TASK="${kv#task=}" ;;
			notes=*)    TF_NOTES="${kv#notes=}" ;;
			notes+=*)   TF_NOTES_ADD="${kv#notes+=}" ;;
			*) die "set: unknown field '$kv' (want status=/attempts=/task=/notes=/notes+=)" ;;
		esac
	done
	if [ -n "$TF_STATUS" ]; then
		case "$TF_STATUS" in
			pending|in_progress|done|blocked) ;;
			*) die "set: bad status '$TF_STATUS'" ;;
		esac
	fi
	if [ -n "$TF_ATTEMPTS" ]; then
		case "$TF_ATTEMPTS" in
			+*[0-9]*|*[0-9]*) ;;
			*) die "set: bad attempts '$TF_ATTEMPTS' (want N or +N)" ;;
		esac
		# reject non-digits mixed in (e.g. +1x) and bare "+"
		case "${TF_ATTEMPTS#+}" in ""|*[!0-9]*) die "set: bad attempts '$TF_ATTEMPTS'" ;; esac
	fi
	for v in "$TF_STATUS" "$TF_ATTEMPTS" "$TF_TASK" "$TF_NOTES" "$TF_NOTES_ADD"; do
		case "$v" in *"|"*|*$'\n'*|*$'\r'*) die "set: field values may not contain '|' or newlines" ;; esac
	done
	export TF_ID TF_STATUS TF_ATTEMPTS TF_TASK TF_NOTES TF_NOTES_ADD
	tmp="$(mktemp "$FILE.XXXXXX")" || die "mktemp failed"
	awk '
		BEGIN{FS="|";OFS="|"
			id=ENVIRON["TF_ID"]; st=ENVIRON["TF_STATUS"]
			att=ENVIRON["TF_ATTEMPTS"]; tk=ENVIRON["TF_TASK"]
			nt=ENVIRON["TF_NOTES"]; nta=ENVIRON["TF_NOTES_ADD"]}
		$0 ~ ("^\\|[[:space:]]*" id "[[:space:]]*\\|") {
			if (st != "")  $4=" " st " "
			if (att != "") {
				if (att ~ /^\+/) { a=$5; gsub(/[^0-9]/,"",a); $5=" " (a + substr(att,2)) " " }
				else $5=" " att " "
			}
			if (tk != "")  $3=" " tk " "
			if (nt != "")  $6=" " nt " "
			if (nta != "") { n=$6; gsub(/^[ \t]+|[ \t]+$/,"",n)
				$6=" " (n == "" ? nta : n " " nta) " " }
			found=1; print; next
		}
		{print}
		END{exit (found?0:3)}
	' "$FILE" >"$tmp" || { rc=$?; [ $rc -eq 3 ] && die "set: no row for id '$TF_ID'"; die "set: awk failed"; }
	install_tmp
	;;

append-detail)
	[ $# -ge 1 ] || die "append-detail: missing task id"
	TF_ID="$1"; shift
	TF_CONTENT="$(cat)"
	[ -n "$TF_CONTENT" ] || die "append-detail: nothing on stdin"
	export TF_ID TF_CONTENT
	tmp="$(mktemp "$FILE.XXXXXX")" || die "mktemp failed"
	awk '
		BEGIN{id=ENVIRON["TF_ID"]; ins=ENVIRON["TF_CONTENT"]
			target="### " id; insec=0; found=0; flushed=0}
		{
			if (!insec) {
				line=$0; gsub(/[ \t]+$/,"",line)
				if (line == target) { insec=1; found=1 }
				print; next
			}
			if ($0 ~ /^#/) {          # next heading ends the section
				if (!flushed) { printf "%s\n\n", ins; flushed=1 }
				insec=0; print; next
			}
			print; next
		}
		END{
			if (insec && !flushed) printf "%s\n", ins
			if (!found) exit 3
		}
	' "$FILE" >"$tmp" || { rc=$?; [ $rc -eq 3 ] && die "append-detail: no '### $TF_ID' section"; die "append-detail: awk failed"; }
	install_tmp
	;;

with-lock)
	[ "${1:-}" = "--" ] && shift
	[ $# -ge 1 ] || die "with-lock: missing command after --"
	"$@"
	exit $?
	;;

*) die "unknown subcommand '$CMD' (want set|append-detail|with-lock)" ;;
esac
