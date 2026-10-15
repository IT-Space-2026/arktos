/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// TST01: ported to QtWebEngine.  Cosmetic rules now inject a <style>
// element asynchronously through runJavaScript, so the test counts
// visible elements via computed style instead of scanning the HTML.

#include <qtest.h>
#include <qsignalspy.h>
#include <qtry.h>

#include "adblockpage.h"

#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "adblockrule.h"
#include "webenginebackend.h"

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

#include <qwebengineview.h>
#include <qwebenginepage.h>
#include <qelapsedtimer.h>
#include <qdebug.h>
#include <qdir.h>
#include <qfile.h>

#include "engineinterface.h"

class tst_AdBlockPage : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void adblockpage_data();
    void adblockpage();

    void applyRulesToPage_data();
    void applyRulesToPage();

    void cosmeticScript_data();
    void cosmeticScript();
    void cosmeticScriptManagerDisabled();
    void scheduleRules();
};

// Subclass that exposes the protected functions.
class SubAdBlockPage : public AdBlockPage
{
public:

};

// Counts the tofu-bearing elements that are still visible; all seven
// elements in test.html match this selector set.
static const char *visibleCountScript =
    "(function(){"
    "var els=document.querySelectorAll('div,textad,table');"
    "var n=0;"
    "for(var i=0;i<els.length;i++)"
    "  if(getComputedStyle(els[i]).display!=='none')n++;"
    "return n;})()";

// This will be called before the first test function is executed.
// It is only called once.
void tst_AdBlockPage::initTestCase()
{
    // COV08: the cosmetic payload tests assert on the NATIVE builder —
    // the Rust engine (the default pick) would otherwise answer first
    // for hosted urls and shadow the code under test.
    AdBlockManager::instance()->setEngine(AdBlockManager::NativeEngine);
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_AdBlockPage::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_AdBlockPage::init()
{
#if defined(ARORA_RUSTCORE)
    // A whitelisted site gets no cosmetic filtering — clear stale rows.
    SiteDecisionStore::clear(SiteDecisionStore::KindAdBlock);
#endif
}

// This will be called after every test function.
void tst_AdBlockPage::cleanup()
{
    AdBlockManager *manager = AdBlockManager::instance();
    QList<AdBlockSubscription*> list = manager->subscriptions();
    for (AdBlockSubscription *s : list)
        manager->removeSubscription(s);
}

void tst_AdBlockPage::adblockpage_data()
{
}

void tst_AdBlockPage::adblockpage()
{
    SubAdBlockPage page;
    page.applyRulesToPage((Engine::Page*)nullptr);
}

void tst_AdBlockPage::applyRulesToPage_data()
{
    QTest::addColumn<QString>("ruleList");
    QTest::addColumn<int>("count");

    int start = 7;
    QTest::newRow("null") << QString() << start;

    //Examples taken from http://adblockplus.org/en/filters

    QTest::newRow("basic-0") << QString("##div.textad") << start - 1;
    QTest::newRow("basic-1") << QString("##div#sponsorad") << start - 1;
    QTest::newRow("basic-2") << QString("##*#sponsorad") << start - 1;
    QTest::newRow("basic-3") << QString("##textad") << start -1;

    // Attribute selectors
    QTest::newRow("attribute-0") << QString("##table[width=\"80%\"]") << start - 2;
    QTest::newRow("attribute-1") << QString("##table[width=\"80%\"][bgcolor=\"white\"]") << start - 1;
    QTest::newRow("attribute-2") << QString("##div[title*=\"adv\"]") << start - 2;
    QTest::newRow("attribute-3") << QString("##div[title^=\"adv\"][title$=\"ert\"]") << start - 1;

    // Advanced selectors
}

// public void applyRulesToPage(QWebEnginePage *page)
void tst_AdBlockPage::applyRulesToPage()
{
    QFETCH(QString, ruleList);
    QFETCH(int, count);

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);

    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), manager);
    subscription->setEnabled(true);
    manager->addSubscription(subscription);

    QStringList rules = ruleList.split(",");
    for (const QString &rule : rules)
        subscription->addRule(AdBlockRule(rule));

    QWebEngineView view;
    QSignalSpy spy1(view.page(), SIGNAL(loadFinished(bool)));
    view.load(QUrl::fromLocalFile(QDir::currentPath() + "/test.html"));
    QTRY_VERIFY_WITH_TIMEOUT(spy1.count() >= 1, 15000);

    SubAdBlockPage page;
    page.applyRulesToPage(WebEnginePageAdapter::forPage(view.page()));

    // The <style> injection is asynchronous; keep evaluating until the
    // visible-element count settles or the deadline passes.
    int visible = -1;
    QElapsedTimer deadline;
    deadline.start();
    do {
        bool jsDone = false;
        int result = -1;
        view.page()->runJavaScript(QString::fromLatin1(visibleCountScript),
                [&](const QVariant &v) { result = v.toInt(); jsDone = true; });
        QTRY_VERIFY_WITH_TIMEOUT(jsDone, 15000);
        visible = result;
        if (visible == count)
            break;
        QTest::qWait(100);
    } while (deadline.elapsed() < 10000);
    QCOMPARE(visible, count);
}

