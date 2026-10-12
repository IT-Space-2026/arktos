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

// COV10: end-to-end coverage for TorRequestInterceptor — a profile's
// QWebEngineUrlRequestInterceptor runs on Chromium's IO thread, so
// the only honest exercise is a real page fetch through a real
// profile (same approach as tst_adblockrequestinterceptor).  A
// loopback HTTP responder records every request that actually reaches
// the wire and aborts on TLS bytes, which is what lets the http->https
// upgrade be observed: the plain-HTTP server sees nothing while the
// upgraded hop fails in the handshake instead of stalling.
//
// The verdicts themselves live in Engine::NavigationPolicy and are
// pinned exhaustively in tst_navigationpolicy — this suite exists to
// cover the adapter: request marshaling, the script-type snapshot
// branch, and each verdict arm (Block / Redirect / Allow + Referer
// rewrite / Pass).

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporaryfile.h>
#include <qwebengineloadinginfo.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <functional>
#include <memory>

#include "adblockmanager.h"
#include "browserapplication.h"
#include "navigationpolicy.h"
#include "schemeaccesshandler.h"
#include "scriptcontrolmanager.h"
#include "torrequestinterceptor.h"
#include "qtest_arora.h"
#include "qtry.h"

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

using Engine::NavigationPolicy;

// Minimal HTTP/1.0 responder: records every request target it sees
// and answers 200 with a canned body.  Bytes that do not open like an
// HTTP method (a TLS ClientHello — where an upgraded request lands)
// get an immediate abort so Chromium fails the hop fast.
class LocalHttpServer : public QObject
{
    Q_OBJECT

public:
    LocalHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this,
                [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                const QByteArray peeked = socket->peek(4096);
                if (peeked.isEmpty()
                        || peeked.at(0) < 'A' || peeked.at(0) > 'Z') {
                    socket->abort();
                    return;
                }
                // Wait for the full header block before answering.
                if (!peeked.contains("\r\n\r\n"))
                    return;
                respond(socket, socket->readAll());
            });
        });
    }

    bool start()
    {
        return m_server.listen(QHostAddress::LocalHost);
    }

    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1%2")
                    .arg(m_server.serverPort()).arg(path));
    }

    QStringList requests;
    QByteArray indexHtml;

private:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArray target = request.split(' ').value(1);
        requests.append(QString::fromUtf8(target));

        QByteArray mimeType = "text/plain";
        QByteArray body = "ok\n";
        if (target == "/" || target.endsWith(".html")) {
            mimeType = "text/html";
            body = indexHtml;
        }

        const QByteArray response =
            QByteArrayLiteral("HTTP/1.0 200 OK\r\nContent-Type: ")
            + mimeType
            + QByteArrayLiteral("\r\nContent-Length: ")
            + QByteArray::number(body.size())
            + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
            + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
};

// Spins the event loop until flag flips or the deadline passes —
// QTest's QTRY_* macros can't live in helpers that return a value.
static bool waitFor(const std::shared_ptr<bool> &flag, int timeout = 15000)
{
    for (int waited = 0; !*flag && waited < timeout; waited += 50)
        QTest::qWait(50);
    return *flag;
}

// Waits for loadFinished regardless of its ok argument; the trigger
// runs after the connection is armed (setHtml or load).
static bool loadSettled(QWebEnginePage *page,
                        const std::function<void()> &trigger)
{
    std::shared_ptr<bool> done(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadFinished, page,
        [done](bool) { *done = true; });
    trigger();
    const bool fired = waitFor(done);
    QObject::disconnect(connection);
    return fired;
}

// A refused/errored navigation is detected through
// loadingChanged(LoadFailed) — plain QWebEnginePage has no not-found
// page to report a deceptive loadFinished(true).
static bool loadAndFail(QWebEnginePage *page, const QUrl &url)
{
    std::shared_ptr<bool> failed(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadingChanged, page,
        [failed](const QWebEngineLoadingInfo &info) {
        if (info.status() == QWebEngineLoadingInfo::LoadFailedStatus)
            *failed = true;
    });
    page->load(url);
    waitFor(failed);
    QObject::disconnect(connection);
    return *failed;
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
    waitFor(done);
    return *result;
}

class tst_TorInterceptor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void clearnetHttpUpgrade();
    void httpSubresourcesUpgrade();
    void onionHttpNotUpgraded();
    void refererRewriteOnAllow();
    void scriptBlockedAtSafer();
    void domainBlockBeatsUpgrade();
    void nonWebSchemePasses();

private:
    QWebEngineProfile *m_profile;
    LocalHttpServer *m_server;
};

void tst_TorInterceptor::initTestCase()
{
    QCoreApplication::setApplicationName("tst_torinterceptor");
    QStandardPaths::setTestModeEnabled(true);

    QSettings settings;
    settings.clear();
    // Dead local list keeps load() away from the live defaults —
    // same trick as tst_adblockrequestinterceptor.
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-cov10.txt"
            "&title=DeadList"));
    AdBlockManager::instance()->setEnabled(true);

    m_server = new LocalHttpServer(this);
    QVERIFY(m_server->start());

    // The interceptor under test on a throwaway OTR profile — the
    // production code installs it on the dedicated tor profile in
    // BrowserApplication; wiring it here reaches the same
    // interceptRequest without needing a live tor daemon.
    m_profile = new QWebEngineProfile(this);
    m_profile->setUrlRequestInterceptor(new TorRequestInterceptor(
        AdBlockManager::instance()->network(), m_profile));
}

