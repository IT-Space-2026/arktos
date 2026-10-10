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

#include "adblockmanager.h"
#include "adblockpage.h"
#include "autofillmanager.h"
#include "browserapplication.h"
#include "browserprofile.h"
#include "browsertheme.h"
#include "containermanager.h"
#include "fileaccesshandler.h"
#include "historymanager.h"
#include "opensearchengine.h"
#include "opensearchmanager.h"
#include "popupblocker.h"
#include "privacyrequestinterceptor.h"
#include "schemeaccesshandler.h"
#include "scriptcontrolmanager.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webenginebackend.h"
#include "webpermissionmanager.h"
#include "webview.h"

#include <qapplication.h>
#include <qbuffer.h>
#include <qcryptographichash.h>
#include <qdebug.h>
#include <qdesktopservices.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qicon.h>
#include <qinputdialog.h>
#include <qmessagebox.h>
#include <qmetaobject.h>
#include <qpixmap.h>
#include <qpointer.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qset.h>
#include <qsslcertificate.h>
#include <qstyle.h>
#include <qtimer.h>
#include <qurlquery.h>
#include <quuid.h>
#include <qvariant.h>
#include <qwebchannel.h>
#include <qwebengineclientcertificateselection.h>
#include <qwebenginehistory.h>
#include <qwebengineloadinginfo.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
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
    QPointer<QWidget> view = page ? QWebEngineView::forPage(page) : nullptr;

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
    // THEME01: same for darkChrome — a mid-session Theme flip fires
    // the notifier, which the qrc start page listens to.
    connect(BrowserTheme::themeNotifier(),
            &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
            this, &JavaScriptAroraObject::darkChromeChanged);
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

OpenSearchEngine *JavaScriptAroraObject::contextEngine() const
{
    const WebPage *page = qobject_cast<const WebPage*>(parent());
    const bool privateContext = page && page->profile()
        && page->profile()->isOffTheRecord();
    return ToolbarSearch::openSearchManager()
        ->engineForContext(privateContext);
}

QString JavaScriptAroraObject::currentEngineName() const
{
    OpenSearchEngine *engine = contextEngine();
    return engine ? engine->name() : QString();
}

bool JavaScriptAroraObject::darkChrome() const
{
    return qApp && BrowserTheme::isDarkPalette(qApp->palette());
}

