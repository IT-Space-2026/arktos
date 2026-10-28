# CEF-BW-VERDICT — bundled Bitwarden 2026.9.3 on CEF's chrome runtime (CS01)

Delta vs CEF-EXT-SPIKE.md: that run verified a fetched 2026.10.0 CRX;
this run reruns the same method against **our bundled zip**
(`src/data/extensions/bitwarden-2026.9.3.zip`, MV3) and measures every
`chrome.*` namespace the bundle touches — empirically, not from docs.

## Build / method

- CEF `152.0.15+gb3d72fb` / Chromium `152.0.7977.169` linux64, prebuilt
  `cefsimple` (`_client` dist, chrome bootstrap), `--use-native
  --load-extension=<unpacked zip>` under `xvfb-run`, `--no-sandbox`.
- Harness: `spikes/cef/bw-audit/run.sh` + `probe.py` (CDP driver).
  Artifacts outside the repo: `~/cef-cache/bw93_unpacked`,
  `~/cef-cache/cs01-result.json` (full measured matrix).
- Extension ID under `--load-extension`:
  `plbhojmhbpcgpakacejjnafckaelalbe` (differs from the store CRX id —
  keyless dev load; expected).
- Namespace coverage enumerated by grepping the unpacked bundle:
  runtime, tabs, i18n, windows, scripting, storage, privacy,
  webNavigation, extension, offscreen, sidePanel, permissions, dom,
  webRequest, notifications, management, alarms, commands, action,
  contextMenus, idle (+ MV2 `browserAction` fallback paths). The bundle
  also references `identity`/`nativeMessaging` on feature-detected
  paths.

## Headline results

- **background.js boots**: live `service_worker` target
  `chrome-extension://<id>/background.js`; `management.getSelf` reports
  `enabled: true`, version `2026.9.3`, all 16 manifest permissions
  granted.
- **Popup opens and is interactive**: `popup/index.html` boots the
  Angular app (polyfills/vendor/vendor-angular/main all loaded), routed
  to `#/default-password-manager-prompt`; clicking **Skip** routed to
  `#/intro-carousel` ("Create account / Log in"). Unlock UI is
  unreachable on a fresh profile *by design* (needs an account — same
  as stock Chrome); the measurable claim — popup app boots, routes,
  responds to input — is verified.
- **cs↔sw messaging works**: Bitwarden's document_start content script
  ran on the fixture page in an isolated world with `chrome.runtime`
  bound (id + manifest version match). `sendMessage` with an unknown
  command got `port closed` lastError (= delivered to a live listener
  that chose not to reply); a probe echo listener installed in the SW
  answered `{echo:'ping-from-cs', senderTab:<id>}` — positive
  round-trip verified.

## Measured gap table

Legend: **works** = real values returned; **typed-error** = API present,
fails with correct Chrome semantics; **absent** = `undefined`
namespace/function.

| API surface | Measured |
|---|---|
| `storage.local` set/get | **works** (round-trip `42`) |
| `storage.session` set/get | **works** |
| `storage.sync` get | **works**, returns `{}` (no sync backend — expected) |
| `storage.managed` get | **works**, returns `{}` (no policy — expected) |
| `storage.getBytesInUse` | **works** (7739 B) |
| `runtime.getContexts` | **works** (BACKGROUND ctx listed) |
| `runtime.getPlatformInfo` / `getManifest` | **works** (linux/x86-64, 2026.9.3) |
| `runtime.connectNative` / `sendNativeMessage` | **ABSENT** — nativeMessaging surface missing entirely — **CS12 correction**: not absent; hidden because BW declares it `optional_permissions` ungranted. Fully works with the permission held (11/11 legs, incl. prebuilt). See CEF-GRAFT-NATIVEMSG.md |
| `commands.getAll` | **works** — all 6 BW commands incl. shortcuts |
| `permissions.contains` | **works** (nativeMessaging → false) |
| `permissions.request` | **typed-error** "must be called during a user gesture" — correct Chrome semantics |
| `i18n.getMessage` / `getUILanguage` | **works** ("Bitwarden Password Manager", en-US) |
| `management.getSelf` | **works** (full manifest reflected) |
| `notifications.create` | **works** (notification id returned) |
| `idle.queryState` | **works** |
| `alarms` create/get/clear | **works** |
| `contextMenus` create/remove | **works** |
| `tabs.query` / `tabs.get` | **works** |
| `tabs.captureVisibleTab` | **typed-error** "Either the '<all_urls>' or 'activeTab' permission is required" — permission-gated (activeTab needs a gesture), not broken |
| `tabs.executeScript` (MV2) | **ABSENT** — expected under MV3 |
| `scripting.executeScript` | **works** (returned `document.title`) |
| `scripting.register/unregisterContentScripts` | **works** |
| `windows.getAll` | **works** |
| `webNavigation.getAllFrames` | **works** |
| `webRequest.onAuthRequired` `asyncBlocking` | **works** — listener accepted (webRequestAuthProvider permission honored); CS09 verified end-to-end: answers reach the network layer (nav + subresource, answer/cancel/re-challenge), zero-line graft, see CEF-GRAFT-AUTH.md |
| `webRequest.onBeforeRequest` `blocking` | **works** — listener accepted |
| `offscreen.hasDocument` / `createDocument` (CLIPBOARD) | **works** — offscreen doc spawned (visible as `background_page` CDP target) |
| `sidePanel.setOptions` | **works** |
| `sidePanel.open` | **typed-error** "may only be called in response to a user gesture" — correct semantics; whether the side-panel **inset renders** inside a CEF chrome window is *not verified* (CS02 scope) |
| `privacy` | **ABSENT** — optional permission not granted; cannot distinguish permission-gated vs missing-on-CEF from this run |
| `identity` | **ABSENT** — not declared in the 9.3 manifest; same caveat |
| `extension.getViews` / `getBackgroundPage` (MV2) | **ABSENT** — expected under MV3 |
| `dom.openOrClosedShadowRoot` | present + callable (invocation error on bogus arg = real impl, not a stub) |
| `browserAction` (MV2) | **ABSENT** — expected |
| clipboardRead/Write | not directly probed; the CLIPBOARD-reason offscreen document BW uses for clipboard work was created successfully |

## Go / no-go

**GO on extension fidelity.** The bundled 9.3 extension's full
manifest permission set is either functional or fails with correct
Chrome-typed errors. Real deltas vs stock Chrome:

1. `nativeMessaging` is **absent** (not just permission-denied) —
   BW's desktop-app/biometric unlock path is dead on CEF. Optional
   feature; the vault itself doesn't need it. **CS12 correction
   (2026-10-28)**: wrong — `connectNative` was hidden by upstream
   permission gating (BW lists it under `optional_permissions`, never
   granted in this probe). With the permission held, the full
   native-messaging stack works unpatched on prebuilt CEF: manifest
   discovery, host spawn, stdio JSON, typed errors. The grant path is
   the popup/options-UI `permissions.request` → `ExtensionInstallPrompt`
   → API unhides. See `.devin/CEF-GRAFT-NATIVEMSG.md`.
2. `privacy`/`identity` namespaces absent in this configuration —
   optional/undeclared perms; verdict "unverified", not "broken".
3. `sidePanel` API surface exists and accepts calls; inset rendering in
   the host window unverified.
4. MV2 legacy surface gone — irrelevant for this MV3 target.

Nothing in the gap table blocks Bitwarden's core loop
(SW + storage + scripting + cs↔sw messaging + popup). CEF's chrome
runtime is a real extension host, not a stub.
