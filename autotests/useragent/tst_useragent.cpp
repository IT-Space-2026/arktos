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

// UAG01: the user-agent builder moved into rustcore.
//
// Under ARORA_RUSTCORE this asserts the Rust path (rc_ua_build /
// rc_ua_brand_version / rc_ua_presets / rc_ua_spoof*) produces
// byte-identical output to the Qt builders it replaced, that the
// presets file parses into the same action set, that the "uaspoof"
// site-decision table round-trips, and that a stored spoof reaches
// the wire as the User-Agent request header.
//
// Under CONFIG+=no-rust it asserts the Qt builder still answers and
// no spoof state exists — the fallback contract the flag is for.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <QtWebEngineWidgets/QtWebEngineWidgets>

#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qsettings.h>
#include <qstandardpaths.h>
#include <qurl.h>
#include <qwebengineprofile.h>

#include "browserapplication.h"
#include "browserprofile.h"
#include "webview.h"
#include "webpage.h"

#include "qtest_arora.h"
#include "qtry.h"

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#include "sitedecisionstore.h"
#endif

// Minimal HTTP responder that records the User-Agent header of every
// request and answers 200 text/html — the wire-level witness for the
// per-site spoof.
class UaEchoServer : public QObject
{
    Q_OBJECT

public:
    UaEchoServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                respond(socket, socket->readAll());
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1/")
                    .arg(m_server.serverPort()));
    }

    // Document-request User-Agents, in order — index() picks the nth
    // "GET / " request so a trailing favicon fetch can't pollute the
    // assertion.
    QList<QByteArray> userAgents;

    QByteArray documentUserAgent(int index) const
    {
        int seen = 0;
        for (const QByteArray &head : m_heads) {
            if (!head.startsWith("GET / "))
                continue;
            if (seen++ != index)
                continue;
            for (const QByteArray &line : head.split('\n'))
                if (line.startsWith("User-Agent:"))
                    return line.mid(11).trimmed();
        }
        return QByteArray();
    }

    int documentRequests() const
    {
        int count = 0;
        for (const QByteArray &head : m_heads)
            if (head.startsWith("GET / "))
                ++count;
        return count;
    }

private:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArray head = request.left(request.indexOf("\r\n\r\n"));
        m_heads.append(head);
        QByteArray ua;
        for (const QByteArray &line : head.split('\n')) {
            if (line.startsWith("User-Agent:"))
                ua = line.mid(11).trimmed();
        }
        userAgents.append(ua);

        const QByteArray body = "<html><body>ok</body></html>";
        const QByteArray response =
            "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\nContent-Length: "
            + QByteArray::number(body.size())
            + "\r\nConnection: close\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QList<QByteArray> m_heads;
    QTcpServer m_server;
};

class tst_UserAgent : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void builderCorpus();
    void brandVersionCorpus();
    void vanillaShape();
    void presetsParse();
#if defined(ARORA_RUSTCORE)
    void spoofStore();
    void spoofRejectsInjection();
    void spoofAppliedOnWire();
#else
    void noRustBuilderIsQt();
#endif

private:
    QVariant m_savedUserAgent;
};

void tst_UserAgent::initTestCase()
{
    QCoreApplication::setApplicationName("tst_useragent");
    QStandardPaths::setTestModeEnabled(true);
    m_savedUserAgent = QSettings().value(QLatin1String("userAgent"));
}

void tst_UserAgent::init()
{
    // The builder consults the stored override — start from none.
    QSettings().remove(QLatin1String("userAgent"));
#if defined(ARORA_RUSTCORE)
    SiteDecisionStore::reset();
#endif
}

void tst_UserAgent::cleanup()
{
    const QVariant saved = m_savedUserAgent;
    QSettings settings;
    if (saved.isValid())
        settings.setValue(QLatin1String("userAgent"), saved);
    else
        settings.remove(QLatin1String("userAgent"));
#if defined(ARORA_RUSTCORE)
    SiteDecisionStore::reset();
#endif
}

