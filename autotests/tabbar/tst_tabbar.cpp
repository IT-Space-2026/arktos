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

    void middleClickPaste();

    void verticalTabRows();
    void verticalTabResize();
    void verticalTabButtons();
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

QTEST_MAIN(tst_TabBar)
#include "tst_tabbar.moc"

