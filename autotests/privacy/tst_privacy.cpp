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
#include <browserprofile.h>
#include <cookiejar.h>
#include <privacyrequestinterceptor.h>

#include <qdir.h>
#include <qsettings.h>
#include <qtemporarydir.h>
#include <qurl.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

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
    void settingsRoundTrip();
    void refererPolicyMigration();
    void refererRewriteMatrix();

    void blockScriptStandard();
    void blockScriptSafer_data();
    void blockScriptSafer();
    void securityLevelAttributes();

    void thirdPartyCookies();
    void thirdPartyCookieExceptions();

    void deferredWipe();
    void deferredExitWipe();

    void chromiumFlags();
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
    QVariant m_savedAcceptLanguages;
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
}

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
    QCOMPARE(PrivacyRequestInterceptor::isUpgradeCandidate(QUrl(url)),
             candidate);
}

void tst_Privacy::downgradeFlow()
{
    const QUrl http("http://neverssl.test/");
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(http));

    // Only https failures record a downgrade; an http "failure" is
    // not an upgrade candidate's problem.
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(http));

    // A failed https load drops the host from candidacy for the rest
    // of the session.
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://neverssl.test/")));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(QLatin1String("neverssl.test")));
    QVERIFY(!PrivacyRequestInterceptor::isUpgradeCandidate(http));

    // Idempotent — the second failure does not "newly" downgrade.
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://neverssl.test/other")));

    // A different host is unaffected.
    QVERIFY(PrivacyRequestInterceptor::isUpgradeCandidate(
                QUrl("http://other.test/")));
}

void tst_Privacy::downgradeBoundaries()
{
    // Private/local hosts are never candidates, so they never join
    // the downgrade set either (isPrivateOrLocalHost gate).
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://192.168.1.1/")));
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://abc.onion/")));
    QVERIFY(!PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://localhost/")));
    QVERIFY(!PrivacyRequestInterceptor::isDowngraded(QLatin1String("localhost")));

    // Case-insensitive host matching both ways.
    QVERIFY(PrivacyRequestInterceptor::noteNavigationFailure(
                QUrl("https://MiXeD.test/")));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(QLatin1String("mixed.test")));
    QVERIFY(PrivacyRequestInterceptor::isDowngraded(QLatin1String("MIXED.TEST")));
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
    QVERIFY(flags.contains(QLatin1String("--enable-features=DnsOverHttps")));

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

    // Leave the privacy group at the shipped defaults for any
    // post-test settings writes elsewhere in the suite.
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
