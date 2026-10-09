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

#include "rustdownloadengine.h"

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockrequestinterceptor.h"
#include "browserapplication.h"
#include "browserpaths.h"
#include "privacyrequestinterceptor.h"

#include <qjsonobject.h>
#include <qjsondocument.h>
#include <qnetworkproxy.h>
#include <qsettings.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <cstring>
#include <mutex>

/*!
    Poll cadence: the spec's ~2 progress updates per second.  The FFI
    poll itself is a handful of atomic reads — cheap enough that the
    timer staying this fast while RUNNING is fine.
 */
static const int s_pollIntervalMs = 500;

// ---- DLACC04 policy gate --------------------------------------------------
// The Rust engine's per-hop policy verdicts come from this side: the
// FFI callback below is a thin byte-shuffle over gateCheck(), which
// composes the same decisions the normal engine's request
// interceptors make — SEC17 tracking-param strip, SAFE01 https-first
// upgrade and https-only veto, SEC18 domain blocklist, the adblock
// matcher, and the private/LAN boundary — so a download can't slip a
// rule a same-origin resource fetch would hit.  Everything it touches
// is a lock-guarded snapshot or a mutex-guarded set, so it is safe on
// the Rust worker threads.

RustDownloadEngine::GateDecision RustDownloadEngine::gateCheck(
        const QUrl &url, const QUrl &prev, const QUrl &firstParty,
        const QString &scope, AdBlockNetwork *network)
{
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return {GateBlock, QString(),
                QStringLiteral("download scheme refused: %1").arg(scheme)};

    // SSRF boundary: the interceptor exempts private/LAN targets from
    // https policy because user-initiated LAN traffic is legitimate —
    // but a redirect hop that crosses public -> private is an open-
    // redirect attack (a public URL must not make the browser fetch
    // http://192.168.x.x/ or the user's localhost).  The first hop
    // (prev empty) is exempt, matching interceptor semantics; so is
    // private -> private LAN-internal chaining.  Runs before any
    // rewrite: no step below may change the host.
    const bool local =
        PrivacyRequestInterceptor::isPrivateOrLocalHost(url.host());
    if (local && !prev.isEmpty() && prev.host() != url.host()
            && !PrivacyRequestInterceptor::isPrivateOrLocalHost(prev.host()))
        return {GateBlock, QString(),
                QStringLiteral("redirect into a private address refused: %1")
                    .arg(url.host())};

    if (PrivacyRequestInterceptor::shouldBlockDomain(url))
        return {GateBlock, QString(),
                QStringLiteral("download host on the blocklist: %1")
                    .arg(url.host())};

    // Same folding as PrivacyRequestInterceptor::interceptRequest():
    // SEC17 strip, then https-first on what remains — the toggle is
    // the caller's check; isUpgradeCandidate only answers "can this
    // host take the upgrade".
    QUrl target = PrivacyRequestInterceptor::strippedUrl(url);
    if (scheme == QLatin1String("http")
            && PrivacyRequestInterceptor::httpsFirstEnabled()
            && PrivacyRequestInterceptor::isUpgradeCandidate(url, scope))
        target.setScheme(QStringLiteral("https"));
    if (target != url)
        return {GateRewrite, QString::fromUtf8(target.toEncoded()),
                QString()};

    if (scheme == QLatin1String("http")
            && PrivacyRequestInterceptor::shouldWarnHttp(url, scope))
        return {GateBlock, QString(),
                QStringLiteral("insecure http:// download refused: %1")
                    .arg(url.host())};

    // Adblock/tracking rules — the same match() the request
    // interceptor consults.  A download maps to no meaningful
    // Chromium ResourceType, so it is gated as "no type": generic and
    // domain rules still hit it, type-scoped rules ($image, $script…)
    // neither block nor except it — no channel through which it
    // could dodge a rule its URL alone would trip.
    if (network) {
        const AdBlockDecision d = network->match(url, firstParty, -1);
        if (d.action == AdBlockDecision::Block)
            return {GateBlock, QString(),
                    QStringLiteral("blocked by content rules: %1")
                        .arg(url.host())};
        if (d.action == AdBlockDecision::Redirect) {
            const QString redir = d.redirectUrl;
            if (redir.startsWith(QLatin1String("http")))
                return {GateRewrite, redir, QString()};
            // A $redirect to a stub resource (data:, native
            // transparent pixel) has no file to download — block
            // rather than hand the crate a scheme it must refuse.
            return {GateBlock, QString(),
                    QStringLiteral("blocked by content rules: %1")
                        .arg(url.host())};
        }
        if (!d.removeParams.isEmpty()) {
            QUrl cleaned = url;
            if (AdBlockRequestInterceptor::stripQueryParams(
                        &cleaned, d.removeParams))
                return {GateRewrite,
                        QString::fromUtf8(cleaned.toEncoded()), QString()};
        }
    }
    return {GateAllow, QString(), QString()};
}

