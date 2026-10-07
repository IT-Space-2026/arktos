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

#include <memory>

#include "qtest_arora.h"

#include "opensearchengine.h"
#include "opensearchmanager.h"

class tst_OpenSearchManager : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void addRemoveEngine_data();
    void addRemoveEngine();
    void setCurrentEngine_data();
    void setCurrentEngine();
    void generateEngineFileName_data();
    void generateEngineFileName();
    void restoreDefaults();
    void keywords();
    void contextEngines();
    void convertKeywordSearchToUrl();
    void convertKeywordSearchToUrl_data();
};

class SubOpenSearchManager : public OpenSearchManager
{
public:
    QString generateEngineFileName(const QString &engineName)
    {
        return OpenSearchManager::generateEngineFileName(engineName);
    }

    static int defaultCount()
    {
        return QDir(":/searchengines/").count();
    }
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_OpenSearchManager::initTestCase()
{
    QCoreApplication::setApplicationName("opensearchtest");

    // Persisted keywords from an earlier run would leak into the
    // assertions below; start from a clean settings slate.
    QSettings settings;
    settings.clear();

    SubOpenSearchManager manager;
    for (const QString &name : manager.allEnginesNames())
        manager.removeEngine(name);
    QCOMPARE(manager.enginesCount(), 1);
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_OpenSearchManager::cleanupTestCase()
{
    SubOpenSearchManager manager;
    QCOMPARE(manager.enginesCount(), 1);
}

// This will be called before each test function is executed.
void tst_OpenSearchManager::init()
{
    SubOpenSearchManager manager;
    QCOMPARE(manager.enginesCount(), 1);
}

// This will be called after every test function.
void tst_OpenSearchManager::cleanup()
{
    SubOpenSearchManager manager;
    for (const QString &name : manager.allEnginesNames())
        manager.removeEngine(name);
    QCOMPARE(manager.enginesCount(), 1);
}

void tst_OpenSearchManager::addRemoveEngine_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("description");
    QTest::addColumn<QString>("searchUrlTemplate");
    QTest::addColumn<bool>("valid");

    QTest::newRow("valid") << "Foo" << "Bar" << "http://foobaz.bar" << true;
    QTest::newRow("invalid") << "Baz" << "Foo" << "" << false;
}

void tst_OpenSearchManager::addRemoveEngine()
{
    QFETCH(QString, name);
    QFETCH(QString, description);
    QFETCH(QString, searchUrlTemplate);
    QFETCH(bool, valid);

    SubOpenSearchManager manager;

    QSignalSpy signalSpy(&manager, SIGNAL(changed()));

    OpenSearchEngine *engine = new OpenSearchEngine();
    engine->setName(name);
    engine->setDescription(description);
    engine->setSearchUrlTemplate(searchUrlTemplate);
    // The manager only takes ownership of engines it accepts; keep a
    // guard for the invalid row so the test does not leak it.
    std::unique_ptr<OpenSearchEngine> engineGuard(engine);

    QCOMPARE(manager.enginesCount(), 1);
    QVERIFY(!manager.engineExists(name));

    bool result = manager.addEngine(engine);
    if (result)
        engineGuard.release();

    QCOMPARE(result, valid);
    QCOMPARE(manager.enginesCount(), (valid ? 2 : 1));
    QCOMPARE(manager.engineExists(name), valid);
    QCOMPARE(signalSpy.count(), (valid ? 1 : 0));

    manager.removeEngine(engine->name());

    QCOMPARE(manager.enginesCount(), 1);
    QVERIFY(!manager.engineExists(name));
    QCOMPARE(signalSpy.count(), (valid ? 2 : 0));
}

void tst_OpenSearchManager::setCurrentEngine_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("description");
    QTest::addColumn<QString>("searchUrlTemplate");
    QTest::addColumn<bool>("valid");

    QTest::newRow("valid") << "Foo" << "Bar" << "http://foobaz.bar" << true;
    QTest::newRow("invalid") << "Baz" << "Foo" << "" << false;
}

