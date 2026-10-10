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
#include "engineinterface.h"
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
#include "webenginebackend.h"
#include "webpage.h"
#include "webview.h"
#include "webviewsearch.h"

#include <qabstractproxymodel.h>
#include <qcompleter.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qevent.h>
#include <qimage.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qmovie.h>
#include <qpainter.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qquickwidget.h>
#include <qregularexpression.h>
#include <qsettings.h>
#include <qstackedwidget.h>
#include <qstyle.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qurlquery.h>
#include <qwebenginehistory.h>
#include <qwebengineprofile.h>

#include <qdebug.h>

#include <algorithm>

//#define USERMODIFIEDBEHAVIOR_DEBUG

// Defined near webViewIconChanged() — the tab marker for
// off-the-record pages (PTAB01).
static QPixmap privateBadgedPixmap(const QIcon &icon);

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
    // CONT06: the same change renames/recolors a level-1 header.
    connect(ContainerManager::instance(), &ContainerManager::containersChanged,
            m_tabBar, [this]() {
        m_tabBar->updateGeometry();
        m_tabBar->update();
        syncContainerStrip();
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
        if (QLineEdit *qLineEdit = locationBar(i))
            qLineEdit->setText(qLineEdit->text());
        // Widget tabs (PREFS01) carry no search bar.
        if (WebViewSearch *search = webViewSearch(i))
            search->clear();
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
    // TABGRP01: after every user drag, reconcile the moved tab's group
    // membership with where it landed.
    normalizeTabGroupMove(toIndex);
}

void TabWidget::addWebAction(QAction *action, Engine::StandardAction webAction)
{
    if (!action)
        return;
    m_actions.append(new WebActionMapper(action, webAction, this));
}

void TabWidget::currentChanged(int index)
{
    Q_ASSERT(m_locationBars->count() == count());

    WebView *oldWebView = this->webView(m_locationBars->currentIndex());
    if (oldWebView) {
        disconnect(oldWebView, &WebView::statusBarMessage,
                   this, &TabWidget::showStatusBarMessage);
        disconnect(oldWebView->enginePage(), &Engine::Page::linkHovered,
                   this, &TabWidget::linkHovered);
        disconnect(oldWebView->enginePage(), &Engine::Page::loadProgress,
                   this, &TabWidget::loadProgress);
    }
    // SLEEP01: the outgoing tab's idle clock starts now.
    markTabActivity(oldWebView);
    m_locationBars->setCurrentIndex(index);

    WebView *webView = this->webView(index);
    if (!webView) {
        // PREFS01: a widget tab (Preferences et al.) has no page —
        // the chrome actions unbind, the window title comes from the
        // tab text, and focus lands on the page widget itself.
        for (int i = 0; i < m_actions.count(); ++i) {
            WebActionMapper *mapper = m_actions[i];
            mapper->updateCurrent(nullptr);
        }
        emit setCurrentTitle(tabText(index));
        emit loadProgress(100);
        emit showStatusBarMessage(QString());
        if (QWidget *page = widget(index))
            page->setFocus();
        return;
    }

    connect(webView, &WebView::statusBarMessage,
            this, &TabWidget::showStatusBarMessage);
    connect(webView->enginePage(), &Engine::Page::linkHovered,
            this, &TabWidget::linkHovered);
    connect(webView->enginePage(), &Engine::Page::loadProgress,
            this, &TabWidget::loadProgress);

    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->updateCurrent(webView->enginePage());
    }
    // SLEEP01: activating a suspended tab wakes it (the engine
    // reloads the page).
    markTabActivity(webView);
    if (isTabSleeping(index))
        wakeTab(index);
    emit setCurrentTitle(webView->title());
    emit loadProgress(webView->progress());
    emit showStatusBarMessage(webView->lastStatusBarText());
    if (webView->url().isEmpty() && webView->hasFocus()) {
        m_locationBars->currentWidget()->setFocus();
    } else if (!webView->url().isEmpty()) {
        webView->setFocus();
    }
    // POL02: the tab just became visible — capture its thumbnail once
    // the compositor presents it.
    scheduleThumbnailCapture(webView);
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
    // Widget tabs (PREFS01) park a plain read-only QLineEdit here.
    return qobject_cast<QLineEdit*>(m_locationBars->widget(index));
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

void TabWidget::newPrivateTab()
{
    makeNewPrivateTab(true);
}

WebView *TabWidget::makeNewTab(bool makeCurrent)
{
    // CONT02: child-tab inheritance — the new tab stays in the
    // current tab's container ("" when the strip is empty or the
    // current tab is in the default container).
    return makeNewTabLike(webView(currentIndex()), makeCurrent);
}

// PTAB01: inheritance covers both context dimensions — a tab spawned
// from an off-the-record page stays off-the-record (a "normal" child
// would record the visit, the very leak private browsing avoids), and
// a tab spawned from a container page keeps the container.  Tor never
// inherits to the shared private profile: its tabs are already
// off-the-record on the hardened tor profile.
WebView *TabWidget::makeNewTabLike(WebView *source, bool makeCurrent)
{
    if (source && source->enginePage()
        && source->enginePage()->isOffTheRecord()
        && !BrowserApplication::isTorMode())
        return makeNewPrivateTab(makeCurrent);
    const QString containerId = source
        ? source->containerId()
        : ContainerManager::defaultContainerId();
    return makeNewTabInContainer(containerId, makeCurrent);
}

QString TabWidget::containerIdForTab(int index) const
{
    WebView *view = webView(index);
    if (!view)
        return ContainerManager::defaultContainerId();
    return view->containerId();
}

bool TabWidget::isTabPrivate(int index) const
{
    WebView *view = webView(index);
    return view && view->enginePage()
        && view->enginePage()->isOffTheRecord();
}

WebView *TabWidget::makeNewPrivateTab(bool makeCurrent)
{
    if (BrowserApplication::isTorMode())
        return makeNewTabInContainer(
            ContainerManager::defaultContainerId(), makeCurrent);
    return makeNewTabOnProfile(
        BrowserApplication::privateWebEngineProfile(), makeCurrent);
}

WebView *TabWidget::makeNewTabInContainer(const QString &containerId, bool makeCurrent)
{
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
    return makeNewTabOnProfile(profile, makeCurrent);
}

WebView *TabWidget::makeNewTabOnProfile(QWebEngineProfile *profile, bool makeCurrent)
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
        // PTAB01: the private context is per-tab — suggestions for a
        // private tab's omnibox resolve to the private engine.
        m_omniboxSuggestions->setPrivateContextProvider(
            [this]() { return isTabPrivate(currentIndex()); });
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

    WebView *webView = new WebView(profile);
    locationBar->setWebView(webView);
    Engine::Page *enginePage = webView->enginePage();
    connect(enginePage, &Engine::Page::loadStarted,
            this, &TabWidget::webViewLoadStarted);
    connect(enginePage, &Engine::Page::loadProgress,
            this, &TabWidget::webViewLoadProgress);
    connect(enginePage, &Engine::Page::loadFinished,
            this, &TabWidget::webViewLoadFinished);
    connect(enginePage, &Engine::Page::iconChanged,
            this, [this]() { webViewIconChanged(); });
    connect(enginePage, &Engine::Page::titleChanged,
            this, &TabWidget::webViewTitleChanged);
    connect(enginePage, &Engine::Page::urlChanged,
            this, &TabWidget::webViewUrlChanged);
    connect(webView, &WebView::search,
            this, [this](const QUrl &url, TabWidget::OpenUrlIn tab) { loadUrl(url, tab); });
    connect(enginePage, &Engine::Page::windowCloseRequested,
            this, &TabWidget::windowCloseRequested);
    connect(enginePage, &Engine::Page::printRequested,
            this, [this, webView]() { emit printRequested(webView->page()); });
    // Qt WebEngine does not surface WebKit's window-feature requests
    // (geometryChangeRequested / *VisibilityChangeRequested); window.open
    // chrome handling is internal to Chromium.

    WebViewWithSearch *webViewWithSearch = new WebViewWithSearch(webView, this);
    addTab(webViewWithSearch, tr("Untitled"));
    // PTAB01: mark the private tab immediately — the favicon badge and
    // the tooltip are refreshed from here on by webViewIconChanged()
    // and webViewTitleChanged().
    if (profile->isOffTheRecord()) {
        const int newIndex = indexOf(webViewWithSearch);
        if (QLabel *label = animationLabel(newIndex, false))
            label->setPixmap(privateBadgedPixmap(
                QIcon(QLatin1String(":graphics/defaulticon.png"))));
        setTabToolTip(newIndex, tr("Private tab — visits are not recorded"));
    }
    // SLEEP01: the idle clock starts at creation — a freshly opened
    // background tab is not immediately suspendable.  A fresh record
    // also drops any state a recycled pointer might have inherited.
    m_sleepStates.insert(webView, TabSleepState());
    markTabActivity(webView);
    // CONT06: a tab bound to a non-active header either pulls the
    // strip to its level (when it is raised) or joins that level's
    // detached row in place — a background-opened container tab must
    // not leak into the visible level.
    if (m_twoLevelStrip && !m_containerFilterAdjust) {
        const QString containerId = webView->containerId();
        if (!containerId.isEmpty()
            && !m_containerHeaderOrder.contains(containerId))
            m_containerHeaderOrder.append(containerId);
        if (containerId != m_activeContainerHeader) {
            if (makeCurrent) {
                setActiveContainerHeader(containerId);
            } else {
                const int index = webViewIndex(webView);
                if (index >= 0)
                    detachTabIntoContainerStore(index);
            }
        }
        syncContainerStrip();
    }
    if (makeCurrent)
        setCurrentWidget(webViewWithSearch);

    // webview actions
    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->addChild(webView->enginePage()->action(mapper->webAction()));
    }

    if (count() == 1)
        currentChanged(currentIndex());
    emit tabsChanged();
    return webView;
}

