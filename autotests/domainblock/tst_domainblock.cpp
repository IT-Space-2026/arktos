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

// SEC18: the local anti-phishing/malware domain blocklist —
// shouldBlockDomain, the veto in WebPage::acceptNavigationRequest,
// the nonce-bound arora-site-block: interstitial it swaps in, its
// proceed/back action links (session-scoped only, no persisted
// exception), the consume-once blocked-nav registry, the
// privacy/domainBlocklist toggle, and the consent-gated updater's
// fetch->write->reload round trip through a loopback fixture feed.
//
// The fixture list is injected through rc_blocklist_load so the
// tests never depend on the vendored seed's contents.  Under a
// no-rust build the whole feature is absent — the tests then pin
// "never blocks" instead of skipping outright.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qwebengineloadinginfo.h>
#include <qwebengineprofile.h>
#include <qtemporarydir.h>

#include "adblockmanager.h"
#include "domainblocklist.h"
#include "privacyrequestinterceptor.h"
#include "schemeaccesshandler.h"
#include "webpage.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

// Promotes the protected navigation hook so tests can drive redirect
// hops and subframe requests the way Chromium reports them.
class ExposedPage : public WebPage
{
public:
    explicit ExposedPage(QWebEngineProfile *profile)
        : WebPage(profile) {}
    using WebPage::acceptNavigationRequest;
};

class tst_DomainBlock : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void blockDecision();
    void builtinSeedBlocks();
    void warningCommits();
    void proceedAllowsHostSession();
    void forgedLinkIgnored();
    void redirectHopWarns();
    void subframeNotWarned();
    void blockedNavRegistry();
    void disabledModeLoadsPlain();
    void updateConsentGate();
    void updateRoundTrip();

private:
    QVariant evalSync(QWebEnginePage *page, const QString &js);
    QString domText(QWebEnginePage *page);
    bool waitInterstitialCommit(WebPage *page, QSignalSpy &loaded,
                                int baseline);
    bool clickElement(WebView *view, const QString &id);
    void forgetHost(const QString &host);

    QVariant m_savedDomainBlocklist;
    QVariant m_savedConsent;
    QVariant m_savedUpdated;
    QStringList m_touchedHosts;
};

void tst_DomainBlock::forgetHost(const QString &host)
{
    m_touchedHosts << host;
    PrivacyRequestInterceptor::clearBlockedDomainAllowance(host);
}

QVariant tst_DomainBlock::evalSync(QWebEnginePage *page, const QString &js)
{
    QVariant result;
    bool done = false;
    page->runJavaScript(js, [&](const QVariant &v) { result = v; done = true; });
    for (int waited = 0; !done && waited < 10000; waited += 50)
        QTest::qWait(50);
    return result;
}

QString tst_DomainBlock::domText(QWebEnginePage *page)
{
    return evalSync(page, QLatin1String(
        "document.documentElement.outerHTML")).toString();
}

// Waits for the interstitial's own loadFinished(true) commit — the
// DOM is only safe to inspect/click afterwards.
bool tst_DomainBlock::waitInterstitialCommit(WebPage *page,
        QSignalSpy &loaded, int baseline)
{
    for (int waited = 0; waited < 15000; waited += 50) {
        bool committed = false;
        for (int i = baseline; i < loaded.count(); ++i)
            committed |= loaded.at(i).at(0).toBool();
        if (committed
            && page->url().scheme() == QLatin1String("arora-site-block"))
            return true;
        QTest::qWait(50);
    }
    return false;
}

// Synthesises a real mouse click inside the element — element.click()
// is not a user gesture, and Chromium gates custom-protocol
// navigations on one.
bool tst_DomainBlock::clickElement(WebView *view, const QString &id)
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

