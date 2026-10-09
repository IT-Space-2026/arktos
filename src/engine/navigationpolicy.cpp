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

#include "navigationpolicy.h"

#if defined(ARORA_RUSTCORE)

// ============================================================
// Rust-backed implementation — every decision is delegated to the
// rustcore policy module via the C ABI.  This file only marshals.
// Fail-open everywhere: an FFI error yields the least restrictive
// verdict (allow/pass), never a phantom block.
// ============================================================

#include <rustcore.h>

#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

namespace Engine {

static QByteArray jsonBytes(const QJsonObject &obj)
{
    return QJsonDocument(obj).toJson(QJsonDocument::Compact);
}

static QJsonArray headersToJson(const HeaderList &headers)
{
    QJsonArray arr;
    for (const auto &pair : headers) {
        QJsonArray kv;
        kv.append(QString::fromUtf8(pair.first));
        kv.append(QString::fromUtf8(pair.second));
        arr.append(kv);
    }
    return arr;
}

// Generic granular endpoint — returns the decoded JSON result, or an
// invalid QVariant on error (call sites then apply their fail-open
// default).
static QVariant policyCall(const QString &op, const QJsonObject &args)
{
    QJsonObject payload = args;
    payload[QStringLiteral("op")] = op;
    const QByteArray body = jsonBytes(payload);
    char *out = rc_policy_call(
        reinterpret_cast<const uint8_t *>(body.constData()),
        static_cast<size_t>(body.size()));
    if (!out)
        return QVariant();
    const QByteArray raw(out);
    rc_string_free(out);
    // QJsonDocument only accepts top-level objects and arrays; ops may
    // legitimately answer with a bare bool, number, or string.  Wrapping
    // the reply in an array decodes every shape uniformly.
    const QJsonDocument doc = QJsonDocument::fromJson(
        QByteArrayLiteral("[") + raw + QByteArrayLiteral("]"));
    if (!doc.isArray() || doc.array().isEmpty())
        return QVariant();
    return doc.array().first().toVariant();
}

static bool callBool(const QString &op, const QJsonObject &args,
                     bool fallback)
{
    const QVariant v = policyCall(op, args);
    return v.isValid() ? v.toBool() : fallback;
}

static QJsonObject urlScopeArgs(const QUrl &url, const QString &scope)
{
    QJsonObject a;
    a[QStringLiteral("url")] = QString::fromUtf8(url.toEncoded());
    a[QStringLiteral("scope")] = scope;
    return a;
}

static QJsonObject hostScopeArgs(const QString &host, const QString &scope)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    a[QStringLiteral("scope")] = scope;
    return a;
}

PolicyVerdict NavigationPolicy::evaluate(const PolicyRequest &request)
{
    QJsonObject manifest;
    manifest[QStringLiteral("url")] = QString::fromUtf8(request.url.toEncoded());
    manifest[QStringLiteral("first_party_url")] =
        QString::fromUtf8(request.firstPartyUrl.toEncoded());
    manifest[QStringLiteral("resource_type")] = request.resourceType;
    manifest[QStringLiteral("method")] = QString::fromUtf8(request.method);
    manifest[QStringLiteral("headers")] = headersToJson(request.headers);
    manifest[QStringLiteral("scope")] = request.scope;
    manifest[QStringLiteral("tor_mode")] = request.torMode;
    manifest[QStringLiteral("script_allowed")] = request.scriptAllowed;
    manifest[QStringLiteral("min_referer_level")] = request.minRefererLevel;

    const QByteArray body = jsonBytes(manifest);
    char *out = rc_policy_evaluate(
        reinterpret_cast<const uint8_t *>(body.constData()),
        static_cast<size_t>(body.size()));

    PolicyVerdict verdict;
    if (!out) {
        // Fail open — a broken core must never block traffic.
        verdict.action = PolicyVerdict::Action::Allow;
        return verdict;
    }
    const QByteArray raw(out);
    rc_string_free(out);
    const QJsonObject v = QJsonDocument::fromJson(raw).object();
    const QString action = v.value(QStringLiteral("action")).toString();

    if (action == QLatin1String("block")) {
        verdict.action = PolicyVerdict::Action::Block;
        verdict.reason = v.value(QStringLiteral("reason")).toString().toUtf8();
    } else if (action == QLatin1String("redirect")) {
        verdict.action = PolicyVerdict::Action::Redirect;
        verdict.redirectUrl = QUrl::fromEncoded(
            v.value(QStringLiteral("url")).toString().toUtf8());
        verdict.reason = v.value(QStringLiteral("reason")).toString().toUtf8();
        if (!verdict.redirectUrl.isValid()) {
            verdict.action = PolicyVerdict::Action::Allow; // fail open
        }
    } else if (action == QLatin1String("allow")) {
        verdict.action = PolicyVerdict::Action::Allow;
        const QJsonObject referer =
            v.value(QStringLiteral("referer")).toObject();
        if (referer.value(QStringLiteral("op")).toString()
                == QLatin1String("set")) {
            verdict.refererSet = true;
            verdict.refererValue =
                referer.value(QStringLiteral("value")).toString().toUtf8();
        }
    }
    // "pass" or anything unexpected → untouched.
    return verdict;
}

