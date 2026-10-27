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
| BW04 | storage.local persistence + quota — details below | done | 0 | storage.local verified end-to-end on committed code: --extension-storage-smoke PASS 0 failures twice (real + isolated HOME) — seed child writes 64 multi-KB vault blobs + 40-op concurrent set/get burst coherent, >10MB write fails typed quota-exceeded w/o unlimitedStorage yet succeeds with it, popup sibling context reads the canonical store; verify child after real process restart: all keys+blobs intact, quota-rejected write never persisted, session area cleared, extstorage/<id>/local.json holds seeded keys. --extension-health-smoke PASS 0 failures under isolated HOME (bw honest Limited: missing webRequestAuthProvider+nativeMessaging). CAVEAT: shared ~/.qttest profile (real HOME, no per-run HOME) can never settle — bundled consumed marker outlived an engine-cleaned broken install dir so Bitwarden never reinstalls; env state, not a code regression — record for BW10 |
| BW11 | storage events + session/managed semantics — details below | done | 0 | storage.onChanged now reaches page agents: runtime keeps m_pageAgents per ext (registered on covered hello, dropped on page death, agentCovers re-proved per emit) and fanOutStorageChanged pushes __arktos.storageChanged{ext,changes,areaName} to each covered agent; page writes excludePage themselves (direct emit is the writer echo); agent handler is ext-scoped; managed is Chrome-shaped empty read-only everywhere (agent drops mutators, shim installs its stub unconditionally over the native area whose get(null) rejected and which exposed mutators). Verify: --extension-storage-smoke PASS 0 failures under isolated HOME — sw+cs onChanged shape legs incl oldValue, cs->sw fanout, session cleared across real restart, sync profile-local persistent, managed empty both worlds; --extension-host-smoke real bw-2026.9.3 PASS 0 failures (managed.get served: shim note logs the native-area replacement); --extension-health-smoke PASS 0 failures, BW honestly Limited |
| BW05 | port messaging (connect/onMessage) — details below | done | 4 | echo fixture cs<->hosted bg: connect + port.postMessage both directions + onDisconnect on tab close & sw teardown all measured PASS; sender.tab rides portConnect (one-shot sendMessage sender.tab gap recorded) |
| BW12 | tabs.sendMessage frame routing — details below | done | 2 | frameId delivery generated into the addressed frame's AgentWorldId (agent.__deliverOneShot + parked-response poll — SEC24 forwarder is main-frame-only), per-frame agentCovers gate before injection, both transports emit the same __arktos.deliverResponse; fixture: main/sub/far(=not-found)/data(=no-host-access) legs + real active-tab resolution all PASS, host-smoke 0 failures |
| BW06 | Autofill E2E — details below | done | 5 | autofill E2E REAL: engine-visible manifest loses content_scripts alongside background (doctorExtensionDir re-doctors stale trees via output-consistency; spoofed marker+sidecar pairs rejected) — kills the native-cs duplicate whose tabless senders threw inside triggerAutofillScriptInjection; agent copy is sole cs, sender.tab rides the relay. Leg: seed fake account via storage.local, gate on activeAccountStatus$ settled non-LoggedOut (4 polls; hist [0,1]), handshake re-posts until agent-world cs answers, monitors arm (isMonitoring true), collectPageDetailsImmediately returns real opids, fillForm writes fixture fields. extagent respond() now carries hadListeners so a fired listener answers port-closed not no-receiving-end. Verify: --extension-host-smoke EXTHOST_SMOKE_DIR=bw-2026.9.3 PASS 0 failures twice incl fillForm values read back (user=smoke-user@arktos.test). KNOWN GAP: fixture cs echo-port leg now FAILs — hosted cs->bg runtime.connect path has a real defect native injection always masked (BW05 measurement likely native-carried); sendMessage path proven working; needs follow-up row. |
| BW07 | offscreen document hosting — details below | done | 2 | chrome.offscreen verified working end-to-end on real Bitwarden: createDocument(reasons honored) loads offscreen-document/index.html with shim+relay context, getContexts reports OFFSCREEN_DOCUMENT, sendMessage round-trips doc<->sw both directions, single-doc verbatim/invalid-argument/bad-path typed errors, closeDocument+teardown clean. Also fixed a real defect the leg exposed: sendMessage double-delivery (native realSend + relay both reach real chrome-extension:// contexts) — wired the dead seenDelivery content dedupe per-transport; sw now fires onMessage exactly once. Verify: --extension-host-smoke fixture 21/21 offscreen legs PASS + real bw-2026.9.3 PASS 0 failures incl new exactly-once check; --extension-health-smoke 0 failures, BW still honestly Limited |
| BW13 | clipboard copy path — details below | done | 1 | clipboard path served per Chrome parity: grant-holders' extension chrome gets per-page JavascriptCanAccessClipboard (=Chrome's AllowWriteToClipboard wiring) so execCommand('copy') reaches the real clipboard from hidden/offscreen contexts (Bitwarden's actual path), focused docs auto-grant navigator.clipboard, resolveExtensionChromePermission answers ClipboardReadWrite grant/deny + denies other features typed; hidden navigator.clipboard rejects NotAllowedError for grant AND no-grant alike (Blink focus gate before permissions — real Chrome parity, crbug 1404835). Verify: --extension-host-smoke real bw-2026.9.3 0 failures under xvfb+xcb — execCommand value 'arktos-clip-bw13-exec' OBSERVED on real clipboard, hidden writeText/readText typed parity rejections, popup unfocused typed rejection (env cannot OS-focus a window); fixture all clipboard legs PASS typed-denied + clipboard untouched; --extension-health-smoke 0 failures, BW still honestly Limited |
| BW08 | nativeMessaging real implementation — details below | done | 0 | nativeMessaging real: NativeMessagingSession resolves Chrome-layout manifests (arktos+chromium+google-chrome user dirs, /etc system dirs) w/ name+type+exec+allowed_origins gating, spawns argv+origin via QProcess, uint32-LE JSON stdio both dirs, shim nativePorts + Chrome-worded lastError; sendNativeMessage = one-shot port. Fixture legs PASS x5 (echo rt, one-shot rt, absent=not-found, wrong-origin=forbidden, host-exit=has-exited w/ pre-EOF reply); real BW sendNativeMessage answers typed no-permission (optional grant unconsented). Engine native connectNative overridden — it answered canned "disabled by administrator". runtimeServes+nativeMessaging: BW missing-apis now only webRequestAuthProvider, still honest Limited. Suite FAILs: pre-existing cs echo-port (BW06 defect). Verify: --extension-host-smoke fixture+real bw, --extension-health-smoke, --extension-smoke all measured |
| BW09 | permissions API + optional-permission flow — details below | done | 1 | chrome.permissions real optional-grant surface: contains/getAll report required U consented-optional (runtime grant record persisted per persona, intersected with still-declared optionals on sync; uninstall drops grants); request() filters already-held + undeclared (typed not-optional), drives ExtensionReviewDialog::requestPermissions consent surface (Deny default, ARKTOS_EXT_PERMISSION_ANSWER env for headless legs, headless=no-surface answers honest false) then grantOptionalPermissions + onAdded push; remove() revokes held optional grants + onRemoved; grant feeds every gate (manifestPermissions, hasExtensionPermission, hostPermissionsFor coverage/seesUrls/agentCovers/registerContentScripts, storage/clipboard/native gates). Verify: --extension-host-smoke real bw-2026.9.3 84 PASS 0 failures — contains(nativeMessaging)=false -> request -> true round trip, post-grant sendNativeMessage answers host-not-found not no-permission, remove/deny/undeclared legs all honest; fixture legs all PASS incl privacy gate unlock; --extension-health-smoke 0 failures, BW still honestly Limited |
| BW10 | Integration verification + honest docs — details below | done | 0 | integration verification + honest docs — fixed the real connect->postMessage race (parked-port queue drained at every end registration; was the BW06-recorded echo-port FAIL); --extension-health-smoke 0 failures isolated HOME, bw honest Limited (only webRequestAuthProvider); host-smoke fixture 0 failures + real bw-2026.9.3 84 PASS under xvfb; bitwardenUnlockFill PASS (real WASM unlock + fill); port legs all PASS; stale frameId expectation re-pointed to BW12 not-found; EXT-RUNTIME-GAPS/BASELINE/E2E regenerated — remaining dead: webRequestAuthProvider+FIDO2+webRequest blocking family |

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
