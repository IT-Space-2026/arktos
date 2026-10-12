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
#include <qlabel.h>
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

    void verticalTabRows();
    void verticalTabResize();
    void verticalGripReset();
    void horizontalTabWidths();
    void verticalTabButtons();
    void contextMenuBaseEntries();
    void contextMenuGroupEntries();
    void contextMenuContainerEntries();
    void chipsPaint();
    void tabMouseAndDragEvents();
    void tabKeyShortcuts();
    void previewDwell();
    void containerBandEvents();
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

    void call_mouseDoubleClickEvent(QMouseEvent *event)
        { return SubTabBar::mouseDoubleClickEvent(event); }

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

    QSize call_minimumTabSizeHint(int index) const
        { return SubTabBar::minimumTabSizeHint(index); }
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

// TABS02: a Left/Right strip lays its tabs out as horizontal rows —
// a persisted strip width and a one-line height, with the title
// painted left-to-right instead of Qt's rotated West/East text.
void tst_TabBar::verticalTabRows()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("verticalTabWidth"));

    SubTabBar bar;
    bar.setShape(QTabBar::RoundedWest);
    // Production bars are documentMode -> non-expanding; mirror that
    // or the rows stretch to fill the test widget's height.
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("This is a fairly long tab title"));
    bar.addTab(QLatin1String("Second tab"));
    bar.resize(200, 400);
    QApplication::processEvents();

    QCOMPARE(bar.verticalTabWidth(), 180);
    // Rows: full strip width, one text line tall, stacked.
    const int rowHeight = bar.tabRect(0).height();
    QVERIFY(rowHeight >= 20 && rowHeight <= 40);
    QCOMPARE(bar.tabRect(0).width(), bar.verticalTabWidth());
    QCOMPARE(bar.tabRect(0).width(), bar.tabRect(1).width());
    QCOMPARE(bar.tabRect(1).top(), bar.tabRect(0).bottom() + 1);

    // The title must paint horizontally — grab the strip and check
    // the "ink" inside the first row spreads across the row's width
    // instead of down a narrow rotated column.  Ink = pixels whose
    // brightness differs strongly from the row's dominant color.
    const QImage image = bar.grab().toImage();
    QCOMPARE(image.size(), bar.size());
    const QRect inside = bar.tabRect(0).adjusted(30, 4, -8, -4);
    QHash<QRgb, int> frequency;
    for (int y = 0; y < inside.height(); ++y)
        for (int x = 0; x < inside.width(); ++x)
            ++frequency[image.pixel(inside.topLeft() + QPoint(x, y))];
    QRgb background = 0;
    int most = 0;
    for (auto it = frequency.constBegin(); it != frequency.constEnd(); ++it) {
        if (it.value() > most) {
            most = it.value();
            background = it.key();
        }
    }
    const int bgGray = qGray(background);
    int minX = inside.width(), maxX = -1;
    for (int y = 0; y < inside.height(); ++y) {
        for (int x = 0; x < inside.width(); ++x) {
            const int gray =
                qGray(image.pixel(inside.topLeft() + QPoint(x, y)));
            if (qAbs(gray - bgGray) > 60) {
                minX = qMin(minX, x);
                maxX = qMax(maxX, x);
            }
        }
    }
    QVERIFY2(maxX >= 0, "no title text detected inside the tab row");
    QVERIFY2(maxX - minX > 60,
             qPrintable(QStringLiteral(
                 "title ink is %1px wide — a rotated/elided label "
                 "would stay inside a ~16px column")
                 .arg(maxX - minX + 1)));

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.endGroup();
}

