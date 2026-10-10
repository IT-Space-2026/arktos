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

// PRIV01: unit coverage for the privacy-hardening seams.
//
// The interceptor's actual redirect()/header-rewrite path needs a
// live QWebEngineUrlRequestInfo, so these tests pin the pure decision
// layer it consults (upgrade candidacy, the session downgrade set)
// plus the pieces that are directly observable:
//   - CookieJar::isAllowedForHost third-party semantics
//   - the arora-site-wipe / arora-exit-wipe sentinel handling in
//     BrowserProfile::clearDeferredSiteStorage()
//   - QTWEBENGINE_CHROMIUM_FLAGS composition in applyChromiumFlags()
//   - the pure referer rewrite decision (rewrittenReferer) and the
//     privacy/refererPolicy selector incl. trimReferer migration
// end-to-end HTTPS upgrade + referer trim is exercised by the
// --privacy-smoke and --referer-smoke modes.

#include <QtTest/QtTest>
#include <qtest_arora.h>

#include <acceptlanguagedialog.h>
#include <adblocknetwork.h>
#include <browserapplication.h>
#include <browserprofile.h>
#include <cookiejar.h>
#include <privacyrequestinterceptor.h>
#include <webpage.h>

#include <qcryptographichash.h>
#include <qdir.h>
#include <qhostaddress.h>
#include <qnetworkcookie.h>
#include <qsettings.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporarydir.h>
#include <qurl.h>
#include <qwebenginecookiestore.h>
#include <qwebengineloadinginfo.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineurlrequestinfo.h>

#if defined(ARORA_RUSTCORE)
#include <rustcore.h>
#endif

#include <memory>

class tst_Privacy : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void upgradeCandidate_data();
    void upgradeCandidate();
    void downgradeFlow();
    void downgradeBoundaries();
    void downgradeErrorScoping();
    void downgradeScopeIsolation();
    void downgradeExpiry();
    void downgradeSelfHeal();
    void settingsRoundTrip();
    void refererPolicyMigration();
    void refererRewriteMatrix();

    void blockScriptStandard();
    void blockScriptSafer_data();
    void blockScriptSafer();
    void securityLevelAttributes();

    void resourceBlockToggles();
    void webSocketBlockDecision();

    void stripTrackingParams_data();
    void stripTrackingParams();
    void stripToggleAndExceptions();

    void thirdPartyCookies();
    void thirdPartyCookieExceptions();
    void thirdPartyCookieEndToEnd();
    void thirdPartyWebSocketEndToEnd();

    void deferredWipe();
    void deferredExitWipe();

    void forceDarkMode();
    void chromiumFlags();
    void secureDnsTorGate();
    void fingerprintNormalization();

private:
    QByteArray m_savedFlags;
    QVariant m_savedHttpsFirst;
    QVariant m_savedTrimReferer;
    QVariant m_savedRefererPolicy;
    QVariant m_savedWebrtc;
    QVariant m_savedSecureDns;
    QVariant m_savedSecureDnsMode;
    QVariant m_savedSecureDnsServer;
    QVariant m_savedBlock3p;
    QVariant m_savedSecurityLevel;
    QVariant m_savedEnableJavascript;
    QVariant m_savedUtcTimezone;
    QVariant m_savedNormalizeLang;
    QVariant m_savedBlockRemoteFonts;
    QVariant m_savedBlockPrefetch;
    QVariant m_savedBlockThirdPartyWs;
    QVariant m_savedStripTrackingParams;
    QVariant m_savedAcceptLanguages;
    QVariant m_savedForceDarkMode;
    QVariant m_savedAutoscroll;
    bool m_savedTzSet;
    QByteArray m_savedTz;
};

class SubCookieJar : public CookieJar
{
public:
    SubCookieJar() : CookieJar() {}
    bool isAllowedForHost(const QString &host, bool thirdParty) const
        { return CookieJar::isAllowedForHost(host, thirdParty); }
};

void tst_Privacy::initTestCase()
{
    m_savedFlags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    // The tests run against the real settings store — remember every
    // key touched so cleanupTestCase() leaves the profile unchanged.
    QSettings settings;
    m_savedHttpsFirst = settings.value(QLatin1String("privacy/httpsFirst"));
    m_savedTrimReferer = settings.value(QLatin1String("privacy/trimReferer"));
    m_savedRefererPolicy =
        settings.value(QLatin1String("privacy/refererPolicy"));
    m_savedWebrtc = settings.value(QLatin1String("privacy/webrtcIpProtection"));
    m_savedSecureDns = settings.value(QLatin1String("privacy/secureDns"));
    m_savedSecureDnsMode =
        settings.value(QLatin1String("privacy/secureDnsMode"));
    m_savedSecureDnsServer =
        settings.value(QLatin1String("privacy/secureDnsServer"));
    m_savedBlock3p = settings.value(QLatin1String("cookies/blockThirdPartyCookies"));
    m_savedSecurityLevel = settings.value(QLatin1String("privacy/securityLevel"));
    m_savedEnableJavascript = settings.value(QLatin1String("websettings/enableJavascript"));
    m_savedUtcTimezone = settings.value(QLatin1String("privacy/reportUtcTimezone"));
    m_savedNormalizeLang = settings.value(QLatin1String("privacy/normalizeAcceptLanguage"));
    m_savedAcceptLanguages = settings.value(QLatin1String("network/acceptLanguages"));
    m_savedForceDarkMode =
        settings.value(QLatin1String("websettings/forceDarkMode"));
    m_savedAutoscroll =
        settings.value(QLatin1String("websettings/middleClickAutoscroll"));
    m_savedBlockRemoteFonts =
        settings.value(QLatin1String("privacy/blockRemoteFonts"));
    m_savedBlockPrefetch =
        settings.value(QLatin1String("privacy/blockPrefetch"));
    m_savedBlockThirdPartyWs =
        settings.value(QLatin1String("privacy/blockThirdPartyWebSockets"));
    m_savedStripTrackingParams =
        settings.value(QLatin1String("privacy/stripTrackingParams"));
    m_savedTzSet = qEnvironmentVariableIsSet("TZ");
    m_savedTz = qgetenv("TZ");
}

static void restoreSetting(QSettings &settings, const QString &key,
                         const QVariant &saved)
{
    if (saved.isValid())
        settings.setValue(key, saved);
    else
        settings.remove(key);
}

void tst_Privacy::cleanupTestCase()
{
    if (m_savedFlags.isNull())
        qunsetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    else
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", m_savedFlags);

    QSettings settings;
    restoreSetting(settings, QLatin1String("privacy/httpsFirst"), m_savedHttpsFirst);
    restoreSetting(settings, QLatin1String("privacy/trimReferer"), m_savedTrimReferer);
    restoreSetting(settings, QLatin1String("privacy/refererPolicy"), m_savedRefererPolicy);
    restoreSetting(settings, QLatin1String("privacy/webrtcIpProtection"), m_savedWebrtc);
    restoreSetting(settings, QLatin1String("privacy/secureDns"), m_savedSecureDns);
    restoreSetting(settings, QLatin1String("privacy/secureDnsMode"), m_savedSecureDnsMode);
    restoreSetting(settings, QLatin1String("privacy/secureDnsServer"), m_savedSecureDnsServer);
    restoreSetting(settings, QLatin1String("cookies/blockThirdPartyCookies"), m_savedBlock3p);
    restoreSetting(settings, QLatin1String("privacy/securityLevel"), m_savedSecurityLevel);
    restoreSetting(settings, QLatin1String("websettings/enableJavascript"), m_savedEnableJavascript);
    restoreSetting(settings, QLatin1String("privacy/reportUtcTimezone"), m_savedUtcTimezone);
    restoreSetting(settings, QLatin1String("privacy/normalizeAcceptLanguage"), m_savedNormalizeLang);
    restoreSetting(settings, QLatin1String("network/acceptLanguages"), m_savedAcceptLanguages);
    restoreSetting(settings, QLatin1String("websettings/forceDarkMode"), m_savedForceDarkMode);
    restoreSetting(settings, QLatin1String("websettings/middleClickAutoscroll"), m_savedAutoscroll);
    restoreSetting(settings, QLatin1String("privacy/blockRemoteFonts"), m_savedBlockRemoteFonts);
    restoreSetting(settings, QLatin1String("privacy/blockPrefetch"), m_savedBlockPrefetch);
    restoreSetting(settings, QLatin1String("privacy/blockThirdPartyWebSockets"), m_savedBlockThirdPartyWs);
    restoreSetting(settings, QLatin1String("privacy/stripTrackingParams"), m_savedStripTrackingParams);
    PrivacyRequestInterceptor::loadSettings();
    if (m_savedTzSet)
        qputenv("TZ", m_savedTz);
    else
        qunsetenv("TZ");
}

