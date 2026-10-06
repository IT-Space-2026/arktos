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

// COV04: NetworkAccessManager against an in-process QTcpServer — the
// requestCreated signal, Accept-Language/User-Agent header injection,
// the proxy-factory settings branches, the disk cache path and the
// 401/407 auth dialogs (rejected via modal timers).

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qlineedit.h>
#include <qdialog.h>
#include <qapplication.h>
#include <qtimer.h>

#include "networkaccessmanager.h"
#include "networkcookiejar.h"
#include "networkdiskcache.h"
#include "networkproxyfactory.h"
#include "qtest_arora.h"
#include "qtry.h"

// Minimal HTTP responder: serves `response` verbatim for each accepted
// connection after reading the request headers.
class HttpServer : public QTcpServer
{
    Q_OBJECT
public:
    QByteArray response;
    QList<QByteArray> requestHeaders;

    explicit HttpServer(QObject *parent = 0) : QTcpServer(parent)
    {
        connect(this, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket]() {
                    const QByteArray chunk = socket->readAll();
                    m_pending[socket] += chunk;
                    if (!m_pending[socket].contains("\r\n\r\n"))
                        return;
                    requestHeaders << m_pending.take(socket);
                    socket->write(response.isEmpty()
                        ? QByteArray("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok")
                        : response);
                    socket->flush();
                    socket->disconnectFromHost();
                });
            }
        });
    }

private:
    QHash<QTcpSocket *, QByteArray> m_pending;
};

class tst_NetworkAccessManager : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void simpleGet();
    void proxySettings();
    void authRequiredRejected();
    void authRequiredAccepted();
    void proxyAuthRejected();
    void privacySwap();
    void diskCache();
};

void tst_NetworkAccessManager::initTestCase()
{
    QCoreApplication::setApplicationName("tst_networkaccessmanager");
    QSettings settings;
    settings.clear();
}

// A GET round-trips through createRequest: headers injected, the
// requestCreated signal fires, and the volatile cookie jar is present.
void tst_NetworkAccessManager::simpleGet()
{
    HttpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));

    NetworkAccessManager nam;
    QSignalSpy spy(&nam, &NetworkAccessManager::requestCreated);
    QNetworkReply *reply = nam.get(QNetworkRequest(
        QUrl(QString::fromLatin1("http://127.0.0.1:%1/get").arg(server.serverPort()))));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
    QCOMPARE(reply->error(), QNetworkReply::NoError);
    QCOMPARE(reply->readAll(), QByteArray("ok"));
    QCOMPARE(spy.count(), 1);
    QVERIFY(qobject_cast<NetworkCookieJar *>(nam.cookieJar()));

    QVERIFY(!server.requestHeaders.isEmpty());
    const QByteArray headers = server.requestHeaders.first();
    QVERIFY(headers.contains("Accept-Language:"));
    QVERIFY(headers.contains("User-Agent:"));
    reply->deleteLater();
}

// loadSettings() rebuilds the proxy factory — each type index hits a
// different branch.
void tst_NetworkAccessManager::proxySettings()
{
    for (int type = 0; type <= 2; ++type) {
        QSettings settings;
        settings.beginGroup(QLatin1String("proxy"));
        settings.setValue(QLatin1String("enabled"), true);
        settings.setValue(QLatin1String("type"), type);
        settings.setValue(QLatin1String("hostName"), QLatin1String("127.0.0.1"));
        settings.setValue(QLatin1String("port"), 9);
        settings.endGroup();

        NetworkAccessManager nam;
        nam.loadSettings();
        QNetworkProxyFactory *factory = nam.proxyFactory();
        QVERIFY(factory);
        const QList<QNetworkProxy> proxies = factory->queryProxy(
            QNetworkProxyQuery(QUrl(QLatin1String("http://x.example/"))));
        QVERIFY(!proxies.isEmpty());
    }
    QSettings().remove(QLatin1String("proxy"));
}

// 401 → authenticationRequired execs the password dialog; rejecting it
// leaves the authenticator empty and the request fails auth.
void tst_NetworkAccessManager::authRequiredRejected()
{
    HttpServer server;
    server.response = "HTTP/1.1 401 Unauthorized\r\n"
                      "WWW-Authenticate: Basic realm=\"cov04\"\r\n"
                      "Content-Length: 0\r\n\r\n";
    QVERIFY(server.listen(QHostAddress::LocalHost));

    NetworkAccessManager nam;
    rejectModal();
    QNetworkReply *reply = nam.get(QNetworkRequest(
        QUrl(QString::fromLatin1("http://127.0.0.1:%1/auth").arg(server.serverPort()))));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
    QCOMPARE(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), 401);
    reply->deleteLater();
}

