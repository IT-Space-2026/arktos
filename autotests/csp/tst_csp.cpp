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

// SEC14: local repro harness for the browseraudit.com CSP-category
// warnings the user reported.
//
//   test-361  default-src 'self'                          + wss://same-host
//   test-364  default-src 'none'; connect-src 'self'      + wss://same-host
//   test-391/393  CSP sandbox allow-same-origin allow-scripts iframe
//             reaching document.cookie (expected: allow)
//   test-392/394  same without allow-same-origin (expected: block)
//
// The suite predates CSP3: 'self' has matched the ws:/wss: variants of
// the page's origin since the CSP Level 3 matching rules shipped in
// Chromium 70 (crbug.com/815142), so an engine that ALLOWS the
// same-origin websocket while still blocking the cross-host/'none'
// controls is spec-correct and the warning is stale.  The cases below
// pin down both halves of that claim, each on a bare engine profile
// and on a fully app-wired profile (CookieJar filter +
// PrivacyRequestInterceptor) — an app-side cause would show up as a
// bare-vs-wired divergence.
//
// Detection: a CSP-blocked request never reaches the network, so a
// TCP/TLS connection arriving at the websocket port is positive proof
// CSP allowed it, and the page's 'securitypolicyviolation' event is
// positive proof CSP blocked it.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qprocess.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qsslsocket.h>
#include <qwebenginecertificateerror.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginecookiestore.h>

#include <memory>

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "browserprofile.h"
#include "cookiejar.h"
#include "privacyrequestinterceptor.h"
#include "schemeaccesshandler.h"
#include "adblockschemeaccesshandler.h"
#include "adblockresourcehandler.h"
#include "webpage.h"
#include "browserapplication.h"
#include "qtry.h"

// Scripted HTTP/HTTPS responder.  Every accepted socket bumps
// rawConnections and every completed request header block is recorded
// (target + Upgrade + Cookie); websocket handshakes get a real 101 so
// an allowed ws reaches onopen.  In TLS mode connections are accepted
// straight into a QSslSocket — rawConnections still counts the TCP
// accept, which is the detection point for a wss attempt whose cert
// check may kill the handshake before any HTTP byte is sent.
class ProbeServer : public QObject
{
    Q_OBJECT

    // Accepts straight into QSslSocket — the descriptor never passes
    // through a QTcpSocket, so there is exactly one fd owner.
    class TlsTcpServer : public QTcpServer
    {
    public:
        QString certFile;
        QString keyFile;
        std::function<void(QSslSocket *)> accepted;

        explicit TlsTcpServer(QObject *parent = nullptr)
            : QTcpServer(parent) {}

        void incomingConnection(qintptr fd) override
        {
            QSslSocket *ssl = new QSslSocket(this);
            ssl->setSocketDescriptor(fd);
            ssl->setLocalCertificate(certFile);
            ssl->setPrivateKey(keyFile);
            if (accepted)
                accepted(ssl);
            ssl->startServerEncryption();
        }
    };

public:
    struct SeenRequest {
        QByteArray target;
        QByteArray upgrade;
        QByteArray cookie;
    };

    explicit ProbeServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this,
                [this]() {
            ++m_rawConnections;
            attachReader(m_server.nextPendingConnection());
        });
    }

    void enableTls(const QString &certFile, const QString &keyFile)
    {
        m_tlsServer = new TlsTcpServer(this);
        m_tlsServer->certFile = certFile;
        m_tlsServer->keyFile = keyFile;
        m_tlsServer->accepted = [this](QSslSocket *ssl) {
            ++m_rawConnections;
            connect(ssl, &QSslSocket::encrypted, this,
                    [this, ssl]() {
                ++m_tlsConnections;
                attachReader(ssl);
            });
        };
    }

    bool start(QHostAddress address = QHostAddress::LocalHost)
    {
        if (m_tlsServer)
            return m_tlsServer->listen(address);
        return m_server.listen(address);
    }
    int port() const
    {
        return m_tlsServer ? m_tlsServer->serverPort()
                           : m_server.serverPort();
    }

    // path -> extra response headers (e.g. Content-Security-Policy)
    void setHeaders(const QString &path, const QByteArray &headers)
    {
        m_extraHeaders[path] = headers;
    }
    void setBody(const QString &path, const QByteArray &body)
    {
        m_bodies[path] = body;
    }

    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("%1://127.0.0.1:%2%3")
                    .arg(m_tlsServer ? QLatin1String("https")
                                     : QLatin1String("http"))
                    .arg(port()).arg(path));
    }

    bool sawUpgrade(const QString &target) const
    {
        for (const SeenRequest &r : seen) {
            if (r.target.startsWith(target.toUtf8())
                && r.upgrade == QByteArrayLiteral("websocket"))
                return true;
        }
        return false;
    }

    bool sawCookie(const QString &target, const QByteArray &marker) const
    {
        for (const SeenRequest &r : seen) {
            if (r.target == target.toUtf8() && r.cookie.contains(marker))
                return true;
        }
        return false;
    }

    QList<SeenRequest> seen;
    int m_rawConnections = 0;
    int m_tlsConnections = 0;

