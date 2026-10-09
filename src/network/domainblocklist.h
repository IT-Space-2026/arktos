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

#ifndef DOMAINBLOCKLIST_H
#define DOMAINBLOCKLIST_H

#include <qdatetime.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qurl.h>

class QNetworkReply;

// SEC18: fetches the remote anti-phishing/malware domain feeds and
// drops them where the rustcore blocklist module picks them up —
// <data dir>/blocklist-domains.txt.  The runtime set is the union of
// the vendored seed and this file, so a failed or poisoned fetch can
// only ever shrink back to the seed, never leave the feature empty.
//
// The fetch rides the adblock-list rules: the app-side
// NetworkAccessManager performs it, HTTPS sources only, and the
// automatic refresh is consent-gated exactly like remote filter
// lists (TELEM01 — AdBlockManager::remoteListsAllowed()).  Matching
// itself never leaves the machine; these are list downloads, not
// per-URL lookups.
//
// Without CONFIG+=rustcore the whole class is inert: there is no
// consumer for the file, so nothing is fetched.
class DomainBlocklist : public QObject
{
    Q_OBJECT

public:
    static DomainBlocklist *instance();

    // <data dir>/blocklist-domains.txt — rustcore's override file.
    static QString listFilePath();
    // The upstream feeds (URLhaus hostfile + OpenPhish community).
    static QList<QUrl> defaultSources();

    // Automatic path: fetch only when remote-list consent was granted
    // AND the stored copy is missing or older than the refresh
    // interval — the same cadence AdBlockSubscription uses.
    void updateIfStale();
    // Explicit fetch — an on-demand update, consent-free like
    // AdBlockSubscription::updateNow().
    void updateNow();

    // Test seams: swap the sources (a loopback fixture may use plain
    // http — the https rule applies to remote sources only) and watch
    // the fetch finish.
    void setSourcesForTest(const QList<QUrl> &sources);
    QDateTime lastUpdate() const;

signals:
    void updateFinished(bool ok);

private slots:
    void replyFinished();

private:
    explicit DomainBlocklist(QObject *parent = nullptr);

    void fetch(const QUrl &url);
    void maybeCommit();

    QList<QUrl> m_sources;
    QList<QPointer<QNetworkReply> > m_replies;
    QByteArray m_merged;
};

#endif // DOMAINBLOCKLIST_H
