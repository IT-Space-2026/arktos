/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

// TST01: rewritten for Qt WebEngine.  Downloads are produced by
// QWebEnginePage::download() and delivered to the manager through
// QWebEngineProfile::downloadRequested; the old "download a real file
// over the network" tests are replaced with in-process data: URLs that
// complete deterministically offscreen.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <QtGui/QtGui>
#include <qmessagebox.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>
#include "downloadgraph.h"
#include "downloadmanager.h"
#include "squeezelabel.h"
#include "qtry.h"

class tst_DownloadManager : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void downloadmanager_data();
    void downloadmanager();
    void cleanupButton_data();
    void cleanupButton();
    void download_data();
    void download();
    void removePolicy_data();
    void removePolicy();
    void modelAccessors();
    void helpers();
    void sanitizeFileName_data();
    void sanitizeFileName();
    void dangerousFileClassification();
    void hostileSuggestedName();
    void dangerousDownload_data();
    void dangerousDownload();
    void overwriteKeepsExisting();
    void partialCleanup();
    void execBitStripped();
    void externalHandler();
    void cardDetails();
    void cardRestart();
    void speedSeries();
    void restoredCard();
};

// Clicks the requested button on any modal QMessageBox that pops while
// this object is alive — the SEC01 "harmful download" prompt exec()s a
// real modal dialog even under the offscreen QPA.  answered() lets a
// test assert no prompt appeared at all.
class ModalAnswer : public QObject
{
public:
    ModalAnswer(QMessageBox::StandardButton button, QObject *parent = nullptr)
        : QObject(parent)
        , m_button(button)
        , m_answered(0)
    {
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!box)
                return;
            QAbstractButton *button = box->button(m_button);
            if (button)
                button->click();
            else
                box->reject();
            ++m_answered;
        });
        timer->start(50);
    }

    int answered() const { return m_answered; }

private:
    QMessageBox::StandardButton m_button;
    int m_answered;
};

// Answers every connection with a complete small response carrying the
// given Content-Type / Content-Disposition so the download finishes.
static void serveDownload(QTcpServer *server, const QByteArray &contentType,
                          const QByteArray &contentDisposition)
{
    const QByteArray body("file contents");
    QObject::connect(server, &QTcpServer::newConnection, server,
                     [server, contentType, contentDisposition, body]() {
        QTcpSocket *socket = server->nextPendingConnection();
        socket->setParent(server);
        socket->readAll();
        QByteArray response = "HTTP/1.1 200 OK\r\n"
            "Content-Type: " + contentType + "\r\n"
            "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!contentDisposition.isEmpty())
            response += "Content-Disposition: " + contentDisposition + "\r\n";
        socket->write(response + "\r\n" + body);
        socket->disconnectFromHost();
    });
}

// Serves a huge Content-Length but trickles single bytes so the
// download stays in-flight until cancelled.
static void serveStalledDownload(QTcpServer *server, const QByteArray &fileName)
{
    QObject::connect(server, &QTcpServer::newConnection, server,
                     [server, fileName]() {
        QTcpSocket *socket = server->nextPendingConnection();
        socket->setParent(server);
        socket->readAll();
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Disposition: attachment; filename=\""
                          + fileName + "\"\r\n"
                      "Content-Length: 104857600\r\n"
                      "\r\n");
        QTimer *trickle = new QTimer(socket);
        QObject::connect(trickle, &QTimer::timeout, socket, [socket]() {
            socket->write("x");
        });
        trickle->start(50);
    });
}

// Subclass that exposes the protected functions.
class SubDownloadManager : public DownloadManager
{
public:
    SubDownloadManager(QWidget *parent = nullptr)
     : DownloadManager(parent)
        {}

};

static const QUrl downloadUrl()
{
    return QUrl(QString::fromLatin1(
        "data:text/plain;base64,") +
        QString::fromLatin1(QByteArray("hello world").toBase64()));
}

