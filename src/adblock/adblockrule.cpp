/**
 * Copyright (c) 2009, Zsombor Gegesy <gzsombor@gmail.com>
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright (c) 2026, The Arora Authors
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "adblockrule.h"

#include <qdebug.h>
#include <qurl.h>

// #define ADBLOCKRULE_DEBUG

// Bit layout for the request-type masks.  Bits 0..21 mirror
// QWebEngineUrlRequestInfo::ResourceType values, 22 is WebSocket
// (resource type 254) and 23 is Unknown/other catch-all.
static quint32 typeBit(int resourceType)
{
    if (resourceType >= 0 && resourceType <= 21)
        return 1u << resourceType;
    if (resourceType == 254) // ResourceTypeWebSocket
        return 1u << 22;
    return 1u << 23;         // ResourceTypeUnknown / -1
}

// ResourceType values from QWebEngineUrlRequestInfo (kept numeric so
// this class stays QtCore-only).
enum {
    RtMainFrame = 0,
    RtSubFrame = 1,
    RtStylesheet = 2,
    RtScript = 3,
    RtImage = 4,
    RtFontResource = 5,
    RtSubResource = 6,
    RtObject = 7,
    RtMedia = 8,
    RtWorker = 9,
    RtSharedWorker = 10,
    RtPrefetch = 11,
    RtFavicon = 12,
    RtXhr = 13,
    RtPing = 14,
    RtServiceWorker = 15,
    RtCspReport = 16,
    RtPluginResource = 17,
    RtNavigationPreloadMainFrame = 19,
    RtNavigationPreloadSubFrame = 20,
    RtJson = 21
};

static quint32 otherTypeMask()
{
    // Everything not covered by a named ABP type.
    quint32 named = typeBit(RtScript) | typeBit(RtImage)
            | typeBit(RtStylesheet) | typeBit(RtObject)
            | typeBit(RtSubFrame) | typeBit(RtMainFrame)
            | typeBit(RtXhr) | typeBit(RtPing) | typeBit(RtMedia)
            | typeBit(RtFontResource) | typeBit(254);
    return ~named & 0xFFFFFFu;
}

static quint32 typeOptionMask(const QString &name, bool *ok)
{
    *ok = true;
    if (name == QLatin1String("script"))
        return typeBit(RtScript);
    if (name == QLatin1String("image")
        || name == QLatin1String("background"))
        return typeBit(RtImage);
    if (name == QLatin1String("stylesheet"))
        return typeBit(RtStylesheet);
    if (name == QLatin1String("object")
        || name == QLatin1String("object-subrequest")
        || name == QLatin1String("object_subrequest"))
        return typeBit(RtObject);
    if (name == QLatin1String("media"))
        return typeBit(RtMedia);
    if (name == QLatin1String("font"))
        return typeBit(RtFontResource);
    if (name == QLatin1String("subdocument"))
        return typeBit(RtSubFrame);
    if (name == QLatin1String("document"))
        return typeBit(RtMainFrame);
    if (name == QLatin1String("xmlhttprequest")
        || name == QLatin1String("xhr"))
        return typeBit(RtXhr);
    if (name == QLatin1String("ping"))
        return typeBit(RtPing);
    if (name == QLatin1String("websocket"))
        return typeBit(254);
    if (name == QLatin1String("other"))
        return otherTypeMask();
    *ok = false;
    return 0;
}

// Options that alter matching in ways we cannot honor; a filter using
// one is parsed but never applied.
static bool isUnsupportedOption(const QString &name)
{
    static const QStringList unsupported = {
        QLatin1String("csp"),         // needs response headers
        QLatin1String("rewrite"),     // uBO response rewrite
        QLatin1String("header"),      // uBO response header
        QLatin1String("replace"),     // ABP response body rewrite
        QLatin1String("cookie"),      // response cookies
        QLatin1String("sitekey"),     // page-supplied key
        QLatin1String("webrtc"),      // non-HTTP traffic
        QLatin1String("popup"),       // new-window context only
        QLatin1String("mp4")          // redirect alias we don't ship
    };
    const int eq = name.indexOf(QLatin1Char('='));
    const QString base = (eq == -1) ? name : name.left(eq);
    return unsupported.contains(base);
}

// A filter line longer than this is hostile or corrupt input — real
// ABP/uBO filters are a few hundred characters; the rule is parsed
// but kept inert instead of compiling an arbitrarily large pattern.
static const int MaximumFilterLength = 4096;

// Raw /regex/ rules are handed to PCRE2 verbatim.  QRegularExpression
// exposes no match-time budget, so a catastrophic-backtracking pattern
// in a hostile subscription would stall the IO thread that runs
// matching.  Reject the classic shape — a group containing an
// unbounded quantifier that is itself unbounded-quantified, e.g.
// (a+)+ / (.*)* / ([a-z]+){2,}.  Deliberately conservative: a false
// positive just means the rule never matches.
static bool hasNestedQuantifiers(const QString &pattern)
{
    QList<bool> groupHasQuantifier;
    bool escaped = false;
    bool inClass = false;
    const int size = pattern.size();
    for (int i = 0; i < size; ++i) {
        const QChar c = pattern.at(i);
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (inClass) {
            if (c == QLatin1Char(']'))
                inClass = false;
            continue;
        }
        if (c == QLatin1Char('[')) {
            inClass = true;
            continue;
        }
        if (c == QLatin1Char('(')) {
            groupHasQuantifier.append(false);
            continue;
        }
        if (c == QLatin1Char('*') || c == QLatin1Char('+')
            || c == QLatin1Char('{')) {
            if (!groupHasQuantifier.isEmpty())
                groupHasQuantifier.last() = true;
            continue;
        }
        if (c != QLatin1Char(')') || groupHasQuantifier.isEmpty())
            continue;
        const bool inner = groupHasQuantifier.takeLast();
        // A quantifier inside a nested group still counts for the
        // enclosing one: ((a+))* is as catastrophic as (a+)*.
        if (inner && !groupHasQuantifier.isEmpty())
            groupHasQuantifier.last() = true;
        if (!inner)
            continue;
        // The group is only dangerous when its own repetition is not
        // bounded to a single iteration: * + {n,} {n,m>m} {n>1}.
        if (i + 1 >= size)
            continue;
        const QChar q = pattern.at(i + 1);
        if (q == QLatin1Char('*') || q == QLatin1Char('+'))
            return true;
        if (q == QLatin1Char('{')) {
            int j = i + 2;
            int minReps = 0, maxReps = 0;
            bool comma = false, sawMaxDigit = false;
            while (j < size && pattern.at(j) != QLatin1Char('}')) {
                if (pattern.at(j).isDigit()) {
                    if (comma) {
                        maxReps = maxReps * 10 + pattern.at(j).digitValue();
                        sawMaxDigit = true;
                    } else {
                        minReps = minReps * 10 + pattern.at(j).digitValue();
                    }
                } else if (pattern.at(j) == QLatin1Char(',')) {
                    comma = true;
                } else {
                    break;
                }
                ++j;
            }
            if (j < size && pattern.at(j) == QLatin1Char('}')) {
                // {n,} is unbounded; {n,m} is dangerous when m > n.
                if ((comma && (!sawMaxDigit || maxReps > qMax(minReps, 1)))
                    || (!comma && minReps > 1))
                    return true;
            }
        }
    }
    return false;
}

// True when spec is a valid, non-catastrophic /regex/ parameter.
static bool isUsableRegExpSpec(const QString &spec)
{
    const QString inner = spec.mid(1, spec.size() - 2);
    const QRegularExpression re(inner);
    return re.isValid() && !hasNestedQuantifiers(inner);
}

AdBlockRule::AdBlockRule(const QString &filter)
{
    setFilter(filter);
}

QString AdBlockRule::filter() const
{
    return m_filter;
}

void AdBlockRule::setFilter(const QString &filter)
{
    m_filter = filter;

    m_cosmetic = false;
    m_cosmeticException = false;
    m_scriptlet = false;
    m_markerOffset = -1;
    m_markerLength = 0;
    m_enabled = true;
    m_exception = false;
    m_supported = true;
    m_important = false;
    m_badFilter = false;
    m_documentException = false;
    m_elemHide = false;
    m_genericHide = false;
    m_genericBlock = false;
    m_party = AnyParty;
    m_typeMask = 0;
    m_notTypeMask = 0;
    m_domainOption.clear();
    m_denyAllow.clear();
    m_redirect.clear();
    m_removeParam.clear();
    m_matchToken.clear();
    bool regExpRule = false;

    if (filter.size() > MaximumFilterLength) {
        m_supported = false;
        return;
    }

    if (filter.startsWith(QLatin1String("!"))
        || filter.trimmed().isEmpty())
        m_enabled = false;

    // Cosmetic-family markers; the earliest one wins.  In uBO/ABP a
    // '#' can only introduce a cosmetic filter (fragment '#' inside a
    // URL pattern is already the end of the matchable string and a
    // single '#' is not a marker).
    static const char *const markers[] = {
        "#@?#", "#@$#", "##", "#@#", "#?#", "#$#", "$$"
    };
    int earliest = -1;
    const char *earliestMarker = 0;
    for (const char *marker : markers) {
        const int offset = filter.indexOf(QLatin1String(marker));
        if (offset != -1 && (earliest == -1 || offset < earliest)) {
            earliest = offset;
            earliestMarker = marker;
        }
    }
    if (earliest != -1) {
        m_cosmetic = true;
        m_markerOffset = earliest;
        m_markerLength = qstrlen(earliestMarker);
        const QLatin1String marker(earliestMarker);
        m_cosmeticException = (marker == QLatin1String("#@#")
                               || marker == QLatin1String("#@?#")
                               || marker == QLatin1String("#@$#"));
        m_scriptlet = (marker == QLatin1String("#$#")
                       || marker == QLatin1String("#@$#")
                       || (marker == QLatin1String("##")
                           && cosmeticBody().startsWith(QLatin1String("+js(")))
                       || (marker == QLatin1String("#?#")
                           && cosmeticBody().startsWith(QLatin1String("+js("))));
        if (marker == QLatin1String("$$"))
            m_supported = false; // uBO HTML filters need response rewriting
        return;
    }

    QString parsedLine = filter;
    if (parsedLine.startsWith(QLatin1String("@@"))) {
        m_exception = true;
        parsedLine = parsedLine.mid(2);
    }
    if (parsedLine.startsWith(QLatin1Char('/'))) {
        if (parsedLine.endsWith(QLatin1Char('/'))) {
            parsedLine = parsedLine.mid(1);
            parsedLine = parsedLine.left(parsedLine.size() - 1);
            regExpRule = true;
        }
    }
    int options = parsedLine.indexOf(QLatin1String("$"), 0);
    bool matchCase = false;
    if (options >= 0) {
        const QStringList rawOptions =
            parsedLine.mid(options + 1).split(QLatin1Char(','));
        parsedLine = parsedLine.left(options);
        QStringList cleaned;
        cleaned.reserve(rawOptions.count());
        for (QString option : rawOptions) {
            option = option.trimmed();
            if (!option.isEmpty())
                cleaned.append(option);
        }
        matchCase = cleaned.contains(QLatin1String("match-case"));
        parseOptions(cleaned);
    }

    setPattern(parsedLine, regExpRule);
    // Raw /regex/ filters come straight from the list — invalid or
    // catastrophic patterns are kept inert (see hasNestedQuantifiers).
    if (regExpRule
        && (!m_regExp.isValid() || hasNestedQuantifiers(parsedLine)))
        m_supported = false;
    if (matchCase)
        m_regExp.setPatternOptions(QRegularExpression::NoPatternOption);
}

void AdBlockRule::parseOptions(const QStringList &options)
{
    for (const QString &option : options) {
        if (isUnsupportedOption(option)) {
            m_supported = false;
            continue;
        }

        const int eq = option.indexOf(QLatin1Char('='));
        const QString name = (eq == -1) ? option : option.left(eq);
        const QString value = (eq == -1) ? QString() : option.mid(eq + 1);

        if (name == QLatin1String("match-case")) {
            continue; // applied after setPattern() by setFilter()
        } else if (name == QLatin1String("~match-case")) {
            continue; // explicitly case-insensitive (the default)
        } else if (name == QLatin1String("important")) {
            m_important = true;
        } else if (name == QLatin1String("badfilter")) {
            m_badFilter = true;
        } else if (name == QLatin1String("elemhide")) {
            m_elemHide = true;
        } else if (name == QLatin1String("generichide")) {
            m_genericHide = true;
        } else if (name == QLatin1String("genericblock")) {
            m_genericBlock = true;
        } else if (name == QLatin1String("third-party")) {
            m_party = ThirdParty;
        } else if (name == QLatin1String("first-party")
                   || name == QLatin1String("~third-party")) {
            m_party = FirstParty;
        } else if (name == QLatin1String("~first-party")) {
            m_party = ThirdParty;
        } else if (name == QLatin1String("domain")) {
            m_domainOption += value.split(QLatin1Char('|'),
                                          Qt::SkipEmptyParts);
        } else if (name == QLatin1String("denyallow")) {
            m_denyAllow += value.split(QLatin1Char('|'),
                                       Qt::SkipEmptyParts);
        } else if (name == QLatin1String("redirect")
                   || name == QLatin1String("redirect-rule")) {
            // redirect-rule (uBO) = request must not be a main frame;
            // enforced at match time via m_typeMask-free flag storage.
            if (name == QLatin1String("redirect-rule"))
                m_notTypeMask |= typeBit(RtMainFrame);
            m_redirect = value;
        } else if (name == QLatin1String("removeparam")) {
            // A /regex/ spec is compiled per request on the IO thread;
            // drop rules whose spec is invalid or catastrophic.
            if (value.startsWith(QLatin1Char('/'))
                && value.endsWith(QLatin1Char('/'))
                && value.size() > 1
                && !isUsableRegExpSpec(value)) {
                m_supported = false;
                continue;
            }
            m_removeParam = value.isEmpty()
                    ? QLatin1String("*") : value;
        } else if (name == QLatin1String("empty")) {
            m_redirect = QLatin1String("noop.txt"); // uBO shorthand
        } else if (name == QLatin1String("all")) {
            // explicitly no type restriction
        } else if (name == QLatin1String("collapse")
                   || name == QLatin1String("~collapse")
                   || name == QLatin1String("donottrack")
                   || name == QLatin1String("xbl")
                   || name == QLatin1String("dtd")) {
            // Dead types / UI hints; ignored.
        } else if (name.startsWith(QLatin1Char('~'))) {
            bool ok = false;
            const quint32 mask = typeOptionMask(name.mid(1), &ok);
            if (ok)
                m_notTypeMask |= mask;
            // Unknown ~types are ignored.
        } else {
            bool ok = false;
            const quint32 mask = typeOptionMask(name, &ok);
            if (ok)
                m_typeMask |= mask;
            // Unknown options are ignored (the filter still applies).
        }
    }

    // `document` on an exception is also a page-unbreak modifier.
    if (m_exception && (m_typeMask & typeBit(RtMainFrame)))
        m_documentException = true;
}

static bool hostMatchesDomain(const QString &host, const QString &domain)
{
    return host == domain || host.endsWith(QLatin1Char('.') + domain);
}

// hostInList: true if host suffix-matches any entry.  Entries are
// matched as "same or subdomain" per ABP semantics.
static bool hostInList(const QString &host, const QStringList &list)
{
    for (const QString &entry : list) {
        if (hostMatchesDomain(host, entry))
            return true;
    }
    return false;
}

// Approximation of ABP's eTLD+1 party test: hosts in the same
// subdomain tree — or sharing the last two labels — are the same
// party.  No public-suffix list, so two unrelated hosts under a
// multi-level public suffix (a.co.uk / b.co.uk) read as same-party.
static bool sameParty(const QString &a, const QString &b)
{
    if (a.isEmpty() || b.isEmpty())
        return a == b;
    if (hostMatchesDomain(a, b) || hostMatchesDomain(b, a))
        return true;
    const QStringList aParts = a.split(QLatin1Char('.'));
    const QStringList bParts = b.split(QLatin1Char('.'));
    const QString aBase = aParts.size() > 2
        ? aParts.mid(aParts.size() - 2).join(QLatin1Char('.')) : a;
    const QString bBase = bParts.size() > 2
        ? bParts.mid(bParts.size() - 2).join(QLatin1Char('.')) : b;
    return aBase == bBase;
}

QString AdBlockRule::cosmeticDomains() const
{
    if (!m_cosmetic)
        return QString();
    return m_filter.left(m_markerOffset);
}

QString AdBlockRule::cosmeticBody() const
{
    if (!m_cosmetic)
        return QString();
    return m_filter.mid(m_markerOffset + m_markerLength);
}

bool AdBlockRule::networkMatch(const QString &encodedUrl,
                               const QString &documentHost,
                               int resourceType) const
{
    if (m_cosmetic || !m_enabled || !m_supported || m_badFilter) {
#if defined(ADBLOCKRULE_DEBUG)
        qDebug() << "AdBlockRule::" << __FUNCTION__
                 << "cosmetic/disabled/unsupported/badfilter";
#endif
        return false;
    }

    const quint32 bit = typeBit(resourceType);
    if (m_typeMask && !(m_typeMask & bit))
        return false;
    if (m_notTypeMask & bit)
        return false;

    if (!m_domainOption.isEmpty() || m_party != AnyParty
        || !m_denyAllow.isEmpty()) {
        const QString requestHost =
            QUrl::fromEncoded(encodedUrl.toUtf8()).host();
        QString sourceHost = documentHost.isEmpty()
                ? requestHost : documentHost;

        if (!m_denyAllow.isEmpty()
            && hostInList(requestHost, m_denyAllow))
            return false;

        if (m_party != AnyParty) {
            const bool thirdParty = !sameParty(requestHost, sourceHost);
            if (m_party == ThirdParty && !thirdParty)
                return false;
            if (m_party == FirstParty && thirdParty)
                return false;
        }

        if (!m_domainOption.isEmpty()) {
            bool hasPositive = false;
            bool positiveMatch = false;
            for (QString domain : m_domainOption) {
                if (domain.isEmpty())
                    continue;
                if (domain.startsWith(QLatin1Char('~'))) {
                    if (hostMatchesDomain(sourceHost, domain.mid(1)))
                        return false;
                } else {
                    hasPositive = true;
                    if (hostMatchesDomain(sourceHost, domain))
                        positiveMatch = true;
                }
            }
            if (hasPositive && !positiveMatch)
                return false;
        }
    }

    const bool matched = m_regExp.match(encodedUrl).hasMatch();
#if defined(ADBLOCKRULE_DEBUG)
    //qDebug() << "AdBlockRule::" << __FUNCTION__ << encodedUrl << "MATCHED" << matched << filter();
#endif
    return matched;
}

bool AdBlockRule::isException() const
{
    return m_exception;
}

void AdBlockRule::setException(bool exception)
{
    m_exception = exception;
}

bool AdBlockRule::isEnabled() const
{
    return m_enabled;
}

void AdBlockRule::setEnabled(bool enabled)
{
    m_enabled = enabled;
    if (!enabled) {
        m_filter = QLatin1String("!") + m_filter;
    } else {
        m_filter = m_filter.mid(1);
    }
}

QString AdBlockRule::badFilterKey() const
{
    QString key = m_filter;
    key.remove(QLatin1String("$badfilter"));
    key.remove(QLatin1String(",badfilter"));
    key.remove(QLatin1String("badfilter,"));
    return key;
}

QString AdBlockRule::regExpPattern() const
{
    return m_regExp.pattern();
}

static QString convertPatternToRegExp(const QString &wildcardPattern) {
    QString pattern = wildcardPattern;
    pattern.replace(QRegularExpression(QLatin1String("\\*+")), QLatin1String("*"));    // remove multiple wildcards
    pattern.replace(QRegularExpression(QLatin1String("\\^\\|$")), QLatin1String("^")); // remove anchors following separator placeholder
    pattern.replace(QRegularExpression(QLatin1String("^(\\*)")), QString());           // remove leading wildcards
    pattern.replace(QRegularExpression(QLatin1String("(\\*)$")), QString());           // remove trailing wildcards
    pattern.replace(QRegularExpression(QLatin1String("(\\W)")), QLatin1String("\\\\1"));// escape special symbols

    // The steps below have literal before/after strings (the pattern text
    // is already escaped at this point), so plain QString::replace is
    // used: QRegularExpression replacement strings would interpret the
    // backslashes in the replacements themselves.
    // process extended anchor at expression start ("||" was escaped to "\|\|")
    if (pattern.startsWith(QLatin1String("\\|\\|")))
        pattern = QLatin1String("^[\\w\\-]+:\\/+(?!\\/)(?:[^\\/]+\\.)?")
                  + pattern.mid(4);
    // process separator placeholders (escaped "^")
    pattern.replace(QLatin1String("\\^"), QLatin1String("(?:[^\\w\\d\\-.%]|$)"));
    // process anchors at expression start / end (escaped "|")
    if (pattern.startsWith(QLatin1String("\\|")))
        pattern = QLatin1String("^") + pattern.mid(2);
    if (pattern.endsWith(QLatin1String("\\|")))
        pattern = pattern.left(pattern.size() - 2) + QLatin1String("$");
    // replace escaped wildcards by .*
    pattern.replace(QLatin1String("\\*"), QLatin1String(".*"));
    return pattern;
}

void AdBlockRule::setPattern(const QString &pattern, bool isRegExp)
{
    m_matchToken.clear();
    if (!isRegExp) {
        // Every character outside the *^| metacharacters becomes a
        // literal atom in the converted regexp, so the longest run of
        // ASCII between them is a substring any matching URL must
        // contain.  Non-ASCII characters split runs: the request URL
        // is matched in percent-encoded form, where such bytes never
        // appear verbatim the way the regexp expects them anyway.
        int bestStart = 0;
        int bestLength = 0;
        int runStart = 0;
        for (int i = 0; i <= pattern.size(); ++i) {
            const bool separator = i == pattern.size()
                || pattern.at(i).unicode() >= 0x80
                || pattern.at(i) == QLatin1Char('*')
                || pattern.at(i) == QLatin1Char('^')
                || pattern.at(i) == QLatin1Char('|');
            if (!separator)
                continue;
            if (i - runStart > bestLength) {
                bestStart = runStart;
                bestLength = i - runStart;
            }
            runStart = i + 1;
        }
        m_matchToken = pattern.mid(bestStart, bestLength);
    }
    m_regExp = QRegularExpression(isRegExp ? pattern : convertPatternToRegExp(pattern),
                                  QRegularExpression::CaseInsensitiveOption);
}