void tst_Privacy::init()
{
    // Pin the interceptor policy so the tests don't depend on the
    // developer machine's real Arora settings.
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("httpsFirst"), true);
    settings.setValue(QLatin1String("trimReferer"), true);
    settings.setValue(QLatin1String("refererPolicy"),
                      int(PrivacyRequestInterceptor::RefererTrimmed));
    settings.setValue(QLatin1String("securityLevel"),
                      int(PrivacyRequestInterceptor::Standard));
    settings.setValue(QLatin1String("blockRemoteFonts"), false);
    settings.setValue(QLatin1String("blockPrefetch"), true);
    settings.setValue(QLatin1String("blockThirdPartyWebSockets"), false);
    settings.setValue(QLatin1String("stripTrackingParams"), true);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    PrivacyRequestInterceptor::clearDowngradedHosts();
}

static void setSecurityLevel(PrivacyRequestInterceptor::SecurityLevel level)
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("securityLevel"), int(level));
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
}

void tst_Privacy::cleanup()
{
    PrivacyRequestInterceptor::clearDowngradedHosts();
    // A test that shrank the TTL must not leak it into the next one.
    PrivacyRequestInterceptor::setDowngradeTtlMs(30 * 60 * 1000);
}

// SAFE07: downgrade marks are keyed by profile scope — the pure
// decision tests share this one.
static const QString testScope = QStringLiteral("test");

void tst_Privacy::upgradeCandidate_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<bool>("candidate");

    QTest::newRow("http public host")
        << QString("http://example.com/") << true;
    QTest::newRow("http public host, port")
        << QString("http://example.com:8080/") << true;
    QTest::newRow("http public IP literal")
        << QString("http://8.8.8.8/") << true;
    QTest::newRow("https untouched")
        << QString("https://example.com/") << false;
    QTest::newRow("data url untouched")
        << QString("data:text/html,hi") << false;
    QTest::newRow("file url untouched")
        << QString("file:///tmp/x.html") << false;
    QTest::newRow("localhost")
        << QString("http://localhost/") << false;
    QTest::newRow("*.localhost")
        << QString("http://app.localhost/") << false;
    QTest::newRow("*.local (mDNS)")
        << QString("http://printer.local/") << false;
    QTest::newRow("*.onion")
        << QString("http://abc.onion/") << false;
    QTest::newRow("loopback IPv4")
        << QString("http://127.0.0.1:8000/") << false;
    QTest::newRow("loopback IPv6")
        << QString("http://[::1]/") << false;
    QTest::newRow("RFC1918 10/8")
        << QString("http://10.1.2.3/") << false;
    QTest::newRow("RFC1918 172.16/12")
        << QString("http://172.16.5.4/") << false;
    QTest::newRow("RFC1918 192.168/16")
        << QString("http://192.168.1.1/") << false;
    QTest::newRow("link-local")
        << QString("http://169.254.1.1/") << false;
    QTest::newRow("ULA IPv6")
        << QString("http://[fd00::1]/") << false;
}

void tst_Privacy::upgradeCandidate()
{
    QVERIFY(PrivacyRequestInterceptor::httpsFirstEnabled());
    QFETCH(QString, url);
    QFETCH(bool, candidate);
    QCOMPARE(PrivacyRequestInterceptor::isUpgradeCandidate(QUrl(url),
                                                         testScope),
             candidate);
}

void tst_Privacy::downgradeFlow()
{
    const int conn = int(QWebEngineLoadingInfo::ConnectionErrorDomain);
    const QUrl http("http://neverssl.test/");
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(http, testScope));

    // Only https failures record a downgrade; an http "failure" is
    // not an upgrade candidate's problem.
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                http, conn, -102, testScope));

    // A failed https load drops the host from candidacy while the
    // mark is live.
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://neverssl.test/"), conn, -102, testScope));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(
                QLatin1String("neverssl.test"), testScope));
    QVERIFY(!PrivacyRequestInterceptor::isUpgradeCandidate(http, testScope));

    // Idempotent — the second failure does not "newly" downgrade.
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://neverssl.test/other"), conn, -102,
                testScope));

    // A different host is unaffected.
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(
                QUrl("http://other.test/"), testScope));
}

void tst_Privacy::downgradeBoundaries()
{
    const int conn = int(QWebEngineLoadingInfo::ConnectionErrorDomain);
    // Private/local hosts are never candidates, so they never join
    // the downgrade set either (isPrivateOrLocalHost gate).
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://192.168.1.1/"), conn, -102, testScope));
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://abc.onion/"), conn, -102, testScope));
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://localhost/"), conn, -102, testScope));
    QVERIFY(!PrivacyRequestInterceptor::isDowngraded(
                QLatin1String("localhost"), testScope));

    // Case-insensitive host matching both ways.
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://MiXeD.test/"), conn, -102, testScope));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(
                QLatin1String("mixed.test"), testScope));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(
                QLatin1String("MIXED.TEST"), testScope));
}

// SAFE07: only genuine connection/TLS failures may downgrade a host —
// vetoes, aborts, interrupted redirects, http status lines, resolver
// faults and proxy plumbing errors are not evidence the site lacks
// TLS, and certificate problems take their own interstitial path.
void tst_Privacy::downgradeErrorScoping()
{
    const int conn = int(QWebEngineLoadingInfo::ConnectionErrorDomain);
    const QUrl https(QStringLiteral("https://errorscope.test/"));
    const QString host(QStringLiteral("errorscope.test"));

    const auto noMark = [conn, &https](int domain, int code) {
        return !PrivacyRequestInterceptor::noteNavigationFailure(
            https, domain, code, testScope);
    };
    // InternalErrorDomain: user/navigation aborts, interceptor and
    // adblock vetoes, superseded requests.
    QVERIFY(noMark(int(QWebEngineLoadingInfo::InternalErrorDomain), -3));   // ERR_ABORTED
    QVERIFY(noMark(int(QWebEngineLoadingInfo::InternalErrorDomain), -20));  // ERR_BLOCKED_BY_CLIENT
    // An http error status is not a TLS failure.
    QVERIFY(noMark(int(QWebEngineLoadingInfo::HttpStatusCodeDomain), 404));
    QVERIFY(noMark(int(QWebEngineLoadingInfo::HttpStatusCodeDomain), 503));
    // Qt's DnsErrorDomain and certificate-domain failures.
    QVERIFY(noMark(int(QWebEngineLoadingInfo::DnsErrorDomain), -800));
    QVERIFY(noMark(int(QWebEngineLoadingInfo::CertificateErrorDomain), -200));
    QVERIFY(noMark(int(QWebEngineLoadingInfo::NoErrorDomain), 0));
    // In-range codes that still say nothing about the origin's TLS:
    // resolver faults, proxy/tunnel plumbing, local machine state.
    QVERIFY(noMark(conn, -105));  // ERR_NAME_NOT_RESOLVED
    QVERIFY(noMark(conn, -137));  // ERR_NAME_RESOLUTION_FAILED
    QVERIFY(noMark(conn, -106));  // ERR_INTERNET_DISCONNECTED
    QVERIFY(noMark(conn, -111));  // ERR_TUNNEL_CONNECTION_FAILED
    QVERIFY(noMark(conn, -130));  // ERR_PROXY_CONNECTION_FAILED
    QVERIFY(noMark(conn, -120));  // ERR_SOCKS_CONNECTION_FAILED
    QVERIFY(noMark(conn, -186));  // ERR_PROXY_UNABLE_TO_CONNECT_TO_DESTINATION
    QVERIFY(!PrivacyRequestInterceptor::isDowngraded(host, testScope));

    // Genuine reachability/TLS failures do mark.
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                https, conn, -102, testScope));   // ERR_CONNECTION_REFUSED
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(host, testScope));
    PrivacyRequestInterceptor::clearDowngradedHosts();
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                https, conn, -107, testScope));   // ERR_SSL_PROTOCOL_ERROR
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(host, testScope));
}

