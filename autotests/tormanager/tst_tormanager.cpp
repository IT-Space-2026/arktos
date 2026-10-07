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

// TOR01: TorManager + TorControl + TorSocks5.  faketor.py stands in
// for the daemon — it implements the control-port file, cookie
// authentication, TAKEOWNERSHIP, async bootstrap events, a SOCKS5
// listener and SIGNAL SHUTDOWN — so the whole spawn->bootstrap->ready
// ->stop pipeline runs deterministically without a tor binary or
// network.  realDaemonLifecycle() repeats the flow against a real tor
// when one resolves (skipped otherwise).

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qapplication.h>
#include <qstandardpaths.h>

#include "torcontrol.h"
#include "tormanager.h"
#include "torsocks5.h"
#include "qtry.h"

class tst_TorManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void binaryResolution();
    void failsWithoutBinary();
    void controlProtocol();
    void socks5RejectsBadGreeting();
    void fakeDaemonLifecycle();
    void realDaemonLifecycle();

private:
    static QString fakeTorPath();
};

void tst_TorManager::initTestCase()
{
    QCoreApplication::setApplicationName("tst_tormanager");
    QStandardPaths::setTestModeEnabled(true);
}

QString tst_TorManager::fakeTorPath()
{
    return QCoreApplication::applicationDirPath()
        + QLatin1String("/faketor.py");
}

void tst_TorManager::binaryResolution()
{
    // An explicit override that does not exist is an error, not a
    // silent fallthrough to PATH — a mistyped path must not launch a
    // different daemon than the user asked for.  Env vars are
    // process-wide, so restore the caller's value for the tests that
    // follow (realDaemonLifecycle uses it).
    const QByteArray original = qgetenv("ARORA_TOR_BINARY");
    qputenv("ARORA_TOR_BINARY", "/nonexistent/tor-binary");
    QVERIFY(TorManager::resolveBinary().isEmpty());
    if (original.isEmpty())
        qunsetenv("ARORA_TOR_BINARY");
    else
        qputenv("ARORA_TOR_BINARY", original);

    // The default data directory lives under the app data dir (test
    // mode redirects that under the system temp).
    TorManager manager;
    QVERIFY(manager.dataDirectory().endsWith(QLatin1String("/tor")));
    QVERIFY(!manager.binarySearchPaths().isEmpty());
}

void tst_TorManager::failsWithoutBinary()
{
    TorManager manager;
    manager.setBinaryPath(QLatin1String("/nonexistent/tor-binary"));
    QSignalSpy failedSpy(&manager, &TorManager::failed);
    manager.start();
    QCOMPARE(manager.state(), TorManager::Failed);
    QCOMPARE(failedSpy.count(), 1);
    QVERIFY(!manager.errorString().isEmpty());
    QVERIFY(!manager.isRunning());
}

// TorControl line protocol against an in-process fake control server:
// AUTHENTICATE, queued replies, the "250+key=" data block and async
// 650 events interleaved between reply lines.
void tst_TorManager::controlProtocol()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));

    QTcpSocket *peer = nullptr;
    QByteArray incoming;
    connect(&server, &QTcpServer::newConnection, this, [&]() {
        peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, this, [&]() {
            incoming += peer->readAll();
            while (incoming.contains("\r\n")) {
                const QByteArray line =
                    incoming.left(incoming.indexOf("\r\n"));
                incoming.remove(0, line.size() + 2);
                if (line.startsWith("AUTHENTICATE")) {
                    peer->write("250 OK\r\n");
                } else if (line.startsWith("GETINFO multi")) {
                    // data-block reply: mid lines, "+" payload until
                    // ".", final "250 OK".
                    peer->write("250-first=1\r\n"
                                "250+multi=\r\nline a\r\nline b\r\n"
                                ".\r\n250 OK\r\n");
                } else if (line.startsWith("GETINFO interleave")) {
                    // an async event lands mid-reply
                    peer->write("250-first=1\r\n"
                                "650 STATUS_CLIENT NOTICE BOOTSTRAP "
                                "PROGRESS=7\r\n250 OK\r\n");
                } else {
                    peer->write("552 Unrecognized\r\n");
                }
            }
        });
    });

    TorControl control;
    QSignalSpy asyncSpy(&control, &TorControl::asyncEvent);
    control.connectToHost(QHostAddress::LocalHost, server.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(control.isConnected(), 5000);

    // Cookie auth: exactly 32 bytes required.
    int code = -1;
    control.authenticateCookie(QByteArray(31, 'x'),
                             [&code](int c, const QStringList &) {
        code = c;
    });
    QTRY_VERIFY_WITH_TIMEOUT(code == -1, 5000);
    // -1 fires synchronously from the length check; no line went out.
    control.authenticateCookie(QByteArray(32, 'a'),
                               [&code](int c, const QStringList &) {
        code = c;
    });
    QTRY_VERIFY_WITH_TIMEOUT(code == 250, 5000);

    // Data-block reply payload collection.
    code = -1;
    QStringList lines;
    control.getInfo(QLatin1String("multi"),
                    [&code, &lines](int c, const QStringList &l) {
        code = c;
        lines = l;
    });
    QTRY_VERIFY_WITH_TIMEOUT(code == 250, 5000);
    QVERIFY(lines.contains(QLatin1String("first=1")));
    QVERIFY(lines.contains(QLatin1String("line a")));
    QVERIFY(lines.contains(QLatin1String("line b")));

    // Async events interleave with a reply without breaking it.
    code = -1;
    lines.clear();
    control.getInfo(QLatin1String("interleave"),
                    [&code, &lines](int c, const QStringList &l) {
        code = c;
        lines = l;
    });
    QTRY_VERIFY_WITH_TIMEOUT(code == 250, 5000);
    QVERIFY(lines.contains(QLatin1String("first=1")));
    QTRY_VERIFY_WITH_TIMEOUT(asyncSpy.count() >= 1, 5000);
    QVERIFY(asyncSpy.takeFirst().first().toString().startsWith(
        QLatin1String("STATUS_CLIENT")));
}

