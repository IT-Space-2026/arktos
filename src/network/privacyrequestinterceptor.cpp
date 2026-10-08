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

#include <qhostaddress.h>
#include <qmutex.h>
#include <qreadwritelock.h>
#include <qset.h>
#include <qsettings.h>
#include <qurl.h>
#include <qwebengineurlrequestinfo.h>

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

// Session-scoped set of hosts whose https main-frame load failed —
// their http: requests stop being upgraded.  Written from the GUI
// thread (noteNavigationFailure), read on the IO thread.
static QMutex s_downgradeLock;
static QSet<QString> s_downgradedHosts;
// Bound so a hostile page cannot grow the set without limit.
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
    QSet<QString> persistedHttpAllowed;
    const QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    for (const QString &host : exceptions) {
        const QString lowered = host.toLower();
        if (!lowered.isEmpty())
            persistedHttpAllowed.insert(lowered);
    }
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
static bool isPrivateOrLocalHost(const QString &host)
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

bool PrivacyRequestInterceptor::isUpgradeCandidate(const QUrl &url)
{
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    if (isPrivateOrLocalHost(host))
        return false;
    const QMutexLocker lock(&s_downgradeLock);
    return !s_downgradedHosts.contains(host.toLower());
}

bool PrivacyRequestInterceptor::isDowngraded(const QString &host)
{
    const QMutexLocker lock(&s_downgradeLock);
    return s_downgradedHosts.contains(host.toLower());
}

bool PrivacyRequestInterceptor::noteNavigationFailure(const QUrl &url)
{
    if (!httpsFirstEnabled() || url.scheme() != QLatin1String("https"))
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    const QMutexLocker lock(&s_downgradeLock);
    if (s_downgradedHosts.contains(host))
        return false;
    if (s_downgradedHosts.size() >= maxDowngradedHosts) {
        // FIFO eviction is not needed — a full set just stops
        // downgrading, which is the conservative direction.
        return false;
    }
    s_downgradedHosts.insert(host);
    return true;
}

void PrivacyRequestInterceptor::clearDowngradedHosts()
{
    const QMutexLocker lock(&s_downgradeLock);
    s_downgradedHosts.clear();
}

bool PrivacyRequestInterceptor::httpsOnlyEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_httpsOnly;
}

bool PrivacyRequestInterceptor::shouldWarnHttp(const QUrl &url)
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
    if (httpsFirst && isUpgradeCandidate(url))
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
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    if (!exceptions.contains(lowered)) {
        exceptions.append(lowered);
        settings.setValue(QLatin1String("httpsOnlyExceptions"), exceptions);
    }
    settings.endGroup();
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
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    QStringList exceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
    if (exceptions.removeAll(lowered) > 0)
        settings.setValue(QLatin1String("httpsOnlyExceptions"), exceptions);
    settings.endGroup();
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

bool PrivacyRequestInterceptor::shouldWarnFormPost(const QUrl &url)
{
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    // The https-first upgrade claims the request before it goes out —
    // the body then travels over TLS (or the submit fails into the
    // downgrade set, which makes the next attempt warn here).
    if (httpsFirstEnabled() && isUpgradeCandidate(url))
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

PrivacyRequestInterceptor::PrivacyRequestInterceptor(AdBlockNetwork *network, QObject *parent)
    : QWebEngineUrlRequestInterceptor(parent)
    , m_adBlock(new AdBlockRequestInterceptor(network, this))
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
    if (httpsFirst
        && info.resourceType() == QWebEngineUrlRequestInfo::ResourceTypeMainFrame
        && isUpgradeCandidate(url)) {
        QUrl https = url;
        https.setScheme(QLatin1String("https"));
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: https-first" << url << "->" << https;
#endif
        info.redirect(https);
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
        && shouldWarnHttp(url)) {
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
