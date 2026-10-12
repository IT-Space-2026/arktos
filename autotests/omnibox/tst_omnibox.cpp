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

#include <history.h>
#include <historycompleter.h>
#include <historymanager.h>
#include <opensearchengine.h>
#include <opensearchmanager.h>
#include <tabwidget.h>
#include <toolbarsearch.h>

#include <qsettings.h>

#ifdef ARORA_RUSTCORE
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <rustcore.h>
#endif

// OMNI01: the rustcore classifier must route every input exactly like
// the in-tree heuristic it replaced — the corpus test runs the same
// input set through both paths and diffs the resolved urls, for both
// values of urlloading/searchEngineFallback.  The suggest test diffs
// rc_history_suggest()'s ranked rows against the legacy
// HistoryFilterModel + HistoryCompletionModel pipeline on a seeded
// store.
class tst_Omnibox : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void classifyCorpus_data();
    void classifyCorpus();
    void classifyExpectations_data();
    void classifyExpectations();
    void frecencyScore_data();
    void frecencyScore();
    void suggestMatchesLegacyPipeline();

private:
    void seedHistory();
    QString m_previousEngine;
};

// The routing helpers are protected — same subclass seam the tabwidget
// test uses for its protected members.
class SubTabWidget : public TabWidget
{
public:
    static QUrl reference(const QString &input)
    {
        return guessUrlFromStringCpp(input, false);
    }
#ifdef ARORA_RUSTCORE
    static QUrl rust(const QString &input)
    {
        return guessUrlFromStringRust(input, false);
    }
#endif
};

void tst_Omnibox::initTestCase()
{
    // Own data dir so the history store the suggest test seeds is
    // isolated from the user's real profile.
    QCoreApplication::setApplicationName(QStringLiteral("omniboxtest"));

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = new OpenSearchEngine;
    engine->setName(QStringLiteral("omnibox-test"));
    engine->setSearchUrlTemplate(QStringLiteral(
        "http://omnibox-test.invalid/s?q={searchTerms}"));
    if (manager->engineExists(engine->name()))
        manager->removeEngine(engine->name());
    QVERIFY(manager->addEngine(engine));
    m_previousEngine = manager->currentEngineName();
    manager->setCurrentEngineName(engine->name());
    manager->setEngineForKeyword(QStringLiteral("ot"), engine);
}

void tst_Omnibox::cleanupTestCase()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->setEngineForKeyword(QStringLiteral("ot"), nullptr);
    manager->setCurrentEngineName(m_previousEngine);
    if (manager->engineExists(QStringLiteral("omnibox-test")))
        manager->removeEngine(QStringLiteral("omnibox-test"));
}

void tst_Omnibox::init()
{
    QSettings().setValue(QStringLiteral("urlloading/searchEngineFallback"),
                         true);
}

void tst_Omnibox::cleanup()
{
}

// Inputs where the two implementations must agree — the union of the
// probed QUrl::fromUserInput corpus and the quirks the heuristic
// carries (scheme lookalikes, keyword wins, garbage that is only a
// search).
void tst_Omnibox::classifyCorpus_data()
{
    QTest::addColumn<QString>("input");
    for (const char *input : {
        "word", "hup", "ph", "browser test", "a phrase with spaces",
        "test.foo bar", "docs.qt.io", "a.b", "localhost", "localhost.",
        "x.localhost", "localhost:8080", "127.0.0.1", "192.168.1.1",
        "[::1]:8080", "::1", "[::1]",
        "http://example.com/x", "https://x", "ftp://h/p",
        "ftp.example.com", "file:///etc/hostname", "/etc/hostname",
        "about:config", "about:home", "about:blank",
        "mailto:a@b.c", "javascript:void(0)", "view-source:x",
        "localhost:abc", "example.com:99999", "host:65536", "word:",
        "a:b:c", "word:x", "C:\\foo",
        "user@host.com", "u:p@host.com", "h:443/", "z:1", "h:080",
        "-flag", ".hidden", "a..b", "~/foo", "./rel", "x y",
        "//proto-rel.example.com/x", "0x7f.0.0.1", "HTTP://UP.COM/x",
        "foo.com?", "foo.com#f", "h:/p", "example.com:80",
        "host:80/x", "h:8080/p?q=1", "a~b", "a%20b", "_foo",
        "foo_bar", "a_b.c", "a--b", "a.", "0", "9lives",
        "ot hello", "ot a.b", "ot", "zz hello", " ",
    })
        QTest::newRow(input) << QString::fromUtf8(input);
}

void tst_Omnibox::classifyCorpus()
{
    QFETCH(QString, input);

#ifdef ARORA_RUSTCORE
    for (bool fallback : {true, false}) {
        QSettings().setValue(
            QStringLiteral("urlloading/searchEngineFallback"), fallback);
        const QUrl expected = SubTabWidget::reference(input);
        const QUrl actual = SubTabWidget::rust(input);
        QVERIFY2(!actual.isEmpty(),
                 qPrintable(QStringLiteral("rust path empty for '%1'")
                                .arg(input)));
        QCOMPARE(actual, expected);
    }
#else
    QUrl result = SubTabWidget::reference(input);
    QVERIFY(!result.isEmpty() || input.trimmed().isEmpty());
#endif
}