// This will be called before the first test function is executed.
// It is only called once.
void tst_DownloadManager::initTestCase()
{
    QCoreApplication::setApplicationName("downloadmanagertest");
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_DownloadManager::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_DownloadManager::init()
{
    QSettings settings;
    settings.clear();
}

// This will be called after every test function.
void tst_DownloadManager::cleanup()
{
}

void tst_DownloadManager::downloadmanager_data()
{
}

void tst_DownloadManager::downloadmanager()
{
    SubDownloadManager manager;
    manager.cleanup();
    manager.download(nullptr, QUrl());
    manager.handleDownloadRequested(nullptr);
    QCOMPARE(manager.removePolicy(), DownloadManager::Never);
    manager.setRemovePolicy(DownloadManager::Never);
}

void tst_DownloadManager::cleanupButton_data()
{
    QTest::addColumn<bool>("waitForDownload");
    QTest::newRow("cancel") << false;
    QTest::newRow("cleanup") << true;
}

// public void cleanup()
void tst_DownloadManager::cleanupButton()
{
    QFETCH(bool, waitForDownload);
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());

    // Serves headers for a huge body that never arrives, keeping one
    // download in-flight so the "cancel" row is deterministic.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        socket->readAll();
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Disposition: attachment; filename=\"bigfile.bin\"\r\n"
                      "Content-Length: 104857600\r\n"
                      "\r\n");
        // Chromium only raises downloadRequested once response body
        // bytes start arriving (headers alone never produce it), so
        // trickle a byte at a time; the huge Content-Length keeps the
        // download in-flight for the whole test.
        QTimer *trickle = new QTimer(socket);
        QObject::connect(trickle, &QTimer::timeout, socket, [socket]() {
            socket->write("x");
        });
        trickle->start(200);
    });
    const QUrl stalledUrl(QString::fromLatin1("http://127.0.0.1:%1/bigfile.bin")
                              .arg(server.serverPort()));

    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QCOMPARE(view->model()->rowCount(), 0);
        QPushButton *cleanupButton = manager.findChild<QPushButton*>();
        QVERIFY(cleanupButton);
        QVERIFY(!cleanupButton->isEnabled());

        QWebEnginePage *page = manager.retryPage(false);
        QVERIFY(page);
        manager.download(page, waitForDownload ? downloadUrl() : stalledUrl);
        // The first real HTTP request boots the WebEngine network
        // service, which can take well over the default QTRY timeout.
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 30000);

        QProgressBar *bar = manager.findChild<QProgressBar*>();
        QVERIFY(bar);

        QList<QPushButton*>buttons = manager.findChildren<QPushButton*>();
        QPushButton *tryAgainButton = nullptr;
        for (int i = 0; i < buttons.count(); ++i)
            if (buttons[i]->text().contains("Try"))
                tryAgainButton = buttons[i];
        QVERIFY(tryAgainButton);
        QVERIFY(tryAgainButton->isHidden());
        QVERIFY(!tryAgainButton->isEnabled());

        QList<DownloadItem*> items = manager.findChildren<DownloadItem*>();
        QCOMPARE(items.count(), 1);
        if (!waitForDownload) {
            // The download stays in-flight, so cleanup cannot drop it.
            QTest::qWait(500);
            QVERIFY(items.first()->downloading());
        } else {
            QTRY_VERIFY(bar->value() == bar->maximum());
        }
        QCOMPARE(cleanupButton->isEnabled(), waitForDownload);
        QCOMPARE(view->model()->rowCount(), 1);
        manager.cleanup();
        QCOMPARE(view->model()->rowCount(), waitForDownload ? 0 : 1);
    }
}

void tst_DownloadManager::download_data()
{
    QTest::addColumn<QStringList>("request");
    QTest::addColumn<int>("rowCount");
    QTest::newRow("onefile") << (QStringList() << downloadUrl().toString()) << 1;
    QTest::newRow("twofiles") << (QStringList() << downloadUrl().toString()
                                               << downloadUrl().toString()) << 2;
    QTest::newRow("empty") << (QStringList() << QString()) << 0;
}

// public void download(QWebEnginePage *page, const QUrl &url, bool requestFileName)
void tst_DownloadManager::download()
{
    QFETCH(QStringList, request);
    QFETCH(int, rowCount);
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QPushButton *cleanupButton = manager.findChild<QPushButton*>();
        QVERIFY(cleanupButton);
        QVERIFY(!cleanupButton->isEnabled());

        QWebEnginePage *page = manager.retryPage(false);
        QVERIFY(page);
        for (int i = 0; i < request.count(); ++i)
            manager.download(page, QUrl(request[i]));

        QTRY_COMPARE(view->model()->rowCount(), rowCount);
        QList<QProgressBar*>bars = manager.findChildren<QProgressBar*>();
        QCOMPARE(bars.count(), rowCount);
    }

    // The downloads land in the temp directory, one file per request.
    QDir dir(downloadDir.path());
    QCOMPARE(dir.entryInfoList(QDir::Files).count(), rowCount);
}

