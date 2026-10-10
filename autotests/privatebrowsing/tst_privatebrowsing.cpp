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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */

// SEC07: private browsing is a per-profile property under Qt
// WebEngine — pages created while BrowserApplication::isPrivate() is
// on live on BrowserProfile::privateProfile(), an unnamed (hence
// off-the-record) profile.  These tests assert the app-side stores
// honour that boundary: nothing a private page does reaches history,
// the session file, the persisted download list, recent searches, or
// the persistent profile's cookies — while protections like adblock
// still apply.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qbuffer.h>
#include <qimage.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <memory>

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockrule.h"
#include "adblocksubscription.h"
#include "browserapplication.h"
#include "browserprofile.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "historymanager.h"
#include "opensearchmanager.h"
#include "qtest_arora.h"
#include "qtry.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "webpage.h"
#include "webview.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

// Minimal HTTP responder that records every request target — blocked
// requests never appear here — and answers 200 with a type-appropriate
// body.  setCookie, when set, rides along on every response.
class LocalHttpServer : public QObject
{
    Q_OBJECT

public:
    LocalHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                respond(socket, socket->readAll());
            });
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url(const QString &path) const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1%2")
                    .arg(m_server.serverPort()).arg(path));
    }

    QStringList requests;
    QByteArray indexHtml;
    QByteArray setCookie;

private:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArray target = request.split(' ').value(1);
        requests.append(QString::fromUtf8(target));

        QByteArray mimeType = "text/plain";
        QByteArray body = "ok\n";
        if (target.contains(".png")) {
            mimeType = "image/png";
            body = pngBody();
        } else if (target.contains(".html") || target == "/") {
            mimeType = "text/html";
            body = indexHtml.isEmpty()
                ? QByteArray("<html><body>ok</body></html>") : indexHtml;
        }

        QByteArray response = "HTTP/1.0 200 OK\r\nContent-Type: " + mimeType
            + "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n";
        if (!setCookie.isEmpty())
            response += "Set-Cookie: " + setCookie + "\r\n";
        response += "Connection: close\r\n\r\n" + body;
        socket->write(response);
        socket->disconnectFromHost();
    }

    static QByteArray pngBody()
    {
        static const QByteArray body = []() {
            QImage image(1, 1, QImage::Format_ARGB32);
            image.fill(Qt::transparent);
            QByteArray out;
            QBuffer buffer(&out);
            buffer.open(QIODevice::WriteOnly);
            image.save(&buffer, "PNG");
            return out;
        }();
        return body;
    }

    QTcpServer m_server;
};

// Serves headers for a huge body that trickles in one byte at a time,
// keeping the download in-flight for as long as the test needs it.
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
        // Chromium only raises downloadRequested once body bytes
        // start arriving, so trickle a byte at a time.
        QTimer *trickle = new QTimer(socket);
        QObject::connect(trickle, &QTimer::timeout, socket, [socket]() {
            socket->write("x");
        });
        trickle->start(50);
    });
}

static bool waitFor(const std::shared_ptr<bool> &flag, int timeout = 15000)
{
    for (int waited = 0; !*flag && waited < timeout; waited += 50)
        QTest::qWait(50);
    return *flag;
}

static bool loadSync(QWebEnginePage *page, const QUrl &url)
{
    std::shared_ptr<bool> done(new bool(false));
    std::shared_ptr<bool> ok(new bool(false));
    QMetaObject::Connection connection = QObject::connect(
        page, &QWebEnginePage::loadFinished, page,
        [done, ok](bool result) { *done = true; *ok = result; });
    page->load(url);
    waitFor(done);
    QObject::disconnect(connection);
    return *ok;
}

// save() is a private slot on both widgets under test; the AutoSaver
// reaches it through the meta-object, and so do we.
static void forceSave(QObject *object)
{
    QVERIFY(QMetaObject::invokeMethod(object, "save"));
}

class tst_PrivateBrowsing : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void privateProfileSelection();
    void otrPageDoesNotFeedHistory();
    void otrTabsExcludedFromSaveState();
    void otrTabSkippedByRecentlyClosed();
    void privateSearchNotRecorded();
    void otrDownloadNotPersisted();
    void otrCookiesIsolated();
    void adblockAppliesOnOtrProfile();

