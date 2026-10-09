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

#include "tabwidget.h"

#include "addbookmarkdialog.h"
#include "aroraicon.h"
#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "containermanager.h"
#include "downloadmanager.h"
#include "history.h"
#include "historycompleter.h"
#include "historymanager.h"
#include "locationbar.h"
#include "omniboxsuggestions.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "safetext.h"
#include "settings.h"
#include "streamingutils.h"
#include "tabbar.h"
#include "toolbarsearch.h"
#include "webactionmapper.h"
#include "webpage.h"
#include "webview.h"
#include "webviewsearch.h"

#include <qabstractproxymodel.h>
#include <qcompleter.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qevent.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qmovie.h>
#include <qpointer.h>
#include <qregularexpression.h>
#include <qsettings.h>
#include <qstackedwidget.h>
#include <qstyle.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qwebenginehistory.h>
#include <qwebengineprofile.h>

#include <qdebug.h>

#include <algorithm>

//#define USERMODIFIEDBEHAVIOR_DEBUG

TabWidget::TabWidget(QWidget *parent)
    : QTabWidget(parent)
    , m_recentlyClosedTabsAction(nullptr)
    , m_newTabAction(nullptr)
    , m_closeTabAction(nullptr)
    , m_bookmarkTabsAction(nullptr)
    , m_nextTabAction(nullptr)
    , m_previousTabAction(nullptr)
    , m_recentlyClosedTabsMenu(nullptr)
    , m_lineEditCompleter(nullptr)
    , m_omniboxSuggestions(nullptr)
    , m_locationBars(nullptr)
    , m_tabBar(new TabBar(this))
    , addTabButton(nullptr)
    , closeTabButton(nullptr)
{
    setElideMode(Qt::ElideRight);

    new QShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_T), this, [this]() { openLastTab(); });
    new QShortcut(QKeySequence::Undo, this, [this]() { openLastTab(); });

    connect(m_tabBar, &TabBar::loadUrl, this,
            [this](const QUrl &url, TabWidget::OpenUrlIn tab) { loadUrl(url, tab); });
    connect(m_tabBar, &TabBar::newTab, this, &TabWidget::newTab);
    connect(m_tabBar, QOverload<int>::of(&TabBar::closeTab), this, &TabWidget::closeTab);
    connect(m_tabBar, QOverload<int>::of(&TabBar::cloneTab), this, &TabWidget::cloneTab);
    connect(m_tabBar, QOverload<int>::of(&TabBar::closeOtherTabs), this, &TabWidget::closeOtherTabs);
    connect(m_tabBar, QOverload<int>::of(&TabBar::reloadTab), this, &TabWidget::reloadTab);
    connect(m_tabBar, &TabBar::reloadAllTabs, this, &TabWidget::reloadAllTabs);
    connect(m_tabBar, &TabBar::reopenInContainer,
            this, &TabWidget::reopenTabInContainer);
    connect(m_tabBar, &TabBar::sleepTab, this, &TabWidget::sleepTab);
    connect(m_tabBar, &TabBar::wakeTab, this, &TabWidget::wakeTab);
    setTabBar(m_tabBar);
    m_tabBar->setAccessibleName(tr("Tabs"));
    setDocumentMode(true);
    connect(m_tabBar, &QTabBar::tabMoved,
            this, &TabWidget::moveTab);

    // Actions
    m_newTabAction = new QAction(this);
    m_newTabAction->setShortcuts(QKeySequence::AddTab);
    connect(m_newTabAction, &QAction::triggered, this, &TabWidget::newTab);

    m_closeTabAction = new QAction(this);
    m_closeTabAction->setShortcuts(QKeySequence::Close);
    m_closeTabAction->setIcon(AroraIcon::get(QLatin1String("window-close")));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_closeTabAction->setIconVisibleInMenu(false);
#endif
    connect(m_closeTabAction, &QAction::triggered, this, [this]() { closeTab(); });

    m_bookmarkTabsAction = new QAction(this);
    connect(m_bookmarkTabsAction, &QAction::triggered, this, &TabWidget::bookmarkTabs);

    m_newTabAction->setIcon(AroraIcon::get(QLatin1String("tab-new")));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_newTabAction->setIconVisibleInMenu(false);
#endif

    m_nextTabAction = new QAction(this);
    connect(m_nextTabAction, &QAction::triggered, this, &TabWidget::nextTab);

    m_previousTabAction = new QAction(this);
    connect(m_previousTabAction, &QAction::triggered, this, &TabWidget::previousTab);
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_previousTabAction->setIcon(AroraIcon::get(QLatin1String("go-previous")));
    m_nextTabAction->setIcon(AroraIcon::get(QLatin1String("go-next")));
#endif

    m_recentlyClosedTabsMenu = new QMenu(this);
    connect(m_recentlyClosedTabsMenu, &QMenu::aboutToShow,
            this, &TabWidget::aboutToShowRecentTabsMenu);
    connect(m_recentlyClosedTabsMenu, &QMenu::triggered,
            this, &TabWidget::aboutToShowRecentTriggeredAction);
    m_recentlyClosedTabsAction = new QAction(this);
    m_recentlyClosedTabsAction->setMenu(m_recentlyClosedTabsMenu);
    m_recentlyClosedTabsAction->setEnabled(false);

#ifndef Q_OS_MACOS // can't seem to figure out the background color :(
    addTabButton = new QToolButton(this);
    addTabButton->setDefaultAction(m_newTabAction);
    addTabButton->setAutoRaise(true);
    addTabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
#endif

    connect(m_tabBar, &QTabBar::tabCloseRequested,
            this, &TabWidget::closeTab);
    connect(this, &QTabWidget::currentChanged,
            this, &TabWidget::currentChanged);

    m_locationBars = new QStackedWidget(this);

    connect(BrowserApplication::historyManager(), &HistoryManager::historyCleared,
        this, &TabWidget::historyCleared);

    // CONT02: a container rename/recolor/delete must repaint the tab
    // strip's chips and recompute the size hints the chip feeds.
    connect(ContainerManager::instance(), &ContainerManager::containersChanged,
            m_tabBar, [this]() {
        m_tabBar->updateGeometry();
        m_tabBar->update();
    });

    // Initialize Actions' labels
    retranslate();
    loadSettings();
}

void TabWidget::historyCleared()
{
    m_recentlyClosedTabs.clear();
    m_recentlyClosedTabsHistory.clear();
    m_recentlyClosedTabsContainers.clear();
    m_recentlyClosedTabsAction->setEnabled(false);
}

void TabWidget::clear()
{
    // clear the recently closed tabs
    m_recentlyClosedTabs.clear();
    m_recentlyClosedTabsHistory.clear();
    m_recentlyClosedTabsContainers.clear();
    m_recentlyClosedTabsAction->setEnabled(false);
    // clear the line edit history
    for (int i = 0; i < m_locationBars->count(); ++i) {
        QLineEdit *qLineEdit = locationBar(i);
        qLineEdit->setText(qLineEdit->text());
        webViewSearch(i)->clear();
    }
}

