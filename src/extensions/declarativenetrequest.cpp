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

#include "declarativenetrequest.h"

#include "chromecommon.h"

#include "extensionbackgroundhost.h"
#include "extensioneventhub.h"
#include "extensionshimbackend.h"
#include "webview.h"

#include <algorithm>
#include <cmath>

#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qsavefile.h>
#include <qthread.h>
#include <qurlquery.h>

// -------------------------------------------------------------------
// MD34 — see the header for the contract.  File layout:
//   1. Chrome constants + the urlFilter/regex compilers
//   2. the RE2-honesty check behind isRegexSupported
//   3. match-pattern host coverage (declarativeNetRequestWithHostAccess)
//   4. rule parsing/validation — every reject is a typed verdict
//   5. the GUI-thread registry (persist + snapshot rebuild)
//   6. the IO-thread evaluator + match recording
// -------------------------------------------------------------------

namespace {

// Chrome's published caps — the JS shim ships the same numbers as
// constants so feature-detecting extensions read the real values.
constexpr int kSessionRulesMax        = 5000;
constexpr int kSessionRegexRulesMax   = 1000;
constexpr int kDynamicRulesMin        = 30000;  // guaranteed pool
constexpr int kUnsafeDynamicGlobal    = 5000;   // shared across exts
constexpr int kStaticRulesetsMax      = 100;
constexpr int kEnabledRulesetsMax     = 50;
constexpr int kStaticGuaranteedRules  = 30000;
constexpr int kMatchedCallsPerWindow  = 20;
constexpr qint64 kMatchedWindowMs     = 10 * 60 * 1000;
constexpr int kMatchLogCap            = 1000;

DeclarativeNetRequest *s_instance = nullptr;

// int resourceType -> Chrome type name.  Literal values mirror
// QWebEngineUrlRequestInfo::ResourceType (the enum stays engine-side:
// EvalInput carries the int so this module never includes webengine
// headers).  Keep in sync with extensionrequestwatcher.cpp's
// chromeTypeName — same mapping, different representation.
QString dnrChromeType(int resourceType)
{
    switch (resourceType) {
    case 0:   // ResourceTypeMainFrame
    case 19:  // ResourceTypeNavigationPreloadMainFrame
        return QStringLiteral("main_frame");
    case 1:   // ResourceTypeSubFrame
    case 20:  // ResourceTypeNavigationPreloadSubFrame
        return QStringLiteral("sub_frame");
    case 2:   return QStringLiteral("stylesheet");
    case 3:   // ResourceTypeScript
    case 21:  // ResourceTypeJson
        return QStringLiteral("script");
    case 4:   // ResourceTypeImage
    case 12:  // ResourceTypeFavicon
        return QStringLiteral("image");
    case 5:   return QStringLiteral("font");
    case 7:   // ResourceTypeObject
    case 17:  // ResourceTypePluginResource
        return QStringLiteral("object");
    case 8:   return QStringLiteral("media");
    case 13:  return QStringLiteral("xmlhttprequest");
    case 14:  return QStringLiteral("ping");
    case 16:  return QStringLiteral("csp_report");
    case 254: // ResourceTypeWebSocket
        return QStringLiteral("websocket");
    default:  return QStringLiteral("other");
    }
}

// True when the Chrome resourceTypes name maps onto a real Qt type —
// DNR's set adds webtransport/webbundle/webidentity which no engine
// request can ever become here; they stay valid-but-unmatchable.
bool knownDnrType(const QString &type)
{
    static const QSet<QString> known = {
        QStringLiteral("main_frame"), QStringLiteral("sub_frame"),
        QStringLiteral("stylesheet"), QStringLiteral("script"),
        QStringLiteral("image"), QStringLiteral("font"),
        QStringLiteral("object"), QStringLiteral("xmlhttprequest"),
        QStringLiteral("ping"), QStringLiteral("csp_report"),
        QStringLiteral("media"), QStringLiteral("websocket"),
        QStringLiteral("webtransport"), QStringLiteral("webbundle"),
        QStringLiteral("webidentity"), QStringLiteral("other")};
    return known.contains(type);
}

// ---- urlFilter compiler -------------------------------------------
// EasyList-flavoured grammar Chrome documents:
//   '*'  wildcard, '|' start/end anchors, '||' domain anchor,
//   '^'  separator (anything but [a-zA-Z0-9_-.%]) or end-of-string.
// Answers the pattern text; *ok false = typed invalid-argument.
QString urlFilterPattern(const QString &filter, bool *ok)
{
    QString re;
    const int n = filter.size();
    for (int i = 0; i < n; ++i) {
        const QChar c = filter.at(i);
        if (c == QLatin1Char('*')) {
            re += QStringLiteral(".*");
        } else if (c == QLatin1Char('|')) {
            if (i == 0 && n > 1 && filter.at(1) == QLatin1Char('|')) {
                // Domain anchor: scheme:// then optional subdomain
                // run, then the rest of the filter literally.
                if (n > 2 && filter.at(2) == QLatin1Char('*')) {
                    *ok = false;
                    return QString();
                }
                re += QStringLiteral(
                    "^[a-zA-Z][a-zA-Z0-9+\\-.]*://(?:[^/?#]+\\.)?");
                i = 1;
            } else if (i == 0) {
                re += QLatin1Char('^');
            } else if (i == n - 1) {
                re += QLatin1Char('$');
            } else {
                *ok = false;
                return QString();
            }
        } else if (c == QLatin1Char('^')) {
            re += QStringLiteral("(?:[^a-zA-Z0-9_\\-.%]|$)");
        } else {
            re += QRegularExpression::escape(QString(c));
        }
    }
    // A filter that is anchors-only never matches anything useful;
    // Chrome requires the pattern itself be non-empty.
    QString body = filter;
    body.remove(QLatin1Char('|')).remove(QLatin1Char('^'))
        .remove(QLatin1Char('*'));
    if (body.isEmpty()) {
        *ok = false;
        return QString();
    }
    *ok = true;
    return re;
}

// ---- RE2 honesty ---------------------------------------------------
// QRegularExpression is PCRE; Chrome compiles regexFilter with RE2.
// Anything PCRE-only must answer isSupported=false rather than land
// a rule the engine would silently treat differently.  The scan is
// conservative: it refuses the constructs RE2 documents as absent —
// lookaround, backreferences, atomic groups, possessive quantifiers,
// \Q..\E literals, \C, recursion/subroutines, conditionals, comments,
// branch-reset, \G \Z \K \h \H \v \V \R \N \L \U \E and \M.
bool looksRe2Safe(const QString &pattern, QString *reason)
{
    if (pattern.isEmpty()) {
        *reason = QStringLiteral("syntaxError");
        return false;
    }
    for (const QChar c : pattern) {
        if (c.unicode() > 0x7f) {
            *reason = QStringLiteral("syntaxError");
            return false;      // regexFilter is ASCII-only upstream
        }
    }
    static const QSet<QString> re2MissingGroups = {
        QStringLiteral("(?="), QStringLiteral("(?!"),
        QStringLiteral("(?<="), QStringLiteral("(?<!"),
        QStringLiteral("(?>"), QStringLiteral("(?#"),
        QStringLiteral("(?|"), QStringLiteral("(?("),
        QStringLiteral("(?R"), QStringLiteral("(?0"),
        QStringLiteral("(?C")
    };
    for (int i = 0; i + 2 < pattern.size(); ++i) {
        const QString tri = pattern.mid(i, 3);
        if (re2MissingGroups.contains(tri)) {
            *reason = QStringLiteral("unsupportedSyntax");
            return false;
        }
    }
    for (int i = 0; i + 2 < pattern.size(); ++i) {
        if (pattern.at(i) == QLatin1Char('(')
                && pattern.at(i + 1) == QLatin1Char('?')
                && pattern.at(i + 2) == QLatin1Char('P')) {
            // (?P<name>…) is fine in RE2; (?P=name) is not.
            if (i + 3 < pattern.size()
                    && pattern.at(i + 3) == QLatin1Char('=')) {
                *reason = QStringLiteral("unsupportedSyntax");
                return false;
            }
        }
    }
    for (int i = 0; i < pattern.size(); ++i) {
        if (pattern.at(i) != QLatin1Char('\\'))
            continue;
        if (i + 1 >= pattern.size())
            break;
        const QChar e = pattern.at(i + 1);
        if (e >= QLatin1Char('1') && e <= QLatin1Char('9')) {
            *reason = QStringLiteral("unsupportedSyntax");
            return false;      // backreference
        }
        static const QString banned =
            QStringLiteral("QCEGZKhHvVRNLUTM");
        if (banned.contains(e)) {
            *reason = QStringLiteral("unsupportedSyntax");
            return false;
        }
        ++i;
    }
    // Possessive quantifiers: *+ ++ ?+ {n[,m]}+
    for (int i = 1; i < pattern.size(); ++i) {
        if (pattern.at(i) != QLatin1Char('+'))
            continue;
        const QChar prev = pattern.at(i - 1);
        if (prev == QLatin1Char('*') || prev == QLatin1Char('+')
                || prev == QLatin1Char('?') || prev == QLatin1Char('}')) {
            *reason = QStringLiteral("unsupportedSyntax");
            return false;
        }
    }
    return true;
}

int capturingGroupCount(const QString &pattern)
{
    int count = 0;
    for (int i = 0; i < pattern.size(); ++i) {
        const QChar c = pattern.at(i);
        if (c == QLatin1Char('\\')) {
            ++i;
            continue;
        }
        if (c == QLatin1Char('[')) {
            // class body — ')' or '(' inside [] is literal
            for (++i; i < pattern.size() && pattern.at(i) != QLatin1Char(']');
                 ++i) {
                if (pattern.at(i) == QLatin1Char('\\'))
                    ++i;
            }
            continue;
        }
        if (c == QLatin1Char('(')) {
            if (i + 1 >= pattern.size()
                    || pattern.at(i + 1) != QLatin1Char('?'))
                ++count;
            else if (i + 2 < pattern.size()
                     && pattern.at(i + 2) == QLatin1Char('<')
                     && !(i + 3 < pattern.size()
                          && (pattern.at(i + 3) == QLatin1Char('=')
                              || pattern.at(i + 3) == QLatin1Char('!'))))
                ++count;   // (?<name>…)
        }
    }
    return count;
}

bool domainMatches(const QString &host, const QSet<QString> &domains)
{
    for (const QString &d : domains) {
        if (host == d || host.endsWith(QLatin1Char('.') + d))
            return true;
    }
    return false;
}

bool validDomainToken(const QString &d)
{
    if (d.isEmpty())
        return false;
    for (const QChar c : d) {
        if (!(c.isLetterOrNumber() || c == QLatin1Char('.')
              || c == QLatin1Char('-') || c == QLatin1Char('_')
              || c == QLatin1Char(':')))
            return false;
    }
    return true;
}

} // namespace

