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
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qurl.h>

class QNetworkReply;

// TRUST04: fetches the remote Tranco top-site list and drops it where
// the rustcore tranco module picks it up —
// <data dir>/tranco-list.dat.  The runtime table is the vendored
// top-100k seed unioned with this file, so a failed or poisoned fetch
// can only ever shrink back to the seed, never leave the feature
// empty.
//
// The fetch rides the adblock-list rules: the app-side
// NetworkAccessManager performs it, HTTPS sources only, and the
// automatic refresh is consent-gated exactly like remote filter
// lists (TELEM01 — AdBlockManager::remoteListsAllowed()).  The
// download is the upstream top-1m.csv.zip body verbatim — the crate
// unzips it on load.  Ranking itself never leaves the machine; this
// is a list download, not a per-host lookup.
//
// Without CONFIG+=rustcore the whole class is inert: there is no
// consumer for the file, so nothing is fetched.
class TrancoList : public QObject
{
    Q_OBJECT

public:
    static TrancoList *instance();

    // <data dir>/tranco-list.dat — rustcore's override file.
    static QString listFilePath();
    // The upstream list download.
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
    explicit TrancoList(QObject *parent = nullptr);

    void fetch(const QUrl &url);
    void maybeCommit();

    QList<QUrl> m_sources;
    QList<QPointer<QNetworkReply> > m_replies;
    QByteArray m_merged;
};

#endif // TRANCOLIST_H
