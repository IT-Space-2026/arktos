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

#include <qcolor.h>
#include <qhash.h>
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
    // PTAB01: a private TAB inside a normal window — the page lives on
    // the shared off-the-record profile while the process stays
    // non-private.  In tor mode every tab is already off-the-record
    // on the dedicated tor profile; this just makes a normal new tab
    // there (the shared private profile is NOT tor-hardened).
    WebView *makeNewPrivateTab(bool makeCurrent = false);
    // The container the tab at index belongs to — the default
    // container id for normal and off-the-record pages.
    QString containerIdForTab(int index) const;
    // PTAB01: true when the tab's page browses off-the-record — the
    // tab marker, the menu gates and the child-tab inheritance all
    // key on the page profile, not the window-global private flag.
    bool isTabPrivate(int index) const;

    // TABGRP01 — named, color-coded tab groups (Chrome/Vivaldi style).
    // Membership is keyed on the tab's WebView so drags never lose it;
    // a group is a membership SET — normally contiguous, though a drag
    // may leave two runs that still count as one group.  A collapsed
    // group keeps only its first member (the "chip") on the strip —
    // the rest are detached, not destroyed, and re-insert on expand.
    QString tabGroupId(int index) const;            // "" when ungrouped
    QStringList tabGroupIds() const;
    QString tabGroupName(const QString &groupId) const;
    QColor tabGroupColor(const QString &groupId) const;
    // Total members including collapsed-hidden ones; 0 for an unknown id.
    int tabGroupSize(const QString &groupId) const;
    bool tabGroupIsCollapsed(const QString &groupId) const;
    // Visible strip indices of the group's members (just the chip while
    // collapsed).
    QList<int> tabGroupMembers(const QString &groupId) const;
    // The tab at index is the only visible member of a collapsed group.
    bool isTabGroupChip(int index) const;
    bool hasCollapsedTabGroup() const;
    // Creates an unnamed group (rotating palette color) holding the tab
    // — returns the new group id, "" when index has no web view.
    QString createTabGroup(int index);
    // Group the tab at index with the tab at targetIndex — joins the
    // target's group or creates one holding both (drag-drop stacking).
    void groupTabWith(int index, int targetIndex);
    void addTabToGroup(int index, const QString &groupId);
    void removeTabFromGroup(int index);
    void renameTabGroup(const QString &groupId, const QString &name);
    void setTabGroupColor(const QString &groupId, const QColor &color);
    void setTabGroupCollapsed(const QString &groupId, bool collapsed);
    void ungroupTabs(const QString &groupId);

    QByteArray saveState() const;
    bool restoreState(const QByteArray &state);

    static OpenUrlIn modifyWithUserBehavior(OpenUrlIn tab);
    WebView *getView(OpenUrlIn tab, WebView *currentView);

    // The omnibox resolver — public so the smoke harness and autotests
    // can assert the routing decision directly (SRCH07).  The two-arg
    // form takes the target tab's private context (PTAB01); the one-arg
    // form resolves it from the process-global private flag.
    static QUrl guessUrlFromString(const QString &url);
    static QUrl guessUrlFromString(const QString &url, bool privateContext);

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
    // POL02: the tab-strip hover preview's thumbnail — the page's
    // last live render at ~thumbnail scale, or null when the tab
    // never rendered a capturable frame (never shown, evicted,
    // blank).  Background tabs re-grab on request where the engine
    // still holds the last compositor frame; otherwise the frame
    // cached while the tab was last current is returned.
    QPixmap tabThumbnail(int index);
    void newTab();
    void newPrivateTab();
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
    QLabel *animationLabel(int index, bool addMovie);
    void retranslate();
    // The shared body of makeNewTabInContainer()/makeNewPrivateTab():
    // a location bar plus a WebView bound to the resolved profile.
    WebView *makeNewTabOnProfile(QWebEngineProfile *profile, bool makeCurrent);
    // Child-tab inheritance for the container AND the private context
    // of source (the current tab for Ctrl+T, the opener page for
    // window.open / open-in-new-tab).
    WebView *makeNewTabLike(WebView *source, bool makeCurrent);
    // SLEEP01: idle bookkeeping + the async state-capture half of
    // beginTabSleep (scroll position and the dirty-form probe come
    // back together from one runJavaScript round trip).
    void markTabActivity(WebView *webView);
    void beginTabSleep(int index, bool automatic);
    void finishTabSleepCapture(WebView *webView, bool automatic,
                             const QVariant &result);
    void applySleepVisuals(int index, bool sleeping);
    void updateSleepTimer();
    // POL02: thumbnail plumbing for the hover preview — the deferred
    // schedule lets the compositor present the freshly shown/loaded
    // page before the snapshot is taken.
    void captureTabThumbnail(int index);
    void scheduleThumbnailCapture(WebView *webView);

    // TABGRP01 internals — see the public accessors above.
    QString nextTabGroupId();
    void assignTabGroup(WebView *webView, const QString &groupId);
    void moveTabIntoGroupRun(int index);
    void detachGroupMember(int index);
    void expandTabGroup(const QString &groupId);
    void normalizeTabGroupMove(int movedIndex);
    void forgetTabGroupIfEmpty(const QString &groupId);
    // Child-tab inheritance: a tab opened from a page joins the
    // opener's group (and hides itself when that group is collapsed).
    void inheritTabGroup(WebView *webView, WebView *opener);
    // Every tab in strip order — with a collapsed group's hidden
    // members spliced in right after their chip.  Used by saveState so
    // no tab drops out of the serialized session.
    QList<WebView*> orderedWebViews() const;

    // A detached member of a collapsed group: its page widget, its
    // location bar and the tab-strip visuals re-applied on expand.
    struct HiddenGroupTab {
        QWidget *tab;
        QWidget *bar;
        QString text;
        QString toolTip;
        QVariant data;
    };
    struct TabGroup {
        QString id;
        QString name;
        QColor color;
        bool collapsed = false;
        QList<HiddenGroupTab> hidden;
    };
    QHash<QString, TabGroup> m_tabGroupInfo;
    // view -> group id for every member, hidden or visible.
    QHash<WebView*, QString> m_tabGroups;
    int m_tabGroupCounter = 0;
    // Reentrancy guard: our own membership-driven moveTab() calls feed
    // back through tabMoved.
    bool m_groupAdjust = false;

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
    // POL02: per-view thumbnail cache for the hover preview — keyed
    // on the view like the sleep bookkeeping so drags never lose it.
    QHash<WebView *, QPixmap> m_tabThumbnails;
};

#endif // TABWIDGET_H

