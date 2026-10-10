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

#include "privacyrequestinterceptor.h"

#include "adblockrequestinterceptor.h"
#include "scriptcontrolmanager.h"

#include <qdatetime.h>
#include <qhostaddress.h>
#include <qmutex.h>
#include <qreadwritelock.h>
#include <qset.h>
#include <qsettings.h>
#include <qurl.h>
#include <qwebengineloadinginfo.h>
#include <qwebengineprofile.h>
#include <qwebengineurlrequestinfo.h>

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#include "sitedecisionstore.h"
#endif

// #define PRIVACYINTERCEPTOR_DEBUG
#if defined(PRIVACYINTERCEPTOR_DEBUG)
#include <qdebug.h>
#endif

// The interceptor callback runs on Chromium's IO thread — policy is
// read from this lock-guarded snapshot, refreshed by loadSettings()
// on the GUI thread.  Defaults favor privacy.
static QReadWriteLock s_policyLock;
static bool s_httpsFirst = true;
static bool s_httpsOnly = true;
static int s_refererPolicy = PrivacyRequestInterceptor::RefererTrimmed;
static int s_securityLevel = PrivacyRequestInterceptor::Standard;
static bool s_blockPings = true;
static bool s_blockRemoteFonts = false;
static bool s_blockPrefetch = true;
static bool s_blockThirdPartyWebSockets = false;
static bool s_stripTrackingParams = true;
static bool s_domainBlocklist = true;

// SEC18: "proceed" choices on the anti-phishing/malware warning are
// remembered for the session — deliberately nothing is persisted:
// a durable bypass of a listed hostile domain is a foot-gun SAFE01's
// plain-http exception can afford to be and this list cannot.
// Written on the GUI thread, read on the IO thread.
static QMutex s_domainBlockLock;
static QSet<QString> s_sessionBlockedAllowed;
static const int maxBlockedAllowedHosts = 256;

// SEC18: main-frame http(s) URLs the interceptor just refused under
// the domain blocklist.  Same consume-once contract as
// s_blockedHttpNavs — WebPage swaps the recorded refusal for the
// warning interstitial.
static QSet<QString> s_blockedDomainNavs;
static const int maxBlockedDomainNavs = 64;

// SAFE07: hosts whose https main-frame load failed with a genuine
// connection/TLS error — their http: requests stop being upgraded.
// Marks are keyed by profile scope (downgradeScope()) so a private
// window or container can never teach another profile to bypass the
// upgrade, and carry the mark time so a transient outage re-probes
// after s_downgradeTtlMs instead of condemning the host for the rest
// of a long session.  Written from the GUI thread
// (noteNavigationFailure), read on the IO thread.
static QMutex s_downgradeLock;
static QHash<QString, QHash<QString, qint64> > s_downgradedHosts;
static qint64 s_downgradeTtlMs = 30 * 60 * 1000;
// Bound (per scope) so a hostile page cannot grow the set without
// limit.
static const int maxDowngradedHosts = 256;

// SAFE01: HTTPS-Only host exceptions.  A "proceed once" choice lands
// in the session set; "always allow" lands in the persisted snapshot
// (refreshed from the privacy/httpsOnlyExceptions QSettings list by
// loadSettings() and allowHttpForHost/clearHttpAllowance on the GUI
// thread).  Written on the GUI thread, read on the IO thread.
static QMutex s_httpAllowLock;
static QSet<QString> s_sessionHttpAllowed;
static QSet<QString> s_persistedHttpAllowed;
static const int maxHttpAllowedHosts = 256;

// SAFE01: main-frame http: URLs the interceptor just refused under
// HTTPS-Only mode.  WebPage consumes a record to recognize its own
// refusal (LoadFailed + ERR_ACCESS_DENIED) and show the warning page
// instead of the generic not-found error.  Bounded — a full set just
// drops the interstitial recognition, never the block itself.
static QMutex s_blockedNavLock;
static QSet<QString> s_blockedHttpNavs;
static const int maxBlockedHttpNavs = 64;

void PrivacyRequestInterceptor::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    const bool httpsFirst = settings.value(QLatin1String("httpsFirst"), true).toBool();
    const bool httpsOnly = settings.value(QLatin1String("httpsOnly"), true).toBool();
    const int refererPolicy = storedRefererPolicy();
    const int securityLevel = qBound(
        int(PrivacyRequestInterceptor::Standard),
        settings.value(QLatin1String("securityLevel"),
                       int(PrivacyRequestInterceptor::Standard)).toInt(),
        int(PrivacyRequestInterceptor::Safest));
    const bool blockPings =
        settings.value(QLatin1String("blockPings"), true).toBool();
    const bool blockRemoteFonts =
        settings.value(QLatin1String("blockRemoteFonts"), false).toBool();
    const bool blockPrefetch =
        settings.value(QLatin1String("blockPrefetch"), true).toBool();
    const bool blockThirdPartyWebSockets =
        settings.value(QLatin1String("blockThirdPartyWebSockets"), false).toBool();
    const bool stripTrackingParams =
        settings.value(QLatin1String("stripTrackingParams"), true).toBool();
    const bool domainBlocklist =
        settings.value(QLatin1String("domainBlocklist"), true).toBool();
    QSet<QString> persistedHttpAllowed;