bool NavigationPolicy::cookieFilter(const CookieGateInput &input)
{
    QJsonObject m;
    m[QStringLiteral("host")] = input.host;
    m[QStringLiteral("third_party")] = input.thirdParty;
    m[QStringLiteral("block_3p")] = input.blockThirdParty;
    m[QStringLiteral("accept_policy")] = input.acceptPolicy;
    QJsonArray block, allow, session;
    for (const QString &h : input.block) block.append(h);
    for (const QString &h : input.allow) allow.append(h);
    for (const QString &h : input.allowForSession) session.append(h);
    m[QStringLiteral("block")] = block;
    m[QStringLiteral("allow")] = allow;
    m[QStringLiteral("allow_session")] = session;

    const QByteArray body = jsonBytes(m);
    const int r = rc_policy_cookie_filter(
        reinterpret_cast<const uint8_t *>(body.constData()),
        static_cast<size_t>(body.size()));
    return r != 0;   // -1 (error) fails open → accept
}

void NavigationPolicy::loadSnapshot(const Snapshot &s)
{
    QJsonObject m;
    m[QStringLiteral("https_first")] = s.httpsFirst;
    m[QStringLiteral("https_only")] = s.httpsOnly;
    m[QStringLiteral("referer_policy")] = s.refererPolicy;
    m[QStringLiteral("security_level")] = s.securityLevel;
    m[QStringLiteral("block_pings")] = s.blockPings;
    m[QStringLiteral("block_remote_fonts")] = s.blockRemoteFonts;
    m[QStringLiteral("block_prefetch")] = s.blockPrefetch;
    m[QStringLiteral("block_third_party_ws")] = s.blockThirdPartyWebSockets;
    m[QStringLiteral("strip_tracking_params")] = s.stripTrackingParams;
    m[QStringLiteral("domain_blocklist")] = s.domainBlocklist;
    QJsonArray exceptions;
    for (const QString &h : s.httpsOnlyExceptions) exceptions.append(h);
    m[QStringLiteral("https_only_exceptions")] = exceptions;
    const QByteArray body = jsonBytes(m);
    rc_policy_load_snapshot(
        reinterpret_cast<const uint8_t *>(body.constData()),
        static_cast<size_t>(body.size()));
}

static bool flagValue(const char *name, bool fallback)
{
    QJsonObject a;
    a[QStringLiteral("name")] = QString::fromUtf8(name);
    return callBool(QStringLiteral("flag"), a, fallback);
}

static int intFlagValue(const char *name, int fallback)
{
    QJsonObject a;
    a[QStringLiteral("name")] = QString::fromUtf8(name);
    const QVariant v = policyCall(QStringLiteral("flag"), a);
    return v.isValid() ? v.toInt() : fallback;
}

bool NavigationPolicy::httpsFirstEnabled() { return flagValue("https_first", true); }
bool NavigationPolicy::httpsOnlyEnabled() { return flagValue("https_only", true); }
int NavigationPolicy::refererPolicy() { return intFlagValue("referer_policy", 1); }
int NavigationPolicy::securityLevel() { return intFlagValue("security_level", 0); }
bool NavigationPolicy::blockPingsEnabled() { return flagValue("block_pings", true); }
bool NavigationPolicy::blockRemoteFontsEnabled() { return flagValue("block_remote_fonts", false); }
bool NavigationPolicy::blockPrefetchEnabled() { return flagValue("block_prefetch", true); }
bool NavigationPolicy::blockThirdPartyWebSocketsEnabled() { return flagValue("block_third_party_ws", false); }
bool NavigationPolicy::stripTrackingParamsEnabled() { return flagValue("strip_tracking_params", true); }
bool NavigationPolicy::domainBlocklistEnabled() { return flagValue("domain_blocklist", true); }

QByteArray NavigationPolicy::referrerMetaValue(int level)
{
    QJsonObject a;
    a[QStringLiteral("level")] = level;
    const QVariant v = policyCall(QStringLiteral("referrer_meta"), a);
    return v.isValid() ? v.toString().toUtf8() : QByteArray();
}

QByteArray NavigationPolicy::rewrittenReferer(int level, const QUrl &source,
                                              const QUrl &target)
{
    QJsonObject a;
    a[QStringLiteral("level")] = level;
    a[QStringLiteral("source")] = source.toString();
    a[QStringLiteral("target")] = target.toString();
    const QVariant v = policyCall(QStringLiteral("rewritten_referer"), a);
    return v.isValid() ? v.toString().toUtf8() : source.toString().toUtf8();
}

bool NavigationPolicy::refererForRequest(const QString &url,
                                         const QByteArray &refererHeader,
                                         int minLevel, QByteArray &outValue)
{
    QJsonObject a;
    a[QStringLiteral("url")] = url;
    a[QStringLiteral("min_level")] = minLevel;
    if (!refererHeader.isEmpty()) {
        QJsonArray headers;
        QJsonArray kv;
        kv.append(QStringLiteral("Referer"));
        kv.append(QString::fromUtf8(refererHeader));
        headers.append(kv);
        a[QStringLiteral("headers")] = headers;
    }
    const QVariant v = policyCall(QStringLiteral("referer_apply"), a);
    if (!v.isValid()) {
        outValue.clear();
        return false;
    }
    const QJsonObject r = v.toJsonObject();
    if (r.value(QStringLiteral("op")).toString() != QLatin1String("set"))
        return false;
    outValue = r.value(QStringLiteral("value")).toString().toUtf8();
    return true;
}

bool NavigationPolicy::isUpgradeCandidate(const QUrl &url, const QString &scope)
{
    return callBool(QStringLiteral("upgrade_candidate"),
                    urlScopeArgs(url, scope), false);
}

