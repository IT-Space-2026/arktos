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

#include "torsocks5.h"

#include <qtcpsocket.h>
#include <qtimer.h>

TorSocks5::TorSocks5(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_timer(new QTimer(this))
    , m_phase(ProxyConnect)
    , m_destPort(0)
    , m_done(false)
{
    m_timer->setSingleShot(true);
    connect(m_timer, &QTimer::timeout, this, [this]() {
        fail(-1);
    });
    connect(m_socket, &QTcpSocket::connected,
            this, &TorSocks5::onConnected);
    connect(m_socket, &QTcpSocket::readyRead,
            this, &TorSocks5::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, [this](QAbstractSocket::SocketError error) {
        // A refusing peer legitimately sends its reply and closes —
        // tor does exactly that for targets exits cannot reach.  Only
        // the remote-close error can still carry a readable refusal;
        // drain whatever is buffered and let a disconnected handler
        // take the final decision.
        if (error != QAbstractSocket::RemoteHostClosedError)
            fail(-1);
    });
    connect(m_socket, &QTcpSocket::disconnected,
            this, [this]() {
        if (m_socket->bytesAvailable() > 0)
            onReadyRead();
        if (!m_done)
            fail(-1);
    });
}

QTcpSocket *TorSocks5::socket() const
{
    return m_socket;
}

void TorSocks5::connectThrough(const QHostAddress &proxyAddress,
                               quint16 proxyPort,
                               const QString &destinationHost,
                               quint16 destinationPort)
{
    m_destHost = destinationHost;
    m_destPort = destinationPort;
    m_phase = ProxyConnect;
    m_timer->start(15000);
    m_socket->connectToHost(proxyAddress, proxyPort);
}

void TorSocks5::fail(int code)
{
    if (m_done)
        return;
    m_done = true;
    m_timer->stop();
    m_socket->abort();
    emit finished(false, code);
}

void TorSocks5::onConnected()
{
    // Greeting: VER=5, NMETHODS=1, METHODS=[0x00 no-auth]
    m_socket->write(QByteArrayLiteral("\x05\x01\x00"));
    m_phase = Greeting;
}

void TorSocks5::sendConnectRequest()
{
    // CONNECT: VER=5 CMD=1 RSV=0 ATYP addr port — IPv4/IPv6 literal or
    // domain name.  (Built byte-by-byte: QByteArray("\x05\x01\x00")
    // would stop at the first NUL.)
    QByteArray request;
    request.append(char(0x05)).append(char(0x01)).append(char(0x00));
    const QHostAddress literal(m_destHost);
    if (literal.protocol() == QAbstractSocket::IPv4Protocol) {
        request.append('\x01');
        const quint32 ipv4 = literal.toIPv4Address();
        request.append(char(ipv4 >> 24))
            .append(char(ipv4 >> 16))
            .append(char(ipv4 >> 8))
            .append(char(ipv4));
    } else if (literal.protocol() == QAbstractSocket::IPv6Protocol) {
        request.append('\x04');
        request.append(reinterpret_cast<const char *>(
                           literal.toIPv6Address().c), 16);
    } else {
        const QByteArray encoded = m_destHost.toLatin1();
        request.append('\x03');
        request.append(char(encoded.size()));
        request.append(encoded);
    }
    request.append(char(m_destPort >> 8)).append(char(m_destPort));
    m_socket->write(request);
    m_phase = ConnectReply;
}

void TorSocks5::onReadyRead()
{
    m_buffer += m_socket->readAll();
    if (m_phase == Greeting) {
        if (m_buffer.size() < 2)
            return;
        if (uchar(m_buffer.at(0)) != 0x05 || uchar(m_buffer.at(1)) != 0x00) {
            fail(-1);
            return;
        }
        m_buffer.remove(0, 2);
        sendConnectRequest();
    }

    if (m_phase == ConnectReply) {
        // VER REP RSV ATYP | BND.ADDR | BND.PORT — header tells us the
        // address form so we know the full reply length.
        if (m_buffer.size() < 4)
            return;
        if (uchar(m_buffer.at(0)) != 0x05 || uchar(m_buffer.at(2)) != 0x00) {
            fail(-1);
            return;
        }
        int total;
        switch (uchar(m_buffer.at(3))) {
        case 0x01: total = 4 + 4 + 2; break;   // IPv4
        case 0x04: total = 4 + 16 + 2; break;  // IPv6
        case 0x03:
            if (m_buffer.size() < 5)
                return;
            total = 4 + 1 + uchar(m_buffer.at(4)) + 2;
            break;
        default:
            fail(-1);
            return;
        }
        if (m_buffer.size() < total)
            return;
        const int rep = uchar(m_buffer.at(1));
        if (m_done)
            return;
        m_done = true;
        m_timer->stop();
        emit finished(rep == 0x00, rep);
    }
}
