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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// SAFE01: HTTPS-Only strict mode — the veto in
// WebPage::acceptNavigationRequest, the nonce-bound
// arora-http-warning: interstitial it swaps in, its proceed/back/
// always action links, the session + persisted host exceptions, and
// the pure shouldWarnHttp decision (including the https-first
// ordering and the loopback/LAN/.onion exemptions).
//
// The full request path — proxy-observable upgrade ordering, the
// interceptor-side block of hops that bypass the navigation hook, and
// the off toggle — is covered by ./arora --httpsonly-smoke.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qwebengineprofile.h>
#include <qtemporarydir.h>

#include "privacyrequestinterceptor.h"
#include "schemeaccesshandler.h"
#include "webpage.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

// Promotes the protected navigation hook so tests can drive redirect
// hops and subframe requests the way Chromium reports them.
class ExposedPage : public WebPage
{
public:
    explicit ExposedPage(QWebEngineProfile *profile)
        : WebPage(profile) {}
    using WebPage::acceptNavigationRequest;
};

class tst_HttpOnly : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void warnDecision_data();
    void warnDecision();
    void upgradeOrdering();
    void warningCommits();
    void proceedAllowsHost();
    void alwaysPersistsException();
    void otrRendersNoAlwaysLink();
    void redirectHopWarns();
    void subframeNotWarned();
    void forgedLinkIgnored();
    void backFallsBackToStartPage();
    void blockedNavRegistry();
    void disabledModeLoadsPlain();

private:
    QVariant evalSync(QWebEnginePage *page, const QString &js);
    QString domText(QWebEnginePage *page);
    bool waitInterstitialCommit(WebPage *page, QSignalSpy &loaded,
                                int baseline);
    bool clickElement(WebView *view, const QString &id);
    void pinPolicy(bool httpsOnly, bool httpsFirst);
    void forgetHost(const QString &host);

    QVariant m_savedHttpsOnly;
    QVariant m_savedHttpsFirst;
    QVariant m_savedExceptions;
    QStringList m_touchedHosts;
};

void tst_HttpOnly::pinPolicy(bool httpsOnly, bool httpsFirst)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("httpsOnly"), httpsOnly);
    settings.setValue(QLatin1String("httpsFirst"), httpsFirst);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    PrivacyRequestInterceptor::clearDowngradedHosts();
}

void tst_HttpOnly::forgetHost(const QString &host)
{
    m_touchedHosts << host;
    PrivacyRequestInterceptor::clearHttpAllowance(host);
}

QVariant tst_HttpOnly::evalSync(QWebEnginePage *page, const QString &js)
{
    QVariant result;
    bool done = false;
    page->runJavaScript(js, [&](const QVariant &v) { result = v; done = true; });
    for (int waited = 0; !done && waited < 10000; waited += 50)
        QTest::qWait(50);
    return result;
}

QString tst_HttpOnly::domText(QWebEnginePage *page)
{
    return evalSync(page, QLatin1String(
        "document.documentElement.outerHTML")).toString();
}

// Waits for the interstitial's own loadFinished(true) commit — the
// DOM is only safe to inspect/click afterwards.  The vetoed
// navigation's failure signal can land before OR after that commit
// depending on how far the refused entry got, so the check is "any
// successful load" plus the interstitial url, not the last entry.
bool tst_HttpOnly::waitInterstitialCommit(WebPage *page,
        QSignalSpy &loaded, int baseline)
{
    for (int waited = 0; waited < 15000; waited += 50) {
        bool committed = false;
        for (int i = baseline; i < loaded.count(); ++i)
            committed |= loaded.at(i).at(0).toBool();
        if (committed
            && page->url().scheme() == QLatin1String("arora-http-warning"))
            return true;
        QTest::qWait(50);
    }
    return false;
}