int TabWidget::addWidgetTab(QWidget *page, const QString &title,
                            const QIcon &icon, bool makeCurrent)
{
    // PREFS01: a non-web page lives in the strip like a
    // WebViewWithSearch would.  Its location-bar slot is a read-only
    // echo of the title — the parallel m_locationBars stack must keep
    // one widget per tab, and a real line edit keeps the chrome's
    // Ctrl+L focus path working on it.
    QLineEdit *bar = new QLineEdit(title);
    bar->setReadOnly(true);
    m_locationBars->addWidget(bar);

    const int index = addTab(page, icon, title);
    setTabToolTip(index, title);
    // CONT06: widget tabs live under the default header — a
    // Preferences/History tab raised in a filtered strip switches to
    // it; a background one waits in the default level's store.
    if (m_twoLevelStrip && !m_containerFilterAdjust
        && !m_activeContainerHeader.isEmpty()) {
        if (makeCurrent)
            setActiveContainerHeader(QString());
        else
            detachTabIntoContainerStore(index);
        syncContainerStrip();
    }
    // The filter pass reorders the strip — re-resolve the slot.
    if (makeCurrent)
        setCurrentIndex(indexOf(page));
    if (count() == 1)
        currentChanged(currentIndex());
    emit tabsChanged();
    return index;
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
    // tor refuses containers entirely.  PTAB01: the check is per-tab —
    // a private tab inside a normal window is just as off-limits.
    // Unknown/deleted ids likewise leave the tab alone.
    if (BrowserApplication::isPrivate() || BrowserApplication::isTorMode()
        || isTabPrivate(index))
        return;
    if (!containerId.isEmpty()
        && !ContainerManager::instance()->isContainerId(containerId))
        return;

    const QUrl url = tab->url();
    // TABGRP01: the replacement keeps the old tab's group — a
    // container swap is a re-home, not an ungroup.
    const QString gid = tabGroupId(index);
    WebView *newTab = makeNewTabInContainer(containerId, true);
    if (!newTab)
        return;
    if (m_twoLevelStrip) {
        // CONT06: raising the replacement pulled the strip to the
        // target level — the old tab already detached into its own
        // level's hidden store, so the index captured above is stale.
        // Close it there and leave the new tab at the end of its
        // level's row.
        if (!gid.isEmpty())
            assignTabGroup(newTab, gid);
        if (!url.isEmpty() && url.isValid())
            newTab->loadUrl(url);
        closeHiddenTab(tab);
        return;
    }
    // The fresh tab appended at the end; slide it into the old tab's
    // slot (moveTab emits tabMoved, which keeps m_locationBars in
    // sync) so the strip order survives the swap.
    const int appendedIndex = count() - 1;
    if (appendedIndex > index)
        m_tabBar->moveTab(appendedIndex, index);
    if (!gid.isEmpty())
        assignTabGroup(newTab, gid);
    if (!url.isEmpty() && url.isValid())
        newTab->loadUrl(url);
    // The old tab is now one slot past the new one's position.  When
    // the group was collapsed, closeTab() expands it to expose the
    // hidden members — collapse it again afterwards.
    const bool wasCollapsed = !gid.isEmpty()
        && m_tabGroupInfo.value(gid).collapsed;
    closeTab(index + 1);
    if (wasCollapsed && m_tabGroupInfo.contains(gid))
        setTabGroupCollapsed(gid, true);
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
    // and the File menu both land here; open the settings page
    // straight on its Containers section.
    SettingsDialog::openPage(this, SettingsDialog::ContainersPage);
}

// TABGRP01 — tab groups ---------------------------------------------------
//
// A tab group is a membership set over this widget's tabs: named and
// colored, rendered on the strip as a colored rail plus a name pill on
// the run's first member.  Membership lives in m_tabGroups keyed on the
// WebView so a drag never loses it; group metadata lives in
// m_tabGroupInfo keyed by a per-window id.  A collapsed group detaches
// every member past the first (the "chip") from both the tab widget and
// the location-bar stack — the widgets are kept, not destroyed, and
// re-insert in order on expand.  Groups are deliberately a UI concept:
// they cross container boundaries freely and never touch profiles.

QString TabWidget::nextTabGroupId()
{
    return QStringLiteral("g%1").arg(++m_tabGroupCounter);
}

QString TabWidget::tabGroupId(int index) const
{
    WebView *view = const_cast<TabWidget*>(this)->webView(index);
    return view ? m_tabGroups.value(view) : QString();
}

// Group ids in strip order (first member position) — the order context
// menus present them.
QStringList TabWidget::tabGroupIds() const
{
    QStringList ids;
    for (int i = 0; i < count(); ++i) {
        const QString gid = tabGroupId(i);
        if (!gid.isEmpty() && !ids.contains(gid))
            ids.append(gid);
    }
    return ids;
}

QString TabWidget::tabGroupName(const QString &groupId) const
{
    return m_tabGroupInfo.value(groupId).name;
}

QColor TabWidget::tabGroupColor(const QString &groupId) const
{
    return m_tabGroupInfo.value(groupId).color;
}

int TabWidget::tabGroupSize(const QString &groupId) const
{
    // m_tabGroups covers visible AND collapsed-hidden members.
    int size = 0;
    for (auto it = m_tabGroups.constBegin(); it != m_tabGroups.constEnd(); ++it) {
        if (it.value() == groupId)
            ++size;
    }
    return size;
}

bool TabWidget::tabGroupIsCollapsed(const QString &groupId) const
{
    return m_tabGroupInfo.value(groupId).collapsed;
}

QList<int> TabWidget::tabGroupMembers(const QString &groupId) const
{
    QList<int> members;
    for (int i = 0; i < count(); ++i) {
        if (tabGroupId(i) == groupId)
            members.append(i);
    }
    return members;
}

bool TabWidget::isTabGroupChip(int index) const
{
    const QString gid = tabGroupId(index);
    return !gid.isEmpty() && m_tabGroupInfo.value(gid).collapsed;
}

