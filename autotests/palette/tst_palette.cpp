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

#include <initializer_list>

#include <commandpalette.h>

#ifdef ARORA_RUSTCORE
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <rustcore.h>
#endif

// CPAL01: the palette's matching decision lives in rustcore — the
// subsequence scorer is a CommandPalette::fuzzyScore port (this suite
// corpus-diffs it against the Qt reference implementation) and
// rc_pal_match ranks the whole item set including the MRU recency
// boost.  The one intentional ranking improvement is the camel-hump
// word boundary, asserted on its own fixture.
class tst_Palette : public QObject
{
    Q_OBJECT

private slots:
    void referenceScores();
    void parityCorpus_data();
    void parityCorpus();
    void camelBoundaryBonus();
#ifdef ARORA_RUSTCORE
    void matchRanking();
    void emptyQueryMruOrder();
    void nonAscii();
    void badInput();
#endif
};

// Hand-computed expectations of the scoring table — pins the rule
// set on the reference path in every build mode.
void tst_Palette::referenceScores()
{
    // Identical strings: +10/run char, +15 at index 0, boundaries
    // inside, the substring bonus and no length penalty.
    QCOMPARE(CommandPalette::fuzzyScoreCpp(
                 QStringLiteral("new tab"), QStringLiteral("New Tab")),
             170);
    // Boundary-hopped acronym.
    QCOMPARE(CommandPalette::fuzzyScoreCpp(
                 QStringLiteral("cpd"),
                 QStringLiteral("Clear Private Data")),
             68);
    QCOMPARE(CommandPalette::fuzzyScoreCpp(
                 QStringLiteral("xyz"), QStringLiteral("New Tab")),
             -1);
    QCOMPARE(CommandPalette::fuzzyScoreCpp(
                 QString(), QStringLiteral("anything")),
             0);
    QCOMPARE(CommandPalette::fuzzyScoreCpp(
                 QStringLiteral("a"), QString()),
             -1);
}

// Queries over realistic matchTexts — the corpus runs through the
// public scorer and the Qt reference path and must score identically
// (no camel-humped candidates here; that bonus is pinned separately).
void tst_Palette::parityCorpus_data()
{
    QTest::addColumn<QString>("query");
    QTest::addColumn<QString>("candidate");
    for (const char *row : {
        "new tab|New Tab File/",
        "private|Private Browsing... incognito private window mode",
        "clear history|Clear Private Data clear history cookies cache erase browsing data",
        "dl|Downloads download manager files",
        "pref|Preferences: Privacy settings preferences options section Privacy",
        "reader|Reader Mode read article clutter-free",
        "smoke-beta|smoke-beta-tab http://127.0.0.1:9/palette",
        "xyz|New Tab File/",
        "|New Tab File/",
        "a|",
        "tab|Tab Widget Window/",
        "zz tab|New Tab File/",
        "büro|Büro",
        "日本|今日の日本語",
    }) {
        const QString line = QString::fromUtf8(row);
        const int bar = line.indexOf(QLatin1Char('|'));
        QTest::newRow(qPrintable(line))
            << line.left(bar) << line.mid(bar + 1);
    }
}

void tst_Palette::parityCorpus()
{
    QFETCH(QString, query);
    QFETCH(QString, candidate);
    QCOMPARE(CommandPalette::fuzzyScore(query, candidate),
             CommandPalette::fuzzyScoreCpp(query, candidate));
}

// The documented improvement: a lower->upper transition earns the
// word-boundary bonus the lowered-haystack reference cannot see.
void tst_Palette::camelBoundaryBonus()
{
    const int reference = CommandPalette::fuzzyScoreCpp(
        QStringLiteral("hub"), QStringLiteral("GitHub"));
    const int rust = CommandPalette::fuzzyScore(
        QStringLiteral("hub"), QStringLiteral("GitHub"));
    QVERIFY(reference > 0);
#ifdef ARORA_RUSTCORE
    QCOMPARE(rust, reference + 12);
#else
    QCOMPARE(rust, reference);
#endif
}

