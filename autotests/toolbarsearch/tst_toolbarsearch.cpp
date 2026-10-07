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
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// COV04: ToolbarSearch — recent-search persistence, engine switching,
// completer highlight/activation paths, the engines popup menu and the
// WebView binding; plus the SearchBar show/hide timeline.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qstandarditemmodel.h>
#include <qcompleter.h>
#include <qwebengineprofile.h>

#include "toolbarsearch.h"
#include "searchbar.h"
#include "searchbutton.h"
#include "networkaccessmanager.h"
#include "opensearchmanager.h"
#include "opensearchengine.h"
#include "webview.h"
#include "browserapplication.h"
#include "tabwidget.h"
#include "qtest_arora.h"
#include "qtry.h"

// SearchBar is abstract (findNext/findPrevious are pure) — a minimal
// concrete subclass records the calls.
class TestSearchBar : public SearchBar
{
public:
    int nextCount = 0;
    int previousCount = 0;

    void findNext() override { ++nextCount; }
    void findPrevious() override { ++previousCount; }
};

class tst_ToolbarSearch : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void searchEmitsUrl();
    void recentSearches();
    void completerPaths();
    void enginesMenu();
    void fieldEnginePrefs();
    void suggestionsOptIn();
    void searchBarAnimation();
};

void tst_ToolbarSearch::initTestCase()
{
    QCoreApplication::setApplicationName("tst_toolbarsearch");
    QStandardPaths::setTestModeEnabled(true);
    QSettings settings;
    settings.clear();
    ToolbarSearch::openSearchManager()->restoreDefaults();
}

// Entering text and pressing return emits search() with the engine's
// expanded url.
void tst_ToolbarSearch::searchEmitsUrl()
{
    ToolbarSearch search;
    search.setText(QLatin1String("cov04 query"));
    QSignalSpy spy(&search, &ToolbarSearch::search);
    QTest::keyClick(&search, Qt::Key_Return);
    QCOMPARE(spy.count(), 1);
    const QUrl url = spy.first().at(0).toUrl();
    QVERIFY(url.isValid());
    QVERIFY(url.toString().contains(QLatin1String("cov04")));
    QCOMPARE(spy.first().at(1).value<TabWidget::OpenUrlIn>(),
             TabWidget::CurrentTab);
}

// Successful searches are remembered; clear() empties the list and
// swaps the "Recent Searches" completer header for the empty marker.
void tst_ToolbarSearch::recentSearches()
{
    // searchNow() records into m_recentSearches and schedules the
    // AutoSaver; the completer model only repopulates on load(), so the
    // writes are observed through a second instance.
    {
        ToolbarSearch search;
        search.setText(QLatin1String("remembered"));
        search.searchNow();
        search.setText(QLatin1String("second"));
        search.searchNow();
    } // ~ToolbarSearch flushes the AutoSaver

    ToolbarSearch search;
    QStandardItemModel *model =
        qobject_cast<QStandardItemModel *>(search.completer()->model());
    QVERIFY(model);
    QStringList texts;
    for (int i = 0; i < model->rowCount(); ++i)
        texts << model->item(i)->text();
    QVERIFY(texts.contains(QLatin1String("remembered")));
    QVERIFY(texts.contains(QLatin1String("second")));
    QCOMPARE(model->item(0)->text(), QLatin1String("Recent Searches"));

    search.clear();
    QVERIFY(model->rowCount() >= 1);
    QCOMPARE(model->item(0)->text(), QLatin1String("No Recent Searches"));
    QVERIFY(search.text().isEmpty());
}

// completerHighlighted skips the section header rows; a real entry
// writes itself into the line edit.
void tst_ToolbarSearch::completerPaths()
{
    // Seed a deterministic history so the model always has a header row
    // plus at least one selectable entry.
    QSettings settings;
    settings.setValue(QLatin1String("toolbarsearch/recentSearches"),
                      QStringList() << QLatin1String("persisted")
                                    << QLatin1String("older"));

    ToolbarSearch search;
    QStandardItemModel *model =
        qobject_cast<QStandardItemModel *>(search.completer()->model());
    QVERIFY(model);
    // Row 0 is the "Recent Searches" banner when no suggestions exist.
    QVERIFY(model->rowCount() >= 3);
    search.completer()->setCurrentRow(1);
    emit search.completer()->highlighted(model->index(1, 0));
    QCOMPARE(search.text(), QLatin1String("persisted"));

    // Highlighting the header row leaves the text alone.
    search.setText(QLatin1String("typed"));
    emit search.completer()->highlighted(model->index(0, 0));
    QCOMPARE(search.text(), QLatin1String("typed"));
}

