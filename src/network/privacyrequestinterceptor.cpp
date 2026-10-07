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
static bool s_trimReferer = true;

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
    const bool trimReferer = settings.value(QLatin1String("trimReferer"), true).toBool();
    settings.endGroup();
    QWriteLocker lock(&s_policyLock);
    s_httpsFirst = httpsFirst;
    s_trimReferer = trimReferer;
}

bool PrivacyRequestInterceptor::httpsFirstEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_httpsFirst;
}

bool PrivacyRequestInterceptor::trimRefererEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_trimReferer;
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

// Cross-site requests carry the *target's* origin as Referer so the
// referring page's identity never leaves (the uBlock Origin referrer
// spoof trick).  Honest bound: the header write is only honored for
// navigation requests — Chromium composes the Referer for subresource
// loads after the interceptor runs, so their referrer falls back to
// its built-in strict-origin-when-cross-origin policy (referring
// origin still leaks; only full-path leaking is prevented there).
// The header is written unconditionally for cross-site navigations
// rather than only when one is visible, since the outgoing value is
// not what httpHeaders() shows; the spoofed value is information the
// destination already knows about itself.  Same-site requests keep
// the full header untouched.
static void trimRefererHeader(QWebEngineUrlRequestInfo &info)
{
    const QUrl firstParty = info.firstPartyUrl();
    const QUrl url = info.requestUrl();
    if (!firstParty.isValid() || firstParty.isEmpty()
        || sameSite(firstParty.host(), url.host()))
        return;
    if (url.scheme() != QLatin1String("http")
        && url.scheme() != QLatin1String("https"))
        return;
    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(url.host());
    if (url.port() != -1)
        origin.setPort(url.port());
    info.setHttpHeader("Referer",
                       (origin.toString() + QLatin1Char('/')).toUtf8());
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
    bool httpsFirst, trimReferer;
    {
        QReadLocker lock(&s_policyLock);
        httpsFirst = s_httpsFirst;
        trimReferer = s_trimReferer;
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

    if (trimReferer)
        trimRefererHeader(info);

    m_adBlock->interceptRequest(info);
}
