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

// ENG02 — the "FakeEngine" adapter test.
//
// Every case the old interceptor behavior tests covered is re-expressed
// engine-neutrally here: PolicyRequest structs go in (no QtWebEngine
// types — this is exactly what a second engine backend would marshal),
// PolicyVerdict structs come out.  The same matrix runs identically in
// the rust build (rustcore policy via FFI) and the no-rust build (Qt
// fallback) — identical verdicts is what proves the adapter delegates
// 1:1.

#include <QtTest>
#include "qtest_arora.h"

#include "navigationpolicy.h"
#include "privacyrequestinterceptor.h"

using Engine::CookieGateInput;
using Engine::HeaderList;
using Engine::NavigationPolicy;
using Engine::PolicyRequest;
using Engine::PolicyVerdict;

class tst_NavigationPolicy : public QObject
{
    Q_OBJECT

private:
    static PolicyRequest makeRequest(const QString &url, int resourceType,
                                     const QString &firstParty = QString())
    {
        PolicyRequest r;
        r.url = QUrl(url);
        r.firstPartyUrl = firstParty.isEmpty() ? QUrl(url) : QUrl(firstParty);
        r.resourceType = resourceType;
        r.method = QByteArrayLiteral("GET");
        r.scope = QStringLiteral("test");
        return r;
    }

    static NavigationPolicy::Snapshot defaultSnapshot()
    {
        return NavigationPolicy::Snapshot();
    }

    static void resetPolicy()
    {
        NavigationPolicy::loadSnapshot(defaultSnapshot());
        NavigationPolicy::clearDowngradedHosts();
        NavigationPolicy::clearBlockedDomainAllowances();
        NavigationPolicy::setDowngradeTtlMs(30 * 60 * 1000);
        // Drain any leftovers from previous cases.
        NavigationPolicy::clearHttpAllowance(QStringLiteral("ex.example"));
    }

private slots:
    void initTestCase();
    void init() { resetPolicy(); }

    void nonWebSchemesPass();
    void pingsAndReportsBlocked();
    void prefetchAndFontBlocks();
    void httpsFirstUpgradesMainFrame();
    void downgradeThenHttpsOnlyBlock();
    void httpAllowanceExempts();
    void scriptBlockTiers();
    void refererLevels();
    void torModeUpgradesEverythingButOnion();
    void cookieGateMatrix();
    void granularApiMatchesInterceptors();
    void failureImpliesDowngradeTable();
};

void tst_NavigationPolicy::initTestCase()
{
    resetPolicy();
}

void tst_NavigationPolicy::nonWebSchemesPass()
{
    const QStringList urls = {
        QStringLiteral("file:///etc/passwd"),
        QStringLiteral("qrc:///start.html"),
        QStringLiteral("chrome://extensions"),
        QStringLiteral("data:text/html,<b>"),
        QStringLiteral("about:blank"),
        QStringLiteral("javascript:alert(1)"),
    };
    for (const QString &u : urls) {
        const auto v = NavigationPolicy::evaluate(makeRequest(u, 0));
        QVERIFY2(v.action == PolicyVerdict::Action::Pass,
                 qPrintable(u));
    }
}

void tst_NavigationPolicy::pingsAndReportsBlocked()
{
    auto v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("https://t.example/ping"), 14));
    QCOMPARE(v.action, PolicyVerdict::Action::Block);

    v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("https://t.example/r"), 16));
    QCOMPARE(v.action, PolicyVerdict::Action::Block);

    // Sec-Fetch-Dest: report on any type.
    auto req = makeRequest(QStringLiteral("https://t.example/x"), 13);
    req.method = QByteArrayLiteral("POST");
    req.headers.append(qMakePair(QByteArray("Sec-Fetch-Dest"),
                                 QByteArray("report")));
    v = NavigationPolicy::evaluate(req);
    QCOMPARE(v.action, PolicyVerdict::Action::Block);

    // Toggle off -> allow.
    auto snap = defaultSnapshot();
    snap.blockPings = false;
    NavigationPolicy::loadSnapshot(snap);
    v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("https://t.example/ping"), 14));
    QCOMPARE(v.action, PolicyVerdict::Action::Allow);
}

