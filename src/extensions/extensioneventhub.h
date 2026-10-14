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

#ifndef EXTENSIONEVENTHUB_H
#define EXTENSIONEVENTHUB_H

#include <qhash.h>
#include <qjsonobject.h>
#include <qobject.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>

class BrowserMainWindow;
class QMenu;
class QTimer;
class TabWidget;
class WebView;

namespace Engine {
struct ContextMenuInfo;
}

// EXT16 — extension event surfaces.  EXT14/EXT15 gave hosted
// extensions a working chrome.* surface; this is the half that
// pushes the browser's own signals into their event listeners:
//
//   chrome.tabs          onCreated/onUpdated/onActivated/onRemoved
//                        (+ onHighlighted/onMoved/onDetached/
//                        onAttached — the signals Arora actually
//                        has), built on the session-stable ids the
//                        shim backend already mints for tabs.query
//   chrome.webNavigation onBeforeNavigate (WebPage::aboutToLoadUrl)
//                        onCommitted (main-frame loadStarted)
//                        onCompleted/onErrorOccurred (loadFinished
//                        with the loadingChanged failure detail)
//                        onHistoryStateUpdated/onReferenceFragment-
//                        Updated for same-document url changes
//   chrome.contextMenus  the EXT15 registry rendered into the real
//                        page context menu; clicks deliver onClicked
//                        (info, tab), and the menu's show/hide
//                        deliver onShown/onHidden
//   chrome.windows       onCreated/onRemoved/onFocusChanged — the
//                        window-level mirror of the tab tracking
//
// Delivery: ExtensionBackgroundRuntime fans the payloads out to
// every registered shim context (hosted backgrounds now, popups
// once EXT17 lands) over each ChromeBridge's hostEvent signal —
// the same channel relay messages already cross.  The hidden host
// page's timers are throttled but the webchannel signal is an IPC
// push, not a timer: deliveries arrive while the view stays
// answerable (EXT14's Active-lifecycle pin + nudge).  Events that
// carry more than the single details object ride a {__args:[...]}
// envelope the shim unpacks.
//
// Permission parity with Chrome (enforced in the runtime's
// fan-out): webNavigation.* and contextMenus.* reach only
// extensions declaring the permission; tabs.*/windows.* events are
// ungated but url-bearing fields are scrubbed unless the manifest
// grants 'tabs' or a host permission covering the tab's URL.
// Host-internal pushes (alarms, relay, ports) always pass.
//
// Privacy: tabs on off-the-record or tor profiles are never
// tracked — no event, no id, no chrome.tabs row (Chrome gates
// incognito visibility behind an explicit per-extension toggle we
// do not have; silent exclusion is the honest subset).  A window
// whose tabs are all untrackable produces no windows.* events
// either — a private or tor window is invisible to extensions.
class ExtensionEventHub : public QObject
{
    Q_OBJECT

public:
    static ExtensionEventHub *instance();

    // The runtime drives tracking with host presence — zero hosted
    // extensions means zero listeners and zero tracking work.
    void setTracking(bool on);
    bool tracking() const { return m_tracking; }
    // Immediate rescan (public so tests and the runtime can force a
    // pass without waiting for a signal).
    void syncNow();
    int trackedTabCount() const { return m_tabs.size(); }

    // The WebView::contextMenuEvent contribution point — appends a
    // per-extension submenu for every hosted extension holding
    // items whose declared contexts match this click, delivers
    // onShown to them, and hooks aboutToHide for onHidden.  Static
    // and entirely registry-driven so a menu built without any
    // hosted extension costs a lookup and returns.
    static void contributeContextMenu(QMenu *menu,
                                      const Engine::ContextMenuInfo &info,
                                      WebView *view);

    // Delivers a contextMenus.onClicked payload — the menu trigger
    // path and the test seam share it.
    static void deliverMenuClicked(const QString &extensionId,
                                   const QString &itemId,
                                   const Engine::ContextMenuInfo &info,
                                   WebView *view);

