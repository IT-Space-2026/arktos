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

// COV03: end-to-end coverage for AdBlockRequestInterceptor — the
// profile's QWebEngineUrlRequestInterceptor runs match() on Chromium's
// IO thread, so the only honest exercise is a real page fetch through
// a real profile.  An in-process HTTP server records which requests
// actually reach the network; subresource success/failure is read back
// through the DOM.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qbuffer.h>
#include <qimage.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebengineloadinginfo.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <memory>

#include "adblockmanager.h"
#include "adblocknetwork.h"
#include "adblockrequestinterceptor.h"
#include "adblockrule.h"
#include "adblocksubscription.h"
#include "schemeaccesshandler.h"
#include "adblockschemeaccesshandler.h"
#include "adblockresourcehandler.h"
#include "webpage.h"
#include "browserapplication.h"
#include "qtry.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

// Minimal HTTP/1.0 responder: records every request target it sees and
// answers 200 with a type-appropriate canned body.  Requests the
// interceptor blocks or redirects never show up here, which is the
// observable evidence the tests assert on.
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
                // Wait for the full header block before answering.
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                const QByteArray request = socket->readAll();
                respond(socket, request);
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

private slots:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArray target = request.split(' ').value(1);
        requests.append(QString::fromUtf8(target));

        QByteArray mimeType = "text/plain";
        QByteArray body = "ok\n";
        if (target.contains(".png")) {
            mimeType = "image/png";
            body = pngBody();
        } else if (target.contains(".gif")) {
            mimeType = "image/gif";
            body = QByteArray::fromBase64("R0lGODlhAQABAIAAAP///////yH5"
                "BAEKAAEALAAAAAABAAEAAAICTAEAOw==");
        } else if (target.contains(".js")) {
            mimeType = "application/javascript";
            body = "window.aroraJsLoaded = (window.aroraJsLoaded || 0) + 1;\n";
        } else if (target.contains(".html") || target == "/") {
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

private:
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

// WebPage replaces failed navigations with its not-found page, whose
// own load then reports loadFinished(true) — so a refused main frame
// is detected through loadingChanged(LoadFailed), not loadFinished.
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

class tst_AdBlockRequestInterceptor : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();
    void cleanup();

    void blockExceptionAndAllow();
    void redirectToStub();
    void redirectUnknownStub();
    void removeParam();
    void mainFrameBlock();
    void blockedCountTracking();
    void webSchemeGate();
    void selfRedirectTerminates();

private:
    // Feeds the given filters into a fresh subscription on the shared
    // manager, then forces the IO-thread matcher snapshot to rebuild.
    void addRules(const QStringList &filters);

    QWebEngineProfile *m_profile;
    LocalHttpServer *m_server;
    AdBlockSubscription *m_subscription;
};

void tst_AdBlockRequestInterceptor::addRules(const QStringList &filters)
{
    AdBlockManager *manager = AdBlockManager::instance();
    m_subscription = new AdBlockSubscription(QUrl(), manager);
    m_subscription->setEnabled(true);
    manager->addSubscription(m_subscription);
    for (const QString &filter : filters) {
        AdBlockRule rule(filter);
        rule.setEnabled(true);
        m_subscription->addRule(rule);
    }
    // rulesChanged already chains into this via the manager's
    // connect()s; rebuild anyway so the snapshot is settled before the
    // first request hits the IO thread.
    manager->network()->rebuildRules();
}

void tst_AdBlockRequestInterceptor::initTestCase()
{
    QCoreApplication::setApplicationName("tst_adblockrequestinterceptor");
    QStandardPaths::setTestModeEnabled(true);

    QSettings settings;
    settings.clear();
    // Dead local list keeps load() away from the live defaults.
    settings.setValue(QLatin1String("AdBlock/subscriptions"),
        QStringList() << QLatin1String(
            "abp:subscribe?location=file%3A%2F%2Fnonexistent-cov03.txt"
            "&title=DeadList"));
    AdBlockManager::instance()->setEnabled(true);

    m_server = new LocalHttpServer(this);
    QVERIFY(m_server->start());

    m_profile = new QWebEngineProfile(this);
    // Installs the interceptor bound to the manager's matcher plus the
    // abp: and arora-resource:// handlers on this throwaway profile.
    AdBlockManager::instance()->installOnProfile(m_profile);
}

void tst_AdBlockRequestInterceptor::init()
{
    m_server->requests.clear();
    m_server->indexHtml.clear();
#if defined(ARORA_RUSTCORE)
    // The site whitelist moved into the decision store — clear it so
    // a stale row cannot silently allow every request.
    SiteDecisionStore::clear(SiteDecisionStore::KindAdBlock);
#endif
}