QReadWriteLock DeclarativeNetRequest::s_lock;
QVector<DeclarativeNetRequest::SnapRule> DeclarativeNetRequest::s_rules;
QHash<QString, DeclarativeNetRequest::ExtMeta> DeclarativeNetRequest::s_meta;
QAtomicInt DeclarativeNetRequest::s_ruleCount;
QMutex DeclarativeNetRequest::s_exemptLock;
QHash<QString, QHash<QString, int>> DeclarativeNetRequest::s_frameExempt;

DeclarativeNetRequest *DeclarativeNetRequest::instance()
{
    if (!s_instance)
        s_instance = new DeclarativeNetRequest;
    return s_instance;
}

DeclarativeNetRequest::DeclarativeNetRequest(QObject *parent)
    : QObject(parent)
{
}

int DeclarativeNetRequest::registeredRuleCount()
{
    return s_ruleCount.loadAcquire();
}

bool DeclarativeNetRequest::regexSupported(const QString &pattern,
                                           bool caseSensitive,
                                           bool requireCapturing,
                                           QString *reason)
{
    Q_UNUSED(caseSensitive);
    QString why;
    if (!looksRe2Safe(pattern, &why)) {
        *reason = why;
        return false;
    }
    const QRegularExpression re(pattern);
    if (!re.isValid()) {
        *reason = QStringLiteral("syntaxError");
        return false;
    }
    if (requireCapturing && capturingGroupCount(pattern) == 0) {
        *reason = QStringLiteral("unsupportedSyntax");
        return false;
    }
    return true;
}

// ---- rule parsing ---------------------------------------------------