void tst_DomainBlock::initTestCase()
{
    QCoreApplication::setApplicationName("tst_domainblock");
    QSettings settings;
    m_savedDomainBlocklist =
        settings.value(QLatin1String("privacy/domainBlocklist"));
    m_savedConsent =
        settings.value(QLatin1String("AdBlock/remoteListsConsent"));
    m_savedUpdated =
        settings.value(QLatin1String("privacy/domainBlocklistUpdated"));
}

void tst_DomainBlock::cleanupTestCase()
{
    QSettings settings;
    const auto restore = [&settings](const QString &key,
                                     const QVariant &saved) {
        if (saved.isValid())
            settings.setValue(key, saved);
        else
            settings.remove(key);
    };
    restore(QLatin1String("privacy/domainBlocklist"),
            m_savedDomainBlocklist);
    restore(QLatin1String("AdBlock/remoteListsConsent"), m_savedConsent);
    restore(QLatin1String("privacy/domainBlocklistUpdated"),
            m_savedUpdated);
    PrivacyRequestInterceptor::loadSettings();
    // The fixture override the updater wrote goes with the suite.
    QFile::remove(DomainBlocklist::listFilePath());
#if defined(ARORA_RUSTCORE)
    rc_blocklist_reload();
#endif
}

void tst_DomainBlock::init()
{
    QSettings settings;
    settings.setValue(QLatin1String("privacy/domainBlocklist"), true);
    PrivacyRequestInterceptor::loadSettings();
#if defined(ARORA_RUSTCORE)
    // Every test starts from a known fixture list — never the
    // vendored seed or a stale download.
    const QByteArray fixture =
        "# tst_domainblock fixture\n"
        "bad-fixture.test\n"
        "listed-parent.test\n"
        "sub.listed.test\n";
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(fixture.constData()),
                 size_t(fixture.size())),
             RC_OK);
#endif
}

void tst_DomainBlock::cleanup()
{
    for (const QString &host : m_touchedHosts)
        PrivacyRequestInterceptor::clearBlockedDomainAllowance(host);
    m_touchedHosts.clear();
    PrivacyRequestInterceptor::clearBlockedDomainAllowances();
}

void tst_DomainBlock::blockDecision()
{
    using P = PrivacyRequestInterceptor;
#if defined(ARORA_RUSTCORE)
    QVERIFY(P::shouldBlockDomain(
                QUrl("http://bad-fixture.test/")));
    QVERIFY(P::shouldBlockDomain(
                QUrl("https://bad-fixture.test/path?q=1")));
    // Suffix match: a listed domain blocks every subdomain.
    QVERIFY(P::shouldBlockDomain(
                QUrl("https://deep.sub.listed.test/")));
    // ...but a listed subdomain never reaches up to its parent.
    QVERIFY(!P::shouldBlockDomain(
                QUrl("https://listed.test/")));
    // Unlisted domains are unaffected, substring lookalikes included.
    QVERIFY(!P::shouldBlockDomain(
                QUrl("https://clean-fixture.test/")));
    QVERIFY(!P::shouldBlockDomain(
                QUrl("https://notbad-fixture.test/")));
    QVERIFY(!P::shouldBlockDomain(
                QUrl("https://bad-fixture.test.evil2.test/")));
    // Non-web schemes never consult the list.
    QVERIFY(!P::shouldBlockDomain(
                QUrl("data:text/html,bad-fixture.test")));
    QVERIFY(!P::shouldBlockDomain(
                QUrl("ftp://bad-fixture.test/")));
    // A session-scoped proceed allowance suppresses the decision.
    P::allowBlockedDomain(QStringLiteral("bad-fixture.test"));
    m_touchedHosts << QStringLiteral("bad-fixture.test");
    QVERIFY(!P::shouldBlockDomain(
                QUrl("https://bad-fixture.test/")));
#else
    // No-rust build: nothing is ever blocked.
    QVERIFY(!P::shouldBlockDomain(
                QUrl("https://bad-fixture.test/")));
    QVERIFY(!P::isDomainBlocked(QStringLiteral("bad-fixture.test")));
#endif
}