void tst_OpenSearchManager::setCurrentEngine()
{
    QFETCH(QString, name);
    QFETCH(QString, description);
    QFETCH(QString, searchUrlTemplate);
    QFETCH(bool, valid);

    SubOpenSearchManager manager;

    QCOMPARE(manager.enginesCount(), 1);

    QString oldCurrentEngineName = manager.currentEngineName();
    OpenSearchEngine *oldCurrentEngine = manager.currentEngine();

    QSignalSpy signalSpy(&manager, SIGNAL(currentEngineChanged()));

    OpenSearchEngine *engine = new OpenSearchEngine();
    engine->setName(name);
    engine->setDescription(description);
    engine->setSearchUrlTemplate(searchUrlTemplate);
    std::unique_ptr<OpenSearchEngine> engineGuard(engine);

    bool result = manager.addEngine(engine);
    if (result)
        engineGuard.release();
    QCOMPARE(result, valid);

    manager.setCurrentEngineName(name);
    QCOMPARE(manager.currentEngineName(), (valid ? name : oldCurrentEngineName));
    QCOMPARE(*manager.currentEngine(), (valid ? *engine : *oldCurrentEngine));
    QCOMPARE(signalSpy.count(), (valid ? 1 : 0));

    manager.removeEngine(engine->name());
    QCOMPARE(signalSpy.count(), (valid ? 2 : 0));
}

void tst_OpenSearchManager::generateEngineFileName_data()
{
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("fileName");

    QTest::newRow("simple") << "FooBar" << "FooBar.xml";
    QTest::newRow("with-spaces") << "Foo Bar" << "Foo_Bar.xml";
    QTest::newRow("with-special-chars") << ":Foo&Bar*Baz-" << "FooBarBaz.xml";
    QTest::newRow("with-special-chars-and-spaces") << ": Foo & Bar -" << "_Foo__Bar_.xml";
}

void tst_OpenSearchManager::generateEngineFileName()
{
    QFETCH(QString, name);
    QFETCH(QString, fileName);

    SubOpenSearchManager manager;

    QCOMPARE(manager.generateEngineFileName(name), fileName);
}

void tst_OpenSearchManager::restoreDefaults()
{
    SubOpenSearchManager manager;

    QCOMPARE(manager.enginesCount(), 1);
    manager.restoreDefaults();
    QCOMPARE(manager.enginesCount(), manager.defaultCount());

    for (const QString &name : manager.allEnginesNames())
        manager.removeEngine(name);

    // Never let the manager have no engines.
    QCOMPARE(manager.enginesCount(), 1);

    OpenSearchEngine *engine = new OpenSearchEngine();
    engine->setName("Foobarbaz");
    engine->setSearchUrlTemplate("http://foobarbaz.baz");

    manager.addEngine(engine);
    manager.restoreDefaults();
    QCOMPARE(manager.enginesCount(), manager.defaultCount() + 1);

    manager.removeEngine(engine->name());
}

