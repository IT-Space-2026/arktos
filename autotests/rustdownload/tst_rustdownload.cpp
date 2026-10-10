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

// DLACC06: card-level verification of the rustdl engine — the same
// local ranged-fixture pattern as the crate's own tests, driven
// through RustDownloadEngine and a real DownloadItem card.  Builds
// under every CONFIG: without rustdl the binary still exists and
// reports a skip so the suite stays green on no-Rust machines.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>

#include "downloadmanager.h"
#include "qtry.h"

#ifdef ARORA_RUSTDL
#include "rustdownloadengine.h"

#include <adblockmanager.h>
#include <adblocknetwork.h>
#include <adblockrule.h>
#include <adblocksubscription.h>
#include <browserapplication.h>
#include <browserpaths.h>
#include <bwrapgenerator.h>
#include <cookiejar.h>
#include <privacyrequestinterceptor.h>
#include <sandboxmanager.h>

#include <qelapsedtimer.h>
#include <qprocess.h>
#include <qwebengineprofile.h>
#include <qwebenginecookiestore.h>

#if defined(ARORA_RUSTCORE)
#include "rustcore.h"
#include "sitedecisionstore.h"
#endif
#endif

class tst_RustDownload : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void availability();
    void selectionSetting();

#ifdef ARORA_RUSTDL
    void engineCompletes();
    void engineSingleStream();
    void engineCancel();
    void engineRedirect();
    void engineTraversalName();
    void cardCompletes();
    void cardCancelRetry();

    // DLACC05: secrets + filesystem hygiene — the scoped cookie
    // export's scope/perms/lifetime, and that cancel leaves no
    // secrets or staging behind.
    void engineCookieExport();
    void engineCancelCleansSecrets();

    // DLACC04: the policy gate's pure decisions — the verdicts the
    // Rust engine asks for on every hop.  (The crate-side fixture
    // matrix proves the hops are re-gated, cookies stripped, proxies
    // honored; these pin the Qt side's verdicts.)
    void gateSchemeRefused();
    void gateHttpsFirstUpgrade();
    void gateHttpsOnlyBlocks();
    void gatePrivateExempt();
    void gateSsrfRedirect();
    void gateAdblockBlocks();
    void canHandleTorProxy();
#if defined(ARORA_RUSTCORE)
    void gateUrlStrip();
    void gateDomainBlocklist();
#endif

    // SAND02: the confined --download-worker subprocess — protocol
    // end-to-end through the real bwrap wrap + the engine's
    // subprocess/fallback selection.
    void workerDownloadE2E();
    void workerConfinesFilesystem();
    void workerGateBridgeBlocks();
    void workerCancelViaStdin();
    void engineRunsSandboxed();
    void engineFallsBackInProcess();
#else
    void noRustBuild();
#endif
};

void tst_RustDownload::initTestCase()
{
    QCoreApplication::setApplicationName("rustdownloadtest");
}

void tst_RustDownload::init()
{
    QSettings settings;
    settings.clear();
#ifdef ARORA_RUSTDL
    // SAND02: keep the FFI fixture suite on the in-process path —
    // the subprocess backend opts in explicitly inside its own tests
    // so both engines get covered deterministically.
    settings.setValue(QLatin1String("downloadmanager/sandboxedWorker"),
                      false);
    // The gate consults the interceptor's lock-guarded snapshot —
    // reload it against the cleared settings so defaults apply.
    PrivacyRequestInterceptor::loadSettings();
#endif
#if defined(ARORA_RUSTCORE)
    // The gate's adblock step consults the decision-store whitelist —
    // a stale row must not leak between tests.
    SiteDecisionStore::clear(SiteDecisionStore::KindAdBlock);
#endif
}

void tst_RustDownload::cleanup()
{
    QSettings settings;
    settings.clear();
    qunsetenv("ARORA_WORKER_BINARY");
    qunsetenv("ARORA_DL_NO_SANDBOX");
#ifdef ARORA_RUSTDL
    PrivacyRequestInterceptor::loadSettings();
    BrowserApplication::setTorMode(false);
    QNetworkProxy::setApplicationProxy(QNetworkProxy());
    // Drop any subscription a gate test installed — the manager is a
    // process singleton.
    AdBlockManager *manager = AdBlockManager::instance();
    const QList<AdBlockSubscription*> subs = manager->subscriptions();
    for (AdBlockSubscription *s : subs)
        manager->removeSubscription(s);
#if defined(ARORA_RUSTCORE)
    // Re-arm the vendored urlstrip set in case a test loaded a
    // synthetic one.
    rc_urlstrip_reload();
#endif
#endif
}

void tst_RustDownload::availability()
{
#ifdef ARORA_RUSTDL
    QVERIFY(RustDownloadEngine::isAvailable());
#else
    QVERIFY(true);
#endif
}

void tst_RustDownload::selectionSetting()
{
#ifdef ARORA_RUSTDL
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));

    settings.remove(QLatin1String("engine"));
    QVERIFY(!RustDownloadEngine::isSelected());

    settings.setValue(QLatin1String("engine"), QLatin1String("rust"));
    QVERIFY(RustDownloadEngine::isSelected());

    settings.setValue(QLatin1String("engine"), QLatin1String("builtin"));
    QVERIFY(!RustDownloadEngine::isSelected());
#else
    QVERIFY(true);
#endif
}

#ifdef ARORA_RUSTDL

// Writes the response body in throttle-sized chunks on a timer so a
// download stays in flight long enough to observe/cancel.
class ChunkedWriter : public QObject
{
public:
    ChunkedWriter(QTcpSocket *socket, const QByteArray &bytes, int chunk,
                  QObject *parent)
        : QObject(parent)
        , m_socket(socket)
        , m_bytes(bytes)
        , m_chunk(chunk)
    {
        socket->setParent(this);
        QTimer *timer = new QTimer(this);
        timer->setInterval(8);
        connect(timer, &QTimer::timeout, this, [this, timer]() {
            if (m_bytes.isEmpty()) {
                timer->stop();
                m_socket->disconnectFromHost();
                deleteLater();
                return;
            }
            const QByteArray part = m_bytes.left(m_chunk);
            m_bytes.remove(0, part.size());
            m_socket->write(part);
        });
        timer->start();
    }

private:
    QTcpSocket *m_socket;
    QByteArray m_bytes;
    int m_chunk;
};

