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

#include "extensioneventhub.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browserprofile.h"
#include "engine/engineinterface.h"
#include "extensionbackgroundhost.h"
#include "extensionshimbackend.h"
#include "tabwidget.h"
#include "webpage.h"
#include "webview.h"

#include <qaction.h>
#include <qapplication.h>
#include <qdatetime.h>
#include <qjsonarray.h>
#include <qmenu.h>
#include <qtimer.h>
#include <qurl.h>
#include <qwebengineloadinginfo.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

namespace {

// The rescan cadence for changes no signal covers — only window
// creation really needs it (tab adds/removes/activations ride
// tabsChanged/currentChanged/destroyed), so it can stay slow.
constexpr int kScanMs = 2500;

// Multi-arg envelope — the shim fires ev.__fire(payload.__args).
QJsonObject argsPayload(std::initializer_list<QJsonValue> args)
{
    QJsonArray a;
    for (const QJsonValue &v : args)
        a.append(v);
    return QJsonObject{{QStringLiteral("__args"), a}};
}

// Chromium net error codes -> the "net::ERR_*" strings extensions
// match on.  Unknown/positive codes degrade to ERR_FAILED with the
// engine's own errorCode carried alongside for debugging.
QString netErrorName(int code)
{
    switch (code) {
    case -2:   return QStringLiteral("ERR_FAILED");
    case -3:   return QStringLiteral("ERR_ABORTED");
    case -6:   return QStringLiteral("ERR_FILE_NOT_FOUND");
    case -7:   return QStringLiteral("ERR_TIMED_OUT");
    case -100: return QStringLiteral("ERR_CONNECTION_CLOSED");
    case -101: return QStringLiteral("ERR_CONNECTION_RESET");
    case -102: return QStringLiteral("ERR_CONNECTION_REFUSED");
    case -103: return QStringLiteral("ERR_CONNECTION_ABORTED");
    case -104: return QStringLiteral("ERR_CONNECTION_FAILED");
    case -105: return QStringLiteral("ERR_NAME_NOT_RESOLVED");
    case -106: return QStringLiteral("ERR_INTERNET_DISCONNECTED");
    case -117: return QStringLiteral("ERR_CONNECTION_TIMED_OUT");
    case -118: return QStringLiteral("ERR_CONNECTION_TIMED_OUT");
    case -130: return QStringLiteral("ERR_PROXY_CONNECTION_FAILED");
    case -137: return QStringLiteral("ERR_NAME_RESOLUTION_FAILED");
    case -138: return QStringLiteral("ERR_NETWORK_ACCESS_DENIED");
    case -139: return QStringLiteral("ERR_NETWORK_CHANGED");
    case -199: return QStringLiteral("ERR_CERT_COMMON_NAME_INVALID");
    case -200: return QStringLiteral("ERR_CERT_DATE_INVALID");
    case -300: return QStringLiteral("ERR_INVALID_URL");
    case -324: return QStringLiteral("ERR_EMPTY_RESPONSE");
    default:   return QStringLiteral("ERR_FAILED");
    }
}

} // namespace

ExtensionEventHub::ExtensionEventHub(QObject *parent)
    : QObject(parent)
    , m_scan(new QTimer(this))
{
    m_scan->setInterval(kScanMs);
    connect(m_scan, &QTimer::timeout, this,
            &ExtensionEventHub::syncNow);
    // New-window discovery without waiting for the scan: a freshly
    // shown window claims focus, and focus changes are themselves a
    // windows.onFocusChanged trigger.
    connect(qApp, &QGuiApplication::focusWindowChanged, this,
            [this](QWindow *) {
        if (m_tracking)
            syncNow();
    });
}

ExtensionEventHub *ExtensionEventHub::instance()
{
    static ExtensionEventHub *hub = nullptr;
    if (!hub)
        hub = new ExtensionEventHub(qApp);
    return hub;
}

void ExtensionEventHub::setTracking(bool on)
{
    if (m_tracking == on)
        return;
    m_tracking = on;
    if (on) {
        m_firstScanDone = false;
        qApp->installEventFilter(this);
        m_scan->start();
        syncNow();
    } else {
        qApp->removeEventFilter(this);
        m_scan->stop();
        m_tabs.clear();
        m_wiredWindows.clear();
        m_windowIds.clear();
        m_dyingWindows.clear();
        m_focusWindowId = -2;
    }
}

