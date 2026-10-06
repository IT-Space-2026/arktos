/*
 * Copyright 2009 Benjamin C. Meyer <ben@meyerhome.net>
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

// TST01: ported from QtWebKit to QtWebEngine.  DOM access is
// asynchronous (runJavaScript callbacks), the plugin factory and
// NPAPI createPlugin hook don't exist under WebEngine, and
// acceptNavigationRequest takes (url, type, isMainFrame).

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <QtNetwork/QtNetwork>
#include <QtWebEngineWidgets>

#include <qstandardpaths.h>
#include <qwebchannel.h>

#include <autofillmanager.h>
#include <opensearchmanager.h>
#include <toolbarsearch.h>
#include <webpage.h>
#include <webview.h>
#include "qtest_arora.h"

class tst_WebPage : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Receiver for QDesktopServices::setUrlHandler so mailto:/ftp:
    // navigations don't escape the test process.
    void openUrl(const QUrl &url) { m_openedUrls.append(url); }

private slots:
    void webpage_data();
    void webpage();

    void loadSettings();
    void webPluginFactory();
    void acceptNavigationRequest_data();
    void acceptNavigationRequest();
    void externalProtocolPrompt();
    void createPlugin();
    void createWindow();
    void handleUnsupportedContent();
    void linkedResources();
    void javaScriptObjects();
    void webChannelStartPage();
    void webChannelForgedAutofillReport();
    void webChannelAddSearchProviderConsent();
    void webChannelAutofillCapture();
    void userAgent();

private:
    QVariant evalSync(QWebEnginePage *page, const QString &js);
    QList<QUrl> m_openedUrls;
};

// Minimal HTTP responder for the channel tests: "/" serves pageBody,
// "/engine.xml" the OpenSearch descriptor — hits are counted per path
// so the test can prove no fetch happened without consent.
class LocalHttpServer : public QObject
{
    Q_OBJECT

public:
    QByteArray pageBody;
    QByteArray engineBody;
    int pageHits = 0;
    int engineHits = 0;

    LocalHttpServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (m_server.hasPendingConnections()) {
                QTcpSocket *socket = m_server.nextPendingConnection();
                socket->setParent(&m_server);
                connect(socket, &QTcpSocket::readyRead, this,
                        [this, socket]() {
                    if (!socket->peek(4096).contains("\r\n\r\n"))
                        return;
                    const QByteArray request = socket->readAll();
                    const QByteArray path = request.split(' ').value(1);
                    QByteArray body;
                    if (path == QByteArrayLiteral("/engine.xml")) {
                        ++engineHits;
                        body = engineBody;
                    } else {
                        ++pageHits;
                        body = pageBody;
                    }
                    socket->write(QByteArrayLiteral(
                        "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                        "Content-Length: ")
                        + QByteArray::number(body.size())
                        + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                        + body);
                    socket->disconnectFromHost();
                });
            }
        });
    }

    bool start() { return m_server.listen(QHostAddress::LocalHost); }

    QUrl url() const
    {
        return QUrl(QString::fromLatin1("http://127.0.0.1:%1/")
                    .arg(m_server.serverPort()));
    }

private:
    QTcpServer m_server;
};

QVariant tst_WebPage::evalSync(QWebEnginePage *page, const QString &js)
{
    QVariant result;
    bool done = false;
    page->runJavaScript(js, [&](const QVariant &v) { result = v; done = true; });
    for (int waited = 0; !done && waited < 10000; waited += 50)
        QTest::qWait(50);
    return result;
}

// Subclass that exposes the protected functions.
class SubWebPage : public WebPage
{
public:
    void call_aboutToLoadUrl(QUrl const &url)
        { emit SubWebPage::aboutToLoadUrl(url); }

    bool call_acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
        { return SubWebPage::acceptNavigationRequest(url, type, isMainFrame); }

    QWebEnginePage *call_createWindow(QWebEnginePage::WebWindowType type)
        { return SubWebPage::createWindow(type); }
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_WebPage::initTestCase()
{
    QCoreApplication::setApplicationName("tst_webpage");
    QStandardPaths::setTestModeEnabled(true);

    QDesktopServices::setUrlHandler(QLatin1String("mailto"), this, "openUrl");
    QDesktopServices::setUrlHandler(QLatin1String("ftp"), this, "openUrl");
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_WebPage::cleanupTestCase()
{
    QDesktopServices::unsetUrlHandler(QLatin1String("mailto"));
    QDesktopServices::unsetUrlHandler(QLatin1String("ftp"));

    QSettings settings;
    settings.setValue("userAgent", QString());
}

// This will be called before each test function is executed.
void tst_WebPage::init()
{
}

// This will be called after every test function.
void tst_WebPage::cleanup()
{
}

void tst_WebPage::webpage_data()
{
}

void tst_WebPage::webpage()
{
    SubWebPage page;
    page.loadSettings();
    page.call_aboutToLoadUrl(QUrl());
    QCOMPARE(page.call_acceptNavigationRequest(QUrl(), QWebEnginePage::NavigationTypeLinkClicked, false), true);
    QWebEnginePage *newPage = page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    QVERIFY(newPage);
    delete QWebEngineView::forPage(newPage);
}

// public void loadSettings()
void tst_WebPage::loadSettings()
{
    SubWebPage page;

    QSignalSpy spy0(&page, SIGNAL(aboutToLoadUrl(QUrl)));

    page.loadSettings();

    QCOMPARE(spy0.count(), 0);
}

// The QtWebKit WebPluginFactory/NPAPI hooks have no WebEngine
// equivalent; plugins are handled entirely inside Chromium.
void tst_WebPage::webPluginFactory()
{
    QSKIP("WebPluginFactory is a QtWebKit-only API.", SkipAll);
}

Q_DECLARE_METATYPE(QWebEnginePage::NavigationType)
Q_DECLARE_METATYPE(Qt::MouseButton)
Q_DECLARE_METATYPE(Qt::KeyboardModifier)
void tst_WebPage::acceptNavigationRequest_data()
{
    QTest::addColumn<Qt::MouseButton>("pressedButton");
    QTest::addColumn<Qt::KeyboardModifier>("pressedKeys");
    QTest::addColumn<bool>("isMainFrame");
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<QWebEnginePage::NavigationType>("type");
    QTest::addColumn<bool>("acceptNavigationRequest");
    QTest::addColumn<int>("spyCount");
    QTest::addColumn<bool>("externalPrompt");

    QTest::newRow("null-noframe") << Qt::NoButton << Qt::NoModifier << false << QUrl() << QWebEnginePage::NavigationTypeLinkClicked << true << 0 << false;
    QTest::newRow("null-frame")   << Qt::NoButton << Qt::NoModifier << true << QUrl() << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;

    // SEC02: external protocols are denied in-page and handed to the
    // desktop only after a consent dialog — main frame prompts, a
    // subframe request is dropped silently.
    QTest::newRow("mailto-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("mailto:foo@bar.com") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << true;
    QTest::newRow("mailto-1") << Qt::NoButton << Qt::NoModifier << false << QUrl("mailto:foo@bar.com") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << false;
    QTest::newRow("ftp-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("ftp:foo@bar.com") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << true;
    QTest::newRow("ftp-1") << Qt::NoButton << Qt::NoModifier << false << QUrl("ftp:foo@bar.com") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << false;
    QTest::newRow("tel-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("tel:+15551234") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << true;

    QTest::newRow("normal-0") << Qt::NoButton << Qt::NoModifier << false << QUrl("http://www.foo.com") << QWebEnginePage::NavigationTypeLinkClicked << true << 0 << false;
    QTest::newRow("normal-1") << Qt::NoButton << Qt::NoModifier << true << QUrl("http://www.foo.com") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;

    // data: is a browser-handled scheme — navigations proceed.
    QTest::newRow("data-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("data:text/html,<p>x</p>") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;

    // Without a WebView/TabWidget the page cannot divert modified clicks
    // to a new tab, so these are accepted like normal clicks now.
    QTest::newRow("midclick-0") << Qt::MiddleButton << Qt::NoModifier << true << QUrl("http://www.foo.com") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;
    QTest::newRow("midclick-1") << Qt::MiddleButton << Qt::ShiftModifier << true << QUrl("http://www.foo.com") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;
    QTest::newRow("midclick-2") << Qt::MiddleButton << Qt::AltModifier << true << QUrl("http://www.foo.com") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;
}

// protected bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
void tst_WebPage::acceptNavigationRequest()
{
    QFETCH(Qt::MouseButton, pressedButton);
    QFETCH(Qt::KeyboardModifier, pressedKeys);
    QFETCH(bool, isMainFrame);
    QFETCH(QUrl, url);
    QFETCH(QWebEnginePage::NavigationType, type);
    QFETCH(bool, acceptNavigationRequest);
    QFETCH(int, spyCount);
    QFETCH(bool, externalPrompt);

    BrowserApplication::instance()->setEventMouseButtons(pressedButton);
    BrowserApplication::instance()->setEventKeyboardModifiers(pressedKeys);
    SubWebPage page;
    QSignalSpy spy0(&page, SIGNAL(aboutToLoadUrl(QUrl)));

    QCOMPARE(page.call_acceptNavigationRequest(url, type, isMainFrame), acceptNavigationRequest);

    QCOMPARE(spy0.count(), spyCount);

    const int openedBefore = m_openedUrls.count();
    if (externalPrompt) {
        // The consent prompt is queued so the navigation call is never
        // blocked on a modal loop; decline it here — the accept path is
        // covered by externalProtocolPrompt().
        QVERIFY(answerModal(QMessageBox::Cancel));
        QCOMPARE(m_openedUrls.count(), openedBefore);
    }

    BrowserApplication::instance()->setEventMouseButtons(Qt::NoButton);
    BrowserApplication::instance()->setEventKeyboardModifiers(Qt::NoModifier);
}

// SEC02: external-protocol navigations are consented hand-offs, never
// silent shell-outs.
void tst_WebPage::externalProtocolPrompt()
{
    SubWebPage page;
    const QUrl mailto(QStringLiteral("mailto:foo@bar.com"));

    // Accept: the url reaches the desktop url handler (trapped by the
    // setUrlHandler in initTestCase).
    m_openedUrls.clear();
    QVERIFY(!page.call_acceptNavigationRequest(mailto,
        QWebEnginePage::NavigationTypeLinkClicked, true));
    QVERIFY(answerModal(QMessageBox::Open));
    QCOMPARE(m_openedUrls, QList<QUrl>() << mailto);

    // Decline: nothing leaves the browser.  tel: deliberately has no
    // registered handler — a leak would hit the real xdg-open.
    QVERIFY(!page.call_acceptNavigationRequest(
        QUrl(QStringLiteral("tel:+15559876")),
        QWebEnginePage::NavigationTypeLinkClicked, true));
    QVERIFY(answerModal(QMessageBox::Cancel));
    QTest::qWait(200);
    QVERIFY(m_openedUrls.count() == 1);

    // Subframe navigations are denied without raising a prompt.
    QVERIFY(!page.call_acceptNavigationRequest(mailto,
        QWebEnginePage::NavigationTypeLinkClicked, false));
    QTest::qWait(300);
    QVERIFY(!QApplication::activeModalWidget());
    QVERIFY(m_openedUrls.count() == 1);
}

// The QtWebKit createPlugin() extension point (NPAPI) has no WebEngine
// equivalent.
void tst_WebPage::createPlugin()
{
    QSKIP("createPlugin is a QtWebKit-only API.", SkipAll);
}

// protected QWebEnginePage *createWindow(QWebEnginePage::WebWindowType type)
void tst_WebPage::createWindow()
{
    SubWebPage page;
    QWebEnginePage *newPage = page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    QVERIFY(newPage);
    QCOMPARE(newPage->profile(), page.profile());
    // The standalone WebView owns the page; close the window to clean up.
    delete QWebEngineView::forPage(newPage);
}

void tst_WebPage::handleUnsupportedContent()
{
    // WebEngine reports failed loads through loadFinished(false);
    // WebPage answers them with the Arora not-found page.
    SubWebPage page;
    QSignalSpy spy(&page, SIGNAL(loadFinished(bool)));
    page.load(QUrl("http://nonexistent.arora-test.invalid/test.html"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 15000);
    QCOMPARE(spy.at(0).at(0).toBool(), false);
}

void tst_WebPage::linkedResources()
{
    SubWebPage page;

    QString html = "<html>"
        "<head>"
            "<link rel=\"stylesheet\" type=\"text/css\" href=\"styles/common.css\" />"
            "<link rel=\"alternate\" type=\"application/rss+xml\" href=\"./rss.xml\" />"
            "<link rel=\"alternate\" type=\"application/atom+xml\" href=\"../atom.xml\" title=\"Feed\" />"
            "<link rel=\"search\" type=\"application/opensearchdescription+xml\" href=\"http://external.foo/search.xml\" />"
        "</head>"
        "<body>"
            "<link rel=\"stylesheet\" type=\"text/css\" href=\"styles/ie.css\" />"
        "</body>"
    "</html>";

    QSignalSpy spy(&page, SIGNAL(loadFinished(bool)));
    page.setHtml(html, QUrl("http://foobar.baz/foo/"));
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 15000);

    QList<WebPageLinkedResource> resources;
    bool done = false;
    page.linkedResources([&](const QList<WebPageLinkedResource> &result) {
        resources = result;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
    QCOMPARE(resources.count(), 4);

    QCOMPARE(resources.at(0).rel, QString("stylesheet"));
    QCOMPARE(resources.at(0).type, QString("text/css"));
    QCOMPARE(resources.at(0).href, QUrl("http://foobar.baz/foo/styles/common.css"));
    QCOMPARE(resources.at(0).title, QString());

    QCOMPARE(resources.at(1).rel, QString("alternate"));
    QCOMPARE(resources.at(1).type, QString("application/rss+xml"));
    QCOMPARE(resources.at(1).href, QUrl("http://foobar.baz/foo/rss.xml"));

    QCOMPARE(resources.at(2).href, QUrl("http://foobar.baz/atom.xml"));
    QCOMPARE(resources.at(2).title, QString("Feed"));

    QCOMPARE(resources.at(3).rel, QString("search"));
    QCOMPARE(resources.at(3).type, QString("application/opensearchdescription+xml"));
    QCOMPARE(resources.at(3).href, QUrl("http://external.foo/search.xml"));

    QString js = "var base = document.createElement('base');"
                 "base.setAttribute('href', 'http://barbaz.foo/bar/');"
                 "document.getElementsByTagName('head')[0].appendChild(base);";

    bool jsDone = false;
    page.runJavaScript(js, [&](const QVariant &) { jsDone = true; });
    QTRY_VERIFY_WITH_TIMEOUT(jsDone, 15000);

    done = false;
    page.linkedResources([&](const QList<WebPageLinkedResource> &result) {
        resources = result;
        done = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
    QCOMPARE(resources.count(), 4);

    QCOMPARE(resources.at(0).href, QUrl("http://barbaz.foo/bar/styles/common.css"));
    QCOMPARE(resources.at(1).href, QUrl("http://barbaz.foo/bar/rss.xml"));
    QCOMPARE(resources.at(2).href, QUrl("http://barbaz.foo/atom.xml"));
    QCOMPARE(resources.at(3).href, QUrl("http://external.foo/search.xml"));
}

void tst_WebPage::javaScriptObjects()
{
    // The QtWebKit addToJavaScriptWindowObject() bridge was replaced by
    // a QWebChannel.  SEC08: only the two web-facing objects are
    // registered unconditionally — "external" (consent-gated
    // AddSearchProvider) and "aroraAutofill" (token-gated submitForm).
    // "arora" is published only while the main frame shows an internal
    // qrc page (webChannelStartPage covers the registration).
    SubWebPage page;
    QWebChannel *channel = page.webChannel();
    QVERIFY(channel);
    QVERIFY(channel->registeredObjects().contains(QLatin1String("external")));
    QVERIFY(!channel->registeredObjects().contains(QLatin1String("arora")));
    QVERIFY(channel->registeredObjects().contains(QLatin1String("aroraAutofill")));
}

// SEC08: the "arora" object appears on the channel only for internal
// qrc pages, and the start page's async bootstrap actually reaches it.
void tst_WebPage::webChannelStartPage()
{
    SubWebPage page;
    QSignalSpy loaded(&page, SIGNAL(loadFinished(bool)));
    page.load(QUrl(QLatin1String("qrc:/startpage.html")));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 15000);

    QVERIFY2(page.webChannel()->registeredObjects()
                .contains(QLatin1String("arora")),
             "arora object must be registered for qrc pages");

    // The channel bootstrap translates the chrome text and reports the
    // current engine name — proving the round trip works for the one
    // page that is meant to have the object.
    QTRY_COMPARE_WITH_TIMEOUT(
        evalSync(&page, QLatin1String("document.title")).toString(),
        QStringLiteral("Welcome to Arora!"), 15000);
    const QString engineName = ToolbarSearch::openSearchManager()
            ->currentEngineName();
    QVERIFY(!engineName.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(evalSync(&page, QLatin1String(
        "document.getElementById('lineEdit').placeholder"))
            .toString() == engineName, 15000);

    // currentEngineName is a plain string now — the channel must never
    // hand a live QObject* (with writable properties/slots) to script.
    QVERIFY(evalSync(&page, QLatin1String(
        "new QWebChannel(qt.webChannelTransport, function (c) {"
        "  window.__engineType = typeof c.objects.arora.currentEngineName;"
        "}); 'ok'")).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(
        evalSync(&page, QLatin1String("window.__engineType")).toString(),
        QStringLiteral("string"), 15000);
}

// SEC08: a hostile page can open its own QWebChannel client — the
// transport is injected into every page and qwebchannel.js is public —
// and reach aroraAutofill.submitForm directly.  Reports without the
// per-load token only the injected autofill.js holds must be dropped:
// no write to the store, not even a save prompt.
void tst_WebPage::webChannelForgedAutofillReport()
{
    QFile channelFile(QLatin1String(":/qtwebchannel/qwebchannel.js"));
    QVERIFY2(channelFile.open(QIODevice::ReadOnly),
             "bundled qwebchannel.js required");

    LocalHttpServer server;
    QVERIFY(server.start());
    server.pageBody = QByteArrayLiteral("<html><head><script>")
        + channelFile.readAll()
        + QByteArrayLiteral("</script></head><body>"
        "<form name='login' onsubmit='return false'>"
        "<input id='u' name='user' type='text'>"
        "<input id='p' name='pw' type='password'></form>"
        "<script>"
        "window.__state='connecting';"
        "new QWebChannel(qt.webChannelTransport, function (channel) {"
        "  window.__channel=channel;"
        "  window.__objects=Object.keys(channel.objects).sort().join(',');"
        "  var b=channel.objects.aroraAutofill;"
        "  if (b) {"
        "    b.submitForm('forged', String(location.href),"
        "      {name:'login',hasPassword:true,elements:["
        "       {name:'user',value:'forgeduser'},"
        "       {name:'pw',value:'forgedpass'}]});"
        "    b.submitForm(String(location.href),"
        "      {name:'login',hasPassword:true,"
        "       elements:[{name:'user',value:'forged2'}]});"
        "  }"
        "  window.__state='done';"
        "});"
        "</script></body></html>");

    AutoFillManager *autoFill = AutoFillManager::instance();
    const QList<AutoFillManager::Form> baseline = autoFill->forms();
    autoFill->setForms(QList<AutoFillManager::Form>());

    // A named (non-off-the-record) profile so capture is actually
    // armed — the token is the only thing stopping the forged write.
    QWebEngineProfile profile(QStringLiteral("tst_webpage_channel"));
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));
    page->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(
        evalSync(page, QLatin1String("window.__state")).toString(),
        QStringLiteral("done"), 15000);

    // The web-facing object set: no "arora" on an untrusted page.
    QCOMPARE(evalSync(page, QLatin1String("window.__objects")).toString(),
             QStringLiteral("aroraAutofill,external"));

    // Wait until the injected capture hook is armed (the prototype
    // submit wrapper is installed inside the channel handshake), then
    // fire one more forged call against the armed bridge.
    QTRY_VERIFY_WITH_TIMEOUT(evalSync(page, QLatin1String(
        "HTMLFormElement.prototype.submit.toString()"
        ".indexOf('nativeSubmit')>=0")).toBool(), 15000);
    // Fire-and-forget: if a broken implementation did raise the save
    // prompt, a synchronous evalSync would hang inside the modal's
    // exec() — poll for a modal instead and reject it.
    page->runJavaScript(QLatin1String(
        "window.__channel.objects.aroraAutofill.submitForm("
        "'still-forged', String(location.href),"
        "{name:'login',hasPassword:true,"
        " elements:[{name:'user',value:'forged3'}]});"));
    bool modalSeen = false;
    for (int i = 0; i < 14; ++i) {
        if (QApplication::activeModalWidget()) {
            modalSeen = true;
            rejectModal(0);
        }
        QTest::qWait(50);
    }
    QVERIFY2(!modalSeen,
             "forged aroraAutofill.submitForm raised the save prompt");
    QVERIFY2(autoFill->forms().isEmpty(),
             "forged submitForm wrote to the autofill store");

    autoFill->setForms(baseline);
}

// SEC08: external.AddSearchProvider is callable by any page, but the
// descriptor fetch is consent-gated — declining means the url is never
// even requested; accepting fetches it and still runs the install
// confirmation naming the parsed engine.
void tst_WebPage::webChannelAddSearchProviderConsent()
{
    QFile channelFile(QLatin1String(":/qtwebchannel/qwebchannel.js"));
    QVERIFY2(channelFile.open(QIODevice::ReadOnly),
             "bundled qwebchannel.js required");

    LocalHttpServer server;
    QVERIFY(server.start());
    server.engineBody = QByteArrayLiteral(
        "<?xml version=\"1.0\"?>"
        "<OpenSearchDescription "
        "xmlns=\"http://a9.com/-/spec/opensearch/1.1/\">"
        "<ShortName>AroraChannelTest</ShortName>"
        "<Description>engine added through the channel</Description>"
        "<Url method=\"get\" type=\"text/html\" "
        "template=\"http://127.0.0.1/search?q={searchTerms}\"/>"
        "</OpenSearchDescription>");
    server.pageBody = QByteArrayLiteral("<html><head><script>")
        + channelFile.readAll()
        + QByteArrayLiteral("</script></head><body><script>"
        "window.__state='connecting';"
        "new QWebChannel(qt.webChannelTransport, function (channel) {"
        "  window.__channel=channel; window.__state='done'; });"
        "</script></body></html>");

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    // Clear a leftover from an interrupted previous run.
    if (manager->engineExists(QLatin1String("AroraChannelTest")))
        manager->removeEngine(QLatin1String("AroraChannelTest"));

    QWebEngineProfile profile(QStringLiteral("tst_webpage_search"));
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    page->load(server.url());
    QTRY_COMPARE_WITH_TIMEOUT(
        evalSync(page, QLatin1String("window.__state")).toString(),
        QStringLiteral("done"), 15000);

    // Fire-and-forget: the consent prompt is queued and would appear
    // while an evalSync pumps events, wedging the test inside the
    // modal's exec() — answerModal's repeating timer must already be
    // armed when it opens.
    const QString call = QStringLiteral(
        "window.__channel.objects.external.AddSearchProvider('%1');")
            .arg(server.url().toString() + QLatin1String("engine.xml"));

    // Decline: no fetch, no install.
    page->runJavaScript(call);
    QVERIFY(answerModal(QMessageBox::No));
    QTest::qWait(300);
    QCOMPARE(server.engineHits, 0);
    QVERIFY(!manager->engineExists(QLatin1String("AroraChannelTest")));

    // Accept: the descriptor is fetched and the install prompt (which
    // names the parsed engine and its host) still runs.
    page->runJavaScript(call);
    QVERIFY(answerModal(QMessageBox::Yes));   // fetch consent
    QVERIFY(answerModal(QMessageBox::Yes));   // install confirmation
    QTRY_VERIFY_WITH_TIMEOUT(
        manager->engineExists(QLatin1String("AroraChannelTest")), 5000);
    QCOMPARE(server.engineHits, 1);
    manager->removeEngine(QLatin1String("AroraChannelTest"));

    // A non-web scheme must be rejected before any prompt or fetch.
    page->runJavaScript(QLatin1String(
        "window.__channel.objects.external.AddSearchProvider("
        "'file:///etc/passwd');"));
    QTest::qWait(400);
    QVERIFY(!QApplication::activeModalWidget());
    QCOMPARE(server.engineHits, 1);
}

// SEC08: the legitimate path still works — the injected autofill.js
// holds the per-load token, so a real submit reaches the store after
// the user's save-password consent.
void tst_WebPage::webChannelAutofillCapture()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    server.pageBody = QByteArrayLiteral(
        "<html><body><form name='login' onsubmit='return false'>"
        "<input id='u' name='user' type='text'>"
        "<input id='p' name='pw' type='password'></form></body></html>");

    AutoFillManager *autoFill = AutoFillManager::instance();
    const QList<AutoFillManager::Form> baseline = autoFill->forms();
    autoFill->setForms(QList<AutoFillManager::Form>());

    QWebEngineProfile profile(QStringLiteral("tst_webpage_capture"));
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));
    page->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 15000);

    // The capture hook installs inside the channel handshake after
    // loadFinished; the prototype submit wrapper is the marker.
    QTRY_VERIFY_WITH_TIMEOUT(evalSync(page, QLatin1String(
        "HTMLFormElement.prototype.submit.toString()"
        ".indexOf('nativeSubmit')>=0")).toBool(), 15000);

    // Fire-and-forget: the report arrives over the channel and the
    // save prompt exec()s during event processing — a synchronous
    // evalSync would wedge inside it before answerModal was armed.
    page->runJavaScript(QLatin1String(
        "document.getElementById('u').value='captureduser';"
        "document.getElementById('p').value='capturedpw';"
        "document.forms[0].dispatchEvent("
        "  new Event('submit', {bubbles:true, cancelable:true}));"));

    // A real submit still reaches the manager — the save prompt is the
    // user's consent to persist it.
    QVERIFY(answerModal(QMessageBox::Yes));
    QTRY_VERIFY_WITH_TIMEOUT(autoFill->forms().count() == 1, 5000);
    const AutoFillManager::Form form = autoFill->forms().first();
    QCOMPARE(form.url, server.url());
    QVERIFY(form.hasAPassword);
    bool found = false;
    for (const AutoFillManager::Element &element : form.elements)
        found |= (element.first == QLatin1String("user")
                  && element.second == QLatin1String("captureduser"));
    QVERIFY(found);

    autoFill->setForms(baseline);
}

void tst_WebPage::userAgent()
{
    QSettings settings;
    settings.setValue("userAgent", QString());
    SubWebPage page;
    page.loadSettings();
    QCOMPARE(WebPage::userAgent(), QString());
    QVERIFY(!page.profile()->httpUserAgent().isEmpty());
    WebPage::setUserAgent("ben");
    QCOMPARE(WebPage::userAgent(), QString("ben"));
    WebPage::setUserAgent(QString());
}


QTEST_MAIN(tst_WebPage)
#include "tst_webpage.moc"
