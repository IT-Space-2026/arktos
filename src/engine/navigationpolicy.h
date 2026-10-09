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

#ifndef NAVIGATIONPOLICY_H
#define NAVIGATIONPOLICY_H

#include <qbytearray.h>
#include <qlist.h>
#include <qpair.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

// ENG02 — the engine-agnostic request-policy adapter.
//
// Every branching decision the request interceptors and the cookie
// gate used to make in C++ is delegated here:
//   * built with CONFIG+=rustcore → verdicts come from the rustcore
//     policy module (pure Rust, memory-safe over attacker URLs);
//   * without it → the legacy Qt fallback (same semantics, kept 1:1).
//
// NO QtWebEngine types cross this file — the WebEngine interceptors
// marshal QWebEngineUrlRequestInfo into PolicyRequest, call evaluate()
// and apply the verdict (block / redirect / Referer write / delegated
// adblock tail).  Thread-safety contract is unchanged: every entry
// point is safe on the engine's request-hook thread.

namespace Engine {

using HeaderList = QList<QPair<QByteArray, QByteArray> >;

// Everything the policy may legitimately observe about one outgoing
// request — mirrors the JSON manifest sent to rustcore 1:1.
struct PolicyRequest {
    QUrl url;
    QUrl firstPartyUrl;
    int resourceType = 255;        // Engine::ResourceType ordinal (engineinterface.h)
    QByteArray method;
    HeaderList headers;
    QString scope;                 // downgrade scope (profile identity)
    bool torMode = false;          // tor pipeline variant
    bool scriptAllowed = false;    // JSCTL grant, pre-resolved by the caller
    int minRefererLevel = 0;       // tor enforces at least RefererTrimmed
};

// What the policy decided — the adapter applies it, nothing more.
struct PolicyVerdict {
    enum class Action {
        Pass,      // not a web request — leave untouched (STALL01)
        Allow,     // policy passed — adapter runs the delegated matcher tail
        Block,
        Redirect
    };
    Action action = Action::Pass;
    QUrl redirectUrl;
    QByteArray reason;
    bool refererSet = false;       // Allow: write Referer = refererValue
    QByteArray refererValue;       // empty value = header removal
};

// Cookie-gate input — the jar ships its own rule lists per call so the
// gate stays stateless on the engine's IO thread.
struct CookieGateInput {
    QString host;
    bool thirdParty = false;
    bool blockThirdParty = false;
    int acceptPolicy = 2;          // CookieJar::AcceptPolicy ordinal
    QStringList block;
    QStringList allow;
    QStringList allowForSession;
};

class NavigationPolicy
{
public:
    // The pipeline — one call per outgoing request.
    static PolicyVerdict evaluate(const PolicyRequest &request);
    static bool cookieFilter(const CookieGateInput &input);

    // Policy snapshot pushed from the GUI thread (loadSettings).
    struct Snapshot {
        bool httpsFirst = true;
        bool httpsOnly = true;
        int refererPolicy = 1;             // RefererTrimmed
        int securityLevel = 0;             // Standard
        bool blockPings = true;
        bool blockRemoteFonts = false;
        bool blockPrefetch = true;
        bool blockThirdPartyWebSockets = false;
        bool stripTrackingParams = true;
        bool domainBlocklist = true;
        QStringList httpsOnlyExceptions;
    };
    static void loadSnapshot(const Snapshot &snapshot);

    // Granular decision/state surface — the pre-refactor public
    // statics on PrivacyRequestInterceptor delegate here 1:1.
    static bool httpsFirstEnabled();
    static bool httpsOnlyEnabled();
    static int refererPolicy();
    static int securityLevel();
    static bool blockPingsEnabled();
    static bool blockRemoteFontsEnabled();
    static bool blockPrefetchEnabled();
    static bool blockThirdPartyWebSocketsEnabled();
    static bool stripTrackingParamsEnabled();
    static bool domainBlocklistEnabled();

    static QByteArray referrerMetaValue(int level);
    static QByteArray rewrittenReferer(int level, const QUrl &source,
                                       const QUrl &target);
    // The referer stage alone: should the Referer header be (re)written
    // for this request, and to what value (empty = removal)?
    static bool refererForRequest(const QString &url,
                                  const QByteArray &refererHeader,
                                  int minLevel, QByteArray &outValue);

    static bool isUpgradeCandidate(const QUrl &url, const QString &scope);
    static bool shouldWarnHttp(const QUrl &url, const QString &scope);
    static bool shouldWarnFormPost(const QUrl &url, const QString &scope);
    static bool isHttpAllowedHost(const QString &host);
    // Returns true when the caller should persist the grant to
    // QSettings (persistent && not already listed) — the policy only
    // keeps the host sets; storage stays Qt-side.
    static bool allowHttpForHost(const QString &host, bool persistent);
    static void clearHttpAllowance(const QString &host);
    static QStringList httpExceptionHosts();

    static bool isDowngraded(const QString &host, const QString &scope);
    static void markDowngraded(const QString &host, const QString &scope);
    static void clearDowngradedHost(const QString &host,
                                   const QString &scope);
    static void clearDowngradedHosts();
    static qint64 downgradeTtlMs();
    static void setDowngradeTtlMs(qint64 ms);
    static bool failureImpliesDowngrade(int errorDomain, int errorCode);
    static bool noteNavigationFailure(const QUrl &url, int errorDomain,
                                      int errorCode, const QString &scope);

    static void recordBlockedHttpNav(const QUrl &url);
    static bool takeBlockedHttpNav(const QUrl &url);
    static void recordBlockedDomainNav(const QUrl &url);
    static bool takeBlockedDomainNav(const QUrl &url);

    static bool isDomainBlocked(const QString &host);
    static bool shouldBlockDomain(const QUrl &url);
    static bool isBlockedDomainAllowed(const QString &host);
    static void allowBlockedDomain(const QString &host);
    static void clearBlockedDomainAllowance(const QString &host);
    static void clearBlockedDomainAllowances();

    static bool isPingTelemetry(int resourceType, const HeaderList &headers);
    static bool shouldBlockResource(int resourceType,
                                    const HeaderList &headers);
    static bool shouldBlockWebSocket(const QUrl &firstPartyUrl,
                                     const QUrl &requestUrl,
                                     int resourceType);
    static bool shouldBlockScript(const QUrl &firstPartyUrl,
                                  int resourceType, bool scriptAllowed);
    static bool isPrivateOrLocalHost(const QString &host);
    static QUrl strippedUrl(const QUrl &url);
};

} // namespace Engine

#endif // NAVIGATIONPOLICY_H