// When index is -1 index chooses the current tab
void TabWidget::reloadTab(int index)
{
    if (index < 0)
        index = currentIndex();
    if (index < 0 || index >= count())
        return;

    if (WebView *tab = webView(index)) {
        tab->reload();
    }
}

void TabWidget::moveTab(int fromIndex, int toIndex)
{
    QWidget *lineEdit = m_locationBars->widget(fromIndex);
    m_locationBars->removeWidget(lineEdit);
    m_locationBars->insertWidget(toIndex, lineEdit);
}

void TabWidget::addWebAction(QAction *action, QWebEnginePage::WebAction webAction)
{
    if (!action)
        return;
    m_actions.append(new WebActionMapper(action, webAction, this));
}

void TabWidget::currentChanged(int index)
{
    WebView *webView = this->webView(index);
    if (!webView)
        return;

    Q_ASSERT(m_locationBars->count() == count());

    WebView *oldWebView = this->webView(m_locationBars->currentIndex());
    if (oldWebView) {
        disconnect(oldWebView, &WebView::statusBarMessage,
                   this, &TabWidget::showStatusBarMessage);
        disconnect(oldWebView->page(), &QWebEnginePage::linkHovered,
                   this, &TabWidget::linkHovered);
        disconnect(oldWebView, &QWebEngineView::loadProgress,
                   this, &TabWidget::loadProgress);
    }

    connect(webView, &WebView::statusBarMessage,
            this, &TabWidget::showStatusBarMessage);
    connect(webView->page(), &QWebEnginePage::linkHovered,
            this, &TabWidget::linkHovered);
    connect(webView, &QWebEngineView::loadProgress,
            this, &TabWidget::loadProgress);

    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->updateCurrent(webView->page());
    }
    // SLEEP01: the outgoing tab's idle clock starts now; activating a
    // suspended tab wakes it (the engine reloads the page).
    markTabActivity(oldWebView);
    markTabActivity(webView);
    if (isTabSleeping(index))
        wakeTab(index);
    emit setCurrentTitle(webView->title());
    m_locationBars->setCurrentIndex(index);
    emit loadProgress(webView->progress());
    emit showStatusBarMessage(webView->lastStatusBarText());
    if (webView->url().isEmpty() && webView->hasFocus()) {
        m_locationBars->currentWidget()->setFocus();
    } else if (!webView->url().isEmpty()) {
        webView->setFocus();
    }
}

QAction *TabWidget::newTabAction() const
{
    return m_newTabAction;
}

QAction *TabWidget::closeTabAction() const
{
    return m_closeTabAction;
}

QAction *TabWidget::bookmarkTabsAction() const
{
    return m_bookmarkTabsAction;
}

QAction *TabWidget::recentlyClosedTabsAction() const
{
    return m_recentlyClosedTabsAction;
}

QAction *TabWidget::nextTabAction() const
{
    return m_nextTabAction;
}

QAction *TabWidget::previousTabAction() const
{
    return m_previousTabAction;
}

QWidget *TabWidget::locationBarStack() const
{
    return m_locationBars;
}

QLineEdit *TabWidget::currentLocationBar() const
{
    return locationBar(m_locationBars->currentIndex());
}

WebView *TabWidget::currentWebView() const
{
    return webView(currentIndex());
}

QLineEdit *TabWidget::locationBar(int index) const
{
    return qobject_cast<LocationBar*>(m_locationBars->widget(index));
}

WebView *TabWidget::webView(int index) const
{
    QWidget *widget = this->widget(index);
    if (WebViewWithSearch *webViewWithSearch = qobject_cast<WebViewWithSearch*>(widget)) {
        return webViewWithSearch->m_webView;
    }
    return nullptr;
}

WebViewSearch *TabWidget::webViewSearch(int index) const
{
    QWidget *widget = this->widget(index);
    if (WebViewWithSearch *webViewWithSearch = qobject_cast<WebViewWithSearch*>(widget)) {
        return webViewWithSearch->m_webViewSearch;
    }
    return nullptr;
}

int TabWidget::webViewIndex(WebView *webView) const
{
    for (int i = 0; i < count(); ++i) {
        QWidget *widget = this->widget(i);
        if (WebViewWithSearch *webViewWithSearch = qobject_cast<WebViewWithSearch*>(widget)) {
            if (webViewWithSearch->m_webView == webView)
                return i;
        }
    }
    return -1;
}

void TabWidget::newTab()
{
    makeNewTab(true);
}

WebView *TabWidget::makeNewTab(bool makeCurrent)
{
    // CONT02: child-tab inheritance — the new tab stays in the
    // current tab's container ("" when the strip is empty or the
    // current tab is in the default container).
    return makeNewTabInContainer(containerIdForTab(currentIndex()), makeCurrent);
}

QString TabWidget::containerIdForTab(int index) const
{
    WebView *view = webView(index);
    if (!view)
        return ContainerManager::defaultContainerId();
    return view->containerId();
}