// SAFE07: marks are per-profile — a private window or container cannot
// teach another profile to bypass the https-first upgrade.
void tst_Privacy::downgradeScopeIsolation()
{
    const int conn = int(QWebEngineLoadingInfo::ConnectionErrorDomain);
    const QString host(QStringLiteral("isolated.test"));
    const QUrl https(QStringLiteral("https://") + host + QLatin1Char('/'));
    const QString scopeA(QStringLiteral("scope-a"));
    const QString scopeB(QStringLiteral("scope-b"));

    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                https, conn, -102, scopeA));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(host, scopeA));
    QVERIFY(!PrivacyRequestInterceptor::isDowngraded(host, scopeB));
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(
                QUrl(QStringLiteral("http://") + host + QLatin1Char('/')),
                scopeB));

    // OTR profiles key by pointer, named ones by storageName — the
    // namespaces must not collide.
    QWebEngineProfile otrA;
    QWebEngineProfile otrB;
    QWebEngineProfile named(QStringLiteral("arora-scopetest"));
    QCOMPARE(PrivacyRequestInterceptor::downgradeScope(&named),
             QStringLiteral("arora-scopetest"));
    QVERIFY(PrivacyRequestInterceptor::downgradeScope(&otrA)
            != PrivacyRequestInterceptor::downgradeScope(&otrB));
    QVERIFY(PrivacyRequestInterceptor::downgradeScope(&otrA)
            != PrivacyRequestInterceptor::downgradeScope(&named));
}

// SAFE07: a mark expires after the TTL — the host gets its silent
// https attempt again instead of staying http-only for the session.
void tst_Privacy::downgradeExpiry()
{
    const int conn = int(QWebEngineLoadingInfo::ConnectionErrorDomain);
    const QString host(QStringLiteral("ttlhost.test"));
    const QUrl https(QStringLiteral("https://") + host + QLatin1Char('/'));
    const QUrl http(QStringLiteral("http://") + host + QLatin1Char('/'));

    PrivacyRequestInterceptor::setDowngradeTtlMs(50);
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                https, conn, -102, testScope));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(host, testScope));
    QTest::qWait(80);
    QVERIFY(!PrivacyRequestInterceptor::isDowngraded(host, testScope));
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(http, testScope));
}

// SAFE07: WebPage clears a mark when an https: main-frame load
// commits — a self-heal for hosts whose TLS recovered.
void tst_Privacy::downgradeSelfHeal()
{
    const int conn = int(QWebEngineLoadingInfo::ConnectionErrorDomain);
    const QString host(QStringLiteral("heal.test"));
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl(QStringLiteral("https://") + host + QLatin1Char('/')),
                conn, -102, testScope));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(host, testScope));
    PrivacyRequestInterceptor::clearDowngradedHost(host, testScope);
    QVERIFY(!PrivacyRequestInterceptor::isDowngraded(host, testScope));
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(
                QUrl(QStringLiteral("http://") + host + QLatin1Char('/')),
                testScope));
}

void tst_Privacy::settingsRoundTrip()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("httpsFirst"), false);
    settings.setValue(QLatin1String("refererPolicy"),
                      int(PrivacyRequestInterceptor::RefererEngineDefault));
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(!PrivacyRequestInterceptor::httpsFirstEnabled());
    QVERIFY(!PrivacyRequestInterceptor::trimRefererEnabled());
    QCOMPARE(PrivacyRequestInterceptor::refererPolicy(),
             int(PrivacyRequestInterceptor::RefererEngineDefault));

    // Restore the test defaults for subsequent cases.
    init();
    QVERIFY(PrivacyRequestInterceptor::httpsFirstEnabled());
    QVERIFY(PrivacyRequestInterceptor::trimRefererEnabled());
    QCOMPARE(PrivacyRequestInterceptor::refererPolicy(),
             int(PrivacyRequestInterceptor::RefererTrimmed));
}

void tst_Privacy::refererPolicyMigration()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));

    // Selector wins over the legacy bool — Strict selected while an
    // old profile still has trimReferer=false means Strict.
    settings.setValue(QLatin1String("trimReferer"), false);
    settings.setValue(QLatin1String("refererPolicy"),
                      int(PrivacyRequestInterceptor::RefererStrict));
    PrivacyRequestInterceptor::loadSettings();
    QCOMPARE(PrivacyRequestInterceptor::refererPolicy(),
             int(PrivacyRequestInterceptor::RefererStrict));
    QVERIFY(PrivacyRequestInterceptor::trimRefererEnabled());

    // Selector absent: the PRIV01 bool migrates — false to
    // EngineDefault, true to Trimmed.
    settings.remove(QLatin1String("refererPolicy"));
    settings.setValue(QLatin1String("trimReferer"), false);
    PrivacyRequestInterceptor::loadSettings();
    QCOMPARE(PrivacyRequestInterceptor::refererPolicy(),
             int(PrivacyRequestInterceptor::RefererEngineDefault));
    settings.setValue(QLatin1String("trimReferer"), true);
    PrivacyRequestInterceptor::loadSettings();
    QCOMPARE(PrivacyRequestInterceptor::refererPolicy(),
             int(PrivacyRequestInterceptor::RefererTrimmed));

    // Out-of-range stored values clamp instead of corrupting policy.
    settings.setValue(QLatin1String("refererPolicy"), 99);
    PrivacyRequestInterceptor::loadSettings();
    QCOMPARE(PrivacyRequestInterceptor::refererPolicy(),
             int(PrivacyRequestInterceptor::RefererNever));
    settings.endGroup();
    init();
}

void tst_Privacy::refererRewriteMatrix()
{
    typedef PrivacyRequestInterceptor P;
    const QUrl srcHttp(QStringLiteral("http://site-a.test/dir/page?q=1"));
    const QUrl srcHttps(QStringLiteral("https://site-a.test/dir/page?q=1"));
    const QUrl sameSite(QStringLiteral("http://site-a.test/other"));
    const QUrl crossSite(QStringLiteral("http://site-b.test/res"));
    const QByteArray full = srcHttp.toString().toUtf8();

    // EngineDefault leaves the renderer's referer untouched.
    QCOMPARE(P::rewrittenReferer(P::RefererEngineDefault, srcHttp,
                                 crossSite), full);

    // Trimmed: the source's path/query never leave — cross-site sees
    // only the *target's* origin, same-site sees only its own origin.
    QCOMPARE(P::rewrittenReferer(P::RefererTrimmed, srcHttp, crossSite),
             QByteArray("http://site-b.test/"));
    QCOMPARE(P::rewrittenReferer(P::RefererTrimmed, srcHttp, sameSite),
             QByteArray("http://site-a.test/"));

    // Strict: cross-site gets nothing, same-site keeps the origin.
    QVERIFY(P::rewrittenReferer(P::RefererStrict, srcHttp, crossSite)
            .isEmpty());
    QCOMPARE(P::rewrittenReferer(P::RefererStrict, srcHttp, sameSite),
             QByteArray("http://site-a.test/"));

    // Never: the header goes away even same-site.
    QVERIFY(P::rewrittenReferer(P::RefererNever, srcHttp, sameSite)
            .isEmpty());
    QVERIFY(P::rewrittenReferer(P::RefererNever, srcHttp, crossSite)
            .isEmpty());

    // https->http downgrade silences the header at every hardened
    // level, even though the referer itself is same-site.
    QVERIFY(P::rewrittenReferer(P::RefererTrimmed, srcHttps,
                                QUrl("http://site-a.test/other"))
            .isEmpty());
    QVERIFY(P::rewrittenReferer(P::RefererStrict, srcHttps,
                                crossSite).isEmpty());
    // ...but http->https is not a downgrade.
    QCOMPARE(P::rewrittenReferer(P::RefererTrimmed, srcHttp,
                                 QUrl("https://site-a.test/other")),
             QByteArray("http://site-a.test/"));

    // Sibling hosts under one site are same-site; a different
    // registrable base is not (same two-label rule as adblock).
    QCOMPARE(P::rewrittenReferer(P::RefererTrimmed, srcHttp,
                                 QUrl("http://cdn.site-a.test/x")),
             QByteArray("http://site-a.test/"));
    QCOMPARE(P::rewrittenReferer(P::RefererTrimmed, srcHttp,
                                 QUrl("http://a.co.uk/x")),
             QByteArray("http://a.co.uk/"));

    // The injected referrer-meta mapping — the legs the interceptor
    // cannot rewrite (redirect follow-ups) are covered by the page's
    // own computed policy instead.
    QVERIFY(P::referrerMetaValue(P::RefererEngineDefault).isEmpty());
    QCOMPARE(P::referrerMetaValue(P::RefererTrimmed),
             QByteArray("strict-origin"));
    QCOMPARE(P::referrerMetaValue(P::RefererStrict),
             QByteArray("same-origin"));
    QCOMPARE(P::referrerMetaValue(P::RefererNever),
             QByteArray("no-referrer"));
}

