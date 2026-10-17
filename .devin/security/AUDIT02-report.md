# AUDIT02 — post-queue security audit, consolidated (slices A–C)

Date: 2026-10-17 · Scope: master after MPROF01-06, PVAULT01/02/04,
VSEC01, VSEC02, UPD01 and the NSIS dual-mode installer landed.

Slice artifacts:
- A (static sweep): `.devin/STAT02-report.md` (raw),
  `.devin/STAT02-findings.txt` (deduped vs STAT01)
- B (triage + dead code): `.devin/STAT02-triage.md`
- C (security delta): findings below
- Prior baseline: `.devin/security/VSEC01-report.md`

## Threat model

Same local adversary as VSEC01 for the vault surface — a same-UID
process or offline disk access that can create files and plant
symlinks under the data root but cannot read process memory or
defeat file permissions. UPD01 adds a network-side adversary — a
malicious or spoofed release-manifest endpoint — bounded by the
GitHub API's TLS. The NSIS surface concerns a Windows operator
installing or removing the package.

## Slice A — static-analysis re-sweep (row AUDIT02)

`gcc -fanalyzer` full build + per-TU `clang --analyze`, deduped
against the STAT01 baseline: 508 gcc + 8 clang in-tree findings →
**21 + 1 NEW** for triage (rest were carry-over sites or the
blanket-justified uic/moc/operator-new families). `main.cpp` skipped
-fanalyzer (>5.2 GB cc1plus OOM; clang covered it) — same fallback
as AUDIT01. Details: STAT02-findings.txt.

## Slice B — triage + dead code (row AUDIT03)

