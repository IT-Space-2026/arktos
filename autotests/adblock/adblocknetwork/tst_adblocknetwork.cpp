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

#include <qtest.h>

#include "adblocknetwork.h"
#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "adblockrule.h"

#include <qdir.h>
#include <qelapsedtimer.h>
#include <qfile.h>
#include <qnetworkrequest.h>
#include <qtextstream.h>

class tst_AdBlockNetwork : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void adblocknetwork_data();
    void adblocknetwork();

    void data();

    void enabled_data();
    void enabled();

    void block_data();
    void block();

    void differential();
    void indexScales();
};

// Subclass that exposes the protected functions.
class SubAdBlockNetwork : public AdBlockNetwork
{
public:

};

// This will be called before the first test function is executed.
// It is only called once.
void tst_AdBlockNetwork::initTestCase()
{
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_AdBlockNetwork::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_AdBlockNetwork::init()
{
}

// This will be called after every test function.
void tst_AdBlockNetwork::cleanup()
{
    AdBlockManager *manager = AdBlockManager::instance();
    QList<AdBlockSubscription*> list = manager->subscriptions();
    for (AdBlockSubscription *s : list)
        manager->removeSubscription(s);
}

void tst_AdBlockNetwork::adblocknetwork_data()
{
}

void tst_AdBlockNetwork::adblocknetwork()
{
    SubAdBlockNetwork network;
    QCOMPARE(network.shouldBlock(QUrl()), false);
}

void tst_AdBlockNetwork::data()
{
    SubAdBlockNetwork network;

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);

    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), manager);
    subscription->setEnabled(true);
    manager->addSubscription(subscription);

    AdBlockRule rule("/");
    rule.setEnabled(true);
    subscription->addRule(rule);

    network.rebuildRules();
    QVERIFY(!network.shouldBlock(QUrl("data://foobar")));
}

void tst_AdBlockNetwork::enabled_data()
{
    QTest::addColumn<bool>("enableManager");
    QTest::addColumn<bool>("enableSubscription");
    QTest::addColumn<bool>("enableRule");
    QTest::addColumn<bool>("block");
    QTest::newRow("null") << true << true << true << true;
    QTest::newRow("m") << false << true << true << false;
    QTest::newRow("s") << true << false << true << false;
    QTest::newRow("r") << true << true << false << false;
}

void tst_AdBlockNetwork::enabled()
{
    QFETCH(bool, enableManager);
    QFETCH(bool, enableSubscription);
    QFETCH(bool, enableRule);
    QFETCH(bool, block);

    SubAdBlockNetwork network;

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(enableManager);

    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), manager);
    subscription->setEnabled(enableSubscription);
    manager->addSubscription(subscription);

    AdBlockRule rule("/");
    rule.setEnabled(enableRule);
    subscription->addRule(rule);

    network.rebuildRules();
    QCOMPARE(network.shouldBlock(QUrl("http://www.google.com")), block);
}

// check that block block and !block blocks other sites
void tst_AdBlockNetwork::block_data()
{
    QTest::addColumn<QString>("ruleList");
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<bool>("block");

    QTest::newRow("null") << QString()
                          << QUrl()
                          << false;

    QUrl google("http://www.google.com");
    QTest::newRow("google") << QString("/")
                          << google
                          << true;

    // defining exception rules
    QTest::newRow("exception0") << QString("@@advice,advice")
                                << QUrl("http://example.com/advice.html")
                                << false;
    QTest::newRow("exception1") << QString("@@|http://example.com")
                                << QUrl("http://example.com/advice.html")
                                << false;
    QTest::newRow("exception2") << QString("@@http://example.com")
                                << QUrl("http://example.com/advice.html")
                                << false;

    QTest::newRow("order0") << QString("advice,@@advice")
                            << QUrl("http://example.com/advice.html")
                            << false;
}

// public QNetworkReply *block(QNetworkRequest const &request)
void tst_AdBlockNetwork::block()
{
    QFETCH(QString, ruleList);
    QFETCH(QUrl, url);
    QFETCH(bool, block);

    SubAdBlockNetwork network;

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);

    AdBlockSubscription *subscription = new AdBlockSubscription(QUrl(), manager);
    subscription->setEnabled(true);
    manager->addSubscription(subscription);

    QStringList rules = ruleList.split(",");
    for (const QString &rule : rules)
        subscription->addRule(AdBlockRule(rule));

    network.rebuildRules();
    QCOMPARE(network.shouldBlock(url), block);
}