void tst_AdBlockRequestInterceptor::cleanup()
{
    if (m_subscription)
        AdBlockManager::instance()->removeSubscription(m_subscription);
    m_subscription = nullptr;
}

// Subresource blocking: the request never reaches the network and the
// element's load fails; an @@ exception rescues an otherwise-matching
// URL.
void tst_AdBlockRequestInterceptor::blockExceptionAndAllow()
{
    addRules(QStringList()
             << QLatin1String("/blocked.png")
             << QLatin1String("except")
             << QLatin1String("@@/excepted.png"));

    m_server->indexHtml =
        "<html><body>"
        "<img id=\"ok\" src=\"/ok.png\">"
        "<img id=\"bad\" src=\"/blocked.png\">"
        "<img id=\"exc\" src=\"/excepted.png\">"
        "</body></html>";

    WebPage page(m_profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/index.html"))));

    // The document itself and the allowed/excepted images hit the
    // server; the blocked one does not.
    QVERIFY(m_server->requests.contains(QLatin1String("/index.html")));
    QVERIFY(m_server->requests.contains(QLatin1String("/ok.png")));
    QVERIFY(m_server->requests.contains(QLatin1String("/excepted.png")));
    QVERIFY(!m_server->requests.contains(QLatin1String("/blocked.png")));

    // naturalWidth is only nonzero when the image actually decoded.
    const QVariant widths = evalSync(&page, QLatin1String(
        "String(document.getElementById('ok').naturalWidth) + ',' +"
        "document.getElementById('bad').naturalWidth + ',' +"
        "document.getElementById('exc').naturalWidth"));
    QCOMPARE(widths.toString(), QLatin1String("1,0,1"));
}

// $redirect=swap: the request is rewritten to a bundled arora-resource
// stub — it succeeds in the page without ever touching the network.
void tst_AdBlockRequestInterceptor::redirectToStub()
{
    addRules(QStringList()
             << QLatin1String("/ad.js$script,redirect=noop.js"));

    m_server->indexHtml =
        "<html><body>"
        "<script src=\"/ad.js\" "
        "onload=\"window.__adState='ok'\" "
        "onerror=\"window.__adState='err'\"></script>"
        "</body></html>";

    WebPage page(m_profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/index.html"))));

    QVERIFY(!m_server->requests.contains(QLatin1String("/ad.js")));
    QCOMPARE(evalSync(&page, QLatin1String(
        "String(window.__adState || 'none')")).toString(),
        QLatin1String("ok"));
}

// An unknown stub name cannot be served — the interceptor falls back
// to a plain block.
void tst_AdBlockRequestInterceptor::redirectUnknownStub()
{
    addRules(QStringList()
             << QLatin1String(
                 "/mystery.js$script,redirect=nonexistent-stub-name"));

    m_server->indexHtml =
        "<html><body>"
        "<script src=\"/mystery.js\" "
        "onload=\"window.__mState='ok'\" "
        "onerror=\"window.__mState='err'\"></script>"
        "</body></html>";

    WebPage page(m_profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/index.html"))));

    QVERIFY(!m_server->requests.contains(QLatin1String("/mystery.js")));
    QCOMPARE(evalSync(&page, QLatin1String(
        "String(window.__mState || 'none')")).toString(),
        QLatin1String("err"));
}

// $removeparam rewrites the query on the way out; the server only ever
// sees the stripped URL.
void tst_AdBlockRequestInterceptor::removeParam()
{
    addRules(QStringList()
             << QLatin1String("/track$removeparam=utm_source")
             << QLatin1String("trackre$removeparam=/^utm_/")
             << QLatin1String("/trackall$removeparam"));

    m_server->indexHtml =
        "<html><body>"
        "<img id=\"t1\" src=\"/track?a=1&utm_source=x&keep=2\">"
        "<img id=\"t2\" src=\"/trackre?utm_medium=x&keep=3\">"
        "<img id=\"t3\" src=\"/trackall?x=1&y=2\">"
        "</body></html>";

    WebPage page(m_profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/index.html"))));

    QString track, trackre, trackall;
    for (const QString &request : m_server->requests) {
        if (request.startsWith(QLatin1String("/track?")))
            track = request;
        else if (request.startsWith(QLatin1String("/trackre")))
            trackre = request;
        else if (request.startsWith(QLatin1String("/trackall")))
            trackall = request;
    }
    QVERIFY(track.contains(QLatin1String("keep=2")));
    QVERIFY(track.contains(QLatin1String("a=1")));
    QVERIFY(!track.contains(QLatin1String("utm_source")));
    // The regex spec strips /^utm_/ but leaves other params.
    QVERIFY(trackre.contains(QLatin1String("keep=3")));
    QVERIFY(!trackre.contains(QLatin1String("utm_medium")));
    // Bare $removeparam strips everything.
    QVERIFY(!trackall.contains(QLatin1Char('?')));
}

// A matching document navigation is refused outright.
void tst_AdBlockRequestInterceptor::mainFrameBlock()
{
    addRules(QStringList() << QLatin1String("/mainblock"));

    WebPage page(m_profile);
    QVERIFY(loadAndFail(&page, m_server->url(QLatin1String("/mainblock"))));
    QVERIFY(!m_server->requests.contains(QLatin1String("/mainblock")));
}

// ADB05: intercepted requests bump the per-first-party-host tally the
// location-bar AdBlockButton diffs against its load-start baseline —
// plain blocks and stub redirects count, allowed requests do not.
void tst_AdBlockRequestInterceptor::blockedCountTracking()
{
    addRules(QStringList()
             << QLatin1String("/counted.png")
             << QLatin1String("/stubbed.js$script,redirect=noop.js"));

    m_server->indexHtml =
        "<html><body>"
        "<img src=\"/counted.png\">"
        "<img src=\"/allowed.png\">"
        "<script src=\"/stubbed.js\"></script>"
        "</body></html>";

    const int before = AdBlockRequestInterceptor::blockedRequestCount(
        QLatin1String("127.0.0.1"));
    WebPage page(m_profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/index.html"))));

    QVERIFY(!m_server->requests.contains(QLatin1String("/counted.png")));
    QVERIFY(!m_server->requests.contains(QLatin1String("/stubbed.js")));
    QVERIFY(m_server->requests.contains(QLatin1String("/allowed.png")));
    QTRY_VERIFY(AdBlockRequestInterceptor::blockedRequestCount(
        QLatin1String("127.0.0.1")) >= before + 2);
}

// STALL01: only real web schemes may reach the matcher — internal
// and non-web schemes (arora-resource:, abp:, devtools:, qrc:,
// arora-file:, arora-cert-error:, chrome:, data:, file:, ...) bypass
// interception untouched so a stub redirect target can never re-enter
// the rule engine, and filter lists cannot swallow browser internals.
void tst_AdBlockRequestInterceptor::webSchemeGate()
{
    typedef AdBlockRequestInterceptor I;
    QVERIFY(I::isWebRequestScheme(QLatin1String("http")));
    QVERIFY(I::isWebRequestScheme(QLatin1String("https")));
    QVERIFY(I::isWebRequestScheme(QLatin1String("ws")));
    QVERIFY(I::isWebRequestScheme(QLatin1String("wss")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("arora-resource")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("arora-file")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("arora-cert-error")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("abp")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("devtools")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("chrome")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("qrc")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("data")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("about")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("file")));
    QVERIFY(!I::isWebRequestScheme(QLatin1String("ftp")));
    QVERIFY(!I::isWebRequestScheme(QString()));
}

// STALL01: a global *$script redirect points every script at
// arora-resource:/noop.js — a URL that itself matches *$script.
// Without the internal-scheme gate the stub request re-enters the
// matcher and re-redirects, churning until the load starves; with it,
// the stub is served and the load terminates.
void tst_AdBlockRequestInterceptor::selfRedirectTerminates()
{
    addRules(QStringList()
             << QLatin1String("*$script,redirect=noop.js"));

    m_server->indexHtml =
        "<html><body>"
        "<script src=\"/loop.js\" "
        "onload=\"window.__loopState='ok'\" "
        "onerror=\"window.__loopState='err'\"></script>"
        "</body></html>";

    WebPage page(m_profile);
    QVERIFY(loadSync(&page, m_server->url(QLatin1String("/index.html"))));

    QVERIFY(!m_server->requests.contains(QLatin1String("/loop.js")));
    QCOMPARE(evalSync(&page, QLatin1String(
        "String(window.__loopState || 'none')")).toString(),
        QLatin1String("ok"));
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    // arora-resource:// must be registered before BrowserApplication's
    // constructor brings up the browsing profile (same order as
    // main.cpp).
    SchemeAccessHandler::registerUrlSchemes();
    AdBlockSchemeAccessHandler::registerUrlScheme();
    AdBlockResourceHandler::registerUrlScheme();
    BrowserApplication app(argc, argv);
    tst_AdBlockRequestInterceptor tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_adblockrequestinterceptor.moc"
