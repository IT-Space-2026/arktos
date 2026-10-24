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

#include "trancolist.h"

#include "adblockmanager.h"
#include "browserpaths.h"
#include "networkaccessmanager.h"
#include "profilesettings.h"

#include <qcoreapplication.h>
#include <qdebug.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qhostaddress.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qpointer.h>
#include <qsavefile.h>

#if defined(ARKTOS_RUSTCORE)
#include "rustcorebridge.h"

#include <arktos_core.h>

#include <qjsonobject.h>
#endif

// The Majestic CSV sits around 80 MB today, so the ceiling moves
// past the adblock-list 64 MB — a body larger than this is still an
// error page or an attack, not a rank list.
static const qint64 MaximumListSize = 128 * 1024 * 1024;
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

QString TrancoList::trancoFileName()
{
    return QLatin1String("tranco-list.dat");
}

QString TrancoList::majesticFileName()
{
    return QLatin1String("majestic-million.csv");
}

QString TrancoList::listFilePath()
{
    return BrowserPaths::dataFilePath(trancoFileName());
}

QString TrancoList::majesticFilePath()
{
    return BrowserPaths::dataFilePath(majesticFileName());
}

QList<TrancoList::Source> TrancoList::defaultSources()
{
    // Tranco's permanent "latest daily list" link — it 307-redirects
    // to the dated top-1m.csv.zip (the https redirect rule below
    // carries that hop).  The response body lands verbatim in
    // tranco-list.dat; the crate extracts the *.csv member on load.
    // SEC19: Majestic Million is the second rank source — a plain
    // multi-column CSV (no zip wrapper) committed under its own file
    // name so the bodies never mix.
    return {
        Source(QUrl(QLatin1String("https://tranco-list.eu/top-1m.csv.zip")),
               trancoFileName()),
        Source(QUrl(QLatin1String(
                   "https://downloads.majestic.com/majestic_million.csv")),
               majesticFileName()),
    };
}

QDateTime TrancoList::trancoLastUpdate() const
{
    return ProfileSettings()
        .value(QLatin1String("privacy/trancoListUpdated"))
        .toDateTime();
}

QDateTime TrancoList::majesticLastUpdate() const
{
    return ProfileSettings()
        .value(QLatin1String("privacy/majesticListUpdated"))
        .toDateTime();
}

QDateTime TrancoList::lastUpdate() const
{
    // The honest answer is the stalest list — a source that never
    // fetched is older than any date, so either side missing stamps
    // the whole row "never" and triggers a refetch.
    const QDateTime tranco = trancoLastUpdate();
    const QDateTime majestic = majesticLastUpdate();
    if (!tranco.isValid() || !majestic.isValid())
        return QDateTime();
    return qMin(tranco, majestic);
}

void TrancoList::setSourcesForTest(const QList<Source> &sources)
{
    m_sources = sources;
}

void TrancoList::updateIfStale()
{
#if !defined(ARKTOS_RUSTCORE)
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
#if !defined(ARKTOS_RUSTCORE)
    emit updateFinished(false);
    return;
#else
    if (!m_replies.isEmpty())
        return;   // a fetch is already in flight
    m_bodies.clear();
    m_replyFile.clear();
    for (const Source &source : m_sources) {
        // List downloads are HTTPS — a plaintext list could be
        // rewritten on the wire to fake rank coverage.  A loopback
        // source is the test fixture's exception.
        const QUrl &url = source.url;
        const QHostAddress literal(url.host());
        const bool loopback = literal.isLoopback()
            || url.host().compare(QLatin1String("localhost"),
                                  Qt::CaseInsensitive) == 0;
        if (url.scheme() != QLatin1String("https") && !loopback) {
            qWarning() << "TrancoList: refusing non-https source"
                       << url;
            continue;
        }
        fetch(url, source.fileName);
    }
    if (m_replies.isEmpty())
        emit updateFinished(false);
#endif
}

void TrancoList::fetch(const QUrl &url, const QString &fileName)
{
    QNetworkReply *reply =
        NetworkAccessManager::instance()->get(QNetworkRequest(url));
    m_replies.append(reply);
    m_replyFile.insert(reply, fileName);
    connect(reply, &QNetworkReply::finished,
            this, &TrancoList::replyFinished);
}

