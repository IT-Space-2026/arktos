# Arktos — CEF-SPIKE lane task table

Lane: `cef-spike` branch, worktree `/home/szefi/Documents/arora-cefspike`.
Goal: answer empirically whether CEF (Chromium Embedded Framework,
prebuilt Spotify binaries) is a viable third/replacement Chromium
backend — with real MV3 extension support as the deciding driver.

Prior art ALREADY on this branch: `spikes/cef/qcefprobe` (OSR +
windowed-chrome render proofs, all PASS) and `.devin/CEF-SPIKE.md` +
`.devin/CEF-EXT-SPIKE.md` (facade mapping; Bitwarden 2026.10.0 CRX
verdict: SW+popup+storage+messaging+scripting all live on the Chrome
runtime). This lane continues from there — do NOT redo settled work.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| CS01 | Bitwarden verdict on CEF with OUR bundled zip — details below | done | 3 | needs:tool:xvfb-run; DONE — bundled BW 2026.9.3 on CEF chrome runtime: SW boots, popup opens+routes (skip->intro-carousel), cs<->sw echo round-trip verified; gaps: nativeMessaging absent, privacy/identity unverified (ungated perms), sidePanel inset unverified; .devin/CEF-BW-VERDICT.md |
| CS02 | Windowed-embed depth — details below | done | 0 | DONE — XTest-driven input legs on reparented chrome view: focus-in/out, SetFocus refocus, ASCII+U+4E2D/é keys, wheel, Tab all PASS; measured gaps: dead-key compose absent (no IM stack), no auto-resize (embedder path works), hidpi needs force-device-scale-factor (verified), Tab never escapes view; .devin/CEF-INPUT-VERDICT.md |
| CS03 | Capability-split design — details below | done | 0 | DONE — .devin/CEF-CAPS.md: split collapses to one bit (extensions is a SHAPE property, CEF#3685); single cef id, shape pinned at initialize() like the Servo load-gate probe, caps.rs row drafted (partial+note grades, no schema bump), updateEngineIndicator untouched, extension gating must read the adapter bit not the facade row; reviewed vs real signatures (engineinterface.h:595-650, servobackend:1717, caps.rs, enginecaps/engineindicator) |
| CS04 | Sandbox + profile isolation — details below | done | 0 | real userns sandbox verified via /proc (Seccomp:2 + nested pidns); bwrap wrap + Chromium sandbox = FATAL abort in all 3 variants (helper unusable/absent/disabled); persona ctxs isolate+persist as direct children of root_cache_path, nested container path silently in-memory, OTR disk-clean; setuid path env-blocked — .devin/CEF-SANDBOX.md, 18 legs 13P/5F |
| CS05 | Final verdict — details below | done | 0 | DONE — .devin/CEF-VERDICT.md: contract-fit GO vs ENGINE-ADAPTER (13 areas mapped, honest partials), cost ~20-34wk bounded / 1176 must-port refs / 74 files, MV3 fidelity verified-parity not differentiator, recommendation REJECT today with named re-entry drivers |

| CS06 | Alloy-style extension reality check on PREBUILT CEF — details below | done | 0 | DONE — CEF#3859 verified+exceeded on both alloy shapes: install+SW+storage+cs<->sw+popup-page all PASS incl OSR (own qcefprobe, pixel witness); dead: chrome:// surfaces, tabs.query enumeration (sender.tab.id still populated), connectNative/identity/privacy; G1 shrinks to tab-model graft — .devin/CEF-ALLOY-EXT.md |
| CS07 | CEF source build at the tested pin — details below | done | 5 | DONE — source build at pin complete (03:30): libcef.so 2.8G + cefsimple + chrome_sandbox, CDP Chrome/152.0.7977.169 + page target PASS; build is dcheck+PA-malloc (non-official defaults) so runtime REQUIRES --disable-features=UseBoringSSLForRandBytes (first-per-thread BoringSSL rand-state malloc inside PA thread-cache op trips PA_REENTRANCY_GUARD FATAL — shipped official binary immune); ~/cef-cache/cs07-result.json |
| CS11 | CS07 artifact verification + pin record — details below | done | 0 | DONE — artifacts re-verified at pin (libcef.so 2.8G + cefsimple + chrome_sandbox, stamp 152.0.15+gb3d72fb+chromium-152.0.7977.169); cs07-verify.sh re-run PASS: CDP Chrome/152.0.7977.169 + 1 page target; pin/args/timing/reproduce + PA-rand runtime-flag caveat recorded in .devin/CEF-SOURCEBUILD.md |
| CS08 | Graft 1: extension-page hosting under alloy — details below | done | 0 | DONE — G1 graft built+verified at pin: AlloyExtensionWindowController/WindowControllerList + tab-model hunks (patches 0001/0002 + BUILD.md); patched alloy-windowed & alloy-OSR legs ALL PASS incl tabs.query/windows.*/tabs.onUpdated (baseline dead) — BW popup rendered in alloy-OSR (pixel witness); stack-smash traced to non-official canary flag (env artifact, flag-documented); .devin/CEF-GRAFT-ALLOY.md |
| CS12 | Graft 2: nativeMessaging on chrome runtime — details below | done | 3 | DONE — G2 zero-line graft: nativeMessaging works UNPATCHED on prebuilt+source, chrome+alloy (11/11 legs: echo/multi/send/host-exit+disconnect+pids/absent+forbidden exact upstream strings, user-level manifest dir). Prior "absent" = ungranted-permission API hiding (BW optional_permissions) — reproduced w/ noperm fixture; opt-perm arc: SW request needs gesture, page click reaches ExtensionInstallPrompt (pends), seeded grant unhides API + echo PASS. .devin/CEF-GRAFT-NATIVEMSG.md |
| CS09 | Graft 3: webRequest.onAuthRequired — details below | done | 7 | DONE — G3 zero-line graft: onAuthRequired answers UNPATCHED on prebuilt+source, chrome+alloy (nav leg PASS all 3; iso legs: blocking/async/cancel/leg-wrong all PASS incl server-verified bad-cred re-challenge; instrumented trace cefCreateLoginDelegate->FwdToExt proxied=1; subresource path = proxied URLLoader; combined-run FAILs were upstream preemptive auth-cache mask; gap: unanswered challenge pends on views login prompt). .devin/CEF-GRAFT-AUTH.md |
| CS10 | Patched-CEF verdict — details below | done | 0 | needs:task:CS08,CS09,CS12; DONE — .devin/CEF-PATCHED-VERDICT.md: G1=2 patches ~852 net ln all legs PASS both alloy shapes, G2/G3 zero-line grafts; bump cost hrs-days bounded vs ~119-patch/ESR gecko bill, dwarfed by 20-34wk port; nativeMessaging blocker dead unpatched, Wayland mostly dead (patched alloy-OSR full MV3, only chrome-UI surfaces by-design); explicit: stay hosted today, patched-CEF = conditional third-engine shape, QtWE replacement still rejected |

## Task details

### CS01
Method mirrors CEF-EXT-SPIKE.md exactly: prebuilt cefsimple (or the
qcefprobe windowed-chrome leg), `--load-extension` pointing at the
UNPACKED bundled zip, xvfb-run, CDP target enumeration. Deltas to
record: our 9.3 bundle vs the tested 10.0 CRX; every API from the
manifest permission list + the grep'd chrome.* usage (the audit list
in the task row notes — re-verify each empirically, not from docs).
The manifest declares: activeTab, alarms, clipboardRead/Write,
contextMenus, idle, offscreen, scripting, sidePanel, storage,
tabs, unlimitedStorage, webNavigation, webRequest,
webRequestAuthProvider, notifications; optional nativeMessaging,
privacy.
Verify: every gap-list entry measured (works/typed-error/crash), not
assumed. Popup opens and reaches unlock UI = the headline result.

