/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2008 Jason A. Donenfeld <Jason@zx2c4.com>
 * Copyright 2008 Ariya Hidayat <ariya.hidayat@gmail.com>
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

#include "webview.h"

#include "adblockdialog.h"
#include "adblockmanager.h"
#include "addbookmarkdialog.h"
#include "browserapplication.h"
#include "browsermainwindow.h"
#include "containermanager.h"
#include "devtoolswindow.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "safetext.h"
#include "scriptblockinfobar.h"
#include "scriptcontrolmanager.h"
#include "toolbarsearch.h"
#include "webpage.h"

#include <qapplication.h>
#include <qclipboard.h>
#include <qdebug.h>
#include <qmenubar.h>
#include <qevent.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <qsettings.h>
#include <qwebenginecontextmenurequest.h>
#include <qwebenginehttprequest.h>
#include <qwebengineprofile.h>

WebView::WebView(QWidget *parent)
    : QWebEngineView(parent)
    , m_progress(0)
    , m_currentZoom(100)
    , m_page(new WebPage(this))
    , m_scriptBlockBar(nullptr)
{
    init();
}

WebView::WebView(QWebEngineProfile *profile, QWidget *parent)
    : QWebEngineView(parent)
    , m_progress(0)
    , m_currentZoom(100)
    , m_page(new WebPage(profile, this))
    , m_scriptBlockBar(nullptr)
{
    init();
}

void WebView::init()
{
    setPage(m_page);
    connect(m_page, &QWebEnginePage::linkHovered,
            this, &WebView::setStatusBarText);
    connect(m_page, &QWebEnginePage::loadStarted,
            this, [this]() { setStatusBarText(QString()); });
    connect(this, &QWebEngineView::loadProgress,
            this, &WebView::setProgress);
    connect(this, &QWebEngineView::loadFinished,
            this, [this]() { loadFinished(); });
    connect(m_page, &WebPage::aboutToLoadUrl,
            this, &QWebEngineView::urlChanged);

    // JSCTL: the per-site/tier script decision is applied to the page
    // before each navigation commits (WebPage::applyJavaScriptPolicy);
    // this bar is the visible "scripts are off" notice and the undo
    // path.
    m_scriptBlockBar = new ScriptBlockInfoBar(this);
    m_scriptBlockBar->hide();
    connect(m_page, &WebPage::javaScriptBlockedChanged,
            this, &WebView::updateScriptBlockBar);
    connect(m_page, &WebPage::javaScriptBlockedChanged,
            this, &WebView::javaScriptBlockedChanged);
    connect(m_scriptBlockBar, &ScriptBlockInfoBar::allowOnce,
            this, [this]() { allowScriptsOnThisSite(false); });
    connect(m_scriptBlockBar, &ScriptBlockInfoBar::allowAlways,
            this, [this]() { allowScriptsOnThisSite(true); });
    // Qt WebEngine has no text-only zoom mode (Chromium zooms the whole
    // page), but keep the zoom-text-only toggle re-applying the zoom so
    // the preference stays wired to something visible.
    if (BrowserApplication *application = BrowserApplication::instance())
        connect(application, &BrowserApplication::zoomTextOnlyChanged,
                this, &WebView::applyZoom);
    setAcceptDrops(true);

    // the zoom values (in percent) are chosen to be like in Mozilla Firefox 3
    m_zoomLevels << 30 << 50 << 67 << 80 << 90;
    m_zoomLevels << 100;
    m_zoomLevels << 110 << 120 << 133 << 150 << 170 << 200 << 240 << 300;
    loadSettings();
}

void WebView::loadSettings()
{
    m_page->loadSettings();
}

TabWidget *WebView::tabWidget() const
{
    QObject *widget = this->parent();
    while (widget) {
        if (TabWidget *tabWidget = qobject_cast<TabWidget*>(widget))
            return tabWidget;
        widget = widget->parent();
    }
    return nullptr;
}