void tst_Privacy::blockScriptStandard()
{
    // Standard blocks nothing, whatever the origin or resource type.
    QCOMPARE(PrivacyRequestInterceptor::securityLevel(),
             int(PrivacyRequestInterceptor::Standard));
    QVERIFY(!PrivacyRequestInterceptor::shouldBlockScript(
        QUrl("http://example.com/"),
        QWebEngineUrlRequestInfo::ResourceTypeScript));
}

void tst_Privacy::blockScriptSafer_data()
{
    QTest::addColumn<QString>("firstParty");
    QTest::addColumn<int>("type");
    QTest::addColumn<bool>("block");

    typedef QWebEngineUrlRequestInfo I;

    QTest::newRow("http page, external script")
        << QString("http://example.com/")
        << int(I::ResourceTypeScript) << true;
    QTest::newRow("http page, worker")
        << QString("http://example.com/")
        << int(I::ResourceTypeWorker) << true;
    QTest::newRow("http page, shared worker")
        << QString("http://example.com/")
        << int(I::ResourceTypeSharedWorker) << true;
    QTest::newRow("http page, service worker")
        << QString("http://example.com/")
        << int(I::ResourceTypeServiceWorker) << true;
    QTest::newRow("http page, image passes")
        << QString("http://example.com/")
        << int(I::ResourceTypeImage) << false;
    QTest::newRow("http page, xhr passes")
        << QString("http://example.com/")
        << int(I::ResourceTypeXhr) << false;
    QTest::newRow("http page, main-frame nav passes")
        << QString("http://example.com/")
        << int(I::ResourceTypeMainFrame) << false;
    QTest::newRow("https page keeps scripts")
        << QString("https://example.com/")
        << int(I::ResourceTypeScript) << false;
    QTest::newRow("file page keeps scripts")
        << QString("file:///tmp/x.html")
        << int(I::ResourceTypeScript) << false;
    // Secure-context exemption: http on loopback is trustworthy.
    QTest::newRow("localhost keeps scripts")
        << QString("http://localhost:8000/")
        << int(I::ResourceTypeScript) << false;
    QTest::newRow("*.localhost keeps scripts")
        << QString("http://app.localhost/")
        << int(I::ResourceTypeScript) << false;
    QTest::newRow("loopback IPv4 keeps scripts")
        << QString("http://127.0.0.1:9/")
        << int(I::ResourceTypeScript) << false;
    QTest::newRow("loopback IPv6 keeps scripts")
        << QString("http://[::1]/")
        << int(I::ResourceTypeScript) << false;
    // ...but a plain http LAN or public host is not trustworthy.
    QTest::newRow("http LAN host blocks scripts")
        << QString("http://192.168.1.1/")
        << int(I::ResourceTypeScript) << true;
    QTest::newRow("http .local host blocks scripts")
        << QString("http://printer.local/")
        << int(I::ResourceTypeScript) << true;
    QTest::newRow("empty first party passes")
        << QString() << int(I::ResourceTypeScript) << false;
}

void tst_Privacy::blockScriptSafer()
{
    setSecurityLevel(PrivacyRequestInterceptor::Safer);
    QCOMPARE(PrivacyRequestInterceptor::securityLevel(),
             int(PrivacyRequestInterceptor::Safer));
    QFETCH(QString, firstParty);
    QFETCH(int, type);
    QFETCH(bool, block);
    QCOMPARE(PrivacyRequestInterceptor::shouldBlockScript(
                 QUrl(firstParty),
                 QWebEngineUrlRequestInfo::ResourceType(type)),
             block);

    // Safest keeps the same interceptor answer (its engine-side
    // JavascriptEnabled-off is stronger anyway).
    setSecurityLevel(PrivacyRequestInterceptor::Safest);
    QVERIFY(PrivacyRequestInterceptor::shouldBlockScript(
        QUrl("http://example.com/"),
        QWebEngineUrlRequestInfo::ResourceTypeScript));
}

void tst_Privacy::securityLevelAttributes()
{
    // The engine half of the tiers is applied to the profile's
    // QWebEngineSettings by BrowserProfile::applySettings().  A
    // scratch off-the-record profile keeps the browser's real
    // profile untouched.
    QWebEngineProfile profile;
    QWebEngineSettings *engineSettings = profile.settings();

    QSettings settings;
    settings.setValue(QLatin1String("websettings/enableJavascript"), true);

    setSecurityLevel(PrivacyRequestInterceptor::Standard);
    BrowserProfile::applySettings(&profile);
    QVERIFY(engineSettings->testAttribute(QWebEngineSettings::JavascriptEnabled));
    QVERIFY(!engineSettings->testAttribute(QWebEngineSettings::PlaybackRequiresUserGesture));

    setSecurityLevel(PrivacyRequestInterceptor::Safer);
    BrowserProfile::applySettings(&profile);
    QVERIFY(engineSettings->testAttribute(QWebEngineSettings::JavascriptEnabled));
    QVERIFY(engineSettings->testAttribute(QWebEngineSettings::PlaybackRequiresUserGesture));

    // Safest's JS-off overrides an enabled websettings checkbox.
    setSecurityLevel(PrivacyRequestInterceptor::Safest);
    BrowserProfile::applySettings(&profile);
    QVERIFY(!engineSettings->testAttribute(QWebEngineSettings::JavascriptEnabled));
    QVERIFY(engineSettings->testAttribute(QWebEngineSettings::PlaybackRequiresUserGesture));

    // Toggling back down restores the stored preference.
    setSecurityLevel(PrivacyRequestInterceptor::Standard);
    BrowserProfile::applySettings(&profile);
    QVERIFY(engineSettings->testAttribute(QWebEngineSettings::JavascriptEnabled));
    QVERIFY(!engineSettings->testAttribute(QWebEngineSettings::PlaybackRequiresUserGesture));
}

// POL03: the "Render pages in dark mode" toggle lands on the
// profile's QWebEngineSettings — applySettings() is the single path
// every profile (normal, private, Tor, containers) goes through.
void tst_Privacy::forceDarkMode()
{
    QWebEngineProfile profile;
    QWebEngineSettings *engineSettings = profile.settings();

    QSettings settings;
    settings.remove(QLatin1String("websettings/forceDarkMode"));
    BrowserProfile::applySettings(&profile);
    QVERIFY(!engineSettings->testAttribute(
                QWebEngineSettings::ForceDarkMode));

    settings.setValue(QLatin1String("websettings/forceDarkMode"), true);
    BrowserProfile::applySettings(&profile);
    QVERIFY(engineSettings->testAttribute(
                QWebEngineSettings::ForceDarkMode));

    settings.setValue(QLatin1String("websettings/forceDarkMode"), false);
    BrowserProfile::applySettings(&profile);
    QVERIFY(!engineSettings->testAttribute(
                QWebEngineSettings::ForceDarkMode));
    settings.remove(QLatin1String("websettings/forceDarkMode"));
}

// SAFE04: the two resource-type toggles — privacy/blockRemoteFonts
// (opt-in, default off) and privacy/blockPrefetch (default on) — feed
// the pure shouldBlockResource() decision the interceptors consult.
void tst_Privacy::resourceBlockToggles()
{
    typedef QWebEngineUrlRequestInfo I;
    typedef PrivacyRequestInterceptor P;
    QSettings settings;
    const QHash<QByteArray, QByteArray> noHeaders;
    // Chromium marks every prefetch flavor with a purpose header —
    // <link rel=prefetch> arrives typed ResourceTypePrefetch, while
    // speculation-rules prefetch/prerender navigations are
    // misclassified as MainFrame and only the header gives them away.
    const QHash<QByteArray, QByteArray> specPrefetchHeaders = {
        { "Sec-Purpose", "prefetch" },
        { "Purpose", "prefetch" },
    };

    // Shipped defaults: prefetch blocked, remote fonts pass.
    QVERIFY(!P::blockRemoteFontsEnabled());
    QVERIFY(P::blockPrefetchEnabled());
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeFontResource,
                                    noHeaders));
    QVERIFY(P::shouldBlockResource(I::ResourceTypePrefetch, noHeaders));
    QVERIFY(P::shouldBlockResource(I::ResourceTypeMainFrame,
                                   specPrefetchHeaders));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeMainFrame, noHeaders));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeImage, noHeaders));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeScript, noHeaders));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeStylesheet,
                                    noHeaders));
    // Header matching is case-insensitive on both sides.
    QVERIFY(P::shouldBlockResource(I::ResourceTypeMainFrame,
                                   {{ "sec-purpose", "Prefetch" }}));
    QVERIFY(P::shouldBlockResource(
                I::ResourceTypeMainFrame,
                {{ "Sec-Purpose", "prefetch" },
                 { "X-Unrelated", "prefetch" }}));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeMainFrame,
                                    {{ "X-Purpose", "prefetch" }}));

    // Arming the font toggle blocks only font downloads.
    settings.setValue(QLatin1String("privacy/blockRemoteFonts"), true);
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(P::blockRemoteFontsEnabled());
    QVERIFY(P::shouldBlockResource(I::ResourceTypeFontResource,
                                   noHeaders));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeImage, noHeaders));

    // The toggles are independent: disarming prefetch still leaves
    // the font block on — and untags both prefetch flavors.
    settings.setValue(QLatin1String("privacy/blockPrefetch"), false);
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(!P::blockPrefetchEnabled());
    QVERIFY(!P::shouldBlockResource(I::ResourceTypePrefetch, noHeaders));
    QVERIFY(!P::shouldBlockResource(I::ResourceTypeMainFrame,
                                    specPrefetchHeaders));
    QVERIFY(P::shouldBlockResource(I::ResourceTypeFontResource,
                                   noHeaders));

    // Back to defaults for the rest of the suite.
    init();
}