// Minimal HTTP fixture: HEAD answers length+Accept-Ranges, GET honors
// Range, /redir bounces to /file, /traversal serves a hostile
// Content-Disposition.  Throttling is per-connection.
class FixtureServer : public QObject
{
public:
    FixtureServer(const QByteArray &body, bool ranges, int throttle,
                  QObject *parent = nullptr)
        : QObject(parent)
        , m_body(body)
        , m_ranges(ranges)
        , m_throttle(throttle)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(this);
            auto *request = new QByteArray;
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket, request]() {
                request->append(socket->readAll());
                if (request->contains("\r\n\r\n")) {
                    serve(socket, *request);
                    // reqwest may pipeline or re-drive a keep-alive
                    // socket — stop listening before dropping the
                    // captured buffer or it writes into freed memory.
                    socket->disconnect();
                    delete request;
                }
            });
        });
    }

    bool listen()
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1%2")
                    .arg(m_server.serverPort()).arg(path));
    }

    int hits() const { return m_hits; }

    // First captured request header for `path` ("" when absent) —
    // DLACC05 asserts which cookies actually rode the wire.
    QByteArray header(const QByteArray &path, const QByteArray &name) const
    {
        for (const auto &r : m_requests) {
            if (r.first != path)
                continue;
            const QByteArray needle = QByteArray("\r\n") + name.toLower() + ":";
            const QByteArray low = r.second.toLower();
            const int pos = low.indexOf(needle);
            if (pos == -1)
                return QByteArray();
            const int start = pos + needle.size();
            const int end = r.second.indexOf("\r\n", start);
            return r.second.mid(start, end - start).trimmed();
        }
        return QByteArray();
    }