// CONT02: a tab's container is a property of its page's profile —
// bound at WebView construction and fixed for the view's lifetime.
QString WebView::containerId() const
{
    if (!m_page)
        return ContainerManager::defaultContainerId();
    return ContainerManager::instance()
        ->containerIdForProfile(m_page->profile());
}

void WebView::contextMenuEvent(QContextMenuEvent *event)
{
    QWebEngineContextMenuRequest *request = lastContextMenuRequest();
    if (!request) {
        QWebEngineView::contextMenuEvent(event);
        return;
    }

    QMenu *menu = new QMenu(this);

    if (!request->linkUrl().isEmpty()) {
        QAction *newWindowAction = menu->addAction(tr("Open in New &Window"), this, &WebView::openActionUrlInNewWindow);
        newWindowAction->setData(request->linkUrl());
        QAction *newTabAction = menu->addAction(tr("Open in New &Tab"), this, &WebView::openActionUrlInNewTab);
        newTabAction->setData(request->linkUrl());
        menu->addSeparator();
        QAction *saveLinkAction = menu->addAction(tr("Save Lin&k"), this, &WebView::downloadLinkToDisk);
        saveLinkAction->setData(request->linkUrl());
        QAction *bookmarkAction = menu->addAction(tr("&Bookmark This Link"), this, &WebView::bookmarkLink);
        bookmarkAction->setData(request->linkUrl());
        menu->addSeparator();
        if (!request->selectedText().isEmpty())
            menu->addAction(pageAction(QWebEnginePage::Copy));
        QAction *copyLinkAction = menu->addAction(tr("&Copy Link Location"), this, &WebView::copyLinkToClipboard);
        copyLinkAction->setData(request->linkUrl());
    }

    if (request->mediaType() == QWebEngineContextMenuRequest::MediaTypeImage
        && !request->mediaUrl().isEmpty()) {
        if (!menu->isEmpty())
            menu->addSeparator();
        QAction *newWindowAction = menu->addAction(tr("Open Image in New &Window"), this, &WebView::openActionUrlInNewWindow);
        newWindowAction->setData(request->mediaUrl());
        QAction *newTabAction = menu->addAction(tr("Open Image in New &Tab"), this, &WebView::openActionUrlInNewTab);
        newTabAction->setData(request->mediaUrl());
        menu->addSeparator();
        QAction *saveImageAction = menu->addAction(tr("&Save Image"), this, &WebView::downloadImageToDisk);
        saveImageAction->setData(request->mediaUrl());
        menu->addAction(tr("&Copy Image"), this, &WebView::copyImageToClipboard);
        menu->addAction(tr("C&opy Image Location"), this, &WebView::copyImageLocationToClipboard)->setData(request->mediaUrl().toString());
        menu->addSeparator();
        menu->addAction(tr("Block Image"), this, &WebView::blockImage)->setData(request->mediaUrl().toString());

        // SRCH04: reverse-image search through the configured image
        // engine — only offered when an engine actually advertises an
        // image-search endpoint.
        if (OpenSearchEngine *imageEngine =
                ToolbarSearch::openSearchManager()->imageSearchEngine()) {
            QAction *imageSearchAction = menu->addAction(
                tr("Search Image with %1")
                    .arg(SafeText::menu(imageEngine->name())),
                this, &WebView::imageSearchRequested);
            imageSearchAction->setData(request->mediaUrl());
        }
    }

    if (!request->selectedText().isEmpty()) {
        if (menu->isEmpty()) {
            menu->addAction(pageAction(QWebEnginePage::Copy));
        } else {
            menu->addSeparator();
        }
        QMenu *searchMenu = menu->addMenu(tr("Search with"));
        const QStringList engineNames = ToolbarSearch::openSearchManager()->allEnginesNames();
        for (const QString &name : engineNames) {
            // Engine names come from opensearch XML; keep '&'/'\t'
            // out of the menu text and carry the real name in data.
            QAction *action = searchMenu->addAction(SafeText::menu(name));
            action->setData(name);
            connect(action, &QAction::triggered,
                    this, [this, action]() { searchRequested(action); });
        }
    }

    if (request->isContentEditable()) {
        // TODO(MIG08): "Add to the toolbar search" — needs the input element's
        // form data via runJavaScript (was synchronous QWebElement access).
    }

    if (menu->isEmpty()) {
        delete menu;
        menu = createStandardContextMenu();
        // The stock menu's own "Inspect element" is the same no-op
        // bare page action — remove it; the hosted one is added below.
        menu->removeAction(pageAction(QWebEnginePage::InspectElement));
    }
    if (!menu->isEmpty())
        menu->addSeparator();
    // The bare page action is a no-op until a devToolsPage is bound —
    // route it through the shared inspector host (DVT01).  Triggering
    // InspectElement still uses the stored context-menu position, so
    // the right-clicked element is the one inspected.
    QAction *inspectPageAction = pageAction(QWebEnginePage::InspectElement);
    QAction *inspectAction = menu->addAction(inspectPageAction->text());
    inspectAction->setIcon(inspectPageAction->icon());
    connect(inspectAction, &QAction::triggered,
            this, [this]() { DevToolsWindow::inspectElement(m_page); });

    if (!menu->isEmpty()) {
        if (BrowserMainWindow *window = BrowserMainWindow::parentWindow(this)) {
            if (!window->menuBar()->isVisible())
                menu->addAction(window->showMenuBarAction());
        }

        menu->exec(event->globalPos());
        delete menu;
        return;
    }
    delete menu;

    QWebEngineView::contextMenuEvent(event);
}

