/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "extensionrequestwatcher.h"

#include "browsermainwindow.h"
#include "chromecommon.h"
#include "extensionbackgroundhost.h"
#include "extensioneventhub.h"
#include "extensionshimbackend.h"
#include "webview.h"

#include <qatomic.h>
#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qjsonarray.h>
#include <qmetatype.h>
#include <qregularexpression.h>
#include <qset.h>
#include <qtimer.h>
#include <qwebengineurlrequestinfo.h>

namespace {

// Zero-subscriber fast path for the IO thread — subscriptions are
// created and counted on the GUI thread before any observation can
// be wanted, so these atomics are the only shared state.
QAtomicInt s_watching;
QAtomicInt s_wantRequestHeaders;
ExtensionRequestWatcher *s_instance = nullptr;

// Qt resource type -> Chrome webRequest ResourceType name.
QString chromeTypeName(int resourceType)
{
    using RT = QWebEngineUrlRequestInfo::ResourceType;
    switch (resourceType) {
    case RT::ResourceTypeMainFrame:
    case RT::ResourceTypeNavigationPreloadMainFrame:
        return QStringLiteral("main_frame");
    case RT::ResourceTypeSubFrame:
    case RT::ResourceTypeNavigationPreloadSubFrame:
        return QStringLiteral("sub_frame");
    case RT::ResourceTypeStylesheet:
        return QStringLiteral("stylesheet");
    case RT::ResourceTypeScript:
    case RT::ResourceTypeJson:
        return QStringLiteral("script");
    case RT::ResourceTypeImage:
    case RT::ResourceTypeFavicon:
        return QStringLiteral("image");
    case RT::ResourceTypeFontResource:
        return QStringLiteral("font");
    case RT::ResourceTypeObject:
    case RT::ResourceTypePluginResource:
        return QStringLiteral("object");
    case RT::ResourceTypeMedia:
        return QStringLiteral("media");
    case RT::ResourceTypeXhr:
        return QStringLiteral("xmlhttprequest");
    case RT::ResourceTypePing:
        return QStringLiteral("ping");
    case RT::ResourceTypeCspReport:
        return QStringLiteral("csp_report");
    case RT::ResourceTypeWebSocket:
        return QStringLiteral("websocket");
    default:
        return QStringLiteral("other");
    }
}

// The set Chrome's RequestFilter types[] accepts — kept explicit so
// a typo'd type fails at registration instead of silently matching
// nothing.
bool knownChromeType(const QString &type)
{
    static const QSet<QString> known = {
        QStringLiteral("main_frame"), QStringLiteral("sub_frame"),
        QStringLiteral("stylesheet"), QStringLiteral("script"),
        QStringLiteral("image"), QStringLiteral("font"),
        QStringLiteral("object"), QStringLiteral("xmlhttprequest"),
        QStringLiteral("ping"), QStringLiteral("csp_report"),
        QStringLiteral("media"), QStringLiteral("websocket"),
        QStringLiteral("webbundle"), QStringLiteral("webtransport"),
        QStringLiteral("webidentity"), QStringLiteral("other")
    };
    return known.contains(type);
}

// Syntactic validity of a match pattern — Chrome rejects a bad one
// at addListener time.
bool validPattern(const QString &pattern)
{
    if (pattern == QLatin1String("<all_urls>"))
        return true;
    const int sep = pattern.indexOf(QLatin1String("://"));
    if (sep < 0)
        return false;
    const QString rest = pattern.mid(sep + 3);
    return !rest.isEmpty();
}

QJsonArray headerJson(const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QJsonArray out;
    for (const auto &h : headers)
        out.append(QJsonObject{
            {QStringLiteral("name"), QString::fromUtf8(h.first)},
            {QStringLiteral("value"), QString::fromUtf8(h.second)}});
    return out;
}

QJsonArray headerJson(const QMultiMap<QByteArray, QByteArray> &headers)
{
    QList<QPair<QByteArray, QByteArray>> list;
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it)
        list.append(qMakePair(it.key(), it.value()));
    return headerJson(list);
}

