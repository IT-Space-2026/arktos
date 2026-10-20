/*
 * Copyright (c) 2026, Arktos Browser - IT-Space
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

// TRUST04: the Tranco top-site rank table — rc_tranco_rank probes
// (exact host -> most specific listed ancestor), the vendored seed,
// the poison-safe seed-union-override merge, and the consent-gated
// updater's fetch->write->reload round trip through a loopback
// fixture serving the real top-1m.csv.zip container shape.
//
// The fixture table is injected through rc_tranco_load so the tests
// never depend on the vendored seed's contents.  Under a no-rust
// build the whole feature is absent — the tests then pin "never
// ranks / never fetches" instead of skipping outright.

#include <QtTest/QtTest>
#include <QtNetwork/QtNetwork>
#include <qtemporarydir.h>

#include "adblockmanager.h"
#include "trancolist.h"
#include "qtest_arktos.h"

#if defined(ARKTOS_RUSTCORE)
#include <rustcore.h>
#endif

// A minimal stored-method zip — the upstream top-1m.csv.zip container
// shape without needing a compressor in the test.
static QByteArray storedZip(const QByteArray &name, const QByteArray &body)
{
    QByteArray zip;
    const auto u16 = [&zip](quint16 v) {
        zip.append(char(v & 0xff));
        zip.append(char(v >> 8));
    };
    const auto u32 = [&zip](quint32 v) {
        zip.append(char(v & 0xff));
        zip.append(char((v >> 8) & 0xff));
        zip.append(char((v >> 16) & 0xff));
        zip.append(char((v >> 24) & 0xff));
    };
    const quint32 size = quint32(body.size());
    zip.append("PK\x03\x04", 4);
    u16(20); u16(0); u16(0); u16(0); u16(0);
    u32(0); u32(size); u32(size);
    u16(quint16(name.size())); u16(0);
    zip.append(name);
    zip.append(body);
    const quint32 cdOff = quint32(zip.size());
    zip.append("PK\x01\x02", 4);
    u16(20); u16(20); u16(0); u16(0); u16(0); u16(0);
    u32(0); u32(size); u32(size);
    u16(quint16(name.size())); u16(0); u16(0); u16(0); u16(0);
    u32(0); u32(0);
    zip.append(name);
    const quint32 cdSize = quint32(zip.size()) - cdOff;
    zip.append("PK\x05\x06", 4);
    u16(0); u16(0); u16(1); u16(1);
    u32(cdSize); u32(cdOff); u16(0);
    return zip;
}

class FixtureListServer : public QObject
{
public:
    int hits = 0;
    QByteArray body;

    bool start()
    {
        if (!m_server.listen(QHostAddress::LocalHost))
            return false;
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *client = m_server.nextPendingConnection()) {
                ++hits;
                connect(client, &QTcpSocket::readyRead, this,
                        [this, client]() {
                    const QByteArray reply =
                        "HTTP/1.1 200 OK\r\nContent-Type: application/zip\r\n"
                        "Content-Length: "
                        + QByteArray::number(body.size())
                        + "\r\nConnection: close\r\n\r\n" + body;
                    client->write(reply);
                    client->disconnectFromHost();
                });
            }
        });
        return true;
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/top-1m.csv.zip")
                    .arg(m_server.serverPort()));
    }

private:
    QTcpServer m_server;
};

class tst_TrancoList : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();

    void rankProbes();
    void builtinSeedRanks();
    void updateConsentGate();
    void updateRoundTrip();
    void updateFailureKeepsSeed();
    void noRustBuildInert();

private:
    QVariant m_savedConsent;
    QVariant m_savedUpdated;
    QVariant m_savedMajestic;
};

void tst_TrancoList::initTestCase()
{
    QCoreApplication::setApplicationName("tst_trancolist");
    QSettings settings;
    m_savedConsent =
        settings.value(QLatin1String("AdBlock/remoteListsConsent"));
    m_savedUpdated =
        settings.value(QLatin1String("privacy/trancoListUpdated"));
    m_savedMajestic =
        settings.value(QLatin1String("privacy/majesticListUpdated"));
}

void tst_TrancoList::cleanupTestCase()
{
    QSettings settings;
    const auto restore = [&settings](const QString &key,
                                     const QVariant &saved) {
        if (saved.isValid())
            settings.setValue(key, saved);
        else
            settings.remove(key);
    };
    restore(QLatin1String("AdBlock/remoteListsConsent"), m_savedConsent);
    restore(QLatin1String("privacy/trancoListUpdated"), m_savedUpdated);
    restore(QLatin1String("privacy/majesticListUpdated"), m_savedMajestic);
    // The fixture overrides the updater wrote go with the suite.
    QFile::remove(TrancoList::listFilePath());
    QFile::remove(TrancoList::majesticFilePath());
#if defined(ARKTOS_RUSTCORE)
    rc_tranco_reload();
#endif
}

void tst_TrancoList::init()
{
#if defined(ARKTOS_RUSTCORE)
    // Every probe test starts from a known fixture table — never the
    // vendored seed or a stale download.
    const QByteArray fixture =
        "# tst_trancolist fixture\n"
        "11,ranked-fixture.test\n"
        "22,sub.deep-fixture.test\n";
    QCOMPARE(rc_tranco_load(
                 reinterpret_cast<const uint8_t *>(fixture.constData()),
                 size_t(fixture.size())),
             RC_OK);
#endif
}

void tst_TrancoList::rankProbes()
{
#if defined(ARKTOS_RUSTCORE)
    QCOMPARE(rc_tranco_rank("ranked-fixture.test"), 11u);
    // Unlisted subdomain walks up to the listed ancestor — the
    // registrable-domain probe.
    QCOMPARE(rc_tranco_rank("www.ranked-fixture.test"), 11u);
    QCOMPARE(rc_tranco_rank("a.b.ranked-fixture.test"), 11u);
    // A listed subdomain's own rank wins over its parent's.
    QCOMPARE(rc_tranco_rank("sub.deep-fixture.test"), 22u);
    QCOMPARE(rc_tranco_rank("x.sub.deep-fixture.test"), 22u);
    // …but a listed subdomain never ranks the parent itself.
    QCOMPARE(rc_tranco_rank("deep-fixture.test"), 0u);
    // Unlisted/malformed hosts are unranked, never an error.
    QCOMPARE(rc_tranco_rank("unlisted.invalid"), 0u);
    QCOMPARE(rc_tranco_rank("not a host"), 0u);
    QCOMPARE(rc_tranco_rank("127.0.0.1"), 0u);
    QCOMPARE(rc_tranco_rank(nullptr), 0u);
#endif
}

void tst_TrancoList::builtinSeedRanks()
{
#if defined(ARKTOS_RUSTCORE)
    QCOMPARE(rc_tranco_reload(), RC_OK);
    // The vendored top-100k seed ranks with no download at all.
    QCOMPARE(rc_tranco_rank("google.com"), 1u);
    QVERIFY(rc_tranco_rank("www.google.com") > 0);
    QCOMPARE(rc_tranco_rank("definitely-not-ranked.invalid"), 0u);
    QVERIFY(rc_tranco_count() >= 100000);
#endif
}

void tst_TrancoList::updateConsentGate()
{
#if defined(ARKTOS_RUSTCORE)
    // Declined consent -> updateIfStale never fetches.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsDeclined);
    QSettings().remove(QLatin1String("privacy/trancoListUpdated"));
    FixtureListServer server;
    server.body = "1,should-not-load.test\n";
    QVERIFY(server.start());
    TrancoList *updater = TrancoList::instance();
    updater->setSourcesForTest({ server.url() });
    QSignalSpy finished(updater, SIGNAL(updateFinished(bool)));

    updater->updateIfStale();
    QTest::qWait(500);
    QCOMPARE(server.hits, 0);
    QCOMPARE(finished.count(), 0);
    updater->setSourcesForTest(TrancoList::defaultSources());
#endif
}

void tst_TrancoList::updateRoundTrip()
{
#if defined(ARKTOS_RUSTCORE)
    // Granted consent + a stale stamp -> updateIfStale fetches the
    // fixture zip AND the fixture Majestic CSV, writes both override
    // files verbatim and reloads the core.  The Tranco fixture also
    // tries to re-rank a seed domain to a worse rank — the best-rank
    // merge keeps the vendored rank.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsGranted);
    QSettings().remove(QLatin1String("privacy/trancoListUpdated"));
    QSettings().remove(QLatin1String("privacy/majesticListUpdated"));
    QFile::remove(TrancoList::listFilePath());
    QFile::remove(TrancoList::majesticFilePath());
    QCOMPARE(rc_tranco_reload(), RC_OK);

    FixtureListServer server;
    server.body = storedZip(
        "top-1m.csv",
        "7,served-fixture.test\n999999,google.com\n55,both-lists.test\n");
    QVERIFY(server.start());
    // SEC19: the Majestic source is a plain GlobalRank,TldRank,
    // Domain,... CSV committed under its own file name.
    FixtureListServer majestic;
    majestic.body =
        "GlobalRank,TldRank,Domain,TLD,RefSubNets\n"
        "3,1,both-lists.test,test,10\n"
        "40,5,mj-only.test,test,11\n";
    QVERIFY(majestic.start());
    TrancoList *updater = TrancoList::instance();
    updater->setSourcesForTest({
        TrancoList::Source(server.url(), TrancoList::trancoFileName()),
        TrancoList::Source(majestic.url(), TrancoList::majesticFileName()),
    });
    QSignalSpy finished(updater, SIGNAL(updateFinished(bool)));

    updater->updateIfStale();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 15000);
    QVERIFY(finished.at(0).at(0).toBool());
    QCOMPARE(server.hits, 1);
    QCOMPARE(majestic.hits, 1);

    const QString path = TrancoList::listFilePath();
    QVERIFY(QFile::exists(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    // The zip body lands verbatim — the crate unzips on load.
    QVERIFY(file.read(4) == "PK\x03\x04");
    file.close();
    QVERIFY(QFile::exists(TrancoList::majesticFilePath()));

    // The update merged on top of the seed…
    QCOMPARE(rc_tranco_rank("served-fixture.test"), 7u);
    QCOMPARE(rc_tranco_rank("www.served-fixture.test"), 7u);
    // …the Majestic source adds its own coverage…
    QCOMPARE(rc_tranco_rank("mj-only.test"), 40u);
    // …and a domain in both lists gets the best (lowest) rank.
    QCOMPARE(rc_tranco_rank("both-lists.test"), 3u);
    // …but nothing could un-rank (or worsen) a seed entry.
    QCOMPARE(rc_tranco_rank("google.com"), 1u);
    // A domain absent from both stays unranked.
    QCOMPARE(rc_tranco_rank("unlisted-fixture.test"), 0u);
    // Both stamps landed, so the shared status reports fresh.
    QVERIFY(QSettings().value(QLatin1String("privacy/trancoListUpdated"))
                .toDateTime().isValid());
    QVERIFY(QSettings().value(QLatin1String("privacy/majesticListUpdated"))
                .toDateTime().isValid());
    QVERIFY(TrancoList::instance()->lastUpdate().isValid());

    // A second stale-check within the interval fetches nothing.
    updater->updateIfStale();
    QTest::qWait(500);
    QCOMPARE(server.hits, 1);
    QCOMPARE(majestic.hits, 1);

    updater->setSourcesForTest(TrancoList::defaultSources());
#endif
}

void tst_TrancoList::updateFailureKeepsSeed()
{
#if defined(ARKTOS_RUSTCORE)
    // A fetched body that yields no usable entries reports failure —
    // the vendored seed keeps ranking either way.
    AdBlockManager::setRemoteListsConsent(AdBlockManager::RemoteListsGranted);
    QSettings().remove(QLatin1String("privacy/trancoListUpdated"));
    QFile::remove(TrancoList::listFilePath());
    QCOMPARE(rc_tranco_reload(), RC_OK);

    FixtureListServer server;
    server.body = "<html><body>404 not found</body></html>";
    QVERIFY(server.start());
    TrancoList *updater = TrancoList::instance();
    updater->setSourcesForTest({ server.url() });
    QSignalSpy finished(updater, SIGNAL(updateFinished(bool)));

    updater->updateIfStale();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 15000);
    QVERIFY(!finished.at(0).at(0).toBool());
    QCOMPARE(rc_tranco_rank("google.com"), 1u);

    updater->setSourcesForTest(TrancoList::defaultSources());
#endif
}

void tst_TrancoList::noRustBuildInert()
{
#if !defined(ARKTOS_RUSTCORE)
    // Without the crate there is no consumer — the updater is inert
    // and never fetches.
    FixtureListServer server;
    server.body = "1,unused.test\n";
    QVERIFY(server.start());
    TrancoList *updater = TrancoList::instance();
    updater->setSourcesForTest({ server.url() });
    QSignalSpy finished(updater, SIGNAL(updateFinished(bool)));

    updater->updateIfStale();
    QTest::qWait(500);
    QCOMPARE(server.hits, 0);

    updater->updateNow();
    QTRY_VERIFY_WITH_TIMEOUT(finished.count() == 1, 5000);
    QVERIFY(!finished.at(0).at(0).toBool());
    QCOMPARE(server.hits, 0);
    updater->setSourcesForTest(TrancoList::defaultSources());
#endif
}

QTEST_MAIN(tst_TrancoList)
#include "tst_trancolist.moc"
