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
#include <qsignalspy.h>
#include <qtry.h>

#include <adblockmanager.h>
#include <adblocksubscription.h>

#include <qdir.h>
#include <qsettings.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporaryfile.h>
#include <qurlquery.h>

class tst_AdBlockSubscription : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void adblocksubscription_data();
    void adblocksubscription();

    void isEnabled_data();
    void isEnabled();
    void location_data();
    void location();
    void saveRules_data();
    void saveRules();
    void title_data();
    void title();
    void updateNow_data();
    void updateNow();
    void allow_data();
    void allow();
    void block_data();
    void block();
    void addRule();
    void removeRule();
    void hostileList();
    void oversizedList();
    void remoteFetchRequiresConsent();
};

// Subclass that exposes the protected functions.
class SubAdBlockSubscription : public AdBlockSubscription
{
public:
    SubAdBlockSubscription(const QUrl &url = QUrl()) : AdBlockSubscription(url) {}

};

// This will be called before the first test function is executed.
// It is only called once.
void tst_AdBlockSubscription::initTestCase()
{
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_AdBlockSubscription::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_AdBlockSubscription::init()
{
}

// This will be called after every test function.
void tst_AdBlockSubscription::cleanup()
{
}

void tst_AdBlockSubscription::adblocksubscription_data()
{
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QUrl>("location");
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<QDateTime>("lastUpdate");
    QTest::newRow("basic0") << QUrl::fromEncoded("abp:subscribe?location=http://easylist.adblockplus.org/easylist.txt&title=EasyList")
                            << "EasyList"
                            << QUrl("http://easylist.adblockplus.org/easylist.txt")
                            << true
                            << QDateTime();


    QTest::addColumn<QUrl>("url");
    QTest::addColumn<QString>("title");
    QTest::addColumn<QUrl>("location");
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<QDateTime>("lastUpdate");
    QTest::newRow("basic1") << QUrl::fromEncoded("abp:subscribe?location=http://easylist.adblockplus.org/easylist.txt&title=EasyList&enabled=false&lastUpdate=2009-01-16T12%3A10%3A55")
                            << "EasyList"
                            << QUrl("http://easylist.adblockplus.org/easylist.txt")
                            << false
                            << QDateTime(QDate(2009, 1, 16), QTime(12, 10, 55));
}

void tst_AdBlockSubscription::adblocksubscription()
{
    QFETCH(QUrl, url);
    QFETCH(QString, title);
    QFETCH(QUrl, location);
    QFETCH(bool, enabled);
    QFETCH(QDateTime, lastUpdate);

    SubAdBlockSubscription subscription(url);
    QCOMPARE(subscription.title(), title);
    QCOMPARE(subscription.location(), location);
    QCOMPARE(subscription.isEnabled(), enabled);
    QCOMPARE(subscription.lastUpdate(), lastUpdate);
    QVERIFY(subscription.url().isValid());
    //QCOMPARE(subscription.url(), url);
    QCOMPARE(subscription.block(QString()), (AdBlockRule const*)0);
    subscription.saveRules();
    subscription.setEnabled(false);
    subscription.setLocation(QUrl());
    subscription.setTitle(QString());
    subscription.updateNow();
    subscription.allRules();
    subscription.removeRule(-1);
    subscription.removeRule(0);
    subscription.removeRule(1);
    subscription.addRule(AdBlockRule());
}

Q_DECLARE_METATYPE(AdBlockRule const*)
void tst_AdBlockSubscription::allow_data()
{
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<bool>("allow");
    QTest::newRow("block") << QUrl("http://example.com/ads/banner123.gif") << false;
    QTest::newRow("allow") << QUrl("http://example.com/ads/advice.html") << true;
}

// public AdBlockRule const *block(QString const &urlString) const
void tst_AdBlockSubscription::allow()
{
    QFETCH(QUrl, url);
    QFETCH(bool, allow);

    SubAdBlockSubscription subscription;
    subscription.setLocation(QUrl::fromLocalFile(QDir::currentPath() + "/rules.txt"));
    subscription.setEnabled(true);
    subscription.updateNow();

    const AdBlockRule *rule = subscription.allow(QString::fromUtf8(url.toEncoded()));
    if (rule)
        QVERIFY(rule->isException());
    QCOMPARE((nullptr != rule), allow);
}

void tst_AdBlockSubscription::block_data()
{
    QTest::addColumn<QUrl>("url");
    QTest::addColumn<bool>("block");
    QTest::newRow("block") << QUrl("http://example.com/ads/banner123.gif") << true;
    QTest::newRow("allow") << QUrl("http://example.com/ads/advice.html") << true;
}

// public AdBlockRule const *block(QString const &urlString) const
void tst_AdBlockSubscription::block()
{
    QFETCH(QUrl, url);
    QFETCH(bool, block);

    SubAdBlockSubscription subscription;
    subscription.setLocation(QUrl::fromLocalFile(QDir::currentPath() + "/rules.txt"));
    subscription.setEnabled(true);
    subscription.updateNow();

    const AdBlockRule *rule = subscription.block(QString::fromUtf8(url.toEncoded()));
    if (rule)
        QVERIFY(!rule->isException());
    QCOMPARE((nullptr != rule), block);
}

void tst_AdBlockSubscription::isEnabled_data()
{
    QTest::addColumn<bool>("isEnabled");
    QTest::newRow("true") << true;
    QTest::newRow("false") << false;
}

// public bool isEnabled() const
void tst_AdBlockSubscription::isEnabled()
{
    QFETCH(bool, isEnabled);

    SubAdBlockSubscription subscription;

    QSignalSpy spy0(&subscription, SIGNAL(rulesChanged()));
    QSignalSpy spy1(&subscription, SIGNAL(changed()));

    bool changed = subscription.isEnabled() != isEnabled;
    subscription.setEnabled(isEnabled);
    QCOMPARE(subscription.isEnabled(), isEnabled);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy1.count(), changed ? 1 : 0);
}

