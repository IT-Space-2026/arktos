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
#include <qlayout.h>
#include <qnetworkcookie.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebenginepermission.h>
#include <qwebengineprofile.h>
#include <qwidgetaction.h>

#include "adblockmanager.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#include <rustcore.h>
#endif
#include "adblockrule.h"
#include "adblocksubscription.h"
#include "aroraicon.h"
#include "browsertheme.h"
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
    void trustSignalLines();
    void trustBadgeLevels();
    void trustBadgeCache();
    void trustBadgeNavigation();
    void trustHeaderStyling();
    void trustGating();
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

    // TRUST03: a gating leg that bailed mid-run must not leave the
    // trust switches off for the other tests in this binary.
    QSettings settings;
    settings.remove(QLatin1String("trust/enabled"));
    settings.remove(QLatin1String("trust/lookalike"));
    settings.remove(QLatin1String("trust/threatlist"));
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
#if defined(ARORA_RUSTCORE)
    const int rowsBefore =
        SiteDecisionStore::entries(SiteDecisionStore::KindAdBlock).count();
#endif

    manager->setSiteWhitelisted(QLatin1String("example.com"), true);
    QVERIFY(manager->isSiteWhitelisted(QLatin1String("example.com")));

    // Toggling twice must not stack duplicate rules.
    manager->setSiteWhitelisted(QLatin1String("example.com"), true);
#if defined(ARORA_RUSTCORE)
    // The whitelist is a keyed row in the decision store — a second
    // toggle rewrites the same row, so duplication is impossible.
    {
        QString stored;
        QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindAdBlock,
                                       QLatin1String("example.com"),
                                       &stored));
        QCOMPARE(stored, QLatin1String("allow"));
        QCOMPARE(SiteDecisionStore::entries(SiteDecisionStore::KindAdBlock)
                     .count(), rowsBefore + 1);
    }
#else
    int count = 0;
    const QList<AdBlockRule> rules = manager->customRules()->allRules();
    for (const AdBlockRule &rule : rules) {
        if (rule.filter() == QLatin1String("@@||example.com^$document"))
            ++count;
    }
    QCOMPARE(count, 1);
#endif

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

    // SHLD02: the merged content-blocking section — the global switch
    // needs no site, the count and settings entry are always present.
    QCheckBox *global = panel.findChild<QCheckBox*>(
        QLatin1String("siteContentBlocking"));
    QVERIFY(global);
    QVERIFY(global->isEnabled());
    QLabel *blockedCount = panel.findChild<QLabel*>(
        QLatin1String("siteBlockedCount"));
    QVERIFY(blockedCount);
    QVERIFY(!blockedCount->text().isEmpty());
    QVERIFY(panel.findChild<QPushButton*>(
                QLatin1String("siteAdBlockSettings")));

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

    // TRUST08: no host — the Trust section hides entirely.
    QWidget *trust = panel.findChild<QWidget*>(QLatin1String("siteTrust"));
    QVERIFY(trust);
    QVERIFY(trust->isHidden());
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

    // SHLD02: the panel's global switch drives AdBlockManager — off
    // greys the per-site row and reports on the count line, back on
    // restores both.
    QCheckBox *global = panel.findChild<QCheckBox*>(
        QLatin1String("siteContentBlocking"));
    QLabel *blockedCount = panel.findChild<QLabel*>(
        QLatin1String("siteBlockedCount"));
    QVERIFY(global);
    QVERIFY(blockedCount);
    global->setChecked(false);
    QVERIFY(!adblock->isEnabled());
    QVERIFY(!block->isEnabled());
    QVERIFY(blockedCount->text().contains(QLatin1String("disabled"),
                                         Qt::CaseInsensitive));
    global->setChecked(true);
    QVERIFY(adblock->isEnabled());
    QVERIFY(block->isEnabled());
    QVERIFY(blockedCount->text().contains(QLatin1String("blocked"),
                                         Qt::CaseInsensitive));
    QVERIFY(panel.findChild<QPushButton*>(
                QLatin1String("siteAdBlockSettings")));
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

    // TRUST08: the Trust section is keyed to the page's host — a
    // localhost page is unlisted, so the informative line is the
    // little-known one; at minimum one real signal line must exist
    // under the header.
    QWidget *trust = panel.findChild<QWidget*>(QLatin1String("siteTrust"));
    QVERIFY(trust);