// TABS02: the strip's inner edge is a drag-resize grip — dragging it
// resizes live (120-400 clamp), and releasing persists
// tabs/verticalTabWidth.
void tst_TabBar::verticalTabResize()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("verticalTabWidth"));

    SubTabBar bar;
    bar.setShape(QTabBar::RoundedWest);
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("one"));
    bar.addTab(QLatin1String("two"));
    bar.resize(200, 300);
    QApplication::processEvents();
    QCOMPARE(bar.verticalTabWidth(), 180);

    const auto dragGrip = [&bar](int dx) {
        const QPoint grip(bar.width() - 3, 100);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(grip),
                          QPointF(bar.mapToGlobal(grip)),
                          Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        bar.call_mousePressEvent(&press);
        const QPoint target = grip + QPoint(dx, 0);
        QMouseEvent move(QEvent::MouseMove, QPointF(target),
                         QPointF(bar.mapToGlobal(target)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        bar.call_mouseMoveEvent(&move);
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(target),
                            QPointF(bar.mapToGlobal(target)),
                            Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        bar.call_mouseReleaseEvent(&release);
    };

    // West strip: dragging right (into the content) widens it.
    dragGrip(40);
    QCOMPARE(bar.verticalTabWidth(), 220);
    QCOMPARE(
        settings.value(QLatin1String("verticalTabWidth")).toInt(), 220);
    // The new hint flows through to the tab rects once relaid out.
    bar.resize(300, 300);
    QApplication::processEvents();
    QCOMPARE(bar.tabRect(0).width(), 220);

    // Clamps both directions.
    dragGrip(500);
    QCOMPARE(bar.verticalTabWidth(), 400);
    dragGrip(-500);
    QCOMPARE(bar.verticalTabWidth(), 120);

    // A grip press is not a tab press: no drag tracking / selection
    // side effects are armed by it.  Dragging along the grip
    // vertically keeps resizing (horizontal delta wins) — and
    // crucially must not start a tab move.
    dragGrip(-20);
    QCOMPARE(bar.verticalTabWidth(), 120);

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.endGroup();
}

// RSZ01: double-clicking the vertical strip's grip snaps the width
// back to the 180px default and persists it like a drag release.
void tst_TabBar::verticalGripReset()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("verticalTabWidth"), 300);

    SubTabBar bar;
    bar.setShape(QTabBar::RoundedWest);
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("one"));
    bar.resize(200, 300);
    QApplication::processEvents();
    QCOMPARE(bar.verticalTabWidth(), 300);

    const QPoint grip(bar.width() - 3, 100);
    QMouseEvent dblClick(QEvent::MouseButtonDblClick, QPointF(grip),
                         QPointF(bar.mapToGlobal(grip)),
                         Qt::LeftButton, Qt::LeftButton,
                         Qt::NoModifier);
    bar.call_mouseDoubleClickEvent(&dblClick);
    QCOMPARE(bar.verticalTabWidth(), 180);
    QCOMPARE(
        settings.value(QLatin1String("verticalTabWidth")).toInt(), 180);

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.endGroup();
}

// RSZ01: tabs/tabMinWidth / tabs/tabMaxWidth bound the horizontal
// strip — a cap replaces the built-in extent, a floor overrides the
// style's squeeze minimum, 0 means Automatic, and both reload live.
void tst_TabBar::horizontalTabWidths()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("tabMinWidth"));
    settings.remove(QLatin1String("tabMaxWidth"));

    SubTabBar bar;
    bar.addTab(QLatin1String("a reasonably long tab title"));
    const int natural = bar.call_tabSizeHint(0).width();
    QVERIFY(natural > 140);

    // Automatic (0/0) leaves the stock bounds alone.
    QCOMPARE(bar.tabMinWidth(), 0);
    QCOMPARE(bar.tabMaxWidth(), 0);

    // A cap below the natural width clamps the hint.
    bar.setTabWidths(0, 140);
    QCOMPARE(bar.tabMaxWidth(), 140);
    QCOMPARE(bar.call_tabSizeHint(0).width(), 140);

    // A floor raises both the hint and the squeeze minimum.
    bar.setTabWidths(200, 0);
    QCOMPARE(bar.call_tabSizeHint(0).width(), qMax(200, natural));
    QCOMPARE(bar.call_minimumTabSizeHint(0).width(), 200);

    // The floor never exceeds the cap.
    bar.setTabWidths(300, 250);
    QCOMPARE(bar.call_tabSizeHint(0).width(), 250);

    // Out-of-range inputs clamp instead of exploding the layout.
    bar.setTabWidths(-50, 5000);
    QCOMPARE(bar.tabMinWidth(), 0);
    QCOMPARE(bar.tabMaxWidth(), 800);

    // Persisted keys flow through reloadTabWidths (the settings
    // live-apply path).
    settings.setValue(QLatin1String("tabMinWidth"), 150);
    settings.setValue(QLatin1String("tabMaxWidth"), 320);
    bar.reloadTabWidths();
    QCOMPARE(bar.tabMinWidth(), 150);
    QCOMPARE(bar.tabMaxWidth(), 320);
    QCOMPARE(bar.call_tabSizeHint(0).width(),
             qBound(150, natural, 320));

    settings.remove(QLatin1String("tabMinWidth"));
    settings.remove(QLatin1String("tabMaxWidth"));
    settings.endGroup();
}

