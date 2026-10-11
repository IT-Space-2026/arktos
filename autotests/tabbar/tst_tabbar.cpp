/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#include <QtGui/QtGui>
#include <QtTest/QtTest>
#include <qabstractbutton.h>
#include <qinputdialog.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <tabbar.h>
#include <tabpreview.h>
#include <tabwidget.h>
#include <webview.h>
#include <containermanager.h>
#include <browserapplication.h>
#include "qtest_arora.h"
#include "qtry.h"

class tst_TabBar : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void tabbar_data();
    void tabbar();

    void showTabBarWhenOneTab_data();
    void showTabBarWhenOneTab();

    void tabSizeHint_data();
    void tabSizeHint();

    void perTabCloseButtons();

    void middleClickPaste();

    void contextMenuBaseEntries();
    void contextMenuGroupEntries();
    void contextMenuContainerEntries();
    void chipsPaint();
    void tabMouseAndDragEvents();
    void tabKeyShortcuts();
    void previewDwell();
};

// Subclass that exposes the protected functions.
class SubTabBar : public TabBar
{
public:
    void call_cloneTab(int index)
        { return SubTabBar::cloneTab(index); }

    void call_closeOtherTabs(int index)
        { return SubTabBar::closeOtherTabs(index); }

    void call_closeTab(int index)
        { return SubTabBar::closeTab(index); }

    void call_dragEnterEvent(QDragEnterEvent *event)
        { return SubTabBar::dragEnterEvent(event); }

    void call_dropEvent(QDropEvent *event)
        { return SubTabBar::dropEvent(event); }

    void call_mouseMoveEvent(QMouseEvent *event)
        { return SubTabBar::mouseMoveEvent(event); }

    void call_mousePressEvent(QMouseEvent *event)
        { return SubTabBar::mousePressEvent(event); }

    void call_mouseReleaseEvent(QMouseEvent *event)
        { return SubTabBar::mouseReleaseEvent(event); }

    void call_newTab()
        { return SubTabBar::newTab(); }

    void call_reloadAllTabs()
        { return SubTabBar::reloadAllTabs(); }

    void call_reloadTab(int index)
        { return SubTabBar::reloadTab(index); }

    void call_tabLayoutChange()
        { return SubTabBar::tabLayoutChange(); }

