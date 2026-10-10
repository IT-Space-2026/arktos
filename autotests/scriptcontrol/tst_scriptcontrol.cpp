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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// JSCTL: per-site JavaScript control — ScriptControlManager rule
// store (normalization, session overlay, persistence, precedence),
// the tier/rule/global-pref decision composition, the interceptor's
// grant-bypass, and the end-to-end blocked/allowed behavior of a real
// page served by an in-process HTTP server, including the info bar.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qlabel.h>
#include <qpushbutton.h>
#include <qsettings.h>
#include <qsignalspy.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineurlrequestinfo.h>

#include "privacyrequestinterceptor.h"
#include "scriptblockinfobar.h"
#include "scriptcontrolmanager.h"
#include "webpage.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

// Minimal HTTP/1.0 responder — same pattern as tst_sitepanel.  The
// page's inline script flips the title so a test can tell whether
// JavaScript actually ran.
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
                const QByteArray body(
                    "<html><head><title>STATIC</title>"
                    "<script>document.title='JS-RAN'</script>"
                    "</head><body></body></html>");
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

class tst_ScriptControl : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void init();
    void cleanup();

private slots:
    void rules();
    void persistence();
    void decision_data();
    void decision();
    void grantBeatsInterceptor();
    void pageRunsScriptsByDefault();
    void blockRuleStopsScriptsAndShowsBar();
    void allowOnceRestoresScripts();
    void allowRuleBeatsSafestTier();
};

// Sets the SECLVL tier the way the settings dialog does, then refreshes
// the interceptor's IO-thread snapshot the way applySettings() does.
static void setSecurityTier(PrivacyRequestInterceptor::SecurityLevel level)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("securityLevel"), int(level));
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
}

static void setGlobalJavaScript(bool enabled)
{
    QSettings().setValue(QLatin1String("websettings/enableJavascript"),
                         enabled);
}

void tst_ScriptControl::initTestCase()
{
    QCoreApplication::setApplicationName("tst_scriptcontrol");
    QVERIFY(ScriptControlManager::instance());
}

void tst_ScriptControl::init()
{
    // Rules and the tier persist in QSettings — reset everything so a
    // previous (possibly interrupted) run cannot leak in.
    ScriptControlManager *scripts = ScriptControlManager::instance();
    scripts->clearPersistentRules();
    scripts->clearSessionRules();
    setSecurityTier(PrivacyRequestInterceptor::Standard);
    setGlobalJavaScript(true);
}

void tst_ScriptControl::cleanup()
{
    ScriptControlManager *scripts = ScriptControlManager::instance();
    scripts->clearPersistentRules();
    scripts->clearSessionRules();
    setSecurityTier(PrivacyRequestInterceptor::Standard);
    QSettings().remove(QLatin1String("websettings/enableJavascript"));
}

void tst_ScriptControl::rules()
{
    ScriptControlManager *scripts = ScriptControlManager::instance();

    QCOMPARE(scripts->ruleForHost(QLatin1String("example.com")),
             ScriptControlManager::SiteDefault);

    // Normalization: case, trailing dot, leading dot.
    scripts->setRuleForHost(QLatin1String("WWW.Example.COM."),
                          ScriptControlManager::Block);
    QCOMPARE(scripts->ruleForHost(QLatin1String("www.example.com")),
             ScriptControlManager::Block);
    scripts->setRuleForHost(QLatin1String(".www.example.com"),
                          ScriptControlManager::Allow);
    QCOMPARE(scripts->ruleForHost(QLatin1String("www.example.com")),
             ScriptControlManager::Allow);
    QCOMPARE(scripts->allowedHosts(),
             QStringList() << QLatin1String("www.example.com"));
    QVERIFY(scripts->blockedHosts().isEmpty());

    // Parent-domain rules cover subdomains but not siblings.
    scripts->setRuleForHost(QLatin1String("example.com"),
                          ScriptControlManager::Block);
    QCOMPARE(scripts->ruleForHost(QLatin1String("example.com")),
             ScriptControlManager::Block);
    QCOMPARE(scripts->ruleForHost(QLatin1String("a.example.com")),
             ScriptControlManager::Block);
    QCOMPARE(scripts->ruleForHost(QLatin1String("example.net")),
             ScriptControlManager::SiteDefault);

    // The session overlay beats a persistent rule; an exact-domain
    // persistent rule beats a looser parent entry of the other kind.
    scripts->setRuleForHost(QLatin1String("www.example.com"),
                          ScriptControlManager::Allow, false);
    QCOMPARE(scripts->ruleForHost(QLatin1String("www.example.com")),
             ScriptControlManager::Allow);
    scripts->setRuleForHost(QLatin1String("www.example.com"),
                          ScriptControlManager::SiteDefault, false);
    // Session cleared — the exact persistent Allow resurfaces over the
    // parent-domain Block.
    QCOMPARE(scripts->ruleForHost(QLatin1String("www.example.com")),
             ScriptControlManager::Allow);

    // SiteDefault removes the exact persistent entry only.
    scripts->setRuleForHost(QLatin1String("www.example.com"),
                          ScriptControlManager::SiteDefault);
    QCOMPARE(scripts->ruleForHost(QLatin1String("www.example.com")),
             ScriptControlManager::Block);   // parent still applies

    scripts->clearPersistentRules();
    scripts->clearSessionRules();
    QVERIFY(scripts->allowedHosts().isEmpty());
    QVERIFY(scripts->blockedHosts().isEmpty());
}

