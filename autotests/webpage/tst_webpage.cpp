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
    void userAgent();

private:
    QList<QUrl> m_openedUrls;
};

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
    // a QWebChannel; verify the objects are registered on the channel.
    SubWebPage page;
    QWebChannel *channel = page.webChannel();
    QVERIFY(channel);
    QVERIFY(channel->registeredObjects().contains(QLatin1String("external")));
    QVERIFY(channel->registeredObjects().contains(QLatin1String("arora")));
    QVERIFY(channel->registeredObjects().contains(QLatin1String("aroraAutofill")));
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
