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
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>
#include "downloadmanager.h"
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
};

// Subclass that exposes the protected functions.
class SubDownloadManager : public DownloadManager
{
public:
    SubDownloadManager(QWidget *parent = 0)
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
    manager.download(0, QUrl());
    manager.handleDownloadRequested(0);
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
        QPushButton *tryAgainButton = 0;
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

QTEST_MAIN(tst_DownloadManager)
#include "tst_downloadmanager.moc"