void tst_Privacy::thirdPartyCookies()
{
    SubCookieJar jar;
    // The jar loads the real QSettings in its ctor — pin every input
    // it reads so a developer profile can't steer the assertions.
    jar.setBlockedCookies(QStringList());
    jar.setAllowedCookies(QStringList());
    jar.setAllowForSessionCookies(QStringList());
    jar.setBlockThirdPartyCookies(true);
    QVERIFY(jar.blockThirdPartyCookies());

    // Even the most permissive accept policy rejects a third-party
    // cookie while the hardening is on.
    jar.setAcceptPolicy(CookieJar::AcceptAlways);
    QVERIFY(!jar.isAllowedForHost(QLatin1String("tracker.com"), true));
    QVERIFY(jar.isAllowedForHost(QLatin1String("tracker.com"), false));

    // Toggle off restores the accept-policy behavior.
    jar.setBlockThirdPartyCookies(false);
    QVERIFY(jar.isAllowedForHost(QLatin1String("tracker.com"), true));

    // Under the default policy the flag changes nothing for
    // first-party cookies.
    jar.setAcceptPolicy(CookieJar::AcceptOnlyFromSitesNavigatedTo);
    jar.setBlockThirdPartyCookies(true);
    QVERIFY(jar.isAllowedForHost(QLatin1String("site.com"), false));
    QVERIFY(!jar.isAllowedForHost(QLatin1String("site.com"), true));
}

void tst_Privacy::thirdPartyCookieExceptions()
{
    SubCookieJar jar;
    jar.setBlockedCookies(QStringList());
    jar.setAllowedCookies(QStringList());
    jar.setAllowForSessionCookies(QStringList());
    jar.setAcceptPolicy(CookieJar::AcceptAlways);
    jar.setBlockThirdPartyCookies(true);

    // Explicit allow rules still win for an exception-listed third
    // party.
    jar.setAllowedCookies(QStringList() << QLatin1String("cdn.needed.com"));
    QVERIFY(jar.isAllowedForHost(QLatin1String("cdn.needed.com"), true));
    QVERIFY(!jar.isAllowedForHost(QLatin1String("other-cdn.com"), true));

    jar.setAllowForSessionCookies(QStringList() << QLatin1String("session-ok.com"));
    QVERIFY(jar.isAllowedForHost(QLatin1String("session-ok.com"), true));

    // Blocked rules run before everything.
    jar.setBlockedCookies(QStringList() << QLatin1String("cdn.needed.com"));
    QVERIFY(!jar.isAllowedForHost(QLatin1String("cdn.needed.com"), true));
    QVERIFY(!jar.isAllowedForHost(QLatin1String("cdn.needed.com"), false));
}

// XSLEAK03: decision-level coverage for the opt-in third-party
// WebSocket block; the wire-level half is thirdPartyWebSocketEndToEnd.
void tst_Privacy::webSocketBlockDecision()
{
    typedef QWebEngineUrlRequestInfo I;
    typedef PrivacyRequestInterceptor P;
    const QUrl pageUrl(QLatin1String("https://example.com/"));
    const QUrl sameSiteSocket(QLatin1String("wss://api.example.com/socket"));
    const QUrl crossSiteSocket(QLatin1String("wss://tracker.io/socket"));

    // Default off — nothing is refused regardless of the parties.
    QVERIFY(!P::blockThirdPartyWebSocketsEnabled());
    QVERIFY(!P::shouldBlockWebSocket(pageUrl, crossSiteSocket,
                                     I::ResourceTypeWebSocket));

    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("blockThirdPartyWebSockets"), true);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();

    QVERIFY(P::blockThirdPartyWebSocketsEnabled());
    QVERIFY(P::shouldBlockWebSocket(pageUrl, crossSiteSocket,
                                    I::ResourceTypeWebSocket));
    // Same-site sockets and non-socket resource types are untouched.
    QVERIFY(!P::shouldBlockWebSocket(pageUrl, sameSiteSocket,
                                     I::ResourceTypeWebSocket));
    QVERIFY(!P::shouldBlockWebSocket(pageUrl, crossSiteSocket,
                                     I::ResourceTypeXhr));
    // No first-party context -> nothing to be third-party to.
    QVERIFY(!P::shouldBlockWebSocket(QUrl(), crossSiteSocket,
                                     I::ResourceTypeWebSocket));

    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("blockThirdPartyWebSockets"), false);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(!P::shouldBlockWebSocket(pageUrl, crossSiteSocket,
                                     I::ResourceTypeWebSocket));
    init();
}

// SEC17: the ClearURLs-style strip decision — with rustcore linked
// these rows pin the vendored ruleset's semantics; without it the
// stage is absent and every URL passes through untouched.
void tst_Privacy::stripTrackingParams_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("expected");

    QTest::newRow("utm-param")
        << "https://example.com/?utm_source=x&real=1"
        << "https://example.com/?real=1";
    QTest::newRow("last-param-drops-qmark")
        << "https://example.com/p?fbclid=zzz"
        << "https://example.com/p";
    QTest::newRow("fragment-preserved")
        << "https://example.com/?gclid=g&a=1#sec"
        << "https://example.com/?a=1#sec";
    QTest::newRow("order-preserved")
        << "https://example.com/?b=1&utm_medium=x&a=2"
        << "https://example.com/?b=1&a=2";
    QTest::newRow("case-insensitive")
        << "https://example.com/?UTM_SOURCE=x&Gclid=y&ok=1"
        << "https://example.com/?ok=1";
    QTest::newRow("encoded-name")
        << "https://example.com/?utm%5Fsource=x&ok=1"
        << "https://example.com/?ok=1";
    QTest::newRow("no-trackers")
        << "https://example.com/?id=42&page=2"
        << "https://example.com/?id=42&page=2";
    QTest::newRow("no-query")
        << "https://example.com/path#frag"
        << "https://example.com/path#frag";
    QTest::newRow("non-http-untouched")
        << "ftp://example.com/f?utm_source=x"
        << "ftp://example.com/f?utm_source=x";
    QTest::newRow("qmark-in-fragment")
        << "https://example.com/#frag?utm_source=x"
        << "https://example.com/#frag?utm_source=x";
}

void tst_Privacy::stripTrackingParams()
{
    QFETCH(QString, url);
    QFETCH(QString, expected);
#if !defined(ARORA_RUSTCORE)
    // No-rust build: the strip stage is absent — the URL passes
    // through untouched (modulo QUrl's own %-escape normalization).
    expected = QString::fromUtf8(QUrl(url).toEncoded());
#endif
    QCOMPARE(QString::fromUtf8(
                 PrivacyRequestInterceptor::strippedUrl(QUrl(url))
                     .toEncoded()),
             expected);
}

void tst_Privacy::stripToggleAndExceptions()
{
    // The toggle is on by default (init pins it).
    QVERIFY(PrivacyRequestInterceptor::stripTrackingParamsEnabled());

    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("stripTrackingParams"), false);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    QVERIFY(!PrivacyRequestInterceptor::stripTrackingParamsEnabled());
    init();
    QVERIFY(PrivacyRequestInterceptor::stripTrackingParamsEnabled());

