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
}

void tst_RustDownload::cleanup()
{
    QSettings settings;
    settings.clear();
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

private:
    void serve(QTcpSocket *socket, const QByteArray &request)
    {
        ++m_hits;
        const QByteArray firstLine = request.left(request.indexOf("\r\n"));
        const QList<QByteArray> parts = firstLine.split(' ');
        const QByteArray method = parts.value(0);
        const QByteArray path = parts.value(1);

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

#else // !ARORA_RUSTDL

void tst_RustDownload::noRustBuild()
{
    // The crate is absent by configuration — nothing to exercise.
    QVERIFY(true);
}

#endif // ARORA_RUSTDL

QTEST_MAIN(tst_RustDownload)
#include "tst_rustdownload.moc"