#if defined(ARORA_RUSTCORE)
    QVERIFY(!trust->isHidden());
    const QList<QLabel*> trustLabels = trust->findChildren<QLabel*>();
    QVERIFY(trustLabels.count() >= 2); // header + >=1 signal line
    QString joined;
    for (const QLabel *label : trustLabels)
        joined += label->text() + QLatin1Char('\n');
    QVERIFY(joined.contains(QLatin1String("known"),
                            Qt::CaseInsensitive));
#else
    QVERIFY(trust->isHidden());
#endif
}

// TRUST08: the verdict-to-lines mapping against the live rustcore
// lookup — fixture tables go in through the *_load test seams and
// the vendored seeds are restored afterwards so no state leaks into
// the other tests in this binary.
void tst_SitePanel::trustSignalLines()
{
#if defined(ARORA_RUSTCORE)
    const QByteArray tranco = "4,known-fixture.test\n1,paypal.com\n";
    QCOMPARE(rc_tranco_load(
                 reinterpret_cast<const uint8_t *>(tranco.constData()),
                 size_t(tranco.size())),
             RC_OK);
    const QByteArray block = "threat-fixture.test\n";
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(block.constData()),
                 size_t(block.size())),
             RC_OK);

    // Ranked host over https -> the 'known site' line.
    QString joined =
        SitePanel::trustSignalLines(QLatin1String("known-fixture.test"),
                                    true).join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QLatin1String("known site"),
                            Qt::CaseInsensitive));

    // A confusable twin of a ranked domain -> 'resembles <domain>'.
    joined = SitePanel::trustSignalLines(QLatin1String("paypa1.com"),
                                       true).join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QLatin1String("resembles"),
                            Qt::CaseInsensitive));
    QVERIFY(joined.contains(QLatin1String("paypal.com")));

    // A listed domain -> the threat line.
    joined =
        SitePanel::trustSignalLines(QLatin1String("threat-fixture.test"),
                                  true).join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QLatin1String("threat"),
                            Qt::CaseInsensitive));

    // An unlisted host -> the informative 'little-known' line, never a
    // neutral "nothing found" noise line.
    joined =
        SitePanel::trustSignalLines(QLatin1String("obscure-fixture.test"),
                                  true).join(QLatin1Char('\n'));
    QVERIFY(joined.contains(QLatin1String("little-known"),
                            Qt::CaseInsensitive));
    QVERIFY(!joined.contains(QLatin1String("no threat"),
                             Qt::CaseInsensitive));

    // An empty host cannot evaluate — no lines, the section hides.
    QVERIFY(SitePanel::trustSignalLines(QString(), true).isEmpty());

    // Restore the vendored seed ∪ override tables for later tests.
    rc_sitetrust_reload();
#endif
}

// TRUST09: the level word behind the badge — fixture tables exercise
// every branch of the verdict mapping, and the tint seam proves
// green/unknown leave the lock alone while amber and red repaint it.
void tst_SitePanel::trustBadgeLevels()
{
#if defined(ARORA_RUSTCORE)
    SiteShieldButton::clearTrustCache();
    const QByteArray tranco = "4,known-fixture.test\n1,paypal.com\n";
    QCOMPARE(rc_tranco_load(
                 reinterpret_cast<const uint8_t *>(tranco.constData()),
                 size_t(tranco.size())),
             RC_OK);
    const QByteArray block = "threat-fixture.test\n";
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(block.constData()),
                 size_t(block.size())),
             RC_OK);

    // Ranked host over https -> green -> the lock stays untouched.
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("known-fixture.test"), true),
             QLatin1String("green"));

    // Threat-list hit and a confusable twin of a ranked domain ->
    // red — the verdict that overrides the lock.
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("threat-fixture.test"), true),
             QLatin1String("red"));
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("paypa1.com"), true),
             QLatin1String("red"));

    // Unranked host -> amber; plaintext http ambers even a ranked
    // host — both get the amber tint.
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("obscure-fixture.test"), true),
             QLatin1String("amber"));
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("known-fixture.test"), false),
             QLatin1String("amber"));

    // Nothing to evaluate -> no level, no badge.
    QVERIFY(SiteShieldButton::trustLevelFor(QString(), true).isEmpty());

    // Icon treatment: only "amber" and "red" repaint the shield.
    const QIcon shield = AroraIcon::get(QLatin1String("security-high"));
    const QSize px(20, 20);
    const QImage plain = shield.pixmap(px).toImage();
    QCOMPARE(SiteShieldButton::tintedBadge(
                 shield, px, QLatin1String("green"))
                 .pixmap(px).toImage(), plain);
    QCOMPARE(SiteShieldButton::tintedBadge(shield, px, QString())
                 .pixmap(px).toImage(), plain);
    const QImage ambered = SiteShieldButton::tintedBadge(
        shield, px, QLatin1String("amber")).pixmap(px).toImage();
    const QImage flagged = SiteShieldButton::tintedBadge(
        shield, px, QLatin1String("red")).pixmap(px).toImage();
    QVERIFY(ambered != plain);
    QVERIFY(flagged != plain);
    QVERIFY(flagged != ambered);

    SiteShieldButton::clearTrustCache();
    rc_sitetrust_reload();
