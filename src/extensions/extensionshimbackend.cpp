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

#include "extensionshimbackend.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserpaths.h"
#include "engineinterface.h"
#include "extensionbackgroundhost.h"
#include "extensioncommands.h"
#include "extensioneventhub.h"
#include "extensionidlewatcher.h"
#include "extensionmanager.h"
#include "extensionnotificationcenter.h"
#include "tabbar.h"
#include "tabwidget.h"
#include "webview.h"

#include <qbuffer.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qimage.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qlocale.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qregularexpression.h>
#include <qsavefile.h>
#include <qsysinfo.h>
#include <qtimer.h>
#include <qvariant.h>

#include <qwebenginepage.h>
#include <qwebengineprofile.h>

namespace {

// Reply shapes: every invoke() answers {ok:true,result:...} or
// {ok:false,error:"<typed code>"} — the JS shim turns failures into
// health-surface gaps.
QJsonObject okReply(const QJsonValue &result = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if (!result.isUndefined())
        o.insert(QStringLiteral("result"), result);
    return o;
}

QJsonObject errReply(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

// Per-extension action state — chrome.action/browserAction setters
// land here; EXT08's action bar (main lane) reads it through
// ExtensionInfo::action* fields on merge.
QHash<QString, ExtensionShimBackend::ActionState> &actionStates()
{
    static QHash<QString, ExtensionShimBackend::ActionState> states;
    return states;
}

// EXT16 shared contextMenus registry — the page context menu reads
// one ordered item list per extension (registration order kept;
// QHash iteration order would scramble a menu).  The owners map
// keys the registry to the live backend: a respawning host hands
// its slot to the new backend, and a stale destructor clears only
// what it still owns.
struct MenuRegistry {
    QHash<QString, QList<QPair<QString, QJsonObject>>> items;
    QHash<QString, QString> names;
    QHash<QString, const ExtensionShimBackend *> owners;
};

MenuRegistry &menuRegistry()
{
    static MenuRegistry registry;
    return registry;
}

// Session-stable id minting for tab/window objects — a dynamic
// property on the object itself, so a destroyed object takes its id
// with it and a recycled address can never inherit it.
int idFor(const QObject *object)
{
    if (!object)
        return -1;
    static int next = 1;
    const QVariant v = object->property("arktosExtObjectId");
    if (v.isValid() && v.toInt() > 0)
        return v.toInt();
    const int id = next++;
    const_cast<QObject *>(object)->setProperty("arktosExtObjectId", id);
    return id;
}

// The window a chrome.tabs call targets for currentWindow /
// lastFocusedWindow — the active top-level, else the first seen.
// Enumerates widgets like the EXT16 hub so the set covers windows
// the app registry does not track (private windows, qtest windows)
// and never dereferences a null instance() under autotests.
BrowserMainWindow *currentWindow()
{
    BrowserMainWindow *fallback = nullptr;
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window)
            continue;
        if (window->isActiveWindow())
            return window;
        if (!fallback)
            fallback = window;
    }
    return fallback;
}

// One resolved tab — every tabs.* op lands on this four-tuple so a
// mid-dispatch strip reorder never confuses (window, index, view)
// again.  index is the strip position at resolve time; ops that
// move things re-resolve through webViewIndex().
struct LocatedTab {
    BrowserMainWindow *window = nullptr;
    TabWidget *tabs = nullptr;
    WebView *view = nullptr;
    int index = -1;
    int windowId = -1;
};

// Resolve a chrome.tabs tabId to the live strip position — ids
// minted by idFor() only describe trackable tabs (the same set
// query enumerates).  tabId < 0 names the current window's current
// tab, the default Chrome gives ops called without one.
LocatedTab locateTab(int tabId)
{
    BrowserMainWindow *current = currentWindow();
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window || !window->tabWidget())
            continue;
        TabWidget *tw = window->tabWidget();
        const bool wantCurrent = tabId < 0 && window == current;
        for (int i = 0; i < tw->count(); ++i) {
            WebView *view = tw->webView(i);
            if (!ExtensionEventHub::trackable(view))
                continue;
            if (tabId < 0 ? (wantCurrent && i == tw->currentIndex())
                          : idFor(view) == tabId) {
                LocatedTab out;
                out.window = window;
                out.tabs = tw;
                out.view = view;
                out.index = i;
                out.windowId = idFor(window);
                return out;
            }
        }
    }
    return LocatedTab();
}

// chrome.tabs reports groupId as an int; the strip keys group
// membership on per-window strings.  Mint a session-stable int per
// (window, group) pair — Chrome's -1 stays "no group".
int chromeGroupId(int windowId, const QString &groupId)
{
    if (groupId.isEmpty())
        return -1;
    static QHash<QString, int> ids;
    static int next = 1;
    const QString key = QString::number(windowId)
        + QLatin1Char('/') + groupId;
    int &slot = ids[key];
    if (!slot)
        slot = next++;
    return slot;
}

// MD11: Chrome's zoomSettings shape for a tab — the recorded
// setZoomSettings request merged over the honest defaults: Arktos
// zoom is per-tab (never per-origin) and persists across
// navigation, which is Chrome's "manual" mode.
QJsonObject zoomSettingsFor(const WebView *view)
{
    QJsonObject out{
        {QStringLiteral("mode"), QStringLiteral("manual")},
        {QStringLiteral("scope"), QStringLiteral("per-tab")},
        {QStringLiteral("defaultZoomFactor"), 1.0}};
    const QJsonObject recorded = view
        ? view->property("arktosExtZoomSettings").toJsonObject()
        : QJsonObject();
    for (const QString &key :
             {QStringLiteral("mode"), QStringLiteral("scope")}) {
        if (recorded.contains(key))
            out.insert(key, recorded.value(key));
    }
    return out;
}

} // namespace

ExtensionShimBackend::ExtensionShimBackend(const QString &extensionId,
                                           const QString &extensionName,
                                           const QString &extensionPath,
                                           QObject *parent)
    : QObject(parent)
    , m_extensionId(extensionId)
    , m_extensionName(extensionName)
    , m_extensionPath(extensionPath)
{
    // EXT16 registry ownership — this backend is now the authority
    // for the extension's context-menu items and display name.
    menuRegistry().owners.insert(m_extensionId, this);
    menuRegistry().names.insert(m_extensionId, m_extensionName);
}

ExtensionShimBackend::~ExtensionShimBackend()
{
    for (Alarm &alarm : m_alarms) {
        if (alarm.timer)
            alarm.timer->stop();
    }
    if (m_localDirty)
        flushLocalStore();
    // The items die with their owner — Chrome clears an extension's
    // context menus when its background context ends.  A respawn's
    // replacement backend already claimed the slot: leave it.
    if (menuRegistry().owners.value(m_extensionId) == this) {
        menuRegistry().owners.remove(m_extensionId);
        menuRegistry().items.remove(m_extensionId);
        menuRegistry().names.remove(m_extensionId);
    }
}

ExtensionShimBackend::ActionState
ExtensionShimBackend::actionState(const QString &extensionId)
{
    return actionStates().value(extensionId);
}

int ExtensionShimBackend::tabIdFor(const QObject *view)
{
    return idFor(view);
}

int ExtensionShimBackend::windowIdFor(const QObject *window)
{
    return idFor(window);
}

WebView *ExtensionShimBackend::viewForTabId(int tabId)
{
    if (tabId < 0)
        return nullptr;
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window || !window->tabWidget())
            continue;
        TabWidget *tw = window->tabWidget();
        for (int i = 0; i < tw->count(); ++i) {
            WebView *view = tw->webView(i);
            if (ExtensionEventHub::trackable(view)
                    && idFor(view) == tabId)
                return view;
        }
    }
    return nullptr;
}

QJsonObject ExtensionShimBackend::invoke(const QString &ns,
                                         const QString &method,
                                         const QJsonObject &args)
{
    if (ns == QLatin1String("storage"))
        return invokeStorage(method, args);
    if (ns == QLatin1String("alarms"))
        return invokeAlarms(method, args);
    if (ns == QLatin1String("tabs"))
        return invokeTabs(method, args);
    if (ns == QLatin1String("webNavigation"))
        return invokeWebNavigation(method, args);
    if (ns == QLatin1String("windows"))
        return invokeWindows(method, args);
    if (ns == QLatin1String("action")
            || ns == QLatin1String("browserAction"))
        return invokeAction(method, args);
    if (ns == QLatin1String("i18n"))
        return invokeI18n(method, args);
    if (ns == QLatin1String("permissions"))
        return invokePermissions(method, args);
    if (ns == QLatin1String("contextMenus")
            || ns == QLatin1String("menus"))
        return invokeContextMenus(method, args);
    if (ns == QLatin1String("sidePanel"))
        return invokeSidePanel(method, args);
    if (ns == QLatin1String("notifications"))
        return invokeNotifications(method, args);
    if (ns == QLatin1String("commands"))
        return invokeCommands(method, args);
    if (ns == QLatin1String("idle"))
        return invokeIdle(method, args);
    if (ns == QLatin1String("management"))
        return invokeManagement(method, args);
    if (ns == QLatin1String("extension"))
        return invokeExtension(method, args);
    if (ns == QLatin1String("sidebarAction"))
        return invokeSidebarAction(method, args);
    if (ns == QLatin1String("runtime"))
        return invokeRuntime(method, args);
    if (ns == QLatin1String("shim"))
        return invokeShim(method, args);
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.storage — per-extension host-side store.  'local' persists
// as JSON under <data>/extstorage/<id>/ (QSaveFile atomic write);
// 'session' is in-memory like the real area.  Mutations answer the
// WebExtensions change record so the shim can fire onChanged.
// -------------------------------------------------------------------

QString ExtensionShimBackend::storageDir() const
{
    // Extension ids are Chromium-derived base16-ish — keep only the
    // safe alphabet anyway, nothing outside gets to name the dir.
    QString safe;
    for (const QChar c : m_extensionId) {
        if (c.isLetterOrNumber() || c == QLatin1Char('_')
                || c == QLatin1Char('-'))
            safe.append(c);
    }
    if (safe.isEmpty())
        safe = QStringLiteral("_");
    return BrowserPaths::dataFilePath(
        QStringLiteral("extstorage")) + QLatin1Char('/') + safe;
}

QJsonObject &ExtensionShimBackend::storeFor(const QString &area)
{
    if (area == QLatin1String("local") && !m_localLoaded) {
        m_localLoaded = true;
        QFile file(storageDir() + QLatin1String("/local.json"));
        if (file.size() <= 8 * 1024 * 1024 && file.open(QIODevice::ReadOnly))
            m_storeLocal =
                QJsonDocument::fromJson(file.readAll()).object();
    }
    return area == QLatin1String("local") ? m_storeLocal
                                          : m_storeSession;
}

void ExtensionShimBackend::flushLocalStore()
{
    if (!m_localLoaded)
        return;
    QDir dir;
    if (!dir.mkpath(storageDir()))
        return;
    QSaveFile file(storageDir() + QLatin1String("/local.json"));
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.write(QJsonDocument(m_storeLocal)
                   .toJson(QJsonDocument::Compact));
    file.commit();
    m_localDirty = false;
}

// WebExtensions get() key shapes: null → whole store, string → one
// key, array → subset, object → default-merged subset.
static QStringList getKeysList(const QJsonValue &keys,
                               const QJsonObject &store,
                               QJsonObject *defaults)
{
    QStringList list;
    if (keys.isNull() || keys.isUndefined()) {
        for (auto it = store.constBegin(); it != store.constEnd(); ++it)
            list.append(it.key());
        return list;
    }
    if (keys.isString()) {
        list.append(keys.toString());
        return list;
    }
    if (keys.isArray()) {
        const QJsonArray arr = keys.toArray();
        for (const QJsonValue &v : arr)
            list.append(v.toString());
        return list;
    }
    if (keys.isObject()) {
        const QJsonObject obj = keys.toObject();
        for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
            list.append(it.key());
            if (defaults)
                defaults->insert(it.key(), it.value());
        }
    }
    return list;
}