// Chromium net error codes -> "net::ERR_*" — the same table the
// hub's webNavigation.onErrorOccurred answers with.
QString netErrorName(int code)
{
    switch (code) {
    case -2:   return QStringLiteral("ERR_FAILED");
    case -3:   return QStringLiteral("ERR_ABORTED");
    case -6:   return QStringLiteral("ERR_FILE_NOT_FOUND");
    case -7:   return QStringLiteral("ERR_TIMED_OUT");
    case -100: return QStringLiteral("ERR_CONNECTION_CLOSED");
    case -101: return QStringLiteral("ERR_CONNECTION_RESET");
    case -102: return QStringLiteral("ERR_CONNECTION_REFUSED");
    case -103: return QStringLiteral("ERR_CONNECTION_ABORTED");
    case -104: return QStringLiteral("ERR_CONNECTION_FAILED");
    case -105: return QStringLiteral("ERR_NAME_NOT_RESOLVED");
    case -106: return QStringLiteral("ERR_INTERNET_DISCONNECTED");
    case -117: return QStringLiteral("ERR_CONNECTION_TIMED_OUT");
    case -118: return QStringLiteral("ERR_CONNECTION_TIMED_OUT");
    case -130: return QStringLiteral("ERR_PROXY_CONNECTION_FAILED");
    case -137: return QStringLiteral("ERR_NAME_RESOLUTION_FAILED");
    case -138: return QStringLiteral("ERR_NETWORK_ACCESS_DENIED");
    case -139: return QStringLiteral("ERR_NETWORK_CHANGED");
    case -199: return QStringLiteral("ERR_CERT_COMMON_NAME_INVALID");
    case -200: return QStringLiteral("ERR_CERT_DATE_INVALID");
    case -300: return QStringLiteral("ERR_INVALID_URL");
    case -324: return QStringLiteral("ERR_EMPTY_RESPONSE");
    default:   return QStringLiteral("ERR_FAILED");
    }
}

} // namespace

ExtensionRequestWatcher::ExtensionRequestWatcher(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<QJsonObject>();
    s_instance = this;
}

ExtensionRequestWatcher *ExtensionRequestWatcher::instance()
{
    if (!s_instance)
        new ExtensionRequestWatcher(qApp);
    return s_instance;
}

void ExtensionRequestWatcher::observeRequest(
        const QWebEngineUrlRequestInfo &info)
{
    if (s_watching.loadAcquire() <= 0 || !s_instance)
        return;
    // Snapshot on the IO thread — the info reference dies with the
    // call.  Header capture is gated on somebody actually asking for
    // them ('requestHeaders'/'extraHeaders').
    QJsonObject snap;
    snap.insert(QStringLiteral("url"), info.requestUrl().toString());
    snap.insert(QStringLiteral("method"),
                QString::fromLatin1(info.requestMethod()));
    snap.insert(QStringLiteral("type"), int(info.resourceType()));
    snap.insert(QStringLiteral("firstParty"),
                info.firstPartyUrl().toString());
    snap.insert(QStringLiteral("initiator"),
                info.initiator().toString());
    snap.insert(QStringLiteral("download"), info.isDownload());
    snap.insert(QStringLiteral("ts"),
                double(QDateTime::currentMSecsSinceEpoch()));
    if (s_wantRequestHeaders.loadAcquire() > 0) {
        QList<QPair<QByteArray, QByteArray>> headers;
        const QHash<QByteArray, QByteArray> raw = info.httpHeaders();
        for (auto it = raw.constBegin(); it != raw.constEnd(); ++it)
            headers.append(qMakePair(it.key(), it.value()));
        snap.insert(QStringLiteral("headers"), headerJson(headers));
    }
    QMetaObject::invokeMethod(s_instance, "requestStarted",
                              Qt::QueuedConnection,
                              Q_ARG(QJsonObject, snap));
}

void ExtensionRequestWatcher::observeBlocked(
        const QWebEngineUrlRequestInfo &info)
{
    if (s_watching.loadAcquire() <= 0 || !s_instance)
        return;
    QJsonObject snap;
    snap.insert(QStringLiteral("url"), info.requestUrl().toString());
    snap.insert(QStringLiteral("firstParty"),
                info.firstPartyUrl().toString());
    snap.insert(QStringLiteral("type"), int(info.resourceType()));
    QMetaObject::invokeMethod(s_instance, "requestBlockedNow",
                              Qt::QueuedConnection,
                              Q_ARG(QJsonObject, snap));
}

// -------------------------------------------------------------------
// Registration
// -------------------------------------------------------------------

