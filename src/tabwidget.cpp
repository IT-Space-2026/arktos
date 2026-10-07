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
#include "history.h"
#include "historycompleter.h"
#include "historymanager.h"
#include "locationbar.h"
#include "omniboxsuggestions.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "safetext.h"
#include "streamingutils.h"
#include "tabbar.h"
#include "toolbarsearch.h"
#include "webactionmapper.h"
#include "webpage.h"
#include "webview.h"
#include "webviewsearch.h"

#include <qabstractproxymodel.h>
#include <qcompleter.h>
#include <qdir.h>
#include <qevent.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qmessagebox.h>
#include <qmovie.h>
#include <qregularexpression.h>
#include <qsettings.h>
#include <qstackedwidget.h>
#include <qstyle.h>
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

    // Initialize Actions' labels
    retranslate();
    loadSettings();
}

void TabWidget::historyCleared()
{
    m_recentlyClosedTabs.clear();
    m_recentlyClosedTabsAction->setEnabled(false);
}

void TabWidget::clear()
{
    // clear the recently closed tabs
    m_recentlyClosedTabs.clear();
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

    // webview — created on the application profile (BrowserProfile's
    // named "arora" profile, or the off-the-record profile while
    // private browsing is on).  BrowserApplication::webEngineProfile()
    // is a static accessor, so this is also safe under autotests that
    // never instantiate the application object.
    WebView *webView = new WebView(BrowserApplication::webEngineProfile());
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
        if (m_recentlyClosedTabs.size() >= TabWidget::m_recentlyClosedTabsSize) {
            m_recentlyClosedTabs.removeLast();
            m_recentlyClosedTabsHistory.removeLast();
        }
    }
    QWidget *lineEdit = m_locationBars->widget(index);
    m_locationBars->removeWidget(lineEdit);
    lineEdit->deleteLater();

    QWidget *webViewWithSearch = widget(index);
    removeTab(index);
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
    if (-1 == index)
        return;
    QString tabTitle = title;
    if (title.isEmpty())
        tabTitle = QString::fromUtf8(webView->url().toEncoded());
    // The title is page-controlled: '&' would become a mnemonic on the
    // tab label and markup would render in the tooltip (tooltips are
    // always rich-text capable).
    setTabText(index, SafeText::menu(tabTitle));
    setTabToolTip(index, SafeText::escaped(tabTitle));
    if (currentIndex() == index)
        emit setCurrentTitle(title);
    // History title updates are handled by WebPage::init (MIG06).
}

void TabWidget::webViewUrlChanged(const QUrl &url)
{
    WebView *webView = qobject_cast<WebView*>(sender());
    int index = webViewIndex(webView);
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
    if (!historyState.isEmpty())
        createTab(historyState, NewTab);
    else
        loadUrl(url, NewTab);
    m_recentlyClosedTabsAction->setEnabled(!m_recentlyClosedTabs.isEmpty());
}

void TabWidget::aboutToShowRecentTabsMenu()
{
    m_recentlyClosedTabsMenu->clear();
    for (int i = 0; i < m_recentlyClosedTabs.count(); ++i) {
        QAction *action = new QAction(m_recentlyClosedTabsMenu);
        action->setData(m_recentlyClosedTabsHistory.at(i));
        QIcon icon = BrowserApplication::icon(m_recentlyClosedTabs.at(i));
        action->setIcon(icon);
        action->setText(SafeText::menu(m_recentlyClosedTabs.at(i).toString()));
        m_recentlyClosedTabsMenu->addAction(action);
    }
}

void TabWidget::aboutToShowRecentTriggeredAction(QAction *action)
{
    if (!action)
        return;

    QByteArray historyState = action->data().toByteArray();
    if (!historyState.isEmpty())
        createTab(historyState, NewTab);
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
                BrowserMainWindow *newMainWindow = application->newMainWindow();
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
    int version = 1;
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);

    stream << qint32(TabWidgetMagic);
    stream << qint32(version);

    QStringList tabs;
    QList<QByteArray> tabsHistory;
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
    }
    stream << tabs;
    stream << savedCurrentIndex;
    stream << tabsHistory;

    return data;
}

bool TabWidget::restoreState(const QByteArray &state)
{
    int version = 1;
    QByteArray sd = state;
    QDataStream stream(&sd, QIODevice::ReadOnly);
    if (stream.atEnd())
        return false;

    qint32 marker;
    qint32 v;
    stream >> marker;
    stream >> v;
    if (marker != TabWidgetMagic || v != version)
        return false;

    QStringList openTabs;
    StreamingUtils::readBoundedList(stream, openTabs);

    int currentTab = -1;
    stream >> currentTab;
    QList<QByteArray> tabHistory;
    StreamingUtils::readBoundedList(stream, tabHistory);
    if (stream.status() != QDataStream::Ok)
        return false;

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
        const TabWidget::OpenUrlIn tab = i == 0
            && (!currentWebView() || currentWebView()->url() == QUrl())
            ? CurrentTab : NewTab;
        if (WebView *webView = getView(tab, currentWebView()))
            webView->loadUrl(url);
    }
    // The saved index is only selectable once the restored tabs exist —
    // setting it before creating them is a no-op against the single
    // placeholder tab.
    if (currentTab >= 0 && currentTab < count())
        setCurrentIndex(currentTab);
    return true;
}

void TabWidget::createTab(const QByteArray &historyState, TabWidget::OpenUrlIn tab)
{
    // Qt WebEngine cannot inject a serialized back/forward stack into a
    // page (there is no QDataStream << QWebEngineHistory), so only the
    // entry that was current when the tab was saved is reopened.
    QUrl url = currentSerializedHistoryUrl(historyState);
    if (!url.isValid())
        return;
    if (WebView *webView = getView(tab, currentWebView()))
        webView->loadUrl(url);
}