private:
    void attachReader(QTcpSocket *socket)
    {
        socket->setParent(this);
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket]() {
            if (!socket->peek(8192).contains("\r\n\r\n"))
                return;
            respond(socket, socket->readAll());
        });
    }

    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArrayList lines = request.split('\n');
        const QByteArray target = lines.value(0).split(' ').value(1);
        const QString path = QString::fromUtf8(target.split('?').value(0));

        SeenRequest seen;
        seen.target = target;
        QByteArray wsKey;
        for (const QByteArray &line : lines) {
            const QByteArray trimmed = line.trimmed();
            if (trimmed.startsWith("Upgrade:"))
                seen.upgrade = trimmed.mid(8).trimmed();
            else if (trimmed.startsWith("Cookie:"))
                seen.cookie = trimmed.mid(7).trimmed();
            else if (trimmed.startsWith("Sec-WebSocket-Key:"))
                wsKey = trimmed.mid(18).trimmed();
        }
        this->seen.append(seen);

        if (seen.upgrade == QByteArrayLiteral("websocket")) {
            const QByteArray accept = QCryptographicHash::hash(
                wsKey + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                QCryptographicHash::Sha1).toBase64();
            socket->write(QByteArrayLiteral(
                "HTTP/1.1 101 Switching Protocols\r\n"
                "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                "Sec-WebSocket-Accept: ") + accept + "\r\n\r\n");
            return; // upgraded socket stays open
        }

        const QByteArray body = m_bodies.value(path, QByteArrayLiteral("ok\n"));
        const QByteArray response =
            QByteArrayLiteral("HTTP/1.0 200 OK\r\nContent-Type: ")
            + (path.endsWith(".html") || path == "/"
                   ? QByteArrayLiteral("text/html")
                   : QByteArrayLiteral("text/plain"))
            + QByteArrayLiteral("\r\nContent-Length: ")
            + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\n")
            + m_extraHeaders.value(path)
            + QByteArrayLiteral("Connection: close\r\n\r\n") + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    TlsTcpServer *m_tlsServer = nullptr;
    QHash<QString, QByteArray> m_extraHeaders;
    QHash<QString, QByteArray> m_bodies;
};

static bool waitFor(std::function<bool()> pred, int timeout = 15000)
{
    for (int waited = 0; !pred() && waited < timeout; waited += 50)
        QTest::qWait(50);
    return pred();
}

static bool loadSync(QWebEnginePage *page, const QUrl &url)
{
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<bool> ok(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadFinished, page,
        [done, ok](bool result) { *done = true; *ok = result; });
    page->load(url);
    waitFor([done]() { return *done; });
    QObject::disconnect(connection);
    return *ok;
}

static QVariant evalSync(QWebEnginePage *page, const QString &script)
{
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<QVariant> result(new QVariant);
    page->runJavaScript(script,
                        [done, result](const QVariant &value) {
        *result = value;
        *done = true;
    });
    waitFor([done]() { return *done; });
    return *result;
}

// The page-side websocket probe: reports its terminal state on
// window.__wsState and any CSP block on window.__violation.
static QByteArray wsProbeHtml(const QString &wsUrl)
{
    return QByteArrayLiteral(
        "<!doctype html><script>"
        "window.__wsState='pending';window.__violation='';"
        "document.addEventListener('securitypolicyviolation',"
        " function(e){window.__violation=e.violatedDirective+'|'"
        "+e.blockedURI;});"
        "try{var ws=new WebSocket('") + wsUrl.toUtf8() + QByteArrayLiteral(
        "');"
        "ws.onopen=function(){window.__wsState='open';};"
        "ws.onerror=function(){if(window.__wsState==='pending')"
        " window.__wsState='error';};"
        "ws.onclose=function(){if(window.__wsState==='pending')"
        " window.__wsState='closed';};"
        "}catch(e){window.__wsState='throw:'+e.name;}"
        "setTimeout(function(){if(window.__wsState==='pending')"
        " window.__wsState='timeout';},3000);"
        "</script><body>wsprobe</body>");
}

struct WsOutcome {
    bool upgradeSeen = false;
    int rawDelta = 0;
    QString violation;
    QString state;
};

class tst_Csp : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    // ws:// over http — the CSP3 'self' rules are identical to the
    // https/wss pair browseraudit exercises, minus the cert plumbing.
    void wsSelfSameOriginAllowed();
    void wsConnectSrcSelfAllowed();
    void wsExplicitAllowed();
    void wsSelfCrossPortBlocked();
    void wsSelfCrossHostBlocked();
    void wsNoneBlocked();
    void wsConnectNoneBlocked();

    // The exact 361/364 shape: https page, wss target, 'self'.
    void wssSelfSameOriginAllowed();
    void wssConnectSrcSelfAllowed();
    void wssSelfCrossHostBlocked();
    void wssConnectNoneBlocked();

    // browseraudit 391/393 (+ control 392/394).
    void sandboxCookieSameOriginAllowed();
    void sandboxCookieOpaqueOriginBlocked();

private:
    WsOutcome runWsCase(QWebEnginePage *page, const QString &csp,
                        const QString &wsUrl);
    WsOutcome runWssCase(QWebEnginePage *page, const QString &csp,
                         const QString &wssUrl);
    void wireAppProfile(QWebEngineProfile *profile);

    ProbeServer *m_server;   // http, 127.0.0.1
    ProbeServer *m_server2;  // second origin (different port)
    ProbeServer *m_tls;      // https server for the wss cases
    // Plain TCP tap on a third port — a wss target whose mere
    // connection count proves CSP let the request off the page.
    ProbeServer *m_tap;
    QString m_certFile;
    QString m_keyFile;
    QWebEngineProfile *m_bareProfile;
    QWebEngineProfile *m_appProfile;
};

