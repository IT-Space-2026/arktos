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
#include "arorastyle.h"
#include "bookmarknode.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "browsertheme.h"
#include "containermanager.h"
#include "downloadmanager.h"
#include "energysaver.h"
#include "engineindicator.h"
#include "engineinterface.h"
#include "engineregistry.h"
#include "enginetab.h"
#include "history.h"
#include "historycompleter.h"
#include "historymanager.h"
#include "lineedit.h"
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

#ifdef ARORA_RUSTCORE
#include <rustcore.h>
#endif

#include <qabstractproxymodel.h>
#include <qcompleter.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qevent.h>
#include <qimage.h>
#include <qjsonarray.h>
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

#include <qdebug.h>

#include <algorithm>

//#define USERMODIFIEDBEHAVIOR_DEBUG

// Defined near webViewIconChanged() — the tab marker for
// off-the-record pages (PTAB01).
static QPixmap privateBadgedPixmap(const QIcon &icon);

// The pin flag (and the group/container hidden stores) key on the
// tab's page widget — the WebViewWithSearch.  PDF02's document
// overlay wraps the WebView in a QStackedWidget inside that
// container, so parentWidget() alone reaches the stack, not the
// page: resolve the page by walking the ancestor chain.
static QWidget *tabPageWidget(WebView *view)
{
    for (QWidget *p = view ? view->parentWidget() : nullptr;
         p; p = p->parentWidget()) {
        if (qobject_cast<WebViewWithSearch *>(p))
            return p;
    }
    return nullptr;
}

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
    , closeTabButton(nullptr)
{
    setElideMode(Qt::ElideRight);

    new QShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_T), this, [this]() { openLastTab(); });
    new QShortcut(QKeySequence::Undo, this, [this]() { openLastTab(); });

    connect(m_tabBar, &TabBar::loadUrl, this,
            [this](const QUrl &url, TabWidget::OpenUrlIn tab) { loadUrl(url, tab); });
    connect(m_tabBar, &TabBar::newTab, this, &TabWidget::newTabPage);
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
    connect(m_newTabAction, &QAction::triggered, this, &TabWidget::newTabPage);

    m_closeTabAction = new QAction(this);
    m_closeTabAction->setShortcuts(QKeySequence::Close);
    // ICONS10: tab-close, not window-close — File > Close Tab and
    // Close Window shared the same glyph.
    m_closeTabAction->setIcon(AroraIcon::getChrome(QLatin1String("tab-close")));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_closeTabAction->setIconVisibleInMenu(false);
#endif
    connect(m_closeTabAction, &QAction::triggered, this, [this]() { closeTab(); });

    m_bookmarkTabsAction = new QAction(this);
    connect(m_bookmarkTabsAction, &QAction::triggered, this, &TabWidget::bookmarkTabs);

    m_newTabAction->setIcon(AroraIcon::getChrome(QLatin1String("tab-new")));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_newTabAction->setIconVisibleInMenu(false);
#endif

    m_nextTabAction = new QAction(this);
    connect(m_nextTabAction, &QAction::triggered, this, &TabWidget::nextTab);

    m_previousTabAction = new QAction(this);
    connect(m_previousTabAction, &QAction::triggered, this, &TabWidget::previousTab);
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_previousTabAction->setIcon(AroraIcon::getChrome(QLatin1String("go-previous")));
    m_nextTabAction->setIcon(AroraIcon::getChrome(QLatin1String("go-next")));
#endif

    m_recentlyClosedTabsMenu = new QMenu(this);
    connect(m_recentlyClosedTabsMenu, &QMenu::aboutToShow,
            this, &TabWidget::aboutToShowRecentTabsMenu);
    connect(m_recentlyClosedTabsMenu, &QMenu::triggered,
            this, &TabWidget::aboutToShowRecentTriggeredAction);
    m_recentlyClosedTabsAction = new QAction(this);
    m_recentlyClosedTabsAction->setMenu(m_recentlyClosedTabsMenu);
    m_recentlyClosedTabsAction->setEnabled(false);

    connect(m_tabBar, &QTabBar::tabCloseRequested,
            this, &TabWidget::closeTab);
    connect(this, &QTabWidget::currentChanged,
            this, &TabWidget::currentChanged);

    m_locationBars = new QStackedWidget(this);

    connect(BrowserApplication::historyManager(), &HistoryManager::historyCleared,
        this, &TabWidget::historyCleared);

    // PREFUI05: Energy Saver flips live — re-evaluate the suspend
    // clock and the loading spinners whenever it toggles.
    connect(EnergySaver::instance(), &EnergySaver::activeChanged,
            this, [this](bool) { applyEnergyCaps(); });

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

void TabWidget::clearRecentlyClosedTabs()
{
    m_recentlyClosedTabs.clear();
    m_recentlyClosedTabsAction->setEnabled(false);
}

bool TabWidget::hasRecentlyClosedTabs() const
{
    return !m_recentlyClosedTabs.isEmpty();
}

void TabWidget::historyCleared()
{
    clearRecentlyClosedTabs();
}

void TabWidget::clear()
{
    // clear the recently closed tabs
    clearRecentlyClosedTabs();
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
    } else if (EngineTab *foreign = engineTab(index)) {
        // ENG16: the strip/context-menu path must reach a foreign
        // engine's page too — webView() is nullptr on those tabs.
        foreign->reload();
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
    // TABS04: the pin boundary converts by position — a tab dropped
    // inside the pinned block pins, a pinned tab dragged past the
    // block's edge unpins, so the strip stays a contiguous prefix
    // either way.  "others" counts the pinned tabs excluding the one
    // that just landed, which makes the comparison the same for both
    // directions of the crossing.
    QWidget *page = widget(toIndex);
    if (page) {
        const bool pinned = m_pinnedTabs.contains(page);
        int others = 0;
        for (int i = 0; i < count(); ++i) {
            if (i != toIndex && isTabPinned(i))
                ++others;
        }
        if (pinned ? toIndex > others : toIndex < others) {
            setPinFlag(page, !pinned);
            // Same layout re-arm trick setVerticalTabWidth uses — the
            // size hint flipped between icon-only and titled.
            m_tabBar->setIconSize(m_tabBar->iconSize());
            m_tabBar->updateGeometry();
            m_tabBar->update();
        }
    }
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
        disconnect(oldWebView->enginePage(), &Engine::Page::loadStarted,
                   this, &TabWidget::loadStarted);
        disconnect(oldWebView->enginePage(), &Engine::Page::loadFinished,
                   this, &TabWidget::loadFinished);
    }
    // SLEEP01: the outgoing tab's idle clock starts now.
    markTabActivity(oldWebView);
    m_locationBars->setCurrentIndex(index);

    WebView *webView = this->webView(index);
    if (!webView) {
        // PREFS01: a widget tab (Preferences et al.) has no page —
        // the chrome actions unbind, the window title comes from the
        // tab text, and focus lands on the page widget itself.
        // ENG16: an EngineTab DOES own an Engine::Page — bind it so
        // whatever the backend exposes through Page::action()
        // (servo: Back/Reload) reaches the menus/toolbar/shortcuts;
        // unmapped actions read disabled per the interface contract.
        EngineTab *foreign = engineTab(index);
        for (int i = 0; i < m_actions.count(); ++i) {
            WebActionMapper *mapper = m_actions[i];
            mapper->updateCurrent(foreign ? foreign->page() : nullptr);
        }
        // TABS12: the window title follows the rename override too.
        emit setCurrentTitle(tabDisplayTitle(index));
        if (foreign)
            emit loadProgress(foreign->progress());
        else
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
    connect(webView->enginePage(), &Engine::Page::loadStarted,
            this, &TabWidget::loadStarted);
    connect(webView->enginePage(), &Engine::Page::loadFinished,
            this, &TabWidget::loadFinished);

    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->updateCurrent(webView->enginePage());
    }
    // SLEEP01: activating a suspended tab wakes it (the engine
    // reloads the page).
    markTabActivity(webView);
    // PREFUI04: activating a lazily restored tab performs its
    // deferred load.
    restorePendingTab(index);
    if (isTabSleeping(index))
        wakeTab(index);
    // TABS12: the override wins over the live page title here too.
    emit setCurrentTitle(tabDisplayTitle(index));
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

// TOR09/ENG13/PTAB02: a fresh tab lands on the branded start page at
// once — tor's about:tor parity, the private variant when the new
// tab's page is off-the-record (an explicit private tab, or one
// inheriting privacy from its source tab), or the servo variant when
// the default engine pick is servo.  Standard-mode tabs stay blank.
static void loadBrandedStartPage(WebView *view)
{
    if (!view)
        return;
    const bool privateContext = view->enginePage()
        && view->enginePage()->isOffTheRecord();
    const QUrl startPage = BrowserApplication::startPageUrl(
        BrowserApplication::defaultEngineId(), privateContext);
    if (startPage != QUrl(QLatin1String("qrc:/startpage.html")))
        view->loadUrl(startPage);
}

// ENG30: every branded start page (the plain variant and the
// -servo/-private/-tor editions) plus the about:home alias stands for
// "the start page", not a literal resource — a navigation that lands
// one on a different engine must re-resolve for the destination, or a
// swapped/typed load keeps the source context's branding.
static bool isBrandedStartPageUrl(const QUrl &url)
{
    if (url.scheme() == QLatin1String("about"))
        return url.path() == QLatin1String("home");
    if (url.scheme() != QLatin1String("qrc"))
        return false;
    const QString path = url.path();
    return path == QLatin1String("/startpage.html")
        || (path.startsWith(QLatin1String("/startpage-"))
            && path.endsWith(QLatin1String(".html")));
}

void TabWidget::newTab()
{
    // ENG05: the default-engine setting applies to explicit new tabs
    // only — context-bound spawns (link targets, session restore,
    // container/private inheritance) stay on WebEngine because a
    // degraded co-engine cannot take over those contracts honestly.
    Engine::Backend *backend = EngineRegistry::defaultBackend();
    if (backend && backend->id() != QLatin1String("webengine")
        && !BrowserApplication::isTorMode()
        && !BrowserApplication::isPrivate()
        && !isTabPrivate(currentIndex())) {
        if (EngineTab *tab = addEngineTab(backend, true)) {
            // ENG30: addEngineTab wires the tab but never navigated
            // it — a servo-default new tab sat blank forever.  Same
            // branded-pick rule as loadBrandedStartPage: the engine
            // variant loads, the plain page would mean no branding.
            const QUrl startPage = BrowserApplication::startPageUrl(
                backend->id(), false);
            if (startPage != QUrl(QLatin1String("qrc:/startpage.html")))
                tab->load(startPage);
            return;
        }
    }
    loadBrandedStartPage(makeNewTab(true));
}

void TabWidget::newTabPage()
{
    if (makeNewTab(true))
        loadNewTabPageUrl();
}

void TabWidget::newTabPageInContainer(const QString &containerId)
{
    if (makeNewTabInContainer(containerId, true))
        loadNewTabPageUrl();
}

void TabWidget::loadNewTabPageUrl()
{
    // START02: a user-opened new tab shows the start page — the same
    // target the Home button resolves (MainWindow/home, default
    // "about:home" -> qrc:/startpage.html), so the setting and the
    // two surfaces never disagree.  Programmatic tabs (the window's
    // startup tab, session restore, link/context spawns) go through
    // newTab()/makeNewTab() and stay bare, leaving the
    // startupBehavior preference authoritative at launch.
    QSettings settings;
    settings.beginGroup(QLatin1String("MainWindow"));
    const QString home = settings.value(QLatin1String("home"),
                                        QLatin1String("about:home"))
                             .toString();
    if (!home.trimmed().isEmpty())
        loadString(home);
}

