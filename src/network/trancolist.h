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

#ifndef TRANCOLIST_H
#define TRANCOLIST_H

#include <qdatetime.h>
#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qurl.h>

class QNetworkReply;

// TRUST04/SEC19: fetches the remote site-rank lists and drops them
// where the rustcore tranco module picks them up — each source lands
// under its own file name in the data dir (the bodies have different
// container shapes, so they are never concatenated):
//   tranco-list.dat      — the Tranco top-1m.csv.zip verbatim; the
//                          crate unzips it on load;
//   majestic-million.csv — the Majestic Million second rank source
//                          (plain GlobalRank,TldRank,Domain,... CSV).
// The runtime table is the vendored top-100k seed merged best-rank-
// wins with these files, so a failed or poisoned fetch can only ever
// shrink coverage back toward the seed, never leave the feature
// empty.
//
// The fetch rides the adblock-list rules: the app-side
// NetworkAccessManager performs it, HTTPS sources only, and the
// automatic refresh is consent-gated exactly like remote filter
// lists (TELEM01 — AdBlockManager::remoteListsAllowed()).  Ranking
// itself never leaves the machine; this is a list download, not a
// per-host lookup.
//
// Without CONFIG+=rustcore the whole class is inert: there is no
// consumer for the files, so nothing is fetched.
class TrancoList : public QObject
{
    Q_OBJECT

public:
    static TrancoList *instance();

    // The override file names the crate merges (see tranco.rs).
    static QString trancoFileName();
    static QString majesticFileName();

    // One fetch target: the url and the data-dir file its body is
    // committed under.  A bare QUrl converts to the Tranco file.
    struct Source {
        QUrl url;
        QString fileName;
        Source() : fileName(trancoFileName()) {}
        Source(const QUrl &u) : url(u), fileName(trancoFileName()) {}
        Source(const QUrl &u, const QString &f) : url(u), fileName(f) {}
    };

    // <data dir>/tranco-list.dat — rustcore's primary override file.
    static QString listFilePath();
    // <data dir>/majestic-million.csv — the second-source file.
    static QString majesticFilePath();
    // The upstream list downloads.
    static QList<Source> defaultSources();

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
    void setSourcesForTest(const QList<Source> &sources);
    // The oldest stamp across the rank lists — invalid ("never")
    // until every list has fetched once.
    QDateTime lastUpdate() const;

signals:
    void updateFinished(bool ok);

private slots:
    void replyFinished();

private:
    explicit TrancoList(QObject *parent = nullptr);

    void fetch(const QUrl &url, const QString &fileName);
    void maybeCommit();

    QList<Source> m_sources;
    QList<QPointer<QNetworkReply> > m_replies;
    QHash<QNetworkReply *, QString> m_replyFile;
    // fileName -> concatenated bodies for that drop point.
    QHash<QString, QByteArray> m_bodies;
};

#endif // TRANCOLIST_H