WebView *TabWidget::makeNewTabInContainer(const QString &containerId, bool makeCurrent)
{
    // line edit
    LocationBar *locationBar = new LocationBar;
    if (!m_lineEditCompleter) {
        HistoryCompletionModel *completionModel = new HistoryCompletionModel(this);
        completionModel->setSourceModel(BrowserApplication::historyManager()->historyFilterModel());
        // SRCH01: search suggestions sit above the history matches in
        // the same dropdown — the merged model keeps the completer
        // hack in HistoryCompleter unaware of the extra rows.
        OmniboxCompletionModel *omniboxModel =
            new OmniboxCompletionModel(completionModel, this);
        // SRCH06: the "@tabs" scope lists this widget's open tabs —
        // supplied as a provider so the shared completion model never
        // depends on TabWidget.
        omniboxModel->setTabEntryProvider(
            [this]() -> QList<OmniboxCompletionModel::TabEntry> {
            QList<OmniboxCompletionModel::TabEntry> entries;
            for (int i = 0; i < count(); ++i) {
                OmniboxCompletionModel::TabEntry entry;
                entry.index = i;
                entry.title = tabText(i);
                if (WebView *view = webView(i))
                    entry.url = QString::fromUtf8(view->url().toEncoded());
                entry.icon = tabIcon(i);
                entries.append(entry);
            }
            return entries;
        });
        m_lineEditCompleter = new HistoryCompleter(omniboxModel, this);
        // activated(QModelIndex) carries an index into the completer's
        // private completion proxy — map it back so @tabs rows can
        // switch to their tab instead of navigating.
        connect(m_lineEditCompleter,
                QOverload<const QModelIndex &>::of(&QCompleter::activated),
                this,
                [this, omniboxModel](const QModelIndex &activatedIndex) {
            QModelIndex index = activatedIndex;
            if (index.model() != omniboxModel) {
                if (QAbstractProxyModel *proxy =
                        qobject_cast<QAbstractProxyModel *>(
                            m_lineEditCompleter->completionModel()))
                    index = proxy->mapToSource(index);
            }
            const QVariant tabIndex =
                index.data(OmniboxCompletionModel::TabIndexRole);
            if (tabIndex.isValid()) {
                const int target = tabIndex.toInt();
                if (target >= 0 && target < count()) {
                    setCurrentIndex(target);
                    if (WebView *view = currentWebView())
                        view->setFocus();
                }
                return;
            }
            loadString(index.data(HistoryModel::UrlStringRole).toString());
        });
        m_omniboxSuggestions = new OmniboxSuggestions(
            omniboxModel, m_lineEditCompleter, this);
        // Should this be in Qt by default?
        QAbstractItemView *popup = m_lineEditCompleter->popup();
        QListView *listView = qobject_cast<QListView*>(popup);
        if (listView) {
            // Urls are always LeftToRight
            listView->setLayoutDirection(Qt::LeftToRight);
            listView->setUniformItemSizes(true);
        }
    }
    locationBar->setCompleter(m_lineEditCompleter);
    connect(locationBar, &QLineEdit::textEdited,
            m_omniboxSuggestions, &OmniboxSuggestions::scheduleSuggestions);
    connect(locationBar, &QLineEdit::returnPressed, this, &TabWidget::lineEditReturnPressed);
    m_locationBars->addWidget(locationBar);
    m_locationBars->setSizePolicy(locationBar->sizePolicy());

#ifndef AUTOTESTS
    if (BrowserMainWindow *window = BrowserMainWindow::parentWindow(this)) {
        if (ToolbarSearch *toolbarSearch = window->findChild<ToolbarSearch *>())
            QWidget::setTabOrder(locationBar, toolbarSearch);
    }
#endif

    // webview — bound to the container's profile (CONT02).  The
    // default container and private browsing both resolve to
    // BrowserApplication::webEngineProfile() (the normal "arora"
    // profile, or the off-the-record profile while private browsing
    // is on) — containers are persistent state and can never take
    // over a private tab, and ContainerManager::profileFor() refuses
    // outright under tor or for unknown/deleted ids.  The accessor is
    // static, so this is also safe under autotests that never
    // instantiate the application object.
    QWebEngineProfile *profile = BrowserApplication::webEngineProfile();
    if (!containerId.isEmpty() && !BrowserApplication::isPrivate()) {
        if (QWebEngineProfile *containerProfile =
                ContainerManager::instance()->profileFor(containerId))
            profile = containerProfile;
    }
    WebView *webView = new WebView(profile);
    locationBar->setWebView(webView);
    connect(webView, &QWebEngineView::loadStarted,
            this, &TabWidget::webViewLoadStarted);
    connect(webView, &QWebEngineView::loadProgress,
            this, &TabWidget::webViewLoadProgress);
    connect(webView, &QWebEngineView::loadFinished,
            this, &TabWidget::webViewLoadFinished);
    connect(webView, &QWebEngineView::iconChanged,
            this, [this]() { webViewIconChanged(); });
    connect(webView, &QWebEngineView::titleChanged,
            this, &TabWidget::webViewTitleChanged);
    connect(webView, &QWebEngineView::urlChanged,
            this, &TabWidget::webViewUrlChanged);
    connect(webView, &WebView::search,
            this, [this](const QUrl &url, TabWidget::OpenUrlIn tab) { loadUrl(url, tab); });
    connect(webView->page(), &QWebEnginePage::windowCloseRequested,
            this, &TabWidget::windowCloseRequested);
    connect(webView, &QWebEngineView::printRequested,
            this, [this, webView]() { emit printRequested(webView->page()); });
    // Qt WebEngine does not surface WebKit's window-feature requests
    // (geometryChangeRequested / *VisibilityChangeRequested); window.open
    // chrome handling is internal to Chromium.

    WebViewWithSearch *webViewWithSearch = new WebViewWithSearch(webView, this);
    addTab(webViewWithSearch, tr("Untitled"));
    // SLEEP01: the idle clock starts at creation — a freshly opened
    // background tab is not immediately suspendable.  A fresh record
    // also drops any state a recycled pointer might have inherited.
    m_sleepStates.insert(webView, TabSleepState());
    markTabActivity(webView);
    if (makeCurrent)
        setCurrentWidget(webViewWithSearch);

    // webview actions
    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->addChild(webView->page()->action(mapper->webAction()));
    }

    if (count() == 1)
        currentChanged(currentIndex());
    emit tabsChanged();
    return webView;
}

void TabWidget::reopenTabInContainer(int index, const QString &containerId)
{
    if (index < 0 || index >= count())
        return;
    WebView *tab = webView(index);
    if (!tab)
        return;
    if (containerIdForTab(index) == containerId)
        return;
    // A private tab must never move onto a persistent container
    // profile — that would write the session to disk (SEC07), and
    // tor refuses containers entirely.  Unknown/deleted ids likewise
    // leave the tab alone.
    if (BrowserApplication::isPrivate() || BrowserApplication::isTorMode())
        return;
    if (!containerId.isEmpty()
        && !ContainerManager::instance()->isContainerId(containerId))
        return;

    const QUrl url = tab->url();
    WebView *newTab = makeNewTabInContainer(containerId, true);
    if (!newTab)
        return;
    // The fresh tab appended at the end; slide it into the old tab's
    // slot (moveTab emits tabMoved, which keeps m_locationBars in
    // sync) so the strip order survives the swap.
    const int appendedIndex = count() - 1;
    if (appendedIndex > index)
        m_tabBar->moveTab(appendedIndex, index);
    if (!url.isEmpty() && url.isValid())
        newTab->loadUrl(url);
    // The old tab is now one slot past the new one's position.
    closeTab(index + 1);
}

void TabWidget::loadUrlInContainer(const QUrl &url, const QString &containerId)
{
    if (!url.isValid())
        return;
    // The diversion raised the tab — the user asked to land on the
    // page, so it takes focus.
    WebView *webView = makeNewTabInContainer(containerId, true);
    if (webView)
        webView->loadUrl(url);
}

void TabWidget::manageContainers()
{
    // CONT03: the shared management entry point — the tab context menu
    // and the File menu both land here; open the settings dialog
    // straight on its Containers page.
    QWidget *parent = BrowserMainWindow::parentWindow(this);
    SettingsDialog dialog(parent ? parent : this);
    dialog.openAtPage(SettingsDialog::ContainersPage);
    dialog.exec();
}

// SLEEP01 — sleeping tabs -------------------------------------------------
//
// A "sleeping" tab keeps its slot on the strip (title + favicon stay
// put) while its QWebEnginePage is discarded — the engine tears down
// the WebContents and reclaims the renderer memory.  Reactivating the
// tab reloads the page; the scroll position captured at sleep time is
// re-applied on the next loadFinished.  Back/forward history survives
// on the page object, but in-page form state does not — tabs with
// unsaved form input are refused.

void TabWidget::markTabActivity(WebView *webView)
{
    if (!webView)
        return;
    m_sleepStates[webView].lastActiveMs = QDateTime::currentMSecsSinceEpoch();
}

void TabWidget::updateSleepTimer()
{
    if (m_suspendIdleMs > 0) {
        if (!m_sleepTimer) {
            m_sleepTimer = new QTimer(this);
            connect(m_sleepTimer, &QTimer::timeout, this, [this]() {
                suspendIdleTabs(m_suspendIdleMs);
            });
        }
        m_sleepTimer->start(15000);
    } else if (m_sleepTimer) {
        m_sleepTimer->stop();
    }
}

