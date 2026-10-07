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

#ifndef TORSOCKS5_H
#define TORSOCKS5_H

#include <qhostaddress.h>
#include <qobject.h>
#include <qstring.h>

class QTcpSocket;
class QTimer;

// TOR01: minimal asynchronous SOCKS5 CONNECT client (RFC 1928),
// used by the --tor-smoke harness and the autotest to prove the
// managed daemon's socks listener actually speaks the protocol.
// Method negotiation is fixed to "no authentication" (0x00) — the
// daemon only listens on loopback, which is exactly what RFC 1929
// user/pass would pretend to protect.
class TorSocks5 : public QObject
{
    Q_OBJECT

public:
    explicit TorSocks5(QObject *parent = nullptr);

    // Drives a fresh QTcpSocket: connect to proxyAddress:proxyPort,
    // negotiate, then CONNECT destinationHost:destinationPort.
    // finished(true, 0) when the proxy granted the tunnel;
    // finished(false, rep) when the proxy answered with a well-formed
    // refusal (rep 0x01-0x08 — still proves SOCKS5 works; exits cannot
    // reach 127.0.0.1 targets, for example);
    // finished(false, -1) on transport errors or a malformed reply.
    void connectThrough(const QHostAddress &proxyAddress, quint16 proxyPort,
                        const QString &destinationHost,
                        quint16 destinationPort);

    QTcpSocket *socket() const;

signals:
    void finished(bool granted, int replyCode);

private:
    enum Phase {
        ProxyConnect,
        Greeting,
        ConnectReply
    };

    void fail(int code);
    void onConnected();
    void onReadyRead();
    void sendConnectRequest();

    QTcpSocket *m_socket;
    QTimer *m_timer;
    Phase m_phase;
    QByteArray m_buffer;
    QString m_destHost;
    quint16 m_destPort;
    bool m_done;
};

#endif // TORSOCKS5_H