### CS02
qcefprobe is standalone qmake in spikes/cef/ — extend, don't rewrite.
Focus/IME/HiDPI are the known-weak spots of reparented foreign
windows; measure honestly. legs: focus-gain/loss, ascii typing,
dead-key/CJK input surface (verdict only, no IME stack needed —
report what breaks), resize (live + final), deviceScaleFactor>1,
wheel scroll, Tab into/out of view.
Verify: ./run-render.sh style script produces a per-leg PASS/FAIL
table; append findings to CEF-BW-VERDICT or new CS02 report doc.

### CS03
Read .devin/ENGINE-ADAPTER.md + src/engine/engineinterface.h for the
capability contract. The split: chrome-style windows embed via X11
reparent (Linux only-ish, Wayland = OSR-only = no extensions);
design how `cef` backend reports both honestly — compare Servo's
runtime-conditional requestInterception. Draft the caps.rs row
shape, don't implement.
Verify: doc reviewed against the real capabilities() signatures.

### CS04
Relevant: src/sandbox/bwrapgenerator.cpp, qcefprobe run scripts.
Real Chromium sandbox wants the setuid helper (chrome-sandbox) or
CLONE_NEWUSER; bwrap-wrapping a sandboxed Chromium = double sandbox
(test: does it nest or break?). Profile mapping is facade-owned —
read RC03/RC09 persona resolution before deciding paths.
Verify: sandbox actually enforced (cat /proc/<renderer>/status
NSpid/seccomp), persona dirs isolated.

