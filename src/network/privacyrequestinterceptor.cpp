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
#include "navigationpolicy.h"
#include "scriptcontrolmanager.h"

#include <qatomic.h>
#include <qsettings.h>
#include <qwebengineprofile.h>
#include <qwebengineurlrequestinfo.h>

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

// #define PRIVACYINTERCEPTOR_DEBUG
#if defined(PRIVACYINTERCEPTOR_DEBUG)
#include <qdebug.h>
#endif

// ENG02: this class is now a thin ADAPTER — it marshals each request
// into an engine-neutral PolicyRequest, delegates EVERY decision to
// Engine::NavigationPolicy (rustcore when linked, the Qt fallback
// otherwise), and applies the verdict.  The adblock matcher remains
// the delegated tail stage on an "allow" verdict.  No branching
// policy logic lives here anymore — settings storage (QSettings)
// and the JSCTL lookup stay Qt-side as inputs, never as decisions.

using Engine::HeaderList;
using Engine::NavigationPolicy;
using Engine::PolicyRequest;
using Engine::PolicyVerdict;

// PDF01 audit probe — counts every request the interceptor sees.
static QAtomicInteger<qint64> s_requestsSeen;

static HeaderList toHeaderList(const QHash<QByteArray, QByteArray> &headers)
{
    HeaderList list;
    list.reserve(headers.size());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it)
        list.append(qMakePair(it.key(), it.value()));
    return list;
}

static PolicyRequest buildRequest(QWebEngineUrlRequestInfo &info,
                                  const QString &scope, bool torMode,
                                  int minRefererLevel)
{
    PolicyRequest req;
    req.url = info.requestUrl();
    req.firstPartyUrl = info.firstPartyUrl();
    req.resourceType = static_cast<int>(info.resourceType());
    req.method = info.requestMethod();
    req.headers = toHeaderList(info.httpHeaders());
    req.scope = scope;
    req.torMode = torMode;
    req.minRefererLevel = minRefererLevel;
    // The JSCTL per-site grant is data the policy cannot reach —
    // the caller resolves it (only script-execution types need it).
    switch (info.resourceType()) {
    case QWebEngineUrlRequestInfo::ResourceTypeScript:
    case QWebEngineUrlRequestInfo::ResourceTypeWorker:
    case QWebEngineUrlRequestInfo::ResourceTypeSharedWorker:
    case QWebEngineUrlRequestInfo::ResourceTypeServiceWorker:
        req.scriptAllowed = ScriptControlManager::isAllowedHostSnapshot(
            req.firstPartyUrl.host());
        break;
    default:
        break;
    }
    return req;
}

// Applies the verdict; returns false when the request was terminated
// (blocked or redirected), true when the delegated matcher tail
// should run.
static bool applyVerdict(const PolicyVerdict &verdict,
                         QWebEngineUrlRequestInfo &info)
{
    switch (verdict.action) {
    case PolicyVerdict::Action::Pass:
        return false;
    case PolicyVerdict::Action::Block:
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: block"
                 << info.requestUrl() << "reason" << verdict.reason;
#endif
        info.block(true);
        return false;
    case PolicyVerdict::Action::Redirect:
#if defined(PRIVACYINTERCEPTOR_DEBUG)
        qDebug() << "PrivacyRequestInterceptor: redirect"
                 << info.requestUrl() << "->" << verdict.redirectUrl
                 << verdict.reason;
#endif
        info.redirect(verdict.redirectUrl);
        return false;
    case PolicyVerdict::Action::Allow:
        if (verdict.refererSet)
            info.setHttpHeader("Referer", verdict.refererValue);
        return true;
    }
    return true;
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
    // PDF01 audit probe — counts every request the interceptor sees.
    s_requestsSeen.fetchAndAddRelaxed(1);

    // Runs on the WebEngine IO thread — only the lock-guarded policy
    // state may be read here (inside NavigationPolicy / rustcore).
    const PolicyRequest req =
        buildRequest(info, m_scope, /*torMode=*/false,
                     RefererEngineDefault);
    const PolicyVerdict verdict = NavigationPolicy::evaluate(req);
    if (applyVerdict(verdict, info)) {
        // "allow" — the adblock matcher is the delegated tail stage.
        m_adBlock->interceptRequest(info);
    }
}

void PrivacyRequestInterceptor::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));

    NavigationPolicy::Snapshot snap;
    snap.httpsFirst = settings.value(QLatin1String("httpsFirst"), true).toBool();
    snap.httpsOnly = settings.value(QLatin1String("httpsOnly"), true).toBool();
    snap.refererPolicy = storedRefererPolicy();
    snap.securityLevel = qBound(
        int(PrivacyRequestInterceptor::Standard),
        settings.value(QLatin1String("securityLevel"),
                       int(PrivacyRequestInterceptor::Standard)).toInt(),
        int(PrivacyRequestInterceptor::Safest));
    snap.blockPings = settings.value(QLatin1String("blockPings"), true).toBool();
    snap.blockRemoteFonts =
        settings.value(QLatin1String("blockRemoteFonts"), false).toBool();
    snap.blockPrefetch =
        settings.value(QLatin1String("blockPrefetch"), true).toBool();
    snap.blockThirdPartyWebSockets =
        settings.value(QLatin1String("blockThirdPartyWebSockets"), false).toBool();
    snap.stripTrackingParams =
        settings.value(QLatin1String("stripTrackingParams"), true).toBool();
    snap.domainBlocklist =
        settings.value(QLatin1String("domainBlocklist"), true).toBool();
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
            snap.httpsOnlyExceptions.append(lowered);
    }
