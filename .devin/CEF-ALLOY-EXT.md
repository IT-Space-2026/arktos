# CEF-ALLOY-EXT — extension reality check on alloy-style surfaces (CS06)

Question: is CEF#3859's maintainer ruling true — "RequestContext-level
extension functionality works under Alloy style; only Chrome-UI
surfaces die by design"? Measured, not trusted. This result defines
exactly which grafts CS08 must write.

Build: same prebuilt pin as the whole lane —
CEF `152.0.15+gb3d72fb` / Chromium `152.0.7977.169` linux64.
Extension: bundled `bitwarden-2026.9.3.zip` unpacked
(`~/cef-cache/bw93_unpacked`, id `plbhojmhbpcgpakacejjnafckaelalbe`).

Harness: `spikes/cef/alloy-ext/` (`probe.py` CDP driver + `run.sh`).
Two shapes measured:

- **alloy-windowed** — prebuilt `cefsimple --use-alloy-style
  --load-extension=<dir>` under `xvfb-run`. Result:
  `~/cef-cache/cs06-alloy-windowed.json`, popup witness
  `~/cef-cache/cs06-popup-alloy-windowed.png`.
- **alloy-osr** — our own `qcefprobe --mode=osr` (`SetAsWindowless`
  forces ALLOY per `cef_window_info.h` — the Wayland shape), offscreen,
  extension + CDP injected via its `CEF_SWITCHES` hatch. Result:
  `~/cef-cache/cs06-alloy-osr.json`, popup witness
  `~/cef-cache/cs06-popup-alloy-osr.png` (visually verified: full BW
  popup UI — logo, default-password-manager prompt, Continue/Skip).

## Measured per-surface table

| Leg | alloy-windowed | alloy-osr | Detail |
|---|---|---|---|
| `--load-extension` install | **PASS** | **PASS** | SW target `chrome-extension://<id>/background.js`; `management.getSelf()` → `installType:"development"`, `enabled:true`, v2026.9.3 |
| SW register + boot | **PASS** | **PASS** | `runtime.getContexts` → live `BACKGROUND` ctx; BW `background.js` ran (WASM SDK loaded) |
| `storage.local` round-trip | **PASS** | **PASS** | set/get `cs06:"ok"` |
| cs↔sw messaging | **PASS** | **PASS** | BW content-script world (`"Bitwarden Password Manager"`, non-default ctx) injected on `http://` fixture; `sendMessage` echo round-trip returned `{echo:"ping-from-cs", senderTab:<id>}` |
| `chrome-extension://` page in the view | **PASS** | **PASS** | navigated the live view to `popup/index.html` → Angular app booted, routed `#/default-password-manager-prompt`, `app-root` present, all 4 bundles loaded; CDP screenshot = real pixels (both shapes) |
| `chrome.action` API | accepted | accepted | `setTitle` resolved without error — API live, UI surface absent by design |
| `chrome://extensions` | **DEAD** | **DEAD** | `Page.navigate` refused — view stayed on previous URL. Chrome-UI surface, dies as designed |
| `tabs.query({})` | `[]` | `[]` | alloy views are **not enumerated** in the extension tab model — yet `sender.tab.id` IS populated on incoming messages (613831633 / 1342028606). Tab objects exist; the query path doesn't cover alloy |
| `scripting.executeScript(tabId)` | typed-error | typed-error | `"No tab with id: 1"` — consequence of the tab-model gap: by-id scripting can't reach alloy tabs |
| `connectNative`/`sendNativeMessage` | absent | absent | same as chrome-style (CS01) — G2 graft still needed — **CS12 correction**: works unpatched on both runtimes (11/11 legs incl. alloy); "absent" was ungranted-permission API hiding. See CEF-GRAFT-NATIVEMSG.md |
| `identity`, `privacy` | absent | absent | same as chrome-style — optional/undeclared perms |
| `webRequest`, `offscreen`, `sidePanel`, `contextMenus`, `notifications`, `alarms`, `windows`, `webNavigation`, `i18n` | present (`object`) | present (`object`) | namespace exists; per-API call depth not re-measured here (CS01 covered the call depth on chrome-style) |

## Verdict

**Maintainer confirmed — and exceeded.** Not only does
RequestContext-level functionality run under alloy, the
`chrome-extension://` **page surface renders inside our own OSR
view** — the popup-in-OSR question CS08's graft G1 feared is already
solved by the runtime. The OSR/Wayland shape is a real extension host:
SW + storage + cs↔sw + extension-page rendering all verified.

What is actually dead/missing under alloy:

1. **Tab-model enumeration** — `tabs.query`/`windows` return nothing;
   `scripting.executeScript(tabId)` typed-errors. Manifest-declared
   content scripts still inject and `sender.tab` is populated, so the
   objects exist — the enumeration surface just isn't wired to alloy
   browsers. (Graft candidate for CS08, smaller than the feared
   extension-page host.)
2. **Chrome-UI surfaces** — `chrome://extensions`, action toolbar,
   sidePanel inset. Dead by design, not sanely graftable; the embedder
   presents its own UI (popup rendering itself works).
3. **`nativeMessaging`** — ~~absent identically on chrome-style; CS08
   G2~~ **CS12 correction**: works unpatched on alloy too (all 11 legs
   PASS); earlier "absent" was upstream permission-gated API hiding.

Honesty notes: `webRequest`/`offscreen`/`sidePanel.setOptions` etc.
were presence-checked on alloy, not call-depth-checked (CS01's call
depth was on chrome-style; the RequestContext machinery is shared).
MV3 SWs idle-terminate — CDP must attach + `Runtime.enable` promptly
or evaluate hangs (observed, workaround in probe).

CS08 scope update: G1 shrinks to *tab-model enumeration on alloy*
(+ embedder-side popup window, which needs no CEF patch); G2
nativeMessaging unchanged; the popup-render patch is **not needed**.