void TabWidget::newPrivateTab()
{
    loadBrandedStartPage(makeNewPrivateTab(true));
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

// TABS04 — pinned tabs ------------------------------------------------
//
// A pinned tab holds a slot in the strip's leading pinned block and
// carries no close button — unpinning is the way to make it closable
// again.  TABS14: it keeps its favicon and title like any other tab —
// the leading position and missing close affordance mark the pin, not
// an icon-only collapse.  The flag is keyed on the tab's page widget —
// the same pointer the collapsed-group and container-level hidden
// stores keep — so pin state survives every detach/re-insert cycle
// untouched.  Pinning is deliberately a strip-only concept: private
// tabs can be pinned for the session but are never serialized
// (SEC07), and a group or container membership is unaffected by the
// flag.

bool TabWidget::isTabPinned(int index) const
{
    return index >= 0 && index < count()
        && m_pinnedTabs.contains(widget(index));
}

int TabWidget::pinnedTabCount() const
{
    int pinned = 0;
    while (pinned < count() && isTabPinned(pinned))
        ++pinned;
    return pinned;
}

// Flag-only update — no repositioning.  TABS14: pin no longer flips
// the tab's size hint (titled geometry both ways) but it does flip
// the close button's visibility, so callers still follow with the
// bar's layout re-arm (setIconSize(iconSize()), same trick
// setVerticalTabWidth uses).
void TabWidget::setPinFlag(QWidget *page, bool pinned)
{
    if (!page)
        return;
    if (pinned)
        m_pinnedTabs.insert(page);
    else
        m_pinnedTabs.remove(page);
}

void TabWidget::setTabPinned(int index, bool pinned)
{
    if (index < 0 || index >= count() || isTabPinned(index) == pinned)
        return;
    // Slide the tab to the edge of the pinned block so the prefix
    // invariant holds: a newly pinned tab joins the block's end (the
    // index of the first non-pinned slot), an unpinned one lands just
    // past the tabs that stay pinned.  "others" is a full count, not
    // the contiguous prefix — the tab being toggled may sit beyond
    // the block, where the prefix scan stops early.
    int others = 0;
    for (int i = 0; i < count(); ++i) {
        if (i != index && isTabPinned(i))
            ++others;
    }
    setPinFlag(widget(index), pinned);
    const int target = others;
    // The m_groupAdjust guard keeps this positioning move from
    // rewriting group membership.
    if (index != target) {
        m_groupAdjust = true;
        m_tabBar->moveTab(index, target);
        m_groupAdjust = false;
    }
    m_tabBar->setIconSize(m_tabBar->iconSize());
    m_tabBar->updateGeometry();
    updateGeometry();
    m_tabBar->update();
    emit tabsChanged();
}

// Re-establish the pinned-prefix invariant after a batch re-insertion
// that preserves order but not positions (a collapsed group
// expanding, a container level rejoining the strip).  Each pinned tab
// bubbles ahead of the first unpinned one; relative order is kept.
void TabWidget::normalizePinnedBlock()
{
    int boundary = 0;
    for (int i = 0; i < count(); ++i) {
        if (!isTabPinned(i))
            continue;
        if (i != boundary) {
            m_groupAdjust = true;
            m_tabBar->moveTab(i, boundary);
            m_groupAdjust = false;
        }
        ++boundary;
    }
}

// TABS12: the stored rename override ("" when the tab shows its live
// title).  Keyed on the page widget like m_pinnedTabs — drags, group
// collapse and container-level filtering all keep the same widget, so
// the override rides along untouched.
QString TabWidget::tabTitleOverride(int index) const
{
    if (index < 0 || index >= count())
        return QString();
    return m_customTitles.value(const_cast<TabWidget*>(this)->widget(index));
}

// The raw, unescaped text the strip shows — the inline editor's
// prefill, the window-title emit and the hover preview all read this
// instead of tabText() (which is SafeText::menu-escaped).
QString TabWidget::tabDisplayTitle(int index) const
{
    if (index < 0 || index >= count())
        return QString();
    QWidget *page = const_cast<TabWidget*>(this)->widget(index);
    const QString custom = m_customTitles.value(page);
    if (!custom.isEmpty())
        return custom;
    if (WebView *view = const_cast<TabWidget*>(this)->webView(index)) {
        if (!view->title().isEmpty())
            return view->title();
        const QString url = QString::fromUtf8(view->url().toEncoded());
        if (!url.isEmpty())
            return url;
    }
    // A widget tab keeps the title it displaced when the override
    // landed; never-renamed ones read their (unescaped) insert text.
    const QString base = m_widgetTabBaseTitles.value(page);
    if (!base.isEmpty())
        return base;
    QString text = const_cast<TabWidget*>(this)->tabText(index);
    text.replace(QLatin1String("&&"), QLatin1String("&"));
    return text;
}

void TabWidget::setTabTitleOverride(int index, const QString &title)
{
    if (index < 0 || index >= count())
        return;
    QWidget *page = widget(index);
    if (!page)
        return;
    const QString cleaned = title.trimmed();
    if (cleaned.isEmpty()) {
        if (!m_customTitles.remove(page))
            return;
    } else {
        // A widget tab's live title exists only on the strip — stash
        // the displaced original once so clearing restores it.
        if (!webView(index) && !m_widgetTabBaseTitles.contains(page))
            m_widgetTabBaseTitles.insert(page, tabDisplayTitle(index));
        m_customTitles.insert(page, cleaned);
    }
    applyTabTitle(index);
    emit tabsChanged();
}

// The map write + strip refresh the container swap, undo-close and
// session restore share — the override re-keys onto a replacement or
// recreated page widget.  Tabs currently detached (a filtered-out
// container level) only get the map write; the hidden store's saved
// text already carries the title.
void TabWidget::setCustomTitleForPage(QWidget *page, const QString &title)
{
    if (!page || title.isEmpty())
        return;
    m_customTitles.insert(page, title);
    const int index = indexOf(page);
    if (index >= 0)
        applyTabTitle(index);
}

// The strip label + tooltip + window-title emit — recomputed whenever
// the effective title may have changed: a page titleChanged, a rename
// commit or an override clear.  The rename override replaces the page
// title on every user-facing surface; the container/private
// decorations still wrap it.
void TabWidget::applyTabTitle(int index)
{
    const QString tabTitle = tabDisplayTitle(index);
    // The title is page/user-controlled: '&' would become a mnemonic
    // on the tab label and markup would render in the tooltip
    // (tooltips are always rich-text capable).
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
        emit setCurrentTitle(tabTitle);
}

WebView *TabWidget::makeNewPrivateTab(bool makeCurrent)
{
    if (BrowserApplication::isTorMode())
        return makeNewTabInContainer(
            ContainerManager::defaultContainerId(), makeCurrent);
    return makeNewTabOnProfile(
        WebEngineProfileAdapter::forProfile(
            BrowserApplication::privateWebEngineProfile()), makeCurrent);
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
    // instantiate the application object.  The engine profiles are
    // wrapped in their canonical Engine::Profile adapters — the tab
    // layer stays on the neutral side of the boundary.
    Engine::Profile *profile = WebEngineProfileAdapter::forProfile(
        BrowserApplication::webEngineProfile());
    if (!containerId.isEmpty() && !BrowserApplication::isPrivate()) {
        if (auto *containerProfile =
                ContainerManager::instance()->profileFor(containerId))
            profile = WebEngineProfileAdapter::forProfile(containerProfile);
    }
    return makeNewTabOnProfile(profile, makeCurrent);
}

// The shared omnibox completer + suggestions overlay, built on first
// use.  Every bar in the parallel stack attaches through
// wireLocationBar() — engine-tab bars included (ENG14) — so the pair
// must come up no matter which tab kind lands first.
void TabWidget::ensureLineEditCompleter()
{
    if (m_lineEditCompleter)
        return;
    HistoryCompletionModel *completionModel = new HistoryCompletionModel(this);
#ifdef ARORA_RUSTCORE
    // OMNI01: the rustcore store serves the ranked completion rows
    // — a thin model in front of rc_history_suggest().  The no-rust
    // build keeps the in-memory filter model.  (Touch the manager
    // so its load() has opened the store before the first query.)
    BrowserApplication::historyManager();
    completionModel->setSourceModel(
        new RustHistorySuggestModel(completionModel));
#else
    completionModel->setSourceModel(BrowserApplication::historyManager()->historyFilterModel());
#endif
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

// The wiring every location-bar slot shares: the shared completer, the
// suggestions feeder, and Enter -> lineEditReturnPressed -> loadString.
// Both the LocationBar of a webengine tab and the plain bar of an
// engine tab attach here so omnibox behavior never forks (ENG14).
void TabWidget::wireLocationBar(QLineEdit *bar)
{
    ensureLineEditCompleter();
    bar->setCompleter(m_lineEditCompleter);
    connect(bar, &QLineEdit::textEdited,
            m_omniboxSuggestions, &OmniboxSuggestions::scheduleSuggestions);
    connect(bar, &QLineEdit::returnPressed, this, &TabWidget::lineEditReturnPressed);
}

// The engine-page/webview signal set every hosted view needs — shared
// by makeNewTabOnProfile() and adoptTransferredTab(): a view arriving
// from another window is still connected to its old TabWidget.
void TabWidget::wireWebView(WebView *webView)
{
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
            this, [this, webView]() { emit printRequested(webView); });
    // Qt WebEngine does not surface WebKit's window-feature requests
    // (geometryChangeRequested / *VisibilityChangeRequested); window.open
    // chrome handling is internal to Chromium.

    // webview actions
    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->addChild(webView->enginePage()->action(mapper->webAction()));
    }
}