#if defined(ARORA_RUSTCORE)
    // SITED01: one-shot import — replay the legacy exception list into
    // the Rust store and retire the key after the writes land.
    const QStringList legacyExceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    bool allWritten = true;
    for (const QString &host : legacyExceptions) {
        const QString lowered = host.toLower();
        if (!lowered.isEmpty())
            allWritten = SiteDecisionStore::set(
                SiteDecisionStore::KindHttpAllow,
                lowered, QLatin1String("allow")) && allWritten;
    }
    if (!legacyExceptions.isEmpty() && allWritten)
        settings.remove(QLatin1String("httpsOnlyExceptions"));
    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(SiteDecisionStore::KindHttpAllow);
    for (auto it = rows.constBegin(); it != rows.constEnd(); ++it) {
        const QString lowered = it.key().toLower();
        if (!lowered.isEmpty())
            persistedHttpAllowed.insert(lowered);
    }
#else
    const QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    for (const QString &host : exceptions) {
        const QString lowered = host.toLower();
        if (!lowered.isEmpty())
            persistedHttpAllowed.insert(lowered);
    }
#endif
    settings.endGroup();
    {
        QWriteLocker lock(&s_policyLock);
        s_httpsFirst = httpsFirst;
        s_httpsOnly = httpsOnly;
        s_refererPolicy = refererPolicy;
        s_securityLevel = securityLevel;
        s_blockPings = blockPings;
        s_blockRemoteFonts = blockRemoteFonts;
        s_blockPrefetch = blockPrefetch;
        s_blockThirdPartyWebSockets = blockThirdPartyWebSockets;
        s_stripTrackingParams = stripTrackingParams;
        s_domainBlocklist = domainBlocklist;
    }
    {
        const QMutexLocker lock(&s_httpAllowLock);
        s_persistedHttpAllowed = persistedHttpAllowed;
    }
}

bool PrivacyRequestInterceptor::httpsFirstEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_httpsFirst;
}

bool PrivacyRequestInterceptor::trimRefererEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_refererPolicy != RefererEngineDefault;
}

int PrivacyRequestInterceptor::refererPolicy()
{
    QReadLocker lock(&s_policyLock);
    return s_refererPolicy;
}

int PrivacyRequestInterceptor::storedRefererPolicy()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    // REF01: the PRIV01 bool folds into the level selector — an old
    // "trimReferer = false" maps to EngineDefault, anything else to
    // the Trimmed default.
    const QVariant storedPolicy =
        settings.value(QLatin1String("refererPolicy"));
    const int level = storedPolicy.isValid()
        ? storedPolicy.toInt()
        : (settings.value(QLatin1String("trimReferer"), true).toBool()
               ? int(RefererTrimmed) : int(RefererEngineDefault));
    settings.endGroup();
    return qBound(int(RefererEngineDefault), level, int(RefererNever));
}

QByteArray PrivacyRequestInterceptor::referrerMetaValue(int level)
{
    switch (level) {
    case RefererTrimmed:
        return QByteArrayLiteral("strict-origin");
    case RefererStrict:
        return QByteArrayLiteral("same-origin");
    case RefererNever:
        return QByteArrayLiteral("no-referrer");
    default:
        return QByteArray();
    }
}

int PrivacyRequestInterceptor::securityLevel()
{
    QReadLocker lock(&s_policyLock);
    return s_securityLevel;
}

bool PrivacyRequestInterceptor::blockPingsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockPings;
}

bool PrivacyRequestInterceptor::blockRemoteFontsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockRemoteFonts;
}

bool PrivacyRequestInterceptor::blockPrefetchEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockPrefetch;
}

bool PrivacyRequestInterceptor::blockThirdPartyWebSocketsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockThirdPartyWebSockets;
}

bool PrivacyRequestInterceptor::stripTrackingParamsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_stripTrackingParams;
}

// SEC18: the list-membership half of the block decision — the match
// itself runs in rustcore (exact + parent-suffix over the merged
// seed ∪ updated set, all memory-safe).  Fail-open like strippedUrl:
// an FFI hiccup or a no-rust build reads as "not listed", never as a
// block.
bool PrivacyRequestInterceptor::isDomainBlocked(const QString &host)
{
#if defined(ARORA_RUSTCORE)
    if (host.isEmpty())
        return false;
    return rc_blocklist_check(host.toUtf8().constData()) == 1;
#else
    Q_UNUSED(host);
    return false;
#endif
}

bool PrivacyRequestInterceptor::domainBlocklistEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_domainBlocklist;
}

// The pure decision — safe on the IO thread (lock-guarded snapshots +
// mutex-guarded sets + the FFI call only).
bool PrivacyRequestInterceptor::shouldBlockDomain(const QUrl &url)
{
    {
        QReadLocker lock(&s_policyLock);
        if (!s_domainBlocklist)
            return false;
    }
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty())
        return false;
    {
        const QMutexLocker lock(&s_domainBlockLock);
        if (s_sessionBlockedAllowed.contains(host))
            return false;
    }
    return isDomainBlocked(host);
}

bool PrivacyRequestInterceptor::isBlockedDomainAllowed(const QString &host)
{
    const QMutexLocker lock(&s_domainBlockLock);
    return s_sessionBlockedAllowed.contains(host.toLower());
}

