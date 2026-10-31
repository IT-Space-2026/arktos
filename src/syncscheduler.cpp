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
#include "syncscheduler.h"

#include "browserapplication.h"
#include "profilestore.h"
#include "rustcorebridge.h"

#include <qmetaobject.h>
#include <qsettings.h>
#include <qthread.h>

namespace {

// Runs the blocking mw round off the GUI thread — identical in shape
// to SyncPairWorker.
class SyncRoundWorker : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
public slots:
    void call(const QJsonObject &payload)
    {
#ifdef ARKTOS_RUSTCORE
        RustCoreBridge *b = RustCoreBridge::instance();
        QJsonObject err;
        QJsonValue v = b
            ? b->mwCommand(AcMw::Sync, AcMw::SyncNow, payload, &err)
            : QJsonValue();
        emit result(v.isObject() ? v.toObject() : err);
#else
        emit result(QJsonObject{
            {QStringLiteral("status"), QStringLiteral("error")},
            {QStringLiteral("message"), QStringLiteral(
                "Sync is unavailable — the Rust core is not loaded.")}});
#endif
    }
signals:
    void result(const QJsonObject &doc);
};

} // namespace

SyncScheduler::SyncScheduler(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
{
    m_intervalMinutes = qBound(
        0,
        QSettings().value(QStringLiteral("sync/intervalMin"), 15).toInt(),
        24 * 60);
    connect(m_timer, &QTimer::timeout, this, [this] { requestRound(); });
    if (m_intervalMinutes > 0)
        m_timer->start(m_intervalMinutes * 60 * 1000);

    m_thread = new QThread(this);
    auto *worker = new SyncRoundWorker;
    worker->moveToThread(m_thread);
    connect(m_thread, &QThread::finished, worker, &QObject::deleteLater);
    connect(worker, &SyncRoundWorker::result,
            this, &SyncScheduler::onWorkerResult);
    m_thread->start();
    m_worker = worker;
}

SyncScheduler *SyncScheduler::instance()
{
    static SyncScheduler *s_instance = new SyncScheduler;
    return s_instance;
}

void SyncScheduler::setIntervalMinutes(int minutes)
{
    m_intervalMinutes = minutes;
    QSettings().setValue(QStringLiteral("sync/intervalMin"), minutes);
    m_timer->stop();
    if (minutes > 0)
        m_timer->start(minutes * 60 * 1000);
}

void SyncScheduler::syncNow()
{
    requestRound();
}

void SyncScheduler::requestRound()
{
    if (m_inFlight || !m_worker)
        return;
    m_inFlight = true;
    emit roundStarted();
    const QJsonObject payload{
        {QStringLiteral("profile"), ProfileStore::activeProfileId()}};
    QMetaObject::invokeMethod(
        m_worker, "call", Qt::QueuedConnection,
        Q_ARG(QJsonObject, payload));
}

void SyncScheduler::onWorkerResult(const QJsonObject &doc)
{
    m_inFlight = false;
    emit roundFinished(doc);
}

#include "syncscheduler.moc"