QJsonObject ExtensionRequestWatcher::subscribe(
        QObject *context, const QString &extensionId,
        const QString &registrationId,
        const QString &event, const QJsonObject &filter,
        const QJsonArray &extraInfoSpec)
{
    auto ok = [](const QJsonValue &v = QJsonValue()) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), true);
        if (!v.isUndefined())
            o.insert(QStringLiteral("result"), v);
        return o;
    };
    auto err = [](const QString &code) {
        QJsonObject o;
        o.insert(QStringLiteral("ok"), false);
        o.insert(QStringLiteral("error"), code);
        return o;
    };

    static const QSet<QString> servable = {
        QStringLiteral("onBeforeRequest"),
        QStringLiteral("onBeforeSendHeaders"),
        QStringLiteral("onSendHeaders"),
        QStringLiteral("onHeadersReceived"),
        QStringLiteral("onCompleted"),
        QStringLiteral("onErrorOccurred"),
    };
    static const QSet<QString> unservable = {
        // No observation source exists — the interceptor sees the
        // request once, Chromium answers redirects/auth internally,
        // and no signal marks "response bytes started".  A listener
        // here is a real functional gap, so it must not register.
        QStringLiteral("onBeforeRedirect"),
        QStringLiteral("onResponseStarted"),
        QStringLiteral("onAuthRequired"),
    };
    if (unservable.contains(event))
        return err(QStringLiteral("unsupported"));
    if (!servable.contains(event))
        return err(QStringLiteral("invalid-argument"));

    const QJsonArray urls = filter.value(QLatin1String("urls")).toArray();
    QStringList urlList;
    for (const QJsonValue &v : urls) {
        const QString pattern = v.toString();
        if (!validPattern(pattern))
            return err(QStringLiteral("invalid-argument"));
        urlList.append(pattern);
    }
    if (urlList.isEmpty())
        return err(QStringLiteral("invalid-argument"));

    QStringList types;
    for (const QJsonValue &v :
             filter.value(QLatin1String("types")).toArray()) {
        const QString type = v.toString();
        if (!knownChromeType(type))
            return err(QStringLiteral("invalid-argument"));
        types.append(type);
    }

    Registration reg;
    reg.context = context;
    reg.event = event;
    reg.urls = urlList;
    reg.types = types;
    reg.tabId = int(filter.value(QLatin1String("tabId")).toDouble(-1));
    reg.windowId =
        int(filter.value(QLatin1String("windowId")).toDouble(-1));

    // extraInfoSpec — what the listener asks the details to carry or
    // the pipeline to allow.  'requestHeaders'/'responseHeaders' we
    // serve where the engine reports them; 'extraHeaders' folds into
    // the same best-effort header set (documented approximation);
    // 'blocking'/'requestBody' can never be honored — the request
    // is registered anyway (Chrome MV3 registers it non-blocking)
    // and the caller still gets the typed 'unsupported' reply.
    bool unsupportedSpec = false;
    for (const QJsonValue &v : extraInfoSpec) {
        const QString spec = v.toString();
        if (spec == QLatin1String("blocking")
                || spec == QLatin1String("requestBody")) {
            unsupportedSpec = true;
        } else if (spec == QLatin1String("requestHeaders")
                   || spec == QLatin1String("extraHeaders")) {
            reg.wantRequestHeaders = true;
            if (event == QLatin1String("onHeadersReceived")
                    || event == QLatin1String("onCompleted")
                    || event == QLatin1String("onResponseStarted"))
                reg.wantResponseHeaders = true;
        } else if (spec == QLatin1String("responseHeaders")) {
            reg.wantResponseHeaders = true;
        }
    }

    const QString regId = registrationId;
    QHash<QString, Registration> &extRegs = m_regs[extensionId];
    if (!extRegs.contains(regId))
        ++m_subscriberCount;
    extRegs.insert(regId, reg);
    if (context)
        connect(context, &QObject::destroyed, this,
                [this, context]() { dropContext(context); });
    s_watching.storeRelease(m_subscriberCount);
    if (reg.wantRequestHeaders)
        s_wantRequestHeaders.storeRelease(1);
    // The first subscription arms the subresource-completion probes
    // on already-tracked pages.
    ExtensionEventHub::instance()->armRequestProbes();

    if (unsupportedSpec)
        return err(QStringLiteral("unsupported"));
    QJsonObject result;
    if (event == QLatin1String("onHeadersReceived")
            || event == QLatin1String("onCompleted")
            || event == QLatin1String("onErrorOccurred")) {
        // Honest fidelity note, surfaced through shim notes: the
        // engine reports responseHeaders for main-frame commits
        // only, statusCode is never exposed, and subresource
        // completion/failure rides the page resource-timing probe
        // (fetch() rejections leave no trace).
        result.insert(QStringLiteral("note"), QStringLiteral(
            "webRequest response details: headers main-frame only, "
            "no statusCode; subresource completion via resource "
            "timing, fetch failures unobservable"));
    }
    return ok(result);
}

QJsonObject ExtensionRequestWatcher::unsubscribe(
        const QString &extensionId, const QString &registrationId)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    auto it = m_regs.find(extensionId);
    if (it == m_regs.end())
        return o;
    if (it->remove(registrationId)) {
        --m_subscriberCount;
        s_watching.storeRelease(m_subscriberCount);
        if (it->isEmpty())
            m_regs.erase(it);
    }
    return o;
}

void ExtensionRequestWatcher::dropContext(QObject *context)
{
    for (auto eit = m_regs.begin(); eit != m_regs.end();) {
        for (auto rit = eit->begin(); rit != eit->end();) {
            if (rit->context == context) {
                rit = eit->erase(rit);
                --m_subscriberCount;
            } else {
                ++rit;
            }
        }
        if (eit->isEmpty())
            eit = m_regs.erase(eit);
        else
            ++eit;
    }
    s_watching.storeRelease(m_subscriberCount);
}

void ExtensionRequestWatcher::dropExtension(const QString &extensionId)
{
    auto it = m_regs.find(extensionId);
    if (it == m_regs.end())
        return;
    m_subscriberCount -= it->size();
    m_regs.erase(it);
    s_watching.storeRelease(m_subscriberCount);
}