void PrivacyRequestInterceptor::allowBlockedDomain(const QString &host)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return;
    const QMutexLocker lock(&s_domainBlockLock);
    if (s_sessionBlockedAllowed.size() < maxBlockedAllowedHosts)
        s_sessionBlockedAllowed.insert(lowered);
}

void PrivacyRequestInterceptor::clearBlockedDomainAllowance(
        const QString &host)
{
    const QMutexLocker lock(&s_domainBlockLock);
    s_sessionBlockedAllowed.remove(host.toLower());
}

void PrivacyRequestInterceptor::clearBlockedDomainAllowances()
{
    const QMutexLocker lock(&s_domainBlockLock);
    s_sessionBlockedAllowed.clear();
    s_blockedDomainNavs.clear();
}

void PrivacyRequestInterceptor::recordBlockedDomainNav(const QUrl &url)
{
    const QMutexLocker lock(&s_domainBlockLock);
    if (s_blockedDomainNavs.size() >= maxBlockedDomainNavs) {
        // A flooded set cannot keep the interstitial mapping — the
        // block still happens, the page just falls back to the
        // generic error.  Same conservative direction as the
        // HTTPS-Only registry.
        return;
    }
    s_blockedDomainNavs.insert(QString::fromUtf8(url.toEncoded()));
}

bool PrivacyRequestInterceptor::takeBlockedDomainNav(const QUrl &url)
{
    const QMutexLocker lock(&s_domainBlockLock);
    return s_blockedDomainNavs.remove(QString::fromUtf8(url.toEncoded()));
}

// SEC17: the strip decision the interceptors redirect through.  The
// ruleset match itself runs in rustcore — safe Rust over the
// attacker-controlled URL — so this wrapper only marshals.  Fail-open
// everywhere: an FFI hiccup or a cleaned URL that does not re-parse
// degrades to no-strip, never to a block.
QUrl PrivacyRequestInterceptor::strippedUrl(const QUrl &url)
{
#if defined(ARORA_RUSTCORE)
    const QString scheme = url.scheme();
    if (!url.hasQuery()
        || (scheme != QLatin1String("http")
            && scheme != QLatin1String("https")))
        return url;
    const QByteArray encoded = url.toEncoded();
    char *out = rc_urlstrip(encoded.constData());
    if (!out)
        return url;
    const QByteArray cleaned(out);
    rc_string_free(out);
    if (cleaned.isEmpty() || cleaned == encoded)
        return url;
    const QUrl result = QUrl::fromEncoded(cleaned);
    return result.isValid() ? result : url;
#else
    return url;   // no-rust build: the strip stage is absent
#endif
}

// SAFE04: remote fonts fingerprint GPU/OS text stacks and ping a
// third-party host on every visit — opt-in since icon fonts break.
// Prefetch loads connect to sites a link merely points at — the user
// never asked for the fetch, so it is on by default.  Speculation-
// rules prefetch/prerender navigations arrive classified as
// ResourceTypeMainFrame (indistinguishable from a link click by type
// alone), but every prefetch flavor is marked with a
// Purpose/Sec-Purpose: prefetch request header — match both.  A
// prefetch-upgraded real navigation keeps the header too; blocking it
// just costs Chromium a non-prefetch refetch, never a broken page.
bool PrivacyRequestInterceptor::shouldBlockResource(
        QWebEngineUrlRequestInfo::ResourceType type,
        const QHash<QByteArray, QByteArray> &headers)
{
    bool blockRemoteFonts, blockPrefetch;
    {
        QReadLocker lock(&s_policyLock);
        blockRemoteFonts = s_blockRemoteFonts;
        blockPrefetch = s_blockPrefetch;
    }
    if (type == QWebEngineUrlRequestInfo::ResourceTypeFontResource)
        return blockRemoteFonts;
    if (!blockPrefetch)
        return false;
    if (type == QWebEngineUrlRequestInfo::ResourceTypePrefetch)
        return true;
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        const QByteArray key = it.key().toLower();
        if ((key == "sec-purpose" || key == "purpose")
            && it.value().toLower().contains("prefetch"))
            return true;
    }
    return false;
}

// A host is upgraded only when TLS has a chance of existing: loopback
// (dev servers, tests), private/link-local LAN addresses (routers,
// printers, NAS boxes are overwhelmingly http-only), .localhost/.local
// and .onion are left alone.  Failed upgrades would just bounce the
// user onto the error page for no benefit.
bool PrivacyRequestInterceptor::isPrivateOrLocalHost(const QString &host)
{
    if (host.isEmpty())
        return true;
    QString lowered = host.toLower();
    // QUrl::host() keeps IPv6 literals bracketed.
    if (lowered.startsWith(QLatin1Char('[')) && lowered.endsWith(QLatin1Char(']')))
        lowered = lowered.mid(1, lowered.size() - 2);
    if (lowered == QLatin1String("localhost")
        || lowered.endsWith(QLatin1String(".localhost"))
        || lowered.endsWith(QLatin1String(".local"))
        || lowered.endsWith(QLatin1String(".onion")))
        return true;
    const QHostAddress address(lowered);
    if (address.isNull())
        return false;   // hostname — public candidate
    if (address.isLoopback() || address.isMulticast())
        return true;
    static const QPair<QHostAddress, int> privateNets[] = {
        { QHostAddress(QLatin1String("10.0.0.0")), 8 },
        { QHostAddress(QLatin1String("172.16.0.0")), 12 },
        { QHostAddress(QLatin1String("192.168.0.0")), 16 },
        { QHostAddress(QLatin1String("169.254.0.0")), 16 },
        { QHostAddress(QLatin1String("100.64.0.0")), 10 },  // CGNAT
        { QHostAddress(QLatin1String("198.18.0.0")), 15 },  // benchmarking
        { QHostAddress(QLatin1String("fc00::")), 7 },       // ULA
        { QHostAddress(QLatin1String("fe80::")), 10 },      // link-local
    };
    for (const auto &net : privateNets) {
        if (address.isInSubnet(net.first, net.second))
            return true;
    }
    // Any other IP literal is fine to upgrade — a public http: service
    // on a literal IP may well serve TLS on 443.
    return false;
}