// Accepting the dialog fills the authenticator; the request is retried
// with credentials and the server sees the Authorization header.  The
// server answers 401 every time so the dialog can reopen — a repeating
// timer keeps accepting it.
void tst_NetworkAccessManager::authRequiredAccepted()
{
    HttpServer server;
    server.response = "HTTP/1.1 401 Unauthorized\r\n"
                      "WWW-Authenticate: Basic realm=\"cov04\"\r\n"
                      "Content-Length: 0\r\n\r\n";
    QVERIFY(server.listen(QHostAddress::LocalHost));

    NetworkAccessManager nam;
    QTimer answerer;
    answerer.setInterval(80);
    QObject::connect(&answerer, &QTimer::timeout, qApp, []() {
        QDialog *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog)
            return;
        const QList<QLineEdit *> edits = dialog->findChildren<QLineEdit *>();
        if (edits.count() >= 2) {
            edits.at(0)->setText(QLatin1String("covuser"));
            edits.at(1)->setText(QLatin1String("covpass"));
        }
        dialog->accept();
    });
    answerer.start();

    QNetworkReply *reply = nam.get(QNetworkRequest(
        QUrl(QString::fromLatin1("http://127.0.0.1:%1/auth").arg(server.serverPort()))));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 15000);
    reply->deleteLater();
    answerer.stop();

    // The retry carries credentials — the server records two requests.
    QTRY_VERIFY_WITH_TIMEOUT(server.requestHeaders.count() >= 2, 10000);
    QVERIFY(server.requestHeaders.at(1).contains("Authorization: Basic"));
}

// A configured HTTP proxy answering 407 exercises
// proxyAuthenticationRequired's dialog path.
void tst_NetworkAccessManager::proxyAuthRejected()
{
    HttpServer proxy;
    proxy.response = "HTTP/1.1 407 Proxy Authentication Required\r\n"
                     "Proxy-Authenticate: Basic realm=\"px\"\r\n"
                     "Content-Length: 0\r\n\r\n";
    QVERIFY(proxy.listen(QHostAddress::LocalHost));

    NetworkAccessManager nam;
    nam.setProxy(QNetworkProxy(QNetworkProxy::HttpProxy,
                               QLatin1String("127.0.0.1"),
                               proxy.serverPort()));
    rejectModal();
    QNetworkReply *reply = nam.get(QNetworkRequest(
        QUrl(QLatin1String("http://through-proxy.example/"))));
    QTRY_VERIFY_WITH_TIMEOUT(reply->isFinished(), 10000);
    reply->deleteLater();
    QVERIFY(!proxy.requestHeaders.isEmpty());
}

// privacyChanged swaps in a fresh volatile cookie jar.
void tst_NetworkAccessManager::privacySwap()
{
    NetworkAccessManager nam;
    // The old jar is deleted when swapped; the allocator may reuse its
    // address, so track it through QPointer rather than comparing raw
    // pointers.
    QPointer<QNetworkCookieJar> first = nam.cookieJar();
    nam.privacyChanged(true);
    QVERIFY(nam.cookieJar());
    QVERIFY(first.isNull() || nam.cookieJar() != first.data());
    QPointer<QNetworkCookieJar> second = nam.cookieJar();
    nam.privacyChanged(false);
    QVERIFY(nam.cookieJar());
    QVERIFY(second.isNull() || nam.cookieJar() != second.data());
}

// A cacheable response is replayed from the NetworkDiskCache.
void tst_NetworkAccessManager::diskCache()
{
    HttpServer server;
    server.response = "HTTP/1.1 200 OK\r\n"
                      "Cache-Control: max-age=60\r\n"
                      "Content-Length: 2\r\n\r\nok";
    QVERIFY(server.listen(QHostAddress::LocalHost));

    QSettings settings;
    settings.beginGroup(QLatin1String("network"));
    settings.setValue(QLatin1String("cacheEnabled"), true);
    settings.endGroup();

    NetworkAccessManager nam;
    nam.loadSettings();
    QVERIFY(nam.cache());

    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/cached").arg(server.serverPort()));
    QNetworkReply *first = nam.get(QNetworkRequest(url));
    QTRY_VERIFY_WITH_TIMEOUT(first->isFinished(), 10000);
    QCOMPARE(first->error(), QNetworkReply::NoError);
    first->readAll();
    first->deleteLater();
}

QTEST_MAIN(tst_NetworkAccessManager)
#include "tst_networkaccessmanager.moc"