void tst_NavigationPolicy::prefetchAndFontBlocks()
{
    // Typed prefetch.
    auto v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("https://t.example/p"), 11));
    QCOMPARE(v.action, PolicyVerdict::Action::Block);

    // Speculation-rules prefetch mislabeled MainFrame — header catches it.
    auto req = makeRequest(QStringLiteral("https://t.example/next"), 0);
    req.headers.append(qMakePair(QByteArray("Sec-Purpose"),
                                 QByteArray("prefetch")));
    v = NavigationPolicy::evaluate(req);
    QCOMPARE(v.action, PolicyVerdict::Action::Block);

    // Fonts are allowed by default, blocked under the opt-in.
    const auto font = makeRequest(QStringLiteral("https://t.example/f.woff2"), 5);
    QCOMPARE(NavigationPolicy::evaluate(font).action,
             PolicyVerdict::Action::Allow);
    auto snap = defaultSnapshot();
    snap.blockRemoteFonts = true;
    NavigationPolicy::loadSnapshot(snap);
    QCOMPARE(NavigationPolicy::evaluate(font).action,
             PolicyVerdict::Action::Block);
}

void tst_NavigationPolicy::httpsFirstUpgradesMainFrame()
{
    auto v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("http://example.com/path?q=1"), 0));
    QCOMPARE(v.action, PolicyVerdict::Action::Redirect);
    QCOMPARE(v.redirectUrl.toString(),
             QStringLiteral("https://example.com/path?q=1"));

    // Subresources are not upgraded by the privacy profile — and a
    // plain http: subresource is not refused either (main-frame only).
    v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("http://example.com/i.png"), 4));
    QCOMPARE(v.action, PolicyVerdict::Action::Allow);

    // Loopback / LAN / .onion hosts are exempt.
    const QStringList exempt = {
        QStringLiteral("http://localhost:8080/x"),
        QStringLiteral("http://127.0.0.1/x"),
        QStringLiteral("http://10.1.2.3/x"),
        QStringLiteral("http://192.168.1.1/x"),
        QStringLiteral("http://router.local/x"),
        QStringLiteral("http://abc.onion/x"),
    };
    for (const QString &u : exempt) {
        const auto r = NavigationPolicy::evaluate(makeRequest(u, 0));
        QVERIFY2(r.action != PolicyVerdict::Action::Redirect,
                 qPrintable(u));
    }
}

void tst_NavigationPolicy::downgradeThenHttpsOnlyBlock()
{
    const QString url = QStringLiteral("http://fails.example/");
    QCOMPARE(NavigationPolicy::evaluate(makeRequest(url, 0)).action,
             PolicyVerdict::Action::Redirect);

    // A genuine connection failure marks the host for this scope.
    QVERIFY(NavigationPolicy::noteNavigationFailure(
        QUrl(QStringLiteral("https://fails.example/")),
        /*ConnectionErrorDomain*/2, /*ERR_CONNECTION_RESET*/-101,
        QStringLiteral("test")));

    const auto v = NavigationPolicy::evaluate(makeRequest(url, 0));
    QCOMPARE(v.action, PolicyVerdict::Action::Block);
    QCOMPARE(v.reason, QByteArray("https-only"));

    // The recorded refusal is consume-once for the interstitial.
    QVERIFY(NavigationPolicy::takeBlockedHttpNav(QUrl(url)));
    QVERIFY(!NavigationPolicy::takeBlockedHttpNav(QUrl(url)));

    // A non-TLS-evidence error never marks (DNS failure).
    QVERIFY(!NavigationPolicy::noteNavigationFailure(
        QUrl(QStringLiteral("https://dnsfail.example/")),
        2, /*ERR_NAME_NOT_RESOLVED*/-105, QStringLiteral("test")));
    QCOMPARE(NavigationPolicy::evaluate(
                 makeRequest(QStringLiteral("http://dnsfail.example/"), 0)).action,
             PolicyVerdict::Action::Redirect);
}