namespace {

// The reqwest-style proxy URL the download must ride — the configured
// application proxy in a normal process, the managed SOCKS listener
// in a tor process (the application proxy IS that listener there:
// TorManager installs it process-wide).  Null when the fetch may go
// direct; a tor download never reaches dl_start without a proxy —
// canHandle() refuses it first and the item falls back to the
// engine-mediated path.
QString proxySpecForDownload()
{
    const QNetworkProxy ap = QNetworkProxy::applicationProxy();
    QString scheme;
    switch (ap.type()) {
    case QNetworkProxy::Socks5Proxy:
        // socks5h — name resolution happens at the SOCKS peer.  In a
        // tor process a local lookup would be a leak all by itself.
        scheme = QStringLiteral("socks5h");
        break;
    case QNetworkProxy::HttpProxy:
    case QNetworkProxy::HttpCachingProxy:
        scheme = QStringLiteral("http");
        break;
    default:
        return QString();
    }
    QString auth;
    if (!ap.user().isEmpty())
        auth = QString::fromUtf8(QUrl::toPercentEncoding(ap.user()))
            + QLatin1Char(':')
            + QString::fromUtf8(QUrl::toPercentEncoding(ap.password()))
            + QLatin1Char('@');
    QString host = ap.hostName();
    if (host.contains(QLatin1Char(':')))
        host = QLatin1Char('[') + host + QLatin1Char(']');
    return scheme + QLatin1String("://") + auth + host
        + QLatin1Char(':') + QString::number(ap.port());
}

// Byte-shuffle adapter between the crate's C callback and
// gateCheck().  Runs on Rust worker threads — everything it reaches
// is a snapshot or lock-guarded, never live GUI state.
int rustdlGateTrampoline(const char *urlUtf8, const char *prevUtf8,
        const char *firstPartyUtf8, const char *scopeUtf8,
        char *outUrl, size_t outUrlCap, char *outReason,
        size_t outReasonCap, void *ctx)
{
    const QUrl url = QUrl::fromEncoded(QByteArray(urlUtf8 ? urlUtf8 : ""));
    const QUrl prev = QUrl::fromEncoded(QByteArray(prevUtf8 ? prevUtf8 : ""));
    const QUrl firstParty =
        QUrl::fromEncoded(QByteArray(firstPartyUtf8 ? firstPartyUtf8 : ""));
    const RustDownloadEngine::GateDecision d =
        RustDownloadEngine::gateCheck(
            url, prev, firstParty,
            QString::fromUtf8(scopeUtf8 ? scopeUtf8 : ""),
            static_cast<AdBlockNetwork*>(ctx));
    const QByteArray payload = d.action == RustDownloadEngine::GateRewrite
        ? d.url.toUtf8() : d.reason.toUtf8();
    char *dst = d.action == RustDownloadEngine::GateRewrite
        ? outUrl : outReason;
    const size_t cap = d.action == RustDownloadEngine::GateRewrite
        ? outUrlCap : outReasonCap;
    if (dst && cap > 1 && !payload.isEmpty()) {
        const size_t n = qMin<size_t>(cap - 1, size_t(payload.size()));
        memcpy(dst, payload.constData(), n);
        dst[n] = '\0';
    }
    return int(d.action);
}

} // namespace