Q_DECLARE_METATYPE(DownloadManager::RemovePolicy)
void tst_DownloadManager::removePolicy_data()
{
    QTest::addColumn<DownloadManager::RemovePolicy>("removePolicy");
    QTest::newRow("Never") << DownloadManager::Never;
    QTest::newRow("Exit") << DownloadManager::Exit;
    QTest::newRow("SuccessFullDownload") << DownloadManager::SuccessFullDownload;
}

// public DownloadManager::RemovePolicy removePolicy() const
void tst_DownloadManager::removePolicy()
{
    QFETCH(DownloadManager::RemovePolicy, removePolicy);
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        manager.setRemovePolicy(removePolicy);
        QCOMPARE(manager.removePolicy(), removePolicy);
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, downloadUrl());
        if (removePolicy == DownloadManager::SuccessFullDownload) {
            // A completed download is dropped immediately, so the row
            // can appear and disappear before any poll sees it; verify
            // via the file that landed and the model settling empty.
            QTRY_VERIFY(QDir(downloadDir.path()).entryInfoList(QDir::Files).count() == 1);
            QTRY_COMPARE(view->model()->rowCount(), 0);
        } else {
            QTRY_COMPARE(view->model()->rowCount(), 1);
            QProgressBar *bar = manager.findChild<QProgressBar*>();
            QVERIFY(bar);
            QTRY_VERIFY(bar->value() == bar->maximum());
        }
    }

    // The finished download is persisted unless the policy dropped it.
    SubDownloadManager manager;
    QTableView *view = manager.findChild<QTableView*>();
    QVERIFY(view);
    QCOMPARE(view->model()->rowCount(), removePolicy == DownloadManager::Never ? 1 : 0);
}

// DownloadModel row accessors: flags, mime data, removeRows — plus the
// manager counters over a finished download.
void tst_DownloadManager::modelAccessors()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QAbstractItemModel *model = view->model();
        QVERIFY(model);

        // Out-of-range accessors on an empty model.
        QCOMPARE(model->data(QModelIndex()), QVariant());
        QCOMPARE(model->flags(QModelIndex()), Qt::ItemFlags());
        QCOMPARE(model->rowCount(model->index(0, 0)), 0);

        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, downloadUrl());
        QTRY_COMPARE(model->rowCount(), 1);

        QProgressBar *bar = manager.findChild<QProgressBar*>();
        QVERIFY(bar);
        QTRY_VERIFY(bar->value() == bar->maximum());
        QCOMPARE(manager.activeDownloads(), 0);
        QVERIFY(manager.allowQuit());

        const QModelIndex index = model->index(0, 0);
        QVERIFY(index.isValid());
        // Finished downloads produce no tooltip and are draggable.
        QCOMPARE(model->data(index, Qt::ToolTipRole), QVariant());
        QVERIFY(model->flags(index) & Qt::ItemIsDragEnabled);
        // removeRows under a valid parent is refused outright.
        QCOMPARE(model->removeRows(0, 1, index), false);
        QMimeData *mime = model->mimeData(QModelIndexList() << index);
        QVERIFY(mime);
        QVERIFY(mime->hasUrls());
        delete mime;

        QVERIFY(model->removeRows(0, 1));
        QCOMPARE(model->rowCount(), 0);
    }
}

// static helpers
void tst_DownloadManager::helpers()
{
    QCOMPARE(DownloadManager::timeString(0), QLatin1String("0 seconds remaining"));
    QVERIFY(DownloadManager::timeString(3600).contains(QLatin1String("minutes")));
    QCOMPARE(DownloadManager::dataString(-1), QLatin1String("-1.0 bytes"));
    QCOMPARE(DownloadManager::dataString(1024), QLatin1String("1.0 kB"));

    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    SubDownloadManager manager;
    manager.setDownloadDirectory(downloadDir.path());
    QCOMPARE(manager.downloadDirectory(), downloadDir.path() + QLatin1Char('/'));
}