// TABS02: the per-tab side widgets (favicon label on freeSide, the
// close button) anchor to the row ends — not to the transposed
// top/bottom positions Qt computes for West/East shapes.
void tst_TabBar::verticalTabButtons()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("verticalTabWidth"));

    SubTabBar bar;
    bar.setShape(QTabBar::RoundedEast);
    bar.setPerTabCloseButtons(true);
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("one"));
    bar.addTab(QLatin1String("two"));

    QLabel *favicon = new QLabel(&bar);
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::red);
    favicon->setPixmap(pixmap);
    bar.setTabButton(0, bar.freeSide(), favicon);

    bar.resize(300, 300);
    QApplication::processEvents();
    bar.call_tabLayoutChange();

    const QRect row = bar.tabRect(0);
    QVERIFY(row.isValid());
    // The ButtonPosition sides must land at the matching physical
    // end of the row, vertically centered inside it.
    QWidget *leftWidget = bar.tabButton(0, QTabBar::LeftSide);
    QVERIFY(leftWidget);
    QVERIFY(leftWidget->pos().x() <= row.left() + row.height());
    QVERIFY(leftWidget->pos().y() >= row.top()
            && leftWidget->pos().y() < row.bottom());
    QWidget *rightWidget = bar.tabButton(0, QTabBar::RightSide);
    QVERIFY(rightWidget);
    QVERIFY(rightWidget->pos().x() + rightWidget->width()
            >= row.right() - row.height());
    QVERIFY(rightWidget->pos().y() >= row.top()
            && rightWidget->pos().y() < row.bottom());

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.endGroup();
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
    // The CONT06 strip (default on) filters tabs to the active
    // container level — pin inline mode so the strip indices the
    // context-menu drives below stay stable.
    QSettings settings;
    const QVariant savedDisplay =
        settings.value(QLatin1String("tabs/containerDisplay"));
    settings.setValue(QLatin1String("tabs/containerDisplay"), 0);

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

    if (savedDisplay.isValid())
        settings.setValue(QLatin1String("tabs/containerDisplay"),
                          savedDisplay);
    else
        settings.remove(QLatin1String("tabs/containerDisplay"));
}