QList<BrowserMainWindow *> ExtensionEventHub::browsingWindows()
{
    QList<BrowserMainWindow *> out;
    const QList<QWidget *> tops = QApplication::topLevelWidgets();
    for (QWidget *top : tops) {
        if (BrowserMainWindow *window =
                qobject_cast<BrowserMainWindow *>(top))
            out.append(window);
    }
    if (out.isEmpty()) {
        // Pre-first-window startup (or a hidden test harness) — the
        // app registry is the only enumeration left.
        if (BrowserApplication *app = BrowserApplication::instance())
            out = app->mainWindows();
    }
    return out;
}

// A tab is trackable when its page lives on a real browsing profile —
// off-the-record (private tab, private window) and the tor profile
// are invisible to extensions by design.
bool ExtensionEventHub::trackable(const WebView *view)
{
    if (!view)
        return false;
    QWebEngineProfile *profile =
        view->page() ? view->page()->profile() : nullptr;
    if (!profile || profile->isOffTheRecord())
        return false;
    if (profile == BrowserProfile::torProfileIfCreated())
        return false;
    return true;
}

QJsonObject ExtensionEventHub::tabJson(WebView *view)
{
    if (!view)
        return QJsonObject();
    TabWidget *tw = view->tabWidget();
    BrowserMainWindow *window =
        BrowserMainWindow::parentWindow(view);
    const int index = tw ? tw->webViewIndex(view) : -1;
    return ExtensionShimBackend::tabObject(
        view, index, ExtensionShimBackend::windowIdFor(window));
}

QJsonObject ExtensionEventHub::windowJson(BrowserMainWindow *window,
                                        bool focused)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"),
             ExtensionShimBackend::windowIdFor(window));
    o.insert(QStringLiteral("focused"), focused);
    o.insert(QStringLiteral("alwaysOnTop"), false);
    o.insert(QStringLiteral("incognito"), false);
    o.insert(QStringLiteral("type"), QStringLiteral("normal"));
    o.insert(QStringLiteral("state"), QStringLiteral("normal"));
    return o;
}

void ExtensionEventHub::emitTabCreated(WebView *view)
{
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        QStringLiteral("tabs.onCreated"),
        argsPayload({tabJson(view)}));
}

void ExtensionEventHub::emitTabRemoved(int tabId, int windowId,
                                       bool isWindowClosing)
{
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        QStringLiteral("tabs.onRemoved"),
        argsPayload({
            tabId,
            QJsonObject{
                {QStringLiteral("windowId"), windowId},
                {QStringLiteral("isWindowClosing"), isWindowClosing}}}));
}

void ExtensionEventHub::emitTabUpdated(WebView *view,
                                       const QJsonObject &changeInfo)
{
    if (!m_tabs.contains(view))
        return;
    ExtensionBackgroundRuntime::instance()->broadcastEvent(
        QStringLiteral("tabs.onUpdated"),
        argsPayload({ExtensionShimBackend::tabIdFor(view),
                     changeInfo, tabJson(view)}));
}

void ExtensionEventHub::emitNavEvent(WebView *view,
                                     const QString &name,
                                     const QUrl &url,
                                     const QJsonObject &extra)
{
    if (!m_tabs.contains(view))
        return;
    QJsonObject details;
    details.insert(QStringLiteral("tabId"),
                   ExtensionShimBackend::tabIdFor(view));
    details.insert(QStringLiteral("url"), url.toString());
    // frameId 0 / parentFrameId -1 = the main frame — the only one
    // the page-level signals describe.  Sub-frame navigation detail
    // stays an honest gap (no per-frame Qt signal).
    details.insert(QStringLiteral("frameId"), 0);
    details.insert(QStringLiteral("parentFrameId"), -1);
    details.insert(QStringLiteral("timeStamp"),
                   double(QDateTime::currentMSecsSinceEpoch()));
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        details.insert(it.key(), it.value());
    ExtensionBackgroundRuntime::instance()->broadcastEvent(name,
                                                           details);
}

bool ExtensionEventHub::eventFilter(QObject *watched, QEvent *event)
{
    if (m_tracking
            && (event->type() == QEvent::Show
                || event->type() == QEvent::WindowActivate)
            && qobject_cast<BrowserMainWindow *>(watched))
        syncNow();
    return QObject::eventFilter(watched, event);
}