int ExtensionRequestWatcher::registrationCount(
        const QString &extensionId) const
{
    return m_regs.value(extensionId).size();
}

// -------------------------------------------------------------------
// Dispatch
// -------------------------------------------------------------------

bool ExtensionRequestWatcher::filterMatches(
        const Registration &reg, const QString &url,
        const QString &chromeType, int tabId, int windowId) const
{
    if (reg.tabId >= 0 && reg.tabId != tabId)
        return false;
    if (reg.windowId >= 0 && reg.windowId != windowId)
        return false;
    if (!reg.types.isEmpty() && !reg.types.contains(chromeType))
        return false;
    const QUrl qurl(url);
    for (const QString &pattern : reg.urls) {
        if (matchPatternCovers(pattern, qurl))
            return true;
    }
    return false;
}

QJsonObject ExtensionRequestWatcher::detailsFor(
        const InFlight &request) const
{
    QJsonObject details;
    details.insert(QStringLiteral("requestId"), request.requestId);
    details.insert(QStringLiteral("url"), request.url);
    details.insert(QStringLiteral("method"), request.method);
    details.insert(QStringLiteral("type"), request.chromeType);
    details.insert(QStringLiteral("tabId"), request.tabId);
    details.insert(QStringLiteral("frameId"), request.frameId);
    details.insert(QStringLiteral("parentFrameId"),
                   request.parentFrameId);
    details.insert(QStringLiteral("timeStamp"),
                   double(request.startedMs));
    if (!request.initiator.isEmpty())
        details.insert(QStringLiteral("initiator"),
                       request.initiator);
    if (request.isDownload)
        details.insert(QStringLiteral("isDownload"), true);
    return details;
}

void ExtensionRequestWatcher::emitFor(
        const QString &event, const InFlight &request,
        const QJsonObject &extra)
{
    if (m_subscriberCount <= 0)
        return;
    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    const QString name =
        QStringLiteral("webRequest.") + event;
    for (auto eit = m_regs.constBegin(); eit != m_regs.constEnd();
         ++eit) {
        const QString &extId = eit.key();
        bool anyMatch = false;
        bool wantReqHeaders = false;
        bool wantRespHeaders = false;
        for (auto rit = eit->constBegin(); rit != eit->constEnd();
             ++rit) {
            const Registration &reg = rit.value();
            if (reg.event != event
                    || !filterMatches(reg, request.url,
                                      request.chromeType,
                                      request.tabId, request.windowId))
                continue;
            anyMatch = true;
            wantReqHeaders |= reg.wantRequestHeaders;
            wantRespHeaders |= reg.wantResponseHeaders;
        }
        if (!anyMatch)
            continue;
        // Host-permission scoping — the request reaches this
        // extension only when its host_permissions cover the URL.
        if (!runtime->requestVisibleTo(extId, QUrl(request.url)))
            continue;
        QJsonObject details = detailsFor(request);
        for (auto it = extra.constBegin(); it != extra.constEnd();
             ++it) {
            // Header arrays are per-listener grants — attach only
            // when a matching registration asked for them.
            if (it.key() == QLatin1String("requestHeaders")
                    && !wantReqHeaders)
                continue;
            if (it.key() == QLatin1String("responseHeaders")
                    && !wantRespHeaders)
                continue;
            details.insert(it.key(), it.value());
        }
        runtime->deliverEvent(extId, name, details);
    }
}

