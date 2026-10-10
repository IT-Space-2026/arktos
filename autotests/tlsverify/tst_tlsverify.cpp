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

// SEC22: second-opinion TLS verification — the rc_tls_check FFI
// verdict matrix (verified / warning classes / unverified / refused),
// the TlsVerifier gate rules (scheme, private hosts, app proxy, tor
// mode) and the async probe->statusChanged->statusFor round trip.
//
// Fixture material lives in src/rustcore/data/testcerts (DER, built by
// gen_testcerts.py): a scratch CA the test installs via
// rc_tls_add_root plus CA-signed leaves exercising every error class.
// The loopback fixture server is a QSslSocket holding each leaf in
// turn.  Under a no-rust build the FFI is absent — the tests then pin
// "nothing is ever probed" instead of skipping outright.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>

#include "browserapplication.h"
#include "tlsverifier.h"
#include "qtest_arora.h"
#include "qtry.h"

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

static QString certDir()
{
    return QCoreApplication::applicationDirPath()
        + QLatin1String("/../../src/rustcore/data/testcerts");
}

static QByteArray certFile(const QString &name)
{
    QFile file(certDir() + QLatin1Char('/') + name);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.readAll();
}

// The TLS peer the probe handshakes with: openssl s_server holding a
// fixture leaf.  A real OpenSSL server is the right peer for a
// second-opinion verifier — an in-process Qt fixture would also
// deadlock here, since the synchronous FFI call blocks the event loop
// the fixture would live on.
class TlsFixture
{
public:
    ~TlsFixture()
    {
        m_proc.kill();
        m_proc.waitForFinished(2000);
    }

    // base is the fixture name ("valid", "expired", ...) — cert/key
    // PEMs sit in src/rustcore/data/testcerts.
    bool start(const QString &base)
    {
        const QString cert =
            certDir() + QLatin1Char('/') + base + QLatin1String(".pem");
        const QString key = certDir() + QLatin1Char('/') + base
            + QLatin1String("-key.pem");
        if (!QFile::exists(cert) || !QFile::exists(key))
            return false;
        quint16 candidate = freePort();
        m_proc.setProgram(QStringLiteral("openssl"));
        m_proc.setArguments({
            QStringLiteral("s_server"),
            QStringLiteral("-accept"),
            QLatin1String("127.0.0.1:") + QString::number(candidate),
            QStringLiteral("-cert"), cert,
            QStringLiteral("-key"), key,
            QStringLiteral("-alpn"), QStringLiteral("http/1.1"),
            QStringLiteral("-quiet"),
        });
        m_proc.start();
        if (!m_proc.waitForStarted(5000))
            return false;
        // Wait for the listener: a bare TCP connect drops the attempt
        // but s_server keeps accepting.
        for (int i = 0; i < 50; ++i) {
            QTcpSocket probe;
            probe.connectToHost(QHostAddress::LocalHost, candidate);
            if (probe.waitForConnected(200)) {
                probe.disconnectFromHost();
                m_port = candidate;
                return true;
            }
            QTest::qWait(50);
        }
        return false;
    }

    quint16 port() const { return m_port; }

private:
    static quint16 freePort()
    {
        QTcpServer server;
        server.listen(QHostAddress::LocalHost);
        return server.serverPort();
    }

    QProcess m_proc;
    quint16 m_port = 0;
};

#if defined(ARORA_RUSTCORE)
// Synchronous FFI probe — the verdict JSON parsed into an object.
static QJsonObject ffiCheck(const char *host, quint16 port, quint32 flags)
{
    char *json = rc_tls_check(host, port, flags);
    if (!json)
        return QJsonObject();
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray(json));
    rc_string_free(json);
    return doc.object();
}
#endif

class tst_TlsVerify : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();

    void ffiVerdictMatrix();
    void ffiRefusesPrivateHosts();
    void ffiNetworkFailureIsUnverified();
    void probeUrlGates();
    void asyncProbeVerified();
    void asyncProbeWarning();
    void asyncProbeUnverified();

private:
    QNetworkProxy m_savedProxy;
};

void tst_TlsVerify::initTestCase()
{
    QCoreApplication::setApplicationName(QLatin1String("tst_tlsverify"));
    QStandardPaths::setTestModeEnabled(true);
    m_savedProxy = QNetworkProxy::applicationProxy();
#if defined(ARORA_RUSTCORE)
    const QByteArray ca = certFile(QLatin1String("ca.der"));
    QVERIFY2(!ca.isEmpty(),
             "fixture CA missing — run data/testcerts/gen_testcerts.py");
    QCOMPARE(rc_tls_add_root(
                 reinterpret_cast<const uint8_t *>(ca.constData()),
                 size_t(ca.size())),
             RC_OK);
#endif
}