private:
    void serve(QTcpSocket *socket, const QByteArray &request)
    {
        ++m_hits;
        const QByteArray firstLine = request.left(request.indexOf("\r\n"));
        const QList<QByteArray> parts = firstLine.split(' ');
        const QByteArray method = parts.value(0);
        const QByteArray path = parts.value(1);
        m_requests.append({path, request});

        QByteArray range;
        const int rpos = request.toLower().indexOf("\r\nrange:");
        if (rpos != -1) {
            const int start = rpos + 8;
            const int end = request.indexOf("\r\n", start);
            range = request.mid(start, end - start).trimmed();
        }

        if (path == "/redir") {
            socket->write("HTTP/1.1 302 Found\r\n"
                          "Location: /file\r\nContent-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }
        if (path == "/warmup") {
            // Instant answer for the page load that brings the
            // profile's network context (and cookie store) up.
            socket->write("HTTP/1.1 204 No Content\r\n"
                          "Content-Length: 0\r\n\r\n");
            socket->disconnectFromHost();
            return;
        }

        QByteArray cd;
        if (path == "/traversal")
            cd = "Content-Disposition: attachment; "
                 "filename=\"../../evil.bin\"\r\n";

        const int total = m_body.size();
        QByteArray body;
        QByteArray head;
        if (method == "HEAD") {
            head = "HTTP/1.1 200 OK\r\nContent-Length: "
                + QByteArray::number(total) + "\r\n" + cd
                + (m_ranges ? "Accept-Ranges: bytes\r\n" : "")
                + "Content-Type: application/octet-stream\r\n\r\n";
        } else if (m_ranges && range.startsWith("bytes=")) {
            const QByteArray spec = range.mid(6);
            const int dash = spec.indexOf('-');
            const qint64 a = spec.left(dash).toLongLong();
            const QByteArray right = spec.mid(dash + 1);
            const qint64 b = right.isEmpty()
                ? total - 1
                : qMin<qint64>(right.toLongLong(), total - 1);
            body = m_body.mid(a, b - a + 1);
            head = "HTTP/1.1 206 Partial Content\r\nContent-Range: bytes "
                + QByteArray::number(a) + "-" + QByteArray::number(b)
                + "/" + QByteArray::number(total) + "\r\nContent-Length: "
                + QByteArray::number(body.size()) + "\r\n" + cd + "\r\n";
        } else {
            body = m_body;
            head = "HTTP/1.1 200 OK\r\nContent-Length: "
                + QByteArray::number(total) + "\r\n" + cd
                + "Content-Type: application/octet-stream\r\n\r\n";
        }

        if (m_throttle <= 0) {
            socket->write(head + body);
            socket->flush();
            socket->disconnectFromHost();
            socket->deleteLater();
        } else {
            socket->write(head);
            new ChunkedWriter(socket, body, m_throttle, this);
        }
    }

    QTcpServer m_server;
    QByteArray m_body;
    bool m_ranges;
    int m_throttle;
    int m_hits = 0;
    QList<QPair<QByteArray, QByteArray>> m_requests;
};

static QByteArray pattern(int size)
{
    QByteArray out(size, '\0');
    for (int i = 0; i < size; ++i)
        out[i] = static_cast<char>((i * 31 + 7) % 251);
    return out;
}

// An engine whose destination the test controls outright — the item
// path goes through DownloadItem::saveFileName instead.
static RustDownloadEngine *startEngine(const QUrl &url,
                                       const QString &destDir,
                                       const QString &fileName,
                                       const QString &suggested = QString(),
                                       QObject *parent = nullptr)
{
    auto *engine = new RustDownloadEngine(nullptr, url, suggested,
                                          QString(), parent);
    engine->setDownloadDirectory(destDir);
    engine->setDownloadFileName(fileName);
    engine->accept();
    return engine;
}

void tst_RustDownload::engineCompletes()
{
    const QByteArray body = pattern(1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    QSettings settings;
    settings.setValue(QLatin1String("downloadmanager/connections"), 4);

    RustDownloadEngine *engine =
        startEngine(server.url("/file"), dest.path(), "big.bin",
                    QLatin1String("big.bin"), this);
    QSignalSpy spy(engine, &RustDownloadEngine::stateChanged);
    QTRY_COMPARE(engine->state(),
                 QWebEngineDownloadRequest::DownloadCompleted);
    QCOMPARE(QFile(dest.filePath("big.bin")).size(), qint64(body.size()));
    QCOMPARE(spy.count() >= 1, true);
    // Parallel segments mean more requests than probe+one GET.
    QVERIFY(server.hits() > 2);
}

void tst_RustDownload::engineSingleStream()
{
    const QByteArray body = pattern(256 * 1024);
    FixtureServer server(body, /*ranges=*/false, /*throttle=*/0);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    RustDownloadEngine *engine =
        startEngine(server.url("/file"), dest.path(), "plain.bin",
                    QLatin1String("plain.bin"), this);
    QTRY_COMPARE(engine->state(),
                 QWebEngineDownloadRequest::DownloadCompleted);
    QFile f(dest.filePath("plain.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);
}

void tst_RustDownload::engineCancel()
{
    const QByteArray body = pattern(2 * 1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/2048);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    QSettings settings;
    settings.setValue(QLatin1String("downloadmanager/connections"), 8);
    RustDownloadEngine *engine =
        startEngine(server.url("/file"), dest.path(), "cancel.bin",
                    QLatin1String("cancel.bin"), this);
    const bool progressed = QTest::qWaitFor(
        [engine]() { return engine->receivedBytes() > 0
                          || engine->isFinished(); }, 10000);
    QVERIFY(progressed);
    if (engine->state() == QWebEngineDownloadRequest::DownloadInterrupted)
        QFAIL(qPrintable(QLatin1String("engine failed early: ")
                         + engine->interruptReasonString()));
    QVERIFY(engine->receivedBytes() > 0);
    engine->cancel();
    QTRY_COMPARE(engine->state(),
                 QWebEngineDownloadRequest::DownloadCancelled);
    QVERIFY(!QFile::exists(dest.filePath("cancel.bin")));
}

void tst_RustDownload::engineRedirect()
{
    const QByteArray body = pattern(64 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    RustDownloadEngine *engine =
        startEngine(server.url("/redir"), dest.path(), "red.bin",
                    QLatin1String("red.bin"), this);
    QTRY_COMPARE(engine->state(),
                 QWebEngineDownloadRequest::DownloadCompleted);
    QFile f(dest.filePath("red.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);
}

void tst_RustDownload::engineTraversalName()
{
    const QByteArray body = pattern(4096);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    // The Rust side re-sanitizes whatever the caller hands it — a
    // traversal name must still land inside the destination dir.
    RustDownloadEngine *engine =
        startEngine(server.url("/traversal"), dest.path(),
                    "../../evil.bin", "../../evil.bin", this);
    QTRY_COMPARE(engine->state(),
                 QWebEngineDownloadRequest::DownloadCompleted);
    QVERIFY(QFile::exists(dest.filePath("evil.bin")));
    QVERIFY(!QFile::exists(QDir(dest.path()).filePath("../evil.bin")));
}

void tst_RustDownload::cardCompletes()
{
    const QByteArray body = pattern(1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    DownloadManager manager;
    manager.setDownloadDirectory(dest.path() + QLatin1Char('/'));

    auto *engine = new RustDownloadEngine(nullptr, server.url("/file"),
                                          QLatin1String("card.bin"),
                                          QString());
    auto *item = new DownloadItem(engine, /*requestFileName=*/false, &manager);
    QTRY_VERIFY(item->downloadedSuccessfully());
    QFile f(dest.filePath("card.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);
    QCOMPARE(item->m_url, server.url("/file"));
}

void tst_RustDownload::cardCancelRetry()
{
    // Throttled so the card is still mid-flight when stop() lands.
    const QByteArray body = pattern(1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/2048);
    QVERIFY(server.listen());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    DownloadManager manager;
    manager.setDownloadDirectory(dest.path() + QLatin1Char('/'));

    auto *engine = new RustDownloadEngine(nullptr, server.url("/file"),
                                          QLatin1String("retry.bin"),
                                          QString());
    auto *item = new DownloadItem(engine, /*requestFileName=*/false, &manager);
    QTRY_VERIFY(item->downloading());
    QMetaObject::invokeMethod(item, "stop", Qt::DirectConnection);
    QTRY_VERIFY(!item->downloading() && !item->downloadedSuccessfully());

    // "Try Again" restarts through the same engine object — restart()
    // rewinds it and the normal filename/accept flow re-issues
    // dl_start, landing on the same name after the partial is dropped.
    QMetaObject::invokeMethod(item, "tryAgain", Qt::DirectConnection);
    QTest::qWaitFor(
        [item, engine]() { return item->downloadedSuccessfully()
                                 || engine->isFinished(); }, 10000);
    if (!item->downloadedSuccessfully())
        QFAIL(qPrintable(QLatin1String("retry did not complete: state=")
                         + QString::number(engine->state())
                         + QLatin1String(" err=")
                         + engine->interruptReasonString()));
    QFile f(dest.filePath("retry.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);
}

// ---- DLACC05 secrets + filesystem hygiene --------------------------

// The cookie export's home: cookies-<random> directly inside the
// 0700 parts root.  Returns "" while no export exists.
static QString findCookieFile(const QString &partsDir)
{
    const QStringList names = QDir(partsDir).entryList(
        {QStringLiteral("cookies-*")}, QDir::Files);
    return names.isEmpty() ? QString()
                           : partsDir + QLatin1Char('/') + names.first();
}

void tst_RustDownload::engineCookieExport()
{
    // Throttled so the export file is observable mid-flight.
    const QByteArray body = pattern(2 * 1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/4096);
    QVERIFY(server.listen());

    // An OTR profile + a live page: the store only runs once a page
    // spins up the profile's network context.  A fresh jar for this
    // profile reads the relaxed policy at construction, so the
    // programmatic seeds below are accepted.
    QSettings settings;
    settings.setValue(QLatin1String("cookies/acceptCookies"),
                      QLatin1String("AcceptAlways"));
    settings.setValue(QLatin1String("cookies/blockThirdPartyCookies"),
                      false);
    QScopedPointer<QWebEngineProfile> profile(new QWebEngineProfile);
    QScopedPointer<QWebEnginePage> page(
        new QWebEnginePage(profile.data()));
    // The store only runs once a navigation spins the profile's
    // network context up — warm it on a path the download never uses.
    QSignalSpy loadSpy(page.data(), &QWebEnginePage::loadFinished);
    page->load(server.url(QStringLiteral("/warmup")));
    QVERIFY(loadSpy.wait(15000));

    // One cookie that matches the download URL and one that does not
    // — the export must carry ONLY the matching rows; the whole jar
    // never reaches disk.
    QNetworkCookie matching("sess", "AAA");
    QNetworkCookie unrelated("other", "ZZZ");
    unrelated.setDomain(QLatin1String("unrelated.example"));
    QSignalSpy storeSpy(profile->cookieStore(),
                        &QWebEngineCookieStore::cookieAdded);
    CookieJar *jar = CookieJar::instance(profile.data());
    profile->cookieStore()->setCookie(matching, server.url("/file"));
    profile->cookieStore()->setCookie(
        unrelated, QUrl(QStringLiteral("http://unrelated.example/")));
    QTRY_VERIFY(storeSpy.count() >= 1);
    QTRY_VERIFY(!jar->cookiesForUrl(server.url("/file")).isEmpty());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    auto *engine = new RustDownloadEngine(page.data(), server.url("/file"),
                                          QLatin1String("ck.bin"),
                                          QString(), this);
    engine->setDownloadDirectory(dest.path());
    engine->setDownloadFileName(QLatin1String("ck.bin"));
    engine->accept();

    const QString partsDir =
        BrowserPaths::dataFilePath(QLatin1String("downloads-parts"));
    QString cookiePath;
    QTRY_VERIFY(!(cookiePath = findCookieFile(partsDir)).isEmpty());

    // 0600 — no group/other access — inside the 0700 root.
    const QFileDevice::Permissions perms = QFileInfo(cookiePath).permissions();
    QVERIFY(!(perms & (QFileDevice::ReadGroup | QFileDevice::WriteGroup
                       | QFileDevice::ExeGroup | QFileDevice::ReadOther
                       | QFileDevice::WriteOther | QFileDevice::ExeOther)));

    QFile f(cookiePath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray rows = f.readAll();
    f.close();
    QVERIFY(rows.contains("sess\tAAA"));
    QVERIFY(!rows.contains("unrelated.example"));
    QVERIFY(!rows.contains("ZZZ"));

    QTest::qWaitFor([engine]() { return engine->isFinished(); }, 30000);
    if (engine->state() != QWebEngineDownloadRequest::DownloadCompleted)
        qWarning() << "engine ended in state" << engine->state()
                   << "err:" << engine->interruptReasonString();
    QCOMPARE(engine->state(),
             QWebEngineDownloadRequest::DownloadCompleted);

    // The session cookie really rode the wire to the target host.
    QVERIFY(server.header("/file", "cookie").contains("sess=AAA"));
    // And the export file is gone once the transfer lands.
    QVERIFY(findCookieFile(partsDir).isEmpty());
}

void tst_RustDownload::engineCancelCleansSecrets()
{
    const QByteArray body = pattern(2 * 1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/2048);
    QVERIFY(server.listen());

    // Same live-profile + permissive-fresh-jar setup as the export
    // test — cancel must remove the export file too.
    QSettings settings;
    settings.setValue(QLatin1String("cookies/acceptCookies"),
                      QLatin1String("AcceptAlways"));
    settings.setValue(QLatin1String("cookies/blockThirdPartyCookies"),
                      false);
    QScopedPointer<QWebEngineProfile> profile(new QWebEngineProfile);
    QScopedPointer<QWebEnginePage> page(
        new QWebEnginePage(profile.data()));
    QSignalSpy loadSpy(page.data(), &QWebEnginePage::loadFinished);
    page->load(server.url(QStringLiteral("/warmup")));
    QVERIFY(loadSpy.wait(15000));
    QSignalSpy storeSpy(profile->cookieStore(),
                        &QWebEngineCookieStore::cookieAdded);
    CookieJar *jar = CookieJar::instance(profile.data());
    profile->cookieStore()->setCookie(
        QNetworkCookie("sess", "AAA"), server.url("/file"));
    QTRY_VERIFY(storeSpy.count() >= 1);
    QTRY_VERIFY(!jar->cookiesForUrl(server.url("/file")).isEmpty());

    QTemporaryDir dest;
    QVERIFY(dest.isValid());
    auto *engine = new RustDownloadEngine(page.data(), server.url("/file"),
                                          QLatin1String("gone.bin"),
                                          QString(), this);
    engine->setDownloadDirectory(dest.path());
    engine->setDownloadFileName(QLatin1String("gone.bin"));
    engine->accept();
    const bool progressed = QTest::qWaitFor(
        [engine]() { return engine->receivedBytes() > 0
                          || engine->isFinished(); }, 10000);
    QVERIFY(progressed);
    if (!engine->isFinished()) {
        engine->cancel();
        QTRY_COMPARE(engine->state(),
                     QWebEngineDownloadRequest::DownloadCancelled);
        // No partial output when the transfer was cancelled.
        QVERIFY(!QFile::exists(dest.filePath("gone.bin")));
    }
    // No dest-dir staging file and no cookie export — the checks
    // hold for either terminal state.
    QVERIFY(QDir(dest.path())
            .entryList({QStringLiteral(".*.ardl")}, QDir::Files | QDir::Hidden)
            .isEmpty());
    const QString partsDir =
        BrowserPaths::dataFilePath(QLatin1String("downloads-parts"));
    QTRY_VERIFY(findCookieFile(partsDir).isEmpty());
}

// ---- DLACC04 gate coverage ----------------------------------------

typedef RustDownloadEngine::GateDecision GateDecision;

static GateDecision check(const QUrl &url, const QUrl &prev = QUrl(),
                          const QUrl &firstParty = QUrl(),
                          AdBlockNetwork *network = nullptr)
{
    return RustDownloadEngine::gateCheck(url, prev, firstParty,
                                         QLatin1String("test"), network);
}

void tst_RustDownload::gateSchemeRefused()
{
    // Only http(s) may be fetched — a download URL or redirect target
    // on anything else is refused before it reaches the wire.
    GateDecision d = check(QUrl("ftp://example.com/file"));
    QCOMPARE(d.action, RustDownloadEngine::GateBlock);
    QVERIFY(d.reason.contains(QLatin1String("ftp")));
    QCOMPARE(check(QUrl("file:///etc/passwd")).action,
             RustDownloadEngine::GateBlock);
}

void tst_RustDownload::gateHttpsFirstUpgrade()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), true);
    settings.setValue(QLatin1String("privacy/httpsOnly"), true);
    PrivacyRequestInterceptor::loadSettings();

    // The engine-side https-first upgrade is replayed as a gate
    // rewrite the crate then follows (and re-gates).
    GateDecision d = check(QUrl("http://example.com/dl.bin?keep=1"));
    QCOMPARE(d.action, RustDownloadEngine::GateRewrite);
    QCOMPARE(QUrl(d.url), QUrl("https://example.com/dl.bin?keep=1"));
}

void tst_RustDownload::gateHttpsOnlyBlocks()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), true);
    PrivacyRequestInterceptor::loadSettings();

    // With the upgrade pass off, a plain-http download hits the
    // HTTPS-Only veto the same way a navigation would.
    GateDecision d = check(QUrl("http://example.com/dl.bin"));
    QCOMPARE(d.action, RustDownloadEngine::GateBlock);
    QVERIFY(d.reason.contains(QLatin1String("http")));

    // And the toggle actually gates — disabled, it passes through.
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();
    QCOMPARE(check(QUrl("http://example.com/dl.bin")).action,
             RustDownloadEngine::GateAllow);
}

void tst_RustDownload::gatePrivateExempt()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), true);
    settings.setValue(QLatin1String("privacy/httpsOnly"), true);
    PrivacyRequestInterceptor::loadSettings();

    // The LAN/loopback/.local/.onion carve-out the interceptor makes
    // applies to downloads too — user-initiated local traffic stands.
    QCOMPARE(check(QUrl("http://127.0.0.1:9/x")).action,
             RustDownloadEngine::GateAllow);
    QCOMPARE(check(QUrl("http://localhost:9/x")).action,
             RustDownloadEngine::GateAllow);
    QCOMPARE(check(QUrl("http://192.168.1.1/router")).action,
             RustDownloadEngine::GateAllow);
    QCOMPARE(check(QUrl("http://printer.local/x")).action,
             RustDownloadEngine::GateAllow);
    QCOMPARE(check(QUrl("http://site.onion/f")).action,
             RustDownloadEngine::GateAllow);
}

void tst_RustDownload::gateSsrfRedirect()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();

    // First hop to a LAN host stands — the user asked for it.
    QCOMPARE(check(QUrl("http://192.168.1.1/f")).action,
             RustDownloadEngine::GateAllow);
    QCOMPARE(check(QUrl("http://127.0.0.1/f")).action,
             RustDownloadEngine::GateAllow);

    // A redirect hop crossing public -> private is refused: a public
    // URL must not make the browser fetch the user's LAN/loopback.
    QCOMPARE(check(QUrl("http://192.168.1.1/f"),
                   QUrl("http://example.com/x")).action,
             RustDownloadEngine::GateBlock);
    QCOMPARE(check(QUrl("http://127.0.0.1/f"),
                   QUrl("https://example.com/x")).action,
             RustDownloadEngine::GateBlock);
    QCOMPARE(check(QUrl("http://10.0.0.2/f"),
                   QUrl("http://example.com/x")).action,
             RustDownloadEngine::GateBlock);
    QCOMPARE(check(QUrl("http://thing.local/f"),
                   QUrl("http://example.com/x")).action,
             RustDownloadEngine::GateBlock);

    // LAN -> LAN and LAN -> public chaining stays legal.
    QCOMPARE(check(QUrl("http://192.168.1.1/f"),
                   QUrl("http://10.0.0.2/x")).action,
             RustDownloadEngine::GateAllow);
    QCOMPARE(check(QUrl("http://example.com/f"),
                   QUrl("http://192.168.1.1/x")).action,
             RustDownloadEngine::GateAllow);
}

void tst_RustDownload::gateAdblockBlocks()
{
    // Isolate the adblock step from the https toggles.
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);
    auto *sub = new AdBlockSubscription(QUrl(), manager);
    sub->setEnabled(true);
    manager->addSubscription(sub);
    AdBlockRule rule(QLatin1String("||dltracker.invalid^"));
    rule.setEnabled(true);
    sub->addRule(rule);
    manager->network()->rebuildRules();

    // The same network::match() the request interceptor consults —
    // a tracker-domain download dies the way the resource fetch
    // would.
    GateDecision d = check(QUrl("https://dltracker.invalid/payload.exe"),
                           QUrl(), QUrl("https://shop.example/cart"),
                           manager->network());
    QCOMPARE(d.action, RustDownloadEngine::GateBlock);
    QVERIFY(d.reason.contains(QLatin1String("content rules")));

    // A host no rule touches still passes.
    QCOMPARE(check(QUrl("https://cdn.clean.invalid/f"), QUrl(),
                   QUrl("https://shop.example/cart"),
                   manager->network()).action,
             RustDownloadEngine::GateAllow);
}

void tst_RustDownload::canHandleTorProxy()
{
    // Normal mode: http(s) goes to the crate, everything else stays
    // with the engine path.
    QVERIFY(RustDownloadEngine::canHandle(QUrl("http://example.com/f")));
    QVERIFY(RustDownloadEngine::canHandle(QUrl("https://example.com/f")));
    QVERIFY(!RustDownloadEngine::canHandle(QUrl("ftp://example.com/f")));
    QVERIFY(!RustDownloadEngine::canHandle(QUrl("file:///etc/passwd")));

    // Tor mode: the release-blocking rule — the crate takes the
    // download only while a SOCKS5 application proxy exists, so a
    // tor download can never ride a direct connection.
    const QNetworkProxy saved = QNetworkProxy::applicationProxy();
    BrowserApplication::setTorMode(true);
    QNetworkProxy::setApplicationProxy(
        QNetworkProxy(QNetworkProxy::NoProxy));
    QVERIFY(!RustDownloadEngine::canHandle(QUrl("https://example.com/f")));
    QNetworkProxy::setApplicationProxy(QNetworkProxy(
        QNetworkProxy::Socks5Proxy, QLatin1String("127.0.0.1"), 9050));
    QVERIFY(RustDownloadEngine::canHandle(QUrl("https://example.com/f")));
    BrowserApplication::setTorMode(false);
    QNetworkProxy::setApplicationProxy(saved);
}

#if defined(ARORA_RUSTCORE)
void tst_RustDownload::gateUrlStrip()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();

    const QByteArray rules = QByteArrayLiteral(
        R"({"version":1,"params":["dl_tok"]})");
    QCOMPARE(rc_urlstrip_load_rules(
                 reinterpret_cast<const uint8_t *>(rules.constData()),
                 size_t(rules.size())),
             RC_OK);

    // SEC17 on the download path: a tracked-param URL is rewritten,
    // and the crate re-gates the rewritten target.
    GateDecision d = check(
        QUrl("https://shop.example/item?dl_tok=abc&ok=1"));
    QCOMPARE(d.action, RustDownloadEngine::GateRewrite);
    QCOMPARE(QUrl(d.url), QUrl("https://shop.example/item?ok=1"));

    rc_urlstrip_reload();
}

void tst_RustDownload::gateDomainBlocklist()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();

    // SEC18: merge a host into the builtin blocklist — exact and
    // parent-suffix matches are refused on the download path too.
    const QByteArray list = QByteArrayLiteral("dlphish.invalid\n");
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(list.constData()),
                 size_t(list.size())),
             RC_OK);
    GateDecision d = check(QUrl("https://dlphish.invalid/f"));
    QCOMPARE(d.action, RustDownloadEngine::GateBlock);
    QVERIFY(d.reason.contains(QLatin1String("blocklist")));
    QCOMPARE(check(QUrl("https://a.dlphish.invalid/f")).action,
             RustDownloadEngine::GateBlock);
}
#endif // ARORA_RUSTCORE

