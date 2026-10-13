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
#include <qtimer.h>
#include <qevent.h>
#include <qapplication.h>

#include "modelmenu.h"
#include "modeltoolbar.h"
#include "bookmarksmenu.h"
#include "bookmarkstoolbar.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "bookmarknode.h"
#include "addbookmarkdialog.h"
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

// Repeating driver for the modal nested loops context menus open.
// Every tick: close a modal dialog if one is up, fire the first popup
// action matching `needle` exactly once, then close the popup so the
// exec() unwinds and the test continues — even when no action matched.
class PopupDriver : public QObject
{
public:
    QString needle;
    bool fired = false;
    int ticks = 0;

    explicit PopupDriver(const QString &needle, QObject *parent = nullptr)
        : QObject(parent), needle(needle)
    {
        QTimer *timer = new QTimer(this);
        connect(timer, &QTimer::timeout, this, [this]() { tick(); });
        timer->start(60);
    }

private:
    void tick()
    {
        if (++ticks > 150) {
            deleteLater();
            return;
        }
        if (QWidget *modal = QApplication::activeModalWidget()) {
            modal->close();
            return;
        }
        QMenu *menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            if (fired)
                deleteLater();
            return;
        }
        if (!fired) {
            fired = true;
            for (QAction *action : menu->actions()) {
                if (action->text().contains(needle)) {
                    action->trigger();
                    return;
                }
            }
        }
        menu->close();
        deleteLater();
    }
};

class tst_ModelMenu : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void accessors();
    void population();
    void caching();
    void subMenus();
    void activation();
    void bookmarksMenu();
    void historyMenu();
    void modelToolBar();
    void modelToolBarDnD();
    void bookmarksToolBar();
    void bookmarksToolBarContextMenu();
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