void tst_TlsVerify::cleanupTestCase()
{
#if defined(ARORA_RUSTCORE)
    rc_tls_clear_roots();
#endif
}

void tst_TlsVerify::cleanup()
{
    BrowserApplication::setTorMode(false);
    QNetworkProxy::setApplicationProxy(m_savedProxy);
}

void tst_TlsVerify::ffiVerdictMatrix()
{
#if defined(ARORA_RUSTCORE)
    struct Row {
        const char *base;
        const char *status;
        const char *errorClass;
    };
    const Row rows[] = {
        { "valid", "verified", nullptr },
        { "expired", "warning", "expired" },
        { "notyet", "warning", "not-yet-valid" },
        { "wronghost", "warning", "bad-hostname" },
        { "selfsigned", "warning", "untrusted-root" },
    };
    for (const Row &row : rows) {
        TlsFixture server;
        QVERIFY2(server.start(QLatin1String(row.base)), row.base);
        const QJsonObject v =
            ffiCheck("localhost", server.port(), RC_TLS_F_ALLOW_LOCAL);
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String(row.status));
        if (row.errorClass)
            QCOMPARE(v.value(QLatin1String("error_class")).toString(),
                     QLatin1String(row.errorClass));
        if (QLatin1String(row.status) == QLatin1String("verified")) {
            // The wire chain is summarized: leaf carries the SAN the
            // verifier matched.
            const QJsonArray chain =
                v.value(QLatin1String("chain")).toArray();
            QCOMPARE(chain.size(), 1);
            bool sawLocalhost = false;
            for (const QJsonValue &san :
                 chain.first()
                     .toObject()
                     .value(QLatin1String("sans"))
                     .toArray())
                sawLocalhost |= san.toString() == QLatin1String("localhost");
            QVERIFY(sawLocalhost);
            QVERIFY(v.value(QLatin1String("negotiated"))
                        .toObject()
                        .value(QLatin1String("cipher_suite"))
                        .isString());
        }
        // Honesty marker, always present.
        QCOMPARE(v.value(QLatin1String("revocation")).toString(),
                 QLatin1String("not-checked"));
    }
#endif
}

void tst_TlsVerify::ffiRefusesPrivateHosts()
{
#if defined(ARORA_RUSTCORE)
    // Without RC_TLS_F_ALLOW_LOCAL every private/loopback name is a
    // refusal — a probe must never become an intranet scanner.
    for (const char *host :
         { "localhost", "127.0.0.1", "10.1.2.3", "192.168.4.4",
           "printer.local", "service.onion", "[::1]" }) {
        const QJsonObject v = ffiCheck(host, 443, 0);
        QCOMPARE(v.value(QLatin1String("status")).toString(),
                 QLatin1String("refused"));
        QCOMPARE(v.value(QLatin1String("error_class")).toString(),
                 QLatin1String("private-host"));
    }
    // …and the flag exists precisely to override that for fixtures.
    const QJsonObject v = ffiCheck("localhost", 1, RC_TLS_F_ALLOW_LOCAL);
    QCOMPARE(v.value(QLatin1String("status")).toString(),
             QLatin1String("unverified"));
#endif
}

void tst_TlsVerify::ffiNetworkFailureIsUnverified()
{
#if defined(ARORA_RUSTCORE)
    // A refused connection never evaluated a chain — the verdict is
    // "unverified", not "warning".
    quint16 port;
    {
        QTcpServer tmp;
        QVERIFY(tmp.listen(QHostAddress::LocalHost));
        port = tmp.serverPort();
    }
    const QJsonObject v =
        ffiCheck("localhost", port, RC_TLS_F_ALLOW_LOCAL);
    QCOMPARE(v.value(QLatin1String("status")).toString(),
             QLatin1String("unverified"));
    QCOMPARE(v.value(QLatin1String("error_class")).toString(),
             QLatin1String("network"));
#endif
}