void tst_DomainBlock::builtinSeedBlocks()
{
#if defined(ARORA_RUSTCORE)
    // The vendored seed blocks without any override file at all —
    // remove a stale updater output, re-point the core at the real
    // data dir and re-merge.
    QFile::remove(DomainBlocklist::listFilePath());
    const QByteArray dir = QFileInfo(DomainBlocklist::listFilePath())
        .absolutePath().toUtf8();
    QCOMPARE(rc_set_data_dir(dir.constData()), RC_OK);
    QCOMPARE(rc_blocklist_reload(), RC_OK);
    QVERIFY(rc_blocklist_count() > 0);
    // Google's own safe-browsing probe domain is in the seed.
    QCOMPARE(rc_blocklist_check("testsafebrowsing.appspot.com"), 1);
    // Unlisted domains are unaffected.
    QCOMPARE(rc_blocklist_check("example.com"), 0);
    QCOMPARE(rc_blocklist_check("arora-browser.org"), 0);
#endif
}

void tst_DomainBlock::warningCommits()
{
    const QString host = QStringLiteral("bad-fixture.test");
    forgetHost(host);
#if defined(ARORA_RUSTCORE)
    QWebEngineProfile profile;   // off-the-record: no disk writes
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy warned(page, SIGNAL(domainBlockInterstitial(QUrl)));
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    const QUrl target(QStringLiteral("https://") + host + QLatin1Char('/'));
    page->load(target);
    QTRY_VERIFY_WITH_TIMEOUT(warned.count() == 1, 15000);
    QCOMPARE(warned.at(0).at(0).toUrl(), target);
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    const QString html = domText(page);
    QVERIFY2(html.contains(host), qPrintable(html));
    QVERIFY(html.contains(QLatin1String("id=\"back\"")));
    QVERIFY(html.contains(QLatin1String("id=\"proceed\"")));
    QVERIFY(html.contains(
        QLatin1String("arora-site-block:proceed?n=")));
    QVERIFY(html.contains(
        QLatin1String("arora-site-block:back?n=")));
    // Deliberately never offered: no persisted bypass of a listed
    // hostile domain — not even on a persistent profile.
    QVERIFY(!html.contains(QLatin1String("id=\"always\"")));

    QVERIFY(!PrivacyRequestInterceptor::isBlockedDomainAllowed(host));
#else
    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(domainBlockInterstitial(QUrl)));
    QVERIFY(page.acceptNavigationRequest(
        QUrl(QStringLiteral("https://") + host + QLatin1Char('/')),
        QWebEnginePage::NavigationTypeTyped, true));
    QTest::qWait(500);
    QCOMPARE(warned.count(), 0);
#endif
}

void tst_DomainBlock::proceedAllowsHostSession()
{
#if defined(ARORA_RUSTCORE)
    const QString host = QStringLiteral("bad-fixture.test");
    forgetHost(host);
    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(QUrl(QStringLiteral("https://") + host + QLatin1Char('/')));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    // "Proceed anyway" records a session-scoped allowance and
    // re-issues the original navigation — which now passes the veto
    // (and fails on the unresolvable name, harmlessly).
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        PrivacyRequestInterceptor::isBlockedDomainAllowed(host), 10000);
    QVERIFY(!PrivacyRequestInterceptor::shouldBlockDomain(
        QUrl(QStringLiteral("https://") + host + QLatin1Char('/'))));

    // Session-scoped only: nothing was written to any QSettings
    // exception list.
    QVERIFY(!QSettings().value(
        QLatin1String("privacy/httpsOnlyExceptions"))
            .toStringList().contains(host));
#endif
}

