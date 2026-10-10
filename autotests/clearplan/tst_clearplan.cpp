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

#include <QtTest/QtTest>
#include "qtest_arora.h"

#include <clearprivatedata.h>

#include <browserprofile.h>
#include <cookiejar.h>
#include <historymanager.h>

#include <qcheckbox.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qwebengineprofile.h>

#ifdef ARORA_RUSTCORE
#include <rustcore.h>
#endif

// CLR01: the clear-private-data decision is a (period, categories,
// scope) -> ordered op list produced by the planner (rustcore under
// ARORA_RUSTCORE, the identical Qt table otherwise) and executed
// per-store.  These tests pin the op vocabulary, the OTR scoping
// rule (a private-context plan must contain no op that touches a
// persistent store) and the execution itself against seeded stores.
class tst_ClearPlan : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void persistentPlanShape();
    void singleCategories();
    void otrPlanShape();
    void otrPersistentOnlyCategories();
    void rangedPeriod();
    void unknownPeriod();
    void executeClearsSeededStores();
    void executeRangedKeepsOldRows();
    void otrPlanLeavesPersistentStores();
    void unknownOpSkipped();
#ifdef ARORA_RUSTCORE
    void parityCorpus_data();
    void parityCorpus();
    void ffiBadInput();
#endif
};

static QStringList opNames(const QJsonArray &ops)
{
    QStringList names;
    for (const QJsonValue &v : ops)
        names << v.toObject().value(QLatin1String("op")).toString();
    return names;
}

void tst_ClearPlan::initTestCase()
{
    QCoreApplication::setApplicationName("tst_clearplan");
    QSettings().clear();
}

void tst_ClearPlan::cleanup()
{
    for (QWidget *widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("QDialog"))
            widget->close();
    }
}

// The full persistent set produces the canonical fan-out — the same
// order the legacy accept()/clearPrivateDataOnExit() bodies ran in.
void tst_ClearPlan::persistentPlanShape()
{
    const QJsonArray ops = ClearPrivateData::plan(
        QLatin1String("all"),
        ClearPrivateData::ClearAll | ClearPrivateData::ClearSession
            | ClearPrivateData::ClearFullWipe,
        false);
    QCOMPARE(opNames(ops),
             QStringList() << QLatin1String("containers.unmaterialized")
                 << QLatin1String("history.clear")
                 << QLatin1String("visited_links.clear")
                 << QLatin1String("downloads.clear")
                 << QLatin1String("search.clear")
                 << QLatin1String("cookies.clear")
                 << QLatin1String("sitedata.page_sweep")
                 << QLatin1String("sitedata.defer_wipe")
                 << QLatin1String("cache.clear")
                 << QLatin1String("nam_cache.clear")
                 << QLatin1String("icons.clear")
                 << QLatin1String("session.clear")
                 << QLatin1String("storage.defer_full_wipe")
                 << QLatin1String("containers.wipe_unmaterialized"));
    const QJsonObject containers = ops.at(0).toObject();
    QCOMPARE(containers.value(QLatin1String("cookies")).toBool(), true);
    QCOMPARE(containers.value(QLatin1String("visited_links")).toBool(), true);
    QCOMPARE(ops.at(2).toObject().value(QLatin1String("scope")).toString(),
             QLatin1String("persistent"));
}

void tst_ClearPlan::singleCategories()
{
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearHistory, false)),
             QStringList() << QLatin1String("containers.unmaterialized")
                 << QLatin1String("history.clear")
                 << QLatin1String("visited_links.clear"));
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearDownloads, false)),
             QStringList() << QLatin1String("downloads.clear"));
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearSearch, false)),
             QStringList() << QLatin1String("search.clear"));
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearCookies, false)),
             QStringList() << QLatin1String("containers.unmaterialized")
                 << QLatin1String("cookies.clear"));
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearSiteData, false)),
             QStringList() << QLatin1String("containers.unmaterialized")
                 << QLatin1String("sitedata.page_sweep")
                 << QLatin1String("sitedata.defer_wipe"));
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearCache, false)),
             QStringList() << QLatin1String("containers.unmaterialized")
                 << QLatin1String("cache.clear")
                 << QLatin1String("nam_cache.clear"));
    QCOMPARE(opNames(ClearPrivateData::plan(
                 QLatin1String("all"), ClearPrivateData::ClearIcons, false)),
             QStringList() << QLatin1String("icons.clear"));
    // No categories -> no work, not even the container sweep.
    QVERIFY(ClearPrivateData::plan(QLatin1String("all"), 0, false).isEmpty());
}