#if defined(ARORA_RUSTCORE)
    typedef PrivacyRequestInterceptor P;
    // Per-site exceptions ride the ruleset: a host where stripping
    // breaks auth is exempt entirely, a keep-list spares named params
    // while the rest still strip.
    const QByteArray rules = QByteArrayLiteral(
        R"({"version":1,"params":["test_*","si"],
            "exceptions":[{"host":"exempt.example"},
                          {"host":"keep.example","keep":["si"]}]})");
    QCOMPARE(rc_urlstrip_load_rules(
                 reinterpret_cast<const uint8_t *>(rules.constData()),
                 size_t(rules.size())),
             RC_OK);

    QCOMPARE(P::strippedUrl(QUrl("https://exempt.example/?test_x=1&si=2")),
             QUrl("https://exempt.example/?test_x=1&si=2"));
    QCOMPARE(P::strippedUrl(QUrl("https://sub.exempt.example/?test_x=1")),
             QUrl("https://sub.exempt.example/?test_x=1"));
    // A keep-listed param survives; other listed params still strip.
    QCOMPARE(P::strippedUrl(QUrl("https://keep.example/?si=1&test_x=2")),
             QUrl("https://keep.example/?si=1"));
    QCOMPARE(P::strippedUrl(QUrl("https://other.example/?test_x=2&ok=1")),
             QUrl("https://other.example/?ok=1"));

    // A malformed ruleset must not disarm stripping — the previous
    // set stays active.
    const QByteArray bad = QByteArrayLiteral("{not json");
    QCOMPARE(rc_urlstrip_load_rules(
                 reinterpret_cast<const uint8_t *>(bad.constData()),
                 size_t(bad.size())),
             RC_CORRUPT);
    QCOMPARE(P::strippedUrl(QUrl("https://other.example/?test_x=2")),
             QUrl("https://other.example/"));

    // reload() with no data-dir override re-arms the vendored set.
    QCOMPARE(rc_urlstrip_reload(), RC_OK);
    QCOMPARE(P::strippedUrl(QUrl("https://other.example/?test_x=2")),
             QUrl("https://other.example/?test_x=2"));
    QCOMPARE(P::strippedUrl(QUrl("https://e.com/?utm_source=x&ok=1")),
             QUrl("https://e.com/?ok=1"));
#endif
}

// XSLEAK03: loopback fixture for the cross-site e2e checks.  One
// QTcpServer answers both the "localhost" parent page and the
// "127.0.0.1" endpoints — different hosts are different sites, so a
// frame on 127.0.0.1 inside a localhost document is genuinely
// third-party.  The request log is the observable evidence: what the
// interceptor or the cookie filter refuses never reaches the wire.
// Every response is marked no-store so Chromium's heuristic cache
// cannot hide a refetch.
class LeakProbeServer : public QObject
{
    Q_OBJECT

public:
    LeakProbeServer(QObject *parent = nullptr)
        : QObject(parent)
    {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket *socket = m_server.nextPendingConnection();
            socket->setParent(&m_server);
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() {
                if (!socket->peek(8192).contains("\r\n\r\n"))
                    return;
                respond(socket, socket->readAll());
            });
        });
    }

    bool start()
    {
        // Any covers both stacks — "localhost" may resolve to ::1
        // while the third-party endpoint is written 127.0.0.1.
        return m_server.listen(QHostAddress::Any);
    }

    int port() const
    {
        return m_server.serverPort();
    }

    QString localUrl(const QString &host, const QString &path) const
    {
        return QString::fromLatin1("http://%1:%2%3")
            .arg(host).arg(m_server.serverPort()).arg(path);
    }

    QStringList requests;                      // every request target
    QHash<QString, QByteArray> cookieHeaders;  // path -> Cookie header

private:
    void respond(QTcpSocket *socket, const QByteArray &request)
    {
        const QByteArray target = request.split(' ').value(1);
        requests.append(QString::fromUtf8(target));
        for (const QByteArray &line : request.split('\n')) {
            if (line.startsWith("Cookie:"))
                cookieHeaders[QString::fromUtf8(target)] =
                    line.mid(7).trimmed();
        }

        const QByteArray port = QByteArray::number(m_server.serverPort());
        if (target == "/ws") {
            // Minimal RFC 6455 accept — the socket is left open so the
            // page's WebSocket reaches readyState OPEN.
            QByteArray key;
            for (const QByteArray &line : request.split('\n')) {
                if (line.startsWith("Sec-WebSocket-Key:"))
                    key = line.mid(18).trimmed();
            }
            const QByteArray accept = QCryptographicHash::hash(
                key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                QCryptographicHash::Sha1).toBase64();
            socket->write("HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
            socket->flush();
            return;
        }

        QByteArray extraHeaders;
        QByteArray body;
        if (target.startsWith("/parent") || target.startsWith("/wspage"))
            extraHeaders += "Set-Cookie: xsleak_first=1; Path=/\r\n";
        if (target.startsWith("/parent")) {
            body = "<html><body><iframe src=\"http://127.0.0.1:" + port
                   + "/frame\"></iframe></body></html>";
        } else if (target.startsWith("/wspage")) {
            // Plain host page — the test drives WebSocket connects
            // through runJavaScript once the load settles.  (A ws
            // upgrade refused during initial parse never resolves
            // Chromium's pending handshake, so the load event — and
            // loadFinished — would hang; the post-load connect tests
            // the same interceptor path without that quirk.)
            body = "<html><body>ws host page</body></html>";
        } else if (target.startsWith("/frame")) {
            // SameSite=None is required because Chromium drops
            // Lax-by-default third-party cookies on its own — with it,
            // only Arora's cookie filter can veto this Set-Cookie, so
            // the armed/disarmed phases isolate the filter's effect.
            // (Secure rides along because SameSite=None demands it;
            // Chromium accepts Secure cookies from loopback origins.)
            extraHeaders += "Set-Cookie: xsleak_third=1; Path=/; "
                            "SameSite=None; Secure\r\n";
            body = "<html><body>frame</body></html>";
        } else {
            body = "<html><body>probe</body></html>";
        }
        socket->write("HTTP/1.0 200 OK\r\nContent-Type: text/html\r\n"
                      "Cache-Control: no-store\r\n"
                      + extraHeaders
                      + "Content-Length: "
                      + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n\r\n" + body);
        socket->disconnectFromHost();
    }

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

// XSLEAK03 fix item (a): prove the PRIV01 third-party cookie block
// engages end-to-end — a real cross-site iframe Set-Cookie through the
// profile's cookie-store filter.  The page lives on localhost and the
// frame on 127.0.0.1: different hosts are different sites, so the
// frame's cookie is genuinely third-party.
void tst_Privacy::thirdPartyCookieEndToEnd()
{
    LeakProbeServer server;
    QVERIFY(server.start());

    // A throwaway off-the-record profile — storage works in memory and
    // nothing persists past the test.
    QWebEngineProfile *profile = new QWebEngineProfile(this);
    CookieJar *jar = new CookieJar(profile, this);
    jar->setBlockedCookies(QStringList());
    jar->setAllowedCookies(QStringList());
    jar->setAllowForSessionCookies(QStringList());
    jar->setAcceptPolicy(CookieJar::AcceptAlways);

    QStringList added;
    connect(profile->cookieStore(),
            &QWebEngineCookieStore::cookieAdded, this,
            [&added](const QNetworkCookie &cookie) {
        added.append(QString::fromUtf8(cookie.name()));
    });

    WebPage page(profile);
    jar->setBlockThirdPartyCookies(true);
    QVERIFY(loadSync(&page,
            QUrl(server.localUrl(QLatin1String("localhost"),
                                 QLatin1String("/parent")))));
    QTRY_VERIFY(server.requests.contains(QLatin1String("/frame")));
    QTest::qWait(400);   // the store commit trails the request log

    // Armed: the parent's first-party cookie lands but the cross-site
    // frame's does not — storage-level proof the filter vetoed it.
    QVERIFY(added.contains(QLatin1String("xsleak_first")));
    QVERIFY(!added.contains(QLatin1String("xsleak_third")));

    // A same-site hop on the frame's origin would send the cookie back
    // had it been stored.
    QVERIFY(loadSync(&page,
            QUrl(server.localUrl(QLatin1String("127.0.0.1"),
                                 QLatin1String("/probe")))));
    QVERIFY(!server.cookieHeaders.value(QLatin1String("/probe"))
                 .contains("xsleak_third"));

    // Control: the same fixture with the block off must both store and
    // send the third-party cookie — otherwise the armed phase proved
    // nothing.
    jar->setBlockThirdPartyCookies(false);
    QVERIFY(loadSync(&page,
            QUrl(server.localUrl(QLatin1String("localhost"),
                                 QLatin1String("/parent?off")))));
    QTRY_VERIFY(server.requests.count(QLatin1String("/frame")) >= 2);
    QTRY_VERIFY(added.contains(QLatin1String("xsleak_third")));
    QVERIFY(loadSync(&page,
            QUrl(server.localUrl(QLatin1String("127.0.0.1"),
                                 QLatin1String("/probe2")))));
    QVERIFY(server.cookieHeaders.value(QLatin1String("/probe2"))
                .contains("xsleak_third=1"));
}

// XSLEAK03 fix item (e): the opt-in third-party WebSocket block —
// armed, a ws:// upgrade from a localhost page to 127.0.0.1 is
// refused inside the interceptor (never reaches the server and the
// socket never opens); same-site and disarmed sockets connect.
// The socket is created after the load settles: a ws refused during
// initial parse leaves Chromium's pending handshake unresolved and the
// load event never fires, which would test the engine quirk rather
// than the policy.
static QString wsProbeScript(const QString &host, int port,
                             const QString &slot)
{
    return QString::fromLatin1(
        "window.%1='pending';"
        "window.%1_ws=new WebSocket('ws://%2:%3/ws');"
        "window.%1_ws.onopen=function(){window.%1='open';};"
        "window.%1_ws.onerror=window.%1_ws.onclose=function(){"
        "window.%1=window.%1==='pending'?'fail':window.%1;};'started'")
        .arg(slot, host).arg(port);
}

void tst_Privacy::thirdPartyWebSocketEndToEnd()
{
    LeakProbeServer server;
    QVERIFY(server.start());

    QWebEngineProfile *profile = new QWebEngineProfile(this);
    // The real interceptor path, with an empty adblock matcher — this
    // test is about the privacy stages, not the rules engine.
    profile->setUrlRequestInterceptor(new PrivacyRequestInterceptor(
        new AdBlockNetwork(profile), profile));

    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("blockThirdPartyWebSockets"), true);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();

    WebPage page(profile);
    QVERIFY(loadSync(&page,
            QUrl(server.localUrl(QLatin1String("localhost"),
                                 QLatin1String("/wspage")))));

    // Armed: the cross-site upgrade is refused inside the interceptor —
    // the wire stays clean and the socket never reaches OPEN.
    evalSync(&page, wsProbeScript(QLatin1String("127.0.0.1"),
                                  server.port(),
                                  QLatin1String("__wsCross")));
    QTest::qWait(500);   // give a wrongly-passed request time to land
    QVERIFY(!server.requests.contains(QLatin1String("/ws")));
    QTRY_VERIFY(evalSync(&page, QLatin1String(
        "String(window.__wsCross)")).toString() != QLatin1String("open"));

    // Same-site control: localhost -> localhost connects even while
    // the cross-site block is armed.
    evalSync(&page, wsProbeScript(QLatin1String("localhost"),
                                  server.port(),
                                  QLatin1String("__wsSame")));
    QTRY_COMPARE(evalSync(&page, QLatin1String(
        "String(window.__wsSame)")).toString(), QLatin1String("open"));
    QCOMPARE(server.requests.count(QLatin1String("/ws")), 1);

    // Disarmed: the identical cross-site socket connects.
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("blockThirdPartyWebSockets"), false);
    settings.endGroup();
    PrivacyRequestInterceptor::loadSettings();
    evalSync(&page, wsProbeScript(QLatin1String("127.0.0.1"),
                                  server.port(),
                                  QLatin1String("__wsOff")));
    QTRY_COMPARE(evalSync(&page, QLatin1String(
        "String(window.__wsOff)")).toString(), QLatin1String("open"));
    QVERIFY(server.requests.count(QLatin1String("/ws")) >= 2);

    init();
}

