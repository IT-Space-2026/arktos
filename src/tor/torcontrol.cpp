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

#include "torcontrol.h"

#include <qfile.h>

#ifdef ARKTOS_RUSTCORE

// RC14: the protocol client lives in the rustcore torctl crate
// (rc_tor_*).  This file keeps the exact public contract — same
// signals, same callback fires exactly once with (code, lines), -1
// when the link dies first — and maps the pull-drain model onto the
// "torctl" change topic the bridge re-emits queued on this thread.

#include "rustcorebridge.h"

#include <qcoreapplication.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <rustcore.h>

namespace {

// Fires a reply callback once with the -1 spelling — the reason text
// comes from the thread-local rustcore error when one is set (the
// socket path put its own reason strings in the lines).
void failCallbackNow(const TorControl::ReplyCallback &callback)
{
    if (!callback)
        return;
    QStringList lines;
    char *err = rc_last_error_message();
    if (err && *err)
        lines << QString::fromUtf8(err);
    rc_string_free(err);
    callback(-1, lines);
}

} // namespace

TorControl::TorControl(QObject *parent)
    : QObject(parent)
    , m_ctl(nullptr)
    , m_disconnectEmitted(false)
{
    // The crate's reader thread announces every queued reply, async
    // event and link-state change through the "torctl" topic; the
    // bridge trampoline hops it onto this thread already queued.
    if (QCoreApplication::instance()) {
        connect(RustCoreBridge::instance(),
                &RustCoreBridge::storeChanged, this,
                [this](const QString &topic) {
            if (topic == QLatin1String("torctl"))
                drainRust();
        });
    }
}

TorControl::~TorControl()
{
    // rc_tor_close flushes the pending commands into -1 replies inside
    // the handle, but the drain that would dispatch them lands after
    // this object is gone — sever and drop silently, the same
    // teardown discipline the socket path documented.
    if (m_ctl) {
        rc_tor_close(m_ctl);
        m_ctl = nullptr;
    }
    m_callbacks.clear();
}

void TorControl::connectToHost(const QHostAddress &address, quint16 port)
{
    if (m_ctl) {
        // Reconnect: sever first so in-flight commands flush into -1
        // replies, drain so their callbacks fire exactly once, then
        // free the handle.
        rc_tor_disconnect(m_ctl);
        drainRust();
        rc_tor_close(m_ctl);
        m_ctl = nullptr;
        m_callbacks.clear();
    }
    m_disconnectEmitted = false;
    const QByteArray host = address.toString().toLatin1();
    // Loopback-only is enforced inside — a non-loopback host is an
    // open failure here, matching the no-leak rule.
    m_ctl = rc_tor_open(host.constData(), port);
    if (!m_ctl) {
        char *err = rc_last_error_message();
        emit errorMessage(QString::fromUtf8(
            err && *err ? err : "connect failed"));
        rc_string_free(err);
        return;
    }
    // rc_tor_open connects synchronously — a returned handle is
    // already live, the same point the socket path emitted
    // connected() at.
    emit connected();
}

void TorControl::disconnectFromHost()
{
    if (!m_ctl)
        return;
    // Pending commands flush as -1 replies, the state lands on
    // RC_TOR_CLOSED; the drain then fires those callbacks and
    // disconnected() — exactly the socket path's ordering.
    rc_tor_disconnect(m_ctl);
    drainRust();
}

bool TorControl::isConnected() const
{
    return m_ctl && rc_tor_state(m_ctl) == RC_TOR_CONNECTED;
}

void TorControl::sendSeq(qint64 seq, const ReplyCallback &callback)
{
    if (seq < 0) {
        failCallbackNow(callback);
        return;
    }
    if (callback)
        m_callbacks.insert(seq, callback);
}

void TorControl::sendCommand(const QByteArray &command,
                             const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    sendSeq(rc_tor_send(m_ctl, command.constData()), callback);
}