bool NavigationPolicy::shouldWarnHttp(const QUrl &url, const QString &scope)
{
    return callBool(QStringLiteral("warn_http"),
                    urlScopeArgs(url, scope), false);
}

bool NavigationPolicy::shouldWarnFormPost(const QUrl &url, const QString &scope)
{
    return callBool(QStringLiteral("warn_form_post"),
                    urlScopeArgs(url, scope), false);
}

bool NavigationPolicy::isHttpAllowedHost(const QString &host)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    return callBool(QStringLiteral("is_http_allowed"), a, false);
}

bool NavigationPolicy::allowHttpForHost(const QString &host, bool persistent)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    a[QStringLiteral("persistent")] = persistent;
    const QVariant v = policyCall(QStringLiteral("allow_http"), a);
    if (!v.isValid())
        return false;
    return v.toJsonObject()
        .value(QStringLiteral("persist_needed"))
        .toBool();
}

void NavigationPolicy::clearHttpAllowance(const QString &host)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    policyCall(QStringLiteral("clear_http_allowance"), a);
}

QStringList NavigationPolicy::httpExceptionHosts()
{
    const QVariant v = policyCall(QStringLiteral("http_exceptions"),
                                  QJsonObject());
    return v.isValid() ? v.toStringList() : QStringList();
}

bool NavigationPolicy::isDowngraded(const QString &host, const QString &scope)
{
    return callBool(QStringLiteral("is_downgraded"),
                    hostScopeArgs(host, scope), false);
}

void NavigationPolicy::markDowngraded(const QString &host, const QString &scope)
{
    policyCall(QStringLiteral("mark_downgraded"),
               hostScopeArgs(host, scope));
}

void NavigationPolicy::clearDowngradedHost(const QString &host,
                                           const QString &scope)
{
    policyCall(QStringLiteral("clear_downgraded"),
               hostScopeArgs(host, scope));
}

void NavigationPolicy::clearDowngradedHosts()
{
    policyCall(QStringLiteral("clear_all_downgraded"), QJsonObject());
}

qint64 NavigationPolicy::downgradeTtlMs()
{
    const QVariant v = policyCall(QStringLiteral("ttl_get"), QJsonObject());
    return v.isValid() ? v.toLongLong() : 30 * 60 * 1000;
}

void NavigationPolicy::setDowngradeTtlMs(qint64 ms)
{
    QJsonObject a;
    a[QStringLiteral("ms")] = static_cast<double>(ms);
    policyCall(QStringLiteral("ttl_set"), a);
}

bool NavigationPolicy::failureImpliesDowngrade(int errorDomain, int errorCode)
{
    QJsonObject a;
    a[QStringLiteral("domain")] = errorDomain;
    a[QStringLiteral("code")] = errorCode;
    return callBool(QStringLiteral("failure_implies_downgrade"), a, false);
}

bool NavigationPolicy::noteNavigationFailure(const QUrl &url, int errorDomain,
                                             int errorCode, const QString &scope)
{
    QJsonObject a = urlScopeArgs(url, scope);
    a[QStringLiteral("domain")] = errorDomain;
    a[QStringLiteral("code")] = errorCode;
    return callBool(QStringLiteral("note_nav_failure"), a, false);
}

static void recordNav(const char *kind, const QUrl &url)
{
    QJsonObject a;
    a[QStringLiteral("kind")] = QString::fromUtf8(kind);
    a[QStringLiteral("url")] = QString::fromUtf8(url.toEncoded());
    policyCall(QStringLiteral("record_blocked_nav"), a);
}

static bool takeNav(const char *kind, const QUrl &url)
{
    QJsonObject a;
    a[QStringLiteral("kind")] = QString::fromUtf8(kind);
    a[QStringLiteral("url")] = QString::fromUtf8(url.toEncoded());
    return callBool(QStringLiteral("take_blocked_nav"), a, false);
}

void NavigationPolicy::recordBlockedHttpNav(const QUrl &url)
{
    recordNav("http", url);
}

bool NavigationPolicy::takeBlockedHttpNav(const QUrl &url)
{
    return takeNav("http", url);
}

void NavigationPolicy::recordBlockedDomainNav(const QUrl &url)
{
    recordNav("domain", url);
}

bool NavigationPolicy::takeBlockedDomainNav(const QUrl &url)
{
    return takeNav("domain", url);
}

bool NavigationPolicy::isDomainBlocked(const QString &host)
{
    if (host.isEmpty())
        return false;
    return rc_blocklist_check(host.toUtf8().constData()) == 1;
}

bool NavigationPolicy::shouldBlockDomain(const QUrl &url)
{
    QJsonObject a;
    a[QStringLiteral("url")] = QString::fromUtf8(url.toEncoded());
    return callBool(QStringLiteral("block_domain"), a, false);
}

bool NavigationPolicy::isBlockedDomainAllowed(const QString &host)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    return callBool(QStringLiteral("is_blocked_domain_allowed"), a, false);
}

void NavigationPolicy::allowBlockedDomain(const QString &host)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    policyCall(QStringLiteral("allow_blocked_domain"), a);
}

void NavigationPolicy::clearBlockedDomainAllowance(const QString &host)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    policyCall(QStringLiteral("clear_blocked_domain_allowance"), a);
}

void NavigationPolicy::clearBlockedDomainAllowances()
{
    policyCall(QStringLiteral("clear_all_blocked_domain"), QJsonObject());
}