    QSize call_tabSizeHint(int index) const
        { return SubTabBar::tabSizeHint(index); }
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_TabBar::initTestCase()
{
    // The TabWidget-driven cases touch the app-level registries
    // (containers, site rules, history); a per-suite application name
    // keeps them out of the real profile's settings.
    QCoreApplication::setApplicationName("tst_tabbar");
    QSettings settings;
    settings.clear();
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_TabBar::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_TabBar::init()
{
}

// This will be called after every test function.
void tst_TabBar::cleanup()
{
}

void tst_TabBar::tabbar_data()
{
}

void tst_TabBar::tabbar()
{
    SubTabBar bar;
    QCOMPARE(bar.showTabBarWhenOneTab(), true);
    bar.setShowTabBarWhenOneTab(false);
    QVERIFY(bar.viewTabBarAction() != nullptr);
    bar.call_tabLayoutChange();
}

void tst_TabBar::showTabBarWhenOneTab_data()
{
    QTest::addColumn<bool>("showTabBarWhenOneTab");
    QTest::newRow("true") << false;
    QTest::newRow("false") << false;
}

// public bool showTabBarWhenOneTab() const
void tst_TabBar::showTabBarWhenOneTab()
{
    QFETCH(bool, showTabBarWhenOneTab);

    SubTabBar bar;
    bar.show();
    bar.setShowTabBarWhenOneTab(showTabBarWhenOneTab);
    QAction *action = bar.viewTabBarAction();
    QVERIFY(action);
    QCOMPARE(action->text(), (showTabBarWhenOneTab ? QString("Hide Tab Bar") : QString("Show Tab Bar")));
    QCOMPARE(bar.showTabBarWhenOneTab(), showTabBarWhenOneTab);

    bar.addTab("one");
    QCOMPARE(bar.count(), 1);
    QCOMPARE(bar.isVisible(), showTabBarWhenOneTab);

    bar.addTab("two");
    QCOMPARE(bar.count(), 2);
    QCOMPARE(bar.isVisible(), true);
    QCOMPARE(action->text(), QString("Hide Tab Bar"));
    QCOMPARE(action->isEnabled(), false);

    bar.removeTab(0);
    QCOMPARE(bar.count(), 1);
    QCOMPARE(bar.isVisible(), showTabBarWhenOneTab);
    QCOMPARE(action->isEnabled(), true);
    QCOMPARE(action->text(), (showTabBarWhenOneTab ? QString("Hide Tab Bar") : QString("Show Tab Bar")));
}

void tst_TabBar::tabSizeHint_data()
{
    QTest::addColumn<int>("index");
    QTest::newRow("0") << 0;
}

// protected QSize tabSizeHint(int index) const
void tst_TabBar::tabSizeHint()
{
    QFETCH(int, index);

    SubTabBar bar;

    QVERIFY(bar.call_tabSizeHint(index).width() <= 250);
}

// UIP02: per-tab close buttons live on the style's close-button side
// and reveal only on the current or hovered tab — not on every tab
// like Qt's native always-visible close indicator.
void tst_TabBar::perTabCloseButtons()
{
    SubTabBar bar;
    bar.setPerTabCloseButtons(true);
    QVERIFY(bar.perTabCloseButtons());
    QVERIFY(!bar.tabsClosable());
    bar.show();
    // The offscreen QPA sends no expose events; process once so the
    // bar lays out its tabs before we poke at their rects.
    QApplication::processEvents();

    bar.addTab(QLatin1String("one"));
    bar.addTab(QLatin1String("two"));
    bar.addTab(QLatin1String("three"));
    QCOMPARE(bar.count(), 3);
    // The bare bar keeps its tiny default geometry under offscreen —
    // give it a real strip so tab rects sit inside the widget.
    bar.resize(400, bar.sizeHint().height());
    QApplication::processEvents();

    const QTabBar::ButtonPosition side =
        (bar.freeSide() == QTabBar::RightSide)
            ? QTabBar::LeftSide : QTabBar::RightSide;

    // A button was installed on the close side of every tab.
    for (int i = 0; i < bar.count(); ++i)
        QVERIFY(bar.tabButton(i, side));

    // Only the current tab's button is visible while unhovered.
    bar.setCurrentIndex(0);
    QVERIFY(bar.tabButton(0, side)->isVisible());
    QVERIFY(!bar.tabButton(1, side)->isVisible());
    QVERIFY(!bar.tabButton(2, side)->isVisible());

    // Hovering a background tab reveals its button.  Synthetic
    // QTest::mouseMove does not reach the widget under the offscreen
    // QPA, so drive the handler directly like the other tests do.
    QMouseEvent moveEvent(QEvent::MouseMove,
                          QPointF(bar.tabRect(1).center()),
                          QPointF(bar.tabRect(1).center()),
                          Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    bar.call_mouseMoveEvent(&moveEvent);
    QVERIFY(bar.tabButton(1, side)->isVisible());
    QVERIFY(bar.tabButton(0, side)->isVisible());
    QVERIFY(!bar.tabButton(2, side)->isVisible());

    // Clicking the current tab's button emits closeTab(index), with
    // the index resolved at click time.
    QSignalSpy spy(&bar, QOverload<int>::of(&TabBar::closeTab));
    static_cast<QAbstractButton *>(bar.tabButton(0, side))->click();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toInt(), 0);

    // A new tab gets a button too; disabling removes them all.
    bar.addTab(QLatin1String("four"));
    QVERIFY(bar.tabButton(3, side));
    bar.setPerTabCloseButtons(false);
    QVERIFY(!bar.perTabCloseButtons());
    for (int i = 0; i < bar.count(); ++i)
        QVERIFY(!bar.tabButton(i, side));
}

// SEC02: middle-click paste loads the PRIMARY selection as a url in a
// new tab — except javascript:, which must never become script
// execution.
void tst_TabBar::middleClickPaste()
{
    SubTabBar bar;
    bar.show();

    QClipboard *clipboard = QApplication::clipboard();
    const QString scriptUrl = QLatin1String("javascript:void(0)");
    clipboard->setText(scriptUrl, QClipboard::Selection);
    if (clipboard->text(QClipboard::Selection) != scriptUrl)
        QSKIP("the offscreen clipboard has no Selection mode");

    QList<QUrl> opened;
    QObject::connect(&bar, &TabBar::loadUrl, &bar,
                     [&opened](const QUrl &url, TabWidget::OpenUrlIn) {
        opened.append(url);
    });

    // Middle-click on the empty bar area (tabAt == -1) takes the
    // paste-as-url path.
    QMouseEvent event(QEvent::MouseButtonRelease, QPointF(1, 1),
                      QPointF(1, 1), Qt::MiddleButton, Qt::NoButton,
                      Qt::NoModifier);
    bar.call_mouseReleaseEvent(&event);
    QVERIFY(opened.isEmpty());

    clipboard->setText(QLatin1String("http://example.com/"),
                       QClipboard::Selection);
    bar.call_mouseReleaseEvent(&event);
    QCOMPARE(opened, QList<QUrl>() << QUrl("http://example.com/"));
}

// COV05 — the tab-strip chrome lives on the TabWidget's own TabBar, so
// everything below drives real TabWidgets (BrowserApplication comes
// from qtest_arora.h's QTEST_MAIN).

// contextMenuRequested() is a private slot that builds a stack-local
// QMenu and exec()s it.  This helper fires the slot, then — inside
// the nested loop — triggers the entry whose text contains `label`
// (descending one submenu level when `submenuLabel` is given), so the
// build path AND the chosen action both run.
static QMenu *findPopupMenu()
{
    if (QMenu *menu = qobject_cast<QMenu *>(
            QApplication::activePopupWidget()))
        return menu;
    // The offscreen QPA does not always register exec'd menus as the
    // "active popup" — fall back to scanning the top levels for a
    // visible QMenu (there is only ever one at a time here).
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *widget : tops) {
        if (QMenu *menu = qobject_cast<QMenu *>(widget)) {
            if (widget->isVisible())
                return menu;
        }
    }
    return nullptr;
}

static bool triggerContextEntry(TabBar *bar, const QPoint &pos,
                                const QString &label,
                                const QString &submenuLabel = QString())
{
    bool fired = false;
    int tries = 0;
    QTimer *timer = new QTimer(qApp);
    timer->setInterval(80);
    QObject::connect(timer, &QTimer::timeout, qApp,
                     [label, submenuLabel, &fired, &tries, timer]() {
        QMenu *menu = findPopupMenu();
        if (!menu) {
            if (++tries > 25)
                timer->stop();   // give up after ~2s
            return;
        }
        timer->stop();
        auto findEntry = [](QMenu *m, const QString &text) -> QAction * {
            for (QAction *action : m->actions()) {
                // '&' mnemonics sit inside the labels — strip them
                // before substring matching.
                if (QString(action->text()).remove(QLatin1Char('&'))
                        .contains(text))
                    return action;
            }
            return nullptr;
        };
        if (!submenuLabel.isEmpty()) {
            QAction *sub = findEntry(menu, submenuLabel);
            QMenu *submenu = sub ? sub->menu() : nullptr;
            QAction *target = submenu ? findEntry(submenu, label)
                                      : nullptr;
            if (target && target->isEnabled()) {
                target->trigger();
                fired = true;
            }
            menu->close();
            return;
        }
        QAction *target = findEntry(menu, label);
        if (target && target->isEnabled()) {
            target->trigger();
            fired = true;
        }
        // A programmatic trigger does not dismiss an exec'd menu —
        // close it or the slot's nested loop never returns.
        menu->close();
    });
    timer->start();
    const bool invoked = QMetaObject::invokeMethod(
        bar, "contextMenuRequested", Q_ARG(QPoint, pos));
    timer->stop();
    timer->deleteLater();
    return invoked && fired;
}

// The ungrouped-tab and empty-strip menus: action signal fan-out plus
// the Sleep entry's enabled gate.
void tst_TabBar::contextMenuBaseEntries()
{
    TabWidget widget;
    widget.resize(800, 240);
    widget.show();
    TabBar *bar = widget.tabBar();
    widget.newTab();
    widget.newTab();
    widget.webView(1)->loadUrl(
        QUrl(QLatin1String("data:text/html,<p>bg</p>")));
    widget.newTab();
    QCOMPARE(widget.count(), 3);
    QApplication::processEvents();

    // A point past the last tab resolves to no tab (the -1 branch):
    // New Tab, Reload All, Bookmark All Tabs.
    const QRect lastRect = bar->tabRect(bar->count() - 1);
    QVERIFY(lastRect.right() + 2 < bar->rect().right());
    const QPoint emptyPoint(lastRect.right() + 2,
                            lastRect.center().y());
    QCOMPARE(bar->tabAt(emptyPoint), -1);
    QSignalSpy reloadAll(bar, &TabBar::reloadAllTabs);
    QVERIFY(triggerContextEntry(bar, emptyPoint,
                                QLatin1String("Reload All Tabs")));
    QCOMPARE(reloadAll.count(), 1);

    // "Bookmark Tabs" routes through the AddBookmarkDialog — accept it.
    acceptModal();
    QVERIFY(triggerContextEntry(bar, emptyPoint,
                                QLatin1String("Bookmark")));

    // Duplicate emits cloneTab(index) — the TabWidget doubles it.
    QSignalSpy cloned(bar, QOverload<int>::of(&TabBar::cloneTab));
    const int before = widget.count();
    QVERIFY(triggerContextEntry(bar, bar->tabRect(1).center(),
                                QLatin1String("Duplicate Tab")));
    QCOMPARE(cloned.count(), 1);
    QCOMPARE(cloned.takeFirst().at(0).toInt(), 1);
    QCOMPARE(widget.count(), before + 1);

    // Sleep is disabled on the current tab, enabled on a loaded
    // background tab; the emitted signal reaches TabWidget::sleepTab.
    widget.setCurrentIndex(0);
    QSignalSpy slept(bar, &TabBar::sleepTab);
    QVERIFY(triggerContextEntry(bar, bar->tabRect(1).center(),
                                QLatin1String("Sleep Tab")));
    QCOMPARE(slept.count(), 1);
    QCOMPARE(slept.takeFirst().at(0).toInt(), 1);

    // Reload Tab emits reloadTab(index).
    QSignalSpy reloaded(bar, QOverload<int>::of(&TabBar::reloadTab));
    QVERIFY(triggerContextEntry(bar, bar->tabRect(2).center(),
                                QLatin1String("Reload Tab")));
    QCOMPARE(reloaded.count(), 1);

    // Close Other Tabs leaves only the target behind.
    QSignalSpy closeOthers(bar,
                           QOverload<int>::of(&TabBar::closeOtherTabs));
    QVERIFY(triggerContextEntry(bar, bar->tabRect(2).center(),
                                QLatin1String("Close Other Tabs")));
    QCOMPARE(closeOthers.count(), 1);
    QCOMPARE(widget.count(), 1);

    // "&Close Tab" empties the strip.
    QSignalSpy closed(bar, QOverload<int>::of(&TabBar::closeTab));
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("Close Tab")));
    QCOMPARE(closed.count(), 1);
    QCOMPARE(widget.count(), 0);
}