WebView *TabWidget::makeNewTabOnProfile(Engine::Profile *profile, bool makeCurrent)
{
    // line edit
    LocationBar *locationBar = new LocationBar;
    wireLocationBar(locationBar);
    // TABS04: a new tab lands right after the pinned block — the
    // pinned prefix never gains a straggler at its far side.  With
    // nothing pinned (or while restoreState() rebuilds the saved
    // order, which already places pinned tabs first) the tab appends
    // at the end as before.
    const int insertPos =
        (pinnedTabCount() > 0 && !m_sessionRestore)
        ? pinnedTabCount() : count();
    m_locationBars->insertWidget(insertPos, locationBar);
    m_locationBars->setSizePolicy(locationBar->sizePolicy());

#ifndef AUTOTESTS
    if (BrowserMainWindow *window = BrowserMainWindow::parentWindow(this)) {
        // ENG08: the nav row's focus chain runs location bar ->
        // engine indicator -> dedicated search box; whichever piece
        // is absent gets skipped so the chain still terminates on
        // the search field.
        QWidget *afterBar = window->findChild<EngineIndicator *>();
        if (!afterBar)
            afterBar = window->findChild<ToolbarSearch *>();
        else if (ToolbarSearch *toolbarSearch = window->findChild<ToolbarSearch *>())
            QWidget::setTabOrder(afterBar, toolbarSearch);
        if (afterBar)
            QWidget::setTabOrder(locationBar, afterBar);
    }
#endif

    WebView *webView = new WebView(profile);
    locationBar->setWebView(webView);
    wireWebView(webView);

    WebViewWithSearch *webViewWithSearch = new WebViewWithSearch(webView, this);
    insertTab(insertPos, webViewWithSearch, tr("Untitled"));
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
    // TABS04: widget tabs obey the same pinned-prefix insert rule.
    const int insertPos =
        (pinnedTabCount() > 0 && !m_sessionRestore)
        ? pinnedTabCount() : count();
    m_locationBars->insertWidget(insertPos, bar);

    const int index = insertTab(insertPos, page, icon, title);
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

EngineTab *TabWidget::engineTab(int index) const
{
    return qobject_cast<EngineTab*>(widget(index));
}

QString TabWidget::tabEngineId(int index) const
{
    if (EngineTab *tab = engineTab(index))
        return tab->backend()->id();
    if (webView(index))
        return QStringLiteral("webengine");
    return QString();
}

// ENG18: foreign backends carry no QWebChannel, so the "arora" page
// object that the internal qrc pages bind against (startpage.js'
// init()) never materializes — a start page on a Servo tab renders
// the bare skeleton: empty title and header, a dead search box.
// After a qrc: commit on an engine tab, inject the equivalent
// page-side shim: translate() answers from the same translation
// domains webpage.cpp's JavaScriptAroraObject serves, searchUrl()
// reuses the live context engine's template and darkChrome tracks
// the real palette — the page gains every feature the transport
// cannot carry.  A page that already defined arora is left alone.
static QString engineTabAroraScript()
{
    const auto jsString = [](const QString &text) {
        QJsonArray array;
        array.append(text);
        const QByteArray json =
            QJsonDocument(array).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(json.mid(1, json.size() - 2));
    };
    static const char *keys[] = {
        "Welcome to Arora!",
        "Arora Start",
        "Search!",
        "Search results provided by",
        "About Arora",
        "QT_LAYOUT_DIRECTION"
    };
    QStringList entries;
    for (const char *key : keys) {
        QString translated = QCoreApplication::translate(
            "JavaScriptAroraObject", key);
        if (translated == QLatin1String(key))
            translated = qApp->tr(key);
        entries.append(jsString(QLatin1String(key)) + QLatin1Char(':')
                       + jsString(translated));
    }
    QString engineName, engineTemplate;
    if (OpenSearchEngine *engine = ToolbarSearch::openSearchManager()
            ->engineForContext(false)) {
        engineName = engine->name();
        engineTemplate = engine->searchUrlTemplate();
    }
    const QString dark = QLatin1String(
        qApp && BrowserTheme::isDarkPalette(qApp->palette())
            ? "true" : "false");
    return QStringLiteral(
        "(function(){"
        "if(window.arora)return;"
        "var T={%1};"
        "window.arora={"
        "currentEngineName:%2,"
        "darkChrome:%3,"
        "darkChromeChanged:{connect:function(){}},"
        "translate:function(s,cb){cb(Object.prototype.hasOwnProperty.call(T,s)?T[s]:s);},"
        "searchUrl:function(q,cb){cb(%4.split('{searchTerms}').join(encodeURIComponent(q)));}"
        "};"
        "document.documentElement.classList.toggle('arora-dark',%3);"
        "if(typeof update==='function')update();"
        "})();")
        .arg(entries.join(QLatin1Char(',')), jsString(engineName),
             dark, jsString(engineTemplate));
}

EngineTab *TabWidget::addEngineTab(Engine::Backend *backend, bool makeCurrent)
{
    if (!backend)
        return nullptr;

    Engine::Profile *profile =
        backend->createProfile(Engine::ProfileOptions(), nullptr);
    EngineTab *tab = new EngineTab(backend, profile, this);
    // The profile is the tab's context — give it the tab as owner so
    // tab teardown frees it.  Backends that hand out a shared object
    // (Servo's single browsing context is parented to the backend)
    // keep their existing owner.
    if (profile && !profile->parent())
        profile->setParent(tab);
    if (!tab->page()) {
        // ENG12: the backend refused the page (servo's one-instance
        // limit, an absent artifact, ...) — surface the refusal
        // rather than silently doing nothing on "Reload in <engine>".
        emit showStatusBarMessage(
            tr("%1 is not available for this tab")
                .arg(backend->displayName()));
        delete tab;
        return nullptr;
    }

    // The parallel location-bar stack needs one widget per tab.  An
    // engine tab's bar is a plain LineEdit — LocationBar's
    // site-icon/shield cluster is WebView-bound — but fully editable
    // like every other tab's (ENG14): Enter routes through
    // lineEditReturnPressed -> loadString -> loadUrl, which dispatches
    // a current-tab load to this tab's Engine::Page, and non-address
    // text resolves through guessUrlFromString's shared search
    // fallback — the engine split never forks omnibox behavior.
    // ENG08: the swap affordance lives on the chrome's shared toolbar
    // indicator, which tracks the ACTIVE tab — it offers "Reload in
    // Chromium" while this tab is current, which is the swap-back
    // path.
    LineEdit *bar = new LineEdit(this);
    wireLocationBar(bar);
    m_locationBars->addWidget(bar);

    // Tab-strip plumbing mirroring the WebView wiring in
    // makeNewTabOnProfile — senders are the EngineTab, not a WebView,
    // so the shared webView* slots don't apply.
    connect(tab, &EngineTab::titleChanged,
            this, [this, tab](const QString &title) {
        const int at = indexOf(tab);
        if (at < 0)
            return;
        QString tabTitle = title;
        if (tabTitle.isEmpty())
            tabTitle = QString::fromUtf8(tab->url().toEncoded());
        // Page-controlled text — same escaping as webViewTitleChanged.
        setTabText(at, SafeText::menu(tabTitle));
        setTabToolTip(at, SafeText::escaped(tabTitle));
        if (at == currentIndex())
            emit setCurrentTitle(title);
    });
    connect(tab, &EngineTab::urlChanged,
            this, [this, tab](const QUrl &url) {
        const int at = indexOf(tab);
        if (at < 0)
            return;
        m_tabBar->setTabData(at, url);
        if (QLineEdit *bar = locationBar(at))
            bar->setText(QString::fromUtf8(url.toEncoded()));
        emit tabsChanged();
    });
    connect(tab, &EngineTab::loadStarted,
            this, [this, tab]() {
        const int at = indexOf(tab);
        if (at < 0)
            return;
        QLabel *label = animationLabel(at, true);
        if (label->movie())
            label->movie()->start();
        if (at == currentIndex())
            emit showStatusBarMessage(tr("Loading..."));
    });
    connect(tab, &EngineTab::loadProgress,
            this, [this, tab](int progress) {
        if (indexOf(tab) == currentIndex() && indexOf(tab) >= 0) {
            emit loadProgress(progress);
            emit showStatusBarMessage(tr("Loading %1%...").arg(progress));
        }
    });
    connect(tab, &EngineTab::loadFinished,
            this, [this, tab](bool ok) {
        const int at = indexOf(tab);
        if (at < 0)
            return;
        QLabel *label = animationLabel(at, false);
        QMovie *movie = label->movie();
        delete movie;
        label->setMovie(nullptr);
#if defined(Q_OS_MACOS)
        // Same end-of-load shape as webViewLoadFinished: macOS tabs
        // carry no favicon label at all.
        m_tabBar->setTabButton(at, m_tabBar->freeSide(), nullptr);
        delete label;
#else
        label->setPixmap(QIcon(QLatin1String(":graphics/defaulticon.png"))
                             .pixmap(16, 16));
#endif
        if (at == currentIndex())
            emit showStatusBarMessage(ok ? tr("Finished loading")
                                         : tr("Failed to load"));
        // ENG18: arm the page-side "arora" shim — internal qrc pages
        // get translated chrome text and a working search box even
        // though a foreign backend has no QWebChannel to serve them.
        if (ok && tab->page()
            && tab->url().scheme() == QLatin1String("qrc"))
            tab->page()->runJavaScript(engineTabAroraScript());
    });

    // webview actions — same wiring makeNewTabOnProfile applies to a
    // WebView's enginePage(): the page's standard actions feed the
    // shared chrome entries (enabled state propagates through
    // changed, triggers dispatch on the page).  A backend that maps
    // none (action() -> nullptr) leaves the entries disabled.
    for (int i = 0; i < m_actions.count(); ++i) {
        WebActionMapper *mapper = m_actions[i];
        mapper->addChild(tab->page()->action(mapper->webAction()));
    }

    // ENG28: an engine restart swaps the Engine::Page object in place
    // (the crashed Servo view's "Try Servo Again" path) — the dead
    // page's actions die with it, so the new page's action() children
    // join the mappers and the current-tab binding re-points.
    connect(tab, &EngineTab::pageRecreated, this, [this, tab]() {
        Engine::Page *fresh = tab->page();
        for (int i = 0; i < m_actions.count(); ++i) {
            WebActionMapper *mapper = m_actions[i];
            mapper->addChild(fresh ? fresh->action(mapper->webAction())
                                   : nullptr);
            if (indexOf(tab) == currentIndex() && indexOf(tab) >= 0)
                mapper->updateCurrent(fresh);
        }
    });

    // ENG15: one favicon surface per tab.  WebView tabs carry theirs
    // in the free-side animationLabel and pass no tab icon — an
    // engine tab that did both (the addTab icon plus the label's
    // pixmap above) painted the default glyph twice on the strip.
    // Install the label eagerly so the single icon exists even
    // before the first load finishes.
    const int index = addTab(tab, backend->displayName());
#if !defined(Q_OS_MACOS)
    animationLabel(index, false)->setPixmap(
        QIcon(QLatin1String(":graphics/defaulticon.png"))
            .pixmap(16, 16));
#endif
    setTabToolTip(index, backend->displayName());
    // CONT06: same strip bookkeeping as addWidgetTab — engine tabs
    // live under the default header.
    if (m_twoLevelStrip && !m_containerFilterAdjust
        && !m_activeContainerHeader.isEmpty()) {
        if (makeCurrent)
            setActiveContainerHeader(QString());
        else
            detachTabIntoContainerStore(index);
        syncContainerStrip();
    }
    if (makeCurrent)
        setCurrentIndex(indexOf(tab));
    if (count() == 1)
        currentChanged(currentIndex());
    emit tabsChanged();
    return tab;
}

bool TabWidget::reloadTabInEngine(int index, const QString &engineId)
{
    if (index < 0 || index >= count())
        return false;
    // Tor windows are locked to Chromium — servo has no SOCKS5
    // support, so a swapped tab would leak traffic off the proxy.
    if (BrowserApplication::isTorMode())
        return false;
    // A private tab has no off-the-record equivalent on a foreign
    // backend — the swap would land its url on a persistent profile.
    if (isTabPrivate(index))
        return false;
    // CONT06: while a non-default container level is active the
    // append+move+close swap would re-filter the strip mid-swap —
    // refuse rather than corrupt the strip.  Engine tabs live on the
    // default level anyway, and a hidden tab's bar is unreachable.
    if (m_twoLevelStrip && containerStripActive()
        && !m_activeContainerHeader.isEmpty())
        return false;
    const QString currentId = tabEngineId(index);
    if (currentId.isEmpty() || currentId == engineId)
        return false;
    Engine::Backend *backend = EngineRegistry::backendForId(engineId);
    if (!backend)
        return false;

    WebView *view = webView(index);
    EngineTab *foreign = engineTab(index);
    // url() names the last COMMITTED navigation — a mid-load swap
    // carries the in-flight target instead, or the replacement
    // silently re-loads the page the user already left.
    const QUrl url = view ? (view->page()->isLoading()
                                 ? view->page()->requestedUrl()
                                 : view->url())
                          : (foreign ? foreign->url() : QUrl());
    // ENG30: a carried branded start page names the SOURCE engine's
    // chrome — the destination always gets its own variant (a servo
    // tab swapped to chromium shows the plain page, and the swap
    // back re-brands to the servo one).  Private contexts never
    // reach here — the private/tor guards above already refused.
    const QUrl targetUrl = isBrandedStartPageUrl(url)
        ? BrowserApplication::startPageUrl(engineId, false)
        : url;
    const bool wasCurrent = (index == currentIndex());
    // TABGRP01: membership keys on WebView — a webengine target keeps
    // the group, a foreign tab drops it (no slot to hang the tag on).
    const QString gid = tabGroupId(index);
    // ENG09: keep the original's widget pointer — the replacement's
    // insert+move and the group expansion inside closeTab() can all
    // shift strip indices, so the old tab is closed by identity, not
    // by the 'index + 1' positional guess.
    QWidget *original = widget(index);

    QWidget *replacement = nullptr;
    if (engineId == QLatin1String("webengine")) {
        WebView *newView = makeNewTabInContainer(
            ContainerManager::defaultContainerId(), wasCurrent);
        if (!newView)
            return false;
        if (targetUrl.isValid())
            newView->loadUrl(targetUrl);
        if (!gid.isEmpty())
            assignTabGroup(newView, gid);
        replacement = widget(webViewIndex(newView));
    } else {
        EngineTab *tab = addEngineTab(backend, wasCurrent);
        if (!tab)
            return false;
        if (targetUrl.isValid())
            tab->load(targetUrl);
        replacement = tab;
    }
    if (!replacement)
        return false;

    // Land the replacement on the old tab's slot, then close the
    // shifted original — same shape as reopenTabInContainer.
    const int newIndex = indexOf(replacement);
    if (newIndex >= 0 && newIndex != index)
        m_tabBar->moveTab(newIndex, index);
    // TABGRP01: the swapped slot can be a collapsed group's chip —
    // closing it expands the group to free the hidden members, so
    // collapse it again around the next member (the foreign tab
    // itself cannot hold membership and take over the chip).
    const bool wasCollapsed = !gid.isEmpty()
        && m_tabGroupInfo.value(gid).collapsed;
    const int originalIndex = indexOf(original);
    if (originalIndex >= 0)
        closeTab(originalIndex);
    if (wasCollapsed && m_tabGroupInfo.contains(gid))
        setTabGroupCollapsed(gid, true);
    return true;
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
    // TABS12: the rename override re-keys onto the replacement page.
    const QString customTitle = tabTitleOverride(index);
    WebView *newTab = makeNewTabInContainer(containerId, true);
    if (!newTab)
        return;
    setCustomTitleForPage(tabPageWidget(newTab), customTitle);
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
    // Slide the fresh tab into the old tab's slot (moveTab emits
    // tabMoved, which keeps m_locationBars in sync) so the strip
    // order survives the swap.  It is NOT necessarily the last tab —
    // TABS04 inserts new tabs after the pinned block.
    const int appendedIndex = webViewIndex(newTab);
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
        // TABS12: a rename that landed while the tab was detached only
        // reached the keyed map — reassert it on the strip now.
        if (m_customTitles.contains(entry.tab))
            applyTabTitle(idx);
        ++insertPos;
    }
    // TABS04: a pinned member can resurface past the pinned block —
    // bubble it back to the prefix.
    normalizePinnedBlock();
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
// Defined further down — resolves the tab a page signal arrived
// from; senders are the WebView or its Engine::Page adapter.
static WebView *webViewForSender(QObject *sender);

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
        } else if (qobject_cast<EngineTab*>(entry.tab)) {
            // ENG15: foreign tabs have no url-keyed favicon — restore
            // the same default glyph addEngineTab installs.
            animationLabel(idx, false)->setPixmap(
                QIcon(QLatin1String(":graphics/defaulticon.png"))
                    .pixmap(16, 16));
        }