// SEC01: the server-suggested file name is untrusted input.
void tst_DownloadManager::sanitizeFileName_data()
{
    QTest::addColumn<QString>("suggested");
    QTest::addColumn<QString>("expected");
    QTest::newRow("plain") << "report.txt" << "report.txt";
    QTest::newRow("unix-traversal") << "../../etc/passwd" << "passwd";
    QTest::newRow("windows-traversal") << "..\\..\\evil.bat" << "evil.bat";
    QTest::newRow("dotdot") << ".." << "";
    QTest::newRow("dot") << "." << "";
    QTest::newRow("only-dots") << "..." << "";
    QTest::newRow("trailing-dots") << "name.." << "name";
    QTest::newRow("trailing-slash") << "a/b/c/" << "";
    QTest::newRow("empty") << "" << "";
    QTest::newRow("control-chars") << "na\x01me.t\x7fxt" << "name.txt";
    QTest::newRow("whitespace") << "  spaced.txt  " << "spaced.txt";
    QTest::newRow("dotfile") << ".bashrc" << ".bashrc";
    QTest::newRow("double-ext") << "archive.tar.gz" << "archive.tar.gz";
    QTest::newRow("encoded-slash-stays") << "a%2Fb.txt" << "a%2Fb.txt";
}

void tst_DownloadManager::sanitizeFileName()
{
    QFETCH(QString, suggested);
    QFETCH(QString, expected);
    QCOMPARE(DownloadItem::sanitizeFileName(suggested), expected);

    // A huge name is capped but keeps its extension.
    const QString longName = QString(300, QLatin1Char('a')) + QLatin1String(".txt");
    const QString sanitized = DownloadItem::sanitizeFileName(longName);
    QVERIFY(sanitized.size() <= 200);
    QVERIFY(sanitized.endsWith(QLatin1String(".txt")));
    // Path components must never survive in any accepted name.
    QVERIFY(!sanitized.contains(QLatin1Char('/')));
    QVERIFY(!sanitized.contains(QLatin1Char('\\')));
}

void tst_DownloadManager::dangerousFileClassification()
{
    // Extensions the OS will run, install, or auto-execute on open.
    QVERIFY(DownloadItem::isDangerousExtension(QLatin1String("evil.sh")));
    QVERIFY(DownloadItem::isDangerousExtension(QLatin1String("evil.SH")));
    QVERIFY(DownloadItem::isDangerousExtension(QLatin1String("evil.desktop")));
    QVERIFY(DownloadItem::isDangerousExtension(QLatin1String("setup.exe")));
    QVERIFY(DownloadItem::isDangerousExtension(QLatin1String("pack.AppImage")));
    QVERIFY(!DownloadItem::isDangerousExtension(QLatin1String("photo.png")));
    QVERIFY(!DownloadItem::isDangerousExtension(QLatin1String("data.bin")));
    QVERIFY(!DownloadItem::isDangerousExtension(QLatin1String("archive.tar.gz")));
    QVERIFY(!DownloadItem::isDangerousExtension(QLatin1String("noextension")));
    QVERIFY(!DownloadItem::isDangerousExtension(QString()));

    // Executable content types warn even behind an innocent name;
    // generic octet-stream must not, or every download would prompt.
    QVERIFY(DownloadItem::isExecutableMimeType(QLatin1String("application/x-msdownload")));
    QVERIFY(DownloadItem::isExecutableMimeType(QLatin1String("APPLICATION/X-MSDOWNLOAD")));
    QVERIFY(DownloadItem::isExecutableMimeType(QLatin1String("application/x-shellscript")));
    QVERIFY(DownloadItem::isExecutableMimeType(QLatin1String("application/x-executable")));
    QVERIFY(DownloadItem::isExecutableMimeType(QLatin1String("application/vnd.debian.binary-package")));
    QVERIFY(DownloadItem::isExecutableMimeType(QLatin1String("application/x-msdownload; q=0.9")));
    QVERIFY(!DownloadItem::isExecutableMimeType(QLatin1String("application/octet-stream")));
    QVERIFY(!DownloadItem::isExecutableMimeType(QLatin1String("image/png")));
    QVERIFY(!DownloadItem::isExecutableMimeType(QLatin1String("text/plain")));
    QVERIFY(!DownloadItem::isExecutableMimeType(QString()));
}