void tst_OpenSearchManager::keywords()
{
    {
        SubOpenSearchManager manager;

        QVERIFY(!manager.engineForKeyword("foo"));
        QVERIFY(!manager.engineForKeyword(QString()));

        manager.setEngineForKeyword("foo", nullptr);
        manager.setEngineForKeyword(QString(), nullptr);
        QVERIFY(!manager.engineForKeyword("foo"));
        QVERIFY(!manager.engineForKeyword(QString()));
        QCOMPARE(manager.keywordsForEngine(nullptr), QStringList());

        manager.setKeywordsForEngine(nullptr, QStringList() << "foo");
        QCOMPARE(manager.keywordsForEngine(nullptr), QStringList());

        manager.restoreDefaults();

        // m_engines is a QHash: keys() order depends on insertion
        // order (directory listing order vs resource order), which is
        // not stable between manager instances.  Pick deterministic
        // engines by sorted name instead.
        QStringList names = manager.allEnginesNames();
        names.sort();
        QVERIFY(names.count() >= 2);
        OpenSearchEngine *engine1 = manager.engine(names.at(0));
        OpenSearchEngine *engine2 = manager.engine(names.at(1));

        QCOMPARE(manager.keywordsForEngine(engine1), QStringList());
        QCOMPARE(manager.keywordsForEngine(engine2), QStringList());

        manager.setEngineForKeyword("foo", engine1);
        manager.setEngineForKeyword("bar", engine1);
        manager.setEngineForKeyword("baz", engine2);

        QCOMPARE(manager.engineForKeyword("foo"), engine1);
        QCOMPARE(manager.engineForKeyword("bar"), engine1);
        QCOMPARE(manager.engineForKeyword("baz"), engine2);

        // QHash::keys() order is unspecified; compare as sorted lists.
        QStringList keys1 = manager.keywordsForEngine(engine1);
        keys1.sort();
        QCOMPARE(keys1, QStringList() << "bar" << "foo");
        QCOMPARE(manager.keywordsForEngine(engine2), QStringList() << "baz");

        manager.setKeywordsForEngine(engine1, QStringList() << "baz");
        manager.setKeywordsForEngine(engine2, QStringList() << "foo" << "bar");

        QCOMPARE(manager.engineForKeyword("foo"), engine2);
        QCOMPARE(manager.engineForKeyword("bar"), engine2);
        QCOMPARE(manager.engineForKeyword("baz"), engine1);

        keys1 = manager.keywordsForEngine(engine2);
        keys1.sort();
        QCOMPARE(keys1, QStringList() << "bar" << "foo");
        QCOMPARE(manager.keywordsForEngine(engine1), QStringList() << "baz");
    }

    {
        SubOpenSearchManager manager;

        manager.restoreDefaults();

        QStringList names = manager.allEnginesNames();
        names.sort();
        QVERIFY(names.count() >= 2);
        OpenSearchEngine *engine1 = manager.engine(names.at(0));
        OpenSearchEngine *engine2 = manager.engine(names.at(1));

        QCOMPARE(*manager.engineForKeyword("foo"), *engine2);
        QCOMPARE(*manager.engineForKeyword("bar"), *engine2);
        QCOMPARE(*manager.engineForKeyword("baz"), *engine1);

        QStringList keys2 = manager.keywordsForEngine(engine2);
        keys2.sort();
        QCOMPARE(keys2, QStringList() << "bar" << "foo");
        QCOMPARE(manager.keywordsForEngine(engine1), QStringList() << "baz");

        manager.setEngineForKeyword("foo", nullptr);

        QVERIFY(!manager.engineForKeyword("foo"));
        QCOMPARE(*manager.engineForKeyword("bar"), *engine2);
        QCOMPARE(*manager.engineForKeyword("baz"), *engine1);

        QCOMPARE(manager.keywordsForEngine(engine2), QStringList() << "bar");
        QCOMPARE(manager.keywordsForEngine(engine1), QStringList() << "baz");

        manager.setKeywordsForEngine(engine1, QStringList());
        manager.setKeywordsForEngine(engine2, QStringList());
    }
}