bool NavigationPolicy::isPingTelemetry(int resourceType,
                                       const HeaderList &headers)
{
    QJsonObject a;
    a[QStringLiteral("type")] = resourceType;
    a[QStringLiteral("headers")] = headersToJson(headers);
    return callBool(QStringLiteral("is_ping"), a, false);
}

bool NavigationPolicy::shouldBlockResource(int resourceType,
                                           const HeaderList &headers)
{
    QJsonObject a;
    a[QStringLiteral("type")] = resourceType;
    a[QStringLiteral("headers")] = headersToJson(headers);
    return callBool(QStringLiteral("block_resource"), a, false);
}

bool NavigationPolicy::shouldBlockWebSocket(const QUrl &firstPartyUrl,
                                            const QUrl &requestUrl,
                                            int resourceType)
{
    QJsonObject a;
    a[QStringLiteral("first_party_url")] =
        QString::fromUtf8(firstPartyUrl.toEncoded());
    a[QStringLiteral("url")] = QString::fromUtf8(requestUrl.toEncoded());
    a[QStringLiteral("type")] = resourceType;
    return callBool(QStringLiteral("block_ws"), a, false);
}

bool NavigationPolicy::shouldBlockScript(const QUrl &firstPartyUrl,
                                         int resourceType, bool scriptAllowed)
{
    QJsonObject a;
    a[QStringLiteral("first_party_url")] =
        QString::fromUtf8(firstPartyUrl.toEncoded());
    a[QStringLiteral("type")] = resourceType;
    a[QStringLiteral("script_allowed")] = scriptAllowed;
    return callBool(QStringLiteral("block_script"), a, false);
}

bool NavigationPolicy::isPrivateOrLocalHost(const QString &host)
{
    QJsonObject a;
    a[QStringLiteral("host")] = host;
    return callBool(QStringLiteral("private_or_local"), a, true);
}

QUrl NavigationPolicy::strippedUrl(const QUrl &url)
{
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
}

} // namespace Engine

#else // !ARORA_RUSTCORE — legacy Qt fallback, kept 1:1 with the pre-refactor code

#include <qdatetime.h>
#include <qhostaddress.h>
#include <qmutex.h>
#include <qreadwritelock.h>
#include <qset.h>
#include <qurl.h>

namespace Engine {

// Snapshot statics — same defaults as the pre-refactor interceptor.
static QReadWriteLock s_policyLock;
static bool s_httpsFirst = true;
static bool s_httpsOnly = true;
static int s_refererPolicy = 1;   // RefererTrimmed
static int s_securityLevel = 0;   // Standard
static bool s_blockPings = true;
static bool s_blockRemoteFonts = false;
static bool s_blockPrefetch = true;
static bool s_blockThirdPartyWebSockets = false;
static bool s_stripTrackingParams = true;
static bool s_domainBlocklist = true;

static QMutex s_domainBlockLock;
static QSet<QString> s_sessionBlockedAllowed;
static QSet<QString> s_blockedDomainNavs;
static const int maxBlockedAllowedHosts = 256;
static const int maxBlockedDomainNavs = 64;

static QMutex s_downgradeLock;
static QHash<QString, QHash<QString, qint64> > s_downgradedHosts;
static qint64 s_downgradeTtlMs = 30 * 60 * 1000;
static const int maxDowngradedHosts = 256;

static QMutex s_httpAllowLock;
static QSet<QString> s_sessionHttpAllowed;
static QSet<QString> s_persistedHttpAllowed;
static const int maxHttpAllowedHosts = 256;

static QMutex s_blockedNavLock;
static QSet<QString> s_blockedHttpNavs;
static const int maxBlockedHttpNavs = 64;

// Resource-type ordinals (QWebEngineUrlRequestInfo::ResourceType).
static const int RT_MAIN_FRAME = 0;
static const int RT_SCRIPT = 3;
static const int RT_FONT = 5;
static const int RT_WORKER = 9;
static const int RT_SHARED_WORKER = 10;
static const int RT_PREFETCH = 11;
static const int RT_PING = 14;
static const int RT_SERVICE_WORKER = 15;
static const int RT_CSP_REPORT = 16;
static const int RT_WEBSOCKET = 254;

void NavigationPolicy::loadSnapshot(const Snapshot &snap)
{
    {
        QWriteLocker lock(&s_policyLock);
        s_httpsFirst = snap.httpsFirst;
        s_httpsOnly = snap.httpsOnly;
        s_refererPolicy = snap.refererPolicy;
        s_securityLevel = snap.securityLevel;
        s_blockPings = snap.blockPings;
        s_blockRemoteFonts = snap.blockRemoteFonts;
        s_blockPrefetch = snap.blockPrefetch;
        s_blockThirdPartyWebSockets = snap.blockThirdPartyWebSockets;
        s_stripTrackingParams = snap.stripTrackingParams;
        s_domainBlocklist = snap.domainBlocklist;
    }
    QSet<QString> persisted;
    for (const QString &host : snap.httpsOnlyExceptions) {
        const QString lowered = host.toLower();
        if (!lowered.isEmpty())
            persisted.insert(lowered);
    }
    const QMutexLocker lock(&s_httpAllowLock);
    s_persistedHttpAllowed = persisted;
}

bool NavigationPolicy::httpsFirstEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_httpsFirst;
}

