# Arktos — EXT-FULL lane task table

Lane: `ext-full` branch, worktree `/home/szefi/Documents/arora-extfull`.
Goal: make bundled + user-installed Chrome extensions ACTUALLY WORK on
the hosted chrome.* runtime — Bitwarden is the proving extension.
"Done" means empirically running, not a green health badge.

Prior art — read before starting:
- `.devin/EXT-RUNTIME-GAPS.md`, `.devin/EXT-RUNTIME-E2E.md` — what the
  hosted runtime serves today.
- `src/extensions/extensionmanager.cpp` — unsupportedPermissions /
  runtimeServes / health verdict machinery.
- `src/extensions/extensionshimbackend.cpp` + `extensioneventhub.cpp`
  + `extensionbackgroundhost.cpp` — the hosted runtime itself.
- The measured Bitwarden surface (from gap audit): background.js
  touches chrome.action, alarms, browserAction, commands, contextMenus,
  dom, extension.getViews/getBackgroundPage, i18n, idle, management,
  notifications, offscreen, permissions, privacy.services, runtime
  (connectNative, getContexts, getManifest, getPlatformInfo, getURL,
  onConnect, onInstalled, onMessage, reload, sendMessage), scripting,
  sidePanel, storage(local/managed/onChanged/session/sync), tabs
  (captureVisibleTab/create/get/getCurrent/on*/query/remove/sendMessage/
  update), webNavigation, webRequest, windows. Native sw dies on
  chrome.webNavigation undefined; hosted host reaches Ready but
  connectNative disconnects.
- Honesty rule everywhere: a stub answers Chrome-typed errors, never
  pretends success; the health verdict must keep reflecting reality.

| ID | Task | Status | Attempts | Notes |
|----|------|--------|----------|-------|
| BW01 | Namespace stub pass — details below | done | 2 | every chrome.* ns/member bitwarden bg+popup+cs touch resolves: added chrome.dom (shim+agent, open roots only) + tabs.TAB_ID_NONE; ARKTOS_EXTSHIM_NSTRACE get-trap instrument measures zero undefined accesses; verify: --extension-host-smoke ARKTOS_EXTHOST_SMOKE_DIR=bw-2026.9.3 host Ready, zero unshimmed-API errors, audit 121/121 (popup-gate/cs legs fail = BW03/BW05, pre-existing); fixture variant 31/0 PASS |
| BW02 | Hosted-only background routing — details below | done | 0 | hosted-only background routing: engine-visible manifest stripped of background via staged/doctored trees (dir, zip via rc_ext_unpack_package, legacy installs doctored at sweep+reload); declared manifest kept in verified sidecar honored only in managed dirs; probes skip hosted-only exts, host state is the verdict; detail reports background=hosted. Verify: --extension-health-smoke PASS, zero native sw registrations in log, Bitwarden host Ready/Limited |
| BW03 | Popup round-trip — details below | done | 2 | popup round-trip real: permissions.contains/getAll stopped claiming optional_permissions as granted (Chrome: ungranted until request()) — the lie sent Bitwardens guarded privacy.* probe into a typed error whose callback threw inside the shim, hanging browserAutofillSettingsOverridden so the router never resolved. Fix unblocks the chain: popup opens, session/new_popup ports + runtime.sendMessage round-trip, router reaches the unlock/login gate. Smoke clicker learned the default-password-manager-prompt onboarding screen. Verify: xvfb ARKTOS_DEBUG_POPUP screenshot shows real BW onboarding UI; --extension-host-smoke on bw-2026.9.3 — real popup presents its unlock/login gate PASS (pw+email inputs at /login), sendMessage-resolved PASS, only pre-existing cs leg FAIL (BW05); --extension-health-smoke PASS 0 failures, verdict Limited honest |
| BW04 | storage.local persistence + quota — details below | blocked | 5 | blocked:exceeded 5 restarts |
| BW11 | storage events + session/managed semantics — details below | blocked | 0 | needs:task:BW04; storage.onChanged delivers to sw AND content listeners (changes dict + area name shape correct), storage.session is per-run (cleared on restart), managed/sync return correct empty/disabled semantics. Verify: onChanged observed + session-cleared leg blocked:dep BW04 blocked |
| BW05 | port messaging (connect/onMessage) — details below | done | 4 | echo fixture cs<->hosted bg: connect + port.postMessage both directions + onDisconnect on tab close & sw teardown all measured PASS; sender.tab rides portConnect (one-shot sendMessage sender.tab gap recorded) |
| BW12 | tabs.sendMessage frame routing — details below | done | 2 | frameId delivery generated into the addressed frame's AgentWorldId (agent.__deliverOneShot + parked-response poll — SEC24 forwarder is main-frame-only), per-frame agentCovers gate before injection, both transports emit the same __arktos.deliverResponse; fixture: main/sub/far(=not-found)/data(=no-host-access) legs + real active-tab resolution all PASS, host-smoke 0 failures |
| BW06 | Autofill E2E — details below | done | 5 | autofill E2E REAL: engine-visible manifest loses content_scripts alongside background (doctorExtensionDir re-doctors stale trees via output-consistency; spoofed marker+sidecar pairs rejected) — kills the native-cs duplicate whose tabless senders threw inside triggerAutofillScriptInjection; agent copy is sole cs, sender.tab rides the relay. Leg: seed fake account via storage.local, gate on activeAccountStatus$ settled non-LoggedOut (4 polls; hist [0,1]), handshake re-posts until agent-world cs answers, monitors arm (isMonitoring true), collectPageDetailsImmediately returns real opids, fillForm writes fixture fields. extagent respond() now carries hadListeners so a fired listener answers port-closed not no-receiving-end. Verify: --extension-host-smoke EXTHOST_SMOKE_DIR=bw-2026.9.3 PASS 0 failures twice incl fillForm values read back (user=smoke-user@arktos.test). KNOWN GAP: fixture cs echo-port leg now FAILs — hosted cs->bg runtime.connect path has a real defect native injection always masked (BW05 measurement likely native-carried); sendMessage path proven working; needs follow-up row. |
| BW07 | offscreen document hosting — details below | pending | 0 | chrome.offscreen document creates in hosted runtime (reasons param honored), its DOM/messaging reachable, document closes cleanly. Verify: offscreen doc created + message round-trip |
| BW13 | clipboard copy path — details below | pending | 0 | needs:task:BW07; clipboardWrite/clipboardRead path works for the copy-password flow — Bitwarden uses both the clipboard API and execCommand('copy') fallback; no grant = typed error, never silent. Verify: copied value observed on the real clipboard |
| BW08 | nativeMessaging real implementation — details below | pending | 0 | implement chrome.runtime.connectNative/sendNativeMessage for real: resolve native messaging host manifests (standard per-user + system dirs, Chrome layout), spawn via QProcess, 4-byte-LE length-prefixed JSON stdio both directions, disconnect semantics. Absent host = correct typed error (the honest answer when no BW desktop installed). Verify: fixture native host echo round-trip + absent-host typed error |
| BW09 | permissions API + optional-permission flow — details below | pending | 0 | chrome.permissions.contains/request/onAdded/onRemoved for Bitwarden's optional set (nativeMessaging, privacy): contains answers honestly, request drives the real consent surface, grants feed the runtime. Verify: contains(nativeMessaging)=false→request→true round-trip |
| BW10 | Integration verification + honest docs — details below | pending | 0 | needs:task:BW01,BW02,BW03,BW06,BW11,BW12,BW13; --extension-health-smoke green; Bitwarden health verdict reflects REAL measured state (Healthy only if actually healthy — never gamed); update .devin/EXT-RUNTIME-GAPS.md + EXT-RUNTIME-E2E.md with the new served surface + what stays dead (FIDO2/passkeys, anything unverifiable) |

