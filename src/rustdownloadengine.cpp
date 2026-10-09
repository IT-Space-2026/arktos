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
#include "bwrapgenerator.h"
#include "cookiejar.h"
#include "privacyrequestinterceptor.h"
#include "sandboxmanager.h"

#include <qcoreapplication.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsonobject.h>
#include <qjsondocument.h>
#include <qnetworkcookie.h>
#include <qnetworkproxy.h>
#include <qsettings.h>
#include <qtemporaryfile.h>
#include <quuid.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <cstring>
#include <mutex>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

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
    stopWorkerProcess();
    if (m_handle) {
        dl_cancel(m_handle);
        dl_free(m_handle);
    }
    removeCookieFile();
    cleanupWorkDir();
}

// DLACC05 — per-host cookie export for the engine's cookie_file
// argument.  The download needs the session's cookies (an
// authenticated fetch must look exactly like the engine-mediated
// one would) but never the jar: CookieJar's live store mirror is
// queried through QNetworkCookieJar's canonical matching, so the
// file holds ONLY the rows this URL may receive.  The crate
// re-filters per redirect hop, so a hop to another host can never
// pull this host's rows onto the wire.
QString RustDownloadEngine::exportCookieFile() const
{
    return exportCookieFileTo(BrowserPaths::dataFilePath(
        QLatin1String("downloads-parts")));
}

QString RustDownloadEngine::exportCookieFileTo(const QString &dir) const
{
    QWebEngineProfile *profile = m_page && m_page->profile()
        ? m_page->profile()
        : QWebEngineProfile::defaultProfile();
    if (!profile)
        return QString();
    CookieJar *jar = CookieJar::instance(profile);
    if (!jar)
        return QString();
    const QList<QNetworkCookie> cookies = jar->cookiesForUrl(m_url);
    if (cookies.isEmpty())
        return QString();

    QByteArray rows("# Netscape HTTP Cookie File\n");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const QNetworkCookie &cookie : cookies) {
        const QByteArray name = cookie.name();
        const QByteArray value = cookie.value();
        // A field holding the row delimiters cannot be represented —
        // skip the cookie rather than write a corrupt line.
        if (name.contains('\t') || name.contains('\n')
                || value.contains('\t') || value.contains('\n'))
            continue;
        QString domain = cookie.domain();
        if (domain.isEmpty())
            domain = m_url.host();
        if (domain.isEmpty())
            continue;
        const bool includeSubdomains = domain.startsWith(QLatin1Char('.'));
        const QString path = cookie.path().isEmpty()
            ? QStringLiteral("/") : cookie.path();
        const qint64 expires = cookie.expirationDate().isValid()
            ? cookie.expirationDate().toSecsSinceEpoch() : 0;
        if (expires > 0 && expires <= now)
            continue; // already dead
        if (cookie.isHttpOnly())
            domain.prepend(QLatin1String("#HttpOnly_"));
        rows += domain.toUtf8() + '\t'
            + (includeSubdomains ? "TRUE" : "FALSE") + '\t'
            + path.toUtf8() + '\t'
            + (cookie.isSecure() ? "TRUE" : "FALSE") + '\t'
            + QByteArray::number(expires) + '\t'
            + name + '\t' + value + '\n';
    }

    // cookies-<random> inside the 0700 parts root: the name never
    // carries the destination file name, and QTemporaryFile's
    // mkstemp-style create lands 0600 on unix before a byte is
    // written — fully flushed/fsynced and closed before dl_start.
    QTemporaryFile file(dir + QLatin1String("/cookies-XXXXXX"));
    if (!file.open())
        return QString();
    file.setPermissions(QFileDevice::ReadUser | QFileDevice::WriteUser);
    if (file.write(rows) != rows.size()) {
        file.setAutoRemove(true);
        return QString();
    }
    file.flush();
#ifdef Q_OS_UNIX
    ::fsync(file.handle());
#endif
    file.setAutoRemove(false);
    const QString path = file.fileName();
    file.close();
    return path;
}