// ---- SAND02: the confined --download-worker subprocess --------------
// These drive the JSONL protocol through the real bwrap wrap — the
// worker binary is `../../arora` relative to this test binary
// (ARORA_WORKER_BINARY overrides it), so the suite must run against a
// built app.

static QString aroraBinary()
{
    const QByteArray env = qgetenv("ARORA_WORKER_BINARY");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QDir(QCoreApplication::applicationDirPath())
        .absoluteFilePath(QStringLiteral("../../arora"));
}

static bool workerWrapAvailable()
{
    return QFileInfo(aroraBinary()).isExecutable()
        && !SandboxManager::bwrapPath().isEmpty();
}

static QJsonObject workerJob(const QUrl &url, const QString &workDir,
                             const QString &destDir,
                             const QString &suggested)
{
    QJsonObject job;
    job.insert(QLatin1String("cmd"), QLatin1String("job"));
    job.insert(QLatin1String("url"), url.toString());
    job.insert(QLatin1String("dest_dir"), destDir);
    job.insert(QLatin1String("suggested_name"), suggested);
    job.insert(QLatin1String("work_dir"), workDir);
    job.insert(QLatin1String("connections"), 4);
    job.insert(QLatin1String("options"), QJsonObject());
    return job;
}

// Spawn `arora --download-worker` under the generated bwrap wrap and
// collect its event stream until a terminal event or process death.
// Gate requests are answered with `gateAction` (0=allow by default —
// the worker blocks on the reply, so answering is not optional).
static QList<QJsonObject> runWorker(const QJsonObject &job,
                                    const QString &workDir,
                                    const QString &destDir,
                                    int gateAction = 0,
                                    int timeoutMs = 45000)
{
    const QStringList wrap = BwrapGenerator::downloadWorkerCommandLine(
        aroraBinary(), {QStringLiteral("--download-worker")},
        workDir, destDir);
    QProcess proc;
    proc.setProgram(wrap.first());
    proc.setArguments(wrap.mid(1));
    proc.start();
    if (!proc.waitForStarted(10000))
        return {};
    proc.write(QJsonDocument(job).toJson(QJsonDocument::Compact)
               + '\n');

    QList<QJsonObject> events;
    QByteArray buf;
    QElapsedTimer timer;
    timer.start();
    bool terminal = false;
    while (!terminal && timer.elapsed() < timeoutMs) {
        // Pump the event loop rather than waitForReadyRead: the
        // FixtureServer lives in this process and can only accept
        // the worker's connections while the loop runs (and pumping
        // also flushes the gate replies we write back).
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        buf += proc.readAllStandardOutput();
        int nl;
        while ((nl = buf.indexOf('\n')) != -1) {
            const QByteArray line = buf.left(nl).trimmed();
            buf.remove(0, nl + 1);
            if (line.isEmpty())
                continue;
            const QJsonObject ev =
                QJsonDocument::fromJson(line).object();
            events.append(ev);
            const QString kind =
                ev.value(QLatin1String("ev")).toString();
            if (kind == QLatin1String("gate")) {
                QJsonObject reply;
                reply.insert(QLatin1String("ev"),
                             QLatin1String("gate-reply"));
                reply.insert(QLatin1String("id"),
                             ev.value(QLatin1String("id")));
                reply.insert(QLatin1String("action"), gateAction);
                if (gateAction == 1)
                    reply.insert(QLatin1String("reason"),
                                 QLatin1String("test gate refusal"));
                proc.write(QJsonDocument(reply)
                               .toJson(QJsonDocument::Compact) + '\n');
            }
            if (kind == QLatin1String("done")
                    || kind == QLatin1String("cancelled")
                    || kind == QLatin1String("error"))
                terminal = true;
        }
        if (proc.state() == QProcess::NotRunning)
            break;
    }
    proc.kill();
    proc.waitForFinished(3000);
    return events;
}

