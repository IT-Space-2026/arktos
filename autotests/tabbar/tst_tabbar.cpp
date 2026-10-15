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
    void verticalRowHeight();
    void firefoxTabMetrics();
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

    QSize call_minimumTabSizeHint(int index) const
        { return SubTabBar::minimumTabSizeHint(index); }

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

    // TABS19: every titled tab reports the shared 225px Firefox
    // metric — a long title elides inside it instead of growing.
    QCOMPARE(bar.call_tabSizeHint(index).width(), 225);
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

// TABS19: the strip sits at Firefox's normal-density metrics — the
// tab face is pinned at content + 17 (a ~33px face on a ~36px
// strip), so per-tab button widgets (favicon label, close button)
// and generous style padding neither stretch nor shrink it.
void tst_TabBar::tabStripHeight()
{
    SubTabBar bar;
    bar.setDocumentMode(true);
    bar.setPerTabCloseButtons(true);
    bar.addTab(QLatin1String("Untitled"));

    // Production installs a favicon QLabel on the free side too.
    QLabel *favicon = new QLabel(&bar);
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::red);
    favicon->setPixmap(pixmap);
    bar.setTabButton(0, bar.freeSide(), favicon);

    const int fmH = bar.fontMetrics().height();
    const int face = qMax(fmH, bar.iconSize().height()) + 17;
    const QSize hint = bar.call_tabSizeHint(0);
    QCOMPARE(hint.height(), face);
    // Standard-DPI sanity: 9pt text / 16px favicon lands at ~33px.
    QVERIFY2(face >= 30 && face <= 40,
             qPrintable(QStringLiteral(
                 "tab face %1 outside the ~33px Firefox band")
                .arg(face)));

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
    // Rows: full strip width, one uniform-height band, stacked.
    // TABS19: the row adopts the same ~33px pad the horizontal face
    // uses — content + 17.
    const int rowHeight = bar.tabRect(0).height();
    const int rowFace = qMax(bar.fontMetrics().height(),
                             bar.iconSize().height()) + 17;
    QCOMPARE(rowHeight, rowFace);
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
    // TABS19: 225px tabs — give the strip room past the run so the
    // '+' trails it in free space.
    bar.resize(560, bar.sizeHint().height());
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
    bar.resize(560, bar.sizeHint().height());
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

    // TABS19: when the tabs fill the strip the '+' parks on its
    // right edge — and the last tab's close button, which would sit
    // under it, yields rather than overlap a dead click target.
    bar.setShape(QTabBar::RoundedNorth);
    bar.setPerTabCloseButtons(true);
    bar.resize(448, bar.sizeHint().height());   // 2 * 225 squeezes
    bar.setCurrentIndex(bar.count() - 1);
    QApplication::processEvents();
    const QRect full = bar.tabRect(bar.count() - 1);
    QVERIFY(full.right() >= bar.width() - 2);
    QVERIFY(button->isVisible());
    QVERIFY(button->x() + button->width() <= bar.width());
    QVERIFY(button->x() < full.right());   // parked over the edge
    QWidget *lastClose = bar.tabButton(
        bar.count() - 1,
        bar.freeSide() == QTabBar::LeftSide ? QTabBar::RightSide
                                            : QTabBar::LeftSide);
    QVERIFY(lastClose);
    QVERIFY(!lastClose->isVisible());

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

// TABS17: a Left/Right rail's rows never render shorter than the
// horizontal automatic path's thickness — and a dragged-in strip
// height (tabs/horizontalTabHeight, clamped 20-64 by the setter)
// feeds the rows only down to that same floor, so a too-small
// persisted value can't crop the title under the row's clip rect.
// TABS19: the floor is now Firefox's ~33px face (content + 17),
// shared with the horizontal strip; icon-only tiles keep their
// favicon sizing either way.
void tst_TabBar::verticalRowHeight()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("horizontalTabHeight"));

    for (const QTabBar::Shape shape :
         { QTabBar::RoundedWest, QTabBar::RoundedEast }) {
        SubTabBar bar;
        bar.setShape(shape);
        bar.setExpanding(false);
        bar.show();
        bar.addTab(QLatin1String("one"));
        bar.addTab(QLatin1String("two"));
        bar.resize(200, 300);
        QApplication::processEvents();

        const int floor = qMax(bar.fontMetrics().height(),
                               bar.iconSize().height()) + 17;
        QCOMPARE(bar.call_tabSizeHint(0).height(), floor);
        for (int i = 0; i < bar.count(); ++i) {
            QVERIFY(bar.tabRect(i).height() >= floor);
            QCOMPARE(bar.call_tabSizeHint(i).height(), floor);
        }

        // A persisted height below the floor clamps up instead of
        // producing an unreadable row.
        bar.setTabStripHeight(20);
        QApplication::processEvents();
        for (int i = 0; i < bar.count(); ++i) {
            QVERIFY(bar.call_tabSizeHint(i).height() >= floor);
            QVERIFY(bar.tabRect(i).height() >= floor);
        }

        // A taller configured height is honored on the rows.
        bar.setTabStripHeight(40);
        QApplication::processEvents();
        QCOMPARE(bar.call_tabSizeHint(0).height(), 40);
        QCOMPARE(bar.tabRect(0).height(), 40);

        // Automatic restores the text-line floor.
        bar.setTabStripHeight(0);
        QApplication::processEvents();
        QCOMPARE(bar.call_tabSizeHint(0).height(), floor);

        // Icon-only collapses the row's width axis only — its height
        // follows the same floor/configured contract as titled rows.
        settings.setValue(QLatin1String("iconOnlyTabs"), true);
        SubTabBar rail;
        rail.setShape(shape);
        rail.addTab(QLatin1String("x"));
        QCOMPARE(rail.call_tabSizeHint(0).height(), floor);
        rail.setTabStripHeight(40);
        QCOMPARE(rail.call_tabSizeHint(0).height(), 40);
        settings.remove(QLatin1String("iconOnlyTabs"));
    }

    // The horizontal strip keeps its own contract — Automatic bounds
    // and the configured override both untouched by the row floor.
    SubTabBar hbar;
    hbar.setShape(QTabBar::RoundedNorth);
    hbar.addTab(QLatin1String("one"));
    const int natural = hbar.call_tabSizeHint(0).height();
    hbar.setTabStripHeight(20);
    QCOMPARE(hbar.call_tabSizeHint(0).height(), 20);
    hbar.setTabStripHeight(0);
    QCOMPARE(hbar.call_tabSizeHint(0).height(), natural);

    settings.endGroup();
}