void tst_TorInterceptor::init()
{
    m_server->requests.clear();
    m_server->indexHtml.clear();
    // Policy is process-global — pin a deterministic snapshot every
    // leg so the developer machine's real settings (or a previous
    // leg's mutation) can't bleed through.
    NavigationPolicy::loadSnapshot(NavigationPolicy::Snapshot());
    ScriptControlManager::instance()->clearSessionRules();
}

// The core TOR02 guarantee: ANY clearnet http request is redirected
// to https before the wire.  The plain-HTTP fixture never sees the
// request, and the rewritten https hop dies in the TLS handshake —
// the server aborts on non-HTTP bytes.
void tst_TorInterceptor::clearnetHttpUpgrade()
{
    QWebEnginePage page(m_profile);
    QVERIFY(loadAndFail(&page, m_server->url(QLatin1String("/index.html"))));
    QVERIFY(m_server->requests.isEmpty());
}

// The upgrade is not main-frame only: img and script subresources on
// an http document get the same rewrite — which additionally drives
// the adapter's ResourceTypeScript branch (the scriptAllowed
// snapshot lookup) before the policy even runs.
void tst_TorInterceptor::httpSubresourcesUpgrade()
{
    QWebEnginePage page(m_profile);
    QVERIFY(loadSettled(&page, [this, &page]() {
        page.setHtml(QStringLiteral(
            "<html><body>"
            "<img src=\"/sub.png\">"
            "<script src=\"/sub.js\"></script>"
            "</body></html>"),
            m_server->url(QLatin1String("/")));
    }));
    QVERIFY(m_server->requests.isEmpty());
}

// .onion hosts are exempt from the upgrade (onion services are
// end-to-end encrypted and many serve http only): the request is
// allowed through to the network untouched — where it can only fail
// at resolution inside a tor-less test run.
void tst_TorInterceptor::onionHttpNotUpgraded()
{
    QWebEnginePage page(m_profile);
    QVERIFY(loadAndFail(&page,
        QUrl(QStringLiteral("http://cov10-fixture.onion/"))));
    QVERIFY(m_server->requests.isEmpty());
}

// Allow + Referer rewrite: a cross-origin https subresource on an
// http document ends in the Allow verdict, whose trimmed Referer
// floor writes the source origin back via info.setHttpHeader.
void tst_TorInterceptor::refererRewriteOnAllow()
{
    QWebEnginePage page(m_profile);
    QVERIFY(loadSettled(&page, [&page]() {
        page.setHtml(QStringLiteral(
            "<html><body>"
            "<img src=\"https://cov10-ref.invalid/x.png\">"
            "</body></html>"),
            QUrl(QStringLiteral("http://cov10-firstparty.test/")));
    }));
    // The fetch was allowed through and failed at resolution — the
    // element never decoded a pixel.
    QCOMPARE(evalSync(&page, QStringLiteral(
        "String(document.images[0].naturalWidth)")).toString(),
        QLatin1String("0"));
}

// Block verdict: at Safer the script gate refuses script fetches on
// non-loopback http documents — the adapter's info.block(true) arm.
// An https target is used deliberately so it is the script gate, not
// the upgrade, that stops the request.
void tst_TorInterceptor::scriptBlockedAtSafer()
{
    NavigationPolicy::Snapshot snapshot;
    snapshot.securityLevel = 1;  // Safer
    NavigationPolicy::loadSnapshot(snapshot);

    QWebEnginePage page(m_profile);
    QVERIFY(loadSettled(&page, [&page]() {
        page.setHtml(QStringLiteral(
            "<html><body>"
            "<script src=\"https://cov10-script.invalid/x.js\" "
            "onload=\"window.__scr='ok'\" "
            "onerror=\"window.__scr='err'\"></script>"
            "</body></html>"),
            QUrl(QStringLiteral("http://cov10-firstparty.test/")));
    }));
    QCOMPARE(evalSync(&page, QStringLiteral(
        "String(window.__scr || 'none')")).toString(),
        QLatin1String("err"));
}

// A blocklisted main frame is refused BEFORE the upgrade stage —
// takeBlockedDomainNav is the consume-once observable that separates
// the Block verdict from a redirect (an upgrade never records a
// blocked-domain nav).  The list lives in rustcore, so the leg is
// absent in no-rust builds where shouldBlockDomain always passes.
void tst_TorInterceptor::domainBlockBeatsUpgrade()
{
#if defined(ARORA_RUSTCORE)
    const QByteArray fixture = "# tst_torinterceptor\n"
        "torcov-blocked.test\n";
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(fixture.constData()),
                 size_t(fixture.size())),
             RC_OK);
    const QUrl url(QStringLiteral("http://torcov-blocked.test/"));
    QWebEnginePage page(m_profile);
    QVERIFY(loadAndFail(&page, url));
    QVERIFY(NavigationPolicy::takeBlockedDomainNav(url));
#endif
}

// Non-web schemes never reach the policy — the Pass arm leaves the
// request untouched and a local file still loads.
void tst_TorInterceptor::nonWebSchemePasses()
{
    QTemporaryFile file(QDir::tempPath()
        + QLatin1String("/arora-torcov-XXXXXX.txt"));
    QVERIFY(file.open());
    file.write("arora tor interceptor fixture\n");
    file.flush();

    QWebEnginePage page(m_profile);
    QVERIFY(loadSettled(&page, [&page, &file]() {
        page.load(QUrl::fromLocalFile(file.fileName()));
    }));
    QVERIFY(evalSync(&page, QStringLiteral(
        "document.body.innerText")).toString().contains(
            QLatin1String("tor interceptor fixture")));
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    // arora-* schemes must be registered before the browsing profile
    // spins up (same order as main.cpp).
    SchemeAccessHandler::registerUrlSchemes();
    BrowserApplication app(argc, argv);
    tst_TorInterceptor tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_torinterceptor.moc"