// A grouped tab's menu: collapse/expand, rename (QInputDialog), the
// color submenu, remove-from-group and ungroup, plus "Add Tab to
// Group" on the ungrouped side (new + existing entries).
void tst_TabBar::contextMenuGroupEntries()
{
    TabWidget widget;
    widget.resize(640, 240);
    widget.show();
    TabBar *bar = widget.tabBar();
    widget.newTab();
    widget.newTab();
    widget.newTab();
    QCOMPARE(widget.count(), 3);
    QApplication::processEvents();

    // Ungrouped side: "Add Tab to Group" -> "&New Group" makes one.
    QVERIFY(triggerContextEntry(bar, bar->tabRect(1).center(),
                                QLatin1String("New Group"),
                                QLatin1String("Add Tab to")));
    const QString gid = widget.tabGroupId(1);
    QVERIFY(!gid.isEmpty());

    // The now-existing group offers itself by name ("Unnamed Group"
    // fallback text) to other ungrouped tabs.
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("Unnamed Group"),
                                QLatin1String("Add Tab to")));
    QCOMPARE(widget.tabGroupId(0), gid);
    QCOMPARE(widget.tabGroupSize(gid), 2);

    // Rename through the modal QInputDialog.
    QTimer::singleShot(120, qApp, []() {
        QInputDialog *dialog = qobject_cast<QInputDialog *>(
            QApplication::activeModalWidget());
        if (!dialog)
            return;
        if (QLineEdit *edit = dialog->findChild<QLineEdit *>())
            edit->setText(QLatin1String("Strip Group"));
        dialog->accept();
    });
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("ename Group")));
    QCOMPARE(widget.tabGroupName(gid), QLatin1String("Strip Group"));

    // The color submenu re-tints the group.
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("Green"),
                                QLatin1String("Color")));
    QCOMPARE(widget.tabGroupColor(gid),
             ContainerManager::defaultColors().at(2));

    // Collapse down to the chip, then expand back out.
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("Collapse Group")));
    QVERIFY(widget.tabGroupIsCollapsed(gid));
    QVERIFY(widget.hasCollapsedTabGroup());
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("Expand Group")));
    QVERIFY(!widget.tabGroupIsCollapsed(gid));

    // One member leaves, the group survives on the other.
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("Remove Tab from Group")));
    QCOMPARE(widget.tabGroupId(0), QString());
    QCOMPARE(widget.tabGroupId(1), gid);

    // Ungroup dissolves the whole set.
    QVERIFY(triggerContextEntry(bar, bar->tabRect(1).center(),
                                QLatin1String("ngroup")));
    QVERIFY(widget.tabGroupIds().isEmpty());
}