// Secure-ish unlink: the file is a few hundred bytes — overwrite the
// rows before removing so cookie values don't linger in free space.
void RustDownloadEngine::removeCookieFile()
{
    if (m_cookieFile.isEmpty())
        return;
    QFile file(m_cookieFile);
    if (file.open(QIODevice::ReadWrite)) {
        const QByteArray zeros(4096, '\0');
        qint64 left = file.size();
        while (left > 0) {
            const qint64 n = file.write(zeros.constData(),
                                        qMin<qint64>(left, zeros.size()));
            if (n <= 0)
                break;
            left -= n;
        }
        file.flush();
        file.close();
    }
    QFile::remove(m_cookieFile);
    m_cookieFile.clear();
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

QByteArray RustDownloadEngine::buildOptionsJson() const
{
    // Header/policy parity (DLACC04): profile UA so downloads are not
    // a fingerprint diff; the page URL as first_party + referer
    // source; the REF01 referer policy (tor enforces at least
    // Trimmed); the SAFE07 downgrade scope the page's profile gates
    // under; the proxy the fetch must ride.  Cookies (DLACC05) ride
    // the scoped export file — never the whole jar.  The same JSON
    // goes into the worker job for the subprocess path (SAND02).
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
    return QJsonDocument(options).toJson(QJsonDocument::Compact);
}

void RustDownloadEngine::accept()
{
    if (m_handle || m_process || m_dir.isEmpty())
        return;

    // SAND02: on Linux the crate runs inside the confined
    // --download-worker subprocess when the wrap is available — the
    // worker's mount namespace exposes only this download's work dir
    // and the destination, not $HOME.  Any failure before the first
    // protocol event falls back to the in-process path below, so a
    // broken sandbox costs confinement, never the download.
    if (!m_workerFellBack
            && sandboxedWorkerEnabled() && startWorkerProcess())
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

    const QByteArray options = buildOptionsJson();

    // DLACC05: the cookie export is written, fsynced and closed
    // before the engine starts; a stale file from a previous accept()
    // on this object is dropped first.
    removeCookieFile();
    m_cookieFile = exportCookieFile();
    const QByteArray c = m_cookieFile.toUtf8();

    const QByteArray u = m_url.toString().toUtf8();
    const QByteArray d = m_dir.toUtf8();
    const QByteArray n = m_fileName.toUtf8();
    const QByteArray &o = options;
    DlHandle handle = 0;
    const DlStatus st = dl_start(u.constData(), d.constData(),
                                 n.isEmpty() ? nullptr : n.constData(),
                                 connections,
                                 c.isEmpty() ? nullptr : c.constData(),
                                 o.constData(), &handle);
    if (st != DL_OK) {
        removeCookieFile();
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
    if (m_process) {
        // The worker owns the transfer — ask it to cancel and give
        // it a moment to exit before the kill.
        m_cancelSent = true;
        QJsonObject cmd;
        cmd.insert(QLatin1String("cmd"), QLatin1String("cancel"));
        writeWorkerLine(cmd);
        QTimer::singleShot(2000, this, [this]() {
            if (m_process && m_cancelSent
                    && m_process->state() != QProcess::NotRunning)
                m_process->kill();
        });
    } else if (m_handle) {
        dl_cancel(m_handle);
    } else {
        // No handle yet (cancelled at the filename prompt) — report
        // the transition directly so the item finishes.
        finish(QWebEngineDownloadRequest::DownloadCancelled);
    }
}

void RustDownloadEngine::restart()
{
    stopWorkerProcess();
    if (m_handle) {
        dl_cancel(m_handle);
        dl_free(m_handle);
        m_handle = 0;
    }
    removeCookieFile();
    cleanupWorkDir();
    m_workerSawEvent = false;
    m_workerFellBack = false;
    m_cancelSent = false;
    m_probeResults = QJsonArray();
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
    removeCookieFile();
    m_error = error;
    m_state = state;
    emit stateChanged(m_state);
}

// ---- SAND02 subprocess backend --------------------------------------------
// `arora --download-worker` runs the crate inside a restrictive bwrap
// wrap (allowlist mounts — $HOME is absent entirely) and speaks the
// JSONL protocol documented in downloadworker.h.  The parent keeps
// owning the policy gate: the worker marshals each dl_set_gate call
// here as a "gate" event and this side answers with gateCheck() —
// the SAME decision function the in-process FFI trampoline uses, so
// the two paths are policy-identical.

bool RustDownloadEngine::sandboxedWorkerEnabled()
{
#if defined(Q_OS_LINUX)
    // Escape hatches mirror SAND01's: an env kill-switch for
    // developers and a settings key for users; both default on.
    if (!qgetenv("ARORA_DL_NO_SANDBOX").isEmpty())
        return false;
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    if (!settings.value(QLatin1String("sandboxedWorker"), true).toBool())
        return false;
    return !SandboxManager::bwrapPath().isEmpty()
        && !workerProgram().isEmpty();
#else
    // The restrictive per-platform worker policies exist in
    // platformgenerators.h but only the Linux bwrap apply path is
    // wired and verified — other platforms stay in-process.
    return false;
#endif
}

QString RustDownloadEngine::workerProgram()
{
    // Tests and dev builds can point at a specific arora binary.
    const QByteArray env = qgetenv("ARORA_WORKER_BINARY");
    if (!env.isEmpty()) {
        const QString path = QString::fromLocal8Bit(env);
        return QFileInfo(path).isExecutable() ? path : QString();
    }
    // Same binary, different entry point — the worker needs no
    // separate install (the self-contained bundle's wrapper script
    // resolves through argv[0] the same way).
    const QString self = QCoreApplication::applicationFilePath();
    return QFileInfo(self).isExecutable() ? self : QString();
}

QString RustDownloadEngine::newWorkDir() const
{
    // Per-download work dir nested inside the destination: the
    // worker's wrap grants destDir and nothing under $HOME, so part
    // files and the scoped cookie export must live there too — a
    // hidden dot-dir is the least surprising thing to briefly appear
    // in ~/Downloads, the merge+rename stays on one mount, and the
    // worker never sees a profile-side path.  0700 so a shared
    // destination doesn't expose the cookie export to other users.
    const QString dir = m_dir
        + QLatin1String("/.arora-dl-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!QDir().mkpath(dir))
        return QString();
    QFile::setPermissions(dir, QFileDevice::ReadUser
        | QFileDevice::WriteUser | QFileDevice::ExeUser);
    return dir;
}

void RustDownloadEngine::cleanupWorkDir()
{
    if (m_workDir.isEmpty())
        return;
    QDir(m_workDir).removeRecursively();
    m_workDir.clear();
}

bool RustDownloadEngine::startWorkerProcess()
{
    const QString bwrap = SandboxManager::bwrapPath();
    const QString program = workerProgram();
    if (bwrap.isEmpty() || program.isEmpty())
        return false;

    cleanupWorkDir();
    m_workDir = newWorkDir();
    if (m_workDir.isEmpty())
        return false;

    // The scoped cookie export is written into the work dir — inside
    // the worker's one writable app-data bind — before spawn.
    removeCookieFile();
    m_cookieFile = exportCookieFileTo(m_workDir);

    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    QJsonObject job;
    job.insert(QLatin1String("cmd"), QLatin1String("job"));
    job.insert(QLatin1String("url"), m_url.toString());
    job.insert(QLatin1String("dest_dir"), m_dir);
    job.insert(QLatin1String("suggested_name"), m_fileName);
    job.insert(QLatin1String("connections"),
               settings.value(QLatin1String("connections"), 0).toInt());
    job.insert(QLatin1String("cookie_file"), m_cookieFile);
    job.insert(QLatin1String("work_dir"), m_workDir);
    job.insert(QLatin1String("options"),
               QJsonDocument::fromJson(buildOptionsJson()).object());
    if (!m_probePaths.isEmpty())
        job.insert(QLatin1String("probe_paths"),
                   QJsonArray::fromStringList(m_probePaths));

    const QStringList wrap = BwrapGenerator::downloadWorkerCommandLine(
        program, {QStringLiteral("--download-worker")},
        m_workDir, m_dir);
    if (wrap.size() < 2)
        return false;

    m_workerInbox.clear();
    m_workerSawEvent = false;
    m_cancelSent = false;
    m_probeResults = QJsonArray();

    m_process = new QProcess(this);
    m_process->setProgram(wrap.first());
    m_process->setArguments(wrap.mid(1));
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &RustDownloadEngine::onWorkerReadyRead);
    connect(m_process, &QProcess::finished,
            this, &RustDownloadEngine::onWorkerFinished);
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onWorkerFinished(-1, QProcess::CrashExit);
    });
    // The job goes down stdin only once the child is actually running
    // — no blocking waitForStarted on the GUI thread; a spawn failure
    // surfaces through errorOccurred/finished and takes the
    // in-process fallback instead.
    connect(m_process, &QProcess::started, this, [this, job]() {
        writeWorkerLine(job);
    });
    m_process->start();

    m_received = 0;
    m_total = -1;
    m_state = QWebEngineDownloadRequest::DownloadInProgress;
    emit stateChanged(m_state);
    return true;
}