All 22 NEW findings triaged — every analyzer hit is a Qt-blindness
FP (QPointer self-zeroing, Qt parent ownership, synchronous
`std::function` sink the analyzer can't see). The mandated
priority-site hand review found and fixed one real bound (1 MiB
release-manifest cap, this slice's (a) area). `clang-tidy` 22.1.8
over 331 TUs → 773 in-tree hits → 3 real fixes (manifest cap,
socket-pump dead-fd guard, volatile zeroization cast). Dead-code
sweep removed 4 confirmed members. Details: STAT02-triage.md.

## Slice C — security delta re-check (this slice)

Only the post-VSEC01/VSEC02 surface: `git diff db1a6268..HEAD`
bounds it to UPD01 (updatechecker/updatedialog), VSEC02
(browserpaths/profilevault/browserprofile/containermanager/
webenginebackend), the NSIS script, and packaging/CI bookkeeping —
no other network or parsing surface landed.

### (a) UPD01 — UpdateChecker / UpdateDialog

**C1 — FIXED. Asset URLs flowed unchecked into the download sink.**
`browser_download_url` was parsed into `ReleaseAsset::downloadUrl`
and handed verbatim to `DownloadManager::download()` — the external-
handler scheme guard (SEC09) covers only the external-manager path,
so a hostile manifest could aim the "update" at any host or a
non-http(s) scheme. Fix: `UpdateChecker::isTrustedDownloadUrl()` —
https-only on github.com / *.github.com / *.githubusercontent.com,
or same-host-no-downgrade vs the configured api url (covers loopback
tests and self-hosted GHE); credential-bearing, relative and
non-http(s) URLs refused. Untrusted assets are dropped at parse so
`platformAssets()` and the dialog never see them; the button then
degrades to "Open Release Page" (a compile-time constant URL).
`html_url` is likewise scheme-bound (http/https only) even though
nothing navigates it today — a stored `Result` can't carry a
javascript: link for a future caller.
Regression: `tst_updatechecker::untrustedAssetUrls`.

**C2 — FIXED. Tor refusal covered the check but not the sinks.**
`check()` refuses under `BrowserApplication::isTorMode()` and the
Help action is disabled — but `downloadLatest()` and the always-live
"View all releases" link called `QDesktopServices::openUrl` /
engine downloads with no tor check; either would put github.com on
clearnet from a tor process. Both slots now refuse under tor mode
and say so in the status label.
Regression: `tst_updatechecker::torDialogSinks`.

**C3 — FIXED (hardening). Server-controlled strings rendered as
markup.** `releaseTitle` went into an AutoText `QLabel` (rich-text
injection — markup/links in a release name would render); the
detail label is now `Qt::PlainText`, and the download-button tooltip
escapes the asset name (tooltips auto-detect rich text).

**Verified clean:** manifest bounds (1 MiB parse cap, 8 MiB read
buffer, 15 s transfer + 20 s outer timeout — AUDIT03), version
compare semantics (tag_name is truth, numeric-core compare),
per-OS asset matching order, single-flight `check()` guard,
cancel/timeout teardown, 404 → honest "no releases".

**Residual (documented, not fixed):** the asset *fetch* rides the
engine/Chromium network stack (`page->download` via the hidden retry
page) or `RustDownloadEngine` — both resolve proxies through
`QNetworkProxy::applicationProxy()`, while the *check* honors the
per-NAM `NetworkProxyFactory` configured in settings. A normal-mode
user behind a configured proxy gets the manifest through the proxy
but the download via Chromium's own resolution — a pre-existing
engine-wide gap for ALL downloads, not UPD01-specific; pushing the
configured proxy into applicationProxy changes global routing and is
out of audit-fix scope. Also by design: no signature verification of
the downloaded package — the feature hands the file to the user's
own package manager, where distro trust applies.

### (b) VSEC02 — engine-hold restore path

**C4 — FIXED. Leaf-only symlink check let a planted hold inject a
foreign engine store.** `unseal()` tested
`QFileInfo(hold).isSymLink()` on `engine-hold/QtWebEngine` — the leaf
only. `QFileInfo::isDir()` follows links, so `engine-hold` itself (or
`cache-hold`/`profiles-hold` in `holdExcluded`) planted as a symlink
to an attacker tree resolved to a real dir, passed the check, and
`QDir().rename()` moved the foreign store into `profiles/<id>/` —
an integrity bypass of the MAC'd blob for the engine data it
replaces (session/cookie injection under a valid unlock). Fix: new
`pathChainHasLink(path, boundary)` walks every component strictly
below the boundary (`BrowserPaths`'s sweep discipline); a linked
`engine-hold` or `<name>-hold` is refused and the link itself is
removed, never followed, never moved. The restored destination
`profiles/<id>/QtWebEngine` gets the same treatment — a planted link
there would route Chromium's writes outside the sealed set, so it
is unlinked before the rename decision.
Regression: `tst_profilee2e::engineHoldRejectsLinks` (planted
engine-hold + cache-hold links refused; blob's own store restored;
foreign trees untouched).

**C5 — FIXED (mitigated). Check→rename race.** Between the hold
check and `rename()` a racing same-UID process could swap the hold
for a link, which then lands at the destination. The restore now
re-verifies `!isSymLink()` on the destination immediately after the
rename and unlinks a loser of that race. Residual frontier: a
process that can continuously race syscalls sits past the local
threat model — documented, not fully closable in userspace Qt.

**C6 — FIXED. `holdExcluded` restore followed planted links.** Same
class as C4 for `cache-hold`/`profiles-hold` — held entries are now
link-checked before `rename()`. Seal side: a *symlinked* live dir
(user plumbing — e.g. cache on tmpfs) is no longer moved aside; it
rides the blob as a link entry (tar `follow_symlinks(false)`) and
the same setup resurrects on unseal.

**C7 — FIXED. Post-seal residue clearing could descend a planted
profile-dir link.** `unseal()`'s residue-only fast path ran
`secureEraseTreeContents(dir)` on whatever `profiles/<id>` pointed
at — a planted link would route the erase into a foreign tree.
Linked `dir` now skips descent and falls to the park path, where
`rename()` moves the link itself harmlessly.

**Verified clean:** `ensureEngineStorageLayout()` — entry lstat
refusal incl. dangling links, `pathChainHasLink` on the destination
chain, both-sides collision abort, empty Qt-materialized residue
dropped not migrated, orphan persona dirs left loudly, cache-side
links unlinked not descended (also confirmed as a STAT02-triage
priority site). `rv_unseal`'s destination policy neutralizes a
linked out_dir outright — extraction lands in a staging dir that
`fs::rename()`s onto the path, replacing any link rather than
writing through it; tar extraction whitelists entry types and
refuses traversal. Failed-retry retention: a failed engine-hold
rename keeps the hold (the only copy) for the next unseal — that is
correct; it sits inside `<id>.vault/` and the next `seal()` erases
it as stale if the blob already covers the engine store.

**Residual (documented):** `engine-hold/` is PLAINTEXT beside the
blob while the persona stays locked — it is a migration way-station
for a pre-VSEC02 store that cannot be re-sealed without the
password. The promise "locked leaves zero Chromium-side plaintext"
has this one bounded exception until first unlock; now stated in
`profilevault.h`. The parked `cache-hold/` plaintext is the already-
documented cache exclusion (disposable by design).

### (c) NSIS dual-mode installer — verified, no defect

`BuildProcess/windowsinstaller.nsi` uses MultiUser.nsh correctly for
the stranding class named in the task:

- `MULTIUSER_INIT` in `.onInit` resolves the mode (page or
  `/AllUsers`/`/CurrentUser` via `MULTIUSER_INSTALLMODE_COMMANDLINE`);
  `SHCTX` is bound once per run and used for every `WriteRegStr`, so
  install-time keys always land in the hive matching the chosen
  context.
- `MULTIUSER_UNINIT` in `un.onInit` re-derives the mode from the
  uninstaller's own location — `uninst.exe` under `%ProgramFiles%`
  implies per-machine (and forces elevation for HKLM cleanup), under
  `%LOCALAPPDATA%` implies per-user. The same `SHCTX` then scopes the
  `DeleteRegKey`s — install and uninstall contexts are symmetric, so
  the wrong-hive stranding the task probes cannot occur.
- Elevation refusal on an all-users pick aborts inside
  `MULTIUSER_INIT` before any payload is written — no partial install
  to strand. Files and `RMDir /r` are confined to `$INSTDIR`;
  shortcut create/delete use the matching shell-var context.

Accepted nit (documented, not changed): no `SetRegView 64`, so on
x64 the Uninstall/App Paths keys land under WOW6432Node — the view
is identical for install and uninstall (still consistent; Add/Remove
Programs reads both views), so nothing strands, but native 64-bit
placement would be the tidier choice for a Qt6 payload.

### (d) Other new surface since VSEC01

Diff `db1a6268..HEAD`: beyond (a)–(c) only packaging/CI deltas —
`Info_mac.plist` metadata additions, the drag-to-Applications dmg
packaging step, deb/rpm/flatpak version bumps, task bookkeeping.
Nothing else accepts network input or parses untrusted data.

## Suppressions / accepted risks

- Engine-mediated downloads (incl. update assets) don't inherit the
  configured per-NAM proxy — engine-wide, pre-existing.
- No signature check on downloaded packages — manual-download UX;
  platform package manager carries trust.
- `engine-hold/` and `cache-hold/` plaintext beside the blob while
  locked — documented in `profilevault.h`; bounded migration residue
  and the spec'd cache exclusion respectively.
- Server-controlled `releaseTitle`/asset names may still contain
  arbitrary text — rendered only as plaintext now; no residual
  markup channel.
- Same-UID live-race frontier on filesystem ops — outside the stated
  threat model; the deterministic planted-link cases are all closed.

## Regression coverage

- `autotests/updatechecker`: `untrustedAssetUrls` + `torDialogSinks`
  added — 15/15 PASS.
- `autotests/profilee2e`: `engineHoldRejectsLinks` added — 22/22
  PASS (warnings show both refusal paths firing).
- `make check`: see row notes.