QString PrivacyRequestInterceptor::downgradeScope(
        const QWebEngineProfile *profile)
{
    if (!profile)
        return QString();
    if (!profile->isOffTheRecord())
        return profile->storageName();
    // Unnamed/off-the-record profiles carry no storageName — the
    // pointer is the only identity.  Their marks die with the process
    // anyway, so a pointer key keeps each OTR profile's set private
    // without persisting anything about it.
    return QLatin1String("otr:")
        + QString::number(reinterpret_cast<quintptr>(profile));
}

bool PrivacyRequestInterceptor::failureImpliesDowngrade(int errorDomain,
                                                        int errorCode)
{
    if (errorDomain != int(QWebEngineLoadingInfo::ConnectionErrorDomain))
        return false;
    // Chromium net_error codes inside the connection range (-100..-199)
    // that say nothing about whether the origin serves TLS:
    switch (errorCode) {
    // Name resolution failing for https: fails for http: too —
    // nothing learned, and a marked host would just hide the real
    // resolver error behind an http attempt that fails the same way.
    case -105:  // ERR_NAME_NOT_RESOLVED
    case -119:  // ERR_HOST_RESOLVER_QUEUE_TOO_LARGE
    case -137:  // ERR_NAME_RESOLUTION_FAILED
    case -166:  // ERR_ICANN_NAME_COLLISION
    // Local machine/socket state, not the origin's fault.
    case -106:  // ERR_INTERNET_DISCONNECTED
    case -108:  // ERR_ADDRESS_INVALID
    case -124:  // ERR_WINSOCK_UNEXPECTED_WRITTEN_BYTES
    case -138:  // ERR_NETWORK_ACCESS_DENIED
    case -142:  // ERR_MSG_TOO_BIG
    case -147:  // ERR_ADDRESS_IN_USE
    case -160:  // ERR_SOCKET_RECEIVE_BUFFER_SIZE_UNCHANGEABLE
    case -161:  // ERR_SOCKET_SEND_BUFFER_SIZE_UNCHANGEABLE
    case -162:  // ERR_SOCKET_RECEIVE_BUFFER_SIZE_UNCHANGEABLE
    case -163:  // ERR_SOCKET_SEND_BUFFER_SIZE_UNCHANGEABLE
    case -174:  // ERR_READ_IF_READY_NOT_IMPLEMENTED
    case -176:  // ERR_NO_BUFFER_SPACE
    case -189:  // ERR_CONTROL_MSG_TOO_BIG
    case -190:  // ERR_MULTICAST_NOT_ALLOWED
    // Retryable throttles/queue limits — transient, not TLS evidence.
    case -133:  // ERR_PRECONNECT_MAX_SOCKET_LIMIT
    case -139:  // ERR_TEMPORARILY_THROTTLED
    case -154:  // ERR_WS_THROTTLE_QUEUE_TOO_LARGE
    // Client-certificate, pinning and CT enforcement: the TLS stack
    // itself worked — the site demonstrably does serve https.
    case -110:  // ERR_SSL_CLIENT_AUTH_CERT_NEEDED
    case -117:  // ERR_BAD_SSL_CLIENT_AUTH_CERT
    case -134:  // ERR_SSL_CLIENT_AUTH_PRIVATE_KEY_ACCESS_DENIED
    case -135:  // ERR_SSL_CLIENT_AUTH_CERT_NO_PRIVATE_KEY
    case -141:  // ERR_SSL_CLIENT_AUTH_SIGNATURE_FAILED
    case -150:  // ERR_SSL_PINNED_KEY_NOT_IN_CERT_CHAIN
    case -151:  // ERR_CLIENT_AUTH_CERT_TYPE_UNSUPPORTED
    case -156:  // ERR_SSL_SERVER_CERT_CHANGED
    case -164:  // ERR_SSL_CLIENT_AUTH_CERT_BAD_FORMAT
    case -168:  // ERR_CT_STH_PARSING_FAILED
    case -169:  // ERR_CT_STH_INCOMPLETE
    case -171:  // ERR_CT_NO_SCTS_VERIFIED_OK
    case -177:  // ERR_SSL_CLIENT_AUTH_NO_COMMON_ALGORITHMS
    // WebSocket protocol faults are not TLS evidence.
    case -145:  // ERR_WS_PROTOCOL_ERROR
    case -173:  // ERR_WS_UPGRADE
    // Early-data rejections — the connection machinery retries; the
    // surfaced error means retry bookkeeping went astray, not that
    // TLS is absent.
    case -178:  // ERR_EARLY_DATA_REJECTED
    case -179:  // ERR_WRONG_VERSION_ON_EARLY_DATA
    // The middlebox is broken or refused, not the origin: SOCKS
    // connect failures, CONNECT tunnel refusals/redirects and proxy
    // auth/plumbing errors.  A proxy can refuse a tunnel to a site
    // that serves TLS fine — under Tor every failure looks like this,
    // which is why marking is skipped there entirely.
    case -111:  // ERR_TUNNEL_CONNECTION_FAILED
    case -115:  // ERR_PROXY_AUTH_UNSUPPORTED
    case -120:  // ERR_SOCKS_CONNECTION_FAILED
    case -121:  // ERR_SOCKS_CONNECTION_HOST_UNREACHABLE
    case -127:  // ERR_PROXY_AUTH_REQUESTED
    case -130:  // ERR_PROXY_CONNECTION_FAILED
    case -131:  // ERR_MANDATORY_PROXY_CONFIGURATION_FAILED
    case -136:  // ERR_PROXY_CERTIFICATE_INVALID
    case -140:  // ERR_HTTPS_PROXY_TUNNEL_RESPONSE_REDIRECT
    case -170:  // ERR_UNABLE_TO_REUSE_CONNECTION_FOR_PROXY_AUTH
    case -186:  // ERR_PROXY_UNABLE_TO_CONNECT_TO_DESTINATION
    case -187:  // ERR_PROXY_DELEGATE_CANCELED_CONNECT_REQUEST
    case -188:  // ERR_PROXY_DELEGATE_CANCELED_CONNECT_RESPONSE
        return false;
    }
    // What is left is real evidence the origin cannot be reached over
    // TLS: refused/reset/closed/timed-out connections and handshake,
    // ALPN or protocol errors inside the tunnel.
    return true;
}