void tst_AdBlockSubscription::location_data()
{
    QTest::addColumn<QUrl>("location");
    QTest::newRow("null") << QUrl();
    QTest::newRow("x") << QUrl("x");
}

// public QUrl location() const
void tst_AdBlockSubscription::location()
{
    QFETCH(QUrl, location);

    SubAdBlockSubscription subscription(QUrl("abp:subscribe"));

    QSignalSpy spy0(&subscription, SIGNAL(rulesChanged()));
    QSignalSpy spy1(&subscription, SIGNAL(changed()));

    bool changed = location != subscription.location();
    subscription.setLocation(location);
    QCOMPARE(subscription.location(), location);
    QUrl expectedUrl(QStringLiteral("abp:subscribe"));
    QUrlQuery expectedQuery;
    expectedQuery.addQueryItem(QLatin1String("location"), location.toString());
    expectedQuery.addQueryItem(QLatin1String("title"), QString());
    expectedUrl.setQuery(expectedQuery);
    QCOMPARE(subscription.url(), expectedUrl);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy1.count(), changed ? 1 : 0);
}

void tst_AdBlockSubscription::saveRules_data()
{
    QTest::addColumn<QUrl>("location");
    QTest::newRow("file") << QUrl::fromLocalFile(QDir::currentPath() + "/rules2.txt");
}

// public void saveRules()
void tst_AdBlockSubscription::saveRules()
{
    QFETCH(QUrl, location);
    SubAdBlockSubscription subscription;
    subscription.setLocation(location);

    subscription.addRule(AdBlockRule());
    subscription.saveRules();
    subscription.removeRule(0);
    subscription.updateNow();
    QCOMPARE(subscription.allRules().count(), 1);
}

void tst_AdBlockSubscription::title_data()
{
    QTest::addColumn<QString>("title");
    QTest::newRow("null") << QString();
    QTest::newRow("foo") << QString("foo");
}

// public QString title() const
void tst_AdBlockSubscription::title()
{
    QFETCH(QString, title);

    SubAdBlockSubscription subscription;

    QSignalSpy spy0(&subscription, SIGNAL(rulesChanged()));
    QSignalSpy spy1(&subscription, SIGNAL(changed()));

    bool changed = title != subscription.title();
    subscription.setTitle(title);
    QCOMPARE(subscription.title(), title);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy1.count(), changed ? 1 : 0);
}

void tst_AdBlockSubscription::updateNow_data()
{
    QTest::addColumn<QUrl>("location");
    QTest::newRow("file") << QUrl::fromLocalFile(QDir::currentPath() + "/rules.txt");
}

// public void updateNow()
void tst_AdBlockSubscription::updateNow()
{
    QFETCH(QUrl, location);

    SubAdBlockSubscription subscription;
    subscription.setLocation(location);

    QSignalSpy spy0(&subscription, SIGNAL(rulesChanged()));
    QSignalSpy spy1(&subscription, SIGNAL(changed()));

    subscription.updateNow();

    QTRY_COMPARE(spy0.count(), 1);
    QCOMPARE(spy1.count(), 1);
    QVERIFY(subscription.lastUpdate() != QDateTime());
    QCOMPARE(subscription.allRules().count(), 2);
}