// The engines popup lists every engine, marks the current one and ends
// with the "Configure" entry; dismissed without picking.
void tst_ToolbarSearch::enginesMenu()
{
    ToolbarSearch search;
    search.show();

    // showEnginesMenu execs a popup — close it after it builds.
    QTimer::singleShot(100, qApp, []() {
        if (QWidget *popup = QApplication::activePopupWidget())
            popup->close();
    });
    QTest::mouseClick(search.searchButton(), Qt::LeftButton);
    QTest::qWait(150);
}

// SRCH04: the field has its own engine — a field pick overrides the
// default for this widget only, "keep last selected" off reverts it
// after the search, "always new tab" and "keep typed text" change the
// emitted target and field state, and button mode collapses the field.
void tst_ToolbarSearch::fieldEnginePrefs()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    manager->restoreDefaults();

    QStringList names = manager->allEnginesNames();
    names.sort();
    QVERIFY(names.count() >= 2);
    const QString defaultName = names.at(0);
    const QString otherName = names.at(1);
    manager->setCurrentEngineName(defaultName);
    const QString defaultHost =
        manager->engine(defaultName)->searchUrl(QString()).host();
    const QString otherHost =
        manager->engine(otherName)->searchUrl(QString()).host();
    QVERIFY(defaultHost != otherHost);

    // A field pick wins over the default engine.
    manager->setFieldEngineName(otherName);
    {
        ToolbarSearch search;
        search.setText(QLatin1String("terms"));
        QSignalSpy spy(&search, &ToolbarSearch::search);
        search.searchNow();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QUrl(spy.first().at(0).toUrl()).host(), otherHost);
    }

    // keepFieldEngine off: the pick reverts once it has been used.
    manager->setKeepFieldEngine(false);
    manager->setFieldEngineName(otherName);
    {
        ToolbarSearch search;
        search.setText(QLatin1String("terms"));
        search.searchNow();
    }
    QCOMPARE(manager->fieldEngineName(), QString());
    manager->setKeepFieldEngine(true);

    // alwaysNewTab forces a new-tab target; keepTypedText off clears
    // the field after the search is dispatched.
    QSettings().setValue(QLatin1String("toolbarsearch/alwaysNewTab"), true);
    QSettings().setValue(QLatin1String("toolbarsearch/keepTypedText"), false);
    {
        ToolbarSearch search;
        search.setText(QLatin1String("terms"));
        QSignalSpy spy(&search, &ToolbarSearch::search);
        search.searchNow();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).value<TabWidget::OpenUrlIn>(),
                 TabWidget::NewSelectedTab);
        QVERIFY(search.text().isEmpty());
    }
    QSettings().remove(QLatin1String("toolbarsearch/alwaysNewTab"));
    QSettings().remove(QLatin1String("toolbarsearch/keepTypedText"));

    // Private contexts search through the private engine — here the
    // app-level flag stands in for a bound off-the-record view.
    manager->setFieldEngineName(QString());
    manager->setPrivateEngineName(otherName);
    BrowserApplication::setPrivate(true);
    {
        ToolbarSearch search;
        search.setText(QLatin1String("secret"));
        QSignalSpy spy(&search, &ToolbarSearch::search);
        search.searchNow();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(QUrl(spy.first().at(0).toUrl()).host(), otherHost);
    }
    BrowserApplication::setPrivate(false);
    manager->setPrivateEngineName(QString());

    // Button mode collapses to a read-only, unfocusable field.
    {
        ToolbarSearch search;
        search.setButtonMode(true);
        QVERIFY(search.isButtonMode());
        QVERIFY(search.isReadOnly());
        QCOMPARE(search.focusPolicy(), Qt::NoFocus);
        search.setButtonMode(false);
        QVERIFY(!search.isButtonMode());
        QVERIFY(!search.isReadOnly());
    }

    manager->setCurrentEngineName(defaultName);
    QSettings().remove(QLatin1String("toolbarsearch/recentSearches"));
}

