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

#include "fileaccesshandler.h"
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
    Q_UNUSED(url);
    // TODO(MIG08): ToolbarSearch::openSearchManager()->addEngine(QUrl(url));
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
    // TODO(MIG08): return ToolbarSearch::openSearchManager()->currentEngine();
    return 0;
}

QString JavaScriptAroraObject::searchUrl(const QString &string) const
{
    Q_UNUSED(string);
    // TODO(MIG08): return ToolbarSearch::openSearchManager()->currentEngine()->searchUrl(string);
    return QString();
}

WebPage::WebPage(QObject *parent)
    : QWebEnginePage(parent)
    , m_openTargetBlankLinksIn(TabWidget::NewWindow)
    , m_javaScriptExternalObject(new JavaScriptExternalObject(this))
    , m_javaScriptAroraObject(new JavaScriptAroraObject(this))
    , m_webChannel(new QWebChannel(this))
{
    init();
}

WebPage::WebPage(QWebEngineProfile *profile, QObject *parent)
    : QWebEnginePage(profile, parent)
    , m_openTargetBlankLinksIn(TabWidget::NewWindow)
    , m_javaScriptExternalObject(new JavaScriptExternalObject(this))
    , m_javaScriptAroraObject(new JavaScriptAroraObject(this))
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
    setWebChannel(m_webChannel);

    // Chromium's built-in error pages are disabled so the Arora
    // notfound.html page can be injected from handleLoadingChanged().
    settings()->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);

    // Downloads are handled application-wide: DownloadManager hooks
    // QWebEngineProfile::downloadRequested for each profile it is
    // installed on (MIG05).  TODO(MIG15): install it on the
    // off-the-record profile too when private browsing is wired up.
    connect(this, &QWebEnginePage::loadingChanged,
            this, &WebPage::handleLoadingChanged);
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

    // Applied to the default profile here; WebPage::init() applies it to
    // whatever profile each new page is created on.
    QWebEngineProfile::defaultProfile()->setHttpUserAgent(userAgent);
}

bool WebPage::acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
{
    QString scheme = url.scheme();
    if (scheme == QLatin1String("mailto")
        || scheme == QLatin1String("ftp")) {
        // TODO(MIG15): BrowserApplication::instance()->askDesktopToOpenUrl(url)
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
    //
    // TODO(MIG14): the TabWidget::modifyWithUserBehavior() routing that let
    // main-frame navigations be diverted into new tabs/windows needs
    // TabWidget, which is still unported.

    bool accepted = QWebEnginePage::acceptNavigationRequest(url, type, isMainFrame);
    if (accepted && isMainFrame) {
        m_requestedUrl = url;
        emit aboutToLoadUrl(url);
    }

    return accepted;
}

QWebEnginePage *WebPage::createWindow(QWebEnginePage::WebWindowType type)
{
    Q_UNUSED(type);
    // TODO(MIG14): return tabWidget()->getView(m_openTargetBlankLinksIn,
    // webView)->page() so new windows respect the open-in preference.
    // Interim: an independent top-level WebView.
    WebView *webView = new WebView;
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
    // TODO(MIG06): BrowserApplication::instance()->historyManager()
    //              ->removeHistoryEntry(errorUrl, title());
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
