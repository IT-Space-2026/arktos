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

#ifndef COOKIERECORD_H
#define COOKIERECORD_H

#include <qdatetime.h>
#include <qjsonobject.h>
#include <qnetworkcookie.h>
#include <qstring.h>

// CA13: the rust store's cookie record schema — one builder for both
// the C++ jar's mirror writes (CookieJar::rustCookieSet, which fills
// "container" with the persona partition) and the extension
// namespace's record reads (chrome.cookies, always the empty default
// partition).  The two anonymous-namespace copies had already
// drifted a comment apart.

inline QJsonObject cookieRecordJson(const QNetworkCookie &cookie,
                                    const QString &container = QString())
{
    QString domain = cookie.domain();
    const bool hostOnly = !domain.startsWith(QLatin1Char('.'));
    if (!hostOnly)
        domain = domain.mid(1);
    QJsonObject o;
    o.insert(QLatin1String("container"), container);
    o.insert(QLatin1String("name"),
             QString::fromUtf8(cookie.name()));
    o.insert(QLatin1String("value"),
             QString::fromUtf8(cookie.value()));
    o.insert(QLatin1String("domain"), domain);
    o.insert(QLatin1String("path"), cookie.path().isEmpty()
             ? QStringLiteral("/") : cookie.path());
    if (cookie.isSessionCookie())
        o.insert(QLatin1String("expires"), QJsonValue());
    else
        o.insert(QLatin1String("expires"),
                 double(cookie.expirationDate().toMSecsSinceEpoch()));
    o.insert(QLatin1String("secure"), cookie.isSecure());
    o.insert(QLatin1String("httpOnly"), cookie.isHttpOnly());
    o.insert(QLatin1String("sameSite"), int(cookie.sameSitePolicy()));
    o.insert(QLatin1String("hostOnly"), hostOnly);
    o.insert(QLatin1String("created"),
             double(QDateTime::currentMSecsSinceEpoch()));
    return o;
}

#endif // COOKIERECORD_H