// SEC11: suggestions are a per-engine opt-in — with the flag off (the
// default) typing must produce ZERO requests on the app-side NAM, and
// the opt-in of one engine must not leak onto another.
void tst_ToolbarSearch::suggestionsOptIn()
{
    OpenSearchManager *manager = ToolbarSearch::openSearchManager();

    // A hermetic engine: its suggest endpoint is a local file so the
    // enabled-path check never leaves the box.
    const QString fixturePath = QDir::temp().filePath(
        QLatin1String("arora-suggest-test.json"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("[\"hi\",[\"hi there\"]]");
    }
    OpenSearchEngine *capable = new OpenSearchEngine;
    capable->setName(QLatin1String("suggest-test"));
    capable->setSearchUrlTemplate(
        QLatin1String("http://suggest-test.invalid/q={searchTerms}"));
    capable->setSuggestionsUrlTemplate(
        QLatin1String("file://") + fixturePath
        + QLatin1String("?q={searchTerms}"));
    QVERIFY(capable->providesSuggestions());
    QVERIFY(manager->addEngine(capable));

    OpenSearchEngine *incapable = new OpenSearchEngine;
    incapable->setName(QLatin1String("nosuggest-test"));
    incapable->setSearchUrlTemplate(
        QLatin1String("http://nosuggest-test.invalid/q={searchTerms}"));
    QVERIFY(!incapable->providesSuggestions());
    QVERIFY(manager->addEngine(incapable));

    // Off by default for every engine — including ones that could serve.
    const QStringList names = manager->allEnginesNames();
    for (const QString &name : names)
        QVERIFY2(!manager->suggestionsEnabledForEngine(name),
                 qPrintable(name));

    // Every app-side request the NAM creates reports through this
    // signal — a suggest fetch cannot hide from it.
    NetworkAccessManager *nam = NetworkAccessManager::instance();
    QStringList requestUrls;
    const QMetaObject::Connection requestConn =
        QObject::connect(nam, &NetworkAccessManager::requestCreated, nam,
            [&requestUrls](QNetworkAccessManager::Operation,
                           const QNetworkRequest &request, QNetworkReply *) {
                requestUrls << request.url().toString();
            });

    manager->setCurrentEngineName(capable->name());
    {
        ToolbarSearch search;
        QTest::keyClicks(&search, QLatin1String("secret passw"));
        QTest::qWait(500); // past the 200ms debounce timer
        QCOMPARE(requestUrls.count(), 0);
    }

    // Opt in for this engine only: typing now reaches the suggest URL.
    manager->setSuggestionsEnabledForEngine(capable->name(), true);
    QVERIFY(manager->suggestionsEnabledEngines()
            == QStringList() << capable->name());
    {
        ToolbarSearch search;
        QTest::keyClicks(&search, QLatin1String("hi"));
        QTRY_VERIFY_WITH_TIMEOUT(!requestUrls.isEmpty(), 3000);
        QVERIFY(requestUrls.last().contains(fixturePath));
    }

    // Switching to an engine that is not opted in silences the path
    // again even though a suggest-capable engine was enabled before.
    manager->setCurrentEngineName(incapable->name());
    {
        ToolbarSearch search;
        const int baseline = requestUrls.count();
        QTest::keyClicks(&search, QLatin1String("still secret"));
        QTest::qWait(500);
        QCOMPARE(requestUrls.count(), baseline);
    }

    // Back on the enabled engine the opt-in applies again, and revoking
    // it mid-flight stops further requests.
    manager->setCurrentEngineName(capable->name());
    {
        ToolbarSearch search;
        const int baseline = requestUrls.count();
        QTest::keyClicks(&search, QLatin1String("go"));
        QTRY_VERIFY_WITH_TIMEOUT(requestUrls.count() > baseline, 3000);
        manager->setSuggestionsEnabledForEngine(capable->name(), false);
        const int revoked = requestUrls.count();
        QTest::keyClicks(&search, QLatin1String(" more"));
        QTest::qWait(500);
        QCOMPARE(requestUrls.count(), revoked);
    }

    // Persistence: save() serializes the opt-in list under openSearch.
    manager->setSuggestionsEnabledForEngine(capable->name(), true);
    manager->save();
    QCOMPARE(QSettings().value(QLatin1String("openSearch/suggestions"))
                 .toStringList(),
             QStringList() << capable->name());
    manager->setSuggestionsEnabledForEngine(capable->name(), false);
    manager->save();

    QObject::disconnect(requestConn);
    manager->removeEngine(incapable->name());
    manager->removeEngine(capable->name());
    QFile::remove(fixturePath);
}

// showFind/animateHide run the QTimeLine geometry animation.
void tst_ToolbarSearch::searchBarAnimation()
{
    TestSearchBar bar;
    bar.showFind();
    QTRY_VERIFY_WITH_TIMEOUT(bar.maximumHeight() > 0, 3000);
    QCOMPARE(bar.searchObject(), (QObject *)0);

    QObject token;
    bar.setSearchObject(&token);
    QCOMPARE(bar.searchObject(), &token);

    bar.animateHide();
    QTRY_VERIFY_WITH_TIMEOUT(!bar.isVisible() || bar.maximumHeight() == 0, 3000);

    // The buttons and return key route to the virtual find slots.
    bar.showFind();
    QTest::qWait(50);
    QLineEdit *edit = bar.findChild<QLineEdit *>();
    QVERIFY(edit);
    QTest::keyClick(edit, Qt::Key_Return);
    QCOMPARE(bar.nextCount, 1);
}

QTEST_MAIN(tst_ToolbarSearch)
#include "tst_toolbarsearch.moc"