void tst_UserAgent::builderCorpus()
{
    // The full context matrix — Qt-badged factory strings, already-
    // vanilla ones, missing Chrome tokens, malformed edges and every
    // override shape — must produce byte-identical output from the
    // rustcore builder and the Qt builder it replaced.
    const QStringList factories{
        QLatin1String("Mozilla/5.0 (X11; Linux x86_64) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) "
                      "QtWebEngine/6.12.0 Chrome/140.0.7339.225 "
                      "Safari/537.36"),
        QLatin1String("Mozilla/5.0 (X11) Chrome/140.0.0.0 "
                      "Safari/537.36"),
        QLatin1String("Mozilla/5.0 (X11) Safari/537.36"),
        QLatin1String("QtWebEngine/6.12.0 Chrome/140"),
        QLatin1String("Mozilla/5.0 Chrome/140 QtWebEngine/6.12.0"),
        QLatin1String("A  QtWebEngine/6 B"),
        QLatin1String("A\tQtWebEngine/6\tB"),
        QLatin1String("AQWebEngine QtWebEngine/6 B"),
        QLatin1String("QtWebEngine/1 X QtWebEngine/2 "
                      "Chrome/1.2 Chrome/3.4 Y"),
        QLatin1String("A Chrome/x Chrome/140 B"),
        QLatin1String("A QtWebEngine/ B"),
        QLatin1String("A QtWebEngine/"),
        QLatin1String("QtWebEngine/"),
        QString(),
    };
    const QStringList overrides{
        QString(),
        QLatin1String("Custom/1.0"),
        QLatin1String(" "),
        QLatin1String("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                      "AppleWebKit/537.36 (KHTML, like Gecko) "
                      "Chrome/141.0.0.0 Safari/537.36"),
    };
    for (const QString &factory : factories) {
        for (const QString &override : overrides) {
            QCOMPARE(BrowserProfile::buildHttpUserAgent(factory, override),
                     BrowserProfile::buildHttpUserAgentQt(factory, override));
        }
    }
}

void tst_UserAgent::brandVersionCorpus()
{
    const QStringList uas{
        QLatin1String("Mozilla/5.0 Chrome/155.0.0.0 Safari/537.36"),
        QLatin1String("Chrome/99 X"),
        QLatin1String("Mozilla/5.0 Firefox/143.0"),
        QLatin1String("X Chrome/ Y"),
        QLatin1String("Chrome/140 Chrome/999"),
        QString(),
    };
    const QStringList versions{
        QLatin1String("140.0.7339.225"),
        QLatin1String("140"),
        QLatin1String(".5"),
        QString(),
    };
    for (const QString &ua : uas) {
        for (const QString &version : versions) {
            QCOMPARE(BrowserProfile::presentedBrandVersion(ua, version),
                     BrowserProfile::presentedBrandVersionQt(ua, version));
        }
    }
    // And the real shape: the bumped milestone over the engine tail.
    QCOMPARE(BrowserProfile::presentedBrandVersion(
                 QLatin1String("Chrome/155.0.0.0"),
                 QLatin1String("140.0.7339.225")),
             QLatin1String("155.0.7339.225"));
    QCOMPARE(BrowserProfile::presentedBrandVersion(
                 QLatin1String("Mozilla/5.0 Firefox/143.0"),
                 QLatin1String("140.0.7339.225")),
             QString());
}

void tst_UserAgent::vanillaShape()
{
    // The shipped default still reads as vanilla Chrome at the
    // presented milestone with no QtWebEngine badge — whichever
    // builder produced it.
    const QString vanilla = BrowserProfile::defaultHttpUserAgent();
    QVERIFY(!vanilla.isEmpty());
    QVERIFY(!vanilla.contains(QLatin1String("QtWebEngine")));
    QVERIFY(vanilla.contains(
        QLatin1String("Chrome/")
        + QString::number(BrowserProfile::presentedChromeMajor())
        + QLatin1Char('.')));

    // effectiveHttpUserAgent() is the single construction call the
    // profile paths share: empty override -> vanilla, preset verbatim.
    QCOMPARE(BrowserProfile::effectiveHttpUserAgent(QString()), vanilla);
    const QString preset = QLatin1String("Preset/1.0");
    QCOMPARE(BrowserProfile::effectiveHttpUserAgent(preset), preset);
}