// Seeds a fake profile storage tree with the site-data dirs the wipe
// knows about; returns the sentinel names present afterwards.
static void seedStorageTree(const QDir &root)
{
    const QStringList dirs = {
        QStringLiteral("Local Storage"),
        QStringLiteral("IndexedDB"),
        QStringLiteral("Service Worker"),
        QStringLiteral("Network"),
        QStringLiteral("Cache"),
        QStringLiteral("Code Cache"),
        QStringLiteral("KeepMe"),   // unrelated profile state
    };
    for (const QString &name : dirs) {
        QVERIFY(root.mkpath(name));
        QFile f(root.filePath(name + QLatin1String("/data.bin")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
    }
    QFile state(root.filePath(QStringLiteral("Network Persistent State")));
    QVERIFY(state.open(QIODevice::WriteOnly));
    state.close();
}

void tst_Privacy::deferredWipe()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir root(tmp.path());
    seedStorageTree(root);

    // No sentinel -> nothing touched.
    QVERIFY(BrowserProfile::clearDeferredSiteStorage(tmp.path()));
    QVERIFY(root.exists(QStringLiteral("Local Storage")));
    QVERIFY(root.exists(QStringLiteral("Network")));

    // Site-wipe sentinel: site data goes, Network/Cache stay.
    QFile sentinel(root.filePath(QStringLiteral("arora-site-wipe.pending")));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.close();
    QVERIFY(BrowserProfile::clearDeferredSiteStorage(tmp.path()));
    QVERIFY(!root.exists(QStringLiteral("Local Storage")));
    QVERIFY(!root.exists(QStringLiteral("IndexedDB")));
    QVERIFY(!root.exists(QStringLiteral("Service Worker")));
    QVERIFY(!root.exists(QStringLiteral("Network Persistent State")));
    QVERIFY(root.exists(QStringLiteral("Network")));
    QVERIFY(root.exists(QStringLiteral("Cache")));
    QVERIFY(root.exists(QStringLiteral("KeepMe")));
    QVERIFY(!root.exists(QStringLiteral("arora-site-wipe.pending")));
}

void tst_Privacy::deferredExitWipe()
{
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir root(tmp.path());
    seedStorageTree(root);

    // The clear-on-exit sentinel additionally removes Chromium's
    // Network/ (cookie db, HSTS) and cache trees.
    QFile sentinel(root.filePath(QStringLiteral("arora-exit-wipe.pending")));
    QVERIFY(sentinel.open(QIODevice::WriteOnly));
    sentinel.close();
    QVERIFY(BrowserProfile::clearDeferredSiteStorage(tmp.path()));
    QVERIFY(!root.exists(QStringLiteral("Local Storage")));
    QVERIFY(!root.exists(QStringLiteral("Network")));
    QVERIFY(!root.exists(QStringLiteral("Cache")));
    QVERIFY(!root.exists(QStringLiteral("Code Cache")));
    QVERIFY(root.exists(QStringLiteral("KeepMe")));
    QVERIFY(!root.exists(QStringLiteral("arora-exit-wipe.pending")));
}

void tst_Privacy::chromiumFlags()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("webrtcIpProtection"), true);
    settings.setValue(QLatin1String("secureDns"), true);
    // The legacy bool alone decides here — a stale mode key from an
    // earlier run would take precedence and mask it.
    settings.remove(QLatin1String("secureDnsMode"));
    settings.endGroup();

    // Pre-existing user flags are kept, nothing is duplicated.
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            "--user-flag=1 --force-webrtc-ip-handling-policy=disable_non_proxied_udp");
    BrowserProfile::applyChromiumFlags();
    const QStringList flags = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")).split(QLatin1Char(' '),
                                                   Qt::SkipEmptyParts);
    QVERIFY(flags.contains(QLatin1String("--user-flag=1")));
    QCOMPARE(flags.count(QLatin1String(
        "--force-webrtc-ip-handling-policy=disable_non_proxied_udp")), 1);
    // The list switches MERGE: DnsOverHttps (secureDns) and
    // ParallelDownloading (DLACC02, on by default) ride the same
    // --enable-features switch rather than shadowing each other.
    QString enableFeatures;
    for (const QString &flag : flags) {
        if (flag.startsWith(QLatin1String("--enable-features=")))
            enableFeatures = flag;
    }
    const QStringList enabled = enableFeatures.mid(
        QStringLiteral("--enable-features=").size()).split(
        QLatin1Char(','), Qt::SkipEmptyParts);
    QVERIFY(enabled.contains(QLatin1String("DnsOverHttps")));
    QVERIFY(enabled.contains(QLatin1String("ParallelDownloading")));

    // Toggles off -> only the unconditional TELEM01 kill-list is
    // appended, user flags stay put, and a second run adds nothing.
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("webrtcIpProtection"), false);
    settings.setValue(QLatin1String("secureDns"), false);
    settings.endGroup();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--user-flag=1");
    BrowserProfile::applyChromiumFlags();
    BrowserProfile::applyChromiumFlags();
    const QStringList quietFlags = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")).split(QLatin1Char(' '),
                                                   Qt::SkipEmptyParts);
    QVERIFY(quietFlags.contains(QLatin1String("--user-flag=1")));
    for (const char *kill :
         { "--disable-background-networking", "--disable-component-update",
           "--disable-domain-reliability", "--disable-metrics",
           "--disable-sync", "--no-first-run" }) {
        QCOMPARE(quietFlags.count(QLatin1String(kill)), 1);
    }
    QVERIFY(!quietFlags.contains(QLatin1String(
        "--force-webrtc-ip-handling-policy=disable_non_proxied_udp")));
    QVERIFY(!quietFlags.contains(
        QLatin1String("--enable-features=DnsOverHttps")));

    // DOH01 precedence: when the mode key exists it decides — "off"
    // wins over a stale legacy bool, and the custom modes arm the
    // feature gate even with the bool absent.
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("secureDns"), true);
    settings.setValue(QLatin1String("secureDnsMode"), 0);
    settings.endGroup();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    QVERIFY(!QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                 .contains(QLatin1String("--enable-features=DnsOverHttps")));

    settings.beginGroup(QLatin1String("privacy"));
    settings.remove(QLatin1String("secureDns"));
    settings.setValue(QLatin1String("secureDnsMode"), 3);
    settings.endGroup();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    QVERIFY(QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                .contains(QLatin1String("--enable-features=DnsOverHttps")));

    // The Qt-side apply path: a strict mode with a syntactically
    // valid https template is accepted by
    // QWebEngineGlobalSettings::setDnsMode (URI validation only —
    // reachability is the resolver's problem), and mode 0 clears it.
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("secureDnsMode"), 3);
    settings.setValue(QLatin1String("secureDnsServer"),
                      QLatin1String("https://127.0.0.1:1/dns-query"));
    settings.endGroup();
    BrowserProfile::applySecureDns();
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("secureDnsMode"), 0);
    settings.endGroup();
    BrowserProfile::applySecureDns();

    // POL03: middle-click autoscroll arms Blink's
    // MiddleClickAutoscroll feature — on by default everywhere but
    // macOS.  The entry must MERGE into an operator's
    // --enable-blink-features switch, never append a shadowing second
    // occurrence (Chromium's last-switch-wins parse would drop the
    // operator's list).
    QSettings().remove(
        QLatin1String("websettings/middleClickAutoscroll"));