void tst_AdBlockSubscription::addRule()
{
    SubAdBlockSubscription subscription;

    QSignalSpy spy0(&subscription, SIGNAL(rulesChanged()));
    QSignalSpy spy1(&subscription, SIGNAL(changed()));

    subscription.addRule(AdBlockRule());
    subscription.addRule(AdBlockRule("/test"));

    QTRY_COMPARE(spy0.count(), 2);
    QCOMPARE(spy1.count(), 0);
    QCOMPARE(subscription.allRules().count(), 2);
}

void tst_AdBlockSubscription::removeRule()
{
    SubAdBlockSubscription subscription;

    QSignalSpy spy0(&subscription, SIGNAL(rulesChanged()));
    QSignalSpy spy1(&subscription, SIGNAL(changed()));

    subscription.addRule(AdBlockRule("/test"));
    subscription.removeRule(0);

    QTRY_COMPARE(spy0.count(), 2);
    QCOMPARE(spy1.count(), 0);
    QCOMPARE(subscription.allRules().count(), 0);
}


// SEC04: a hostile list can carry megabyte-long lines — reads are
// capped so parsing continues to the well-formed rules that follow,
// and the oversized fragments are kept inert by AdBlockRule's own
// filter-length cap.
void tst_AdBlockSubscription::hostileList()
{
    SubAdBlockSubscription subscription;
    subscription.setLocation(QUrl::fromLocalFile(
        QDir::currentPath() + "/hostile_rules.txt"));
    subscription.setEnabled(true);
    subscription.updateNow();

    QVERIFY(subscription.block(QString::fromUtf8(
        QUrl("http://real-ad.example/x").toEncoded())));
    const QList<AdBlockRule> rules = subscription.allRules();
    QVERIFY(rules.count() >= 2);
    for (const AdBlockRule &rule : rules) {
        if (rule.filter().size() > 4096)
            QVERIFY(!rule.isSupported());
    }
}

// SEC04: a rules file beyond the size cap is not parsed at all.
void tst_AdBlockSubscription::oversizedList()
{
    QTemporaryFile file;
    QVERIFY(file.open());
    QVERIFY(file.resize(64 * 1024 * 1024 + 1));
    file.flush();

    SubAdBlockSubscription subscription;
    subscription.setLocation(QUrl::fromLocalFile(file.fileName()));
    subscription.setEnabled(true);
    subscription.updateNow();

    QCOMPARE(subscription.allRules().count(), 0);
}

// TELEM01: the constructor's automatic staleness update must not
// reach the network until the user consented to remote list
// downloads; an explicit updateNow() remains an on-demand fetch.
void tst_AdBlockSubscription::remoteFetchRequiresConsent()
{
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QUrl location(QStringLiteral("http://127.0.0.1:%1/rules.txt")
                        .arg(server.serverPort()));
    const QByteArray subscribeUrl = QByteArray("abp:subscribe?location=")
        + location.toEncoded();

    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsUndecided);
    {
        // Undecided: a never-fetched remote list is "stale", yet the
        // constructor must not fetch it.
        AdBlockSubscription subscription(
            QUrl::fromEncoded(subscribeUrl));
        QVERIFY(!subscription.lastUpdate().isValid());
        QVERIFY(!server.waitForNewConnection(500));
    }
    {
        // Declined persists the silence.
        AdBlockManager::setRemoteListsConsent(
            AdBlockManager::RemoteListsDeclined);
        AdBlockSubscription subscription(
            QUrl::fromEncoded(subscribeUrl));
        QVERIFY(!server.waitForNewConnection(500));
    }

    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsGranted);
    {
        AdBlockSubscription subscription(
            QUrl::fromEncoded(subscribeUrl));
        QVERIFY(server.waitForNewConnection(8000));
        QTcpSocket *client = server.nextPendingConnection();
        QTRY_VERIFY(client->bytesAvailable() > 0);
        QVERIFY(client->readAll().startsWith("GET /rules.txt"));
        const QByteArray body =
            "[Adblock Plus 0.7.1]\n||consent-smoke.invalid^\n";
        client->write("HTTP/1.1 200 OK\r\nContent-Length: "
                      + QByteArray::number(body.size())
                      + "\r\nConnection: close\r\n\r\n" + body);
        client->disconnectFromHost();
        QTRY_VERIFY(subscription.lastUpdate().isValid());
        QVERIFY(subscription.allRules().count() >= 1);
    }

    AdBlockManager::setRemoteListsConsent(
        AdBlockManager::RemoteListsUndecided);
}

QTEST_MAIN(tst_AdBlockSubscription)
#include "tst_adblocksubscription.moc"