bool NavigationPolicy::httpsOnlyEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_httpsOnly;
}

int NavigationPolicy::refererPolicy()
{
    QReadLocker lock(&s_policyLock);
    return s_refererPolicy;
}

int NavigationPolicy::securityLevel()
{
    QReadLocker lock(&s_policyLock);
    return s_securityLevel;
}

bool NavigationPolicy::blockPingsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockPings;
}

bool NavigationPolicy::blockRemoteFontsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockRemoteFonts;
}

bool NavigationPolicy::blockPrefetchEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockPrefetch;
}

bool NavigationPolicy::blockThirdPartyWebSocketsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_blockThirdPartyWebSockets;
}

bool NavigationPolicy::stripTrackingParamsEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_stripTrackingParams;
}

bool NavigationPolicy::domainBlocklistEnabled()
{
    QReadLocker lock(&s_policyLock);
    return s_domainBlocklist;
}

QByteArray NavigationPolicy::referrerMetaValue(int level)
{
    switch (level) {
    case 1: return QByteArrayLiteral("strict-origin");
    case 2: return QByteArrayLiteral("same-origin");
    case 3: return QByteArrayLiteral("no-referrer");
    default: return QByteArray();
    }
}

bool NavigationPolicy::isPingTelemetry(int resourceType,
                                       const HeaderList &headers)
{
    if (resourceType == RT_PING || resourceType == RT_CSP_REPORT)
        return true;
    for (const auto &pair : headers) {
        if (pair.first == "Sec-Fetch-Dest" && pair.second == "report")
            return true;
    }
    return false;
}

bool NavigationPolicy::shouldBlockResource(int resourceType,
                                           const HeaderList &headers)
{
    bool blockRemoteFonts, blockPrefetch;
    {
        QReadLocker lock(&s_policyLock);
        blockRemoteFonts = s_blockRemoteFonts;
        blockPrefetch = s_blockPrefetch;
    }
    if (resourceType == RT_FONT)
        return blockRemoteFonts;
    if (!blockPrefetch)
        return false;
    if (resourceType == RT_PREFETCH)
        return true;
    for (const auto &pair : headers) {
        const QByteArray key = pair.first.toLower();
        if ((key == "sec-purpose" || key == "purpose")
            && pair.second.toLower().contains("prefetch"))
            return true;
    }
    return false;
}

bool NavigationPolicy::isPrivateOrLocalHost(const QString &host)
{
    if (host.isEmpty())
        return true;
    QString lowered = host.toLower();
    if (lowered.startsWith(QLatin1Char('[')) && lowered.endsWith(QLatin1Char(']')))
        lowered = lowered.mid(1, lowered.size() - 2);
    if (lowered == QLatin1String("localhost")
        || lowered.endsWith(QLatin1String(".localhost"))
        || lowered.endsWith(QLatin1String(".local"))
        || lowered.endsWith(QLatin1String(".onion")))
        return true;
    const QHostAddress address(lowered);
    if (address.isNull())
        return false;
    if (address.isLoopback() || address.isMulticast())
        return true;
    static const QPair<QHostAddress, int> privateNets[] = {
        { QHostAddress(QLatin1String("10.0.0.0")), 8 },
        { QHostAddress(QLatin1String("172.16.0.0")), 12 },
        { QHostAddress(QLatin1String("192.168.0.0")), 16 },
        { QHostAddress(QLatin1String("169.254.0.0")), 16 },
        { QHostAddress(QLatin1String("100.64.0.0")), 10 },
        { QHostAddress(QLatin1String("198.18.0.0")), 15 },
        { QHostAddress(QLatin1String("fc00::")), 7 },
        { QHostAddress(QLatin1String("fe80::")), 10 },
    };
    for (const auto &net : privateNets) {
        if (address.isInSubnet(net.first, net.second))
            return true;
    }
    return false;
}

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

static bool markHostDowngraded(const QString &scope, const QString &host)
{
    const QMutexLocker lock(&s_downgradeLock);
    QHash<QString, qint64> &marks = s_downgradedHosts[scope];
    const auto it = marks.find(host);
    if (it != marks.end()) {
        it.value() = QDateTime::currentMSecsSinceEpoch();
        return false;
    }
    if (marks.size() >= maxDowngradedHosts)
        return false;
    marks.insert(host, QDateTime::currentMSecsSinceEpoch());
    return true;
}

bool NavigationPolicy::isUpgradeCandidate(const QUrl &url, const QString &scope)
{
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    if (isPrivateOrLocalHost(host))
        return false;
    return !isMarkedDowngraded(scope, host);
}

bool NavigationPolicy::isDowngraded(const QString &host, const QString &scope)
{
    return isMarkedDowngraded(scope, host);
}

bool NavigationPolicy::failureImpliesDowngrade(int errorDomain, int errorCode)
{
    // QWebEngineLoadingInfo::ConnectionErrorDomain == 2 — kept as a
    // literal so this file stays free of QtWebEngine types.
    if (errorDomain != 2)
        return false;
    switch (errorCode) {
    case -105: case -119: case -137: case -166:
    case -106: case -108: case -124: case -138: case -142: case -147:
    case -160: case -161: case -162: case -163: case -174: case -176:
    case -189: case -190:
    case -133: case -139: case -154:
    case -110: case -117: case -134: case -135: case -141: case -150:
    case -151: case -156: case -164: case -168: case -169: case -171:
    case -177:
    case -145: case -173:
    case -178: case -179:
    case -111: case -115: case -120: case -121: case -127: case -130:
    case -131: case -136: case -140: case -170: case -186: case -187:
    case -188:
        return false;
    }
    return true;
}