void ExtensionRequestWatcher::openRequest(
        const QJsonObject &snapshot, InFlight *out)
{
    purgeExpired();

    InFlight r;
    r.requestId = QString::number(++m_nextRequestId);
    r.url = snapshot.value(QLatin1String("url")).toString();
    r.method = snapshot.value(QLatin1String("method")).toString();
    const int rt = snapshot.value(QLatin1String("type")).toInt();
    r.chromeType = chromeTypeName(rt);
    r.isDownload =
        snapshot.value(QLatin1String("download")).toBool();
    r.startedMs =
        qint64(snapshot.value(QLatin1String("ts")).toDouble());
    const QString initiator =
        snapshot.value(QLatin1String("initiator")).toString();
    const QUrl iurl(initiator);
    if (iurl.isValid()
            && (iurl.scheme() == QLatin1String("http")
                || iurl.scheme() == QLatin1String("https")
                || iurl.scheme() == QLatin1String("chrome-extension")
                || iurl.scheme() == QLatin1String("file")))
        r.initiator = iurl.scheme() == QLatin1String("chrome-extension")
            ? initiator
            : iurl.scheme() + QStringLiteral("://") + iurl.host()
                + (iurl.port() > 0
                   ? QStringLiteral(":") + QString::number(iurl.port())
                   : QString());

    // Tab attribution: the hub resolves the tracked view the request
    // belongs to — navigations match the pending/committed url,
    // everything else matches the first-party (document) url.  An
    // untracked origin stays Chrome's tabId -1.
    const bool mainFrame = r.chromeType == QLatin1String("main_frame");
    WebView *view = ExtensionEventHub::instance()->viewForRequest(
        QUrl(r.url), mainFrame,
        QUrl(snapshot.value(QLatin1String("firstParty")).toString()));
    if (view) {
        r.tabId = ExtensionShimBackend::tabIdFor(view);
        r.windowId = ExtensionShimBackend::windowIdFor(
            BrowserMainWindow::parentWindow(view));
        // frameId 0 = main frame.  A sub_frame request's own frame is
        // not resolvable from the interceptor — Chrome would name the
        // real frame; -1 (unassociated) is the honest answer, with
        // parentFrameId 0 since the first party IS the main document.
        r.frameId = 0;
        r.parentFrameId = -1;
        if (r.chromeType == QLatin1String("sub_frame")) {
            r.frameId = -1;
            r.parentFrameId = 0;
        }
    }

    // A commit signal that beat this queued start — absorb the
    // headers; requestStarted emits onHeadersReceived in order.  An
    // orphan record (tabId -1, attribution lost the same race the
    // buffer covers) claims its commit by url and adopts the key.
    if (r.chromeType == QLatin1String("main_frame")) {
        int pcKey = r.tabId;
        if (m_pendingCommit.value(pcKey).url != r.url
                && r.tabId == -1) {
            for (auto it = m_pendingCommit.constBegin();
                 it != m_pendingCommit.constEnd(); ++it) {
                if (it->url == r.url) {
                    pcKey = it.key();
                    break;
                }
            }
        }
        const PendingCommit pc = m_pendingCommit.value(pcKey);
        if (pc.url == r.url) {
            r.tabId = pcKey;
            r.responseHeaders = pc.responseHeaders;
            r.commitEarly = true;
            r.committed = true;
            m_pendingCommit.remove(pcKey);
        }
    }

    m_inFlight.insert(r.requestId, r);
    m_byTabUrl[r.tabId][r.url].append(r.requestId);
    if (out)
        *out = r;
}

bool ExtensionRequestWatcher::takeInFlight(int tabId,
                                           const QString &url,
                                           InFlight *out)
{
    auto tit = m_byTabUrl.find(tabId);
    if (tit == m_byTabUrl.end())
        return false;
    auto uit = tit->find(url);
    if (uit == tit->end() || uit->isEmpty())
        return false;
    const QString id = uit->takeFirst();
    if (uit->isEmpty())
        tit->erase(uit);
    auto iit = m_inFlight.find(id);
    if (iit == m_inFlight.end())
        return false;
    if (out)
        *out = iit.value();
    m_inFlight.erase(iit);
    return true;
}

void ExtensionRequestWatcher::purgeExpired()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    // Completions that never pair — subframes, originless pages,
    // prefetches — must not pin the ledger.  Five minutes is far
    // past any real request's lifetime.
    for (auto it = m_inFlight.begin(); it != m_inFlight.end();) {
        if (now - it->startedMs > 5 * 60 * 1000) {
            auto tit = m_byTabUrl.find(it->tabId);
            if (tit != m_byTabUrl.end()) {
                auto uit = tit->find(it->url);
                if (uit != tit->end()) {
                    uit->removeAll(it->requestId);
                    if (uit->isEmpty())
                        tit->erase(uit);
                }
            }
            it = m_inFlight.erase(it);
        } else {
            ++it;
        }
    }
    // Hard bound — purge only ages out; a busy page could still
    // outgrow the map between calls.
    while (m_inFlight.size() > 4096) {
        auto it = m_inFlight.begin();
        auto tit = m_byTabUrl.find(it->tabId);
        if (tit != m_byTabUrl.end()) {
            auto uit = tit->find(it->url);
            if (uit != tit->end())
                uit->removeAll(it->requestId);
        }
        m_inFlight.erase(it);
    }
}