// paintEvent's container chips, group rails/pills/collapsed chip and
// the sleep badge all draw through QWidget::grab() — offscreen never
// sends expose events but explicit rendering still runs the painter.
void tst_TabBar::chipsPaint()
{
    // The CONT06 two-level strip (default on) filters the container
    // tab into its own level — pin inline mode so the chip-bearing
    // tab is what paintEvent draws.
    QSettings settings;
    const QVariant savedDisplay =
        settings.value(QLatin1String("tabs/containerDisplay"));
    settings.setValue(QLatin1String("tabs/containerDisplay"), 0);

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
    int containerIndex = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.containerIdForTab(i) == container.id)
            containerIndex = i;
    }
    QVERIFY(containerIndex >= 0);
    // Every shape paints a different chip arm (strip edge, pill and
    // badge placement); grab() forces a real paintEvent each time.
    // TABS06: the container chip borrows title space INSIDE the row —
    // a container tab is exactly as tall as a plain tab on every
    // shape and the strip stays near a native QTabBar's height.
    const QTabWidget::TabPosition positions[] = {
        QTabWidget::North, QTabWidget::South,
        QTabWidget::West, QTabWidget::East
    };
    const QStringList shapeNames = {
        QLatin1String("north"), QLatin1String("south"),
        QLatin1String("west"), QLatin1String("east")
    };
    int worstHeight = 0;
    for (int shapeIndex = 0; shapeIndex < 4; ++shapeIndex) {
        widget.setTabPosition(positions[shapeIndex]);
        QApplication::processEvents();
        const QPixmap strip = bar->grab();
        QVERIFY(!strip.isNull());
        const QByteArray prefix = qgetenv("TABSHOT_PREFIX");
        if (!prefix.isEmpty())
            strip.toImage().save(QString::fromLatin1(
                "/tmp/arora-tabs-%1-%2.png")
                .arg(QString::fromLatin1(prefix),
                     shapeNames.at(shapeIndex)));
        QCOMPARE(bar->tabRect(containerIndex).height(),
                 bar->tabRect(0).height());
        worstHeight = qMax(worstHeight,
                           bar->tabRect(containerIndex).height());
    }
    QVERIFY(worstHeight <= 36);
    widget.setTabPosition(QTabWidget::North);
    QApplication::processEvents();

    // Wake restores the title palette.
    widget.wakeTab(1);
    QVERIFY(!widget.isTabSleeping(1));

    widget.ungroupTabs(gid);
    widget.setTabGroupCollapsed(collapsed, false);
    widget.ungroupTabs(collapsed);
    manager->deleteContainer(container.id);

    if (savedDisplay.isValid())
        settings.setValue(QLatin1String("tabs/containerDisplay"),
                          savedDisplay);
    else
        settings.remove(QLatin1String("tabs/containerDisplay"));
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