// A hostile Content-Disposition filename must not escape the download
// directory regardless of what Chromium passes through.
void tst_DownloadManager::hostileSuggestedName()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveDownload(&server, "text/plain",
                  "attachment; filename=\"../../sec01-pwned.txt\"");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/payload")
                       .arg(server.serverPort()));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, url);
        QTRY_VERIFY(QDir(downloadDir.path())
                        .entryInfoList(QDir::Files).count() >= 1);
    }
    // Whatever name it landed under, it landed inside the directory.
    const QFileInfoList files = QDir(downloadDir.path())
                                    .entryInfoList(QDir::Files);
    for (const QFileInfo &info : files) {
        QVERIFY(!info.fileName().contains(QLatin1Char('/')));
        QVERIFY(!info.fileName().contains(QLatin1Char('\\')));
        QVERIFY(info.fileName() != QLatin1String(".."));
    }
    QVERIFY(!QFile::exists(downloadDir.path()
                           + QLatin1String("/../../sec01-pwned.txt")));
}

// The harmful-download prompt: dangerous extensions and disguised
// executables must be confirmed before anything is saved.
void tst_DownloadManager::dangerousDownload_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<QString>("contentType");
    QTest::addColumn<bool>("promptExpected");
    QTest::addColumn<bool>("keep");
    QTest::newRow("safe") << "readme.txt" << "text/plain" << false << true;
    QTest::newRow("dangerous-keep") << "evil.sh" << "application/x-sh"
                                    << true << true;
    QTest::newRow("dangerous-discard") << "evil2.sh" << "application/x-sh"
                                       << true << false;
    QTest::newRow("mime-mismatch-discard") << "photo.png"
                                           << "application/x-msdownload"
                                           << true << false;
    QTest::newRow("mime-mismatch-keep") << "photo.png"
                                        << "application/x-msdownload"
                                        << true << true;
}

void tst_DownloadManager::dangerousDownload()
{
    QFETCH(QString, fileName);
    QFETCH(QString, contentType);
    QFETCH(bool, promptExpected);
    QFETCH(bool, keep);
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveDownload(&server, contentType.toLatin1(),
                  "attachment; filename=\"" + fileName.toLatin1() + "\"");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/%2")
                       .arg(server.serverPort()).arg(fileName));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        ModalAnswer answer(keep ? QMessageBox::Save : QMessageBox::Discard,
                           &manager);
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, url);
        QTRY_COMPARE(view->model()->rowCount(), 1);

        const QString landed = downloadDir.path() + QLatin1Char('/') + fileName;
        if (!promptExpected || keep) {
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(landed), 30000);
            // A kept file must never carry the exec bit (SEC01).
            QVERIFY(!(QFile::permissions(landed) & QFileDevice::ExeOwner));
        } else {
            QTRY_VERIFY(answer.answered() >= 1);
            QTest::qWait(400);
            QVERIFY(!QFile::exists(landed));
        }
        QCOMPARE(answer.answered(), promptExpected ? 1 : 0);
    }
}

// Overwrite policy: an existing file on disk is never clobbered — the
// new download dedups onto a "-N" name.
void tst_DownloadManager::overwriteKeepsExisting()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    const QString existing = downloadDir.path() + QLatin1String("/report.txt");
    {
        QFile file(existing);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("original");
    }
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveDownload(&server, "text/plain",
                  "attachment; filename=\"report.txt\"");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/report.txt")
                       .arg(server.serverPort()));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, url);
        QTRY_VERIFY(QDir(downloadDir.path())
                        .entryInfoList(QDir::Files).count() == 2);
    }
    // The pre-existing file is untouched; the download took a new name.
    QFile file(existing);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("original"));
    QFile deduped(downloadDir.path() + QLatin1String("/report-1.txt"));
    QVERIFY(deduped.open(QIODevice::ReadOnly));
    QCOMPARE(deduped.readAll(), QByteArray("file contents"));
}

