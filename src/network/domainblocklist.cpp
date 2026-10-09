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

#include "domainblocklist.h"

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

// Same ceiling the adblock subscriptions apply — a feed larger than
// this is an error page or an attack, not a domain list.
static const qint64 MaximumListSize = 64 * 1024 * 1024;
// The refresh cadence AdBlockSubscription uses.
static const int RefreshIntervalDays = 7;

DomainBlocklist *DomainBlocklist::instance()
{
    static DomainBlocklist *s_instance = new DomainBlocklist(qApp);
    return s_instance;
}

DomainBlocklist::DomainBlocklist(QObject *parent)
    : QObject(parent)
    , m_sources(defaultSources())
{
}

QString DomainBlocklist::listFilePath()
{
    return BrowserPaths::dataFilePath(QLatin1String("blocklist-domains.txt"));
}

QList<QUrl> DomainBlocklist::defaultSources()
{
    // Both verified live and stable — URLhaus's hostfile feed is the
    // maintained domain export (malware/C2/droppers), OpenPhish's
    // community feed is one phishing URL per line.  The rustcore
    // parser understands both row shapes natively, so the bodies are
    // concatenated verbatim.
    return {
        QUrl(QLatin1String("https://urlhaus.abuse.ch/downloads/hostfile/")),
        QUrl(QLatin1String("https://openphish.com/feed.txt")),
    };
}

QDateTime DomainBlocklist::lastUpdate() const
{
    return QSettings()
        .value(QLatin1String("privacy/domainBlocklistUpdated"))
        .toDateTime();
}

void DomainBlocklist::setSourcesForTest(const QList<QUrl> &sources)
{
    m_sources = sources;
}

void DomainBlocklist::updateIfStale()
{
#if !defined(ARORA_RUSTCORE)
    return;   // no consumer for the file — fetch nothing
#endif
    // TELEM01: identical consent gate to the remote filter lists —
    // enabling the blocker or the first-launch prompt is the user's
    // "yes"; until then the vendored seed alone does the blocking.
    if (!AdBlockManager::remoteListsAllowed())
        return;
    const QDateTime last = lastUpdate();
    if (last.isValid()
        && last.addDays(RefreshIntervalDays) >= QDateTime::currentDateTime())
        return;
    updateNow();
}

void DomainBlocklist::updateNow()
{
#if !defined(ARORA_RUSTCORE)
    emit updateFinished(false);
    return;
#else
    if (!m_replies.isEmpty())
        return;   // a fetch is already in flight
    m_merged.clear();
    for (const QUrl &url : m_sources) {
        // List downloads are HTTPS — a plaintext feed could be
        // rewritten on the wire to unblock a malicious domain.  A
        // loopback source is the test fixture's exception.
        const QHostAddress literal(url.host());
        const bool loopback = literal.isLoopback()
            || url.host().compare(QLatin1String("localhost"),
                                  Qt::CaseInsensitive) == 0;
        if (url.scheme() != QLatin1String("https") && !loopback) {
            qWarning() << "DomainBlocklist: refusing non-https source"
                       << url;
            continue;
        }
        fetch(url);
    }
    if (m_replies.isEmpty())
        emit updateFinished(false);
#endif
}

void DomainBlocklist::fetch(const QUrl &url)
{
    QNetworkReply *reply =
        NetworkAccessManager::instance()->get(QNetworkRequest(url));
    m_replies.append(reply);
    connect(reply, &QNetworkReply::finished,
            this, &DomainBlocklist::replyFinished);
}

void DomainBlocklist::replyFinished()
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
        qWarning() << "DomainBlocklist: fetch failed:" << errorString;
    } else if (redirect.isValid()) {
        // One redirect hop, same as AdBlockSubscription — must land
        // on https as well (a downgrade would strip the integrity
        // the source list needs).
        const QUrl target = reply->url().resolved(redirect);
        if (target.scheme() == QLatin1String("https"))
            fetch(target);
        else
            qWarning() << "DomainBlocklist: refusing non-https redirect"
                       << target;
    } else if (!body.isEmpty()) {
        m_merged += body;
        if (!m_merged.endsWith('\n'))
            m_merged += '\n';
    }

    if (m_replies.isEmpty())
        maybeCommit();
}

void DomainBlocklist::maybeCommit()
{
#if defined(ARORA_RUSTCORE)
    if (!m_merged.trimmed().isEmpty()) {
        const QString path = listFilePath();
        QByteArray out =
            "# Arora SEC18 merged domain blocklist\n"
            "# sources: ";
        for (const QUrl &source : m_sources) {
            out += source.host().toUtf8();
            out += ' ';
        }
        out += '\n';
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
            rc_blocklist_reload();
            QSettings().setValue(
                QLatin1String("privacy/domainBlocklistUpdated"),
                QDateTime::currentDateTime());
            emit updateFinished(true);
            return;
        }
        qWarning() << "DomainBlocklist: cannot write" << path;
    }
#endif
    emit updateFinished(false);
}