private:
    LocalHttpServer *m_server;
    AdBlockSubscription *m_subscription;
};

void tst_PrivateBrowsing::initTestCase()
{
    QCoreApplication::setApplicationName("tst_privatebrowsing");
    QStandardPaths::setTestModeEnabled(true);

    QSettings settings;
    settings.clear();
    // A dead local list keeps AdBlockManager::load() off the network.
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-sec07.txt"
            "&title=DeadList"));
    AdBlockManager::instance()->setEnabled(true);
    ToolbarSearch::openSearchManager()->restoreDefaults();

    m_server = new LocalHttpServer(this);
    QVERIFY(m_server->start());
    m_subscription = nullptr;
}

void tst_PrivateBrowsing::init()
{
    QSettings settings;
    settings.clear();
#if defined(ARORA_RUSTCORE)
    // Per-site decisions outlive the QSettings wipe — start clean.
    SiteDecisionStore::reset();
#endif
    m_server->requests.clear();
    m_server->indexHtml.clear();
    m_server->setCookie.clear();
}

void tst_PrivateBrowsing::cleanup()
{
    BrowserApplication::setPrivate(false);
    if (m_subscription)
        AdBlockManager::instance()->removeSubscription(m_subscription);
    m_subscription = nullptr;
}

// Private browsing hands out a dedicated off-the-record profile; tabs
// created while it is on live there, tabs created after do not.
void tst_PrivateBrowsing::privateProfileSelection()
{
    QVERIFY(!BrowserApplication::webEngineProfile()->isOffTheRecord());

    BrowserApplication::setPrivate(true);
    QWebEngineProfile *otr = BrowserApplication::webEngineProfile();
    QVERIFY(otr->isOffTheRecord());
    QCOMPARE(otr, BrowserProfile::privateProfile());

    TabWidget widget;
    widget.newTab();
    QVERIFY(widget.webView(0));
    QVERIFY(widget.webView(0)->page()->profile()->isOffTheRecord());

    BrowserApplication::setPrivate(false);
    widget.newTab();
    QVERIFY(widget.webView(1));
    QVERIFY(!widget.webView(1)->page()->profile()->isOffTheRecord());
}

// The app-side history feed in WebPage::init is only wired for pages
// on a persistent profile — a private load must leave no entry.
void tst_PrivateBrowsing::otrPageDoesNotFeedHistory()
{
    const QString otrUrl = m_server->url(QLatin1String("/sec07-otr-visit.html"))
                               .toString();
    WebPage otrPage(BrowserProfile::privateProfile());
    QVERIFY(otrPage.profile()->isOffTheRecord());
    QVERIFY(loadSync(&otrPage, QUrl(otrUrl)));
    // The feed is a direct signal connection — by the time the load
    // finished the entry would already be recorded if it were coming.
    QTest::qWait(200);
    QVERIFY(!HistoryManager::instance()->historyContains(otrUrl));

    const QString normalUrl = m_server->url(QLatin1String("/sec07-normal-visit.html"))
                                  .toString();
    WebPage normalPage(BrowserProfile::normalProfile());
    QVERIFY(!normalPage.profile()->isOffTheRecord());
    QVERIFY(loadSync(&normalPage, QUrl(normalUrl)));
    QTRY_VERIFY(HistoryManager::instance()->historyContains(normalUrl));
}

// Decodes a TabWidget::saveState() blob back into every url it
// recorded — the top-level tab list plus the current entry of each
// per-tab serialized history (QDataStream stores QStrings as UTF-16,
// so byte-level substring checks cannot see them).
static QStringList urlsInState(const QByteArray &state)
{
    QStringList urls;
    QDataStream stream(state);
    qint32 marker = 0, version = 0, currentTab = -1;
    QStringList tabs;
    QList<QByteArray> histories;
    stream >> marker >> version >> tabs >> currentTab >> histories;
    if (marker != 0xaa || stream.status() != QDataStream::Ok)
        return urls;
    urls += tabs;
    for (const QByteArray &blob : histories) {
        QDataStream historyStream(blob);
        qint32 historyVersion = 0, current = -1;
        QStringList historyUrls;
        historyStream >> historyVersion >> historyUrls >> current;
        urls += historyUrls;
    }
    return urls;
}