// Live-mark check: expired entries are lazily dropped, which is what
// turns the TTL into a re-probe — the host becomes upgradeable again.
static bool isMarkedDowngraded(const QString &scope, const QString &host)
{
    const QMutexLocker lock(&s_downgradeLock);
    const auto scopeIt = s_downgradedHosts.find(scope);
    if (scopeIt == s_downgradedHosts.end())
        return false;
    const auto hostIt = scopeIt->find(host.toLower());
    if (hostIt == scopeIt->end())
        return false;
    if (QDateTime::currentMSecsSinceEpoch() - hostIt.value()
            > s_downgradeTtlMs) {
        scopeIt->erase(hostIt);
        return false;
    }
    return true;
}

bool PrivacyRequestInterceptor::isUpgradeCandidate(const QUrl &url,
                                                   const QString &scope)
{
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    if (isPrivateOrLocalHost(host))
        return false;
    return !isMarkedDowngraded(scope, host);
}

bool PrivacyRequestInterceptor::isDowngraded(const QString &host,
                                             const QString &scope)
{
    return isMarkedDowngraded(scope, host);
}

bool PrivacyRequestInterceptor::noteNavigationFailure(const QUrl &url,
        int errorDomain, int errorCode, const QString &scope)
{
    if (!httpsFirstEnabled()
        || url.scheme() != QLatin1String("https")
        || !failureImpliesDowngrade(errorDomain, errorCode))
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    const QMutexLocker lock(&s_downgradeLock);
    QHash<QString, qint64> &marks = s_downgradedHosts[scope];
    const auto it = marks.find(host);
    if (it != marks.end()) {
        // Still failing — restart the re-probe window but do not
        // report a new downgrade.
        it.value() = QDateTime::currentMSecsSinceEpoch();
        return false;
    }
    if (marks.size() >= maxDowngradedHosts) {
        // FIFO eviction is not needed — a full set just stops
        // downgrading, which is the conservative direction.
        return false;
    }
    marks.insert(host, QDateTime::currentMSecsSinceEpoch());
    return true;
}

void PrivacyRequestInterceptor::clearDowngradedHost(const QString &host,
                                                    const QString &scope)
{
    const QMutexLocker lock(&s_downgradeLock);
    const auto scopeIt = s_downgradedHosts.find(scope);
    if (scopeIt != s_downgradedHosts.end())
        scopeIt->remove(host.toLower());
}

void PrivacyRequestInterceptor::clearDowngradedHosts()
{
    const QMutexLocker lock(&s_downgradeLock);
    s_downgradedHosts.clear();
}

qint64 PrivacyRequestInterceptor::downgradeTtlMs()
{
    const QMutexLocker lock(&s_downgradeLock);
    return s_downgradeTtlMs;
}

void PrivacyRequestInterceptor::setDowngradeTtlMs(qint64 ms)
{
    const QMutexLocker lock(&s_downgradeLock);
    s_downgradeTtlMs = ms;
}

bool PrivacyRequestInterceptor::httpsOnlyEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_httpsOnly;
}