bool TabWidget::hasCollapsedTabGroup() const
{
    for (const TabGroup &group : m_tabGroupInfo) {
        if (group.collapsed)
            return true;
    }
    return false;
}

// Raw membership assignment — no positioning.  Callers that need
// contiguity (menu adds, drop-stacking) follow with
// moveTabIntoGroupRun(); the drag-normalize path must not.
void TabWidget::assignTabGroup(WebView *webView, const QString &groupId)
{
    if (!webView)
        return;
    if (groupId.isEmpty())
        m_tabGroups.remove(webView);
    else if (m_tabGroupInfo.contains(groupId))
        m_tabGroups.insert(webView, groupId);
}

QString TabWidget::createTabGroup(int index)
{
    WebView *view = webView(index);
    if (!view)
        return QString();
    // The tab may already carry a membership — creating a group for a
    // collapsed group's chip would strand that group's hidden members.
    const QString oldGroup = m_tabGroups.value(view);
    TabGroup group;
    group.id = nextTabGroupId();
    // Groups rotate through the container accent palette — same eight
    // swatches the container UI and the color submenu expose.
    const QList<QColor> palette = ContainerManager::defaultColors();
    group.color = palette.value(m_tabGroupInfo.count() % palette.count());
    m_tabGroupInfo.insert(group.id, group);
    assignTabGroup(view, group.id);
    forgetTabGroupIfEmpty(oldGroup);
    m_tabBar->updateGeometry();
    m_tabBar->update();
    return group.id;
}

// Slide the tab next to the rest of its group when it is not already
// inside the run — membership alone would leave it orphaned at the far
// end of the strip.
void TabWidget::moveTabIntoGroupRun(int index)
{
    const QString gid = tabGroupId(index);
    if (gid.isEmpty())
        return;
    int first = -1;
    int last = -1;
    for (int member : tabGroupMembers(gid)) {
        if (member == index)
            continue;
        if (first < 0)
            first = member;
        last = member;
    }
    if (first < 0)
        return;
    m_groupAdjust = true;
    if (index > last)
        m_tabBar->moveTab(index, last + 1);
    else if (index < first)
        m_tabBar->moveTab(index, first);
    m_groupAdjust = false;
}

void TabWidget::addTabToGroup(int index, const QString &groupId)
{
    auto it = m_tabGroupInfo.find(groupId);
    if (it == m_tabGroupInfo.end())
        return;
    WebView *view = webView(index);
    if (!view)
        return;
    // Re-assigning a member (drop-stacking a grouped tab, or a
    // collapsed group's chip) strands the old group without a visible
    // member — forgetTabGroupIfEmpty re-expands or drops it.
    const QString oldGroup = m_tabGroups.value(view);
    assignTabGroup(view, groupId);
    if (oldGroup != groupId)
        forgetTabGroupIfEmpty(oldGroup);
    if (it->collapsed) {
        // A collapsed group swallows the new member — the chip is the
        // only tab that stays visible.  A member landing left of the
        // chip would itself become the first member, so slide it
        // behind the chip first.
        QList<int> members = tabGroupMembers(groupId);
        if (members.count() > 1 && members.first() == index) {
            m_groupAdjust = true;
            m_tabBar->moveTab(index, members.at(1));
            m_groupAdjust = false;
            index = webViewIndex(view);
            members = tabGroupMembers(groupId);
        }
        if (index == currentIndex() && !members.isEmpty()
            && members.first() != index)
            setCurrentIndex(members.first());
        if (members.count() > 1)
            detachGroupMember(index);
    } else {
        moveTabIntoGroupRun(index);
    }
    m_tabBar->updateVisibility();
    m_tabBar->updateGeometry();
    m_tabBar->update();
}

void TabWidget::removeTabFromGroup(int index)
{
    WebView *view = webView(index);
    if (!view)
        return;
    const QString gid = m_tabGroups.value(view);
    if (gid.isEmpty())
        return;
    m_tabGroups.remove(view);
    forgetTabGroupIfEmpty(gid);
    m_tabBar->updateGeometry();
    m_tabBar->update();
}

void TabWidget::groupTabWith(int index, int targetIndex)
{
    if (index == targetIndex || index < 0 || targetIndex < 0
        || index >= count() || targetIndex >= count())
        return;
    QString gid = tabGroupId(targetIndex);
    if (gid.isEmpty())
        gid = createTabGroup(targetIndex);
    if (!gid.isEmpty())
        addTabToGroup(index, gid);
}

void TabWidget::renameTabGroup(const QString &groupId, const QString &name)
{
    auto it = m_tabGroupInfo.find(groupId);
    if (it == m_tabGroupInfo.end())
        return;
    it->name = name.trimmed();
    m_tabBar->updateGeometry();
    m_tabBar->update();
}

void TabWidget::setTabGroupColor(const QString &groupId, const QColor &color)
{
    auto it = m_tabGroupInfo.find(groupId);
    if (it == m_tabGroupInfo.end())
        return;
    it->color = color;
    m_tabBar->update();
}

void TabWidget::inheritTabGroup(WebView *webView, WebView *opener)
{
    const QString gid = m_tabGroups.value(opener);
    if (gid.isEmpty() || !webView)
        return;
    assignTabGroup(webView, gid);
    const int index = webViewIndex(webView);
    if (index < 0)
        return;
    if (m_tabGroupInfo.value(gid).collapsed)
        detachGroupMember(index);
    else
        moveTabIntoGroupRun(index);
}

// Group cleanup after a member left: a collapsed group whose last
// visible member departed has no chip to click — expand it so the
// hidden members return to the strip rather than staying invisible
// forever.  Then drop the record once no view claims it, so group
// names and colors never outlive their last member.
void TabWidget::forgetTabGroupIfEmpty(const QString &groupId)
{
    if (groupId.isEmpty())
        return;
    auto it = m_tabGroupInfo.find(groupId);
    if (it == m_tabGroupInfo.end())
        return;
    if (it->collapsed && tabGroupMembers(groupId).isEmpty())
        setTabGroupCollapsed(groupId, false);
    if (tabGroupSize(groupId) == 0)
        m_tabGroupInfo.remove(groupId);
    m_tabBar->update();
}

// Detaches the tab at index from both the tab widget and the location
// bar stack into its (collapsed) group's hidden list.  The widgets are
// kept — never deleted — and re-inserted in order by expandTabGroup().
// The group's first visible member is always kept as the chip.
void TabWidget::detachGroupMember(int index)
{
    WebView *view = webView(index);
    if (!view)
        return;
    const QString gid = m_tabGroups.value(view);
    auto it = m_tabGroupInfo.find(gid);
    if (it == m_tabGroupInfo.end() || !it->collapsed)
        return;
    const QList<int> members = tabGroupMembers(gid);
    if (members.count() <= 1 || members.first() == index)
        return;
    // The chip keeps focus — a member about to be hidden cannot stay
    // current.
    if (index == currentIndex())
        setCurrentIndex(members.first());

    HiddenGroupTab hidden;
    hidden.tab = widget(index);
    hidden.bar = m_locationBars->widget(index);
    hidden.text = tabText(index);
    hidden.toolTip = tabToolTip(index);
    hidden.data = m_tabBar->tabData(index);
    // The transient tab-button widgets (favicon label, close button)
    // are dropped and re-created on expand through tabInserted() plus
    // the icon refresh — detaching them first keeps removeTab() from
    // deleting widgets we still reference.
    const QTabBar::ButtonPosition sides[2] = { QTabBar::LeftSide,
                                               QTabBar::RightSide };
    for (const QTabBar::ButtonPosition side : sides) {
        if (QWidget *button = m_tabBar->tabButton(index, side)) {
            m_tabBar->setTabButton(index, side, nullptr);
            button->deleteLater();
        }
    }
    m_locationBars->removeWidget(hidden.bar);
    removeTab(index);
    it->hidden.append(hidden);
}