void ExtensionRequestWatcher::requestStarted(const QJsonObject &snapshot)
{
    InFlight r;
    openRequest(snapshot, &r);
    if (r.requestId.isEmpty())
        return;
    emitFor(QStringLiteral("onBeforeRequest"), r);
    // onBeforeSendHeaders and onSendHeaders share the single
    // interception point — the engine calls it once, immediately
    // before the request leaves (Chrome's own ordering).  The
    // headers array reaches only registrations that asked.
    QJsonObject extra;
    if (snapshot.contains(QLatin1String("headers"))) {
        extra.insert(QStringLiteral("requestHeaders"),
                     snapshot.value(QLatin1String("headers")));
        emitFor(QStringLiteral("onBeforeSendHeaders"), r, extra);
        emitFor(QStringLiteral("onSendHeaders"), r, extra);
    } else {
        emitFor(QStringLiteral("onBeforeSendHeaders"), r);
        emitFor(QStringLiteral("onSendHeaders"), r);
    }
    // Drain the milestones that raced ahead of this queued start.
    if (r.commitEarly)
        emitFor(QStringLiteral("onHeadersReceived"), r,
                QJsonObject{{QStringLiteral("responseHeaders"),
                             r.responseHeaders}});
    // A failure/completion milestone that raced ahead — the buffers
    // are tab-keyed, but an orphan record (attribution lost the same
    // race) claims its milestone by url and adopts the key as tab.
    int pfKey = r.tabId;
    if (m_pendingFail.value(pfKey).url != r.url && r.tabId == -1) {
        for (auto it = m_pendingFail.constBegin();
             it != m_pendingFail.constEnd(); ++it) {
            if (it->url == r.url) {
                pfKey = it.key();
                break;
            }
        }
    }
    const PendingFail pf = m_pendingFail.value(pfKey);
    if (pf.url == r.url) {
        m_pendingFail.remove(pfKey);
        InFlight dropped;
        takeInFlight(r.tabId, r.url, &dropped);
        if (dropped.requestId.isEmpty())
            dropped = r;
        else
            retire(dropped);
        dropped.tabId = pfKey;
        QJsonObject fe{
            {QStringLiteral("error"),
             QStringLiteral("net::") + netErrorName(pf.errorCode)},
            {QStringLiteral("errorCode"), pf.errorCode}};
        if (!pf.errorString.isEmpty())
            fe.insert(QStringLiteral("errorDetail"), pf.errorString);
        emitFor(QStringLiteral("onErrorOccurred"), dropped, fe);
        return;
    }
    int pdKey = r.tabId;
    if (m_pendingDone.value(pdKey) != r.url && r.tabId == -1) {
        for (auto it = m_pendingDone.constBegin();
             it != m_pendingDone.constEnd(); ++it) {
            if (it.value() == r.url) {
                pdKey = it.key();
                break;
            }
        }
    }
    if (m_pendingDone.value(pdKey) == r.url) {
        m_pendingDone.remove(pdKey);
        InFlight done;
        takeInFlight(r.tabId, r.url, &done);
        if (done.requestId.isEmpty())
            done = r;
        else
            retire(done);
        done.tabId = pdKey;
        emitFor(QStringLiteral("onCompleted"), done,
                done.responseHeaders.isEmpty()
                    ? QJsonObject()
                    : QJsonObject{{QStringLiteral("responseHeaders"),
                                   done.responseHeaders}});
    }
}

void ExtensionRequestWatcher::requestBlockedNow(
        const QJsonObject &snapshot)
{
    const QString url =
        snapshot.value(QLatin1String("url")).toString();
    const QString firstParty =
        snapshot.value(QLatin1String("firstParty")).toString();
    const bool mainFrame =
        chromeTypeName(snapshot.value(QLatin1String("type")).toInt())
            == QLatin1String("main_frame");
    WebView *view = ExtensionEventHub::instance()->viewForRequest(
        QUrl(url), mainFrame, QUrl(firstParty));
    const int tabId = view ? ExtensionShimBackend::tabIdFor(view) : -1;
    InFlight r;
    if (!takeInFlight(tabId, url, &r)) {
        // The start was never announced (pre-subscription traffic) —
        // no error event either; Chrome wouldn't split the pair.
        return;
    }
    emitFor(QStringLiteral("onErrorOccurred"), r,
            QJsonObject{{QStringLiteral("error"),
                         QStringLiteral("net::ERR_BLOCKED_BY_CLIENT")}});
}

// -------------------------------------------------------------------
// Navigation/probe completion feeds
// -------------------------------------------------------------------

ExtensionRequestWatcher::InFlight *
ExtensionRequestWatcher::findRecord(int tabId, const QString &url)
{
    auto uit = m_byTabUrl.find(tabId);
    if (uit == m_byTabUrl.end())
        return nullptr;
    auto lit = uit->find(url);
    if (lit == uit->end())
        return nullptr;
    for (const QString &id : *lit) {
        auto iit = m_inFlight.find(id);
        if (iit != m_inFlight.end())
            return &*iit;
    }
    return nullptr;
}

ExtensionRequestWatcher::InFlight *
ExtensionRequestWatcher::findOrphan(const QString &url,
                                    bool mainFrameOnly)
{
    for (auto it = m_inFlight.begin(); it != m_inFlight.end(); ++it) {
        if (it->tabId == -1 && it->url == url
                && (!mainFrameOnly
                    || it->chromeType
                           == QLatin1String("main_frame")))
            return &*it;
    }
    return nullptr;
}

void ExtensionRequestWatcher::adoptTab(InFlight *rec, int tabId,
                                       int windowId)
{
    if (!rec || rec->tabId == tabId)
        return;
    auto tit = m_byTabUrl.find(rec->tabId);
    if (tit != m_byTabUrl.end()) {
        auto uit = tit->find(rec->url);
        if (uit != tit->end()) {
            uit->removeAll(rec->requestId);
            if (uit->isEmpty())
                tit->erase(uit);
        }
    }
    rec->tabId = tabId;
    rec->windowId = windowId;
    m_byTabUrl[tabId][rec->url].append(rec->requestId);
}