void tst_RustDownload::workerDownloadE2E()
{
    if (!workerWrapAvailable())
        QSKIP("arora binary or bwrap missing — worker wrap untestable");
    const QByteArray body = pattern(512 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());
    QTemporaryDir work;
    QTemporaryDir dest;
    QVERIFY(work.isValid() && dest.isValid());

    const QList<QJsonObject> events = runWorker(
        workerJob(server.url("/file"), work.path(), dest.path(),
                  QLatin1String("w.bin")),
        work.path(), dest.path());
    QVERIFY(!events.isEmpty());

    bool sawProgress = false;
    QString output;
    for (const QJsonObject &ev : events) {
        if (ev.value(QLatin1String("ev")) == QLatin1String("progress"))
            sawProgress = true;
        if (ev.value(QLatin1String("ev")) == QLatin1String("done"))
            output = ev.value(QLatin1String("output")).toString();
    }
    QVERIFY(sawProgress);
    QVERIFY(!output.isEmpty());
    QFile f(output);
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);
    // The file must land INSIDE the destination dir (absolutePath of
    // a dir QFileInfo is its parent — compare against the dir's own
    // absoluteFilePath).
    QVERIFY2(QFileInfo(output).absolutePath()
                 == QFileInfo(dest.path()).absoluteFilePath(),
             qPrintable(QStringLiteral("output=%1 dest=%2")
                        .arg(output, dest.path())));
}

