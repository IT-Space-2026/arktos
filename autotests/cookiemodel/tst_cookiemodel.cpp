/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

// COV02: CookieModel and CookieExceptionsModel — the two table models
// backing the cookie dialogs.  Both are thin views over CookieJar
// (which the cookiejar autotest covers); here we pin the row layout,
// role data, add/remove rule semantics and model-consistency via
// QAbstractItemModelTester.

#include <QtTest/QtTest>
#include <modeltest.h>
#include <cookiejar.h>
#include <cookiemodel.h>
#include <cookieexceptionsmodel.h>
#include <QWebEngineProfile>

#if defined(ARORA_RUSTCORE)
#include "sitedecisionstore.h"
#endif

class tst_CookieModel : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void cookieModel();
    void cookieModel_data();
    void cookieModelRemoveRows();
    void cookieModelCookiesChanged();
    void exceptionsModel();
    void exceptionsAddRule_data();
    void exceptionsAddRule();
    void exceptionsRemoveRows();
};

static QNetworkCookie makeCookie(const QString &domain, const QByteArray &name = "a",
                                 const QByteArray &value = "b")
{
    QNetworkCookie cookie(name, value);
    cookie.setDomain(domain);
    return cookie;
}

void tst_CookieModel::initTestCase()
{
    QCoreApplication::setApplicationName("tst_cookiemodel");
    QSettings settings;
    settings.clear();
}

void tst_CookieModel::cleanupTestCase()
{
}

void tst_CookieModel::init()
{
    QSettings settings;
    settings.clear();
#if defined(ARORA_RUSTCORE)
    // Per-site cookie rules live in the decision store now — clear
    // rows a previous test wrote, same as the QSettings wipe.
    SiteDecisionStore::clear(SiteDecisionStore::KindCookie);
#endif
}

void tst_CookieModel::cleanup()
{
}