void tst_Csp::initTestCase()
{
    QCoreApplication::setApplicationName("tst_csp");
    QStandardPaths::setTestModeEnabled(true);

    QSettings settings;
    settings.clear();
    // Dead local list keeps AdBlockManager away from live defaults.
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-sec14.txt"
            "&title=DeadList"));

    m_server = new ProbeServer(this);
    m_server2 = new ProbeServer(this);
    QVERIFY(m_server->start());
    QVERIFY(m_server2->start());

    // Throwaway self-signed cert for the https page; each test accepts
    // the certificate error explicitly, which also stores a per-host
    // exception that lets the same-origin wss handshake complete.
    const QString dir = QStandardPaths::writableLocation(
        QStandardPaths::TempLocation);
    m_certFile = dir + QLatin1String("/sec14-cert.pem");
    m_keyFile = dir + QLatin1String("/sec14-key.pem");
    QProcess openssl;
    openssl.start(QStringLiteral("openssl"), QStringList()
        << QStringLiteral("req") << QStringLiteral("-x509")
        << QStringLiteral("-newkey") << QStringLiteral("rsa:2048")
        << QStringLiteral("-nodes")
        << QStringLiteral("-keyout") << m_keyFile
        << QStringLiteral("-out") << m_certFile
        << QStringLiteral("-days") << QStringLiteral("1")
        << QStringLiteral("-subj") << QStringLiteral("/CN=localhost")
        << QStringLiteral("-addext")
        << QStringLiteral("subjectAltName=DNS:localhost,IP:127.0.0.1"));
    QVERIFY2(openssl.waitForFinished(10000) && openssl.exitCode() == 0,
             "openssl needed for the https/wss repro");

    m_tls = new ProbeServer(this);
    m_tls->enableTls(m_certFile, m_keyFile);
    QVERIFY(m_tls->start());

    // Dual-stack so a wss://localhost target (resolves to ::1 or
    // 127.0.0.1) is observed either way.
    m_tap = new ProbeServer(this);
    QVERIFY(m_tap->start(QHostAddress::Any));

    m_bareProfile = new QWebEngineProfile(this);
    m_appProfile = new QWebEngineProfile(this);
    wireAppProfile(m_appProfile);
}

