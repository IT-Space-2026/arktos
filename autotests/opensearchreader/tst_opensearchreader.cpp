/*
 * Copyright 2009 Jakub Wieczorek <faw217@gmail.com>
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

#include "opensearchreader.h"
#include "opensearchengine.h"

class tst_OpenSearchReader : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void read_data();
    void read();
    void imageSearch();
    void hostileInput();
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_OpenSearchReader::initTestCase()
{
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_OpenSearchReader::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_OpenSearchReader::init()
{
}

// This will be called after every test function.
void tst_OpenSearchReader::cleanup()
{
}

Q_DECLARE_METATYPE(OpenSearchEngine::Parameters)
void tst_OpenSearchReader::read_data()
{
    QTest::addColumn<QString>("fileName");
    QTest::addColumn<bool>("valid");
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("description");
    QTest::addColumn<QString>("searchUrlTemplate");
    QTest::addColumn<QString>("suggestionsUrlTemplate");
    QTest::addColumn<QString>("imageUrl");
    QTest::addColumn<OpenSearchEngine::Parameters>("searchParameters");
    QTest::addColumn<OpenSearchEngine::Parameters>("suggestionsParameters");
    QTest::addColumn<QString>("searchMethod");
    QTest::addColumn<QString>("suggestionsMethod");

    QTest::newRow("null") << QString(":/doesNotExist") << false << QString() << QString() << QString() << QString()
            << QString() << OpenSearchEngine::Parameters() << OpenSearchEngine::Parameters() << QString("get") << QString("get");

    QTest::newRow("testfile1") << QString(":/testfile1.xml") << true << QString("Wikipedia (en)")
            << QString("Full text search in the English Wikipedia") << QString("http://en.wikipedia.org/bar")
            << QString("http://en.wikipedia.org/foo") << QString("http://en.wikipedia.org/favicon.ico")
            << OpenSearchEngine::Parameters() << OpenSearchEngine::Parameters() << QString("post") << QString("get");

    QTest::newRow("testfile2") << QString(":/testfile2.xml") << false << QString("Wikipedia (en)")
            << QString() << QString() << QString("http://en.wikipedia.org/foo") << QString("http://en.wikipedia.org/favicon.ico")
            << OpenSearchEngine::Parameters() << OpenSearchEngine::Parameters() << QString("get") << QString("get");

    QTest::newRow("testfile3") << QString(":/testfile3.xml") << true << QString("GitHub") << QString("Search GitHub")
            << QString("http://github.com/search") << QString("http://github.com/suggestions") << QString()
            << (OpenSearchEngine::Parameters() << OpenSearchEngine::Parameter(QString("q"), QString("{searchTerms}"))
                                               << OpenSearchEngine::Parameter(QString("b"), QString("foo")))
            << (OpenSearchEngine::Parameters() << OpenSearchEngine::Parameter(QString("bar"), QString("baz")))
            << QString("get") << QString("post");

    QTest::newRow("testfile4") << QString(":/testfile4.xml") << true << QString("Google") << QString("Google Web Search")
            << QString("http://www.google.com/search?bar") << QString("http://suggestqueries.google.com/complete/foo")
            << QString("http://www.google.com/favicon.ico") << OpenSearchEngine::Parameters()
            << OpenSearchEngine::Parameters() << QString("get") << QString("get");

    QTest::newRow("testfile5") << QString(":/testfile5.xml") << false << QString() << QString() << QString() << QString()
            << QString() << OpenSearchEngine::Parameters() << OpenSearchEngine::Parameters() << QString("get") << QString("get");

    QTest::newRow("testfile6") << QString(":/testfile6.xml") << false << QString() << QString() << QString() << QString()
            << QString() << OpenSearchEngine::Parameters() << OpenSearchEngine::Parameters()  << QString("get") << QString("get");

    QTest::newRow("testfile7") << QString(":/testfile7.xml") << false << QString() << QString() << QString() << QString()
            << QString() << OpenSearchEngine::Parameters() << OpenSearchEngine::Parameters()  << QString("get") << QString("get");
}

void tst_OpenSearchReader::read()
{
    QFETCH(QString, fileName);
    QFETCH(bool, valid);
    QFETCH(QString, name);
    QFETCH(QString, description);
    QFETCH(QString, searchUrlTemplate);
    QFETCH(QString, suggestionsUrlTemplate);
    QFETCH(QString, imageUrl);
    QFETCH(OpenSearchEngine::Parameters, searchParameters);
    QFETCH(OpenSearchEngine::Parameters, suggestionsParameters);
    QFETCH(QString, searchMethod);
    QFETCH(QString, suggestionsMethod);

    QFile file(fileName);
    static_cast<void>(file.open(QIODevice::ReadOnly));
    OpenSearchReader reader;
    OpenSearchEngine *engine = reader.read(&file);

    QCOMPARE(engine->isValid(), valid);
    QCOMPARE(engine->name(), name);
    QCOMPARE(engine->description(), description);
    QCOMPARE(engine->searchUrlTemplate(), searchUrlTemplate);
    QCOMPARE(engine->suggestionsUrlTemplate(), suggestionsUrlTemplate);
    QCOMPARE(engine->searchParameters(), searchParameters);
    QCOMPARE(engine->suggestionsParameters(), suggestionsParameters);
    QCOMPARE(engine->imageUrl(), imageUrl);
    QCOMPARE(engine->searchMethod(), searchMethod);
    QCOMPARE(engine->suggestionsMethod(), suggestionsMethod);

    delete engine;
}

// SRCH04: a <Url purpose="image"> element carries the engine's
// image-search endpoint — its own template, parameters and method.
// Placing it AFTER the <Image> favicon also proves the early-exit
// check does not stop before it is reached.
void tst_OpenSearchReader::imageSearch()
{
    QByteArray doc =
        "<OpenSearchDescription xmlns='http://a9.com/-/spec/opensearch/1.1/'>"
        "<ShortName>Img</ShortName>"
        "<Description>image capable</Description>"
        "<Url type='text/html' method='get' template='http://img.test/search?q={searchTerms}'/>"
        "<Url type='application/x-suggestions+json' method='get' template='http://img.test/suggest?q={searchTerms}'/>"
        "<Image width='16' height='16'>http://img.test/favicon.ico</Image>"
        "<Url type='text/html' purpose='image' method='get' template='http://img.test/images'>"
        "<Param name='url' value='{searchTerms}'/>"
        "</Url>"
        "</OpenSearchDescription>";
    QBuffer buffer(&doc);
    QVERIFY(buffer.open(QIODevice::ReadOnly));
    OpenSearchReader reader;
    OpenSearchEngine *engine = reader.read(&buffer);
    QVERIFY(engine);
    QVERIFY(engine->isValid());
    QVERIFY(!reader.hasError());

    QVERIFY(engine->providesImageSearch());
    QCOMPARE(engine->imageSearchUrlTemplate(),
             QStringLiteral("http://img.test/images"));
    QCOMPARE(engine->imageSearchMethod(), QStringLiteral("get"));
    QCOMPARE(engine->imageSearchParameters(),
             OpenSearchEngine::Parameters()
                 << OpenSearchEngine::Parameter(QStringLiteral("url"),
                                                QStringLiteral("{searchTerms}")));
    // Parameter values run through the same template+query pipeline
    // as ordinary search parameters.
    QCOMPARE(engine->imageSearchUrl(QStringLiteral("kitten")).toString(),
             QStringLiteral("http://img.test/images?url=kitten"));

    // The regular search and favicon fields are untouched.
    QCOMPARE(engine->searchUrlTemplate(),
             QStringLiteral("http://img.test/search?q={searchTerms}"));
    QCOMPARE(engine->imageUrl(),
             QStringLiteral("http://img.test/favicon.ico"));
    delete engine;

    // An engine without an image endpoint reports no support.
    engine = new OpenSearchEngine;
    QVERIFY(!engine->providesImageSearch());
    QVERIFY(!engine->imageSearchUrl(QStringLiteral("x")).isValid());
    delete engine;
}

// Hostile/malformed input: the parser must terminate and report an
// error on every case (SEC04).
void tst_OpenSearchReader::hostileInput()
{
    // Truncated inside <Url> — before the atEnd() guards this spun
    // forever on the exhausted reader.
    {
        QByteArray doc =
            "<OpenSearchDescription xmlns='http://a9.com/-/spec/opensearch/1.1/'>"
            "<Url type='text/html' template='http://example.com/?q={searchTerms}'>";
        QBuffer buffer(&doc);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        OpenSearchReader reader;
        OpenSearchEngine *engine = reader.read(&buffer);
        QVERIFY(engine);
        QVERIFY(!engine->isValid());
        QVERIFY(reader.hasError());
        delete engine;
    }

    // A DTD is never legitimate in an OpenSearch description — it is
    // rejected before any entity handling comes into play.
    {
        QByteArray doc =
            "<!DOCTYPE d [ <!ENTITY a 'x'> ]>"
            "<OpenSearchDescription xmlns='http://a9.com/-/spec/opensearch/1.1/'>"
            "<ShortName>x</ShortName>"
            "<Url template='http://example.com/?q={searchTerms}'/>"
            "</OpenSearchDescription>";
        QBuffer buffer(&doc);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        OpenSearchReader reader;
        OpenSearchEngine *engine = reader.read(&buffer);
        QVERIFY(reader.hasError());
        delete engine;
    }

    // Oversized input is refused before it is parsed.
    {
        QByteArray doc(1024 * 1024 + 8, 'x');
        QBuffer buffer(&doc);
        QVERIFY(buffer.open(QIODevice::ReadOnly));
        OpenSearchReader reader;
        OpenSearchEngine *engine = reader.read(&buffer);
        QVERIFY(reader.hasError());
        delete engine;
    }

    // An unreadable device reports an error instead of silently
    // returning a half-parsed engine.
    {
        QBuffer buffer;
        OpenSearchReader reader;
        OpenSearchEngine *engine = reader.read(&buffer);
        QVERIFY(reader.hasError());
        delete engine;
    }
}

QTEST_MAIN(tst_OpenSearchReader)

#include "tst_opensearchreader.moc"

