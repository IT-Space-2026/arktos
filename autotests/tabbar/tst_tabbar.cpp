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
#include <qstyleoption.h>
#include <qtoolbutton.h>
#include <tabbar.h>

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

    void tabStripHeight();

    void middleClickPaste();

    void verticalTabRows();
    void verticalTabResize();
    void verticalTabWidthMigration();
    void horizontalTabResize();
    void verticalTabButtons();
    void iconOnlyTabs();
    void newTabButton();
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

    void call_mouseDoubleClickEvent(QMouseEvent *event)
        { return SubTabBar::mouseDoubleClickEvent(event); }

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

    QSize call_baseTabSizeHint(int index) const
        { return QTabBar::tabSizeHint(index); }

    void call_initStyleOption(QStyleOptionTab *option, int tabIndex) const
        { return SubTabBar::initStyleOption(option, tabIndex); }
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_TabBar::initTestCase()
{
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
    // A mid-test QVERIFY exit would otherwise leak the icon-only key
    // into the shared store — later tests' bars would build at the
    // icon tile width and fail their geometry legs.
    QSettings settings;
    settings.remove(QLatin1String("tabs/iconOnlyTabs"));
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

// TABS07: the strip stays at browser-normal icon+text height — per-tab
// button widgets (favicon label, close button) and generous style
// padding must not stretch it.  The thickness is floored at one text
// line plus breathing room and capped at the content line plus modest
// padding.
void tst_TabBar::tabStripHeight()
{
    SubTabBar bar;
    bar.setDocumentMode(true);
    bar.setExpanding(false);
    bar.setPerTabCloseButtons(true);
    bar.addTab(QLatin1String("Untitled"));

    // Production installs a favicon QLabel on the free side too.
    QLabel *favicon = new QLabel(&bar);
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::red);
    favicon->setPixmap(pixmap);
    bar.setTabButton(0, bar.freeSide(), favicon);

    const int fmH = bar.fontMetrics().height();
    const int floorH = fmH + 8;
    const int capH = qMax(fmH, bar.iconSize().height()) + 9;
    const QSize natural = bar.call_baseTabSizeHint(0);
    const QSize hint = bar.call_tabSizeHint(0);
    QCOMPARE(hint.height(), qBound(floorH, natural.height(), capH));
    QVERIFY2(hint.height() <= capH,
             qPrintable(QStringLiteral(
                 "tab height %1 exceeds the icon+text cap %2")
                .arg(hint.height()).arg(capH)));

    // The installed close button is bounded to the text line and both
    // buttons stay vertically centered inside the tab rect.
    const QTabBar::ButtonPosition closeSide =
        (bar.freeSide() == QTabBar::RightSide)
            ? QTabBar::LeftSide : QTabBar::RightSide;
    QWidget *close = bar.tabButton(0, closeSide);
    QVERIFY(close);
    QVERIFY(close->height() <= qBound(12, fmH, 16));

    bar.show();
    bar.resize(400, bar.sizeHint().height());
    QApplication::processEvents();
    const QRect rect = bar.tabRect(0);
    QVERIFY(qAbs(close->geometry().center().y() - rect.center().y()) <= 2);
    QVERIFY(qAbs(favicon->geometry().center().y() - rect.center().y()) <= 2);
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

    // TABS16: the roomier 180px default is back — TABS10's 140
    // clipped typical titles at ~10-12 chars.
    QCOMPARE(bar.verticalTabWidth(), 180);
    // Rows: full strip width, one text line tall, stacked.
    const int rowHeight = bar.tabRect(0).height();
    QVERIFY(rowHeight >= bar.fontMetrics().height());
    QVERIFY(rowHeight <= bar.fontMetrics().height() + 12);
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

    // Offscreen grab for visual review — set ARORA_TABBAR_GRAB_DIR
    // to a directory to save it.
    const QByteArray grabDir = qgetenv("ARORA_TABBAR_GRAB_DIR");
    if (!grabDir.isEmpty())
        image.save(QString::fromLocal8Bit(grabDir)
                   + QLatin1String("/vertical-rows.png"));

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

    // RSZ02: double-clicking the grip resets to the TABS16 180
    // default and persists like a drag release.
    const QPoint grip(bar.width() - 3, 100);
    QMouseEvent dclick(QEvent::MouseButtonDblClick, QPointF(grip),
                       QPointF(bar.mapToGlobal(grip)),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    bar.call_mouseDoubleClickEvent(&dclick);
    QCOMPARE(bar.verticalTabWidth(), 180);
    QCOMPARE(
        settings.value(QLatin1String("verticalTabWidth")).toInt(), 180);

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.endGroup();
}

// TABS16: first-run users get the roomier 180 default and a stored
// past default migrates forward (it means the grip was never
// dragged to a pick); a genuinely user-picked width — including one
// set after the migration — stays untouched.
void tst_TabBar::verticalTabWidthMigration()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));

    // No key at all -> the default.
    settings.remove(QLatin1String("verticalTabWidth"));
    {
        SubTabBar bar;
        QCOMPARE(bar.verticalTabWidth(), 180);
    }

    // A stored TABS10-era compact default -> migrated.
    settings.setValue(QLatin1String("verticalTabWidth"), 140);
    {
        SubTabBar bar;
        QCOMPARE(bar.verticalTabWidth(), 180);
        bar.reloadVerticalTabWidth();
        QCOMPARE(bar.verticalTabWidth(), 180);
    }

    // A stored pre-TABS10 180 is already the new default —
    // indistinguishable from a pick and the same outcome either way.
    settings.setValue(QLatin1String("verticalTabWidth"), 180);
    {
        SubTabBar bar;
        QCOMPARE(bar.verticalTabWidth(), 180);
    }

    // A user-set width -> preserved, and the loadSettings reload path
    // agrees.
    settings.setValue(QLatin1String("verticalTabWidth"), 260);
    {
        SubTabBar bar;
        QCOMPARE(bar.verticalTabWidth(), 260);
        bar.reloadVerticalTabWidth();
        QCOMPARE(bar.verticalTabWidth(), 260);
    }

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.endGroup();
}

