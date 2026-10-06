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

// COV04: the history UI stack — HistoryTreeModel/HistoryMenuModel
// proxies, the HistoryDialog (tree + context menu + copy/open), and the
// HistoryCompleter/HistoryCompletionModel pair used by the location bar.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qcompleter.h>
#include <qmenu.h>
#include <qclipboard.h>
#include <qapplication.h>

#include "historymanager.h"
#include "history.h"
#include "historycompleter.h"
#include "edittreeview.h"
#include "modeltest.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_HistoryUi : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void treeModel();
    void menuModel();
    void filterModel();
    void completionModel();
    void completer();
    void historyDialog();
};

void tst_HistoryUi::initTestCase()
{
    QCoreApplication::setApplicationName("tst_historyui");
    QSettings settings;
    settings.clear();

    HistoryManager *manager = HistoryManager::instance();
    manager->clear();
    QList<HistoryEntry> entries;
    // Anchor to noon today: entries relative to "now" can cross the
    // day boundary (the test once ran at 00:01 and its 4-minute-old
    // entry landed in yesterday's group).
    const QDateTime todayNoon(QDate::currentDate(), QTime(12, 0));
    for (int i = 0; i < 5; ++i) {
        HistoryEntry entry;
        entry.url = QString::fromLatin1("http://ui%1.example.com/page%2")
                        .arg(i).arg(i);
        entry.title = QString::fromLatin1("UI Entry %1").arg(i);
        entry.dateTime = todayNoon.addSecs(-60 * i);
        entries << entry;
    }
    // And one from yesterday so the tree model has two date groups.
    HistoryEntry old;
    old.url = QLatin1String("http://old.example.com/");
    old.title = QLatin1String("Yesterday");
    old.dateTime = todayNoon.addDays(-1);
    entries << old;
    manager->setHistory(entries);
}

// HistoryTreeModel folds the flat list into one top-level node per day.
void tst_HistoryUi::treeModel()
{
    HistoryManager *manager = HistoryManager::instance();
    HistoryTreeModel *tree = manager->historyTreeModel();
    ModelTest tester(tree);
    Q_UNUSED(tester);

    QCOMPARE(tree->rowCount(), 2); // today + yesterday
    const QModelIndex today = tree->index(0, 0);
    QVERIFY(today.isValid());
    QVERIFY(tree->hasChildren(today));
    QVERIFY(tree->rowCount(today) >= 5);
    QVERIFY(!tree->data(today, Qt::DisplayRole).toString().isEmpty());
    QCOMPARE(tree->headerData(0, Qt::Horizontal).toString(),
             QLatin1String("Title"));

    const QModelIndex leaf = tree->index(0, 0, today);
    QVERIFY(!tree->hasChildren(leaf));
    QVERIFY(!tree->data(leaf, HistoryModel::UrlRole).toUrl().isEmpty());
    QVERIFY(tree->flags(leaf).testFlag(Qt::ItemIsEnabled));

    // mapToSource/mapFromSource round-trip on a leaf.
    const QModelIndex source = tree->mapToSource(leaf);
    QVERIFY(source.isValid());
    QCOMPARE(tree->mapFromSource(source), leaf);
}

void tst_HistoryUi::menuModel()
{
    HistoryManager *manager = HistoryManager::instance();
    HistoryMenuModel model(manager->historyTreeModel());

    QVERIFY(model.bumpedRows() >= 0);
    QCOMPARE(model.columnCount(QModelIndex()), 2);
    QVERIFY(model.rowCount() >= 1);

    const QModelIndex first = model.index(0, 0);
    QVERIFY(first.isValid());
    QCOMPARE(model.buddy(first), first);

    // mimeData carries each row's url (possibly empty for day groups).
    QMimeData *mime = model.mimeData(QModelIndexList() << first);
    QVERIFY(mime);
    delete mime;
}

void tst_HistoryUi::filterModel()
{
    HistoryManager *manager = HistoryManager::instance();
    HistoryFilterModel *filter = manager->historyFilterModel();
    ModelTest tester(filter);
    Q_UNUSED(tester);

    QVERIFY(filter->historyContains(QLatin1String("http://ui0.example.com/page0")));
    QVERIFY(filter->historyLocation(
                QLatin1String("http://ui0.example.com/page0")) >= 0);
    QCOMPARE(filter->historyLocation(
                 QLatin1String("http://missing.example.com/")), 0);

    // Frecency + display data through the proxy.
    QVERIFY(filter->rowCount() >= 6);
    const QModelIndex idx = filter->index(0, 0);
    QVERIFY(!idx.data(HistoryModel::UrlStringRole).toString().isEmpty());
    QVERIFY(idx.data(HistoryFilterModel::FrecencyRole).toInt() >= 0);
    filter->recalculateFrecencies();
}