void tst_ScriptControl::persistence()
{
    ScriptControlManager *scripts = ScriptControlManager::instance();

    // A persistent rule lands in the durable store.
    scripts->setRuleForHost(QLatin1String("persist.example"),
                          ScriptControlManager::Allow, true);
#if defined(ARORA_RUSTCORE)
    {
        QString stored;
        QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindJavaScript,
                                       QLatin1String("persist.example"),
                                       &stored));
        QCOMPARE(stored, QLatin1String("allow"));
    }
#else
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("scriptcontrol"));
        QCOMPARE(settings.value(QLatin1String("allowed")).toStringList(),
                 QStringList() << QLatin1String("persist.example"));
    }
#endif

    // A session rule writes nothing — new settings objects and a fresh
    // manager see only the persistent entry.
    scripts->setRuleForHost(QLatin1String("session.example"),
                          ScriptControlManager::Block, false);
#if defined(ARORA_RUSTCORE)
    {
        QString stored;
        QVERIFY(SiteDecisionStore::get(SiteDecisionStore::KindJavaScript,
                                       QLatin1String("persist.example"),
                                       &stored));
        QCOMPARE(stored, QLatin1String("allow"));
        QVERIFY(!SiteDecisionStore::get(SiteDecisionStore::KindJavaScript,
                                        QLatin1String("session.example"),
                                        &stored));
    }
#else
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("scriptcontrol"));
        QCOMPARE(settings.value(QLatin1String("allowed")).toStringList(),
                 QStringList() << QLatin1String("persist.example"));
        QVERIFY(!settings.contains(QLatin1String("blocked")));
    }
#endif

    // A second manager instance reloads the persistent lists only.
    ScriptControlManager reloaded;
    QCOMPARE(reloaded.ruleForHost(QLatin1String("persist.example")),
             ScriptControlManager::Allow);
    QCOMPARE(reloaded.ruleForHost(QLatin1String("session.example")),
             ScriptControlManager::SiteDefault);
}