// RSZ02: Top/Bottom strips get the same contract — the strip's
// content-facing edge is a drag grip for its HEIGHT
// (tabs/horizontalTabHeight, 20-64, 0 = Automatic), the hover reads
// as a resize cursor, and a double-click restores Automatic.
void tst_TabBar::horizontalTabResize()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("horizontalTabHeight"));

    SubTabBar bar;
    bar.setShape(QTabBar::RoundedNorth);
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("one"));
    bar.addTab(QLatin1String("two"));
    bar.resize(400, 40);
    QApplication::processEvents();
    // 0 = Automatic — the metrics-bound height drives.
    QCOMPARE(bar.tabStripHeight(), 0);

    // The grip zone is the strip's content-facing edge — the bottom
    // of a North bar, the top of a South bar.
    const auto gripPoint = [&bar]() -> QPoint {
        return (bar.shape() == QTabBar::RoundedSouth)
            ? QPoint(50, 3) : QPoint(50, bar.height() - 3);
    };
    const auto dragGrip = [&bar, &gripPoint](int dy) {
        const QPoint grip = gripPoint();
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(grip),
                          QPointF(bar.mapToGlobal(grip)),
                          Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        bar.call_mousePressEvent(&press);
        const QPoint target = grip + QPoint(0, dy);
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

    // From Automatic the drag measures against the strip's rendered
    // height (40px here), so a small pull doesn't snap to the floor.
    dragGrip(10);
    QCOMPARE(bar.tabStripHeight(), 50);
    QCOMPARE(
        settings.value(QLatin1String("horizontalTabHeight")).toInt(),
        50);
    // The override feeds the tab hint (plus any container chip).
    QCOMPARE(bar.call_tabSizeHint(0).height(), 50);

    // Clamps at both ends.
    dragGrip(500);
    QCOMPARE(bar.tabStripHeight(), 64);
    dragGrip(-500);
    QCOMPARE(bar.tabStripHeight(), 20);

    // A grip press is not a tab press — the current tab doesn't
    // change and the press arms no move drag.
    bar.setCurrentIndex(0);
    dragGrip(10);
    QCOMPARE(bar.currentIndex(), 0);

    // Double-click restores Automatic and persists the 0.
    QMouseEvent dclick(QEvent::MouseButtonDblClick,
                       QPointF(gripPoint()),
                       QPointF(bar.mapToGlobal(gripPoint())),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    bar.call_mouseDoubleClickEvent(&dclick);
    QCOMPARE(bar.tabStripHeight(), 0);
    QCOMPARE(
        settings.value(QLatin1String("horizontalTabHeight")).toInt(),
        0);

    // Bottom (South) bars mirror the direction — dragging up grows
    // the strip, seeded from the same rendered height.
    bar.setShape(QTabBar::RoundedSouth);
    dragGrip(-10);
    QCOMPARE(bar.tabStripHeight(), 50);

    settings.remove(QLatin1String("horizontalTabHeight"));
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

// TABS09: icon-only collapse — a persisted, bar-level view mode that
// shrinks every tab to its favicon tile, keeps the real title,
// restores on untoggle, and fans out to every live strip.
void tst_TabBar::iconOnlyTabs()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("iconOnlyTabs"));

    SubTabBar bar;
    bar.setPerTabCloseButtons(true);
    // Non-expanding so tab rects land at their size hints.
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("a tab with a fairly long title"));
    bar.addTab(QLatin1String("two"));
    bar.addTab(QLatin1String("three"));
    bar.resize(400, bar.sizeHint().height());
    QApplication::processEvents();

    QVERIFY(!bar.iconOnlyTabs());
    QAction *action = bar.iconOnlyTabsAction();
    QVERIFY(action);
    QVERIFY(action->isCheckable());
    QVERIFY(!action->isChecked());
    const int iconWidth = bar.iconSize().width() + 20;
    const int normalWidth = bar.call_tabSizeHint(0).width();
    QVERIFY(normalWidth > iconWidth);
    const int normalRect = bar.tabRect(0).width();

    // Triggering the shared action collapses every tab to the
    // favicon tile and persists the mode.
    action->trigger();
    QApplication::processEvents();
    QVERIFY(bar.iconOnlyTabs());
    QVERIFY(action->isChecked());
    QCOMPARE(settings.value(QLatin1String("iconOnlyTabs")).toBool(),
             true);
    for (int i = 0; i < bar.count(); ++i)
        QCOMPARE(bar.call_tabSizeHint(i).width(), iconWidth);
    QVERIFY(bar.tabRect(0).width() <= iconWidth + 8);
    QVERIFY(bar.tabRect(0).width() < normalRect);
    // The painted option loses the title only — the tab's own text
    // is untouched: a view mode, not a data change.
    QStyleOptionTab option;
    bar.call_initStyleOption(&option, 0);
    QVERIFY(option.text.isEmpty());
    QCOMPARE(bar.tabText(0),
             QLatin1String("a tab with a fairly long title"));

    // New tabs inherit the mode — it is the strip's, not the tab's.
    bar.addTab(QLatin1String("four"));
    QCOMPARE(bar.call_tabSizeHint(3).width(), iconWidth);

    // A strip constructed while the key is set starts in the mode —
    // the restart/persistence contract.
    SubTabBar other;
    QVERIFY(other.iconOnlyTabs());
    QVERIFY(other.iconOnlyTabsAction()->isChecked());

    // A vertical strip collapses to an icon rail of the same width.
    SubTabBar vbar;
    vbar.setShape(QTabBar::RoundedWest);
    vbar.setExpanding(false);
    vbar.addTab(QLatin1String("one"));
    QVERIFY(vbar.iconOnlyTabs());
    QCOMPARE(vbar.call_tabSizeHint(0).width(), iconWidth);

    // Untoggling one bar fans the mode back out to every live strip.
    action->trigger();
    QApplication::processEvents();
    QVERIFY(!bar.iconOnlyTabs());
    QVERIFY(!action->isChecked());
    QVERIFY(!other.iconOnlyTabs());
    QVERIFY(!other.iconOnlyTabsAction()->isChecked());
    QVERIFY(!vbar.iconOnlyTabs());
    QCOMPARE(settings.value(QLatin1String("iconOnlyTabs")).toBool(),
             false);
    QCOMPARE(bar.call_tabSizeHint(0).width(), normalWidth);

    settings.remove(QLatin1String("iconOnlyTabs"));
    settings.endGroup();
}