void tst_CookieModel::cookieModel()
{
    QWebEngineProfile profile;
    CookieJar jar(&profile);
    CookieModel model(&jar);
    ModelTest tester(&model);

    QCOMPARE(model.columnCount(), 6);
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(model.data(QModelIndex()), QVariant());

    // Header labels for all six columns.
    QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Website"));
    QCOMPARE(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Name"));
    QCOMPARE(model.headerData(2, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Path"));
    QCOMPARE(model.headerData(3, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Secure"));
    QCOMPARE(model.headerData(4, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Expires"));
    QCOMPARE(model.headerData(5, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Contents"));
    QVERIFY(model.headerData(6, Qt::Horizontal, Qt::DisplayRole).isNull() ||
            !model.headerData(6, Qt::Horizontal, Qt::DisplayRole).isValid());
    QVERIFY(model.headerData(0, Qt::Horizontal, Qt::SizeHintRole).isValid());
}

void tst_CookieModel::cookieModel_data()
{
    QWebEngineProfile profile;
    CookieJar jar(&profile);

    QNetworkCookie plain = makeCookie("foo.com");
    plain.setPath(QLatin1String("/dir"));
    QNetworkCookie secure = makeCookie("bar.com", "s", "v");
    secure.setSecure(true);
    QNetworkCookie dated = makeCookie("baz.com", "d", "v");
    QDateTime expires = QDateTime::currentDateTimeUtc().addDays(1);
    dated.setExpirationDate(expires);
    jar.setCookies(QList<QNetworkCookie>() << plain << secure << dated);

    CookieModel model(&jar);
    ModelTest tester(&model);
    QCOMPARE(model.rowCount(), 3);

    // DisplayRole per column.
    QCOMPARE(model.data(model.index(0, 0)).toString(), QLatin1String("foo.com"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QLatin1String("a"));
    QCOMPARE(model.data(model.index(0, 2)).toString(), QLatin1String("/dir"));
    QCOMPARE(model.data(model.index(0, 3)).toString(), QLatin1String("false"));
    QCOMPARE(model.data(model.index(0, 4)).toString(), QLatin1String("Session cookie"));
    QCOMPARE(model.data(model.index(0, 5)).toString(), QLatin1String("b"));

    QCOMPARE(model.data(model.index(1, 3)).toString(), QLatin1String("true"));
    QCOMPARE(model.data(model.index(2, 4)).toString(), expires.toString());

    // SortRole returns the raw values.
    QCOMPARE(model.data(model.index(0, 0), CookieModel::SortRole).toString(),
             QLatin1String("foo.com"));
    QCOMPARE(model.data(model.index(1, 3), CookieModel::SortRole).toBool(), true);
    QCOMPARE(model.data(model.index(2, 4), CookieModel::SortRole).toDateTime(), expires);

    // Out-of-range and font role.
    QCOMPARE(model.data(model.index(42, 0)), QVariant());
    QVERIFY(model.data(model.index(0, 0), Qt::FontRole).isValid());
}

// removeRows edits the jar itself, not just the view.
void tst_CookieModel::cookieModelRemoveRows()
{
    QWebEngineProfile profile;
    CookieJar jar(&profile);
    jar.setCookies(QList<QNetworkCookie>()
                   << makeCookie("a.com", "a")
                   << makeCookie("b.com", "b")
                   << makeCookie("c.com", "c"));

    CookieModel model(&jar);
    ModelTest tester(&model);
    QCOMPARE(model.rowCount(), 3);

    QVERIFY(model.removeRows(1, 1));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(jar.cookies().count(), 2);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QLatin1String("a.com"));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QLatin1String("c.com"));

    QVERIFY(!model.removeRows(0, 1, model.index(0, 0)));
    QVERIFY(model.removeRows(0, 2));
    QCOMPARE(model.rowCount(), 0);
    QCOMPARE(jar.cookies().count(), 0);
}

// cookiesChanged on the jar resets the model.
void tst_CookieModel::cookieModelCookiesChanged()
{
    QWebEngineProfile profile;
    CookieJar jar(&profile);
    CookieModel model(&jar);
    ModelTest tester(&model);

    jar.setCookies(QList<QNetworkCookie>() << makeCookie("a.com"));
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QLatin1String("a.com"));
}

void tst_CookieModel::exceptionsModel()
{
    QWebEngineProfile profile;
    CookieJar jar(&profile);
    jar.setAllowedCookies(QStringList() << QLatin1String("a.com"));
    jar.setBlockedCookies(QStringList() << QLatin1String("b.com"));
    jar.setAllowForSessionCookies(QStringList() << QLatin1String("c.com"));

    CookieExceptionsModel model(&jar);
    ModelTest tester(&model);

    QCOMPARE(model.columnCount(), 2);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(QModelIndex()), QVariant());
    QCOMPARE(model.data(model.index(42, 0)), QVariant());

    // Allowed rows come first, then blocked, then session.
    QCOMPARE(model.data(model.index(0, 0)).toString(), QLatin1String("a.com"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QLatin1String("Allow"));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QLatin1String("b.com"));
    QCOMPARE(model.data(model.index(1, 1)).toString(), QLatin1String("Block"));
    QCOMPARE(model.data(model.index(2, 0)).toString(), QLatin1String("c.com"));
    QCOMPARE(model.data(model.index(2, 1)).toString(), QLatin1String("Allow For Session"));

    QCOMPARE(model.headerData(0, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Website"));
    QCOMPARE(model.headerData(1, Qt::Horizontal, Qt::DisplayRole).toString(), QLatin1String("Rule"));
    QVERIFY(model.headerData(0, Qt::Horizontal, Qt::SizeHintRole).isValid());
    QVERIFY(model.data(model.index(0, 0), Qt::FontRole).isValid());
}

void tst_CookieModel::exceptionsAddRule_data()
{
    QTest::addColumn<QString>("host");
    QTest::addColumn<CookieJar::CookieRule>("rule");
    QTest::addColumn<int>("allowed");
    QTest::addColumn<int>("blocked");
    QTest::addColumn<int>("session");

    QTest::newRow("empty") << QString() << CookieJar::Allow << 0 << 0 << 0;
    QTest::newRow("allow") << "a.com" << CookieJar::Allow << 1 << 0 << 0;
    QTest::newRow("block") << "a.com" << CookieJar::Block << 0 << 1 << 0;
    QTest::newRow("session") << "a.com" << CookieJar::AllowForSession << 0 << 0 << 1;
}

Q_DECLARE_METATYPE(CookieJar::CookieRule)
// void addRule(QString host, CookieJar::CookieRule rule)
void tst_CookieModel::exceptionsAddRule()
{
    QFETCH(QString, host);
    QFETCH(CookieJar::CookieRule, rule);
    QFETCH(int, allowed);
    QFETCH(int, blocked);
    QFETCH(int, session);

    QWebEngineProfile profile;
    CookieJar jar(&profile);
    CookieExceptionsModel model(&jar);
    ModelTest tester(&model);
    model.addRule(host, rule);
    QCOMPARE(model.rowCount(), allowed + blocked + session);
}

// addRule keeps a host in exactly one list, and treats "x.com" and
// ".x.com" as the same rule.
void tst_CookieModel::exceptionsRemoveRows()
{
    QWebEngineProfile profile;
    CookieJar jar(&profile);
    CookieExceptionsModel model(&jar);
    ModelTest tester(&model);

    // Re-adding under a different rule moves the host between lists.
    model.addRule(QLatin1String("a.com"), CookieJar::Allow);
    model.addRule(QLatin1String("a.com"), CookieJar::Block);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 1)).toString(), QLatin1String("Block"));

    // The dotted twin replaces, it does not duplicate.
    model.addRule(QLatin1String(".a.com"), CookieJar::Allow);
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QLatin1String(".a.com"));
    QCOMPARE(model.data(model.index(0, 1)).toString(), QLatin1String("Allow"));

    // Rows: [allow] [block] [session] — removing a middle block row
    // must shift the session row down.
    model.addRule(QLatin1String("s.com"), CookieJar::AllowForSession);
    model.addRule(QLatin1String("b.com"), CookieJar::Block);
    QCOMPARE(model.rowCount(), 3);
    QCOMPARE(model.data(model.index(2, 0)).toString(), QLatin1String("s.com"));

    QVERIFY(model.removeRows(1, 1));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(model.data(model.index(0, 0)).toString(), QLatin1String(".a.com"));
    QCOMPARE(model.data(model.index(1, 0)).toString(), QLatin1String("s.com"));

    QVERIFY(!model.removeRows(0, 1, model.index(0, 0)));
    QVERIFY(model.removeRows(0, 2));
    QCOMPARE(model.rowCount(), 0);
}

QTEST_MAIN(tst_CookieModel)
#include "tst_cookiemodel.moc"