void tst_DomainBlock::forgedLinkIgnored()
{
#if defined(ARORA_RUSTCORE)
    const QString host = QStringLiteral("bad-fixture.test");
    forgetHost(host);
    QWebEngineProfile profile;
    WebView view(&profile);
    view.resize(800, 600);
    view.show();
    WebPage *page = view.webPage();
    QSignalSpy loaded(page, SIGNAL(loadFinished(bool)));

    page->load(QUrl(QStringLiteral("https://") + host + QLatin1Char('/')));
    QVERIFY(waitInterstitialCommit(page, loaded, 0));

    // A proceed link without the rendered nonce resolves nothing.
    evalSync(page, QLatin1String(
        "var a = document.createElement('a');"
        "a.id = 'forge'; a.href = 'arora-site-block:proceed?n=forged';"
        "a.textContent = 'forge'; document.body.appendChild(a); true"));
    QVERIFY(clickElement(&view, QLatin1String("forge")));
    QTest::qWait(1000);
    QVERIFY(!PrivacyRequestInterceptor::isBlockedDomainAllowed(host));
    QVERIFY(domText(page).contains(QLatin1String("id=\"proceed\"")));

    // The genuine link still works afterwards.
    QVERIFY(clickElement(&view, QLatin1String("proceed")));
    QTRY_VERIFY_WITH_TIMEOUT(
        PrivacyRequestInterceptor::isBlockedDomainAllowed(host), 10000);
#endif
}

void tst_DomainBlock::redirectHopWarns()
{
#if defined(ARORA_RUSTCORE)
    const QString host = QStringLiteral("bad-fixture.test");
    forgetHost(host);
    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(domainBlockInterstitial(QUrl)));

    // A mid-chain redirect lands on a listed host — the hook fires
    // with NavigationTypeRedirect and the hop is refused the same as
    // a typed navigation.
    const QUrl hop(QStringLiteral("https://") + host
                   + QLatin1String("/land"));
    QVERIFY(!page.acceptNavigationRequest(
        hop, QWebEnginePage::NavigationTypeRedirect, true));
    QTRY_VERIFY_WITH_TIMEOUT(warned.count() == 1, 15000);
    QCOMPARE(warned.at(0).at(0).toUrl(), hop);
#endif
}

void tst_DomainBlock::subframeNotWarned()
{
#if defined(ARORA_RUSTCORE)
    const QString host = QStringLiteral("bad-fixture.test");
    forgetHost(host);
    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(domainBlockInterstitial(QUrl)));

    // A listed host inside a frame is NOT the navigation
    // interstitial's job — the veto must not fire on non-main-frame
    // requests (the adblock matcher covers subresources).
    const QUrl sub(QStringLiteral("https://") + host
                   + QLatin1String("/embed"));
    page.acceptNavigationRequest(
        sub, QWebEnginePage::NavigationTypeRedirect, false);
    QTest::qWait(500);
    QCOMPARE(warned.count(), 0);
    QVERIFY(!PrivacyRequestInterceptor::isBlockedDomainAllowed(host));
#endif
}

void tst_DomainBlock::blockedNavRegistry()
{
    // The interceptor-side record: WebPage consumes a blocked nav to
    // swap in the warning instead of the generic failure page.
    const QUrl url(QStringLiteral("https://registry-block.test/x"));
    QVERIFY(!PrivacyRequestInterceptor::takeBlockedDomainNav(url));
    PrivacyRequestInterceptor::recordBlockedDomainNav(url);
    QVERIFY(PrivacyRequestInterceptor::takeBlockedDomainNav(url));
    // Consumed — a second take does not fire.
    QVERIFY(!PrivacyRequestInterceptor::takeBlockedDomainNav(url));
}

void tst_DomainBlock::disabledModeLoadsPlain()
{
    QSettings().setValue(QLatin1String("privacy/domainBlocklist"), false);
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(!PrivacyRequestInterceptor::domainBlocklistEnabled());

    QWebEngineProfile profile;
    ExposedPage page(&profile);
    QSignalSpy warned(&page, SIGNAL(domainBlockInterstitial(QUrl)));

    const QUrl url(QStringLiteral("https://bad-fixture.test/"));
    QVERIFY(page.acceptNavigationRequest(
        url, QWebEnginePage::NavigationTypeTyped, true));
    QTest::qWait(500);
    QCOMPARE(warned.count(), 0);
    QVERIFY(!PrivacyRequestInterceptor::shouldBlockDomain(url));
}

