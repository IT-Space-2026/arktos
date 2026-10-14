/*
 * Copyright 2026 The Arora Authors
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
#include "tabwidget.h"
#include "webview.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qlocale.h>
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

// Session-stable id minting for tab/window objects — a dynamic
// property on the object itself, so a destroyed object takes its id
// with it and a recycled address can never inherit it.
int idFor(const QObject *object)
{
    if (!object)
        return -1;
    static int next = 1;
    const QVariant v = object->property("aroraExtObjectId");
    if (v.isValid() && v.toInt() > 0)
        return v.toInt();
    const int id = next++;
    const_cast<QObject *>(object)->setProperty("aroraExtObjectId", id);
    return id;
}

// The window a chrome.tabs call targets for currentWindow /
// lastFocusedWindow — the active window, else the most recently
// created one.
BrowserMainWindow *currentWindow()
{
    BrowserApplication *app = BrowserApplication::instance();
    const QList<BrowserMainWindow *> windows = app->mainWindows();
    for (BrowserMainWindow *window : windows) {
        if (window && window->isActiveWindow())
            return window;
    }
    return windows.isEmpty() ? nullptr : windows.last();
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
}

ExtensionShimBackend::~ExtensionShimBackend()
{
    for (Alarm &alarm : m_alarms) {
        if (alarm.timer)
            alarm.timer->stop();
    }
    if (m_localDirty)
        flushLocalStore();
}

ExtensionShimBackend::ActionState
ExtensionShimBackend::actionState(const QString &extensionId)
{
    return actionStates().value(extensionId);
}

QJsonObject ExtensionShimBackend::actionStateJson(const QString &extensionId)
{
    const ActionState state = actionStates().value(extensionId);
    QJsonObject o;
    o.insert(QStringLiteral("title"), state.title);
    o.insert(QStringLiteral("badgeText"), state.badgeText);
    o.insert(QStringLiteral("badgeColor"), state.badgeColor);
    o.insert(QStringLiteral("badgeTextColor"), state.badgeTextColor);
    o.insert(QStringLiteral("popup"), state.popup);
    o.insert(QStringLiteral("enabled"), state.enabled);
    return o;
}

int ExtensionShimBackend::tabIdFor(const QObject *view)
{
    return idFor(view);
}

int ExtensionShimBackend::windowIdFor(const QObject *window)
{
    return idFor(window);
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
                                            int windowId) const
{
    QJsonObject tab;
    tab.insert(QStringLiteral("id"), idFor(view));
    tab.insert(QStringLiteral("index"), index);
    tab.insert(QStringLiteral("windowId"), windowId);
    QWebEnginePage *page = view ? view->page() : nullptr;
    const bool active = view && view->tabWidget()
        && view->tabWidget()->currentWebView() == view;
    tab.insert(QStringLiteral("active"), active);
    tab.insert(QStringLiteral("highlighted"), active);
    tab.insert(QStringLiteral("selected"), active);
    tab.insert(QStringLiteral("pinned"), false);
    tab.insert(QStringLiteral("audible"), false);
    tab.insert(QStringLiteral("discarded"), false);
    tab.insert(QStringLiteral("autoDiscardable"), true);
    tab.insert(QStringLiteral("groupId"), -1);
    tab.insert(QStringLiteral("incognito"),
               page && page->profile()
                   && page->profile()->isOffTheRecord());
    QJsonObject muted;
    muted.insert(QStringLiteral("muted"), false);
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

QJsonObject ExtensionShimBackend::invokeTabs(const QString &method,
                                             const QJsonObject &args)
{
    BrowserApplication *app = BrowserApplication::instance();
    const QList<BrowserMainWindow *> windows = app->mainWindows();

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
            if (q.contains(QStringLiteral("windowId"))
                    && int(q.value(QStringLiteral("windowId"))
                               .toDouble(-2)) != wid)
                continue;
            if (q.value(QStringLiteral("currentWindow")).toBool()
                    && wid != currentWid)
                continue;
            if (q.value(QStringLiteral("lastFocusedWindow")).toBool()
                    && wid != currentWid)
                continue;
            TabWidget *tw = window->tabWidget();
            if (!tw)
                continue;
            for (int i = 0; i < tw->count(); ++i) {
                WebView *view = tw->webView(i);
                if (!view)
                    continue;   // engine/widget tabs have no WebView
                QJsonObject tab = tabObject(view, i, wid);
                if (q.contains(QStringLiteral("active"))
                        && q.value(QStringLiteral("active")).toBool()
                               != tab.value(QStringLiteral("active"))
                                   .toBool())
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
                out.append(tab);
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
                if (view && idFor(view) == wanted)
                    return okReply(tabObject(view, i, idFor(window)));
            }
        }
        return errReply(QStringLiteral("not-found"));
    }
    if (method == QLatin1String("create")) {
        const QJsonObject props =
            args.value(QStringLiteral("props")).toObject();
        BrowserMainWindow *window = currentWindow();
        if (props.contains(QStringLiteral("windowId"))) {
            const int wanted = int(
                props.value(QStringLiteral("windowId")).toDouble(-2));
            for (BrowserMainWindow *w : windows) {
                if (w && idFor(w) == wanted) {
                    window = w;
                    break;
                }
            }
        }
        if (!window || !window->tabWidget())
            return errReply(QStringLiteral("no-window"));
        const QUrl url(props.value(QStringLiteral("url")).toString());
        // The untrusted-input gate — an extension may only ask for
        // navigable urls, never javascript:/data: payload execution.
        if (!url.isEmpty()
                && !WebView::isUrlAllowedOnUntrustedInput(url))
            return errReply(QStringLiteral("url-refused"));
        const bool active = !props.contains(QStringLiteral("active"))
            || props.value(QStringLiteral("active")).toBool();
        WebView *view = window->tabWidget()->makeNewTab(active);
        if (!view)
            return errReply(QStringLiteral("no-tab"));
        if (!url.isEmpty())
            view->loadUrl(url);
        const int index =
            window->tabWidget()->webViewIndex(view);
        return okReply(tabObject(view, index, idFor(window)));
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
                if (!view)
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
            const QUrl url(
                props.value(QStringLiteral("url")).toString());
            if (!url.isEmpty()
                    && !WebView::isUrlAllowedOnUntrustedInput(url))
                return errReply(QStringLiteral("url-refused"));
            if (!url.isEmpty())
                target->loadUrl(url);
        }
        if (props.value(QStringLiteral("active")).toBool())
            targetWindow->tabWidget()->setCurrentIndex(targetIndex);
        return okReply(tabObject(target, targetIndex,
                                 idFor(targetWindow)));
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
        } else if (key == QLatin1String("popup"))
            state.popup = args.value(QStringLiteral("value")).toString();
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

QJsonObject ExtensionShimBackend::invokeI18n(const QString &method,
                                             const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method != QLatin1String("messages"))
        return errReply(QStringLiteral("unimplemented"));
    if (m_extensionPath.isEmpty())
        return errReply(QStringLiteral("no-path"));

    // Locale pick order: UI languages, then the manifest's declared
    // default, then 'en'.
    QStringList langs = QLocale().uiLanguages();
    const QString deflt = manifestJson()
        .value(QStringLiteral("default_locale")).toString();
    if (!deflt.isEmpty())
        langs.append(deflt);
    langs.append(QStringLiteral("en"));

    for (const QString &lang : langs) {
        QString dir = QLocale(lang).name();
        QFile file(m_extensionPath + QLatin1String("/_locales/")
                   + dir + QLatin1String("/messages.json"));
        if (!file.exists()) {
            // Chrome also accepts the short tag (_locales/en).
            const QString shortTag = dir.section(QLatin1Char('_'), 0, 0);
            file.setFileName(m_extensionPath
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
        return okReply(flat);
    }
    return okReply(QJsonObject());
}

// -------------------------------------------------------------------
// chrome.permissions — manifest-declared grants (the JS side answers
// from the handshake manifest; this is the bridge fallback).
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokePermissions(
        const QString &method, const QJsonObject &args)
{
    const QJsonObject manifest = manifestJson();
    QStringList perms;
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("permissions")).toArray())
        perms.append(v.toString());
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("optional_permissions")).toArray())
        perms.append(v.toString());
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
// chrome.contextMenus — per-extension item registry; the page-menu
// contribution point lands with EXT16.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeContextMenus(
        const QString &method, const QJsonObject &args)
{
    if (method == QLatin1String("create")) {
        QJsonObject props = args.value(QStringLiteral("props")).toObject();
        QString id = props.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            id = QStringLiteral("item-%1").arg(m_menuItems.size() + 1);
        props.insert(QStringLiteral("id"), id);
        m_menuItems.insert(id, props);
        return okReply(QJsonObject{{QStringLiteral("id"), id}});
    }
    if (method == QLatin1String("update")) {
        const QString id = args.value(QStringLiteral("id")).toString();
        if (!m_menuItems.contains(id))
            return errReply(QStringLiteral("not-found"));
        QJsonObject props = m_menuItems.value(id);
        const QJsonObject patch =
            args.value(QStringLiteral("props")).toObject();
        for (auto it = patch.constBegin(); it != patch.constEnd(); ++it)
            props.insert(it.key(), it.value());
        m_menuItems.insert(id, props);
        return okReply();
    }
    if (method == QLatin1String("remove")) {
        const QString id = args.value(QStringLiteral("id")).toString();
        if (!m_menuItems.remove(id))
            return errReply(QStringLiteral("not-found"));
        return okReply();
    }
    if (method == QLatin1String("removeAll")) {
        m_menuItems.clear();
        return okReply();
    }
    return errReply(QStringLiteral("unimplemented"));
}

// -------------------------------------------------------------------
// chrome.sidePanel — Arora has no side-panel surface; options are
// recorded honestly so EXT17/later work can render them.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeSidePanel(
        const QString &method, const QJsonObject &args)
{
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

// -------------------------------------------------------------------
// chrome.notifications — no notification presenter exists in Arora;
// mutations answer a typed 'unsupported' (the shim records the gap),
// getAll answers the honest empty set.
// -------------------------------------------------------------------

QJsonObject ExtensionShimBackend::invokeNotifications(
        const QString &method, const QJsonObject &args)
{
    Q_UNUSED(args);
    if (method == QLatin1String("getAll"))
        return okReply(QJsonObject());
    return errReply(QStringLiteral("unsupported"));
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