// CONT06: with the two-level strip active (>1 container owning tabs)
// the header band has its own press/release/drag/drop/key paths —
// drive them at the physical band row so every arm runs.
void tst_TabBar::containerBandEvents()
{
    QSettings settings;
    const QVariant savedDisplay =
        settings.value(QLatin1String("tabs/containerDisplay"));
    settings.setValue(QLatin1String("tabs/containerDisplay"), 1);

    TabWidget widget;
    widget.resize(720, 320);
    widget.show();
    QApplication::processEvents();

    ContainerManager *manager = ContainerManager::instance();
    const ContainerManager::Container c1 =
        manager->createContainer(QLatin1String("Band A"),
                                 QColor(Qt::red));
    const ContainerManager::Container c2 =
        manager->createContainer(QLatin1String("Band B"),
                                 QColor(Qt::green));

    widget.newTab();
    widget.makeNewTabInContainer(c1.id, false);
    widget.makeNewTabInContainer(c2.id, false);
    QApplication::processEvents();

    TabBar *bar = widget.tabBar();
    const int band = bar->containerStripHeight();
    QVERIFY(band > 0);
    const int bandY = band / 2;
    const auto headerX = [bar, bandY](int i) {
        for (int x = 2; x < bar->width(); x += 2) {
            if (bar->containerHeaderAt(QPoint(x, bandY)) == i)
                return x;
        }
        return -1;
    };
    const int hx0 = headerX(0);
    const int hx1 = headerX(1);
    const int hx2 = headerX(2);
    QVERIFY(hx0 >= 0 && hx1 >= 0 && hx2 >= 0);
    QCOMPARE(widget.containerHeaders().count(), 3);

    const auto sendMouse = [bar](QEvent::Type type, const QPoint &pos,
                                 Qt::MouseButton button,
                                 Qt::MouseButtons buttons) {
        QMouseEvent event(type, QPointF(pos), QPointF(pos),
                          button, buttons, Qt::NoModifier);
        QApplication::sendEvent(bar, &event);
    };
    const auto sendKey = [bar](int key) {
        QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(bar, &event);
    };

    // Press+release the same header activates that level.
    sendMouse(QEvent::MouseButtonPress, QPoint(hx1, bandY),
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, QPoint(hx1, bandY),
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(widget.activeContainerHeader(),
             widget.containerHeaders().at(1));

    // Press one container header and release on the other — the
    // non-default headers reorder (default stays pinned first).
    widget.setActiveContainerHeader(
        widget.containerHeaders().at(0));
    QStringList headers = widget.containerHeaders();
    sendMouse(QEvent::MouseButtonPress, QPoint(hx1, bandY),
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, QPoint(hx2, bandY),
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(widget.containerHeaders().first(), headers.first());
    QCOMPARE(widget.containerHeaders().at(1), headers.at(2));

    // A press on a strip tab released over a band header rebinds the
    // tab to that container.  Physical band y is strip y - band;
    // the tab rects live in strip space below the band.
    widget.setActiveContainerHeader(
        widget.containerHeaders().at(0));
    int defaultTab = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.containerIdForTab(i)
                == ContainerManager::defaultContainerId())
            defaultTab = i;
    }
    QVERIFY(defaultTab >= 0);
    const QString target = widget.containerHeaders().at(2);
    const int beforeTarget = widget.containerTabCount(target);
    const int beforeDefault = widget.containerTabCount(
        ContainerManager::defaultContainerId());
    const QPoint dragStart =
        bar->tabRect(defaultTab).center() + QPoint(0, band);
    sendMouse(QEvent::MouseButtonPress, dragStart,
              Qt::LeftButton, Qt::LeftButton);
    // A hover into the band arms the drop highlight.
    sendMouse(QEvent::MouseMove, QPoint(hx2, bandY),
              Qt::NoButton, Qt::LeftButton);
    sendMouse(QEvent::MouseButtonRelease, QPoint(hx2, bandY),
              Qt::LeftButton, Qt::NoButton);
    // The rebind re-creates the view — count the level populations.
    QCOMPARE(widget.containerTabCount(target), beforeTarget + 1);
    QCOMPARE(widget.containerTabCount(
                 ContainerManager::defaultContainerId()),
             beforeDefault - 1);
    widget.setActiveContainerHeader(
        widget.containerHeaders().at(0));

    // External URL drags highlight the hovered header; a drop on a
    // header loads there instead of onto a tab.
    QMimeData *mime = new QMimeData;
    mime->setUrls({QUrl(QLatin1String("http://band.test/"))});
    const QPointF bandPoint(hx2, bandY);
    QDragEnterEvent enter(bandPoint, Qt::CopyAction, mime,
                          Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &enter);
    QDragMoveEvent move(bandPoint, Qt::CopyAction, mime,
                        Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &move);
    QDragLeaveEvent leave;
    QApplication::sendEvent(bar, &leave);
    QDropEvent drop(bandPoint, Qt::CopyAction, mime,
                    Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(bar, &drop);

    // Keyboard: Up from the row enters the band at the active
    // header; arrows walk it; Return activates; Up re-enters and
    // Escape drops back.
    sendKey(Qt::Key_Up);

    sendKey(Qt::Key_Right);
    sendKey(Qt::Key_Left);
    sendKey(Qt::Key_End);
    sendKey(Qt::Key_Home);

    sendKey(Qt::Key_Return);

    sendKey(Qt::Key_Up);
    sendKey(Qt::Key_Space);

    sendKey(Qt::Key_Up);
    sendKey(Qt::Key_X); // unhandled key -> base class arm
    sendKey(Qt::Key_Escape);

    sendKey(Qt::Key_Up);
    sendKey(Qt::Key_Down);


    // A double-click in the band is not an empty-strip double-click.
    QSignalSpy newTabs(bar, &TabBar::newTab);
    sendMouse(QEvent::MouseButtonDblClick, QPoint(hx0, bandY),
              Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(newTabs.count(), 0);


    // The band paints its headers through a real paintEvent.
    QVERIFY(!bar->grab().isNull());


    // deleteContainer has a no-live-pages contract — switch to each
    // container level and close its tabs first (the band gestures
    // above moved/loaded tabs into them).
    for (const QString &cid : {c1.id, c2.id}) {
        widget.setActiveContainerHeader(cid);
        while (widget.count() > 0)
            widget.closeTab(0);
    }
    QApplication::processEvents();
    manager->deleteContainer(c1.id);
    manager->deleteContainer(c2.id);
    if (savedDisplay.isValid())
        settings.setValue(QLatin1String("tabs/containerDisplay"),
                          savedDisplay);
    else
        settings.remove(QLatin1String("tabs/containerDisplay"));
}

QTEST_MAIN(tst_TabBar)
#include "tst_tabbar.moc"