// Synthesises a real mouse click inside the element — element.click()
// is not a user gesture, and Chromium gates custom-protocol
// navigations on one.  Uses the FIRST client rect: an inline <a> that
// wraps across lines reports a getBoundingClientRect() union box
// whose centre can sit on a neighbouring button.
bool tst_HttpOnly::clickElement(WebView *view, const QString &id)
{
    const QVariantList rect = evalSync(view->page(), QString::fromLatin1(
        "var e = document.getElementById('%1');"
        "var r = e && e.getClientRects().length"
        "      ? e.getClientRects()[0] : null;"
        "r ? [r.left + r.width / 2, r.top + r.height / 2] : null")
        .arg(id)).toList();
    if (rect.isEmpty())
        return false;
    const QPointF pos(rect.at(0).toDouble(), rect.at(1).toDouble());
    QWidget *proxy = view->focusProxy() ? view->focusProxy() : view;
    QMouseEvent press(QEvent::MouseButtonPress, pos,
                      proxy->mapToGlobal(pos.toPoint()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, pos,
                        proxy->mapToGlobal(pos.toPoint()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(proxy, &press);
    QCoreApplication::sendEvent(proxy, &release);
    return true;
}

void tst_HttpOnly::initTestCase()
{
    QCoreApplication::setApplicationName("tst_httponly");
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    m_savedHttpsOnly = settings.value(QLatin1String("httpsOnly"));
    m_savedHttpsFirst = settings.value(QLatin1String("httpsFirst"));
    m_savedExceptions =
        settings.value(QLatin1String("httpsOnlyExceptions"));
    settings.endGroup();
}

void tst_HttpOnly::cleanupTestCase()
{
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
    restore(QLatin1String("httpsOnlyExceptions"), m_savedExceptions);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    PrivacyRequestInterceptor::clearDowngradedHosts();
}

void tst_HttpOnly::init()
{
    // Default stance for the tests: the strict warn gate on and the
    // https-first upgrade off, so every public http: URL reaches the
    // warn check directly.  Tests that need the upgrade ordering
    // re-pin it themselves.
    pinPolicy(true, false);
}

void tst_HttpOnly::cleanup()
{
    for (const QString &host : m_touchedHosts)
        PrivacyRequestInterceptor::clearHttpAllowance(host);
    m_touchedHosts.clear();
    PrivacyRequestInterceptor::clearDowngradedHosts();
}

void tst_HttpOnly::warnDecision_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("warn");

    QTest::newRow("public http warns")
        << QString("http://example.test/") << true;
    QTest::newRow("https never warns")
        << QString("https://example.test/") << false;
    QTest::newRow("localhost exempt")
        << QString("http://localhost/") << false;
    QTest::newRow("loopback v4 exempt")
        << QString("http://127.0.0.1:8080/") << false;
    QTest::newRow("loopback v6 exempt")
        << QString("http://[::1]/") << false;
    QTest::newRow("lan 10/8 exempt")
        << QString("http://10.0.0.4/") << false;
    QTest::newRow("lan 192.168 exempt")
        << QString("http://192.168.0.1/") << false;
    QTest::newRow("lan link-local exempt")
        << QString("http://169.254.1.2/") << false;
    QTest::newRow(".local exempt")
        << QString("http://nas.local/") << false;
    QTest::newRow(".onion exempt")
        << QString("http://abcdefghijklmnop.onion/") << false;
    QTest::newRow("data: not a navigation target")
        << QString("data:text/html,hi") << false;
    QTest::newRow("public IP literal warns")
        << QString("http://203.0.113.7/") << true;
}

void tst_HttpOnly::warnDecision()
{
    QFETCH(QString, url);
    QFETCH(bool, warn);
    QCOMPARE(PrivacyRequestInterceptor::shouldWarnHttp(QUrl(url)), warn);
}

void tst_HttpOnly::upgradeOrdering()
{
    // The https-first upgrade runs BEFORE the warn check: a fresh
    // public host still gets its silent https attempt; only once the
    // host is downgraded (secure load failed) does http: warn.
    pinPolicy(true, true);
    const QString host = QStringLiteral("ordered-httponly.test");
    forgetHost(host);
    const QUrl http(QStringLiteral("http://") + host + QLatin1Char('/'));
    const QUrl https(QStringLiteral("https://") + host + QLatin1Char('/'));

    QVERIFY(!PrivacyRequestInterceptor::shouldWarnHttp(http));
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(http));

    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(https));
    QVERIFY(PrivacyRequestInterceptor::shouldWarnHttp(http));

    // A recorded host exception beats the warning again.
    PrivacyRequestInterceptor::allowHttpForHost(host, false);
    QVERIFY(!PrivacyRequestInterceptor::shouldWarnHttp(http));
    PrivacyRequestInterceptor::clearHttpAllowance(host);
    QVERIFY(PrivacyRequestInterceptor::shouldWarnHttp(http));
}