void tst_ScriptControl::decision_data()
{
    QTest::addColumn<int>("tier");
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("ruleHost");
    QTest::addColumn<int>("rule");
    QTest::addColumn<bool>("globalJs");
    QTest::addColumn<bool>("expected");

    typedef PrivacyRequestInterceptor P;
    typedef ScriptControlManager S;
    const int standard = int(P::Standard);
    const int safer = int(P::Safer);
    const int safest = int(P::Safest);
    const int none = int(S::SiteDefault);
    const int allow = int(S::Allow);
    const int block = int(S::Block);

    // Standard: scripts on unless explicitly blocked or globally off.
    QTest::newRow("standard http default")
        << standard << "http://example.com/" << QString() << none
        << true << true;
    QTest::newRow("standard https default")
        << standard << "https://example.com/" << QString() << none
        << true << true;
    QTest::newRow("standard + block")
        << standard << "https://example.com/" << "example.com" << block
        << true << false;
    QTest::newRow("standard + global off")
        << standard << "https://example.com/" << QString() << none
        << false << false;
    QTest::newRow("standard + global off + allow")
        << standard << "https://example.com/" << "example.com" << allow
        << false << true;

    // Safer: insecure http pages lose scripts; https and loopback keep
    // them; an explicit Allow beats the tier.
    QTest::newRow("safer http")
        << safer << "http://example.com/" << QString() << none
        << true << false;
    QTest::newRow("safer http subdomain")
        << safer << "http://www.example.com/" << QString() << none
        << true << false;
    QTest::newRow("safer https")
        << safer << "https://example.com/" << QString() << none
        << true << true;
    QTest::newRow("safer http loopback")
        << safer << "http://127.0.0.1:8080/" << QString() << none
        << true << true;
    QTest::newRow("safer http + allow")
        << safer << "http://example.com/" << "example.com" << allow
        << true << true;

    // Safest: scripts off everywhere except internal schemes; Allow
    // still wins, Block on an internal host still loses to nothing
    // (internal schemes short-circuit only *without* an explicit rule).
    QTest::newRow("safest https")
        << safest << "https://example.com/" << QString() << none
        << true << false;
    QTest::newRow("safest https + allow")
        << safest << "https://example.com/" << "example.com" << allow
        << true << true;
    QTest::newRow("safest internal qrc")
        << safest << "qrc:///start.html" << QString() << none
        << true << true;
    QTest::newRow("safest internal about")
        << safest << "about:blank" << QString() << none
        << true << true;
}

void tst_ScriptControl::decision()
{
    QFETCH(int, tier);
    QFETCH(QString, url);
    QFETCH(QString, ruleHost);
    QFETCH(int, rule);
    QFETCH(bool, globalJs);
    QFETCH(bool, expected);

    setSecurityTier(PrivacyRequestInterceptor::SecurityLevel(tier));
    setGlobalJavaScript(globalJs);

    ScriptControlManager *scripts = ScriptControlManager::instance();
    if (!ruleHost.isEmpty())
        scripts->setRuleForHost(ruleHost, ScriptControlManager::Rule(rule));

    QCOMPARE(scripts->isJavaScriptEnabledFor(QUrl(url)), expected);
}

void tst_ScriptControl::grantBeatsInterceptor()
{
    // Under Safer the interceptor drops script fetches on insecure http
    // pages — an Allow grant must lift that too or the page would get
    // JavascriptEnabled but still load nothing.
    setSecurityTier(PrivacyRequestInterceptor::Safer);
    const QUrl httpPage(QLatin1String("http://example.com/"));
    QVERIFY(PrivacyRequestInterceptor::shouldBlockScript(
            httpPage, QWebEngineUrlRequestInfo::ResourceTypeScript));

    ScriptControlManager *scripts = ScriptControlManager::instance();
    scripts->setRuleForHost(QLatin1String("example.com"),
                          ScriptControlManager::Allow, false);
    QVERIFY(!PrivacyRequestInterceptor::shouldBlockScript(
            httpPage, QWebEngineUrlRequestInfo::ResourceTypeScript));

    // ...but the exemption is script-fetch specific — images on the
    // same page are not affected by the grant either way.
    QVERIFY(!PrivacyRequestInterceptor::shouldBlockScript(
            httpPage, QWebEngineUrlRequestInfo::ResourceTypeImage));

    // A blocked host is not exempted.
    scripts->setRuleForHost(QLatin1String("example.com"),
                          ScriptControlManager::Block, false);
    QVERIFY(PrivacyRequestInterceptor::shouldBlockScript(
            httpPage, QWebEngineUrlRequestInfo::ResourceTypeScript));
}

void tst_ScriptControl::pageRunsScriptsByDefault()
{
    LocalHttpServer server;
    QVERIFY(server.start());

    WebView view;
    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_COMPARE(loadSpy.count(), 1);
    QTRY_COMPARE(view.title(), QStringLiteral("JS-RAN"));
    QVERIFY(!view.isJavaScriptBlocked());

    ScriptBlockInfoBar *bar =
        view.findChild<ScriptBlockInfoBar*>(
            QLatin1String("scriptBlockInfoBar"));
    QVERIFY(bar);
    QVERIFY(bar->isHidden());
}