bool TabWidget::isTabSleeping(int index) const
{
    WebView *view = const_cast<TabWidget*>(this)->webView(index);
    return view && m_sleepStates.value(view).sleeping;
}

int TabWidget::sleepingTabCount() const
{
    int count = 0;
    for (const TabSleepState &state : m_sleepStates) {
        if (state.sleeping)
            ++count;
    }
    return count;
}

QString TabWidget::sleepBlockReason(int index) const
{
    if (index < 0 || index >= count())
        return QLatin1String("invalid");
    if (index == currentIndex())
        return QLatin1String("current");
    WebView *view = const_cast<TabWidget*>(this)->webView(index);
    if (!view || !view->page() || view->url().isEmpty())
        return QLatin1String("empty");
    const TabSleepState state = m_sleepStates.value(view);
    if (state.sleeping
        || view->page()->lifecycleState()
               == QWebEnginePage::LifecycleState::Discarded)
        return QLatin1String("sleeping");
    if (state.sleepInFlight)
        return QLatin1String("inflight");
    if (state.formDirty)
        return QLatin1String("form");
    if (view->page()->isLoading())
        return QLatin1String("loading");
    if (view->page()->recentlyAudible())
        return QLatin1String("audible");
    if (DownloadManager::instance()->hasActiveDownloadForPage(view->page()))
        return QLatin1String("download");
    return QString();
}

void TabWidget::suspendIdleTabs(qint64 idleMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < count(); ++i) {
        WebView *view = webView(i);
        if (!view)
            continue;
        const qint64 last = m_sleepStates.value(view).lastActiveMs;
        // No recorded activity (tab predates the bookkeeping) counts
        // as active now, never as long-idle.
        if (last == 0 || now - last < idleMs)
            continue;
        beginTabSleep(i, true);
    }
}

void TabWidget::sleepTab(int index)
{
    if (index < 0)
        index = currentIndex();
    beginTabSleep(index, false);
}

void TabWidget::wakeTab(int index)
{
    if (index < 0)
        index = currentIndex();
    WebView *view = webView(index);
    if (!view || !view->page())
        return;
    auto it = m_sleepStates.find(view);
    if (it == m_sleepStates.end() || !it->sleeping)
        return;
    it->sleeping = false;
    markTabActivity(view);
    // Active out of Discarded makes the engine rebuild the
    // WebContents and reload the page's current entry.
    if (view->page()->lifecycleState()
        == QWebEnginePage::LifecycleState::Discarded)
        view->page()->setLifecycleState(QWebEnginePage::LifecycleState::Active);
    applySleepVisuals(index, false);
}

// The one async round trip before a discard: grab the scroll
// position and probe for unsaved form input in a single evaluation.
// The result lands in finishTabSleepCapture.
void TabWidget::beginTabSleep(int index, bool automatic)
{
    if (index < 0 || index >= count() || index == currentIndex())
        return;
    WebView *view = webView(index);
    if (!view || !view->page() || view->url().isEmpty())
        return;
    TabSleepState &state = m_sleepStates[view];
    if (state.sleeping || state.sleepInFlight)
        return;
    // The explicit "Sleep Tab" menu entry skips the idle exemptions —
    // putting an audio/download tab to sleep is a valid way to stop
    // it — but the unsaved-form refusal applies to both paths (the
    // data loss would be silent and unrecoverable either way).
    if (state.formDirty
        || (automatic && !sleepBlockReason(index).isEmpty()))
        return;
    state.sleepInFlight = true;
    const QString capture = QLatin1String(
        "(function(){var d=false;try{"
        "var els=document.querySelectorAll('input,textarea,select');"
        "for(var i=0;i<els.length;i++){var e=els[i];"
        "var t=(e.type||'').toLowerCase();"
        "if(e.disabled||e.readOnly)continue;"
        "if(t==='hidden'||t==='submit'||t==='button'||t==='reset'"
        "||t==='image'||t==='file')continue;"
        "if(t==='checkbox'||t==='radio'){if(e.checked!==e.defaultChecked)"
        "{d=true;break}}"
        "else if(e.value!==e.defaultValue){d=true;break}}}catch(x){}"
        "return JSON.stringify({x:window.scrollX||0,y:window.scrollY||0,"
        "dirty:d});})()");
    QPointer<WebView> guard(view);
    view->page()->runJavaScript(capture,
        [this, guard, automatic](const QVariant &result) {
        if (guard)
            finishTabSleepCapture(guard.data(), automatic, result);
    });
}

void TabWidget::finishTabSleepCapture(WebView *webView, bool automatic,
                                    const QVariant &result)
{
    auto it = m_sleepStates.find(webView);
    if (it == m_sleepStates.end())
        return;
    it->sleepInFlight = false;
    const int index = webViewIndex(webView);
    if (index < 0 || index == currentIndex() || it->sleeping
        || !webView->page())
        return;

    const QJsonObject captured = QJsonDocument::fromJson(
        result.toString().toUtf8()).object();
    if (captured.value(QLatin1String("dirty")).toBool()) {
        // Latched until the next navigation — a dirty form survives
        // every later sweep without re-running the probe.
        it->formDirty = true;
        if (!automatic)
            emit showStatusBarMessage(
                tr("Tab was not suspended: it has unsaved form input"));
        return;
    }
    // The async round trip gave the page time to start playing audio
    // or kick off a download — re-check the live exemptions.
    if (automatic && !sleepBlockReason(index).isEmpty())
        return;
    it->scrollX = captured.value(QLatin1String("x")).toDouble();
    it->scrollY = captured.value(QLatin1String("y")).toDouble();
    it->restoreScroll = it->scrollX != 0 || it->scrollY != 0;
    it->sleeping = true;
    webView->page()->setLifecycleState(
        QWebEnginePage::LifecycleState::Discarded);
    applySleepVisuals(index, true);
}

// Dim the title and flag the tab so the bar can paint its "zZ"
// badge; undo both on wake.
void TabWidget::applySleepVisuals(int index, bool sleeping)
{
    if (index < 0 || index >= count())
        return;
    const QPalette::ColorGroup group = sleeping ? QPalette::Disabled
                                                : QPalette::Active;
    m_tabBar->setTabTextColor(index,
        m_tabBar->palette().color(group, QPalette::WindowText));
    m_tabBar->update();
}

void TabWidget::reloadAllTabs()
{
    for (int i = 0; i < count(); ++i) {
        if (WebView *tab = webView(i)) {
            tab->reload();
        }
    }
}

void TabWidget::bookmarkTabs()
{
    AddBookmarkDialog dialog;
    dialog.setFolder(true);
    dialog.setTitle(tr("Saved Tabs"));
    dialog.exec();

    BookmarkNode *folder = dialog.addedNode();
    if (!folder)
        return;

    for (int i = 0; i < count(); ++i) {
        WebView *tab = webView(i);
        if (!tab)
            continue;

        QString title = tab->title();
        QString url = QString::fromUtf8(tab->url().toEncoded());
        BookmarkNode *bookmark = new BookmarkNode(BookmarkNode::Bookmark);
        bookmark->url = url;
        bookmark->title = title;
        BrowserApplication::bookmarksManager()->addBookmark(folder, bookmark);
    }
}