// Session serialization drops off-the-record tabs entirely: the blob
// holds no private url in either the tab list or the per-tab history.
void tst_PrivateBrowsing::otrTabsExcludedFromSaveState()
{
    TabWidget widget;
    widget.newTab();
    const QUrl normalUrl = m_server->url(QLatin1String("/sec07-tab-normal.html"));
    widget.loadUrl(normalUrl, TabWidget::CurrentTab);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), normalUrl, 15000);

    BrowserApplication::setPrivate(true);
    widget.newTab();
    QVERIFY(widget.webView(1)->page()->profile()->isOffTheRecord());
    const QUrl otrUrl = m_server->url(QLatin1String("/sec07-tab-private-marker.html"));
    widget.loadUrl(otrUrl, TabWidget::CurrentTab);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), otrUrl, 15000);
    BrowserApplication::setPrivate(false);

    const QString joined = urlsInState(widget.saveState())
                               .join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QLatin1String("sec07-tab-normal")));
    QVERIFY(!joined.contains(QLatin1String("sec07-tab-private-marker")));

    // A window of nothing-but-private tabs serializes an empty list.
    TabWidget privateOnly;
    BrowserApplication::setPrivate(true);
    privateOnly.newTab();
    QVERIFY(privateOnly.webView(0)->page()->profile()->isOffTheRecord());
    BrowserApplication::setPrivate(false);
    const QByteArray empty = privateOnly.saveState();
    QVERIFY(!empty.isEmpty()); // magic + version + empty lists still write
    QCOMPARE(urlsInState(empty).count(), 0);
}

// A closed private tab is not queued for reopen — "Open Last Closed
// Tab" would load the url in a normal-profile page where the visit
// would then be recorded.
void tst_PrivateBrowsing::otrTabSkippedByRecentlyClosed()
{
    TabWidget widget;
    BrowserApplication::setPrivate(true);
    widget.newTab();
    const QUrl otrUrl = m_server->url(QLatin1String("/sec07-closed-private.html"));
    widget.loadUrl(otrUrl, TabWidget::CurrentTab);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), otrUrl, 15000);
    widget.closeTab(0);
    QVERIFY(!widget.recentlyClosedTabsAction()->isEnabled());
    BrowserApplication::setPrivate(false);

    widget.newTab();
    const QUrl normalUrl = m_server->url(QLatin1String("/sec07-closed-normal.html"));
    widget.loadUrl(normalUrl, TabWidget::CurrentTab);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), normalUrl, 15000);
    widget.closeTab(0);
    QVERIFY(widget.recentlyClosedTabsAction()->isEnabled());
}

// A search whose target is private is never appended to the persisted
// recent-searches list — both when the bound view is off-the-record
// and when the app flag is on with no view wired.
void tst_PrivateBrowsing::privateSearchNotRecorded()
{
    WebView otrView(BrowserProfile::privateProfile());
    QVERIFY(otrView.page()->profile()->isOffTheRecord());
    {
        ToolbarSearch search;
        search.setWebView(&otrView);
        search.setText(QLatin1String("sec07-otr-search"));
        search.searchNow();
    }   // ~ToolbarSearch flushes the AutoSaver through save()
    {
        QSettings settings;
        QVERIFY(!settings.value(QLatin1String("toolbarsearch/recentSearches"))
                     .toStringList().contains(QLatin1String("sec07-otr-search")));
    }

    BrowserApplication::setPrivate(true);
    {
        ToolbarSearch search;
        search.setText(QLatin1String("sec07-flag-search"));
        search.searchNow();
    }
    BrowserApplication::setPrivate(false);
    {
        QSettings settings;
        QVERIFY(!settings.value(QLatin1String("toolbarsearch/recentSearches"))
                     .toStringList().contains(QLatin1String("sec07-flag-search")));
    }

    // Control: a normal search is still remembered.
    {
        ToolbarSearch search;
        search.setText(QLatin1String("sec07-normal-search"));
        search.searchNow();
    }
    {
        QSettings settings;
        QVERIFY(settings.value(QLatin1String("toolbarsearch/recentSearches"))
                    .toStringList().contains(QLatin1String("sec07-normal-search")));
    }
}