QString JavaScriptAroraObject::searchUrl(const QString &string) const
{
    OpenSearchEngine *engine = contextEngine();
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
    , m_javaScriptBlocked(false)
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
    , m_javaScriptBlocked(false)
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
    // TOR02: tor-mode pages get no channel at all — the bridge objects
    // (autofill capture, search-provider install) are surface a tor
    // session does not need and must not offer.
    if (!BrowserApplication::isTorMode()) {
        m_webChannel->registerObject(QLatin1String("external"), m_javaScriptExternalObject);
        m_webChannel->registerObject(QLatin1String("aroraAutofill"), m_autoFillBridge);
        setWebChannel(m_webChannel);

        // CHAN01: arm the shared-channel bootstrap at DocumentCreation.
        // qt.webChannelTransport is one object per frame with a single
        // onmessage slot — every new QWebChannel replaces it and the
        // displaced client's in-flight responses then crash the new
        // owner's execCallbacks dispatch (the recurring console noise).
        // The bootstrap demultiplexes that slot across clients and
        // hands Arora's features one shared lazy client through
        // window.__aroraChannel, so our own code never runs two clients
        // and a page-side client can coexist instead of corrupting
        // either side.  The demux has to be installed before page
        // script can construct a channel client of its own — hence
        // DocumentCreation, once per page rather than per navigation.
        QString bootstrapSource;
        QFile channelClientFile(
            QLatin1String(":/qtwebchannel/qwebchannel.js"));
        if (channelClientFile.open(QIODevice::ReadOnly))
            bootstrapSource += QString::fromUtf8(
                channelClientFile.readAll());
        QFile bootstrapFile(QLatin1String(":arora-channel.js"));
        if (bootstrapFile.open(QIODevice::ReadOnly))
            bootstrapSource += QString::fromUtf8(bootstrapFile.readAll());
        if (!bootstrapSource.isEmpty()) {
            QWebEngineScript bootstrap;
            bootstrap.setName(QLatin1String("arora:channel"));
            bootstrap.setInjectionPoint(
                QWebEngineScript::DocumentCreation);
            bootstrap.setWorldId(QWebEngineScript::MainWorld);
            bootstrap.setRunsOnSubFrames(false);
            bootstrap.setSourceCode(bootstrapSource);
            scripts().insert(bootstrap);
        }

        // PIP01: the requestPictureInPicture shim.  QtWebEngine ships
        // no PiP delegate, so pages calling it would hit an
        // unimplemented path; the shim routes the request over the
        // channel (gesture-gated) to PictureInPicture's pop-out.
        QFile shimFile(QLatin1String(":pip-shim.js"));
        if (shimFile.open(QIODevice::ReadOnly)) {
            QWebEngineScript shim;
            shim.setName(QLatin1String("arora:pip-shim"));
            shim.setInjectionPoint(QWebEngineScript::DocumentCreation);
            shim.setWorldId(QWebEngineScript::MainWorld);
            shim.setRunsOnSubFrames(false);
            shim.setSourceCode(QString::fromUtf8(shimFile.readAll()));
            scripts().insert(shim);
        }

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
    }

    // SEC16: commits that did not pass through acceptNavigationRequest
    // (or whose final url differs after a redirect) still arm the
    // per-page DocumentReady scripts once the committed url is known.
    // urlChanged also fires for same-document navigations (fragment
    // changes, history.pushState) — verified on Qt 6.12: those emit
    // neither loadStarted nor a LoadStarted loadingChanged entry, only
    // a bare LoadSucceeded.  Re-arming there would rotate the autofill
    // report token out from under the still-live script, so re-arming
    // is gated on a real load being in flight.
    connect(this, &QWebEnginePage::loadStarted, this,
            [this]() { m_documentLoadPending = true; });
    connect(this, &QWebEnginePage::loadingChanged, this,
            [this](const QWebEngineLoadingInfo &info) {
        if (info.status() != QWebEngineLoadingInfo::LoadStartedStatus)
            m_documentLoadPending = false;
    });
    connect(this, &QWebEnginePage::urlChanged, this,
            [this](const QUrl &url) {
        if (m_documentLoadPending)
            schedulePageScripts(url);
    });

    // SEC15: keep Chromium's built-in error pages enabled.  With them
    // disabled, failed SUBFRAME loads produce no error commit and no
    // loadFinished signal for the frame — pages that gate on an
    // iframe's .load() (including failed loads) hang.  The built-in
    // page commits first and handleLoadingChanged() still replaces the
    // main-frame failure with Arora's notfound.html, so the custom
    // error page is preserved either way.
    settings()->setAttribute(QWebEngineSettings::ErrorPageEnabled, true);

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

    // BADSSL03: TLS client-certificate requests.  Left unanswered,
    // Chromium continues without a certificate — matching cert-less
    // Chrome.  The handler lets installed certificates actually flow
    // (BrowserProfile loads them into the profile's
    // clientCertificateStore at bring-up) and still declines when the
    // store is empty.
    connect(this, &QWebEnginePage::selectClientCertificate,
            this, [this](QWebEngineClientCertificateSelection selection) {
        handleClientCertificateSelection(selection);
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
            // The warning interstitials are chrome, not visited
            // pages — keep them out of history.
            const QString scheme = url().scheme();
            if (ok && scheme != QLatin1String("arora-cert-error")
                && scheme != QLatin1String("arora-http-warning")
                && scheme != QLatin1String("arora-site-block"))
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
    // UA02: Google's bot check redirects a search to its /sorry/
    // interstitial with an ordinary 200 — neither loadFinished(false)
    // nor an error domain reports it.  Detect the committed url and
    // lay a readable notice over the page so the failure mode is
    // diagnosable (and points at the engine switcher) instead of a
    // bare Google error page.
    connect(this, &QWebEnginePage::loadFinished, this, [this](bool ok) {
        if (ok)
            showRateLimitNoticeIfNeeded();
    });

    // Apply the configured user agent to whichever profile this page is
    // on (private windows run on the off-the-record profile).
    if (!s_userAgent.isEmpty()) {
        profile()->setHttpUserAgent(s_userAgent);
        BrowserProfile::applyClientHints(profile());
    }
    loadSettings();
    // JSCTL: the empty url has no host, so this only applies the tier
    // baseline — every accepted navigation reapplies per-site.
    applyJavaScriptPolicy(url());
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
    const auto applyTo = [effectiveAgent](QWebEngineProfile *profile) {
        profile->setHttpUserAgent(effectiveAgent);
        // UA02: the client hints must tell the same story as the UA.
        BrowserProfile::applyClientHints(profile);
    };
    applyTo(BrowserProfile::normalProfile());
    if (QWebEngineProfile *otrProfile = BrowserProfile::privateProfileIfCreated())
        applyTo(otrProfile);
    // CONT01: a UA override applies on materialized container profiles
    // too — containers send the same UA as the rest of the browser.
    const QList<QWebEngineProfile*> containerProfiles =
        ContainerManager::instance()->createdProfiles();
    for (QWebEngineProfile *containerProfile : containerProfiles)
        applyTo(containerProfile);
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

// SAFE02: while one insecure-form prompt is open, a nested insecure
// form post (the still-live document's scripts can trigger another
// navigation while exec() waits) is refused rather than stacking a
// second modal on top of the first.
static bool s_formPostPromptActive = false;

// Deliberately synchronous: the navigation hook must answer
// synchronously, and refusing now to re-issue the submit after a
// queued prompt would drop the POST body — Chromium never hands it
// back.  The engine's own javascriptConfirm() runs the same kind of
// nested loop while a navigation throttle waits.
bool WebPage::confirmInsecureFormPost(const QUrl &url)
{
    if (s_formPostPromptActive)
        return false;
    s_formPostPromptActive = true;
    QWidget *parent = QWebEngineView::forPage(this);
    QMessageBox box(QMessageBox::Warning,
        tr("Insecure Form Submission"),
        tr("This form is sending information to %1 over an insecure "
           "connection.\n\nAnyone on the network could read or "
           "change the data — including passwords.")
            .arg(QString::fromUtf8(url.host().toUtf8())),
        QMessageBox::Cancel, parent);
    // The destination host is page-controlled; render it literally.
    box.setTextFormat(Qt::PlainText);
    QPushButton *submitButton =
        box.addButton(tr("Submit Anyway"), QMessageBox::AcceptRole);
    box.setDefaultButton(QMessageBox::Cancel);
    box.exec();
    s_formPostPromptActive = false;
    return box.clickedButton() == submitButton;
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
            return SchemeAccessHandler::hasInterstitialPage(nonce);
        if (m_certErrorPending && nonce == m_certErrorNonce)
            resolveCertificateErrorLink(url);
        return false;
    }

    // SAFE01: the HTTPS-Only warning interstitial and its action
    // links — same nonce-bound scheme trick as the certificate-error
    // pages: only the rendered warning knows the nonce, so web
    // content cannot forge an "always allow" for a host it did not
    // trigger a warning for.
    if (scheme == QLatin1String("arora-http-warning")) {
        if (!isMainFrame)
            return false;
        const QString nonce = QUrlQuery(url)
                .queryItemValue(QLatin1String("n"));
        if (url.path() == QLatin1String("interstitial"))
            return SchemeAccessHandler::hasInterstitialPage(nonce);
        if (m_httpWarningPending && nonce == m_httpWarningNonce)
            resolveHttpWarningLink(url);
        return false;
    }

    // SEC18: the domain-block warning interstitial and its action
    // links — the same nonce-bound scheme trick: only the rendered
    // warning knows the nonce, so web content cannot forge a
    // "proceed" for a host it did not trigger a block for.
    if (scheme == QLatin1String("arora-site-block")) {
        if (!isMainFrame)
            return false;
        const QString nonce = QUrlQuery(url)
                .queryItemValue(QLatin1String("n"));
        if (url.path() == QLatin1String("interstitial"))
            return SchemeAccessHandler::hasInterstitialPage(nonce);
        if (m_domainBlockPending && nonce == m_domainBlockNonce)
            resolveDomainBlockLink(url);
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

    // CONT04: "Always open this site in <container>" — a main-frame
    // navigation whose host is ruled into a different container than
    // this page's cannot commit here; the url is reopened in a tab
    // bound to the ruled container and the request refused.  Redirect
    // hops re-enter this hook, so a mid-chain hop onto a ruled host
    // diverts too.  Private and tor windows have no containers and
    // never divert — PTAB01: the gate is the page's profile, so a
    // private TAB inside a normal window is covered too (diverting it
    // would record the visit on a persistent container profile).
    // Placement is ahead of the HTTPS-Only and insecure-form warnings
    // so those decisions are made by the tab that will actually load
    // the url.
    if (isMainFrame
        && (scheme == QLatin1String("http")
            || scheme == QLatin1String("https"))
        && !profile()->isOffTheRecord()
        && divertToContainerRule(url)) {
        return false;
    }

    // SEC18: anti-phishing/malware domain blocklist — a main-frame
    // navigation whose host is on the local list is refused and
    // swapped for the warning interstitial.  Redirect hops re-enter
    // this hook, and the request interceptor covers whatever bypasses
    // it, so every hop is gated.  Applies in tor windows too (a
    // listed clearnet host is just as hostile there).
    if (isMainFrame && PrivacyRequestInterceptor::shouldBlockDomain(url)) {
        showDomainBlockWarning(url);
        return false;
    }

    // Qt WebEngine asks the user about resubmitting POST data itself; the old
    // NavigationTypeFormResubmitted prompt has no equivalent here.

    // SAFE01/BADSSL03: an https->http redirect hop that returns to the
    // same address with only the scheme changed is the secure endpoint
    // bouncing the navigation to plaintext (badssl.com's http tests do
    // exactly this).  Left alone it loops forever — the interceptor
    // re-upgrades the hop and the server redirects down again until
    // Chromium aborts the whole navigation with an ERR_FAILED
    // attributed to the https: source, landing on the generic error
    // page instead of the HTTPS-Only warning.  m_requestedUrl is the
    // last accepted main-frame request — its equality with the hop's
    // https: form identifies the bounce.  Marking the host downgraded
    // stops the upgrade claiming the hop, and the ordinary decision
    // below then applies: HTTPS-Only warns, plain https-first lets the
    // plaintext page load.  Gate on isUpgradeCandidate so a host the
    // upgrade already skips (private/local, already marked) is
    // untouched — and a cross-path bounce (https://h/a -> http://h/b)
    // still converges after one extra round-trip, when the re-upgraded
    // https://h/b itself bounces.
    if (isMainFrame
        && type == QWebEnginePage::NavigationTypeRedirect
        && scheme == QLatin1String("http")
        && m_requestedUrl.scheme() == QLatin1String("https")
        && !BrowserApplication::isTorMode()) {
        QUrl httpsForm = url;
        httpsForm.setScheme(QLatin1String("https"));
        const QString scope =
            PrivacyRequestInterceptor::downgradeScope(profile());
        if (httpsForm == m_requestedUrl
            && PrivacyRequestInterceptor::httpsFirstEnabled()
            && PrivacyRequestInterceptor::isUpgradeCandidate(url, scope))
            PrivacyRequestInterceptor::markDowngraded(url.host(), scope);
    }

    // SAFE01: HTTPS-Only mode — an http: main-frame navigation that
    // the https-first upgrade did not claim (upgradeable hosts were
    // already redirected inside the interceptor; a host surviving here
    // as http: was downgraded after a failed https load, or the
    // upgrade is off) is refused and swapped for the warning
    // interstitial.  Redirect hops reach this hook too — the probe
    // run for this task verified a mid-chain redirect re-fires
    // acceptNavigationRequest — and the request interceptor covers
    // whatever bypasses it, so every http: hop warns.  Tor windows
    // exempt .onion and upgrade everything else, so nothing warns
    // there.
    if (isMainFrame && !BrowserApplication::isTorMode()
        && PrivacyRequestInterceptor::shouldWarnHttp(
            url, PrivacyRequestInterceptor::downgradeScope(profile()))) {
        showHttpWarning(url);
        return false;
    }

    // SAFE02: insecure form submission — a POST bound for a public
    // http: endpoint exposes its contents to anyone on the network
    // path, so the user confirms it once per target host per page.
    // The HTTPS-Only veto above claims non-excepted hosts first (the
    // two never double-prompt), so this fires for excepted/downgraded
    // hosts and when the strict mode is off.  Subframe form posts
    // warn too — the same data leaves the machine.  Only
    // FormSubmitted is gated: no other navigation type carries a
    // request body through this hook (resubmits on reload get
    // Chromium's own prompt).
    if (type == QWebEnginePage::NavigationTypeFormSubmitted
        && !BrowserApplication::isTorMode()
        && PrivacyRequestInterceptor::shouldWarnFormPost(
            url, PrivacyRequestInterceptor::downgradeScope(profile()))) {
        // Approvals belong to the document that asked — this->url()
        // is still the submitting page while the hook runs, so a
        // committed navigation re-keys the set lazily on the next
        // post and every page gets asked once per target.
        const QUrl source =
            this->url().adjusted(QUrl::RemoveFragment);
        if (m_insecureFormApprovalsPage != source) {
            m_insecureFormApprovedHosts.clear();
            m_insecureFormApprovalsPage = source;
        }
        const QString targetHost = url.host().toLower();
        if (!m_insecureFormApprovedHosts.contains(targetHost)) {
            if (!confirmInsecureFormPost(url))
                return false;
            m_insecureFormApprovedHosts.insert(targetHost);
        }
    }

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
        // JSCTL: decide this navigation's script policy while the load
        // is still pending — the per-page attribute must be set before
        // the commit for the renderer to honor it.
        applyJavaScriptPolicy(url);
        // SEC16: arm the per-document scripts pre-commit so they are
        // in the collection before the new document is created —
        // DocumentReady injection can't race a fast local load.  A
        // link click that only changes the fragment is same-document:
        // the armed scripts keep running in the existing document, so
        // re-arming would only rotate the autofill token they carry.
        const bool sameDocument =
            url.adjusted(QUrl::RemoveFragment)
                == this->url().adjusted(QUrl::RemoveFragment);
        if (!sameDocument
            && (m_blockedPopupCount != 0
                || !m_blockedPopupUrls.isEmpty())) {
            // POPUP01: a new document gets a fresh blocked pop-up
            // tally — the indicator shows per-page counts.
            m_blockedPopupCount = 0;
            m_blockedPopupUrls.clear();
            emit popupBlocked();
        }
        if (!sameDocument || type != QWebEnginePage::NavigationTypeLinkClicked)
            schedulePageScripts(url);
        // A real navigation supersedes any pending cert decision — the
        // deferred request is dead by the time the new load commits.
        m_certErrorPending = false;
        // Same for a pending HTTPS-Only warning — another navigation
        // being accepted means the refused target no longer applies.
        m_httpWarningPending = false;
        // SAFE07: a real navigation supersedes any uncommitted error
        // page — its phantom success no longer needs suppressing.
        // setHtml()'s internal data: commit passes through this hook
        // too and must NOT disarm it — that commit is the error page.
        if (url.scheme() != QLatin1String("data"))
            m_pendingErrorPages = 0;
        m_requestedUrl = url;
        emit aboutToLoadUrl(url);
    }

    return accepted;
}

// CONT04: true when url's host is ruled into a container other than
// this page's and the navigation has been handed to a tab on the
// ruled profile — acceptNavigationRequest then refuses the request
// so nothing commits on the wrong profile.  A form submission that
// diverts is re-issued as a plain load: the POST body cannot move
// profiles, and letting it commit here would leak it into the wrong
// container's storage.
bool WebPage::divertToContainerRule(const QUrl &url)
{
    ContainerManager *manager = ContainerManager::instance();
    const QString target = manager->containerIdForHost(url.host());
    if (target.isEmpty()
        || target == manager->containerIdForProfile(profile()))
        return false;

    // Bound the spawn: a cyclic cross-container redirect chain — a
    // host ruled into A redirecting to one ruled into B redirecting
    // back — would otherwise open tabs forever.  Eight diversions in
    // five seconds is far beyond any real chain and kills the cycle;
    // the refused navigation then loads in the current container.
    static QElapsedTimer s_diversionWindow;
    static int s_diversions = 0;
    if (!s_diversionWindow.isValid()
        || s_diversionWindow.elapsed() > 5000) {
        s_diversionWindow.start();
        s_diversions = 0;
    }
    if (++s_diversions > 8) {
        qWarning() << "WebPage: container-rule diversion rate exceeded;"
                   << url << "loads in the current container";
        return false;
    }

    WebView *webView = qobject_cast<WebView*>(QWebEngineView::forPage(this));
    if (webView) {
        if (TabWidget *tabs = webView->tabWidget()) {
            tabs->loadUrlInContainer(url, target);
            return true;
        }
        // A detached view has no tab strip — same fallback
        // openUrlInTarget uses: a standalone window on the ruled
        // container's profile.
        if (QWebEngineProfile *profile = manager->profileFor(target)) {
            WebView *detached = new WebView(profile);
            detached->setAttribute(Qt::WA_DeleteOnClose);
            detached->show();
            detached->loadUrl(url);
        }
    }
    // A page with no view at all (autotest harness, blocked pop-up
    // probe) has no chrome to host the diverted load — the navigation
    // simply cannot commit on the wrong profile.
    return true;
}

// POPUP01: the dead-end page a blocked pop-up gets in place of a real
// window.  window.open() still hands script a live window handle —
// the same thing a genuine block looks like to the page — but every
// navigation is refused.  The first http(s)/ftp target is reported
// back to the opener so the blocked indicator can offer "Open once",
// then the probe deletes itself; pop-ups the blocked page itself
// tries to open stay inside the dead-end, and a handle that only
// ever gets document.writes is reaped by the grace timer.
class PopupProbePage : public WebPage
{
public:
    PopupProbePage(QWebEngineProfile *profile, WebPage *source)
        : WebPage(profile, source)
        , m_source(source)
    {
        QTimer::singleShot(30000, this, &QObject::deleteLater);
    }

protected:
    bool acceptNavigationRequest(const QUrl &url,
                                 NavigationType type,
                                 bool isMainFrame) override
    {
        Q_UNUSED(type);
        if (!isMainFrame)
            return false;
        const QString scheme = url.scheme();
        if (scheme == QLatin1String("http")
            || scheme == QLatin1String("https")
            || scheme == QLatin1String("ftp")) {
            if (WebPage *source = m_source)
                source->noteBlockedPopupTarget(url);
            deleteLater();
        }
        // about:blank and script/data urls keep the probe alive a
        // moment longer — a script may navigate the handle to the
        // real target right after opening it — but never load.
        return false;
    }

    QWebEnginePage *createWindow(QWebEnginePage::WebWindowType type) override
    {
        Q_UNUSED(type);
        return new PopupProbePage(profile(), m_source);
    }

private:
    QPointer<WebPage> m_source;
};

QWebEnginePage *WebPage::createWindow(QWebEnginePage::WebWindowType type)
{
    // POPUP01: Chromium only withholds window.open calls that lack a
    // user gesture (the JavascriptCanOpenWindows=false binding keeps
    // doing that upstream, and keeping it matters — a gesture-less
    // plain window.open would otherwise arrive here tab-typed and
    // slip through as a pop-under).  Every call that can produce a
    // window arrives at createWindow; the blocker gates the
    // pop-up-shaped kinds — window.open with a feature string arrives
    // as WebBrowserWindow or WebDialog — while
    // WebBrowserTab/WebBrowserBackgroundTab are target=_blank-style
    // requests governed by the openTargetBlankLinksIn preference.
    // Chromium does not say whether a feature-less window.open
    // produced a tab-typed request, so that ambiguity deliberately
    // belongs to the tab-placement setting, not the pop-up blocker.
    const bool popupShaped = type == QWebEnginePage::WebBrowserWindow
        || type == QWebEnginePage::WebDialog;
    if (popupShaped && PopupBlocker::instance()->shouldBlock(url())) {
        ++m_blockedPopupCount;
        emit popupBlocked();
        return new PopupProbePage(profile(), this);
    }

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

// POPUP01: a dead-end probe reports the url the refused pop-up tried
// to reach.  Only urls worth offering for "Open once" are recorded
// (the probe itself filters to http/https/ftp); duplicates are
// folded and the list is capped so a pop-up storm cannot grow it.
void WebPage::noteBlockedPopupTarget(const QUrl &url)
{
    if (m_blockedPopupUrls.contains(url)
        || m_blockedPopupUrls.size() >= 20)
        return;
    m_blockedPopupUrls.append(url);
    emit popupBlocked();
}

void WebPage::handleLoadingChanged(const QWebEngineLoadingInfo &loadingInfo)
{
    const QString downgradeScope =
        PrivacyRequestInterceptor::downgradeScope(profile());

    // SAFE07: a committed https: main-frame load self-heals — the
    // host demonstrably serves TLS, so a stale downgrade mark (expired
    // or about to expire) is dropped before it can gate a future
    // navigation.  The error page shown for a failed load commits its
    // generated document under the failed url — swallow those phantom
    // commits or a mark would be wiped by its own error page.
    if (loadingInfo.status() == QWebEngineLoadingInfo::LoadSucceededStatus) {
        if (m_pendingErrorPages > 0)
            --m_pendingErrorPages;
        else if (loadingInfo.url().scheme() == QLatin1String("https")
                 && !loadingInfo.isErrorPage())
            PrivacyRequestInterceptor::clearDowngradedHost(
                loadingInfo.url().host(), downgradeScope);
        return;
    }

    if (loadingInfo.status() != QWebEngineLoadingInfo::LoadFailedStatus)
        return;

    // Certificate failures are presented by the interstitial page
    // (handleCertificateError), not by the generic notfound page — and
    // a deferred error that the user rejected via "back to safety"
    // must not stomp the page the user is navigating to.  They must
    // not mark the host downgraded either — a certificate problem is
    // not evidence the site lacks TLS.
    if (loadingInfo.errorDomain() == QWebEngineLoadingInfo::CertificateErrorDomain)
        return;

    QUrl errorUrl = loadingInfo.url();
    if (errorUrl.isEmpty())
        return;

    // SEC18: a main-frame request the interceptor just refused under
    // the domain blocklist arrives here as a failed load — swap in
    // the warning interstitial instead of the not-found page, same
    // consume-once contract as the HTTPS-Only registry below.
    if (PrivacyRequestInterceptor::takeBlockedDomainNav(errorUrl)) {
        showDomainBlockWarning(errorUrl);
        return;
    }
    if (m_domainBlockPending && errorUrl == m_domainBlockUrl)
        return;

    // SAFE01: a main-frame http: request the privacy interceptor just
    // refused under HTTPS-Only mode arrives here as a failed load
    // (ERR_ACCESS_DENIED, verified by the task's probe).  Swap in the
    // warning interstitial instead of the not-found page.  A vetoed
    // navigation never reaches the interceptor, so this path covers
    // the hops acceptNavigationRequest missed; the pending check
    // below silences the vetoed navigation's own failure signal when
    // one is still emitted.
    if (PrivacyRequestInterceptor::takeBlockedHttpNav(errorUrl)) {
        showHttpWarning(errorUrl);
        return;
    }
    // BADSSL03: a vetoed https->http redirect hop reports its failure
    // under the https: redirect SOURCE, not the refused http: target
    // the record names — try the downgraded spelling too.
    if (errorUrl.scheme() == QLatin1String("https")) {
        QUrl httpForm = errorUrl;
        httpForm.setScheme(QLatin1String("http"));
        if (PrivacyRequestInterceptor::takeBlockedHttpNav(httpForm)) {
            showHttpWarning(httpForm);
            return;
        }
    }
    // Same attribution gap for a hop the page vetoed itself: the
    // pending warning's failure can arrive under the https: form of
    // the refused http: target.  Suppress it either way — the queued
    // interstitial load must not be stomped by an error page.
    if (m_httpWarningPending) {
        QUrl httpsForm = m_httpWarningUrl;
        httpsForm.setScheme(QLatin1String("https"));
        if (errorUrl == m_httpWarningUrl || errorUrl == httpsForm)
            return;
    }

    if (errorUrl != m_requestedUrl) {
        // PRIV01: the HTTPS-first interceptor upgrades http:
        // navigations inside the network stack — acceptNavigationRequest
        // recorded the http: form in m_requestedUrl, so an upgraded
        // failure arrives under https: and must be matched back to it.
        // m_requestedUrl tracks each accepted redirect hop, so only
        // the hop that actually failed can match — a mid-chain abort
        // or a failure for an unrelated target returns early here and
        // cannot mark a host it never proved unreachable (SAFE07).
        QUrl downgraded = errorUrl;
        downgraded.setScheme(QLatin1String("http"));
        if (errorUrl.scheme() != QLatin1String("https")
            || downgraded != m_requestedUrl)
            return;
    }
    // SAFE07: mark the host http: only when the failure is genuine
    // TLS/connectivity evidence (a refused/reset/timed-out connection
    // or a TLS handshake failure) — vetoes, navigation aborts, DNS
    // faults and proxy plumbing errors are filtered inside
    // noteNavigationFailure, and under Tor nothing is marked at all:
    // every failure there is SOCKS-attributed and the Tor interceptor
    // never consults the set anyway.  Marks are scoped to this page's
    // profile and expire — see failureImpliesDowngrade/downgradeScope.
    const bool downgraded = !BrowserApplication::isTorMode()
        && PrivacyRequestInterceptor::noteNavigationFailure(
            errorUrl, int(loadingInfo.errorDomain()),
            loadingInfo.errorCode(), downgradeScope);

    showErrorPage(errorUrl, loadingInfo.errorString(),
                  downgraded || PrivacyRequestInterceptor::isDowngraded(
                      errorUrl.host(), downgradeScope));
}

// PRIV01: the httpsUpgradeFailed flag appends the downgrade notice to
// the suggestion list — the user sees why http: is allowed again and
// that the allowance expires with the session.
// The chromium guys have documented many examples of incompatibilities that
// different browsers have when they mime sniff.
// http://src.chromium.org/viewvc/chrome/trunk/src/net/base/mime_sniffer.cc
void WebPage::showErrorPage(const QUrl &errorUrl, const QString &errorString,
                            bool httpsUpgradeFailed)
{
    // Generate translated not found error page with an image
    QFile notFoundErrorFile(QLatin1String(":/notfound.html"));
    if (!notFoundErrorFile.open(QIODevice::ReadOnly))
        return;
    QString title = tr("Error loading page: %1").arg(QString::fromUtf8(errorUrl.toEncoded()));
    QString html = QLatin1String(notFoundErrorFile.readAll());
    QPixmap pixmap = QIcon(QLatin1String(":/arora.svg")).pixmap(QSize(64, 64));
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
    if (httpsUpgradeFailed) {
        // PRIV01: the visible downgrade-warning path — the host sits
        // in the session downgrade set, so the suggestion explains
        // both the failure and what plain http: does next: under
        // HTTPS-Only mode every request warns first, otherwise it is
        // simply allowed for the rest of the session.
        html.replace(QLatin1String("</ul>"),
            QLatin1String("<li>")
            + (PrivacyRequestInterceptor::httpsOnlyEnabled()
                ? tr("The secure (HTTPS) connection failed.  Plain "
                     "HTTP requests to this site will show a warning "
                     "before loading.")
                : tr("The secure (HTTPS) connection failed.  Plain "
                     "HTTP requests to this site will be allowed for "
                     "the rest of this session."))
            + QLatin1String("</li></ul>"));
    }
    BrowserTheme::decorateInternalPage(html);
    ++m_pendingErrorPages;   // SAFE07: the setHtml commit reports a
                             // LoadSucceeded for errorUrl — not real
                             // TLS evidence, don't self-heal on it.
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
    SchemeAccessHandler::publishInterstitialPage(m_certErrorNonce,
            certificateErrorHtml(error));
    error.rejectCertificate();

    // The interstitial is a real navigation served by the scheme
    // handler — not setHtml(): a data: document committed over a
    // failed navigation is sandboxed and its custom-scheme links never
    // reach acceptNavigationRequest (about:blank#blocked).
    SchemeAccessHandler::installInterstitialHandlers(profile());
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
    QPixmap pixmap = QIcon(QLatin1String(":/arora.svg")).pixmap(QSize(64, 64));
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
    BrowserTheme::decorateInternalPage(html);
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

// BADSSL03: TLS client-certificate requests arrive through the
// QWebEnginePage::selectClientCertificate signal (Qt5's
// selectClientCertificate() override is gone).  Declining when the
// profile's clientCertificateStore holds nothing matching is the
// same continue-without outcome an unanswered signal produces —
// e.g. client.badssl.com's HTTP 400, which is also what cert-less
// Chrome shows.  With candidates installed the user picks once per
// authority per page; the decision is remembered so a negotiation's
// repeated challenges don't each raise a dialog.
void WebPage::handleClientCertificateSelection(
        QWebEngineClientCertificateSelection selection)
{
    const QList<QSslCertificate> certs = selection.certificates();
    if (certs.isEmpty()) {
        selection.selectNone();
        return;
    }
    const QString authority = selection.host().authority();
    const auto remembered = m_clientCertChoices.constFind(authority);
    if (remembered != m_clientCertChoices.constEnd()) {
        // A null record is a remembered decline; a recorded cert that
        // the store no longer offers falls back to declining rather
        // than re-prompting mid-load.
        if (!remembered->isNull() && certs.contains(*remembered))
            selection.select(*remembered);
        else
            selection.selectNone();
        return;
    }

    // Test hook: --clientcert-smoke cannot drive the picker headless.
    if (qEnvironmentVariableIsSet("ARORA_CLIENTCERT_AUTOSELECT")) {
        m_clientCertChoices.insert(authority, certs.first());
        selection.select(certs.first());
        return;
    }

    QWidget *view = QWebEngineView::forPage(this);
    if (!view) {   // pages with no chrome (autotests, probes) decline
        selection.selectNone();
        return;
    }
    QStringList items;
    for (const QSslCertificate &cert : certs) {
        items << tr("%1 (issued by %2, expires %3)")
                .arg(cert.subjectDisplayName(),
                     cert.issuerDisplayName(),
                     cert.expiryDate().date().toString(Qt::ISODate));
    }
    bool ok = false;
    const QString choice = QInputDialog::getItem(view,
            tr("Client Certificate Requested"),
            tr("The site at %1 asks you to identify yourself with a "
               "certificate.  Pick one to send, or cancel to continue "
               "without identifying yourself.")
                .arg(selection.host().host()),
            items, 0, false, &ok);
    const int index = items.indexOf(choice);
    if (ok && index >= 0) {
        m_clientCertChoices.insert(authority, certs.at(index));
        selection.select(certs.at(index));
    } else {
        m_clientCertChoices.insert(authority, QSslCertificate());
        selection.selectNone();
    }
}

// SAFE01: HTTPS-Only strict mode — the http: target a navigation was
// stopped for becomes a nonce-bound interstitial on the private
// arora-http-warning: scheme (same machinery as the certificate-error
// page; setHtml() cannot work — a data: document's custom-scheme
// links are sandboxed to about:blank#blocked).  Called both from the
// acceptNavigationRequest veto and from handleLoadingChanged when the
// interceptor refused a hop, so the load() below is always queued —
// a navigation issued synchronously inside the veto would re-enter
// Chromium's navigation machinery.
void WebPage::showHttpWarning(const QUrl &target)
{
    m_httpWarningPending = true;
    m_httpWarningUrl = target;
    m_httpWarningNonce =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    SchemeAccessHandler::publishInterstitialPage(m_httpWarningNonce,
            httpWarningHtml(target));
    SchemeAccessHandler::installInterstitialHandlers(profile());
    const QUrl interstitial(
        QLatin1String("arora-http-warning:interstitial?n=")
        + m_httpWarningNonce);
    QPointer<WebPage> page(this);
    QTimer::singleShot(0, this, [page, interstitial]() {
        if (page)
            page->load(interstitial);
    });
    emit httpOnlyInterstitial(target);
}

QString WebPage::httpWarningHtml(const QUrl &target)
{
    QFile warningFile(QLatin1String(":/certerror.html"));
    if (!warningFile.open(QIODevice::ReadOnly))
        return QString();
    QString html = QLatin1String(warningFile.readAll());
    QPixmap pixmap = QIcon(QLatin1String(":/arora.svg")).pixmap(QSize(64, 64));
    QBuffer imageBuffer;
    imageBuffer.open(QBuffer::ReadWrite);
    if (pixmap.save(&imageBuffer, "PNG")) {
        html.replace(QLatin1String("IMAGE_BINARY_DATA_HERE"),
                     QLatin1String(imageBuffer.buffer().toBase64()));
    }

    // Everything interpolated here is web-controlled — escape it.
    const QString shownHost =
        QString::fromUtf8(target.host().toUtf8()).toHtmlEscaped();
    const QString shownUrl =
        QString::fromUtf8(target.toEncoded()).toHtmlEscaped();

    QString buttons = tr("<a id=\"back\" href=\"arora-http-warning:back?n=%1\">Back to safety</a>")
            .arg(m_httpWarningNonce);
    buttons += tr("<a id=\"proceed\" href=\"arora-http-warning:proceed?n=%1\">Proceed anyway (unsafe)</a>")
            .arg(m_httpWarningNonce);
    // "Always" persists a host exception — off-the-record profiles
    // never write settings, so they only get the session-scoped path.
    if (!profile()->isOffTheRecord()) {
        buttons += tr("<a id=\"always\" href=\"arora-http-warning:always?n=%1\">Always allow HTTP on this site</a>")
                .arg(m_httpWarningNonce);
    }

    html = html.arg(
        tr("Insecure connection: %1").arg(shownUrl),
        tr("This site does not support HTTPS"),
        tr("Arora stopped %1 from loading because it can only be "
           "reached over an unencrypted HTTP connection.  Anyone on "
           "the network path can read or change everything sent to or "
           "received from this site — including passwords and "
           "cookies.").arg(shownHost),
        QLatin1String("<li>")
            + tr("Requested address: %1").arg(shownUrl)
            + QLatin1String("</li>"),
        QLatin1String("<li>")
            + tr("The site may simply not offer a secure version, or "
                 "the connection may have been stripped back to HTTP "
                 "by a network attacker.")
            + QLatin1String("</li>"),
        buttons);
    BrowserTheme::decorateInternalPage(html);
    return html;
}

void WebPage::resolveHttpWarningLink(const QUrl &command)
{
    const QUrl target = m_httpWarningUrl;
    m_httpWarningPending = false;
    QPointer<WebPage> page(this);
    if (command.path() == QLatin1String("proceed")
        || command.path() == QLatin1String("always")) {
        // "proceed" remembers the host for the session; "always"
        // persists it in QSettings.  An off-the-record page never
        // renders the always link — force session scope anyway so a
        // nonce-carrying forged navigation cannot persist either.
        const bool persist = command.path() == QLatin1String("always")
            && !profile()->isOffTheRecord();
        PrivacyRequestInterceptor::allowHttpForHost(target.host(),
                                                  persist);
        // Queued: navigating from inside acceptNavigationRequest
        // would re-enter Chromium's navigation machinery.
        QTimer::singleShot(0, this, [page, target]() {
            if (page)
                page->load(target);
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

// SEC18: the refused navigation's target becomes a nonce-bound
// interstitial on the private arora-site-block: scheme — same
// machinery as the certificate-error and HTTPS-Only pages.  Called
// both from the acceptNavigationRequest veto and from
// handleLoadingChanged when the interceptor refused a hop, so the
// load() below is always queued (a navigation issued synchronously
// inside the veto would re-enter Chromium's navigation machinery).
void WebPage::showDomainBlockWarning(const QUrl &target)
{
    m_domainBlockPending = true;
    m_domainBlockUrl = target;
    m_domainBlockNonce =
        QUuid::createUuid().toString(QUuid::WithoutBraces);
    SchemeAccessHandler::publishInterstitialPage(m_domainBlockNonce,
            domainBlockWarningHtml(target));
    SchemeAccessHandler::installInterstitialHandlers(profile());
    const QUrl interstitial(
        QLatin1String("arora-site-block:interstitial?n=")
        + m_domainBlockNonce);
    QPointer<WebPage> page(this);
    QTimer::singleShot(0, this, [page, interstitial]() {
        if (page)
            page->load(interstitial);
    });
    emit domainBlockInterstitial(target);
}

QString WebPage::domainBlockWarningHtml(const QUrl &target)
{
    QFile warningFile(QLatin1String(":/certerror.html"));
    if (!warningFile.open(QIODevice::ReadOnly))
        return QString();
    QString html = QLatin1String(warningFile.readAll());
    QWidget *view = QWebEngineView::forPage(this);
    QPixmap pixmap = qApp->style()->standardIcon(QStyle::SP_MessageBoxCritical, nullptr, view).pixmap(QSize(32, 32));
    QBuffer imageBuffer;
    imageBuffer.open(QBuffer::ReadWrite);
    if (pixmap.save(&imageBuffer, "PNG")) {
        html.replace(QLatin1String("IMAGE_BINARY_DATA_HERE"),
                     QLatin1String(imageBuffer.buffer().toBase64()));
    }

    // Everything interpolated here is web-controlled — escape it.
    const QString shownHost =
        QString::fromUtf8(target.host().toUtf8()).toHtmlEscaped();
    const QString shownUrl =
        QString::fromUtf8(target.toEncoded()).toHtmlEscaped();

    // Two actions only: back to safety, or proceed for this session.
    // There is deliberately no "always" link — a persisted bypass of
    // a listed phishing/malware domain is a foot-gun SAFE01's plain
    // http exception can afford to be and this list cannot.
    QString buttons = tr("<a id=\"back\" href=\"arora-site-block:back?n=%1\">Back to safety</a>")
            .arg(m_domainBlockNonce);
    buttons += tr("<a id=\"proceed\" href=\"arora-site-block:proceed?n=%1\">Proceed anyway (unsafe)</a>")
            .arg(m_domainBlockNonce);

    html = html.arg(
        tr("Dangerous site blocked: %1").arg(shownUrl),
        tr("This site is on the local phishing/malware blocklist"),
        tr("Arora stopped %1 from loading because the domain is "
           "listed as serving phishing or malware.  Visiting it can "
           "steal credentials or infect this computer.")
            .arg(shownHost),
        QLatin1String("<li>")
            + tr("Requested address: %1").arg(shownUrl)
            + QLatin1String("</li>"),
        QLatin1String("<li>")
            + tr("The list is a local copy — no lookup left this "
                 "computer.  Proceeding is remembered only for this "
                 "session.")
            + QLatin1String("</li>"),
        buttons);
    BrowserTheme::decorateInternalPage(html);
    return html;
}

void WebPage::resolveDomainBlockLink(const QUrl &command)
{
    const QUrl target = m_domainBlockUrl;
    m_domainBlockPending = false;
    QPointer<WebPage> page(this);
    if (command.path() == QLatin1String("proceed")) {
        // Session-scoped only — the choice is logged for the session
        // and never written to settings.
        qInfo() << "WebPage: domain-blocklist proceed for"
                << target.host();
        PrivacyRequestInterceptor::allowBlockedDomain(target.host());
        // Queued: navigating from inside acceptNavigationRequest
        // would re-enter Chromium's navigation machinery.
        QTimer::singleShot(0, this, [page, target]() {
            if (page)
                page->load(target);
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

// UA02: Google serves its "unusual traffic" bot check from
// <google host>/sorry/index.  No public-suffix list is available in
// Qt6, so the host check is a label-position approximation: a
// "google" label in second-to-last place (google.com, www.google.de)
// or third-to-last with a plausible public-suffix second-level label
// (google.co.uk, www.google.com.au).  Substring matches would also
// fire on lookalikes like "google.com.evil.example".
bool WebPage::isRateLimitInterstitialUrl(const QUrl &url)
{
    if (!url.path().startsWith(QLatin1String("/sorry/")))
        return false;
    static const QSet<QString> secondLevel = {
        QStringLiteral("ac"),  QStringLiteral("ad"),
        QStringLiteral("co"),  QStringLiteral("com"),
        QStringLiteral("edu"), QStringLiteral("gen"),
        QStringLiteral("go"),  QStringLiteral("gov"),
        QStringLiteral("gouv"),QStringLiteral("id"),
        QStringLiteral("in"),  QStringLiteral("ind"),
        QStringLiteral("me"),  QStringLiteral("med"),
        QStringLiteral("mil"), QStringLiteral("ne"),
        QStringLiteral("net"), QStringLiteral("nom"),
        QStringLiteral("or"),  QStringLiteral("org"),
        QStringLiteral("res"), QStringLiteral("sch"),
        QStringLiteral("web"),
    };
    const QStringList labels = url.host().split(QLatin1Char('.'));
    const int count = labels.size();
    for (int i = 0; i < count; ++i) {
        if (labels.at(i) != QLatin1String("google"))
            continue;
        if (i == count - 2 && labels.at(i + 1).size() >= 2)
            return true;
        if (i == count - 3 && secondLevel.contains(labels.at(i + 1))
                && labels.at(i + 2).size() >= 2)
            return true;
    }
    return false;
}

// The /sorry/ page already carries the recovery path (a captcha that
// unblocks the profile), so the notice is a banner over it rather than
// a replacement interstitial — replacing the page would hide the only
// way back out.
void WebPage::showRateLimitNoticeIfNeeded()
{
    if (!isRateLimitInterstitialUrl(url()))
        return;

    const auto jsQuote = [](QString text) {
        text.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
        text.replace(QLatin1Char('"'), QLatin1String("\\\""));
        text.replace(QLatin1Char('\n'), QLatin1String("\\n"));
        text.remove(QLatin1Char('\r'));
        return text;
    };
    const QString script = QStringLiteral(
        "(function(){"
        "if(document.getElementById('arora-rate-limit'))return;"
        "var bar=document.createElement('div');"
        "bar.id='arora-rate-limit';"
        "bar.setAttribute('style','all:initial;display:block;position:relative;"
            "z-index:2147483647;background:#fff4ce;color:#3b3105;"
            "border-bottom:2px solid #e0c45f;"
            "font:14px/1.45 sans-serif;padding:10px 16px;');"
        "var title=document.createElement('strong');"
        "title.textContent=\"%1 \";"
        "bar.appendChild(title);"
        "var body=document.createElement('span');"
        "body.textContent=\"%2\";"
        "bar.appendChild(body);"
        "var parent=document.body||document.documentElement;"
        "parent.insertBefore(bar,parent.firstChild);"
        "})();")
        .arg(jsQuote(tr("Google rate-limited this connection.")),
             jsQuote(tr("This is Google's bot check, not an Arora error — "
                        "it temporarily flagged search traffic from this "
                        "profile. Complete the prompt below or try again "
                        "later; or switch the default search engine under "
                        "Tools > Options > Search.")));
    runJavaScript(script);
}

void WebPage::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    m_openTargetBlankLinksIn = (TabWidget::OpenUrlIn)settings.value(QLatin1String("openTargetBlankLinksIn"),
                                                                    TabWidget::NewSelectedTab).toInt();
    settings.endGroup();
    setUserAgent(settings.value(QLatin1String("userAgent")).toString());
    // JSCTL: the settings dialog replays loadSettings on every live
    // page — re-evaluate so a tier or global-pref flip reaches open
    // tabs without waiting for their next navigation.  (Scripts
    // already running in the document are not retroactively killed;
    // the attribute governs the page's next script execution.)
    applyJavaScriptPolicy(url());
}

// JSCTL: applies the ScriptControlManager decision to this page's own
// settings object and tracks the blocked state the info bar and shield
// icon surface.  The bar only makes sense for real sites, so the
// blocked flag is limited to host-bearing web urls — internal pages
// keep scripts at every tier anyway.
void WebPage::applyJavaScriptPolicy(const QUrl &url)
{
    const bool enabled =
        ScriptControlManager::instance()->isJavaScriptEnabledFor(url);
    if (settings()->testAttribute(QWebEngineSettings::JavascriptEnabled)
            != enabled)
        settings()->setAttribute(QWebEngineSettings::JavascriptEnabled,
                                 enabled);

    const QString scheme = url.scheme();
    const bool blocked = !enabled
        && (scheme == QLatin1String("http")
            || scheme == QLatin1String("https"));
    m_javaScriptBlocked = blocked;
    m_javaScriptBlockedHost = blocked ? url.host() : QString();
    emit javaScriptBlockedChanged(blocked);
}

// SEC16: arms the adblock cosmetic pass and the autofill bundle as
// per-page QWebEngineScripts injected at DocumentReady.  Both used to
// run from WebView::loadFinished, where their renderer-side work
// competed with the paint-gated subresource scheduling that happens
// on the load-event turn (the browseraudit latency group).  The
// dedup is load-identity, not just bookkeeping: re-arming an already
// committed url would rotate the autofill report token while a script
// carrying the previous token may already be running.
void WebPage::schedulePageScripts(const QUrl &url)
{
    if (url == m_scheduledScriptUrl)
        return;
    m_scheduledScriptUrl = url;
    if (!m_injectedScriptsEnabled)
        return;
    Engine::Page *enginePage = WebEnginePageAdapter::forPage(this);
    AdBlockManager::instance()->page()->scheduleRulesOnPage(enginePage, url);
    // TOR02: no autofill fill/capture in a tor window — stored
    // credentials are a cross-context identity leak.
    if (!BrowserApplication::isTorMode())
        AutoFillManager::instance()->scheduleOnPage(enginePage, url);
}

void WebPage::setInjectedScriptsEnabled(bool enabled)
{
    if (m_injectedScriptsEnabled == enabled)
        return;
    m_injectedScriptsEnabled = enabled;
    if (!enabled) {
        // The "arora:" name prefix namespaces only the two scripts this
        // path arms — audit-harness capture scripts use "arora-*" and
        // are deliberately not matched.
        QWebEngineScriptCollection &collection = scripts();
        const QList<QWebEngineScript> installed = collection.toList();
        for (const QWebEngineScript &script : installed) {
            if (script.name().startsWith(QLatin1String("arora:")))
                collection.remove(script);
        }
        return;
    }
    const QUrl armed = m_scheduledScriptUrl;
    m_scheduledScriptUrl = QUrl();
    schedulePageScripts(armed.isEmpty() ? url() : armed);
}