void tst_TlsVerify::probeUrlGates()
{
    TlsVerifier *verifier = TlsVerifier::instance();

    // Only https main-frame commits are probe candidates.
    verifier->probeUrl(QUrl(QLatin1String("http://gate-http.example/")));
    verifier->probeUrl(QUrl(QLatin1String("ftp://gate-ftp.example/f")));
    verifier->probeUrl(QUrl(QLatin1String("file:///etc/passwd")));
    QCOMPARE(verifier->statusFor(QLatin1String("gate-http.example")),
             TlsVerifier::Status::None);
    QCOMPARE(verifier->statusFor(QLatin1String("gate-ftp.example")),
             TlsVerifier::Status::None);

    // Private/loopback/LAN targets are never probed — same rule the
    // interceptors enforce (the crate refuses too, but the Qt gate
    // must never issue them in the first place).
    verifier->probeUrl(QUrl(QLatin1String("https://localhost/")));
    verifier->probeUrl(QUrl(QLatin1String("https://127.0.0.1/")));
    verifier->probeUrl(QUrl(QLatin1String("https://192.168.33.4/")));
    verifier->probeUrl(QUrl(QLatin1String("https://hidden.onion/")));
    QCOMPARE(verifier->statusFor(QLatin1String("localhost")),
             TlsVerifier::Status::None);
    QCOMPARE(verifier->statusFor(QLatin1String("127.0.0.1")),
             TlsVerifier::Status::None);
    QCOMPARE(verifier->statusFor(QLatin1String("192.168.33.4")),
             TlsVerifier::Status::None);
    QCOMPARE(verifier->statusFor(QLatin1String("hidden.onion")),
             TlsVerifier::Status::None);

    // Application proxy set → no direct handshake.  The probe cannot
    // ride SOCKS/HTTP-CONNECT, and bypassing the user's route would
    // leak the destination.
    QNetworkProxy::setApplicationProxy(QNetworkProxy(
        QNetworkProxy::Socks5Proxy, QLatin1String("127.0.0.1"), 9050));
    verifier->probeUrl(QUrl(QLatin1String("https://proxied.example/")));
    QCOMPARE(verifier->statusFor(QLatin1String("proxied.example")),
             TlsVerifier::Status::None);
    QNetworkProxy::setApplicationProxy(QNetworkProxy::NoProxy);

    // Tor hard rule: a tor window must never produce a direct clearnet
    // handshake from this process (de-anonymization class).
    BrowserApplication::setTorMode(true);
    verifier->probeUrl(QUrl(QLatin1String("https://tor-mode.example/")));
    QCOMPARE(verifier->statusFor(QLatin1String("tor-mode.example")),
             TlsVerifier::Status::None);
    BrowserApplication::setTorMode(false);
}

void tst_TlsVerify::asyncProbeVerified()
{
#if defined(ARORA_RUSTCORE)
    TlsFixture server;
    QVERIFY(server.start(QLatin1String("valid")));
    TlsVerifier *verifier = TlsVerifier::instance();
    QSignalSpy spy(verifier, &TlsVerifier::statusChanged);
    verifier->probeHost(QLatin1String("localhost"), server.port(),
                        RC_TLS_F_ALLOW_LOCAL);
    QTRY_COMPARE(verifier->statusFor(QLatin1String("localhost"),
                                     server.port()),
                 TlsVerifier::Status::Verified);
    QVERIFY(spy.count() >= 1);
    QVERIFY(verifier->errorClass(QLatin1String("localhost"),
                               server.port())
                .isEmpty());
    QVERIFY(!verifier->tlsVersion(QLatin1String("localhost"),
                                server.port())
                 .isEmpty());
#else
    // No crate, no probe — the gate path itself stays inert.
    TlsVerifier::instance()->probeHost(QLatin1String("localhost"), 443, 0);
    QCOMPARE(TlsVerifier::instance()->statusFor(QLatin1String("localhost")),
             TlsVerifier::Status::None);
#endif
}

void tst_TlsVerify::asyncProbeWarning()
{
#if defined(ARORA_RUSTCORE)
    // An evaluated-and-failed chain surfaces as Warning with the
    // class the panel prints — this is the "TLS WARNING: <class>"
    // chip path.
    TlsFixture server;
    QVERIFY(server.start(QLatin1String("expired")));
    TlsVerifier *verifier = TlsVerifier::instance();
    verifier->probeHost(QLatin1String("localhost"), server.port(),
                        RC_TLS_F_ALLOW_LOCAL);
    QTRY_COMPARE(verifier->statusFor(QLatin1String("localhost"),
                                     server.port()),
                 TlsVerifier::Status::Warning);
    QCOMPARE(verifier->errorClass(QLatin1String("localhost"),
                                server.port()),
             QLatin1String("expired"));
#endif
}

void tst_TlsVerify::asyncProbeUnverified()
{
#if defined(ARORA_RUSTCORE)
    // Probe failure → Unverified, NEVER Warning: a network hiccup is
    // not a bad chain.
    quint16 port;
    {
        QTcpServer tmp;
        QVERIFY(tmp.listen(QHostAddress::LocalHost));
        port = tmp.serverPort();
    }
    TlsVerifier::instance()->probeHost(QLatin1String("localhost"), port,
                                       RC_TLS_F_ALLOW_LOCAL);
    QTRY_COMPARE(
        TlsVerifier::instance()->statusFor(QLatin1String("localhost"),
                                           port),
        TlsVerifier::Status::Unverified);
#endif
}

QTEST_MAIN(tst_TlsVerify)
#include "tst_tlsverify.moc"
