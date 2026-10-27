<!--
ER00 baseline — generated verbatim by the bitwardenGapBaseline leg in
autotests/extensionmanager/tst_extensionmanager.cpp.  Regenerate:

  source .devin/qt-env.sh
  env HOME=<scratch> XDG_CONFIG_HOME=<scratch>/.config \
      XDG_DATA_HOME=<scratch>/.local/share XDG_CACHE_HOME=<scratch>/.cache \
      QT_QPA_PLATFORM=offscreen QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
      ARKTOS_EXTTEST_DIR=<unpacked extension dir> \
      ARKTOS_EXTTEST_OUT=$PWD/.devin/EXT-RUNTIME-BASELINE.md \
      ./autotests/extensionmanager/tst_extensionmanager bitwardenGapBaseline

The live Bitwarden bundle stays read-only — the leg fixture-copies it.
ER18 replays this file: every typed gap must be implemented or moved to
.devin/EXT-RUNTIME-GAPS.md as intentional.

Reading note: `touched` counts API *accesses* (the shim's probes run on
property get, so Bitwarden's feature-detection sweep shows up without
producing gaps); a typed gap is recorded only when an unserved call is
actually *invoked*.  chrome.* event addListener registrations land on
shim event objects silently and do not appear here.
-->

# ER00 — hosted-runtime boot baseline
generated: 2026-10-27T08:50:52Z
bundle: bw-2026.9.3 name="__MSG_extName__" version=2026.9.3
installed id: ineaacigeeojcljdjllkcjickocohgea
host state: ready
popup bridge: up
contexts: BACKGROUND, POPUP
manager row: health=limited hostState=hosted hostError=""
  healthReason: it declares chrome.* APIs Qt WebEngine doesn't provide (webRequestAuthProvider) — the features using them won't work
  missingApis: webRequestAuthProvider
windows created by extension calls: 1

## Typed shim gaps — the lane work order (2 unique, 0 undocumented)
- chrome.action.setIcon: invalid-path  [documented]
- chrome.scripting.executeScript: no-host-access  [documented]

## background context
doc: ready=complete title="" url=chrome-extension://ineaacigeeojcljdjllkcjickocohgea/manifest.json#arktos-hosted
host: state=ready bridgeUp=1 kind=background
touched (41): action.set=2, action.setIcon=1, alarms.clear=4, alarms.create=8, alarms.get=8, chrome.i18n.getMessage=13, chrome.permissions.contains=4, chrome.runtime.sendMessage=3, chrome.scripting.executeScript=1, chrome.storage.managed.get=2, chrome.tabs.detectLanguage=1, chrome.tabs.executeScript=1, chrome.tabs.insertCSS=1, chrome.tabs.removeCSS=1, chrome.tabs.sendRequest=1, idle.setDetectionInterval=1, idle.subscribe=2, management.getSelf=1, permissions.contains=4, relay.portAccept=2, relay.portMessage=3, relay.register=1, relay.send=3, runtime.getContexts=3, scripting.executeScript=1, shim.gap=2, shim.note=9, sidePanel.setOptions=1, storage.changed=8, storage.local.get=28, storage.local.getKeys=1, storage.local.migrated=2, storage.local.set=6, storage.session.get=3, storage.session.set=2, storage.sync.migrated=2, tabs.create=1, tabs.get=1, tabs.query=7, webNavigation.getAllFrames=1, webRequest.subscribe=1
notes (6):
- chrome.storage.managed: native area replaced — empty read-only stub matches Arktos (no policy infra)
- chrome.storage.onChanged: native event replaced — the engine never fires it
- chrome.offscreen: native binding overridden — an engine-spawned document would carry no chromeBridge or relay context
- chrome.sidePanel.setOptions: options recorded host-side; Arktos has no side-panel surface
- chrome.webRequest.onHeadersReceived: webRequest response details: headers main-frame only, no statusCode; subresource completion via resource timing, fetch failures unobservable
- chrome.idle: input is measured inside Arktos windows only; OS-level idle and the locked state are never reported
audit.missing (0): 
errors (0):

## popup context
doc: ready=complete title="Bitwarden" url=chrome-extension://ineaacigeeojcljdjllkcjickocohgea/popup/index.html#/default-password-manager-prompt
host: state=ready bridgeUp=1 kind=page
touched (21): chrome.extension.getBackgroundPage=1, chrome.i18n.getMessage=24, chrome.permissions.contains=3, chrome.runtime.connect=2, chrome.tabs.detectLanguage=1, chrome.tabs.executeScript=1, chrome.tabs.getCurrent=3, chrome.tabs.insertCSS=1, chrome.tabs.removeCSS=1, chrome.tabs.sendRequest=1, permissions.contains=3, relay.portConnect=2, relay.portMessage=2, relay.register=1, runtime.getContexts=1, shim.note=7, storage.local.get=14, storage.local.migrated=1, storage.session.get=2, storage.sync.migrated=1, tabs.getCurrent=3
notes (4):
- chrome.storage.managed: native area replaced — empty read-only stub matches Arktos (no policy infra)
- chrome.storage.onChanged: native event replaced — the engine never fires it
- chrome.offscreen: native binding overridden — an engine-spawned document would carry no chromeBridge or relay context
- chrome.extension.getBackgroundPage: null under MV3 matches Chrome (service worker, no window)
audit.missing (0): 
errors (0):

