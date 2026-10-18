<!--
ER06 decision gate — generated verbatim by the bitwardenHandshakeGate
leg in autotests/extensionmanager/tst_extensionmanager.cpp.  Regenerate:

  source .devin/qt-env.sh
  env HOME=<scratch> XDG_CONFIG_HOME=<scratch>/.config \
      XDG_DATA_HOME=<scratch>/.local/share XDG_CACHE_HOME=<scratch>/.cache \
      QT_QPA_PLATFORM=offscreen QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
      ARKTOS_EXTTEST_DIR=<unpacked extension dir> \
      ARKTOS_EXTTEST_OUT=$PWD/.devin/EXT-RUNTIME-SPIKE.md \
      ./autotests/extensionmanager/tst_extensionmanager bitwardenHandshakeGate

The live Bitwarden bundle stays read-only — the leg fixture-copies it.

Verdict semantics: PASS = the real bundle's declared content scripts
reached the real hosted background through the ER01/02 bridge in BOTH
directions (sendMessage + connect in; tabs.sendMessage deliver + the
content-message-handler's real forward-back reply out), each carrying a
Chrome-shaped sender.tab — no isolated-world identity check blocked any
of it.  A BLOCK verdict here would have recorded the exact failing
primitive instead; the gate PASSED, so the lane proceeds to the
mechanical namespaces (scripting, offscreen, ...).  The two typed gaps
below are the expected ER07/ER11 work items, not gate failures.
-->

# ER06 — hosted-runtime decision gate (Bitwarden handshake)
generated: 2026-10-18T22:27:17Z
bundle: bitwarden-2026.9.3_2QIFbk name="__MSG_extName__" version=2026.9.3
installed id: papmgageojnkkcmkjbajbecedfgamofe
host state: ready
tab: id=2 url=http://127.0.0.1:45171/login
VERDICT: PASS — the cs<->bg handshake crossed the world-emulation boundary through real Bitwarden code on both ends; no world-identity requirement surfaced.

## handshake legs
- cs->bg sendMessage: triggerAutofillScriptInjection delivered with sender.tab.id=2 — PASS
- cs->bg connect: autofill-injected-script-port accepted by the real onConnect with sender.tab.id=2 — PASS
- bg dispatch: 1 executeScript call(s) (scripting/tabs) after the handshake — typed 'unsupported' gap recorded (ER07), PASS
- bg->cs deliver + real cs forward-back: bgUnlockPopoutOpened round-tripped — PASS

## background touched: alarms.clear=4, alarms.create=6, alarms.get=6, chrome.i18n.getMessage=13, chrome.permissions.contains=1, chrome.runtime.sendMessage=1, chrome.scripting.executeScript=1, chrome.tabs.captureVisibleTab=1, chrome.tabs.connect=1, chrome.tabs.detectLanguage=1, chrome.tabs.discard=1, chrome.tabs.duplicate=1, chrome.tabs.executeScript=1, chrome.tabs.getAllInWindow=1, chrome.tabs.getSelected=1, chrome.tabs.getZoom=1, chrome.tabs.getZoomSettings=1, chrome.tabs.goBack=1, chrome.tabs.goForward=1, chrome.tabs.highlight=1, chrome.tabs.insertCSS=1, chrome.tabs.move=1, chrome.tabs.reload=1, chrome.tabs.remove=1, chrome.tabs.removeCSS=1, chrome.tabs.sendMessage=1, chrome.tabs.sendRequest=1, chrome.tabs.setZoom=1, chrome.tabs.setZoomSettings=1, relay.portAccept=1, relay.register=1, relay.respond=2, relay.send=1, runtime.getContexts=1, sidePanel.setOptions=1, tabs.get=1, tabs.query=2, tabs.sendMessage=1
## agent probe: active=1 bridgeUp=1 deliveries=1 ports=1
agent gaps:
agent errors:
host shimGaps:
- chrome.scripting.executeScript: unsupported
- chrome.webNavigation.getAllFrames: unsupported

## gate capture (every cs->bg crossing)
- msg {"command":"triggerAutofillScriptInjection","origin":"http://127.0.0.1:45171","tab":2,"url":"http://127.0.0.1:45171/login"}
- msg {"command":"bgUnlockPopoutOpened","origin":"http://127.0.0.1:45171","tab":2,"url":"http://127.0.0.1:45171/login"}
- port {"frameId":null,"name":"autofill-injected-script-port","tab":2}

## unlock-init dependency: chrome.offscreen
- surface: {"supported":true}
- hasDocument() answer: {"r":false,"err":null}
- note: a fresh install has no enrolled account, so the real unlock-init (Argon2 in the offscreen document) is not reachable from the harness.  chrome.offscreen resolves to a live ENGINE binding here (hasDocument answered false — the schema exists, the delegate may not) — whether createDocument yields a working document is ER10's measurement, not assumed.