static bool sameDecision(const AdBlockDecision &a, const AdBlockDecision &b)
{
    return a.action == b.action
        && a.redirectResource == b.redirectResource
        && a.redirectUrl == b.redirectUrl
        && a.removeParams == b.removeParams;
}

// A rule corpus exercising every path of the matcher: anchors,
// separators, wildcards, short/no-token patterns, regex rules, type
// and party options, domain=/denyallow=, exceptions incl. page-level
// modifiers, $important, $redirect, $removeparam and match-case.
static QStringList corpusRules()
{
    QStringList rules;
    rules << QLatin1String("||doubleclick.net^")
          << QLatin1String("@@||doubleclick.net^$subdocument")
          << QLatin1String("banner")
          << QLatin1String("/banners/*")
          << QLatin1String("|http://ads.example.com/")
          << QLatin1String("ad*$image")
          << QLatin1String(".js?")
          << QLatin1String("@@||good.example^$document")
          << QLatin1String("@@||genblock.example^$genericblock")
          << QLatin1String("@@||elemhide.example^$elemhide")
          << QLatin1String("||tracker.test^$third-party")
          << QLatin1String("||same.test^$first-party")
          << QLatin1String("||restricted.test^$domain=example.com|~sub.example.com")
          << QLatin1String("||deny.test^$denyallow=allowed.test")
          << QLatin1String("||imp.test^$important")
          << QLatin1String("@@||imp.test^")
          << QLatin1String("||redir.test^$redirect=noop.js")
          << QLatin1String("||param.test^$removeparam=utm_source")
          << QLatin1String("@@||param.test^$removeparam=utm_campaign")
          << QLatin1String("||paramall.test^$removeparam")
          << QLatin1String("/re(ge)x[0-9]+/")
          << QLatin1String("||CaseSensitive.Test^$match-case")
          << QLatin1String("*wild*")
          << QLatin1String("^|tricky")
          << QLatin1String(".js")
          << QLatin1String("ad")
          << QLatin1String("||a-b_c.example^")
          << QLatin1String("%20track%20")
          << QLatin1String("||utf8.example/üñí^")
          << QLatin1String("@@||exc-first.test^")
          << QLatin1String("||exc-first.test^")
          << QLatin1String("||ws.test^$websocket")
          << QLatin1String("||css.test^$stylesheet");
    // Volume filler so candidates and generic buckets interleave.
    for (int i = 0; i < 400; ++i) {
        rules << QString(QLatin1String("||host%1.tracker%2.test^$image,script"))
                     .arg(i).arg(i % 37);
        rules << QString(QLatin1String("/generated/path%1/ad_%2.js^"))
                     .arg(i).arg(i % 7);
    }
    return rules;
}

static QList<QPair<QUrl, QUrl> > corpusUrls()
{
    QList<QPair<QUrl, QUrl> > urls;
    const QStringList targets = {
        QLatin1String("http://doubleclick.net/ad.js"),
        QLatin1String("http://sub.doubleclick.net/ad.js"),
        QLatin1String("http://doubleclick.net.evil.test/ad.js"),
        QLatin1String("http://example.com/banners/leader.gif"),
        QLatin1String("http://ads.example.com/"),
        QLatin1String("http://example.com/ad123"),
        QLatin1String("https://example.com/lib.js?v=1"),
        QLatin1String("http://tracker.test/p.gif"),
        QLatin1String("http://same.test/p.gif"),
        QLatin1String("http://restricted.test/x.js"),
        QLatin1String("http://deny.test/x.js"),
        QLatin1String("http://imp.test/x.js"),
        QLatin1String("http://redir.test/x.js"),
        QLatin1String("http://param.test/x?utm_source=a&utm_campaign=b"),
        QLatin1String("http://paramall.test/x?a=1&b=2"),
        QLatin1String("http://example.com/regex5"),
        QLatin1String("http://CaseSensitive.Test/x"),
        QLatin1String("http://casesensitive.test/x"),
        QLatin1String("http://example.com/wildcard"),
        QLatin1String("http://host7.tracker3.test/p.js"),
        QLatin1String("http://example.com/generated/path9/ad_3.js"),
        QLatin1String("http://ws.test/socket"),
        QLatin1String("http://css.test/site.css"),
        QLatin1String("http://unrelated.example/page"),
        QLatin1String("data:image/png;base64,AAAA"),
        QLatin1String("http://example.com/a%20track%20b"),
        QLatin1String("http://utf8.example/x"),
        QLatin1String("https://a-b_c.example/y")
    };
    const QStringList firstParties = {
        QString(), // treated as request's own party
        QLatin1String("http://example.com/"),
        QLatin1String("http://sub.example.com/"),
        QLatin1String("http://good.example/"),
        QLatin1String("http://genblock.example/"),
        QLatin1String("http://other.test/")
    };
    for (const QString &target : targets) {
        for (const QString &firstParty : firstParties) {
            urls.append(qMakePair(QUrl(target), QUrl(firstParty)));
        }
    }
    return urls;
}

