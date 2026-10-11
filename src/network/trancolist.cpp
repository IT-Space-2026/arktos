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

#include "trancolist.h"

#include "adblockmanager.h"
#include "browserpaths.h"
#include "networkaccessmanager.h"

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qfileinfo.h>
#include <qhostaddress.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qpointer.h>
#include <qsavefile.h>
#include <qsettings.h>

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

// Same ceiling the adblock subscriptions apply — the top-1m.csv.zip
// sits around 10 MB, so a body larger than this is an error page or
// an attack, not a rank list.
static const qint64 MaximumListSize = 64 * 1024 * 1024;
// The refresh cadence AdBlockSubscription uses.
static const int RefreshIntervalDays = 7;

TrancoList *TrancoList::instance()
{
    static TrancoList *s_instance = new TrancoList(qApp);
    return s_instance;
}

TrancoList::TrancoList(QObject *parent)
    : QObject(parent)
    , m_sources(defaultSources())
{
}

QString TrancoList::listFilePath()
{
    return BrowserPaths::dataFilePath(QLatin1String("tranco-list.dat"));
}

QList<QUrl> TrancoList::defaultSources()
{
    // Tranco's permanent "latest daily list" link — it 307-redirects
    // to the dated top-1m.csv.zip (the https redirect rule below
    // carries that hop).  The response body lands verbatim in
    // tranco-list.dat; the crate extracts the *.csv member on load.
    return {
        QUrl(QLatin1String("https://tranco-list.eu/top-1m.csv.zip")),
    };
}

QDateTime TrancoList::lastUpdate() const
{
    return QSettings()
        .value(QLatin1String("privacy/trancoListUpdated"))
        .toDateTime();
}

void TrancoList::setSourcesForTest(const QList<QUrl> &sources)
{
    m_sources = sources;
}

void TrancoList::updateIfStale()
{
#if !defined(ARORA_RUSTCORE)
    return;   // no consumer for the file — fetch nothing
#endif
    // TELEM01: identical consent gate to the remote filter lists —
    // enabling the blocker or the first-launch prompt is the user's
    // "yes"; until then the vendored seed alone does the ranking.
    if (!AdBlockManager::remoteListsAllowed())
        return;
    const QDateTime last = lastUpdate();
    if (last.isValid()
        && last.addDays(RefreshIntervalDays) >= QDateTime::currentDateTime())
        return;
    updateNow();
}

void TrancoList::updateNow()
{
#if !defined(ARORA_RUSTCORE)
    emit updateFinished(false);
    return;
#else
    if (!m_replies.isEmpty())
        return;   // a fetch is already in flight
    m_merged.clear();
    for (const QUrl &url : m_sources) {
        // List downloads are HTTPS — a plaintext list could be
        // rewritten on the wire to fake rank coverage.  A loopback
        // source is the test fixture's exception.
        const QHostAddress literal(url.host());
        const bool loopback = literal.isLoopback()
            || url.host().compare(QLatin1String("localhost"),
                                  Qt::CaseInsensitive) == 0;
        if (url.scheme() != QLatin1String("https") && !loopback) {
            qWarning() << "TrancoList: refusing non-https source"
                       << url;
            continue;
        }
        fetch(url);
    }
    if (m_replies.isEmpty())
        emit updateFinished(false);
#endif
}

void TrancoList::fetch(const QUrl &url)
{
    QNetworkReply *reply =
        NetworkAccessManager::instance()->get(QNetworkRequest(url));
    m_replies.append(reply);
    connect(reply, &QNetworkReply::finished,
            this, &TrancoList::replyFinished);
}

void TrancoList::replyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);

    const QByteArray body = reply->size() <= MaximumListSize
        ? reply->readAll() : QByteArray();
    const QUrl redirect = reply->attribute(
        QNetworkRequest::RedirectionTargetAttribute).toUrl();
    const QNetworkReply::NetworkError error = reply->error();
    const QString errorString = reply->errorString();
    reply->close();
    reply->deleteLater();

    if (error != QNetworkReply::NoError) {
        qWarning() << "TrancoList: fetch failed:" << errorString;
    } else if (redirect.isValid()) {
        // One redirect hop, same as AdBlockSubscription — must land
        // on https as well (a downgrade would strip the integrity
        // the source list needs).
        const QUrl target = reply->url().resolved(redirect);
        if (target.scheme() == QLatin1String("https"))
            fetch(target);
        else
            qWarning() << "TrancoList: refusing non-https redirect"
                       << target;
    } else if (!body.isEmpty()) {
        m_merged += body;
        // Line-oriented bodies get the record-terminating newline
        // the blocklist feeds carry; a zip container must not —
        // trailing bytes invalidate its central directory.
        if (!m_merged.startsWith("PK\x03\x04")
            && !m_merged.endsWith('\n'))
            m_merged += '\n';
    }

    if (m_replies.isEmpty())
        maybeCommit();
}

void TrancoList::maybeCommit()
{
#if defined(ARORA_RUSTCORE)
    if (!m_merged.trimmed().isEmpty()) {
        const QString path = listFilePath();
        QByteArray out;
        // A zip body must land verbatim — a prepended text header
        // would break the crate's PK-magic sniff.  Plaintext CSV
        // gets the provenance comment the blocklist file carries.
        if (!m_merged.startsWith("PK\x03\x04")) {
            out += "# Arora TRUST04 Tranco list update\n# sources: ";
            for (const QUrl &source : m_sources) {
                out += source.host().toUtf8();
                out += ' ';
            }
            out += '\n';
        }
        out += m_merged;
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)
            && file.write(out) == out.size()
            && file.commit()) {
            // Point the core at the data dir (the SecureStore shim
            // may not have run yet) and re-merge seed ∪ override.
            const QByteArray dir =
                QFileInfo(path).absolutePath().toUtf8();
            rc_set_data_dir(dir.constData());
            if (rc_tranco_reload() == RC_OK) {
                QSettings().setValue(
                    QLatin1String("privacy/trancoListUpdated"),
                    QDateTime::currentDateTime());
                emit updateFinished(true);
                return;
            }
            // The downloaded body yielded no usable entries (corrupt
            // zip, error page) — the seed keeps ranking; the file is
            // left for diagnosis but reports failure.
            qWarning() << "TrancoList: downloaded list did not parse";
            emit updateFinished(false);
            return;
        }
        qWarning() << "TrancoList: cannot write" << path;
    }
#endif
    emit updateFinished(false);
}