void ExtensionEventHub::wireWindow(BrowserMainWindow *window)
{
    if (m_wiredWindows.contains(window))
        return;
    m_wiredWindows.insert(window);
    TabWidget *tw = window->tabWidget();
    if (tw) {
        connect(tw, &TabWidget::tabsChanged, this,
                [this]() { syncNow(); });
        connect(tw, &QTabWidget::currentChanged, this,
                [this, tw](int index) { onTabActivated(tw, index); });
    }
    // The destroyed() signal runs while the object is already sliced
    // to QObject — a slot parameter cast would be UB, so the typed
    // pointer is captured at wire time and never dereferenced after.
    connect(window, &QObject::destroyed, this,
            [this, window]() { onWindowDestroyed(window); });
}

void ExtensionEventHub::wireView(WebView *view)
{
    WebPage *webPage = view->webPage();
    QWebEnginePage *page = view->page();
    if (webPage) {
        connect(webPage, &WebPage::aboutToLoadUrl, this,
                [this, view](const QUrl &url) {
            emitNavEvent(view,
                         QStringLiteral("webNavigation.onBeforeNavigate"),
                         url);
        });
    }
    if (!page)
        return;
    connect(page, &QWebEnginePage::loadStarted, this,
            [this, view]() {
        auto it = m_tabs.find(view);
        if (it == m_tabs.end())
            return;
        it->loading = true;
        it->lastError.clear();
        it->lastUrl = view->url().toString();
        emitNavEvent(view,
                     QStringLiteral("webNavigation.onCommitted"),
                     view->url(),
                     QJsonObject{
                         {QStringLiteral("transitionType"),
                          QStringLiteral("link")},
                         {QStringLiteral("transitionQualifiers"),
                          QJsonArray()},
                         {QStringLiteral("processId"), -1}});
        emitTabUpdated(view,
                       QJsonObject{
                           {QStringLiteral("status"),
                            QStringLiteral("loading")},
                           {QStringLiteral("url"),
                            view->url().toString()}});
    });
    connect(page, &QWebEnginePage::loadFinished, this,
            [this, view](bool ok) {
        auto it = m_tabs.find(view);
        if (it == m_tabs.end())
            return;
        it->loading = false;
        if (ok) {
            emitNavEvent(view,
                         QStringLiteral("webNavigation.onCompleted"),
                         view->url());
        } else {
            QJsonObject extra{
                {QStringLiteral("error"),
                 QStringLiteral("net::")
                     + netErrorName(it->lastErrorCode)},
                {QStringLiteral("errorCode"), it->lastErrorCode}};
            if (!it->lastError.isEmpty())
                extra.insert(QStringLiteral("errorDetail"),
                             it->lastError);
            emitNavEvent(view,
                         QStringLiteral("webNavigation.onErrorOccurred"),
                         view->url(), extra);
        }
        emitTabUpdated(view,
                       QJsonObject{
                           {QStringLiteral("status"),
                            QStringLiteral("complete")}});
    });
    // The failure detail rides loadingChanged, not loadFinished —
    // stash it so onErrorOccurred can answer a real net:: name.
    connect(page, &QWebEnginePage::loadingChanged, this,
            [this, view](const QWebEngineLoadingInfo &info) {
        auto it = m_tabs.find(view);
        if (it == m_tabs.end())
            return;
        if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus) {
            it->lastError = info.errorString();
            it->lastErrorCode = info.errorCode();
            it->lastErrorDomain = int(info.errorDomain());
        }
    });
    connect(page, &QWebEnginePage::urlChanged, this,
            [this, view](const QUrl &url) {
        auto it = m_tabs.find(view);
        if (it == m_tabs.end())
            return;
        emitTabUpdated(view,
                       QJsonObject{{QStringLiteral("url"),
                                    url.toString()}});
        // Same-document commits never produce loadStarted — Chrome
        // names them onReferenceFragmentUpdated (fragment-only) or
        // onHistoryStateUpdated (pushState/replaceState) instead.
        if (!it->loading) {
            const QString next = url.toString();
            const bool fragmentOnly =
                url.adjusted(QUrl::RemoveFragment)
                    == QUrl(it->lastUrl)
                           .adjusted(QUrl::RemoveFragment);
            emitNavEvent(view,
                         QString::fromLatin1(fragmentOnly
                             ? "webNavigation.onReferenceFragmentUpdated"
                             : "webNavigation.onHistoryStateUpdated"),
                         url,
                         QJsonObject{{QStringLiteral("transitionType"),
                                      QStringLiteral("link")}});
            it->lastUrl = next;
        }
    });
    connect(page, &QWebEnginePage::titleChanged, this,
            [this, view](const QString &title) {
        emitTabUpdated(view,
                       QJsonObject{{QStringLiteral("title"), title}});
    });
    connect(view, &QObject::destroyed, this,
            [this, view]() { onViewDestroyed(view); });
}