// match() must decide identically to the unindexed linear reference
// on every request, including which subscription's exception wins.
void tst_AdBlockNetwork::differential()
{
    SubAdBlockNetwork network;

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);

    AdBlockSubscription *first = new AdBlockSubscription(QUrl(), manager);
    first->setEnabled(true);
    manager->addSubscription(first);
    // Second subscription shifts ordering: its rules only apply when
    // the first produces no decision.
    AdBlockSubscription *second = new AdBlockSubscription(QUrl(), manager);
    second->setEnabled(true);
    manager->addSubscription(second);

    for (const QString &line : corpusRules())
        first->addRule(AdBlockRule(line));
    second->addRule(AdBlockRule(QLatin1String("||second.test^")));
    second->addRule(AdBlockRule(QLatin1String("@@||second-exc.test^")));
    second->addRule(AdBlockRule(QLatin1String("banner")));

    network.rebuildRules();

    const QList<QPair<QUrl, QUrl> > urls = corpusUrls();
    const int resourceTypes[] = { -1, 0, 2, 3, 4, 13, 254 };
    int checks = 0;
#if defined(ARORA_ADBLOCK_RUST)
    // Under the Rust engine match() is rust-primary with a native
    // residual on allows plus shared document-unbreak handling (see
    // AdBlockNetwork::match) — the composite must decide identically
    // to the linear reference on the whole corpus.  Divergences are
    // collected first so a failure dumps the full inventory.
    QStringList divergences;
    for (const QPair<QUrl, QUrl> &pair : urls) {
        for (const int type : resourceTypes) {
            const AdBlockDecision indexed =
                network.match(pair.first, pair.second, type);
            const AdBlockDecision linear =
                network.matchLinear(pair.first, pair.second, type);
            if (!sameDecision(indexed, linear))
                divergences.append(QString(QLatin1String(
                    "url=%1 firstParty=%2 type=%3 indexed=%4 linear=%5"))
                    .arg(pair.first.toString(), pair.second.toString())
                    .arg(type).arg(indexed.action).arg(linear.action));
            ++checks;
        }
    }
    QVERIFY2(divergences.isEmpty(),
             qPrintable(QLatin1String("divergences:\n")
                        + divergences.join(QLatin1String("\n"))));
#else
    for (const QPair<QUrl, QUrl> &pair : urls) {
        for (const int type : resourceTypes) {
            const AdBlockDecision indexed =
                network.match(pair.first, pair.second, type);
            const AdBlockDecision linear =
                network.matchLinear(pair.first, pair.second, type);
            QVERIFY2(sameDecision(indexed, linear),
                     qPrintable(QString(QLatin1String(
                         "url=%1 firstParty=%2 type=%3 indexed=%4 linear=%5"))
                        .arg(pair.first.toString(), pair.second.toString())
                        .arg(type).arg(indexed.action).arg(linear.action)));
            ++checks;
        }
    }
#endif
    // Ordering across subscriptions: when the first subscription
    // produces no decision the second one's rules still apply — an
    // exception there beats the block there.
    QCOMPARE(network.shouldBlock(QUrl(QLatin1String("http://second.test/x"))),
             true);
    QCOMPARE(network.shouldBlock(QUrl(QLatin1String("http://second-exc.test/x"))),
             false);
    QVERIFY(checks > 500);
}

