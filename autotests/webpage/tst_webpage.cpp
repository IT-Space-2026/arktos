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
#include <popupblocker.h>
#include <privacyrequestinterceptor.h>
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
    void popupBlocking();
    void popupTargetCapture();
    void popupStateReset();
    void popupClickBlocking();
    void handleUnsupportedContent();
    void linkedResources();
    void javaScriptObjects();
    void webChannelStartPage();
    void webChannelForgedAutofillReport();
    void webChannelAddSearchProviderConsent();
    void webChannelAutofillCapture();
    void webChannelAutofillIsolation();
    void userAgent();
    void rateLimitInterstitialUrl_data();
    void rateLimitInterstitialUrl();
    void insecureFormDecision_data();
    void insecureFormDecision();
    void insecureFormUpgradeInterplay();
    void insecureFormPrompt();
    void insecureFormHttpsOnlyWins();
    void insecureFormRealSubmit();

private:
    QVariant evalSync(QWebEnginePage *page, const QString &js);
    void pinPrivacy(bool httpsOnly, bool httpsFirst);
    QList<QUrl> m_openedUrls;
    QVariant m_savedHttpsOnly;
    QVariant m_savedHttpsFirst;
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
    SubWebPage(QObject *parent = nullptr) : WebPage(parent) {}
    SubWebPage(QWebEngineProfile *profile, QObject *parent = nullptr)
        : WebPage(profile, parent) {}

    void call_aboutToLoadUrl(QUrl const &url)
        { emit SubWebPage::aboutToLoadUrl(url); }

    bool call_acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame)
        { return SubWebPage::acceptNavigationRequest(url, type, isMainFrame); }

    QWebEnginePage *call_createWindow(QWebEnginePage::WebWindowType type)
        { return SubWebPage::createWindow(type); }

    // CHAN01: collects renderer console messages so tests can assert
    // on uncaught page errors (e.g. the execCallbacks TypeError).
    QStringList consoleMessages;
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
            const QString &message, int lineNumber,
            const QString &sourceId) override
    {
        consoleMessages.append(message);
        WebPage::javaScriptConsoleMessage(level, message, lineNumber,
                                          sourceId);
    }
    int execCallbackErrors() const
    {
        int count = 0;
        for (const QString &message : consoleMessages)
            count += message.contains(QLatin1String("execCallbacks"));
        return count;
    }

    // POPUP01: records the shape Chromium reports for each window
    // request so tests can see whether a window.open arrived at all.
    QList<QWebEnginePage::WebWindowType> windowTypes;
    QWebEnginePage *createWindow(QWebEnginePage::WebWindowType type) override
    {
        windowTypes.append(type);
        return WebPage::createWindow(type);
    }
};

// SAFE02: the insecure-form warning exec()s inside
// acceptNavigationRequest, so the answer has to be armed before the
// call — the timer polls inside the nested modal loop.  count records
// how many dialogs were dismissed: 0 proves none appeared.  The
// warning's "Submit Anyway" is a role button, so the click is by
// ButtonRole, not StandardButton.
class ModalClicker
{
public:
    explicit ModalClicker(QMessageBox::ButtonRole role)
    {
        m_timer.setInterval(50);
        QObject::connect(&m_timer, &QTimer::timeout, qApp,
                         [this, role]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (!box)
                return;
            QAbstractButton *target = nullptr;
            for (QAbstractButton *candidate : box->buttons()) {
                if (box->buttonRole(candidate) == role) {
                    target = candidate;
                    break;
                }
            }
            if (target)
                target->click();
            else
                box->reject();
            ++count;
        });
        m_timer.start();
    }
    int count = 0;

private:
    QTimer m_timer;
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_WebPage::initTestCase()
{
    QCoreApplication::setApplicationName("tst_webpage");
    QStandardPaths::setTestModeEnabled(true);

    QDesktopServices::setUrlHandler(QLatin1String("mailto"), this, "openUrl");
    QDesktopServices::setUrlHandler(QLatin1String("ftp"), this, "openUrl");
    QDesktopServices::setUrlHandler(QLatin1String("aroraext"), this, "openUrl");

    // SAFE02: the form-post tests pin the HTTPS policy keys; stash
    // the user's values so nothing leaks past the suite.
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    m_savedHttpsOnly = settings.value(QLatin1String("httpsOnly"));
    m_savedHttpsFirst = settings.value(QLatin1String("httpsFirst"));
    settings.endGroup();
}

