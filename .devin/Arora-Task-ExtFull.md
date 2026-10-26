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
| BW01 | Namespace stub pass — details below | pending | 0 | every chrome.* namespace bundled bitwarden background.js + popup + content scripts touch top-level must EXIST in the hosted context (commands, permissions, management, dom, i18n, extension.getViews/getBackgroundPage, runtime.getContexts, storage.managed/session/sync, tabs.captureVisibleTab + any found missing). Stubs return Chrome-typed errors for uncalled members, minimal real behavior for called-at-eval members. Verify: instrumented eval — hosted background.js finishes without ReferenceError/TypeError, host reaches Ready |
| BW02 | Hosted-only background routing — details below | pending | 0 | extensions needing namespaces Qt's native sw lacks must not double-run: the hosted runtime is their authoritative background; the native registration path must be skipped/disabled for them (not left racing). Verify: exactly one background context per ext id in a debug dump; no native sw crash loop in logs |
| BW03 | Popup round-trip — details below | pending | 0 | needs:tool:xvfb-run; action popup (ExtensionActionBar host) opens → runtime.sendMessage reaches hosted sw → response returns. Bitwarden popup must reach its unlock/vault UI, not a blank page. Verify: xvfb screenshot or DOM probe of popup shows BW UI; message round-trip logged |
| BW04 | storage correctness — details below | pending | 0 | storage.local in hosted context: vault-shaped writes persist across app restart, onChanged delivers to listeners, storage.session is per-run, managed/sync return correct empty/disabled semantics. Verify: fixture round-trip over a restart + onChanged observed |
| BW05 | cs<->sw messaging bridge — details below | pending | 0 | runtime.connect/onMessage/tabs.sendMessage between declared content scripts and hosted background on real loaded pages (not just eval probes): long-lived ports deliver both directions, disconnect propagates. Verify: echo fixture content script <-> background |
| BW06 | Autofill E2E — details below | pending | 0 | needs:task:BW05; on a fixture login form Bitwarden's declared content scripts inject, sw receives their messages, scripted fill lands in fields (or honest partial verdict documenting exactly where the chain breaks). Verify: fixture field values observed post-fill |
| BW07 | Clipboard + offscreen path — details below | pending | 0 | offscreen document creates in hosted runtime; clipboardWrite/Read path works for the copy-password flow. Verify: copied value observed on the real clipboard |
| BW08 | nativeMessaging real implementation — details below | pending | 0 | implement chrome.runtime.connectNative/sendNativeMessage for real: resolve native messaging host manifests (standard per-user + system dirs, Chrome layout), spawn via QProcess, 4-byte-LE length-prefixed JSON stdio both directions, disconnect semantics. Absent host = correct typed error (the honest answer when no BW desktop installed). Verify: fixture native host echo round-trip + absent-host typed error |
| BW09 | permissions API + optional-permission flow — details below | pending | 0 | chrome.permissions.contains/request/onAdded/onRemoved for Bitwarden's optional set (nativeMessaging, privacy): contains answers honestly, request drives the real consent surface, grants feed the runtime. Verify: contains(nativeMessaging)=false→request→true round-trip |
| BW10 | Integration verification + honest docs — details below | pending | 0 | needs:task:BW01,BW02,BW03,BW06; --extension-health-smoke green; Bitwarden health verdict reflects REAL measured state (Healthy only if actually healthy — never gamed); update .devin/EXT-RUNTIME-GAPS.md + EXT-RUNTIME-E2E.md with the new served surface + what stays dead (FIDO2/passkeys, anything unverifiable) |

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
read/write, quota behavior (unlimitedStorage declared). onChanged
must fire in the sw AND any content listeners.

### BW05
Long-lived Port semantics: Port.postMessage both directions,
onDisconnect on tab close + sw restart. tabs.sendMessage routing must
hit the right frame's content script (frameId semantics where
declared).

### BW06
Fixture: local login-form page + scripted expectation. If the chain
breaks at a specific hop (e.g., scripting.ExecutionWorld unsupported),
document it precisely — honest partial beats fake pass.

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