void tst_RustDownload::workerConfinesFilesystem()
{
    if (!workerWrapAvailable())
        QSKIP("arora binary or bwrap missing — worker wrap untestable");
    const QByteArray body = pattern(64 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());
    QTemporaryDir work;
    QTemporaryDir dest;
    QVERIFY(work.isValid() && dest.isValid());
    // A host-side secret OUTSIDE any granted path — the wrap must
    // hide it (this is the profile/keyring stand-in: the worker's
    // filesystem has no $HOME at all).
    QTemporaryDir secret;
    QVERIFY(secret.isValid());
    QFile secretFile(secret.filePath("keys.pem"));
    QVERIFY(secretFile.open(QIODevice::WriteOnly));
    secretFile.write("PRIVATE KEY");
    secretFile.close();

    QJsonObject job = workerJob(server.url("/file"), work.path(),
                                dest.path(), QLatin1String("w.bin"));
    job.insert(QLatin1String("probe_paths"),
               QJsonArray::fromStringList(
                   {QDir::homePath(), secretFile.fileName(),
                    BrowserPaths::dataFilePath(QString()),
                    dest.path(), work.path(),
                    QStringLiteral("/etc")}));
    const QList<QJsonObject> events = runWorker(job, work.path(),
                                                dest.path());

    QHash<QString, bool> readable;
    for (const QJsonObject &ev : events) {
        if (ev.value(QLatin1String("ev")) == QLatin1String("probe"))
            readable.insert(ev.value(QLatin1String("path")).toString(),
                            ev.value(QLatin1String("readable"))
                                .toBool());
    }
    QCOMPARE(readable.size(), 6);
    // $HOME, the secret file and the profile data dir are all
    // invisible inside the worker's mount namespace.
    QVERIFY(!readable.value(QDir::homePath(), true));
    QVERIFY(!readable.value(secretFile.fileName(), true));
    QVERIFY(!readable.value(BrowserPaths::dataFilePath(QString()),
                            true));
    // And the granted surface is visible.
    QVERIFY(readable.value(dest.path(), false));
    QVERIFY(readable.value(work.path(), false));
    QVERIFY(readable.value(QStringLiteral("/etc"), false));
}

