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

#ifndef TORCONTROL_H
#define TORCONTROL_H

#include <qbytearray.h>
#include <qhostaddress.h>
#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>

#include <functional>
#include <deque>

class QTcpSocket;

// Minimal client for the Tor control protocol (control-spec.txt,
// tor.git).  Line protocol over a plain loopback TCP socket:
// commands go out CRLF-terminated, replies come back as
//   <code>-<text>   mid-reply line
//   <code>+<text>   data block follows, terminated by a line "."
//   <code> <text>   final reply line
// and 650 lines are asynchronous events that can interleave with a
// multi-line reply in progress.
//
// Every command is queued and its callback fired exactly once with the
// final status code (250 on success, 4xx/5xx on error) and the reply
// payload lines (code prefix stripped; data-block lines appended raw).
class TorControl : public QObject
{
    Q_OBJECT

public:
    typedef std::function<void(int code, const QStringList &lines)>
        ReplyCallback;

    explicit TorControl(QObject *parent = nullptr);

    void connectToHost(const QHostAddress &address, quint16 port);
    void disconnectFromHost();
    bool isConnected() const;

    // Queues a command; callback gets the final code and reply lines.
    void sendCommand(const QByteArray &command,
                     const ReplyCallback &callback = ReplyCallback());

    // Cookie auth: the 32-byte cookie from
    // <DataDirectory>/control_auth_cookie, sent as one hex token.
    void authenticateCookie(const QByteArray &cookie,
                            const ReplyCallback &callback = ReplyCallback());
    void authenticateCookieFile(const QString &path,
                                const ReplyCallback &callback = ReplyCallback());

    // TAKEOWNERSHIP — tor exits when this connection closes (app-crash
    // safety: the daemon can never be orphaned).
    void takeOwnership(const ReplyCallback &callback = ReplyCallback());
    // e.g. setEvents({"STATUS_CLIENT"}) for bootstrap progress events.
    void setEvents(const QStringList &events,
                   const ReplyCallback &callback = ReplyCallback());
    void getInfo(const QString &key,
                 const ReplyCallback &callback = ReplyCallback());
    // Graceful shutdown — tor catches SIGTERM and exits cleanly.
    void signalShutdown(const ReplyCallback &callback = ReplyCallback());

signals:
    void connected();
    void disconnected();
    void errorMessage(const QString &message);
    // Raw asynchronous event line, code 650 stripped —
    // e.g. "STATUS_CLIENT NOTICE BOOTSTRAP PROGRESS=100 TAG=done ...".
    void asyncEvent(const QString &line);

private:
    void sendNext();
    void onReadyRead();
    bool takeLine(QByteArray *line);

    struct Pending {
        QByteArray command;
        ReplyCallback callback;
    };

    QTcpSocket *m_socket;
    QByteArray m_buffer;
    std::deque<Pending> m_queue;
    Pending *m_current;
    Pending m_currentStorage;
    bool m_waitingForReply;
    // Collecting a "250+key=" data block until the "." terminator.
    bool m_inDataBlock;
    QStringList m_replyLines;
};

#endif // TORCONTROL_H
