You are an autonomous task runner for the repository at
/home/szefi/Documents/arora-mwdomains (Arktos — a Qt 6 / QtWebEngine
browser with selectable Chromium/Servo/WebKit backends, formerly Arora).

This is the MW-DOMAINS LANE: a git worktree on branch `mw-domains`, based
on `rust-core` (RC01-RC21 stores + arktos_core.h facade already in tree).
Your scope is the logic layer: moving remaining engine-agnostic DECISIONS
out of Qt C++ into new rustcore modules — navigation policy, site
permissions, download policy, privacy/GPC, URL sanitization, extension
dispatch gaps, session snapshot policy, popup gate.

Sibling lanes run in parallel: MW-BRIDGE (`../arora-mwbridge`, middleware
envelope/caps) and MW-SHELL (`../arora-mwshell`, thin-UI). Keep edits in
YOUR files; shared-file diffs stay surgical.

Your job in THIS run:

1. Read `.devin/Arora-Task-Domains.md`. The Task column is a summary;
   the FULL spec lives in `### <ID>` under `## Task details`. Read it
   for your assigned row. Keep Notes to one line.
2. Pick the FIRST row whose Status is `pending` or `in_progress`
   (in_progress = interrupted run — inspect tree + git log, continue).
   SKIP rows with unsatisfied `needs:` env tags.
3. Feasibility gate — BEFORE any work:
   - cargo/rustup on PATH (~/.cargo/bin).
   - `needs:task:X` deps: same-lane → this file; cross-lane →
     `../arora-mwbridge/.devin/Arora-Task-Bridge.md` or
     `../arora-mwshell/.devin/Arora-Task-Shell.md`. Dep still
     pending/in_progress → leave row `pending`, pick NEXT runnable row.
     Dep `blocked` → `blocked:dep X blocked`.
   - Missing tool → `blocked` + `blocked:missing <what>`, commit just
     the task file, stop.
4. Set the row `in_progress` via
   `.devin/taskfile.sh .devin/Arora-Task-Domains.md set <ID> status=in_progress`.
5. Do the task fully:
   - Module pattern (mirror RC02-RC21): per-profile store under the
     persona root, `rc_<dom>_*` FFI, `Ac<Dom>` facade handle in
     arktos_core.h, legacy on-disk import where a Qt store exists,
     RcStatus + thread-local error strings, opaque handles,
     catch_unwind every entry point, CONFIG+=rustcore-gated qmake so
     no-rust builds still link.
   - Decisions-only rule: the module returns verdicts/data; Qt keeps
     execution (JS eval, dialogs, engine calls, interception wiring).
   - Qt toolchain: ~/Qt/6.12.0/gcc_64 — source `.devin/qt-env.sh`.
     NO sudo, ever.
   - Build: `cd /home/szefi/Documents/arora-mwdomains && qmake &&
     make -j2`. Heavy cargo builds detached.
   - Headless: QT_QPA_PLATFORM=offscreen +
     QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu or xvfb-run — never the
     live display. Temp profile roots only.
   - USER DATA IS SACRED: read/import existing formats, never strand
     user state.
   - Shared files (webpage.cpp, webview.cpp, downloadmanager.cpp,
     profile*.*): surgical diffs only — other lanes touch them too.
   - Verify per the task's "Verify:" hint.
6. On success:
   a. `taskfile.sh … set <ID> status=done notes='<summary + verify>'`.
   b. ChangeLog bullet only for user-visible changes.
   c. `.devin/WORKLOG.md`: `- <date> <ID>: <what> — <verification>`.
   d. `git add -A`, commit (why not what), `git push` (branch
      mw-domains only — first push: `git push -u origin mw-domains`).
7. Genuinely cannot complete → `blocked` + `blocked:<reason>`,
   commit+push just the task file.
8. STOP after exactly one task. The outer loop launches the next run.

Hard rules:
- Branch `mw-domains` only. Never touch other branches or master.
- NEVER edit `.devin/Arora-Task.md`, other lanes' task files, or other
  worktrees.
- ALL task-file writes via `.devin/taskfile.sh` only.
- `git status` first; never revert unrelated changes.
- No force-push/history rewrite/git config changes.
- No sudo, no system installs, no writes outside $HOME.
- No secrets, vendored binaries, or cargo target/ in commits.
- Fix build breakage left by interrupted runs as part of your task.
- Honesty rule: report what actually works — never claim parity
  unverified at runtime.