// Removing a row for a cancelled/interrupted download deletes the
// half-written file; a completed download's file survives cleanup.
void tst_DownloadManager::partialCleanup()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveStalledDownload(&server, "partial.bin");
    const QUrl stalledUrl(QString::fromLatin1("http://127.0.0.1:%1/partial.bin")
                              .arg(server.serverPort()));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QWebEnginePage *page = manager.retryPage(false);

        // A quick completed download whose file must survive cleanup.
        manager.download(page, downloadUrl());
        manager.download(page, stalledUrl);
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 2, 30000);

        QList<DownloadItem*> items = manager.findChildren<DownloadItem*>();
        QCOMPARE(items.count(), 2);
        DownloadItem *stalled = items.at(1);
        // Let some bytes land so Chromium has a partial file on disk,
        // then cancel it.
        QTest::qWait(500);
        QVERIFY(stalled->downloading());
        QMetaObject::invokeMethod(stalled, "stop");
        QTRY_VERIFY(!stalled->downloading());
        QStringList before = QDir(downloadDir.path()).entryList(QDir::Files);
        QVERIFY2(before.count() >= 1, qPrintable(before.join(",")));

        // Cleanup removes both rows (the cancelled one is removable via
        // its Try Again state); the partial file is deleted with it.
        manager.cleanup();
        QCOMPARE(view->model()->rowCount(), 0);
        const QStringList after = QDir(downloadDir.path())
                                      .entryList(QDir::Files);
        // Only the completed download's file may remain.
        for (const QString &name : after)
            QVERIFY(!name.startsWith(QLatin1String("partial")));
    }
}

void tst_DownloadManager::execBitStripped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.path() + QLatin1String("/script.sh");
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("#!/bin/sh\n");
    }
    QVERIFY(QFile::setPermissions(path, QFileDevice::ExeOwner
                                        | QFileDevice::ReadOwner
                                        | QFileDevice::WriteOwner));
    DownloadItem::removeExecutableBit(path);
    QVERIFY(!(QFile::permissions(path) & QFileDevice::ExeOwner));
    QVERIFY(QFile::permissions(path) & QFileDevice::ReadOwner);

    // Non-executable files and missing paths are untouched/no-ops.
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    DownloadItem::removeExecutableBit(path);
    const QFileDevice::Permissions execBits =
        QFileDevice::ExeOwner | QFileDevice::ExeUser
        | QFileDevice::ExeGroup | QFileDevice::ExeOther;
    QVERIFY(!(QFile::permissions(path) & execBits));
    QVERIFY(QFile::permissions(path) & QFileDevice::ReadOwner);
    DownloadItem::removeExecutableBit(dir.path() + QLatin1String("/missing"));
    DownloadItem::removeExecutableBit(QString());
}

// SEC09: the external handler is an explicit user choice (the Settings
// dialog is the only writer of downloadmanager/external +
// externalPath) and the url must reach it as a single percent-encoded
// argv element — never a local/internal scheme, never extra args.
void tst_DownloadManager::externalHandler()
{
#ifdef Q_OS_WIN
    QSKIP("the recorder helper is a POSIX shell script");
#else
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString recordPath = dir.filePath(QLatin1String("argv.txt"));
    const QString scriptPath = dir.filePath(QLatin1String("handler.sh"));
    {
        QFile script(scriptPath);
        QVERIFY(script.open(QIODevice::WriteOnly));
        // Record argv, one argument per line.
        script.write("#!/bin/sh\nprintf '%s\\n' \"$@\" > \""
                     + QFile::encodeName(recordPath) + "\"\n");
    }
    QVERIFY(QFile::setPermissions(scriptPath, QFileDevice::ReadOwner
            | QFileDevice::WriteOwner | QFileDevice::ExeOwner));

    QSettings settings;
    // A quoted path plus a user-defined flag exercises the argv split.
    settings.setValue(QLatin1String("downloadmanager/external"), true);
    settings.setValue(QLatin1String("downloadmanager/externalPath"),
        QStringLiteral("\"%1\" --record-flag").arg(scriptPath));

    // An allowed scheme launches only the configured program; the url
    // arrives percent-encoded as exactly one trailing argv element.
    QVERIFY(DownloadManager::externalDownload(
        QUrl(QLatin1String("https://example.com/a b.zip"))));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(recordPath), 10000);
    {
        QFile record(recordPath);
        QVERIFY(record.open(QIODevice::ReadOnly));
        const QStringList argv = QString::fromUtf8(record.readAll())
            .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        QCOMPARE(argv.count(), 2);
        QCOMPARE(argv.at(0), QLatin1String("--record-flag"));
        QCOMPARE(argv.at(1),
                 QLatin1String("https://example.com/a%20b.zip"));
    }

    // Browser-internal and non-download schemes are refused outright —
    // the url falls back to the internal download path instead.
    QFile::remove(recordPath);
    const char *rejected[] = {
        "javascript:alert(1)",
        "file:///etc/passwd",
        "data:text/plain;base64,aGk=",
        "blob:https://example.com/id",
        "arora-resource:noop.js",
        "arora-file:///tmp",
        "smb://evil.example/share",
        "mailto:a@b.c",
    };
    for (const char *raw : rejected) {
        QVERIFY2(!DownloadManager::externalDownload(
                     QUrl(QLatin1String(raw))), raw);
    }
    QTest::qWait(500);
    QVERIFY(!QFile::exists(recordPath));

    // A disabled handler never launches, whatever the scheme.
    settings.setValue(QLatin1String("downloadmanager/external"), false);
    QVERIFY(!DownloadManager::externalDownload(
        QUrl(QLatin1String("https://example.com/file.zip"))));
    QTest::qWait(300);
    QVERIFY(!QFile::exists(recordPath));