#endif
}

// TRUST09: verdicts are cached per host+https — a repeat visit to a
// seen combination never reaches the FFI again, and a scheme flip on
// the same host does (the https bit is part of the key).
void tst_SitePanel::trustBadgeCache()
{
#if defined(ARORA_RUSTCORE)
    SiteShieldButton::clearTrustCache();
    const int before = SiteShieldButton::trustLookupCount();

    SiteShieldButton::trustLevelFor(
        QLatin1String("cache-fixture-a.test"), true);
    QCOMPARE(SiteShieldButton::trustLookupCount(), before + 1);

    // The repeat visit is a cache hit.
    SiteShieldButton::trustLevelFor(
        QLatin1String("cache-fixture-a.test"), true);
    QCOMPARE(SiteShieldButton::trustLookupCount(), before + 1);

    // The same host on the other scheme is a different verdict —
    // the key carries the https bit.
    SiteShieldButton::trustLevelFor(
        QLatin1String("cache-fixture-a.test"), false);
    QCOMPARE(SiteShieldButton::trustLookupCount(), before + 2);

    // A different host misses too.
    SiteShieldButton::trustLevelFor(
        QLatin1String("cache-fixture-b.test"), true);
    QCOMPARE(SiteShieldButton::trustLookupCount(), before + 3);

    // An empty host never reaches the FFI.
    SiteShieldButton::trustLevelFor(QString(), true);
    QCOMPARE(SiteShieldButton::trustLookupCount(), before + 3);

    // A cleared cache re-evaluates.
    SiteShieldButton::clearTrustCache();
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("cache-fixture-a.test"), true),
             QLatin1String("amber"));
    QCOMPARE(SiteShieldButton::trustLookupCount(), before + 4);
#endif
}

// TRUST09: the badge follows the live page — a plaintext loopback
// load ambers the shield, a non-web scheme drops the badge entirely.
void tst_SitePanel::trustBadgeNavigation()
{
#if defined(ARORA_RUSTCORE)
    SiteShieldButton::clearTrustCache();
#endif

    LocalHttpServer server;
    QVERIFY(server.start());

    WebView view;
    SiteShieldButton shield;
    shield.setWebView(&view);

    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_COMPARE(loadSpy.count(), 1);
    QVERIFY(!shield.icon().isNull());
#if defined(ARORA_RUSTCORE)
    // 127.0.0.1 is unranked and the page is plaintext http — amber.
    QCOMPARE(shield.trustBadge(), QLatin1String("amber"));
#else
    QVERIFY(shield.trustBadge().isEmpty());
#endif

    // Leaving the web scheme drops the badge — the icon is the plain
    // shield again.
    view.load(QUrl(QLatin1String("about:blank")));
    QTRY_VERIFY(shield.trustBadge().isEmpty());
}

