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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// SEC06: certificate-error interstitial, exercised end-to-end — an
// in-process QSslServer with a self-signed certificate triggers
// QWebEnginePage::certificateError; WebPage defers the error and
// renders the interstitial whose action links resolve the deferred
// decision via the arora-cert-error: scheme.
//
// Verified Qt 6.11.3 semantics this suite encodes:
//  * acceptCertificate() resumes the parked request and marks the
//    (host, certificate) pair allowed in Chromium's in-memory cert
//    policy — a second navigation to the same host on the SAME profile
//    is auto-allowed without firing certificateError again.
//  * Nothing is persisted: a different profile (and a new app run)
//    prompts again.  "Proceed" therefore means "this session only".
//
// Timing note: the interstitial commits a loadFinished(true) of its
// own when the arora-cert-error: page is served.  Every test waits for
// that commit before evaluating page DOM or clicking action links.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qsslserver.h>
#include <qsslcertificate.h>
#include <qsslconfiguration.h>
#include <qsslkey.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebengineprofile.h>

#include "browserprofile.h"
#include "schemeaccesshandler.h"
#include "webpage.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

// Self-signed throwaway fixture (openssl req -x509, CN=localhost,
// notBefore 2026-10-06, notAfter 2126).  Serving it produces
// QWebEngineCertificateError::CertificateAuthorityInvalid, which is
// overridable — exactly what the interstitial's Proceed path needs.
static const char kCertPem[] =
    "-----BEGIN CERTIFICATE-----\n"
    "MIIDCzCCAfOgAwIBAgIUTbMgHE3Sc5n8Vzi77pb041A4KnwwDQYJKoZIhvcNAQEL\n"
    "BQAwFDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MTAwNjExMjcwNloYDzIxMjYw\n"
    "OTEyMTEyNzA2WjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwggEiMA0GCSqGSIb3DQEB\n"
    "AQUAA4IBDwAwggEKAoIBAQDDyiouyyclnFPAnyP9QIoraxV4yKNEj5z1qmf3NwFt\n"
    "ocntiCV922hWrPXA/+WWTsLj4yzR2a1FoeBrk0u14mwvNACF5wCbYSwWV72R3KKh\n"
    "cSjECNBPbpUxQ7S586BEQRz05E2HU7CsWqsH9J3TWCSbYmM4gwqt9YY4/pJYuZZS\n"
    "4BeInjbbd4xbbe7pWKOyWCuyslmRaBkYHaEYx3udRDue3C0aT2atpl/l7L2OkLnk\n"
    "W8jWUI0h87JyM+WnEPmzx6PZw/JH9y9Kn0h1y82hFhIMZAUUU50XB8eVh4vQuOB3\n"
    "1BDVd0F2XCUyhzmRWYlFwjqOmw4Nh2QeG9yKUTt325ktAgMBAAGjUzBRMB0GA1Ud\n"
    "DgQWBBQmYJe74z7A2yBW7YsV+qx3duyGuDAfBgNVHSMEGDAWgBQmYJe74z7A2yBW\n"
    "7YsV+qx3duyGuDAPBgNVHRMBAf8EBTADAQH/MA0GCSqGSIb3DQEBCwUAA4IBAQBy\n"
    "yrvccHeE6v+53NSLhbdsVHuOMGHcD1SY5FV9GJu012ZVVD5RbHsSGfRUyUqakJzE\n"
    "2/Ckmph84nKXGYjrNZ2X28TfP6W6t0aImdoE7Rt2x+ZgT4Xq+BdjuIXDrv4jnM8M\n"
    "rvS6HVHkcAIlN6UPlq3wlQPeaXXGUClSbM955M+5jpxZZNf72uG02STAzQx+CD1q\n"
    "5FPanpgkkNl3k8A2aaIDfGTP9wT4PgDoQnymukkOTd2kNZa/JdbtiIe3d1lsOoQh\n"
    "QQKVI9/Qf93wye4HoX+1WRX99x2ECVdZuuxFsKDMCTCfenj/zTNz6WoFGk3WyBWw\n"
    "B1olcT7+r8q+KC4lTIPm\n"
    "-----END CERTIFICATE-----\n";

