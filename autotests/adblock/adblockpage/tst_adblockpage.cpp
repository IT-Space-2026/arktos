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

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

#include <qwebengineview.h>
#include <qwebenginepage.h>
#include <qelapsedtimer.h>
#include <qdebug.h>
#include <qdir.h>

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
    page.applyRulesToPage((QWebEnginePage*)0);
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
    page.applyRulesToPage(view.page());

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

QTEST_MAIN(tst_AdBlockPage)
#include "tst_adblockpage.moc"
