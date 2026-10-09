/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#ifndef TABWIDGET_H
#define TABWIDGET_H

#include <qtabwidget.h>

#include <qwebenginepage.h>
#include <qurl.h>

QT_BEGIN_NAMESPACE
class QCompleter;
class QLabel;
class QLineEdit;
class QMenu;
class QStackedWidget;
class QTimer;
QT_END_NAMESPACE

class BrowserMainWindow;
class OmniboxSuggestions;
class TabBar;
class WebView;
class WebActionMapper;
class WebViewSearch;
class QToolButton;

/*!
    TabWidget that contains WebViews and a stack widget of associated line edits.

    Connects up the current tab's signals to this class's signal and uses WebActionMapper
    to proxy the actions.
 */
class TabWidget : public QTabWidget
{
    Q_OBJECT

signals:
    // tab widget signals
    void tabsChanged();
    void lastTabClosed();

    // current tab signals
    void setCurrentTitle(const QString &url);
    void showStatusBarMessage(const QString &message);
    void linkHovered(const QString &link);
    void loadProgress(int progress);
    void printRequested(QWebEnginePage *page);

public:
    enum OpenUrlIn {
        NewWindow,
        NewSelectedTab,
        NewNotSelectedTab,
        CurrentTab,
        UserOrCurrent,
        NewTab = NewNotSelectedTab
    };

    TabWidget(QWidget *parent = nullptr);

    void loadSettings();
    TabBar *tabBar() { return m_tabBar; }
    void clear();
    void addWebAction(QAction *action, QWebEnginePage::WebAction webAction);

    QAction *newTabAction() const;
    QAction *closeTabAction() const;
    QAction *bookmarkTabsAction() const;
    QAction *recentlyClosedTabsAction() const;
    QAction *nextTabAction() const;
    QAction *previousTabAction() const;

    QWidget *locationBarStack() const;
    QLineEdit *currentLocationBar() const;
    WebView *currentWebView() const;
    WebView *webView(int index) const;
    WebViewSearch *webViewSearch(int index) const;
    QLineEdit *locationBar(int index) const;
    int webViewIndex(WebView *webView) const;
    // Child-tab inheritance (CONT02): a plain new tab is bound to the
    // CURRENT tab's container — Firefox's rule — so tabs opened out
    // of a container stay inside it.  An explicit container comes
    // through makeNewTabInContainer().
    WebView *makeNewTab(bool makeCurrent = false);
    // Creates a tab bound to containerId's profile; the empty
    // (default) container id selects the shared browsing context.
    // Private browsing and tor windows always produce an
    // off-the-record tab — a persistent container profile would leak
    // the session — and unknown/deleted ids degrade to the default
    // container.
    WebView *makeNewTabInContainer(const QString &containerId, bool makeCurrent = false);
    // The container the tab at index belongs to — the default
    // container id for normal and off-the-record pages.
    QString containerIdForTab(int index) const;

    QByteArray saveState() const;
    bool restoreState(const QByteArray &state);

