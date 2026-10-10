/*
 * Copyright 2026 Benjamin C Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef BIDICLIENT_H
#define BIDICLIENT_H

/*
 * DEVT03 — Qt-side wrapper for the rustcore BiDi devtools channel.
 *
 * The panel speaks WebDriver-BiDi-shaped commands (see
 * rustcore/src/bidi.rs for the surface); the crate's WebEngine backend
 * translates them onto CDP over the loopback remote-debugging socket
 * armed by BrowserProfile::applyChromiumFlags().  A future Servo
 * backend answers the same surface natively — nothing here is
 * CDP-shaped.
 *
 * Without CONFIG+=rustcore every connect attempt reports unavailable.
 * Callbacks from the crate arrive on internal threads and are
 * re-queued onto this object's thread before being re-emitted.
 */

#include <qobject.h>
#include <qstring.h>

class BidiClient : public QObject
{
    Q_OBJECT

public:
    enum State {
        Disconnected = 0,
        Connecting,
        Connected,
        Unavailable
    };
    Q_ENUM(State)

    explicit BidiClient(QObject *parent = nullptr);
    ~BidiClient() override;

    State state() const;
    QString unavailableReason() const;

    // Attempts the connection to this process's debug endpoint.
    // Safe to re-call after Disconnected/Unavailable.
    void connectToEngine();

    // Queues a BiDi-shaped command; returns the correlation id (0 on
    // failure).  The answer arrives as responseReceived(id, json).
    quint64 sendCommand(const QString &method,
                        const QByteArray &paramsJson = "{}");

    // True when a debug endpoint could exist at all (port armed,
    // not a tor process).
    static bool backendArmed();

signals:
    void responseReceived(quint64 id, const QByteArray &json);
    void eventReceived(const QByteArray &json);
    void connectionChanged(int state);

private:
    Q_INVOKABLE void deliver(const QString &kind, quint64 id,
                             const QByteArray &json);
    Q_INVOKABLE void connectFinished(quintptr handle,
                                     const QString &error);
    void setState(State state, const QString &reason = QString());
    void attemptConnect();

    struct Impl;
    Impl *m_impl;
    State m_state;
    QString m_reason;
    int m_connectAttempts;
};

#endif // BIDICLIENT_H