// The container submenu (non-private/non-tor build): reopen entries,
// the always-open site rules, and the dialog-launching actions.
void tst_TabBar::contextMenuContainerEntries()
{
    TabWidget widget;
    widget.resize(640, 240);
    widget.show();
    TabBar *bar = widget.tabBar();
    ContainerManager *manager = ContainerManager::instance();
    const ContainerManager::Container container =
        manager->createContainer(QLatin1String("CtxTest"),
                                 QColor(Qt::green));
    QVERIFY(manager->isContainerId(container.id));

    widget.newTab();
    widget.webView(0)->loadUrl(
        QUrl(QLatin1String("http://ctxtab.test/")));
    widget.newTab();
    widget.setCurrentIndex(0);
    QApplication::processEvents();

    // Default-container tab on an http host: "Always Open Site in
    // Container" submenu claims the host and re-homes the tab.
    QSignalSpy reopen(bar, &TabBar::reopenInContainer);
    QVERIFY(triggerContextEntry(bar, bar->tabRect(0).center(),
                                QLatin1String("CtxTest"),
                                QLatin1String("Always Open Site in")));
    QCOMPARE(manager->containerIdForHost(
                 QLatin1String("ctxtab.test")), container.id);
    manager->removeSiteRule(QLatin1String("ctxtab.test"));
    QVERIFY(reopen.count() >= 1);

    // A container tab shows the flat "in This Container" checkbox.
    int containerIndex = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.containerIdForTab(i) == container.id) {
            containerIndex = i;
            break;
        }
    }
    QVERIFY(containerIndex >= 0);
    QVERIFY(triggerContextEntry(
        bar, bar->tabRect(containerIndex).center(),
        QLatin1String("Always Open This Site")));
    QCOMPARE(manager->containerIdForHost(
                 QLatin1String("ctxtab.test")), container.id);
    manager->removeSiteRule(QLatin1String("ctxtab.test"));

    // "&No Container" re-homes the tab to the default profile.
    QVERIFY(triggerContextEntry(
        bar, bar->tabRect(containerIndex).center(),
        QLatin1String("No Container"),
        QLatin1String("Reopen in Container")));

    // Dialog entries: New Container (reject) and Manage Containers
    // (accept the SettingsDialog).
    rejectModal();
    triggerContextEntry(bar, bar->tabRect(0).center(),
                        QLatin1String("New Container"),
                        QLatin1String("Reopen in Container"));
    acceptModal();
    triggerContextEntry(bar, bar->tabRect(0).center(),
                        QLatin1String("Manage Containers"),
                        QLatin1String("Reopen in Container"));

    manager->deleteContainer(container.id);
}