// Locks the semantic contract on a few representative inputs so a
// verdict-kind regression cannot hide inside a matching-by-accident
// diff.  Compared with the trailing slash stripped, like the
// loadString test does.
void tst_Omnibox::classifyExpectations_data()
{
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("expected");

    OpenSearchEngine engine;
    engine.setSearchUrlTemplate(QStringLiteral(
        "http://omnibox-test.invalid/s?q={searchTerms}"));
    const QString search = engine.searchUrl(
        QStringLiteral("word")).toString();
    const QString kwSearch = engine.searchUrl(
        QStringLiteral("hello")).toString();

    QTest::newRow("bare word")
        << QStringLiteral("word") << search;
    QTest::newRow("dotted host")
        << QStringLiteral("docs.qt.io")
        << QStringLiteral("http://docs.qt.io");
    QTest::newRow("localhost port")
        << QStringLiteral("localhost:8080")
        << QStringLiteral("http://localhost:8080");
    QTest::newRow("file url")
        << QStringLiteral("file:///x") << QStringLiteral("file:///x");
    QTest::newRow("about page")
        << QStringLiteral("about:config")
        << QStringLiteral("about:config");
    QTest::newRow("start page remap")
        << QStringLiteral("about:home")
        << QStringLiteral("qrc:/startpage.html");
    QTest::newRow("absolute path")
        << QStringLiteral("/etc/hostname")
        << QStringLiteral("file:///etc/hostname");
    QTest::newRow("keyword wins")
        << QStringLiteral("ot hello") << kwSearch;
}

void tst_Omnibox::classifyExpectations()
{
    QFETCH(QString, input);
    QFETCH(QString, expected);

    const QUrl resolved = SubTabWidget::reference(input);
    QCOMPARE(resolved.toString(QUrl::StripTrailingSlash),
             QUrl(expected).toString(QUrl::StripTrailingSlash));
#ifdef ARORA_RUSTCORE
    QCOMPARE(SubTabWidget::rust(input)
                 .toString(QUrl::StripTrailingSlash),
             QUrl(expected).toString(QUrl::StripTrailingSlash));
#endif
}

void tst_Omnibox::frecencyScore_data()
{
#ifdef ARORA_RUSTCORE
    QTest::addColumn<qint64>("ageDays");
    QTest::addColumn<qint64>("expected");

    // Mid-bucket ages — a local-day boundary can shift the diff by one,
    // which stays inside the bucket everywhere except "days <= 1".
    QTest::newRow("same day") << qint64(0) << qint64(100);
    QTest::newRow("three days") << qint64(3) << qint64(90);
    QTest::newRow("two weeks") << qint64(10) << qint64(70);
    QTest::newRow("a month") << qint64(22) << qint64(50);
    QTest::newRow("three months") << qint64(60) << qint64(30);
    QTest::newRow("ancient") << qint64(200) << qint64(10);
#endif
}

void tst_Omnibox::frecencyScore()
{
#ifdef ARORA_RUSTCORE
    QFETCH(qint64, ageDays);
    QFETCH(qint64, expected);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject args;
    args.insert(QStringLiteral("visits"),
                QJsonArray::fromVariantList(
                    {now - ageDays * 86400000}));
    args.insert(QStringLiteral("now_ms"), double(now));
    int64_t score = 0;
    QCOMPARE(rc_frecency_score(
                 QJsonDocument(args).toJson().constData(), &score),
             RC_OK);
    QCOMPARE(score, expected);
#endif
}

void tst_Omnibox::seedHistory()
{
    HistoryManager *manager = HistoryManager::instance();
    const QDateTime now = QDateTime::currentDateTime();
    QList<HistoryEntry> list;
    // Score-distinct where deterministic order matters: b sums 270
    // from three ~3-day visits, c decays to 70, and the same-day
    // trio old/a/new all score 100 — exercising the legacy tie rule
    // (oldest representative row first).
    list << HistoryEntry(QStringLiteral("http://b.example/old"),
                         now.addDays(-3), QStringLiteral("B"));
    list << HistoryEntry(QStringLiteral("http://b.example/old"),
                         now.addDays(-3).addSecs(-60),
                         QStringLiteral("B"));
    list << HistoryEntry(QStringLiteral("http://b.example/old"),
                         now.addDays(-3).addSecs(-120),
                         QStringLiteral("B"));
    list << HistoryEntry(QStringLiteral("http://a.example/fresh"),
                         now.addSecs(-10), QStringLiteral("A"));
    // Matches only via its title, not the url.
    list << HistoryEntry(QStringLiteral("http://c.example/"),
                         now.addDays(-10), QStringLiteral("needle page"));
    list << HistoryEntry(QStringLiteral("http://old.same-day/"),
                         now.addSecs(-30), QStringLiteral("Old"));
    list << HistoryEntry(QStringLiteral("http://new.same-day/"),
                         now, QStringLiteral("New"));
    manager->setHistory(list);
}

void tst_Omnibox::suggestMatchesLegacyPipeline()
{
    seedHistory();
    HistoryManager *manager = HistoryManager::instance();

    for (const QString &term :
         {QString(), QStringLiteral("example"),
          QStringLiteral("needle"), QStringLiteral("b")}) {
        // The legacy ordering pipeline.
        HistoryCompletionModel legacy;
        legacy.setSourceModel(manager->historyFilterModel());
        legacy.setSearchString(term);
        legacy.sort(0);
        QStringList expected;
        for (int i = 0; i < legacy.rowCount(); ++i)
            expected << legacy.index(i, 0)
                            .data(HistoryModel::UrlStringRole)
                            .toString();

#ifdef ARORA_RUSTCORE
        QStringList actual;
        const QByteArray t = term.toUtf8();
        char *json = rc_history_suggest(t.constData(), 500);
        QVERIFY(json);
        const QJsonArray rows =
            QJsonDocument::fromJson(QByteArray(json)).array();
        rc_string_free(json);
        for (const QJsonValue &v : rows)
            actual << v.toObject()
                          .value(QStringLiteral("url")).toString();
        QCOMPARE(actual, expected);
#else
        QVERIFY(!expected.isEmpty());
#endif
    }
}

QTEST_MAIN(tst_Omnibox)
#include "tst_omnibox.moc"
