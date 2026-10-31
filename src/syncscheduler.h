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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */
#ifndef SYNCSCHEDULER_H
#define SYNCSCHEDULER_H

#include <qjsonobject.h>
#include <qobject.h>
#include <qtimer.h>

class QThread;

// SYNC11: drives the periodic sync round (the mw sync_now op) at the
// user-chosen interval plus the manual "Sync now" button.  The op can
// block for tens of seconds (Arti bootstrap on the tor leg of the
// auto transport), so every round runs on a dedicated worker thread —
// the same discipline SyncPairDialog uses for its mw calls.  Rounds
// never overlap: a timer tick during an in-flight round is skipped,
// and the round itself stops the transport it started, so the idle
// browser keeps no Arti/mDNS footprint between intervals.
class SyncScheduler : public QObject
{
    Q_OBJECT

public:
    static SyncScheduler *instance();

    int intervalMinutes() const { return m_intervalMinutes; }
    void setIntervalMinutes(int minutes);

    bool roundInFlight() const { return m_inFlight; }
    void syncNow();

signals:
    void roundStarted();
    void roundFinished(const QJsonObject &result);

private slots:
    void onWorkerResult(const QJsonObject &doc);

private:
    explicit SyncScheduler(QObject *parent = nullptr);

    void requestRound();

    QTimer *m_timer;
    QThread *m_thread = nullptr;
    QObject *m_worker = nullptr;
    int m_intervalMinutes = 15;
    bool m_inFlight = false;
};

#endif // SYNCSCHEDULER_H