RustDownloadEngine::RustDownloadEngine(QWebEnginePage *page, const QUrl &url,
                                       const QString &suggestedFileName,
                                       const QString &mimeType, QObject *parent)
    : QObject(parent)
    , m_handle(0)
    , m_page(page)
    , m_url(url)
    , m_suggested(suggestedFileName)
    , m_mimeType(mimeType)
    , m_state(QWebEngineDownloadRequest::DownloadRequested)
    , m_received(0)
    , m_total(-1)
{
    m_timer.setInterval(s_pollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &RustDownloadEngine::poll);
}

RustDownloadEngine::~RustDownloadEngine()
{
    if (m_handle) {
        dl_cancel(m_handle);
        dl_free(m_handle);
    }
}

bool RustDownloadEngine::isAvailable()
{
    return dl_is_available() != 0;
}

bool RustDownloadEngine::isSelected()
{
    if (!isAvailable())
        return false;
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    const QVariant v = settings.value(QLatin1String("engine"));
    const QString s = v.toString().toLower();
    return s.contains(QLatin1String("rust"))
        || s.contains(QLatin1String("accelerated"))
        || v.toInt() == 1;
}

bool RustDownloadEngine::canHandle(const QUrl &url)
{
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    if (BrowserApplication::isTorMode()) {
        // The hard rule of DLACC04: in a tor process a download exits
        // via the managed SOCKS listener or via Chromium's own
        // proxied network path — never direct from this engine.
        return QNetworkProxy::applicationProxy().type()
            == QNetworkProxy::Socks5Proxy;
    }
    return true;
}

void RustDownloadEngine::accept()
{
    if (m_handle || m_dir.isEmpty())
        return;

    // Part files live under the app data dir (0700), never next to
    // user-visible content until the atomic rename lands the result.
    static bool tempArmed = false;
    if (!tempArmed) {
        const QByteArray p = BrowserPaths::dataFilePath(
            QLatin1String("downloads-parts")).toUtf8();
        if (dl_set_temp_dir(p.constData()) != DL_OK) {
            finish(QWebEngineDownloadRequest::DownloadInterrupted,
                   tr("Download engine temp directory unavailable"));
            return;
        }
        tempArmed = true;
    }

    // User-configurable 4-16 segments (0/empty = engine default 8);
    // the DLACC01 settings page owns the visible control.
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    const int connections = settings.value(QLatin1String("connections"), 0).toInt();

    // DLACC04: install the policy gate once.  The callback is pure
    // (settings snapshots + lock-guarded blocklist state only), so it
    // is safe for the Rust worker threads to invoke it on every hop.
    static std::once_flag s_gateOnce;
    std::call_once(s_gateOnce, []() {
        dl_set_gate(&rustdlGateTrampoline,
                    AdBlockManager::instance()->network());
    });

    // Header/policy parity (DLACC04): profile UA so downloads are not
    // a fingerprint diff; the page URL as first_party + referer
    // source; the REF01 referer policy (tor enforces at least
    // Trimmed); the SAFE07 downgrade scope the page's profile gates
    // under; the proxy the fetch must ride.  No cookies are exported
    // yet — DLACC05 wires the per-host Netscape file; until then
    // authenticated sites fall back naturally on the server side
    // (403 -> Interrupted, retryable).
    QJsonObject options;
    if (m_page && m_page->profile()) {
        const QString ua = m_page->profile()->httpUserAgent();
        if (!ua.isEmpty())
            options.insert(QLatin1String("user_agent"), ua);
        options.insert(QLatin1String("scope"),
            PrivacyRequestInterceptor::downgradeScope(m_page->profile()));
    }
    const QUrl firstParty = m_page ? m_page->url() : QUrl();
    const QString fpScheme = firstParty.scheme();
    if (fpScheme == QLatin1String("http") || fpScheme == QLatin1String("https"))
        options.insert(QLatin1String("first_party"),
            QString::fromUtf8(firstParty.toEncoded()));
    int refererPolicy = PrivacyRequestInterceptor::storedRefererPolicy();
    if (BrowserApplication::isTorMode())
        // TorRequestInterceptor floor — tor never sends the page's
        // chosen EngineDefault lower than Trimmed.
        refererPolicy = qMax(refererPolicy,
            int(PrivacyRequestInterceptor::RefererTrimmed));
    options.insert(QLatin1String("referer_policy"), refererPolicy);
    const QString proxy = proxySpecForDownload();
    if (!proxy.isEmpty())
        options.insert(QLatin1String("proxy"), proxy);
    // Second belt under canHandle(): if the proxy vanished between
    // selection and start, the crate fails closed instead of opening
    // a direct socket in a tor process.
    options.insert(QLatin1String("require_proxy"),
                   BrowserApplication::isTorMode());

    const QByteArray u = m_url.toString().toUtf8();
    const QByteArray d = m_dir.toUtf8();
    const QByteArray n = m_fileName.toUtf8();
    const QByteArray o = QJsonDocument(options).toJson(QJsonDocument::Compact);
    DlHandle handle = 0;
    const DlStatus st = dl_start(u.constData(), d.constData(),
                                 n.isEmpty() ? nullptr : n.constData(),
                                 connections, nullptr, o.constData(),
                                 &handle);
    if (st != DL_OK) {
        char *msg = dl_last_error_message();
        const QString reason = msg ? QString::fromUtf8(msg) : QString();
        if (msg)
            dl_string_free(msg);
        finish(QWebEngineDownloadRequest::DownloadInterrupted,
               reason.isEmpty() ? tr("Download engine failed to start") : reason);
        return;
    }
    m_handle = handle;
    m_received = 0;
    m_total = -1;
    m_state = QWebEngineDownloadRequest::DownloadInProgress;
    emit stateChanged(m_state);
    m_timer.start();
    poll();
}