void TabWidget::expandTabGroup(const QString &groupId)
{
    auto it = m_tabGroupInfo.find(groupId);
    if (it == m_tabGroupInfo.end())
        return;
    const QList<HiddenGroupTab> hidden = it->hidden;
    it->hidden.clear();
    if (hidden.isEmpty())
        return;
    const QList<int> members = tabGroupMembers(groupId);
    int insertPos = members.isEmpty() ? count() : members.first() + 1;
    for (const HiddenGroupTab &entry : hidden) {
        const int idx = insertTab(insertPos, entry.tab, entry.text);
        setTabToolTip(idx, entry.toolTip);
        m_tabBar->setTabData(idx, entry.data);
        m_locationBars->insertWidget(idx, entry.bar);
#if !defined(Q_OS_MACOS)
        // Rebuild the favicon label — the icon is URL-derived so it is
        // re-resolved rather than snapshotted.
        if (WebViewWithSearch *withSearch =
                qobject_cast<WebViewWithSearch*>(entry.tab)) {
            QLabel *label = animationLabel(idx, false);
            label->setPixmap(BrowserApplication::icon(
                withSearch->m_webView->url()).pixmap(16, 16));
        }
#endif
        ++insertPos;
    }
    // CONT06: a member re-appearing under a filtered-out level goes
    // straight back to that level's store.
    if (m_twoLevelStrip)
        applyContainerFilter();
}

void TabWidget::setTabGroupCollapsed(const QString &groupId, bool collapsed)
{
    auto it = m_tabGroupInfo.find(groupId);
    if (it == m_tabGroupInfo.end() || it->collapsed == collapsed)
        return;
    if (!collapsed) {
        it->collapsed = false;
        expandTabGroup(groupId);
    } else {
        const QList<int> members = tabGroupMembers(groupId);
        if (members.isEmpty())
            return;
        it->collapsed = true;
        // The chip keeps focus — a member about to be hidden cannot
        // stay current.
        if (members.contains(currentIndex())
            && currentIndex() != members.first())
            setCurrentIndex(members.first());
        // Detach in strip order — the hidden list is the expand order,
        // so it must match what the user saw.  Snapshot the views up
        // front: each detach shifts the strip, so the captured member
        // indices would skip tabs if reused directly.
        QList<WebView*> memberViews;
        for (int k = 1; k < members.count(); ++k)
            memberViews.append(webView(members.at(k)));
        for (WebView *memberView : std::as_const(memberViews)) {
            const int memberIndex = webViewIndex(memberView);
            if (memberIndex >= 0)
                detachGroupMember(memberIndex);
        }
    }
    m_tabBar->updateVisibility();
    m_tabBar->updateGeometry();
    m_tabBar->update();
}

void TabWidget::ungroupTabs(const QString &groupId)
{
    if (!m_tabGroupInfo.contains(groupId))
        return;
    if (m_tabGroupInfo.value(groupId).collapsed)
        setTabGroupCollapsed(groupId, false);
    for (auto it = m_tabGroups.begin(); it != m_tabGroups.end();) {
        if (it.value() == groupId)
            it = m_tabGroups.erase(it);
        else
            ++it;
    }
    m_tabGroupInfo.remove(groupId);
    m_tabBar->updateGeometry();
    m_tabBar->update();
}

// Post-drag membership normalization, called from moveTab() after the
// location-bar stack is re-synced: a tab dropped between two members of
// the same group joins it, a grouped tab dropped with no same-group
// neighbor left or right leaves its group (Chrome's drag-out rule).
// Both rules are one-line membership changes — no further moving.
void TabWidget::normalizeTabGroupMove(int movedIndex)
{
    if (m_groupAdjust)
        return;
    const QString gid = tabGroupId(movedIndex);
    const QString left = tabGroupId(movedIndex - 1);
    const QString right = tabGroupId(movedIndex + 1);
    if (!left.isEmpty() && left == right && gid != left) {
        assignTabGroup(webView(movedIndex), left);
        m_tabBar->updateGeometry();
        m_tabBar->update();
    } else if (!gid.isEmpty() && left != gid && right != gid) {
        assignTabGroup(webView(movedIndex), QString());
        forgetTabGroupIfEmpty(gid);
        m_tabBar->update();
    }
}

// Strip order with each collapsed group's hidden members spliced in
// directly after its chip — the order saveState() serializes and
// restoreState() re-creates.
QList<WebView*> TabWidget::orderedWebViews() const
{
    QList<WebView*> ordered;
    for (int i = 0; i < count(); ++i) {
        WebView *view = const_cast<TabWidget*>(this)->webView(i);
        if (!view)
            continue;
        ordered.append(view);
        const QString gid = m_tabGroups.value(view);
        if (gid.isEmpty())
            continue;
        const TabGroup group = m_tabGroupInfo.value(gid);
        if (!group.collapsed)
            continue;
        const QList<int> members = tabGroupMembers(gid);
        if (members.isEmpty() || members.first() != i)
            continue;
        for (const HiddenGroupTab &hidden : group.hidden) {
            if (WebViewWithSearch *withSearch =
                    qobject_cast<WebViewWithSearch*>(hidden.tab))
                ordered.append(withSearch->m_webView);
        }
    }
    // CONT06: tabs filtered off the strip by a non-active level append
    // after the visible row, grouped by their header in strip order —
    // every tab serializes.  A container-hidden collapsed-group chip
    // still carries its hidden members with it, or they would drop
    // out of the session entirely.
    for (const QString &containerId : containerHeaders()) {
        for (const HiddenGroupTab &hidden : m_containerHidden.value(containerId)) {
            WebViewWithSearch *withSearch =
                qobject_cast<WebViewWithSearch*>(hidden.tab);
            if (!withSearch)
                continue;
            ordered.append(withSearch->m_webView);
            const QString gid = m_tabGroups.value(withSearch->m_webView);
            const TabGroup group = m_tabGroupInfo.value(gid);
            if (gid.isEmpty() || !group.collapsed)
                continue;
            for (const HiddenGroupTab &member : group.hidden) {
                if (WebViewWithSearch *memberSearch =
                        qobject_cast<WebViewWithSearch*>(member.tab))
                    ordered.append(memberSearch->m_webView);
            }
        }
    }
    return ordered;
}

// Defined further down — closeHiddenTab() records the closed page's
// history for "reopen closed tab" the same way closeTab() does.
static QByteArray serializePageHistory(Engine::Page *page);

// CONT06 — two-level container strip ---------------------------------
//
// The level-1 row lives inside TabBar's top band (it paints and
// hit-tests the headers itself); this side owns the model: which
// header is active, which tabs each header holds, and the detach /
// re-insert mechanics that swap the strip's contents when the active
// header changes.  Detaching reuses the collapsed-group pattern —
// the page widget, its location bar and the tab visuals are kept and
// restored verbatim, so pages in filtered levels keep loading and
// retain their state.

bool TabWidget::twoLevelStrip() const
{
    return m_twoLevelStrip;
}

QString TabWidget::activeContainerHeader() const
{
    return m_activeContainerHeader;
}

int TabWidget::containerTabCount(const QString &containerId) const
{
    int total = m_containerHidden.value(containerId).size();
    for (int i = 0; i < count(); ++i) {
        if (const_cast<TabWidget*>(this)->containerIdForTab(i)
            == containerId)
            ++total;
    }
    // Collapsed-group members sit in a second hidden store — a
    // level's badge counts them even when the whole level is already
    // filtered out.
    for (const TabGroup &group : m_tabGroupInfo) {
        for (const HiddenGroupTab &hidden : group.hidden) {
            if (WebViewWithSearch *withSearch =
                    qobject_cast<WebViewWithSearch*>(hidden.tab)) {
                if (withSearch->m_webView->containerId() == containerId)
                    ++total;
            }
        }
    }
    return total;
}