static const char kKeyPem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDDyiouyyclnFPA\n"
    "nyP9QIoraxV4yKNEj5z1qmf3NwFtocntiCV922hWrPXA/+WWTsLj4yzR2a1FoeBr\n"
    "k0u14mwvNACF5wCbYSwWV72R3KKhcSjECNBPbpUxQ7S586BEQRz05E2HU7CsWqsH\n"
    "9J3TWCSbYmM4gwqt9YY4/pJYuZZS4BeInjbbd4xbbe7pWKOyWCuyslmRaBkYHaEY\n"
    "x3udRDue3C0aT2atpl/l7L2OkLnkW8jWUI0h87JyM+WnEPmzx6PZw/JH9y9Kn0h1\n"
    "y82hFhIMZAUUU50XB8eVh4vQuOB31BDVd0F2XCUyhzmRWYlFwjqOmw4Nh2QeG9yK\n"
    "UTt325ktAgMBAAECggEADyqX6Ja1akUZJRtZbwK9hpjS9E+b0Mch133jHn6qTlMc\n"
    "EJmyncnjOTjzXdyfbOyeTYeJhJqiFiTF/Pn6AWrlX9AIF6yfRWoE6kN9sdDaNZCj\n"
    "IccKVpIwXb1AQK9N7juaV0tgHhLY5WEDTVMg7qrYmxjHVYIBJRyXJc8X3budQ2oU\n"
    "PchtYdmpsT234Q12qKMCa03PYY4WWfr5f8j95HhwtWKD3Y1UpT4LKi03Db+AoIb2\n"
    "puG3N6SIg7nro+4ZsFvLposVuSt4Qt9sxqKPv69/fuAnj+nMPsczWXsKUGiP2PWw\n"
    "xS6FlO/+Gg+/9Z3B7bCjtKVvDyBYkyxv5dDlriQm2QKBgQD7Utdrxwsyvm5IDuKD\n"
    "HT/DUd7onr/qSL2eqnuXFt6STXW0FyZXXVmxc2dgkKkJknX+FCUGvUCBwUh/Ix9n\n"
    "MWTkUy0/0dYjLLVes+B76ANEPaEqST4+KHaQtis3Aedv8Xdd8fdp6VO+/xEPycCA\n"
    "Ev3gVI0kB8qO3DcNZj0dklbe1wKBgQDHbsrV/di5NeOGPSEfTMQ1f6RXdsSyDP83\n"
    "X9DmtCJdwPvGGD7mxncWLlMks1igNyiQm32umVusy456oF0ikoPCqzHuUm8aYx13\n"
    "jfq7AGo30c3tLci/a3OxAWVZ6Xduf4+cHneZyd51fpJERNauMGKWrJEJ5cNv7Hys\n"
    "mkXDHA4bmwKBgQDYqBy/c9LKhvHpOrO4lhFu3vhMDvahEN4ulwd+Q5/R/ea3PrG1\n"
    "5Oq6mCMJUwv2DYWcaF9XBPVEJozJ1UxwGFvCnZXHi6yPnC4qmuStzGBshriWDJgU\n"
    "26sCq5hrjj+m+EGQAlTov9WLNLXPp3xErJqhtiIKapELQPpQmc+b25j59wKBgQCf\n"
    "NayKHyfH2+RygxSRJziwOOEazf8C4WtRapWbx4xz6h9VKn/0vdXOCFdpwh9rb/1b\n"
    "TDaOj3FnRe3NqX4QnoS/gOnQh1CY8S1SBy914EProftScC2F2yM12JZvq/kjZoRg\n"
    "LtZuGIEWrV/ZTldQIeJixQrYqutVy6ZQKKyusRP2JwKBgG1E2m76kEWlhL+sHGvw\n"
    "pMuudfbb12aqqOJDzTe3mO0eG5+vtjTAF0A8XS97M3M++0lo9VgJ/k0VRwJCGtzb\n"
    "yJE/U585+C3kHXj9Zzb0J6R4xhr1lH38OB2Nsb85rPaInT8eNTIMWRwOtCqebRMW\n"
    "lV8TxxQIoK3MCPJRjJOtX+/w\n"
    "-----END PRIVATE KEY-----\n";