void tst_WebPage::pinPrivacy(bool httpsOnly, bool httpsFirst)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("httpsOnly"), httpsOnly);
    settings.setValue(QLatin1String("httpsFirst"), httpsFirst);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    PrivacyRequestInterceptor::clearDowngradedHosts();
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_WebPage::cleanupTestCase()
{
    QDesktopServices::unsetUrlHandler(QLatin1String("mailto"));
    QDesktopServices::unsetUrlHandler(QLatin1String("ftp"));
    QDesktopServices::unsetUrlHandler(QLatin1String("aroraext"));

    QSettings settings;
    settings.setValue("userAgent", QString());
    const auto restore = [&settings](const QString &key,
                                     const QVariant &saved) {
        if (saved.isValid())
            settings.setValue(key, saved);
        else
            settings.remove(key);
    };
    restore(QLatin1String("privacy/httpsOnly"), m_savedHttpsOnly);
    restore(QLatin1String("privacy/httpsFirst"), m_savedHttpsFirst);
    PrivacyRequestInterceptor::loadSettings();
    PrivacyRequestInterceptor::clearDowngradedHosts();
}

// This will be called before each test function is executed.
void tst_WebPage::init()
{
    // POPUP01: the blocker singleton is QSettings-backed — each test
    // starts enabled with an empty exception store.
    PopupBlocker *blocker = PopupBlocker::instance();
    blocker->clearAllowedHosts();
    blocker->clearSessionHosts();
    blocker->setEnabled(true);
}