void TrancoList::replyFinished()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    m_replies.removeAll(reply);
    const QString fileName = m_replyFile.take(reply);

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
        // the source list needs).  The hop keeps the source's drop
        // point — it is the same list, relocated.
        const QUrl target = reply->url().resolved(redirect);
        if (target.scheme() == QLatin1String("https"))
            fetch(target, fileName);
        else
            qWarning() << "TrancoList: refusing non-https redirect"
                       << target;
    } else if (!body.isEmpty() && !fileName.isEmpty()) {
        QByteArray &merged = m_bodies[fileName];
        merged += body;
        // Line-oriented bodies get the record-terminating newline
        // the blocklist feeds carry; a zip container must not —
        // trailing bytes invalidate its central directory.
        if (!merged.startsWith("PK\x03\x04")
            && !merged.endsWith('\n'))
            merged += '\n';
    }

    if (m_replies.isEmpty())
        maybeCommit();
}

void TrancoList::maybeCommit()
{
#if defined(ARKTOS_RUSTCORE)
    // Commit each drop point separately: the bodies have different
    // container shapes, so they land under their own file names and
    // a bad fetch can only ever cost its own file.
    QStringList written;
    bool failed = false;
    for (auto it = m_bodies.constBegin(); it != m_bodies.constEnd();
         ++it) {
        const QString &fileName = it.key();
        const QByteArray &body = it.value();
        if (fileName.isEmpty() || fileName.contains(QLatin1Char('/'))
            || body.trimmed().isEmpty())
            continue;
        const QString path =
            BrowserPaths::dataFilePath(fileName);
        QByteArray out;
        // A zip body must land verbatim — a prepended text header
        // would break the crate's PK-magic sniff.  Plaintext CSV
        // gets the provenance comment the blocklist file carries.
        if (!body.startsWith("PK\x03\x04")) {
            out += "# Arktos site-rank list update\n# sources: ";
            for (const Source &source : m_sources) {
                if (source.fileName == fileName) {
                    out += source.url.host().toUtf8();
                    out += ' ';
                }
            }
            out += '\n';
        }
        out += body;
        QSaveFile file(path);
        if (file.open(QIODevice::WriteOnly)
            && file.write(out) == out.size()
            && file.commit()) {
            written += fileName;
        } else {
            qWarning() << "TrancoList: cannot write" << path;
            failed = true;
        }
    }
    if (written.isEmpty()) {
        emit updateFinished(false);
        return;
    }
    // Point the core at the data dir (the SecureStore shim may not
    // have run yet), then validate every drop point through the
    // crate before merging: a body that yields no usable entries
    // (corrupt zip, error page) is junk — drop it so it can neither
    // fail the reload nor jam a sibling's future commits, and sweep
    // any stale corrupt file left by an interrupted run too.
    // MB07: the envelope ops bind the ambient persona's dir inside
    // the crate per call — the rc_set_data_dir anchor is the legacy
    // fallback.
    RustCoreBridge *bridge = RustCoreBridge::serving();
    const QJsonObject scope{{QLatin1String("profile"),
                             BrowserPaths::activeProfileId()}};
    if (!bridge) {
        const QByteArray dir =
            QFileInfo(listFilePath()).absolutePath().toUtf8();
        rc_set_data_dir(dir.constData());
    }
    const QStringList known = { trancoFileName(), majesticFileName() };
    QStringList kept;
    for (const QString &name : known) {
        const QString path = BrowserPaths::dataFilePath(name);
        if (!QFile::exists(path))
            continue;
        const size_t entries = bridge
            ? size_t(bridge
                     ->mwQuery(AcMw::Policy,
                               AcMw::PolicyTrancoFileEntries,
                               QJsonObject{{QLatin1String("profile"),
                                            BrowserPaths::activeProfileId()},
                                           {QLatin1String("name"), name}})
                     .toObject()
                     .value(QLatin1String("entries"))
                     .toInteger())
            : rc_tranco_file_entries(name.toUtf8().constData());
        if (entries > 0) {
            kept += name;
        } else {
            qWarning() << "TrancoList:" << name
                       << "yielded no usable entries — dropping it";
            QFile::remove(path);
            failed = true;
        }
    }
    const bool reloaded = bridge
        ? bridge
              ->mwCommand(AcMw::Policy, AcMw::PolicyTrancoReload, scope)
              .isObject()
        : rc_tranco_reload() == RC_OK;
    if (reloaded) {
        const QDateTime now = QDateTime::currentDateTime();
        ProfileSettings settings;
        for (const QString &name : written) {
            if (!kept.contains(name))
                continue;
            settings.setValue(
                name == majesticFileName()
                    ? QLatin1String("privacy/majesticListUpdated")
                    : QLatin1String("privacy/trancoListUpdated"),
                now);
        }
        // Partial success still commits the good files — the failure
        // flag is honest about the fetch as a whole.
        emit updateFinished(!failed);
        return;
    }
    qWarning() << "TrancoList: merged table did not reload";
#endif
    emit updateFinished(false);
}