// Minimal HTTPS responder.  QSslServer signals newConnection when the
// TCP connection is accepted, but the socket only lands in the pending
// queue after the TLS handshake completes — with a certificate error
// the client stalls mid-handshake until the deferred decision lands,
// so the pending queue must be drained on a timer, not just on
// newConnection.  Bytes can already be buffered by then, hence the
// bytesAvailable() check after connecting readyRead.
class LocalTlsServer : public QObject
{
    Q_OBJECT

public:
    LocalTlsServer(QObject *parent = 0)
        : QObject(parent)
    {
        QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
        ssl.setLocalCertificate(QSslCertificate(kCertPem));
        ssl.setPrivateKey(QSslKey(kKeyPem, QSsl::Rsa, QSsl::Pem,
                                  QSsl::PrivateKey));
        m_server.setSslConfiguration(ssl);

        connect(&m_server, &QTcpServer::newConnection, this,
                [this]() { drain(); });
        connect(&m_drainTimer, &QTimer::timeout, this,
                [this]() { drain(); });
        m_drainTimer.start(20);
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        return QUrl(QString::fromLatin1("https://127.0.0.1:%1/")
                    .arg(m_server.serverPort()));
    }

    int requestCount = 0;

private:
    void drain()
    {
        while (m_server.hasPendingConnections()) {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            auto respond = [this, socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                socket->readAll();
                ++requestCount;
                const QByteArray body =
                    QByteArrayLiteral("<html><body>secure-ok</body></html>");
                socket->write(QByteArrayLiteral(
                    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: ")
                    + QByteArray::number(body.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + body);
                socket->flush();
                socket->disconnectFromHost();
            };
            connect(socket, &QTcpSocket::readyRead, socket, respond);
            if (socket->bytesAvailable() > 0)
                respond();
        }
    }

    QSslServer m_server;
    QTimer m_drainTimer;
};

// Plain HTTP responder for the "good" page the Back button returns to.
class LocalHttpServer : public QObject
{
    Q_OBJECT

public:
    LocalHttpServer(QObject *parent = 0)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this, [socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                socket->readAll();
                const QByteArray body =
                    QByteArrayLiteral("<html><body>plain-ok</body></html>");
                socket->write(QByteArrayLiteral(
                    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: ")
                    + QByteArray::number(body.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + body);
                socket->disconnectFromHost();
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1/")
                    .arg(m_server.serverPort()));
    }

private:
    QTcpServer m_server;
};

class tst_CertError : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void interstitialShowsDetails();
    void proceedLoadsPage();
    void backToSafety();
    void backFallsBackToStartPage();
    void forgedProceedLinkIgnored();
    void sessionPolicyAndRePrompt();
    void subframeDeniedSilently();

private:
    QVariant evalSync(QWebEnginePage *page, const QString &js);
    QString domText(QWebEnginePage *page);
    // Waits for the interstitial's own loadFinished(true) commit after
    // the certificateErrorInterstitial signal — the DOM is only safe to
    // inspect/click afterwards.  baseline is the loaded-signal count
    // before the failing navigation was issued.
    bool waitInterstitialCommit(QSignalSpy &interstitial,
                                QSignalSpy &loaded, int baseline);
    // Synthesises a real mouse click at the element's centre —
    // element.click() is not a user gesture, and Chromium gates
    // custom-protocol navigations on one.
    bool clickElement(WebView *view, const QString &id);
};

QVariant tst_CertError::evalSync(QWebEnginePage *page, const QString &js)
{
    QVariant result;
    bool done = false;
    page->runJavaScript(js, [&](const QVariant &v) { result = v; done = true; });
    for (int waited = 0; !done && waited < 10000; waited += 50)
        QTest::qWait(50);
    return result;
}

QString tst_CertError::domText(QWebEnginePage *page)
{
    return evalSync(page, QLatin1String(
        "document.documentElement.outerHTML")).toString();
}

bool tst_CertError::waitInterstitialCommit(QSignalSpy &interstitial,
        QSignalSpy &loaded, int baseline)
{
    for (int waited = 0; waited < 15000; waited += 50) {
        if (interstitial.count() >= 1 && loaded.count() > baseline
            && loaded.last().at(0).toBool())
            return true;
        QTest::qWait(50);
    }
    return false;
}

bool tst_CertError::clickElement(WebView *view, const QString &id)
{
    const QVariantList rect = evalSync(view->webPage(), QString::fromLatin1(
        "var e = document.getElementById('%1');"
        "var r = e ? e.getBoundingClientRect() : null;"
        "r ? [r.left + r.width / 2, r.top + r.height / 2] : null")
        .arg(id)).toList();
    if (rect.isEmpty())
        return false;
    const QPointF pos(rect.at(0).toDouble(), rect.at(1).toDouble());
    // WebEngine input goes to the render delegate child (focusProxy),
    // not the QWebEngineView itself — same pattern as --browser-smoke.
    QWidget *proxy = view->focusProxy() ? view->focusProxy() : view;
    QMouseEvent press(QEvent::MouseButtonPress, pos,
                      proxy->mapToGlobal(pos.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos,
                        proxy->mapToGlobal(pos.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &press);
    QCoreApplication::sendEvent(proxy, &release);
    return true;
}

void tst_CertError::initTestCase()
{
    QCoreApplication::setApplicationName("tst_certerror");
    QVERIFY2(QSslSocket::supportsSsl(), "TLS backend required");
}

void tst_CertError::interstitialShowsDetails()
{
    LocalTlsServer server;
    QVERIFY(server.start());

    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(interstitial.count() == 1, 15000);
    QCOMPARE(interstitial.at(0).at(0).toUrl(), server.url());
    QVERIFY(waitInterstitialCommit(interstitial, loaded, 0));

    // The real request never reached the server — it is parked while
    // the interstitial waits for a decision.
    QCOMPARE(server.requestCount, 0);

    const QString html = domText(page);
    // Error description and the failing host are shown...
    QVERIFY2(html.contains(QLatin1String("127.0.0.1")), qPrintable(html));
    QVERIFY(html.contains(QLatin1String("certificate"), Qt::CaseInsensitive));
    // ... certificate chain detail (subject CN=localhost) ...
    QVERIFY2(html.contains(QLatin1String("localhost")), qPrintable(html));
    // ... and both action links for an overridable error.
    QVERIFY(html.contains(QLatin1String("id=\"back\"")));
    QVERIFY(html.contains(QLatin1String("id=\"proceed\"")));
    QVERIFY(html.contains(QLatin1String("arora-cert-error:proceed?n=")));
    QVERIFY(html.contains(QLatin1String("arora-cert-error:back?n=")));
}

void tst_CertError::proceedLoadsPage()
{
    LocalTlsServer server;
    QVERIFY(server.start());

    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(server.url());
    QVERIFY(waitInterstitialCommit(interstitial, loaded, 0));

    // Clicking "Proceed anyway" accepts the deferred certificate error
    // and the parked request resumes — the real page loads.
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        domText(page).contains(QLatin1String("secure-ok")), 15000);
    QCOMPARE(page->url(), server.url());
}

void tst_CertError::backToSafety()
{
    LocalHttpServer http;
    QVERIFY(http.start());
    LocalTlsServer tls;
    QVERIFY(tls.start());

    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();

    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));
    page->load(http.url());
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 15000);
    QVERIFY(domText(page).contains(QLatin1String("plain-ok")));