void ExtensionEventHub::onTabActivated(TabWidget *tabs, int index)
{
    if (!m_tracking || !tabs)
        return;
    WebView *view = tabs->webView(index);
    if (!view || !m_tabs.contains(view))
        return;
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    const int windowId = ExtensionShimBackend::windowIdFor(
        BrowserMainWindow::parentWindow(view));
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    runtime->broadcastEvent(
        QStringLiteral("tabs.onActivated"),
        argsPayload({tabId, windowId}));
    runtime->broadcastEvent(
        QStringLiteral("tabs.onHighlighted"),
        argsPayload({QJsonObject{
            {QStringLiteral("windowId"), windowId},
            {QStringLiteral("tabIds"), QJsonArray{tabId}}}}));
}

void ExtensionEventHub::onViewDestroyed(WebView *view)
{
    // 'view' is mid-destruction — use it ONLY as a map key; every
    // emitted id was minted while the object was alive.
    const auto it = m_tabs.find(view);
    if (it == m_tabs.end())
        return;
    const TrackedTab tab = it.value();
    m_tabs.erase(it);
    // The window is already dying (or itself gone) when the tab's
    // removal is window teardown rather than a tab close.  A dead
    // window is already out of m_windowIds — identity keys only, the
    // QPointer<BMW> casts would be UB on the mid-destruction object.
    const bool closing =
        m_dyingWindows.contains(tab.windowObject)
        || !m_windowIds.contains(tab.windowObject);
    // The recorded windowId — the live map may already have dropped
    // the dying window.
    emitTabRemoved(tab.tabId, tab.windowId, closing);
}

void ExtensionEventHub::onWindowDestroyed(BrowserMainWindow *window)
{
    m_dyingWindows.insert(window);
    // The children die next; their destroyed() handlers answer
    // isWindowClosing:true, then the rescan records the window's
    // own onRemoved.
    syncNow();
    m_dyingWindows.remove(window);
    if (m_windowIds.contains(window)) {
        const int windowId = m_windowIds.take(window);
        ExtensionBackgroundRuntime::instance()->broadcastEvent(
            QStringLiteral("windows.onRemoved"),
            argsPayload({windowId}));
    }
    m_wiredWindows.remove(window);
}