void TabWidget::lineEditReturnPressed()
{
    if (QLineEdit *lineEdit = qobject_cast<QLineEdit*>(sender())) {
        OpenUrlIn tab = CurrentTab;
        if (qApp->keyboardModifiers() == Qt::AltModifier)
            tab = NewSelectedTab;

        loadString(lineEdit->text(), tab);
        if (m_locationBars->currentWidget() == lineEdit)
            currentWebView()->setFocus();
    }
}

void TabWidget::windowCloseRequested()
{
    WebPage *webPage = qobject_cast<WebPage*>(sender());
    if (!webPage)
        return;
    // QWebEnginePage has no view() — forPage() is the reverse lookup.
    WebView *webView = qobject_cast<WebView*>(QWebEngineView::forPage(webPage));
    int index = webViewIndex(webView);
    if (index >= 0) {
        if (count() == 1) {
            if (BrowserMainWindow *window = BrowserMainWindow::parentWindow(this))
                window->close();
        } else {
            closeTab(index);
        }
    }
}

void TabWidget::closeOtherTabs(int index)
{
    if (-1 == index)
        return;
    for (int i = count() - 1; i > index; --i)
        closeTab(i);
    for (int i = index - 1; i >= 0; --i)
        closeTab(i);
}

// When index is -1 index chooses the current tab
void TabWidget::cloneTab(int index)
{
    if (index < 0)
        index = currentIndex();
    if (index < 0 || index >= count())
        return;
    QUrl url = webView(index)->url();
    WebView *tab = makeNewTab();
    tab->loadUrl(url);
}

// Qt WebEngine cannot stream a page history into a QDataStream like
// QWebHistory could, so the url stack and current index are serialized
// by hand.  The back/forward stack cannot be injected into a WebEngine
// page afterwards — restoring only reopens the current entry.
static QByteArray serializePageHistory(const QWebEngineHistory *history)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream << qint32(1); // serialization version
    QStringList urls;
    const QList<QWebEngineHistoryItem> items = history->items();
    for (const QWebEngineHistoryItem &item : items)
        urls.append(QString::fromUtf8(item.url().toEncoded()));
    stream << urls;
    stream << qint32(history->currentItemIndex());
    return data;
}

static QUrl currentSerializedHistoryUrl(const QByteArray &data)
{
    QDataStream stream(data);
    qint32 version = 0;
    QStringList urls;
    qint32 currentIndex = -1;
    stream >> version;
    StreamingUtils::readBoundedList(stream, urls);
    stream >> currentIndex;
    if (stream.status() != QDataStream::Ok || version != 1)
        return QUrl();
    return QUrl::fromEncoded(urls.value(currentIndex).toUtf8());
}

// When index is -1 index chooses the current tab
void TabWidget::closeTab(int index)
{
    if (index < 0)
        index = currentIndex();
    if (index < 0 || index >= count())
        return;

    WebView *tab = webView(index);
    bool hasFocus = tab && tab->hasFocus();

    // A private tab is never queued for reopen: "Open Last Closed Tab"
    // would load the url in a normal-profile page where the visit is
    // recorded — the very trace private browsing avoids (SEC07).
    const bool recordable = tab && !tab->url().isEmpty()
        && !(tab->page() && tab->page()->profile()->isOffTheRecord());
    if (recordable) {
        m_recentlyClosedTabsAction->setEnabled(true);
        m_recentlyClosedTabs.prepend(tab->url());
        m_recentlyClosedTabsHistory.prepend(serializePageHistory(tab->history()));
        // CONT02: the container id rides with the entry so "reopen
        // closed tab" returns to the same browsing context.
        m_recentlyClosedTabsContainers.prepend(containerIdForTab(index));
        if (m_recentlyClosedTabs.size() >= TabWidget::m_recentlyClosedTabsSize) {
            m_recentlyClosedTabs.removeLast();
            m_recentlyClosedTabsHistory.removeLast();
            m_recentlyClosedTabsContainers.removeLast();
        }
    }
    QWidget *lineEdit = m_locationBars->widget(index);
    m_locationBars->removeWidget(lineEdit);
    lineEdit->deleteLater();

    QWidget *webViewWithSearch = widget(index);
    removeTab(index);
    m_sleepStates.remove(tab);
    webViewWithSearch->setParent(nullptr);
    webViewWithSearch->deleteLater();

    emit tabsChanged();
    if (hasFocus && count() > 0 && currentWebView())
        currentWebView()->setFocus();
    if (count() == 0)
        emit lastTabClosed();
}

QLabel *TabWidget::animationLabel(int index, bool addMovie)
{
    if (-1 == index)
        return nullptr;
    QTabBar::ButtonPosition side = m_tabBar->freeSide();
    QLabel *loadingAnimation = qobject_cast<QLabel*>(m_tabBar->tabButton(index, side));
    if (!loadingAnimation) {
        loadingAnimation = new QLabel(this);
        // The label alternates between the loading spinner and the
        // page favicon inside a tab; give assistive tools a name for it.
        loadingAnimation->setAccessibleName(tr("Page Icon"));
    }
    if (addMovie && !loadingAnimation->movie()) {
        QMovie *movie = new QMovie(QLatin1String(":graphics/loading.gif"), QByteArray(), loadingAnimation);
        movie->setSpeed(50);
        loadingAnimation->setMovie(movie);
        movie->start();
    }
    m_tabBar->setTabButton(index, side, nullptr);
    m_tabBar->setTabButton(index, side, loadingAnimation);
    return loadingAnimation;
}

void TabWidget::webViewLoadStarted()
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);
    // SLEEP01: a navigation both counts as activity and clears the
    // unsaved-form latch — the page that held the input is gone.
    markTabActivity(webView);
    auto sleepIt = m_sleepStates.find(webView);
    if (sleepIt != m_sleepStates.end())
        sleepIt->formDirty = false;
    if (-1 != index) {
        QLabel *label = animationLabel(index, true);
        if (label->movie())
            label->movie()->start();
    }

    if (index != currentIndex())
        return;

    emit showStatusBarMessage(tr("Loading..."));
}

void TabWidget::webViewLoadProgress(int progress)
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);

    if (index != currentIndex()
        || index < 0)
        return;

    emit showStatusBarMessage(tr("Loading %1%...").arg(progress));
}

void TabWidget::webViewLoadFinished(bool ok)
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);

    // SLEEP01: fresh activity + the scroll restore a waking tab asked
    // for — re-applied once after the reload finishes.
    markTabActivity(webView);
    auto sleepIt = m_sleepStates.find(webView);
    if (sleepIt != m_sleepStates.end() && sleepIt->restoreScroll && ok) {
        sleepIt->restoreScroll = false;
        webView->page()->runJavaScript(QStringLiteral(
            "window.scrollTo(%1, %2);")
            .arg(sleepIt->scrollX).arg(sleepIt->scrollY));
    }

    if (-1 != index) {
        QLabel *label = animationLabel(index, true);
        if (label->movie())
            label->movie()->stop();
#if defined(Q_OS_MACOS)
        QTabBar::ButtonPosition side = m_tabBar->freeSide();
        m_tabBar->setTabButton(index, side, nullptr);
        delete label;
#endif
    }
    webViewIconChanged();

    if (index != currentIndex())
        return;

    if (ok)
        emit showStatusBarMessage(tr("Finished loading"));
    else
        emit showStatusBarMessage(tr("Failed to load"));
}