bool PrivacyRequestInterceptor::shouldWarnHttp(const QUrl &url,
                                               const QString &scope)
{
    bool httpsFirst, httpsOnly;
    {
        QReadLocker lock(&s_policyLock);
        httpsFirst = s_httpsFirst;
        httpsOnly = s_httpsOnly;
    }
    if (!httpsOnly)
        return false;
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    {
        const QMutexLocker lock(&s_httpAllowLock);
        if (s_sessionHttpAllowed.contains(host)
            || s_persistedHttpAllowed.contains(host))
            return false;
    }
    // The https-first upgrade claims upgradeable hosts first — when
    // it is on, only a host that can no longer be upgraded away
    // (downgraded after a failed https load) still warns.  With the
    // upgrade off, every public http: navigation warns.
    if (httpsFirst && isUpgradeCandidate(url, scope))
        return false;
    return true;
}

bool PrivacyRequestInterceptor::isHttpAllowedHost(const QString &host)
{
    const QString lowered = host.toLower();
    const QMutexLocker lock(&s_httpAllowLock);
    return s_sessionHttpAllowed.contains(lowered)
        || s_persistedHttpAllowed.contains(lowered);
}

void PrivacyRequestInterceptor::allowHttpForHost(const QString &host,
                                                 bool persistent)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return;
    {
        const QMutexLocker lock(&s_httpAllowLock);
        if (s_sessionHttpAllowed.size() < maxHttpAllowedHosts)
            s_sessionHttpAllowed.insert(lowered);
        if (!persistent)
            return;
        if (s_persistedHttpAllowed.contains(lowered))
            return;
    }
#if defined(ARORA_RUSTCORE)
    SiteDecisionStore::set(SiteDecisionStore::KindHttpAllow,
                           lowered, QLatin1String("allow"));
#else
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    if (!exceptions.contains(lowered)) {
        exceptions.append(lowered);
        settings.setValue(QLatin1String("httpsOnlyExceptions"), exceptions);
    }
    settings.endGroup();
#endif
    const QMutexLocker lock(&s_httpAllowLock);
    s_persistedHttpAllowed.insert(lowered);
}

void PrivacyRequestInterceptor::clearHttpAllowance(const QString &host)
{
    const QString lowered = host.toLower();
    {
        const QMutexLocker lock(&s_httpAllowLock);
        s_sessionHttpAllowed.remove(lowered);
        s_persistedHttpAllowed.remove(lowered);
    }
#if defined(ARORA_RUSTCORE)
    SiteDecisionStore::remove(SiteDecisionStore::KindHttpAllow, lowered);
#else
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    if (exceptions.removeAll(lowered) > 0)
        settings.setValue(QLatin1String("httpsOnlyExceptions"), exceptions);
    settings.endGroup();
#endif
}

QStringList PrivacyRequestInterceptor::httpExceptionHosts()
{
    const QMutexLocker lock(&s_httpAllowLock);
    QStringList hosts = s_persistedHttpAllowed.values();
    hosts.sort();
    return hosts;
}

void PrivacyRequestInterceptor::recordBlockedHttpNav(const QUrl &url)
{
    const QMutexLocker lock(&s_blockedNavLock);
    if (s_blockedHttpNavs.size() >= maxBlockedHttpNavs) {
        // A flooded set cannot keep the interstitial mapping — the
        // block still happens, the page just falls back to the
        // generic error.  Dropping is the conservative direction.
        return;
    }
    s_blockedHttpNavs.insert(QString::fromUtf8(url.toEncoded()));
}

bool PrivacyRequestInterceptor::takeBlockedHttpNav(const QUrl &url)
{
    const QMutexLocker lock(&s_blockedNavLock);
    return s_blockedHttpNavs.remove(QString::fromUtf8(url.toEncoded()));
}

bool PrivacyRequestInterceptor::shouldWarnFormPost(const QUrl &url,
                                                   const QString &scope)
{
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    // The https-first upgrade claims the request before it goes out —
    // the body then travels over TLS (or the submit fails into the
    // downgrade set, which makes the next attempt warn here).
    if (httpsFirstEnabled() && isUpgradeCandidate(url, scope))
        return false;
    return true;
}

// Narrower than isPrivateOrLocalHost: only a real loopback page is a
// "potentially trustworthy" secure context (Chromium treats
// http://localhost and http://127.0.0.0/8 as secure).  LAN/private
// hosts are still insecure http origins and keep the Safer block.
static bool isLoopbackHost(const QString &host)
{
    QString lowered = host.toLower();
    if (lowered.startsWith(QLatin1Char('[')) && lowered.endsWith(QLatin1Char(']')))
        lowered = lowered.mid(1, lowered.size() - 2);
    if (lowered == QLatin1String("localhost")
        || lowered.endsWith(QLatin1String(".localhost")))
        return true;
    const QHostAddress address(lowered);
    return !address.isNull() && address.isLoopback();
}

bool PrivacyRequestInterceptor::shouldBlockScript(
        const QUrl &firstPartyUrl,
        QWebEngineUrlRequestInfo::ResourceType type)
{
    if (securityLevel() < Safer)
        return false;
    switch (type) {
    case QWebEngineUrlRequestInfo::ResourceTypeScript:
    case QWebEngineUrlRequestInfo::ResourceTypeWorker:
    case QWebEngineUrlRequestInfo::ResourceTypeSharedWorker:
    case QWebEngineUrlRequestInfo::ResourceTypeServiceWorker:
        break;
    default:
        return false;
    }
    if (firstPartyUrl.scheme() != QLatin1String("http"))
        return false;
    // JSCTL: an explicit per-site Allow grant beats the tier — without
    // this skip the page would get JavascriptEnabled but its external
    // scripts would still be dropped here.
    if (ScriptControlManager::isAllowedHostSnapshot(firstPartyUrl.host()))
        return false;
    return !isLoopbackHost(firstPartyUrl.host());
}