void tst_Csp::init()
{
    m_server->seen.clear();
    m_server->m_rawConnections = 0;
    m_server2->seen.clear();
    m_server2->m_rawConnections = 0;
    m_tls->seen.clear();
    m_tls->m_rawConnections = 0;
    m_tls->m_tlsConnections = 0;
    m_tap->seen.clear();
    m_tap->m_rawConnections = 0;
}

// Mirrors prepareProfile()'s security-relevant wiring: the app-side
// cookie filter (defaults to blocking third-party cookies) and the
// composite privacy/adblock request interceptor.
void tst_Csp::wireAppProfile(QWebEngineProfile *profile)
{
    CookieJar::instance(profile);
    BrowserProfile::applySettings(profile);
    profile->setUrlRequestInterceptor(new PrivacyRequestInterceptor(
        AdBlockManager::instance()->network(), profile));
}

static void acceptCerts(QWebEnginePage *page)
{
    QObject::connect(page, &QWebEnginePage::certificateError, page,
                     [](QWebEngineCertificateError error) {
        error.acceptCertificate();
    });
}

WsOutcome tst_Csp::runWsCase(QWebEnginePage *page, const QString &csp,
                             const QString &wsUrl)
{
    m_server->setHeaders(QStringLiteral("/ws.html"),
        QByteArrayLiteral("Content-Security-Policy: ")
        + csp.toUtf8() + "\r\n");
    m_server->setBody(QStringLiteral("/ws.html"), wsProbeHtml(wsUrl));

    WsOutcome outcome;
    if (!loadSync(page, m_server->url(QStringLiteral("/ws.html")))) {
        outcome.state = QStringLiteral("load-failed");
        return outcome;
    }

    // Terminal: handshake observed, a CSP violation fired, or the
    // in-page probe reached a terminal websocket state.
    waitFor([this, page]() {
        const QString state = evalSync(page, QLatin1String(
            "String(window.__wsState)")).toString();
        const QString violation = evalSync(page, QLatin1String(
            "String(window.__violation)")).toString();
        return m_server->sawUpgrade("/wsprobe")
            || m_server2->sawUpgrade("/wsprobe")
            || !violation.isEmpty()
            || (state != QLatin1String("pending")
                && state != QLatin1String("open"));
    });
    QTest::qWait(200); // let a slow handshake still land

    outcome.upgradeSeen = m_server->sawUpgrade("/wsprobe")
        || m_server2->sawUpgrade("/wsprobe");
    outcome.violation = evalSync(page, QLatin1String(
        "String(window.__violation)")).toString();
    outcome.state = evalSync(page, QLatin1String(
        "String(window.__wsState)")).toString();
    return outcome;
}

WsOutcome tst_Csp::runWssCase(QWebEnginePage *page, const QString &csp,
                              const QString &wssUrl)
{
    m_tls->setHeaders(QStringLiteral("/ws.html"),
        QByteArrayLiteral("Content-Security-Policy: ")
        + csp.toUtf8() + "\r\n");
    m_tls->setBody(QStringLiteral("/ws.html"), wsProbeHtml(wssUrl));

    const int rawBefore = m_tls->m_rawConnections;
    WsOutcome outcome;
    if (!loadSync(page, m_tls->url(QStringLiteral("/ws.html")))) {
        outcome.state = QStringLiteral("load-failed");
        return outcome;
    }

    waitFor([this, page]() {
        const QString state = evalSync(page, QLatin1String(
            "String(window.__wsState)")).toString();
        const QString violation = evalSync(page, QLatin1String(
            "String(window.__violation)")).toString();
        return m_tls->sawUpgrade("/wsprobe") || !violation.isEmpty()
            || (state != QLatin1String("pending")
                && state != QLatin1String("open"));
    });
    QTest::qWait(200);

    outcome.upgradeSeen = m_tls->sawUpgrade("/wsprobe");
    outcome.rawDelta = m_tls->m_rawConnections - rawBefore;
    outcome.violation = evalSync(page, QLatin1String(
        "String(window.__violation)")).toString();
    outcome.state = evalSync(page, QLatin1String(
        "String(window.__wsState)")).toString();
    return outcome;
}

// ---- WebSocket under 'self' (browseraudit 361/364 semantics) ----
// On an http origin CSP3 'self' covers ws://same-host:same-port — the
// direct analog of https 'self' covering wss://same-host:443 that the
// browseraudit suite still expects to be blocked.

