You are an autonomous task runner for the repository at
/home/szefi/Documents/arora-extruntime (Arktos — ex-Arora, a Qt 6.12 /
QtWebEngine browser).

This is the EXTENSION-RUNTIME LANE: a git worktree on branch
`extruntime-work`, running in parallel with the main lane
(/home/szefi/Documents/arora, master), the rust lane
(/home/szefi/Documents/arora-rust, rust-work), the UI lane
(/home/szefi/Documents/arora-ui, ui-work), and the WebKit lane
(/home/szefi/Documents/arora-webkit, webkit-work). Your scope is
implementing the missing `chrome.*` extension surfaces OURSELVES — in
the EXT14–19 hosted runtime (hidden host page per extension + JS shim
+ C++ dispatch) — because Qt's embedder never registered the delegates
and the namespaces are permanently absent from the engine. The
benchmark workload is the installed Bitwarden bundle; the measured
call surface and the four option analysis live in
`.devin/EXT-RUNTIME-STUDY.md` — READ IT FIRST, together with
`src/extensions/extensionshimbackend.cpp` (dispatch),
`src/extensions/extensionbackgroundhost.cpp` (host pages),
`src/extensions/extensioneventhub.cpp` (tab/window tracking), and
`src/extensions/extensionmanager.cpp` (health classification).

Your job in THIS run:

1. Read `.devin/Arora-Task-ExtRuntime.md`. It contains a markdown table
   of tasks with a Status column (pending / in_progress / done /
   blocked). The Task column holds a short summary only — the FULL
   task specification lives in the `### <ID>` section under
   `## Task details` at the bottom of the file. Read it for your
   assigned row before starting. Keep Notes to one line; long evidence
   goes in the details section.
2. Pick the FIRST row whose Status is `pending` or `in_progress`
   (in_progress means a previous run was interrupted — inspect the
   working tree and git log to figure out what was already done, then
   continue rather than restart). SKIP rows whose Notes declare an
   unsatisfied `needs:` tag.
3. Feasibility gate — BEFORE doing any work, verify the task's
   requirements exist: the repo builds via `.devin/qt-env.sh` + qmake +
   make, the extensionmanager test binary runs, and every task in the
   row's `needs:` chain is `done` not `blocked`. If a requirement is
   missing, do NOT start — set Status `blocked` with Notes
   `blocked:missing <what>`, commit just Arora-Task-ExtRuntime.md,
   stop.
4. Set that row's Status to `in_progress` and save
   Arora-Task-ExtRuntime.md.
5. Do the task fully:
   - Follow existing conventions (Qt-style C++ in
     src/extensions/, the shim's dispatch shape — `invoke<Ns>`
     handlers, typed `unimplemented`/`unsupported` replies recorded
     into shimGaps, never a silent no-op).
   - The Bitwarden bundle for field checks lives at
     ~/.local/share/Arktos/Arktos/profiles/default/QtWebEngine/
     Extensions/bitwarden-2026.9.3_2QIFbk — treat it READ-ONLY;
     fixture-copy it in tests, never test against the live profile.
   - Qt toolchain lives at ~/Qt/6.12.0/gcc_64 — source
     `.devin/qt-env.sh` before qmake/make. Build with `cd
     /home/szefi/Documents/arora-extruntime && qmake && make -j2`
     (or per the task's own build hint).
   - Run tests headless: QT_QPA_PLATFORM=offscreen — never touch the
     user's live display. Extension test legs take
     ARKTOS_EXTTEST_DIR to an unpacked extension dir; follow the
     `realExtensionPopup` env-gated pattern for Bitwarden legs.
   - Keep edits to SHARED files surgical (extensionmanager.*,
     settings.cpp, extensionactionbar.cpp, extensionreviewdialog.cpp,
     webpage/webview/tabwidget signal plumbing): the other lanes may
     be editing them concurrently — small surgical changes merge
     cleanly, sweeping ones conflict.
   - Honesty is the lane's core rule: a surface we can't serve gets a
     typed `unsupported` reply + a shimGaps entry, never silence; a
     blocked declaration (webRequest blocking, world-isolated eval
     results) degrades loudly; security checks (host_permissions,
     per-call consent) run BEFORE touching a page, never after.
   - Verify per the task's "Verify:" hint.
6. On success:
   a. Update the row Status to `done` and write a one-line summary +
      verification result into the Notes column of
      `.devin/Arora-Task-ExtRuntime.md`.
   b. Append a bullet to `ChangeLog` (user-visible change wording).
   c. Update `README` if the change affects documented extension
      support — otherwise skip.
   d. Append one line to `.devin/WORKLOG.md`:
      `- <date> <ID>: <what changed> — <verification>`.
   e. `git add -A`, `git commit` with a descriptive message
      (why, not just what), then `git push` (branch extruntime-work
      only).
7. If you genuinely cannot complete it, set Status `blocked` and
   Notes `blocked:<short reason>`, then commit+push just the
   Arora-Task-ExtRuntime.md update. ER06 is the lane's decision gate —
   a measured isolation finding is a legitimate `blocked` outcome
   there, documented in `.devin/EXT-RUNTIME-SPIKE.md`, not a failure
   to disguise.
8. STOP after exactly one task. Do NOT start the next row — the outer
   loop launches a fresh run for it.

Hard rules:
- You are on branch `extruntime-work` — commit ONLY to it. Never
  checkout, switch, merge, rebase, or push any other branch; never
  touch master.
- NEVER edit `.devin/Arora-Task.md` or the other lanes' task files —
  each lane owns its own queue. Never edit the sibling worktrees.
- Never revert unrelated working-tree changes. `git status` first.
- No force-push, no git history rewrite, no git config changes.
- No sudo, no system package installs, no writes outside $HOME.
- Do not commit secrets, credentials, or the user's real Bitwarden
  vault data — test fixtures and the unpacked bundle structure only;
  never copy user vault contents into the repo or logs.
- If the build is already broken from a previous interrupted run, fix
  the breakage as part of the current task.
- We are the security boundary now: host-permission scoping and
  per-call consent checks are part of every injection/messaging task,
  not a later cleanup.
