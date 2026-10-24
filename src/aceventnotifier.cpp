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

#include "aceventnotifier.h"

#include <qdebug.h>

#include <arktos_core.h>

AcEventNotifier::AcEventNotifier(AcCore *core,
                                 const QByteArray &topicFilter,
                                 QObject *parent)
    : QObject(parent)
    , m_guard(new Guard)
    , m_core(core)
    , m_sub(0)
{
    m_guard->self = this;
    if (core == nullptr)
        return;
    m_sub = ac_events_subscribe(
        core, topicFilter.isEmpty() ? nullptr : topicFilter.constData(),
        &AcEventNotifier::trampoline, m_guard);
    if (m_sub == 0) {
        char *err = ac_last_error();
        qWarning() << "AcEventNotifier: ac_events_subscribe failed:"
                   << QString::fromUtf8(err ? err : "unknown");
        ac_string_free(err);
    }
}

AcEventNotifier::~AcEventNotifier()
{
    // Order matters: flip dead under the guard mutex FIRST so a
    // trampoline already dispatched on a Rust thread either finished
    // its queued invoke before we get here or drops the event after.
    // Unsubscribing inside the same locked section means no new
    // dispatch can start between the flag and the registry removal.
    {
        QMutexLocker lock(&m_guard->mutex);
        m_guard->dead = true;
        if (m_sub != 0 && m_core != nullptr)
            ac_events_unsubscribe(m_core, m_sub);
        m_sub = 0;
    }
    // Deliberately leaked — see the Guard note in the header.
    m_guard->self = nullptr;
}

bool AcEventNotifier::isValid() const
{
    return m_sub != 0;
}

quint64 AcEventNotifier::subscriptionId() const
{
    return m_sub;
}

void AcEventNotifier::trampoline(const char *topic,
                                 const char *payloadJson,
                                 void *userdata)
{
    Guard *guard = static_cast<Guard *>(userdata);
    const QString t = QString::fromUtf8(topic ? topic : "");
    const QByteArray p = QByteArray(payloadJson ? payloadJson : "");
    QMutexLocker lock(&guard->mutex);
    if (guard->dead || guard->self == nullptr)
        return;
    AcEventNotifier *self = guard->self;
    // Rust may call from any thread; hop to the object's thread.
    // Delivery past a queued-but-undelivered event to a destroyed
    // receiver is handled by Qt — the dead flag covers the invoke
    // itself.
    QMetaObject::invokeMethod(self, [self, t, p]() {
        emit self->storeChanged(t, p);
    }, Qt::QueuedConnection);
}