QStringList TabWidget::containerHeaders() const
{
    // The default header always exists; every other container earns a
    // header only while it owns at least one tab (a level empties out
    // of the strip entirely — there is no pinned state yet).
    QStringList headers;
    headers.append(ContainerManager::defaultContainerId());
    ContainerManager *manager = ContainerManager::instance();
    for (const QString &id : std::as_const(m_containerHeaderOrder)) {
        if (containerTabCount(id) > 0 && manager->isContainerId(id)
            && !headers.contains(id))
            headers.append(id);
    }
    // A container that gained tabs without an order entry yet (a
    // restored session, a diverted load) lands at the end in
    // registry order.
    for (const ContainerManager::Container &container : manager->containers()) {
        if (containerTabCount(container.id) > 0
            && !headers.contains(container.id))
            headers.append(container.id);
    }
    return headers;
}

bool TabWidget::containerStripActive() const
{
    return m_twoLevelStrip && containerHeaders().count() > 1;
}

int TabWidget::totalTabCount() const
{
    int total = count();
    for (const QList<HiddenGroupTab> &hidden : m_containerHidden)
        total += hidden.size();
    for (const TabGroup &group : m_tabGroupInfo)
        total += group.hidden.size();
    return total;
}

// Rebuild the strip so it shows exactly m_activeContainerHeader's
// tabs: everything else detaches into its own container's list (in
// strip order), then the active container's detached tabs re-insert
// in the order they left.
void TabWidget::applyContainerFilter()
{
    if (!m_twoLevelStrip || m_containerFilterAdjust)
        return;
    m_containerFilterAdjust = true;
    for (int i = 0; i < count();) {
        if (containerIdForTab(i) != m_activeContainerHeader)
            detachTabIntoContainerStore(i);
        else
            ++i;
    }
    restoreHiddenContainerTabs(m_activeContainerHeader);
    m_containerFilterAdjust = false;
    if (currentIndex() < 0 && count() > 0)
        setCurrentIndex(0);
    m_tabBar->updateGeometry();
    updateGeometry();
    m_tabBar->updateAccessibleStrip();
    m_tabBar->update();
    // Header switches change the persisted session (the active level
    // is serialized) — the AutoSaver hook needs to know.
    emit tabsChanged();
}

// The detach half of a filter pass — identical mechanics to a
// collapsed group's hidden member: the page widget and its location
// bar are kept, the transient tab buttons are dropped (tabInserted()
// re-creates them on the way back).
void TabWidget::detachTabIntoContainerStore(int index)
{
    QWidget *page = widget(index);
    if (!page)
        return;
    const QString containerId = containerIdForTab(index);
    HiddenGroupTab hidden;
    hidden.tab = page;
    hidden.bar = m_locationBars->widget(index);
    hidden.text = tabText(index);
    hidden.toolTip = tabToolTip(index);
    hidden.data = m_tabBar->tabData(index);
    const QTabBar::ButtonPosition sides[2] = { QTabBar::LeftSide,
                                               QTabBar::RightSide };
    for (const QTabBar::ButtonPosition side : sides) {
        if (QWidget *button = m_tabBar->tabButton(index, side)) {
            m_tabBar->setTabButton(index, side, nullptr);
            button->deleteLater();
        }
    }
    m_locationBars->removeWidget(hidden.bar);
    removeTab(index);
    m_containerHidden[containerId].append(hidden);
}

void TabWidget::restoreHiddenContainerTabs(const QString &containerId)
{
    const QList<HiddenGroupTab> hidden =
        m_containerHidden.take(containerId);
    int insertPos = count();
    for (const HiddenGroupTab &entry : hidden) {
        const int idx = insertTab(insertPos, entry.tab, entry.text);
        setTabToolTip(idx, entry.toolTip);
        m_tabBar->setTabData(idx, entry.data);
        m_locationBars->insertWidget(idx, entry.bar);
#if !defined(Q_OS_MACOS)
        if (WebViewWithSearch *withSearch =
                qobject_cast<WebViewWithSearch*>(entry.tab)) {
            QLabel *label = animationLabel(idx, false);
            label->setPixmap(BrowserApplication::icon(
                withSearch->m_webView->url()).pixmap(16, 16));
        }
#endif
        ++insertPos;
    }
}

void TabWidget::setActiveContainerHeader(const QString &containerId)
{
    QString resolved = containerId;
    if (!resolved.isEmpty()
        && !ContainerManager::instance()->isContainerId(resolved))
        resolved = ContainerManager::defaultContainerId();
    if (m_activeContainerHeader == resolved)
        return;
    if (WebView *current = currentWebView())
        m_lastActiveInHeader[m_activeContainerHeader] = current;
    m_activeContainerHeader = resolved;
    applyContainerFilter();
    // Land on the level's last current tab when it is still around.
    if (WebView *preferred = m_lastActiveInHeader.value(resolved)) {
        const int index = webViewIndex(preferred);
        if (index >= 0)
            setCurrentIndex(index);
    }
}

void TabWidget::moveContainerHeader(int from, int to)
{
    // Header indices include the pinned default header at 0 — the
    // order list only carries the movable non-default entries.
    --from;
    --to;
    // Re-sync the order list with the live headers first — it only
    // grows at tab-creation time, so a restored session (or any path
    // that skipped registration) would silently make every move a
    // no-op.
    const QStringList headers = containerHeaders();
    QStringList order;
    for (const QString &id : std::as_const(m_containerHeaderOrder)) {
        if (headers.contains(id))
            order.append(id);
    }
    for (const QString &id : headers) {
        if (!id.isEmpty() && !order.contains(id))
            order.append(id);
    }
    m_containerHeaderOrder = order;
    if (from < 0 || to < 0 || from >= m_containerHeaderOrder.count()
        || to >= m_containerHeaderOrder.count() || from == to)
        return;
    m_containerHeaderOrder.move(from, to);
    m_tabBar->update();
}

// Called after tab-strip mutations that can change the level model —
// a new tab's container registration and the "last tab of the active
// header closed" fallback.  Filtering itself only runs through
// setActiveContainerHeader/applyContainerFilter.
void TabWidget::syncContainerStrip()
{
    if (!m_twoLevelStrip || m_containerFilterAdjust)
        return;
    if (containerTabCount(m_activeContainerHeader) == 0) {
        // The active level emptied — fall back to the default header,
        // or to whatever still has tabs.
        QString fallback = ContainerManager::defaultContainerId();
        if (containerTabCount(fallback) == 0) {
            const QStringList headers = containerHeaders();
            fallback = headers.count() > 1 ? headers.at(1)
                                         : fallback;
        }
        m_activeContainerHeader = fallback;
        applyContainerFilter();
    }
    m_tabBar->updateGeometry();
    updateGeometry();
    m_tabBar->updateAccessibleStrip();
    m_tabBar->update();
}

// The hidden-store counterpart of closeTab()'s tail — a tab that
// detached into a non-active level still needs the recently-closed
// entry and the bookkeeping teardown when it dies off-strip (the
// reopen-in-container swap is the one caller today).
void TabWidget::closeHiddenTab(WebView *view)
{
    for (auto it = m_containerHidden.begin();
         it != m_containerHidden.end(); ++it) {
        for (int k = 0; k < it->size(); ++k) {
            WebViewWithSearch *withSearch =
                qobject_cast<WebViewWithSearch*>(it->at(k).tab);
            if (!withSearch || withSearch->m_webView != view)
                continue;
            if (view && !view->url().isEmpty()
                && !(view->enginePage()
                     && view->enginePage()->isOffTheRecord())) {
                m_recentlyClosedTabsAction->setEnabled(true);
                m_recentlyClosedTabs.prepend(view->url());
                m_recentlyClosedTabsHistory.prepend(
                    serializePageHistory(view->enginePage()));
                m_recentlyClosedTabsContainers.prepend(it.key());
                if (m_recentlyClosedTabs.size()
                    >= TabWidget::m_recentlyClosedTabsSize) {
                    m_recentlyClosedTabs.removeLast();
                    m_recentlyClosedTabsHistory.removeLast();
                    m_recentlyClosedTabsContainers.removeLast();
                }
            }
            HiddenGroupTab hidden = it->takeAt(k);
            hidden.bar->deleteLater();
            const QString closingGroup = m_tabGroups.take(view);
            hidden.tab->deleteLater();
            m_sleepStates.remove(view);
            m_tabThumbnails.remove(view);
            forgetTabGroupIfEmpty(closingGroup);
            emit tabsChanged();
            return;
        }
    }
}

