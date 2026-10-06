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
#include "schemeaccesshandler.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpermissionmanager.h"
#include "webview.h"

#include <qapplication.h>
#include <qbuffer.h>
#include <qcryptographichash.h>
#include <qdesktopservices.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qmessagebox.h>
#include <qmetaobject.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qsettings.h>
#include <qset.h>
#include <qsslcertificate.h>
#include <qstyle.h>
#include <qtimer.h>
#include <qurlquery.h>
#include <quuid.h>
#include <qvariant.h>
#include <qwebchannel.h>
#include <qwebenginehistory.h>
#include <qwebengineloadinginfo.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>

QString WebPage::s_userAgent;

JavaScriptExternalObject::JavaScriptExternalObject(QObject *parent)
    : QObject(parent)
{
}

// SEC08: while one search-engine consent prompt is open, further
// AddSearchProvider calls are dropped — a page must not stack modals.
static bool s_searchProviderPromptActive = false;

void JavaScriptExternalObject::AddSearchProvider(const QString &url)
{
    // The descriptor url arrives over the page's QWebChannel and is
    // untrusted input: fetching it blindly would let any web page
    // drive GETs through the application-side QNetworkAccessManager
    // (unlike Chromium's renderer-side fetches, those carry app-side
    // cookies).  Only web schemes may be fetched, and only after the
    // user consents; the downloaded engine still passes through
    // OpenSearchManager's install confirmation, which names what was
    // actually fetched.
    const QUrl descriptorUrl(
        QString::fromUtf8(url.toUtf8().left(2048)));
    const QString scheme = descriptorUrl.scheme();
    if (!descriptorUrl.isValid()
        || (scheme != QLatin1String("http") && scheme != QLatin1String("https")))
        return;
    if (s_searchProviderPromptActive)
        return;
    s_searchProviderPromptActive = true;

    QWebEnginePage *page = qobject_cast<QWebEnginePage*>(parent());
    const QString source = page
            ? QString::fromUtf8(page->url().toEncoded()) : QString();
    QPointer<QWidget> view = page ? QWebEngineView::forPage(page) : 0;

    // Queued: a modal exec() inside the channel dispatch stack is
    // asking for re-entrancy trouble — same pattern as the
    // external-url consent prompt in WebPage::confirmAndOpenExternalUrl.
    QMetaObject::invokeMethod(qApp, [view, descriptorUrl, source]() {
        QString shown = QString::fromUtf8(descriptorUrl.toEncoded());
        if (shown.size() > 256)
            shown = shown.left(256) + QLatin1String("…");
        QMessageBox box(QMessageBox::Question,
            WebPage::tr("Add Search Engine"),
            WebPage::tr("The page at %1 wants to add a search engine "
                        "described by:\n\n%2\n\nDownload and inspect "
                        "it?").arg(source, shown),
            QMessageBox::Yes | QMessageBox::No, view);
        // URLs are page-controlled; render the prompt literally.
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::No);
        const QMessageBox::StandardButton choice =
            static_cast<QMessageBox::StandardButton>(box.exec());
        s_searchProviderPromptActive = false;
        if (choice == QMessageBox::Yes)
            ToolbarSearch::openSearchManager()->addEngine(descriptorUrl);
    }, Qt::QueuedConnection);
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

    // Live-update currentEngineName on internal pages when the user
    // switches search engines.
    connect(ToolbarSearch::openSearchManager(),
            &OpenSearchManager::currentEngineChanged,
            this, &JavaScriptAroraObject::currentEngineNameChanged);
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