#else
    snap.httpsOnlyExceptions =
        settings.value(QLatin1String("httpsOnlyExceptions")).toStringList();
#endif
    settings.endGroup();

    NavigationPolicy::loadSnapshot(snap);
}

QString PrivacyRequestInterceptor::downgradeScope(
        const QWebEngineProfile *profile)
{
    if (!profile)
        return QString();
    if (!profile->isOffTheRecord())
        return profile->storageName();
    // Unnamed/off-the-record profiles carry no storageName — the
    // pointer is the only identity; marks die with the process anyway.
    return QLatin1String("otr:")
        + QString::number(reinterpret_cast<quintptr>(profile));
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

// ---- delegated decision surface (1:1 with the policy) -----------------

bool PrivacyRequestInterceptor::httpsFirstEnabled()
{
    return NavigationPolicy::httpsFirstEnabled();
}

bool PrivacyRequestInterceptor::trimRefererEnabled()
{
    return NavigationPolicy::refererPolicy() != RefererEngineDefault;
}

int PrivacyRequestInterceptor::refererPolicy()
{
    return NavigationPolicy::refererPolicy();
}

QByteArray PrivacyRequestInterceptor::referrerMetaValue(int level)
{
    return NavigationPolicy::referrerMetaValue(level);
}

int PrivacyRequestInterceptor::securityLevel()
{
    return NavigationPolicy::securityLevel();
}

bool PrivacyRequestInterceptor::blockPingsEnabled()
{
    return NavigationPolicy::blockPingsEnabled();
}

bool PrivacyRequestInterceptor::isPingTelemetryRequest(
        QWebEngineUrlRequestInfo &info)
{
    return NavigationPolicy::isPingTelemetry(
        static_cast<int>(info.resourceType()), toHeaderList(info.httpHeaders()));
}

bool PrivacyRequestInterceptor::blockRemoteFontsEnabled()
{
    return NavigationPolicy::blockRemoteFontsEnabled();
}

bool PrivacyRequestInterceptor::blockPrefetchEnabled()
{
    return NavigationPolicy::blockPrefetchEnabled();
}

bool PrivacyRequestInterceptor::blockThirdPartyWebSocketsEnabled()
{
    return NavigationPolicy::blockThirdPartyWebSocketsEnabled();
}

bool PrivacyRequestInterceptor::stripTrackingParamsEnabled()
{
    return NavigationPolicy::stripTrackingParamsEnabled();
}

bool PrivacyRequestInterceptor::isDomainBlocked(const QString &host)
{
    return NavigationPolicy::isDomainBlocked(host);
}

bool PrivacyRequestInterceptor::domainBlocklistEnabled()
{
    return NavigationPolicy::domainBlocklistEnabled();
}

bool PrivacyRequestInterceptor::shouldBlockDomain(const QUrl &url)
{
    return NavigationPolicy::shouldBlockDomain(url);
}

bool PrivacyRequestInterceptor::isBlockedDomainAllowed(const QString &host)
{
    return NavigationPolicy::isBlockedDomainAllowed(host);
}

void PrivacyRequestInterceptor::allowBlockedDomain(const QString &host)
{
    NavigationPolicy::allowBlockedDomain(host);
}

void PrivacyRequestInterceptor::clearBlockedDomainAllowance(
        const QString &host)
{
    NavigationPolicy::clearBlockedDomainAllowance(host);
}

void PrivacyRequestInterceptor::clearBlockedDomainAllowances()
{
    NavigationPolicy::clearBlockedDomainAllowances();
}

void PrivacyRequestInterceptor::recordBlockedDomainNav(const QUrl &url)
{
    NavigationPolicy::recordBlockedDomainNav(url);
}

bool PrivacyRequestInterceptor::takeBlockedDomainNav(const QUrl &url)
{
    return NavigationPolicy::takeBlockedDomainNav(url);
}

QUrl PrivacyRequestInterceptor::strippedUrl(const QUrl &url)
{
    return NavigationPolicy::strippedUrl(url);
}

bool PrivacyRequestInterceptor::shouldBlockResource(
        QWebEngineUrlRequestInfo::ResourceType type,
        const QHash<QByteArray, QByteArray> &headers)
{
    return NavigationPolicy::shouldBlockResource(
        static_cast<int>(type), toHeaderList(headers));
}

bool PrivacyRequestInterceptor::isPrivateOrLocalHost(const QString &host)
{
    return NavigationPolicy::isPrivateOrLocalHost(host);
}

bool PrivacyRequestInterceptor::failureImpliesDowngrade(int errorDomain,
                                                        int errorCode)
{
    return NavigationPolicy::failureImpliesDowngrade(errorDomain, errorCode);
}

bool PrivacyRequestInterceptor::isUpgradeCandidate(const QUrl &url,
                                                   const QString &scope)
{
    return NavigationPolicy::isUpgradeCandidate(url, scope);
}

bool PrivacyRequestInterceptor::isDowngraded(const QString &host,
                                             const QString &scope)
{
    return NavigationPolicy::isDowngraded(host, scope);
}

bool PrivacyRequestInterceptor::noteNavigationFailure(const QUrl &url,
        int errorDomain, int errorCode, const QString &scope)
{
    return NavigationPolicy::noteNavigationFailure(url, errorDomain,
                                                   errorCode, scope);
}

void PrivacyRequestInterceptor::markDowngraded(const QString &host,
                                               const QString &scope)
{
    NavigationPolicy::markDowngraded(host, scope);
}

void PrivacyRequestInterceptor::clearDowngradedHost(const QString &host,
                                                    const QString &scope)
{
    NavigationPolicy::clearDowngradedHost(host, scope);
}

void PrivacyRequestInterceptor::clearDowngradedHosts()
{
    NavigationPolicy::clearDowngradedHosts();
}

qint64 PrivacyRequestInterceptor::downgradeTtlMs()
{
    return NavigationPolicy::downgradeTtlMs();
}

void PrivacyRequestInterceptor::setDowngradeTtlMs(qint64 ms)
{
    NavigationPolicy::setDowngradeTtlMs(ms);
}

bool PrivacyRequestInterceptor::httpsOnlyEnabled()
{
    return NavigationPolicy::httpsOnlyEnabled();
}

bool PrivacyRequestInterceptor::shouldWarnHttp(const QUrl &url,
                                               const QString &scope)
{
    return NavigationPolicy::shouldWarnHttp(url, scope);
}

bool PrivacyRequestInterceptor::isHttpAllowedHost(const QString &host)
{
    return NavigationPolicy::isHttpAllowedHost(host);
}

void PrivacyRequestInterceptor::allowHttpForHost(const QString &host,
                                                 bool persistent)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return;
    // The policy keeps the host sets; the decision store (or
    // QSettings in no-rust builds) persists them.
    const bool persistNeeded =
        NavigationPolicy::allowHttpForHost(lowered, persistent);
    if (!persistent || !persistNeeded)
        return;
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
}

