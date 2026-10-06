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

// COV04: the model-driven chrome widgets — ModelMenu (population,
// submenus, separators, activation, drag/drop entry points),
// BookmarksMenu/BookmarksMenuBarMenu and ModelToolBar/BookmarksToolBar.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qstandarditemmodel.h>
#include <qmimedata.h>
#include <qmenu.h>

#include "modelmenu.h"
#include "modeltoolbar.h"
#include "bookmarksmenu.h"
#include "bookmarkstoolbar.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "bookmarknode.h"
#include "historymanager.h"
#include "history.h"
#include "qtest_arora.h"
#include "qtry.h"

// Exposes the protected event handlers for direct dispatch.
class SubModelMenu : public ModelMenu
{
public:
    void call_createMenu(const QModelIndex &parent, int max,
                         QMenu *parentMenu = nullptr, QMenu *menu = nullptr)
        { return ModelMenu::createMenu(parent, max, parentMenu, menu); }
    bool call_prePopulated()
        { return ModelMenu::prePopulated(); }
    void call_postPopulated()
        { return ModelMenu::postPopulated(); }
    ModelMenu *call_createBaseMenu()
        { return ModelMenu::createBaseMenu(); }
};

class tst_ModelMenu : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void accessors();
    void population();
    void subMenus();
    void activation();
    void bookmarksMenu();
    void historyMenu();
    void modelToolBar();
    void bookmarksToolBar();
};

void tst_ModelMenu::initTestCase()
{
    QCoreApplication::setApplicationName("tst_modelmenu");
    QSettings settings;
    settings.clear();
}

void tst_ModelMenu::accessors()
{
    SubModelMenu menu;
    QVERIFY(!menu.model());

    QStandardItemModel model;
    menu.setModel(&model);
    QCOMPARE(menu.model(), &model);

    menu.setMaxRows(7);
    QCOMPARE(menu.maxRows(), 7);
    menu.setFirstSeparator(3);
    QCOMPARE(menu.firstSeparator(), 3);
    menu.setStatusBarTextRole(Qt::ToolTipRole);
    QCOMPARE(menu.statusBarTextRole(), (int)Qt::ToolTipRole);
    menu.setSeparatorRole(Qt::UserRole + 9);
    QCOMPARE(menu.separatorRole(), (int)(Qt::UserRole + 9));
    QVERIFY(!menu.rootIndex().isValid());
    QVERIFY(!menu.call_prePopulated());
    menu.call_postPopulated();
    QVERIFY(menu.call_createBaseMenu());
    QVERIFY(!menu.index(nullptr).isValid());
}

// aboutToShow rebuilds the menu from the model: flat rows become
// actions whose data() carries the QModelIndex.
void tst_ModelMenu::population()
{
    QStandardItemModel model;
    for (int i = 0; i < 4; ++i) {
        QStandardItem *item = new QStandardItem(QString::number(i));
        item->setToolTip(QLatin1String("tip"));
        model.appendRow(item);
    }

    ModelMenu menu;
    menu.setModel(&model);
    menu.setStatusBarTextRole(Qt::ToolTipRole);
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QCOMPARE(menu.actions().count(), 4);
    QCOMPARE(menu.actions().first()->statusTip(), QLatin1String("tip"));

    // maxRows + firstSeparator add a mid-list separator.
    ModelMenu limited;
    limited.setModel(&model);
    limited.setMaxRows(2);
    limited.setFirstSeparator(1);
    emit static_cast<QMenu *>(&limited)->aboutToShow();
    QVERIFY(limited.actions().count() <= 4);
}

// Rows with children become nested ModelMenus via createBaseMenu().
void tst_ModelMenu::subMenus()
{
    QStandardItemModel model;
    QStandardItem *parent = new QStandardItem(QLatin1String("parent"));
    parent->appendRow(new QStandardItem(QLatin1String("child0")));
    parent->appendRow(new QStandardItem(QLatin1String("child1")));
    model.appendRow(parent);
    model.appendRow(new QStandardItem(QLatin1String("flat")));

    ModelMenu menu;
    menu.setModel(&model);
    emit static_cast<QMenu *>(&menu)->aboutToShow();

    QCOMPARE(menu.actions().count(), 2);
    QMenu *sub = menu.actions().first()->menu();
    QVERIFY(sub);
    QCOMPARE(sub->title(), QLatin1String("parent"));
    emit static_cast<QMenu *>(sub)->aboutToShow();
    QCOMPARE(sub->actions().count(), 2);
}

// Triggering a generated action re-emits activated() with the index.
void tst_ModelMenu::activation()
{
    QStandardItemModel model;
    model.appendRow(new QStandardItem(QLatin1String("entry")));

    ModelMenu menu;
    menu.setModel(&model);
    emit static_cast<QMenu *>(&menu)->aboutToShow();

    QSignalSpy spy(&menu, &ModelMenu::activated);
    menu.actions().first()->trigger();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<QModelIndex>(),
             model.index(0, 0));
}