// The OTR plan must contain only ops scoped to the off-the-record
// context — no persistent store may appear, ever.
void tst_ClearPlan::otrPlanShape()
{
    const QJsonArray ops = ClearPrivateData::plan(
        QLatin1String("all"),
        ClearPrivateData::ClearAll | ClearPrivateData::ClearSession
            | ClearPrivateData::ClearFullWipe,
        true);
    QCOMPARE(opNames(ops),
             QStringList() << QLatin1String("visited_links.clear")
                 << QLatin1String("downloads.clear_otr")
                 << QLatin1String("cookies.clear")
                 << QLatin1String("sitedata.page_sweep")
                 << QLatin1String("cache.clear"));
    for (const QJsonValue &v : ops) {
        const QJsonObject op = v.toObject();
        const QString name = op.value(QLatin1String("op")).toString();
        QVERIFY(op.value(QLatin1String("scope")).toString()
                    == QLatin1String("otr")
                || name == QLatin1String("downloads.clear_otr"));
        QVERIFY(!name.contains(QLatin1String("history.clear")));
        QVERIFY(!name.contains(QLatin1String("session")));
        QVERIFY(!name.contains(QLatin1String("icons")));
        QVERIFY(!name.contains(QLatin1String("search.clear")));
        QVERIFY(!name.contains(QLatin1String("containers")));
    }
}

void tst_ClearPlan::otrPersistentOnlyCategories()
{
    // Categories that own only persistent stores produce nothing in
    // OTR scope.
    QVERIFY(ClearPrivateData::plan(
                QLatin1String("all"),
                ClearPrivateData::ClearSearch | ClearPrivateData::ClearIcons
                    | ClearPrivateData::ClearSession | ClearPrivateData::ClearFullWipe,
                true)
                .isEmpty());
}

// A ranged period tags every op with the window and marks the one
// ranged-capable store; the rest degrade honestly to a full wipe.
void tst_ClearPlan::rangedPeriod()
{
    const QJsonArray ops = ClearPrivateData::plan(
        QLatin1String("hour"), ClearPrivateData::ClearAll, false);
    QVERIFY(!ops.isEmpty());
    for (const QJsonValue &v : ops) {
        const QJsonObject op = v.toObject();
        QCOMPARE(op.value(QLatin1String("since_secs")).toInteger(), qint64(3600));
        if (op.value(QLatin1String("op")).toString()
            == QLatin1String("history.clear")) {
            QCOMPARE(op.value(QLatin1String("ranged")).toBool(), true);
            QVERIFY(!op.contains(QLatin1String("degraded")));
        } else {
            QCOMPARE(op.value(QLatin1String("ranged")).toBool(), false);
            QCOMPARE(op.value(QLatin1String("degraded")).toBool(), true);
        }
    }
}

void tst_ClearPlan::unknownPeriod()
{
    // An unrecognized window is a no-op plan, never a guess.
    QVERIFY(ClearPrivateData::plan(
                QLatin1String("fortnight"), ClearPrivateData::ClearAll, false)
                .isEmpty());
    QVERIFY(ClearPrivateData::plan(
                QLatin1String("hour;drop"), ClearPrivateData::ClearAll, false)
                .isEmpty());
}

// Executing a persistent plan against seeded stores wipes exactly
// the stores the categories named — the settings-smoke fixture set.
void tst_ClearPlan::executeClearsSeededStores()
{
    HistoryManager *history = HistoryManager::instance();
    const QUrl url(QLatin1String("http://clearplan-seed.example/"));
    history->addHistoryEntry(url.toString());
    const QIcon seededIcon(QPixmap(4, 4));
    history->setIcon(url, seededIcon);
    QVERIFY(history->historyContains(url.toString()));

    CookieJar *jar = CookieJar::instance(BrowserProfile::normalProfile());
    jar->setCookies(QList<QNetworkCookie>()
                    << QNetworkCookie("seed", "1"));
    QVERIFY(!jar->cookies().isEmpty());

    ClearPrivateData::executePlan(ClearPrivateData::plan(
        QLatin1String("all"), ClearPrivateData::ClearAll, false));

    QVERIFY(!history->historyContains(url.toString()));
    QVERIFY(jar->cookies().isEmpty());
    QVERIFY(history->icon(url).cacheKey() != seededIcon.cacheKey());
}

// The ranged leg: rows inside the window go, older rows survive —
// and the mirror+store agree afterwards.
void tst_ClearPlan::executeRangedKeepsOldRows()
{
    HistoryManager *history = HistoryManager::instance();
    const QDateTime now = QDateTime::currentDateTime();
    const QUrl oldUrl(QLatin1String("http://clearplan-old.example/"));
    const QUrl midUrl(QLatin1String("http://clearplan-mid.example/"));
    const QUrl newUrl(QLatin1String("http://clearplan-new.example/"));
    QList<HistoryEntry> seeded;
    seeded << HistoryEntry(oldUrl.toString(), now.addSecs(-7200), QString())
           << HistoryEntry(midUrl.toString(), now.addSecs(-1800), QString())
           << HistoryEntry(newUrl.toString(), now, QString());
    history->setHistory(seeded);
    QVERIFY(history->historyContains(oldUrl.toString()));
    QVERIFY(history->historyContains(midUrl.toString()));
    QVERIFY(history->historyContains(newUrl.toString()));

    // A cutoff between the two newest timestamps deletes only the
    // newest row.
    const qint64 cutoff = now.addSecs(-10).toMSecsSinceEpoch();
    history->clearSince(cutoff);
    QVERIFY(history->historyContains(oldUrl.toString()));
    QVERIFY(history->historyContains(midUrl.toString()));
    QVERIFY(!history->historyContains(newUrl.toString()));

    // A last-hour plan removes the in-window row and keeps the
    // two-hour-old one.
    ClearPrivateData::executePlan(ClearPrivateData::plan(
        QLatin1String("hour"), ClearPrivateData::ClearHistory, false));
    QVERIFY(history->historyContains(oldUrl.toString()));
    QVERIFY(!history->historyContains(midUrl.toString()));
    history->clear();
}