// The persisted download list never holds an off-the-record item: an
// in-flight private download must be filtered out of save().
void tst_PrivateBrowsing::otrDownloadNotPersisted()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    serveStalledDownload(&server, "sec07-private.bin");
    const QUrl url(QString::fromLatin1("http://127.0.0.1:%1/sec07-private.bin")
                       .arg(server.serverPort()));

    // A stack manager keeps the items away from the app singleton.
    DownloadManager manager;
    manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
    QTableView *view = manager.findChild<QTableView*>();
    QVERIFY(view);

    QWebEnginePage *otrPage = manager.retryPage(true);
    QVERIFY(otrPage->profile()->isOffTheRecord());
    manager.download(otrPage, url);
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 1, 30000);

    forceSave(&manager);
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("downloadmanager"));
        QVERIFY(!settings.contains(QLatin1String("download_0_url")));
    }

    // Control: a normal-profile download under the same policy is
    // persisted — and lands on index 0 because the private item is
    // filtered before numbering.
    QTcpServer normalServer;
    QVERIFY(normalServer.listen(QHostAddress::LocalHost));
    serveStalledDownload(&normalServer, "sec07-normal.bin");
    const QUrl normalUrl(QString::fromLatin1("http://127.0.0.1:%1/sec07-normal.bin")
                             .arg(normalServer.serverPort()));
    QWebEnginePage *normalPage = manager.retryPage(false);
    QVERIFY(!normalPage->profile()->isOffTheRecord());
    manager.download(normalPage, normalUrl);
    QTRY_COMPARE_WITH_TIMEOUT(view->model()->rowCount(), 2, 30000);

    forceSave(&manager);
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    QCOMPARE(settings.value(QLatin1String("download_0_url")).toUrl(), normalUrl);
    QVERIFY(!settings.contains(QLatin1String("download_1_url")));
}

// Cookies a private page receives stay inside the throwaway
// off-the-record store — the persistent profile's jar never sees them.
void tst_PrivateBrowsing::otrCookiesIsolated()
{
    QWebEngineProfile *otrProfile = BrowserProfile::privateProfile();
    CookieJar *otrJar = CookieJar::instance(otrProfile);
    CookieJar *normalJar = CookieJar::instance(BrowserProfile::normalProfile());
    QVERIFY(otrJar->isPrivate());
    QVERIFY(!normalJar->isPrivate());

    const QString cookieName = QLatin1String("sec07-otr-marker");
    m_server->setCookie = cookieName.toUtf8() + "=1; Path=/";

    WebPage otrPage(otrProfile);
    QVERIFY(loadSync(&otrPage, m_server->url(QLatin1String("/sec07-cookie.html"))));

    const auto hasCookie = [](CookieJar *jar, const QString &name) {
        const QList<QNetworkCookie> cookies = jar->cookies();
        for (const QNetworkCookie &cookie : cookies)
            if (cookie.name() == name.toUtf8())
                return true;
        return false;
    };
    QTRY_VERIFY(hasCookie(otrJar, cookieName));
    QVERIFY(!hasCookie(normalJar, cookieName));
}

// The adblock interceptor that prepareProfile() installs on the
// off-the-record profile blocks subresources exactly like the normal
// one — private pages stay protected.
void tst_PrivateBrowsing::adblockAppliesOnOtrProfile()
{
    AdBlockManager *manager = AdBlockManager::instance();
    m_subscription = new AdBlockSubscription(QUrl(), manager);
    m_subscription->setEnabled(true);
    manager->addSubscription(m_subscription);
    AdBlockRule rule(QLatin1String("/sec07-blocked.png"));
    rule.setEnabled(true);
    m_subscription->addRule(rule);
    manager->network()->rebuildRules();

    BrowserApplication::setPrivate(true);
    QWebEngineProfile *otr = BrowserApplication::webEngineProfile();
    QVERIFY(otr->isOffTheRecord());

    m_server->indexHtml =
        "<html><body><img src=\"/sec07-blocked.png\"></body></html>";
    WebPage page(otr);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/sec07-index.html"))));

    QVERIFY(m_server->requests.contains(QLatin1String("/sec07-index.html")));
    QVERIFY(!m_server->requests.contains(QLatin1String("/sec07-blocked.png")));
    BrowserApplication::setPrivate(false);
}

QTEST_MAIN(tst_PrivateBrowsing)
#include "tst_privatebrowsing.moc"