void RustDownloadEngine::stopWorkerProcess()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    m_process->deleteLater();
    m_process = nullptr;
}

void RustDownloadEngine::writeWorkerLine(const QJsonObject &line)
{
    if (!m_process || m_process->state() != QProcess::Running)
        return;
    m_process->write(
        QJsonDocument(line).toJson(QJsonDocument::Compact) + '\n');
}

void RustDownloadEngine::onWorkerReadyRead()
{
    if (!m_process)
        return;
    m_workerInbox += m_process->readAllStandardOutput();
    int nl;
    while ((nl = m_workerInbox.indexOf('\n')) != -1) {
        const QByteArray line = m_workerInbox.left(nl).trimmed();
        m_workerInbox.remove(0, nl + 1);
        if (!line.isEmpty()) {
            handleWorkerEvent(QJsonDocument::fromJson(line).object());
            if (!m_process)
                break;
        }
    }
}

void RustDownloadEngine::onWorkerFinished(int exitCode,
                                          QProcess::ExitStatus)
{
    // The worker's last protocol lines can still sit in the pipe when
    // finished() fires (the terminal event is written right before
    // exit) — drain them before drawing conclusions.
    if (m_process) {
        m_workerInbox += m_process->readAllStandardOutput();
        int nl;
        while ((nl = m_workerInbox.indexOf('\n')) != -1) {
            const QByteArray line = m_workerInbox.left(nl).trimmed();
            m_workerInbox.remove(0, nl + 1);
            if (!line.isEmpty())
                handleWorkerEvent(QJsonDocument::fromJson(line).object());
        }
    }

    const QByteArray err =
        m_process ? m_process->readAllStandardError() : QByteArray();
    if (!err.trimmed().isEmpty())
        qWarning("Arora: download worker exited %d — stderr: %s",
                 exitCode, err.trimmed().constData());

    const bool hadEvents = m_workerSawEvent;
    const bool cancelled = m_cancelSent;
    // Detach before any fallback re-entry — accept() gates on
    // m_process being null.
    QProcess *dead = m_process;
    m_process = nullptr;
    m_workerInbox.clear();
    if (dead)
        dead->deleteLater();
    cleanupWorkDir();

    if (isFinished())
        return; // a terminal protocol event already reported
    if (cancelled) {
        finish(QWebEngineDownloadRequest::DownloadCancelled);
        return;
    }
    if (!hadEvents && !m_workerFellBack) {
        // The wrap or the spawn failed before the job started — the
        // download never began, so the in-process path can take it
        // over cleanly.  Costs confinement, never the download.
        m_workerFellBack = true;
        qWarning("Arora: sandboxed download worker unavailable — "
                 "running the accelerated engine in-process");
        accept();
        return;
    }
    finish(QWebEngineDownloadRequest::DownloadInterrupted,
           tr("Download worker exited unexpectedly"));
}