void ExtensionRequestWatcher::popRecord(const QString &requestId)
{
    auto it = m_inFlight.find(requestId);
    if (it == m_inFlight.end())
        return;
    auto tit = m_byTabUrl.find(it->tabId);
    if (tit != m_byTabUrl.end()) {
        auto uit = tit->find(it->url);
        if (uit != tit->end()) {
            uit->removeAll(requestId);
            if (uit->isEmpty())
                tit->erase(uit);
        }
    }
    m_inFlight.erase(it);
}

void ExtensionRequestWatcher::retire(const InFlight &request)
{
    QStringList &list = m_retired[request.tabId];
    list.append(request.url);
    while (list.size() > 8)
        list.removeFirst();
}

bool ExtensionRequestWatcher::consumeRetired(int tabId,
                                             const QString &url)
{
    auto it = m_retired.find(tabId);
    if (it == m_retired.end())
        return false;
    const int idx = it->indexOf(url);
    if (idx < 0)
        return false;
    it->removeAt(idx);
    if (it->isEmpty())
        m_retired.erase(it);
    return true;
}

void ExtensionRequestWatcher::finishMainFrame(InFlight *match)
{
    InFlight r = *match;
    popRecord(r.requestId);
    retire(r);
    emitFor(QStringLiteral("onCompleted"), r,
            r.responseHeaders.isEmpty()
                ? QJsonObject()
                : QJsonObject{{QStringLiteral("responseHeaders"),
                               r.responseHeaders}});
}

void ExtensionRequestWatcher::mainFrameCommitted(
        WebView *view, const QUrl &url,
        const QMultiMap<QByteArray, QByteArray> &responseHeaders)
{
    if (!view)
        return;
    const QJsonArray headers = headerJson(responseHeaders);
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    // The committing navigation's request — match by url first, and
    // when that misses (aboutToLoadUrl reported a different final
    // url) fall back to the tab's oldest in-flight main_frame.
    InFlight *match = findRecord(tabId, url.toString());
    if (!match
            || match->chromeType != QLatin1String("main_frame")) {
        match = nullptr;
        for (auto rit = m_inFlight.begin(); rit != m_inFlight.end();
             ++rit) {
            if (rit->tabId == tabId
                    && rit->chromeType
                           == QLatin1String("main_frame")) {
                match = &*rit;
                break;
            }
        }
    }
    if (!match) {
        // The navigation's start is parked under tabId -1 — its
        // queued requestStarted ran before the tab announced the
        // pending url.  Claim it; the commit names the true tab.
        match = findOrphan(url.toString(), true);
        if (match)
            adoptTab(match, tabId,
                     ExtensionShimBackend::windowIdFor(
                         BrowserMainWindow::parentWindow(view)));
    }
    if (!match) {
        if (consumeRetired(tabId, url.toString()))
            // A duplicate status pass for a navigation already
            // settled — not a racing start.
            return;
        // The committed navigation's requestStarted is still queued
        // behind this signal — buffer the commit for openRequest.
        m_pendingCommit.insert(
            tabId, PendingCommit{url.toString(), headers});
        return;
    }
    if (match->committed)
        return;
    match->committed = true;
    // The real response headers — served only to registrations that
    // asked; statusCode stays honestly absent (the engine never
    // exposes it).  Kept on the record so the later onCompleted can
    // re-serve them.
    if (!headers.isEmpty())
        match->responseHeaders = headers;
    emitFor(QStringLiteral("onHeadersReceived"), *match,
            headers.isEmpty()
                ? QJsonObject()
                : QJsonObject{{QStringLiteral("responseHeaders"),
                               headers}});
    if (match->completedPending)
        // loadFinished ran first (the engine reports it before the
        // succeeded-status info) — this commit is the second
        // milestone, so it owes the record its onCompleted.
        finishMainFrame(match);
}

void ExtensionRequestWatcher::mainFrameCompleted(WebView *view,
                                                 const QUrl &url)
{
    if (!view)
        return;
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    InFlight *match = findRecord(tabId, url.toString());
    if (!match
            || match->chromeType != QLatin1String("main_frame")) {
        // The committed url may differ from the announced target —
        // fall back to the tab's oldest main_frame record.
        match = nullptr;
        for (auto it = m_inFlight.begin(); it != m_inFlight.end();
             ++it) {
            if (it->tabId == tabId
                    && it->chromeType
                           == QLatin1String("main_frame")) {
                match = &*it;
                break;
            }
        }
    }
    if (!match) {
        // Same orphan adoption as the commit feed — the finished
        // navigation's start may be parked under tabId -1.
        match = findOrphan(url.toString(), true);
        if (match)
            adoptTab(match, tabId,
                     ExtensionShimBackend::windowIdFor(
                         BrowserMainWindow::parentWindow(view)));
    }
    if (!match) {
        if (consumeRetired(tabId, url.toString()))
            return;
        // Completion before the queued start — buffer it the
        // same way.
        m_pendingDone.insert(tabId, url.toString());
        return;
    }
    if (match->committed) {
        // The commit already served onHeadersReceived — the
        // document's load is the second milestone; emit and settle.
        finishMainFrame(match);
        return;
    }
    // No commit yet — mark it owed and arm a bounded fallback so a
    // load whose succeeded-status never arrives (cached or
    // non-network commit) still answers onCompleted, honestly
    // without responseHeaders.
    match->completedPending = true;
    const QString requestId = match->requestId;
    QTimer::singleShot(400, this, [this, requestId]() {
        auto it = m_inFlight.find(requestId);
        if (it == m_inFlight.end() || it->committed)
            return;
        finishMainFrame(&*it);
    });
}