#endif
        // TABS12: same reassert as the group expand — a rename stored
        // while the tab sat filtered off-strip reaches it here.
        if (m_customTitles.contains(entry.tab))
            applyTabTitle(idx);
        ++insertPos;
    }
    // TABS04: a level's pinned tabs re-insert at the end of the row —
    // restore the prefix within the visible strip.
    normalizePinnedBlock();
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
            const QString closingGroup = m_tabGroups.value(view);
            // TABS05: off the strip means no index — the record keeps
            // the level id it was filtered into as its container.
            pushRecentlyClosedTab(view, -1, it.key(), closingGroup);
            HiddenGroupTab hidden = it->takeAt(k);
            hidden.bar->deleteLater();
            m_tabGroups.remove(view);
            m_pinnedTabs.remove(hidden.tab);
            m_customTitles.remove(hidden.tab);
            m_widgetTabBaseTitles.remove(hidden.tab);
            hidden.tab->deleteLater();
            m_sleepStates.remove(view);
            m_tabThumbnails.remove(view);
            m_pendingScrollRestore.remove(view);
            m_pendingRestores.remove(view);
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

// PREFUI05: while Energy Saver is active the suspend clock is pinned
// to the Memory Saver "Maximum" preset (10 min) — it overrides the
// page's own pick, including "Never", for as long as the saver is on.
qint64 TabWidget::suspendIdleIntervalMs() const
{
    if (EnergySaver::instance()->active())
        return 10 * 60 * 1000;
    return m_suspendIdleMs;
}

void TabWidget::applyEnergyCaps()
{
    updateSleepTimer();
    // Engaging the saver should bite now, not on the next 15s sweep.
    if (EnergySaver::instance()->active())
        suspendIdleTabs(suspendIdleIntervalMs());
    // Loading spinners are pure per-frame repaint cost — freeze them
    // on the current frame while the saver is on.
    const QTabBar::ButtonPosition side = m_tabBar->freeSide();
    const bool pause = EnergySaver::instance()->active();
    for (int i = 0; i < count(); ++i) {
        QLabel *label =
            qobject_cast<QLabel*>(m_tabBar->tabButton(i, side));
        if (QMovie *movie = label ? label->movie() : nullptr)
            movie->setPaused(pause);
    }
}