    // Every top-level BrowserMainWindow — the real window set.  The
    // app's m_mainWindows registry deliberately excludes private
    // windows and is empty under qtest (windows constructed
    // directly), so events and tabs.query enumerate the widgets
    // instead and the OTR/tor exclusion happens per tab.
    static QList<BrowserMainWindow *> browsingWindows();
    // A tab events and queries may describe — real browsing
    // profiles only; off-the-record and tor pages stay invisible
    // to extensions.
    static bool trackable(const WebView *view);

private:
    explicit ExtensionEventHub(QObject *parent = nullptr);

    // App-level filter: a BrowserMainWindow's Show/WindowActivate
    // lands synchronously inside show(), so wiring on it catches a
    // new window before the code that created it can drive a tab
    // (session restore, scripted windows, the smoke harness) — the
    // 2.5s scan alone would miss the burst.
    bool eventFilter(QObject *watched, QEvent *event) override;

    void wireWindow(BrowserMainWindow *window);
    void wireView(WebView *view);
    void onTabActivated(TabWidget *tabs, int index);
    // destroyed() handlers receive the TYPED pointer captured at wire
    // time — during the signal the object is already sliced to
    // QObject and any downcast or member access on it is UB.
    void onViewDestroyed(WebView *view);
    void onWindowDestroyed(BrowserMainWindow *window);

    // Emitters — one per event family; all shapes per the
    // WebExtensions spec, multi-arg events via the __args envelope.
    void emitTabCreated(WebView *view);
    void emitTabRemoved(int tabId, int windowId,
                        bool isWindowClosing);
    void emitTabUpdated(WebView *view, const QJsonObject &changeInfo);
    void emitNavEvent(WebView *view, const QString &name,
                      const QUrl &url, const QJsonObject &extra
                      = QJsonObject());

    static QJsonObject tabJson(WebView *view);
    static QJsonObject windowJson(BrowserMainWindow *window, bool focused);
    static QJsonObject menuClickInfo(const QString &itemId,
                                     const QJsonObject &props,
                                     const Engine::ContextMenuInfo &info);

    bool m_tracking = false;
    bool m_firstScanDone = false;

    struct TrackedTab {
        QPointer<WebView> view;
        QPointer<BrowserMainWindow> window;
        // Identity-only keys: windowObject is the window's address as
        // a plain QObject — usable in dead-object compares where
        // QPointer::data()'s checked cast would be UB.  tabId is
        // minted while the view lives, so teardown emits it without
        // touching the dead object.
        QObject *windowObject = nullptr;
        int tabId = -1;
        int windowId = -1;  // minted once, kept for teardown — the
                            // window->id map may be gone by then
        int index = -1;
        bool loading = false;
        // The last committed url — same-document urlChanged hops are
        // classified against it (fragment vs history-state update).
        QString lastUrl;
        // The last failed load's detail — onErrorOccurred answers a
        // net:: name + the engine's own error code/string.
        QString lastError;
        int lastErrorCode = 0;
        int lastErrorDomain = -1;
    };
    QHash<WebView *, TrackedTab> m_tabs;

    // Windows whose tab widget is already wired — raw pointers are
    // safe: every wired window's destroyed() removes the entry.
    QSet<BrowserMainWindow *> m_wiredWindows;
    // Windows seen on a previous scan — their ids let teardown fire
    // windows.onRemoved even after the widget is gone.  Keyed by
    // QObject identity: entries get consulted while the window is
    // mid-destruction, where a typed-pointer cast is UB.
    QHash<QObject *, int> m_windowIds;
    // Windows in ~QObject — onWindowDestroyed marks them before the
    // child WebViews' own destroyed() lands, so each tab's onRemoved
    // can answer isWindowClosing honestly.
    QSet<QObject *> m_dyingWindows;
    int m_focusWindowId = -2;   // -2 = unset (first scan is quiet)
    QTimer *m_scan = nullptr;
};

#endif // EXTENSIONEVENTHUB_H
