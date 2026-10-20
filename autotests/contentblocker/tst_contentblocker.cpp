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

// WKB01: the EasyList→WebKit content-blocker JSON converter —
// end-to-end through the arktos-adblock-ffi C ABI via
// AdBlockRustEngine::contentBlockerJson.  Asserts the document shape
// ({rules, dropped, meta}), the covered rule classes, the drop
// channel, and Safari-ordering invariants a
// WebKitUserContentFilterStore consumer relies on.  Under a no-rust
// build the surface is absent and the test pins that instead.

#include <QtTest/QtTest>
#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qset.h>

#include "qtest_arktos.h"

#if defined(ARKTOS_ADBLOCK_RUST)
#include "adblockrustengine.h"
#endif

class tst_ContentBlocker : public QObject
{
    Q_OBJECT

private slots:
#if defined(ARKTOS_ADBLOCK_RUST)
    void convertsKnownRules();
    void fixtureCoverage();
    void dropChannelHonest();
    void safariRuleOrdering();
    void emptyInput();
#else
    void noRustBuild();
#endif
};

#if defined(ARKTOS_ADBLOCK_RUST)

static QJsonObject convertDoc(const QByteArray &rules)
{
    const QByteArray raw = AdBlockRustEngine::contentBlockerJson(rules);
    if (raw.isEmpty())
        return QJsonObject();
    return QJsonDocument::fromJson(raw).object();
}

static QStringList actionTypes(const QJsonArray &rules)
{
    QStringList types;
    for (const QJsonValue &v : rules)
        types << v.toObject().value(QLatin1String("action"))
                     .toObject().value(QLatin1String("type")).toString();
    return types;
}

void tst_ContentBlocker::convertsKnownRules()
{
    const QJsonObject doc = convertDoc(
        "||ads.example^\n"
        "||track.example^$image,third-party\n"
        "@@||ads.example/ok.js^$script\n"
        "example.com##.ad-banner\n");
    QVERIFY(!doc.isEmpty());

    const QJsonArray rules = doc.value(QLatin1String("rules")).toArray();
    QCOMPARE(rules.size(), 5);

    // ||domain^ → block with an anchored url-filter regex.
    const QJsonObject ads = rules.at(0).toObject();
    QCOMPARE(ads.value(QLatin1String("action")).toObject()
                 .value(QLatin1String("type")).toString(),
             QLatin1String("block"));
    const QString filter = ads.value(QLatin1String("trigger")).toObject()
                               .value(QLatin1String("url-filter")).toString();
    QVERIFY(filter.startsWith(QLatin1Char('^')));
    QVERIFY(filter.contains(QLatin1String("ads\\.example")));

    // $image,third-party → resource-type + load-type on the trigger.
    const QJsonObject track = rules.at(1).toObject()
                                .value(QLatin1String("trigger")).toObject();
    QCOMPARE(track.value(QLatin1String("resource-type")).toArray(),
             QJsonArray::fromStringList({QLatin1String("image")}));
    QCOMPARE(track.value(QLatin1String("load-type")).toArray(),
             QJsonArray::fromStringList({QLatin1String("third-party")}));

    // ##selector → css-display-none carrying the selector, scoped by
    // if-domain.
    bool sawHide = false;
    for (const QJsonValue &v : rules) {
        const QJsonObject r = v.toObject();
        const QJsonObject action = r.value(QLatin1String("action")).toObject();
        if (action.value(QLatin1String("type")).toString()
                != QLatin1String("css-display-none"))
            continue;
        sawHide = true;
        QCOMPARE(action.value(QLatin1String("selector")).toString(),
                 QLatin1String(".ad-banner"));
        QCOMPARE(r.value(QLatin1String("trigger")).toObject()
                     .value(QLatin1String("if-domain")).toArray(),
                 QJsonArray::fromStringList({QLatin1String("example.com")}));
    }
    QVERIFY(sawHide);
}