QJsonObject ExtensionShimBackend::changesForSet(const QJsonObject &store,
                                              const QJsonObject &items) const
{
    QJsonObject changes;
    for (auto it = items.constBegin(); it != items.constEnd(); ++it) {
        QJsonObject rec;
        if (store.contains(it.key()))
            rec.insert(QStringLiteral("oldValue"), store.value(it.key()));
        rec.insert(QStringLiteral("newValue"), it.value());
        changes.insert(it.key(), rec);
    }
    return changes;
}

QJsonObject ExtensionShimBackend::changesForRemove(
        const QJsonObject &store, const QStringList &keys) const
{
    QJsonObject changes;
    for (const QString &key : keys) {
        if (!store.contains(key))
            continue;
        QJsonObject rec;
        rec.insert(QStringLiteral("oldValue"), store.value(key));
        changes.insert(key, rec);
    }
    return changes;
}

QJsonObject ExtensionShimBackend::invokeStorage(const QString &method,
                                                const QJsonObject &args)
{
    // ER16: Chrome exposes chrome.storage only to extensions
    // declaring 'storage' — the bridge object answers a direct call
    // from any code in the context, so the namespace's absence is
    // enforced here, not in the shim's install list.
    if (!manifestPermissions().contains(QLatin1String("storage")))
        return errReply(QStringLiteral("no-permission"));
    // ER17: 'changed' is the notify op — a shim context that wrote
    // through the REAL engine area (the hosted page keeps it so the
    // extension's store stays shared with real extension pages)
    // reports the change record it computed, and it fans out to the
    // extension's sibling contexts as storage.onChanged +
    // storage.<area>.onChanged.  The writer fires its own copy
    // synchronously in the shim; ChromeBridge::invoke intercepts
    // this op to exclude the posting bridge, leaving only direct
    // backend callers here — page agents are never relay contexts,
    // so an un-excluded fan-out cannot double-deliver.  The engine's
    // own storage delegate never emits either — the fan-out is the
    // only onChanged these pages will ever see.  Same-domain trust:
    // a covered context can already message its own extension's
    // other contexts, so the report needs no re-proof.
    if (method == QLatin1String("changed")) {
        const QString area =
            args.value(QStringLiteral("area")).toString();
        if (area != QLatin1String("local")
                && area != QLatin1String("session")
                && area != QLatin1String("sync"))
            return errReply(QStringLiteral("invalid-area"));
        ExtensionBackgroundRuntime::instance()->fanOutStorageChanged(
            m_extensionId, area,
            args.value(QStringLiteral("changes")).toObject());
        return okReply();
    }
    const int dot = method.indexOf(QLatin1Char('.'));
    const QString area = method.left(dot);
    const QString op = method.mid(dot + 1);
    if (area != QLatin1String("local") && area != QLatin1String("session")
            && area != QLatin1String("sync")
            && area != QLatin1String("managed"))
        return errReply(QStringLiteral("invalid-area"));
    QJsonObject &store = storeFor(area);

    if (op == QLatin1String("get")) {
        QJsonObject defaults;
        const QStringList keys = getKeysList(
            args.value(QStringLiteral("keys")), store, &defaults);
        QJsonObject out = defaults;
        for (const QString &key : keys) {
            if (store.contains(key))
                out.insert(key, store.value(key));
        }
        return okReply(out);
    }
    if (op == QLatin1String("getBytesInUse")) {
        // Approximation: serialized size of the matched entries.
        QJsonObject picked;
        const QStringList keys = getKeysList(
            args.value(QStringLiteral("keys")), store, nullptr);
        for (const QString &key : keys) {
            if (store.contains(key))
                picked.insert(key, store.value(key));
        }
        return okReply(QJsonDocument(picked)
                           .toJson(QJsonDocument::Compact).size());
    }
    if (op == QLatin1String("set")) {
        if (area == QLatin1String("managed"))
            return errReply(QStringLiteral("read-only"));
        const QJsonObject items =
            args.value(QStringLiteral("items")).toObject();
        QJsonObject result;
        result.insert(QStringLiteral("changes"),
                      changesForSet(store, items));
        for (auto it = items.constBegin(); it != items.constEnd(); ++it)
            store.insert(it.key(), it.value());
        if (area == QLatin1String("local")) {
            m_localDirty = true;
            flushLocalStore();
        }
        return okReply(result);
    }
    if (op == QLatin1String("remove")) {
        if (area == QLatin1String("managed"))
            return errReply(QStringLiteral("read-only"));
        QStringList keys;
        const QJsonValue k = args.value(QStringLiteral("keys"));
        if (k.isString())
            keys.append(k.toString());
        else {
            const QJsonArray arr = k.toArray();
            for (const QJsonValue &v : arr)
                keys.append(v.toString());
        }
        QJsonObject result;
        result.insert(QStringLiteral("changes"),
                      changesForRemove(store, keys));
        for (const QString &key : keys)
            store.remove(key);
        if (area == QLatin1String("local")) {
            m_localDirty = true;
            flushLocalStore();
        }
        return okReply(result);
    }
    if (op == QLatin1String("clear")) {
        if (area == QLatin1String("managed"))
            return errReply(QStringLiteral("read-only"));
        QStringList keys;
        for (auto it = store.constBegin(); it != store.constEnd(); ++it)
            keys.append(it.key());
        QJsonObject result;
        result.insert(QStringLiteral("changes"),
                      changesForRemove(store, keys));
        store = QJsonObject();
        if (area == QLatin1String("local")) {
            m_localDirty = true;
            flushLocalStore();
        }
        return okReply(result);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.alarms — host QTimers so onAlarm genuinely fires.  Alarm
// objects are {name, scheduledTime, periodInMinutes} per the spec.
// -------------------------------------------------------------------

void ExtensionShimBackend::fireAlarm(const QString &name)
{
    const Alarm alarm = m_alarms.value(name);
    if (alarm.name.isEmpty())
        return;
    QJsonObject payload;
    payload.insert(QStringLiteral("name"), alarm.name);
    payload.insert(QStringLiteral("scheduledTime"),
                   double(alarm.scheduledTime));
    if (alarm.periodInMinutes > 0)
        payload.insert(QStringLiteral("periodInMinutes"),
                       alarm.periodInMinutes);
    emit pushEvent(QStringLiteral("alarms.onAlarm"), payload);

    if (alarm.periodInMinutes > 0) {
        Alarm &a = m_alarms[name];
        a.scheduledTime = QDateTime::currentMSecsSinceEpoch()
            + qint64(a.periodInMinutes * 60000.0);
        a.timer->setInterval(int(a.periodInMinutes * 60000.0));
        a.timer->start();
    } else {
        if (alarm.timer)
            alarm.timer->deleteLater();
        m_alarms.remove(name);
    }
}

QJsonObject ExtensionShimBackend::alarmJson(
        const ExtensionShimBackend::Alarm &a)
{
    QJsonObject o;
    o.insert(QStringLiteral("name"), a.name);
    o.insert(QStringLiteral("scheduledTime"),
             double(a.scheduledTime));
    if (a.periodInMinutes > 0)
        o.insert(QStringLiteral("periodInMinutes"),
                 a.periodInMinutes);
    return o;
}

QJsonObject ExtensionShimBackend::invokeAlarms(const QString &method,
                                               const QJsonObject &args)
{
    // ER16: 'alarms' permission gates the namespace like Chrome.
    if (!manifestPermissions().contains(QLatin1String("alarms")))
        return errReply(QStringLiteral("no-permission"));
    if (method == QLatin1String("create")) {
        const QString name =
            args.value(QStringLiteral("name")).toString();
        const QJsonObject info =
            args.value(QStringLiteral("info")).toObject();
        Alarm alarm;
        alarm.name = name;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (info.contains(QStringLiteral("when"))) {
            alarm.scheduledTime =
                qint64(info.value(QStringLiteral("when")).toDouble());
        } else {
            // delayInMinutes — fractional is allowed; clamp the floor
            // so a 0 delay can't spin the timer thread.
            const double delayMin = qMax(0.002,
                info.value(QStringLiteral("delayInMinutes"))
                    .toDouble(0.0));
            alarm.scheduledTime = now + qint64(delayMin * 60000.0);
        }
        alarm.periodInMinutes =
            info.value(QStringLiteral("periodInMinutes"))
                .toDouble(0.0);
        alarm.timer = new QTimer(this);
        alarm.timer->setSingleShot(alarm.periodInMinutes <= 0);
        alarm.timer->setInterval(
            int(qMax(qint64(100), alarm.scheduledTime - now)));
        connect(alarm.timer, &QTimer::timeout, this,
                [this, name]() { fireAlarm(name); });
        alarm.timer->start();
        if (Alarm old = m_alarms.value(name); old.timer)
            old.timer->deleteLater();
        m_alarms.insert(name, alarm);
        return okReply();
    }
    if (method == QLatin1String("get")) {
        const QString name =
            args.value(QStringLiteral("name")).toString();
        const Alarm a = m_alarms.value(name);
        return okReply(a.name.isEmpty() ? QJsonValue(QJsonValue::Undefined)
                                      : alarmJson(a));
    }
    if (method == QLatin1String("getAll")) {
        QJsonArray all;
        for (const Alarm &a : m_alarms)
            all.append(alarmJson(a));
        return okReply(all);
    }
    if (method == QLatin1String("clear")) {
        const QString name =
            args.value(QStringLiteral("name")).toString();
        const Alarm a = m_alarms.take(name);
        if (a.timer)
            a.timer->deleteLater();
        return okReply(!a.name.isEmpty());
    }
    if (method == QLatin1String("clearAll")) {
        for (const Alarm &a : m_alarms) {
            if (a.timer)
                a.timer->deleteLater();
        }
        const bool had = !m_alarms.isEmpty();
        m_alarms.clear();
        return okReply(had);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.tabs — live snapshot over the app's windows/tab widgets.
// Event DELIVERY (onUpdated etc.) is EXT16's job; this answers the
// query surface the 27-entry delta measured.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::tabObject(WebView *view, int index,
                                            int windowId)
{
    QJsonObject tab;
    tab.insert(QStringLiteral("id"), idFor(view));
    tab.insert(QStringLiteral("index"), index);
    tab.insert(QStringLiteral("windowId"), windowId);
    QWebEnginePage *page = view ? view->page() : nullptr;
    TabWidget *tw = view ? view->tabWidget() : nullptr;
    Engine::Page *engine = view ? view->enginePage() : nullptr;
    const bool active = view && tw
        && tw->currentWebView() == view;
    tab.insert(QStringLiteral("active"), active);
    // The strip has no multi-select, so Chrome's highlighted set
    // collapses to the one active tab.
    tab.insert(QStringLiteral("highlighted"), active);
    tab.insert(QStringLiteral("selected"), active);
    // MD10 honesty — real strip/page state instead of constants:
    // pinned and group membership come off the TabWidget records,
    // discarded maps the SLEEP01 sleeping state (Chrome's name for
    // the same renderer release), audible/muted come off the live
    // page, and autoDiscardable reads the flag tabs.update's setter
    // plants on the view (Chrome's default is true).
    tab.insert(QStringLiteral("pinned"),
               tw && tw->isTabPinned(index));
    tab.insert(QStringLiteral("audible"),
               engine && engine->recentlyAudible());
    tab.insert(QStringLiteral("discarded"),
               (tw && tw->isTabSleeping(index))
                   || (engine && engine->lifecycleState()
                           == Engine::Page::LifecycleState::Discarded));
    const QVariant autoDiscard =
        view ? view->property("arktosExtAutoDiscardable") : QVariant();
    tab.insert(QStringLiteral("autoDiscardable"),
               !autoDiscard.isValid() || autoDiscard.toBool());
    tab.insert(QStringLiteral("groupId"),
               tw ? chromeGroupId(windowId, tw->tabGroupId(index))
                  : -1);
    tab.insert(QStringLiteral("incognito"),
               page && page->profile()
                   && page->profile()->isOffTheRecord());
    QJsonObject muted;
    muted.insert(QStringLiteral("muted"),
                 page && page->isAudioMuted());
    tab.insert(QStringLiteral("mutedInfo"), muted);
    tab.insert(QStringLiteral("url"),
               view ? view->url().toString() : QString());
    tab.insert(QStringLiteral("title"),
               page ? page->title() : QString());
    tab.insert(QStringLiteral("status"),
               view && view->isLoading()
                   ? QStringLiteral("loading")
                   : QStringLiteral("complete"));
    return tab;
}

QJsonObject ExtensionShimBackend::shapedTab(const QJsonObject &tab) const
{
    if (ExtensionBackgroundRuntime::instance()->seesUrls(
            m_extensionId,
            tab.value(QStringLiteral("url")).toString()))
        return tab;
    QJsonObject out = tab;
    out.remove(QStringLiteral("url"));
    out.remove(QStringLiteral("title"));
    out.remove(QStringLiteral("favIconUrl"));
    out.remove(QStringLiteral("pendingUrl"));
    return out;
}

QJsonObject ExtensionShimBackend::invokeTabs(const QString &method,
                                             const QJsonObject &args)
{
    // EXT16 parity: the query surface describes the same tab set
    // the event hub tracks — every top-level window, minus tabs on
    // profiles extensions may not see (OTR, tor).
    const QList<BrowserMainWindow *> windows =
        ExtensionEventHub::browsingWindows();

    if (method == QLatin1String("query")) {
        const QJsonObject q =
            args.value(QStringLiteral("query")).toObject();
        BrowserMainWindow *current = currentWindow();
        const int currentWid = idFor(current);
        QJsonArray out;
        for (BrowserMainWindow *window : windows) {
            if (!window)
                continue;
            const int wid = idFor(window);
            // WINDOW_ID_CURRENT (-2) resolves to the live window;
            // WINDOW_ID_NONE (-1) matches only window-less tabs —
            // the engine has none, so it filters everything.
            if (q.contains(QStringLiteral("windowId"))) {
                int wanted = int(q.value(QStringLiteral("windowId"))
                                     .toDouble(-2));
                if (wanted == -2)
                    wanted = currentWid;
                if (wanted != wid)
                    continue;
            }
            if (q.value(QStringLiteral("currentWindow")).toBool()
                    && wid != currentWid)
                continue;
            if (q.value(QStringLiteral("lastFocusedWindow")).toBool()
                    && wid != currentWid)
                continue;
            // Window-scoped filter — every Arktos browser window is
            // "normal"; a query for popup/panel/app/devtools matches
            // nothing rather than mislabeling the surface.
            if (q.contains(QStringLiteral("windowType"))
                    && q.value(QStringLiteral("windowType")).toString()
                           != QLatin1String("normal"))
                continue;
            TabWidget *tw = window->tabWidget();
            if (!tw)
                continue;
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                // Engine/widget tabs have no WebView; OTR and tor
                // pages are invisible to extensions (EXT16 parity).
                if (!ExtensionEventHub::trackable(view))
                    continue;
                QJsonObject tab = tabObject(view, i, wid);
                // Boolean state filters — Chrome's query keys map
                // one-to-one onto the tab object fields now that they
                // carry real state ('muted' reads mutedInfo.muted).
                const auto boolFilter = [&q, &tab](
                        const QString &key, const QJsonValue &actual) {
                    return !q.contains(key)
                        || q.value(key).toBool() == actual.toBool();
                };
                if (!boolFilter(QStringLiteral("active"),
                                tab.value(QStringLiteral("active")))
                        || !boolFilter(QStringLiteral("highlighted"),
                                tab.value(QStringLiteral("highlighted")))
                        || !boolFilter(QStringLiteral("pinned"),
                                tab.value(QStringLiteral("pinned")))
                        || !boolFilter(QStringLiteral("audible"),
                                tab.value(QStringLiteral("audible")))
                        || !boolFilter(QStringLiteral("discarded"),
                                tab.value(QStringLiteral("discarded")))
                        || !boolFilter(QStringLiteral("autoDiscardable"),
                                tab.value(QStringLiteral("autoDiscardable")))
                        || !boolFilter(QStringLiteral("muted"),
                                tab.value(QStringLiteral("mutedInfo"))
                                    .toObject()
                                    .value(QStringLiteral("muted"))))
                    continue;
                if (q.contains(QStringLiteral("groupId"))
                        && int(q.value(QStringLiteral("groupId"))
                                   .toDouble(-1))
                               != tab.value(QStringLiteral("groupId"))
                                      .toInt(-2))
                    continue;
                if (q.contains(QStringLiteral("index"))
                        && int(q.value(QStringLiteral("index"))
                                   .toDouble(-1)) != i)
                    continue;
                if (q.contains(QStringLiteral("status"))
                        && q.value(QStringLiteral("status")).toString()
                               != tab.value(QStringLiteral("status"))
                                      .toString())
                    continue;
                if (q.contains(QStringLiteral("title"))) {
                    const QString needle =
                        q.value(QStringLiteral("title")).toString();
                    if (!tab.value(QStringLiteral("title")).toString()
                            .contains(needle, Qt::CaseInsensitive))
                        continue;
                }
                if (q.contains(QStringLiteral("url"))) {
                    // Url match patterns are a Chrome syntax — do the
                    // honest subset: substring match (array = OR).
                    const QString url =
                        tab.value(QStringLiteral("url")).toString();
                    const QJsonValue patterns =
                        q.value(QStringLiteral("url"));
                    QStringList needles;
                    if (patterns.isString())
                        needles.append(patterns.toString());
                    else {
                        for (const QJsonValue &v : patterns.toArray())
                            needles.append(v.toString());
                    }
                    bool urlOk = needles.isEmpty();
                    for (const QString &needle : needles) {
                        // '*://*.host/*' style patterns collapse to
                        // their significant literal for the contains
                        // check; exact urls keep full fidelity.
                        QString lit = needle;
                        lit.remove(QLatin1Char('*'));
                        lit.remove(QStringLiteral("://"));
                        if (lit.isEmpty()
                                || url.contains(lit,
                                                Qt::CaseInsensitive))
                            urlOk = true;
                    }
                    if (!urlOk)
                        continue;
                }
                out.append(shapedTab(tab));
            }
        }
        return okReply(out);
    }
    if (method == QLatin1String("get")) {
        const int wanted =
            int(args.value(QStringLiteral("id")).toDouble(-1));
        for (BrowserMainWindow *window : windows) {
            if (!window)
                continue;
            TabWidget *tw = window->tabWidget();
            if (!tw)
                continue;
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (ExtensionEventHub::trackable(view)
                        && idFor(view) == wanted)
                    return okReply(shapedTab(
                        tabObject(view, i, idFor(window))));
            }
        }
        return errReply(QStringLiteral("not-found"));
    }
    if (method == QLatin1String("create")) {
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        // ER16: scheme-scoped navigation gate — an extension may put
        // a real tab only on url classes Chrome accepts from it
        // (web schemes, data:, about:blank, or its OWN extension
        // documents; relative spellings resolve there too).  Checked
        // before any window/tab is touched.
        const QString urlText =
            props.value(QStringLiteral("url")).toString();
        const QUrl url = extensionNavUrl(urlText);
        if (!urlText.isEmpty() && !url.isValid())
            return errReply(QStringLiteral("url-refused"));
        BrowserMainWindow *window = currentWindow();
        if (props.contains(QStringLiteral("windowId"))) {
            int wanted = int(
                props.value(QStringLiteral("windowId")).toDouble(-2));
            // WINDOW_ID_CURRENT names the live window, like query.
            if (wanted == -2)
                wanted = idFor(window);
            window = nullptr;
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    window = w;
                    break;
                }
            }
            // An explicit windowId that resolves nowhere is a typed
            // failure — Chrome answers "No window with id", we answer
            // no-window; it must NOT silently create one.
            if (!window)
                return errReply(QStringLiteral("no-window"));
        }
        if (!window) {
            // Chrome parity: with no window to host the tab,
            // tabs.create opens one — the same fallback
            // invokeWindows create() already uses.  A refused
            // creation (vault-locked persona) keeps no-window.
            BrowserApplication *app = BrowserApplication::instance();
            window = app ? app->newMainWindow() : nullptr;
        }
        if (!window || !window->tabWidget())
            return errReply(QStringLiteral("no-window"));
        const bool active = !props.contains(QStringLiteral("active"))
            || props.value(QStringLiteral("active")).toBool();
        WebView *view = window->tabWidget()->makeNewTab(active);
        if (!view)
            return errReply(QStringLiteral("no-tab"));
        if (!url.isEmpty())
            view->loadUrl(url);
        const int index =
            window->tabWidget()->webViewIndex(view);
        return okReply(shapedTab(
            tabObject(view, index, idFor(window))));
    }
    if (method == QLatin1String("update")) {
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        int wanted = int(args.value(QStringLiteral("id"))
                             .toDouble(-1));
        WebView *target = nullptr;
        BrowserMainWindow *targetWindow = nullptr;
        int targetIndex = -1;
        for (BrowserMainWindow *window : windows) {
            if (!window || !window->tabWidget())
                continue;
            TabWidget *tw = window->tabWidget();
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (!ExtensionEventHub::trackable(view))
                    continue;
                if (wanted < 0) {
                    // No id -> the active tab of the current window.
                    if (window == currentWindow()
                            && i == tw->currentIndex()) {
                        target = view;
                        targetWindow = window;
                        targetIndex = i;
                    }
                } else if (idFor(view) == wanted) {
                    target = view;
                    targetWindow = window;
                    targetIndex = i;
                }
            }
        }
        if (!target)
            return errReply(QStringLiteral("not-found"));
        if (props.contains(QStringLiteral("url"))) {
            // ER16: same navigation gate create() applies.
            const QString urlText =
                props.value(QStringLiteral("url")).toString();
            const QUrl url = extensionNavUrl(urlText);
            if (!urlText.isEmpty() && !url.isValid())
                return errReply(QStringLiteral("url-refused"));
            if (!url.isEmpty())
                target->loadUrl(url);
        }
        if (props.contains(QStringLiteral("pinned")))
            targetWindow->tabWidget()->setTabPinned(
                targetIndex,
                props.value(QStringLiteral("pinned")).toBool());
        if (props.contains(QStringLiteral("muted"))
                && target->page())
            target->page()->setAudioMuted(
                props.value(QStringLiteral("muted")).toBool());
        if (props.contains(QStringLiteral("autoDiscardable"))) {
            const bool on =
                props.value(QStringLiteral("autoDiscardable")).toBool();
            target->setProperty("arktosExtAutoDiscardable", on);
            // Chrome: autoDiscardable:false un-discards the tab.
            if (!on)
                targetWindow->tabWidget()->wakeTab(targetIndex);
        }
        if (props.value(QStringLiteral("active")).toBool())
            targetWindow->tabWidget()->setCurrentIndex(targetIndex);
        // setTabPinned slides the tab to the pinned-block edge — the
        // captured index is stale once a pin/unpin landed.
        const int replyIndex =
            targetWindow->tabWidget()->webViewIndex(target);
        return okReply(shapedTab(
            tabObject(target, replyIndex, idFor(targetWindow))));
    }
    if (method == QLatin1String("remove")) {
        // Chrome accepts one id or an array — resolve the whole set
        // first so a bogus id refuses the batch before any tab dies.
        // A missing/null argument is a call error, never "the
        // current tab" (unlike the optional-id ops below).
        const QJsonValue idv = args.value(QStringLiteral("ids"));
        if (!idv.isDouble() && !idv.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        QList<int> ids;
        if (idv.isArray()) {
            const QJsonArray arr = idv.toArray();
            for (const QJsonValue &v : arr)
                ids.append(int(v.toDouble(-1)));
        } else {
            ids.append(int(idv.toDouble(-1)));
        }
        QList<LocatedTab> targets;
        for (const int id : ids) {
            const LocatedTab t = id > 0 ? locateTab(id) : LocatedTab();
            if (!t.view)
                return errReply(QStringLiteral("not-found"));
            targets.append(t);
        }
        for (const LocatedTab &t : targets) {
            // Strip indexes shift as earlier closes land — re-resolve
            // each view's live position right before its close.
            const int at = t.tabs->webViewIndex(t.view);
            if (at >= 0)
                t.tabs->closeTab(at);
        }
        return okReply();
    }
    if (method == QLatin1String("reload")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        if (t.tabs->isTabSleeping(t.index)) {
            // A discarded page has no live document to reload —
            // waking rebuilds the WebContents on the current entry,
            // which IS the reload Chrome performs there.
            t.tabs->wakeTab(t.index);
        } else if (args.value(QStringLiteral("bypassCache")).toBool()
                   && t.view->enginePage()) {
            QAction *bypass = t.view->enginePage()->action(
                Engine::StandardAction::ReloadAndBypassCache);
            if (bypass)
                bypass->trigger();
            else
                t.tabs->reloadTab(t.index);
        } else {
            t.tabs->reloadTab(t.index);
        }
        return okReply();
    }
    if (method == QLatin1String("duplicate")) {
        const QJsonValue idv = args.value(QStringLiteral("id"));
        if (!idv.isDouble())
            return errReply(QStringLiteral("invalid-argument"));
        const LocatedTab t = locateTab(int(idv.toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        // cloneTab() keeps the copy in the source's group and private
        // context but appends it — Chrome lands the duplicate right
        // after the source, so slide it there.
        QSet<WebView *> before;
        for (int i = 0; i < t.tabs->count(); ++i)
            before.insert(t.tabs->webView(i));
        t.tabs->cloneTab(t.index);
        WebView *copy = nullptr;
        for (int i = 0; i < t.tabs->count(); ++i) {
            WebView *v = t.tabs->webView(i);
            if (v && !before.contains(v)) {
                copy = v;
                break;
            }
        }
        if (!copy)
            return errReply(QStringLiteral("no-tab"));
        const int at = t.tabs->webViewIndex(copy);
        if (at > t.index + 1)
            t.tabs->tabBar()->moveTab(at, t.index + 1);
        return okReply(shapedTab(tabObject(
            copy, t.tabs->webViewIndex(copy), t.windowId)));
    }
    if (method == QLatin1String("highlight")) {
        // Chrome takes tab INDEXES inside a window, not ids — and
        // multi-selects the rest.  The strip has no multi-select, so
        // the first listed tab activates and the reply reports the
        // one highlight that actually landed.
        int wanted = int(
            args.value(QStringLiteral("windowId")).toDouble(-2));
        BrowserMainWindow *target = nullptr;
        if (wanted == -2) {
            target = currentWindow();
        } else {
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    target = w;
                    break;
                }
            }
        }
        if (!target || !target->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *tw = target->tabWidget();
        const QJsonValue tv = args.value(QStringLiteral("indexes"));
        if (!tv.isDouble() && !tv.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        QList<int> indexes;
        if (tv.isArray()) {
            const QJsonArray arr = tv.toArray();
            for (const QJsonValue &v : arr)
                indexes.append(int(v.toDouble(-1)));
        } else {
            indexes.append(int(tv.toDouble(-1)));
        }
        int activated = -1;
        for (const int i : indexes) {
            if (i >= 0 && i < tw->count()
                    && ExtensionEventHub::trackable(tw->webView(i))) {
                tw->setCurrentIndex(i);
                activated = i;
                break;
            }
        }
        if (activated < 0)
            return errReply(QStringLiteral("not-found"));
        QJsonObject win = windowObject(target);
        win.insert(QStringLiteral("tabs"),
                   QJsonArray{shapedTab(tabObject(
                       tw->webView(activated), activated,
                       idFor(target)))});
        return okReply(win);
    }
    if (method == QLatin1String("move")) {
        const QJsonValue idv = args.value(QStringLiteral("ids"));
        if (!idv.isDouble() && !idv.isArray())
            return errReply(QStringLiteral("invalid-argument"));
        QList<int> ids;
        if (idv.isArray()) {
            const QJsonArray arr = idv.toArray();
            for (const QJsonValue &v : arr)
                ids.append(int(v.toDouble(-1)));
        } else {
            ids.append(int(idv.toDouble(-1)));
        }
        if (ids.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        const int to =
            int(args.value(QStringLiteral("index")).toDouble(-1));
        int wanted = int(
            args.value(QStringLiteral("windowId")).toDouble(-2));
        BrowserMainWindow *target = nullptr;
        if (wanted == -2) {
            // No windowId: same-strip reorder — the destination is
            // the moved tab's own window.
            const LocatedTab first = locateTab(ids.first());
            if (!first.view)
                return errReply(QStringLiteral("not-found"));
            target = first.window;
        } else {
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    target = w;
                    break;
                }
            }
            if (!target)
                return errReply(QStringLiteral("no-window"));
        }
        if (!target || !target->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *dst = target->tabWidget();
        QJsonArray moved;
        for (const int id : ids) {
            // Re-resolve per tab — earlier moves shift indexes.
            const LocatedTab t =
                id > 0 ? locateTab(id) : LocatedTab();
            if (!t.view)
                return errReply(QStringLiteral("not-found"));
            const int at = t.tabs->webViewIndex(t.view);
            if (at < 0)
                return errReply(QStringLiteral("not-found"));
            const int pos = moved.size();
            if (t.tabs == dst) {
                // Same-strip reorder — the bar's moveTab rides the
                // tabMoved bookkeeping (location bars, group
                // normalize, pin-boundary conversion).
                const int dstIndex = to < 0
                    ? dst->count() - 1
                    : qBound(0, to + pos, dst->count() - 1);
                if (dstIndex != at)
                    t.tabs->tabBar()->moveTab(at, dstIndex);
            } else {
                const int dstIndex =
                    to < 0 ? -1 : to + pos;
                if (!t.tabs->moveTabTo(at, dst, dstIndex))
                    return errReply(QStringLiteral("move-refused"));
            }
            // The reply reports the landing position — for a
            // cross-window move the view's strip changed entirely.
            TabWidget *landed = t.view->tabWidget();
            const int landedAt =
                landed ? landed->webViewIndex(t.view) : -1;
            moved.append(shapedTab(tabObject(
                t.view, landedAt, idFor(target))));
        }
        return okReply(ids.size() == 1
            ? moved.at(0) : QJsonValue(moved));
    }
    if (method == QLatin1String("goBack")
            || method == QLatin1String("goForward")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        Engine::Page *page = t.view ? t.view->enginePage() : nullptr;
        if (!page)
            return errReply(QStringLiteral("not-found"));
        const bool back = method == QLatin1String("goBack");
        if (back ? !page->canGoBack() : !page->canGoForward())
            return errReply(QStringLiteral("no-history"));
        if (back)
            page->back();
        else
            page->forward();
        return okReply();
    }
    if (method == QLatin1String("discard")) {
        const QJsonValue idv = args.value(QStringLiteral("id"));
        if (idv.isUndefined() || idv.isNull()) {
            // No id — Chrome picks the least-important discardable
            // tab.  The honest subset: the first background tab the
            // sleep path would actually take (the current tab, empty
            // pages and sleep-in-flight entries are never taken).
            BrowserMainWindow *current = currentWindow();
            TabWidget *tw = current ? current->tabWidget() : nullptr;
            if (!tw)
                return okReply(QJsonValue(QJsonValue::Null));
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (i == tw->currentIndex()
                        || !ExtensionEventHub::trackable(view)
                        || tw->isTabSleeping(i)
                        || view->url().isEmpty())
                    continue;
                tw->sleepTab(i);
                return okReply(shapedTab(
                    tabObject(view, i, idFor(current))));
            }
            return okReply(QJsonValue(QJsonValue::Null));
        }
        const LocatedTab t = locateTab(int(idv.toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        // An already-discarded tab answers its state again (Chrome
        // returns the tab); one the sleep path cannot take — the
        // active tab, an empty page — answers Chrome's undefined.
        if (t.tabs->isTabSleeping(t.index))
            return okReply(shapedTab(
                tabObject(t.view, t.index, t.windowId)));
        if (t.index == t.tabs->currentIndex()
                || t.view->url().isEmpty())
            return okReply(QJsonValue(QJsonValue::Null));
        t.tabs->sleepTab(t.index);
        // The discard commits after the async scroll/form capture —
        // the reply's tab reflects the request landing, and a dirty
        // form can still veto the drop itself.
        return okReply(shapedTab(
            tabObject(t.view, t.index, t.windowId)));
    }
    // MD11 — zoom surface.  get/setZoom ride the view's own zoom
    // machinery (setTabZoomFactor keeps m_currentZoom, the zoom
    // readout and reload reapplication coherent); zoomSettings are
    // a recorded model like sidePanel's — mode/scope store on the
    // tab, 'disabled' is enforced on this surface, and requests the
    // engine cannot honor (per-origin scoping, navigation-reset
    // 'automatic') land fidelity notes.
    if (method == QLatin1String("getZoom")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        return okReply(qreal(t.view->currentZoom()) / 100.0);
    }
    if (method == QLatin1String("setZoom")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        const QJsonObject settings = zoomSettingsFor(t.view);
        if (settings.value(QStringLiteral("mode")).toString()
                == QLatin1String("disabled"))
            return errReply(QStringLiteral("zoom-disabled"));
        const double zoom =
            args.value(QStringLiteral("zoom")).toDouble(-1.0);
        if (zoom <= 0.0)
            return errReply(QStringLiteral("invalid-argument"));
        const double before = qreal(t.view->currentZoom()) / 100.0;
        const double applied = t.view->setTabZoomFactor(zoom);
        // Chrome fires tabs.onZoomChange on every zoom commit —
        // push it with the factors and the tab's settings shape.
        emit pushEvent(QStringLiteral("tabs.onZoomChange"), QJsonObject{
            {QStringLiteral("tabId"), idFor(t.view)},
            {QStringLiteral("oldZoomFactor"), before},
            {QStringLiteral("newZoomFactor"), applied},
            {QStringLiteral("zoomSettings"), settings}});
        return okReply();
    }
    if (method == QLatin1String("getZoomSettings")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        return okReply(zoomSettingsFor(t.view));
    }
    if (method == QLatin1String("setZoomSettings")) {
        const LocatedTab t = locateTab(
            int(args.value(QStringLiteral("id")).toDouble(-1)));
        if (!t.view)
            return errReply(QStringLiteral("not-found"));
        const QJsonObject req =
            args.value(QStringLiteral("settings")).toObject();
        QJsonObject recorded =
            t.view->property("arktosExtZoomSettings").toJsonObject();
        const QString mode =
            req.value(QStringLiteral("mode")).toString();
        if (!mode.isEmpty()) {
            if (mode != QLatin1String("automatic")
                    && mode != QLatin1String("manual")
                    && mode != QLatin1String("disabled"))
                return errReply(QStringLiteral("invalid-argument"));
            recorded.insert(QStringLiteral("mode"), mode);
            if (mode == QLatin1String("automatic"))
                recordNote(QStringLiteral(
                    "tabs.setZoomSettings: 'automatic' recorded — "
                    "Arktos tab zoom persists across navigation "
                    "(manual semantics)"));
        }
        const QString scope =
            req.value(QStringLiteral("scope")).toString();
        if (!scope.isEmpty()) {
            if (scope != QLatin1String("per-origin")
                    && scope != QLatin1String("per-tab"))
                return errReply(QStringLiteral("invalid-argument"));
            recorded.insert(QStringLiteral("scope"), scope);
            if (scope == QLatin1String("per-origin"))
                recordNote(QStringLiteral(
                    "tabs.setZoomSettings: 'per-origin' recorded — "
                    "Arktos zoom applies per-tab"));
        }
        t.view->setProperty("arktosExtZoomSettings", recorded);
        return okReply();
    }
    if (method == QLatin1String("captureVisibleTab")) {
        int wanted = int(
            args.value(QStringLiteral("windowId")).toDouble(-2));
        BrowserMainWindow *target = nullptr;
        if (wanted == -2) {
            target = currentWindow();
        } else {
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    target = w;
                    break;
                }
            }
        }
        if (!target || !target->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *tw = target->tabWidget();
        WebView *view = tw->currentWebView();
        if (!ExtensionEventHub::trackable(view))
            return errReply(QStringLiteral("not-found"));
        // The capture exposes page pixels — the same url-disclosure
        // class as the tab object's url fields, so it rides the
        // seesUrls gate shapedTab applies ('tabs' or host coverage;
        // Chrome's activeTab invocation grant is not modeled
        // anywhere in this runtime).
        if (!ExtensionBackgroundRuntime::instance()->seesUrls(
                m_extensionId, view->url().toString()))
            return errReply(QStringLiteral("no-permission"));
        const QJsonObject opts =
            args.value(QStringLiteral("options")).toObject();
        const QString format =
            opts.value(QStringLiteral("format"))
                .toString(QStringLiteral("png"));
        if (format != QLatin1String("png")
                && format != QLatin1String("jpeg"))
            return errReply(QStringLiteral("invalid-argument"));
        QPixmap pixmap = view->grab();
        if (pixmap.isNull() && !view->size().isEmpty()) {
            // grab() asks the compositor — a never-realized view
            // still paints through render() into a scratch image.
            QImage fallback(view->size(), QImage::Format_ARGB32);
            fallback.fill(Qt::transparent);
            view->render(&fallback);
            pixmap = QPixmap::fromImage(fallback);
        }
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        const int quality =
            int(opts.value(QStringLiteral("quality")).toDouble(-1));
        const bool saved = !pixmap.isNull()
            && (format == QLatin1String("jpeg")
                ? pixmap.toImage().save(&buffer, "JPEG",
                                        quality < 0 ? -1
                                                    : qBound(0, quality,
                                                             100))
                : pixmap.save(&buffer, "PNG"));
        if (!saved || bytes.isEmpty())
            return errReply(QStringLiteral("capture-failed"));
        return okReply(QStringLiteral("data:image/%1;base64,%2")
            .arg(format, QString::fromLatin1(bytes.toBase64())));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// EXT17 — context-local resolution: the tab whose page IS the asking
// context.  An extension page hosted in a real browser tab (options
// pages via tabs.create) answers itself; a popup/detached page gets
// Chrome's undefined (null result), never a guess.
QJsonObject ExtensionShimBackend::tabForPageReply(QWebEnginePage *page)
{
    if (page) {
        for (BrowserMainWindow *window
             : ExtensionEventHub::browsingWindows()) {
            if (!window || !window->tabWidget())
                continue;
            TabWidget *tw = window->tabWidget();
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (ExtensionEventHub::trackable(view)
                        && view->page() == page)
                    return okReply(tabObject(view, i, idFor(window)));
            }
        }
    }
    return okReply(QJsonValue(QJsonValue::Null));
}

// -------------------------------------------------------------------
// chrome.webNavigation — frame lookup.  The engine only registers
// the main frame per tab (no subframe map is kept), so getFrame /
// getAllFrames answer frameId 0 honestly and report Chrome's own
// 'no frame' shape for anything deeper rather than fabricating a
// frame.  documentId is deliberately absent — callers that verify a
// navigation race (Bitwarden's IPC send) treat a missing id as
// 'cannot check' and proceed, matching Chrome's legacy profile.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeWebNavigation(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("getFrame")
            || method == QLatin1String("getAllFrames")) {
        const int tabId =
                args.value(QStringLiteral("tabId")).toInt(-1);
        WebView *view = viewForTabId(tabId);
        if (!view || !view->page())
            return errReply(QStringLiteral("no-tab"));
        const QJsonObject frame{
            {QStringLiteral("errorOccurred"), false},
            {QStringLiteral("frameId"), 0},
            {QStringLiteral("parentFrameId"), -1},
            {QStringLiteral("frameType"),
             QStringLiteral("outermost_frame")},
            {QStringLiteral("processId"), -1},
            {QStringLiteral("documentLifecycle"),
             QStringLiteral("active")},
            {QStringLiteral("url"), view->page()->url().toString()}};
        if (method == QLatin1String("getFrame")) {
            const int frameId =
                    args.value(QStringLiteral("frameId")).toInt(-1);
            if (frameId != 0)
                return errReply(QStringLiteral("no-frame"));
            return okReply(frame);
        }
        return okReply(QJsonArray{frame});
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.windows — the window-level mirror of the tabs snapshot.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::windowObject(BrowserMainWindow *window)
{
    QJsonObject out;
    out.insert(QStringLiteral("id"), idFor(window));
    out.insert(QStringLiteral("focused"),
               window && window->isActiveWindow());
    out.insert(QStringLiteral("state"), QStringLiteral("normal"));
    out.insert(QStringLiteral("type"), QStringLiteral("normal"));
    out.insert(QStringLiteral("alwaysOnTop"), false);
    out.insert(QStringLiteral("incognito"), false);
    return out;
}

QJsonObject ExtensionShimBackend::invokeWindows(const QString &method,
                                              const QJsonObject &args)
{
    // EXT16 parity: a window with no trackable tab (all-OTR, tor)
    // is invisible to extensions — filter it out of every answer.
    QList<BrowserMainWindow *> windows;
    for (BrowserMainWindow *window
         : ExtensionEventHub::browsingWindows()) {
        if (!window || !window->tabWidget())
            continue;
        TabWidget *tw = window->tabWidget();
        bool visible = false;
        for (int i = 0; i < tw->count(); ++i) {
            if (ExtensionEventHub::trackable(tw->webView(i))) {
                visible = true;
                break;
            }
        }
        if (visible)
            windows.append(window);
    }
    if (method == QLatin1String("get")
            || method == QLatin1String("getCurrent")
            || method == QLatin1String("getLastFocused")) {
        if (method == QLatin1String("get")) {
            const int wanted =
                int(args.value(QStringLiteral("id")).toDouble(-1));
            for (BrowserMainWindow *window : windows) {
                if (window && idFor(window) == wanted)
                    return okReply(windowObject(window));
            }
            return errReply(QStringLiteral("not-found"));
        }
        BrowserMainWindow *current = currentWindow();
        if (!current || !windows.contains(current))
            return errReply(QStringLiteral("no-window"));
        return okReply(windowObject(current));
    }
    if (method == QLatin1String("getAll")) {
        QJsonArray out;
        for (BrowserMainWindow *window : windows)
            out.append(windowObject(window));
        return okReply(out);
    }
    if (method == QLatin1String("create")) {
        // EXT17 — extensions open real windows (Bitwarden's pop-out
        // and SSO flows call this).  type:'popup'/'panel' has no
        // chrome-less shell to map to, so it lands as a normal
        // window; the url list fans out one tab each.
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        QStringList urls;
        const QJsonValue urlVal = props.value(QStringLiteral("url"));
        if (urlVal.isArray()) {
            for (const QJsonValue &u : urlVal.toArray())
                urls.append(u.toString());
        } else if (urlVal.isString()) {
            urls.append(urlVal.toString());
        }
        QList<QUrl> navUrls;
        for (const QString &u : urls) {
            // ER16: same navigation gate chrome.tabs applies.
            const QUrl url = extensionNavUrl(u);
            if (!u.isEmpty() && !url.isValid())
                return errReply(QStringLiteral("url-refused"));
            navUrls.append(url);
        }
        BrowserApplication *app = BrowserApplication::instance();
        if (!app)
            return errReply(QStringLiteral("no-window"));
        BrowserMainWindow *window = app->newMainWindow();
        if (!window || !window->tabWidget())
            return errReply(QStringLiteral("no-window"));
        TabWidget *tw = window->tabWidget();
        WebView *first = tw->count() ? tw->webView(0) : nullptr;
        bool usedFirst = false;
        for (const QUrl &url : navUrls) {
            WebView *view = (!usedFirst && first) ? first
                : tw->makeNewTab(false);
            if (!view)
                continue;
            usedFirst = true;
            if (!url.isEmpty())
                view->loadUrl(url);
        }
        if (props.value(QStringLiteral("focused")).toBool(true))
            window->activateWindow();
        QJsonObject reply = windowObject(window);
        // Chrome answers with the tabs array populated — callers
        // read win.tabs[0].id to drive follow-up chrome.tabs calls.
        QJsonArray tabs;
        for (int i = 0; i < tw->count(); ++i) {
            WebView *view = tw->webView(i);
            if (ExtensionEventHub::trackable(view))
                tabs.append(tabObject(view, i, idFor(window)));
        }
        reply.insert(QStringLiteral("tabs"), tabs);
        return okReply(reply);
    }
    if (method == QLatin1String("update")) {
        const int wanted =
            int(args.value(QStringLiteral("id")).toDouble(-1));
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        for (BrowserMainWindow *window : windows) {
            if (!window || idFor(window) != wanted)
                continue;
            const QString state =
                props.value(QStringLiteral("state")).toString();
            if (state == QLatin1String("minimized"))
                window->showMinimized();
            else if (state == QLatin1String("maximized"))
                window->showMaximized();
            else if (state == QLatin1String("fullscreen"))
                window->showFullScreen();
            else if (state == QLatin1String("normal"))
                window->showNormal();
            if (props.value(QStringLiteral("focused")).toBool())
                window->activateWindow();
            return okReply(windowObject(window));
        }
        return errReply(QStringLiteral("not-found"));
    }
    if (method == QLatin1String("remove")) {
        const int wanted =
            int(args.value(QStringLiteral("id")).toDouble(-1));
        for (BrowserMainWindow *window : windows) {
            if (!window || idFor(window) != wanted)
                continue;
            window->close();
            return okReply();
        }
        return errReply(QStringLiteral("not-found"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.action / chrome.browserAction — state feeds the EXT08
// button strip via ExtensionInfo; no native toolbar delegate exists.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeAction(const QString &method,
                                               const QJsonObject &args)
{
    ActionState &state = actionStates()[m_extensionId];
    const QString key = args.value(QStringLiteral("key")).toString();
    if (method == QLatin1String("set")) {
        if (key == QLatin1String("title"))
            state.title = args.value(QStringLiteral("value")).toString();
        else if (key == QLatin1String("badgeText"))
            state.badgeText =
                args.value(QStringLiteral("value")).toString();
        else if (key == QLatin1String("badgeColor")
                 || key == QLatin1String("badgeTextColor")) {
            const QJsonValue v = args.value(QStringLiteral("value"));
            QString css;
            if (v.isString())
                css = v.toString();
            else if (v.isArray()) {
                const QJsonArray c = v.toArray();
                if (c.size() >= 3)
                    css = QStringLiteral("rgba(%1,%2,%3,%4)")
                        .arg(c.at(0).toInt()).arg(c.at(1).toInt())
                        .arg(c.at(2).toInt())
                        .arg(c.size() > 3
                                 ? c.at(3).toDouble() / 255.0 : 1.0);
            }
            if (key == QLatin1String("badgeColor"))
                state.badgeColor = css;
            else
                state.badgeTextColor = css;
        } else if (key == QLatin1String("popup")) {
            state.popup = args.value(QStringLiteral("value")).toString();
            state.popupSet = true;
        }
        else if (key == QLatin1String("enabled"))
            state.enabled =
                args.value(QStringLiteral("value")).toBool();
        else
            return errReply(QStringLiteral("invalid-key"));
        emit actionStateChanged();
        return okReply();
    }
    if (method == QLatin1String("get")) {
        if (key == QLatin1String("title"))
            return okReply(state.title);
        if (key == QLatin1String("badgeText"))
            return okReply(state.badgeText);
        if (key == QLatin1String("badgeColor")
                || key == QLatin1String("badgeTextColor")) {
            const QString css = key == QLatin1String("badgeColor")
                ? state.badgeColor : state.badgeTextColor;
            // Chrome answers [r,g,b,a]; we stored css — parse the
            // rgba() we wrote, else answer transparent.
            QJsonArray arr{0, 0, 0, 0};
            static const QRegularExpression re(
                QStringLiteral(
                    "rgba?\\((\\d+),(\\d+),(\\d+)(?:,([\\d.]+))?\\)"));
            const QRegularExpressionMatch m = re.match(css);
            if (m.hasMatch()) {
                arr = QJsonArray{m.captured(1).toInt(),
                                 m.captured(2).toInt(),
                                 m.captured(3).toInt(),
                                 m.captured(4).isEmpty()
                                     ? 255
                                     : int(m.captured(4).toDouble()
                                           * 255)};
            }
            return okReply(arr);
        }
        if (key == QLatin1String("popup"))
            return okReply(state.popup);
        if (key == QLatin1String("enabled"))
            return okReply(state.enabled);
        return errReply(QStringLiteral("invalid-key"));
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.i18n — fallback message lookup from _locales/<lang>/
// messages.json (the real binding reads the same files when present).
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::manifestJson() const
{
    if (!m_manifestLoaded) {
        ExtensionShimBackend *self =
            const_cast<ExtensionShimBackend *>(this);
        self->m_manifestLoaded = true;
        QFile file(m_extensionPath
                   + QLatin1String("/manifest.json"));
        if (file.size() <= 1024 * 1024
                && file.open(QIODevice::ReadOnly))
            self->m_manifest =
                QJsonDocument::fromJson(file.readAll()).object();
    }
    return m_manifest;
}

QStringList ExtensionShimBackend::manifestPermissions() const
{
    // manifestJson()'s "permissions" key is the REQUIRED set already
    // — optional_permissions live under their own key and stay inert
    // (invokePermissions answers request() typed unsupported), so an
    // optional declaration must never unlock a gated call.
    QStringList perms;
    for (const QJsonValue &v : manifestJson()
             .value(QStringLiteral("permissions")).toArray())
        perms.append(v.toString());
    return perms;
}

QUrl ExtensionShimBackend::extensionNavUrl(const QString &text) const
{
    const QUrl url(text);
    if (url.isEmpty())
        return url;
    const QString scheme = url.scheme();
    if (scheme.isEmpty()) {
        // Relative spelling — Chrome resolves it against the
        // extension's own origin; apply the same bundle-path hygiene
        // openOptionsPage does before the rewrite ('//host/x' dies
        // here rather than escaping as a network-path reference).
        const QString rel = url.toString();
        if (rel.startsWith(QLatin1Char('/'))
                || rel.contains(QLatin1Char(':'))
                || rel.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            return QUrl();
        return QUrl(QStringLiteral("chrome-extension://")
                    + m_extensionId + QLatin1Char('/') + rel);
    }
    if (!WebView::isUrlAllowedOnUntrustedInput(url))
        return QUrl();
    if (scheme == QLatin1String("chrome-extension"))
        // An extension may open its own documents — never another
        // extension's.
        return url.host() == m_extensionId ? url : QUrl();
    if (scheme == QLatin1String("about"))
        return url.path().isEmpty()
                || url.path() == QLatin1String("blank")
            ? url : QUrl();
    if (scheme == QLatin1String("http")
            || scheme == QLatin1String("https")
            || scheme == QLatin1String("ftp")
            || scheme == QLatin1String("data"))
        return url;
    return QUrl();
}

QJsonObject ExtensionShimBackend::i18nMessages(
        const QString &extensionPath)
{
    if (extensionPath.isEmpty())
        return QJsonObject();

    // Locale pick order: UI languages, then the manifest's declared
    // default, then 'en'.
    QStringList langs = QLocale().uiLanguages();
    {
        QFile mfile(extensionPath + QLatin1String("/manifest.json"));
        if (mfile.size() <= 1024 * 1024
                && mfile.open(QIODevice::ReadOnly)) {
            const QString deflt = QJsonDocument::fromJson(
                mfile.readAll()).object()
                .value(QStringLiteral("default_locale")).toString();
            if (!deflt.isEmpty())
                langs.append(deflt);
        }
    }
    langs.append(QStringLiteral("en"));

    for (const QString &lang : langs) {
        QString dir = QLocale(lang).name();
        QFile file(extensionPath + QLatin1String("/_locales/")
                   + dir + QLatin1String("/messages.json"));
        if (!file.exists()) {
            // Chrome also accepts the short tag (_locales/en).
            const QString shortTag = dir.section(QLatin1Char('_'), 0, 0);
            file.setFileName(extensionPath
                             + QLatin1String("/_locales/") + shortTag
                             + QLatin1String("/messages.json"));
        }
        if (file.size() > 1024 * 1024
                || !file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject root =
            QJsonDocument::fromJson(file.readAll()).object();
        QJsonObject flat;
        for (auto it = root.constBegin(); it != root.constEnd(); ++it) {
            const QString msg = it.value().toObject()
                .value(QStringLiteral("message")).toString();
            if (!msg.isEmpty())
                flat.insert(it.key(), msg);
        }
        return flat;
    }
    return QJsonObject();
}

QJsonObject ExtensionShimBackend::invokeI18n(const QString &method,
                                             const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method != QLatin1String("messages"))
        return errReply(QStringLiteral("unimplemented"));
    if (m_extensionPath.isEmpty())
        return errReply(QStringLiteral("no-path"));
    return okReply(i18nMessages(m_extensionPath));
}

// -------------------------------------------------------------------
// chrome.permissions — manifest-declared grants (the JS side answers
// from the handshake manifest; this is the bridge fallback).
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokePermissions(
        const QString &method, const QJsonObject &args)
{
    const QJsonObject manifest = manifestJson();
    // ER16: the GRANTED set is what Chrome reports — required
    // permissions only.  optional_permissions are never granted
    // (request() below is typed unsupported), so listing them would
    // over-claim the extension's own grants.
    const QStringList perms = manifestPermissions();
    QStringList origins;
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("host_permissions")).toArray())
        origins.append(v.toString());

    if (method == QLatin1String("contains")) {
        bool ok = true;
        for (const QJsonValue &v : args.value(
                 QStringLiteral("permissions")).toArray()) {
            if (!perms.contains(v.toString()))
                ok = false;
        }
        for (const QJsonValue &v : args.value(
                 QStringLiteral("origins")).toArray()) {
            if (!origins.contains(v.toString()))
                ok = false;
        }
        return okReply(ok);
    }
    if (method == QLatin1String("getAll")) {
        QJsonObject all;
        QJsonArray p, o;
        for (const QString &s : perms) p.append(s);
        for (const QString &s : origins) o.append(s);
        all.insert(QStringLiteral("permissions"), p);
        all.insert(QStringLiteral("origins"), o);
        return okReply(all);
    }
    // request/remove would need a grant UI — typed error.
    return errReply(QStringLiteral("unsupported"));
}

// -------------------------------------------------------------------
// chrome.contextMenus — the shared registry EXT16's page-menu
// contribution point renders.  create() answers the id like Chrome
// and refuses duplicates (a silent overwrite would orphan a
// listener's menuItemId).
// -------------------------------------------------------------------

QList<ExtensionShimBackend::ContextMenuEntry>
ExtensionShimBackend::contextMenuEntries()
{
    QList<ContextMenuEntry> out;
    const MenuRegistry &registry = menuRegistry();
    for (auto it = registry.items.constBegin();
         it != registry.items.constEnd(); ++it) {
        for (const auto &item : it.value()) {
            ContextMenuEntry entry;
            entry.extensionId = it.key();
            entry.extensionName = registry.names.value(it.key());
            entry.itemId = item.first;
            entry.props = item.second;
            out.append(entry);
        }
    }
    return out;
}

QJsonObject ExtensionShimBackend::contextMenuItem(
        const QString &extensionId, const QString &itemId)
{
    const auto items = menuRegistry().items.value(extensionId);
    for (const auto &item : items) {
        if (item.first == itemId)
            return item.second;
    }
    return QJsonObject();
}

void ExtensionShimBackend::setContextMenuChecked(
        const QString &extensionId, const QString &itemId,
        bool checked)
{
    auto &items = menuRegistry().items[extensionId];
    for (auto &item : items) {
        if (item.first == itemId) {
            item.second.insert(QStringLiteral("checked"), checked);
            return;
        }
    }
}

QJsonObject ExtensionShimBackend::invokeContextMenus(
        const QString &method, const QJsonObject &args)
{
    // ER16: 'contextMenus' (or Firefox's 'menus') gates the
    // namespace — without it an ungated create() would render
    // extension-controlled entries into the user's context menu.
    const QStringList perms = manifestPermissions();
    if (!perms.contains(QLatin1String("contextMenus"))
            && !perms.contains(QLatin1String("menus")))
        return errReply(QStringLiteral("no-permission"));
    // Re-claim the slot — a stale owner's destructor may already
    // have cleared it (respawn ordering or test backends sharing an
    // id); whoever writes owns the entries.
    if (menuRegistry().owners.value(m_extensionId) != this) {
        menuRegistry().owners.insert(m_extensionId, this);
        menuRegistry().names.insert(m_extensionId, m_extensionName);
    }
    auto &items = menuRegistry().items[m_extensionId];
    const auto findItem = [&items](const QString &id) {
        for (int i = 0; i < items.size(); ++i) {
            if (items.at(i).first == id)
                return i;
        }
        return -1;
    };
    if (method == QLatin1String("create")) {
        QJsonObject props = args.value(QStringLiteral("props")).toObject();
        QString id = props.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = QStringLiteral("item-%1").arg(items.size() + 1);
        if (findItem(id) >= 0)
            return errReply(QStringLiteral("duplicate-id"));
        props.insert(QStringLiteral("id"), id);
        items.append({id, props});
        return okReply(QJsonObject{{QStringLiteral("id"), id}});
    }
    if (method == QLatin1String("update")) {
        const QString id = args.value(QStringLiteral("id")).toString();
        const int at = findItem(id);
        if (at < 0)
            return errReply(QStringLiteral("not-found"));
        QJsonObject props = items.at(at).second;
        const QJsonObject patch =
            args.value(QStringLiteral("props")).toObject();
        for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
            props.insert(it.key(), it.value());
        items[at].second = props;
        return okReply();
    }
    if (method == QLatin1String("remove")) {
        const QString id = args.value(QStringLiteral("id")).toString();
        const int at = findItem(id);
        if (at < 0)
            return errReply(QStringLiteral("not-found"));
        items.removeAt(at);
        return okReply();
    }
    if (method == QLatin1String("removeAll")) {
        items.clear();
        return okReply();
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.sidePanel — Arktos has no side-panel surface; options are
// recorded honestly so EXT17/later work can render them.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeSidePanel(
        const QString &method, const QJsonObject &args)
{
    // ER16: 'sidePanel' gates the namespace like Chrome.
    if (!manifestPermissions().contains(QLatin1String("sidePanel")))
        return errReply(QStringLiteral("no-permission"));
    if (method == QLatin1String("setOptions")) {
        m_sidePanelOptions =
            args.value(QStringLiteral("options")).toObject();
        return okReply();
    }
    if (method == QLatin1String("getOptions"))
        return okReply(m_sidePanelOptions);
    if (method == QLatin1String("setPanelBehavior")) {
        m_sidePanelBehavior =
            args.value(QStringLiteral("behavior")).toObject();
        return okReply();
    }
    if (method == QLatin1String("getPanelBehavior"))
        return okReply(m_sidePanelBehavior);
    return errReply(QStringLiteral("unimplemented"));
}

void ExtensionShimBackend::recordNote(const QString &message)
{
    if (m_notes.size() < 500)
        m_notes.append(message.left(2048));
    emit noteRecorded(message.left(2048));
}

// -------------------------------------------------------------------
// chrome.notifications — ER12.  Chrome exposes the namespace only to
// extensions declaring the 'notifications' permission — the gate
// reads the same installed manifest the permissions.* surface does.
// The record registry and platform presentation live in
// ExtensionNotificationCenter (freedesktop Notify on the session
// bus); events route back through the runtime's per-extension
// fan-out.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeNotifications(
        const QString &method, const QJsonObject &args)
{
    if (!manifestPermissions().contains(
            QLatin1String("notifications")))
        return errReply(QStringLiteral("no-permission"));

    ExtensionNotificationCenter *center =
        ExtensionNotificationCenter::instance();
    if (method == QLatin1String("create"))
        return center->create(
            this, args.value(QStringLiteral("id")).toString(),
            args.value(QStringLiteral("options")).toObject());
    if (method == QLatin1String("update"))
        return center->update(
            this, args.value(QStringLiteral("id")).toString(),
            args.value(QStringLiteral("options")).toObject());
    if (method == QLatin1String("clear"))
        return center->clear(
            this, args.value(QStringLiteral("id")).toString());
    if (method == QLatin1String("getAll"))
        return center->getAll(this);
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.commands — ER14.  No permission gate: Chrome serves the
// namespace to every extension.  getAll answers the registry's
// bound-state rows — every declared command with the CURRENTLY
// bound shortcut in Chrome key format, '' for absent/unbindable
// suggestions (Chrome's unassigned rows).  The event side is the
// C++-driven commands.onCommand dispatch from
// ExtensionCommandRegistry.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeCommands(
        const QString &method, const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method == QLatin1String("getAll"))
        return okReply(ExtensionCommandRegistry::instance()
                           ->commandList(m_extensionId));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.idle — ER13.  Chrome exposes the namespace only to
// extensions declaring 'idle'; the gate reads the same installed
// manifest permissions.* answers from.  State lives in
// ExtensionIdleWatcher (browser-global, like Chrome's): an
// application-wide input filter feeding a QTimer at the configured
// detection interval.  Activity is measured inside Arktos windows
// only and 'locked' is never reported — the shim notes the
// approximation once per context.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeIdle(
        const QString &method, const QJsonObject &args)
{
    if (!manifestPermissions().contains(QLatin1String("idle")))
        return errReply(QStringLiteral("no-permission"));

    ExtensionIdleWatcher *watcher = ExtensionIdleWatcher::instance();
    if (method == QLatin1String("queryState")) {
        const QJsonValue v = args.value(QStringLiteral("interval"));
        if (!v.isDouble() || v.toDouble() < 0)
            return errReply(QStringLiteral("invalid-argument"));
        return okReply(watcher->queryState(int(v.toDouble())));
    }
    if (method == QLatin1String("setDetectionInterval")) {
        const QJsonValue v = args.value(QStringLiteral("interval"));
        if (!v.isDouble() || v.toDouble() < 0)
            return errReply(QStringLiteral("invalid-argument"));
        watcher->setDetectionInterval(int(v.toDouble()));
        return okReply();
    }
    // The shim's onStateChanged onAdd — a listener registration is
    // what arms the watcher for an extension that only listens.
    if (method == QLatin1String("subscribe")) {
        watcher->subscribe();
        return okReply();
    }
    // getAutoLockDelay exists on ChromeOS only — typed, not silent.
    if (method == QLatin1String("getAutoLockDelay"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.management — ER15.  getSelf is ungated like Chrome (the
// only member that needs no 'management' permission) and answers the
// real ExtensionInfo shape from the manifest + the manager's own
// records.  Every other member rides Chrome's failure vocabulary:
// 'management' undeclared -> no-permission, declared-but-unserved ->
// unsupported.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeManagement(
        const QString &method, const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method != QLatin1String("getSelf")) {
        return errReply(
            manifestPermissions().contains(QLatin1String("management"))
                ? QStringLiteral("unsupported")
                : QStringLiteral("no-permission"));
    }

    const QJsonObject manifest = manifestJson();
    const auto localized = [this](const QString &value) {
        // Manifests localize user-facing strings as __MSG_key__ —
        // getSelf answers the resolved text like Chrome does.
        static const QRegularExpression msgRe(
            QStringLiteral("^__MSG_(.+)__$"));
        const QRegularExpressionMatch m = msgRe.match(value);
        if (!m.hasMatch())
            return value;
        const QString resolved = i18nMessages(m_extensionPath)
            .value(m.captured(1)).toString();
        return resolved.isEmpty() ? value : resolved;
    };

    // The manager's record is the source of truth for enabled and
    // install provenance; a hosted backend that the manager lost
    // track of still answers enabled (it is running).
    bool enabled = true;
    QString installType = QStringLiteral("development");
    if (ExtensionManager *manager = ExtensionManager::instance()) {
        for (const ExtensionManager::ExtensionInfo &info
             : manager->extensions()) {
            if (info.id != m_extensionId)
                continue;
            enabled = info.enabled;
            installType = info.installed
                ? QStringLiteral("normal")
                : QStringLiteral("development");
            break;
        }
    }

    QJsonObject info;
    info.insert(QStringLiteral("id"), m_extensionId);
    info.insert(QStringLiteral("name"),
                localized(manifest.value(QStringLiteral("name"))
                              .toString(m_extensionName)));
    info.insert(QStringLiteral("shortName"),
                localized(manifest.value(QStringLiteral("short_name"))
                              .toString(info.value(
                                  QStringLiteral("name")).toString())));
    info.insert(QStringLiteral("description"),
                localized(manifest.value(
                    QStringLiteral("description")).toString()));
    info.insert(QStringLiteral("version"),
                manifest.value(QStringLiteral("version")).toString());
    info.insert(QStringLiteral("versionName"),
                manifest.value(QStringLiteral("version_name"))
                    .toString());
    info.insert(QStringLiteral("manifestVersion"),
                manifest.value(QStringLiteral("manifest_version"))
                    .toInt());
    info.insert(QStringLiteral("enabled"), enabled);
    info.insert(QStringLiteral("mayEnable"), !enabled);
    info.insert(QStringLiteral("mayDisable"), enabled);
    info.insert(QStringLiteral("type"), QStringLiteral("extension"));
    info.insert(QStringLiteral("installType"), installType);
    info.insert(QStringLiteral("offlineEnabled"),
                manifest.value(QStringLiteral("offline_enabled"))
                    .toBool(true));
    const QString homepage =
        manifest.value(QStringLiteral("homepage_url")).toString();
    if (!homepage.isEmpty())
        info.insert(QStringLiteral("homepageUrl"), homepage);
    const QString updateUrl =
        manifest.value(QStringLiteral("update_url")).toString();
    if (!updateUrl.isEmpty())
        info.insert(QStringLiteral("updateUrl"), updateUrl);
    // options page — same validation openOptionsPage applies before
    // turning the manifest path into a chrome-extension url.
    QString options = manifest.value(QStringLiteral("options_ui"))
        .toObject().value(QStringLiteral("page")).toString();
    if (options.isEmpty())
        options = manifest.value(QStringLiteral("options_page"))
            .toString();
    if (!options.isEmpty() && !options.startsWith(QLatin1Char('/'))
            && !options.contains(QLatin1Char(':'))
            && !options.split(QLatin1Char('/')).contains(
                QStringLiteral("..")))
        info.insert(QStringLiteral("optionsUrl"),
                    QStringLiteral("chrome-extension://")
                        + m_extensionId + QLatin1Char('/') + options);
    QJsonArray icons;
    const QJsonObject iconMap =
        manifest.value(QStringLiteral("icons")).toObject();
    for (auto it = iconMap.constBegin(); it != iconMap.constEnd();
         ++it) {
        const QString path = it.value().toString();
        if (path.isEmpty() || path.startsWith(QLatin1Char('/'))
                || path.contains(QLatin1Char(':'))
                || path.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            continue;
        QJsonObject icon;
        icon.insert(QStringLiteral("size"), it.key().toInt());
        icon.insert(QStringLiteral("url"),
                    QStringLiteral("chrome-extension://")
                        + m_extensionId + QLatin1Char('/') + path);
        icons.append(icon);
    }
    if (!icons.isEmpty())
        info.insert(QStringLiteral("icons"), icons);
    QJsonArray permissions;
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("permissions")).toArray())
        permissions.append(v.toString());
    info.insert(QStringLiteral("permissions"), permissions);
    // hostPermissions = host_permissions ∪ declared content-script
    // matches ∪ MV2-style match patterns inside permissions —
    // Chrome's management surface reports the union.
    QJsonArray hostPermissions;
    const auto addHost = [&hostPermissions](const QString &pattern) {
        for (const QJsonValue &v : hostPermissions) {
            if (v.toString() == pattern)
                return;
        }
        hostPermissions.append(pattern);
    };
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("host_permissions")).toArray())
        addHost(v.toString());
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("content_scripts")).toArray()) {
        for (const QJsonValue &m : v.toObject().value(
                 QStringLiteral("matches")).toArray())
            addHost(m.toString());
    }
    for (const QJsonValue &v : permissions) {
        const QString p = v.toString();
        if (p.contains(QLatin1String("://"))
                || p == QLatin1String("<all_urls>"))
            addHost(p);
    }
    info.insert(QStringLiteral("hostPermissions"), hostPermissions);
    return okReply(info);
}

// -------------------------------------------------------------------
// chrome.extension — ER15.  The one bridge op the shim cannot answer
// in-page: a getViews() handle's location.reload() — the real
// capability Chrome's Window reference would carry, routed to the
// context's actual page.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeExtension(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("reloadView")) {
        const QString contextId =
            args.value(QStringLiteral("contextId")).toString();
        if (contextId.isEmpty())
            return errReply(QStringLiteral("invalid-argument"));
        return ExtensionBackgroundRuntime::instance()
            ->reloadContextPage(m_extensionId, contextId);
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.sidebarAction — ER15.  The Firefox-shape sugar over the
// sidePanel bookkeeping: panel/title/icon are recorded honestly the
// same way m_sidePanelOptions is; isOpen answers false (no sidebar
// surface can be open — that is the truth, not a stub); open/close/
// toggle degrade typed because there is nothing to open.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeSidebarAction(
        const QString &method, const QJsonObject &args)
{
    // ER16: Firefox installs browser.sidebarAction only when the
    // manifest declares sidebar_action — the shim mirrors that at
    // install time; this is the call-level boundary behind it.
    if (!manifestJson().contains(QStringLiteral("sidebar_action")))
        return errReply(QStringLiteral("no-permission"));
    if (method == QLatin1String("setPanel")) {
        m_sidebarAction.insert(
            QStringLiteral("panel"),
            args.value(QStringLiteral("panel")).toString());
        return okReply();
    }
    if (method == QLatin1String("getPanel")) {
        QJsonObject out;
        out.insert(QStringLiteral("panel"),
                   m_sidebarAction.value(
                       QStringLiteral("panel")).toString());
        return okReply(out);
    }
    if (method == QLatin1String("setTitle")) {
        m_sidebarAction.insert(
            QStringLiteral("title"),
            args.value(QStringLiteral("title")).toString());
        return okReply();
    }
    if (method == QLatin1String("getTitle")) {
        QJsonObject out;
        out.insert(QStringLiteral("title"),
                   m_sidebarAction.value(
                       QStringLiteral("title")).toString());
        return okReply(out);
    }
    if (method == QLatin1String("setIcon")) {
        // No icon surface exists — accepted for bookkeeping only,
        // the note makes the unrendered state visible.
        m_sidebarAction.insert(
            QStringLiteral("icon"),
            args.value(QStringLiteral("icon")));
        recordNote(QStringLiteral(
            "sidebarAction.setIcon: recorded; Arktos has no sidebar "
            "surface to render it"));
        return okReply();
    }
    if (method == QLatin1String("isOpen"))
        return okReply(false);
    if (method == QLatin1String("open")
            || method == QLatin1String("close")
            || method == QLatin1String("toggle"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.runtime extras — context bookkeeping.  getViews can't move
// Window objects across the channel; it answers context counts and
// the shim composes the return array in-page.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeRuntime(const QString &method,
                                                const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method == QLatin1String("getViews")) {
        QJsonObject counts;
        counts.insert(QStringLiteral("background"), 1);
        counts.insert(QStringLiteral("popup"), 0);
        counts.insert(QStringLiteral("tab"), 0);
        return okReply(counts);
    }
    if (method == QLatin1String("getManifest"))
        return okReply(manifestJson());
    // EXT17: the shim's merged getContexts — synthetic entries for
    // every registered context of THIS extension (the hosted
    // background, open popups, extension pages in tabs).
    if (method == QLatin1String("getContexts"))
        return okReply(ExtensionBackgroundRuntime::instance()
                           ->contextsJson(m_extensionId));
    if (method == QLatin1String("openOptionsPage")) {
        // options_ui.page (or the legacy options_page) opens in a
        // normal tab — the EXT17 page injection gives it the shim
        // there like any extension document.
        const QJsonObject manifest = manifestJson();
        QString page = manifest.value(QStringLiteral("options_ui"))
            .toObject().value(QStringLiteral("page")).toString();
        if (page.isEmpty())
            page = manifest.value(QStringLiteral("options_page"))
                .toString();
        if (page.isEmpty() || page.startsWith(QLatin1Char('/'))
                || page.contains(QLatin1Char(':'))
                || page.split(QLatin1Char('/')).contains(
                    QStringLiteral("..")))
            return errReply(QStringLiteral("no-options-page"));
        const QJsonObject open = invokeTabs(
            QStringLiteral("create"),
            QJsonObject{{QStringLiteral("props"),
                         QJsonObject{{QStringLiteral("url"),
                                      QStringLiteral(
                                          "chrome-extension://")
                                          + m_extensionId
                                          + QLatin1Char('/') + page},
                                     {QStringLiteral("active"),
                                      true}}}});
        if (!open.value(QStringLiteral("ok")).toBool())
            return open;
        return okReply();
    }
    if (method == QLatin1String("platformInfo")) {
        // chrome.runtime.getPlatformInfo shape — real values from
        // QSysInfo so extensions feature-detecting the platform get
        // an honest answer.
        QJsonObject info;
        const QString product = QSysInfo::productType();
        info.insert(QStringLiteral("os"),
                    product.startsWith(QLatin1String("win"))
                        ? QStringLiteral("win")
                    : product.startsWith(QLatin1String("mac"))
                          || product.startsWith(QLatin1String("osx"))
                        ? QStringLiteral("mac")
                    : product.contains(QLatin1String("android"))
                        ? QStringLiteral("android")
                    : product.contains(QLatin1String("bsd"))
                          || product.startsWith(QLatin1String("ios"))
                        ? QStringLiteral("openbsd")
                    : QStringLiteral("linux"));
        const QString cpu = QSysInfo::currentCpuArchitecture();
        info.insert(QStringLiteral("arch"),
                    cpu.contains(QLatin1String("64"))
                        ? (cpu.startsWith(QLatin1String("arm"))
                               ? QStringLiteral("arm64")
                               : QStringLiteral("x86-64"))
                    : cpu.startsWith(QLatin1String("arm"))
                        ? QStringLiteral("arm")
                        : QStringLiteral("x86-32"));
        info.insert(QStringLiteral("nacl_arch"),
                    info.value(QStringLiteral("arch")).toString());
        return okReply(info);
    }
    if (method == QLatin1String("reload"))
        return errReply(QStringLiteral("unsupported"));
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// shim.* — diagnostics channel: benign notes vs health-relevant gaps.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeShim(const QString &method,
                                             const QJsonObject &args)
{
    if (method == QLatin1String("note")) {
        const QString message =
            args.value(QStringLiteral("message")).toString().left(2048);
        if (m_notes.size() < 500)
            m_notes.append(message);
        emit noteRecorded(message);
        return okReply();
    }
    if (method == QLatin1String("gap")) {
        const QString api =
            args.value(QStringLiteral("api")).toString().left(512);
        const QString error =
            args.value(QStringLiteral("error")).toString().left(512);
        const QString entry = api + QLatin1String(": ") + error;
        if (!m_gaps.contains(entry) && m_gaps.size() < 500) {
            m_gaps.append(entry);
            emit gapReported(api, error);
        }
        return okReply();
    }
    return errReply(QStringLiteral("unimplemented"));
}