void WebView::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        int numDegrees = event->angleDelta().y() / 8;
        int numSteps = numDegrees / 15;
        m_currentZoom = m_currentZoom + numSteps * 10;
        applyZoom();
        event->accept();
        return;
    }
    QWebEngineView::wheelEvent(event);
}

void WebView::downloadLinkToDisk()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        QUrl linkUrl = action->data().toUrl();
        if (!linkUrl.isEmpty())
            m_page->download(linkUrl);
    }
}

void WebView::copyLinkToClipboard()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        QApplication::clipboard()->setText(action->data().toUrl().toString());
}

void WebView::openActionUrlInNewTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openUrlInTarget(action->data().toUrl(), TabWidget::NewNotSelectedTab);
}

void WebView::openActionUrlInNewWindow()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openUrlInTarget(action->data().toUrl(), TabWidget::NewWindow);
}

void WebView::openUrlInTarget(const QUrl &linkUrl, TabWidget::OpenUrlIn target)
{
    WebView *newView = nullptr;
    if (TabWidget *tabs = tabWidget())
        newView = tabs->getView(target, this);
    if (!newView) {
        // Detached view (no TabWidget above us): fall back to a
        // standalone window on the same profile.
        newView = new WebView(m_page->profile());
        newView->setAttribute(Qt::WA_DeleteOnClose);
        newView->show();
    }
    QWebEngineHttpRequest request(linkUrl);
    request.setHeader("Referer", url().toEncoded());
    newView->load(request);
}

void WebView::downloadImageToDisk()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        QUrl imageUrl = action->data().toUrl();
        if (!imageUrl.isEmpty())
            m_page->download(imageUrl);
    }
}

void WebView::copyImageToClipboard()
{
    // No URL equivalent: copying the image pixels has to go through the
    // page action, which uses the live context-menu request.
    pageAction(QWebEnginePage::CopyImageToClipboard)->trigger();
}

void WebView::copyImageLocationToClipboard()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        QApplication::clipboard()->setText(action->data().toString());
    }
}

void WebView::blockImage()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        AdBlockManager::instance()->showDialog()->addCustomRule(action->data().toString());
}

void WebView::bookmarkLink()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        AddBookmarkDialog dialog(this);
        dialog.setUrl(action->data().toUrl().toString());
        dialog.exec();
    }
}