void tst_HttpOnly::warningCommits()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;   // off-the-record: no disk writes
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy warned(page, SIGNAL(httpOnlyInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    const QUrl target(QStringLiteral("http://warned-httponly.test/"));
    page->load(target);
    QTRY_VERIFY_WITH_TIMEOUT(warned.count() == 1, 15000);
    QCOMPARE(warned.at(0).at(0).toUrl(), target);
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    const QString html = domText(page);
    QVERIFY2(html.contains(QLatin1String("warned-httponly.test")),
             qPrintable(html));
    QVERIFY(html.contains(QLatin1String("id=\"back\"")));
    QVERIFY(html.contains(QLatin1String("id=\"proceed\"")));
    QVERIFY(html.contains(
        QLatin1String("arora-http-warning:proceed?n=")));
    QVERIFY(html.contains(
        QLatin1String("arora-http-warning:back?n=")));
    // Off-the-record profiles never offer the persistent exception.
    QVERIFY(!html.contains(QLatin1String("id=\"always\"")));

    // No host was allowed — the veto still applies.
    QVERIFY(!PrivacyRequestInterceptor::isHttpAllowedHost(
        QStringLiteral("warned-httponly.test")));
}

void tst_HttpOnly::proceedAllowsHost()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    const QString host = QStringLiteral("proceed-httponly.test");
    forgetHost(host);
    page->load(QUrl(QStringLiteral("http://") + host + QLatin1Char('/')));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    // "Proceed anyway" records a session-scoped host exception and
    // re-issues the original navigation — which now passes the veto
    // (and fails on the unresolvable name, harmlessly).
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        PrivacyRequestInterceptor::isHttpAllowedHost(host), 10000);
    QVERIFY(!PrivacyRequestInterceptor::shouldWarnHttp(
        QUrl(QStringLiteral("http://") + host + QLatin1Char('/'))));

    // Session-scoped: nothing was written to the persisted list.
    QSettings settings;
    QVERIFY(!settings.value(
        QLatin1String("privacy/httpsOnlyExceptions"))
            .toStringList().contains(host));
}

void tst_HttpOnly::alwaysPersistsException()
{
    pinPolicy(true, false);
    // A named (persistent) profile so the interstitial renders the
    // "always" link — storage stays inside the temporary dir.
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QWebEngineProfile profile(QStringLiteral("tst-httponly-always"));
    profile.setPersistentStoragePath(
        tmp.filePath(QLatin1String("persist")));
    profile.setCachePath(tmp.filePath(QLatin1String("cache")));
    QVERIFY(!profile.isOffTheRecord());
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    const QString host = QStringLiteral("always-httponly.test");
    forgetHost(host);
    page->load(QUrl(QStringLiteral("http://") + host + QLatin1Char('/')));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    const QString html = domText(page);
    QVERIFY2(html.contains(QLatin1String("id=\"always\"")),
             qPrintable(html));

    QVERIFY(clickElement(&view, QLatin1String("always")));
    QTRY_VERIFY_WITH_TIMEOUT(
        PrivacyRequestInterceptor::isHttpAllowedHost(host), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        QSettings().value(QLatin1String("privacy/httpsOnlyExceptions"))
            .toStringList().contains(host), 10000);

    // A reload of the exception snapshot keeps the exception — this is
    // the "survives restart" half (a fresh process re-reads the same
    // QSettings list in loadSettings).
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(PrivacyRequestInterceptor::isHttpAllowedHost(host));
}

void tst_HttpOnly::otrRendersNoAlwaysLink()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;   // off-the-record
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    const QString host = QStringLiteral("otr-httponly.test");
    forgetHost(host);
    page->load(QUrl(QStringLiteral("http://") + host + QLatin1Char('/')));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    QVERIFY(!domText(page).contains(QLatin1String("id=\"always\"")));

    // A forged always link carrying the REAL nonce (leaked page
    // markup, say) must still not persist — resolveHttpWarningLink
    // forces session scope on off-the-record profiles.  Extract the
    // nonce from the rendered proceed link and drive it through a
    // real click on an injected anchor.
    const QString nonce = evalSync(page, QLatin1String(
        "document.getElementById('proceed').href.split('n=')[1]"))
        .toString();
    QVERIFY(!nonce.isEmpty());
    evalSync(page, QString::fromLatin1(
        "var a = document.createElement('a');"
        "a.id = 'forge'; a.href = 'arora-http-warning:always?n=%1';"
        "a.textContent = 'forge'; document.body.appendChild(a); true")
        .arg(nonce));
    QVERIFY(clickElement(&view, QLatin1String("forge")));
    QTRY_VERIFY_WITH_TIMEOUT(
        PrivacyRequestInterceptor::isHttpAllowedHost(host), 10000);
    QTest::qWait(500);
    QVERIFY(!QSettings().value(
        QLatin1String("privacy/httpsOnlyExceptions"))
            .toStringList().contains(host));
}

