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

// SHLD01: the shield panel backend and widget — whitelist filter
// generation/validation, AdBlockManager per-site toggle, CookieJar
// per-host rules and cookie removal, and the SitePanel itself driven
// against a real page from an in-process HTTP server (cookie count,
// permission revoke, content-blocking toggle, clear site data).

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qcheckbox.h>
#include <qcombobox.h>
#include <qlabel.h>
#include <qnetworkcookie.h>
#include <qpushbutton.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginepermission.h>
#include <qwebengineprofile.h>
#include <qwidgetaction.h>

#include "adblockmanager.h"
#include "adblockrule.h"
#include "adblocksubscription.h"
#include "cookiejar.h"
#include "popupblocker.h"
#include "sitepanel.h"
#include "siteshield.h"
#include "webpermissionmanager.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

// Minimal HTTP/1.0 responder — same pattern as tst_webpermissions.
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
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                if (!socket->peek(4096).contains("\r\n\r\n"))
                    return;
                socket->readAll();
                const QByteArray body("<html><body>hi</body></html>");
                socket->write(QByteArrayLiteral(
                    "HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                    "Content-Length: ")
                    + QByteArray::number(body.size())
                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                    + body);
                socket->disconnectFromHost();
            });
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

class tst_SitePanel : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void init();
    void cleanup();

private slots:
    void siteWhitelistFilter_data();
    void siteWhitelistFilter();
    void siteWhitelistToggle();
    void cookieRuleHelpers();
    void removeCookiesForHost();
    void panelWithoutPage();
    void panelForSite();
};

void tst_SitePanel::initTestCase()
{
    QCoreApplication::setApplicationName("tst_sitepanel");
    QVERIFY(AdBlockManager::instance());
}

void tst_SitePanel::init()
{
    // The exception lists are QSettings-backed — wipe what an earlier
    // (possibly interrupted) run left behind so each test starts empty.
    CookieJar jar;
    jar.setBlockedCookies(QStringList());
    jar.setAllowedCookies(QStringList());
    jar.setAllowForSessionCookies(QStringList());
    jar.setCookies(QList<QNetworkCookie>());
    PopupBlocker::instance()->clearAllowedHosts();
    PopupBlocker::instance()->clearSessionHosts();
}

void tst_SitePanel::cleanup()
{
    // The custom-rules file and the cookie exceptions persist — never
    // leak test state into the next run or a real profile.
    AdBlockManager::instance()->setSiteWhitelisted(
        QLatin1String("127.0.0.1"), false);
    AdBlockManager::instance()->setSiteWhitelisted(
        QLatin1String("example.com"), false);
    if (CookieJar *jar = CookieJar::instance()) {
        jar->clearRuleForHost(QLatin1String("127.0.0.1"));
        jar->clearRuleForHost(QLatin1String("example.com"));
        jar->setCookies(QList<QNetworkCookie>());
    }
    WebPermissionManager::instance()->clearEntries();
    PopupBlocker::instance()->removeAllowedHost(QLatin1String("127.0.0.1"));
    PopupBlocker::instance()->removeAllowedHost(QLatin1String("example.com"));
    PopupBlocker::instance()->clearSessionHosts();
}

// public static QString siteWhitelistFilter(const QString &host)
void tst_SitePanel::siteWhitelistFilter_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<QString>("filter");

    QTest::newRow("domain")
        << "example.com" << "@@||example.com^$document";
    QTest::newRow("subdomain")
        << "www.Example.COM" << "@@||www.example.com^$document";
    QTest::newRow("ipv4")
        << "127.0.0.1" << "@@||127.0.0.1^$document";
    QTest::newRow("idn punycode")
        << QString::fromUtf8("b\u00fccher.example")
        << "@@||xn--bcher-kva.example^$document";
    QTest::newRow("slash rejected") << "evil.com/path" << "";
    QTest::newRow("space rejected") << "exa mple.com" << "";
    QTest::newRow("ipv6 rejected") << "::1" << "";
    QTest::newRow("empty rejected") << "" << "";
}

void tst_SitePanel::siteWhitelistFilter()
{
    QFETCH(QString, host);
    QFETCH(QString, filter);
    QCOMPARE(AdBlockManager::siteWhitelistFilter(host), filter);
}

void tst_SitePanel::siteWhitelistToggle()
{
    AdBlockManager *manager = AdBlockManager::instance();
    QVERIFY(!manager->isSiteWhitelisted(QLatin1String("example.com")));

    manager->setSiteWhitelisted(QLatin1String("example.com"), true);
    QVERIFY(manager->isSiteWhitelisted(QLatin1String("example.com")));

    // Toggling twice must not stack duplicate rules.
    manager->setSiteWhitelisted(QLatin1String("example.com"), true);
    int count = 0;
    const QList<AdBlockRule> rules = manager->customRules()->allRules();
    for (const AdBlockRule &rule : rules) {
        if (rule.filter() == QLatin1String("@@||example.com^$document"))
            ++count;
    }
    QCOMPARE(count, 1);

    manager->setSiteWhitelisted(QLatin1String("example.com"), false);
    QVERIFY(!manager->isSiteWhitelisted(QLatin1String("example.com")));
}