bool NavigationPolicy::noteNavigationFailure(const QUrl &url, int errorDomain,
                                             int errorCode, const QString &scope)
{
    if (!httpsFirstEnabled()
        || url.scheme() != QLatin1String("https")
        || !failureImpliesDowngrade(errorDomain, errorCode))
        return false;
    const QString host = url.host().toLower();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    return markHostDowngraded(scope, host);
}

void NavigationPolicy::markDowngraded(const QString &host, const QString &scope)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty() || isPrivateOrLocalHost(lowered))
        return;
    markHostDowngraded(scope, lowered);
}

void NavigationPolicy::clearDowngradedHost(const QString &host,
                                           const QString &scope)
{
    const QMutexLocker lock(&s_downgradeLock);
    const auto scopeIt = s_downgradedHosts.find(scope);
    if (scopeIt != s_downgradedHosts.end())
        scopeIt->remove(host.toLower());
}

void NavigationPolicy::clearDowngradedHosts()
{
    const QMutexLocker lock(&s_downgradeLock);
    s_downgradedHosts.clear();
}

qint64 NavigationPolicy::downgradeTtlMs()
{
    const QMutexLocker lock(&s_downgradeLock);
    return s_downgradeTtlMs;
}

void NavigationPolicy::setDowngradeTtlMs(qint64 ms)
{
    const QMutexLocker lock(&s_downgradeLock);
    s_downgradeTtlMs = ms;
}

bool NavigationPolicy::shouldWarnHttp(const QUrl &url, const QString &scope)
{
    bool httpsFirst, httpsOnly;
    {
        QReadLocker lock(&s_policyLock);
        httpsFirst = s_httpsFirst;
        httpsOnly = s_httpsOnly;
    }
    if (!httpsOnly || url.scheme() != QLatin1String("http"))
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
    if (httpsFirst && isUpgradeCandidate(url, scope))
        return false;
    return true;
}

bool NavigationPolicy::isHttpAllowedHost(const QString &host)
{
    const QString lowered = host.toLower();
    const QMutexLocker lock(&s_httpAllowLock);
    return s_sessionHttpAllowed.contains(lowered)
        || s_persistedHttpAllowed.contains(lowered);
}

bool NavigationPolicy::allowHttpForHost(const QString &host, bool persistent)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return false;
    const QMutexLocker lock(&s_httpAllowLock);
    if (s_sessionHttpAllowed.size() < maxHttpAllowedHosts)
        s_sessionHttpAllowed.insert(lowered);
    if (!persistent)
        return false;
    if (s_persistedHttpAllowed.contains(lowered))
        return false;
    s_persistedHttpAllowed.insert(lowered);
    return true;   // caller persists to QSettings
}

void NavigationPolicy::clearHttpAllowance(const QString &host)
{
    const QString lowered = host.toLower();
    const QMutexLocker lock(&s_httpAllowLock);
    s_sessionHttpAllowed.remove(lowered);
    s_persistedHttpAllowed.remove(lowered);
}

QStringList NavigationPolicy::httpExceptionHosts()
{
    const QMutexLocker lock(&s_httpAllowLock);
    QStringList hosts = s_persistedHttpAllowed.values();
    hosts.sort();
    return hosts;
}

void NavigationPolicy::recordBlockedHttpNav(const QUrl &url)
{
    const QMutexLocker lock(&s_blockedNavLock);
    if (s_blockedHttpNavs.size() >= maxBlockedHttpNavs)
        return;
    s_blockedHttpNavs.insert(QString::fromUtf8(url.toEncoded()));
}

bool NavigationPolicy::takeBlockedHttpNav(const QUrl &url)
{
    const QMutexLocker lock(&s_blockedNavLock);
    return s_blockedHttpNavs.remove(QString::fromUtf8(url.toEncoded()));
}

void NavigationPolicy::recordBlockedDomainNav(const QUrl &url)
{
    const QMutexLocker lock(&s_domainBlockLock);
    if (s_blockedDomainNavs.size() >= maxBlockedDomainNavs)
        return;
    s_blockedDomainNavs.insert(QString::fromUtf8(url.toEncoded()));
}

bool NavigationPolicy::takeBlockedDomainNav(const QUrl &url)
{
    const QMutexLocker lock(&s_domainBlockLock);
    return s_blockedDomainNavs.remove(QString::fromUtf8(url.toEncoded()));
}

bool NavigationPolicy::shouldWarnFormPost(const QUrl &url, const QString &scope)
{
    if (url.scheme() != QLatin1String("http"))
        return false;
    const QString host = url.host();
    if (host.isEmpty() || isPrivateOrLocalHost(host))
        return false;
    if (httpsFirstEnabled() && isUpgradeCandidate(url, scope))
        return false;
    return true;
}

bool NavigationPolicy::shouldBlockScript(const QUrl &firstPartyUrl,
                                         int resourceType, bool scriptAllowed)
{
    if (securityLevel() < 1)   // Safer
        return false;
    switch (resourceType) {
    case RT_SCRIPT: case RT_WORKER: case RT_SHARED_WORKER:
    case RT_SERVICE_WORKER:
        break;
    default:
        return false;
    }
    if (firstPartyUrl.scheme() != QLatin1String("http"))
        return false;
    if (scriptAllowed)
        return false;
    return !isLoopbackHost(firstPartyUrl.host());
}

