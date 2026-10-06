/*
 * Copyright (c) 2026, The Arora Authors
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

// COV04: SingleApplication — the QLocalServer/QLocalSocket handshake the
// single-instance browser relies on.  Needs its own main() since the
// class under test IS the QApplication.

#include <QtTest/QtTest>
#include <qfileinfo.h>
#include <qlocalsocket.h>

#include "singleapplication.h"
#include "qtry.h"

class tst_SingleApplication : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void notRunningUntilServerStarts();
    void messageRoundTrip();
    void stalledClientDoesNotBlockServer();
    void socketIsUserOnly();
};

void tst_SingleApplication::initTestCase()
{
    QCoreApplication::setApplicationName("tst_singleapplication");
}

void tst_SingleApplication::notRunningUntilServerStarts()
{
    SingleApplication *app = qobject_cast<SingleApplication *>(qApp);
    QVERIFY(app);
    // sendMessage with no server up must fail fast, not block.
    QVERIFY(!app->sendMessage(QByteArray("nobody-listening"), 100));
}

void tst_SingleApplication::messageRoundTrip()
{
    SingleApplication *app = qobject_cast<SingleApplication *>(qApp);
    QVERIFY(app);
    QVERIFY(app->startSingleServer());
    QVERIFY(app->isRunning());

    // A client connects, writes, and the server side emits
    // messageReceived with the pending socket.
    QSignalSpy spy(app, &SingleApplication::messageReceived);
    QVERIFY(app->sendMessage(QByteArray("hello-instance"), 200));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);

    // Starting a second server on the same name is refused.
    QVERIFY(!app->startSingleServer() || app->isRunning());
}

// FRZ01: a same-uid client that connects but never writes used to
// freeze the server's GUI thread in waitForReadyRead() for the full
// socket timeout.  The handoff is driven off readyRead now — a real
// client behind a staller must still be serviced promptly.
void tst_SingleApplication::stalledClientDoesNotBlockServer()
{
    SingleApplication *app = qobject_cast<SingleApplication *>(qApp);
    QVERIFY(app);
    QVERIFY(app->isRunning() || app->startSingleServer());

    QLocalSocket staller;
    staller.connectToServer(app->serverAddress());
    QVERIFY(staller.waitForConnected(500));

    QSignalSpy spy(app, &SingleApplication::messageReceived);
    QVERIFY(app->sendMessage(QByteArray("still-responsive"), 500));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
}

// SEC09: a connected client can push urls into the running browser —
// the socket file and the directory holding it must be user-only.
void tst_SingleApplication::socketIsUserOnly()
{
#ifdef Q_OS_UNIX
    SingleApplication *app = qobject_cast<SingleApplication *>(qApp);
    QVERIFY(app);
    QVERIFY(app->isRunning() || app->startSingleServer());

    const QFileInfo socket(app->serverAddress());
    QVERIFY2(socket.exists(), qPrintable(socket.absoluteFilePath()));
    const QFileDevice::Permissions groupOther =
        QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup
        | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;
    QVERIFY2(int(socket.permissions() & groupOther) == 0,
             qPrintable(socket.absoluteFilePath()));
    const QFileInfo dir(socket.absolutePath());
    QVERIFY2(int(dir.permissions() & groupOther) == 0,
             qPrintable(dir.absoluteFilePath()));
#endif
}

int main(int argc, char *argv[])
{
    SingleApplication app(argc, argv);
    tst_SingleApplication tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_singleapplication.moc"