// TRUST10: the Trust section header — a divider rule that shows and
// hides with the section and a bold "Trust" label whose text color
// follows the verdict (amber on an unranked page, red on a threat
// hit, re-derived on every refresh).  The level->color mapping seam
// splits tone by theme so it reads on day and night palettes alike.
void tst_SitePanel::trustHeaderStyling()
{
    // The mapping seam: red rides the palette's alert role (same one
    // the alert signal rows use), green and amber pick a darker tone
    // on light themes and a brighter one on dark, and anything else —
    // including an un-evaluable verdict — keeps normal text color.
    const QPalette light;
    const QPalette dark = BrowserTheme::darkPalette();
    QVERIFY(!BrowserTheme::isDarkPalette(light));
    QVERIFY(BrowserTheme::isDarkPalette(dark));

    QCOMPARE(SitePanel::trustHeaderColor(QLatin1String("red"), light),
             light.color(QPalette::BrightText));
    QCOMPARE(SitePanel::trustHeaderColor(QLatin1String("red"), dark),
             dark.color(QPalette::BrightText));
    QCOMPARE(SitePanel::trustHeaderColor(QString(), light),
             light.color(QPalette::WindowText));
    QCOMPARE(SitePanel::trustHeaderColor(QLatin1String("bogus"), light),
             light.color(QPalette::WindowText));

    const QColor greenLight = SitePanel::trustHeaderColor(
        QLatin1String("green"), light);
    const QColor greenDark = SitePanel::trustHeaderColor(
        QLatin1String("green"), dark);
    QVERIFY(greenLight != greenDark);
    QVERIFY(greenLight.lightness() < greenDark.lightness());
    const QColor amberLight = SitePanel::trustHeaderColor(
        QLatin1String("amber"), light);
    const QColor amberDark = SitePanel::trustHeaderColor(
        QLatin1String("amber"), dark);
    QVERIFY(amberLight != amberDark);
    QVERIFY(amberLight.lightness() < amberDark.lightness());

    // No page: the section hides — and the divider rule with it.
    // The header itself is always present and bold.
    {
        SitePanel panel;
        panel.refresh();
        QFrame *separator = panel.findChild<QFrame*>(
            QLatin1String("siteTrustSeparator"));
        QVERIFY(separator);
        QVERIFY(separator->isHidden());
        QLabel *header = panel.findChild<QLabel*>(
            QLatin1String("siteTrustHeader"));
        QVERIFY(header);
        QCOMPARE(header->text(), QLatin1String("Trust"));
        QVERIFY(header->font().bold());
    }

#if defined(ARORA_RUSTCORE)
    QSettings settings;
    settings.remove(QLatin1String("trust/enabled"));
    settings.remove(QLatin1String("trust/lookalike"));
    settings.remove(QLatin1String("trust/threatlist"));
    SiteShieldButton::clearTrustCache();

    LocalHttpServer server;
    QVERIFY(server.start());

    WebView view;
    SitePanel panel;
    panel.setWebView(&view);
    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_COMPARE(loadSpy.count(), 1);
    panel.refresh();

    QWidget *trust = panel.findChild<QWidget*>(
        QLatin1String("siteTrust"));
    QFrame *separator = panel.findChild<QFrame*>(
        QLatin1String("siteTrustSeparator"));
    QLabel *header = panel.findChild<QLabel*>(
        QLatin1String("siteTrustHeader"));
    QVERIFY(trust);
    QVERIFY(separator);
    QVERIFY(header);
    QVERIFY(!trust->isHidden());
    QVERIFY(!separator->isHidden());
    // The rule sits directly above the section it heads.
    QCOMPARE(panel.layout()->indexOf(separator) + 1,
             panel.layout()->indexOf(trust));

    // 127.0.0.1 is unranked and the page is plaintext http -> amber.
    QCOMPARE(header->palette().color(QPalette::WindowText),
             SitePanel::trustHeaderColor(QLatin1String("amber"),
                                         panel.palette()));

    // Threat-listing the host flips the verdict to red — the header
    // recolors on the next refresh while the panel is already built.
    // No event-loop pump between the fixture load and refresh, so an
    // in-flight list update cannot swap the table mid-assert.
    const QByteArray block = "127.0.0.1\n";
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(block.constData()),
                 size_t(block.size())),
             RC_OK);
    panel.refresh();
    QCOMPARE(header->palette().color(QPalette::WindowText),
             SitePanel::trustHeaderColor(QLatin1String("red"),
                                         panel.palette()));

    // Restore the vendored seed ∪ override tables for later tests.
    rc_sitetrust_reload();
    SiteShieldButton::clearTrustCache();
#endif
}

