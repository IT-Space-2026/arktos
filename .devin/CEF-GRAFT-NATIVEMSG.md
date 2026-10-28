# CEF-GRAFT-NATIVEMSG — G2: nativeMessaging on CEF (CS12)

**Verdict: G2 is a zero-line graft.** The whole upstream stack —
`chrome/browser/extensions/api/messaging/` →
`ChromeMessagingDelegate` → `NativeMessageHost::Create` +
host-manifest discovery (`chrome::DIR_USER_NATIVE_MESSAGING` =
`<user-data-dir>/NativeMessagingHosts`) — is linked into `libcef.so`
and wired on **both** chrome-style and alloy runtimes, on the shipped
prebuilt as well as the CS07 source build. Every verify leg passes on
the stock binary: **no patch exists because none is needed.**

Prior "absent" verdicts (CEF-BW-VERDICT, CEF-VERDICT, CEF-ALLOY-EXT)
were a **fixture artifact**: Chromium hides permission-gated API
surfaces when the extension doesn't hold the permission, and the
earlier measurements used Bitwarden, which declares `nativeMessaging`
in `optional_permissions` — ungranted → `connectNative` is
`undefined`. Reproduced exactly with a no-permission fixture below.

## Measured results (spikes/cef/cs12/)

`run.sh` + `probe.py` drive the `nmprobe` fixture (MV3, mandatory
`nativeMessaging` permission) over CDP from the extension SW context.
`opt_probe.py` + `opt-leg.sh` drive `nmprobe-opt` (permission in
`optional_permissions`) through the grant flow. Host fixture:
`fixtures/nmprobe/host/echo_host.py` (length-prefixed stdio JSON
echo + `__exit__` + EOF bookkeeping).

### Mandatory permission — 11/11 PASS on every runtime

| Leg | src-chrome | src-alloy | prebuilt-chrome |
|---|---|---|---|
| SW boot | PASS | PASS | PASS |
| `connectNative`/`sendNativeMessage` present | PASS | PASS | PASS |
| connectNative echo round-trip | PASS | PASS | PASS |
| multi-message same port | PASS | PASS | PASS |
| `sendNativeMessage` one-shot | PASS | PASS | PASS |
| host exits mid-port → `onDisconnect` `"Native host has exited."` | PASS | PASS | PASS |
| `port.disconnect()` reaps host (pid dead, clean stdin EOF) | PASS | PASS | PASS |
| fresh `connectNative` spawns fresh host (new pid) | PASS | PASS | PASS |
| absent host → `"Specified native messaging host not found."` | PASS | PASS | PASS |
| `sendNativeMessage` absent host → same typed error | PASS | PASS | PASS |
| `allowed_origins` mismatch → `"Access to the specified native messaging host is forbidden."` | PASS | PASS | PASS |

Per-leg JSON: `~/cef-cache/cs12-{src-chrome,src-alloy,prebuilt-chrome}.json`.
Error strings are the exact upstream `native_messaging_api` messages;
manifest discovery honored the user-level dir
(`<user-data-dir>/NativeMessagingHosts/*.json`) written by the probe —
no system paths, no sudo.

### The "absent" artifact, reproduced

`nmprobe-noperm` (identical fixture minus the permission) on the same
prebuilt binary: `connectNative`/`sendNativeMessage` are `undefined`,
`chrome.runtime.connect` still a function — the identical shape CS01
recorded for Bitwarden. `~/cef-cache/cs12-prebuilt-noperm.json`.

### Optional-permission grant flow (`nmprobe-opt`)

| Step | Result | Detail |
|---|---|---|
| ungranted surface | measured | `connectNative` undefined, `permissions.contains` → false |
| `permissions.request` from SW | FAIL (upstream-typed) | `"This function must be called during a user gesture"` — same as stock Chrome |
| trusted click on options page → `permissions.request` | measured pending | reaches `ExtensionInstallPrompt::ShowDialog` (`permissions_api.cc`), promise held on the modal permission dialog; not auto-acceptable via CDP |
| post-grant state (prefs seeded exactly as `GrantOptionalPermissions` writes: `active_permissions` + `granted_permissions` api+=nativeMessaging) | PASS | `connectNative` → function, `contains` → true, `permissions.request` now auto-grants, echo round-trip PASS |

`~/cef-cache/cs12-prebuilt-{opt,seeded}.json`. The pending-on-prompt
leg means the prompt machinery is live; whether the modal renders
visibly in `cefsimple`'s window was not pixel-verified (bare Xvfb, no
WM — CDP input goes to web contents, not views dialogs). Post-grant
behavior is fully verified via the seeded-prefs equivalent.

## Implications

- **BW desktop/biometric path is viable**: `nativeMessaging` is an
  optional permission in Bitwarden's manifest — the real flow is a
  popup/options-UI `permissions.request` (gesture → prompt → grant →
  API unhides → connectNative). Every post-grant piece verified.
- **Corrections propagated** to CEF-VERDICT.md, CEF-BW-VERDICT.md,
  CEF-CAPS.md, CEF-ALLOY-EXT.md: "absent" → "present behind the
  permission; hidden when ungranted (upstream gating)".
- **CS10 cost column**: G2 patch lines = 0. The remaining honest gap
  for this surface is the permission-prompt modal accept, which is a
  chrome UI surface question, not a messaging-stack one.
- Wayland is orthogonal: messaging runs in the browser process on both
  runtimes; OSR/headless embedders get it for free.

## Repro

```bash
spikes/cef/cs12/run.sh all          # 3 runtimes x 11 legs
spikes/cef/cs12/opt-leg.sh          # optional-perm grant arc
```
