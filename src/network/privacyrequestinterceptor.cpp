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
static int s_refererPolicy = PrivacyRequestInterceptor::RefererTrimmed;
static int s_securityLevel = PrivacyRequestInterceptor::Standard;
static bool s_blockPings = true;

// Session-scoped set of hosts whose https main-frame load failed —
// their http: requests stop being upgraded.  Written from the GUI
// thread (noteNavigationFailure), read on the IO thread.
static QMutex s_downgradeLock;
static QSet<QString> s_downgradedHosts;
// Bound so a hostile page cannot grow the set without limit.
static const int maxDowngradedHosts = 256;

void PrivacyRequestInterceptor::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    const bool httpsFirst = settings.value(QLatin1String("httpsFirst"), true).toBool();
    const int refererPolicy = storedRefererPolicy();
    const int securityLevel = qBound(
        int(PrivacyRequestInterceptor::Standard),
        settings.value(QLatin1String("securityLevel"),
                       int(PrivacyRequestInterceptor::Standard)).toInt(),
        int(PrivacyRequestInterceptor::Safest));
    const bool blockPings =
        settings.value(QLatin1String("blockPings"), true).toBool();
    settings.endGroup();
    QWriteLocker lock(&s_policyLock);
    s_httpsFirst = httpsFirst;
    s_refererPolicy = refererPolicy;
    s_securityLevel = securityLevel;
    s_blockPings = blockPings;
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