void tst_ScriptControl::blockRuleStopsScriptsAndShowsBar()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QString host = server.url().host();

    // Loopback is exempt from the Safer auto-block, so an explicit
    // Block rule is the way to get a blocked loopback page.
    ScriptControlManager *scripts = ScriptControlManager::instance();
    scripts->setRuleForHost(host, ScriptControlManager::Block, false);

    WebView view;
    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    QSignalSpy blockSpy(&view, SIGNAL(javaScriptBlockedChanged(bool)));
    view.load(server.url());
    QTRY_COMPARE(loadSpy.count(), 1);

    // The policy was applied before commit: the inline script never
    // ran and the view reports the blocked state + host.
    QCOMPARE(view.title(), QStringLiteral("STATIC"));
    QVERIFY(view.isJavaScriptBlocked());
    QTRY_COMPARE(blockSpy.count(), 1);
    QCOMPARE(blockSpy.first().first().toBool(), true);

    ScriptBlockInfoBar *bar =
        view.findChild<ScriptBlockInfoBar*>(
            QLatin1String("scriptBlockInfoBar"));
    QVERIFY(bar);
    QVERIFY(!bar->isHidden());
    QLabel *label = bar->findChild<QLabel*>();
    QVERIFY(label);
    QCOMPARE(label->text(),
             QStringLiteral("Scripts blocked on %1").arg(host));
    QCOMPARE(label->textFormat(), Qt::PlainText);
}

void tst_ScriptControl::allowOnceRestoresScripts()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QString host = server.url().host();

    ScriptControlManager *scripts = ScriptControlManager::instance();
    scripts->setRuleForHost(host, ScriptControlManager::Block, true);

    WebView view;
    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_COMPARE(loadSpy.count(), 1);
    QCOMPARE(view.title(), QStringLiteral("STATIC"));

    ScriptBlockInfoBar *bar =
        view.findChild<ScriptBlockInfoBar*>(
            QLatin1String("scriptBlockInfoBar"));
    QVERIFY(bar);
    QVERIFY(!bar->isHidden());
    QPushButton *allowOnce = bar->findChild<QPushButton*>(
        QLatin1String("scriptAllowOnce"));
    QVERIFY(allowOnce);

    // "Allow once" records a session rule and reloads — the page's
    // script now runs.
    allowOnce->click();
    QTRY_COMPARE(loadSpy.count(), 2);
    QTRY_COMPARE(view.title(), QStringLiteral("JS-RAN"));
    QVERIFY(!view.isJavaScriptBlocked());
    QTRY_VERIFY(bar->isHidden());

    // Session-scoped: the persistent allow list stays empty and the
    // stored Block rule is untouched.
    QVERIFY(scripts->allowedHosts().isEmpty());
    QCOMPARE(scripts->blockedHosts(), QStringList() << host);
    scripts->clearSessionRules();
    QCOMPARE(scripts->ruleForHost(host), ScriptControlManager::Block);
}

void tst_ScriptControl::allowRuleBeatsSafestTier()
{
    LocalHttpServer server;
    QVERIFY(server.start());
    const QString host = server.url().host();

    setSecurityTier(PrivacyRequestInterceptor::Safest);

    WebView view;
    QSignalSpy loadSpy(&view, SIGNAL(loadFinished(bool)));
    view.load(server.url());
    QTRY_COMPARE(loadSpy.count(), 1);
    QCOMPARE(view.title(), QStringLiteral("STATIC"));
    QVERIFY(view.isJavaScriptBlocked());

    // An explicit Allow grant re-enables scripts even at Safest —
    // verified by clicking the bar's "Always allow" (the test view's
    // profile is off-the-record, so the grant is session-scoped).
    ScriptBlockInfoBar *bar =
        view.findChild<ScriptBlockInfoBar*>(
            QLatin1String("scriptBlockInfoBar"));
    QVERIFY(bar);
    QPushButton *allowAlways = bar->findChild<QPushButton*>(
        QLatin1String("scriptAllowAlways"));
    QVERIFY(allowAlways);
    allowAlways->click();
    QTRY_COMPARE(loadSpy.count(), 2);
    QTRY_COMPARE(view.title(), QStringLiteral("JS-RAN"));
    QVERIFY(!view.isJavaScriptBlocked());

    ScriptControlManager *scripts = ScriptControlManager::instance();
    QCOMPARE(scripts->ruleForHost(host), ScriptControlManager::Allow);
    // OTR: nothing was written to the persistent lists.
    QVERIFY(scripts->allowedHosts().isEmpty());
    QVERIFY(scripts->blockedHosts().isEmpty());
}

QTEST_MAIN(tst_ScriptControl)
#include "tst_scriptcontrol.moc"