bool NavigationPolicy::shouldBlockWebSocket(const QUrl &firstPartyUrl,
                                            const QUrl &requestUrl,
                                            int resourceType)
{
    if (resourceType != RT_WEBSOCKET)
        return false;
    if (!blockThirdPartyWebSocketsEnabled())
        return false;
    if (firstPartyUrl.host().isEmpty())
        return false;
    return !sameSite(firstPartyUrl.host(), requestUrl.host());
}

static QByteArray refererOrigin(const QUrl &url)
{
    QUrl origin;
    origin.setScheme(url.scheme());
    origin.setHost(url.host());
    if (url.port() != -1)
        origin.setPort(url.port());
    return (origin.toString() + QLatin1Char('/')).toUtf8();
}

QByteArray NavigationPolicy::rewrittenReferer(int level, const QUrl &source,
                                              const QUrl &target)
{
    if (level <= 0)
        return source.toString().toUtf8();
    if (level == 3)
        return QByteArray();
    if (source.scheme() == QLatin1String("https")
        && target.scheme() == QLatin1String("http"))
        return QByteArray();
    const bool crossSite = !sameSite(source.host(), target.host());
    if (crossSite) {
        if (level >= 2)
            return QByteArray();
        return refererOrigin(target);
    }
    return refererOrigin(source);
}

bool NavigationPolicy::refererForRequest(const QString &url,
                                         const QByteArray &refererHeader,
                                         int minLevel, QByteArray &outValue)
{
    outValue.clear();
    int level;
    {
        QReadLocker lock(&s_policyLock);
        level = qMax(s_refererPolicy, minLevel);
    }
    if (level == 0)
        return false;
    const QUrl requestUrl(url);
    if (requestUrl.scheme() != QLatin1String("http")
        && requestUrl.scheme() != QLatin1String("https"))
        return false;
    if (refererHeader.isEmpty())
        return false;
    const QUrl refUrl(QString::fromUtf8(refererHeader));
    if (refUrl.scheme() != QLatin1String("http")
        && refUrl.scheme() != QLatin1String("https"))
        return false;
    outValue = rewrittenReferer(level, refUrl, requestUrl);
    return true;
}

bool NavigationPolicy::isDomainBlocked(const QString &host)
{
    // No-rust build: the local list lives in rustcore — never blocks.
    Q_UNUSED(host);
    return false;
}

bool NavigationPolicy::shouldBlockDomain(const QUrl &url)
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

bool NavigationPolicy::isBlockedDomainAllowed(const QString &host)
{
    const QMutexLocker lock(&s_domainBlockLock);
    return s_sessionBlockedAllowed.contains(host.toLower());
}

void NavigationPolicy::allowBlockedDomain(const QString &host)
{
    const QString lowered = host.toLower();
    if (lowered.isEmpty())
        return;
    const QMutexLocker lock(&s_domainBlockLock);
    if (s_sessionBlockedAllowed.size() < maxBlockedAllowedHosts)
        s_sessionBlockedAllowed.insert(lowered);
}

void NavigationPolicy::clearBlockedDomainAllowance(const QString &host)
{
    const QMutexLocker lock(&s_domainBlockLock);
    s_sessionBlockedAllowed.remove(host.toLower());
}

void NavigationPolicy::clearBlockedDomainAllowances()
{
    const QMutexLocker lock(&s_domainBlockLock);
    s_sessionBlockedAllowed.clear();
    s_blockedDomainNavs.clear();
}

QUrl NavigationPolicy::strippedUrl(const QUrl &url)
{
    // No-rust build: the ruleset lives in rustcore — the strip stage
    // is absent, same as pre-refactor.
    return url;
}

// ---- the pipeline (fallback) -------------------------------------------

static bool hasQuery(const QUrl &url)
{
    return url.hasQuery();
}

static PolicyVerdict evaluatePrivacy(const PolicyRequest &req)
{
    PolicyVerdict v;
    bool httpsFirst, blockPings, stripParams, httpsOnly;
    {
        QReadLocker lock(&s_policyLock);
        httpsFirst = s_httpsFirst;
        blockPings = s_blockPings;
        stripParams = s_stripTrackingParams;
        httpsOnly = s_httpsOnly;
    }
    const QUrl &url = req.url;

    if (blockPings && NavigationPolicy::isPingTelemetry(req.resourceType, req.headers)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "ping";
        return v;
    }
    if (NavigationPolicy::shouldBlockResource(req.resourceType, req.headers)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "resource";
        return v;
    }
    if (NavigationPolicy::shouldBlockWebSocket(req.firstPartyUrl, url, req.resourceType)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "websocket";
        return v;
    }
    if (req.resourceType == RT_MAIN_FRAME && NavigationPolicy::shouldBlockDomain(url)) {
        NavigationPolicy::recordBlockedDomainNav(url);
        v.action = PolicyVerdict::Action::Block;
        v.reason = "domain";
        return v;
    }

    QUrl target = url;
    if (stripParams
        && (req.method == QByteArrayLiteral("GET")
            || req.method == QByteArrayLiteral("HEAD"))
        && hasQuery(url))
        target = NavigationPolicy::strippedUrl(url);

    if (httpsFirst && req.resourceType == RT_MAIN_FRAME
        && NavigationPolicy::isUpgradeCandidate(url, req.scope)) {
        target.setScheme(QLatin1String("https"));
        v.action = PolicyVerdict::Action::Redirect;
        v.redirectUrl = target;
        v.reason = "https-first";
        return v;
    }
    if (target != url) {
        v.action = PolicyVerdict::Action::Redirect;
        v.redirectUrl = target;
        v.reason = "strip";
        return v;
    }
    if (httpsOnly && req.resourceType == RT_MAIN_FRAME
        && NavigationPolicy::shouldWarnHttp(url, req.scope)) {
        NavigationPolicy::recordBlockedHttpNav(url);
        v.action = PolicyVerdict::Action::Block;
        v.reason = "https-only";
        return v;
    }
    if (NavigationPolicy::shouldBlockScript(req.firstPartyUrl, req.resourceType,
                                            req.scriptAllowed)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "script";
        return v;
    }
    v.action = PolicyVerdict::Action::Allow;
    QByteArray refHeader;
    for (const auto &pair : req.headers) {
        if (pair.first == "Referer") {
            refHeader = pair.second;
            break;
        }
    }
    QByteArray refValue;
    if (NavigationPolicy::refererForRequest(QString::fromUtf8(url.toEncoded()),
                                            refHeader, req.minRefererLevel,
                                            refValue)) {
        v.refererSet = true;
        v.refererValue = refValue;
    }
    return v;
}

