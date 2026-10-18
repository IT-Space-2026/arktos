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
generated: 2026-10-18T19:37:03Z
bundle: bitwarden-2026.9.3_2QIFbk name="__MSG_extName__" version=2026.9.3
installed id: jdaehcgmbajmnipnmjgchodbanobbehm
host state: ready
popup bridge: up
contexts: BACKGROUND, POPUP
manager row: health=not-supported hostState=hosted hostError=""
  healthReason: it requires chrome.* APIs Qt WebEngine does not provide (scripting, webRequest) — this extension cannot work in this browser
  missingApis: contextMenus, idle, sidePanel, tabs, notifications, nativeMessaging, privacy

## Typed shim gaps — the lane work order (0 unique)

## background context
doc: ready=complete title="" url=chrome-extension://jdaehcgmbajmnipnmjgchodbanobbehm/manifest.json#arktos-hosted
host: state=ready bridgeUp=1 kind=background
touched (34): alarms.clear=4, alarms.create=6, alarms.get=6, chrome.i18n.getMessage=13, chrome.permissions.contains=1, chrome.runtime.sendMessage=1, chrome.tabs.captureVisibleTab=1, chrome.tabs.connect=1, chrome.tabs.detectLanguage=1, chrome.tabs.discard=1, chrome.tabs.duplicate=1, chrome.tabs.executeScript=1, chrome.tabs.getAllInWindow=1, chrome.tabs.getSelected=1, chrome.tabs.getZoom=1, chrome.tabs.getZoomSettings=1, chrome.tabs.goBack=1, chrome.tabs.goForward=1, chrome.tabs.highlight=1, chrome.tabs.insertCSS=1, chrome.tabs.move=1, chrome.tabs.reload=1, chrome.tabs.remove=1, chrome.tabs.removeCSS=1, chrome.tabs.sendRequest=1, chrome.tabs.setZoom=1, chrome.tabs.setZoomSettings=1, relay.portAccept=2, relay.portMessage=3, relay.register=1, relay.send=1, runtime.getContexts=1, sidePanel.setOptions=1, tabs.query=2
notes (1):
- chrome.sidePanel.setOptions: options recorded host-side; Arktos has no side-panel surface
audit.missing (0): 
errors (0):

## popup context
doc: ready=complete title="Bitwarden" url=chrome-extension://jdaehcgmbajmnipnmjgchodbanobbehm/popup/index.html#/intro-carousel
host: state=ready bridgeUp=1 kind=page
touched (29): chrome.extension.getBackgroundPage=1, chrome.i18n.getMessage=32, chrome.runtime.connect=2, chrome.tabs.captureVisibleTab=1, chrome.tabs.connect=1, chrome.tabs.detectLanguage=1, chrome.tabs.discard=1, chrome.tabs.duplicate=1, chrome.tabs.executeScript=1, chrome.tabs.getAllInWindow=1, chrome.tabs.getCurrent=3, chrome.tabs.getSelected=1, chrome.tabs.getZoom=1, chrome.tabs.getZoomSettings=1, chrome.tabs.goBack=1, chrome.tabs.goForward=1, chrome.tabs.highlight=1, chrome.tabs.insertCSS=1, chrome.tabs.move=1, chrome.tabs.reload=1, chrome.tabs.remove=1, chrome.tabs.removeCSS=1, chrome.tabs.sendRequest=1, chrome.tabs.setZoom=1, chrome.tabs.setZoomSettings=1, relay.portConnect=2, relay.portMessage=2, relay.register=1, tabs.getCurrent=3
notes (1):
- chrome.extension.getBackgroundPage: null under MV3 matches Chrome (service worker, no window)
audit.missing (0): 
errors (0):

