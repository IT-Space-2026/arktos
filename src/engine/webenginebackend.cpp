/*
 * Copyright 2026 The Arora Authors
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

#include "webenginebackend.h"

#include <qfileinfo.h>
#include <qtwebenginecoreglobal.h>
#include <qwebenginecertificateerror.h>
#include <qwebenginecookiestore.h>
#include <qwebenginedownloadrequest.h>
#include <qwebenginefindtextresult.h>
#include <qwebenginehistory.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginescript.h>
#include <qwebenginescriptcollection.h>
#include <qwebenginesettings.h>
#include <qwebengineurlrequestinfo.h>
#include <qwebengineurlrequestinterceptor.h>
#include <qwebengineview.h>

// ---------------------------------------------------------------------------
// enum mapping helpers — the interface enums mirror Qt's values from an
// older release; Qt 6.12 inserted ResourceTypeJson and keeps WebSocket at
// 254, so the mapping is explicit rather than a cast.

static Engine::ResourceType engineResourceType(QWebEngineUrlRequestInfo::ResourceType type)
{
    switch (type) {
    case QWebEngineUrlRequestInfo::ResourceTypeMainFrame: return Engine::ResourceType::MainFrame;
    case QWebEngineUrlRequestInfo::ResourceTypeSubFrame: return Engine::ResourceType::SubFrame;
    case QWebEngineUrlRequestInfo::ResourceTypeStylesheet: return Engine::ResourceType::Stylesheet;
    case QWebEngineUrlRequestInfo::ResourceTypeScript: return Engine::ResourceType::Script;
    case QWebEngineUrlRequestInfo::ResourceTypeImage: return Engine::ResourceType::Image;
    case QWebEngineUrlRequestInfo::ResourceTypeFontResource: return Engine::ResourceType::FontResource;
    case QWebEngineUrlRequestInfo::ResourceTypeSubResource: return Engine::ResourceType::SubResource;
    case QWebEngineUrlRequestInfo::ResourceTypeObject: return Engine::ResourceType::Object;
    case QWebEngineUrlRequestInfo::ResourceTypeMedia: return Engine::ResourceType::Media;
    case QWebEngineUrlRequestInfo::ResourceTypeWorker: return Engine::ResourceType::Worker;
    case QWebEngineUrlRequestInfo::ResourceTypeSharedWorker: return Engine::ResourceType::SharedWorker;
    case QWebEngineUrlRequestInfo::ResourceTypePrefetch: return Engine::ResourceType::Prefetch;
    case QWebEngineUrlRequestInfo::ResourceTypeFavicon: return Engine::ResourceType::Favicon;
    case QWebEngineUrlRequestInfo::ResourceTypeXhr: return Engine::ResourceType::Xhr;
    case QWebEngineUrlRequestInfo::ResourceTypePing: return Engine::ResourceType::Ping;
    case QWebEngineUrlRequestInfo::ResourceTypeServiceWorker: return Engine::ResourceType::ServiceWorker;
    case QWebEngineUrlRequestInfo::ResourceTypeCspReport: return Engine::ResourceType::CspReport;
    case QWebEngineUrlRequestInfo::ResourceTypePluginResource: return Engine::ResourceType::PluginResource;
    case QWebEngineUrlRequestInfo::ResourceTypeNavigationPreloadMainFrame:
        return Engine::ResourceType::NavigationPreloadMainFrame;
    case QWebEngineUrlRequestInfo::ResourceTypeNavigationPreloadSubFrame:
        return Engine::ResourceType::NavigationPreloadSubFrame;
    case QWebEngineUrlRequestInfo::ResourceTypeWebSocket: return Engine::ResourceType::WebSocket;
    default: return Engine::ResourceType::Unknown;
    }
}

static Engine::NavigationType engineNavigationType(QWebEngineUrlRequestInfo::NavigationType type)
{
    switch (type) {
    case QWebEngineUrlRequestInfo::NavigationTypeLink: return Engine::NavigationType::LinkClicked;
    case QWebEngineUrlRequestInfo::NavigationTypeTyped: return Engine::NavigationType::Typed;
    case QWebEngineUrlRequestInfo::NavigationTypeFormSubmitted: return Engine::NavigationType::FormSubmitted;
    case QWebEngineUrlRequestInfo::NavigationTypeBackForward: return Engine::NavigationType::BackOrForward;
    case QWebEngineUrlRequestInfo::NavigationTypeReload: return Engine::NavigationType::Reload;
    case QWebEngineUrlRequestInfo::NavigationTypeRedirect: return Engine::NavigationType::Redirect;
    default: return Engine::NavigationType::Other;
    }
}

static QWebEnginePage::FindFlags webEngineFindFlags(Engine::FindFlags flags)
{
    QWebEnginePage::FindFlags out;
    if (flags & Engine::FindBackward)
        out |= QWebEnginePage::FindBackward;
    if (flags & Engine::FindCaseSensitively)
        out |= QWebEnginePage::FindCaseSensitively;
    return out;
}

static QWebEngineScript::InjectionPoint webEngineInjectionPoint(Engine::InjectionPoint point)
{
    switch (point) {
    case Engine::InjectionPoint::DocumentCreation: return QWebEngineScript::DocumentCreation;
    case Engine::InjectionPoint::DocumentReady: return QWebEngineScript::DocumentReady;
    default: return QWebEngineScript::Deferred;
    }
}

static Engine::InjectionPoint engineInjectionPoint(QWebEngineScript::InjectionPoint point)
{
    switch (point) {
    case QWebEngineScript::DocumentCreation: return Engine::InjectionPoint::DocumentCreation;
    case QWebEngineScript::DocumentReady: return Engine::InjectionPoint::DocumentReady;
    default: return Engine::InjectionPoint::Deferred;
    }
}

static QWebEngineSettings::WebAttribute webEngineAttribute(const QString &name, bool *known)
{
    static const QHash<QString, QWebEngineSettings::WebAttribute> attributes = {
        { QStringLiteral("AutoLoadImages"), QWebEngineSettings::AutoLoadImages },
        { QStringLiteral("JavascriptEnabled"), QWebEngineSettings::JavascriptEnabled },
        { QStringLiteral("JavascriptCanOpenWindows"), QWebEngineSettings::JavascriptCanOpenWindows },
        { QStringLiteral("JavascriptCanAccessClipboard"), QWebEngineSettings::JavascriptCanAccessClipboard },
        { QStringLiteral("LocalStorageEnabled"), QWebEngineSettings::LocalStorageEnabled },
        { QStringLiteral("PluginsEnabled"), QWebEngineSettings::PluginsEnabled },
        { QStringLiteral("FullScreenSupportEnabled"), QWebEngineSettings::FullScreenSupportEnabled },
        { QStringLiteral("ScreenCaptureEnabled"), QWebEngineSettings::ScreenCaptureEnabled },
        { QStringLiteral("WebGLEnabled"), QWebEngineSettings::WebGLEnabled },
        { QStringLiteral("ScrollAnimatorEnabled"), QWebEngineSettings::ScrollAnimatorEnabled },
        { QStringLiteral("ErrorPageEnabled"), QWebEngineSettings::ErrorPageEnabled },
        { QStringLiteral("FocusOnNavigationEnabled"), QWebEngineSettings::FocusOnNavigationEnabled },
        { QStringLiteral("DnsPrefetchEnabled"), QWebEngineSettings::DnsPrefetchEnabled },
        { QStringLiteral("PdfViewerEnabled"), QWebEngineSettings::PdfViewerEnabled },
        { QStringLiteral("ForceDarkMode"), QWebEngineSettings::ForceDarkMode },
        { QStringLiteral("HyperlinkAuditingEnabled"), QWebEngineSettings::HyperlinkAuditingEnabled },
        { QStringLiteral("AllowRunningInsecureContent"), QWebEngineSettings::AllowRunningInsecureContent },
        { QStringLiteral("LocalContentCanAccessRemoteUrls"), QWebEngineSettings::LocalContentCanAccessRemoteUrls },
        { QStringLiteral("LocalContentCanAccessFileUrls"), QWebEngineSettings::LocalContentCanAccessFileUrls },
    };
    const auto it = attributes.constFind(name);
    *known = it != attributes.constEnd();
    return *known ? it.value() : QWebEngineSettings::AutoLoadImages;
}

// ---------------------------------------------------------------------------
// The single marshaling interceptor a profile installs for Engine::
// RequestPolicy.  When the app's existing interceptors (privacy, adblock,
// tor) migrate behind the interface they compose into one policy and all
// reach the engine through this hook — a profile can only carry one
// QWebEngineUrlRequestInterceptor, so it is also the merge point.
//
// interceptRequest runs on the engine's IO thread — the Engine::
// RequestPolicy contract requires implementations to be thread-safe
// (the rustcore policy marshals across the FFI boundary).

class PolicyInterceptor : public QWebEngineUrlRequestInterceptor
{
public:
    explicit PolicyInterceptor(WebEngineProfileAdapter *owner)
        : QWebEngineUrlRequestInterceptor(owner)
        , m_owner(owner)
    {
    }

    void interceptRequest(QWebEngineUrlRequestInfo &info) override
    {
        Engine::RequestPolicy *policy = m_owner->requestPolicy();
        if (!policy)
            return;

        Engine::NavigationRequest request;
        request.url = info.requestUrl();
        request.resourceType = engineResourceType(info.resourceType());
        request.navigationType = engineNavigationType(info.navigationType());
        request.firstPartyUrl = info.firstPartyUrl();
        request.initiatorOrigin = info.initiator();
        request.isMainFrame = info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame;
        // QWebEngineUrlRequestInfo exposes no request-header read access,
        // so fetch metadata (Sec-Purpose) stays empty on this backend.

        const Engine::RequestDecision decision = policy->decide(request);
        switch (decision.action) {
        case Engine::RequestAction::Block:
            info.block(true);
            break;
        case Engine::RequestAction::Redirect:
            info.redirect(decision.redirectUrl);
            break;
        default:
            break;
        }
        if (!decision.referer.isEmpty())
            info.setHttpHeader("Referer", decision.referer);
    }

private:
    WebEngineProfileAdapter *m_owner;
};

// ---------------------------------------------------------------------------

WebEnginePageAdapter::WebEnginePageAdapter(QWebEnginePage *page, QObject *parent)
    : Engine::Page(parent)
    , m_page(page)
{
    connect(m_page, &QWebEnginePage::loadStarted,
            this, &Engine::Page::loadStarted);
    connect(m_page, &QWebEnginePage::loadProgress,
            this, &Engine::Page::loadProgress);
    connect(m_page, &QWebEnginePage::loadFinished,
            this, &Engine::Page::loadFinished);
    connect(m_page, &QWebEnginePage::urlChanged,
            this, &Engine::Page::urlChanged);
    connect(m_page, &QWebEnginePage::titleChanged,
            this, &Engine::Page::titleChanged);
    connect(m_page, &QWebEnginePage::iconChanged,
            this, &Engine::Page::iconChanged);
    connect(m_page, &QWebEnginePage::linkHovered,
            this, &Engine::Page::linkHovered);
    connect(m_page, &QWebEnginePage::windowCloseRequested,
            this, &Engine::Page::windowCloseRequested);
    connect(m_page, &QWebEnginePage::printRequested,
            this, &Engine::Page::printRequested);
    connect(m_page, &QWebEnginePage::renderProcessPidChanged,
            this, &Engine::Page::renderProcessIdChanged);
    connect(m_page, &QWebEnginePage::findTextFinished,
            this, [this](const QWebEngineFindTextResult &result) {
        Engine::FindResult translated;
        translated.numberOfMatches = result.numberOfMatches();
        translated.activeMatch = result.activeMatch();
        emit findTextFinished(translated);
    });
    connect(m_page, &QWebEnginePage::certificateError,
            this, [this](const QWebEngineCertificateError &error) {
        Engine::CertificateErrorInfo info;
        info.url = error.url();
        info.error = static_cast<int>(error.type());
        info.description = error.description();
        info.overridable = error.isOverridable();
        info.isMainFrame = error.isMainFrame();
        info.certificateChain = error.certificateChain();
        bool accepted = false;
        emit certificateError(info, &accepted);
        // Only an explicit accept is applied — leaving rejection to the
        // engine's default keeps any handler already on the page (the
        // app's interstitial defers its own decision) authoritative.
        if (accepted) {
            QWebEngineCertificateError decision = error;
            decision.acceptCertificate();
        }
    });
}

QWebEnginePage *WebEnginePageAdapter::webEnginePage() const
{
    return m_page;
}

void WebEnginePageAdapter::load(const QUrl &url)
{
    m_page->load(url);
}

void WebEnginePageAdapter::stop()
{
    m_page->triggerAction(QWebEnginePage::Stop);
}

void WebEnginePageAdapter::reload()
{
    m_page->triggerAction(QWebEnginePage::Reload);
}

QUrl WebEnginePageAdapter::url() const
{
    return m_page->url();
}

bool WebEnginePageAdapter::canGoBack() const
{
    return m_page->history()->canGoBack();
}

bool WebEnginePageAdapter::canGoForward() const
{
    return m_page->history()->canGoForward();
}

void WebEnginePageAdapter::back()
{
    m_page->triggerAction(QWebEnginePage::Back);
}

void WebEnginePageAdapter::forward()
{
    m_page->triggerAction(QWebEnginePage::Forward);
}

void WebEnginePageAdapter::setZoomFactor(qreal factor)
{
    m_page->setZoomFactor(factor);
}

qreal WebEnginePageAdapter::zoomFactor() const
{
    return m_page->zoomFactor();
}

void WebEnginePageAdapter::findText(const QString &subString, Engine::FindFlags options)
{
    m_page->findText(subString, webEngineFindFlags(options));
}

void WebEnginePageAdapter::runJavaScript(
        const QString &source,
        const std::function<void(const QVariant &)> &resultCallback)
{
    m_page->runJavaScript(source, resultCallback);
}

void WebEnginePageAdapter::setPageAttribute(const QString &name, bool on)
{
    bool known = false;
    const QWebEngineSettings::WebAttribute attribute = webEngineAttribute(name, &known);
    if (known)
        m_page->settings()->setAttribute(attribute, on);
}

void WebEnginePageAdapter::setLifecycleState(Engine::Page::LifecycleState state)
{
    m_page->setLifecycleState(static_cast<QWebEnginePage::LifecycleState>(state));
}

Engine::Page::LifecycleState WebEnginePageAdapter::lifecycleState() const
{
    return static_cast<LifecycleState>(m_page->lifecycleState());
}

qint64 WebEnginePageAdapter::renderProcessId() const
{
    return m_page->renderProcessPid();
}

Engine::Page *WebEnginePageAdapter::createWindow(Engine::WebWindowType type)
{
    return m_createWindow ? m_createWindow(type) : nullptr;
}

void WebEnginePageAdapter::setCreateWindowHandler(
        const std::function<Engine::Page *(Engine::WebWindowType)> &handler)
{
    m_createWindow = handler;
}

// ---------------------------------------------------------------------------

WebEngineDownloadRequest::WebEngineDownloadRequest(QWebEngineDownloadRequest *request,
                                                   QObject *parent)
    : Engine::DownloadRequest(parent)
    , m_request(request)
{
    connect(m_request, &QWebEngineDownloadRequest::stateChanged,
            this, &Engine::DownloadRequest::stateChanged);
    connect(m_request, &QWebEngineDownloadRequest::receivedBytesChanged,
            this, &Engine::DownloadRequest::receivedBytesChanged);
}

QUrl WebEngineDownloadRequest::url() const
{
    return m_request->url();
}

QString WebEngineDownloadRequest::suggestedFileName() const
{
    return m_request->suggestedFileName();
}

void WebEngineDownloadRequest::accept(const QString &filePath)
{
    m_request->setDownloadDirectory(QFileInfo(filePath).absolutePath());
    m_request->setDownloadFileName(QFileInfo(filePath).fileName());
    m_request->accept();
}

void WebEngineDownloadRequest::cancel()
{
    m_request->cancel();
}

void WebEngineDownloadRequest::pause()
{
    m_request->pause();
}

void WebEngineDownloadRequest::resume()
{
    m_request->resume();
}

qint64 WebEngineDownloadRequest::receivedBytes() const
{
    return m_request->receivedBytes();
}

qint64 WebEngineDownloadRequest::totalBytes() const
{
    return m_request->totalBytes();
}

// ---------------------------------------------------------------------------

WebEngineProfileAdapter::WebEngineProfileAdapter(QWebEngineProfile *profile,
                                                 QObject *parent)
    : Engine::Profile(parent)
    , m_profile(profile)
{
    if (m_profile) {
        connect(m_profile, &QWebEngineProfile::downloadRequested,
                this, [this](QWebEngineDownloadRequest *request) {
            // The adapter outlives each request; the request itself is
            // engine-owned and dies with the download.
            emit downloadRequested(new WebEngineDownloadRequest(request, this));
        });
    }
}

QWebEngineProfile *WebEngineProfileAdapter::webEngineProfile() const
{
    return m_profile;
}

bool WebEngineProfileAdapter::isOffTheRecord() const
{
    return m_profile->isOffTheRecord();
}

QString WebEngineProfileAdapter::storageName() const
{
    return m_profile->storageName();
}

void WebEngineProfileAdapter::setUserAgent(const QString &userAgent)
{
    m_profile->setHttpUserAgent(userAgent);
}

void WebEngineProfileAdapter::setRequestPolicy(Engine::RequestPolicy *policy)
{
    m_policy = policy;
    if (policy && !m_policyInterceptor)
        m_policyInterceptor = new PolicyInterceptor(this);
    m_profile->setUrlRequestInterceptor(policy ? m_policyInterceptor : nullptr);
}

Engine::RequestPolicy *WebEngineProfileAdapter::requestPolicy() const
{
    return m_policy;
}

void WebEngineProfileAdapter::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    if (!filter) {
        m_profile->cookieStore()->setCookieFilter(std::function<bool(
                const QWebEngineCookieStore::FilterRequest &)>());
        return;
    }
    m_profile->cookieStore()->setCookieFilter(
            [filter](const QWebEngineCookieStore::FilterRequest &request) {
        Engine::CookieAttempt attempt;
        attempt.firstPartyUrl = request.firstPartyUrl;
        attempt.origin = request.origin;
        attempt.thirdParty = request.thirdParty;
        return filter(attempt);
    });
}

void WebEngineProfileAdapter::insertScript(const Engine::Script &script)
{
    QWebEngineScript engineScript;
    engineScript.setName(script.name);
    engineScript.setSourceCode(script.sourceCode);
    engineScript.setInjectionPoint(webEngineInjectionPoint(script.injectionPoint));
    engineScript.setWorldId(script.worldId);
    engineScript.setRunsOnSubFrames(script.runsOnSubFrames);
    m_profile->scripts()->insert(engineScript);
}

void WebEngineProfileAdapter::removeScript(const QString &name)
{
    const QList<QWebEngineScript> matches = m_profile->scripts()->find(name);
    for (const QWebEngineScript &script : matches)
        m_profile->scripts()->remove(script);
}

QList<Engine::Script> WebEngineProfileAdapter::scripts() const
{
    QList<Engine::Script> out;
    const QList<QWebEngineScript> list = m_profile->scripts()->toList();
    out.reserve(list.size());
    for (const QWebEngineScript &script : list) {
        Engine::Script translated;
        translated.name = script.name();
        translated.sourceCode = script.sourceCode();
        translated.injectionPoint = engineInjectionPoint(script.injectionPoint());
        translated.worldId = script.worldId();
        translated.runsOnSubFrames = script.runsOnSubFrames();
        out.append(translated);
    }
    return out;
}

void WebEngineProfileAdapter::clear(Engine::StorageAreas areas)
{
    if (areas & Engine::HttpCacheArea)
        m_profile->clearHttpCache();
    if (areas & Engine::CookiesArea)
        m_profile->cookieStore()->deleteAllCookies();
    if (areas & Engine::VisitedLinksArea)
        m_profile->clearAllVisitedLinks();
    // DomStorage/ServiceWorker have no per-area runtime clear on
    // QtWebEngine — the app's wipe path goes through the profile's
    // data directories and keeps doing so until the boundary grows a
    // backend-specific answer.
}

void WebEngineProfileAdapter::setProfileAttribute(const QString &name,
                                                  const QVariant &value)
{
    // The settings-apply path still writes QWebEngineSettings directly;
    // this bag keeps backend-neutral keys for when that touchpoint
    // group migrates onto the interface.
    m_attributes.insert(name, value);
}

// ---------------------------------------------------------------------------

WebEngineBackend::WebEngineBackend(QObject *parent)
    : Engine::Backend(parent)
{
}

WebEngineBackend *WebEngineBackend::instance()
{
    // App-lifetime singleton — created on first use, never destroyed
    // (same contract as the other process singletons).
    static WebEngineBackend *backend = nullptr;
    if (!backend)
        backend = new WebEngineBackend;
    return backend;
}

QString WebEngineBackend::id() const
{
    return QStringLiteral("webengine");
}

QString WebEngineBackend::displayName() const
{
    return QStringLiteral("Chromium (QtWebEngine %1)")
            .arg(QString::fromLatin1(qWebEngineChromiumVersion()));
}

Engine::Capabilities WebEngineBackend::capabilities() const
{
    Engine::Capabilities caps;
    caps.requestInterception = true;
    caps.scriptInjection = true;
    caps.scriptEvaluation = true;
    caps.cookieFilter = true;
    caps.perPageSettings = true;
    caps.lifecycleDiscard = true;
    caps.contextMenuInfo = true;
    caps.certificateOverride = true;
    caps.devTools = true;
    caps.extensions = true;
    caps.downloads = true;
    return caps;
}

bool WebEngineBackend::initialize()
{
    // QtWebEngine initializes lazily on first use; the pre-QApplication
    // work main() does today (scheme registration, chromium flags, dns
    // mode) moves behind this hook when that touchpoint group lands.
    return true;
}

Engine::Profile *WebEngineBackend::createProfile(const Engine::ProfileOptions &options,
                                                 QObject *parent)
{
    // storageName empty + offTheRecord = unnamed OTR profile; storageName
    // empty + persistent = the shared default profile (not ours to own).
    QWebEngineProfile *profile = nullptr;
    bool owned = false;
    if (options.storageName.isEmpty() && !options.offTheRecord) {
        profile = QWebEngineProfile::defaultProfile();
    } else {
        profile = options.offTheRecord && options.storageName.isEmpty()
                ? new QWebEngineProfile
                : new QWebEngineProfile(options.storageName);
        owned = true;
        if (!options.dataPath.isEmpty())
            profile->setPersistentStoragePath(options.dataPath);
        if (!options.cachePath.isEmpty())
            profile->setCachePath(options.cachePath);
    }
    auto *adapter = new WebEngineProfileAdapter(profile, parent);
    if (owned)
        profile->setParent(adapter);
    return adapter;
}

Engine::Page *WebEngineBackend::createPage(Engine::Profile *profile, QObject *parent)
{
    auto *profileAdapter = qobject_cast<WebEngineProfileAdapter*>(profile);
    QWebEngineProfile *engineProfile = profileAdapter
            ? profileAdapter->webEngineProfile()
            : QWebEngineProfile::defaultProfile();
    auto *page = new QWebEnginePage(engineProfile);
    auto *adapter = new WebEnginePageAdapter(page, parent);
    page->setParent(adapter);
    return adapter;
}

QWidget *WebEngineBackend::createView(Engine::Page *page, QWidget *parent)
{
    auto *pageAdapter = qobject_cast<WebEnginePageAdapter*>(page);
    if (!pageAdapter)
        return nullptr;
    auto *view = new QWebEngineView(parent);
    view->setPage(pageAdapter->webEnginePage());
    return view;
}
