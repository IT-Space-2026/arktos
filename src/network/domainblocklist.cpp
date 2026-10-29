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

#include "domainblocklist.h"

#include "adblockmanager.h"
#include "browserpaths.h"
#include "networkaccessmanager.h"
#include "profilesettings.h"
#include "remotelistfetch.h"

#include <qcoreapplication.h>
#include <qdebug.h>
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
    // All verified live and stable — URLhaus's hostfile feed is the
    // maintained domain export (malware/C2/droppers), OpenPhish's
    // community feed is one phishing URL per line, and SEC19 adds
    // Phishing.Database's ACTIVE domain list (MIT, ~11 MB of bare
    // domains — under the same 64 MB response cap).  The rustcore
    // parser understands every row shape natively, so the bodies are
    // concatenated verbatim.
    return {
        QUrl(QLatin1String("https://urlhaus.abuse.ch/downloads/hostfile/")),
        QUrl(QLatin1String("https://openphish.com/feed.txt")),
        QUrl(QLatin1String(
            "https://raw.githubusercontent.com/mitchellkrogza/"
            "Phishing.Database/master/phishing-domains-ACTIVE.txt")),
    };
}

QDateTime DomainBlocklist::lastUpdate() const
{
    return ProfileSettings()
        .value(QLatin1String("privacy/domainBlocklistUpdated"))
        .toDateTime();
}

void DomainBlocklist::setSourcesForTest(const QList<QUrl> &sources)
{
    m_sources = sources;
}

void DomainBlocklist::updateIfStale()
{
#if !defined(ARKTOS_RUSTCORE)
    return;   // no consumer for the file — fetch nothing
#endif
    // TELEM01: identical consent gate to the remote filter lists —
    // enabling the blocker or the first-launch prompt is the user's
    // "yes"; until then the vendored seed alone does the blocking.
    if (!RemoteListFetch::updateDue(lastUpdate(), RefreshIntervalDays))
        return;
    updateNow();
}

void DomainBlocklist::updateNow()
{
#if !defined(ARKTOS_RUSTCORE)
    emit updateFinished(false);
    return;
#else
    if (!m_replies.isEmpty())
        return;   // a fetch is already in flight
    m_merged.clear();
    for (const QUrl &url : m_sources) {
        // List downloads are HTTPS — a plaintext feed could be
        // rewritten on the wire to unblock a malicious domain.
        if (RemoteListFetch::sourceAllowed(url, "DomainBlocklist"))
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

    const RemoteListFetch::Reply got =
        RemoteListFetch::take(reply, MaximumListSize);
    const QByteArray &body = got.body;
    const QUrl &redirect = got.redirect;

    if (got.failed) {
        qWarning() << "DomainBlocklist: fetch failed:"
                   << got.errorString;
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
#if defined(ARKTOS_RUSTCORE)
    if (!m_merged.trimmed().isEmpty()) {
        const QString path = listFilePath();
        QByteArray out =
            "# Arktos SEC18 merged domain blocklist\n"
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
            // MB07: the envelope command binds the ambient persona's
            // dir inside the crate — the rc_set_data_dir anchor is
            // the legacy fallback.
            if (RustCoreBridge *bridge = RustCoreBridge::serving()) {
                bridge->mwCommand(
                    AcMw::Policy, AcMw::PolicyBlocklistReload,
                    QJsonObject{{QLatin1String("profile"),
                                 BrowserPaths::activeProfileId()}});
            } else {
                const QByteArray dir =
                    QFileInfo(path).absolutePath().toUtf8();
                rc_set_data_dir(dir.constData());
                rc_blocklist_reload();
            }
            ProfileSettings().setValue(
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