// The de-scoping guarantee: an OTR plan executes its ops and leaves
// every persistent store untouched — history, icons, cookies.
void tst_ClearPlan::otrPlanLeavesPersistentStores()
{
    HistoryManager *history = HistoryManager::instance();
    const QUrl url(QLatin1String("http://clearplan-otr.example/"));
    history->addHistoryEntry(url.toString());
    const QIcon seededIcon(QPixmap(4, 4));
    history->setIcon(url, seededIcon);

    CookieJar *jar = CookieJar::instance(BrowserProfile::normalProfile());
    jar->setCookies(QList<QNetworkCookie>()
                    << QNetworkCookie("otr-seed", "1"));
    QVERIFY(!jar->cookies().isEmpty());

    ClearPrivateData::executePlan(ClearPrivateData::plan(
        QLatin1String("all"),
        ClearPrivateData::ClearAll | ClearPrivateData::ClearSession
            | ClearPrivateData::ClearFullWipe,
        true));

    QVERIFY(history->historyContains(url.toString()));
    QCOMPARE(history->icon(url).cacheKey(), seededIcon.cacheKey());
    QVERIFY(!jar->cookies().isEmpty());

    history->clear();
    history->clearIcons();
    jar->clear();
}

// An op name the executor does not know is skipped with a warning —
// forward compatibility degrades to less clearing, never to a guess.
void tst_ClearPlan::unknownOpSkipped()
{
    HistoryManager *history = HistoryManager::instance();
    const QUrl url(QLatin1String("http://clearplan-unknown.example/"));
    const QIcon seededIcon(QPixmap(4, 4));
    history->setIcon(url, seededIcon);

    QJsonArray ops;
    ops.append(QJsonObject{{QLatin1String("op"), QLatin1String("made.up.op")}});
    ops.append(QJsonObject{{QLatin1String("op"), QLatin1String("icons.clear")}});
    ClearPrivateData::executePlan(ops);

    QVERIFY(history->icon(url).cacheKey() != seededIcon.cacheKey());
}

#ifdef ARORA_RUSTCORE
void tst_ClearPlan::parityCorpus_data()
{
    QTest::addColumn<QString>("period");
    QTest::addColumn<quint32>("categories");
    QTest::addColumn<bool>("otr");

    const quint32 cats[] = {
        0,
        ClearPrivateData::ClearHistory,
        ClearPrivateData::ClearCookies,
        ClearPrivateData::ClearAll,
        ClearPrivateData::ClearAll | ClearPrivateData::ClearSession
            | ClearPrivateData::ClearFullWipe,
    };
    const char *periods[] = {"all", "everything", "hour", "day",
                             "last-week", "", "fortnight"};
    for (const char *period : periods) {
        for (quint32 c : cats) {
            for (bool otr : {false, true}) {
                QTest::newRow(qPrintable(
                        QString(QLatin1String("%1/%2/%3"))
                            .arg(QString::fromUtf8(period))
                            .arg(c)
                            .arg(otr)))
                    << QString::fromUtf8(period) << c << otr;
                }
        }
    }
}

// The rustcore planner and the Qt transliteration are one table —
// the whole corpus must produce identical op lists in both scopes.
void tst_ClearPlan::parityCorpus()
{
    QFETCH(QString, period);
    QFETCH(quint32, categories);
    QFETCH(bool, otr);
    QCOMPARE(ClearPrivateData::plan(period, categories, otr),
             ClearPrivateData::planQt(period, categories, otr));
}

void tst_ClearPlan::ffiBadInput()
{
    QCOMPARE(rc_clearplan(nullptr, 0xff, 0), (char *)nullptr);
    QCOMPARE(rc_clearplan("bogus", 0xff, 0), (char *)nullptr);
    // A valid call still produces the plan wrapper.
    char *out = rc_clearplan("all", ClearPrivateData::ClearIcons, 0);
    QVERIFY(out);
    const QJsonObject plan =
        QJsonDocument::fromJson(QByteArray(out)).object();
    rc_string_free(out);
    QCOMPARE(plan.value(QLatin1String("scope")).toString(),
             QLatin1String("persistent"));
    QCOMPARE(plan.value(QLatin1String("ops")).toArray().size(), 1);
}
#endif

QTEST_MAIN(tst_ClearPlan)
#include "tst_clearplan.moc"