static PolicyVerdict evaluateTor(const PolicyRequest &req)
{
    PolicyVerdict v;
    bool stripParams, blockPings;
    {
        QReadLocker lock(&s_policyLock);
        stripParams = s_stripTrackingParams;
        blockPings = s_blockPings;
    }
    const QUrl &url = req.url;

    if (req.resourceType == RT_MAIN_FRAME && NavigationPolicy::shouldBlockDomain(url)) {
        NavigationPolicy::recordBlockedDomainNav(url);
        v.action = PolicyVerdict::Action::Block;
        v.reason = "domain";
        return v;
    }
    QUrl target = url;
    if (stripParams
        && (req.method == QByteArrayLiteral("GET")
            || req.method == QByteArrayLiteral("HEAD"))
        && hasQuery(url))
        target = NavigationPolicy::strippedUrl(url);
    if (url.scheme() == QLatin1String("http")
        && !url.host().endsWith(QLatin1String(".onion"))) {
        target.setScheme(QLatin1String("https"));
        v.action = PolicyVerdict::Action::Redirect;
        v.redirectUrl = target;
        v.reason = "upgrade";
        return v;
    }
    if (target != url) {
        v.action = PolicyVerdict::Action::Redirect;
        v.redirectUrl = target;
        v.reason = "strip";
        return v;
    }
    if (blockPings && NavigationPolicy::isPingTelemetry(req.resourceType, req.headers)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "ping";
        return v;
    }
    if (NavigationPolicy::shouldBlockResource(req.resourceType, req.headers)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "resource";
        return v;
    }
    if (NavigationPolicy::shouldBlockWebSocket(req.firstPartyUrl, url, req.resourceType)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "websocket";
        return v;
    }
    if (NavigationPolicy::shouldBlockScript(req.firstPartyUrl, req.resourceType,
                                            req.scriptAllowed)) {
        v.action = PolicyVerdict::Action::Block;
        v.reason = "script";
        return v;
    }
    v.action = PolicyVerdict::Action::Allow;
    QByteArray refHeader;
    for (const auto &pair : req.headers) {
        if (pair.first == "Referer") {
            refHeader = pair.second;
            break;
        }
    }
    QByteArray refValue;
    if (NavigationPolicy::refererForRequest(QString::fromUtf8(url.toEncoded()),
                                            refHeader, req.minRefererLevel,
                                            refValue)) {
        v.refererSet = true;
        v.refererValue = refValue;
    }
    return v;
}

PolicyVerdict NavigationPolicy::evaluate(const PolicyRequest &request)
{
    PolicyVerdict v;
    const QString scheme = request.url.scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
        && scheme != QLatin1String("ws") && scheme != QLatin1String("wss"))
        return v;   // Pass
    return request.torMode ? evaluateTor(request) : evaluatePrivacy(request);
}

static bool isOnDomainList(const QStringList &rules, const QString &domain)
{
    for (const QString &rule : rules) {
        if (rule.startsWith(QLatin1String("."))) {
            if (domain.endsWith(rule))
                return true;
            const QStringView withoutDot = QStringView(rule).right(rule.size() - 1);
            if (domain == withoutDot)
                return true;
        } else {
            if (domain.endsWith(QLatin1Char('.') + rule))
                return true;
            if (rule == domain)
                return true;
        }
    }
    return false;
}

bool NavigationPolicy::cookieFilter(const CookieGateInput &input)
{
    const bool block = isOnDomainList(input.block, input.host);
    const bool allow = !block && isOnDomainList(input.allow, input.host);
    const bool allowForSession = !block && !allow
        && isOnDomainList(input.allowForSession, input.host);
    if (block)
        return false;
    if (input.thirdParty && input.blockThirdParty
        && !allow && !allowForSession)
        return false;
    switch (input.acceptPolicy) {
    case 0:   // AcceptAlways
        return true;
    case 1:   // AcceptNever
        return allow || allowForSession;
    default:  // AcceptOnlyFromSitesNavigatedTo
        return allow || allowForSession || !input.thirdParty;
    }
}

} // namespace Engine

#endif // ARORA_RUSTCORE