// A tiny HTTP fixture that serves a domain list and counts hits.
class FixtureListServer : public QObject
{
public:
    int hits = 0;
    QByteArray body =
        "# fixture feed\n"
        "served-fixture.test\n"
        "127.0.0.1\thostfile-served.test\n"
        "https://url-served.test/phish/path\n";

    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *client = m_server.nextPendingConnection()) {
                ++hits;
                connect(client, &QTcpSocket::readyRead, this,
                        [this, client]() {
                    const QByteArray reply =
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: "
                        + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n" + body;
                    client->write(reply);
                    client->disconnectFromHost();
                });
            }
        });
        return true;
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/feed")
                    .arg(m_server.serverPort()));
    }

private:
    QTcpServer m_server;
};

void tst_DomainBlock::updateConsentGate()
{
#if defined(ARORA_RUSTCORE)
    // Declined consent -> updateIfStale never fetches.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsDeclined);
    QSettings().remove(QLatin1String("privacy/domainBlocklistUpdated"));
    FixtureListServer server;
    QVERIFY(server.start());
    DomainBlocklist *updater = DomainBlocklist::instance();
    updater->setSourcesForTest({ server.url() });
    QSignalSpy finished(updater, SIGNAL(updateFinished(bool)));

    updater->updateIfStale();
    QTest::qWait(500);
    QCOMPARE(server.hits, 0);
    QCOMPARE(finished.count(), 0);
#endif
}

void tst_DomainBlock::updateRoundTrip()
{
#if defined(ARORA_RUSTCORE)
    // Granted consent + a stale stamp -> updateIfStale fetches the
    // fixture feed, writes the override file and reloads the core.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsGranted);
    QSettings().remove(QLatin1String("privacy/domainBlocklistUpdated"));
    QFile::remove(DomainBlocklist::listFilePath());
    FixtureListServer server;
    QVERIFY(server.start());
    DomainBlocklist *updater = DomainBlocklist::instance();
    updater->setSourcesForTest({ server.url() });
    QSignalSpy finished(updater, SIGNAL(updateFinished(bool)));

    updater->updateIfStale();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 15000);
    QVERIFY(finished.at(0).at(0).toBool());
    QCOMPARE(server.hits, 1);

    const QString path = DomainBlocklist::listFilePath();
    QVERIFY(QFile::exists(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(file.readAll())
            .contains(QLatin1String("served-fixture.test")));
    file.close();

    // Every row shape the feed carries landed in the merged set.
    QCOMPARE(rc_blocklist_check("served-fixture.test"), 1);
    QCOMPARE(rc_blocklist_check("hostfile-served.test"), 1);
    QCOMPARE(rc_blocklist_check("url-served.test"), 1);
    QCOMPARE(rc_blocklist_check("www.served-fixture.test"), 1);
    QCOMPARE(rc_blocklist_check("unlisted-served.test"), 0);
    QVERIFY(DomainBlocklist::instance()->lastUpdate().isValid());

    // A second stale-check within the interval fetches nothing.
    updater->updateIfStale();
    QTest::qWait(500);
    QCOMPARE(server.hits, 1);

    updater->setSourcesForTest(DomainBlocklist::defaultSources());
#endif
}

int main(int argc, char *argv[])
{
    Q_INIT_RESOURCE(htmls);
    Q_INIT_RESOURCE(data);
    // arora-site-block: must be registered before the browsing
    // profile spins up (same order as main.cpp) — unregistered schemes
    // go down Chromium's external-protocol path and never reach
    // acceptNavigationRequest.
    SchemeAccessHandler::registerUrlSchemes();
    BrowserApplication app(argc, argv);
    tst_DomainBlock tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "tst_domainblock.moc"