// Same last-two-labels approximation AdBlockRule uses for its
// first-party/third-party test — no public-suffix list, so unrelated
// hosts under multi-level public suffixes read as same-party.
static bool sameSite(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return a == b;
    const QString lowerA = a.toLower();
    const QString lowerB = b.toLower();
    if (lowerA == lowerB)
        return true;
    if (lowerA.endsWith(QLatin1Char('.') + lowerB)
        || lowerB.endsWith(QLatin1Char('.') + lowerA))
        return true;
    const QStringList aParts = lowerA.split(QLatin1Char('.'));
    const QStringList bParts = lowerB.split(QLatin1Char('.'));
    const QString aBase = aParts.size() > 2
        ? aParts.mid(aParts.size() - 2).join(QLatin1Char('.')) : lowerA;
    const QString bBase = bParts.size() > 2
        ? bParts.mid(bParts.size() - 2).join(QLatin1Char('.')) : lowerB;
    return aBase == bBase;
}

// XSLEAK03: a cross-site WebSocket's connect accept/refuse outcome is
// an XS-Leak oracle (xsinator's WebSocket vector) — and unlike the
// response-header leaks, the upgrade request IS visible to the
// interceptor as ResourceTypeWebSocket, so this is the one reachable
// app-side mitigation.  Opt-in only: it breaks sites that legitimately
// open sockets to another site.
bool PrivacyRequestInterceptor::shouldBlockWebSocket(
        const QUrl &firstPartyUrl, const QUrl &requestUrl,
        QWebEngineUrlRequestInfo::ResourceType type)
{
    if (type != QWebEngineUrlRequestInfo::ResourceTypeWebSocket)
        return false;
    if (!blockThirdPartyWebSocketsEnabled())
        return false;
    if (firstPartyUrl.host().isEmpty())
        return false;
    return !sameSite(firstPartyUrl.host(), requestUrl.host());
}

// scheme://host[:port]/ — the "origin" a Referer is trimmed down to.
static QByteArray refererOrigin(const QUrl &url)
{
    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(url.host());
    if (url.port() != -1)
        origin.setPort(url.port());
    return (origin.toString() + QLatin1Char('/')).toUtf8();
}

void PrivacyRequestInterceptor::applyRefererPolicy(
        QWebEngineUrlRequestInfo &info, int minimumLevel)
{
    int level;
    {
        QReadLocker lock(&s_policyLock);
        level = qMax(s_refererPolicy, minimumLevel);
    }
    if (level == RefererEngineDefault)
        return;

    const QUrl url = info.requestUrl();
    if (url.scheme() != QLatin1String("http")
        && url.scheme() != QLatin1String("https"))
        return;

    // The renderer-computed Referer (already honoring the page's own
    // referrer policy) is visible here for navigations AND
    // subresources.  No header means the referer was deliberately
    // withheld — typed navigation, rel=noreferrer, a no-referrer page
    // policy or a downgrade — and a hardening feature must not
    // synthesize one.
    const QByteArray refHeader = info.httpHeaders().value("Referer");
    if (refHeader.isEmpty())
        return;
    const QUrl refUrl(QString::fromUtf8(refHeader));
    if (refUrl.scheme() != QLatin1String("http")
        && refUrl.scheme() != QLatin1String("https"))
        return;  // opaque/scheme-less referer — leave it alone

    const QByteArray rewritten = rewrittenReferer(level, refUrl, url);
    static const bool debug =
        qEnvironmentVariableIsSet("ARORA_PRIVACY_DEBUG");
    if (debug)
        qInfo().noquote() << "PrivacyRequestInterceptor: referer"
            << refHeader << "->" << rewritten << "for" << url;
    info.setHttpHeader("Referer", rewritten);
}

QByteArray PrivacyRequestInterceptor::rewrittenReferer(
        int level, const QUrl &source, const QUrl &target)
{
    if (level <= RefererEngineDefault)
        return source.toString().toUtf8();
    if (level == RefererNever)
        return QByteArray();

    // strict-origin's downgrade rule everywhere: an https origin is
    // never revealed inside a plaintext http request.
    if (source.scheme() == QLatin1String("https")
        && target.scheme() == QLatin1String("http"))
        return QByteArray();

    const bool crossSite = !sameSite(source.host(), target.host());
    if (crossSite) {
        if (level >= RefererStrict)
            return QByteArray();
        // uBO referrer-spoof: the destination only ever sees itself.
        return refererOrigin(target);
    }
    // Same-site: the origin alone — the full path and query never
    // leave, even to another endpoint of the same site.
    return refererOrigin(source);
}

PrivacyRequestInterceptor::PrivacyRequestInterceptor(AdBlockNetwork *network,
                                                     QWebEngineProfile *profile)
    : QWebEngineUrlRequestInterceptor(profile)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
    , m_scope(downgradeScope(profile))
{
}