void tst_RustDownload::workerGateBridgeBlocks()
{
    if (!workerWrapAvailable())
        QSKIP("arora binary or bwrap missing — worker wrap untestable");
    const QByteArray body = pattern(64 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());
    QTemporaryDir work;
    QTemporaryDir dest;
    QVERIFY(work.isValid() && dest.isValid());

    const QList<QJsonObject> events = runWorker(
        workerJob(server.url("/file"), work.path(), dest.path(),
                  QLatin1String("w.bin")),
        work.path(), dest.path(), /*gateAction=*/1);
    bool sawGate = false;
    bool sawError = false;
    for (const QJsonObject &ev : events) {
        const QString kind = ev.value(QLatin1String("ev")).toString();
        if (kind == QLatin1String("gate"))
            sawGate = true;
        if (kind == QLatin1String("error"))
            sawError = true;
    }
    // The request was refused at the gate — the download dies before
    // a byte lands and nothing appears in the destination.
    QVERIFY(sawGate);
    QVERIFY(sawError);
    QVERIFY(!QFile::exists(dest.filePath("w.bin")));
}

void tst_RustDownload::workerCancelViaStdin()
{
    if (!workerWrapAvailable())
        QSKIP("arora binary or bwrap missing — worker wrap untestable");
    // Throttled so a cancel mid-flight is observable.
    const QByteArray body = pattern(2 * 1024 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/4096);
    QVERIFY(server.listen());
    QTemporaryDir work;
    QTemporaryDir dest;
    QVERIFY(work.isValid() && dest.isValid());

    const QStringList wrap = BwrapGenerator::downloadWorkerCommandLine(
        aroraBinary(), {QStringLiteral("--download-worker")},
        work.path(), dest.path());
    QProcess proc;
    proc.setProgram(wrap.first());
    proc.setArguments(wrap.mid(1));
    proc.start();
    QVERIFY(proc.waitForStarted(10000));
    proc.write(QJsonDocument(workerJob(server.url("/file"),
                                       work.path(), dest.path(),
                                       QLatin1String("w.bin")))
                   .toJson(QJsonDocument::Compact) + '\n');

    QList<QJsonObject> events;
    QByteArray buf;
    QElapsedTimer timer;
    timer.start();
    bool cancelled = false;
    bool sentCancel = false;
    while (!cancelled && timer.elapsed() < 30000) {
        // Same event-loop pump as runWorker — the FixtureServer is
        // in-process and starves without it.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        buf += proc.readAllStandardOutput();
        int nl;
        while ((nl = buf.indexOf('\n')) != -1) {
            const QByteArray line = buf.left(nl).trimmed();
            buf.remove(0, nl + 1);
            if (line.isEmpty())
                continue;
            const QJsonObject ev =
                QJsonDocument::fromJson(line).object();
            events.append(ev);
            const QString kind =
                ev.value(QLatin1String("ev")).toString();
            if (kind == QLatin1String("gate")) {
                QJsonObject reply;
                reply.insert(QLatin1String("ev"),
                             QLatin1String("gate-reply"));
                reply.insert(QLatin1String("id"),
                             ev.value(QLatin1String("id")));
                reply.insert(QLatin1String("action"), 0);
                proc.write(QJsonDocument(reply)
                               .toJson(QJsonDocument::Compact) + '\n');
            }
            if (kind == QLatin1String("progress") && !sentCancel) {
                sentCancel = true;
                proc.write(QByteArray("{\"cmd\":\"cancel\"}\n"));
            }
            if (kind == QLatin1String("cancelled"))
                cancelled = true;
        }
        if (proc.state() == QProcess::NotRunning)
            break;
    }
    QVERIFY(sentCancel);
    QVERIFY(cancelled);
    QVERIFY(!QFile::exists(dest.filePath("w.bin")));
    proc.kill();
    proc.waitForFinished(3000);
}