// This will be called after every test function.
void tst_WebPage::cleanup()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    blocker->removeAllowedHost(QLatin1String("127.0.0.1"));
    blocker->clearSessionHosts();
    blocker->setEnabled(true);

    // SAFE02: any privacy pinning a test did is undone — the saved
    // values (or defaults when none existed) go back.
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    const auto restore = [&settings](const QString &key,
                                     const QVariant &saved) {
        if (saved.isValid())
            settings.setValue(key, saved);
        else
            settings.remove(key);
    };
    restore(QLatin1String("httpsOnly"), m_savedHttpsOnly);
    restore(QLatin1String("httpsFirst"), m_savedHttpsFirst);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    PrivacyRequestInterceptor::clearDowngradedHosts();
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

    // SAFE05: the gate keys on the browser-handled set, so ANY other
    // scheme — vendor handlers included — must prompt, never silently
    // reach the OS.  Typed omnibox input and redirect hops go through
    // the same hook.
    QTest::newRow("magnet-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("magnet:?xt=urn:btih:0123456789abcdef") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << true;
    QTest::newRow("intent-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("intent://host/#Intent;scheme=https;end") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << true;
    QTest::newRow("vnc-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("vnc://192.0.2.1:5900") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << true;
    QTest::newRow("sms-subframe") << Qt::NoButton << Qt::NoModifier << false << QUrl("sms:+15551234?body=x") << QWebEnginePage::NavigationTypeLinkClicked << false << 0 << false;
    QTest::newRow("mailto-typed") << Qt::NoButton << Qt::NoModifier << true << QUrl("mailto:foo@bar.com") << QWebEnginePage::NavigationTypeTyped << false << 0 << true;
    QTest::newRow("custom-redirect") << Qt::NoButton << Qt::NoModifier << true << QUrl("weirdproto://open/thing") << QWebEnginePage::NavigationTypeRedirect << false << 0 << true;

    // Registered internal schemes and the built-ins are unaffected —
    // no prompt, the navigation proceeds to the scheme handler.
    QTest::newRow("arora-resource-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("arora-resource:/noop.js") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;
    QTest::newRow("abp-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("abp:subscribe?location=http://x/list.txt") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;
    QTest::newRow("viewsource-0") << Qt::NoButton << Qt::NoModifier << true << QUrl("view-source:http://www.foo.com") << QWebEnginePage::NavigationTypeLinkClicked << true << 1 << false;

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

    // SAFE05: a scheme nobody registered still follows the same
    // consent path — the gate is "browser-handled or prompt", not a
    // protocol allowlist.  The test-side url handler traps the
    // QDesktopServices hand-off so nothing reaches the real desktop.
    const QUrl custom(QStringLiteral("aroraext:thing"));
    QVERIFY(!page.call_acceptNavigationRequest(custom,
        QWebEnginePage::NavigationTypeLinkClicked, true));
    QVERIFY(answerModal(QMessageBox::Open));
    QCOMPARE(m_openedUrls.last(), custom);

    // And denying the same custom scheme launches nothing.
    QVERIFY(!page.call_acceptNavigationRequest(custom,
        QWebEnginePage::NavigationTypeLinkClicked, true));
    QVERIFY(answerModal(QMessageBox::Cancel));
    QTest::qWait(200);
    QCOMPARE(m_openedUrls.count(), 2);
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

// POPUP01: pop-up-shaped window requests dead-end in a probe page —
// no real window is created and the blocked counter ticks — while
// tab-shaped requests still follow the openTargetBlankLinksIn path.
void tst_WebPage::popupBlocking()
{
    PopupBlocker *blocker = PopupBlocker::instance();
    LocalHttpServer server;
    server.pageBody = "<html><body>popup opener</body></html>";
    QVERIFY(server.start());

    SubWebPage page;
    QSignalSpy loaded(&page, SIGNAL(loadFinished(bool)));
    page.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QCOMPARE(loaded.last().at(0).toBool(), true);
    QCOMPARE(page.url().host(), QLatin1String("127.0.0.1"));

    QSignalSpy blockedSpy(&page, SIGNAL(popupBlocked()));

    QWebEnginePage *windowProbe =
        page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    QVERIFY(windowProbe);
    QVERIFY(!QWebEngineView::forPage(windowProbe));
    QCOMPARE(page.blockedPopupCount(), 1);
    QCOMPARE(blockedSpy.count(), 1);

    QWebEnginePage *dialogProbe =
        page.call_createWindow(QWebEnginePage::WebDialog);
    QVERIFY(dialogProbe);
    QVERIFY(!QWebEngineView::forPage(dialogProbe));
    QCOMPARE(page.blockedPopupCount(), 2);

    // Tab-shaped requests are target=_blank territory — governed by
    // the tab-placement preference, never by the pop-up blocker.
    QWebEnginePage *tabPage =
        page.call_createWindow(QWebEnginePage::WebBrowserTab);
    QVERIFY(tabPage);
    QVERIFY(QWebEngineView::forPage(tabPage));
    delete QWebEngineView::forPage(tabPage);
    QCOMPARE(page.blockedPopupCount(), 2);

    QWebEnginePage *bgTabPage =
        page.call_createWindow(QWebEnginePage::WebBrowserBackgroundTab);
    QVERIFY(bgTabPage);
    QVERIFY(QWebEngineView::forPage(bgTabPage));
    delete QWebEngineView::forPage(bgTabPage);

    // A persistent site exception lets pop-ups through again.
    blocker->allowHost(QLatin1String("127.0.0.1"));
    QWebEnginePage *allowed =
        page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    QVERIFY(allowed);
    QVERIFY(QWebEngineView::forPage(allowed));
    delete QWebEngineView::forPage(allowed);
    QCOMPARE(page.blockedPopupCount(), 2);
    blocker->removeAllowedHost(QLatin1String("127.0.0.1"));

    // Blocker switched off: pop-up shapes get real windows again.
    blocker->setEnabled(false);
    QWebEnginePage *unblocked =
        page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    QVERIFY(unblocked);
    QVERIFY(QWebEngineView::forPage(unblocked));
    delete QWebEngineView::forPage(unblocked);
    blocker->setEnabled(true);
}

// POPUP01: the dead-end probe refuses every navigation but reports
// the first http(s)/ftp target back so the indicator can offer it
// for "Open once".  about:blank keeps the probe alive unrecorded —
// scripts routinely open a blank handle then steer it.
void tst_WebPage::popupTargetCapture()
{
    LocalHttpServer server;
    server.pageBody = "<html><body>popup opener</body></html>";
    QVERIFY(server.start());

    SubWebPage page;
    QSignalSpy loaded(&page, SIGNAL(loadFinished(bool)));
    page.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    QWebEnginePage *probe =
        page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    QVERIFY(probe);
    QCOMPARE(page.blockedPopupCount(), 1);
    QVERIFY(page.blockedPopupUrls().isEmpty());

    probe->load(QUrl("about:blank"));
    QTest::qWait(300);
    QVERIFY(page.blockedPopupUrls().isEmpty());

    const QUrl target(QStringLiteral("http://popup.invalid/landing"));
    probe->load(target);
    QTRY_VERIFY_WITH_TIMEOUT(
        page.blockedPopupUrls().contains(target), 15000);

    // Duplicates fold and the list is capped — a pop-up storm cannot
    // grow the indicator menu without bound.
    page.noteBlockedPopupTarget(target);
    QCOMPARE(page.blockedPopupUrls().count(), 1);
    for (int i = 0; i < 25; ++i)
        page.noteBlockedPopupTarget(QUrl(
            QStringLiteral("http://p%1.invalid/").arg(i)));
    QCOMPARE(page.blockedPopupUrls().count(), 20);
}

// POPUP01: the blocked tally belongs to the document that produced it
// — a fragment-only click keeps it, a real navigation clears it.
void tst_WebPage::popupStateReset()
{
    LocalHttpServer server;
    server.pageBody = "<html><body>popup opener</body></html>";
    QVERIFY(server.start());

    SubWebPage page;
    QSignalSpy loaded(&page, SIGNAL(loadFinished(bool)));
    page.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);

    delete page.call_createWindow(QWebEnginePage::WebBrowserWindow);
    page.noteBlockedPopupTarget(QUrl("http://popup.invalid/a"));
    QCOMPARE(page.blockedPopupCount(), 1);
    QCOMPARE(page.blockedPopupUrls().count(), 1);

    QUrl sameDoc = page.url();
    sameDoc.setFragment(QStringLiteral("here"));
    QVERIFY(page.call_acceptNavigationRequest(sameDoc,
        QWebEnginePage::NavigationTypeLinkClicked, true));
    QCOMPARE(page.blockedPopupCount(), 1);

    QUrl next = server.url();
    next.setPath(QStringLiteral("/other"));
    QSignalSpy blockedSpy(&page, SIGNAL(popupBlocked()));
    QVERIFY(page.call_acceptNavigationRequest(next,
        QWebEnginePage::NavigationTypeLinkClicked, true));
    QCOMPARE(page.blockedPopupCount(), 0);
    QVERIFY(page.blockedPopupUrls().isEmpty());
    QCOMPARE(blockedSpy.count(), 1);
}

// POPUP01 e2e: the Kephyr click-test scenario — a real button click
// calls window.open with a feature string, which is exactly what
// JavascriptCanOpenWindows alone lets through.  With the app-side
// blocker the request dead-ends: the tally ticks, the probe reports
// the destination, and no window appears.
void tst_WebPage::popupClickBlocking()
{
    LocalHttpServer server;
    server.pageBody =
        "<html><body><button id=\"pop\""
        " style=\"position:absolute;left:0;top:0;width:200px;height:50px\""
        " onclick=\"window.__clicked=1;"
        "var w=window.open('popup.html','p','width=200,height=200');"
        "window.__handle=(w!==null)\""
        ">open</button></body></html>";
    QVERIFY(server.start());

    WebView view;
    view.resize(400, 300);
    // SubWebPage exposes the blocked counters; setPage attaches it to
    // the view so createWindow sees the real WebView path.
    SubWebPage *page = new SubWebPage(view.webPage()->profile(), &view);
    view.setPage(page);
    view.show();

    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));
    page->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QCOMPARE(loaded.last().at(0).toBool(), true);

    // The gesture-less window.open suppression lives upstream in the
    // renderer; for the exercise of createWindow itself the attribute
    // is flipped on — the app-side blocker is what must stop the call.
    page->settings()->setAttribute(
        QWebEngineSettings::JavascriptCanOpenWindows, true);

    // Input must go to the render delegate — Chromium reads mouse
    // events off its own child widget, and the synthetic click counts
    // as a user gesture there, so the window.open survives the
    // renderer's gesture check and reaches createWindow.
    QWidget *surface = nullptr;
    const QList<QWidget*> children = view.findChildren<QWidget*>();
    for (QWidget *child : children) {
        if (child->objectName().contains(
                QLatin1String("render"), Qt::CaseInsensitive)
            || child->inherits(
                "RenderWidgetHostViewQtDelegateWidget"))
            surface = child;
    }
    if (!surface && !children.isEmpty())
        surface = children.last();
    QVERIFY(surface);
    QTest::mouseMove(surface, QPoint(50, 25));
    QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier,
                      QPoint(50, 25));
    QTRY_VERIFY_WITH_TIMEOUT(
        evalSync(page, QLatin1String("window.__clicked")).toInt() == 1,
        15000);
    QTRY_VERIFY_WITH_TIMEOUT(!page->windowTypes.isEmpty(), 15000);
    // A feature-string window.open reports as WebDialog (or
    // WebBrowserWindow) — never a tab shape — which is why the
    // createWindow gate keys on the window type.
    QCOMPARE(page->windowTypes.count(), 1);
    QVERIFY(page->windowTypes.first() == QWebEnginePage::WebBrowserWindow
            || page->windowTypes.first() == QWebEnginePage::WebDialog);
    QTRY_COMPARE_WITH_TIMEOUT(page->blockedPopupCount(), 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !page->blockedPopupUrls().isEmpty(), 15000);
    QCOMPARE(page->blockedPopupUrls().first().path(),
             QLatin1String("/popup.html"));
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

    // CHAN01: the page's own channel client coexists with the armed
    // autofill bundle — neither may steal the other's transport
    // messages.
    QTest::qWait(300);
    QVERIFY2(page.execCallbackErrors() == 0,
             qPrintable(page.consoleMessages.join(QLatin1Char('\n'))));
}

// SEC08: a hostile page can open its own QWebChannel client — the
// transport is injected into every page and qwebchannel.js is public —
// and reach aroraAutofill.submitForm directly.  Reports without the
// per-load token only the injected autofill.js holds must be dropped:
// no write to the store, not even a save prompt.  CHAN01: with the
// transport demux armed (arora-channel.js) the forged-call exercise
// also proves two clients coexist on one transport — any misrouted
// response would surface as an execCallbacks console error.
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

    // The web-facing object set: aroraAutofill + aroraReader (READ01)
    // + aroraPip (PIP01 — ferries opaque nonces only) are deliberately
    // reachable, nothing more.
    QCOMPARE(evalSync(page, QLatin1String("window.__objects")).toString(),
             QStringLiteral("aroraAutofill,aroraPip,aroraReader,external"));

    // Wait until the injected capture hook is armed (the DocumentReady
    // bundle wraps the prototype submit), then fire one more forged
    // call against the armed bridge.
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

    // The capture hook arms as a per-page DocumentReady script; the
    // main-world shim's prototype submit wrapper is the marker.
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

// CHAN01: qt.webChannelTransport is a single per-frame object with
// one onmessage slot — the injected autofill client and a page-side
// QWebChannel client used to fight over it, and responses addressed
// to one landed in the other's execCallbacks table
// ("channel.execCallbacks[message.id] is not a function" on every
// load).  arora-channel.js now demultiplexes the slot across clients,
// so a live page-side channel plus a submit report must coexist: the
// report arrives, the page's own channel keeps round-tripping, and
// the console stays clean.
void tst_WebPage::webChannelAutofillIsolation()
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
        "  window.__channel=channel; window.__state='done'; });"
        "</script></body></html>");

    AutoFillManager *autoFill = AutoFillManager::instance();
    const QList<AutoFillManager::Form> baseline = autoFill->forms();
    autoFill->setForms(QList<AutoFillManager::Form>());

    QWebEngineProfile profile(QStringLiteral("tst_webpage_isolation"));
    QWebEngineView view;
    SubWebPage *page = new SubWebPage(&profile, &view);
    view.setPage(page);
    view.resize(800, 600);
    view.show();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));
    page->load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty()
            && loaded.last().at(0).toBool(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(
        evalSync(page, QLatin1String("window.__state")).toString(),
        QStringLiteral("done"), 15000);

    // Wait for the capture hook, then submit and immediately exercise
    // the page's own channel — the report spins up the shared client
    // while the page's invoke round-trips in flight, the interleaving
    // that produced the execCallbacks errors on the shared transport.
    // (AddSearchProvider on a file: url is refused before prompting;
    // a void slot still answers a callback, so __pageCall proves the
    // page's own client is fully functional alongside ours.)
    QTRY_VERIFY_WITH_TIMEOUT(evalSync(page, QLatin1String(
        "HTMLFormElement.prototype.submit.toString()"
        ".indexOf('nativeSubmit')>=0")).toBool(), 15000);
    page->runJavaScript(QLatin1String(
        "window.__pageCall='pending';"
        "document.getElementById('u').value='isolateduser';"
        "document.getElementById('p').value='isolatedpw';"
        "document.forms[0].dispatchEvent("
        "  new Event('submit', {bubbles:true, cancelable:true}));"
        "window.__channel.objects.external.AddSearchProvider("
        "  'file:///etc/passwd', function () {"
        "    window.__pageCall='done'; });"));

    QVERIFY(answerModal(QMessageBox::Yes));
    QTRY_VERIFY_WITH_TIMEOUT(autoFill->forms().count() == 1, 5000);
    QCOMPARE(autoFill->forms().first().url, server.url());
    QTRY_COMPARE_WITH_TIMEOUT(
        evalSync(page, QLatin1String("window.__pageCall")).toString(),
        QStringLiteral("done"), 5000);

    // Let any misrouted responses settle, then assert no client hit a
    // foreign callback table.
    QTest::qWait(500);
    QVERIFY2(page->execCallbackErrors() == 0,
             qPrintable(page->consoleMessages.join(QLatin1Char('\n'))));

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

// UA02: the /sorry/ banner is driven by WebPage::
// isRateLimitInterstitialUrl — it must catch the real Google bot-check
// url shapes without matching unrelated pages or lookalike hosts.
void tst_WebPage::rateLimitInterstitialUrl_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("isInterstitial");

    QTest::newRow("google.com /sorry/index")
        << "https://www.google.com/sorry/index?continue=x&q=y" << true;
    QTest::newRow("google.com /sorry/")
        << "https://google.com/sorry/" << true;
    QTest::newRow("google.co.uk")
        << "https://www.google.co.uk/sorry/index" << true;
    QTest::newRow("google.com.au")
        << "https://google.com.au/sorry/" << true;
    QTest::newRow("subdomain")
        << "https://ipv4.google.com/sorry/index" << true;

    QTest::newRow("search page, not /sorry/")
        << "https://www.google.com/search?q=sorry" << false;
    QTest::newRow("google root")
        << "https://www.google.com/" << false;
    QTest::newRow("other host /sorry/")
        << "https://duckduckgo.com/sorry/" << false;
    QTest::newRow("google as subdomain of lookalike")
        << "https://google.com.evil.example/sorry/" << false;
    QTest::newRow("google label too deep")
        << "https://www.google.com.evil.com/sorry/" << false;
    QTest::newRow("label containing google")
        << "https://evilgoogle.com/sorry/" << false;
    QTest::newRow("non-google second level")
        << "https://google.foo.bar/sorry/" << false;
    QTest::newRow("not a url")
        << "about:blank" << false;
}