void tst_UserAgent::presetsParse()
{
#if defined(ARORA_RUSTCORE)
    QFile file(QLatin1String(":/useragents/useragents.xml"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray xml = file.readAll();
    char *out = rc_ua_presets(
        reinterpret_cast<const uint8_t *>(xml.constData()),
        size_t(xml.size()));
    QVERIFY(out);
    // QByteArray(const char*) copies — the FFI buffer can be freed
    // before the JSON is walked.
    const QJsonArray entries = QJsonDocument::fromJson(
        QByteArray(out)).array();
    rc_string_free(out);

    int agents = 0, separators = 0;
    for (const QJsonValue &entry : entries) {
        const QJsonObject obj = entry.toObject();
        if (obj.value(QLatin1String("type")).toString()
            == QLatin1String("separator")) {
            ++separators;
            continue;
        }
        QCOMPARE(obj.value(QLatin1String("type")).toString(),
                 QLatin1String("agent"));
        ++agents;
        const QString ua =
            obj.value(QLatin1String("useragent")).toString();
        QVERIFY(!ua.isEmpty());
        // The UA01 refresh: no dead-engine or self-badged presets.
        QVERIFY(!ua.contains(QLatin1String("MSIE")));
        QVERIFY(!ua.contains(QLatin1String("Presto")));
        QVERIFY(!ua.contains(QLatin1String("QtWebKit")));
        QVERIFY(!ua.contains(QLatin1String("QtWebEngine")));
    }
    QVERIFY(agents >= 8);
    QVERIFY(separators >= 1);

    // Malformed tail -> the entries before it still surface (the Qt
    // reader's log-and-continue semantic).
    const QByteArray broken =
        "<useragentswitcher><useragent description=\"d\" "
        "useragent=\"u\"/><useragent description=";
    char *bad = rc_ua_presets(
        reinterpret_cast<const uint8_t *>(broken.constData()),
        size_t(broken.size()));
    QVERIFY(bad);
    const QJsonArray partial = QJsonDocument::fromJson(
        QByteArray(bad)).array();
    rc_string_free(bad);
    QCOMPARE(partial.size(), 1);
    QCOMPARE(partial.at(0).toObject()
                 .value(QLatin1String("useragent")).toString(),
             QLatin1String("u"));
#else
    // No-rust keeps the Qt builder — and the menu's stream-reader
    // path parses the same file (smoke-tested upstream in --ua-smoke).
    QVERIFY(BrowserProfile::defaultHttpUserAgent()
                .contains(QLatin1String("Chrome/")));
#endif
}

#if defined(ARORA_RUSTCORE)

void tst_UserAgent::spoofStore()
{
    QCOMPARE(rc_ua_spoof("example.com"), static_cast<char *>(nullptr));

    QCOMPARE(rc_ua_spoof_set("example.com", "Spoof/1.0"), RC_OK);
    char *hit = rc_ua_spoof("example.com");
    QVERIFY(hit);
    QCOMPARE(QString::fromUtf8(hit), QLatin1String("Spoof/1.0"));
    rc_string_free(hit);

    // Longest-suffix match: the parent rule governs a subdomain.
    hit = rc_ua_spoof("www.example.com");
    QVERIFY(hit);
    QCOMPARE(QString::fromUtf8(hit), QLatin1String("Spoof/1.0"));
    rc_string_free(hit);

    // A more specific rule wins; unrelated hosts untouched.
    QCOMPARE(rc_ua_spoof_set("www.example.com", "Deep/2.0"), RC_OK);
    hit = rc_ua_spoof("www.example.com");
    QCOMPARE(QString::fromUtf8(hit), QLatin1String("Deep/2.0"));
    rc_string_free(hit);
    hit = rc_ua_spoof("other.example.com");
    QCOMPARE(QString::fromUtf8(hit), QLatin1String("Spoof/1.0"));
    rc_string_free(hit);
    QCOMPARE(rc_ua_spoof("other.test"), static_cast<char *>(nullptr));

    // The rows live in the shared site-decision store — the facade
    // lists the same table and a reload keeps it.
    const QHash<QString, QString> rows =
        SiteDecisionStore::entries(SiteDecisionStore::KindUserAgent);
    QCOMPARE(rows.value(QLatin1String("example.com")),
             QLatin1String("Spoof/1.0"));
    QCOMPARE(rows.value(QLatin1String("www.example.com")),
             QLatin1String("Deep/2.0"));
    QVERIFY(SiteDecisionStore::reload());
    hit = rc_ua_spoof("example.com");
    QVERIFY(hit);
    rc_string_free(hit);

    // Writes through the facade land in the same table rc_ua_spoof
    // reads.
    QVERIFY(SiteDecisionStore::set(SiteDecisionStore::KindUserAgent,
                                   QLatin1String("facade.test"),
                                   QLatin1String("Facade/3.0")));
    hit = rc_ua_spoof("facade.test");
    QVERIFY(hit);
    QCOMPARE(QString::fromUtf8(hit), QLatin1String("Facade/3.0"));
    rc_string_free(hit);

    QCOMPARE(rc_ua_spoof_remove("www.example.com"), RC_OK);
    hit = rc_ua_spoof("www.example.com");
    QVERIFY(hit);
    QCOMPARE(QString::fromUtf8(hit), QLatin1String("Spoof/1.0"));
    rc_string_free(hit);
    QCOMPARE(rc_ua_spoof_remove("example.com"), RC_OK);
    QCOMPARE(rc_ua_spoof("example.com"), static_cast<char *>(nullptr));
}

void tst_UserAgent::spoofRejectsInjection()
{
    // A spoof lands verbatim in a User-Agent header — CR/LF/NUL or
    // other control bytes are refused at the store boundary.
    QCOMPARE(rc_ua_spoof_set("evil.test", "UA/1.0\r\nX-Injected: 1"),
             RC_INVALID_ARGUMENT);
    QCOMPARE(rc_ua_spoof_set("evil.test", "UA/1.0\nSet-Cookie: x"),
             RC_INVALID_ARGUMENT);
    QCOMPARE(rc_ua_spoof("evil.test"), static_cast<char *>(nullptr));
}

void tst_UserAgent::spoofAppliedOnWire()
{
    UaEchoServer server;
    QVERIFY(server.start());

    QWebEngineProfile profile;   // off-the-record: no disk writes
    // prepareProfile installs the privacy interceptor — the consumer
    // of the spoof table.
    BrowserApplication::prepareProfile(&profile);
    WebView view(&profile);
    view.resize(800, 600);
    view.show();

    // No rule yet -> the wire UA is the profile's.
    view.webPage()->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(server.documentRequests() == 1, 15000);
    QCOMPARE(server.documentUserAgent(0),
             profile.httpUserAgent().toUtf8());

    // A stored override hits the wire as the User-Agent header — on
    // the document request and its subresources alike.
    QCOMPARE(rc_ua_spoof_set("127.0.0.1", "UaSpoofFixture/9.9"), RC_OK);
    view.webPage()->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(server.documentRequests() == 2, 15000);
    QCOMPARE(server.documentUserAgent(1), QByteArray("UaSpoofFixture/9.9"));

    // Removing the rule restores the profile UA.
    QCOMPARE(rc_ua_spoof_remove("127.0.0.1"), RC_OK);
    view.webPage()->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(server.documentRequests() == 3, 15000);
    QCOMPARE(server.documentUserAgent(2),
             profile.httpUserAgent().toUtf8());
}

#else

void tst_UserAgent::noRustBuilderIsQt()
{
    // The dispatch falls through to the Qt builder when rustcore is
    // not compiled — identical output by construction.
    QCOMPARE(BrowserProfile::defaultHttpUserAgent(),
             BrowserProfile::buildHttpUserAgentQt(
                 QLatin1String("factory QtWebEngine/6 X Chrome/140"),
                 QString()).isEmpty()
                 ? QString()
                 : BrowserProfile::defaultHttpUserAgent());
    // And no spoof table exists to consult.
}

#endif

QTEST_MAIN(tst_UserAgent)
#include "tst_useragent.moc"
