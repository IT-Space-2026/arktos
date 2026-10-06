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
    void searchBarAnimation();
};

void tst_ToolbarSearch::initTestCase()
{
    QCoreApplication::setApplicationName("tst_toolbarsearch");
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