void ExtensionRequestWatcher::mainFrameFailed(WebView *view,
                                              const QUrl &url,
                                              int errorCode,
                                              const QString &errorString)
{
    if (!view)
        return;
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    InFlight r;
    if (!takeInFlight(tabId, url.toString(), &r)) {
        // The failed url should be the announced target — when it
        // is not, fall back to the tab's oldest main_frame record.
        for (auto it = m_inFlight.begin(); it != m_inFlight.end();
             ++it) {
            if (it->tabId == tabId
                    && it->chromeType
                           == QLatin1String("main_frame")) {
                r = it.value();
                popRecord(it->requestId);
                break;
            }
        }
        if (r.requestId.isEmpty()) {
            // Same orphan adoption as the commit feed — a nav that
            // failed near-instantly can have its start parked
            // under tabId -1.
            if (InFlight *orphan =
                    findOrphan(url.toString(), true)) {
                adoptTab(orphan, tabId,
                         ExtensionShimBackend::windowIdFor(
                             BrowserMainWindow::parentWindow(view)));
                takeInFlight(tabId, url.toString(), &r);
            }
        }
        if (r.requestId.isEmpty()) {
            if (consumeRetired(tabId, url.toString()))
                return;
            // The request's queued start has not run yet — buffer
            // the failure; requestStarted drains it in order.
            m_pendingFail.insert(
                tabId, PendingFail{url.toString(), errorCode,
                                   errorString});
            return;
        }
    }
    retire(r);
    QJsonObject extra{
        {QStringLiteral("error"),
         QStringLiteral("net::") + netErrorName(errorCode)},
        {QStringLiteral("errorCode"), errorCode}};
    if (!errorString.isEmpty())
        extra.insert(QStringLiteral("errorDetail"), errorString);
    emitFor(QStringLiteral("onErrorOccurred"), r, extra);
}

int ExtensionRequestWatcher::resourceSettled(WebView *view,
                                             const QJsonArray &entries,
                                             double originEpoch)
{
    if (!view)
        return 0;
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    int paired = 0;
    // The probe reports responseEnd as ms offset from
    // performance.timeOrigin — reconstruct the epoch timestamp.
    const double origin = originEpoch;
    for (const QJsonValue &v : entries) {
        const QJsonObject e = v.toObject();
        const QString url =
            e.value(QLatin1String("url")).toString();
        InFlight r;
        if (!takeInFlight(tabId, url, &r)) {
            // Same orphan adoption as the milestone feeds — the
            // subresource's first-party attribution may still have
            // been pending when its queued start ran.
            if (InFlight *orphan = findOrphan(url, false)) {
                adoptTab(orphan, tabId,
                         ExtensionShimBackend::windowIdFor(
                             BrowserMainWindow::parentWindow(view)));
                takeInFlight(tabId, url, &r);
            }
        }
        if (r.requestId.isEmpty())
            continue;   // never announced — stale or forged report
        ++paired;
        if (origin > 0)
            r.startedMs =
                qint64(origin
                       + e.value(QLatin1String("end")).toDouble());
        emitFor(QStringLiteral("onCompleted"), r,
                QJsonObject{{QStringLiteral("fromCache"),
                             e.value(QLatin1String("transfer"))
                                     .toDouble(1) == 0.0}});
    }
    return paired;
}

int ExtensionRequestWatcher::resourceFailed(WebView *view,
                                            const QUrl &url,
                                            const QString &typeHint)
{
    Q_UNUSED(typeHint);
    if (!view)
        return 0;
    const int tabId = ExtensionShimBackend::tabIdFor(view);
    InFlight r;
    if (!takeInFlight(tabId, url.toString(), &r)) {
        if (InFlight *orphan = findOrphan(url.toString(), false)) {
            adoptTab(orphan, tabId,
                     ExtensionShimBackend::windowIdFor(
                         BrowserMainWindow::parentWindow(view)));
            takeInFlight(tabId, url.toString(), &r);
        }
    }
    if (r.requestId.isEmpty())
        return 0;
    emitFor(QStringLiteral("onErrorOccurred"), r,
            QJsonObject{{QStringLiteral("error"),
                         QStringLiteral("net::ERR_FAILED")}});
    return 1;
}