    static OpenUrlIn modifyWithUserBehavior(OpenUrlIn tab);
    WebView *getView(OpenUrlIn tab, WebView *currentView);

protected:
    void changeEvent(QEvent *event) override;

public slots:
    void loadString(const QString &string, OpenUrlIn tab = CurrentTab);
    // The gate for urls that arrive from outside the browser chrome —
    // the command line, a forwarded second-instance message.  The
    // string is resolved through guessUrlFromString() first so a
    // payload that only parses to javascript: after normalization is
    // still refused (SEC09).  Typed input uses loadString().
    void loadStringFromUntrustedSource(const QString &string, OpenUrlIn tab = CurrentTab);
    void loadUrlFromUser(const QUrl &url, const QString &title = QString());
    void loadUrl(const QUrl &url, TabWidget::OpenUrlIn tab = CurrentTab, const QString &title = QString());
    void createTab(const QByteArray &historyState, TabWidget::OpenUrlIn tab = CurrentTab,
                   const QString &containerId = QString());
    // CONT02: reopen the tab at index bound to a different container —
    // a fresh tab on the target profile takes its slot and url, then
    // the old tab closes (a page's profile is immutable, so the swap
    // is the only possible implementation; history cannot carry).
    void reopenTabInContainer(int index, const QString &containerId);
    // CONT04: opens url in a fresh tab bound to containerId — the
    // "always open in this container" diversion path in WebPage hands
    // ruled navigations here so they land on the right profile.
    void loadUrlInContainer(const QUrl &url, const QString &containerId);
    // Entry point for container management — the tab context menu and
    // the File menu both land here so CONT03's containers page has a
    // single seam.
    void manageContainers();
    // SLEEP01: sleeping tabs.  A slept tab's page is discarded by the
    // engine (its renderer memory is reclaimed) while the tab slot,
    // title and favicon stay on the strip; activating the tab reloads
    // the page and restores the scroll position.
    void sleepTab(int index = -1);
    void wakeTab(int index = -1);
    bool isTabSleeping(int index) const;
    int sleepingTabCount() const;
    // Why a tab cannot be auto-slept right now — "current",
    // "sleeping", "inflight", "loading", "audible", "download",
    // "form", "empty" — or an empty string when it is eligible.  Used
    // by the idle sweep and exposed for the smoke test.
    QString sleepBlockReason(int index) const;
    // Auto-suspend sweep: sleep every background tab idle for at
    // least idleMs milliseconds.  The settings-driven timer calls
    // this; the smoke test drives it directly.
    void suspendIdleTabs(qint64 idleMs);
    void newTab();
    void cloneTab(int index = -1);
    void closeTab(int index = -1);
    void closeOtherTabs(int index);
    void reloadTab(int index = -1);
    void reloadAllTabs();
    void nextTab();
    void previousTab();
    void bookmarkTabs();

private slots:
    void currentChanged(int index);
    void openLastTab();
    void aboutToShowRecentTabsMenu();
    void aboutToShowRecentTriggeredAction(QAction *action);
    void webViewLoadStarted();
    void webViewLoadProgress(int progress);
    void webViewLoadFinished(bool ok);
    void webViewIconChanged();
    void webViewTitleChanged(const QString &title);
    void webViewUrlChanged(const QUrl &url);
    void lineEditReturnPressed();
    void windowCloseRequested();
    void moveTab(int fromIndex, int toIndex);
    void historyCleared();

private:
    static QUrl guessUrlFromString(const QString &url);
    QLabel *animationLabel(int index, bool addMovie);
    void retranslate();
    // SLEEP01: idle bookkeeping + the async state-capture half of
    // beginTabSleep (scroll position and the dirty-form probe come
    // back together from one runJavaScript round trip).
    void markTabActivity(WebView *webView);
    void beginTabSleep(int index, bool automatic);
    void finishTabSleepCapture(WebView *webView, bool automatic,
                             const QVariant &result);
    void applySleepVisuals(int index, bool sleeping);
    void updateSleepTimer();

    QAction *m_recentlyClosedTabsAction;
    QAction *m_newTabAction;
    QAction *m_closeTabAction;
    QAction *m_bookmarkTabsAction;
    QAction *m_nextTabAction;
    QAction *m_previousTabAction;

    QMenu *m_recentlyClosedTabsMenu;
    static const int m_recentlyClosedTabsSize = 10;
    QList<QUrl> m_recentlyClosedTabs;
    QList<QByteArray> m_recentlyClosedTabsHistory;
    // CONT02: parallel to m_recentlyClosedTabs — reopening a closed
    // tab returns it to the container it was closed in.
    QList<QString> m_recentlyClosedTabsContainers;
    QList<WebActionMapper*> m_actions;

    QCompleter *m_lineEditCompleter;
    OmniboxSuggestions *m_omniboxSuggestions;
    QStackedWidget *m_locationBars;
    TabBar *m_tabBar;
    QToolButton *addTabButton;
    QToolButton *closeTabButton;

    // SLEEP01: per-tab suspend bookkeeping, keyed on the view so tab
    // drags never disturb it.  formDirty latches the unsaved-input
    // exemption until the next navigation; scrollX/Y are the captured
    // position restored after the wake reload.
    struct TabSleepState {
        bool sleeping = false;
        bool sleepInFlight = false;
        bool formDirty = false;
        bool restoreScroll = false;
        double scrollX = 0;
        double scrollY = 0;
        qint64 lastActiveMs = 0;
    };
    QHash<WebView *, TabSleepState> m_sleepStates;
    QTimer *m_sleepTimer = nullptr;
    qint64 m_suspendIdleMs = 0;
};

#endif // TABWIDGET_H