void tst_RustDownload::engineRunsSandboxed()
{
    if (!workerWrapAvailable())
        QSKIP("arora binary or bwrap missing — worker wrap untestable");
    const QByteArray body = pattern(256 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());
    QTemporaryDir dest;
    QVERIFY(dest.isValid());

    qputenv("ARORA_WORKER_BINARY", aroraBinary().toUtf8());
    QSettings settings;
    settings.setValue(QLatin1String("downloadmanager/sandboxedWorker"),
                      true);
    QVERIFY(RustDownloadEngine::sandboxedWorkerEnabled());

    auto *engine = new RustDownloadEngine(nullptr, server.url("/file"),
                                          QLatin1String("e.bin"),
                                          QString(), this);
    engine->setDownloadDirectory(dest.path());
    engine->setDownloadFileName(QLatin1String("e.bin"));
    engine->setProbePaths({dest.path(), QDir::homePath()});
    engine->accept();
    QVERIFY2(engine->subprocessActive(),
             "expected the confined worker path, got in-process");
    QTRY_COMPARE(engine->state(),
                 QWebEngineDownloadRequest::DownloadCompleted);

    QFile f(dest.filePath("e.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);

    // Probe evidence the subprocess really ran the wrap: the
    // destination was visible, $HOME was not.
    bool destSeen = false;
    bool homeSeen = false;
    const QJsonArray probes = engine->probeResults();
    for (const QJsonValue &v : probes) {
        const QJsonObject p = v.toObject();
        const QString path =
            p.value(QLatin1String("path")).toString();
        if (path == dest.path())
            destSeen = p.value(QLatin1String("readable")).toBool();
        if (path == QDir::homePath())
            homeSeen = p.value(QLatin1String("readable")).toBool();
    }
    QVERIFY(probes.size() >= 2);
    QVERIFY(destSeen);
    QVERIFY(!homeSeen);
}

void tst_RustDownload::engineFallsBackInProcess()
{
    if (SandboxManager::bwrapPath().isEmpty())
        QSKIP("bwrap missing — fallback path is the only path");
    const QByteArray body = pattern(128 * 1024);
    FixtureServer server(body, /*ranges=*/true, /*throttle=*/0);
    QVERIFY(server.listen());
    QTemporaryDir dest;
    QVERIFY(dest.isValid());

    // An executable that is not the worker: bwrap runs it, it exits
    // instantly, no protocol line ever arrives — the engine must
    // fall back to the in-process dl_start and still complete.
    qputenv("ARORA_WORKER_BINARY", "/bin/false");
    QSettings settings;
    settings.setValue(QLatin1String("downloadmanager/sandboxedWorker"),
                      true);
    QVERIFY(RustDownloadEngine::sandboxedWorkerEnabled());

    auto *engine = new RustDownloadEngine(nullptr, server.url("/file"),
                                          QLatin1String("f.bin"),
                                          QString(), this);
    engine->setDownloadDirectory(dest.path());
    engine->setDownloadFileName(QLatin1String("f.bin"));
    engine->accept();
    QTRY_COMPARE_WITH_TIMEOUT(
        engine->state(),
        QWebEngineDownloadRequest::DownloadCompleted, 30000);
    QVERIFY(!engine->subprocessActive());
    QFile f(dest.filePath("f.bin"));
    QVERIFY(f.open(QIODevice::ReadOnly));
    QCOMPARE(f.readAll(), body);
}

#else // !ARORA_RUSTDL

void tst_RustDownload::noRustBuild()
{
    // The crate is absent by configuration — nothing to exercise.
    QVERIFY(true);
}

#endif // ARORA_RUSTDL

QTEST_MAIN(tst_RustDownload)
#include "tst_rustdownload.moc"