void RustDownloadEngine::cancel()
{
    if (m_handle) {
        dl_cancel(m_handle);
    } else {
        // No handle yet (cancelled at the filename prompt) — report
        // the transition directly so the item finishes.
        finish(QWebEngineDownloadRequest::DownloadCancelled);
    }
}

void RustDownloadEngine::restart()
{
    if (m_handle) {
        dl_cancel(m_handle);
        dl_free(m_handle);
        m_handle = 0;
    }
    m_timer.stop();
    m_received = 0;
    m_total = -1;
    m_error.clear();
    m_output.clear();
    m_state = QWebEngineDownloadRequest::DownloadRequested;
    emit stateChanged(m_state);
}

bool RustDownloadEngine::isFinished() const
{
    switch (m_state) {
    case QWebEngineDownloadRequest::DownloadCompleted:
    case QWebEngineDownloadRequest::DownloadCancelled:
    case QWebEngineDownloadRequest::DownloadInterrupted:
        return true;
    default:
        return false;
    }
}

void RustDownloadEngine::poll()
{
    if (!m_handle)
        return;

    DlProgress p;
    if (dl_poll(m_handle, &p) != DL_OK)
        return;

    const qint64 received = p.bytes_done;
    const qint64 total = p.bytes_total;
    if (received != m_received) {
        m_received = received;
        emit receivedBytesChanged();
    }
    if (total != m_total) {
        m_total = total;
        emit totalBytesChanged();
    }

    switch (static_cast<DlState>(p.state)) {
    case DL_PROBING:
    case DL_RUNNING:
    case DL_MERGING:
        break;
    case DL_DONE: {
        char *out = dl_output_path(m_handle);
        if (out) {
            m_output = QString::fromUtf8(out);
            dl_string_free(out);
        }
        finish(QWebEngineDownloadRequest::DownloadCompleted);
        break;
    }
    case DL_FAILED: {
        char *msg = dl_error_message(m_handle);
        const QString reason = msg ? QString::fromUtf8(msg) : QString();
        if (msg)
            dl_string_free(msg);
        finish(QWebEngineDownloadRequest::DownloadInterrupted,
               reason.isEmpty() ? tr("Download interrupted") : reason);
        break;
    }
    case DL_CANCELLED:
        finish(QWebEngineDownloadRequest::DownloadCancelled);
        break;
    }
}

void RustDownloadEngine::finish(QWebEngineDownloadRequest::DownloadState state,
                                const QString &error)
{
    if (isFinished())
        return;
    m_timer.stop();
    m_error = error;
    m_state = state;
    emit stateChanged(m_state);
}