// paintEvent's container chips, group rails/pills/collapsed chip and
// the sleep badge all draw through QWidget::grab() — offscreen never
// sends expose events but explicit rendering still runs the painter.
void tst_TabBar::chipsPaint()
{
    TabWidget widget;
    widget.resize(720, 320);
    widget.show();
    QApplication::processEvents();

    ContainerManager *manager = ContainerManager::instance();
    const ContainerManager::Container container =
        manager->createContainer(QLatin1String("PaintBox"),
                                 QColor(Qt::blue));

    // Tab 1: the to-sleep tab (loaded so the JS probe can run).
    widget.newTab();
    widget.newTab();
    widget.webView(1)->loadUrl(
        QUrl(QLatin1String("data:text/html,<p>zz</p>")));
    // Tabs 2+3: a named group; 4+5: a group that gets collapsed.
    widget.newTab();
    widget.newTab();
    const QString gid = widget.createTabGroup(2);
    widget.groupTabWith(3, 2);
    widget.renameTabGroup(gid, QLatin1String("Painted"));
    widget.newTab();
    widget.newTab();
    const QString collapsed = widget.createTabGroup(4);
    widget.groupTabWith(5, 4);
    widget.renameTabGroup(collapsed, QLatin1String("Fold"));
    widget.setTabGroupCollapsed(collapsed, true);
    // Tab 6: the container tab.
    widget.makeNewTabInContainer(container.id, false);
    widget.setCurrentIndex(0);
    QApplication::processEvents();

    // Sleep tab 1 once it has finished loading so the "zZ" badge
    // branch has a tab to draw on.
    QTRY_VERIFY_WITH_TIMEOUT(
        !widget.webView(1)->page()->isLoading(), 15000);
    widget.sleepTab(1);
    QTRY_VERIFY_WITH_TIMEOUT(widget.isTabSleeping(1), 15000);

    TabBar *bar = widget.tabBar();
    // Every shape paints a different chip arm (strip edge, pill and
    // badge placement); grab() forces a real paintEvent each time.
    const QTabWidget::TabPosition positions[] = {
        QTabWidget::North, QTabWidget::South,
        QTabWidget::West, QTabWidget::East
    };
    for (QTabWidget::TabPosition position : positions) {
        widget.setTabPosition(position);
        QApplication::processEvents();
        QVERIFY(!bar->grab().isNull());
    }
    widget.setTabPosition(QTabWidget::North);
    QApplication::processEvents();

    // Wake restores the title palette.
    widget.wakeTab(1);
    QVERIFY(!widget.isTabSleeping(1));

    widget.ungroupTabs(gid);
    widget.setTabGroupCollapsed(collapsed, false);
    widget.ungroupTabs(collapsed);
    manager->deleteContainer(container.id);
}