namespace {

using Rule = DeclarativeNetRequest::ParsedRule;

bool parseDomainList(const QJsonValue &v, QSet<QString> *out)
{
    if (v.isUndefined() || v.isNull())
        return true;
    if (!v.isArray())
        return false;
    for (const QJsonValue &e : v.toArray()) {
        const QString d = e.toString().toLower();
        if (!validDomainToken(d))
            return false;
        out->insert(d);
    }
    return true;
}

// Reply envelopes — same {ok,result|error} shape the backend's
// okReply/errReply produce, local so this module never depends on
// the shim backend's file-statics.
QJsonObject okResult(const QJsonValue &result = QJsonValue())
{
    QJsonObject r{{QStringLiteral("ok"), true}};
    if (!result.isUndefined())
        r.insert(QStringLiteral("result"), result);
    return r;
}

QJsonObject errResult(const QString &code)
{
    return QJsonObject{{QStringLiteral("ok"), false},
                       {QStringLiteral("error"), code}};
}

// Parses one rule json; *err carries the typed verdict code.
// Returns false without touching *out on any invalid input.
bool parseRule(const QJsonObject &in, const QString &rulesetId,
               int source, int order, Rule *out, QString *err)
{
    const QJsonValue idv = in.value(QStringLiteral("id"));
    const double idd = idv.toDouble(0);
    if (!idv.isDouble() || idd != std::floor(idd) || idd < 1
            || idd > 2147483647.0) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }
    const QJsonValue priv = in.value(QStringLiteral("priority"));
    const double prid = priv.isUndefined() ? 1 : priv.toDouble(0);
    if ((!priv.isUndefined() && !priv.isDouble())
            || prid != std::floor(prid) || prid < 1
            || prid > 2147483647.0) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }
    const QJsonObject action = in.value(QStringLiteral("action")).toObject();
    const QJsonObject cond = in.value(QStringLiteral("condition")).toObject();
    const QString actionType =
        action.value(QStringLiteral("type")).toString();
    if (action.isEmpty() || cond.isEmpty() || actionType.isEmpty()) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }

    Rule r;
    r.rulesetId = rulesetId;
    r.source = source;
    r.ruleId = int(idd);
    r.order = order;
    r.priority = int(prid);
    r.unsafe = false;

    // -- condition --
    bool hasConditionKey = false;
    const QString urlFilter =
        cond.value(QStringLiteral("urlFilter")).toString();
    const QString regexFilter =
        cond.value(QStringLiteral("regexFilter")).toString();
    if (!urlFilter.isEmpty() && !regexFilter.isEmpty()) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }
    const bool caseSensitive = cond.value(
        QStringLiteral("isUrlFilterCaseSensitive")).toBool(false);
    if (!urlFilter.isEmpty()) {
        hasConditionKey = true;
        for (const QChar c : urlFilter) {
            if (c.unicode() > 0x7f) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
        }
        bool ok = false;
        const QString pat = urlFilterPattern(urlFilter, &ok);
        if (!ok) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        r.cond.urlRe = QRegularExpression(
            pat, caseSensitive ? QRegularExpression::NoPatternOption
                               : QRegularExpression::CaseInsensitiveOption);
        if (!r.cond.urlRe.isValid()) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        r.cond.hasUrlRe = true;
    } else if (!regexFilter.isEmpty()) {
        hasConditionKey = true;
        QString why;
        if (!looksRe2Safe(regexFilter, &why)) {
            *err = why == QLatin1String("unsupportedSyntax")
                ? QStringLiteral("unsupported")
                : QStringLiteral("invalid-argument");
            return false;
        }
        r.cond.urlRe = QRegularExpression(
            regexFilter,
            caseSensitive ? QRegularExpression::NoPatternOption
                          : QRegularExpression::CaseInsensitiveOption);
        if (!r.cond.urlRe.isValid()) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        r.cond.hasUrlRe = true;
        r.unsafe = true;
    }

    static const QSet<QString> domainKeys = {
        QStringLiteral("initiatorDomains"),
        QStringLiteral("excludedInitiatorDomains"),
        QStringLiteral("requestDomains"),
        QStringLiteral("excludedRequestDomains"),
        QStringLiteral("domains"),           // deprecated aliases
        QStringLiteral("excludedDomains")
    };
    for (const QString &k : domainKeys) {
        if (cond.contains(k))
            hasConditionKey = true;
    }
    if (!parseDomainList(cond.value(QStringLiteral("initiatorDomains")),
                         &r.cond.initiatorDomains)
            || !parseDomainList(
                cond.value(QStringLiteral("excludedInitiatorDomains")),
                &r.cond.excludedInitiatorDomains)
            || !parseDomainList(
                cond.value(QStringLiteral("requestDomains")),
                &r.cond.requestDomains)
            || !parseDomainList(
                cond.value(QStringLiteral("excludedRequestDomains")),
                &r.cond.excludedRequestDomains)
            || !parseDomainList(cond.value(QStringLiteral("domains")),
                                &r.cond.initiatorDomains)
            || !parseDomainList(
                cond.value(QStringLiteral("excludedDomains")),
                &r.cond.excludedInitiatorDomains)) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }

    const auto parseTypeList = [&cond](const QString &key,
                                       QSet<QString> *out) -> int {
        const QJsonValue v = cond.value(key);
        if (v.isUndefined() || v.isNull())
            return 1;
        if (!v.isArray())
            return 0;
        const QJsonArray arr = v.toArray();
        if (arr.isEmpty())
            return -1;       // Chrome refuses empty resourceTypes
        for (const QJsonValue &e : arr) {
            const QString t = e.toString();
            if (!knownDnrType(t))
                return 0;
            out->insert(t);
        }
        return 1;
    };
    if (cond.contains(QStringLiteral("resourceTypes"))
            || cond.contains(QStringLiteral("excludedResourceTypes")))
        hasConditionKey = true;
    {
        QSet<QString> inc, exc;
        const int ri = parseTypeList(QStringLiteral("resourceTypes"), &inc);
        const int re = parseTypeList(
            QStringLiteral("excludedResourceTypes"), &exc);
        if (ri <= 0 || re < 0) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        r.typeNames = inc;
        r.excludedTypeNames = exc;
    }

    const auto parseMethodList = [&cond](const QString &key,
                                         QSet<QString> *out) {
        const QJsonValue v = cond.value(key);
        if (v.isUndefined() || v.isNull())
            return true;
        if (!v.isArray())
            return false;
        for (const QJsonValue &e : v.toArray()) {
            const QString m = e.toString().toUpper();
            bool ok = !m.isEmpty();
            for (const QChar c : m) {
                if (!(c.isLetterOrNumber() || c == QLatin1Char('!')
                      || c == QLatin1Char('#') || c == QLatin1Char('$')
                      || c == QLatin1Char('%') || c == QLatin1Char('&')
                      || c == QLatin1Char('\'') || c == QLatin1Char('*')
                      || c == QLatin1Char('+') || c == QLatin1Char('-')
                      || c == QLatin1Char('.') || c == QLatin1Char('^')
                      || c == QLatin1Char('_') || c == QLatin1Char('`')
                      || c == QLatin1Char('|') || c == QLatin1Char('~')))
                    ok = false;
            }
            if (!ok)
                return false;
            out->insert(m);
        }
        return true;
    };
    if (cond.contains(QStringLiteral("requestMethods"))
            || cond.contains(QStringLiteral("excludedRequestMethods")))
        hasConditionKey = true;
    if (!parseMethodList(QStringLiteral("requestMethods"),
                         &r.cond.requestMethods)
            || !parseMethodList(
                QStringLiteral("excludedRequestMethods"),
                &r.cond.excludedRequestMethods)) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }

    const QString domainType =
        cond.value(QStringLiteral("domainType")).toString();
    if (!domainType.isEmpty()) {
        hasConditionKey = true;
        if (domainType == QLatin1String("firstParty"))
            r.cond.domainType = 0;
        else if (domainType == QLatin1String("thirdParty"))
            r.cond.domainType = 1;
        else {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
    }

    // Conditions this layer cannot honor answer typed 'unsupported'
    // instead of silently never matching.
    static const QStringList unsupportedCondKeys = {
        QStringLiteral("tabIds"), QStringLiteral("excludedTabIds"),
        QStringLiteral("responseHeaders"),
        QStringLiteral("initiators"),
        QStringLiteral("excludedInitiators"),
        QStringLiteral("topDomains"),
        QStringLiteral("excludedTopDomains")
    };
    for (const QString &k : unsupportedCondKeys) {
        if (cond.contains(k)) {
            *err = QStringLiteral("unsupported");
            return false;
        }
    }
    if (!hasConditionKey) {
        *err = QStringLiteral("invalid-argument");
        return false;
    }

    // -- action --
    if (actionType == QLatin1String("allow"))
        r.action = DeclarativeNetRequest::ActionAllow;
    else if (actionType == QLatin1String("allowAllRequests"))
        r.action = DeclarativeNetRequest::ActionAllowAllRequests;
    else if (actionType == QLatin1String("block"))
        r.action = DeclarativeNetRequest::ActionBlock;
    else if (actionType == QLatin1String("upgradeScheme"))
        r.action = DeclarativeNetRequest::ActionUpgradeScheme;
    else if (actionType == QLatin1String("redirect"))
        r.action = DeclarativeNetRequest::ActionRedirect;
    else if (actionType == QLatin1String("modifyHeaders"))
        r.action = DeclarativeNetRequest::ActionModifyHeaders;
    else {
        *err = QStringLiteral("invalid-argument");
        return false;
    }

    if (r.action == DeclarativeNetRequest::ActionAllowAllRequests) {
        // Chrome: resourceTypes is required and may only name
        // main_frame / sub_frame.
        if (r.typeNames.isEmpty()) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        for (const QString &t : r.typeNames) {
            if (t != QLatin1String("main_frame")
                    && t != QLatin1String("sub_frame")) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
        }
    }

    if (r.action == DeclarativeNetRequest::ActionRedirect) {
        const QJsonObject red =
            action.value(QStringLiteral("redirect")).toObject();
        int kinds = 0;
        if (red.contains(QStringLiteral("url")))  ++kinds;
        if (red.contains(QStringLiteral("extensionPath"))) ++kinds;
        if (red.contains(QStringLiteral("transform"))) ++kinds;
        if (red.contains(QStringLiteral("regexSubstitution"))) ++kinds;
        if (kinds != 1) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        if (red.contains(QStringLiteral("url"))) {
            const QUrl u(red.value(QStringLiteral("url")).toString());
            const QString scheme = u.scheme();
            if (!u.isValid()
                    || (scheme != QLatin1String("http")
                        && scheme != QLatin1String("https")
                        && scheme != QLatin1String("chrome-extension"))) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
            r.redirectKind = 1;
            r.redirectUrl = u;
        } else if (red.contains(QStringLiteral("extensionPath"))) {
            const QString p =
                red.value(QStringLiteral("extensionPath")).toString();
            if (!p.startsWith(QLatin1Char('/'))
                    || p.split(QLatin1Char('/')).contains(
                        QStringLiteral(".."))) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
            r.redirectKind = 2;
            r.redirectExtPath = p;
        } else if (red.contains(QStringLiteral("transform"))) {
            r.redirectKind = 3;
            r.redirectTransform =
                red.value(QStringLiteral("transform")).toObject();
        } else {
            // regexSubstitution requires a regexFilter condition.
            if (regexFilter.isEmpty()) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
            const QString sub = red.value(
                QStringLiteral("regexSubstitution")).toString();
            if (sub.isEmpty()) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
            r.redirectKind = 4;
            r.regexSubstitution = sub;
            r.unsafe = true;
        }
    }

    if (r.action == DeclarativeNetRequest::ActionModifyHeaders) {
        // Request headers only — the interceptor has no response
        // hook, so response-side modification is honestly refused.
        const QJsonArray resp = action.value(
            QStringLiteral("responseHeaders")).toArray();
        if (!resp.isEmpty()) {
            *err = QStringLiteral("unsupported");
            return false;
        }
        const QJsonArray req = action.value(
            QStringLiteral("requestHeaders")).toArray();
        if (req.isEmpty()) {
            *err = QStringLiteral("invalid-argument");
            return false;
        }
        for (const QJsonValue &hv : req) {
            const QJsonObject h = hv.toObject();
            const QString name =
                h.value(QStringLiteral("header")).toString();
            const QString op =
                h.value(QStringLiteral("operation")).toString();
            const QString value =
                h.value(QStringLiteral("value")).toString();
            if (name.isEmpty()) {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
            for (const QChar c : name) {
                if (c.unicode() >= 0x7f || c.unicode() < 0x21
                        || c == QLatin1Char(':')
                        || c == QLatin1Char('(')
                        || c == QLatin1Char(')')
                        || c == QLatin1Char('<')
                        || c == QLatin1Char('>')
                        || c == QLatin1Char('@')
                        || c == QLatin1Char(',')
                        || c == QLatin1Char(';')
                        || c == QLatin1Char('\\')
                        || c == QLatin1Char('"')
                        || c == QLatin1Char('/')
                        || c == QLatin1Char('[')
                        || c == QLatin1Char(']')
                        || c == QLatin1Char('?')
                        || c == QLatin1Char('=')
                        || c == QLatin1Char('{')
                        || c == QLatin1Char('}')) {
                    *err = QStringLiteral("invalid-argument");
                    return false;
                }
            }
            DeclarativeNetRequest::RequestHeaderOp hop;
            hop.name = name.toLatin1();
            if (op == QLatin1String("append"))
                hop.op = DeclarativeNetRequest::HeaderAppend;
            else if (op == QLatin1String("set"))
                hop.op = DeclarativeNetRequest::HeaderSet;
            else if (op == QLatin1String("remove"))
                hop.op = DeclarativeNetRequest::HeaderRemove;
            else {
                *err = QStringLiteral("invalid-argument");
                return false;
            }
            if (hop.op != DeclarativeNetRequest::HeaderRemove) {
                if (!h.contains(QStringLiteral("value"))) {
                    *err = QStringLiteral("invalid-argument");
                    return false;
                }
                hop.value = value.toUtf8();
            }
            r.headerOps.append(hop);
        }
    }

    r.json = in;
    *out = r;
    return true;
}

QString frameKeyFor(const QUrl &url)
{
    return url.adjusted(QUrl::RemoveFragment).toString();
}

} // namespace