void tst_NavigationPolicy::httpAllowanceExempts()
{
    const QString url = QStringLiteral("http://ex.example/");
    NavigationPolicy::markDowngraded(QStringLiteral("ex.example"),
                                     QStringLiteral("test"));
    QCOMPARE(NavigationPolicy::evaluate(makeRequest(url, 0)).action,
             PolicyVerdict::Action::Block);

    // Session allowance exempts.
    NavigationPolicy::allowHttpForHost(QStringLiteral("ex.example"), false);
    QVERIFY(NavigationPolicy::isHttpAllowedHost(QStringLiteral("ex.example")));
    QCOMPARE(NavigationPolicy::evaluate(makeRequest(url, 0)).action,
             PolicyVerdict::Action::Allow);
    NavigationPolicy::clearHttpAllowance(QStringLiteral("ex.example"));

    // Persisted exception via snapshot exempts too.
    NavigationPolicy::markDowngraded(QStringLiteral("ex.example"),
                                     QStringLiteral("test"));
    auto snap = defaultSnapshot();
    snap.httpsOnlyExceptions = {QStringLiteral("ex.example")};
    NavigationPolicy::loadSnapshot(snap);
    QCOMPARE(NavigationPolicy::evaluate(makeRequest(url, 0)).action,
             PolicyVerdict::Action::Allow);
    QVERIFY(NavigationPolicy::httpExceptionHosts().contains(
        QStringLiteral("ex.example")));
}

void tst_NavigationPolicy::scriptBlockTiers()
{
    auto scriptReq = [](const QString &fp, bool allowed = false) {
        auto r = makeRequest(QStringLiteral("https://cdn.example/s.js"), 3, fp);
        r.scriptAllowed = allowed;
        return r;
    };

    // Standard tier: scripts on http pages are fine.
    QCOMPARE(NavigationPolicy::evaluate(scriptReq(
                 QStringLiteral("http://site.example/"))).action,
             PolicyVerdict::Action::Allow);

    auto snap = defaultSnapshot();
    snap.securityLevel = 1;   // Safer
    NavigationPolicy::loadSnapshot(snap);

    // http page scripts die; loopback + https pages keep theirs.
    QCOMPARE(NavigationPolicy::evaluate(scriptReq(
                 QStringLiteral("http://site.example/"))).action,
             PolicyVerdict::Action::Block);
    QCOMPARE(NavigationPolicy::evaluate(scriptReq(
                 QStringLiteral("http://127.0.0.1:8000/"))).action,
             PolicyVerdict::Action::Allow);
    QCOMPARE(NavigationPolicy::evaluate(scriptReq(
                 QStringLiteral("https://site.example/"))).action,
             PolicyVerdict::Action::Allow);
    // JSCTL grant wins.
    QCOMPARE(NavigationPolicy::evaluate(
                 scriptReq(QStringLiteral("http://site.example/"), true)).action,
             PolicyVerdict::Action::Allow);
    // Non-script types untouched.
    QCOMPARE(NavigationPolicy::evaluate(
                 makeRequest(QStringLiteral("https://cdn.example/i.png"), 4,
                             QStringLiteral("http://site.example/"))).action,
             PolicyVerdict::Action::Allow);
}

void tst_NavigationPolicy::refererLevels()
{
    auto reqWithRef = [](int level, const QString &ref, const QString &target) {
        auto snap = NavigationPolicy::Snapshot();
        snap.refererPolicy = level;
        NavigationPolicy::loadSnapshot(snap);
        auto r = makeRequest(target, 4);
        r.headers.append(qMakePair(QByteArray("Referer"), ref.toUtf8()));
        return NavigationPolicy::evaluate(r);
    };

    // Trimmed: cross-site -> target's own origin.
    auto v = reqWithRef(1, QStringLiteral("https://a.example/deep?x=1"),
                        QStringLiteral("https://b.example/x"));
    QVERIFY(v.refererSet);
    QCOMPARE(v.refererValue, QByteArray("https://b.example/"));
    // Trimmed: same-site -> source origin.
    v = reqWithRef(1, QStringLiteral("https://a.example/deep?x=1"),
                   QStringLiteral("https://sub.a.example/x"));
    QCOMPARE(v.refererValue, QByteArray("https://a.example/"));
    // Strict cross-site -> gone.
    v = reqWithRef(2, QStringLiteral("https://a.example/p"),
                   QStringLiteral("https://b.example/x"));
    QVERIFY(v.refererSet);
    QCOMPARE(v.refererValue, QByteArray());
    // https -> http downgrade loses the header at every level > 0.
    v = reqWithRef(1, QStringLiteral("https://a.example/p"),
                   QStringLiteral("http://b.example/x"));
    QCOMPARE(v.refererValue, QByteArray());
    // Never -> gone even same-site.
    v = reqWithRef(3, QStringLiteral("https://a.example/p"),
                   QStringLiteral("https://a.example/x"));
    QCOMPARE(v.refererValue, QByteArray());
    // EngineDefault -> keep.
    v = reqWithRef(0, QStringLiteral("https://a.example/p?x=1"),
                   QStringLiteral("https://b.example/x"));
    QVERIFY(!v.refererSet);
    // No header -> never synthesized.
    auto snap = defaultSnapshot();
    NavigationPolicy::loadSnapshot(snap);
    v = NavigationPolicy::evaluate(
        makeRequest(QStringLiteral("https://b.example/x"), 4));
    QVERIFY(!v.refererSet);
}