void TabWidget::webViewIconChanged()
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);
    if (-1 != index) {
#if !defined(Q_OS_MACOS)
        QIcon icon = BrowserApplication::icon(webView->url());
        QLabel *label = animationLabel(index, false);
        QMovie *movie = label->movie();
        delete movie;
        label->setMovie(nullptr);
        label->setPixmap(icon.pixmap(16, 16));
#endif
    }
}

void TabWidget::webViewTitleChanged(const QString &title)
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);
    markTabActivity(webView);
    if (-1 == index)
        return;
    QString tabTitle = title;
    if (title.isEmpty())
        tabTitle = QString::fromUtf8(webView->url().toEncoded());
    // The title is page-controlled: '&' would become a mnemonic on the
    // tab label and markup would render in the tooltip (tooltips are
    // always rich-text capable).
    setTabText(index, SafeText::menu(tabTitle));
    // CONT02: a container tab's tooltip names its container.
    QString toolTip = tabTitle;
    const QString containerId = containerIdForTab(index);
    if (!containerId.isEmpty()) {
        const QString name = ContainerManager::instance()
            ->containerForId(containerId).name;
        if (!name.isEmpty())
            toolTip = QStringLiteral("[%1] %2").arg(name, tabTitle);
    }
    setTabToolTip(index, SafeText::escaped(toolTip));
    if (currentIndex() == index)
        emit setCurrentTitle(title);
    // History title updates are handled by WebPage::init (MIG06).
}

void TabWidget::webViewUrlChanged(const QUrl &url)
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);
    markTabActivity(webView);
    if (-1 == index)
        return;
    m_tabBar->setTabData(index, url);
    emit tabsChanged();
}

void TabWidget::openLastTab()
{
    if (m_recentlyClosedTabs.isEmpty())
        return;
    QUrl url = m_recentlyClosedTabs.takeFirst();
    QByteArray historyState = m_recentlyClosedTabsHistory.takeFirst();
    const QString containerId = m_recentlyClosedTabsContainers.isEmpty()
        ? QString() : m_recentlyClosedTabsContainers.takeFirst();
    if (!historyState.isEmpty())
        createTab(historyState, NewTab, containerId);
    else if (WebView *view = makeNewTabInContainer(containerId, true))
        view->loadUrl(url);
    m_recentlyClosedTabsAction->setEnabled(!m_recentlyClosedTabs.isEmpty());
}

void TabWidget::aboutToShowRecentTabsMenu()
{
    m_recentlyClosedTabsMenu->clear();
    for (int i = 0; i < m_recentlyClosedTabs.count(); ++i) {
        QAction *action = new QAction(m_recentlyClosedTabsMenu);
        action->setData(m_recentlyClosedTabsHistory.at(i));
        QString label = m_recentlyClosedTabs.at(i).toString();
        // CONT02: the container id rides as an action property —
        // data() already carries the serialized history blob — and
        // the container name prefixes the label.
        const QString containerId = m_recentlyClosedTabsContainers.value(i);
        if (!containerId.isEmpty()) {
            action->setProperty("aroraContainerId", containerId);
            const QString name = ContainerManager::instance()
                ->containerForId(containerId).name;
            if (!name.isEmpty())
                label = QStringLiteral("[%1] %2").arg(name, label);
        }
        QIcon icon = BrowserApplication::icon(m_recentlyClosedTabs.at(i));
        action->setIcon(icon);
        action->setText(SafeText::menu(label));
        m_recentlyClosedTabsMenu->addAction(action);
    }
}

void TabWidget::aboutToShowRecentTriggeredAction(QAction *action)
{
    if (!action)
        return;

    QByteArray historyState = action->data().toByteArray();
    if (!historyState.isEmpty())
        createTab(historyState, NewTab,
                  action->property("aroraContainerId").toString());
}

void TabWidget::retranslate()
{
    m_nextTabAction->setText(tr("Show Next Tab"));
    QList<QKeySequence> shortcuts;
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_BraceRight));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_PageDown));
    shortcuts.append(tr("Ctrl-]"));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Less));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Tab));
    m_nextTabAction->setShortcuts(shortcuts);
    m_previousTabAction->setText(tr("Show Previous Tab"));
    shortcuts.clear();
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_BraceLeft));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_PageUp));
    shortcuts.append(tr("Ctrl-["));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Greater));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_Tab));
    m_previousTabAction->setShortcuts(shortcuts);
    m_recentlyClosedTabsAction->setText(tr("Recently Closed Tabs"));
    m_newTabAction->setText(tr("New &Tab"));
    m_closeTabAction->setText(tr("&Close Tab"));
    m_bookmarkTabsAction->setText(tr("Bookmark All Tabs"));
    m_tabBar->updateViewToolBarAction();
}

void TabWidget::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslate();
    QTabWidget::changeEvent(event);
}

/*
    Transform string into a QUrl and then load it.

    When you already have a QUrl call loadUrl()
 */
void TabWidget::loadString(const QString &string, OpenUrlIn tab)
{
    if (string.isEmpty())
        return;

    QUrl url = guessUrlFromString(string);
    loadUrl(url, tab);
}

void TabWidget::loadStringFromUntrustedSource(const QString &string, OpenUrlIn tab)
{
    if (string.isEmpty())
        return;

    const QUrl url = guessUrlFromString(string);
    if (!WebView::isUrlAllowedOnUntrustedInput(url)) {
        qWarning() << "TabWidget: refusing url from an untrusted source:" << url;
        return;
    }
    loadUrl(url, tab);
}

// SRCH01: a whitespace-free token that Qt already resolved to a host
// name navigates only when it actually looks like an address —
// localhost / *.localhost, host:port (localhost:8080, [::1]:8080) or
// a dotted/TLD-shaped name (docs.qt.io, 127.0.0.1).  QUrl::
// fromUserInput() claims ANY single token is a host ("word" ->
// http://word), so the omnibox needs its own address shape check:
// anything else typed alone is a search term.
static bool looksLikeAddress(const QString &text)
{
    const QString host = text.section(QLatin1Char('/'), 0, 0);

    if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0
        || host.endsWith(QLatin1String(".localhost"), Qt::CaseInsensitive))
        return true;

    static const QRegularExpression hostPort(
        QLatin1String("^(\\[[0-9a-fA-F:]+\\]|[A-Za-z0-9._~-]+):\\d+(/.*)?$"));
    if (hostPort.match(text).hasMatch())
        return true;

    const int dot = host.indexOf(QLatin1Char('.'));
    return dot > 0;
}