## Task details

### BW01
Method: instrument the hosted background eval (env flag or probe
hook) to log each failed namespace/member access; iterate until the
top-level eval completes. Stub shape follows the existing shim
conventions — a namespace object whose members exist and answer
chrome.runtime.lastError-typed errors when called, EXCEPT members
that must return synchronously real values at eval time
(getManifest, getURL, id, on* addListener presence, storage.local
basic surface). List measured, not guessed — diff against the grep
audit + runtime instrumentation.

### BW02
Find where native MV3 sw registration happens (extension load path /
QWebEngineExtension install) vs hosted runtime spawn
(extensionbackgroundhost). For extensions whose manifest needs
unserved-in-native namespaces, skip the native registration entirely —
otherwise two backgrounds race (measured earlier: native crash AND
hosted host both spawned). Keep the decision honest: report in the
health detail which path the background took.

### BW03
xvfb-run (a live display, extension UI is real UI). The Bitwarden
popup reaching its unlock/onboarding route = the leg that matters —
proves sw+storage+messaging+render chain end-to-end. A blank popup
with a live sw is a FAIL to investigate, not a pass to report.

### BW04
Vault-shaped = kilobyte JSON blobs under many keys, concurrent
read/write, quota behavior (unlimitedStorage declared). Persistence
across restart is the leg; event/session semantics are BW11.

### BW11
onChanged payload shape matters (changes dict + area name); it must
fire in the sw AND any content listeners. storage.session stays
memory-only — never touches disk across restarts. managed/sync
answer honest empty/disabled — no managed-policy infra exists.

### BW05
Long-lived Port semantics: Port.postMessage both directions,
onDisconnect on tab close + sw restart.

### BW12
One-shot messaging: tabs.sendMessage(tabId, msg, {frameId}) routes
to the declared frame's content script; callback/promise response
path returns. Active-tab resolution must reflect the real tab.

### BW06
Fixture: local login-form page + scripted expectation. If the chain
breaks at a specific hop (e.g., scripting.ExecutionWorld unsupported),
document it precisely — honest partial beats fake pass.

### BW07
offscreen.createDocument with honest reasons; the hosted runtime owns
the document lifecycle (no Chrome fallback). Bitwarden's vault-lock
and crypto flows run here — messaging to sw must round-trip.

### BW13
Bitwarden copies via the clipboard API and an execCommand('copy')
fallback — cover both paths end to end. A denied clipboard
permission must surface as a typed error, never silent success.

### BW08
Chrome protocol: host manifest JSON at
~/.config/{arktos,chromium,google-chrome}/NativeMessagingHosts/<host>.json
(+ /etc/… system dirs), spawn argv[0] + origin arg, stdin/stdout
messages = uint32 LE length + JSON. Port lifecycle: host exit =
disconnect with typed error. This is OUR runtime — Qt support is not
required.

### BW09
The consent surface exists (extension review dialog + dangerous-
permission deny-by-default) — wire grants into what the runtime
reports; denied optional perms must answer contains()=false and the
API calls must fail typed, never silently work.

### BW10
Never adjust the health classifier to look better — adjust reality,
let the verdict follow. Document the honest remaining-dead list.