void TabWidget::setTwoLevelStrip(bool enabled)
{
    if (m_twoLevelStrip == enabled)
        return;
    m_twoLevelStrip = enabled;
    if (enabled) {
        // Keep showing what the user is looking at — the current
        // tab's container becomes the active header.
        m_activeContainerHeader = containerIdForTab(currentIndex());
        applyContainerFilter();
    } else {
        // Fold every hidden level back onto the strip.  Tabs append
        // in header order — per-container order is preserved, the
        // original cross-container interleave is not.
        m_activeContainerHeader = ContainerManager::defaultContainerId();
        for (const QString &containerId : containerHeaders())
            restoreHiddenContainerTabs(containerId);
        m_containerHidden.clear();
    }
    syncContainerStrip();
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
    if (!view || !view->enginePage() || view->url().isEmpty())
        return QLatin1String("empty");
    const TabSleepState state = m_sleepStates.value(view);
    if (state.sleeping
        || view->enginePage()->lifecycleState()
               == Engine::Page::LifecycleState::Discarded)
        return QLatin1String("sleeping");
    if (state.sleepInFlight)
        return QLatin1String("inflight");
    if (state.formDirty)
        return QLatin1String("form");
    if (view->enginePage()->isLoading())
        return QLatin1String("loading");
    if (view->enginePage()->recentlyAudible())
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
    if (!view || !view->enginePage())
        return;
    auto it = m_sleepStates.find(view);
    if (it == m_sleepStates.end() || !it->sleeping)
        return;
    it->sleeping = false;
    markTabActivity(view);
    // Active out of Discarded makes the engine rebuild the
    // WebContents and reload the page's current entry.
    if (view->enginePage()->lifecycleState()
        == Engine::Page::LifecycleState::Discarded)
        view->enginePage()->setLifecycleState(Engine::Page::LifecycleState::Active);
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
    if (!view || !view->enginePage() || view->url().isEmpty())
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
    view->enginePage()->runJavaScript(capture,
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
        || !webView->enginePage())
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
    webView->enginePage()->setLifecycleState(
        Engine::Page::LifecycleState::Discarded);
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

// POL02: QtWebEngine paints each page into a QQuickWidget delegate
// inside the view; its framebuffer still holds the last compositor
// frame where QWidget::grab() on the outer view only yields the
// widget's blank background (background tabs, offscreen platform).
static QImage webViewThumbnailFrame(WebView *view)
{
    if (QQuickWidget *delegate = view->findChild<QQuickWidget *>()) {
        const QImage frame = delegate->grabFramebuffer();
        if (!frame.isNull())
            return frame;
    }
    return view->grab().toImage();
}

// A frame that is uniformly near-white (or fully transparent) is what
// the compositor offers when it has nothing real to show — never
// rendered, evicted, discarded, or no compositing on this platform —
// so the caller keeps the cached snapshot or the icon fallback.
// Uniform-but-coloured pages are legitimate frames and pass.
static bool blankThumbnailFrame(const QImage &image)
{
    if (image.isNull())
        return true;
    const QImage sample = image.scaled(
        4, 4, Qt::IgnoreAspectRatio, Qt::FastTransformation);
    for (int y = 0; y < sample.height(); ++y) {
        for (int x = 0; x < sample.width(); ++x) {
            const QRgb px = sample.pixel(x, y);
            if (qAlpha(px) < 16)
                continue;
            if (qRed(px) < 240 || qGreen(px) < 240 || qBlue(px) < 240)
                return false;
        }
    }
    // Either everything was transparent, or everything was near-white.
    return true;
}

// Grab the tab's view into the thumbnail cache.  Only the current
// tab's view is actually visible inside the stack, so the scheduled
// captures below only ever fire there — request-time grabbing for
// background tabs lives in tabThumbnail().
void TabWidget::captureTabThumbnail(int index)
{
    WebView *view = webView(index);
    if (!view || !view->isVisible())
        return;
    const QImage frame = webViewThumbnailFrame(view);
    if (blankThumbnailFrame(frame))
        return;
    m_tabThumbnails.insert(view, QPixmap::fromImage(
        frame.scaledToWidth(384, Qt::SmoothTransformation)));
}

void TabWidget::scheduleThumbnailCapture(WebView *webView)
{
    QPointer<WebView> guard(webView);
    QTimer::singleShot(300, this, [this, guard]() {
        if (guard && currentWebView() == guard.data())
            captureTabThumbnail(currentIndex());
    });
}

QPixmap TabWidget::tabThumbnail(int index)
{
    WebView *view = webView(index);
    if (!view)
        return QPixmap();
    // A hidden WebEngine view usually still paints its last compositor
    // frame on grab(), so preview-time grabbing works for background
    // tabs too — the blank check catches the platforms where it
    // doesn't and keeps the cached snapshot instead.
    const QImage frame = webViewThumbnailFrame(view);
    if (!frame.isNull() && !blankThumbnailFrame(frame)) {
        const QPixmap thumb = QPixmap::fromImage(
            frame.scaledToWidth(384, Qt::SmoothTransformation));
        m_tabThumbnails.insert(view, thumb);
        return thumb;
    }
    return m_tabThumbnails.value(view);
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

    // orderedWebViews() covers the filtered container levels and
    // collapsed-group members too — "all tabs" means the window, not
    // just the visible row.
    for (WebView *tab : orderedWebViews()) {
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
        if (m_locationBars->currentWidget() == lineEdit) {
            // A widget tab (PREFS01) has no view to focus.
            if (WebView *view = currentWebView())
                view->setFocus();
        }
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
        // CONT06: hidden container levels still hold live tabs — the
        // window should only close when nothing is left anywhere.
        if (totalTabCount() == 1) {
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
    // Widget tabs (PREFS01) have no page to clone.
    WebView *source = webView(index);
    if (!source)
        return;
    QUrl url = source->url();
    // TABGRP01: a clone stays in the cloned tab's group.
    const QString gid = tabGroupId(index);
    // PTAB01: a clone stays in the cloned tab's context — duplicating
    // a private tab keeps the copy off-the-record (a normal-profile
    // clone would record the url it reloads).
    WebView *tab = isTabPrivate(index)
        ? makeNewPrivateTab()
        : makeNewTab();
    if (!gid.isEmpty()) {
        assignTabGroup(tab, gid);
        const int tabIndex = webViewIndex(tab);
        if (m_tabGroupInfo.value(gid).collapsed)
            detachGroupMember(tabIndex);
        else
            moveTabIntoGroupRun(tabIndex);
    }
    tab->loadUrl(url);
}

// Qt WebEngine cannot stream a page history into a QDataStream like
// QWebHistory could, so the url stack and current index are serialized
// by hand.  The back/forward stack cannot be injected into a WebEngine
// page afterwards — restoring only reopens the current entry.
static QByteArray serializePageHistory(Engine::Page *page)
{
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);
    stream << qint32(1); // serialization version
    QStringList urls;
    const QList<Engine::HistoryEntry> items = page->historyItems();
    for (const Engine::HistoryEntry &item : items)
        urls.append(QString::fromUtf8(item.url.toEncoded()));
    stream << urls;
    stream << qint32(page->currentHistoryIndex());
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

    // TABGRP01: the chip is the only visible member of a collapsed
    // group — closing it expands the group first so its hidden members
    // re-appear instead of being stranded off-strip.
    const QString closingGroup = tabGroupId(index);
    if (!closingGroup.isEmpty()
        && m_tabGroupInfo.value(closingGroup).collapsed)
        setTabGroupCollapsed(closingGroup, false);
    // Expansion re-inserts hidden members right after the group's
    // first visible tab — when that is not the tab being closed (the
    // reopen-in-container swap creates exactly this state) the strip
    // index captured above has shifted, so re-resolve it from the view.
    if (tab) {
        index = webViewIndex(tab);
        if (index < 0 || index >= count())
            return;
    }

    // A private tab is never queued for reopen: "Open Last Closed Tab"
    // would load the url in a normal-profile page where the visit is
    // recorded — the very trace private browsing avoids (SEC07).
    const bool recordable = tab && !tab->url().isEmpty()
        && !(tab->enginePage() && tab->enginePage()->isOffTheRecord());
    if (recordable) {
        m_recentlyClosedTabsAction->setEnabled(true);
        m_recentlyClosedTabs.prepend(tab->url());
        m_recentlyClosedTabsHistory.prepend(serializePageHistory(tab->enginePage()));
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
    m_tabThumbnails.remove(tab);
    // TABGRP01: drop the view's membership; the group record dies with
    // its last member.
    m_tabGroups.remove(tab);
    forgetTabGroupIfEmpty(closingGroup);
    webViewWithSearch->setParent(nullptr);
    webViewWithSearch->deleteLater();

    // CONT06: the active level may have emptied — fall back and
    // refill the strip before the zero-count check below decides
    // lastTabClosed (filtered-out tabs still count as open).
    syncContainerStrip();

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

// The tab a page signal arrived from.  Senders are either the WebView
// itself or — through the ENG04 adapter — the page's Engine::Page
// wrapper; forPage() is the engine's reverse lookup (the same path
// windowCloseRequested uses below).
static WebView *webViewForSender(QObject *sender)
{
    if (WebView *view = qobject_cast<WebView*>(sender))
        return view;
    if (WebEnginePageAdapter *adapter =
            qobject_cast<WebEnginePageAdapter*>(sender))
        return qobject_cast<WebView*>(
                QWebEngineView::forPage(adapter->webEnginePage()));
    return nullptr;
}

void TabWidget::webViewLoadStarted()
{
    WebView *webView = webViewForSender(sender());
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
    WebView *webView = webViewForSender(sender());
    int index = webViewIndex(webView);

    if (index != currentIndex()
        || index < 0)
        return;

    emit showStatusBarMessage(tr("Loading %1%...").arg(progress));
}

void TabWidget::webViewLoadFinished(bool ok)
{
    WebView *webView = webViewForSender(sender());
    int index = webViewIndex(webView);

    // SLEEP01: fresh activity + the scroll restore a waking tab asked
    // for — re-applied once after the reload finishes.
    markTabActivity(webView);
    auto sleepIt = m_sleepStates.find(webView);
    if (sleepIt != m_sleepStates.end() && sleepIt->restoreScroll && ok) {
        sleepIt->restoreScroll = false;
        webView->enginePage()->runJavaScript(QStringLiteral(
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

    // POL02: refresh the hover-preview thumbnail once the freshly
    // loaded page has a frame to show.
    if (index == currentIndex())
        scheduleThumbnailCapture(webView);

    if (index != currentIndex())
        return;

    if (ok)
        emit showStatusBarMessage(tr("Finished loading"));
    else
        emit showStatusBarMessage(tr("Failed to load"));
}

// PTAB01: a private tab's favicon carries the private mask in the
// bottom-right corner — the off-the-record marker survives whatever
// favicon the page resolves to (a private page never stores icons, so
// the base is usually the default glyph).
static QPixmap privateBadgedPixmap(const QIcon &icon)
{
    QPixmap base = icon.pixmap(16, 16);
    if (base.isNull()) {
        base = QPixmap(16, 16);
        base.fill(Qt::transparent);
    }
    const QPixmap badge(QLatin1String(":graphics/private.png"));
    const int size = base.width() / 2 + 1;
    QPainter painter(&base);
    painter.drawPixmap(base.width() - size, base.height() - size,
                       size, size, badge);
    return base;
}

void TabWidget::webViewIconChanged()
{
    WebView *webView = webViewForSender(sender());
    int index = webViewIndex(webView);
    if (-1 != index) {
#if !defined(Q_OS_MACOS)
        QIcon icon = BrowserApplication::icon(webView->url());
        QLabel *label = animationLabel(index, false);
        QMovie *movie = label->movie();
        delete movie;
        label->setMovie(nullptr);
        label->setPixmap(isTabPrivate(index)
            ? privateBadgedPixmap(icon)
            : icon.pixmap(16, 16));
#endif
    }
}

void TabWidget::webViewTitleChanged(const QString &title)
{
    WebView *webView = webViewForSender(sender());
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
    // PTAB01: the tooltip also names the private context.
    if (isTabPrivate(index))
        toolTip = QStringLiteral("[%1] %2").arg(tr("Private"), toolTip);
    setTabToolTip(index, SafeText::escaped(toolTip));
    if (currentIndex() == index)
        emit setCurrentTitle(title);
    // History title updates are handled by WebPage::init (MIG06).
}

void TabWidget::webViewUrlChanged(const QUrl &url)
{
    WebView *webView = webViewForSender(sender());
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

    // PTAB01: the target is the current tab (or a new tab inheriting
    // its context) — a private tab's typed input resolves through the
    // private search engine.
    const bool privateContext = BrowserApplication::isPrivate()
        || isTabPrivate(currentIndex());
    QUrl url = guessUrlFromString(string, privateContext);
    loadUrl(url, tab);
}

void TabWidget::loadStringFromUntrustedSource(const QString &string, OpenUrlIn tab)
{
    if (string.isEmpty())
        return;

    const bool privateContext = BrowserApplication::isPrivate()
        || isTabPrivate(currentIndex());
    const QUrl url = guessUrlFromString(string, privateContext);
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
    return guessUrlFromString(string, BrowserApplication::isPrivate());
}

QUrl TabWidget::guessUrlFromString(const QString &string, bool privateContext)
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
    // SRCH04/PTAB01: private contexts (the app-global flag, tor mode
    // and per-tab private browsing) search through the configured
    // private engine.
    const auto fallbackUrl = [search, &trimmed, privateContext, manager]() -> QUrl {
        if (search) {
            OpenSearchEngine *engine =
                manager->engineForContext(privateContext);
            // SRCH07: a stale saved engine name or a descriptor that
            // failed to load leaves engineForContext() with nothing —
            // degrade to the compiled-in default engine before giving
            // up on search entirely.
            if (!engine)
                engine = manager->engine(QLatin1String("DuckDuckGo"));
            if (engine) {
                const QUrl searchUrl = engine->searchUrl(trimmed);
                if (!searchUrl.isEmpty() && searchUrl.isValid())
                    return searchUrl;
            }
            // Last resort: emit the default engine's endpoint
            // directly.  A bare http://<term> could only DNS-fail —
            // after the HTTPS-first upgrade the user saw the exact
            // https://<term> error page this guards against.
            QUrl lastResort(QLatin1String("https://duckduckgo.com/"));
            QUrlQuery query;
            query.addQueryItem(QLatin1String("q"), trimmed);
            lastResort.setQuery(query);
            qWarning() << "guessUrlFromString: no usable search engine —"
                          "falling back to" << lastResort;
            return lastResort;
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
        if (v && v->enginePage())
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
    // TABS02: re-read the persisted vertical strip width so the same
    // live-apply path that carries the position lands it everywhere.
    m_tabBar->reloadVerticalTabWidth();

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

    // CONT06: container display — 0 keeps CONT02's inline chips on
    // every tab, 1 (the default) splits the strip into the two-level
    // container headers + filtered row once more than one container
    // owns tabs.
    setTwoLevelStrip(
        settings.value(QLatin1String("containerDisplay"), 1).toInt() != 0);

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
                // PTAB01: a window spawned from a private tab browses
                // privately too — the ctor's fresh first tab is
                // swapped for an off-the-record one (a page's profile
                // is fixed at creation, so the swap is the only
                // way).  Global private/tor windows already produce
                // off-the-record first tabs.
                if (webView && currentView && currentView->enginePage()
                    && currentView->enginePage()->isOffTheRecord()
                    && !(webView->enginePage()
                         && webView->enginePage()->isOffTheRecord())) {
                    if (WebView *privateTab =
                            newMainWindow->tabWidget()->makeNewPrivateTab(true)) {
                        newMainWindow->tabWidget()->closeTab(0);
                        webView = privateTab;
                    }
                }
            }
            webView->setFocus();
            break;
        }

        case NewSelectedTab: {
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
            qDebug() << __FUNCTION__ << "NewSelectedTab";
#endif
            webView = makeNewTabLike(currentView, true);
            // TABGRP01: a child tab inherits its opener's group, the
            // same rule the container inheritance follows.
            inheritTabGroup(webView, currentView);
            webView->setFocus();
            break;
        }

        case NewNotSelectedTab: {
#ifdef USERMODIFIEDBEHAVIOR_DEBUG
            qDebug() << __FUNCTION__ << "NewNotSelectedTab";
#endif
            webView = makeNewTabLike(currentView, false);
            inheritTabGroup(webView, currentView);
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
    int version = 4; // CONT06: v4 tails with the active container header
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);

    stream << qint32(TabWidgetMagic);
    stream << qint32(version);

    QStringList tabs;
    QList<QByteArray> tabsHistory;
    QStringList tabContainers;
    QStringList tabGroups;
    // Private tabs live on the off-the-record profile — their urls and
    // history are never written into the saved session (SEC07).  The
    // current index is remapped onto the filtered list.
    int savedCurrentIndex = -1;
    // orderedWebViews() splices a collapsed group's hidden members in
    // after its chip so no tab drops out of the serialized session.
    const QList<WebView*> ordered = orderedWebViews();
    for (WebView *tab : ordered) {
        if (!tab)
            continue;
        if (tab->enginePage() && tab->enginePage()->isOffTheRecord())
            continue;
        if (tab == currentWebView())
            savedCurrentIndex = tabs.count();
        tabs.append(QString::fromUtf8(tab->url().toEncoded()));
        if (tab->enginePage()->historyCount() != 0)
            tabsHistory.append(serializePageHistory(tab->enginePage()));
        else
            tabsHistory.append(QByteArray());
        tabContainers.append(tab->containerId());
        tabGroups.append(m_tabGroups.value(tab));
    }
    stream << tabs;
    stream << savedCurrentIndex;
    stream << tabsHistory;
    stream << tabContainers;

    // TABGRP01: the group table — one record per group that still has a
    // saved member, ordered by first appearance on the strip.  Orphaned
    // ids in tabGroups (a record that somehow vanished) degrade to
    // ungrouped on restore.
    QList<TabGroup> savedGroups;
    for (const QString &gid : tabGroups) {
        if (gid.isEmpty())
            continue;
        const auto it = m_tabGroupInfo.constFind(gid);
        if (it == m_tabGroupInfo.constEnd())
            continue;
        bool listed = false;
        for (const TabGroup &group : savedGroups) {
            if (group.id == gid) {
                listed = true;
                break;
            }
        }
        if (!listed)
            savedGroups.append(*it);
    }
    stream << tabGroups;
    stream << qint32(savedGroups.count());
    for (const TabGroup &group : savedGroups)
        stream << group.id << group.name << group.color
               << qint32(group.collapsed ? 1 : 0);

    // CONT06: the window's active level-1 selection — restoreState()
    // re-applies it once the strip is rebuilt.
    stream << m_activeContainerHeader;

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
    if (marker != TabWidgetMagic || v < 1 || v > 4)
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
    // TABGRP01: v3 tails with each tab's group id plus the group table
    // (name/color/collapsed).  A truncated tail simply leaves the
    // session ungrouped — the same defensive floor the container tail
    // established.
    QStringList savedGroupIds;
    struct SavedGroup {
        QString id;
        QString name;
        QColor color;
        bool collapsed;
    };
    QList<SavedGroup> savedGroups;
    if (v >= 3) {
        StreamingUtils::readBoundedList(stream, savedGroupIds);
        qint32 groupCount = 0;
        stream >> groupCount;
        for (qint32 i = 0; i < groupCount && i < 1024; ++i) {
            SavedGroup group;
            qint32 collapsed = 0;
            stream >> group.id >> group.name >> group.color >> collapsed;
            group.collapsed = (collapsed != 0);
            savedGroups.append(group);
        }
    }
    // CONT06: v4 tails with the window's active container header — a
    // truncated tail restores on the default header.
    QString savedHeader;
    if (v >= 4)
        stream >> savedHeader;
    if (stream.status() != QDataStream::Ok)
        return false;

    // The empty placeholder tab a fresh window comes with is only a
    // fit for the first saved tab when its container matches — reusing
    // it for a container tab would put the restored page on the wrong
    // profile, so it is closed after the loop instead.
    bool leftoverPlaceholder = false;
    QList<WebView*> createdViews;
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
        createdViews.append(webView);
        if (webView)
            webView->loadUrl(url);
    }
    if (leftoverPlaceholder && count() > 1)
        closeTab(0);

    // TABGRP01: rebuild the group table under fresh ids, then attach
    // each saved tab to its group.  Groups go in expanded — the
    // collapse pass runs only after all membership is set (collapsing
    // earlier would hide the chip later members index against, and
    // setTabGroupCollapsed no-ops if the flag is already latched).
    QHash<QString, QString> groupIdRemap;
    QStringList collapsedGroups;
    for (const SavedGroup &saved : savedGroups) {
        if (saved.id.isEmpty() || groupIdRemap.contains(saved.id))
            continue;
        TabGroup group;
        group.id = nextTabGroupId();
        group.name = saved.name;
        group.color = saved.color;
        m_tabGroupInfo.insert(group.id, group);
        groupIdRemap.insert(saved.id, group.id);
        if (saved.collapsed)
            collapsedGroups.append(group.id);
    }
    for (int i = 0; i < createdViews.count(); ++i) {
        const QString gid = groupIdRemap.value(savedGroupIds.value(i));
        if (!gid.isEmpty() && createdViews.at(i))
            m_tabGroups.insert(createdViews.at(i), gid);
    }
    for (const QString &gid : std::as_const(collapsedGroups))
        setTabGroupCollapsed(gid, true);

    // The saved index is only selectable once the restored tabs exist —
    // setting it before creating them is a no-op against the single
    // placeholder tab.  A saved-current tab that ended up hidden inside
    // a collapsed group selects the group's chip instead.
    WebView *savedCurrent = (currentTab >= 0 && currentTab < createdViews.count())
        ? createdViews.at(currentTab) : nullptr;
    int selectIndex = savedCurrent ? webViewIndex(savedCurrent) : -1;
    if (selectIndex < 0 && savedCurrent) {
        const QList<int> members =
            tabGroupMembers(m_tabGroups.value(savedCurrent));
        if (!members.isEmpty())
            selectIndex = members.first();
    }
    if (selectIndex < 0 && currentTab >= 0 && currentTab < count())
        selectIndex = currentTab;
    if (selectIndex >= 0)
        setCurrentIndex(selectIndex);
    // CONT06: rebuild the level filter — the saved header wins when
    // it still owns tabs, otherwise the saved current tab's container
    // (a window restored on "Work" lands there, not on "Tabs").
    if (m_twoLevelStrip) {
        QString header = savedHeader;
        if (header.isEmpty() || containerTabCount(header) == 0) {
            if (WebView *current = currentWebView())
                header = current->containerId();
        }
        if (!header.isEmpty()
            && !ContainerManager::instance()->isContainerId(header))
            header = ContainerManager::defaultContainerId();
        if (header != m_activeContainerHeader
            || containerTabCount(m_activeContainerHeader) < count()) {
            m_activeContainerHeader = header;
            applyContainerFilter();
        }
        if (WebView *preferred = savedCurrent) {
            const int index = webViewIndex(preferred);
            if (index >= 0)
                setCurrentIndex(index);
        }
    }
    m_tabBar->updateVisibility();
    m_tabBar->update();
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