// SRCH04: the per-context engine picks — private, image, search
// field — resolve with their documented fallbacks, persist through
// save()/load(), and are cleared when the engine they point at is
// removed.
void tst_OpenSearchManager::contextEngines()
{
    SubOpenSearchManager manager;
    manager.restoreDefaults();

    QStringList names = manager.allEnginesNames();
    names.sort();
    QVERIFY(names.count() >= 3);

    // An engine that offers image search (the bundled DuckDuckGo,
    // Google and Yahoo! do) and one that does not.
    QString imageName, plainName;
    for (const QString &name : names) {
        OpenSearchEngine *engine = manager.engine(name);
        if (engine->providesImageSearch() && imageName.isEmpty())
            imageName = name;
        if (!engine->providesImageSearch() && plainName.isEmpty())
            plainName = name;
    }
    QVERIFY(!imageName.isEmpty());
    QVERIFY(!plainName.isEmpty());
    const QString other = names.first() == imageName
        ? names.at(1) : names.first();

    manager.setCurrentEngineName(plainName);

    // Private context: unset falls back to the default engine;
    // unknown names are rejected.
    QCOMPARE(manager.privateEngineName(), QString());
    QCOMPARE(manager.engineForContext(true), manager.engine(plainName));
    QCOMPARE(manager.engineForContext(false), manager.engine(plainName));
    manager.setPrivateEngineName(imageName);
    QCOMPARE(manager.engineForContext(true), manager.engine(imageName));
    QCOMPARE(manager.engineForContext(false), manager.engine(plainName));
    manager.setPrivateEngineName(QLatin1String("does-not-exist"));
    QCOMPARE(manager.privateEngineName(), imageName);

    // Field engine: normal contexts honor the pick; private contexts
    // resolve to the private engine instead.
    QCOMPARE(manager.fieldEngineName(), QString());
    QCOMPARE(manager.searchFieldEngine(false), manager.engine(plainName));
    manager.setFieldEngineName(other);
    QCOMPARE(manager.searchFieldEngine(false), manager.engine(other));
    QCOMPARE(manager.searchFieldEngine(true), manager.engine(imageName));

    // "Keep last selected" off drops the stored field pick.
    QVERIFY(manager.keepFieldEngine());
    manager.setKeepFieldEngine(false);
    QCOMPARE(manager.fieldEngineName(), QString());
    QCOMPARE(manager.searchFieldEngine(false), manager.engine(plainName));

    // Image engine: the default engine lacks an image endpoint here,
    // so an unset pick resolves to nothing; once configured the
    // capable engine answers.
    QVERIFY(!manager.imageSearchEngine());
    manager.setImageEngineName(imageName);
    QCOMPARE(manager.imageSearchEngine(), manager.engine(imageName));
    // An image-incapable pick cannot serve even when stored — the
    // default engine (plainName, itself incapable) gives no fallback.
    manager.setImageEngineName(plainName);
    QVERIFY(!manager.imageSearchEngine());
    manager.setImageEngineName(imageName);

    // Suggestion-context toggles: defaults on/on/off.
    QVERIFY(manager.suggestionsInAddressField());
    QVERIFY(manager.suggestionsInSearchField());
    QVERIFY(!manager.suggestionsOnlyWithKeyword());
    manager.setSuggestionsInAddressField(false);
    manager.setSuggestionsOnlyWithKeyword(true);

    // Persistence: everything above survives a save/load cycle.
    manager.save();
    {
        SubOpenSearchManager reloaded;
        QCOMPARE(reloaded.privateEngineName(), imageName);
        QCOMPARE(reloaded.imageEngineName(), imageName);
        QVERIFY(!reloaded.suggestionsInAddressField());
        QVERIFY(reloaded.suggestionsInSearchField());
        QVERIFY(reloaded.suggestionsOnlyWithKeyword());
        // keepFieldEngine was left off — the stored field pick is gone.
        QVERIFY(!reloaded.keepFieldEngine());
        QCOMPARE(reloaded.fieldEngineName(), QString());
    }

    // Removing the engine clears the configured picks.
    manager.removeEngine(imageName);
    QCOMPARE(manager.privateEngineName(), QString());
    QCOMPARE(manager.imageEngineName(), QString());

    // Restore defaults so later tests see a clean slate.
    manager.setKeepFieldEngine(true);
    manager.setSuggestionsInAddressField(true);
    manager.setSuggestionsOnlyWithKeyword(false);
    manager.save();
}

void tst_OpenSearchManager::convertKeywordSearchToUrl_data()
{
    QTest::addColumn<QString>("string");
    QTest::addColumn<bool>("valid");

    QTest::newRow("invalid-0") << "null" << false;
    QTest::newRow("invalid-1") << "foo" << false;
    QTest::newRow("invalid-2") << "bar" << false;
    QTest::newRow("invalid-3") << "baz" << false;
    QTest::newRow("invalid-4") << "foo " << false;
    QTest::newRow("invalid-5") << "foobar" << false;
    QTest::newRow("valid-0") << "foo searchstring" << true;
}

void tst_OpenSearchManager::convertKeywordSearchToUrl()
{
    QFETCH(QString, string);
    QFETCH(bool, valid);

    SubOpenSearchManager manager;
    manager.restoreDefaults();
    QStringList names = manager.allEnginesNames();
    names.sort();
    QVERIFY(names.count() >= 2);
    OpenSearchEngine *engine1 = manager.engine(names.at(0));
    manager.setEngineForKeyword("foo", engine1);
    manager.setEngineForKeyword("bar", engine1);
    OpenSearchEngine *engine2 = manager.engine(names.at(1));
    manager.setEngineForKeyword("baz", engine2);

    QCOMPARE(manager.convertKeywordSearchToUrl(string).isValid(), valid);
}

QTEST_MAIN(tst_OpenSearchManager)

#include "tst_opensearchmanager.moc"