    // A cert-error navigation on top of real history: "Back to safety"
    // denies the certificate and returns to the previous page.
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    const int baseline = loaded.count();
    page->load(tls.url());
    QVERIFY(waitInterstitialCommit(interstitial, loaded, baseline));

    QVERIFY(clickElement(&view, QLatin1String("back")));
    QTRY_VERIFY_WITH_TIMEOUT(page->url() == http.url(), 15000);
    QVERIFY(domText(page).contains(QLatin1String("plain-ok")));
    QCOMPARE(tls.requestCount, 0);
}

void tst_CertError::backFallsBackToStartPage()
{
    LocalTlsServer server;
    QVERIFY(server.start());

    // No prior history — "Back to safety" falls back to the start page.
    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(server.url());
    QVERIFY(waitInterstitialCommit(interstitial, loaded, 0));

    QVERIFY(clickElement(&view, QLatin1String("back")));
    QTRY_VERIFY_WITH_TIMEOUT(
        page->url() == QUrl(QLatin1String("qrc:/startpage.html")), 15000);
}

void tst_CertError::forgedProceedLinkIgnored()
{
    LocalTlsServer server;
    QVERIFY(server.start());

    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(server.url());
    QVERIFY(waitInterstitialCommit(interstitial, loaded, 0));

    // A proceed link without the rendered nonce must not resolve the
    // deferred error — inject a forged anchor into the live page and
    // drive it through a real click so the navigation goes through the
    // same path the genuine link uses.
    evalSync(page, QLatin1String(
        "var a = document.createElement('a');"
        "a.id = 'forge'; a.href = 'arora-cert-error:proceed?n=forged';"
        "a.textContent = 'forge'; document.body.appendChild(a); true"));
    QVERIFY(clickElement(&view, QLatin1String("forge")));
    QTest::qWait(1000);
    QVERIFY(domText(page).contains(QLatin1String("id=\"proceed\"")));
    QCOMPARE(server.requestCount, 0);

    // The real link still works afterwards.
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        domText(page).contains(QLatin1String("secure-ok")), 15000);
}