void tst_WebPage::rateLimitInterstitialUrl()
{
    QFETCH(QString, url);
    QFETCH(bool, isInterstitial);
    QCOMPARE(WebPage::isRateLimitInterstitialUrl(QUrl(url)),
             isInterstitial);
}

// SAFE02: the pure decision — warn only when the POST would really
// travel plaintext: public http targets warn, https is silent, and
// the loopback/LAN/.onion carve-outs match the https-first ones.
void tst_WebPage::insecureFormDecision_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("warn");

    QTest::newRow("public http warns")
        << QString("http://formpost.test/login") << true;
    QTest::newRow("https silent")
        << QString("https://formpost.test/login") << false;
    QTest::newRow("localhost exempt")
        << QString("http://localhost:8080/form") << false;
    QTest::newRow("loopback v4 exempt")
        << QString("http://127.0.0.1/form") << false;
    QTest::newRow("loopback v6 exempt")
        << QString("http://[::1]/form") << false;
    QTest::newRow("lan 10/8 exempt")
        << QString("http://10.1.2.3/form") << false;
    QTest::newRow("lan 192.168 exempt")
        << QString("http://192.168.1.10/form") << false;
    QTest::newRow(".local exempt")
        << QString("http://nas.local/form") << false;
    QTest::newRow(".onion exempt")
        << QString("http://abcdefghijklmnop.onion/form") << false;
    QTest::newRow("public IP literal warns")
        << QString("http://203.0.113.9/form") << true;
    QTest::newRow("hostless http is not a warning")
        << QString("http:///form") << false;
}