// BookmarksMenu populates from the shared manager's menu node and adds
// the trailing "Open All" action.
void tst_ModelMenu::bookmarksMenu()
{
    BookmarksManager manager;
    BookmarkNode *bookmark = new BookmarkNode(BookmarkNode::Bookmark);
    bookmark->title = QLatin1String("MenuBM");
    bookmark->url = QLatin1String("http://menubm.example.com/");
    manager.addBookmark(manager.menu(), bookmark);

    BookmarksMenu menu;
    menu.setModel(manager.bookmarksModel());
    menu.setRootIndex(manager.bookmarksModel()->index(manager.menu()));
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QVERIFY(menu.actions().count() >= 2); // bookmark + "Open All"

    // Triggering a leaf action emits the openUrl signal.
    QSignalSpy spy(&menu, QOverload<const QUrl &, const QString &>::of(&BookmarksMenu::openUrl));
    const QList<QAction *> menuActions = menu.actions();
    for (QAction *action : menuActions) {
        const QModelIndex idx = menu.index(action);
        if (idx.isValid() && !menu.model()->hasChildren(idx)) {
            action->trigger();
            break;
        }
    }
    QVERIFY(spy.count() >= 1);

    // BookmarksMenuBarMenu prepends its fixed initial actions.  The
    // action must not be parented to the menu: QMenu::clear() (run by
    // aboutToShow) deletes actions owned by the menu, which would leave
    // a dangling pointer in m_initialActions — in the real browser the
    // initial actions are owned by the main window.
    BookmarksMenuBarMenu barMenu;
    QAction *fixed = new QAction(QLatin1String("fixed"), this);
    barMenu.setInitialActions(QList<QAction *>() << fixed);
    barMenu.setModel(manager.bookmarksModel());
    barMenu.setRootIndex(manager.bookmarksModel()->index(manager.menu()));
    emit static_cast<QMenu *>(&barMenu)->aboutToShow();
    QCOMPARE(barMenu.actions().first(), fixed);
}

// HistoryMenu maps model rows to urls via the HistoryMenuModel.
void tst_ModelMenu::historyMenu()
{
    HistoryManager *history = HistoryManager::instance();
    history->clear();
    history->addHistoryEntry(QLatin1String("http://histmenu.example.com/"));

    HistoryMenu menu;
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QVERIFY(menu.actions().count() >= 1);

    // activated() on an invalid index is a no-op; initial actions and
    // the trailing "Show All"/"Clear" entries carry no index.
    QSignalSpy spy(&menu, &HistoryMenu::openUrl);
    const QList<QAction *> menuActions = menu.actions();
    for (QAction *action : menuActions) {
        if (menu.index(action).isValid()) {
            action->trigger();
            break;
        }
    }
    QVERIFY(spy.count() >= 1);
}

// ModelToolBar rebuilds its buttons on show(); activation is emitted
// from an eventFilter on the tool buttons' mouse release.
void tst_ModelMenu::modelToolBar()
{
    QStandardItemModel model;
    for (int i = 0; i < 3; ++i)
        model.appendRow(new QStandardItem(QString::number(i)));

    ModelToolBar bar;
    bar.setModel(&model);
    bar.show();
    QTest::qWait(50);
    QVERIFY(bar.actions().count() >= 3);

    QSignalSpy spy(&bar, &ModelToolBar::activated);
    QWidget *button = bar.widgetForAction(bar.actions().first());
    QVERIFY(button);
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.first().at(0).value<QModelIndex>(), model.index(0, 0));

    bar.setRootIndex(model.index(0, 0));
    QCOMPARE(bar.rootIndex().row(), 0);
    QVERIFY(!ModelToolBar::index(nullptr).isValid());
    QVERIFY(ModelToolBar::index(bar.actions().first()).isValid());
}

// BookmarksToolBar roots itself on the shared manager's toolbar node
// and routes activation to openUrl.
void tst_ModelMenu::bookmarksToolBar()
{
    BookmarksManager *manager = BookmarksManager::instance();
    // The shared manager's toolbar folder ships default bookmarks —
    // clear it so our entry is the first action.
    while (manager->toolbar()->children().count() > 0)
        manager->removeBookmark(manager->toolbar()->children().first());

    BookmarkNode *bookmark = new BookmarkNode(BookmarkNode::Bookmark);
    bookmark->title = QLatin1String("TbBM");
    bookmark->url = QLatin1String("http://toolbar.example.com/");
    manager->addBookmark(manager->toolbar(), bookmark);

    BookmarksToolBar bar(manager->bookmarksModel());
    bar.show();
    QTest::qWait(50);
    QVERIFY(bar.actions().count() >= 1);

    QSignalSpy spy(&bar, QOverload<const QUrl &, const QString &>::of(&BookmarksToolBar::openUrl));
    QWidget *button = bar.widgetForAction(bar.actions().first());
    QVERIFY(button);
    QTest::mouseClick(button, Qt::LeftButton);
    QVERIFY(spy.count() >= 1);
    QCOMPARE(spy.first().at(0).toUrl(),
             QUrl(QLatin1String("http://toolbar.example.com/")));

    manager->removeBookmark(bookmark);
}

QTEST_MAIN(tst_ModelMenu)
#include "tst_modelmenu.moc"