### CS05
Synthesis only — no new code. Reuse CEF-SPIKE.md touchpoint counts,
CS01/CS02/CS04 findings, ENGINE-ADAPTER contract. The recommendation
must name which option (replace / third engine / reject) and why.
Verify: doc exists, table complete, recommendation explicit.

### CS06
cefsimple already lives in ~/cef-cache (prebuilt dist). Legs:
(a) --use-alloy-style windowed + --load-extension=<unpacked bw zip> —
does the ext install at all (chrome.runtime.lastError / install event)?
(b) does its SW register + boot under alloy? (c) chrome.storage.local
round-trip? (d) cs<->sw messaging on a real page? (e) an alloy-style
browser navigated to chrome-extension://<id>/popup/index.html — does it
render (the popup-in-our-OSR question)? Also probe OSR specifically:
cefclient --off-screen-rendering-enabled --use-alloy-style.
Each leg measured pass/fail + exact failure shape. This row exists so
CS08 patches only what's actually broken — the maintainer says
RequestContext-level works; verify, don't trust.

### CS07
Pin: the CEF branch matching the lane's verified build (152.x stable —
match cef-builds index entry used earlier, record the exact
cef/chromium version pair). Standard CEF build flow: depot_tools in
~/depot_tools, fetch --nohooks cef, cd chromium/src + cef checkout,
gclient sync --with_branch_heads --nohooks, then ./cef/create.sh +
ninja -C out/Release_GN_x64 cef. ALL detached/nohup — the 2GB cap kills
foreground compiles; ninja resumes. Disk guard: <10GB free = blocked
with reason, never delete user data. If checkout alone exceeds
remaining disk, record and block honestly.

### CS11
Verification only — no build changes. cs07-build.service runs
cs07-verify.sh on exit and drops ~/cef-cache/cs07-result.json.
Confirm libcef.so + cefsimple + chrome_sandbox at the recorded pin,
cefsimple loads a test URL (offscreen/xvfb), record artifact sizes,
wall-clock build time, GN_DEFINES actually used, reproduce command
in .devin/CEF-SOURCEBUILD.md.

### CS08
Grafts live as .patch files under spikes/cef/patches/ + BUILD.md.
G1 extension-page host: whatever CS06 proved missing for alloy-style
chrome-extension page rendering + install path — likely thin delegate
wiring, since the machinery exists for chrome-style.
Verify: BW popup rendered inside an OSR view (pixel witness).

### CS12
G2 nativeMessaging: chromium has extensions::NativeMessagingHost +
host-manifest resolution under chrome/browser/native_messaging —
graft into CEF's chrome runtime or as an embedder-side service; honor
the real manifest discovery dirs, spawn the host, length-prefixed
stdio JSON both directions, disconnect semantics.
Verify legs: connectNative echo fixture; absent host = typed error.

### CS09
chrome/browser/extensions or //extensions level: onAuthRequired fires
for extensions with webRequestAuthProvider; answer path must reach a
real credential provider. Measure scope first — if it requires the
chrome/browser auth UI stack, document the honest scope and skip to a
typed verdict.

### CS10
Synthesis. The deciding number: lines-of-patch vs Chromium-bump
rebase risk, vs the 20-34wk estimate from CEF-VERDICT and the
119-patch/ESR Gecko bill. Reconcile with the corrected #3859
understanding. Explicit: stay on hosted runtime / adopt patched CEF
as third engine / adopt as QtWebEngine replacement.