// ---- registry --------------------------------------------------------

DeclarativeNetRequest::ExtState &DeclarativeNetRequest::ensureLoaded(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool withHostAccess, bool feedbackGranted)
{
    ExtState &st = m_exts[extensionId];
    st.withHostAccess = withHostAccess;
    st.feedback = feedbackGranted;
    if (st.regSeq == 0)
        st.regSeq = m_nextRegSeq++;
    // host patterns: host_permissions ∪ declared content-script
    // matches — the same coverage invokePermissions reports.
    st.hostPatterns.clear();
    for (const QJsonValue &v : manifest.value(
             QStringLiteral("host_permissions")).toArray())
        st.hostPatterns.append(v.toString());
    if (!st.loaded) {
        st.loaded = true;
        // Manifest-declared static rulesets.
        st.declared.clear();
        const QJsonArray rr = manifest.value(
            QStringLiteral("declarative_net_request")).toObject()
                .value(QStringLiteral("rule_resources")).toArray();
        QSet<QString> seen;
        for (const QJsonValue &v : rr) {
            const QJsonObject o = v.toObject();
            RulesetDecl d;
            d.id = o.value(QStringLiteral("id")).toString();
            d.path = o.value(QStringLiteral("path")).toString();
            d.enabledDefault =
                o.value(QStringLiteral("enabled")).toBool(false);
            if (d.id.isEmpty() || d.path.isEmpty() || seen.contains(d.id))
                continue;
            seen.insert(d.id);
            if (st.declared.size() < kStaticRulesetsMax)
                st.declared.append(d);
            else
                emit noteRecorded(QStringLiteral(
                    "declarativeNetRequest: ruleset '%1' dropped — "
                    "manifest declares more than %2 rule_resources")
                    .arg(d.id).arg(kStaticRulesetsMax));
        }
        // Persisted dynamic rules + ruleset overrides.
        QFile file(storeDir + QLatin1String("/dnr.json"));
        if (file.size() <= 12 * 1024 * 1024
                && file.open(QIODevice::ReadOnly)) {
            const QJsonObject doc =
                QJsonDocument::fromJson(file.readAll()).object();
            int order = 1;
            for (const QJsonValue &v : doc.value(
                     QStringLiteral("dynamicRules")).toArray()) {
                Rule r;
                QString err;
                if (v.isObject()
                        && parseRule(v.toObject(), QStringLiteral("_dynamic"),
                                     SourceDynamic, order++, &r, &err))
                    st.dynamicRules.append(r);
                else
                    emit noteRecorded(QStringLiteral(
                        "declarativeNetRequest: persisted dynamic "
                        "rule dropped during load (%1)").arg(err));
            }
            const QJsonObject ov = doc.value(
                QStringLiteral("rulesetOverrides")).toObject();
            for (const QJsonValue &v : ov.value(
                     QStringLiteral("enable")).toArray())
                st.enabledOverride.insert(v.toString());
            for (const QJsonValue &v : ov.value(
                     QStringLiteral("disable")).toArray())
                st.disabledOverride.insert(v.toString());
            st.unsafeDynamic = 0;
            for (const Rule &r : st.dynamicRules) {
                if (r.unsafe)
                    ++st.unsafeDynamic;
            }
        }
        // Eagerly load manifest-enabled static rulesets so a first
        // getEnabledRulesets/evaluate reflects the manifest state.
        for (const RulesetDecl &d : st.declared) {
            if (d.enabledDefault && !st.disabledOverride.contains(d.id))
                loadRulesetFile(st, extensionPath, d);
        }
        rebuildSnapshot();
    }
    return st;
}

void DeclarativeNetRequest::loadRulesetFile(ExtState &state,
                                            const QString &extensionPath,
                                            const RulesetDecl &decl)
{
    if (state.staticLoaded.contains(decl.id))
        return;
    state.staticLoaded.insert(decl.id);
    // Bundle-relative hygiene — same shape the options-page check
    // applies: absolute paths and '..' legs never leave the bundle.
    if (decl.path.startsWith(QLatin1Char('/'))
            || decl.path.contains(QLatin1Char(':'))
            || decl.path.split(QLatin1Char('/')).contains(
                QStringLiteral(".."))) {
        emit noteRecorded(QStringLiteral(
            "declarativeNetRequest: ruleset '%1' path '%2' refused")
            .arg(decl.id, decl.path));
        return;
    }
    QFile file(extensionPath + QLatin1Char('/') + decl.path);
    if (file.size() > 10 * 1024 * 1024
            || !file.open(QIODevice::ReadOnly)) {
        emit noteRecorded(QStringLiteral(
            "declarativeNetRequest: ruleset '%1' unreadable")
            .arg(decl.id));
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isArray()) {
        emit noteRecorded(QStringLiteral(
            "declarativeNetRequest: ruleset '%1' is not a rule array")
            .arg(decl.id));
        return;
    }
    QVector<ParsedRule> rules;
    QSet<int> seenIds;
    int order = 1;
    for (const QJsonValue &v : doc.array()) {
        if (!v.isObject())
            continue;
        Rule r;
        QString err;
        if (parseRule(v.toObject(), decl.id, SourceStatic, order++,
                      &r, &err)) {
            if (seenIds.contains(r.ruleId)) {
                emit noteRecorded(QStringLiteral(
                    "declarativeNetRequest: static rule %1 in '%2' "
                    "skipped (duplicate-rule-id)")
                    .arg(r.ruleId).arg(decl.id));
                continue;
            }
            seenIds.insert(r.ruleId);
            rules.append(r);
        } else {
            emit noteRecorded(QStringLiteral(
                "declarativeNetRequest: static rule %1 in '%2' "
                "skipped (%3)")
                .arg(v.toObject()
                     .value(QStringLiteral("id")).toInt())
                .arg(decl.id, err));
        }
    }
    state.staticRules.insert(decl.id, rules);
}