void TabWidget::updateSleepTimer()
{
    if (suspendIdleIntervalMs() > 0) {
        if (!m_sleepTimer) {
            m_sleepTimer = new QTimer(this);
            connect(m_sleepTimer, &QTimer::timeout, this, [this]() {
                suspendIdleTabs(suspendIdleIntervalMs());
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
    // TABS04: pinned tabs are keep-alive by definition — the idle
    // sweep must never discard their page.
    if (isTabPinned(index))
        return QLatin1String("pinned");
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
    if (DownloadManager::instance()->hasActiveDownloadForPage(
            view->enginePage()))
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

// Grab the tab's view into the thumbnail cache, tagged with the url
// the frame belongs to.  Only the current tab's view is actually
// visible inside the stack, so the scheduled captures below only
// ever fire there — background tabs are served exclusively from this
// cache by tabThumbnail().
void TabWidget::captureTabThumbnail(int index)
{
    WebView *view = webView(index);
    if (!view || !view->isVisible())
        return;
    const QImage frame = webViewThumbnailFrame(view);
    if (blankThumbnailFrame(frame))
        return;
    CachedThumbnail thumb;
    thumb.pixmap = QPixmap::fromImage(
        frame.scaledToWidth(384, Qt::SmoothTransformation));
    thumb.url = view->url();
    m_tabThumbnails.insert(view, thumb);
}

void TabWidget::scheduleThumbnailCapture(WebView *webView)
{
    // PREFUI05: hover previews are capped while Energy Saver is on —
    // skip the deferred frame grab entirely.
    if (EnergySaver::instance()->active())
        return;
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
    // TPREV02: only the current tab's view is grab-verified — it is
    // the one actually painting, so the frame it offers is this tab's
    // live page.  A hidden view's grabFramebuffer() can hand back a
    // stale pre-navigation remnant, an evicted compositor's leftover,
    // or another context's pixels, and the blank check cannot tell a
    // stale painted frame from a current one — background tabs are
    // therefore served only the snapshot cached while they last
    // painted, and only while its recorded url still matches (a
    // same-document pushState/hash navigation changes the url without
    // a loadStarted, so it is rejected here too).  On a miss the
    // caller falls back to the title+url card — a wrong-page image
    // is never shown.
    if (index == currentIndex()) {
        const QImage frame = webViewThumbnailFrame(view);
        if (!frame.isNull() && !blankThumbnailFrame(frame)) {
            CachedThumbnail thumb;
            thumb.pixmap = QPixmap::fromImage(
                frame.scaledToWidth(384, Qt::SmoothTransformation));
            thumb.url = view->url();
            m_tabThumbnails.insert(view, thumb);
            return thumb.pixmap;
        }
    }
    const auto it = m_tabThumbnails.constFind(view);
    if (it != m_tabThumbnails.constEnd() && it->url == view->url())
        return it->pixmap;
    return QPixmap();
}

void TabWidget::reloadAllTabs()
{
    for (int i = 0; i < count(); ++i) {
        if (WebView *tab = webView(i)) {
            tab->reload();
        } else if (EngineTab *foreign = engineTab(i)) {
            foreign->reload();
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
            // A widget tab (PREFS01) has no view to focus; on an
            // engine tab the backend's view takes it so key input
            // reaches the foreign page (ENG14).
            if (WebView *view = currentWebView()) {
                view->setFocus();
            } else if (EngineTab *foreign = engineTab(currentIndex())) {
                if (foreign->page() && foreign->page()->view())
                    foreign->page()->view()->setFocus();
            }
        }
    }
}

void TabWidget::windowCloseRequested()
{
    // The signal arrives through the Engine::Page adapter — resolve
    // the owning tab the same way the webView* slots below do.
    WebView *webView = webViewForSender(sender());
    if (!webView)
        return;
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
    // TABS04: pinned tabs survive "Close Other Tabs" in both
    // directions — closing indices above first keeps each lower
    // index stable when the check runs.
    for (int i = count() - 1; i > index; --i) {
        if (!isTabPinned(i))
            closeTab(i);
    }
    for (int i = index - 1; i >= 0; --i) {
        if (!isTabPinned(i))
            closeTab(i);
    }
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
    // recorded — the very trace private browsing avoids (SEC07).  The
    // record snapshot happens before the strip slot is torn down.
    if (tab)
        pushRecentlyClosedTab(tab, index, containerIdForTab(index),
                              closingGroup);
    QWidget *lineEdit = m_locationBars->widget(index);
    m_locationBars->removeWidget(lineEdit);
    lineEdit->deleteLater();

    QWidget *webViewWithSearch = widget(index);
    removeTab(index);
    m_sleepStates.remove(tab);
    m_tabThumbnails.remove(tab);
    m_pendingScrollRestore.remove(tab);
    m_pendingRestores.remove(tab);
    // TABGRP01: drop the view's membership; the group record dies with
    // its last member.
    m_tabGroups.remove(tab);
    m_pinnedTabs.remove(webViewWithSearch);
    // TABS12: the rename override dies with its page widget.
    m_customTitles.remove(webViewWithSearch);
    m_widgetTabBaseTitles.remove(webViewWithSearch);
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

// TABS13 — the take half of a cross-window move.  Unlike closeTab the
// widgets are never deleted and nothing is queued for reopen: the tab
// is leaving, not closing.  Same strip mechanics as the
// collapsed-group/container-store detaches — transient tab buttons are
// dropped (tabInserted re-creates them) and every record keyed on the
// page is lifted into the carrier.
bool TabWidget::moveTabTo(int index, TabWidget *target, int targetIndex)
{
    if (!target || target == this || index < 0 || index >= count())
        return false;
    QWidget *page = widget(index);
    if (!page)
        return false;

    // TABS13(d) — the transfer constraints.
    //
    // Engine tabs: the movable web tab is a WebViewWithSearch (page,
    // history and engine state all ride inside); a widget tab is a
    // bare QWidget that moves whole.  A backend that cannot transfer
    // (the servo spike's host) must mark its page so the drop refuses
    // instead of tearing a foreign view off its window.
    if (page->property("aroraNoTabTransfer").toBool())
        return false;
    // Privacy boundary: private browsing is per-tab (PTAB01) — the
    // OTR bit lives on the page's profile and moves with the widget,
    // and every persistence path (SEC07 session save, the
    // recently-closed stack) filters per page, so an OTR tab stays
    // non-recording wherever it lands.  The reverse is a leak: a
    // persistent page inside a private/tor window would keep writing
    // history where the user reads every tab as throwaway.  The modes
    // are process-global today — a pointer-validated same-process
    // drag can never cross them — but the guard pins the invariant
    // to the code instead of assuming it.  Container pages are
    // non-OTR by construction (containers are refused under
    // private/tor at creation), so the same check covers them.
    WebView *guardView = webView(index);
    const bool pageIsPrivate = guardView && guardView->enginePage()
        && guardView->enginePage()->isOffTheRecord();
    if (!pageIsPrivate && BrowserApplication::isPrivate())
        return false;

    // The chip is the only visible member of a collapsed group —
    // expand first so only the dragged tab leaves, the same rule
    // closeTab() applies.
    const QString gid = tabGroupId(index);
    if (!gid.isEmpty() && m_tabGroupInfo.value(gid).collapsed)
        setTabGroupCollapsed(gid, false);
    index = indexOf(page);
    if (index < 0)
        return false;

    WebView *view = webView(index);
    TransferredTab tab;
    tab.page = page;
    tab.bar = m_locationBars->widget(index);
    tab.icon = m_tabBar->tabIcon(index);
    tab.text = tabText(index);
    tab.toolTip = tabToolTip(index);
    tab.data = m_tabBar->tabData(index);
    tab.pinned = m_pinnedTabs.contains(page);
    tab.customTitle = m_customTitles.value(page);
    tab.widgetTabBaseTitle = m_widgetTabBaseTitles.value(page);
    tab.wasGrouped = !gid.isEmpty();
    tab.groupName = tabGroupName(gid);
    tab.groupColor = tabGroupColor(gid);
    tab.containerId = view ? view->containerId() : QString();
    if (view) {
        auto sleep = m_sleepStates.constFind(view);
        if (sleep != m_sleepStates.constEnd()) {
            tab.hasSleepState = true;
            tab.sleepState = sleep.value();
        }
        auto pending = m_pendingRestores.constFind(view);
        if (pending != m_pendingRestores.constEnd()) {
            tab.hasPendingRestore = true;
            tab.pendingRestore = pending.value();
        }
        auto scroll = m_pendingScrollRestore.constFind(view);
        if (scroll != m_pendingScrollRestore.constEnd()) {
            tab.hasPendingScroll = true;
            tab.pendingScroll = scroll.value();
        }
        auto thumb = m_tabThumbnails.constFind(view);
        if (thumb != m_tabThumbnails.constEnd()) {
            tab.hasThumbnail = true;
            tab.thumbnail = thumb.value();
        }
    }

    // Sever this widget's bindings before the strip slot goes — the
    // page's signals and the bar's editing signal must stop landing
    // here.  Other receivers (managers, the bar's own view link) are
    // untouched, and the view's Qt-level parentage moves with the
    // page widget.
    if (view && view->enginePage()) {
        Engine::Page *enginePage = view->enginePage();
        disconnect(enginePage, nullptr, this, nullptr);
        for (WebActionMapper *mapper : std::as_const(m_actions)) {
            if (QAction *child = enginePage->action(mapper->webAction()))
                disconnect(child, nullptr, mapper, nullptr);
        }
    }
    if (view)
        disconnect(view, nullptr, this, nullptr);
    if (tab.bar) {
        disconnect(tab.bar, nullptr, this, nullptr);
        if (m_omniboxSuggestions)
            disconnect(tab.bar, nullptr, m_omniboxSuggestions, nullptr);
    }

    const QTabBar::ButtonPosition sides[2] = { QTabBar::LeftSide,
                                               QTabBar::RightSide };
    for (const QTabBar::ButtonPosition side : sides) {
        if (QWidget *button = m_tabBar->tabButton(index, side)) {
            m_tabBar->setTabButton(index, side, nullptr);
            button->deleteLater();
        }
    }
    m_locationBars->removeWidget(tab.bar);
    removeTab(index);
    m_pinnedTabs.remove(page);
    m_customTitles.remove(page);
    m_widgetTabBaseTitles.remove(page);
    if (view) {
        m_tabGroups.remove(view);
        m_sleepStates.remove(view);
        m_tabThumbnails.remove(view);
        m_pendingScrollRestore.remove(view);
        m_pendingRestores.remove(view);
        for (auto it = m_lastActiveInHeader.begin();
             it != m_lastActiveInHeader.end();) {
            if (it.value() == view)
                it = m_lastActiveInHeader.erase(it);
            else
                ++it;
        }
    }
    forgetTabGroupIfEmpty(gid);

    // The widgets join the target's tree before the emptied-strip
    // bookkeeping runs — a lastTabClosed that closes this window must
    // never take the moved tab with it.
    page->setParent(target);
    if (tab.bar)
        tab.bar->setParent(target->m_locationBars);

    syncContainerStrip();
    emit tabsChanged();

    const int adopted = target->adoptTransferredTab(tab, targetIndex);
    // Nothing may follow the lastTabClosed emit — it can close (and
    // delete) the window this widget lives in.
    if (count() == 0)
        emit lastTabClosed();
    return adopted >= 0;
}

// The adopt half — the tab lands in this strip at index (-1 appends)
// with its carried state re-keyed onto the same widgets.
int TabWidget::adoptTransferredTab(const TransferredTab &tab, int index)
{
    QWidget *page = tab.page;
    if (!page)
        return -1;
    WebViewWithSearch *withSearch = qobject_cast<WebViewWithSearch*>(page);
    WebView *view = withSearch ? withSearch->m_webView : nullptr;

    const int dst = index < 0 ? count() : qBound(0, index, count());
    if (tab.bar)
        m_locationBars->insertWidget(dst, tab.bar);
    const int idx = insertTab(dst, page, tab.icon,
        tab.text.isEmpty() ? tr("Untitled") : tab.text);
    setTabToolTip(idx, tab.toolTip);
    m_tabBar->setTabData(idx, tab.data);

    if (view) {
        // The page's signals still point at the widget it left —
        // bind this side's handlers and action children.
        wireWebView(view);
        if (LocationBar *bar = qobject_cast<LocationBar*>(tab.bar))
            wireLocationBar(bar);
    }

#if !defined(Q_OS_MACOS)
    // Rebuild the favicon label — the icon is URL-derived so it is
    // re-resolved rather than snapshotted, with the private badge for
    // off-the-record pages.
    if (view) {
        if (QLabel *label = animationLabel(idx, false)) {
            const QIcon icon = BrowserApplication::icon(view->url());
            label->setPixmap(isTabPrivate(idx)
                ? privateBadgedPixmap(icon) : icon.pixmap(16, 16));
        }
    }
#endif

    // TABS04's drop-position pin rule, same as moveTab(): landing
    // inside the pinned block pins, past its edge unpins — the
    // carried flag holds only at the boundary itself.
    setPinFlag(page, tab.pinned);
    {
        const bool pinned = m_pinnedTabs.contains(page);
        int others = 0;
        for (int i = 0; i < count(); ++i) {
            if (i != idx && isTabPinned(i))
                ++others;
        }
        if (pinned ? idx > others : idx < others)
            setPinFlag(page, !pinned);
    }

    if (!tab.customTitle.isEmpty())
        m_customTitles.insert(page, tab.customTitle);
    if (!tab.widgetTabBaseTitle.isEmpty())
        m_widgetTabBaseTitles.insert(page, tab.widgetTabBaseTitle);
    if (m_customTitles.contains(page))
        applyTabTitle(idx);

    // Group binding — a drop between two members of one target group
    // joins it (the in-bar normalize rule); otherwise the carried
    // binding re-materializes as a fresh group id (group ids are
    // per-window) keeping the name and color.
    if (view) {
        const QString left = tabGroupId(idx - 1);
        const QString right = tabGroupId(idx + 1);
        if (!left.isEmpty() && left == right) {
            assignTabGroup(view, left);
        } else if (tab.wasGrouped) {
            const QString newGid = nextTabGroupId();
            TabGroup group;
            group.id = newGid;
            group.name = tab.groupName;
            group.color = tab.groupColor;
            m_tabGroupInfo.insert(newGid, group);
            assignTabGroup(view, newGid);
        }
    }

    if (view) {
        if (tab.hasSleepState) {
            TabSleepState state = tab.sleepState;
            state.lastActiveMs = QDateTime::currentMSecsSinceEpoch();
            m_sleepStates.insert(view, state);
            if (state.sleeping)
                applySleepVisuals(idx, true);
        } else {
            m_sleepStates.insert(view, TabSleepState());
            markTabActivity(view);
        }
        if (tab.hasPendingRestore)
            m_pendingRestores.insert(view, tab.pendingRestore);
        if (tab.hasPendingScroll)
            m_pendingScrollRestore.insert(view, tab.pendingScroll);
        if (tab.hasThumbnail)
            m_tabThumbnails.insert(view, tab.thumbnail);
    }

    // CONT06: a merged tab bound to a filtered-out level joins that
    // level's hidden store — the strip keeps showing the active
    // header, and the level registers so the header row can pick it
    // back up.  Widget tabs belong to the default level the same way
    // addWidgetTab() files them.
    int visibleIndex = idx;
    if (m_twoLevelStrip && !m_containerFilterAdjust) {
        const QString cid = tab.containerId;
        if (!cid.isEmpty() && !m_containerHeaderOrder.contains(cid))
            m_containerHeaderOrder.append(cid);
        if (cid != m_activeContainerHeader) {
            detachTabIntoContainerStore(idx);
            visibleIndex = -1;
        }
        syncContainerStrip();
    }

    // Raise the moved tab in its new window — the drop was an
    // explicit "put it here", not a background open.  A tab filed
    // into a filtered-out level leaves the current tab alone.
    if (visibleIndex >= 0)
        setCurrentIndex(indexOf(page));
    if (count() == 1)
        currentChanged(currentIndex());
    m_tabBar->setIconSize(m_tabBar->iconSize());
    m_tabBar->updateGeometry();
    m_tabBar->update();
    emit tabsChanged();
    return idx;
}

// The tear-off: the drag ended unhandled, so the tab leaves for a
// fresh window — the real page moves (history, scroll and an
// in-flight load all ride along), and the usual lastTabClosed path
// decides whether the emptied window closes.
void TabWidget::tearTabToNewWindow(int index)
{
    QWidget *page = widget(index);
    if (!page)
        return;
    // instance() is null when qApp is not a BrowserApplication
    // (autotests) — a bare window keeps the path drivable there.
    BrowserApplication *application = BrowserApplication::instance();
    BrowserMainWindow *window = application
        ? application->newMainWindow()
        : new BrowserMainWindow();
    if (!window)
        return;
    if (!application)
        window->show();
    TabWidget *target = window->tabWidget();
    WebView *placeholder = target->webView(0);
    if (!moveTabTo(index, target, 0))
        return;
    if (target->count() > 1) {
        // The window ctor seeded a blank first tab — close it now
        // that the moved tab sits ahead of it.
        target->closeTab(1);
        return;
    }
    if (target->indexOf(page) >= 0)
        return;
    // CONT06: a two-level strip (the default) can file the moved tab
    // under its own container level — the window would open on the
    // placeholder instead.  Activate the tab's level so it is what
    // shows, then drop the never-loaded placeholder out of the
    // hidden store.
    if (WebViewWithSearch *withSearch =
            qobject_cast<WebViewWithSearch*>(page)) {
        target->setActiveContainerHeader(
            withSearch->m_webView->containerId());
        if (placeholder)
            target->closeHiddenTab(placeholder);
    }
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
        // TOR06: setTabButton() shows the label and QWidget's
        // first-show adjustSize() snaps it to the *current* size hint
        // — an empty label's 7x14 — and latches it: the private/tor
        // path below installs it before any pixmap exists, so the
        // 16px masked favicon then painted center-clipped (a "half-
        // hidden" icon at the strip's left edge).  Pin it to the icon
        // box; every payload is 16px (loading.gif, icon.pixmap(16),
        // privateBadgedPixmap).
        loadingAnimation->setFixedSize(16, 16);
    }
    if (addMovie && !loadingAnimation->movie()) {
        QMovie *movie = new QMovie(QLatin1String(":graphics/loading.gif"), QByteArray(), loadingAnimation);
        movie->setSpeed(50);
        loadingAnimation->setMovie(movie);
        movie->start();
        // PREFUI05: Energy Saver freezes the spinner on frame one.
        movie->setPaused(EnergySaver::instance()->active());
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
        return qobject_cast<WebView*>(adapter->view());
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
    // TPREV02: the page the cached thumbnail showed is being replaced
    // — drop it now so a pre-navigation frame can never surface as a
    // hover preview of the new page (covers same-url reloads, which
    // keep the url the entry is keyed against).
    m_tabThumbnails.remove(webView);
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

    // TABS05: a reopened tab's recorded scroll position re-applies on
    // its first finished load — same once-only pattern as the wake
    // restore above.
    auto scrollIt = m_pendingScrollRestore.find(webView);
    if (scrollIt != m_pendingScrollRestore.end()) {
        const QPointF pos = scrollIt.value();
        m_pendingScrollRestore.erase(scrollIt);
        if (ok)
            webView->enginePage()->runJavaScript(QStringLiteral(
                "window.scrollTo(%1, %2);").arg(pos.x()).arg(pos.y()));
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
    Q_UNUSED(title);
    WebView *webView = webViewForSender(sender());
    int index = webViewIndex(webView);
    markTabActivity(webView);
    if (-1 == index)
        return;
    // TABS12: applyTabTitle resolves the override first — a renamed
    // tab ignores the page's title stream.
    applyTabTitle(index);
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

// TABS05: snapshot the tab about to die — shared by closeTab's strip
// path (index >= 0, visuals readable) and closeHiddenTab's
// filtered-level path (index -1).  Off-the-record pages never enter:
// reopening would replay the url into a recorded-profile page, the
// very trace private browsing avoids (SEC07).
void TabWidget::pushRecentlyClosedTab(WebView *view, int index,
                                      const QString &containerId,
                                      const QString &groupId)
{
    // PREFUI04: a lazily restored tab has no live url/history yet —
    // its stashed pair is what "reopen closed tab" should carry.
    const auto pending = view ? m_pendingRestores.constFind(view)
                              : m_pendingRestores.constEnd();
    const QUrl closedUrl = pending != m_pendingRestores.constEnd()
        ? pending->url : (view ? view->url() : QUrl());
    if (!view || closedUrl.isEmpty()
        || (view->enginePage() && view->enginePage()->isOffTheRecord()))
        return;
    RecentlyClosedTab closed;
    closed.url = closedUrl;
    // The page title outlives the strip text — reopen restores the
    // full title, not whatever elided text the tab last painted.
    closed.title = view->title().isEmpty()
        ? QString::fromUtf8(closedUrl.toEncoded()) : view->title();
    closed.icon = BrowserApplication::icon(closedUrl);
    closed.index = index;
    closed.historyState = pending != m_pendingRestores.constEnd()
        ? pending->historyState
        : (view->enginePage()
           ? serializePageHistory(view->enginePage()) : QByteArray());
    // CONT02: the container id rides with the entry so "reopen
    // closed tab" returns to the same browsing context.
    closed.containerId = containerId;
    closed.groupId = groupId;
    // TABS04: the pin flag keys on the page widget — the view's
    // WebViewWithSearch parent — so it reads the same on and off the
    // strip.  TABS12: the rename override keys on it the same way.
    closed.pinned = m_pinnedTabs.contains(tabPageWidget(view));
    closed.customTitle = m_customTitles.value(tabPageWidget(view));
    if (view->enginePage()) {
        const QPointF scroll = view->enginePage()->scrollPosition();
        closed.scrollValid = true;
        closed.scrollX = scroll.x();
        closed.scrollY = scroll.y();
    }
    m_recentlyClosedTabs.prepend(closed);
    while (m_recentlyClosedTabs.size() > m_recentlyClosedTabsSize)
        m_recentlyClosedTabs.removeLast();
    m_recentlyClosedTabsAction->setEnabled(true);
}

// Shared undo path for openLastTab() and the recently-closed menu:
// recreate the tab on its recorded container, then put back the strip
// slot it was closed from — the pin block first, then a still-living
// group, otherwise the recorded index clamped to today's strip.
void TabWidget::restoreClosedTab(const RecentlyClosedTab &closed)
{
    // A container deleted since the close degrades to the default —
    // makeNewTabInContainer resolves unknown ids that way already.
    WebView *view = makeNewTabInContainer(closed.containerId, true);
    if (!view)
        return;
    // The engine cannot inject a serialized back/forward stack (see
    // createTab), so the history blob contributes its current entry;
    // an unreadable blob falls back to the flat url.
    const QUrl historyUrl = currentSerializedHistoryUrl(closed.historyState);
    view->loadUrl(historyUrl.isValid() ? historyUrl : closed.url);

    int index = webViewIndex(view);
    if (closed.pinned) {
        if (index >= 0)
            setTabPinned(index, true);
    } else if (!closed.groupId.isEmpty()
               && m_tabGroupInfo.contains(closed.groupId)) {
        // The group outlived the tab — rejoin it.  A collapsed group
        // reopens so the restored tab is actually visible (closeTab
        // expands before closing a chip, so the recorded state was
        // expanded anyway).
        assignTabGroup(view, closed.groupId);
        if (m_tabGroupInfo.value(closed.groupId).collapsed)
            setTabGroupCollapsed(closed.groupId, false);
        index = webViewIndex(view);
        if (index >= 0)
            moveTabIntoGroupRun(index);
    } else {
        const int target =
            qBound(pinnedTabCount(), closed.index, count() - 1);
        if (index >= 0 && index != target) {
            // m_groupAdjust keeps this positional move from rewriting
            // the tab's group membership (it has none yet anyway).
            m_groupAdjust = true;
            m_tabBar->moveTab(index, target);
            m_groupAdjust = false;
        }
    }
    if (closed.scrollValid && (closed.scrollX != 0 || closed.scrollY != 0))
        m_pendingScrollRestore.insert(
            view, QPointF(closed.scrollX, closed.scrollY));
    // TABS12: a renamed tab reopens renamed.
    setCustomTitleForPage(tabPageWidget(view), closed.customTitle);
}

void TabWidget::openLastTab()
{
    if (m_recentlyClosedTabs.isEmpty())
        return;
    restoreClosedTab(m_recentlyClosedTabs.takeFirst());
    m_recentlyClosedTabsAction->setEnabled(!m_recentlyClosedTabs.isEmpty());
}

void TabWidget::aboutToShowRecentTabsMenu()
{
    m_recentlyClosedTabsMenu->clear();
    for (int i = 0; i < m_recentlyClosedTabs.count(); ++i) {
        const RecentlyClosedTab &closed = m_recentlyClosedTabs.at(i);
        QAction *action = new QAction(m_recentlyClosedTabsMenu);
        // data() carries the record's stack position — the triggered
        // slot resolves and removes it.
        action->setData(i);
        QString label = closed.title;
        if (!closed.containerId.isEmpty()) {
            const QString name = ContainerManager::instance()
                ->containerForId(closed.containerId).name;
            if (!name.isEmpty())
                label = QStringLiteral("[%1] %2").arg(name, label);
        }
        action->setIcon(closed.icon.isNull()
                        ? BrowserApplication::icon(closed.url)
                        : closed.icon);
        action->setText(SafeText::menu(label));
        m_recentlyClosedTabsMenu->addAction(action);
    }
}

void TabWidget::aboutToShowRecentTriggeredAction(QAction *action)
{
    if (!action)
        return;

    const int i = action->data().toInt();
    if (i < 0 || i >= m_recentlyClosedTabs.size())
        return;
    restoreClosedTab(m_recentlyClosedTabs.takeAt(i));
    m_recentlyClosedTabsAction->setEnabled(!m_recentlyClosedTabs.isEmpty());
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

void TabWidget::paintEvent(QPaintEvent *event)
{
    QTabWidget::paintEvent(event);
    // THEM02: the selected card merges into the page — the pane's
    // hairline row next to the strip takes the card's color where
    // the card lands, so no seam boxes the lifted tab.  Vertical
    // rails keep floating cards and need no gap; the strip's own
    // edge line opens its matching segment in paintResizeGrip().
    if (!m_tabBar || !m_tabBar->isVisible() || currentIndex() < 0)
        return;
    const bool south = tabPosition() == QTabWidget::South;
    if (tabPosition() == QTabWidget::West
            || tabPosition() == QTabWidget::East)
        return;
    const QRect sel = m_tabBar->tabRect(currentIndex())
        .translated(m_tabBar->pos());
    QPainter painter(this);
    painter.setPen(Qt::NoPen);
    painter.setBrush(AroraStyle::tabActiveColor(palette()));
    painter.drawRect(sel.left() - 1,
                     south ? m_tabBar->geometry().top() - 1
                           : m_tabBar->geometry().bottom() + 1,
                     sel.width() + 2, 1);
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

// SRCH12: the opt-out's stored key is renamed — a persisted
// "searchEngineFallback" value can no longer be trusted: Qt4-era
// Preferences saves wrote it unconditionally under the old
// off-by-default checkbox, and pre-isolation test runs polluted real
// profiles with "false" (the recurring dead-Enter the user kept
// hitting).  The new "searchInAddressField" key carries the live
// opt-out; the legacy key is dropped on sight so a stale value can
// never gate search again — anyone who genuinely wants the bare-http
// guess re-unchecks the box once.
static bool searchInAddressFieldEnabled()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("urlloading"));
    if (settings.contains(QLatin1String("searchEngineFallback")))
        settings.remove(QLatin1String("searchEngineFallback"));
    return settings.value(QLatin1String("searchInAddressField"), true)
        .toBool();
}

#ifdef ARORA_RUSTCORE
// OMNI01: rc_classify_input() owns the url-or-search decision; this
// resolves its verdict against the Qt-owned engine table and the
// private-context routing.  An empty QUrl means FFI or JSON trouble —
// the caller falls back to the reference path.
QUrl TabWidget::guessUrlFromStringRust(const QString &string, bool privateContext)
{
    const QString trimmed = string.trimmed();
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();

    const bool searchFallback = searchInAddressFieldEnabled();

    QJsonObject options;
    options.insert(QLatin1String("search_fallback"), searchFallback);
    options.insert(QLatin1String("keywords"),
                   QJsonArray::fromStringList(manager->keywords()));
    const QByteArray optionsJson =
        QJsonDocument(options).toJson(QJsonDocument::Compact);

    char *json = rc_classify_input(trimmed.toUtf8().constData(),
                                   optionsJson.constData());
    if (!json)
        return QUrl();
    const QJsonObject verdict =
        QJsonDocument::fromJson(QByteArray(json)).object();
    rc_string_free(json);

    const QString kind = verdict.value(QLatin1String("kind")).toString();
    const auto parse = [](const QJsonValue &value) {
        return QUrl::fromEncoded(value.toString().toUtf8(),
                                 QUrl::TolerantMode);
    };
    if (kind == QLatin1String("navigate"))
        return parse(verdict.value(QLatin1String("url")));
    if (kind == QLatin1String("internal")) {
        QUrl url = parse(verdict.value(QLatin1String("url")));
        if (url.scheme() == QLatin1String("about")
            && url.path() == QLatin1String("home"))
            // TOR09/ENG13: same branded pick the C++ path makes —
            // tor/servo start pages ride the about:home alias.  The
            // pick must take privateContext, not the app-global flag:
            // a private tab's about:home (typed or Home) is the
            // private variant.
            url = BrowserApplication::startPageUrl(
                BrowserApplication::defaultEngineId(), privateContext);
        return url;
    }
    if (kind == QLatin1String("file"))
        return QUrl::fromLocalFile(
            verdict.value(QLatin1String("path")).toString());
    if (kind != QLatin1String("search"))
        return QUrl();

    // "engine" names a keyword to resolve, or is null for the
    // context engine.
    const QString keyword =
        verdict.value(QLatin1String("engine")).toString();
    QString query = verdict.value(QLatin1String("query")).toString();
    if (!keyword.isEmpty()) {
        if (OpenSearchEngine *engine =
                manager->engineForKeyword(keyword)) {
            const QUrl searchUrl = engine->searchUrl(query);
            if (!searchUrl.isEmpty() && searchUrl.isValid())
                return searchUrl;
        }
        // A keyword that vanished (or whose template produced an
        // unusable url) is a miss: the reference path re-routes the
        // raw input through the ordinary search fallback.
        query = trimmed;
    }
    if (searchFallback)
        return manager->searchUrlForEngine(
            manager->engineForContext(privateContext), query);
    // SRCH09/SRCH11: the bare-http opt-out only has meaning where the
    // guess produces a usable url — a guess that cannot parse
    // resolves to search instead of an invalid url loadUrl() would
    // silently drop.
    const QUrl guessed = QUrl::fromEncoded(
        (QLatin1String("http://") + trimmed).toUtf8(),
        QUrl::TolerantMode);
    if (guessed.isValid() && !guessed.host().isEmpty())
        return guessed;
    return manager->searchUrlForEngine(
        manager->engineForContext(privateContext), trimmed);
}
#endif // ARORA_RUSTCORE

QUrl TabWidget::guessUrlFromString(const QString &string)
{
    return guessUrlFromString(string, BrowserApplication::isPrivate());
}

QUrl TabWidget::guessUrlFromString(const QString &string, bool privateContext)
{
#ifdef ARORA_RUSTCORE
    const QUrl resolved = guessUrlFromStringRust(string, privateContext);
    if (!resolved.isEmpty())
        return resolved;
#endif
    return guessUrlFromStringCpp(string, privateContext);
}

QUrl TabWidget::guessUrlFromStringCpp(const QString &string, bool privateContext)
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
        url = BrowserApplication::startPageUrl(
            BrowserApplication::defaultEngineId(), privateContext);

    // SRCH01: the omnibox searches by default — the checkbox is now an
    // opt-out that restores the historic bare-http guess.
    const bool search = searchInAddressFieldEnabled();
    // SRCH04/PTAB01: private contexts (the app-global flag, tor mode
    // and per-tab private browsing) search through the configured
    // private engine.
    const auto fallbackUrl = [search, &trimmed, privateContext, manager]() -> QUrl {
        const auto engineSearch = [&trimmed, privateContext, manager]() -> QUrl {
            // SRCH07/SRCH09/SRCH11: stale or broken engine state resolves
            // through the shared rescue — a missing context engine
            // degrades to the bundled DuckDuckGo descriptor, an
            // unusable template to its endpoint directly.
            return manager->searchUrlForEngine(
                manager->engineForContext(privateContext), trimmed);
        };
        if (search)
            return engineSearch();
        const QString urlString = QLatin1String("http://") + trimmed;
        const QUrl guessed =
            QUrl::fromEncoded(urlString.toUtf8(), QUrl::TolerantMode);
        // SRCH09/SRCH11: the bare-http opt-out only has meaning where
        // the guess yields a usable url — a guess that cannot parse
        // (multi-word input, host-less or otherwise malformed text)
        // resolves to search instead of an invalid url loadUrl()
        // would silently drop.
        return guessed.isValid() && !guessed.host().isEmpty()
                   ? guessed
                   : engineSearch();
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
    // West/East in order.  The '+' is no longer a corner widget —
    // TABS15's in-strip button anchors right after the last tab in
    // every orientation; the single close button is the only corner
    // resident left (and it still hides while the bar is vertical —
    // QTabWidget gives corner widgets zero-size geometry there).
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
    // RSZ02: the horizontal strip's dragged height rides the same
    // fan-out (0 = Automatic).
    m_tabBar->reloadTabStripHeight();
    // TPREV01: the hover-preview opt-out rides the same fan-out.
    m_tabBar->reloadTabHoverPreviews();
    // RSZ01: the horizontal min/max width bounds ride it too.
    m_tabBar->reloadTabWidths();
    // TABS09: so does the icon-only view mode.
    m_tabBar->reloadIconOnlyTabs();

    bool newTabButtonInRightCorner = settings.value(QLatin1String("newTabButtonInRightCorner"), true).toBool();
    // TABS15: the strip's own '+' keeps this corner clear — the key
    // survives only to pick the single close button's corner.
    setCornerWidget(nullptr,
                    newTabButtonInRightCorner ? Qt::TopRightCorner : Qt::TopLeftCorner);

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

    // PREFUI04: lazy session restore — restored tabs defer their
    // first load until activation; pinned tabs can be exempted.
    settings.endGroup();
    settings.beginGroup(QLatin1String("performance"));
    m_lazyRestoreTabs = settings.value(
        QLatin1String("lazyRestoreTabs"), false).toBool();
    m_lazyRestorePinnedTabs = settings.value(
        QLatin1String("alwaysLoadPinnedTabs"), true).toBool();
    settings.endGroup();
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
    // SRCH09/SRCH11: an invalid url used to vanish here without a
    // trace — exactly the "nothing happens on Enter" the opt-out's
    // "http://two words" guess produced.  Complain so a dead url is
    // never mistaken for a dead key.
    if (!url.isValid()) {
        qWarning() << "TabWidget::loadUrl: refusing to load invalid"
                      "url" << url;
        return;
    }
    // ENG14: a current-tab open on a foreign-engine tab navigates
    // THAT tab's page — the parallel bar is editable now, so typed
    // urls/searches and every "open here" caller (bookmarks, history,
    // the home button) land on the engine page instead of silently
    // spawning a WebEngine tab.  A page-less engine tab (the backend
    // refused the page after the tab landed) cannot navigate — fall
    // through to the widget-tab fallback below.
    if (tab == CurrentTab) {
        if (EngineTab *foreign = engineTab(currentIndex())) {
            if (foreign->page()) {
                // ENG30: the destination's branding wins — a resolved
                // about:home (Home button, typed input, a bookmark)
                // names the DEFAULT engine's variant, which on a
                // foreign tab is the wrong chrome; re-pick it for
                // this backend.  Engine tabs are never private — the
                // newTab()/reloadTabInEngine() guards keep them on
                // standard contexts.
                const QUrl target = isBrandedStartPageUrl(url)
                    ? BrowserApplication::startPageUrl(
                          foreign->backend()->id(), false)
                    : url;
                if (QLineEdit *bar = locationBar(currentIndex()))
                    bar->setText(QString::fromUtf8(target.toEncoded()));
                foreign->load(target);
                return;
            }
        }
    }
    // PREFS01: a widget tab (Preferences, History) has no page to
    // navigate — a "current tab" open (bookmark, history entry, home
    // button) lands in a fresh tab instead of silently dying.
    if (tab == CurrentTab && !currentWebView())
        tab = NewSelectedTab;
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
                WebView *detachedView = new WebView(WebEngineProfileAdapter::forProfile(
                    currentView ? currentView->webPage()->profile()
                                : BrowserApplication::webEngineProfile()));
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

// PREFUI04 — lazy session restore ----------------------------------
//
// A restored tab that the user has not activated yet is never
// loadUrl()ed: the WebView stays blank while m_pendingRestores holds
// the url + the serialized history blob restoreState() decoded.  The
// tab keeps its slot, saved title and favicon so the strip reads
// fully restored; the first activation runs restorePendingTab() and
// the page loads on demand — same end state the eager path reaches,
// minus the startup cost of every background tab.

bool TabWidget::isTabPendingRestore(int index) const
{
    WebView *view = const_cast<TabWidget*>(this)->webView(index);
    return view && m_pendingRestores.contains(view);
}

QUrl TabWidget::pendingRestoreUrl(int index) const
{
    WebView *view = const_cast<TabWidget*>(this)->webView(index);
    return m_pendingRestores.value(view).url;
}

void TabWidget::restorePendingTab(int index)
{
    WebView *webView = this->webView(index);
    if (!webView)
        return;
    const auto it = m_pendingRestores.find(webView);
    if (it == m_pendingRestores.end())
        return;
    const QUrl url = it->url;
    m_pendingRestores.erase(it);
    if (index >= 0)
        locationBar(index)->setText(QString::fromUtf8(url.toEncoded()));
    webView->loadUrl(url);
    markTabActivity(webView);
}

// The title the session blob does not store: a restored tab's last
// known title comes from the history store, falling back to the url
// itself (the webViewTitleChanged fallback's behavior).
static QString restoredTabTitle(const QUrl &url)
{
    const QString urlString = QString::fromUtf8(url.toEncoded());
    if (HistoryManager *history = BrowserApplication::historyManager()) {
        const QList<HistoryEntry> entries = history->history();
        for (const HistoryEntry &entry : entries) {
            if (entry.url == urlString && !entry.title.isEmpty())
                return entry.title;
        }
    }
    return QString();
}

static const qint32 TabWidgetMagic = 0xaa;

TabWidget::TabSessionSnapshot TabWidget::collectSessionSnapshot() const
{
    TabSessionSnapshot snap;
    // Private tabs live on the off-the-record profile — their urls and
    // history are never written into the saved session (SEC07).  The
    // current index is remapped onto the filtered list.
    // orderedWebViews() splices a collapsed group's hidden members in
    // after its chip so no tab drops out of the serialized session.
    const QList<WebView*> ordered = orderedWebViews();
    for (WebView *tab : ordered) {
        if (!tab)
            continue;
        if (tab->enginePage() && tab->enginePage()->isOffTheRecord())
            continue;
        if (tab == currentWebView())
            snap.currentIndex = snap.urls.count();
        // PREFUI04: a lazily restored tab never loaded its page — its
        // stashed url + history blob stand in for the live state so a
        // re-saved session loses nothing.
        const auto pending = m_pendingRestores.constFind(tab);
        if (pending != m_pendingRestores.constEnd()) {
            snap.urls.append(QString::fromUtf8(pending->url.toEncoded()));
            snap.histories.append(pending->historyState);
        } else {
            snap.urls.append(QString::fromUtf8(tab->url().toEncoded()));
            if (tab->enginePage()->historyCount() != 0)
                snap.histories.append(serializePageHistory(tab->enginePage()));
            else
                snap.histories.append(QByteArray());
        }
        snap.containers.append(tab->containerId());
        snap.groupIds.append(m_tabGroups.value(tab));
        // TABS04: the pin flag keys on the page widget — the view's
        // WebViewWithSearch parent, which is also what the strip and
        // the hidden stores hold.
        snap.pinned.append(m_pinnedTabs.contains(tabPageWidget(tab))
                           ? 1 : 0);
        // TABS12: the rename override keys on the same page widget.
        snap.titles.append(m_customTitles.value(tabPageWidget(tab)));
    }

    // TABGRP01: the group table — one record per group that still has a
    // saved member, ordered by first appearance on the strip.  Orphaned
    // ids in groupIds (a record that somehow vanished) degrade to
    // ungrouped on restore.
    for (const QString &gid : snap.groupIds) {
        if (gid.isEmpty())
            continue;
        const auto it = m_tabGroupInfo.constFind(gid);
        if (it == m_tabGroupInfo.constEnd())
            continue;
        bool listed = false;
        for (const TabGroup &group : snap.groups) {
            if (group.id == gid) {
                listed = true;
                break;
            }
        }
        if (!listed)
            snap.groups.append(*it);
    }
    // CONT06: the window's active level-1 selection —
    // restoreSessionSnapshot() re-applies it once the strip is rebuilt.
    snap.activeContainerHeader = m_activeContainerHeader;
    return snap;
}

QByteArray TabWidget::saveState() const
{
    int version = 6; // TABS12: v6 tails with each tab's rename override
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);

    stream << qint32(TabWidgetMagic);
    stream << qint32(version);

    const TabSessionSnapshot snap = collectSessionSnapshot();
    stream << snap.urls;
    stream << snap.currentIndex;
    stream << snap.histories;
    stream << snap.containers;
    stream << snap.groupIds;
    stream << qint32(snap.groups.count());
    for (const TabGroup &group : snap.groups)
        stream << group.id << group.name << group.color
               << qint32(group.collapsed ? 1 : 0);

    // CONT06: the window's active level-1 selection — restoreState()
    // re-applies it once the strip is rebuilt.
    stream << snap.activeContainerHeader;

    // TABS04: the per-tab pinned flags — read back under v>=5.
    stream << snap.pinned;

    // TABS12: the per-tab rename overrides — read back under v>=6.
    stream << snap.titles;

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
    if (marker != TabWidgetMagic || v < 1 || v > 6)
        return false;

    TabSessionSnapshot snap;
    StreamingUtils::readBoundedList(stream, snap.urls);

    stream >> snap.currentIndex;
    StreamingUtils::readBoundedList(stream, snap.histories);
    // CONT02: v2 tails the stream with each tab's container id; a v1
    // session (or a truncated blob) restores to the default container.
    if (v >= 2)
        StreamingUtils::readBoundedList(stream, snap.containers);
    // TABGRP01: v3 tails with each tab's group id plus the group table
    // (name/color/collapsed).  A truncated tail simply leaves the
    // session ungrouped — the same defensive floor the container tail
    // established.
    if (v >= 3) {
        StreamingUtils::readBoundedList(stream, snap.groupIds);
        qint32 groupCount = 0;
        stream >> groupCount;
        for (qint32 i = 0; i < groupCount && i < 1024; ++i) {
            TabGroup group;
            qint32 collapsed = 0;
            stream >> group.id >> group.name >> group.color >> collapsed;
            group.collapsed = (collapsed != 0);
            snap.groups.append(group);
        }
    }
    // CONT06: v4 tails with the window's active container header — a
    // truncated tail restores on the default header.
    if (v >= 4)
        stream >> snap.activeContainerHeader;
    // TABS04: v5 tails with each tab's pinned flag — a missing or
    // truncated tail restores everything unpinned, the same
    // defensive floor the earlier tails established.
    if (v >= 5)
        StreamingUtils::readBoundedList(stream, snap.pinned);
    // TABS12: v6 tails with each tab's rename override — a missing or
    // truncated tail restores every tab on its live page title, the
    // same defensive floor the earlier tails established.
    if (v >= 6)
        StreamingUtils::readBoundedList(stream, snap.titles);
    if (stream.status() != QDataStream::Ok)
        return false;

    return restoreSessionSnapshot(snap);
}

bool TabWidget::restoreSessionSnapshot(const TabSessionSnapshot &snap)
{
    const QStringList &openTabs = snap.urls;
    const int currentTab = snap.currentIndex;
    const QList<QByteArray> &tabHistory = snap.histories;
    const QStringList &tabContainers = snap.containers;
    const QStringList &savedGroupIds = snap.groupIds;
    const QList<TabGroup> &savedGroups = snap.groups;
    const QList<qint32> &savedPinned = snap.pinned;
    const QStringList &savedTitles = snap.titles;

    // The empty placeholder tab a fresh window comes with is only a
    // fit for the first saved tab when its container matches — reusing
    // it for a container tab would put the restored page on the wrong
    // profile, so it is closed after the loop instead.
    bool leftoverPlaceholder = false;
    WebView *placeholderView = nullptr;
    QList<WebView*> createdViews;
    // TABS04: saved order already has the pinned prefix — appending
    // (rather than the interactive insert-after-pinned rule) keeps the
    // unpinned tabs in their serialized sequence.
    m_sessionRestore = true;
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
            && currentWebView() && currentWebView()->url() == QUrl()) {
            leftoverPlaceholder = true;
            placeholderView = currentWebView();
        }
        WebView *webView = reusePlaceholder
            ? currentWebView()
            : makeNewTabInContainer(containerId, false);
        createdViews.append(webView);
        if (webView) {
            // TABS04: flag the restored pin before the next saved tab
            // is created — the pinned-prefix insert then keeps the
            // strip order identical to the one that was saved.
            if (savedPinned.value(i)) {
                const int index = webViewIndex(webView);
                if (index >= 0)
                    setPinFlag(widget(index), true);
            }
            // PREFUI04: under lazy restore a background tab stashes
            // its url + history blob instead of loading — the page
            // only loads when the tab is first activated (or saved
            // out again, which re-serializes the stash).  The
            // "Always Load Pinned Tabs" opt-out exempts pinned tabs;
            // the tab that ends up selected is flushed at the end of
            // restoreState() either way.
            const bool deferred = m_lazyRestoreTabs
                && !(savedPinned.value(i) && m_lazyRestorePinnedTabs);
            const int index = webViewIndex(webView);
            if (deferred && index >= 0) {
                m_pendingRestores.insert(webView, {url, historyState});
                // The strip reads restored before the first load:
                // saved title (history store lookup, url fallback)
                // and the persisted favicon.
                locationBar(index)->setText(
                    QString::fromUtf8(url.toEncoded()));
                m_tabBar->setTabData(index, url);
                const QString title = restoredTabTitle(url);
                const QString tabTitle = title.isEmpty()
                    ? QString::fromUtf8(url.toEncoded()) : title;
                setTabText(index, SafeText::menu(tabTitle));
                setTabToolTip(index, SafeText::escaped(tabTitle));
                const QIcon icon = BrowserApplication::icon(url);
                if (QLabel *label = animationLabel(index, false)) {
                    label->setPixmap(isTabPrivate(index)
                        ? privateBadgedPixmap(icon)
                        : icon.pixmap(16, 16));
                }
            } else {
                webView->loadUrl(url);
            }
            // TABS12: the saved rename override lands last so it wins
            // over the restored-from-history title above; a tab that
            // detached into a filtered level at creation only gets
            // the map write — the level's reinsert path reapplies it.
            setCustomTitleForPage(tabPageWidget(webView),
                                  savedTitles.value(i));
        }
    }
    m_sessionRestore = false;
    if (leftoverPlaceholder && count() > 1) {
        // Resolve by view, not index 0 — TABS04 inserts restored tabs
        // after the pinned block, which may sit ahead of the
        // placeholder.
        const int index = webViewIndex(placeholderView);
        if (index >= 0)
            closeTab(index);
    }

    // TABGRP01: rebuild the group table under fresh ids, then attach
    // each saved tab to its group.  Groups go in expanded — the
    // collapse pass runs only after all membership is set (collapsing
    // earlier would hide the chip later members index against, and
    // setTabGroupCollapsed no-ops if the flag is already latched).
    QHash<QString, QString> groupIdRemap;
    QStringList collapsedGroups;
    for (const TabGroup &saved : savedGroups) {
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

    // TABS04: pin flags were applied during creation, where a
    // non-prefix flag list (a crafted blob — a real session always
    // saves pinned tabs first) may have shuffled the order — repair
    // the prefix before selection indices are resolved.
    normalizePinnedBlock();

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
    // The selected tab flushes its deferral as soon as it becomes
    // current — setCurrentIndex() only fires currentChanged() when
    // the index moves, so cover the already-current case too.  This
    // must run after the saved index is applied: the pre-restore
    // current tab is not necessarily the saved-current one.
    restorePendingTab(currentIndex());
    // CONT06: rebuild the level filter — the saved header wins when
    // it still owns tabs, otherwise the saved current tab's container
    // (a window restored on "Work" lands there, not on "Tabs").
    if (m_twoLevelStrip) {
        QString header = snap.activeContainerHeader;
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

#ifdef ARORA_RUSTCORE
// RCORE03: the rustcore session store consumes this JSON manifest —
// {"current":i,"tabs":[{url,container,group,engine,state}],
//  "groups":[{id,name,color,collapsed}]}.  "state" is the opaque
// engine-state blob (serializePageHistory's output, base64) and
// "engine" the tag that produced it, so a future engine adapter's
// tabs coexist without the core ever interpreting either field.
QJsonObject TabWidget::sessionStateJson() const
{
    const TabSessionSnapshot snap = collectSessionSnapshot();
    QJsonArray tabs;
    for (int i = 0; i < snap.urls.count(); ++i) {
        QJsonObject tab;
        tab.insert(QLatin1String("url"), snap.urls.at(i));
        tab.insert(QLatin1String("container"), snap.containers.value(i));
        tab.insert(QLatin1String("group"), snap.groupIds.value(i));
        tab.insert(QLatin1String("engine"), QLatin1String("webengine"));
        tab.insert(QLatin1String("state"),
                   QString::fromLatin1(snap.histories.value(i).toBase64()));
        tabs.append(tab);
    }
    QJsonArray groups;
    for (const TabGroup &group : snap.groups) {
        QJsonObject g;
        g.insert(QLatin1String("id"), group.id);
        g.insert(QLatin1String("name"), group.name);
        g.insert(QLatin1String("color"), group.color.name(QColor::HexArgb));
        g.insert(QLatin1String("collapsed"), group.collapsed);
        groups.append(g);
    }
    QJsonObject state;
    state.insert(QLatin1String("current"), snap.currentIndex);
    state.insert(QLatin1String("activeContainer"), snap.activeContainerHeader);
    state.insert(QLatin1String("tabs"), tabs);
    state.insert(QLatin1String("groups"), groups);
    return state;
}

bool TabWidget::restoreSessionState(const QJsonObject &state)
{
    TabSessionSnapshot snap;
    snap.currentIndex = state.value(QLatin1String("current")).toInt(-1);
    snap.activeContainerHeader =
        state.value(QLatin1String("activeContainer")).toString();
    const QJsonArray tabs = state.value(QLatin1String("tabs")).toArray();
    for (const QJsonValue &v : tabs) {
        const QJsonObject tab = v.toObject();
        snap.urls.append(tab.value(QLatin1String("url")).toString());
        snap.containers.append(
            tab.value(QLatin1String("container")).toString());
        snap.groupIds.append(tab.value(QLatin1String("group")).toString());
        // The opaque engine-state blob routes by engine tag — only
        // WebEngine's serializePageHistory format is understood here;
        // a foreign engine's tab still restores its flat url.
        QByteArray engineState;
        if (tab.value(QLatin1String("engine"))
                .toString(QLatin1String("webengine"))
            == QLatin1String("webengine"))
            engineState = QByteArray::fromBase64(
                tab.value(QLatin1String("state")).toString().toLatin1());
        snap.histories.append(engineState);
    }
    const QJsonArray groups = state.value(QLatin1String("groups")).toArray();
    for (const QJsonValue &v : groups) {
        const QJsonObject g = v.toObject();
        TabGroup group;
        group.id = g.value(QLatin1String("id")).toString();
        group.name = g.value(QLatin1String("name")).toString();
        group.color = QColor::fromString(
            g.value(QLatin1String("color")).toString());
        group.collapsed = g.value(QLatin1String("collapsed")).toBool();
        snap.groups.append(group);
    }
    return restoreSessionSnapshot(snap);
}
#endif // ARORA_RUSTCORE

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

