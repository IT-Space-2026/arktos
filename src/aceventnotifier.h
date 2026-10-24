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

#ifndef ACEVENTNOTIFIER_H
#define ACEVENTNOTIFIER_H

#include <qobject.h>
#include <qbytearray.h>
#include <qmutex.h>
#include <qstring.h>

struct AcCore;

// MB03: the Qt half of the rustcore namespaced event lane — the
// opt-in, typed sibling of RustCoreBridge's storeChanged() (MW-ARCH
// §5).  Constructed against an AcCore with a topic filter, it
// registers an ac_events_subscribe callback and re-emits each
// delivery as a queued storeChanged() on its own thread — a Rust
// worker thread never touches the GUI thread directly.
//
// Filter grammar (arktos_core.h): empty or "*" subscribes to every
// event, a trailing slash ("history/") takes the whole namespace,
// anything else is an exact topic ("history/changed").
//
// The payload is the v1 JSON document verbatim —
// {"v":1,"topic":<t>,"profile":<id-or-null>, ...domain fields}; the
// namespaced topic registry (declared contract plus per-topic field
// lists) comes from ac_events_topics().
//
// Compiled only under CONFIG+=rustcore (ARKTOS_RUSTCORE).
class AcEventNotifier : public QObject
{
    Q_OBJECT

public:
    // Empty filter subscribes to every namespaced event.  A NULL or
    // already-shutdown core leaves the notifier invalid — isValid()
    // reports whether the subscription took.
    explicit AcEventNotifier(AcCore *core,
                             const QByteArray &topicFilter = QByteArray(),
                             QObject *parent = nullptr);
    ~AcEventNotifier() override;

    bool isValid() const;
    // The id ac_events_subscribe returned — 0 when invalid.
    quint64 subscriptionId() const;

signals:
    void storeChanged(const QString &topic, const QByteArray &payload);

private:
    static void trampoline(const char *topic, const char *payloadJson,
                           void *userdata);
    // The trampoline's userdata — a heap block the destructor leaves
    // allocated on purpose: an event dispatched on a Rust thread can
    // be inside the callback while ~AcEventNotifier runs, and the
    // dead flag is the only ordering primitive that cannot itself be
    // freed under the reader.  Leaking a few bytes once per notifier
    // is cheaper than a lock the Rust side cannot wait on.
    struct Guard {
        QMutex mutex;
        bool dead = false;
        AcEventNotifier *self = nullptr;
    };
    Guard *m_guard;
    AcCore *m_core;
    quint64 m_sub;
};

#endif // ACEVENTNOTIFIER_H