void tst_WebPage::insecureFormDecision()
{
    pinPrivacy(false, false);   // both HTTPS modes off: raw decision
    QFETCH(QString, url);
    QFETCH(bool, warn);
    QCOMPARE(PrivacyRequestInterceptor::shouldWarnFormPost(QUrl(url)),
             warn);
}

// The https-first upgrade rides the POST onto TLS before the wire —
// an upgradeable host does not warn; once the secure load failed and
// the host is downgraded, the same post is plaintext and does warn.
// Unlike HTTPS-Only, the http-exception list does NOT suppress the
// form warning: an excepted host is exactly where plaintext posts
// still flow.
void tst_WebPage::insecureFormUpgradeInterplay()
{
    const QUrl http(QStringLiteral("http://upgradeable-form.test/x"));
    const QUrl https(QStringLiteral("https://upgradeable-form.test/x"));

    pinPrivacy(false, true);    // strict veto off, upgrade on
    QVERIFY(!PrivacyRequestInterceptor::shouldWarnFormPost(http));
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(http));

    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(https));
    QVERIFY(PrivacyRequestInterceptor::shouldWarnFormPost(http));
    PrivacyRequestInterceptor::clearDowngradedHosts();

    pinPrivacy(true, false);    // strict veto on, upgrade off
    const QString allowedHost = QStringLiteral("excepted-form.test");
    PrivacyRequestInterceptor::allowHttpForHost(allowedHost, false);
    QVERIFY(!PrivacyRequestInterceptor::shouldWarnHttp(
        QUrl(QStringLiteral("http://") + allowedHost
             + QLatin1Char('/'))));
    QVERIFY(PrivacyRequestInterceptor::shouldWarnFormPost(
        QUrl(QStringLiteral("http://") + allowedHost
             + QLatin1String("/form"))));
    PrivacyRequestInterceptor::clearHttpAllowance(allowedHost);
}