// TABS19: Firefox's tab geometry — every titled tab is 225px wide
// while space allows (a lone tab included, left-aligned with free
// strip behind it), the squeeze shares the strip equally down to
// the 76px floor, and only then do the scroll buttons take over.
// The non-expanding documentMode strip is what makes the shared
// hint the rendered width.
void tst_TabBar::firefoxTabMetrics()
{
    SubTabBar bar;
    bar.setDocumentMode(true);
    bar.show();
    bar.addTab(QLatin1String("one"));
    QCOMPARE(bar.call_tabSizeHint(0).width(), 225);
    QCOMPARE(bar.call_tabSizeHint(0).height(),
             qMax(bar.fontMetrics().height(),
                  bar.iconSize().height()) + 17);
    QCOMPARE(bar.call_minimumTabSizeHint(0).width(), 76);

    // A lone tab keeps the Firefox metric — it does not stretch to
    // fill the strip the way an expanding bar would.
    bar.resize(800, bar.sizeHint().height());
    QApplication::processEvents();
    QCOMPARE(bar.tabRect(0).width(), 225);
    QCOMPARE(bar.tabRect(0).x(), 0);

    // Ample space: every tab — whatever its title length — gets the
    // same 225px, left-aligned with the free run after the last.
    bar.addTab(QLatin1String("A medium length title"));
    bar.addTab(QLatin1String(
        "A much longer tab title that would have stretched"));
    bar.resize(800, bar.sizeHint().height());
    QApplication::processEvents();
    for (int i = 0; i < bar.count(); ++i)
        QCOMPARE(bar.tabRect(i).width(), 225);
    QCOMPARE(bar.tabRect(0).x(), 0);
    QCOMPARE(bar.tabRect(1).x(), 225);
    QCOMPARE(bar.tabRect(2).x(), 450);

    // The squeeze: tabs shrink to equal widths sharing the strip
    // until the 76px floor, then the scroll path takes over.
    bar.resize(500, bar.sizeHint().height());
    QApplication::processEvents();
    const int w0 = bar.tabRect(0).width();
    QVERIFY(w0 > 76 && w0 < 225);
    // The share-out may hand a remainder pixel to one tab.
    QVERIFY(qAbs(bar.tabRect(1).width() - w0) <= 1);
    QVERIFY(qAbs(bar.tabRect(2).width() - w0) <= 1);
    // 3 * 76 = 228 barely fits a 230px strip — no squeeze below the
    // ~76px floor (Qt may hand out a remainder pixel); tighter still
    // and the scroll path takes over.
    bar.resize(230, bar.sizeHint().height());
    QApplication::processEvents();
    for (int i = 0; i < bar.count(); ++i) {
        QVERIFY(bar.tabRect(i).width() >= 76);
        QVERIFY(bar.tabRect(i).width() <= 78);
    }
    bar.resize(150, bar.sizeHint().height());
    QApplication::processEvents();
    for (int i = 0; i < bar.count(); ++i)
        QVERIFY(bar.tabRect(i).width() >= 76);
    // Tabs past the visible run scrolled off the left edge.
    QVERIFY(bar.tabRect(bar.count() - 1).right()
            > bar.width() || bar.tabRect(0).x() < 0);

    // Offscreen grabs for visual review — 1 tab at the metric,
    // many in equal-width squeeze.  ARORA_TABBAR_GRAB_DIR to save.
    const QByteArray grabDir = qgetenv("ARORA_TABBAR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        const QString base = QString::fromLocal8Bit(grabDir);
        SubTabBar lone;
        lone.setDocumentMode(true);
        lone.show();
        lone.addTab(QLatin1String("Lone tab"));
        lone.resize(800, lone.sizeHint().height());
        QApplication::processEvents();
        lone.grab().save(base + QLatin1String("/ff-one.png"));
        SubTabBar many;
        many.setDocumentMode(true);
        many.show();
        for (int i = 0; i < 8; ++i)
            many.addTab(QStringLiteral("Tab %1").arg(i + 1));
        many.resize(800, many.sizeHint().height());
        QApplication::processEvents();
        many.grab().save(base + QLatin1String("/ff-many.png"));
    }
}

QTEST_MAIN(tst_TabBar)
#include "tst_tabbar.moc"

