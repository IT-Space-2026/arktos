/*
 * Copyright 2026 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef REMOTELISTFETCH_H
#define REMOTELISTFETCH_H

#include "adblockmanager.h"

#include <qbytearray.h>
#include <qdatetime.h>
#include <qhostaddress.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qurl.h>

// CA08: the HTTPS-gated list-fetch vocabulary DomainBlocklist and
// TrancoList each carried — TELEM01's consent+staleness gate, the
// https/loopback source check and the reply teardown.
namespace RemoteListFetch {

// The remote-list refresh is allowed only when the user opted into
// remote content (enabling the blocker or answering the first-launch
// prompt is the "yes" — until then the vendored seed alone works)
// and the last fetch is older than refreshDays.
inline bool updateDue(const QDateTime &lastUpdate, int refreshDays)
{
    if (!AdBlockManager::remoteListsAllowed())
        return false;
    return !lastUpdate.isValid()
        || lastUpdate.addDays(refreshDays) < QDateTime::currentDateTime();
}

// HTTPS-only sources — a plaintext feed could be rewritten on the
// wire (unblocking a malicious domain, faking rank coverage).  A
// loopback source is the test fixture's exception.  `listName` tags
// the refusal warning.
inline bool sourceAllowed(const QUrl &url, const char *listName)
{
    const QHostAddress literal(url.host());
    const bool loopback = literal.isLoopback()
        || url.host().compare(QLatin1String("localhost"),
                              Qt::CaseInsensitive) == 0;
    if (url.scheme() != QLatin1String("https") && !loopback) {
        qWarning().noquote() << listName
                             << ": refusing non-https source" << url;
        return false;
    }
    return true;
}

// What a list fetcher needs out of a finished reply — readAll is
// skipped entirely past the size cap (a feed larger than that is an
// error page or an attack, not a domain list).
struct Reply {
    QByteArray body;       // empty on error or oversize
    QUrl redirect;         // valid when the reply carried a target
    bool failed = false;   // transport-level failure (logged by caller)
    QString errorString;
};

// Drains `reply` — closes and deleteLaters it.  The caller owns the
// redirect-hop and body-consume decisions.
inline Reply take(QNetworkReply *reply, qint64 maxBytes)
{
    Reply out;
    out.body = reply->size() <= maxBytes
        ? reply->readAll() : QByteArray();
    out.redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    out.failed = reply->error() != QNetworkReply::NoError;
    out.errorString = reply->errorString();
    reply->close();
    reply->deleteLater();
    return out;
}

} // namespace RemoteListFetch

#endif // REMOTELISTFETCH_H