void tst_HttpOnly::redirectHopWarns()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(httpOnlyInterstitial(QUrl)));

    // A mid-chain redirect lands on plain http: — the hook fires with
    // NavigationTypeRedirect and the hop is refused the same as a
    // typed navigation.
    const QUrl hop(QStringLiteral("http://redirect-httponly.test/land"));
    QVERIFY(!page.acceptNavigationRequest(
        hop, QWebEnginePage::NavigationTypeRedirect, true));
    QTRY_VERIFY_WITH_TIMEOUT(warned.count() == 1, 15000);
    QCOMPARE(warned.at(0).at(0).toUrl(), hop);
}

void tst_HttpOnly::subframeNotWarned()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(httpOnlyInterstitial(QUrl)));

    // An iframe or other subresource navigation to http: is Chromium's
    // mixed-content job, not the interstitial's — the veto must not
    // fire on non-main-frame requests.
    const QUrl sub(QStringLiteral("http://frame-httponly.test/embed"));
    page.acceptNavigationRequest(
        sub, QWebEnginePage::NavigationTypeRedirect, false);
    QTest::qWait(500);
    QCOMPARE(warned.count(), 0);
    QVERIFY(!PrivacyRequestInterceptor::isHttpAllowedHost(
        QStringLiteral("frame-httponly.test")));
}

void tst_HttpOnly::forgedLinkIgnored()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    const QString host = QStringLiteral("forged-httponly.test");
    forgetHost(host);
    page->load(QUrl(QStringLiteral("http://") + host + QLatin1Char('/')));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    // A proceed link without the rendered nonce resolves nothing.
    evalSync(page, QLatin1String(
        "var a = document.createElement('a');"
        "a.id = 'forge'; a.href = 'arora-http-warning:proceed?n=forged';"
        "a.textContent = 'forge'; document.body.appendChild(a); true"));
    QVERIFY(clickElement(&view, QLatin1String("forge")));
    QTest::qWait(1000);
    QVERIFY(!PrivacyRequestInterceptor::isHttpAllowedHost(host));
    QVERIFY(domText(page).contains(QLatin1String("id=\"proceed\"")));

    // The genuine link still works afterwards.
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        PrivacyRequestInterceptor::isHttpAllowedHost(host), 10000);
}

void tst_HttpOnly::backFallsBackToStartPage()
{
    pinPolicy(true, false);
    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(QUrl(QStringLiteral("http://back-httponly.test/")));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    // No real history entry precedes the refused navigation — "Back
    // to safety" falls back to the start page.
    QVERIFY(clickElement(&view, QLatin1String("back")));
    QTRY_VERIFY_WITH_TIMEOUT(
        page->url() == QUrl(QLatin1String("qrc:/startpage.html")),
        15000);
}

void tst_HttpOnly::blockedNavRegistry()
{
    // The interceptor-side record: WebPage consumes a blocked nav to
    // swap in the warning instead of the generic failure page.
    const QUrl url(QStringLiteral("http://registry-httponly.test/x"));
    QVERIFY(!PrivacyRequestInterceptor::takeBlockedHttpNav(url));
    PrivacyRequestInterceptor::recordBlockedHttpNav(url);
    QVERIFY(PrivacyRequestInterceptor::takeBlockedHttpNav(url));
    // Consumed — a second take does not fire.
    QVERIFY(!PrivacyRequestInterceptor::takeBlockedHttpNav(url));
}

void tst_HttpOnly::disabledModeLoadsPlain()
{
    pinPolicy(false, false);   // HTTPS-Only off, upgrade off
    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(httpOnlyInterstitial(QUrl)));

    const QUrl url(QStringLiteral("http://plain-httponly.test/"));
    QVERIFY(page.acceptNavigationRequest(
        url, QWebEnginePage::NavigationTypeTyped, true));
    QTest::qWait(500);
    QCOMPARE(warned.count(), 0);
    QVERIFY(!PrivacyRequestInterceptor::shouldWarnHttp(url));
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    // arora-http-warning: must be registered before the browsing
    // profile spins up (same order as main.cpp) — unregistered schemes
    // go down Chromium's external-protocol path and never reach
    // acceptNavigationRequest.
    SchemeAccessHandler::registerUrlSchemes();
    BrowserApplication app(argc, argv);
    tst_HttpOnly tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_httponly.moc"