void tst_NavigationPolicy::torModeUpgradesEverythingButOnion()
{
    auto torReq = [](const QString &url, int rtype) {
        auto r = makeRequest(url, rtype);
        r.torMode = true;
        r.minRefererLevel = 1;
        return r;
    };
    // Clearnet http on ANY resource type upgrades.
    auto v = NavigationPolicy::evaluate(
        torReq(QStringLiteral("http://example.com/i.png"), 4));
    QCOMPARE(v.action, PolicyVerdict::Action::Redirect);
    QCOMPARE(v.redirectUrl.toString(),
             QStringLiteral("https://example.com/i.png"));
    // .onion http stays.
    v = NavigationPolicy::evaluate(
        torReq(QStringLiteral("http://abc.onion/i.png"), 4));
    QCOMPARE(v.action, PolicyVerdict::Action::Allow);
    // At least Trimmed referer even under EngineDefault.
    auto snap = defaultSnapshot();
    snap.refererPolicy = 0;
    NavigationPolicy::loadSnapshot(snap);
    auto req = torReq(QStringLiteral("https://b.example/x"), 4);
    req.headers.append(qMakePair(QByteArray("Referer"),
                                 QByteArray("https://a.example/p")));
    v = NavigationPolicy::evaluate(req);
    QVERIFY(v.refererSet);
    QCOMPARE(v.refererValue, QByteArray("https://b.example/"));
}

void tst_NavigationPolicy::cookieGateMatrix()
{
    auto gate = [](int accept, bool b3p, bool third, const QString &host,
                   const QStringList &block = {},
                   const QStringList &allow = {},
                   const QStringList &session = {}) {
        CookieGateInput in;
        in.host = host;
        in.thirdParty = third;
        in.blockThirdParty = b3p;
        in.acceptPolicy = accept;
        in.block = block;
        in.allow = allow;
        in.allowForSession = session;
        return NavigationPolicy::cookieFilter(in);
    };
    // AcceptAlways / AcceptNever / FirstPartyOnly.
    QVERIFY(gate(0, false, false, QStringLiteral("a.com")));
    QVERIFY(!gate(1, false, false, QStringLiteral("a.com")));
    QVERIFY(gate(2, false, false, QStringLiteral("a.com")));
    QVERIFY(!gate(2, false, true, QStringLiteral("a.com")));
    // Third-party toggle rejects only third parties.
    QVERIFY(!gate(0, true, true, QStringLiteral("a.com")));
    QVERIFY(gate(0, true, false, QStringLiteral("a.com")));
    // Block list beats everything; allow lists beat the 3p rule.
    QVERIFY(!gate(0, false, false, QStringLiteral("a.com"),
                  {QStringLiteral("a.com")}));
    QVERIFY(gate(1, true, true, QStringLiteral("a.com"), {},
                 {QStringLiteral("a.com")}));
    QVERIFY(gate(1, true, true, QStringLiteral("a.com"), {}, {},
                 {QStringLiteral(".a.com")}));
    // Under a blocked parent the allow list does not rescue.
    QVERIFY(!gate(1, true, true, QStringLiteral("x.a.com"),
                  {QStringLiteral("a.com")}, {QStringLiteral("x.a.com")}));
    // Suffix rules match subdomains, not siblings.
    QVERIFY(gate(2, false, true, QStringLiteral("x.a.com"), {},
                 {QStringLiteral("a.com")}));
    QVERIFY(!gate(2, false, true, QStringLiteral("xa.com"), {},
                  {QStringLiteral("a.com")}));
}