// EasyList-scale proof: ~60k block rules, a realistic ~120-request
// page mix.  The indexed matcher must be at least 10x faster than
// the linear reference; the measured numbers are printed for Notes.
void tst_AdBlockNetwork::indexScales()
{
    SubAdBlockNetwork network;

    AdBlockManager *manager = AdBlockManager::instance();
    manager->setEnabled(true);

    // Synthetic EasyList-shaped corpus written to a file and loaded
    // like a real subscription: mostly untyped ||domain^ rules, a
    // minority of type-restricted/path rules, ~2% untokenizable
    // (regex + short-token) rules and some exceptions — close to the
    // real list's shape so the generic bucket is honestly sized.
    const int ruleCount = 60000;
    const QString listPath = QDir::temp().filePath(
        QLatin1String("arora-perf02-easylist.txt"));
    QFile listFile(listPath);
    QVERIFY(listFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QTextStream out(&listFile);
    out << QLatin1String("[Adblock Plus 0.7.1]") << Qt::endl;
    for (int i = 0; i < ruleCount; ++i) {
        QString line;
        if (i % 97 == 0)
            line = QString(QLatin1String("/regpat%1/[a-z]*/")).arg(i);
        else if (i % 89 == 0)
            line = QString(QLatin1String("t%1")).arg(i % 10);
        else if (i % 11 == 0)
            line = QString(QLatin1String("/track/%1/pixel%2.gif^"))
                       .arg(i).arg(i % 13);
        else if (i % 7 == 0)
            line = QString(QLatin1String("||adsrv%1.example%2.test^$image,script"))
                       .arg(i).arg(i % 211);
        else
            line = QString(QLatin1String("||adsrv%1.example%2.test^"))
                       .arg(i).arg(i % 211);
        out << line << Qt::endl;
        if (i % 503 == 0) {
            out << QString(QLatin1String("@@||adsrv%1.example%2.test^$image"))
                       .arg(i).arg(i % 211)
                << Qt::endl;
        }
    }
    out.flush();
    listFile.close();

    // Subscribe via the abp: URL path so the file loads in one pass.
    QUrl subscribeUrl(QString(QLatin1String(
        "abp:subscribe?location=%1&title=perf02"))
        .arg(QString::fromUtf8(
            QUrl::fromLocalFile(listPath).toEncoded())));
    AdBlockSubscription *subscription =
        new AdBlockSubscription(subscribeUrl, manager);
    manager->addSubscription(subscription);
    network.rebuildRules();

    // ~120 requests: mostly unmatched hosts (the common case), some
    // matching, plus query-heavy and long URLs.
    QList<QUrl> requests;
    for (int i = 0; i < 90; ++i) {
        requests.append(QUrl(QString(
            QLatin1String("https://cdn%1.pub%2.example/static/app.js?v=%3"))
                .arg(i).arg(i % 17).arg(i)));
    }
    for (int i = 0; i < 20; ++i) {
        requests.append(QUrl(QString(
            QLatin1String("http://adsrv%1.example%2.test/pixel.gif?r=%3"))
                .arg(i * 2500).arg((i * 2500) % 211).arg(i)));
    }
    for (int i = 0; i < 10; ++i) {
        QString longUrl = QLatin1String("https://deep.example/");
        for (int d = 0; d < 20; ++d)
            longUrl += QString(QLatin1String("dir%1/")).arg(d);
        longUrl += QString(QLatin1String("file%1.js?a=1&b=2&c=3")).arg(i);
        requests.append(QUrl(longUrl));
    }

    // Realistic resource type (script) so type-masked rules evaluate
    // their regexes instead of dying on the type check.
    const int scriptType = 3;

    // Warm the indexed path once so its measurement excludes
    // first-use effects (hash seeding).
    for (const QUrl &u : requests)
        network.match(u, QUrl(), scriptType);

    // The linear reference scans all ~60k rules per request (tens of
    // ms each at this scale), so a small sample is enough for a
    // stable per-request figure.
    const int linearSample = qMin(5, requests.count());
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < linearSample; ++i)
        network.matchLinear(requests.at(i), QUrl(), scriptType);
    const qint64 linearNs = timer.nsecsElapsed() / linearSample;

    timer.restart();
    for (const QUrl &u : requests)
        network.match(u, QUrl(), scriptType);
    const qint64 indexedNs = timer.nsecsElapsed() / requests.count();

    qInfo() << "indexScales:" << ruleCount << "rules — linear"
            << linearNs / 1000.0 << "us/req;"
            << "indexed" << indexedNs / 1000.0 << "us/req;"
            << "speedup"
            << (indexedNs ? double(linearNs) / double(indexedNs) : -1.0)
            << "x";

    QVERIFY2(indexedNs * 10 < linearNs,
             qPrintable(QString(QLatin1String(
                 "indexed %1ns/req not >=10x faster than linear %2ns/req"))
                .arg(indexedNs).arg(linearNs)));

    QFile::remove(listPath);
}

QTEST_MAIN(tst_AdBlockNetwork)
#include "tst_adblocknetwork.moc"