// The mouse/drag/wheel/leave handlers on a real strip: drop-stacking,
// collapsed-chip expand, double-click new tab, middle-click close,
// and the preview-drop paths in wheel/leave.
void tst_TabBar::tabMouseAndDragEvents()
{
    TabWidget widget;
    widget.resize(800, 240);
    widget.show();
    TabBar *bar = widget.tabBar();
    widget.newTab();
    widget.newTab();
    widget.newTab();
    QCOMPARE(widget.count(), 3);
    QApplication::processEvents();

    auto sendMouse = [bar](QEvent::Type type, const QPoint &pos,
                           Qt::MouseButton button,
                           Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(pos), QPointF(pos),
                          button, buttons, Qt::NoModifier);
        QApplication::sendEvent(bar, &event);
    };

    // Drop-stacking: left-press starts tracking; a tabMoved during the
    // drag records the landing index; releasing over the middle of a
    // different tab groups them.
    sendMouse(QEvent::MouseButtonPress, bar->tabRect(0).center(),
              Qt::LeftButton, Qt::LeftButton);
    bar->moveTab(0, 1);
    sendMouse(QEvent::MouseButtonRelease, bar->tabRect(2).center(),
              Qt::LeftButton, Qt::NoButton);
    const QString draggedGroup = widget.tabGroupId(1);
    QVERIFY(!draggedGroup.isEmpty());
    QCOMPARE(widget.tabGroupId(2), draggedGroup);

    // Releasing over a tab's EDGE band is a reorder, not a stack —
    // and moving the remaining ungrouped tab off the strip's start
    // does not join it to the adjacent group (only a drop BETWEEN two
    // members of the same group normalizes in).
    sendMouse(QEvent::MouseButtonPress, bar->tabRect(0).center(),
              Qt::LeftButton, Qt::LeftButton);
    bar->moveTab(0, 2);
    const QRect edge = bar->tabRect(0);
    sendMouse(QEvent::MouseButtonRelease,
              QPoint(edge.left() + 2, edge.center().y()),
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(widget.tabGroupSize(draggedGroup), 2);
    QCOMPARE(widget.tabGroupId(2), QString());

    // A collapsed group's chip expands on press.
    widget.setTabGroupCollapsed(draggedGroup, true);
    int chipIndex = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.isTabGroupChip(i)) {
            chipIndex = i;
            break;
        }
    }
    QVERIFY(chipIndex >= 0);
    sendMouse(QEvent::MouseButtonPress, bar->tabRect(chipIndex).center(),
              Qt::LeftButton, Qt::LeftButton);
    QVERIFY(!widget.tabGroupIsCollapsed(draggedGroup));
    sendMouse(QEvent::MouseButtonRelease,
              bar->tabRect(chipIndex).center(),
              Qt::LeftButton, Qt::NoButton);

    // Double-click on empty strip space requests a new tab; on a tab
    // it does not.
    QSignalSpy newTabs(bar, &TabBar::newTab);
    const QRect lastRect = bar->tabRect(bar->count() - 1);
    const QPoint emptyPoint(lastRect.right() + 2,
                            lastRect.center().y());
    QVERIFY(bar->tabAt(emptyPoint) == -1);
    sendMouse(QEvent::MouseButtonDblClick, emptyPoint,
              Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(newTabs.count(), 1);
    sendMouse(QEvent::MouseButtonDblClick, bar->tabRect(0).center(),
              Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(newTabs.count(), 1);
    QCOMPARE(widget.count(), 4); // the emitted newTab opened one

    // Middle-click on a tab emits closeTab; on empty space it pastes.
    QSignalSpy closed(bar, QOverload<int>::of(&TabBar::closeTab));
    sendMouse(QEvent::MouseButtonRelease, bar->tabRect(0).center(),
              Qt::MiddleButton, Qt::NoButton);
    QCOMPARE(closed.count(), 1);

    // Wheel + leave only drop the hover preview bookkeeping.
    QWheelEvent wheel(QPointF(bar->rect().center()),
                      QPointF(bar->mapToGlobal(bar->rect().center())),
                      QPoint(), QPoint(0, -120),
                      Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QApplication::sendEvent(bar, &wheel);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(bar, &leave);
}

// Ctrl+1..9 TabShortcuts select tabs by index; the view-tab-bar
// action flips the always-show preference.
void tst_TabBar::tabKeyShortcuts()
{
    TabWidget widget;
    widget.resize(640, 240);
    widget.show();
    TabBar *bar = widget.tabBar();
    widget.newTab();
    widget.newTab();
    widget.newTab();
    QCOMPARE(widget.count(), 3);

    const QList<TabShortcut *> shortcuts =
        bar->findChildren<TabShortcut *>();
    QCOMPARE(shortcuts.count(), 9);
    for (TabShortcut *shortcut : shortcuts) {
        if (shortcut->tab() == 1) {
            QMetaObject::invokeMethod(shortcut, "activated");
            break;
        }
    }
    QCOMPARE(widget.currentIndex(), 1);

    // The view-tab-bar action only arms on a single-tab strip.
    widget.closeTab(0);
    widget.closeTab(0);
    QCOMPARE(widget.count(), 1);
    QAction *toggle = bar->viewTabBarAction();
    QVERIFY(toggle->isEnabled());
    const bool before = bar->showTabBarWhenOneTab();
    toggle->trigger();
    QCOMPARE(bar->showTabBarWhenOneTab(), !before);
    toggle->trigger();
    QCOMPARE(bar->showTabBarWhenOneTab(), before);
}

// POL02: hovering a tab arms the 500ms dwell timer; the card shows,
// retargets on a fresh tab, and drops on leave.
void tst_TabBar::previewDwell()
{
    TabWidget widget;
    widget.resize(640, 240);
    widget.show();
    TabBar *bar = widget.tabBar();
    widget.newTab();
    widget.newTab();
    QApplication::processEvents();

    QMouseEvent hover(QEvent::MouseMove,
                      QPointF(bar->tabRect(1).center()),
                      QPointF(bar->tabRect(1).center()),
                      Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &hover);
    QTRY_VERIFY_WITH_TIMEOUT(bar->tabPreview()
                                 && bar->tabPreview()->isVisible(),
                             3000);

    // Fresh hover retargets the live card instantly.
    QMouseEvent rehover(QEvent::MouseMove,
                        QPointF(bar->tabRect(0).center()),
                        QPointF(bar->tabRect(0).center()),
                        Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &rehover);
    QTRY_VERIFY_WITH_TIMEOUT(bar->tabPreview()->isVisible(), 3000);

    // A press drops the card for good.
    QMouseEvent press(QEvent::MouseButtonPress,
                      QPointF(bar->tabRect(0).center()),
                      QPointF(bar->tabRect(0).center()),
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &press);
    QVERIFY(!bar->tabPreview()->isVisible());
    QMouseEvent release(QEvent::MouseButtonRelease,
                        QPointF(bar->tabRect(0).center()),
                        QPointF(bar->tabRect(0).center()),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &release);
}

QTEST_MAIN(tst_TabBar)
#include "tst_tabbar.moc"