void ExtensionEventHub::syncNow()
{
    if (!m_tracking)
        return;
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();

    // Every top-level BrowserMainWindow — untrackable-tab windows
    // (all-OTR, tor) drop out naturally below.  QObject-identity set:
    // dying-window compares must never cast the pointer.
    QSet<QObject *> liveWindows;
    QSet<WebView *> seen;
    for (BrowserMainWindow *window : browsingWindows()) {
        if (!window)
            continue;
        if (m_dyingWindows.contains(window))
            continue;   // mid-destruction — no member calls on it
        wireWindow(window);
        TabWidget *tw = window->tabWidget();
        if (!tw)
            continue;
        bool anyTrackable = false;
        for (int i = 0; i < tw->count(); ++i) {
            WebView *view = tw->webView(i);
            if (!trackable(view))
                continue;   // widget tabs, OTR, tor
            anyTrackable = true;
            seen.insert(view);
            const bool isNew = !m_tabs.contains(view);
            TrackedTab &tab = m_tabs[view];
            if (isNew) {
                tab.view = view;
                tab.window = window;
                tab.windowObject = window;
                tab.tabId = ExtensionShimBackend::tabIdFor(view);
                tab.windowId =
                    ExtensionShimBackend::windowIdFor(window);
                tab.index = i;
                wireView(view);
                // Tabs the extension never saw get no onCreated —
                // Chrome fires it only for tabs born after the
                // listener registered.  The first scan seeds the
                // baseline silently.
                if (m_firstScanDone)
                    emitTabCreated(view);
            } else {
                if (tab.windowObject != window) {
                    // A tear-between-windows: the WebView identity
                    // survives, Chrome detaches and re-attaches.
                    if (m_firstScanDone) {
                        runtime->broadcastEvent(
                            QStringLiteral("tabs.onDetached"),
                            argsPayload({
                                ExtensionShimBackend::tabIdFor(view),
                                QJsonObject{
                                    {QStringLiteral("oldWindowId"),
                                     m_windowIds.value(
                                         tab.windowObject, -1)},
                                    {QStringLiteral("oldPosition"),
                                     tab.index}}}));
                    }
                    tab.window = window;
                    tab.windowObject = window;
                    tab.windowId =
                        ExtensionShimBackend::windowIdFor(window);
                    if (m_firstScanDone) {
                        runtime->broadcastEvent(
                            QStringLiteral("tabs.onAttached"),
                            argsPayload({
                                ExtensionShimBackend::tabIdFor(view),
                                QJsonObject{
                                    {QStringLiteral("newWindowId"),
                                     ExtensionShimBackend::
                                         windowIdFor(window)},
                                    {QStringLiteral("newPosition"),
                                     i}}}));
                    }
                }
                if (tab.index != i && m_firstScanDone) {
                    runtime->broadcastEvent(
                        QStringLiteral("tabs.onMoved"),
                        argsPayload({
                            ExtensionShimBackend::tabIdFor(view),
                            QJsonObject{
                                {QStringLiteral("windowId"),
                                 ExtensionShimBackend::
                                     windowIdFor(window)},
                                {QStringLiteral("fromIndex"),
                                 tab.index},
                                {QStringLiteral("toIndex"), i}}}));
                }
                tab.index = i;
            }
        }
        if (!anyTrackable)
            continue;
        liveWindows.insert(window);
        if (!m_windowIds.contains(window)) {
            m_windowIds.insert(window,
                               ExtensionShimBackend::windowIdFor(window));
            if (m_firstScanDone) {
                runtime->broadcastEvent(
                    QStringLiteral("windows.onCreated"),
                    windowJson(window, window->isActiveWindow()));
            }
        }
    }

    // Removed tabs — view dead or no longer in a widget slot.
    for (auto it = m_tabs.begin(); it != m_tabs.end();) {
        WebView *view = it.key();
        TrackedTab tab = it.value();
        if (!tab.view || !seen.contains(view)) {
            it = m_tabs.erase(it);
            const bool closing =
                m_dyingWindows.contains(tab.windowObject)
                || !liveWindows.contains(tab.windowObject);
            emitTabRemoved(tab.tabId, tab.windowId, closing);
        } else {
            ++it;
        }
    }

    // Removed windows — gone from the top-level set entirely.
    for (auto it = m_windowIds.begin(); it != m_windowIds.end();) {
        QObject *window = it.key();
        if (!liveWindows.contains(window)) {
            const int windowId = it.value();
            it = m_windowIds.erase(it);
            runtime->broadcastEvent(
                QStringLiteral("windows.onRemoved"),
                argsPayload({windowId}));
        } else {
            ++it;
        }
    }

    // Focus — the active top-level or WINDOW_ID_NONE (-1).  Iterate
    // the registry (live typed windows) rather than liveWindows: the
    // identity set holds QObject* keys for teardown compares.
    int focusedId = -1;
    for (BrowserMainWindow *window : browsingWindows()) {
        if (!window || m_dyingWindows.contains(window))
            continue;
        if (window->isActiveWindow()) {
            focusedId = m_windowIds.value(window, -1);
            break;
        }
    }
    if (m_firstScanDone && focusedId != m_focusWindowId) {
        runtime->broadcastEvent(
            QStringLiteral("windows.onFocusChanged"),
            argsPayload({focusedId}));
    }
    m_focusWindowId = focusedId;
    m_firstScanDone = true;
}

// -------------------------------------------------------------------
// chrome.contextMenus — the EXT15 registry rendered into the real
// page context menu.  Item property subset: id, parentId, title,
// type normal|checkbox|radio|separator, checked, enabled, visible,
// contexts (all/page/link/selection/image/video/audio/editable —
// 'page' applies only when nothing more specific does).
// documentUrlPatterns/targetUrlPatterns are unhonored — a recorded
// gap, not a silent drop, would need the registration path to flag
// them; they are ignored here the same way the registry ignores
// them.
// -------------------------------------------------------------------