void WebView::searchRequested(QAction *action)
{
    if (!action)
        return;
    // The menu text is mnemonic-escaped; the real engine name is in
    // the action data.
    OpenSearchEngine *engine =
        ToolbarSearch::openSearchManager()->engine(action->data().toString());
    if (!engine || selectedText().isEmpty())
        return;
    // SRCH04: selection search can open in a background tab instead
    // of stealing focus.
    QSettings settings;
    const bool background = settings.value(
        QLatin1String("urlloading/selectionSearchInBackground"),
        false).toBool();
    emit search(engine->searchUrl(selectedText()),
                background ? TabWidget::NewNotSelectedTab
                           : TabWidget::NewSelectedTab);
}

void WebView::imageSearchRequested()
{
    QAction *action = qobject_cast<QAction*>(sender());
    if (!action)
        return;
    const QUrl imageUrl = action->data().toUrl();
    if (imageUrl.isEmpty())
        return;
    OpenSearchEngine *engine =
        ToolbarSearch::openSearchManager()->imageSearchEngine();
    if (!engine)
        return;
    const QUrl searchUrl = engine->imageSearchUrl(imageUrl.toString());
    if (!searchUrl.isEmpty() && searchUrl.isValid())
        emit search(searchUrl, TabWidget::NewSelectedTab);
}

void WebView::setProgress(int progress)
{
    m_progress = progress;
}

int WebView::levelForZoom(int zoom)
{
    int i;

    i = m_zoomLevels.indexOf(zoom);
    if (i >= 0)
        return i;

    for (i = 0 ; i < m_zoomLevels.count(); ++i)
        if (zoom <= m_zoomLevels[i])
            break;

    if (i == m_zoomLevels.count())
        return i - 1;
    if (i == 0)
        return i;

    if (zoom - m_zoomLevels[i-1] > m_zoomLevels[i] - zoom)
        return i;
    else
        return i-1;
}

void WebView::applyZoom()
{
    setZoomFactor(qreal(m_currentZoom) / 100.0);
    emit zoomChanged(m_currentZoom);
}

void WebView::zoomIn()
{
    int i = levelForZoom(m_currentZoom);

    if (i < m_zoomLevels.count() - 1)
        m_currentZoom = m_zoomLevels[i + 1];
    applyZoom();
}

void WebView::zoomOut()
{
    int i = levelForZoom(m_currentZoom);

    if (i > 0)
        m_currentZoom = m_zoomLevels[i - 1];
    applyZoom();
}

void WebView::resetZoom()
{
    m_currentZoom = 100;
    applyZoom();
}

void WebView::loadFinished()
{
    if (100 != m_progress) {
        qWarning() << "Received finished signal while progress is still:" << progress()
                   << "Url:" << url();
    }
    m_progress = 0;
}

bool WebView::isUrlAllowedOnUntrustedInput(const QUrl &url)
{
    return url.scheme() != QLatin1String("javascript");
}

void WebView::loadUrl(const QUrl &url, const QString &title)
{
    if (url.scheme() == QLatin1String("javascript")) {
        QString scriptSource = QUrl::fromPercentEncoding(url.toString(QUrl::RemoveScheme).toUtf8());
        m_page->runJavaScript(scriptSource);
        return;
    }
    m_initialUrl = url;
    if (!title.isEmpty())
        emit titleChanged(tr("Loading..."));
    else
        emit titleChanged(title);
    load(url);
}

QString WebView::lastStatusBarText() const
{
    return m_statusBarText;
}

QUrl WebView::url() const
{
    QUrl url = QWebEngineView::url();
    if (!url.isEmpty())
        return url;

    return m_initialUrl;
}

void WebView::mousePressEvent(QMouseEvent *event)
{
    // Stash the current modifiers so WebPage::acceptNavigationRequest
    // can map the click through modifyWithUserBehavior (ctrl/middle
    // click -> new tab etc.).
    if (BrowserApplication *application = BrowserApplication::instance()) {
        application->setEventMouseButtons(event->buttons());
        application->setEventKeyboardModifiers(event->modifiers());
    }
    switch (event->button()) {
    case Qt::XButton1:
        triggerPageAction(QWebEnginePage::Back);
        break;
    case Qt::XButton2:
        triggerPageAction(QWebEnginePage::Forward);
        break;
    default:
        QWebEngineView::mousePressEvent(event);
        break;
    }
}