// The prompt itself: cancel refuses the navigation, submit lets the
// POST proceed (synchronously, so the body is not lost), and the
// approval sticks for the rest of the page — the same target asks
// once, a different host asks again, https and exempt hosts never
// ask, and subframe posts warn like main-frame ones.
void tst_WebPage::insecureFormPrompt()
{
    pinPrivacy(false, false);
    SubWebPage page;
    const QUrl post1(QStringLiteral("http://formpost-one.test/submit"));
    const QUrl post2(QStringLiteral("http://formpost-two.test/submit"));
    const QUrl post3(QStringLiteral("http://formpost-three.test/x"));
    const QUrl secure(
        QStringLiteral("https://formpost-one.test/submit"));
    const QUrl local(
        QStringLiteral("http://127.0.0.1:9/submit"));

    {   // Cancel aborts the submission.
        ModalClicker clicker(QMessageBox::RejectRole);
        QVERIFY(!page.call_acceptNavigationRequest(
            post1, QWebEnginePage::NavigationTypeFormSubmitted, true));
        QCOMPARE(clicker.count, 1);
    }
    {   // Submit Anyway lets the navigation through.
        ModalClicker clicker(QMessageBox::AcceptRole);
        QVERIFY(page.call_acceptNavigationRequest(
            post1, QWebEnginePage::NavigationTypeFormSubmitted, true));
        QCOMPARE(clicker.count, 1);
    }
    {   // One-shot per page: the same target is silent now.
        ModalClicker clicker(QMessageBox::RejectRole);
        QVERIFY(page.call_acceptNavigationRequest(
            post1, QWebEnginePage::NavigationTypeFormSubmitted, true));
        QCOMPARE(clicker.count, 0);
    }
    {   // A different plaintext target asks again.
        ModalClicker clicker(QMessageBox::AcceptRole);
        QVERIFY(page.call_acceptNavigationRequest(
            post2, QWebEnginePage::NavigationTypeFormSubmitted, true));
        QCOMPARE(clicker.count, 1);
    }
    {   // https posts never prompt.
        ModalClicker clicker(QMessageBox::RejectRole);
        QVERIFY(page.call_acceptNavigationRequest(
            secure, QWebEnginePage::NavigationTypeFormSubmitted, true));
        QCOMPARE(clicker.count, 0);
    }
    {   // Neither do loopback targets.
        ModalClicker clicker(QMessageBox::RejectRole);
        QVERIFY(page.call_acceptNavigationRequest(
            local, QWebEnginePage::NavigationTypeFormSubmitted, true));
        QCOMPARE(clicker.count, 0);
    }
    {   // Subframe form posts warn the same way.
        ModalClicker clicker(QMessageBox::RejectRole);
        QVERIFY(!page.call_acceptNavigationRequest(
            post3, QWebEnginePage::NavigationTypeFormSubmitted, false));
        QCOMPARE(clicker.count, 1);
    }
    {   // Link clicks and other navigation types are unaffected.
        ModalClicker clicker(QMessageBox::RejectRole);
        QVERIFY(page.call_acceptNavigationRequest(
            post1, QWebEnginePage::NavigationTypeLinkClicked, true));
        QCOMPARE(clicker.count, 0);
    }
}