void ExtensionEventHub::contributeContextMenu(
        QMenu *menu, const Engine::ContextMenuInfo &info,
        WebView *view)
{
    const QList<ExtensionShimBackend::ContextMenuEntry> entries =
        ExtensionShimBackend::contextMenuEntries();
    if (entries.isEmpty() || !menu)
        return;

    // The click's active contexts — 'page' only when nothing more
    // specific applies (Chrome semantics).
    QSet<QString> active;
    if (info.isContentEditable)
        active.insert(QStringLiteral("editable"));
    if (!info.linkUrl.isEmpty())
        active.insert(QStringLiteral("link"));
    if (info.hasImage)
        active.insert(QStringLiteral("image"));
    if (info.hasVideo)
        active.insert(QStringLiteral("video"));
    else if (info.hasMedia)
        active.insert(QStringLiteral("audio"));
    if (!info.selectedText.isEmpty())
        active.insert(QStringLiteral("selection"));
    const bool plainPage = active.isEmpty();
    if (plainPage)
        active.insert(QStringLiteral("page"));

    const auto itemVisible = [&active](const QJsonObject &props) {
        if (!props.value(QStringLiteral("visible")).toBool(true))
            return false;
        const QJsonArray contexts =
            props.value(QStringLiteral("contexts")).toArray();
        if (contexts.isEmpty())
            return active.contains(QStringLiteral("page"));
        for (const QJsonValue &c : contexts) {
            const QString ctx = c.toString();
            if (ctx == QLatin1String("all")
                    || active.contains(ctx))
                return true;
        }
        return false;
    };

    // Group by extension, preserving registration order.
    QStringList order;
    QHash<QString, QList<ExtensionShimBackend::ContextMenuEntry>>
        byExtension;
    for (const ExtensionShimBackend::ContextMenuEntry &entry
         : entries) {
        if (!byExtension.contains(entry.extensionId))
            order.append(entry.extensionId);
        byExtension[entry.extensionId].append(entry);
    }

    bool contributed = false;
    for (const QString &extensionId : order) {
        const auto &items = byExtension.value(extensionId);
        QList<ExtensionShimBackend::ContextMenuEntry> visible;
        for (const auto &entry : items) {
            if (itemVisible(entry.props))
                visible.append(entry);
        }
        if (visible.isEmpty())
            continue;
        if (!contributed) {
            menu->addSeparator();
            contributed = true;
        }
        const QString name =
            byExtension.value(extensionId).first().extensionName;
        QMenu *sub = menu->addMenu(
            name.isEmpty() ? QStringLiteral("Extension") : name);
        // Parents materialize as nested menus; children route under
        // their parent's submenu.  One pass, ids may forward-
        // reference so parents are built lazily.
        QHash<QString, QMenu *> parents;
        const auto menuFor = [&](const QJsonObject &props) -> QMenu * {
            const QString parentId =
                props.value(QStringLiteral("parentId")).toString();
            if (parentId.isEmpty())
                return sub;
            if (parents.contains(parentId))
                return parents.value(parentId);
            QString title = parentId;
            for (const auto &entry : items) {
                if (entry.itemId == parentId) {
                    title = entry.props.value(QStringLiteral("title"))
                        .toString(parentId);
                    break;
                }
            }
            QMenu *created = sub->addMenu(title);
            parents.insert(parentId, created);
            return created;
        };
        for (const auto &entry : visible) {
            const QJsonObject &props = entry.props;
            const QString type =
                props.value(QStringLiteral("type")).toString();
            QMenu *target = menuFor(props);
            if (type == QLatin1String("separator")) {
                target->addSeparator();
                continue;
            }
            QAction *action = target->addAction(
                props.value(QStringLiteral("title")).toString());
            const bool checkable =
                type == QLatin1String("checkbox")
                || type == QLatin1String("radio");
            if (checkable) {
                action->setCheckable(true);
                action->setChecked(
                    props.value(QStringLiteral("checked")).toBool());
            }
            action->setEnabled(
                props.value(QStringLiteral("enabled")).toBool(true));
            const QString extId = extensionId;
            const QString itemId = entry.itemId;
            const Engine::ContextMenuInfo click = info;
            const QPointer<WebView> guard(view);
            QObject::connect(action, &QAction::triggered, menu,
                    [extId, itemId, click, guard]() {
                deliverMenuClicked(extId, itemId, click, guard.data());
            });
        }
        // The show/hide events go to the extensions that actually
        // contributed — Chrome delivers them per-listener-extension.
        // OnShownData carries the shown item ids plus the contexts
        // that were active for this click.
        QJsonObject shown = menuClickInfo(QString(), QJsonObject(),
                                          info);
        shown.insert(QStringLiteral("pageUrl"),
                     view ? view->url().toString() : QString());
        QJsonArray menuIds;
        for (const auto &entry : visible)
            menuIds.append(entry.itemId);
        shown.insert(QStringLiteral("menuIds"), menuIds);
        QJsonArray ctxNames;
        for (const QString &ctx : std::as_const(active))
            ctxNames.append(ctx);
        shown.insert(QStringLiteral("contexts"), ctxNames);
        const QJsonObject shownArgs =
            argsPayload({shown, tabJson(view)});
        ExtensionBackgroundRuntime::instance()->deliverEvent(
            extensionId, QStringLiteral("contextMenus.onShown"),
            shownArgs);
        QObject::connect(menu, &QMenu::aboutToHide, menu,
                [extensionId, shownArgs]() {
            ExtensionBackgroundRuntime::instance()->deliverEvent(
                extensionId,
                QStringLiteral("contextMenus.onHidden"), shownArgs);
        });
    }
}