// TABS15: the '+' is a real button docked inside the strip right
// after the last tab in every orientation — trailing the row on
// Top/Bottom, a full-width appended row on Left/Right — and it
// fires the same newTab() a double-click on the empty strip does.
void tst_TabBar::newTabButton()
{
    SubTabBar bar;
    // Non-expanding so tab rects land at their size hints and free
    // strip space follows the last tab.
    bar.setExpanding(false);
    bar.show();
    bar.addTab(QLatin1String("one"));
    bar.addTab(QLatin1String("two"));
    bar.resize(400, bar.sizeHint().height());
    QApplication::processEvents();

    QToolButton *button = bar.newTabButton();
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QVERIFY(!button->toolTip().isEmpty());
    QSignalSpy spy(&bar, &TabBar::newTab);

    // Top strip: anchored just after the last tab's rect, inside
    // the widget vertically.
    const QRect last = bar.tabRect(bar.count() - 1);
    QVERIFY(button->x() >= last.right());
    QVERIFY(button->x() <= last.right() + 8);
    QVERIFY(button->y() >= 0 && button->y() + button->height()
            <= bar.height());
    button->click();
    QCOMPARE(spy.count(), 1);

    // Bottom strip keeps the same after-last-tab anchor.
    bar.setShape(QTabBar::RoundedSouth);
    bar.resize(400, bar.sizeHint().height());
    QApplication::processEvents();
    QVERIFY(button->isVisible());
    const QRect slast = bar.tabRect(bar.count() - 1);
    QVERIFY(button->x() >= slast.right());
    QVERIFY(button->x() <= slast.right() + 8);
    button->click();
    QCOMPARE(spy.count(), 2);

    // Left rail: an appended row under the last tab, spanning the
    // strip's width.
    bar.setShape(QTabBar::RoundedWest);
    bar.resize(160, 300);
    QApplication::processEvents();
    QVERIFY(button->isVisible());
    const QRect wlast = bar.tabRect(bar.count() - 1);
    QVERIFY(button->y() >= wlast.bottom());
    QVERIFY(button->y() <= wlast.bottom() + 8);
    QVERIFY(button->width() >= bar.width() - 8);
    button->click();
    QCOMPARE(spy.count(), 3);

    // Right rail mirrors it.
    bar.setShape(QTabBar::RoundedEast);
    bar.resize(160, 300);
    QApplication::processEvents();
    QVERIFY(button->isVisible());
    const QRect elast = bar.tabRect(bar.count() - 1);
    QVERIFY(button->y() >= elast.bottom());
    QVERIFY(button->y() <= elast.bottom() + 8);
    QVERIFY(button->width() >= bar.width() - 8);
    button->click();
    QCOMPARE(spy.count(), 4);

    // Offscreen grabs for visual review — set ARORA_TABBAR_GRAB_DIR.
    const QByteArray grabDir = qgetenv("ARORA_TABBAR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        const QString base = QString::fromLocal8Bit(grabDir);
        bar.setShape(QTabBar::RoundedNorth);
        bar.resize(400, bar.sizeHint().height());
        QApplication::processEvents();
        bar.grab().save(base + QLatin1String("/plus-top.png"));
        bar.setShape(QTabBar::RoundedSouth);
        QApplication::processEvents();
        bar.grab().save(base + QLatin1String("/plus-bottom.png"));
        bar.setShape(QTabBar::RoundedWest);
        bar.resize(160, 300);
        QApplication::processEvents();
        bar.grab().save(base + QLatin1String("/plus-left.png"));
        bar.setShape(QTabBar::RoundedEast);
        QApplication::processEvents();
        bar.grab().save(base + QLatin1String("/plus-right.png"));
    }
}

QTEST_MAIN(tst_TabBar)
#include "tst_tabbar.moc"