void tst_HistoryUi::completionModel()
{
    HistoryManager *manager = HistoryManager::instance();
    HistoryCompletionModel model;
    model.setSourceModel(manager->historyFilterModel());

    model.setSearchString(QLatin1String("ui1"));
    QCOMPARE(model.searchString(), QLatin1String("ui1"));
    QVERIFY(model.rowCount() >= 1);
    // All filtered rows report the faked completion role once valid.
    model.setValid(true);
    QVERIFY(model.isValid());
    QCOMPARE(model.index(0, 0).data(HistoryCompletionModel::HistoryCompletionRole).toString(),
             QLatin1String("a"));
    // Column 1 renders in the light font.
    QCOMPARE(model.index(0, 1).data(Qt::FontRole).value<QFont>().weight(),
             QFont::Light);

    model.setSearchString(QLatin1String("nothing-matches-this-xyz"));
    QCOMPARE(model.rowCount(), 0);
}

void tst_HistoryUi::completer()
{
    HistoryManager *manager = HistoryManager::instance();
    // QCompleter does not take ownership of the model — parent it so it
    // does not leak past the test.
    HistoryCompletionModel *model = new HistoryCompletionModel(this);
    model->setSourceModel(manager->historyFilterModel());

    HistoryCompleter completer(model, this);
    // HistoryCompletionView has no Q_OBJECT, so className() reports the
    // base class; the table-view popup is what matters.
    QVERIFY(qobject_cast<QTableView *>(completer.popup()));

    // splitPath drives the deferred filter; pathFromIndex returns urls.
    completer.splitPath(QLatin1String("ui2"));
    QTRY_VERIFY_WITH_TIMEOUT(model->searchString() == QLatin1String("ui2"), 3000);
    const QModelIndex idx = model->index(0, 0);
    if (idx.isValid())
        QVERIFY(completer.pathFromIndex(idx).contains(QLatin1String("ui2")));
    QCOMPARE(completer.splitPath(QLatin1String("ui2")), QStringList() << QLatin1String("a"));
}

void tst_HistoryUi::historyDialog()
{
    HistoryManager *manager = HistoryManager::instance();
    HistoryDialog dialog(0, manager);
    dialog.show();

    QVERIFY(dialog.tree->model()->rowCount() >= 1);
    const QModelIndex day = dialog.tree->model()->index(0, 0);
    QVERIFY(day.isValid());
    dialog.tree->expand(day);
    const QModelIndex leaf = dialog.tree->model()->index(0, 0, day);
    QVERIFY(leaf.isValid());
    dialog.tree->setCurrentIndex(leaf);

    // open() emits openUrl for leaf nodes.
    QSignalSpy spy(&dialog, &HistoryDialog::openUrl);
    QMetaObject::invokeMethod(&dialog, "open");
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).toUrl().toString(),
             leaf.data(HistoryModel::UrlRole).toUrl().toString());

    // copy() puts the url on the clipboard.
    QMetaObject::invokeMethod(&dialog, "copy");
    QCOMPARE(QApplication::clipboard()->text(),
             leaf.data(HistoryModel::UrlStringRole).toString());

    // The context menu runs a nested exec — close the popup.
    QTimer::singleShot(100, qApp, []() {
        if (QWidget *popup = QApplication::activePopupWidget())
            popup->close();
    });
    const QPoint pos = dialog.tree->visualRect(leaf).center();
    QMetaObject::invokeMethod(&dialog, "customContextMenuRequested",
                              Q_ARG(QPoint, pos));
    QTest::qWait(150);

    // Search filtering narrows the proxy.
    dialog.search->setText(QLatin1String("ui3"));
    QTRY_VERIFY_WITH_TIMEOUT(dialog.tree->model()->rowCount() >= 1, 3000);

    // removeButton deletes the selected leaf.
    const int before = manager->history().count();
    dialog.search->setText(QString());
    const QModelIndex dayAgain = dialog.tree->model()->index(0, 0);
    dialog.tree->expand(dayAgain);
    const QModelIndex target = dialog.tree->model()->index(0, 0, dayAgain);
    QVERIFY(target.isValid());
    dialog.tree->setCurrentIndex(target);
    dialog.removeButton->click();
    QCOMPARE(manager->history().count(), before - 1);
}

QTEST_MAIN(tst_HistoryUi)
#include "tst_historyui.moc"