QJsonObject ExtensionEventHub::menuClickInfo(
        const QString &itemId, const QJsonObject &props,
        const Engine::ContextMenuInfo &info)
{
    QJsonObject data;
    if (!itemId.isEmpty())
        data.insert(QStringLiteral("menuItemId"), itemId);
    const QString parentId =
        props.value(QStringLiteral("parentId")).toString();
    if (!parentId.isEmpty())
        data.insert(QStringLiteral("parentMenuItemId"), parentId);
    data.insert(QStringLiteral("editable"), info.isContentEditable);
    data.insert(QStringLiteral("frameId"), 0);
    if (!info.linkUrl.isEmpty()) {
        data.insert(QStringLiteral("linkUrl"),
                    info.linkUrl.toString());
        if (!info.linkText.isEmpty())
            data.insert(QStringLiteral("linkText"),
                        info.linkText.left(4096));
    }
    if (!info.mediaUrl.isEmpty()) {
        data.insert(QStringLiteral("srcUrl"),
                    info.mediaUrl.toString());
        data.insert(QStringLiteral("mediaType"),
                    info.hasVideo ? QStringLiteral("video")
                    : info.hasImage ? QStringLiteral("image")
                    : QStringLiteral("audio"));
    }
    if (!info.selectedText.isEmpty())
        data.insert(QStringLiteral("selectionText"),
                    info.selectedText.left(4096));
    if (props.value(QStringLiteral("checked")).isBool())
        data.insert(QStringLiteral("checked"),
                    props.value(QStringLiteral("checked")).toBool());
    return data;
}

void ExtensionEventHub::deliverMenuClicked(
        const QString &extensionId, const QString &itemId,
        const Engine::ContextMenuInfo &info, WebView *view)
{
    if (extensionId.isEmpty() || itemId.isEmpty())
        return;
    const QJsonObject props =
        ExtensionShimBackend::contextMenuItem(extensionId, itemId);
    // Checkbox/radio clicks toggle the recorded state first —
    // OnClickData.checked reports the new value like Chrome.
    QJsonObject mutableProps = props;
    const QString type =
        props.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("checkbox")
            || type == QLatin1String("radio")) {
        const bool was =
            props.value(QStringLiteral("checked")).toBool();
        ExtensionShimBackend::setContextMenuChecked(
            extensionId, itemId, !was);
        mutableProps.insert(QStringLiteral("checked"), !was);
    }
    QJsonObject click = menuClickInfo(itemId, mutableProps, info);
    click.insert(QStringLiteral("pageUrl"),
                 view ? view->url().toString() : QString());
    ExtensionBackgroundRuntime::instance()->deliverEvent(
        extensionId, QStringLiteral("contextMenus.onClicked"),
        argsPayload({click, tabJson(view)}));
}