QString JavaScriptAroraObject::currentEngineName() const
{
    OpenSearchEngine *engine = ToolbarSearch::openSearchManager()->currentEngine();
    return engine ? engine->name() : QString();
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
    , m_certErrorPending(false)
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
    , m_certErrorPending(false)
{
    init();
}

void WebPage::init()
{
    // Qt WebEngine pages cannot be given a QNetworkAccessManager; web loads
    // go through Chromium's network stack and the profile's cookie store.
    //
    // The old per-frame addToJavaScriptWindowObject() binding is replaced by
    // a QWebChannel.  qwebchannel.js is a public file and the transport is
    // injected into every page, so ANY web content can reach the registered
    // objects (SEC08) — everything exposed here is hardened accordingly:
    //
    //  * aroraAutofill.submitForm requires the per-load token that only
    //    the C++-injected autofill.js holds in a closure; forged calls
    //    are dropped (autofillmanager.cpp).
    //  * external.AddSearchProvider validates the descriptor url and
    //    asks for consent before any fetch happens.
    m_webChannel->registerObject(QLatin1String("external"), m_javaScriptExternalObject);
    m_webChannel->registerObject(QLatin1String("aroraAutofill"), m_autoFillBridge);
    setWebChannel(m_webChannel);

    // The "arora" object serves only internal qrc pages (the start
    // page).  It is registered when the main frame commits to a qrc
    // url — before the page's channel handshake — and removed again
    // when the frame navigates away, so web content never sees it.
    connect(this, &QWebEnginePage::urlChanged, this,
            [this](const QUrl &url) {
        if (url.scheme() == QLatin1String("qrc"))
            m_webChannel->registerObject(QLatin1String("arora"),
                                         m_javaScriptAroraObject);
        else
            m_webChannel->deregisterObject(m_javaScriptAroraObject);
    });

    // Chromium's built-in error pages are disabled so the Arora
    // notfound.html page can be injected from handleLoadingChanged().
    settings()->setAttribute(QWebEngineSettings::ErrorPageEnabled, false);

    // Downloads are handled application-wide: DownloadManager hooks
    // QWebEngineProfile::downloadRequested for each profile it is
    // installed on (MIG05) — BrowserApplication installs it on both the
    // normal and the off-the-record private profile (MIG15).
    connect(this, &QWebEnginePage::loadingChanged,
            this, &WebPage::handleLoadingChanged);

    // SEC05: route every feature-permission request (notifications,
    // geolocation, capture devices, clipboard, ...) through the
    // default-deny broker.  Without this connect the request is left
    // pending and the site simply hangs; Chromium never grants
    // unhandled requests, but nothing resolved them either.
    connect(this, &QWebEnginePage::permissionRequested,
            this, [this](const QWebEnginePermission &permission) {
        WebPermissionManager::instance()->handleRequest(
            QWebEngineView::forPage(this), permission,
            profile()->isOffTheRecord());
    });

    // SEC06: certificate failures surface here instead of as a generic
    // load error.  The handler rejects the failed request and shows an
    // interstitial page; its action links resolve the decision —
    // "proceed" whitelists the (host, certificate) pair for this
    // session and profile, never a trust store.
    connect(this, &QWebEnginePage::certificateError,
            this, [this](const QWebEngineCertificateError &error) {
        handleCertificateError(error);
    });

    // MIG06: feed the app-side history store.  QtWebKit pushed visited
    // urls into QWebHistoryInterface itself; WebEngine keeps Chromium's
    // own internal history, so the application records visits from page
    // signals instead.  Pages on the off-the-record profile never reach
    // the manager — that is the private-browsing guarantee now.
    if (!profile()->isOffTheRecord()) {
        HistoryManager *history = HistoryManager::instance();
        connect(this, &QWebEnginePage::loadFinished, this,
                [this, history](bool ok) {
            // The certificate-error interstitial is chrome, not a
            // visited page — keep it out of history.
            if (ok && url().scheme() != QLatin1String("arora-cert-error"))
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

// Schemes Chromium renders itself plus the ones this application
// serves through registered QWebEngineUrlSchemeHandlers (main.cpp).
// A navigation to anything else — mailto:, tel:, magnet:, ftp: and
// friends — can only be serviced by handing the url to the desktop,
// which launches an external program.  That is a shell-out and needs
// explicit consent; a web page must never trigger it silently (SEC02).
static bool isBrowserHandledScheme(const QString &scheme)
{
    static const QSet<QString> schemes = {
        QStringLiteral("about"),
        QStringLiteral("abp"),
        QStringLiteral("arora-file"),
        QStringLiteral("arora-resource"),
        QStringLiteral("blob"),
        QStringLiteral("chrome"),
        QStringLiteral("chrome-extension"),
        QStringLiteral("data"),
        QStringLiteral("devtools"),
        QStringLiteral("file"),
        QStringLiteral("filesystem"),
        QStringLiteral("http"),
        QStringLiteral("https"),
        QStringLiteral("javascript"),
        QStringLiteral("qrc"),
        QStringLiteral("view-source"),
    };
    return schemes.contains(scheme.toLower());
}

// While one consent dialog is open, further external navigations are
// denied outright — a redirect loop must not stack modal prompts.
static bool s_externalPromptActive = false;

void WebPage::confirmAndOpenExternalUrl(const QUrl &url)
{
    if (s_externalPromptActive)
        return;
    s_externalPromptActive = true;
    // Queued: a modal exec() inside acceptNavigationRequest would
    // re-enter Chromium's navigation machinery while it waits for an
    // answer.
    const QString source = this->url().toString();
    QPointer<QWidget> parent = QWebEngineView::forPage(this);
    QMetaObject::invokeMethod(qApp, [parent, url, source]() {
        // The percent-encoded form keeps control characters and
        // embedded newlines from spoofing the dialog text.
        QString shown = QString::fromUtf8(url.toEncoded());
        if (shown.size() > 256)
            shown = shown.left(256) + QLatin1String("…");
        QMessageBox box(QMessageBox::Question,
            WebPage::tr("Open External Application"),
            WebPage::tr("The page at %1 wants to open an external "
                        "application to handle this link:\n\n%2\n\n"
                        "Allow it?").arg(source, shown),
            QMessageBox::Open | QMessageBox::Cancel, parent);
        // URLs are page-controlled; render the prompt literally.
        box.setTextFormat(Qt::PlainText);
        box.setDefaultButton(QMessageBox::Cancel);
        const QMessageBox::StandardButton choice =
            static_cast<QMessageBox::StandardButton>(box.exec());
        s_externalPromptActive = false;
        if (choice != QMessageBox::Open)
            return;
        // askDesktopToOpenUrl records the url so a scheme handler that
        // loops back into the browser is detected and dropped.
        if (BrowserApplication *application = BrowserApplication::instance())
            application->askDesktopToOpenUrl(url);
        else
            QDesktopServices::openUrl(url);
    }, Qt::QueuedConnection);
}

bool WebPage::acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
{
    const QString scheme = url.scheme();

    // SEC06: the certificate-error interstitial and its action links
    // live on this private scheme — a link click surfaces as an
    // ordinary navigation that resolves the pending decision.  Only
    // the rendered page knows the nonce, so web content cannot forge a
    // "proceed" for a certificate it did not trigger; forged or stale
    // links are dropped silently.
    if (scheme == QLatin1String("arora-cert-error")) {
        if (!isMainFrame)
            return false;
        const QString nonce = QUrlQuery(url)
                .queryItemValue(QLatin1String("n"));
        if (url.path() == QLatin1String("interstitial"))
            return SchemeAccessHandler::hasCertErrorPage(nonce);
        if (m_certErrorPending && nonce == m_certErrorNonce)
            resolveCertificateErrorLink(url);
        return false;
    }

    if (!scheme.isEmpty() && !isBrowserHandledScheme(scheme)) {
        // Subframe requests are dropped without prompting — an iframe
        // must not raise dialogs on the user's behalf.
        if (isMainFrame)
            confirmAndOpenExternalUrl(url);
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
        // A real navigation supersedes any pending cert decision — the
        // deferred request is dead by the time the new load commits.
        m_certErrorPending = false;
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

    // Certificate failures are presented by the interstitial page
    // (handleCertificateError), not by the generic notfound page — and
    // a deferred error that the user rejected via "back to safety"
    // must not stomp the page the user is navigating to.
    if (loadingInfo.errorDomain() == QWebEngineLoadingInfo::CertificateErrorDomain)
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

// SEC06: session-scoped whitelist of (host, certificate) pairs the
// user chose to proceed past, keyed per profile.  This backs the
// interstitial's "proceed" path (the re-issued load re-fires
// certificateError and is answered from here) and additionally lets a
// second WebPage on the same profile go straight through — matching
// the in-memory policy Chromium records on acceptCertificate().
// Nothing is ever persisted; the whitelist dies with the process.
static QSet<QString> &certErrorWhitelistFor(QWebEngineProfile *profile)
{
    static QHash<QWebEngineProfile *, QSet<QString> > whitelist;
    if (!whitelist.contains(profile)) {
        whitelist.insert(profile, QSet<QString>());
        QObject::connect(profile, &QObject::destroyed, profile, [profile]() {
            whitelist.remove(profile);
        });
    }
    return whitelist[profile];
}

static QString certErrorKey(const QWebEngineCertificateError &error)
{
    // Host plus the SHA-256 fingerprint of the leaf certificate — a
    // changed certificate prompts again.
    QString fingerprint;
    const QList<QSslCertificate> chain = error.certificateChain();
    if (!chain.isEmpty())
        fingerprint = QString::fromLatin1(
                chain.first().digest(QCryptographicHash::Sha256).toHex());
    return error.url().host() + QLatin1Char('|') + fingerprint;
}

// SEC06: certificate failures arrive here instead of becoming a
// generic load failure.  The request is rejected outright — a deferred
// QWebEngineCertificateError does not survive the interstitial's own
// navigation commit, so there is nothing to resolve later — and the
// interstitial's "proceed" link re-issues the load, which re-fires
// this signal and is answered from the session whitelist.  Errors
// Chromium refuses to override (isOverridable() == false, e.g.
// HSTS-pinned hosts) render without a proceed link.
void WebPage::handleCertificateError(QWebEngineCertificateError error)
{
    // Subframe/subresource certificate failures cannot show an
    // interstitial page — deny them outright, like Chromium does when
    // the signal is left unhandled.  A second main-frame error while a
    // decision is pending is denied too.
    if (!error.isMainFrame() || m_certErrorPending) {
        error.rejectCertificate();
        return;
    }

    const QString key = certErrorKey(error);
    if (certErrorWhitelistFor(profile()).contains(key)) {
        error.acceptCertificate();
        return;
    }

    m_certErrorPending = true;
    m_pendingCertError = error;
    // The nonce is embedded in the page's action links and published
    // alongside the markup — only the rendered interstitial knows it.
    m_certErrorNonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
    SchemeAccessHandler::publishCertErrorPage(m_certErrorNonce,
            certificateErrorHtml(error));
    error.rejectCertificate();

    // The interstitial is a real navigation served by the scheme
    // handler — not setHtml(): a data: document committed over a
    // failed navigation is sandboxed and its custom-scheme links never
    // reach acceptNavigationRequest (about:blank#blocked).
    SchemeAccessHandler::installCertErrorHandler(profile());
    load(QUrl(QLatin1String("arora-cert-error:interstitial?n=")
              + m_certErrorNonce));
    emit certificateErrorInterstitial(error.url());
}

QString WebPage::certificateErrorHtml(const QWebEngineCertificateError &error)
{
    QFile certErrorFile(QLatin1String(":/certerror.html"));
    if (!certErrorFile.open(QIODevice::ReadOnly))
        return QString();
    const QUrl errorUrl = error.url();
    QString title = tr("Certificate error: %1")
            .arg(QString::fromUtf8(errorUrl.toEncoded()));
    QString html = QLatin1String(certErrorFile.readAll());
    QWidget *view = QWebEngineView::forPage(this);
    QPixmap pixmap = qApp->style()->standardIcon(QStyle::SP_MessageBoxCritical, 0, view).pixmap(QSize(32, 32));
    QBuffer imageBuffer;
    imageBuffer.open(QBuffer::ReadWrite);
    if (pixmap.save(&imageBuffer, "PNG")) {
        html.replace(QLatin1String("IMAGE_BINARY_DATA_HERE"),
                     QLatin1String(imageBuffer.buffer().toBase64()));
    }

    // The error type's symbolic name ("CertificateAuthorityInvalid" …)
    // is more useful than the raw net error code.
    const QMetaEnum typeEnum = QMetaEnum::fromType<QWebEngineCertificateError::Type>();
    const char *typeName = typeEnum.valueToKey(static_cast<int>(error.type()));
    QString errorInfo = tr("Error: %1 (%2)")
            .arg(error.description().toHtmlEscaped(),
                 QString::fromLatin1(typeName ? typeName : "unknown").toHtmlEscaped());

    // Certificate chain detail — subject, issuer, validity window and
    // fingerprint so the user can judge what they are connecting to.
    // Everything interpolated here is attacker-influenced; escape it.
    QString certInfo;
    const QList<QSslCertificate> chain = error.certificateChain();
    if (!chain.isEmpty()) {
        const QSslCertificate cert = chain.first();
        certInfo += tr("<li>Subject: %1</li>").arg(cert.subjectDisplayName().toHtmlEscaped());
        certInfo += tr("<li>Issuer: %1</li>").arg(cert.issuerDisplayName().toHtmlEscaped());
        certInfo += tr("<li>Valid from %1 to %2</li>")
                .arg(cert.effectiveDate().toString(Qt::ISODate).toHtmlEscaped(),
                     cert.expiryDate().toString(Qt::ISODate).toHtmlEscaped());
        certInfo += tr("<li>Serial: %1</li>")
                .arg(QString::fromLatin1(cert.serialNumber()).toHtmlEscaped());
        certInfo += tr("<li>SHA-256 fingerprint: %1</li>")
                .arg(QString::fromLatin1(cert.digest(QCryptographicHash::Sha256).toHex()).toHtmlEscaped());
    }

    QString buttons = tr("<a id=\"back\" href=\"arora-cert-error:back?n=%1\">Back to safety</a>")
            .arg(m_certErrorNonce);
    if (error.isOverridable()) {
        buttons += tr("<a id=\"proceed\" href=\"arora-cert-error:proceed?n=%1\">Proceed anyway (unsafe)</a>")
                .arg(m_certErrorNonce);
    }

    html = html.arg(title,
                    tr("This site's certificate is not trusted"),
                    tr("Arora cannot verify the identity of %1. The certificate presented by the server "
                       "is invalid — continuing could expose the connection to an attacker.")
                        .arg(QString::fromUtf8(errorUrl.host().toUtf8()).toHtmlEscaped()),
                    QLatin1String("<li>") + errorInfo + QLatin1String("</li>"),
                    certInfo,
                    buttons);
    return html;
}

void WebPage::resolveCertificateErrorLink(const QUrl &command)
{
    const QWebEngineCertificateError error = m_pendingCertError;
    m_certErrorPending = false;
    QPointer<WebPage> page(this);
    if (command.path() == QLatin1String("proceed") && error.isOverridable()) {
        // Session-scoped override only: the (host, certificate) pair is
        // whitelisted for this profile and the original URL reloaded —
        // the re-fired certificateError is then accepted automatically.
        // Nothing is persisted to a trust store and a new session (or a
        // different profile) prompts again.
        const QString key = certErrorKey(error);
        const QUrl url = error.url();
        certErrorWhitelistFor(profile()).insert(key);
        // Queued: navigating from inside acceptNavigationRequest would
        // re-enter Chromium's navigation machinery (CHECK failure).
        QTimer::singleShot(0, this, [page, url]() {
            if (page)
                page->load(url);
        });
        return;
    }
    // "back to safety" (or anything unrecognised): leave the
    // interstitial — back in history when there is one, otherwise the
    // start page.  Queued for the same re-entrancy reason.
    QTimer::singleShot(0, this, [page]() {
        if (!page)
            return;
        if (page->history()->canGoBack())
            page->history()->back();
        else
            page->load(QUrl(QLatin1String("qrc:/startpage.html")));
    });
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