#endif
}

// DOWN01: the detail card expands to show source/destination,
// timestamps, size and the speed graph; the compact row is unchanged
// while collapsed.
void tst_DownloadManager::cardDetails()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveDownload(&server, "text/plain",
                  "attachment; filename=\"card.txt\"");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/card.txt")
                       .arg(server.serverPort()));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, url);
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 30000);

        QList<DownloadItem*> items = manager.findChildren<DownloadItem*>();
        QCOMPARE(items.count(), 1);
        DownloadItem *item = items.first();
        QWidget *details = item->findChild<QWidget*>(
            QLatin1String("detailsWidget"));
        QVERIFY(details);

        // The finished row is the compact baseline — stop/progress
        // are hidden by then, so it is shorter than the live row.
        QTRY_VERIFY_WITH_TIMEOUT(item->downloadedSuccessfully(), 30000);
        QVERIFY(!item->isExpanded());
        QVERIFY(details->isHidden());
        const int compactHeight = view->rowHeight(0);
        item->setExpanded(true);
        QVERIFY(item->isExpanded());
        QVERIFY(!details->isHidden());
        QTRY_VERIFY(view->rowHeight(0) > compactHeight);
        QToolButton *chevron = item->findChild<QToolButton*>(
            QLatin1String("expandButton"));
        QVERIFY(chevron);
        QVERIFY(chevron->isChecked());
        item->setExpanded(false);
        QVERIFY(details->isHidden());
        QCOMPARE(view->rowHeight(0), item->sizeHint().height());
        chevron->click();
        QVERIFY(item->isExpanded());

        // The card carries the untrusted strings (full text lives in
        // the tooltip — the SqueezeLabel elides its own text()).
        SqueezeLabel *source = item->findChild<SqueezeLabel*>(
            QLatin1String("sourceLabel"));
        SqueezeLabel *destination = item->findChild<SqueezeLabel*>(
            QLatin1String("destinationLabel"));
        QVERIFY(source);
        QVERIFY(destination);
        QVERIFY(source->toolTip().contains(QLatin1String("card.txt")));
        QVERIFY(destination->toolTip().contains(QLatin1String("card.txt")));

        QVERIFY(item->startedTime().isValid());
        QVERIFY(item->finishedTime().isValid());
        QVERIFY(item->finishedTime() >= item->startedTime());
        QLabel *finished = item->findChild<QLabel*>(
            QLatin1String("finishedLabel"));
        QVERIFY(finished);
        QVERIFY(!finished->text().endsWith(QLatin1String("-")));

        // A finished card offers Restart and the folder reveal.
        QPushButton *restart = item->findChild<QPushButton*>(
            QLatin1String("restartButton"));
        QPushButton *showInFolder = item->findChild<QPushButton*>(
            QLatin1String("showInFolderButton"));
        QVERIFY(restart);
        QVERIFY(showInFolder);
        QVERIFY(restart->isEnabled());
        QVERIFY(showInFolder->isEnabled());
    }
}