void tst_CertError::sessionPolicyAndRePrompt()
{
    LocalTlsServer server;
    QVERIFY(server.start());

    QWebEngineProfile profile1;
    WebView view(&profile1);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(server.url());
    QVERIFY(waitInterstitialCommit(interstitial, loaded, 0));
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        domText(page).contains(QLatin1String("secure-ok")), 15000);

    // Same profile, same host: Chromium's in-memory cert policy
    // auto-allows the decision — the load commits without another
    // interstitial (same as Chrome's "proceed" behaviour, but unlike
    // Chrome nothing is persisted to disk).
    WebView view2(&profile1);
    view2.resize(800, 600);
    view2.show();
    WebPage *page2 = view2.webPage();
    QSignalSpy interstitial2(page2,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded2(page2, SIGNAL(loadFinished(bool)));
    page2->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(
        domText(page2).contains(QLatin1String("secure-ok")), 15000);
    QTest::qWait(500);
    QCOMPARE(interstitial2.count(), 0);

    // A different profile is a different session context — the
    // certificate error prompts again.
    QWebEngineProfile profile2;
    WebView view3(&profile2);
    view3.resize(800, 600);
    view3.show();
    WebPage *page3 = view3.webPage();
    QSignalSpy interstitial3(page3,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    page3->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(interstitial3.count() == 1, 15000);
}

void tst_CertError::subframeDeniedSilently()
{
    LocalTlsServer server;
    QVERIFY(server.start());
    LocalHttpServer http;
    QVERIFY(http.start());

    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy interstitial(page,
            SIGNAL(certificateErrorInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->setHtml(QString::fromLatin1(
        "<html><body>host-page"
        "<iframe src=\"%1\"></iframe></body></html>")
        .arg(server.url().toString()), http.url());
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
    QTest::qWait(1500);

    // The subframe's certificate error is denied without disturbing the
    // host page — no interstitial, no navigation.
    QCOMPARE(interstitial.count(), 0);
    QVERIFY(domText(page).contains(QLatin1String("host-page")));
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    // arora-cert-error: must be registered before BrowserApplication's
    // constructor brings up the browsing profile (same order as
    // main.cpp) — unregistered schemes go down Chromium's
    // external-protocol path and never reach acceptNavigationRequest.
    SchemeAccessHandler::registerUrlSchemes();
    BrowserApplication app(argc, argv);
    tst_CertError tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_certerror.moc"