void WebView::dragEnterEvent(QDragEnterEvent *event)
{
    event->acceptProposedAction();
}

void WebView::dragMoveEvent(QDragMoveEvent *event)
{
    event->ignore();
    if (event->source() != this) {
        if (!event->mimeData()->urls().isEmpty()) {
            event->acceptProposedAction();
        } else {
            QUrl url(event->mimeData()->text());
            if (url.isValid())
                event->acceptProposedAction();
        }
    }
    if (!event->isAccepted()) {
        QWebEngineView::dragMoveEvent(event);
    }
}

void WebView::dropEvent(QDropEvent *event)
{
    QWebEngineView::dropEvent(event);
    if (!event->isAccepted()
        && event->source() != this
        && event->possibleActions() & Qt::CopyAction) {

        QUrl url;
        if (!event->mimeData()->urls().isEmpty())
            url = event->mimeData()->urls().first();
        if (!url.isValid())
            url = event->mimeData()->text();
        // A dropped javascript: url would run its script in this page's
        // origin — a drop must never become script injection (SEC02).
        // javascript: is honored only when typed or invoked as a
        // bookmarklet.
        if (url.isValid() && isUrlAllowedOnUntrustedInput(url)) {
            loadUrl(url);
            event->acceptProposedAction();
        }
    }
}

void WebView::mouseReleaseEvent(QMouseEvent *event)
{
    const bool isAccepted = event->isAccepted();
    m_page->event(event);
    if (!event->isAccepted()
        && event->button() == Qt::MiddleButton) {
        // X11 style: load the PRIMARY selection as a URL.  javascript:
        // is excluded (SEC02) — clipboard contents come from anywhere
        // and must not run as script in the current page.
        QUrl url(QApplication::clipboard()->text(QClipboard::Selection));
        if (!url.isEmpty() && url.isValid() && !url.scheme().isEmpty()
            && isUrlAllowedOnUntrustedInput(url)) {
            loadUrl(url);
        }
    }
    event->setAccepted(isAccepted);
}

void WebView::setStatusBarText(QString string)
{
    m_statusBarText = std::move(string);
    emit statusBarMessage(m_statusBarText);
}

bool WebView::isJavaScriptBlocked() const
{
    return m_page->isJavaScriptBlocked();
}

// JSCTL: show/hide the "Scripts blocked on <host>" bar for the current
// page.  It overlays the top edge of the view; reload follows either
// allow choice so the lifted policy applies cleanly.
void WebView::updateScriptBlockBar(bool blocked)
{
    if (!m_scriptBlockBar)
        return;
    if (blocked) {
        // url() still names the previous page while the navigation is
        // pending — the page records the blocked host for us.
        m_scriptBlockBar->setHost(m_page->javaScriptBlockedHost());
        m_scriptBlockBar->setGeometry(0, 0, width(),
                m_scriptBlockBar->sizeHint().height());
        m_scriptBlockBar->show();
        // The render widget is a sibling created after the bar — keep
        // the bar on top.
        m_scriptBlockBar->raise();
    } else {
        m_scriptBlockBar->hide();
    }
}

void WebView::allowScriptsOnThisSite(bool persistent)
{
    const QString host = m_page->javaScriptBlockedHost();
    if (host.isEmpty())
        return;
    // Off-the-record pages never write the persistent store — their
    // "always" is this session only (same discipline as SEC05).
    const bool persist = persistent
        && !m_page->profile()->isOffTheRecord();
    ScriptControlManager::instance()->setRuleForHost(
        host, ScriptControlManager::Allow, persist);
    reload();
}

void WebView::resizeEvent(QResizeEvent *event)
{
    QWebEngineView::resizeEvent(event);
    if (m_scriptBlockBar && m_scriptBlockBar->isVisible()) {
        m_scriptBlockBar->setGeometry(0, 0, width(),
                m_scriptBlockBar->sizeHint().height());
        m_scriptBlockBar->raise();
    }
}