void PrivacyRequestInterceptor::interceptRequest(QWebEngineUrlRequestInfo &info)
{
    // Runs on the WebEngine IO thread — only the lock-guarded
    // snapshots may be read here.
    //
    // STALL01: internal/non-web requests (devtools:, chrome:, qrc:,
    // arora-*:, abp:, data:, ...) pass through untouched — none of
    // the policies below are meaningful outside http(s)/ws(s), and
    // the delegated adblock match must never see them.
    if (!AdBlockRequestInterceptor::isWebRequestScheme(
            info.requestUrl().scheme()))
        return;
    bool httpsFirst;
    bool blockPings;
    {
        QReadLocker lock(&s_policyLock);
        httpsFirst = s_httpsFirst;
        blockPings = s_blockPings;
    }

    // PING01: navigator.sendBeacon, <a ping> hyperlink auditing and
    // CSP report uploads are telemetry-shaped POSTs — page-requested
    // outbound traffic the user never sees.  Drop them at the request
    // layer; CSP enforcement is unaffected (it happens regardless of
    // whether the report is delivered).
    const QWebEngineUrlRequestInfo::ResourceType resourceType =
        info.resourceType();
    if (blockPings
        && (resourceType == QWebEngineUrlRequestInfo::ResourceTypePing
            || resourceType
                   == QWebEngineUrlRequestInfo::ResourceTypeCspReport)) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: ping block"
                 << info.requestUrl() << "type" << resourceType;
#endif
        info.block(true);
        return;
    }

    // SAFE04: opt-in remote-font block and the default-on prefetch
    // block — dropped before the navigation/script stages, which only
    // ever see other resource types.  The header scan catches
    // speculation-rules prefetches Chromium mislabels as MainFrame.
    if (shouldBlockResource(resourceType, info.httpHeaders())) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: resource block"
                 << info.requestUrl() << "type" << resourceType;
#endif
        info.block(true);
        return;
    }

    const QUrl url = info.requestUrl();

    // XSLEAK03: opt-in third-party WebSocket block (default off) —
    // the upgrade arrives classified ResourceTypeWebSocket, so this
    // layer is the one place a cross-site socket can be refused.
    if (shouldBlockWebSocket(info.firstPartyUrl(), url, resourceType)) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: third-party ws block"
                 << url << "on" << info.firstPartyUrl();
#endif
        info.block(true);
        return;
    }

    // SEC18: anti-phishing/malware domain blocklist — a listed host
    // is stopped before the strip/upgrade stages (no point paying a
    // redirect into a domain the next hop would refuse anyway, and
    // the recorded refusal keeps the originally requested URL for
    // WebPage's interstitial swap).  Redirect follow-ups re-enter
    // this whole pipeline, so a mid-chain hop onto a listed domain
    // is caught identically.
    if (resourceType == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
        && shouldBlockDomain(url)) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: domain block" << url;
#endif
        recordBlockedDomainNav(url);
        info.block(true);
        return;
    }

    // SEC17: ClearURLs-style tracking-param strip — when a rustcore
    // rule fires the request is redirected to the cleaned URL before
    // it leaves (the re-issued request re-runs this whole pipeline,
    // so every block stage still applies to it).  GET/HEAD only: a
    // redirect on a body-carrying method could renegotiate the
    // method, so POSTs keep their params.  Folded into the
    // https-first redirect so a tracked http: navigation still costs
    // one redirect, not two.
    bool stripParams;
    {
        QReadLocker lock(&s_policyLock);
        stripParams = s_stripTrackingParams;
    }
    QUrl target = url;
    if (stripParams
        && (info.requestMethod() == QByteArrayLiteral("GET")
            || info.requestMethod() == QByteArrayLiteral("HEAD")))
        target = strippedUrl(url);

    if (httpsFirst
        && info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
        && isUpgradeCandidate(url, m_scope)) {
        target.setScheme(QLatin1String("https"));
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: https-first" << url << "->" << target;
#endif
        info.redirect(target);
        return;
    }
    if (target != url) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: strip" << url << "->" << target;
#endif
        info.redirect(target);
        return;
    }

    // SAFE01: HTTPS-Only mode — an http: main-frame request still
    // standing after the https-first upgrade pass (upgradeable hosts
    // were redirected above; only downgraded/allowed/exempt http:
    // reaches this line) is refused here.  WebPage recognizes the
    // recorded refusal and shows the warning interstitial; the
    // acceptNavigationRequest veto covers the navigations it sees,
    // this block covers every redirect hop and anything that bypasses
    // the page hook.
    if (resourceType == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
        && shouldWarnHttp(url, m_scope)) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: https-only block" << url;
#endif
        recordBlockedHttpNav(url);
        info.block(true);
        return;
    }

    // SECLVL Safer+: drop script-execution fetches on insecure http
    // pages before referrer trimming or adblock rules run — a dead
    // request needs neither.
    if (shouldBlockScript(info.firstPartyUrl(), info.resourceType())) {
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: safer-js block"
                 << url << "on" << info.firstPartyUrl();
#endif
        info.block(true);
        return;
    }

    applyRefererPolicy(info);

    m_adBlock->interceptRequest(info);
}