// Adds an enabled subscription carrying the given filter list; the
// manager owns it until cleanup() removes it.
static AdBlockSubscription *addSubscriptionRules(AdBlockManager *manager,
                                               const QStringList &rules)
{
    AdBlockSubscription *subscription =
        new AdBlockSubscription(QUrl(), manager);
    subscription->setEnabled(true);
    manager->addSubscription(subscription);
    for (const QString &rule : rules)
        subscription->addRule(AdBlockRule(rule));
    return subscription;
}

// COV08: drive cosmeticScriptForUrl (the AUTOTESTS seam in
// adblockpage.h) across the whole rule grammar — cosmetic selectors,
// domain scoping, exceptions, ABP pseudo translation, procedural ops
// and scriptlets — asserting on the generated payload text.
void tst_AdBlockPage::cosmeticScript_data()
{
    QTest::addColumn<QStringList>("rules");
    QTest::addColumn<QString>("url");
    QTest::addColumn<QStringList>("present");
    QTest::addColumn<QStringList>("absent");

    QTest::newRow("generic-css")
        << QStringList{"##.ad"}
        << QString("http://site.test/")
        << QStringList{"arora-adblock", ":is(.ad)",
                       "display: none !important"}
        << QStringList{"PROCS"};

    QTest::newRow("domain-match")
        << QStringList{"ok.com##.ad"}
        << QString("http://ok.com/")
        << QStringList{":is(.ad)"}
        << QStringList{};
    QTest::newRow("domain-subdomain")
        << QStringList{"ok.com##.ad"}
        << QString("http://a.ok.com/")
        << QStringList{":is(.ad)"}
        << QStringList{};
    QTest::newRow("domain-miss")
        << QStringList{"ok.com##.ad"}
        << QString("http://other.test/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("domain-negated-hit")
        << QStringList{"~bad.com##.ad"}
        << QString("http://bad.com/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("domain-negated-miss")
        << QStringList{"~bad.com##.ad"}
        << QString("http://good.com/")
        << QStringList{":is(.ad)"}
        << QStringList{};
    QTest::newRow("domain-excluded-subdomain")
        << QStringList{"good.com,~sub.good.com##.ad"}
        << QString("http://sub.good.com/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("domain-excluded-sibling")
        << QStringList{"good.com,~sub.good.com##.ad"}
        << QString("http://www.good.com/")
        << QStringList{":is(.ad)"}
        << QStringList{};

    // PIP03: a hide rule targeting a document-root element (##body,
    // ##html, ##*, ##:root) blanks the whole page — every element's
    // layout box goes to zero (this broke PiP's fixture: the video
    // and its button became unclickable, hit-tests landed on the
    // HTML root).  Dropped whether the rule is generic or scoped;
    // sibling selectors in the same list still apply.
    QTest::newRow("root-hide-body-dropped")
        << QStringList{"##body"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{"body", ":is("};
    QTest::newRow("root-hide-scoped-dropped")
        << QStringList{"site.test##body"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{"body", ":is("};
    QTest::newRow("root-hide-variants-dropped")
        << QStringList{"##html", "##*", "##:root", "##html body",
                       "##html>body", "##BODY"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{":is("};
    QTest::newRow("root-hide-mixed-keeps-rest")
        << QStringList{"##body", "##.ad"}
        << QString("http://site.test/")
        << QStringList{":is(.ad)"}
        << QStringList{"body"};
    QTest::newRow("root-hide-lookalikes-kept")
        << QStringList{"##bodytext", "##.body", "##body .ad",
                       "##body.ad", "##*#sponsorad"}
        << QString("http://site.test/")
        << QStringList{":is(bodytext,.body,body .ad,body.ad,*#sponsorad)"}
        << QStringList{};

    // Cosmetic exceptions remove same-body rules, whether they were
    // parsed before or after the rule they cancel.
    QTest::newRow("exception-after-rule")
        << QStringList{"##.ad", "#@#.ad"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("exception-before-rule")
        << QStringList{"#@#.ad", "##.ad"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{".ad"};

    // Document-level network exceptions.
    QTest::newRow("elemhide")
        << QStringList{"@@||site.test^$elemhide", "##.ad"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("document-exception")
        << QStringList{"@@||site.test^$document", "##.ad"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("generichide-drops-generic")
        << QStringList{"@@||site.test^$generichide", "##.ad"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{".ad"};
    QTest::newRow("generichide-keeps-scoped")
        << QStringList{"@@||site.test^$generichide", "##.gen",
                       "site.test##.scoped"}
        << QString("http://site.test/")
        << QStringList{".scoped"}
        << QStringList{".gen"};

    // ABP extended pseudos translate to Chromium-native selectors.
    QTest::newRow("abp-has")
        << QStringList{"##div:-abp-has(.ad)"}
        << QString("http://site.test/")
        << QStringList{":has(.ad)"}
        << QStringList{":-abp-"};
    QTest::newRow("abp-has-not")
        << QStringList{"##div:-abp-has-not(.ad)"}
        << QString("http://site.test/")
        << QStringList{":not(:has(.ad))"}
        << QStringList{":-abp-"};
    QTest::newRow("abp-if")
        << QStringList{"##div:-abp-if(.ad)"}
        << QString("http://site.test/")
        << QStringList{":has(.ad)"}
        << QStringList{":-abp-"};
    QTest::newRow("abp-if-not")
        << QStringList{"##div:-abp-if-not(.ad)"}
        << QString("http://site.test/")
        << QStringList{":not(:has(.ad))"}
        << QStringList{":-abp-"};
    QTest::newRow("abp-unbalanced-dropped")
        << QStringList{"##div:-abp-has(.ad"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{"display: none"};

    // Procedural operators route through the JS engine payload.
    QTest::newRow("proc-has-text")
        << QStringList{"##div:has-text(banner)"}
        << QString("http://site.test/")
        << QStringList{"PROCS", "has-text", "banner"}
        << QStringList{":is("};
    QTest::newRow("proc-contains-alias")
        << QStringList{"##div:contains(/re/)"}
        << QString("http://site.test/")
        << QStringList{"has-text"}
        << QStringList{};
    QTest::newRow("proc-abp-contains-alias")
        << QStringList{"##div:-abp-contains(x)"}
        << QString("http://site.test/")
        << QStringList{"has-text"}
        << QStringList{};
    QTest::newRow("proc-matches-css")
        << QStringList{"##div:matches-css(color: red)"}
        << QString("http://site.test/")
        << QStringList{"matches-css"}
        << QStringList{};
    QTest::newRow("proc-style")
        << QStringList{"##div:style(display:none)"}
        << QString("http://site.test/")
        << QStringList{"style"}
        << QStringList{};
    QTest::newRow("proc-remove")
        << QStringList{"##div:remove()"}
        << QString("http://site.test/")
        << QStringList{"remove"}
        << QStringList{};
    QTest::newRow("proc-remove-attr")
        << QStringList{"##div:remove-attr(title)"}
        << QString("http://site.test/")
        << QStringList{"remove-attr"}
        << QStringList{};
    QTest::newRow("proc-remove-class")
        << QStringList{"##div:remove-class(c)"}
        << QString("http://site.test/")
        << QStringList{"remove-class"}
        << QStringList{};
    QTest::newRow("proc-upward")
        << QStringList{"##div:upward(2)"}
        << QString("http://site.test/")
        << QStringList{"upward"}
        << QStringList{};
    QTest::newRow("proc-min-text-length")
        << QStringList{"##div:min-text-length(5)"}
        << QString("http://site.test/")
        << QStringList{"min-text-length"}
        << QStringList{};
    QTest::newRow("proc-chained")
        << QStringList{"##div:has-text(x):upward(1)"}
        << QString("http://site.test/")
        << QStringList{"has-text", "upward"}
        << QStringList{};
    QTest::newRow("proc-xpath-dropped")
        << QStringList{"##div:xpath(..)"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{"PROCS"};

    // Scriptlets: +js() form, bare #$# form, #$# css injection, and
    // the #@$# exception that cancels one.
    QTest::newRow("scriptlet-js")
        << QStringList{"##+js(set-constant, foo.bar, true)"}
        << QString("http://site.test/")
        << QStringList{"SCRS", "set-constant"}
        << QStringList{};
    QTest::newRow("scriptlet-bare")
        << QStringList{"x.com#$#hide-if-contains needle .sel"}
        << QString("http://x.com/")
        << QStringList{"SCRS", "hide-if-contains"}
        << QStringList{};
    QTest::newRow("scriptlet-css-injection")
        << QStringList{"#$#.banner { display:none }"}
        << QString("http://site.test/")
        << QStringList{"PROCS", "banner"}
        << QStringList{};
    QTest::newRow("scriptlet-exception")
        << QStringList{"#$#+js(set-constant, x, 1)",
                       "#@$#+js(set-constant, x, 1)"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{"set-constant"};
    QTest::newRow("plus-js-css-body-dropped")
        << QStringList{"##div+js(x)"}
        << QString("http://site.test/")
        << QStringList{}
        << QStringList{"PROCS", ":is("};
}

void tst_AdBlockPage::cosmeticScript()
{
    QFETCH(QStringList, rules);
    QFETCH(QString, url);
    QFETCH(QStringList, present);
    QFETCH(QStringList, absent);

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);
    addSubscriptionRules(manager, rules);

    SubAdBlockPage page;
    const QString script = page.cosmeticScriptForUrl(QUrl(url));
    for (const QString &needle : present)
        QVERIFY2(script.contains(needle),
                 qPrintable(QStringLiteral("missing '%1' in:\n%2")
                            .arg(needle, script)));
    for (const QString &needle : absent)
        QVERIFY2(!script.contains(needle),
                 qPrintable(QStringLiteral("unexpected '%1' in:\n%2")
                            .arg(needle, script)));
    if (present.isEmpty())
        QVERIFY2(script.isEmpty(), qPrintable(script));
}

void tst_AdBlockPage::cosmeticScriptManagerDisabled()
{
    AdBlockManager *manager = AdBlockManager::instance();
    addSubscriptionRules(manager, {QStringLiteral("##.ad")});
    manager->setEnabled(false);
    SubAdBlockPage page;
    QVERIFY(page.cosmeticScriptForUrl(QUrl("http://site.test/"))
            .isEmpty());
    manager->setEnabled(true);
}

// Covers scheduleRulesOnPage + replacePageScript in both directions —
// arming the named user script when the payload is non-empty and
// removing it when no rule applies — plus the ARORA_DEBUG_COSMETIC
// dump path.
void tst_AdBlockPage::scheduleRules()
{
    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);
    addSubscriptionRules(manager, {QStringLiteral("ok.com##.ad")});

    QWebEngineView view;
    Engine::Page *page = WebEnginePageAdapter::forPage(view.page());
    QVERIFY(page);

    const QString dumpPath = QDir::temp().filePath(
        QStringLiteral("arora-cosmetic-dump.js"));
    qputenv("ARORA_DEBUG_COSMETIC", dumpPath.toUtf8());

    AdBlockPage blocker;
    blocker.scheduleRulesOnPage(page, QUrl("http://ok.com/"));

    bool armed = false;
    for (const Engine::Script &script : page->scripts())
        armed |= (script.name == QLatin1String("arora:adblock-cosmetic"));
    QVERIFY(armed);
    QVERIFY(QFile::exists(dumpPath));

    qunsetenv("ARORA_DEBUG_COSMETIC");

    // No matching rule -> empty payload -> the named script comes off.
    blocker.scheduleRulesOnPage(page, QUrl("http://other.test/"));
    armed = false;
    for (const Engine::Script &script : page->scripts())
        armed |= (script.name == QLatin1String("arora:adblock-cosmetic"));
    QVERIFY(!armed);

    blocker.scheduleRulesOnPage(nullptr, QUrl("http://ok.com/"));
}

QTEST_MAIN(tst_AdBlockPage)
#include "tst_adblockpage.moc"
