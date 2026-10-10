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
#include "pictureinpicture.h"
#include "qrcodedialog.h"
#include "readermode.h"
#include "safetext.h"
#include "scriptblockinfobar.h"
#include "scriptcontrolmanager.h"
#include "toolbarsearch.h"
#include "tormanager.h"
#include "urlcleaner.h"
#include "webenginebackend.h"
#include "webpage.h"

#include <qapplication.h>
#include <qclipboard.h>
#include <qdebug.h>
#include <qfile.h>
#include <qimage.h>
#include <qmenubar.h>
#include <qevent.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <qpointer.h>
#include <qsettings.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwebenginecontextmenurequest.h>
#include <qwebenginehttprequest.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

WebView::WebView(QWidget *parent)
    : QWebEngineView(parent)
    , m_progress(0)
    , m_currentZoom(100)
    , m_page(new WebPage(this))
    , m_enginePage(nullptr)
    , m_scriptBlockBar(nullptr)
{
    init();
}

WebView::WebView(QWebEngineProfile *profile, QWidget *parent)
    : QWebEngineView(parent)
    , m_progress(0)
    , m_currentZoom(100)
    , m_page(new WebPage(profile, this))
    , m_enginePage(nullptr)
    , m_scriptBlockBar(nullptr)
{
    init();
}

WebView::WebView(Engine::Profile *profile, QWidget *parent)
    : WebView(WebEngineProfileAdapter::of(profile)
                  ? WebEngineProfileAdapter::of(profile)->webEngineProfile()
                  : nullptr,
              parent)
{
}