// CookieJar::ruleForHost / setRuleForHost / clearRuleForHost
void tst_SitePanel::cookieRuleHelpers()
{
    CookieJar jar;
    CookieJar::CookieRule rule = CookieJar::Allow;

    // No exception at all — the jar reports "no rule".
    QVERIFY(!jar.ruleForHost(QLatin1String("example.com"), &rule));

    jar.setRuleForHost(QLatin1String("example.com"), CookieJar::Block);
    QVERIFY(jar.ruleForHost(QLatin1String("example.com"), &rule));
    QCOMPARE(rule, CookieJar::Block);
    QCOMPARE(jar.blockedCookies(),
             QStringList() << QLatin1String("example.com"));
    QVERIFY(jar.allowedCookies().isEmpty());
    QVERIFY(jar.allowForSessionCookies().isEmpty());

    // Switching rules moves the host between lists.
    jar.setRuleForHost(QLatin1String("example.com"),
                       CookieJar::AllowForSession);
    QVERIFY(jar.ruleForHost(QLatin1String("example.com"), &rule));
    QCOMPARE(rule, CookieJar::AllowForSession);
    QVERIFY(jar.blockedCookies().isEmpty());
    QCOMPARE(jar.allowForSessionCookies(),
             QStringList() << QLatin1String("example.com"));

    jar.setRuleForHost(QLatin1String("example.com"), CookieJar::Allow);
    QVERIFY(jar.ruleForHost(QLatin1String("example.com"), &rule));
    QCOMPARE(rule, CookieJar::Allow);
    QCOMPARE(jar.allowedCookies(),
             QStringList() << QLatin1String("example.com"));

    jar.clearRuleForHost(QLatin1String("example.com"));
    QVERIFY(!jar.ruleForHost(QLatin1String("example.com"), &rule));

    // A parent-domain ".example.com" entry still scopes the host.
    jar.setBlockedCookies(QStringList() << QLatin1String(".example.com"));
    QVERIFY(jar.ruleForHost(QLatin1String("www.example.com"), &rule));
    QCOMPARE(rule, CookieJar::Block);
    // Block wins over an allow list containing the same host.
    jar.setAllowedCookies(QStringList() << QLatin1String(".example.com"));
    QVERIFY(jar.ruleForHost(QLatin1String("www.example.com"), &rule));
    QCOMPARE(rule, CookieJar::Block);
}

static QNetworkCookie makeSiteCookie(const QByteArray &name,
                                     const QString &domain)
{
    QNetworkCookie cookie(name, "1");
    cookie.setDomain(domain);
    return cookie;
}

void tst_SitePanel::removeCookiesForHost()
{
    CookieJar jar;
    jar.setCookies(QList<QNetworkCookie>()
        // scoped to the host itself
        << makeSiteCookie("a", "example.com")
        // host-attribute form
        << makeSiteCookie("b", ".example.com")
        // a subdomain the site controls
        << makeSiteCookie("c", "sub.example.com")
        // a parent domain that still covers the host
        << makeSiteCookie("d", "example.com")
        // unrelated — must survive
        << makeSiteCookie("e", "other.org"));

    // Every example.com-scoped cookie is delivered to sub.example.com,
    // so scoping to the subdomain still wipes the parent-domain jars.
    jar.removeCookiesForHost(QLatin1String("sub.example.com"));
    QCOMPARE(jar.cookies().count(), 1);
    QCOMPARE(jar.cookies().first().name(), QByteArray("e"));
}

void tst_SitePanel::panelWithoutPage()
{
    SitePanel panel;
    panel.refresh();
    QLabel *hostLabel = panel.findChild<QLabel*>(
        QLatin1String("sitePanelHost"));
    QVERIFY(hostLabel);
    QCOMPARE(hostLabel->text(), QStringLiteral("This page"));

    QComboBox *combo = panel.findChild<QComboBox*>(
        QLatin1String("siteCookieRule"));
    QVERIFY(combo);
    QVERIFY(!combo->isEnabled());

    QCheckBox *block = panel.findChild<QCheckBox*>(
        QLatin1String("siteBlockContent"));
    QVERIFY(block);
    QVERIFY(!block->isEnabled());

    // POPUP01: no site means nothing to scope a pop-up exception to.
    QCheckBox *popups = panel.findChild<QCheckBox*>(
        QLatin1String("siteAllowPopups"));
    QVERIFY(popups);
    QVERIFY(!popups->isEnabled());
    QVERIFY(!popups->isChecked());

    QPushButton *clear = panel.findChild<QPushButton*>(
        QLatin1String("siteClearData"));
    QVERIFY(clear);
    QVERIFY(!clear->isEnabled());
}