#if defined(Q_OS_MACOS)
    const bool autoscrollDefault = false;
#else
    const bool autoscrollDefault = true;
#endif
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
            "--enable-blink-features=SomeOperatorFeature");
    BrowserProfile::applyChromiumFlags();
    const QStringList mergedFlags = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS")).split(
        QLatin1Char(' '), Qt::SkipEmptyParts);
    int blinkSwitches = 0;
    QString blinkList;
    for (const QString &flag : mergedFlags) {
        if (flag.startsWith(QLatin1String("--enable-blink-features="))) {
            ++blinkSwitches;
            blinkList = flag;
        }
    }
    QCOMPARE(blinkSwitches, 1);
    QVERIFY(blinkList.contains(QLatin1String("SomeOperatorFeature")));
    QCOMPARE(blinkList.contains(QLatin1String("MiddleClickAutoscroll")),
             autoscrollDefault);

    // A stored "off" leaves the feature out entirely; a stored "on"
    // puts it back.  The same merge covers --enable-features:
    // DnsOverHttps joins the operator's list rather than clobbering
    // it.
    QSettings().setValue(QLatin1String("websettings/middleClickAutoscroll"),
                         false);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    QVERIFY(!QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                 .contains(QLatin1String("MiddleClickAutoscroll")));

    QSettings().setValue(QLatin1String("websettings/middleClickAutoscroll"),
                         true);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=Foo");
    BrowserProfile::applyChromiumFlags();
    QVERIFY(QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                .contains(QLatin1String("--enable-blink-features=MiddleClickAutoscroll")));
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("secureDnsMode"), 3);
    settings.endGroup();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--enable-features=Foo");
    BrowserProfile::applyChromiumFlags();
    const QString features = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
    QVERIFY(features.contains(
        QLatin1String("--enable-features=Foo,DnsOverHttps")));
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("secureDnsMode"), 0);
    settings.endGroup();
    QSettings().remove(QLatin1String("websettings/middleClickAutoscroll"));

    // DLACC02: a stored "off" leaves ParallelDownloading out of the
    // merged switch entirely; removing the key restores the
    // default-on.
    QSettings().setValue(
        QLatin1String("downloadmanager/parallelSegments"), false);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    QVERIFY(!QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                 .contains(QLatin1String("ParallelDownloading")));
    QSettings().remove(
        QLatin1String("downloadmanager/parallelSegments"));
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    QVERIFY(QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                .contains(QLatin1String("ParallelDownloading")));

    // Leave the privacy group at the shipped defaults for any
    // post-test settings writes elsewhere in the suite.
    init();
}

void tst_Privacy::secureDnsTorGate()
{
    // SEC20: a tor process resolves names remotely through the managed
    // SOCKS proxy — local DoH would bypass the tunnel, so the stored
    // mode is forced off regardless of what normal-window settings
    // say.  Covers both surfaces that read it: the feature switch in
    // applyChromiumFlags() and the resolver mode in applySecureDns().
    QSettings settings;
    settings.beginGroup(QLatin1String("privacy"));
    settings.setValue(QLatin1String("secureDnsMode"), 3);
    settings.setValue(QLatin1String("secureDnsServer"),
                      QLatin1String("https://127.0.0.1:1/dns-query"));
    settings.endGroup();

    QCOMPARE(BrowserProfile::effectiveSecureDnsMode(), 3);
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    QVERIFY(QString::fromLocal8Bit(qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"))
                .contains(QLatin1String("DnsOverHttps")));

    // Gather-then-assert: QVERIFY would early-return with s_torMode
    // still armed, poisoning every later test in this binary.
    BrowserApplication::setTorMode(true);
    const int torMode = BrowserProfile::effectiveSecureDnsMode();
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", QByteArray());
    BrowserProfile::applyChromiumFlags();
    const QString torFlags = QString::fromLocal8Bit(
        qgetenv("QTWEBENGINE_CHROMIUM_FLAGS"));
    BrowserProfile::applySecureDns();
    BrowserApplication::setTorMode(false);

    QCOMPARE(torMode, 0);
    QVERIFY(!torFlags.contains(QLatin1String("DnsOverHttps")));
    QCOMPARE(BrowserProfile::effectiveSecureDnsMode(), 3);

    init();
}

void tst_Privacy::fingerprintNormalization()
{
    QSettings settings;

    // PRIV02 (a): TZ is forced to UTC, then the caller's original
    // env is restored verbatim — an exported user TZ is never lost.
    const bool tzWasSet = qEnvironmentVariableIsSet("TZ");
    const QByteArray tzBefore = qgetenv("TZ");
    settings.setValue(QLatin1String("privacy/reportUtcTimezone"), true);
    BrowserProfile::applyFingerprintEnvironment();
    QCOMPARE(qgetenv("TZ"), QByteArray("UTC"));
    settings.setValue(QLatin1String("privacy/reportUtcTimezone"), false);
    BrowserProfile::applyFingerprintEnvironment();
    QCOMPARE(qEnvironmentVariableIsSet("TZ"), tzWasSet);
    QCOMPARE(qgetenv("TZ"), tzBefore);

    // PRIV02 (b): a distinctive configured list proves the toggle
    // replaces the value on the wire while leaving the stored list
    // untouched.
    settings.setValue(QLatin1String("network/acceptLanguages"),
                      QStringList{QLatin1String("Klingon [tlh]")});
    QCOMPARE(AcceptLanguageDialog::acceptLanguages(),
             QStringList({QLatin1String("Klingon [tlh]")}));
    settings.setValue(QLatin1String("privacy/normalizeAcceptLanguage"), true);
    QCOMPARE(AcceptLanguageDialog::acceptLanguages(),
             AcceptLanguageDialog::normalizedAcceptLanguages());
    QCOMPARE(AcceptLanguageDialog::httpString(
                 AcceptLanguageDialog::acceptLanguages()),
             QByteArray("en-US, en;q=0.9"));
    settings.setValue(QLatin1String("privacy/normalizeAcceptLanguage"), false);
    QCOMPARE(AcceptLanguageDialog::acceptLanguages(),
             QStringList({QLatin1String("Klingon [tlh]")}));

    settings.remove(QLatin1String("network/acceptLanguages"));
    init();
}

QTEST_MAIN(tst_Privacy)
#include "tst_privacy.moc"
