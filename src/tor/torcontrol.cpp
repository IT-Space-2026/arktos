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

#include "torcontrol.h"

#include <qfile.h>
#include <qtcpsocket.h>

TorControl::TorControl(QObject *parent)
    : QObject(parent)
    , m_socket(new QTcpSocket(this))
    , m_current(nullptr)
    , m_waitingForReply(false)
    , m_inDataBlock(false)
{
    connect(m_socket, &QTcpSocket::connected,
            this, [this]() {
        sendNext();
        emit connected();
    });
    connect(m_socket, &QTcpSocket::readyRead,
            this, &TorControl::onReadyRead);
    connect(m_socket, &QTcpSocket::errorOccurred,
            this, [this](QAbstractSocket::SocketError) {
        emit errorMessage(m_socket->errorString());
    });
    connect(m_socket, &QTcpSocket::disconnected,
            this, [this]() {
        // Fail every outstanding command — callers must not hang
        // waiting on a dead daemon.
        while (!m_queue.empty()) {
            Pending pending = std::move(m_queue.front());
            m_queue.pop_front();
            if (pending.callback)
                pending.callback(-1, QStringList());
        }
        if (m_waitingForReply) {
            m_waitingForReply = false;
            if (m_current->callback)
                m_current->callback(-1, m_replyLines);
            m_current = nullptr;
        }
        emit disconnected();
    });
}

void TorControl::connectToHost(const QHostAddress &address, quint16 port)
{
    m_socket->connectToHost(address, port);
}

void TorControl::disconnectFromHost()
{
    m_socket->disconnectFromHost();
}

bool TorControl::isConnected() const
{
    return m_socket->state() == QAbstractSocket::ConnectedState;
}

void TorControl::sendCommand(const QByteArray &command,
                             const ReplyCallback &callback)
{
    m_queue.push_back({command, callback});
    sendNext();
}

void TorControl::authenticateCookie(const QByteArray &cookie,
                                    const ReplyCallback &callback)
{
    // The cookie is 32 raw bytes; the protocol wants it hex-encoded as
    // a single unquoted token ("AUTHENTICATE <hex>").
    if (cookie.size() != 32) {
        if (callback)
            callback(-1, QStringList()
                     << QLatin1String("bad control_auth_cookie length"));
        return;
    }
    sendCommand("AUTHENTICATE " + cookie.toHex(), callback);
}

void TorControl::authenticateCookieFile(const QString &path,
                                        const ReplyCallback &callback)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (callback)
            callback(-1, QStringList()
                     << QLatin1String("cannot read ") + path);
        return;
    }
    authenticateCookie(file.readAll(), callback);
}

void TorControl::takeOwnership(const ReplyCallback &callback)
{
    sendCommand(QByteArrayLiteral("TAKEOWNERSHIP"), callback);
}

void TorControl::setEvents(const QStringList &events,
                           const ReplyCallback &callback)
{
    sendCommand("SETEVENTS " + events.join(QLatin1Char(' ')).toLatin1(),
                callback);
}

void TorControl::getInfo(const QString &key,
                         const ReplyCallback &callback)
{
    sendCommand("GETINFO " + key.toLatin1(), callback);
}

void TorControl::signalShutdown(const ReplyCallback &callback)
{
    sendCommand(QByteArrayLiteral("SIGNAL SHUTDOWN"), callback);
}

void TorControl::sendNext()
{
    if (m_waitingForReply || m_queue.empty() || !isConnected())
        return;
    m_currentStorage = std::move(m_queue.front());
    m_queue.pop_front();
    m_current = &m_currentStorage;
    m_waitingForReply = true;
    m_inDataBlock = false;
    m_replyLines.clear();
    m_socket->write(m_current->command + "\r\n");
}

bool TorControl::takeLine(QByteArray *line)
{
    const int end = m_buffer.indexOf("\r\n");
    if (end < 0)
        return false;
    *line = m_buffer.left(end);
    m_buffer.remove(0, end + 2);
    return true;
}

void TorControl::onReadyRead()
{
    m_buffer += m_socket->readAll();
    QByteArray rawLine;
    while (takeLine(&rawLine)) {
        const QString line = QString::fromUtf8(rawLine);

        if (m_inDataBlock) {
            // Data block content ends with a lone "."; per spec the
            // first line of the block was the "250+key=" line itself.
            if (line == QLatin1String("."))
                m_inDataBlock = false;
            else
                m_replyLines << line;
            continue;
        }

        if (rawLine.size() >= 4 && rawLine.left(3) == "650"
            && rawLine.at(3) == ' ') {
            emit asyncEvent(line.mid(4));
            continue;
        }

        if (rawLine.size() < 4 || (rawLine.at(0) < 48 || rawLine.at(0) > 57)) {
            emit errorMessage(QLatin1String("unparsable control line: ")
                             + line);
            continue;
        }
        const int code = rawLine.left(3).toInt();
        const char sep = rawLine.at(3);
        const QString text = line.mid(4);
        if (sep == '-') {
            m_replyLines << text;
        } else if (sep == '+') {
            m_replyLines << text;
            m_inDataBlock = true;
        } else if (sep == ' ') {
            m_replyLines << text;
            if (m_waitingForReply && m_current) {
                Pending pending = std::move(*m_current);
                m_waitingForReply = false;
                m_inDataBlock = false;
                const QStringList lines = m_replyLines;
                m_replyLines.clear();
                m_current = nullptr;
                if (pending.callback)
                    pending.callback(code, lines);
            }
            sendNext();
        } else {
            emit errorMessage(QLatin1String("unparsable control line: ")
                             + line);
        }
    }
}