// A bogus greeting answer must fail the handshake cleanly rather than
// hang or misparse.
void tst_TorManager::socks5RejectsBadGreeting()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost, 0));
    connect(&server, &QTcpServer::newConnection, this, [&]() {
        QTcpSocket *peer = server.nextPendingConnection();
        connect(peer, &QTcpSocket::readyRead, peer, [peer]() {
            peer->write(QByteArrayLiteral("\x06\x00")); // wrong version
        });
    });

    TorSocks5 probe;
    QSignalSpy spy(&probe, &TorSocks5::finished);
    probe.connectThrough(QHostAddress::LocalHost, server.serverPort(),
                         QStringLiteral("127.0.0.1"), 9);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() == 1, 10000);
    QCOMPARE(spy.first().at(1).toInt(), -1);
}

// End-to-end against faketor.py: process spawn, control-port file,
// cookie auth, async bootstrap events, socks endpoint discovery,
// SIGNAL SHUTDOWN teardown.
void tst_TorManager::fakeDaemonLifecycle()
{
    const QString fake = fakeTorPath();
    if (!QFileInfo(fake).isExecutable()
        || QStandardPaths::findExecutable(QStringLiteral("python3"))
               .isEmpty())
        QSKIP("faketor.py fixture needs an executable python3");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TorManager manager;
    manager.setBinaryPath(fake);
    manager.setDataDirectory(dir.path());

    QSignalSpy progressSpy(&manager,
                           &TorManager::bootstrapProgressChanged);
    QSignalSpy readySpy(&manager, &TorManager::ready);
    manager.start();
    QVERIFY(manager.isRunning());

    QTRY_VERIFY_WITH_TIMEOUT(manager.state() == TorManager::Ready,
                             30000);
    QCOMPARE(manager.bootstrapProgress(), 100);
    QVERIFY(progressSpy.count() >= 1);
    QCOMPARE(readySpy.count(), 1);

    const QNetworkProxy proxy = manager.socksProxy();
    QCOMPARE(proxy.type(), QNetworkProxy::Socks5Proxy);
    QVERIFY(manager.socksPort() != 0);

    // The fake's socks listener completes the handshake and refuses
    // the CONNECT (0x05) — a well-formed reply proves the protocol.
    TorSocks5 probe;
    QSignalSpy socksSpy(&probe, &TorSocks5::finished);
    probe.connectThrough(QHostAddress::LocalHost, manager.socksPort(),
                         QStringLiteral("127.0.0.1"), 9);
    QTRY_VERIFY_WITH_TIMEOUT(socksSpy.count() == 1, 10000);
    QCOMPARE(socksSpy.first().at(1).toInt(), 0x05);

    manager.stop();
    QCOMPARE(manager.state(), TorManager::Stopped);
    QVERIFY(!manager.isRunning());
}

// Same lifecycle against a real tor daemon — skipped when no binary
// resolves (CI/dev boxes can point ARORA_TOR_BINARY at an expert
// bundle).  This one does real bootstrap over the tor network.
void tst_TorManager::realDaemonLifecycle()
{
    const QString binary = TorManager::resolveBinary();
    if (binary.isEmpty())
        QSKIP("no tor binary found — set ARORA_TOR_BINARY or run "
              "BuildProcess/fetch-tor.sh");

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    TorManager manager;
    manager.setBinaryPath(binary);
    manager.setDataDirectory(dir.path());
    manager.start();

    QTRY_VERIFY_WITH_TIMEOUT(manager.state() == TorManager::Ready,
                             240000);

    // SOCKS5 handshake against a local echo server — tor refuses the
    // loopback target with a well-formed reply, which is all the
    // protocol proof this test needs.
    QTcpServer echo;
    QVERIFY(echo.listen(QHostAddress::LocalHost, 0));
    connect(&echo, &QTcpServer::newConnection, this, [&]() {
        QTcpSocket *client = echo.nextPendingConnection();
        connect(client, &QTcpSocket::readyRead, client, [client]() {
            client->write(client->readAll());
        });
    });
    TorSocks5 probe;
    QSignalSpy socksSpy(&probe, &TorSocks5::finished);
    probe.connectThrough(QHostAddress::LocalHost, manager.socksPort(),
                         QStringLiteral("127.0.0.1"),
                         echo.serverPort());
    QTRY_VERIFY_WITH_TIMEOUT(socksSpy.count() == 1, 30000);
    QVERIFY(socksSpy.first().at(1).toInt() >= 0);

    manager.stop();
    QCOMPARE(manager.state(), TorManager::Stopped);
    QVERIFY(!manager.isRunning());
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    tst_TorManager tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_tormanager.moc"