QUrl TabWidget::guessUrlFromString(const QString &string)
{
    const QString trimmed = string.trimmed();
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();

    // 'keyword terms' engine shortcuts keep first priority.
    QUrl url = manager->convertKeywordSearchToUrl(trimmed);
    if (url.isValid())
        return url;

    url = QUrl::fromUserInput(trimmed);

    if (url.scheme() == QLatin1String("about")
        && url.path() == QLatin1String("home"))
        url = QUrl(QLatin1String("qrc:/startpage.html"));

    QSettings settings;
    settings.beginGroup(QLatin1String("urlloading"));
    // SRCH01: the omnibox searches by default — the checkbox is now an
    // opt-out that restores the historic bare-http guess.
    const bool search =
        settings.value(QLatin1String("searchEngineFallback"), true).toBool();
    // SRCH04: private browsing is app-global in Arora (the flag also
    // covers tor mode) — those contexts search through the configured
    // private engine.
    const bool privateContext = BrowserApplication::isPrivate();
    const auto fallbackUrl = [search, &trimmed, privateContext]() -> QUrl {
        if (search) {
            if (OpenSearchEngine *engine =
                    ToolbarSearch::openSearchManager()
                        ->engineForContext(privateContext)) {
                const QUrl searchUrl = engine->searchUrl(trimmed);
                if (!searchUrl.isEmpty() && searchUrl.isValid())
                    return searchUrl;
            }
        }
        const QString urlString = QLatin1String("http://") + trimmed;
        return QUrl::fromEncoded(urlString.toUtf8(), QUrl::TolerantMode);
    };

    // Input with whitespace or no resolvable address at all ("browser
    // test", "~/foo") can only ever be a search.
    const bool hasSpace = std::any_of(trimmed.cbegin(), trimmed.cend(),
        [](QChar c) { return c.isSpace(); });
    if (trimmed.isEmpty() || hasSpace || url.isEmpty() || !url.isValid())
        return fallbackUrl();

    // An explicit scheme (file:, javascript:, mailto:, qrc:...) or an
    // explicitly typed http(s):// is an address — load it as typed.
    // The untrusted-input scheme gate lives in
    // loadStringFromUntrustedSource / isUrlAllowedOnUntrustedInput.
    const bool guessedHttp =
        (url.scheme() == QLatin1String("http")
         || url.scheme() == QLatin1String("https"))
        && !trimmed.startsWith(QLatin1String("http"), Qt::CaseInsensitive);
    if (!guessedHttp)
        return url;

    if (looksLikeAddress(trimmed))
        return url;

    return fallbackUrl();
}

/*
   Somewhere in the browser interface a users wants to open a url

   By default open this url in the current tab, unless mouse or keyboard
   modifiers are set.
 */
void TabWidget::loadUrlFromUser(const QUrl &url, const QString &title)
{
    loadUrl(url, modifyWithUserBehavior(CurrentTab), title);
}

void TabWidget::loadSettings()
{
    for (int i = 0; i < count(); ++i) {
        WebView *v = webView(i);
        if (v && v->page())
            v->loadSettings();
    }

    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));

    // Tab bar position — the settings combo indexes North/South/
    // West/East in order.  QTabWidget only lays corner widgets out for
    // North/South (on a vertical bar they get zero-size geometry), so
    // the corner buttons hide while the bar is vertical; New Tab stays
    // reachable through the action's shortcut and menus.
    QTabWidget::TabPosition position = North;
    switch (settings.value(QLatin1String("tabBarPosition"), 0).toInt()) {
    case 1: position = South; break;
    case 2: position = West; break;
    case 3: position = East; break;
    default: break;
    }
    setTabPosition(position);
    const bool horizontal = (position == North || position == South);

    bool newTabButtonInRightCorner = settings.value(QLatin1String("newTabButtonInRightCorner"), true).toBool();
#ifndef Q_OS_MACOS
    setCornerWidget(horizontal ? static_cast<QWidget*>(addTabButton) : nullptr,
                    newTabButtonInRightCorner ? Qt::TopRightCorner : Qt::TopLeftCorner);
    addTabButton->setVisible(horizontal);
#endif

    const Qt::Corner closeCorner = newTabButtonInRightCorner ? Qt::TopLeftCorner : Qt::TopRightCorner;
    bool oneCloseButton = settings.value(QLatin1String("oneCloseButton"), false).toBool();
    if (oneCloseButton && horizontal) {
        if (!closeTabButton) {
            closeTabButton = new QToolButton(this);
            closeTabButton->setDefaultAction(m_closeTabAction);
            closeTabButton->setAutoRaise(true);
            closeTabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
        }
        setCornerWidget(closeTabButton, closeCorner);
        closeTabButton->setVisible(true);
    } else {
        setCornerWidget(nullptr, closeCorner);
        if (closeTabButton)
            closeTabButton->setVisible(false);
    }
    // UIP02: per-tab close buttons appear on the current + hovered
    // tab; the single corner close button remains the opt-out.
    m_tabBar->setPerTabCloseButtons(!oneCloseButton);

    // SLEEP01: opt-in idle suspend — disabled unless the user turns
    // it on in Settings > Tabs.
    const bool suspend =
        settings.value(QLatin1String("suspendTabs"), false).toBool();
    const int suspendMinutes =
        settings.value(QLatin1String("suspendTabsMinutes"), 30).toInt();
    m_suspendIdleMs = suspend ? qint64(suspendMinutes) * 60 * 1000 : 0;
    updateSleepTimer();
}

/*
    Replace the openIn behavior with the behavior the user wants.

    // ctrl open in new tab
    // ctrl-shift open in new tab and select
    // ctrl-alt open in new window
 */
TabWidget::OpenUrlIn TabWidget::modifyWithUserBehavior(OpenUrlIn tab) {
    BrowserApplication *application = BrowserApplication::instance();
    if (!application)
        return tab;
    Qt::KeyboardModifiers modifiers = application->eventKeyboardModifiers();
    Qt::MouseButtons buttons = application->eventMouseButtons();
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
    qDebug() << __FUNCTION__ << "start" << modifiers << buttons << tab;
#endif
    if (modifiers & Qt::ControlModifier || buttons == Qt::MiddleButton) {
        if (modifiers & Qt::AltModifier) {
            tab = NewWindow;
        } else {
            QSettings settings;
            settings.beginGroup(QLatin1String("tabs"));
            bool select = settings.value(QLatin1String("selectNewTabs"), false).toBool();
            if (modifiers & Qt::ShiftModifier)
                tab = !select ? NewSelectedTab : NewNotSelectedTab;
            else
                tab = select ? NewSelectedTab : NewNotSelectedTab;
        }
    }
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
    qDebug() << __FUNCTION__ << "end" << modifiers << buttons << tab;
#endif
    application->setEventKeyboardModifiers(Qt::KeyboardModifiers());
    application->setEventMouseButtons(Qt::NoButton);
    return tab;
}

/*
   Somewhere in the browser interface a users wants to open a url in a specific tab.
 */
void TabWidget::loadUrl(const QUrl &url, OpenUrlIn tab, const QString &title)
{
    if (tab == UserOrCurrent) {
        loadUrlFromUser(url, title);
        return;
    }
    if (!url.isValid())
        return;
    WebView *webView = getView(tab, currentWebView());
    if (webView) {
        int index = webViewIndex(webView);
        if (index != -1)
            locationBar(index)->setText(QString::fromUtf8(url.toEncoded()));
        webView->loadUrl(url, title);
    }
}