void TorControl::authenticateCookie(const QByteArray &cookie,
                                    const ReplyCallback &callback)
{
    // The cookie is 32 raw bytes; the protocol wants it hex-encoded
    // as a single unquoted token — the length check mirrors the
    // socket path and never reaches the wire on failure.
    if (cookie.size() != 32) {
        if (callback)
            callback(-1, QStringList()
                     << QLatin1String("bad control_auth_cookie length"));
        return;
    }
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    sendSeq(rc_tor_auth_cookie(
                m_ctl,
                reinterpret_cast<const uint8_t *>(cookie.constData()),
                size_t(cookie.size())),
            callback);
}

void TorControl::authenticateCookieFile(const QString &path,
                                        const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    // The read happens inside the crate — the cookie never crosses
    // the ABI as a buffer.
    const QByteArray p = path.toUtf8();
    sendSeq(rc_tor_auth_cookie_file(m_ctl, p.constData()), callback);
}

void TorControl::takeOwnership(const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    sendSeq(rc_tor_takeownership(m_ctl), callback);
}

void TorControl::setEvents(const QStringList &events,
                           const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    const QByteArray list = events.join(QLatin1Char(' ')).toLatin1();
    sendSeq(rc_tor_setevents(m_ctl, list.constData()), callback);
}

void TorControl::getInfo(const QString &key,
                         const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    const QByteArray k = key.toLatin1();
    sendSeq(rc_tor_getinfo(m_ctl, k.constData()), callback);
}

void TorControl::signalShutdown(const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    sendSeq(rc_tor_signal(m_ctl, "SHUTDOWN"), callback);
}

void TorControl::signalNewNym(const ReplyCallback &callback)
{
    if (!m_ctl || rc_tor_state(m_ctl) != RC_TOR_CONNECTED) {
        failCallbackNow(callback);
        return;
    }
    sendSeq(rc_tor_signal(m_ctl, "NEWNYM"), callback);
}

// Pulls everything the reader thread queued: completed replies
// dispatch to their submitter's callback in order, async 650 lines
// re-emit as asyncEvent, protocol complaints as errorMessage, and a
// Closed state reports the connection error + disconnected once.
void TorControl::drainRust()
{
    if (!m_ctl)
        return;
    for (;;) {
        RcBuffer buf{nullptr, 0};
        if (rc_tor_take_reply(m_ctl, &buf) != RC_OK)
            break;
        const QByteArray raw(
            reinterpret_cast<const char *>(buf.data),
            qsizetype(buf.len));
        rc_buffer_free(buf);
        const QJsonObject obj = QJsonDocument::fromJson(raw).object();
        const qint64 seq = qint64(obj.value(QLatin1String("seq"))
                                      .toDouble());
        const int code = obj.value(QLatin1String("code")).toInt(-1);
        QStringList lines;
        for (const QJsonValue &v :
             obj.value(QLatin1String("lines")).toArray())
            lines << v.toString();
        const ReplyCallback callback = m_callbacks.take(seq);
        if (callback)
            callback(code, lines);
    }
    for (;;) {
        char *line = rc_tor_take_event(m_ctl);
        if (!line)
            break;
        emit asyncEvent(QString::fromUtf8(line));
        rc_string_free(line);
    }
    for (;;) {
        char *line = rc_tor_take_error(m_ctl);
        if (!line)
            break;
        emit errorMessage(QString::fromUtf8(line));
        rc_string_free(line);
    }
    if (rc_tor_state(m_ctl) == RC_TOR_CLOSED && !m_disconnectEmitted) {
        m_disconnectEmitted = true;
        char *err = rc_tor_error(m_ctl);
        const QString msg = QString::fromUtf8(err ? err : "");
        rc_string_free(err);
        if (!msg.isEmpty())
            emit errorMessage(msg);
        emit disconnected();
    }
}

#else

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

TorControl::~TorControl()
{
    // Dropping the socket (child, destroyed after this body runs) can
    // emit disconnected() — which would fire every outstanding reply
    // callback into an owner that is itself mid-destruction.  Sever
    // the signal link and drain the queue silently instead.
    if (m_socket)
        m_socket->disconnect(this);
    m_queue.clear();
    m_current = nullptr;
    m_waitingForReply = false;
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

void TorControl::signalNewNym(const ReplyCallback &callback)
{
    sendCommand(QByteArrayLiteral("SIGNAL NEWNYM"), callback);
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

#endif // ARKTOS_RUSTCORE