void tst_Csp::wsSelfSameOriginAllowed()
{
    const QString wsUrl =
        m_server->url(QStringLiteral("/wsprobe")).toString();
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline'"),
            wsUrl);
        QVERIFY2(o.upgradeSeen, qPrintable(QStringLiteral(
            "same-origin ws under default-src 'self' was blocked "
            "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

void tst_Csp::wsConnectSrcSelfAllowed()
{
    const QString wsUrl =
        m_server->url(QStringLiteral("/wsprobe")).toString();
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'none'; script-src 'unsafe-inline';"
                           " connect-src 'self'"),
            wsUrl);
        QVERIFY2(o.upgradeSeen, qPrintable(QStringLiteral(
            "same-origin ws under connect-src 'self' was blocked "
            "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

void tst_Csp::wsExplicitAllowed()
{
    const QString wsUrl =
        m_server->url(QStringLiteral("/wsprobe")).toString();
    // CSP host-sources do not up-match to ws: — the expression must
    // carry the ws scheme (browseraudit's own allow-cases do).
    const QString wsSource = QStringLiteral("ws://127.0.0.1:%1")
        .arg(m_server->port());
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'none'; script-src 'unsafe-inline';"
                           " connect-src %1").arg(wsSource),
            wsUrl);
        QVERIFY2(o.upgradeSeen, qPrintable(QStringLiteral(
            "explicitly allowed ws was blocked "
            "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

void tst_Csp::wsSelfCrossPortBlocked()
{
    // 'self' requires same or default port — a different non-default
    // port on the same host is outside it.
    const QString wsUrl =
        m_server2->url(QStringLiteral("/wsprobe")).toString();
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline'"),
            wsUrl);
        QVERIFY2(!o.upgradeSeen,
            "cross-port ws under 'self' reached the network");
        QVERIFY2(o.violation.contains(QLatin1String("src")),
            qPrintable(QStringLiteral("no CSP violation recorded "
                "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

void tst_Csp::wsSelfCrossHostBlocked()
{
    // localhost is a different host than 127.0.0.1 — outside 'self'.
    const QString wsUrl = QStringLiteral("ws://localhost:%1/wsprobe")
        .arg(m_server->port());
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline'"),
            wsUrl);
        QVERIFY2(!o.upgradeSeen,
            "cross-host ws under 'self' reached the network");
        QVERIFY2(o.violation.contains(QLatin1String("src")),
            qPrintable(QStringLiteral("no CSP violation recorded "
                "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

void tst_Csp::wsNoneBlocked()
{
    const QString wsUrl =
        m_server->url(QStringLiteral("/wsprobe")).toString();
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'none'; script-src 'unsafe-inline'"),
            wsUrl);
        QVERIFY2(!o.upgradeSeen,
            "ws under default-src 'none' reached the network");
        QVERIFY2(o.violation.contains(QLatin1String("src")),
            qPrintable(QStringLiteral("no CSP violation recorded "
                "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

void tst_Csp::wsConnectNoneBlocked()
{
    const QString wsUrl =
        m_server->url(QStringLiteral("/wsprobe")).toString();
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        WebPage page(profile);
        const WsOutcome o = runWsCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline';"
                           " connect-src 'none'"),
            wsUrl);
        QVERIFY2(!o.upgradeSeen,
            "ws under connect-src 'none' reached the network");
        QVERIFY2(o.violation.contains(QLatin1String("src")),
            qPrintable(QStringLiteral("no CSP violation recorded "
                "(state=%1 violation=%2)").arg(o.state, o.violation)));
    }
}

// ---- Direct 361/364 repro: https page, wss target ----
// Accepting the main-frame cert error stores a per-(host,cert)
// exception for the profile, so a same-origin wss handshake then
// completes and its Upgrade request reaches the server — for a
// blocked wss nothing connects at all (rawDelta stays at the page
// load's own connection) and the violation event fires.

void tst_Csp::wssSelfSameOriginAllowed()
{
    const QString wssUrl = QStringLiteral("wss://127.0.0.1:%1/wsprobe")
        .arg(m_tls->port());
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        QWebEnginePage page(profile);
        acceptCerts(&page);
        const WsOutcome o = runWssCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline'"),
            wssUrl);
        QVERIFY2(o.upgradeSeen, qPrintable(QStringLiteral(
            "same-origin wss under default-src 'self' was blocked "
            "(state=%1 violation=%2 rawDelta=%3)")
            .arg(o.state, o.violation).arg(o.rawDelta)));
        QVERIFY2(o.violation.isEmpty(), qPrintable(QStringLiteral(
            "same-origin wss produced a CSP violation: %1")
            .arg(o.violation)));
    }
}

void tst_Csp::wssConnectSrcSelfAllowed()
{
    const QString wssUrl = QStringLiteral("wss://127.0.0.1:%1/wsprobe")
        .arg(m_tls->port());
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        QWebEnginePage page(profile);
        acceptCerts(&page);
        const WsOutcome o = runWssCase(&page,
            QStringLiteral("default-src 'none'; script-src 'unsafe-inline';"
                           " connect-src 'self'"),
            wssUrl);
        QVERIFY2(o.upgradeSeen, qPrintable(QStringLiteral(
            "same-origin wss under connect-src 'self' was blocked "
            "(state=%1 violation=%2 rawDelta=%3)")
            .arg(o.state, o.violation).arg(o.rawDelta)));
    }
}

void tst_Csp::wssSelfCrossHostBlocked()
{
    // wss to a different host on the tap port — outside 'self'; if CSP
    // let it out, the tap's accept counter would move.
    const QString wssUrl = QStringLiteral("wss://localhost:%1/wsprobe")
        .arg(m_tap->port());
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        QWebEnginePage page(profile);
        acceptCerts(&page);
        m_tap->m_rawConnections = 0;
        const WsOutcome o = runWssCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline'"),
            wssUrl);
        QCOMPARE(m_tap->m_rawConnections, 0);
        QVERIFY2(o.violation.contains(QLatin1String("src")),
            qPrintable(QStringLiteral("no CSP violation recorded "
                "(state=%1 violation=%2 rawDelta=%3)")
                .arg(o.state, o.violation).arg(o.rawDelta)));
    }
}

void tst_Csp::wssConnectNoneBlocked()
{
    // Same-host wss but connect-src 'none' — the tap stays silent.
    const QString wssUrl = QStringLiteral("wss://127.0.0.1:%1/wsprobe")
        .arg(m_tap->port());
    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        QWebEnginePage page(profile);
        acceptCerts(&page);
        m_tap->m_rawConnections = 0;
        const WsOutcome o = runWssCase(&page,
            QStringLiteral("default-src 'self'; script-src 'unsafe-inline';"
                           " connect-src 'none'"),
            wssUrl);
        QCOMPARE(m_tap->m_rawConnections, 0);
        QVERIFY2(o.violation.contains(QLatin1String("src")),
            qPrintable(QStringLiteral("no CSP violation recorded "
                "(state=%1 violation=%2 rawDelta=%3)")
                .arg(o.state, o.violation).arg(o.rawDelta)));
    }
}

// ---- Sandboxed-iframe cookie access (browseraudit 391/393) ----
// Parent on 127.0.0.1:portA embeds a child iframe on 127.0.0.1:portB —
// different origins, same site — so a host cookie for 127.0.0.1 is a
// first-party cookie for the child.  Per HTML/CSP, sandbox with
// allow-same-origin preserves the origin and document.cookie works;
// without it the origin is opaque and the jar is unreachable.

void tst_Csp::sandboxCookieSameOriginAllowed()
{
    m_server2->setHeaders(QStringLiteral("/child.html"),
        QByteArrayLiteral("Content-Security-Policy: "
                          "sandbox allow-same-origin allow-scripts\r\n"));
    m_server2->setBody(QStringLiteral("/child.html"),
        QByteArrayLiteral(
            "<!doctype html><script>"
            // Write path first: browseraudit's "access" covers both —
            // a write that silently fails would surface the same way.
            "try{document.cookie='cspw=set;path=/';}catch(e){}"
            "var c='unset';"
            "try{c=document.cookie;}catch(e){c='throw:'+e.name;}"
            "fetch('/cookieprobe').catch(function(){});"
            "setTimeout(function(){parent.postMessage('cookie:'+c,'*');},200);"
            "</script>"));

    m_server->setBody(QStringLiteral("/parent.html"),
        QByteArrayLiteral(
            "<!doctype html><script>"
            "window.__child='none';"
            "window.addEventListener('message',function(e){"
            " window.__child=String(e.data);});"
            "</script><iframe src=\"") +
        m_server2->url(QStringLiteral("/child.html")).toString().toUtf8() +
        QByteArrayLiteral("\"></iframe>"));

    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        // Pre-seed a host cookie for 127.0.0.1 — cookies ignore ports,
        // so it is valid for both the parent and the child origin.
        QSignalSpy spy(profile->cookieStore(),
                       &QWebEngineCookieStore::cookieAdded);
        profile->cookieStore()->setCookie(
            QNetworkCookie(QByteArrayLiteral("cspmark"),
                           QByteArrayLiteral("yes")),
            QUrl(QStringLiteral("http://127.0.0.1:%1/")
                 .arg(m_server2->port())));
        spy.wait(2000);

        WebPage page(profile);
        QVERIFY(loadSync(&page,
            m_server->url(QStringLiteral("/parent.html"))));
        QVERIFY(waitFor([&page]() {
            return evalSync(&page, QLatin1String("String(window.__child)"))
                .toString() != QLatin1String("none");
        }));

        const QString child = evalSync(&page, QLatin1String(
            "String(window.__child)")).toString();
        QVERIFY2(child.contains(QLatin1String("cspmark=yes")),
            qPrintable(QStringLiteral(
                "sandbox allow-same-origin child could not read "
                "its own site cookie (got '%1')").arg(child)));
        QVERIFY2(child.contains(QLatin1String("cspw=set")),
            qPrintable(QStringLiteral(
                "sandbox allow-same-origin child could not write "
                "its own site cookie (got '%1')").arg(child)));
        QVERIFY2(m_server2->sawCookie(
                     QStringLiteral("/cookieprobe"), "cspmark=yes"),
            "cookieprobe request went out without the Cookie header");
        m_server2->seen.clear();
    }
}

void tst_Csp::sandboxCookieOpaqueOriginBlocked()
{
    m_server2->setHeaders(QStringLiteral("/child.html"),
        QByteArrayLiteral("Content-Security-Policy: "
                          "sandbox allow-scripts\r\n"));
    m_server2->setBody(QStringLiteral("/child.html"),
        QByteArrayLiteral(
            "<!doctype html><script>"
            "var c='unset';"
            "try{c=document.cookie;}catch(e){c='throw:'+e.name;}"
            "fetch('/cookieprobe').catch(function(){});"
            "setTimeout(function(){parent.postMessage('cookie:'+c,'*');},200);"
            "</script>"));

    m_server->setBody(QStringLiteral("/parent.html"),
        QByteArrayLiteral(
            "<!doctype html><script>"
            "window.__child='none';"
            "window.addEventListener('message',function(e){"
            " window.__child=String(e.data);});"
            "</script><iframe src=\"") +
        m_server2->url(QStringLiteral("/child.html")).toString().toUtf8() +
        QByteArrayLiteral("\"></iframe>"));

    for (QWebEngineProfile *profile : { m_bareProfile, m_appProfile }) {
        QSignalSpy spy(profile->cookieStore(),
                       &QWebEngineCookieStore::cookieAdded);
        profile->cookieStore()->setCookie(
            QNetworkCookie(QByteArrayLiteral("cspmark2"),
                           QByteArrayLiteral("yes")),
            QUrl(QStringLiteral("http://127.0.0.1:%1/")
                 .arg(m_server2->port())));
        spy.wait(2000);

        WebPage page(profile);
        QVERIFY(loadSync(&page,
            m_server->url(QStringLiteral("/parent.html"))));
        QVERIFY(waitFor([&page]() {
            return evalSync(&page, QLatin1String("String(window.__child)"))
                .toString() != QLatin1String("none");
        }));

        const QString child = evalSync(&page, QLatin1String(
            "String(window.__child)")).toString();
        QVERIFY2(!child.contains(QLatin1String("cspmark2")),
            qPrintable(QStringLiteral(
                "opaque-origin sandboxed child unexpectedly read the "
                "site cookie (got '%1')").arg(child)));
        QVERIFY2(!m_server2->sawCookie(
                     QStringLiteral("/cookieprobe"), "cspmark2"),
            "opaque-origin child's request carried the site Cookie");
        m_server2->seen.clear();
    }
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    SchemeAccessHandler::registerUrlSchemes();
    AdBlockSchemeAccessHandler::registerUrlScheme();
    AdBlockResourceHandler::registerUrlScheme();
    BrowserApplication app(argc, argv);
    tst_Csp tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_csp.moc"