// The card's Restart re-issues the url for a completed download —
// the retry dedups onto a "-1" name since the first file is kept.
void tst_DownloadManager::cardRestart()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveDownload(&server, "text/plain",
                  "attachment; filename=\"restart.txt\"");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/restart.txt")
                       .arg(server.serverPort()));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, url);
        QTRY_VERIFY_WITH_TIMEOUT(
            QFile::exists(downloadDir.path()
                          + QLatin1String("/restart.txt")), 30000);

        QList<DownloadItem*> items = manager.findChildren<DownloadItem*>();
        QCOMPARE(items.count(), 1);
        DownloadItem *item = items.first();
        QTRY_VERIFY(item->downloadedSuccessfully());
        const QDateTime firstStart = item->startedTime();

        QPushButton *restart = item->findChild<QPushButton*>(
            QLatin1String("restartButton"));
        QVERIFY(restart);
        QVERIFY(restart->isEnabled());
        QVERIFY(QMetaObject::invokeMethod(item, "restart"));
        QTRY_VERIFY_WITH_TIMEOUT(
            QFile::exists(downloadDir.path()
                          + QLatin1String("/restart-1.txt")), 30000);
        // The restart ran through the normal request flow and
        // re-attached to the same item with a fresh start time.
        QTRY_VERIFY(item->downloadedSuccessfully());
        QVERIFY(item->startedTime() >= firstStart);
    }
    // The first download is untouched.
    QFile original(downloadDir.path() + QLatin1String("/restart.txt"));
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), QByteArray("file contents"));
}

// The sparkline samples instantaneous speed while in flight and
// freezes (with a finish timestamp) when the download ends.
void tst_DownloadManager::speedSeries()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveStalledDownload(&server, "speed.bin");
    const QUrl stalledUrl(QString::fromLatin1("http://127.0.0.1:%1/speed.bin")
                              .arg(server.serverPort()));
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, stalledUrl);
        QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 30000);

        DownloadItem *item = manager.findChildren<DownloadItem*>().first();
        QVERIFY(item->downloading());
        // ~2 samples/sec against the 50ms trickle.
        QTRY_VERIFY_WITH_TIMEOUT(item->speedSampleCount() >= 2, 15000);
        DownloadGraph *graph = item->findChild<DownloadGraph*>(
            QLatin1String("downloadGraph"));
        QVERIFY(graph);
        QCOMPARE(graph->sampleCount(), item->speedSampleCount());

        QLabel *speed = item->findChild<QLabel*>(
            QLatin1String("speedLabel"));
        QVERIFY(speed);
        QVERIFY(speed->text().contains(QLatin1String("/s")));

        // Cancelling freezes the series and stamps the finish time.
        QMetaObject::invokeMethod(item, "stop");
        QTRY_VERIFY(!item->downloading());
        QVERIFY(item->finishedTime().isValid());
        const int frozen = item->speedSampleCount();
        QTest::qWait(700);
        QCOMPARE(item->speedSampleCount(), frozen);
        QCOMPARE(graph->sampleCount(), frozen);
    }
}

// A restored item shows the persisted url/path/size/timestamps and
// omits the graph — there is no live series to draw.
void tst_DownloadManager::restoredCard()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveDownload(&server, "text/plain",
                  "attachment; filename=\"restored.txt\"");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/restored.txt")
                       .arg(server.serverPort()));
    QDateTime finished;
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QWebEnginePage *page = manager.retryPage(false);
        manager.download(page, url);
        QTRY_VERIFY_WITH_TIMEOUT(
            QFile::exists(downloadDir.path()
                          + QLatin1String("/restored.txt")), 30000);
        DownloadItem *item = manager.findChildren<DownloadItem*>().first();
        QTRY_VERIFY(item->finishedTime().isValid());
        finished = item->finishedTime();
    }

    SubDownloadManager manager;
    QList<DownloadItem*> items = manager.findChildren<DownloadItem*>();
    QCOMPARE(items.count(), 1);
    DownloadItem *item = items.first();
    QVERIFY(item->startedTime().isValid());
    QCOMPARE(item->finishedTime(), finished);

    item->setExpanded(true);
    QWidget *graph = item->findChild<QWidget*>(
        QLatin1String("downloadGraph"));
    QVERIFY(graph);
    QVERIFY(graph->isHidden());
    SqueezeLabel *destination = item->findChild<SqueezeLabel*>(
        QLatin1String("destinationLabel"));
    QVERIFY(destination);
    QVERIFY(destination->toolTip().contains(QLatin1String("restored.txt")));
    QLabel *size = item->findChild<QLabel*>(QLatin1String("sizeLabel"));
    QVERIFY(size);
    QVERIFY(size->text().contains(QLatin1String("bytes")));
    // Restart is offered for a restored card too.
    QPushButton *restart = item->findChild<QPushButton*>(
        QLatin1String("restartButton"));
    QVERIFY(restart);
    QVERIFY(restart->isEnabled());
}

QTEST_MAIN(tst_DownloadManager)
#include "tst_downloadmanager.moc"