void tst_SitePanel::panelForSite()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QUrl url = server.url();
    const QString host = url.host();
    const QUrl origin = QUrl(QString::fromLatin1("http://%1:%2")
                             .arg(host).arg(url.port()));

    WebView view;
    SitePanel panel;
    panel.setWebView(&view);

    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    view.load(url);
    QTRY_COMPARE(loadSpy.count(), 1);
    panel.refresh();

    // Host + HTTP security line.
    QLabel *hostLabel = panel.findChild<QLabel*>(
        QLatin1String("sitePanelHost"));
    QVERIFY(hostLabel);
    QCOMPARE(hostLabel->text(), host);
    QLabel *security = panel.findChild<QLabel*>(
        QLatin1String("sitePanelSecurity"));
    QVERIFY(security);
    QVERIFY(security->text().contains(QLatin1String("not secure")));

    // Cookie count reflects the profile jar's mirror.
    CookieJar *jar = CookieJar::instance(view.page()->profile());
    QVERIFY(jar);
    jar->setCookies(QList<QNetworkCookie>()
        << makeSiteCookie("a", host)
        << makeSiteCookie("e", "other.org"));
    panel.refresh();
    QLabel *count = panel.findChild<QLabel*>(
        QLatin1String("siteCookieCount"));
    QVERIFY(count);
    QVERIFY(count->text().startsWith(QLatin1String("1 cookies")));

    // Cookie combo writes through to the jar's exception lists.
    QComboBox *combo = panel.findChild<QComboBox*>(
        QLatin1String("siteCookieRule"));
    QVERIFY(combo);
    QVERIFY(combo->isEnabled());
    combo->activated(combo->findData(static_cast<int>(CookieJar::Block)));
    CookieJar::CookieRule rule = CookieJar::Allow;
    QVERIFY(jar->ruleForHost(host, &rule));
    QCOMPARE(rule, CookieJar::Block);
    combo->activated(0);
    QVERIFY(!jar->ruleForHost(host, &rule));

    // The content-blocking toggle drives the adblock whitelist.
    QCheckBox *block = panel.findChild<QCheckBox*>(
        QLatin1String("siteBlockContent"));
    QVERIFY(block);
    AdBlockManager *adblock = AdBlockManager::instance();
    const bool enabled = adblock->isEnabled();
    adblock->setEnabled(true);
    panel.refresh();
    QVERIFY(block->isEnabled());
    QVERIFY(block->isChecked());
    block->setChecked(false);
    QVERIFY(adblock->isSiteWhitelisted(host));
    panel.refresh();
    QVERIFY(!block->isChecked());
    block->setChecked(true);
    QVERIFY(!adblock->isSiteWhitelisted(host));
    adblock->setEnabled(enabled);

    // POPUP01: the pop-up toggle writes the PopupBlocker exception —
    // checked again after a refresh, and removed on untoggle.
    QCheckBox *popups = panel.findChild<QCheckBox*>(
        QLatin1String("siteAllowPopups"));
    QVERIFY(popups);
    QVERIFY(popups->isEnabled());
    QVERIFY(!popups->isChecked());
    PopupBlocker *blocker = PopupBlocker::instance();
    popups->setChecked(true);
    QVERIFY(blocker->isAllowedHost(host));
    panel.refresh();
    QVERIFY(popups->isChecked());
    popups->setChecked(false);
    QVERIFY(!blocker->isAllowedHost(host));
    panel.refresh();
    QVERIFY(!popups->isChecked());

    // SEC05 permission rows: seed a remembered grant, revoke it from
    // the panel, and check the broker forgets it.
    WebPermissionManager *permissions = WebPermissionManager::instance();
    permissions->setEntry(origin,
        QWebEnginePermission::PermissionType::Notifications, true);
    panel.refresh();
    QWidget *box = panel.findChild<QWidget*>(
        QLatin1String("sitePermissions"));
    QVERIFY(box);
    QPushButton *revoke = nullptr;
    const QList<QPushButton*> buttons = box->findChildren<QPushButton*>();
    for (QPushButton *button : buttons) {
        if (button->text() == QLatin1String("Revoke"))
            revoke = button;
    }
    QVERIFY(revoke);
    revoke->click();
    bool stillThere = false;
    const QList<WebPermissionManager::Entry> entries =
        permissions->entries();
    for (const WebPermissionManager::Entry &entry : entries) {
        if (entry.origin == origin)
            stillThere = true;
    }
    QVERIFY(!stillThere);

    // Clear site data empties the site's cookies from the mirror.
    QPushButton *clear = panel.findChild<QPushButton*>(
        QLatin1String("siteClearData"));
    QVERIFY(clear);
    QVERIFY(clear->isEnabled());
    clear->click();
    QCOMPARE(jar->cookiesForUrl(url).count(), 0);
    QCOMPARE(jar->cookies().count(), 1); // other.org survives
}

QTEST_MAIN(tst_SitePanel)
#include "tst_sitepanel.moc"
