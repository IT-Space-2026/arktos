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

#ifndef EXTENSIONREQUESTWATCHER_H
#define EXTENSIONREQUESTWATCHER_H

#include <qhash.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qmap.h>
#include <qobject.h>
#include <qpointer.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

class QWebEngineUrlRequestInfo;
class WebView;

// ER11 — chrome.webRequest, observe-only.  Qt's extension embedder
// never registered the webRequest delegates, so the hosted runtime
// serves the event surface itself:
//
//   onBeforeRequest / onBeforeSendHeaders / onSendHeaders
//       — from the profile's QWebEngineUrlRequestInterceptor (the
//         PrivacyRequestInterceptor prepareProfile installs calls
//         observeRequest() before any decision; off-the-record and
//         tor profiles never report — extensions never see them).
//   onHeadersReceived / onCompleted / onErrorOccurred
//       — main-frame navigations pair with the hub's
//         loadingChanged/loadFinished signals (real responseHeaders
//         arrive on the commit); subresource + fetch completions
//         pair with the page-side resource-timing probe
//         (src/data/extresprobe.js) reporting through the page
//         bridge.  A completion is only ever emitted for a request
//         the interceptor already announced — an unpaired page-side
//         report is a forged-or-stale call and is dropped.
//   onBeforeRedirect / onResponseStarted / onAuthRequired
//       — no observation source exists; subscribe answers a typed
//         'unsupported' and the shim logs the gap.
//
// NOT blocking: 'blocking' and 'requestBody' extraInfoSpec answer a
// typed 'unsupported' (the interceptor cannot mutate and reading a
// body would consume it); the listener still registers as a plain
// observer, matching Chrome MV3's non-blocking degrade.
//
// The security boundary is ours: a request reaches an extension only
// when (a) some registered filter matches it AND (b) the extension's
// host_permissions cover the request URL — host coverage is re-proved
// per event in emitFor(), never delegated to the page side.  The
// shim additionally applies each listener's own urls/types/tabId
// filter before invoking its callback.
//
// Threading: interceptRequest runs on the engine's IO thread.
// observeRequest() snapshots the info and queued-delivers to the
// GUI thread; zero subscribers costs an atomic load.
class ExtensionRequestWatcher : public QObject
{
    Q_OBJECT

public:
    static ExtensionRequestWatcher *instance();

    // IO-thread entry — called by PrivacyRequestInterceptor.  Cheap
    // atomic bail when nothing is subscribed.
    static void observeRequest(const QWebEngineUrlRequestInfo &info);
    // Same, after the privacy/adblock stages applied a block — the
    // extension-visible name is Chrome's net::ERR_BLOCKED_BY_CLIENT.
    static void observeBlocked(const QWebEngineUrlRequestInfo &info);

    // Registration surface for ChromeBridge::invokeWebRequest —
    // 'context' is the calling bridge; its destruction drops the
    // registration (listeners die with their shim context like
    // Chrome's).  Answers the {ok,result}|{ok:false,error} shape.
    QJsonObject subscribe(QObject *context, const QString &extensionId,
                          const QString &registrationId,
                          const QString &event, const QJsonObject &filter,
                          const QJsonArray &extraInfoSpec);
    QJsonObject unsubscribe(const QString &extensionId,
                            const QString &registrationId);
    void dropContext(QObject *context);
    void dropExtension(const QString &extensionId);
    bool hasSubscribers() const { return m_subscriberCount > 0; }
    int registrationCount(const QString &extensionId) const;

    // Navigation-milestone feeds — ExtensionEventHub's per-page
    // load signals.  Pair with the in-flight main-frame record the
    // interceptor opened; unpaired calls are no-ops.
    void mainFrameCommitted(
            WebView *view, const QUrl &url,
            const QMultiMap<QByteArray, QByteArray> &responseHeaders);
    void mainFrameCompleted(WebView *view, const QUrl &url);
    void mainFrameFailed(WebView *view, const QUrl &url,
                         int errorCode, const QString &errorString);
    // Page-probe feeds — ExtensionPageBridge's 'resourceTiming' op.
    // Both are forge-gated: a report only completes/errors a request
    // the interceptor actually announced for this tab.  Answers the
    // count paired.  'originEpoch' is the page's
    // performance.timeOrigin — the probe's responseEnd offsets
    // reconstruct onto it.
    int resourceSettled(WebView *view, const QJsonArray &entries,
                        double originEpoch);
    int resourceFailed(WebView *view, const QUrl &url,
                       const QString &typeHint);

private:
    explicit ExtensionRequestWatcher(QObject *parent = nullptr);

