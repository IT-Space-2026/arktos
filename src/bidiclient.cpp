/*
 * Copyright 2026 Benjamin C Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "bidiclient.h"

#include "browserapplication.h"
#include "browserprofile.h"

#ifdef ARORA_RUSTCORE
#include <rustcore.h>
#endif

#include <qbytearray.h>
#include <qdebug.h>
#include <qthread.h>
#include <qtimer.h>

#include <atomic>
#include <thread>

struct BidiClient::Impl {
#ifdef ARORA_RUSTCORE
    // Written by the connect worker before it posts connectFinished,
    // so the handle is never lost even if that queued call is dropped
    // during destruction.
    std::atomic<RcBidiClient *> handle{nullptr};
#endif
    // Async connect attempts still running — the destructor must wait
    // for them so their completion post never lands on a dead object.
    std::atomic<int> inFlight{0};
};

#ifdef ARORA_RUSTCORE
// Fires on a rustcore reader thread — hop to the GUI thread before
// touching Qt state.  A queued call to a destroyed receiver is dropped
// by QObject's destructor, so no lifetime guard beyond that is needed.
static void bidiCallback(void *userdata, const char *kind, uint64_t id,
                         const char *json)
{
    BidiClient *self = static_cast<BidiClient *>(userdata);
    QMetaObject::invokeMethod(
        self, "deliver", Qt::QueuedConnection,
        Q_ARG(QString, QString::fromUtf8(kind ? kind : "")),
        Q_ARG(quint64, static_cast<quint64>(id)),
        Q_ARG(QByteArray, QByteArray(json ? json : "")));
}
#endif

BidiClient::BidiClient(QObject *parent)
    : QObject(parent)
    , m_impl(new Impl)
    , m_state(Disconnected)
    , m_connectAttempts(0)
{
}

BidiClient::~BidiClient()
{
    // Wait out an in-flight connect worker — its completion post is
    // the only thread touching |this| outside the rust callback, and
    // posting to a destroyed QObject is unsafe.
    while (m_impl->inFlight.load() > 0)
        QThread::msleep(2);
#ifdef ARORA_RUSTCORE
    RcBidiClient *handle = m_impl->handle.load();
    if (handle) {
        // Joins the crate's threads — after this no callback is live.
        m_impl->handle.store(nullptr);
        rc_bidi_free(handle);
    }
#endif
    delete m_impl;
}

BidiClient::State BidiClient::state() const
{
    return m_state;
}

QString BidiClient::unavailableReason() const
{
    return m_reason;
}

bool BidiClient::backendArmed()
{
    return BrowserProfile::bidiDebugPort() != 0
        && !BrowserApplication::isTorMode();
}

void BidiClient::connectToEngine()
{
    if (m_state == Connecting || m_state == Connected)
        return;
#ifdef ARORA_RUSTCORE
    if (!backendArmed()) {
        setState(Unavailable,
                 BrowserApplication::isTorMode()
                     ? tr("Debug channel is disabled in Tor windows.")
                     : tr("Debug channel is disabled "
                          "(devtools/bidiBackend)."));
        return;
    }
    m_connectAttempts = 0;
    setState(Connecting);
    attemptConnect();
#else
    setState(Unavailable, tr("Built without rustcore — no BiDi backend."));
#endif
}

void BidiClient::attemptConnect()
{
#ifdef ARORA_RUSTCORE
    const quint16 port = BrowserProfile::bidiDebugPort();
    const QByteArray origin = BrowserProfile::bidiDebugOrigin().toUtf8();
    m_impl->inFlight.fetch_add(1);
    // rc_bidi_connect blocks in socket I/O — it MUST NOT run on the
    // GUI thread: QtWebEngine pumps Chromium's task queue on that same
    // loop, so a blocking connect would starve the devtools HTTP
    // handler it is trying to reach.  Run it on a worker and post the
    // outcome back through the event queue.
    std::thread([this, port, origin]() {
        RcBidiClient *handle = rc_bidi_connect(
            "127.0.0.1", port, "", origin.constData(),
            &bidiCallback, this);
        QString msg;
        if (!handle) {
            char *err = rc_last_error_message();
            msg = err ? QString::fromUtf8(err) : QString();
            if (err)
                rc_string_free(err);
        } else {
            m_impl->handle.store(handle);
        }
        QMetaObject::invokeMethod(
            this, "connectFinished", Qt::QueuedConnection,
            Q_ARG(quintptr, reinterpret_cast<quintptr>(handle)),
            Q_ARG(QString, msg));
        m_impl->inFlight.fetch_sub(1);
    }).detach();
#endif
}

void BidiClient::connectFinished(quintptr handle, const QString &error)
{
    Q_UNUSED(handle);
#ifdef ARORA_RUSTCORE
    if (m_state != Connecting)
        return;
    if (!m_impl->handle.load()) {
        // The debug endpoint can lag its own listen socket; retry a
        // few times before declaring the channel dead.
        if (++m_connectAttempts < 6) {
            QTimer::singleShot(400, this, [this]() {
                if (m_state == Connecting)
                    attemptConnect();
            });
            return;
        }
        setState(Unavailable,
                 tr("Could not connect to the debug channel: %1")
                     .arg(error));
        return;
    }
    setState(Connected);
    // Open the BiDi session and subscribe to the groups the panel
    // shows — subsequent targets auto-attach through the crate.
    sendCommand(QLatin1String("session.new"));
    sendCommand(QLatin1String("session.subscribe"),
                QByteArray("{\"events\":[\"browsingContext\",\"log\",\"network\"]}"));
#else
    Q_UNUSED(error);
#endif
}

quint64 BidiClient::sendCommand(const QString &method,
                                const QByteArray &paramsJson)
{
#ifdef ARORA_RUSTCORE
    RcBidiClient *handle = m_impl->handle.load();
    if (!handle)
        return 0;
    return rc_bidi_command(handle, method.toUtf8().constData(),
                           paramsJson.isEmpty() ? "{}" : paramsJson.constData());
#else
    Q_UNUSED(method);
    Q_UNUSED(paramsJson);
    return 0;
#endif
}

void BidiClient::deliver(const QString &kind, quint64 id,
                         const QByteArray &json)
{
    if (kind == QLatin1String("response")) {
        emit responseReceived(id, json);
    } else if (kind == QLatin1String("event")) {
        // payload is {"method": ..., "params": ...}
        emit eventReceived(json);
    } else if (kind == QLatin1String("state")) {
        if (json.contains("\"closed\""))
            setState(Disconnected);
    }
}

void BidiClient::setState(State state, const QString &reason)
{
    m_state = state;
    m_reason = reason;
    emit connectionChanged(state);
}