// PERF03: aboutToShow rebuilds only while dirty — repeated opens keep
// the same action objects, and a model mutation invalidates them.
void tst_ModelMenu::caching()
{
    QStandardItemModel model;
    for (int i = 0; i < 2000; ++i)
        model.appendRow(new QStandardItem(QString::number(i)));

    ModelMenu menu;
    menu.setModel(&model);
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QCOMPARE(menu.actions().count(), 2000);
    QAction *first = menu.actions().constFirst();

    // Unchanged model: reopening keeps the populated actions.
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QCOMPARE(menu.actions().count(), 2000);
    QCOMPARE(menu.actions().constFirst(), first);

    // A row insertion dirties the menu → the next show rebuilds.
    model.appendRow(new QStandardItem(QLatin1String("tail")));
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QCOMPARE(menu.actions().count(), 2001);
    QVERIFY(menu.actions().constFirst() != first);

    // dataChanged (no row-count change) dirties it too.
    QAction *rebuiltFirst = menu.actions().constFirst();
    model.item(0)->setText(QLatin1String("renamed"));
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QVERIFY(menu.actions().constFirst() != rebuiltFirst);
    QCOMPARE(menu.actions().constFirst()->text(),
             QString::fromLatin1("renamed"));

    // A new model dirties the menu again.
    QStandardItemModel other;
    other.appendRow(new QStandardItem(QLatin1String("other")));
    QAction *beforeSwap = menu.actions().constFirst();
    menu.setModel(&other);
    emit static_cast<QMenu *>(&menu)->aboutToShow();
    QCOMPARE(menu.actions().count(), 1);
    QVERIFY(menu.actions().constFirst() != beforeSwap);
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

    // MENU08: a submenu built lazily at aboutToShow copies the font
    // of the menu it hangs off — popups never inherit it on their
    // own, so the app menu's enlarged text would shed one level down.
    // The chain propagates at every depth: the grandchild copies the
    // submenu's font, not the app default.
    QStandardItemModel deepModel;
    QStandardItem *outer = new QStandardItem(QLatin1String("outer"));
    QStandardItem *inner = new QStandardItem(QLatin1String("inner"));
    inner->appendRow(new QStandardItem(QLatin1String("leaf")));
    outer->appendRow(inner);
    deepModel.appendRow(outer);
    ModelMenu styled;
    styled.setModel(&deepModel);
    QFont bigFont = styled.font();
    if (bigFont.pointSizeF() > 0)
        bigFont.setPointSizeF(bigFont.pointSizeF() + 3);
    else
        bigFont.setPixelSize(bigFont.pixelSize() + 4);
    styled.setFont(bigFont);
    emit static_cast<QMenu *>(&styled)->aboutToShow();
    QMenu *styledSub = styled.actions().first()->menu();
    QVERIFY(styledSub);
    QCOMPARE(styledSub->font(), bigFont);
    emit static_cast<QMenu *>(styledSub)->aboutToShow();
    QMenu *grandSub = styledSub->actions().first()->menu();
    QVERIFY(grandSub);
    QCOMPARE(grandSub->font(), bigFont);
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

// COV08: the toolbar's drag/drop entry points, the titled ctor, model
// swaps and the folder->submenu arm in build().
void tst_ModelMenu::modelToolBarDnD()
{
    QStandardItemModel model;
    model.appendRow(new QStandardItem(QLatin1String("a")));
    model.appendRow(new QStandardItem(QLatin1String("b")));
    QStandardItem *folder = new QStandardItem(QLatin1String("folder"));
    folder->appendRow(new QStandardItem(QLatin1String("child")));
    model.appendRow(folder);

    // Titled ctor + setModel on an already-set model (disconnect leg).
    ModelToolBar bar(QLatin1String("titled"));
    bar.setModel(&model);
    QStandardItemModel other;
    other.appendRow(new QStandardItem(QLatin1String("x")));
    bar.setModel(&other);
    QCOMPARE(bar.model(), &other);
    bar.setModel(&model);
    bar.show();
    QTest::qWait(50);
    QCOMPARE(bar.actions().count(), 3);
    // Rows with children arm a ModelMenu on their action.
    QVERIFY(bar.actions().at(2)->menu() != nullptr);

    // dragEnter accepts mime the model lists, ignores anything else.
    // The model's own drag payload always matches its mimeTypes() and
    // is the format its dropMimeData actually decodes.
    QMimeData *good = model.mimeData({model.index(0, 0)});
    QDragEnterEvent enter(QPointF(4, 4), Qt::CopyAction, good,
                          Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &enter);
    QVERIFY(enter.isAccepted());

    QMimeData bad;
    bad.setData(QLatin1String("application/x-not-the-model"),
                QByteArray("x"));
    QDragEnterEvent refuse(QPointF(4, 4), Qt::CopyAction, &bad,
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &refuse);
    QVERIFY(!refuse.isAccepted());

    // A drop under the cursor position (no action there) appends at
    // the root — the model grows a row.
    const int rows = model.rowCount();
    QDropEvent drop(QPointF(4, 4), Qt::CopyAction, good,
                    Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &drop);
    QVERIFY(drop.isAccepted());
    QVERIFY(model.rowCount() > rows);

    // mouseMove with the left button held: no press armed a drag
    // start on a button, so actionAt() finds nothing and the handler
    // falls through to the base implementation.
    QMouseEvent move(QEvent::MouseMove, QPointF(200, 200),
                     QPointF(200, 200), bar.mapToGlobal(QPoint(200, 200)),
                     Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bar, &move);

    // hide() clears, show() rebuilds through showEvent.
    bar.hide();
    QTest::qWait(10);
    QCOMPARE(bar.actions().count(), 0);
    bar.show();
    QTest::qWait(50);
    // The drop above appended a row, so the rebuild shows one more.
    QCOMPARE(bar.actions().count(), model.rowCount());

    // No model at all: the guards take the base-class legs.
    ModelToolBar bare;
    QDragEnterEvent enter2(QPointF(4, 4), Qt::CopyAction, good,
                           Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bare, &enter2);
    QDropEvent drop2(QPointF(4, 4), Qt::CopyAction, good,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bare, &drop2);
    QMouseEvent move2(QEvent::MouseMove, QPointF(20, 20),
                      QPointF(20, 20), bare.mapToGlobal(QPoint(20, 20)),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&bare, &move2);
}

// COV08: the toolbar's custom context menu — item arm over a leaf,
// the open/remove/add slots routed through real menu actions, and the
// no-action arm on empty space.
void tst_ModelMenu::bookmarksToolBarContextMenu()
{
    BookmarksManager *manager = BookmarksManager::instance();
    while (manager->toolbar()->children().count() > 0)
        manager->removeBookmark(manager->toolbar()->children().first());

    BookmarkNode *leaf = new BookmarkNode(BookmarkNode::Bookmark);
    leaf->title = QLatin1String("CtxBM");
    leaf->url = QLatin1String("http://ctxbm.example.com/");
    manager->addBookmark(manager->toolbar(), leaf);
    BookmarkNode *folder = new BookmarkNode(BookmarkNode::Folder);
    folder->title = QLatin1String("CtxFolder");
    manager->addBookmark(manager->toolbar(), folder);

    BookmarksToolBar bar(manager->bookmarksModel());
    bar.show();
    QTest::qWait(50);
    QVERIFY(bar.actions().count() >= 2);
    QAction *leafAction = bar.actions().first();
    QVERIFY(!leafAction->menu());
    // The folder row armed a BookmarksMenu through createMenu().
    QVERIFY(bar.actions().at(1)->menu() != nullptr);

    const QPoint pos = bar.actionGeometry(leafAction).center();

    // "Open in New &Tab" through the context menu — the slot resolves
    // the sender action's QModelIndex and re-emits openUrl.
    QSignalSpy spy(&bar,
        QOverload<const QUrl &, TabWidget::OpenUrlIn, const QString &>::of(
            &BookmarksToolBar::openUrl));
    PopupDriver *driver = new PopupDriver(QLatin1String("New"), &bar);
    QContextMenuEvent onLeaf(QContextMenuEvent::Mouse, pos);
    QApplication::sendEvent(&bar, &onLeaf);
    QVERIFY(driver->fired);
    QTRY_VERIFY_WITH_TIMEOUT(spy.count() >= 1, 5000);
    QCOMPARE(spy.first().at(0).toUrl(),
             QUrl(QLatin1String("http://ctxbm.example.com/")));
    QCOMPARE(spy.first().at(1).value<TabWidget::OpenUrlIn>(),
             TabWidget::NewTab);
    QCOMPARE(spy.first().at(2).toString(), QLatin1String("CtxBM"));

    // "Remove" deletes the leaf through the model.
    driver = new PopupDriver(QLatin1String("Remove"), &bar);
    QContextMenuEvent onLeaf2(QContextMenuEvent::Mouse, pos);
    QApplication::sendEvent(&bar, &onLeaf2);
    QVERIFY(driver->fired);
    QTRY_COMPARE_WITH_TIMEOUT(manager->toolbar()->children().count(),
                              1, 5000);

    // Empty-space menu (no action under the point) only offers the
    // add entries — driving it just opens and closes the exec() so
    // contextMenuEvent's no-action arm gets covered.  (Triggering
    // "Add Bookmark..." here would exec() a modal dialog while the
    // menu still holds the popup grab; on offscreen that deadlocks,
    // so newBookmark()/newFolder() are invoked directly below.)
    driver = new PopupDriver(QLatin1String("no-such-action"), &bar);
    const QPoint empty = bar.rect().bottomRight() - QPoint(1, 1);
    QContextMenuEvent onEmpty(QContextMenuEvent::Mouse, empty);
    QApplication::sendEvent(&bar, &onEmpty);
    QVERIFY(driver->fired);

    // The sender()-routed slots still fire safely with no sender —
    // invokeMethod reaches protected slots directly.  The
    // newBookmark/newFolder dialog exec()s need the driver to close
    // their modals.
    QSignalSpy openSpy(&bar,
        QOverload<const QUrl &, const QString &>::of(
            &BookmarksToolBar::openUrl));
    QVERIFY(QMetaObject::invokeMethod(&bar, "openBookmark"));
    QVERIFY(QMetaObject::invokeMethod(&bar, "openBookmarkInCurrentTab"));
    new PopupDriver(QLatin1String("no-such-action"), &bar);
    QVERIFY(QMetaObject::invokeMethod(&bar, "newBookmark"));
    QVERIFY(QMetaObject::invokeMethod(&bar, "newFolder"));
    // Only openBookmark() emits the two-argument overload — the tab
    // variants went through the three-argument one spied above.
    QCOMPARE(openSpy.count(), 1);

    while (manager->toolbar()->children().count() > 0)
        manager->removeBookmark(manager->toolbar()->children().first());
}

QTEST_MAIN(tst_ModelMenu)
#include "tst_modelmenu.moc"