// TRUST03: the trust/* switches gate the lookup itself.  The master
// switch off means no signal lines, no badge level and the FFI is
// never reached; each per-signal switch drops only its own input;
// and a Preferences flip re-evaluates live (the probe mask is part
// of the badge cache key, so the next call is a miss).
void tst_SitePanel::trustGating()
{
#if defined(ARORA_RUSTCORE)
    QSettings settings;
    settings.remove(QLatin1String("trust/enabled"));
    settings.remove(QLatin1String("trust/lookalike"));
    settings.remove(QLatin1String("trust/threatlist"));

    // Master off: no lines, no level — and the counter proves the
    // FFI never ran.  No fixture tables are needed yet: a gated
    // lookup never reaches the crate.
    settings.setValue(QLatin1String("trust/enabled"), false);
    const int lookupsBefore = SiteShieldButton::trustLookupCount();
    QVERIFY(SitePanel::trustSignalLines(
                QLatin1String("threat-fixture.test"), true).isEmpty());
    QVERIFY(SiteShieldButton::trustLevelFor(
                QLatin1String("threat-fixture.test"), true).isEmpty());
    QCOMPARE(SiteShieldButton::trustLookupCount(), lookupsBefore);

    // The live panel hides the section even on a page whose verdict
    // would have something to say.
    {
        LocalHttpServer server;
        QVERIFY(server.start());
        WebView view;
        SitePanel panel;
        panel.setWebView(&view);
        QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
        view.load(server.url());
        QTRY_COMPARE(loadSpy.count(), 1);
        QWidget *trust = panel.findChild<QWidget*>(
            QLatin1String("siteTrust"));
        QVERIFY(trust);
        QVERIFY(trust->isHidden());
    }
    settings.remove(QLatin1String("trust/enabled"));

    // Fixture tables go in only now — after the last event-loop pump
    // above — so an in-flight list refresh completing mid-test
    // (DomainBlocklist::maybeCommit re-merges the tables) cannot
    // swap them out under the asserts.  Everything below runs to
    // completion without returning to the event loop.
    const QByteArray tranco = "4,known-fixture.test\n1,paypal.com\n";
    QCOMPARE(rc_tranco_load(
                 reinterpret_cast<const uint8_t *>(tranco.constData()),
                 size_t(tranco.size())),
             RC_OK);
    const QByteArray block = "threat-fixture.test\n";
    QCOMPARE(rc_blocklist_load(
                 reinterpret_cast<const uint8_t *>(block.constData()),
                 size_t(block.size())),
             RC_OK);
    SiteShieldButton::clearTrustCache();

    // Lookalike off: the confusable twin loses only its signal — no
    // 'resembles' line, and the level drops to what the remaining
    // inputs say (unranked -> amber, not red).
    settings.setValue(QLatin1String("trust/lookalike"), false);
    const QString lookalikeLines = SitePanel::trustSignalLines(
        QLatin1String("paypa1.com"), true).join(QLatin1Char('\n'));
    QVERIFY(!lookalikeLines.contains(QLatin1String("resembles"),
                                     Qt::CaseInsensitive));
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("paypa1.com"), true),
             QLatin1String("amber"));
    settings.remove(QLatin1String("trust/lookalike"));

    // Threatlist off: the listed host stops flagging in both the
    // lines and the badge level.
    settings.setValue(QLatin1String("trust/threatlist"), false);
    const QString threatLines = SitePanel::trustSignalLines(
        QLatin1String("threat-fixture.test"), true)
        .join(QLatin1Char('\n'));
    QVERIFY(!threatLines.contains(QLatin1String("threat"),
                                  Qt::CaseInsensitive));
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("threat-fixture.test"), true),
             QLatin1String("amber"));
    settings.remove(QLatin1String("trust/threatlist"));

    // Re-enabling re-evaluates live — the flipped probe mask makes a
    // different cache key, so this is a fresh FFI call returning the
    // full verdict.
    const int beforeReenable = SiteShieldButton::trustLookupCount();
    QCOMPARE(SiteShieldButton::trustLevelFor(
                 QLatin1String("threat-fixture.test"), true),
             QLatin1String("red"));
    QCOMPARE(SiteShieldButton::trustLookupCount(), beforeReenable + 1);
    const QString full = SitePanel::trustSignalLines(
        QLatin1String("threat-fixture.test"), true)
        .join(QLatin1Char('\n'));
    QVERIFY(full.contains(QLatin1String("threat"), Qt::CaseInsensitive));

    SiteShieldButton::clearTrustCache();
    rc_sitetrust_reload();
#endif
}

QTEST_MAIN(tst_SitePanel)
#include "tst_sitepanel.moc"