void tst_NavigationPolicy::granularApiMatchesInterceptors()
{
    // The WebEngine-facing statics must answer identically to the
    // engine-neutral surface — the 1:1 delegation proof.
    const QUrl url(QStringLiteral("http://fails.example/"));
    NavigationPolicy::markDowngraded(QStringLiteral("fails.example"),
                                     QStringLiteral("test"));

    QCOMPARE(PrivacyRequestInterceptor::isUpgradeCandidate(url, QStringLiteral("test")),
             NavigationPolicy::isUpgradeCandidate(url, QStringLiteral("test")));
    QCOMPARE(PrivacyRequestInterceptor::shouldWarnHttp(url, QStringLiteral("test")),
             NavigationPolicy::shouldWarnHttp(url, QStringLiteral("test")));
    QCOMPARE(PrivacyRequestInterceptor::shouldWarnFormPost(url, QStringLiteral("test")),
             NavigationPolicy::shouldWarnFormPost(url, QStringLiteral("test")));
    QCOMPARE(PrivacyRequestInterceptor::isPrivateOrLocalHost(QStringLiteral("10.0.0.1")),
             NavigationPolicy::isPrivateOrLocalHost(QStringLiteral("10.0.0.1")));
    QCOMPARE(PrivacyRequestInterceptor::isPrivateOrLocalHost(QStringLiteral("example.com")),
             NavigationPolicy::isPrivateOrLocalHost(QStringLiteral("example.com")));
    QCOMPARE(PrivacyRequestInterceptor::referrerMetaValue(1),
             NavigationPolicy::referrerMetaValue(1));
    QCOMPARE(PrivacyRequestInterceptor::rewrittenReferer(
                 1, QUrl(QStringLiteral("https://a.example/p")),
                 QUrl(QStringLiteral("https://b.example/x"))),
             NavigationPolicy::rewrittenReferer(
                 1, QUrl(QStringLiteral("https://a.example/p")),
                 QUrl(QStringLiteral("https://b.example/x"))));
    QCOMPARE(PrivacyRequestInterceptor::isDowngraded(
                 QStringLiteral("fails.example"), QStringLiteral("test")),
             NavigationPolicy::isDowngraded(
                 QStringLiteral("fails.example"), QStringLiteral("test")));
    QCOMPARE(PrivacyRequestInterceptor::httpsFirstEnabled(),
             NavigationPolicy::httpsFirstEnabled());
    QCOMPARE(PrivacyRequestInterceptor::httpsOnlyEnabled(),
             NavigationPolicy::httpsOnlyEnabled());
    QCOMPARE(PrivacyRequestInterceptor::blockPingsEnabled(),
             NavigationPolicy::blockPingsEnabled());
}

void tst_NavigationPolicy::failureImpliesDowngradeTable()
{
    QVERIFY(!NavigationPolicy::failureImpliesDowngrade(1, -101));
    QVERIFY(!NavigationPolicy::failureImpliesDowngrade(2, -105));
    QVERIFY(!NavigationPolicy::failureImpliesDowngrade(2, -106));
    QVERIFY(!NavigationPolicy::failureImpliesDowngrade(2, -111));
    QVERIFY(!NavigationPolicy::failureImpliesDowngrade(2, -110));
    QVERIFY(!NavigationPolicy::failureImpliesDowngrade(2, -139));
    QVERIFY(NavigationPolicy::failureImpliesDowngrade(2, -101));
    QVERIFY(NavigationPolicy::failureImpliesDowngrade(2, -102));
    QVERIFY(NavigationPolicy::failureImpliesDowngrade(2, -118));
}

QTEST_MAIN(tst_NavigationPolicy)
#include "tst_navigationpolicy.moc"
