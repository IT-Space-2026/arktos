/*
 * Copyright 2026 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef EXTENSIONSHIMBACKEND_H
#define EXTENSIONSHIMBACKEND_H

#include "extensionmanager.h"

#include <qhash.h>
#include <qimage.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qlist.h>
#include <qmap.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>

class QTimer;
class QWebEnginePage;
class QWebEngineProfile;

// EXT15 — host-side backend behind ChromeBridge::invoke() for the
// chrome.* gapfill shim (src/data/extshim.js).  One instance per
// hosted extension background context.  Every namespace the page
// can reach answers either a real result or a typed
// {ok:false,error:"..."} reply — the shim turns the error into a
// recorded health gap; nothing silently no-ops.
//
// What lives here and why:
//   storage.*    — per-extension JSON store under
//                  <data>/extstorage/<id>/ (local + sync persist
//                  profile-local — no cloud sync roams sync, session
//                  is in-memory, managed is read-only); Chrome's
//                  quota bounds and the getKeys/setAccessLevel
//                  extras are enforced/recorded host-side (MD19);
//                  the page keeps real chrome.storage when present,
//                  this backs contexts without bindings; the
//                  'storage' manifest permission gates the
//                  namespace (ER16)
//   alarms.*     — QTimer-backed alarms so onAlarm really fires (the
//                  hosted page's binding is present-but-inert);
//                  'alarms' permission gates the namespace (ER16).
//                  MD20 edges: Chrome's MV3 30s periodInMinutes floor
//                  clamps with a recorded note, unnamed alarms live
//                  under Chrome's "" default (object-form
//                  alarmInfo.name honoured, both-forms refused),
//                  getAll answers in creation order
//   tabs.*       — query/get/create/update over a live
//                  BrowserMainWindow/TabWidget snapshot; ids are
//                  session-stable via the registry below
//   action.*     — title/badge/popup setters feeding the action-
//                  state registry EXT08's button strip reads
//   i18n.*       — _locales/<lang>/messages.json lookup
//   permissions.*— manifest-declared contains/getAll fallback
//   contextMenus.*— per-extension item registry (EXT16 renders);
//                  'contextMenus' (or Firefox's 'menus') permission
//                  gates it (ER16)
//   sidePanel.*  — options recorded (Arktos has no side-panel
//                  surface); 'sidePanel' permission gates it (ER16)
//   notifications.* — record registry + freedesktop presentation via
//                  ExtensionNotificationCenter (ER12); 'notifications'
//                  manifest permission gates the namespace
//   idle.*       — queryState/setDetectionInterval/onStateChanged
//                  over ExtensionIdleWatcher's in-app input clock
//                  (ER13); 'idle' manifest permission gates it
//   management.* — getSelf answers the real id/name/version/enabled
//                  from the manifest + manager records (ER15);
//                  ungated like Chrome.  MD16 serves the read ops
//                  the 'management' grant unlocks: getAll/get
//                  enumerate ExtensionManager::extensions() through
//                  the same ExtensionInfo shape, and the
//                  permissionWarnings pair derive their strings
//                  from the manifest's grant list (the same wording
//                  the install review shows).  MD17 serves the
//                  mutations: setEnabled/uninstall ride
//                  ExtensionManager's engine calls and fan
//                  management.onEnabled/onDisabled/onUninstalled
//                  out to every 'management'-declaring context.
//                  The rest stays typed no-permission/unsupported.
//   extension.*  — reloadView, the one cross-context capability the
//                  shim's getViews handles carry (ER15); the
//                  isAllowed*Access pair answer the honest
//                  capability verdicts (MD15 — both false: regular-
//                  profile hosting, no file: grant)
//   sidebarAction.* — panel/title/icon bookkeeping in the same
//                  recorded-options model sidePanel uses (ER15);
//                  open/close/toggle answer typed unsupported;
//                  the manifest's sidebar_action key gates it (ER16)
//   bookmarks.*  — MD21: the real bookmark tree over the RC05
//                  store through BookmarksManager/BookmarkNode (the
//                  facade/FFI lane the C++ wrappers already ride —
//                  mutations flow through the undoable manager ops so
//                  the model, toolbar and autosave stay consistent);
//                  ids "0"/"1"/"2" name root/bar/menu like Chrome,
//                  other nodes serialize their store handle; the
//                  'bookmarks' manifest permission gates the
//                  namespace and its on* event fan-out
//   history.*    — MD22: the real visit log over the RC04 store
//                  through HistoryManager (the facade/FFI lane the
//                  UI already rides — deletes walk the same
//                  remove/clear path checkForExpired uses);
//                  per-url HistoryItem aggregates answer Chrome's
//                  search shape, 'history' manifest permission gates
//                  the namespace and its onVisited/onVisitRemoved
//                  fan-out; transition/typedCount the store never
//                  records answer 'link'/0 with a recorded note
//   cookies.*    — MD23: get/getAll/getAllCookieStores/set/remove
//                  over the RC15 persistent mirror (reads) plus the
//                  QWebEngineCookieStore live jar (set/remove write
//                  both); 'cookies' gates the namespace, host
//                  coverage scopes urls and per-cookie domains;
//                  onChanged rides the engine-store signal tap,
//                  withheld per cookie-domain coverage
//   dns.*        — MD29: resolve() alone (Chrome publishes no other
//                  member): QHostInfo::lookupHost runs the system
//                  resolver off-thread, so the invoke answers a
//                  ticket and the verdict lands as an
//                  '__arktos.dnsResolved' push — {addresses:[...]}
//                  on success, a typed resolve-failed on resolver
//                  error or the bounded wait expiring; 'dns' gates
//                  the namespace like Chrome's declared-grant
//                  requirement
//   system.*     — MD30: honest-machine-facts sub-namespaces, each
//                  gated on its own 'system.<sub>' declared grant
//                  like Chrome.  memory/cpu/display/storage
//                  getInfo answer measured values (/proc/meminfo +
//                  sysconf, /proc/cpuinfo + /proc/stat per-core
//                  jiffies, the live QScreen list, QStorageInfo on
//                  the volumes holding profile state);
//                  storage.getAvailableCapacity is live too.
//                  Declared-but-unserved subs and the ChromeOS-only
//                  mutators answer typed 'unsupported' — the grant
//                  is honored, the surface is missing.
//   downloads.*  — MD24: a subset over DownloadManager's record
//                  list — download() rides the manager's own policy
//                  path (dlwpolicy verdict, save-as gate, external
//                  handoff, rustdl claim) through the hidden retry
//                  page, the id arriving via a deferred
//                  '__arktos.downloadCreated' push since the engine
//                  delivers requests asynchronously; search/erase
//                  enumerate the same item cards the sidebar
//                  renders, pause/resume/cancel/open/show/removeFile
//                  drive the card's own actions, getFileIcon answers
//                  a real platform type icon; 'downloads' gates the
//                  namespace and the onCreated/onChanged/onErased
//                  fan-out (eventGate)
//   sessions.*   — MD25: a minimal surface over TabWidget's TABS05
//                  closed-tab undo stack.  getDevices answers the
//                  honest empty list (no sync service, so no other
//                  device can report); getRecentlyClosed enumerates
//                  the same records the "Recently Closed Tabs" menu
//                  drives — merged across windows, newest first,
//                  capped at Chrome's MAX_SESSION_RESULTS (25, the
//                  stack's own bound); restore() addresses records
//                  by the serial sessionId and rebuilds through the
//                  shared undo path; 'sessions' gates the namespace
//                  and onChanged rides the hub's
//                  recentlyClosedTabsChanged tap (eventGate)
//   tabGroups.*  — MD26: get/query/update/move/ungroup over the
//                  TABGRP machinery (per-window membership keyed on
//                  the WebView); Chrome's int ids ride the minted
//                  map tabObject's groupId reports, colors map the
//                  enum onto the container palette.  Creation lives
//                  in chrome.tabs.group/ungroup like the real API;
//                  'tabGroups' gates the namespace, and the
//                  onCreated/onUpdated/onMoved/onRemoved fan-out is
//                  the hub's tabGroupsChanged diff tap (eventGate)
//   runtime.*    — context bookkeeping (getViews counts); MD18
//                  lifecycle ops: reload rides ExtensionManager's
//                  unload+load, requestUpdateCheck rides
//                  checkForUpdates/updateResultFor with the verdict
//                  pushed back as '__arktos.updateCheck' (the check
//                  runs real fetches the invoke reply cannot wait
//                  out), setUninstallURL persists the page Chrome
//                  opens when the removal lands
//   shim.*       — diagnostics channel: 'note' -> notes(), 'gap' ->
//                  gapReported() -> the host's health surface
class ExtensionShimBackend : public QObject
{
    Q_OBJECT

public:
    ExtensionShimBackend(const QString &extensionId,
                         const QString &extensionName,
                         const QString &extensionPath,
                         QObject *parent = nullptr);
    ~ExtensionShimBackend() override;

    QJsonObject invoke(const QString &ns, const QString &method,
                       const QJsonObject &args);

    QStringList notes() const { return m_notes; }
    QStringList gaps() const { return m_gaps; }

    QString extensionId() const { return m_extensionId; }
    QString extensionName() const { return m_extensionName; }
    QString extensionPath() const { return m_extensionPath; }

    // Records a fidelity note into the health surface — the same
    // list the shim's shim.note calls feed; async features
    // (ExtensionNotificationCenter's presentation failures) report
    // through it.
    void recordNote(const QString &message);

    // The extension's raw manifest.json (cached) — the handshake
    // ships it to the page so synchronous lookups never block.
    QJsonObject manifestJson() const;

    // chrome.action / chrome.browserAction state — the EXT08 action
    // bar (main lane) merges against this registry; until then it is
    // surfaced through ExtensionInfo::action*.
    struct ActionState {
        QString title;
        QString badgeText;
        QString badgeColor;      // css color string as the ext set it
        QString badgeTextColor;
        QString popup;
        // ER14: distinguishes "no setPopup call" (manifest default
        // applies) from "setPopup('')" (popup removed — the action
        // click fires onClicked, Chrome parity).
        bool popupSet = false;
        bool enabled = true;
        // MD14: chrome.action.setIcon overrides the manifest's
        // declared icon — both maps empty = the manifest applies.
        // iconPaths are bundle-relative files under the extension
        // dir (the same hygiene openOptionsPage applies); key 0 is
        // a bare-string path detail with no declared size.
        // iconImages are the decoded imageData pixels (raw RGBA on
        // the wire); readers prefer them — pixels are authoritative
        // where a path is just a reference.
        QMap<int, QString> iconPaths;
        QMap<int, QImage> iconImages;
    };
    static ActionState actionState(const QString &extensionId);

    // Session-stable ids for tabs/windows so chrome.tabs ids stay
    // consistent across query+get and (EXT16) event payloads.
    static int tabIdFor(const QObject *view);
    static int windowIdFor(const QObject *window);
    // ER02: tabId -> the live WebView, resolved over the same
    // window/tab set chrome.tabs.query enumerates (top-level
    // windows, OTR/tor excluded).  nullptr when no tab carries it.
    static class WebView *viewForTabId(int tabId);

    // The chrome.tabs object shape — shared by invokeTabs,
    // EXT16 event payloads and contextMenus.onClicked's tab arg.
    static QJsonObject tabObject(class WebView *view, int index,
                                 int windowId);

    // EXT17 context-local queries — resolves the tab whose page IS
    // the asking context (an extension page living in a real tab),
    // else {ok,result:null} like Chrome's undefined answer for
    // popups/background contexts.
    static QJsonObject tabForPageReply(QWebEnginePage *page);
    // The chrome.windows object shape for invokeWindows.
    static QJsonObject windowObject(class BrowserMainWindow *window);

    // EXT17: the flat _locales/<lang>/messages.json map — shared by
    // invokeI18n (bridge fetch) and the injected __ARKTOS_HOST_CONFIG
    // so getMessage resolves synchronously before page scripts run.
    static QJsonObject i18nMessages(const QString &extensionPath);

    // MD24: the Chrome DownloadItem / onChanged DownloadDelta
    // shapes + the deferred downloads.download ticket claim — public
    // statics (tabObject/windowObject discipline) so the
    // namespace-scope event taps can drive them; the DownloadItem
    // friend grant (backend handle, OTR flag, card actions) applies
    // inside this class only.
    static QJsonObject downloadsItemJson(class DownloadItem *item);
    static QJsonObject downloadsDeltaJson(class DownloadItem *item,
                                        bool finished);
    static bool downloadsClaimPending(class DownloadItem *item);
    // MD26: the chrome.tabGroups TabGroup shape + the minted int id
    // for a (window, strip group-id) pair — public statics (the
    // tabObject/windowObject discipline) so the EXT16 hub's
    // group-diff tap can emit identical payloads for UI-driven
    // mutations.
    static QJsonObject tabGroupJson(class TabWidget *tabs,
                                    const QString &groupId,
                                    int windowId);
    static int chromeGroupIdFor(int windowId, const QString &groupId);
    // The card's statusChanged arrives BEFORE QtWebEngine applies a
    // pause (the request's isPaused flips asynchronously) — a delta
    // emitted from that signal alone always reads stale.  This tap
    // rides the request's own isPausedChanged so onChanged reports
    // the pause the engine actually applied.
    static void downloadsPauseTap(class DownloadItem *item);
    // Post-finish reconciliation for API-minted items — see the
    // implementation comment (page->download auto-accept quirk).
    static void downloadsFileFixup(class DownloadItem *item);

    // ER16: the manifest's REQUIRED permissions — the gate source
    // for every permission-scoped namespace.  optional_permissions
    // are inert here: chrome.permissions.request answers typed
    // unsupported, so an optional grant must never unlock a call.
    QStringList manifestPermissions() const;
    // ER16: a url an extension may put a real tab on — Chrome
    // refuses file:/chrome:/qrc:/foreign-extension documents from
    // tabs.create/update and windows.create outright; the SEC09
    // untrusted-input gate is only the javascript: floor.  Relative
    // spellings resolve against the extension's own
    // chrome-extension origin (options-page pop-outs rely on it).
    // Answers the url to load; an invalid result means refused.
    QUrl extensionNavUrl(const QString &text) const;

    // EXT16: the page context menu reads this shared registry — an
    // ordered item list per extension plus the display name its
    // submenu shows.  Owned by whichever live backend holds the id:
    // a respawned host inherits the registry slot and the stale
    // backend's destructor cannot clear the new owner's items.
    struct ContextMenuEntry {
        QString extensionId;
        QString extensionName;
        QString itemId;
        QJsonObject props;
    };
    static QList<ContextMenuEntry> contextMenuEntries();
    static QJsonObject contextMenuItem(const QString &extensionId,
                                     const QString &itemId);
    static void setContextMenuChecked(const QString &extensionId,
                                    const QString &itemId,
                                    bool checked);

signals:
    // Pushed into the hosted page's hostEvent handler — the shim
    // fans it out to registered event listeners ('alarms.onAlarm',
    // EXT16's 'tabs.onUpdated', ...).
    void pushEvent(const QString &name, const QJsonObject &payload);
    void noteRecorded(const QString &message);
    void gapReported(const QString &api, const QString &error);
    void actionStateChanged();

private:
    QJsonObject invokeStorage(const QString &method,
                              const QJsonObject &args);
    QJsonObject invokeAlarms(const QString &method,
                             const QJsonObject &args);
    QJsonObject invokeTabs(const QString &method,
                           const QJsonObject &args);
    // QUAL03: apply the EXT16 url-field scrub to chrome.tabs replies —
    // url/title leave the host only when the extension holds 'tabs'
    // or a host permission covering the tab's url.
    QJsonObject shapedTab(const QJsonObject &tab) const;
    QJsonObject invokeWebNavigation(const QString &method,
                                    const QJsonObject &args);
    QJsonObject invokeWindows(const QString &method,
                              const QJsonObject &args);
    QJsonObject invokeAction(const QString &method,
                             const QJsonObject &args);
    QJsonObject invokeI18n(const QString &method,
                           const QJsonObject &args);
    QJsonObject invokePermissions(const QString &method,
                                  const QJsonObject &args);
    QJsonObject invokeContextMenus(const QString &method,
                                   const QJsonObject &args);
    QJsonObject invokeSidePanel(const QString &method,
                                const QJsonObject &args);
    QJsonObject invokeNotifications(const QString &method,
                                    const QJsonObject &args);
    QJsonObject invokeCommands(const QString &method,
                               const QJsonObject &args);
    QJsonObject invokeIdle(const QString &method,
                           const QJsonObject &args);
    QJsonObject invokeManagement(const QString &method,
                                 const QJsonObject &args);
    // MD16: the chrome.management ExtensionInfo object shape —
    // shared by getSelf and the gated getAll/get enumeration.
    // `record` is the manager's live row (nullptr = a hosted backend
    // the manager lost track of: still answers enabled like a
    // running extension, installType 'development').
    static QJsonObject managementInfoJson(
            const QString &id, const QString &fallbackName,
            const QString &extensionPath, const QJsonObject &manifest,
            const ExtensionManager::ExtensionInfo *record);
    // Warning strings for a manifest's grant list — the same wording
    // ExtensionReviewDialog's consent rows show, deduplicated in
    // declaration order.
    static QJsonArray managementWarnings(const QJsonObject &manifest);
    // MD17: the Chrome-parity event fan-out for the mutations —
    // broadcastEvent reaches every relay-registered context of a
    // 'management'-declaring extension (fanOut gates on the
    // manifest snapshot); a bare backend with no registered context
    // (tests, mid-respawn) still sees the event on pushEvent — the
    // same fallback the notification center applies.
    void managementFanOut(const QString &name,
                          const QJsonArray &eventArgs);
    // MD21: chrome.bookmarks over the RC05 store — same fan-out
    // discipline the management mutations use (broadcast to
    // 'bookmarks'-declaring contexts, pushEvent fallback for a bare
    // backend).
    QJsonObject invokeBookmarks(const QString &method,
                                const QJsonObject &args);
    void bookmarksFanOut(const QString &name,
                         const QJsonArray &eventArgs);
    // MD22: chrome.history over the RC04 store — the same fan-out
    // discipline bookmarks/management use (broadcast to
    // 'history'-declaring contexts, pushEvent fallback for a bare
    // backend).
    QJsonObject invokeHistory(const QString &method,
                              const QJsonObject &args);
    void historyFanOut(const QString &name,
                       const QJsonArray &eventArgs);
    // MD27: chrome.topSites over the RC04 hosts rollup — 'topSites'
    // gates the namespace (Chrome requires the declared grant);
    // get() answers the visit-count-ranked MostVisitedURL list, the
    // only member Chrome publishes, and the namespace has no events.
    QJsonObject invokeTopSites(const QString &method,
                               const QJsonObject &args);
    // MD28: chrome.browsingData over the CLR01 clear-plan machinery —
    // 'browsingData' gates the namespace; settings/remove plus the
    // five served remove* members (history/cookies/downloads/cache/
    // formData) map Chrome's DataTypeSet onto ClearPrivateData
    // category bits and the plan vocabulary decides scope + ops.
    // The other nine Chrome dataTypes stay typed 'unsupported' —
    // inside a mixed remove() they are reported in the removal
    // result, never silently skipped; per-origin scoping
    // (origins/excludeOrigins, non-web originTypes) is typed
    // unservable rather than widened to a whole-store wipe.
    QJsonObject invokeBrowsingData(const QString &method,
                                   const QJsonObject &args);
    // MD29: chrome.dns — 'dns' gates the namespace like Chrome's
    // declared-grant requirement; resolve() is the only published
    // member.  The lookup is deferred like downloads.download: the
    // reply is a ticket and the real answer arrives on the
    // '__arktos.dnsResolved' push once QHostInfo's off-thread
    // resolver answers — {ticket,addresses} on success,
    // {ticket,error:'resolve-failed'} on resolver error or the
    // bounded wait expiring.
    QJsonObject invokeDns(const QString &method,
                          const QJsonObject &args);
    // MD30: chrome.system — each sub-namespace gates on its own
    // 'system.<sub>' declared grant (the check runs before the
    // surface check so a declared-but-unserved sub hears
    // 'unsupported', not 'no-permission').  Methods ride the dotted
    // "sub.member" vocabulary: memory/cpu/display/storage getInfo
    // answer measured machine facts plus storage
    // getAvailableCapacity; everything else Chrome publishes stays
    // typed 'unsupported' — no fake values anywhere.
    QJsonObject invokeSystem(const QString &method,
                             const QJsonObject &args);
    // MD23: chrome.cookies — reads over the RC15 persistent mirror
    // (the jar's on-disk shadow, fed by the engine's cookie signals;
    // the C++ jar's live mirror is the no-rust read path), writes go
    // to the QWebEngineCookieStore live jar AND the mirror so a
    // synchronous get() sees its own set.  'cookies' gates the
    // namespace; every url-bearing op additionally requires host
    // coverage (no 'tabs' shortcut — same gate webRequest rides) and
    // getAll drops rows on uncovered domains.  onChanged fans out
    // through the engine-store tap, eventGate-scoped to
    // 'cookies'-declaring contexts and withheld per cookie domain.
    QJsonObject invokeCookies(const QString &method,
                              const QJsonObject &args);
    // MD24: chrome.downloads — 'downloads' gates the namespace; the
    // events are all tap-driven (itemAdded -> onCreated, the card's
    // status/progress signals -> onChanged, model row removal ->
    // onErased) so API and UI changes fan out identically through
    // the broadcast lane — no member-emit path exists here.
    QJsonObject invokeDownloads(const QString &method,
                                const QJsonObject &args);
    static bool downloadsQueryMatch(const QJsonObject &query,
                                    class DownloadItem *item);
    // MD25: chrome.sessions over the per-window TABS05 undo stacks —
    // 'sessions' gates the namespace; every member is served
    // (getDevices/getRecentlyClosed/restore), so only a mistyped
    // call or an unknown/stale sessionId produces a typed verdict.
    // onChanged is entirely tap-driven (the hub wires
    // recentlyClosedTabsChanged), so no member-emit path exists here.
    QJsonObject invokeSessions(const QString &method,
                               const QJsonObject &args);
    // MD26: chrome.tabGroups over the TABGRP machinery — 'tabGroups'
    // gates the namespace; every member is served (get/query/
    // update/move/ungroup), so only mistyped calls, stale ids and
    // the cross-window move produce typed verdicts.  The four on*
    // events are tap-driven in the hub, like sessions/downloads.
    QJsonObject invokeTabGroups(const QString &method,
                                const QJsonObject &args);
    QJsonObject invokeExtension(const QString &method,
                                const QJsonObject &args);
    QJsonObject invokeSidebarAction(const QString &method,
                                    const QJsonObject &args);
    QJsonObject invokeRuntime(const QString &method,
                              const QJsonObject &args);
    QJsonObject invokeShim(const QString &method,
                           const QJsonObject &args);

    static QJsonObject ok(const QJsonValue &result = QJsonValue());
    static QJsonObject err(const QString &code);

    // storage area helpers — 'local' and 'sync' persist under
    // extstorage/, 'session' is in-memory, 'managed' a never-written
    // read-only store.
    QJsonObject &storeFor(const QString &area);
    QJsonObject loadStoreFile(const QString &area) const;
    void flushStoreFile(const QString &area,
                        const QJsonObject &store);
    void persistArea(const QString &area);
    void flushLocalStore();
    void flushSyncStore();
    QString storageDir() const;
    QJsonObject changesForSet(const QJsonObject &store,
                              const QJsonObject &items) const;
    QJsonObject changesForRemove(const QJsonObject &store,
                                 const QStringList &keys) const;

    void fireAlarm(const QString &name);

    QString m_extensionId;
    QString m_extensionName;
    QString m_extensionPath;

    QJsonObject m_storeLocal;
    QJsonObject m_storeSync;
    QJsonObject m_storeSession;
    QJsonObject m_storeManaged;
    bool m_localLoaded = false;
    bool m_localDirty = false;
    bool m_syncLoaded = false;
    bool m_syncDirty = false;
    // MD19: the recorded setAccessLevel level per storage area —
    // Chrome scopes TRUSTED/UNTRUSTED_CONTEXTS on the session area;
    // the bridge store serves every context of the extension either
    // way, so the level is honest bookkeeping, not a gate.
    QHash<QString, QString> m_storageAccessLevels;

    struct Alarm {
        QString name;
        qint64 scheduledTime = 0;   // ms since epoch
        double periodInMinutes = 0;
        // MD20: creation sequence — Chrome's getAll iterates its
        // per-extension alarm list in creation order (a replaced
        // alarm re-appends at the end); QHash order would scramble it.
        quint64 order = 0;
        QTimer *timer = nullptr;
    };
    QHash<QString, Alarm> m_alarms;
    quint64 m_alarmSeq = 0;
    static QJsonObject alarmJson(const Alarm &alarm);

    QJsonObject m_sidePanelOptions;
    QJsonObject m_sidePanelBehavior;
    // ER15: chrome.sidebarAction bookkeeping — {panel,title,icon},
    // the same recorded-but-unrendered model m_sidePanelOptions
    // carries (Arktos has no sidebar surface).
    QJsonObject m_sidebarAction;

    QStringList m_notes;
    QStringList m_gaps;
    QJsonObject m_manifest;         // invalid until manifestJson()
    bool m_manifestLoaded = false;

    // MD18: a requestUpdateCheck whose verdict is still in flight —
    // a second call answers 'throttled' like Chrome.
    bool m_updateCheckPending = false;

    // MD21: the getRecent serial-ordering approximation is recorded
    // once per context, not on every call.
    bool m_bmRecentNoted = false;
    // MD22: the untracked transition-type approximation is recorded
    // once per context, not on every getVisits.
    bool m_histTransitionNoted = false;
    // MD23: the onChanged 'cause' caveat (Qt signals distinguish no
    // eviction/expiration/overwrite causes) — recorded once per
    // context.
    bool m_cookieCauseNoted = false;
    // MD24: finalUrl answers the request url verbatim (no redirect
    // tracking in the card), and conflictAction 'overwrite' is a
    // recorded typed refusal — each noted once per context.
    bool m_dlFinalUrlNoted = false;
    // MD25: the getDevices honest-empty caveat (no sync service) is
    // recorded once per context, not on every call.
    bool m_sessionsDevicesNoted = false;
    // MD28: the served-subset vocabulary + window-degradation caveat
    // is recorded once per context, not on every remove.
    bool m_browsingDataNoted = false;
};

#endif // EXTENSIONSHIMBACKEND_H
