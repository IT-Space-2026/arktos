/*
 * Copyright (c) 2026, The Arora Authors
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
 */

// POL01: UrlCleaner — the tracking-parameter stripper behind
// 'Copy Clean Link'.

#include <QtTest/QtTest>
#include <QtGui/QtGui>

#include <qsettings.h>
#include <qurlquery.h>

#include <urlcleaner.h>

#include "qtest_arora.h"

class tst_UrlCleaner : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanupTestCase();

    void cleanedUrl_data();
    void cleanedUrl();
    void matchingIsCaseInsensitive();
    void prefixEntries();
    void exactEntriesDoNotOvermatch();
    void additionalParameters();
    void settingsParameters();
};

void tst_UrlCleaner::init()
{
    // The blocklist reads the user key on every call — keep each test
    // hermetic.
    QSettings().remove(QLatin1String("urlcleaner/extraParams"));
    UrlCleaner::setAdditionalParameters(QStringList());
}

void tst_UrlCleaner::cleanupTestCase()
{
    QSettings().remove(QLatin1String("urlcleaner/extraParams"));
}

void tst_UrlCleaner::cleanedUrl_data()
{
    QTest::addColumn<QString>("url");
    QTest::addColumn<QString>("expected");

    QTest::newRow("utm-basic")
        << "https://example.com/?utm_source=news&id=42"
        << "https://example.com/?id=42";
    QTest::newRow("utm-all")
        << "https://example.com/p?utm_source=a&utm_medium=b&utm_campaign=c"
        << "https://example.com/p";
    QTest::newRow("param-only-drops-question-mark")
        << "https://example.com/page?utm_source=a"
        << "https://example.com/page";
    QTest::newRow("fragment-preserved")
        << "https://example.com/?utm_source=a&id=1#sec-2"
        << "https://example.com/?id=1#sec-2";
    QTest::newRow("fragment-only-trackers")
        << "https://example.com/?fbclid=zzz#frag"
        << "https://example.com/#frag";
    QTest::newRow("order-preserved")
        << "https://example.com/?b=1&utm_source=x&a=2&c=3"
        << "https://example.com/?b=1&a=2&c=3";
    QTest::newRow("duplicates-kept")
        << "https://example.com/?id=1&utm_medium=x&id=2"
        << "https://example.com/?id=1&id=2";
    QTest::newRow("click-ids")
        << "https://example.com/?gclid=G1&fbclid=F1&msclkid=M1&dclid=D1&ok=1"
        << "https://example.com/?ok=1";
    QTest::newRow("marketing-automation")
        << "https://example.com/?mc_cid=1&mc_eid=2&mkt_tok=3&_hsenc=4&k=5"
        << "https://example.com/?k=5";
    QTest::newRow("no-query")
        << "https://example.com/path#frag"
        << "https://example.com/path#frag";
    QTest::newRow("no-trackers")
        << "https://example.com/?id=42&page=2"
        << "https://example.com/?id=42&page=2";
    QTest::newRow("valueless-tracker")
        << "https://example.com/?utm_source&id=1"
        << "https://example.com/?id=1";
    QTest::newRow("percent-encoded-name")
        << "https://example.com/?utm%5Fsource=x&id=1"
        << "https://example.com/?id=1";
    // Values pass through decode→re-encode unchanged.
    QTest::newRow("encoded-value-kept")
        << "https://example.com/?q=a%20b&utm_term=x"
        << "https://example.com/?q=a%20b";
    QTest::newRow("host-port-auth-preserved")
        << "https://user:pw@example.com:8443/p?utm_source=x&ok=1"
        << "https://user:pw@example.com:8443/p?ok=1";
    QTest::newRow("ftp-not-cleaned")
        << "ftp://example.com/f?utm_source=x"
        << "ftp://example.com/f?utm_source=x";
    QTest::newRow("mailto-not-cleaned")
        << "mailto:a@b.c?utm_source=x&subject=hi"
        << "mailto:a@b.c?utm_source=x&subject=hi";
    QTest::newRow("data-not-cleaned")
        << "data:text/plain,utm_source=x"
        << "data:text/plain,utm_source=x";
    QTest::newRow("empty-url")
        << ""
        << "";
}

void tst_UrlCleaner::cleanedUrl()
{
    QFETCH(QString, url);
    QFETCH(QString, expected);
    // toEncoded sidesteps toString's display decoding ('%20' shows as
    // a literal space): the assertion is about the URL's content.
    QCOMPARE(QString::fromUtf8(UrlCleaner::cleanedUrl(QUrl(url)).toEncoded()),
             expected);
}

void tst_UrlCleaner::matchingIsCaseInsensitive()
{
    QCOMPARE(UrlCleaner::cleanedUrl(
        QUrl("https://example.com/?UTM_SOURCE=x&Gclid=y&ok=1")).toString(),
        QString("https://example.com/?ok=1"));
}

void tst_UrlCleaner::prefixEntries()
{
    // utm_* is a prefix entry: any utm_-prefixed name goes, while a
    // merely similar name (utmx, utm) stays.
    QCOMPARE(UrlCleaner::cleanedUrl(
        QUrl("https://example.com/?utm_anything=x&utm=y&utmx=z&ok=1"))
            .toString(),
        QString("https://example.com/?utm=y&utmx=z&ok=1"));
}

void tst_UrlCleaner::exactEntriesDoNotOvermatch()
{
    // 'si', 'ref_src' and 'trk' are exact entries — they must not eat
    // same-prefix application parameters.
    QCOMPARE(UrlCleaner::cleanedUrl(
        QUrl("https://example.com/?si=1&simple=2&ref_src=t&ref_source=3"
             "&trk=4&trkInfo=5")).toString(),
        QString("https://example.com/?simple=2&ref_source=3&trkInfo=5"));
}

void tst_UrlCleaner::additionalParameters()
{
    const QUrl url("https://example.com/?partner=acme&sessionid=s1&id=1");
    QCOMPARE(UrlCleaner::cleanedUrl(url), url);

    UrlCleaner::setAdditionalParameters(
        QStringList() << "partner" << "session*");
    QCOMPARE(UrlCleaner::cleanedUrl(url).toString(),
             QString("https://example.com/?id=1"));
}

void tst_UrlCleaner::settingsParameters()
{
    const QString key = QLatin1String("urlcleaner/extraParams");
    QSettings settings;
    const QVariant old = settings.value(key);
    settings.setValue(key, QStringList() << "campaign_id");

    const QUrl url("https://example.com/?campaign_id=c9&id=1");
    const QString cleaned = UrlCleaner::cleanedUrl(url).toString();

    if (old.isValid())
        settings.setValue(key, old);
    else
        settings.remove(key);

    QCOMPARE(cleaned, QString("https://example.com/?id=1"));
}

QTEST_MAIN(tst_UrlCleaner)
#include "tst_urlcleaner.moc"