void PrivacyRequestInterceptor::clearHttpAllowance(const QString &host)
{
    const QString lowered = host.toLower();
    NavigationPolicy::clearHttpAllowance(lowered);
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
    return NavigationPolicy::httpExceptionHosts();
}

void PrivacyRequestInterceptor::recordBlockedHttpNav(const QUrl &url)
{
    NavigationPolicy::recordBlockedHttpNav(url);
}

bool PrivacyRequestInterceptor::takeBlockedHttpNav(const QUrl &url)
{
    return NavigationPolicy::takeBlockedHttpNav(url);
}

bool PrivacyRequestInterceptor::shouldWarnFormPost(const QUrl &url,
                                                   const QString &scope)
{
    return NavigationPolicy::shouldWarnFormPost(url, scope);
}

bool PrivacyRequestInterceptor::shouldBlockScript(
        const QUrl &firstPartyUrl,
        QWebEngineUrlRequestInfo::ResourceType type)
{
    const bool scriptAllowed = ScriptControlManager::isAllowedHostSnapshot(
        firstPartyUrl.host());
    return NavigationPolicy::shouldBlockScript(
        firstPartyUrl, static_cast<int>(type), scriptAllowed);
}

bool PrivacyRequestInterceptor::shouldBlockWebSocket(
        const QUrl &firstPartyUrl, const QUrl &requestUrl,
        QWebEngineUrlRequestInfo::ResourceType type)
{
    return NavigationPolicy::shouldBlockWebSocket(
        firstPartyUrl, requestUrl, static_cast<int>(type));
}

void PrivacyRequestInterceptor::applyRefererPolicy(
        QWebEngineUrlRequestInfo &info, int minimumLevel)
{
    const QByteArray refHeader = info.httpHeaders().value("Referer");
    QByteArray value;
    if (NavigationPolicy::refererForRequest(
            QString::fromUtf8(info.requestUrl().toEncoded()),
            refHeader, minimumLevel, value)) {
        info.setHttpHeader("Referer", value);
    }
}

QByteArray PrivacyRequestInterceptor::rewrittenReferer(
        int level, const QUrl &source, const QUrl &target)
{
    return NavigationPolicy::rewrittenReferer(level, source, target);
}

qint64 PrivacyRequestInterceptor::requestsSeen()
{
    return s_requestsSeen.loadRelaxed();
}

void PrivacyRequestInterceptor::resetRequestsSeen()
{
    s_requestsSeen.storeRelaxed(0);
}