Engine::Page *WebView::enginePage() const
{
    // The canonical adapter for the page — every holder of the same
    // engine page resolves to this object, so Engine::Page pointers
    // are identity-comparable (forPage is parented to m_page and
    // shares its clock).
    if (!m_enginePage)
        m_enginePage = WebEnginePageAdapter::forPage(m_page);
    return m_enginePage;
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

    // READ01: reader mode — a Shadow-DOM article overlay driven by the
    // bundled Readability.js; extraction failures surface as status
    // bar text.
    m_readerMode = new ReaderMode(this);
    connect(m_readerMode, &ReaderMode::message,
            this, [this](const QString &message) {
        emit statusBarMessage(message);
    });

    // PIP01: Picture-in-Picture — pops the page's video into an
    // app-owned floating window (QtWebEngine ships no PiP delegate).
    m_pip = new PictureInPicture(this);
    connect(m_pip, &PictureInPicture::message,
            this, [this](const QString &message) {
        emit statusBarMessage(message);
    });
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

void WebView::toggleReaderMode()
{
    if (m_readerMode)
        m_readerMode->toggle();
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

// Translates the engine's context-menu payload into the interface's
// neutral shape — the menu logic below consumes only Engine::
// ContextMenuInfo so the request type never crosses the boundary.
static Engine::ContextMenuInfo contextMenuInfo(
        const QWebEngineContextMenuRequest *request)
{
    Engine::ContextMenuInfo info;
    info.position = request->position();
    info.linkUrl = request->linkUrl();
    info.linkText = request->linkText();
    info.mediaUrl = request->mediaUrl();
    info.selectedText = request->selectedText();
    // QWebEngineContextMenuRequest carries no pageUrl — the struct's
    // field stays empty and the menu reads the page url from the view.
    info.isContentEditable = request->isContentEditable();
    switch (request->mediaType()) {
    case QWebEngineContextMenuRequest::MediaTypeImage:
        info.hasImage = true;
        break;
    case QWebEngineContextMenuRequest::MediaTypeVideo:
        info.hasMedia = true;
        info.hasVideo = true;
        break;
    case QWebEngineContextMenuRequest::MediaTypeAudio:
        info.hasMedia = true;
        break;
    case QWebEngineContextMenuRequest::MediaTypeCanvas:
        info.isCanvas = true;
        break;
    default:
        break;
    }
    return info;
}

void WebView::contextMenuEvent(QContextMenuEvent *event)
{
    QWebEngineContextMenuRequest *request = lastContextMenuRequest();
    if (!request) {
        QWebEngineView::contextMenuEvent(event);
        return;
    }
    const Engine::ContextMenuInfo info = contextMenuInfo(request);

    QMenu *menu = new QMenu(this);

    if (!info.linkUrl.isEmpty()) {
        // CONT07: every entry hands an attacker-influenced page url to
        // a new browsing context — the slots gate the scheme through
        // isUrlAllowedFromPageLink, and entries that can never succeed
        // are disabled at build time rather than refused on trigger.
        const bool allowed = isUrlAllowedFromPageLink(info.linkUrl);
        QAction *newTabAction = menu->addAction(tr("Open in New &Tab"), this, &WebView::openLinkInNewTab);
        newTabAction->setData(info.linkUrl);
        newTabAction->setEnabled(allowed);
        QAction *newWindowAction = menu->addAction(tr("Open in New &Window"), this, &WebView::openLinkInNewWindow);
        newWindowAction->setData(info.linkUrl);
        newWindowAction->setEnabled(allowed);
        // The private entries are hidden wherever they could not
        // honestly deliver a new isolated context: a fully private
        // window already routes every new tab/window through the OTR
        // profile, and inside a tor window they would have to land on
        // the CLEARNET OTR profile — a leak (isPrivate() covers tor
        // mode).  'Open in New Tor Window' always works — it spawns a
        // real --tor process.
        if (!BrowserApplication::isPrivate()) {
            QAction *privateTabAction = menu->addAction(tr("Open in New &Private Tab"), this, &WebView::openUrlInNewPrivateTab);
            privateTabAction->setData(info.linkUrl);
            privateTabAction->setEnabled(allowed);
            QAction *privateWindowAction = menu->addAction(tr("Open in New Pri&vate Window"), this, &WebView::openUrlInNewPrivateWindow);
            privateWindowAction->setData(info.linkUrl);
            privateWindowAction->setEnabled(allowed);
        }
        QAction *torWindowAction = menu->addAction(tr("Open in New T&or Window"), this, &WebView::openUrlInNewTorWindow);
        torWindowAction->setData(info.linkUrl);
        torWindowAction->setEnabled(allowed);
        if (TorManager::resolveBinary().isEmpty()) {
            torWindowAction->setEnabled(false);
            torWindowAction->setToolTip(
                tr("No tor binary found — install tor or run "
                   "BuildProcess/fetch-tor.sh"));
        }
        menu->addSeparator();
        QAction *saveLinkAction = menu->addAction(tr("Save Lin&k"), this, &WebView::downloadLinkToDisk);
        saveLinkAction->setData(info.linkUrl);
        QAction *bookmarkAction = menu->addAction(tr("&Bookmark This Link"), this, &WebView::bookmarkLink);
        bookmarkAction->setData(info.linkUrl);
        menu->addSeparator();
        if (!info.selectedText.isEmpty())
            menu->addAction(enginePage()->action(Engine::StandardAction::Copy));
        QAction *copyLinkAction = menu->addAction(tr("&Copy Link Location"), this, &WebView::copyLinkToClipboard);
        copyLinkAction->setData(info.linkUrl);
        // POL01: same copy but with tracking query parameters removed.
        QAction *cleanLinkAction = menu->addAction(tr("Copy &Clean Link"), this, &WebView::copyCleanLinkToClipboard);
        cleanLinkAction->setData(info.linkUrl);
    }

    const bool isImage = info.hasImage;
    const bool isCanvas = info.isCanvas;

    if (isImage && !info.mediaUrl.isEmpty()) {
        if (!menu->isEmpty())
            menu->addSeparator();
        // The same-profile opens keep the permissive base gate — a
        // blob: media url is only resolvable inside this profile.  The
        // private/tor entries hand the page-supplied url to another
        // context and take the strict page-link gate (CONT07).
        QAction *newTabAction = menu->addAction(tr("Open Image in New &Tab"), this, &WebView::openActionUrlInNewTab);
        newTabAction->setData(info.mediaUrl);
        QAction *newWindowAction = menu->addAction(tr("Open Image in New &Window"), this, &WebView::openActionUrlInNewWindow);
        newWindowAction->setData(info.mediaUrl);
        const bool allowed = isUrlAllowedFromPageLink(info.mediaUrl);
        if (!BrowserApplication::isPrivate()) {
            QAction *privateTabAction = menu->addAction(tr("Open Image in New &Private Tab"), this, &WebView::openUrlInNewPrivateTab);
            privateTabAction->setData(info.mediaUrl);
            privateTabAction->setEnabled(allowed);
            QAction *privateWindowAction = menu->addAction(tr("Open Image in New Pri&vate Window"), this, &WebView::openUrlInNewPrivateWindow);
            privateWindowAction->setData(info.mediaUrl);
            privateWindowAction->setEnabled(allowed);
        }
        QAction *torWindowAction = menu->addAction(tr("Open Image in New T&or Window"), this, &WebView::openUrlInNewTorWindow);
        torWindowAction->setData(info.mediaUrl);
        torWindowAction->setEnabled(allowed);
        if (TorManager::resolveBinary().isEmpty()) {
            torWindowAction->setEnabled(false);
            torWindowAction->setToolTip(
                tr("No tor binary found — install tor or run "
                   "BuildProcess/fetch-tor.sh"));
        }
        menu->addSeparator();
        QAction *saveImageAction = menu->addAction(tr("&Save Image"), this, &WebView::downloadImageToDisk);
        saveImageAction->setData(info.mediaUrl);
        menu->addAction(tr("&Copy Image"), this, &WebView::copyImageToClipboard);
        menu->addAction(tr("C&opy Image Location"), this, &WebView::copyImageLocationToClipboard)->setData(info.mediaUrl.toString());
        menu->addSeparator();
        menu->addAction(tr("Block Image"), this, &WebView::blockImage)->setData(info.mediaUrl.toString());

        // SRCH04: reverse-image search through the configured image
        // engine — only offered when an engine actually advertises an
        // image-search endpoint.
        if (OpenSearchEngine *imageEngine =
                ToolbarSearch::openSearchManager()->imageSearchEngine()) {
            QAction *imageSearchAction = menu->addAction(
                tr("Search Image with %1")
                    .arg(SafeText::menu(imageEngine->name())),
                this, &WebView::imageSearchRequested);
            imageSearchAction->setData(info.mediaUrl);
        }
    } else if (isImage || isCanvas) {
        // CTX01: a <canvas> reports no mediaUrl (its pixels only exist
        // in page memory) and an <img> occasionally arrives with an
        // empty one — resolve the content under the click point
        // in-page via contextimage.js.  Canvas grabs hand back a png
        // data: url, so open/save can share the url-based paths; a
        // canvas has no location to copy or block rule to write.
        if (!menu->isEmpty())
            menu->addSeparator();
        const QPoint position = info.position;
        menu->addAction(tr("Open Image in New &Window"), this,
                [this, position, isCanvas]() {
            grabContextImage(position, isCanvas,
                    [this](const QUrl &resolved) {
                openUrlInTarget(resolved, TabWidget::NewWindow);
            });
        });
        menu->addAction(tr("Open Image in New &Tab"), this,
                [this, position, isCanvas]() {
            grabContextImage(position, isCanvas,
                    [this](const QUrl &resolved) {
                openUrlInTarget(resolved, TabWidget::NewNotSelectedTab);
            });
        });
        menu->addSeparator();
        menu->addAction(tr("&Save Image"), this,
                [this, position, isCanvas]() {
            grabContextImage(position, isCanvas,
                    [this](const QUrl &resolved) {
                if (!resolved.isEmpty())
                    m_page->download(resolved);
            });
        });
        if (isCanvas) {
            // The pixels serialize to a png data url — decode it into
            // the clipboard image rather than the page's copy action
            // (Chromium offers no canvas copy equivalent).
            menu->addAction(tr("&Copy Image"), this,
                    [this, position]() {
                grabContextImage(position, true,
                        [this](const QUrl &resolved) {
                    const QString data = resolved.toString();
                    const int comma = data.indexOf(QLatin1Char(','));
                    const QImage image = QImage::fromData(
                        QByteArray::fromBase64(
                            data.mid(comma + 1).toLatin1()));
                    if (!image.isNull()) {
                        QApplication::clipboard()->setImage(image);
                    } else {
                        setStatusBarText(tr(
                            "Could not decode the canvas image."));
                    }
                });
            });
        } else {
            // The renderer still holds the pixels even when mediaUrl
            // came through empty — the stock copy action reaches them.
            menu->addAction(tr("&Copy Image"), this,
                    &WebView::copyImageToClipboard);
            menu->addAction(tr("C&opy Image Location"), this,
                    [this, position]() {
                grabContextImage(position, false,
                        [](const QUrl &resolved) {
                    QApplication::clipboard()->setText(
                        resolved.toString());
                });
            });
            menu->addSeparator();
            menu->addAction(tr("Block Image"), this,
                    [this, position]() {
                grabContextImage(position, false,
                        [](const QUrl &resolved) {
                    AdBlockManager::instance()->showDialog()
                        ->addCustomRule(resolved.toString());
                });
            });
        }
    }

    // PIP01: right-click on a <video> offers the pop-out.  The
    // element itself is resolved in-page (click point + media url).
    if (info.hasVideo) {
        if (!menu->isEmpty())
            menu->addSeparator();
        const QUrl mediaUrl = info.mediaUrl;
        const QPoint position = info.position;
        menu->addAction(tr("Picture-in-Picture"), this,
                [this, mediaUrl, position]() {
            m_pip->popOutContext(mediaUrl, position);
        });
        // CTX01: the poster frame is the video's image — the request
        // carries only the stream url, so the poster attribute is
        // resolved in-page (contextimage.js).
        menu->addAction(tr("Open &Poster in New Window"), this,
                [this, position]() {
            openContextPosterInTarget(position, TabWidget::NewWindow);
        });
        menu->addAction(tr("Open Poster in New &Tab"), this,
                [this, position]() {
            openContextPosterInTarget(position, TabWidget::NewNotSelectedTab);
        });
    }

    if (!info.selectedText.isEmpty()) {
        if (menu->isEmpty()) {
            menu->addAction(enginePage()->action(Engine::StandardAction::Copy));
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

    if (info.isContentEditable) {
        // TODO(MIG08): "Add to the toolbar search" — needs the input element's
        // form data via runJavaScript (was synchronous QWebElement access).
    }

    if (menu->isEmpty()) {
        delete menu;
        menu = createStandardContextMenu();
        // The stock menu's own "Inspect element" is the same no-op
        // bare page action — remove it; the hosted one is added below.
        menu->removeAction(enginePage()->action(Engine::StandardAction::InspectElement));
    }
    if (!menu->isEmpty())
        menu->addSeparator();
    // POL01: page-level share helpers.  On a link menu 'Copy Clean
    // Link' was already added against the link URL above; here it
    // cleans the page address itself.  The QR action is always for
    // the current page.
    if (info.linkUrl.isEmpty()) {
        menu->addAction(tr("Copy &Clean Link"), this, [this]() {
            QApplication::clipboard()->setText(
                UrlCleaner::cleanedUrl(url()).toString());
        });
    }
    menu->addAction(tr("Show &QR Code for This Page"), this,
            [this]() { QrCodeDialog::showForUrl(url(), this); });
    menu->addSeparator();

    // The bare page action is a no-op until a devToolsPage is bound —
    // route it through the shared inspector host (DVT01).  Triggering
    // InspectElement still uses the stored context-menu position, so
    // the right-clicked element is the one inspected.
    QAction *inspectPageAction = enginePage()->action(Engine::StandardAction::InspectElement);
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

// POL01: 'Copy Clean Link' — the clicked link minus its tracking
// query parameters (utm_*, fbclid, ...).
void WebView::copyCleanLinkToClipboard()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        QApplication::clipboard()->setText(
            UrlCleaner::cleanedUrl(action->data().toUrl()).toString());
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

// CONT07: the link-menu New Tab/New Window slots take the strict
// page-link gate — the same-profile image opens above stay permissive
// so a blob: media url keeps working inside its own profile.
void WebView::openLinkInNewTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openPageUrlInTarget(action->data().toUrl(),
                            TabWidget::NewNotSelectedTab);
}

void WebView::openLinkInNewWindow()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openPageUrlInTarget(action->data().toUrl(),
                            TabWidget::NewWindow);
}

void WebView::openUrlInNewPrivateTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openPageUrlInPrivateTab(action->data().toUrl());
}

void WebView::openUrlInNewPrivateWindow()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openPageUrlInPrivateWindow(action->data().toUrl());
}

void WebView::openUrlInNewTorWindow()
{
    if (QAction *action = qobject_cast<QAction*>(sender()))
        openPageUrlInTorWindow(action->data().toUrl());
}

void WebView::openPageUrlInTarget(const QUrl &linkUrl,
                                  TabWidget::OpenUrlIn target)
{
    if (!isUrlAllowedFromPageLink(linkUrl))
        return;
    openUrlInTarget(linkUrl, target);
}

void WebView::openPageUrlInPrivateTab(const QUrl &linkUrl)
{
    if (!isUrlAllowedFromPageLink(linkUrl))
        return;
    WebView *newView = nullptr;
    if (TabWidget *tabs = tabWidget())
        // Background placement, matching 'Open in New Tab'.  Reached
        // from a tor window anyway (the entry is hidden there), this
        // resolves to a tor-profile tab — never clearnet OTR.
        newView = tabs->makeNewPrivateTab(false);
    if (!newView) {
        // Detached view (no TabWidget above us): a standalone OTR
        // WebView.  privateWebEngineProfile() is always the CLEARNET
        // off-the-record profile — a tor process must hand out its
        // own profile here.
        newView = new WebView(BrowserApplication::isTorMode()
            ? BrowserApplication::webEngineProfile()
            : BrowserApplication::privateWebEngineProfile());
        newView->setAttribute(Qt::WA_DeleteOnClose);
        newView->show();
    }
    loadUrlInView(newView, linkUrl);
}

void WebView::openPageUrlInPrivateWindow(const QUrl &linkUrl)
{
    if (!isUrlAllowedFromPageLink(linkUrl))
        return;
    WebView *newView = nullptr;
    if (BrowserApplication *application = BrowserApplication::instance()) {
        // The same first-tab swap getView(NewWindow) performs for an
        // off-the-record source: a page's profile is fixed at
        // creation, so the fresh window's default tab is replaced by
        // an off-the-record one — which also means no container
        // binding is carried across.
        BrowserMainWindow *window = application->newMainWindow();
        if (WebView *privateTab =
                window->tabWidget()->makeNewPrivateTab(true)) {
            window->tabWidget()->closeTab(0);
            newView = privateTab;
        } else {
            // The OTR tab could not be bound — closing the window is
            // safer than letting the link land in its default tab.
            window->close();
        }
    }
    if (!newView) {
        newView = new WebView(BrowserApplication::isTorMode()
            ? BrowserApplication::webEngineProfile()
            : BrowserApplication::privateWebEngineProfile());
        newView->setAttribute(Qt::WA_DeleteOnClose);
        newView->show();
    }
    loadUrlInView(newView, linkUrl);
}

void WebView::openPageUrlInTorWindow(const QUrl &linkUrl)
{
    // The url travels to the new --tor process as its own argv
    // element — never through a shell — and is re-gated there as
    // untrusted input (BrowserApplication::torStartup).  Refusing the
    // dangerous schemes on this side too keeps a dead hand-off from
    // ever being offered.
    if (!isUrlAllowedFromPageLink(linkUrl))
        return;
    BrowserApplication::openTorWindow(linkUrl);
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
    loadUrlInView(newView, linkUrl);
}

// CTX01: keep the request-based load for http(s) — the Referer header
// preserves hotlink-protection behavior, but only when the target
// page lives on THIS page's profile: attaching it to a private or
// tor hand-off would leak the source page's url across a privacy
// boundary (CONT07).  Everything else (data: canvas dumps, blob:
// media, file:, view-source:) has no use for the header and
// QWebEngineHttpRequest only applies extra headers to http(s)
// anyway; the plain url load keeps javascript: refused through
// isUrlAllowedOnUntrustedInput (SEC02).
void WebView::loadUrlInView(WebView *newView, const QUrl &linkUrl)
{
    const QString scheme = linkUrl.scheme();
    if (scheme == QLatin1String("http")
        || scheme == QLatin1String("https")) {
        QWebEngineHttpRequest request(linkUrl);
        if (newView->webPage()->profile() == m_page->profile())
            request.setHeader("Referer", url().toEncoded());
        newView->load(request);
        return;
    }
    if (isUrlAllowedOnUntrustedInput(linkUrl))
        newView->loadUrl(linkUrl);
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
    enginePage()->action(Engine::StandardAction::CopyImageToClipboard)->trigger();
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
    enginePage()->setZoomFactor(qreal(m_currentZoom) / 100.0);
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

bool WebView::isUrlAllowedFromPageLink(const QUrl &url)
{
    const QString scheme = url.scheme();
    return isUrlAllowedOnUntrustedInput(url)
        && scheme != QLatin1String("data")
        && scheme != QLatin1String("blob");
}

void WebView::loadUrl(const QUrl &url, const QString &title)
{
    if (url.scheme() == QLatin1String("javascript")) {
        QString scriptSource = QUrl::fromPercentEncoding(url.toString(QUrl::RemoveScheme).toUtf8());
        enginePage()->runJavaScript(scriptSource);
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
        enginePage()->back();
        break;
    case Qt::XButton2:
        enginePage()->forward();
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

// CTX01: the contextimage.js resolver, loaded once and concatenated
// ahead of each call (the fetchLinks.js / pip.js convention).
static QString contextImageBundle()
{
    static const QString bundle = [] {
        QFile file(QLatin1String(":contextimage.js"));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "WebView: Unable to open :contextimage.js";
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }();
    return bundle;
}

void WebView::runContextImageScript(
        const QString &call,
        const std::function<void(const QVariant &)> &callback)
{
    const QString program = contextImageBundle()
        + QLatin1Char('\n') + call;
    // JSCTL/SECLVL: a video poster or canvas content is still
    // rendered while JavascriptEnabled is off — the click is an
    // explicit gesture on it, so the script goes through the lifted
    // path (Engine::Page owns the attribute juggling now).
    enginePage()->runJavaScriptLifted(program, callback);
}

// Resolves the image content under a context-menu click point:
// canvas=true serializes the <canvas> to a png data url, canvas=false
// resolves the <img>'s live source (currentSrc covers <picture> and
// blob: sources).  Failures surface as a status-bar message.
void WebView::grabContextImage(const QPoint &viewPos, bool canvas,
        const std::function<void(const QUrl &)> &callback)
{
    // The request's position is in view pixels; elementFromPoint wants
    // CSS pixels (same conversion PictureInPicture applies).
    qreal zoom = enginePage()->zoomFactor();
    if (zoom <= 0)
        zoom = 1.0;
    const QString call = QStringLiteral("__aroraCtxImage.%1(%2,%3);")
        .arg(QLatin1String(canvas ? "canvasData" : "imageUrl"))
        .arg(viewPos.x() / zoom)
        .arg(viewPos.y() / zoom);
    QPointer<WebView> self(this);
    runContextImageScript(call,
            [self, canvas, callback](const QVariant &result) {
        if (!self)
            return;
        const QVariantMap map = result.toMap();
        if (map.value(QLatin1String("ok")).toBool()) {
            callback(QUrl(map.value(QLatin1String(
                canvas ? "dataUrl" : "url")).toString()));
            return;
        }
        const QString reason =
            map.value(QLatin1String("reason")).toString();
        self->setStatusBarText(reason == QLatin1String("tainted")
            ? tr("This canvas cannot be read — it is tainted by "
                 "cross-origin content.")
            : tr("No image found at that position."));
    });
}

void WebView::openContextPosterInTarget(const QPoint &viewPos,
        TabWidget::OpenUrlIn target)
{
    qreal zoom = enginePage()->zoomFactor();
    if (zoom <= 0)
        zoom = 1.0;
    const QString call = QStringLiteral("__aroraCtxImage.posterUrl(%1,%2);")
        .arg(viewPos.x() / zoom)
        .arg(viewPos.y() / zoom);
    QPointer<WebView> self(this);
    runContextImageScript(call,
            [self, target](const QVariant &result) {
        if (!self)
            return;
        const QVariantMap map = result.toMap();
        if (map.value(QLatin1String("ok")).toBool()) {
            self->openUrlInTarget(
                QUrl(map.value(QLatin1String("url")).toString()),
                target);
            return;
        }
        self->setStatusBarText(tr("This video has no poster image."));
    });
}