#ifdef ARORA_RUSTCORE
static QByteArray matchRequest(const QJsonArray &items,
                               const QStringList &mru)
{
    QJsonObject request;
    request.insert(QStringLiteral("items"), items);
    request.insert(QStringLiteral("mru"),
                   QJsonArray::fromStringList(mru));
    return QJsonDocument(request).toJson(QJsonDocument::Compact);
}

static QJsonArray itemsOf(std::initializer_list<const char *> rows)
{
    QJsonArray items;
    for (const char *row : rows) {
        const QString line = QString::fromUtf8(row);
        const int bar = line.indexOf(QLatin1Char('|'));
        QJsonObject entry;
        entry.insert(QStringLiteral("match"), line.left(bar));
        entry.insert(QStringLiteral("id"), line.mid(bar + 1));
        items.append(entry);
    }
    return items;
}

static QJsonArray palMatch(const QString &query, const QByteArray &request)
{
    const QByteArray q = query.toUtf8();
    char *out = rc_pal_match(q.constData(), request.constData());
    if (!out)
        return QJsonArray();
    const QJsonArray rows = QJsonDocument::fromJson(QByteArray(out)).array();
    rc_string_free(out);
    return rows;
}

void tst_Palette::matchRanking()
{
    const QJsonArray items = itemsOf({
        "alpha|a", "beta|b", "gamma|g", "zzz|z",
    });
    const QJsonArray rows = palMatch(
        QStringLiteral("a"), matchRequest(items, {"g", "a"}));
    QCOMPARE(rows.size(), 3);
    // alpha: 50 + mru 59 = 109; gamma: 35 + 60 = 95; beta: 35.
    QCOMPARE(rows.at(0).toObject().value(QStringLiteral("index")).toInt(), 0);
    QCOMPARE(rows.at(0).toObject().value(QStringLiteral("score")).toInt(), 109);
    QCOMPARE(rows.at(1).toObject().value(QStringLiteral("index")).toInt(), 2);
    QCOMPARE(rows.at(2).toObject().value(QStringLiteral("index")).toInt(), 1);
}

void tst_Palette::emptyQueryMruOrder()
{
    const QJsonArray items = itemsOf({
        "alpha|a", "beta|b", "gamma|g", "zzz|z",
    });
    const QJsonArray rows = palMatch(
        QString(), matchRequest(items, {"g", "a"}));
    // Every row matches at 0; MRU ids rank in MRU order, then the
    // untouched item order.
    QCOMPARE(rows.size(), 4);
    QCOMPARE(rows.at(0).toObject().value(QStringLiteral("index")).toInt(), 2);
    QCOMPARE(rows.at(0).toObject().value(QStringLiteral("score")).toInt(), 60);
    QCOMPARE(rows.at(1).toObject().value(QStringLiteral("index")).toInt(), 0);
    QCOMPARE(rows.at(1).toObject().value(QStringLiteral("score")).toInt(), 59);
    QCOMPARE(rows.at(2).toObject().value(QStringLiteral("index")).toInt(), 1);
    QCOMPARE(rows.at(3).toObject().value(QStringLiteral("index")).toInt(), 3);
}

void tst_Palette::nonAscii()
{
    QVERIFY(rc_pal_score("büro", "Büro") > 0);
    QVERIFY(rc_pal_score("日本", "今日の日本語") > 0);
    QVERIFY(rc_pal_score("𝕏", "a𝕏b") > 0);
}

void tst_Palette::badInput()
{
    QCOMPARE(qint64(rc_pal_score(nullptr, "x")), qint64(-1));
    QCOMPARE(qint64(rc_pal_score("x", nullptr)), qint64(-1));
    // Malformed request JSON -> NULL, never a verdict.
    QCOMPARE(rc_pal_match("x", "not json"), (char *)nullptr);
    QCOMPARE(rc_pal_match("x", nullptr), (char *)nullptr);
    // Missing keys degrade to an empty result, not an error.
    char *out = rc_pal_match("x", "{}");
    QVERIFY(out);
    QCOMPARE(QJsonDocument::fromJson(QByteArray(out)).array().size(), 0);
    rc_string_free(out);
}
#endif

QTEST_MAIN(tst_Palette)
#include "tst_palette.moc"