    Q_INVOKABLE void requestStarted(const QJsonObject &snapshot);
    Q_INVOKABLE void requestBlockedNow(const QJsonObject &snapshot);

    struct Registration {
        QPointer<QObject> context;
        QString event;
        QStringList urls;
        QStringList types;
        int tabId = -1;
        int windowId = -1;
        bool wantRequestHeaders = false;
        bool wantResponseHeaders = false;
    };
    struct InFlight {
        QString requestId;
        QString url;
        QString method;
        QString chromeType;
        QString initiator;
        int tabId = -1;
        int windowId = -1;
        int frameId = -1;
        int parentFrameId = -1;
        bool isDownload = false;
        qint64 startedMs = 0;
        // Real response headers captured at the main-frame commit —
        // kept so the later onCompleted can re-serve them.
        QJsonArray responseHeaders;
        // The commit signal beat the queued start — requestStarted
        // still owes the record its onHeadersReceived emit (after
        // onBeforeSendHeaders, preserving Chrome's ordering).
        bool commitEarly = false;
        // The engine's commit milestone (loadingChanged's
        // Succeeded status) has landed — onHeadersReceived served.
        bool committed = false;
        // loadFinished ran first (the engine reports it before the
        // succeeded-status info): the commit still owes this record
        // its onCompleted emit, or the fallback timer answers it.
        bool completedPending = false;
    };

    bool filterMatches(const Registration &reg, const QString &url,
                       const QString &chromeType, int tabId,
                       int windowId) const;
    // Emission: details -> every extension holding a matching
    // registration whose host permissions cover the request url.
    // Header fields reach only registrations that asked for them.
    void emitFor(const QString &event, const InFlight &request,
                 const QJsonObject &extra = QJsonObject());
    void openRequest(const QJsonObject &snapshot, InFlight *out);
    bool takeInFlight(int tabId, const QString &url, InFlight *out);
    InFlight *findRecord(int tabId, const QString &url);
    // A record parked under tabId -1 — its queued start ran before
    // the tab's pending/document url updated, so attribution lost
    // the same race the milestone buffers exist for.  The feeds
    // claim it by url and adopt the resolving tab; 'mainFrameOnly'
    // scopes the claim to navigations.
    InFlight *findOrphan(const QString &url, bool mainFrameOnly);
    void adoptTab(InFlight *rec, int tabId, int windowId);
    void popRecord(const QString &requestId);
    // A completed/failed main-frame url is remembered briefly so a
    // late second milestone (loadFinished arriving after the commit
    // already settled the record) is not mistaken for a racing
    // queued start.
    void retire(const InFlight &request);
    bool consumeRetired(int tabId, const QString &url);
    void finishMainFrame(InFlight *match);
    void purgeExpired();
    QJsonObject detailsFor(const InFlight &request) const;

    QHash<QString, QHash<QString, Registration>> m_regs;
    QHash<QString, InFlight> m_inFlight;
    // tabId -> url -> FIFO requestIds — the completion pairing key.
    QHash<int, QHash<QString, QStringList>> m_byTabUrl;
    // The interceptor's requestStarted is a queued delivery — a fast
    // commit/failure can legitimately land first.  Buffer the
    // milestone (bounded: one per tab) so the late-arriving start
    // still serves headersReceived/errorOccurred in Chrome order.
    struct PendingCommit {
        QString url;
        QJsonArray responseHeaders;
    };
    struct PendingFail {
        QString url;
        int errorCode = 0;
        QString errorString;
    };
    QHash<int, PendingCommit> m_pendingCommit;
    QHash<int, PendingFail> m_pendingFail;
    QHash<int, QString> m_pendingDone;   // tabId -> committed url
    QHash<int, QStringList> m_retired;   // tabId -> settled urls
    int m_subscriberCount = 0;
    qint64 m_nextRequestId = 0;
};

#endif // EXTENSIONREQUESTWATCHER_H
