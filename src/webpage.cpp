/*
 * Copyright 2009 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2009 Jakub Wieczorek <faw217@gmail.com>
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

#include "webpage.h"

#include "autofillmanager.h"
#include "browserapplication.h"
#include "browserprofile.h"
#include "fileaccesshandler.h"
#include "historymanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webview.h"

#include <qapplication.h>
#include <qbuffer.h>
#include <qdesktopservices.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qmessagebox.h>
#include <qpixmap.h>
#include <qsettings.h>
#include <qstyle.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwebchannel.h>
#include <qwebengineloadinginfo.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>

QString WebPage::s_userAgent;

JavaScriptExternalObject::JavaScriptExternalObject(QObject *parent)
    : QObject(parent)
{
}

void JavaScriptExternalObject::AddSearchProvider(const QString &url)
{
    ToolbarSearch::openSearchManager()->addEngine(QUrl(url));
}

JavaScriptAroraObject::JavaScriptAroraObject(QObject *parent)
    : QObject(parent)
{
    static const char *translations[] = {
        QT_TR_NOOP("Welcome to Arora!"),
        QT_TR_NOOP("Arora Start"),
        QT_TR_NOOP("Search!"),
        QT_TR_NOOP("Search results provided by"),
        QT_TR_NOOP("About Arora")
    };
    Q_UNUSED(translations);
}

QString JavaScriptAroraObject::translate(const QString &string)
{
    QString translatedString = tr(string.toUtf8().constData());

    // If the translation is the same as the original string
    // it could not be translated.  In that case
    // try to translate using the QApplication domain
    if (translatedString != string)
        return translatedString;
    else
        return qApp->tr(string.toUtf8().constData());
}

QObject *JavaScriptAroraObject::currentEngine() const
{
    return ToolbarSearch::openSearchManager()->currentEngine();
}

QString JavaScriptAroraObject::searchUrl(const QString &string) const
{
    OpenSearchEngine *engine = ToolbarSearch::openSearchManager()->currentEngine();
    return engine ? engine->searchUrl(string).toString() : QString();
}

WebPage::WebPage(QObject *parent)
    : QWebEnginePage(parent)
    , m_openTargetBlankLinksIn(TabWidget::NewWindow)
    , m_javaScriptExternalObject(new JavaScriptExternalObject(this))
    , m_javaScriptAroraObject(new JavaScriptAroraObject(this))
    , m_autoFillBridge(new AutoFillBridge(this))
    , m_webChannel(new QWebChannel(this))
{
    init();
}

WebPage::WebPage(QWebEngineProfile *profile, QObject *parent)
    : QWebEnginePage(profile, parent)
    , m_openTargetBlankLinksIn(TabWidget::NewWindow)
    , m_javaScriptExternalObject(new JavaScriptExternalObject(this))
    , m_javaScriptAroraObject(new JavaScriptAroraObject(this))
    , m_autoFillBridge(new AutoFillBridge(this))
    , m_webChannel(new QWebChannel(this))
{
    init();
}

void WebPage::init()
{
    // Qt WebEngine pages cannot be given a QNetworkAccessManager; web loads
    // go through Chromium's network stack and the profile's cookie store.
    //
    // The old per-frame addToJavaScriptWindowObject() binding is replaced by
    // a QWebChannel.  Objects are only visible to pages that explicitly load
    // qwebchannel.js, so registering them unconditionally is safe.
    // TODO(MIG16): make startpage.html pull in qrc:///qtwebchannel/qwebchannel.js.
    m_webChannel->registerObject(QLatin1String("external"), m_javaScriptExternalObject);
    m_webChannel->registerObject(QLatin1String("arora"), m_javaScriptAroraObject);
    // MIG10: form-submit reports from the injected autofill.js arrive
    // through this object; the manager enables/disables capture per
    // page (off-the-record pages never capture).
    m_webChannel->registerObject(QLatin1String("aroraAutofill"), m_autoFillBridge);
    setWebChannel(m_webChannel);

    // Chromium's built-in error pages are disabled so the Arora
    // notfound.html page can be injected from handleLoadingChanged().
    settings()->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);

    // Downloads are handled application-wide: DownloadManager hooks
    // QWebEngineProfile::downloadRequested for each profile it is
    // installed on (MIG05) — BrowserApplication installs it on both the
    // normal and the off-the-record private profile (MIG15).
    connect(this, &QWebEnginePage::loadingChanged,
            this, &WebPage::handleLoadingChanged);

    // MIG06: feed the app-side history store.  QtWebKit pushed visited
    // urls into QWebHistoryInterface itself; WebEngine keeps Chromium's
    // own internal history, so the application records visits from page
    // signals instead.  Pages on the off-the-record profile never reach
    // the manager — that is the private-browsing guarantee now.
    if (!profile()->isOffTheRecord()) {
        HistoryManager *history = HistoryManager::instance();
        connect(this, &QWebEnginePage::loadFinished, this,
                [this, history](bool ok) {
            if (ok)
                history->addHistoryEntry(url().toString());
        });
        connect(this, &QWebEnginePage::titleChanged, this,
                [this, history](const QString &title) {
            history->updateHistoryEntry(url(), title);
        });
        connect(this, &QWebEnginePage::iconChanged, this,
                [this, history](const QIcon &icon) {
            history->setIcon(url(), icon);
        });
    }
    // Apply the configured user agent to whichever profile this page is
    // on (private windows run on the off-the-record profile).
    if (!s_userAgent.isEmpty())
        profile()->setHttpUserAgent(s_userAgent);
    loadSettings();
}

void WebPage::linkedResources(const QString &relation,
        const std::function<void(const QList<WebPageLinkedResource> &)> &resultCallback)
{
    QFile file(QLatin1String(":fetchLinks.js"));
    if (!file.open(QFile::ReadOnly)) {
        resultCallback(QList<WebPageLinkedResource>());
        return;
    }
    QString script = QString::fromUtf8(file.readAll());

    runJavaScript(script, [relation, resultCallback](const QVariant &result) {
        QList<WebPageLinkedResource> resources;
        const QVariantList list = result.toList();
        for (const QVariant &variant : list) {
            QVariantMap map = variant.toMap();
            QString rel = map[QLatin1String("rel")].toString();
            QString type = map[QLatin1String("type")].toString();
            QString href = map[QLatin1String("href")].toString();
            QString title = map[QLatin1String("title")].toString();

            if (href.isEmpty() || type.isEmpty())
                continue;
            if (!relation.isEmpty() && rel != relation)
                continue;

            WebPageLinkedResource resource;
            resource.rel = rel;
            resource.type = type;
            // fetchLinks.js reports element.href which is already absolute
            resource.href = QUrl::fromEncoded(href.toUtf8());
            resource.title = title;

            resources.append(resource);
        }
        resultCallback(resources);
    });
}

void WebPage::linkedResources(const std::function<void(const QList<WebPageLinkedResource> &)> &resultCallback)
{
    linkedResources(QString(), resultCallback);
}

QString WebPage::userAgent()
{
    return s_userAgent;
}

void WebPage::setUserAgent(const QString &userAgent)
{
    if (userAgent == s_userAgent)
        return;

    QSettings settings;
    if (userAgent.isEmpty()) {
        settings.remove(QLatin1String("userAgent"));
    } else {
        settings.setValue(QLatin1String("userAgent"), userAgent);
    }

    s_userAgent = userAgent;

    // Apply to every profile the app browses on: the named browsing
    // profile and the off-the-record private profile when it exists.
    // (QWebEngineProfile::defaultProfile() is itself off-the-record in
    // Qt6 and Arora never browses on it — setting the UA there had no
    // effect.)  An empty override restores the vanilla UA, not Qt's
    // QtWebEngine-badged default.
    const QString effectiveAgent = userAgent.isEmpty()
        ? BrowserProfile::defaultHttpUserAgent()
        : userAgent;
    BrowserProfile::normalProfile()->setHttpUserAgent(effectiveAgent);
    if (QWebEngineProfile *otrProfile = BrowserProfile::privateProfileIfCreated())
        otrProfile->setHttpUserAgent(effectiveAgent);
}

bool WebPage::acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
{
    QString scheme = url.scheme();
    if (scheme == QLatin1String("mailto")
        || scheme == QLatin1String("ftp")) {
        // askDesktopToOpenUrl records the url so a scheme handler that
        // loops back into the browser is detected and dropped.
        if (BrowserApplication *application = BrowserApplication::instance())
            application->askDesktopToOpenUrl(url);
        else
            QDesktopServices::openUrl(url);
        return false;
    }

    // file:// is built into Chromium and cannot take a custom scheme
    // handler; directories are rerouted to arora-file:// so the
    // FileAccessHandler can render Arora's own directory listing.
    if (scheme == QLatin1String("file") && isMainFrame
        && QFileInfo(url.toLocalFile()).isDir()) {
        const QUrl dirUrl = FileAccessHandler::urlForLocalPath(url.toLocalFile());
        QTimer::singleShot(0, this, [this, dirUrl]() { load(dirUrl); });
        return false;
    }

    // Qt WebEngine asks the user about resubmitting POST data itself; the old
    // NavigationTypeFormResubmitted prompt has no equivalent here.

    bool accepted = QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);

    // Divert main-frame link clicks the user modifier-mapped to a new
    // tab/window (e.g. ctrl+click); WebView::mousePressEvent stashes the
    // modifiers modifyWithUserBehavior reads.
    if (accepted && isMainFrame
        && type == QWebEnginePage::NavigationTypeLinkClicked) {
        WebView *webView = qobject_cast<WebView*>(QWebEngineView::forPage(this));
        if (webView) {
            TabWidget::OpenUrlIn target =
                TabWidget::modifyWithUserBehavior(TabWidget::CurrentTab);
            if (target != TabWidget::CurrentTab) {
                if (TabWidget *tabs = webView->tabWidget())
                    tabs->loadUrl(url, target);
                return false;
            }
        }
    }
    if (accepted && isMainFrame) {
        m_requestedUrl = url;
        emit aboutToLoadUrl(url);
    }

    return accepted;
}

QWebEnginePage *WebPage::createWindow(QWebEnginePage::WebWindowType type)
{
    Q_UNUSED(type);
    WebView *sourceView = qobject_cast<WebView*>(QWebEngineView::forPage(this));
    if (sourceView && sourceView->tabWidget()) {
        if (WebView *webView = sourceView->tabWidget()->getView(
                    m_openTargetBlankLinksIn, sourceView))
            return webView->webPage();
    }
    // Detached page (no TabWidget above the view): open a standalone
    // window on the same profile so private browsing propagates.
    WebView *webView = new WebView(profile());
    webView->setAttribute(Qt::WA_DeleteOnClose);
    webView->show();
    return webView->webPage();
}

void WebPage::handleLoadingChanged(const QWebEngineLoadingInfo &loadingInfo)
{
    if (loadingInfo.status() != QWebEngineLoadingInfo::LoadFailedStatus)
        return;

    QUrl errorUrl = loadingInfo.url();
    if (errorUrl.isEmpty() || errorUrl != m_requestedUrl)
        return;

    showErrorPage(errorUrl, loadingInfo.errorString());
}

// The chromium guys have documented many examples of incompatibilities that
// different browsers have when they mime sniff.
// http://src.chromium.org/viewvc/chrome/trunk/src/net/base/mime_sniffer.cc
void WebPage::showErrorPage(const QUrl &errorUrl, const QString &errorString)
{
    // Generate translated not found error page with an image
    QFile notFoundErrorFile(QLatin1String(":/notfound.html"));
    if (!notFoundErrorFile.open(QIODevice::ReadOnly))
        return;
    QString title = tr("Error loading page: %1").arg(QString::fromUtf8(errorUrl.toEncoded()));
    QString html = QLatin1String(notFoundErrorFile.readAll());
    QWidget *view = QWebEngineView::forPage(this);
    QPixmap pixmap = qApp->style()->standardIcon(QStyle::SP_MessageBoxWarning, 0, view).pixmap(QSize(32, 32));
    QBuffer imageBuffer;
    imageBuffer.open(QBuffer::ReadWrite);
    if (pixmap.save(&imageBuffer, "PNG")) {
        html.replace(QLatin1String("IMAGE_BINARY_DATA_HERE"),
                     QLatin1String(imageBuffer.buffer().toBase64()));
    }
    html = html.arg(title,
                    errorString,
                    tr("When connecting to: %1.").arg(QString::fromUtf8(errorUrl.toEncoded())),
                    tr("Check the address for errors such as <b>ww</b>.arora-browser.org instead of <b>www</b>.arora-browser.org"),
                    tr("If the address is correct, try checking the network connection."),
                    tr("If your computer or network is protected by a firewall or proxy, make sure that the browser is permitted to access the network."));
    setHtml(html, errorUrl);
    // A failed load is normally never recorded (only loadFinished(true)
    // feeds the manager), but a page that loaded and then errored —
    // e.g. a navigation interrupted mid-way — may have gotten in.
    HistoryManager::instance()->removeHistoryEntry(errorUrl, this->title());
}

void WebPage::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    m_openTargetBlankLinksIn = (TabWidget::OpenUrlIn)settings.value(QLatin1String("openTargetBlankLinksIn"),
                                                                    TabWidget::NewSelectedTab).toInt();
    settings.endGroup();
    setUserAgent(settings.value(QLatin1String("userAgent")).toString());
}
