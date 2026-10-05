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
#include "adblockpage.h"
#include "webpage.h"

#include <qapplication.h>
#include <qclipboard.h>
#include <qdebug.h>
#include <qevent.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <qsettings.h>
#include <qwebenginecontextmenurequest.h>
#include <qwebenginehttprequest.h>

WebView::WebView(QWidget *parent)
    : QWebEngineView(parent)
    , m_progress(0)
    , m_currentZoom(100)
    , m_page(new WebPage(this))
{
    init();
}

WebView::WebView(QWebEngineProfile *profile, QWidget *parent)
    : QWebEngineView(parent)
    , m_progress(0)
    , m_currentZoom(100)
    , m_page(new WebPage(profile, this))
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
    // TODO(MIG15): reconnect BrowserApplication::zoomTextOnlyChanged ->
    // applyZoom; Qt WebEngine has no text-only zoom mode.
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
    // inherits() instead of qobject_cast: TabWidget's meta object lives in
    // tabwidget.cpp which is not linked until MIG14 lands.
    QObject *widget = this->parent();
    while (widget) {
        if (widget->inherits("TabWidget"))
            return static_cast<TabWidget*>(widget);
        widget = widget->parent();
    }
    return 0;
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
    }

    if (!request->selectedText().isEmpty()) {
        if (menu->isEmpty()) {
            menu->addAction(pageAction(QWebEnginePage::Copy));
        } else {
            menu->addSeparator();
        }
        // TODO(MIG08): "Search with..." submenu from
        // ToolbarSearch::openSearchManager()->allEnginesNames()
        // feeding searchRequested(QAction *).
    }

    if (request->isContentEditable()) {
        // TODO(MIG08): "Add to the toolbar search" — needs the input element's
        // form data via runJavaScript (was synchronous QWebElement access).
    }

    if (menu->isEmpty()) {
        delete menu;
        menu = createStandardContextMenu();
    } else {
        menu->addSeparator();
        menu->addAction(pageAction(QWebEnginePage::InspectElement));
    }

    if (!menu->isEmpty()) {
        // TODO(MIG14): re-add "Show Menu Bar" when the menubar is hidden
        // (BrowserMainWindow::parentWindow(tabWidget())->showMenuBarAction()).

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
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        // TODO(MIG14): load into tabWidget()->getView(TabWidget::NewNotSelectedTab, this)
        // Interim: an independent top-level WebView.
        WebView *newView = new WebView;
        newView->setAttribute(Qt::WA_DeleteOnClose);
        newView->show();
        QWebEngineHttpRequest request(action->data().toUrl());
        request.setHeader("Referer", url().toEncoded());
        newView->load(request);
    }
}

void WebView::openActionUrlInNewWindow()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        // TODO(MIG14): load into tabWidget()->getView(TabWidget::NewWindow, this)
        WebView *newView = new WebView;
        newView->setAttribute(Qt::WA_DeleteOnClose);
        newView->show();
        QWebEngineHttpRequest request(action->data().toUrl());
        request.setHeader("Referer", url().toEncoded());
        newView->load(request);
    }
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
    // TODO(MIG07): AddBookmarkDialog dialog; dialog.setUrl(...); dialog.exec();
}

void WebView::searchRequested(QAction *action)
{
    Q_UNUSED(action);
    // TODO(MIG08): look the engine up in ToolbarSearch::openSearchManager()
    // and emit search(engine->searchUrl(selectedText), NewSelectedTab).
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
    AdBlockManager::instance()->page()->applyRulesToPage(page());
    // TODO(MIG10): BrowserApplication::instance()->autoFillManager()->fill(page());
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
    // TODO(MIG15): BrowserApplication's eventMouseButtons/KeyboardModifiers
    // tracking for open-in-tab modifier behavior.
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
        if (url.isValid()) {
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
        // X11 style: load the PRIMARY selection as a URL
        QUrl url(QApplication::clipboard()->text(QClipboard::Selection));
        if (!url.isEmpty() && url.isValid() && !url.scheme().isEmpty()) {
            loadUrl(url);
        }
    }
    event->setAccepted(isAccepted);
}

void WebView::setStatusBarText(const QString &string)
{
    m_statusBarText = string;
}