/*
    Return the view that matches the openIn behavior creating
    a new view/window if necessary.
 */
WebView *TabWidget::getView(OpenUrlIn tab, WebView *currentView)
{
    WebView *webView = nullptr;
    switch (tab) {
        case NewWindow: {
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
            qDebug() << __FUNCTION__ << "NewWindow";
#endif
            // No BrowserApplication (e.g. autotests): fall back to a
            // detached WebView on the source page's profile.
            BrowserApplication *application = BrowserApplication::instance();
            if (!application) {
                WebView *detachedView = new WebView(currentView ? currentView->webPage()->profile()
                                                                : BrowserApplication::webEngineProfile());
                detachedView->setAttribute(Qt::WA_DeleteOnClose);
                detachedView->show();
                webView = detachedView;
            } else {
                // CONT02: a window opened from a container tab keeps
                // the opener's container — the new window's first tab
                // binds to the same container profile.
                BrowserMainWindow *newMainWindow =
                    application->newMainWindowInContainer(
                        currentView ? currentView->containerId()
                                    : ContainerManager::defaultContainerId());
                webView = newMainWindow->currentTab();
            }
            webView->setFocus();
            break;
        }

        case NewSelectedTab: {
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
            qDebug() << __FUNCTION__ << "NewSelectedTab";
#endif
            webView = makeNewTab(true);
            webView->setFocus();
            break;
        }

        case NewNotSelectedTab: {
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
            qDebug() << __FUNCTION__ << "NewNotSelectedTab";
#endif
            webView = makeNewTab(false);
            break;
        }

        case CurrentTab:
        default:
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
            qDebug() << __FUNCTION__ << "CurrentTab";
#endif
            webView = currentView;
            if (!webView)
                return nullptr;
            webView->setFocus();
            break;
    }
    return webView;
}

void TabWidget::nextTab()
{
    int next = currentIndex() + 1;
    if (next == count())
        next = 0;
    setCurrentIndex(next);
}

void TabWidget::previousTab()
{
    int next = currentIndex() - 1;
    if (next < 0)
        next = count() - 1;
    setCurrentIndex(next);
}

static const qint32 TabWidgetMagic = 0xaa;

QByteArray TabWidget::saveState() const
{
    int version = 2; // CONT02: v2 tails the stream with container ids
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);

    stream << qint32(TabWidgetMagic);
    stream << qint32(version);

    QStringList tabs;
    QList<QByteArray> tabsHistory;
    QStringList tabContainers;
    // Private tabs live on the off-the-record profile — their urls and
    // history are never written into the saved session (SEC07).  The
    // current index is remapped onto the filtered list.
    int savedCurrentIndex = -1;
    for (int i = 0; i < count(); ++i) {
        WebView *tab = webView(i);
        if (!tab)
            continue;
        if (tab->page() && tab->page()->profile()->isOffTheRecord())
            continue;
        if (i == currentIndex())
            savedCurrentIndex = tabs.count();
        tabs.append(QString::fromUtf8(tab->url().toEncoded()));
        if (tab->history()->count() != 0)
            tabsHistory.append(serializePageHistory(tab->history()));
        else
            tabsHistory.append(QByteArray());
        tabContainers.append(containerIdForTab(i));
    }
    stream << tabs;
    stream << savedCurrentIndex;
    stream << tabsHistory;
    stream << tabContainers;

    return data;
}

bool TabWidget::restoreState(const QByteArray &state)
{
    QByteArray sd = state;
    QDataStream stream(&sd, QIODevice::ReadOnly);
    if (stream.atEnd())
        return false;

    qint32 marker;
    qint32 v;
    stream >> marker;
    stream >> v;
    if (marker != TabWidgetMagic || v < 1 || v > 2)
        return false;

    QStringList openTabs;
    StreamingUtils::readBoundedList(stream, openTabs);

    int currentTab = -1;
    stream >> currentTab;
    QList<QByteArray> tabHistory;
    StreamingUtils::readBoundedList(stream, tabHistory);
    QStringList tabContainers;
    // CONT02: v2 tails the stream with each tab's container id; a v1
    // session (or a truncated blob) restores to the default container.
    if (v >= 2)
        StreamingUtils::readBoundedList(stream, tabContainers);
    if (stream.status() != QDataStream::Ok)
        return false;

    // The empty placeholder tab a fresh window comes with is only a
    // fit for the first saved tab when its container matches — reusing
    // it for a container tab would put the restored page on the wrong
    // profile, so it is closed after the loop instead.
    bool leftoverPlaceholder = false;
    for (int i = 0; i < openTabs.count(); ++i) {
        QUrl url = QUrl::fromEncoded(openTabs.at(i).toUtf8());
        const QByteArray historyState = tabHistory.value(i);
        if (!historyState.isEmpty()) {
            // The saved history's current entry wins when it parses;
            // an unreadable blob (such as a Qt4-era QWebHistory stream,
            // which has a different layout) falls back to the flat tab
            // url instead of silently dropping the tab.
            const QUrl historyUrl = currentSerializedHistoryUrl(historyState);
            if (historyUrl.isValid())
                url = historyUrl;
        }
        // A container the registry no longer knows (deleted between
        // sessions) degrades to the default container — the tab is
        // never dropped.
        QString containerId = tabContainers.value(i);
        if (!containerId.isEmpty()
            && !ContainerManager::instance()->isContainerId(containerId))
            containerId = ContainerManager::defaultContainerId();
        const bool reusePlaceholder = i == 0
            && currentWebView() && currentWebView()->url() == QUrl()
            && containerIdForTab(currentIndex()) == containerId;
        if (i == 0 && !reusePlaceholder
            && currentWebView() && currentWebView()->url() == QUrl())
            leftoverPlaceholder = true;
        WebView *webView = reusePlaceholder
            ? currentWebView()
            : makeNewTabInContainer(containerId, false);
        if (webView)
            webView->loadUrl(url);
    }
    if (leftoverPlaceholder && count() > 1)
        closeTab(0);
    // The saved index is only selectable once the restored tabs exist —
    // setting it before creating them is a no-op against the single
    // placeholder tab.
    if (currentTab >= 0 && currentTab < count())
        setCurrentIndex(currentTab);
    return true;
}

void TabWidget::createTab(const QByteArray &historyState, TabWidget::OpenUrlIn tab,
                          const QString &containerId)
{
    // Qt WebEngine cannot inject a serialized back/forward stack into a
    // page (there is no QDataStream << QWebEngineHistory), so only the
    // entry that was current when the tab was saved is reopened.
    QUrl url = currentSerializedHistoryUrl(historyState);
    if (!url.isValid())
        return;
    WebView *webView = nullptr;
    switch (tab) {
    case NewNotSelectedTab:
    case NewSelectedTab:
        // CONT02: an explicit container ("" = default) — the recently
        // closed entry's own, not whatever the current tab uses.
        webView = makeNewTabInContainer(containerId, tab == NewSelectedTab);
        break;
    default:
        webView = getView(tab, currentWebView());
        break;
    }
    if (webView)
        webView->loadUrl(url);
}