void RustDownloadEngine::handleWorkerEvent(const QJsonObject &event)
{
    m_workerSawEvent = true;
    const QString ev = event.value(QLatin1String("ev")).toString();

    if (ev == QLatin1String("progress")) {
        const qint64 received = qint64(
            event.value(QLatin1String("bytes")).toDouble());
        const qint64 total = qint64(
            event.value(QLatin1String("total")).toDouble());
        if (received != m_received) {
            m_received = received;
            emit receivedBytesChanged();
        }
        if (total > 0 && total != m_total) {
            m_total = total;
            emit totalBytesChanged();
        }
    } else if (ev == QLatin1String("gate")) {
        // The worker's dl_set_gate trampoline needs a policy verdict
        // — answer it through the same gateCheck() the in-process
        // FFI hook uses, keeping the two paths policy-identical.
        const GateDecision d = gateCheck(
            QUrl::fromEncoded(
                event.value(QLatin1String("url")).toString().toUtf8()),
            QUrl::fromEncoded(event.value(QLatin1String("prev_url"))
                .toString().toUtf8()),
            QUrl::fromEncoded(event.value(QLatin1String("first_party"))
                .toString().toUtf8()),
            event.value(QLatin1String("scope")).toString(),
            AdBlockManager::instance()->network());
        QJsonObject reply;
        reply.insert(QLatin1String("ev"), QLatin1String("gate-reply"));
        reply.insert(QLatin1String("id"), event.value(QLatin1String("id")));
        reply.insert(QLatin1String("action"), int(d.action));
        if (d.action == GateRewrite)
            reply.insert(QLatin1String("url"), d.url);
        else if (d.action == GateBlock)
            reply.insert(QLatin1String("reason"), d.reason);
        writeWorkerLine(reply);
    } else if (ev == QLatin1String("probe")) {
        m_probeResults.append(event);
    } else if (ev == QLatin1String("done")) {
        m_output = event.value(QLatin1String("output")).toString();
        const qint64 total = qint64(
            event.value(QLatin1String("total")).toDouble());
        if (total > 0)
            m_total = total;
        finish(QWebEngineDownloadRequest::DownloadCompleted);
    } else if (ev == QLatin1String("cancelled")) {
        finish(QWebEngineDownloadRequest::DownloadCancelled);
    } else if (ev == QLatin1String("error")) {
        const QString msg =
            event.value(QLatin1String("message")).toString();
        finish(QWebEngineDownloadRequest::DownloadInterrupted,
               msg.isEmpty() ? tr("Download interrupted") : msg);
    }
    // "file-name" is informational — the item keeps its chosen name.
}
