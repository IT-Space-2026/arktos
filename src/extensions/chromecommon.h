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

#ifndef CHROMECOMMON_H
#define CHROMECOMMON_H

#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qregularexpression.h>
#include <qstring.h>
#include <qurl.h>

// CA08: the shared chrome.* bridge vocabulary every extension-side
// module carried as anonymous-namespace copies — the {ok,result} /
// {ok:false,error} reply envelopes and the Chrome match-pattern
// grammar (three verbatim copies of the matcher drifted apart in
// comments only; the security-relevant URL matching now has one
// implementation).
//
// Names intentionally keep the okReply/errReply spelling the call
// sites already use — they are inline so the copies simply delete
// their local versions.

// Every invoke() answers {ok:true,result:...} or
// {ok:false,error:"<typed code>"} — the JS shim turns failures into
// health-surface gaps.
inline QJsonObject okReply(const QJsonValue &result = QJsonValue())
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), true);
    if (!result.isUndefined())
        o.insert(QStringLiteral("result"), result);
    return o;
}

inline QJsonObject errReply(const QString &code)
{
    QJsonObject o;
    o.insert(QStringLiteral("ok"), false);
    o.insert(QStringLiteral("error"), code);
    return o;
}

// Chrome match-pattern check against a concrete URL —
// scheme://host/path decomposition, '*' wildcards and the
// '<all_urls>' meta-pattern.  file:/ftp: URLs are never covered:
// file-scheme access stays ungranted (isAllowedFileSchemeAccess is
// false and there is no toggle) and Chrome retired ftp outright.
inline bool matchPatternCovers(const QString &pattern, const QUrl &url)
{
    const QString scheme = url.scheme();
    // CA09/S4: no file-scheme access exists in this runtime —
    // chrome.extension.isAllowedFileSchemeAccess answers false and
    // there is no user toggle to grant it — so a declared pattern
    // must never cover file: (Chrome gates the same coverage behind
    // that exact per-extension toggle).  ftp: went with Chrome's own
    // removal of the scheme.
    if (scheme == QLatin1String("file")
            || scheme == QLatin1String("ftp"))
        return false;
    if (pattern == QLatin1String("<all_urls>"))
        return scheme == QLatin1String("http")
            || scheme == QLatin1String("https");
    const int sep = pattern.indexOf(QLatin1String("://"));
    if (sep < 0)
        return false;
    const QString pscheme = pattern.left(sep);
    const QString rest = pattern.mid(sep + 3);
    const int slash = rest.indexOf(QLatin1Char('/'));
    QString phost = slash < 0 ? rest : rest.left(slash);
    const QString ppath =
        slash < 0 ? QStringLiteral("/*") : rest.mid(slash);
    // Chrome lets a pattern pin a port ('127.0.0.1:*') — split it
    // off before the host compare.
    QString pport;
    const int colon = phost.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        pport = phost.mid(colon + 1);
        phost = phost.left(colon);
    }
    if (pscheme == QLatin1String("*")) {
        // '*://' covers http + https only (Chrome semantics).
        if (scheme != QLatin1String("http")
                && scheme != QLatin1String("https"))
            return false;
    } else if (pscheme != scheme) {
        return false;
    }
    if (!pport.isEmpty() && pport != QLatin1String("*")) {
        const int defPort = scheme == QLatin1String("https") ? 443
            : scheme == QLatin1String("http") ? 80 : -1;
        if (url.port(defPort) != pport.toInt())
            return false;
    }
    const QString host = url.host();
    if (phost == QLatin1String("*")) {
        // any host
    } else if (phost.startsWith(QLatin1String("*."))) {
        const QString base = phost.mid(2);
        if (host != base && !host.endsWith(QLatin1Char('.') + base))
            return false;
    } else if (phost != host) {
        return false;
    }
    QString path = url.path(QUrl::FullyEncoded);
    if (url.hasQuery())
        path += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    const QRegularExpression re(
        QRegularExpression::wildcardToRegularExpression(ppath));
    return re.match(path).hasMatch();
}

#endif // CHROMECOMMON_H