QStringList DeclarativeNetRequest::enabledRulesetIds(
        const ExtState &state) const
{
    QStringList out;
    for (const RulesetDecl &d : state.declared) {
        const bool on = state.enabledOverride.contains(d.id)
            || (d.enabledDefault && !state.disabledOverride.contains(d.id));
        if (on)
            out.append(d.id);
    }
    return out;
}

void DeclarativeNetRequest::persist(const QString &extensionId,
                                    const QString &storeDir,
                                    ExtState &state)
{
    Q_UNUSED(extensionId);
    QDir().mkpath(storeDir);
    QJsonArray dyn;
    for (const ParsedRule &r : state.dynamicRules)
        dyn.append(r.json);
    QJsonArray en, dis;
    for (const QString &id : state.enabledOverride) en.append(id);
    for (const QString &id : state.disabledOverride) dis.append(id);
    QJsonObject doc;
    doc.insert(QStringLiteral("dynamicRules"), dyn);
    doc.insert(QStringLiteral("rulesetOverrides"),
               QJsonObject{{QStringLiteral("enable"), en},
                           {QStringLiteral("disable"), dis}});
    QSaveFile file(storeDir + QLatin1String("/dnr.json"));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(doc).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

int DeclarativeNetRequest::registeredUnsafeGlobal() const
{
    int total = 0;
    for (const ExtState &st : m_exts)
        total += st.unsafeDynamic;
    return total;
}

QJsonObject DeclarativeNetRequest::updateRules(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool sessionScope, bool withHostAccess, bool feedbackGranted,
        const QJsonArray &addRules, const QJsonArray &removeRuleIds)
{
    ExtState &st = ensureLoaded(extensionId, extensionPath, manifest,
                                storeDir, withHostAccess,
                                feedbackGranted);
    QVector<ParsedRule> &live =
        sessionScope ? st.sessionRules : st.dynamicRules;
    QVector<ParsedRule> target = live;

    // -- removal phase: ids must exist and not repeat ----------------
    QSet<int> removeIds;
    for (const QJsonValue &v : removeRuleIds) {
        const double d = v.toDouble(-1);
        if (!v.isDouble() || d != std::floor(d) || d < 1
                || d > 2147483647.0)
            return errResult(QStringLiteral("invalid-argument"));
        const int id = int(d);
        if (removeIds.contains(id))
            return errResult(QStringLiteral("duplicate-rule-id"));
        removeIds.insert(id);
    }
    if (!removeIds.isEmpty()) {
        QSet<int> have;
        for (const ParsedRule &r : target)
            have.insert(r.ruleId);
        for (const int id : removeIds) {
            if (!have.contains(id))
                return errResult(QStringLiteral("rule-not-found"));
        }
        QVector<ParsedRule> kept;
        for (const ParsedRule &r : target) {
            if (!removeIds.contains(r.ruleId))
                kept.append(r);
        }
        target = kept;
    }

    // -- add phase: validate the whole batch before any commit ------
    QVector<ParsedRule> adds;
    QSet<int> addIds;
    int order = 0;
    for (const ParsedRule &r : target)
        order = qMax(order, r.order);
    for (const QJsonValue &v : addRules) {
        if (!v.isObject())
            return errResult(QStringLiteral("invalid-argument"));
        ParsedRule r;
        QString err;
        if (!parseRule(v.toObject(),
                       sessionScope ? QStringLiteral("_session")
                                    : QStringLiteral("_dynamic"),
                       sessionScope ? SourceSession : SourceDynamic,
                       ++order, &r, &err))
            return errResult(err);
        if (addIds.contains(r.ruleId))
            return errResult(QStringLiteral("duplicate-rule-id"));
        addIds.insert(r.ruleId);
        adds.append(r);
    }
    for (const ParsedRule &r : target) {
        if (addIds.contains(r.ruleId))
            return errResult(QStringLiteral("duplicate-rule-id"));
    }

    // -- quotas -------------------------------------------------------
    if (sessionScope) {
        if (target.size() + adds.size() > kSessionRulesMax)
            return errResult(QStringLiteral("quota-exceeded"));
        int regexes = 0;
        for (const ParsedRule &r : target) {
            if (r.json.value(QStringLiteral("condition")).toObject()
                    .contains(QStringLiteral("regexFilter")))
                ++regexes;
        }
        for (const ParsedRule &r : adds) {
            if (r.json.value(QStringLiteral("condition")).toObject()
                    .contains(QStringLiteral("regexFilter")))
                ++regexes;
        }
        if (regexes > kSessionRegexRulesMax)
            return errResult(QStringLiteral("quota-exceeded"));
    } else {
        // Chrome splits dynamic quota: 30000 guaranteed safe rules
        // per extension plus a 5000-rule global pool shared across
        // extensions for "unsafe" rules (regexFilter/regexSubstitution).
        int safe = 0, unsafe = 0;
        for (const ParsedRule &r : target) {
            if (r.unsafe) ++unsafe; else ++safe;
        }
        for (const ParsedRule &r : adds) {
            if (r.unsafe) ++unsafe; else ++safe;
        }
        if (safe > kDynamicRulesMin)
            return errResult(QStringLiteral("quota-exceeded"));
        const int otherUnsafe =
            registeredUnsafeGlobal() - st.unsafeDynamic;
        if (unsafe + otherUnsafe > kUnsafeDynamicGlobal)
            return errResult(QStringLiteral("quota-exceeded"));
        st.unsafeDynamic = unsafe;
    }

    target.append(adds);
    live = target;
    if (!sessionScope)
        persist(extensionId, storeDir, st);
    rebuildSnapshot();
    return okResult();
}

QJsonArray DeclarativeNetRequest::rulesFor(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool sessionScope, bool withHostAccess, bool feedbackGranted,
        const QList<int> &filterIds)
{
    const ExtState &st = ensureLoaded(extensionId, extensionPath,
                                      manifest, storeDir,
                                      withHostAccess, feedbackGranted);
    const QVector<ParsedRule> &rules =
        sessionScope ? st.sessionRules : st.dynamicRules;
    QJsonArray out;
    for (const ParsedRule &r : rules) {
        if (!filterIds.isEmpty() && !filterIds.contains(r.ruleId))
            continue;
        out.append(r.json);
    }
    return out;
}

QJsonObject DeclarativeNetRequest::updateEnabledRulesets(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool withHostAccess, bool feedbackGranted,
        const QStringList &enable, const QStringList &disable)
{
    ExtState &st = ensureLoaded(extensionId, extensionPath, manifest,
                                storeDir, withHostAccess,
                                feedbackGranted);
    QSet<QString> declaredIds;
    for (const RulesetDecl &d : st.declared)
        declaredIds.insert(d.id);
    for (const QString &id : enable) {
        if (!declaredIds.contains(id))
            return errResult(QStringLiteral("ruleset-not-found"));
    }
    for (const QString &id : disable) {
        if (!declaredIds.contains(id))
            return errResult(QStringLiteral("ruleset-not-found"));
    }
    QSet<QString> nextEnabled = st.enabledOverride;
    QSet<QString> nextDisabled = st.disabledOverride;
    for (const QString &id : enable) {
        nextDisabled.remove(id);
        nextEnabled.insert(id);
    }
    for (const QString &id : disable) {
        nextEnabled.remove(id);
        nextDisabled.insert(id);
    }
    // Quota checks run against the PROPOSED set before any state
    // moves — atomicity like updateRules: an over-quota call leaves
    // the previous overrides untouched.
    int enabledCount = 0;
    for (const RulesetDecl &d : st.declared) {
        if (nextEnabled.contains(d.id)
                || (d.enabledDefault && !nextDisabled.contains(d.id)))
            ++enabledCount;
    }
    if (enabledCount > kEnabledRulesetsMax)
        return errResult(QStringLiteral("quota-exceeded"));
    // Ruleset files load lazily — reading them for a proposed set is
    // a harmless cache warm even if the call then fails a quota.
    for (const RulesetDecl &d : st.declared) {
        const bool on = nextEnabled.contains(d.id)
            || (d.enabledDefault && !nextDisabled.contains(d.id));
        if (on)
            loadRulesetFile(st, extensionPath, d);
    }
    int staticRules = 0;
    for (const RulesetDecl &d : st.declared) {
        const bool on = nextEnabled.contains(d.id)
            || (d.enabledDefault && !nextDisabled.contains(d.id));
        if (on)
            staticRules += st.staticRules.value(d.id).size();
    }
    if (staticRules > kStaticGuaranteedRules)
        return errResult(QStringLiteral("quota-exceeded"));

    st.enabledOverride = nextEnabled;
    st.disabledOverride = nextDisabled;
    persist(extensionId, storeDir, st);
    rebuildSnapshot();
    return okResult();
}

QStringList DeclarativeNetRequest::enabledRulesets(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool withHostAccess, bool feedbackGranted)
{
    const ExtState &st = ensureLoaded(extensionId, extensionPath,
                                      manifest, storeDir,
                                      withHostAccess, feedbackGranted);
    return enabledRulesetIds(st);
}

int DeclarativeNetRequest::availableStaticRuleCount(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool withHostAccess, bool feedbackGranted)
{
    ExtState &st = ensureLoaded(extensionId, extensionPath, manifest,
                                storeDir, withHostAccess,
                                feedbackGranted);
    for (const RulesetDecl &d : st.declared) {
        const bool on = st.enabledOverride.contains(d.id)
            || (d.enabledDefault && !st.disabledOverride.contains(d.id));
        if (on)
            loadRulesetFile(st, extensionPath, d);
    }
    int count = 0;
    for (const QString &id : enabledRulesetIds(st))
        count += st.staticRules.value(id).size();
    return qMax(0, kStaticGuaranteedRules - count);
}

// ---- evaluator -------------------------------------------------------

namespace {

// Chrome's action precedence inside one extension at equal priority:
// allow/allowAllRequests > block > upgradeScheme > redirect.
int actionRank(int action)
{
    using A = DeclarativeNetRequest;
    switch (action) {
    case A::ActionAllow:
    case A::ActionAllowAllRequests:
        return 4;
    case A::ActionBlock:
        return 3;
    case A::ActionUpgradeScheme:
        return 2;
    case A::ActionRedirect:
        return 1;
    default:
        return 0;
    }
}

// Task-specified precedence: priority first, then the rule source
// lane session > dynamic > static, then Chrome's action rank, then
// registration order — a total order, so verdicts never depend on
// hash iteration.
bool betterWithinExt(const DeclarativeNetRequest::ParsedRule &a,
                     const DeclarativeNetRequest::ParsedRule &b)
{
    if (a.priority != b.priority)
        return a.priority > b.priority;
    if (a.source != b.source)
        return a.source < b.source;
    if (actionRank(a.action) != actionRank(b.action))
        return actionRank(a.action) > actionRank(b.action);
    return a.order < b.order;
}

// Full condition check against the request snapshot.
bool ruleMatchesRequest(const DeclarativeNetRequest::ParsedRule &r,
                        const QString &urlStr, const QString &chromeType,
                        const QString &methodUpper,
                        const QString &hostLower,
                        const QString &initiatorHost)
{
    const auto &c = r.cond;
    if (c.hasUrlRe && !c.urlRe.match(urlStr).hasMatch())
        return false;
    if (!r.typeNames.isEmpty()
            && !r.typeNames.contains(chromeType))
        return false;
    if (r.excludedTypeNames.contains(chromeType))
        return false;
    if (!c.requestDomains.isEmpty()
            && !domainMatches(hostLower, c.requestDomains))
        return false;
    if (domainMatches(hostLower, c.excludedRequestDomains))
        return false;
    if (!c.initiatorDomains.isEmpty()) {
        if (initiatorHost.isEmpty())
            return false;   // no initiator cannot satisfy an inclusion
        if (!domainMatches(initiatorHost, c.initiatorDomains))
            return false;
    }
    if (!initiatorHost.isEmpty()
            && domainMatches(initiatorHost, c.excludedInitiatorDomains))
        return false;
    if (!c.requestMethods.isEmpty()
            && !c.requestMethods.contains(methodUpper))
        return false;
    if (c.excludedRequestMethods.contains(methodUpper))
        return false;
    if (c.domainType >= 0) {
        const bool firstParty = initiatorHost.isEmpty()
            || hostLower == initiatorHost
            || hostLower.endsWith(QLatin1Char('.') + initiatorHost)
            || initiatorHost.endsWith(QLatin1Char('.') + hostLower);
        if (c.domainType == 0 && !firstParty)
            return false;
        if (c.domainType == 1 && firstParty)
            return false;
    }
    return true;
}

// Resolves a redirect rule's target for a matched request.
QUrl resolveRedirect(const DeclarativeNetRequest::ParsedRule &r,
                     const QUrl &url, const QString &urlStr)
{
    switch (r.redirectKind) {
    case 1:
        return r.redirectUrl;
    case 2:
        return QUrl(QStringLiteral("chrome-extension://")
                    + r.extId + r.redirectExtPath);
    case 3: {
        QUrl out = url;
        const QJsonObject t = r.redirectTransform;
        const QString scheme =
            t.value(QStringLiteral("scheme")).toString();
        if (!scheme.isEmpty())
            out.setScheme(scheme);
        const QString host = t.value(QStringLiteral("host")).toString();
        if (!host.isEmpty())
            out.setHost(host);
        if (t.contains(QStringLiteral("port"))) {
            const QString p =
                t.value(QStringLiteral("port")).toString();
            out.setPort(p.isEmpty() ? -1 : p.toInt());
        }
        const QString path = t.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            out.setPath(path);
        const QString fragment =
            t.value(QStringLiteral("fragment")).toString();
        if (!fragment.isEmpty())
            out.setFragment(fragment);
        if (t.contains(QStringLiteral("query"))) {
            const QString q =
                t.value(QStringLiteral("query")).toString();
            out.setQuery(q);
        }
        if (t.contains(QStringLiteral("queryKey"))) {
            QUrlQuery q(out);
            q.removeAllQueryItems(
                t.value(QStringLiteral("queryKey")).toString());
            const QString kv =
                t.value(QStringLiteral("queryValue")).toString();
            if (!kv.isEmpty())
                q.addQueryItem(
                    t.value(QStringLiteral("queryKey")).toString(), kv);
            out.setQuery(q);
        }
        if (t.contains(QStringLiteral("queryTransform"))) {
            const QJsonObject qt = t.value(
                QStringLiteral("queryTransform")).toObject();
            QUrlQuery q(out);
            for (const QJsonValue &rv : qt.value(
                     QStringLiteral("removeParams")).toArray())
                q.removeAllQueryItems(rv.toString());
            for (const QJsonValue &pv : qt.value(
                     QStringLiteral("addOrReplaceParams")).toArray()) {
                const QJsonObject p = pv.toObject();
                const QString key =
                    p.value(QStringLiteral("key")).toString();
                if (key.isEmpty())
                    continue;
                q.removeAllQueryItems(key);
                q.addQueryItem(
                    key, p.value(QStringLiteral("value")).toString());
            }
            out.setQuery(q);
        }
        return out;
    }
    case 4: {
        QString out = urlStr;
        out.replace(r.cond.urlRe, r.regexSubstitution);
        return QUrl(out);
    }
    default:
        return QUrl();
    }
}

} // namespace

DeclarativeNetRequest::Verdict DeclarativeNetRequest::evaluate(
        const EvalInput &in)
{
    Verdict verdict;
    if (s_ruleCount.loadAcquire() <= 0)
        return verdict;

    const QString urlStr = in.url.toString();
    const QString chromeType = dnrChromeType(in.resourceType);
    const QString methodUpper =
        QString::fromLatin1(in.method).toUpper();
    const QString hostLower = in.url.host().toLower();
    const QString initiatorHost = in.firstPartyUrl.host().toLower();
    const bool frameRequest = in.resourceType == 0
        || in.resourceType == 1 || in.resourceType == 19
        || in.resourceType == 20;

    QJsonArray matchRecords;     // feedback-granted matches to log

    struct ExtEval {
        const SnapRule *candidate = nullptr;   // best non-modifyHeaders
        int maxAllowPri = 0;
        QVector<const SnapRule *> headerRules;
    };

    {
        QReadLocker rl(&s_lock);
        QHash<QString, ExtEval> perExt;
        const QString frameKey = frameKeyFor(in.firstPartyUrl);
        for (const SnapRule &sr : s_rules) {
            const ExtMeta meta = s_meta.value(sr.extId);
            if (meta.withHostAccess) {
                bool covered = false;
                for (const QString &p : meta.hostPatterns) {
                    if (matchPatternCovers(p, in.url)
                            || (!in.firstPartyUrl.isEmpty()
                                && matchPatternCovers(p, in.firstPartyUrl))) {
                        covered = true;
                        break;
                    }
                }
                if (!covered)
                    continue;
            }
            if (!ruleMatchesRequest(sr.rule, urlStr, chromeType,
                                    methodUpper, hostLower,
                                    initiatorHost))
                continue;
            // allowAllRequests frame exemption: a match on a frame
            // request exempts this ext's equal/lower-priority rules
            // for requests issued under that frame's document url.
            if (sr.rule.action == ActionAllowAllRequests
                    && frameRequest) {
                QMutexLocker el(&s_exemptLock);
                auto &b = s_frameExempt[sr.extId];
                const QString key = frameKeyFor(in.url);
                if (!b.contains(key)
                        || b.value(key) < sr.rule.priority)
                    b.insert(key, sr.rule.priority);
                continue;
            }
            {
                QMutexLocker el(&s_exemptLock);
                const auto extIt =
                    s_frameExempt.constFind(sr.extId);
                if (extIt != s_frameExempt.constEnd()) {
                    const auto pit = extIt->constFind(frameKey);
                    if (pit != extIt->constEnd()
                            && sr.rule.priority <= pit.value())
                        continue;
                }
            }
            ExtEval &e = perExt[sr.extId];
            if (sr.rule.action == ActionModifyHeaders) {
                e.headerRules.append(&sr);
                continue;
            }
            if (sr.rule.action == ActionAllow
                    || sr.rule.action == ActionAllowAllRequests)
                e.maxAllowPri = qMax(e.maxAllowPri, sr.rule.priority);
            if (!e.candidate
                    || betterWithinExt(sr.rule, e.candidate->rule))
                e.candidate = &sr;
        }

        // Cross-extension winner — Chrome's action precedence for
        // the block lane: block > upgradeScheme > redirect; an
        // allow-only candidate never fires anything.
        const SnapRule *winner = nullptr;
        QString winnerExt;
        for (auto it = perExt.constBegin(); it != perExt.constEnd();
             ++it) {
            const SnapRule *c = it->candidate;
            if (!c || c->rule.action == ActionAllow
                    || c->rule.action == ActionAllowAllRequests)
                continue;
            if (!winner) {
                winner = c;
                winnerExt = it.key();
                continue;
            }
            if (actionRank(c->rule.action)
                    != actionRank(winner->rule.action)) {
                if (actionRank(c->rule.action)
                        > actionRank(winner->rule.action)) {
                    winner = c;
                    winnerExt = it.key();
                }
                continue;
            }
            // Same action — most recently registered extension wins
            // (Chrome's install-recency analogue, deterministic).
            if (s_meta.value(it.key()).regSeq
                    > s_meta.value(winnerExt).regSeq) {
                winner = c;
                winnerExt = it.key();
            }
        }

        if (winner) {
            // Chrome logs the MATCHED rule on the feedback lane even
            // when the resolution is a no-op (e.g. upgradeScheme on
            // an already-https url) — the match itself is the signal.
            if (s_meta.value(winnerExt).feedback) {
                matchRecords.append(QJsonObject{
                    {QStringLiteral("extId"), winnerExt},
                    {QStringLiteral("ruleId"), winner->rule.ruleId},
                    {QStringLiteral("rulesetId"), winner->rule.rulesetId},
                    {QStringLiteral("url"), urlStr},
                    {QStringLiteral("method"), methodUpper},
                    {QStringLiteral("type"), chromeType},
                    {QStringLiteral("initiator"),
                     in.firstPartyUrl.toString()},
                    {QStringLiteral("isFrame"), frameRequest}});
            }
            if (winner->rule.action == ActionBlock) {
                verdict.blocked = true;
            } else if (winner->rule.action == ActionUpgradeScheme) {
                const QString scheme = in.url.scheme();
                if (scheme == QLatin1String("http")
                        || scheme == QLatin1String("ftp")) {
                    QUrl out = in.url;
                    out.setScheme(QStringLiteral("https"));
                    verdict.redirectUrl = out;
                }
            } else if (winner->rule.action == ActionRedirect) {
                const QUrl out =
                    resolveRedirect(winner->rule, in.url, urlStr);
                if (out.isValid())
                    verdict.redirectUrl = out;
            }
        }

        // Request-header rules apply when the request survives —
        // Chrome admits only those outranking the ext's own matching
        // allow/allowAllRequests rules, ordered by extension recency
        // then priority.
        if (!verdict.blocked && !verdict.redirectUrl.isValid()) {
            struct HeaderRule {
                const SnapRule *sr;
                int seq;
            };
            QVector<HeaderRule> ordered;
            for (auto it = perExt.constBegin(); it != perExt.constEnd();
                 ++it) {
                const int seq = s_meta.value(it.key()).regSeq;
                const bool feedback = s_meta.value(it.key()).feedback;
                for (const SnapRule *hr : it->headerRules) {
                    if (hr->rule.priority <= it->maxAllowPri)
                        continue;
                    ordered.append({hr, seq});
                    if (feedback) {
                        matchRecords.append(QJsonObject{
                            {QStringLiteral("extId"), it.key()},
                            {QStringLiteral("ruleId"),
                             hr->rule.ruleId},
                            {QStringLiteral("rulesetId"),
                             hr->rule.rulesetId},
                            {QStringLiteral("url"), urlStr},
                            {QStringLiteral("method"), methodUpper},
                            {QStringLiteral("type"), chromeType},
                            {QStringLiteral("initiator"),
                             in.firstPartyUrl.toString()},
                            {QStringLiteral("isFrame"), frameRequest}});
                    }
                }
            }
            std::sort(ordered.begin(), ordered.end(),
                      [](const HeaderRule &a, const HeaderRule &b) {
                if (a.seq != b.seq) return a.seq > b.seq;
                if (a.sr->rule.priority != b.sr->rule.priority)
                    return a.sr->rule.priority > b.sr->rule.priority;
                return a.sr->rule.order > b.sr->rule.order;
            });
            // Header composition: append locks the header to appends,
            // set locks it to appends from the same extension, remove
            // locks it entirely — Chrome's documented ordering.
            struct HeaderState {
                int state = 0;      // 0 free,1 appends,2 set,3 removed
                QString ext;
            };
            QHash<QString, HeaderState> states;
            for (const HeaderRule &hr : ordered) {
                for (const RequestHeaderOp &op : hr.sr->rule.headerOps) {
                    const QString key =
                        QString::fromLatin1(op.name).toLower();
                    HeaderState &hs = states[key];
                    bool allow = false;
                    switch (hs.state) {
                    case 0: allow = true; break;
                    case 1: allow =
                        op.op == HeaderAppend; break;
                    case 2: allow =
                        op.op == HeaderAppend && hs.ext == hr.sr->extId;
                        break;
                    default: allow = false;
                    }
                    if (!allow)
                        continue;
                    if (hs.state == 0) {
                        if (op.op == HeaderSet) {
                            hs.state = 2; hs.ext = hr.sr->extId;
                        } else if (op.op == HeaderAppend)
                            hs.state = 1;
                        else hs.state = 3;
                    }
                    // state 1/2 keep their lock — a same-ext append
                    // after a set does not reopen the header.
                    verdict.headerOps.append(op);
                }
            }
        }
    }

    // Feedback logging: matched rules ride a queued hop so the IO
    // thread never blocks on the GUI lane.  A GUI-thread caller (the
    // parity test) lands them synchronously instead.
    if (!matchRecords.isEmpty() && s_instance) {
        for (const QJsonValue &rv : matchRecords) {
            const QJsonObject rec = rv.toObject();
            if (QThread::currentThread() == s_instance->thread())
                s_instance->noteMatch(rec);
            else
                QMetaObject::invokeMethod(
                    s_instance, "noteMatch", Qt::QueuedConnection,
                    Q_ARG(QJsonObject, rec));
        }
    }
    return verdict;
}

// ---- feedback lane ---------------------------------------------------

void DeclarativeNetRequest::noteMatch(const QJsonObject &record)
{
    const QString extId = record.value(QStringLiteral("extId")).toString();
    if (extId.isEmpty())
        return;
    ExtState &st = m_exts[extId];
    const QUrl url(record.value(QStringLiteral("url")).toString());
    const QUrl firstParty(
        record.value(QStringLiteral("initiator")).toString());
    const bool isFrame =
        record.value(QStringLiteral("isFrame")).toBool();
    int tabId = -1;
    if (ExtensionEventHub *hub = ExtensionEventHub::instance()) {
        if (WebView *view =
                hub->viewForRequest(url, isFrame, firstParty))
            tabId = ExtensionShimBackend::tabIdFor(view);
    }
    const double ts = double(QDateTime::currentMSecsSinceEpoch());
    const QJsonObject rule{
        {QStringLiteral("ruleId"),
         record.value(QStringLiteral("ruleId")).toInt()},
        {QStringLiteral("rulesetId"),
         record.value(QStringLiteral("rulesetId")).toString()},
        {QStringLiteral("extensionId"), extId}};
    const QJsonObject request{
        {QStringLiteral("url"),
         record.value(QStringLiteral("url")).toString()},
        {QStringLiteral("method"),
         record.value(QStringLiteral("method")).toString()},
        {QStringLiteral("type"),
         record.value(QStringLiteral("type")).toString()},
        {QStringLiteral("initiator"),
         record.value(QStringLiteral("initiator")).toString()}};
    const QJsonObject info{
        {QStringLiteral("rule"), rule},
        {QStringLiteral("tabId"), tabId},
        {QStringLiteral("timeStamp"), ts},
        {QStringLiteral("request"), request}};
    if (st.matchLog.size() >= kMatchLogCap)
        st.matchLog.removeFirst();
    st.matchLog.append(info);
    if (ExtensionBackgroundRuntime *rt =
            ExtensionBackgroundRuntime::instance()) {
        rt->deliverEvent(extId,
            QStringLiteral("declarativeNetRequest.onRuleMatchedDebug"),
            QJsonObject{{QStringLiteral("__args"), QJsonArray{info}}});
    }
}

bool DeclarativeNetRequest::matchedRulesQuota(const QString &extensionId)
{
    ExtState &st = m_exts[extensionId];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    while (!st.matchedCalls.isEmpty()
           && now - st.matchedCalls.first() > kMatchedWindowMs)
        st.matchedCalls.removeFirst();
    if (st.matchedCalls.size() >= kMatchedCallsPerWindow)
        return false;
    st.matchedCalls.append(now);
    return true;
}

QJsonArray DeclarativeNetRequest::matchedRules(
        const QString &extensionId, int tabIdFilter, double minTimeStamp)
{
    QJsonArray out;
    const ExtState &st = m_exts.value(extensionId);
    for (const QJsonObject &info : st.matchLog) {
        if (tabIdFilter >= 0
                && info.value(QStringLiteral("tabId")).toInt()
                    != tabIdFilter)
            continue;
        if (minTimeStamp > 0
                && info.value(QStringLiteral("timeStamp")).toDouble()
                    < minTimeStamp)
            continue;
        QJsonObject slim = info;
        slim.remove(QStringLiteral("request"));
        out.append(slim);
    }
    return out;
}

QJsonArray DeclarativeNetRequest::testMatchOutcome(
        const QString &extensionId, const QString &extensionPath,
        const QJsonObject &manifest, const QString &storeDir,
        bool withHostAccess, bool feedbackGranted,
        const QJsonObject &request)
{
    ExtState &st = ensureLoaded(extensionId, extensionPath, manifest,
                                storeDir, withHostAccess,
                                feedbackGranted);
    for (const RulesetDecl &d : st.declared) {
        const bool on = st.enabledOverride.contains(d.id)
            || (d.enabledDefault && !st.disabledOverride.contains(d.id));
        if (on)
            loadRulesetFile(st, extensionPath, d);
    }
    const QString type =
        request.value(QStringLiteral("type")).toString();
    const QUrl url(request.value(QStringLiteral("url")).toString());
    const QUrl initiator(
        request.value(QStringLiteral("initiator")).toString());
    const QString method = request.value(
        QStringLiteral("method")).toString().toUpper();
    int resourceType = -1;
    // name -> the canonical int the evaluator expects
    static const QHash<QString, int> nameToType = {
        {QStringLiteral("main_frame"), 0},
        {QStringLiteral("sub_frame"), 1},
        {QStringLiteral("stylesheet"), 2},
        {QStringLiteral("script"), 3},
        {QStringLiteral("image"), 4},
        {QStringLiteral("font"), 5},
        {QStringLiteral("object"), 7},
        {QStringLiteral("media"), 8},
        {QStringLiteral("xmlhttprequest"), 13},
        {QStringLiteral("ping"), 14},
        {QStringLiteral("csp_report"), 16},
        {QStringLiteral("websocket"), 254},
        {QStringLiteral("other"), -2}
    };
    resourceType = nameToType.value(type, -1);
    EvalInput in;
    in.url = url;
    in.firstPartyUrl = initiator;
    in.resourceType = resourceType;
    in.method = method.toLatin1();
    const QString urlStr = url.toString();
    const QString chromeType = dnrChromeType(resourceType);
    const QString hostLower = url.host().toLower();
    const QString initHost = initiator.host().toLower();

    QVector<const ParsedRule *> hits;
    const auto consider = [&](const QVector<ParsedRule> &rules) {
        for (const ParsedRule &r : rules) {
            // request.type is already a Chrome name — compare it
            // directly so webtransport/webbundle conditions resolve
            // against the declared type, not the mapped 'other'.
            if (ruleMatchesRequest(r, urlStr, type,
                                   method, hostLower, initHost))
                hits.append(&r);
        }
    };
    consider(st.sessionRules);
    consider(st.dynamicRules);
    for (const QString &id : enabledRulesetIds(st))
        consider(st.staticRules.value(id));
    std::sort(hits.begin(), hits.end(),
              [](const ParsedRule *a, const ParsedRule *b) {
        return betterWithinExt(*a, *b);
    });
    QJsonArray out;
    for (const ParsedRule *r : hits) {
        out.append(QJsonObject{
            {QStringLiteral("ruleId"), r->ruleId},
            {QStringLiteral("rulesetId"), r->rulesetId},
            {QStringLiteral("extensionId"), extensionId}});
    }
    return out;
}

void DeclarativeNetRequest::dropExtension(const QString &extensionId)
{
    if (!m_exts.remove(extensionId))
        return;
    {
        QMutexLocker el(&s_exemptLock);
        s_frameExempt.remove(extensionId);
    }
    rebuildSnapshot();
}

void DeclarativeNetRequest::rebuildSnapshot()
{
    QVector<SnapRule> flat;
    QHash<QString, ExtMeta> meta;
    int total = 0;
    for (auto it = m_exts.begin(); it != m_exts.end(); ++it) {
        ExtState &st = it.value();
        ExtMeta m;
        m.withHostAccess = st.withHostAccess;
        m.feedback = st.feedback;
        m.hostPatterns = st.hostPatterns;
        m.regSeq = st.regSeq;
        meta.insert(it.key(), m);
        const auto push = [&](const QVector<ParsedRule> &rules) {
            for (const ParsedRule &r : rules) {
                SnapRule sr;
                sr.extId = it.key();
                sr.rule = r;
                sr.rule.extId = it.key();
                flat.append(sr);
                ++total;
            }
        };
        push(st.sessionRules);
        push(st.dynamicRules);
        for (const QString &id : enabledRulesetIds(st))
            push(st.staticRules.value(id));
    }
    {
        QWriteLocker wl(&s_lock);
        s_rules = flat;
        s_meta = meta;
        s_ruleCount.storeRelease(total);
    }
}