void tst_ContentBlocker::fixtureCoverage()
{
    // The crate's own fixture — kept in lockstep with the cargo tests.
    QFile f(QString::fromLatin1(QT_TESTCASE_BUILDDIR)
            + QLatin1String("/../../src/adblock/rust/fixtures/easylist-sample.txt"));
    QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(f.fileName()));
    const QJsonObject doc = convertDoc(f.readAll());
    QVERIFY(!doc.isEmpty());
    const QJsonObject meta = doc.value(QLatin1String("meta")).toObject();
    // WK18: the fixture's $redirect rule degrades to a block — it is
    // emitted and counted under meta.degraded, not dropped.
    QCOMPARE(meta.value(QLatin1String("emitted")).toInt(), 14);
    QCOMPARE(meta.value(QLatin1String("dropped")).toInt(), 9);
    QCOMPARE(meta.value(QLatin1String("degraded")).toInt(), 1);
    QCOMPARE(doc.value(QLatin1String("dropped")).toArray().size(), 9);
}

void tst_ContentBlocker::dropChannelHonest()
{
    // Every unexpressible rule lands in dropped with a reason code —
    // the drop list is output, not a log line.
    const QJsonObject doc = convertDoc(
        "||redir.example^$redirect=noop\n"
        "||csp.example^$csp=script-src 'none'\n"
        "||param.example^$removeparam=utm\n"
        "example.com##+js(set-constant)\n");
    const QJsonArray dropped = doc.value(QLatin1String("dropped")).toArray();
    // WK18: $redirect is no longer a drop — Safari cannot serve a
    // stub, so the rule degrades to an honest block counted under
    // meta.degraded.  The three truly unexpressible rules still land
    // in dropped with their reason codes.
    QCOMPARE(dropped.size(), 3);

    QSet<QString> whys;
    for (const QJsonValue &v : dropped) {
        const QJsonObject d = v.toObject();
        QVERIFY(!d.value(QLatin1String("rule")).toString().isEmpty());
        whys.insert(d.value(QLatin1String("why")).toString());
    }
    QVERIFY(whys.contains(QLatin1String("csp")));
    QVERIFY(whys.contains(QLatin1String("removeparam")));
    QVERIFY(whys.contains(QLatin1String("scriptlet")));

    QCOMPARE(doc.value(QLatin1String("meta")).toObject()
                 .value(QLatin1String("degraded")).toInt(), 1);
    bool sawDegradedBlock = false;
    for (const QJsonValue &v : doc.value(QLatin1String("rules")).toArray()) {
        const QJsonObject r = v.toObject();
        if (r.value(QLatin1String("trigger")).toObject()
                .value(QLatin1String("url-filter")).toString()
                .contains(QLatin1String("redir"))) {
            QCOMPARE(r.value(QLatin1String("action")).toObject()
                         .value(QLatin1String("type")).toString(),
                     QLatin1String("block"));
            sawDegradedBlock = true;
        }
    }
    QVERIFY(sawDegradedBlock);
}

void tst_ContentBlocker::safariRuleOrdering()
{
    // ignore-previous-rules erases earlier actions only, so every
    // exception must sit after the blocks it cancels, and the
    // first-party-document guard comes last of all.
    const QJsonObject doc = convertDoc(
        "||ads.example^\n"
        "@@||ads.example/ok.js^\n");
    const QStringList types = actionTypes(
        doc.value(QLatin1String("rules")).toArray());
    const int lastBlock = types.lastIndexOf(QLatin1String("block"));
    const int firstIgnore = types.indexOf(QLatin1String("ignore-previous-rules"));
    QVERIFY(lastBlock >= 0);
    QVERIFY(firstIgnore > lastBlock);
    QCOMPARE(types.last(), QLatin1String("ignore-previous-rules"));
}

void tst_ContentBlocker::emptyInput()
{
    QVERIFY(AdBlockRustEngine::contentBlockerJson(QByteArray()).isNull());

    const QJsonObject doc = convertDoc("! only comments\n[Adblock Plus 2.0]\n");
    QVERIFY(!doc.isEmpty());
    QCOMPARE(doc.value(QLatin1String("rules")).toArray().size(), 0);
    QCOMPARE(doc.value(QLatin1String("dropped")).toArray().size(), 0);
}

#else // !ARKTOS_ADBLOCK_RUST

void tst_ContentBlocker::noRustBuild()
{
    // The crate is absent by configuration — nothing to exercise.
    QVERIFY(true);
}

#endif // ARKTOS_ADBLOCK_RUST

QTEST_MAIN(tst_ContentBlocker)
#include "tst_contentblocker.moc"