// Ordering with SAFE01: a non-excepted http: navigation is owned by
// the HTTPS-Only interstitial — the form prompt must not stack a
// second dialog on the refusal.
void tst_WebPage::insecureFormHttpsOnlyWins()
{
    pinPrivacy(true, false);
    SubWebPage page;
    QSignalSpy warned(&page, SIGNAL(httpOnlyInterstitial(QUrl)));
    const QUrl post(QStringLiteral("http://formpost-veto.test/login"));

    ModalClicker clicker(QMessageBox::RejectRole);
    QVERIFY(!page.call_acceptNavigationRequest(
        post, QWebEnginePage::NavigationTypeFormSubmitted, true));
    QCOMPARE(clicker.count, 0);          // no form prompt
    QCOMPARE(warned.count(), 1);         // interstitial took over
    QCOMPARE(warned.at(0).at(0).toUrl(), post);
}

// End to end: a real form in a real page.  The test page lives on
// loopback (exempt) and POSTs to a public-looking host — cancel keeps
// the page put, submit releases the navigation (which then fails on
// the unresolvable name, harmlessly).
void tst_WebPage::insecureFormRealSubmit()
{
    pinPrivacy(false, false);
    LocalHttpServer server;
    server.pageBody =
        "<html><body><form id=f method=post"
        " action='http://realpost.invalid/submit'>"
        "<input name=q value=x></form></body></html>";
    QVERIFY(server.start());

    SubWebPage page;
    QSignalSpy loaded(&page, SIGNAL(loadFinished(bool)));
    page.load(server.url());
    QTRY_VERIFY_WITH_TIMEOUT(loaded.count() >= 1, 15000);
    QCOMPARE(loaded.last().at(0).toBool(), true);
    QCOMPARE(page.url(), server.url());

    QSignalSpy navigating(&page, SIGNAL(aboutToLoadUrl(QUrl)));

    {   // Cancel: the form navigation is refused, the page stays.
        ModalClicker clicker(QMessageBox::RejectRole);
        page.runJavaScript(QLatin1String(
            "document.getElementById('f').requestSubmit(); true"));
        QTRY_VERIFY_WITH_TIMEOUT(clicker.count == 1, 10000);
        QTest::qWait(300);
        QCOMPARE(page.url(), server.url());
        QCOMPARE(navigating.count(), 0);
    }

    {   // Submit Anyway: the navigation is released to the network.
        ModalClicker clicker(QMessageBox::AcceptRole);
        page.runJavaScript(QLatin1String(
            "document.getElementById('f').requestSubmit(); true"));
        QTRY_VERIFY_WITH_TIMEOUT(clicker.count == 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(navigating.count() == 1, 10000);
        QCOMPARE(navigating.at(0).at(0).toUrl().host(),
                 QStringLiteral("realpost.invalid"));
    }
}


QTEST_MAIN(tst_WebPage)
#include "tst_webpage.moc"
