/*
 * Copyright 2026 The Arora Authors
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

// THEME01: the central chrome style source — spacing scale,
// palette-derived separator colors, the flat-chrome app stylesheet
// and the metrics proxy (frameless toolbars, semibold group-box
// titles), plus the tor stylesheet builders the violet window quotes.

#include "qtest_arora.h"

#include <arorastyle.h>
#include <browsermainwindow.h>
#include <browsertheme.h>
#include <containermanager.h>
#include <tabbar.h>
#include <tabwidget.h>

#include <qdir.h>
#include <qgroupbox.h>
#include <qlabel.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qsettings.h>
#include <qstatusbar.h>
#include <qstyle.h>
#include <qstylefactory.h>
#include <qstyleoption.h>
#include <qtabbar.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>
#include <qboxlayout.h>

class tst_Arorastyle : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void spacingScale();
    void paletteDerivedColors();
    void chromeSheetResolvesPalette();
    void flatChromeInstalled();
    void groupBoxTitleWeight();
    void flatGroupSection();
    void dialogAndMenuSheet();
    void flatMenuChrome();
    void polishSurvivesSchemeFlip();
    void torSheetsShareOneSource();
    void tabCardShape();
    void tabControlHover();
    void roundedTabChrome();
    void fixedFamilies();
    void contrastAudit();
    void chromeGrab();
};

// Channel-wise color comparison — rendered pixels pick up
// anti-aliased edges and platform noise.
static bool nearColor(const QColor &a, const QColor &b,
                      int tolerance = 8)
{
    return a.isValid() && b.isValid()
        && qAbs(a.red() - b.red()) <= tolerance
        && qAbs(a.green() - b.green()) <= tolerance
        && qAbs(a.blue() - b.blue()) <= tolerance;
}

void tst_Arorastyle::initTestCase()
{
    // Pin the scheme inputs so the flip leg restores deterministically
    // — a stray user pick would keep the palette dark.
    qputenv("ARORA_COLOR_SCHEME", "light");
    QSettings().remove(QLatin1String("browser/colorScheme"));
    BrowserTheme::applyColorScheme();
}

void tst_Arorastyle::cleanupTestCase()
{
    qunsetenv("ARORA_COLOR_SCHEME");
    BrowserTheme::applyColorScheme();
}

void tst_Arorastyle::spacingScale()
{
    QCOMPARE(AroraStyle::Spacing::Small, 4);
    QCOMPARE(AroraStyle::Spacing::Medium, 8);
    QCOMPARE(AroraStyle::Spacing::Large, 12);
}

void tst_Arorastyle::paletteDerivedColors()
{
    // Blend endpoints and the mix itself.
    QCOMPARE(AroraStyle::blend(Qt::black, Qt::white, 0.0),
             QColor(Qt::black));
    QCOMPARE(AroraStyle::blend(Qt::black, Qt::white, 1.0),
             QColor(Qt::white));

    // The separator must differ from the surface on both scheme ends
    // — light text on dark Window lifts it, dark text on light Window
    // sinks it — and stay subtle (nowhere near the text color).
    const QColor lightSep =
        AroraStyle::separatorColor(QApplication::palette());
    QVERIFY(lightSep.isValid());
    QVERIFY(lightSep
            != QApplication::palette().color(QPalette::Window));
    QVERIFY(qAbs(lightSep.lightness()
                 - QApplication::palette()
                       .color(QPalette::Window).lightness()) < 60);

    const QPalette dark = BrowserTheme::darkPalette();
    const QColor darkSep = AroraStyle::separatorColor(dark);
    QVERIFY(darkSep.isValid());
    QVERIFY(darkSep != dark.color(QPalette::Window));
    QVERIFY(darkSep.lightness()
            > dark.color(QPalette::Window).lightness());
    QVERIFY(lightSep != darkSep);
}

void tst_Arorastyle::chromeSheetResolvesPalette()
{
    const QString sheet = AroraStyle::chromeStyleSheet();
    QVERIFY(sheet.contains(QLatin1String("QToolBar")));
    QVERIFY(sheet.contains(QLatin1String("QStatusBar")));
    QVERIFY(sheet.contains(QLatin1String("border: none")));
    QVERIFY(sheet.contains(AroraStyle::separatorColor(
        QApplication::palette()).name()));
}

void tst_Arorastyle::flatChromeInstalled()
{
    // BrowserApplication's constructor ran install() — the app style
    // is the polish proxy and the chrome sheet is live.  The frame
    // metric carries the Small padding so the bar keeps its breathing
    // room while drawing nothing.
    QToolBar bar;
    QCOMPARE(QApplication::style()->pixelMetric(
                 QStyle::PM_ToolBarFrameWidth, nullptr, &bar),
             AroraStyle::Spacing::Small);
    QVERIFY(qApp->styleSheet().contains(
        QLatin1String("QToolBar { border: none; padding: 4px; }")));
    QVERIFY(qApp->styleSheet().contains(
        QLatin1String("QStatusBar::item { border: none; }")));
    // baseStyleName() unwraps the proxy — a creatable factory name.
    const QString baseName = AroraStyle::baseStyleName();
    QVERIFY(!baseName.isEmpty());
    QStyle *base = QStyleFactory::create(baseName);
    QVERIFY(base);
    delete base;
}

void tst_Arorastyle::groupBoxTitleWeight()
{
    // The proxy draws QGroupBox titles at DemiBold; a twin box on the
    // bare base style must render its title differently — count ink
    // in the title band (heavier glyphs cover more pixels).
    QWidget host;
    host.resize(320, 80);
    QHBoxLayout *layout = new QHBoxLayout(&host);
    QGroupBox *polished = new QGroupBox(QLatin1String("Section"), &host);
    QGroupBox *plain = new QGroupBox(QLatin1String("Section"), &host);
    QStyle *fusion = QStyleFactory::create(QLatin1String("fusion"));
    QVERIFY(fusion);
    plain->setStyle(fusion);
    layout->addWidget(polished);
    layout->addWidget(plain);
    host.show();
    QApplication::processEvents();

    const auto titleInk = [](QGroupBox *box) {
        const QImage image = box->grab().toImage();
        qlonglong ink = 0;
        const int band = qMin(box->fontMetrics().height() + 4,
                              image.height());
        for (int y = 0; y < band; ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (qAbs(c.lightness()
                         - box->palette().color(
                               QPalette::Window).lightness()) > 60)
                    ++ink;
            }
        return ink;
    };
    const qlonglong heavy = titleInk(polished);
    const qlonglong normal = titleInk(plain);
    QVERIFY(heavy > 0);
    QVERIFY(heavy != normal);

    // The weight is applied at paint time only — the widget's own
    // font (and its children's) stays untouched.
    QVERIFY(polished->font().weight() == plain->font().weight());

    host.hide();
    delete fusion;
}

// THEM03 §a: a titled group box is a flat section — a palette
// hairline under the semibold header, no framed panel edge around
// the sides and bottom.
void tst_Arorastyle::flatGroupSection()
{
    QGroupBox box(QLatin1String("Section Header"));
    box.resize(360, 140);
    box.show();
    QApplication::processEvents();
    const QImage image = box.grab().toImage();
    const QPalette palette = box.palette();
    const QColor window = palette.color(QPalette::Window);
    const QColor sep = AroraStyle::separatorColor(palette);

    // No side or bottom frame — the edge pixels stay on Window.
    QVERIFY(nearColor(image.pixelColor(0, image.height() / 2),
                      window));
    QVERIFY(nearColor(image.pixelColor(image.width() - 1,
                                       image.height() / 2), window));
    QVERIFY(nearColor(image.pixelColor(image.width() / 2,
                                       image.height() - 1), window));

    // The hairline row under the title band spans the box width.
    int lineY = -1;
    const int band = box.fontMetrics().height() + 8;
    for (int y = 0; y < band && y < image.height(); ++y) {
        if (nearColor(image.pixelColor(image.width() / 2, y), sep)) {
            lineY = y;
            break;
        }
    }
    QVERIFY(lineY > 0);
    QVERIFY(nearColor(image.pixelColor(8, lineY), sep));
    QVERIFY(nearColor(image.pixelColor(image.width() - 8, lineY),
                      sep));

    // A checkable section keeps its indicator next to the title —
    // ink in the left column of the header band.
    QGroupBox checkable(QLatin1String("Checkable"));
    checkable.setCheckable(true);
    checkable.resize(360, 140);
    checkable.show();
    QApplication::processEvents();
    const QImage checkImage = checkable.grab().toImage();
    bool indicatorInk = false;
    for (int y = 0; y < band && y < checkImage.height(); ++y) {
        for (int x = 4; x < 30; ++x) {
            const QColor c = checkImage.pixelColor(x, y);
            if (c.alpha() > 0
                    && !nearColor(c, window)
                    && !nearColor(c, sep)) {
                indicatorInk = true;
                break;
            }
        }
        if (indicatorInk)
            break;
    }
    QVERIFY(indicatorInk);
    box.hide();
    checkable.hide();
}

// THEM03: the chrome sheet carries the dialog/panel/menu polish —
// flat menus on the spacing scale, flat sidebar surfaces, and the
// frameless Preferences nav rail — all palette-resolved.
void tst_Arorastyle::dialogAndMenuSheet()
{
    const QString sheet = AroraStyle::chromeStyleSheet();
    const QPalette palette = QApplication::palette();
    const QString sep = AroraStyle::separatorColor(palette).name();
    const QString highlight =
        palette.color(QPalette::Highlight).name();
    const QString hover = AroraStyle::hoverColor(palette).name();

    QVERIFY(sheet.contains(
        QStringLiteral("QMenu { border: 1px solid %1").arg(sep)));
    QVERIFY(sheet.contains(
        QStringLiteral("QMenu::item:selected { background: %1")
            .arg(highlight)));
    // THEM04: selected-item text resolves through
    // selectionTextColor — the platform role where it reads, the
    // contrast pick where the platform pairs below AA.
    QVERIFY(sheet.contains(
        QStringLiteral("color: %1").arg(
            AroraStyle::selectionTextColor(palette).name())));
    QVERIFY(sheet.contains(QLatin1String("margin: 4px 4px")));
    QVERIFY(sheet.contains(
        QLatin1String("QComboBox QAbstractItemView")));
    QVERIFY(sheet.contains(
        QLatin1String("#sidebarTabs QAbstractItemView")));
    QVERIFY(sheet.contains(
        QLatin1String("QFrame#sidebarDownloadDetail")));
    QVERIFY(sheet.contains(
        QLatin1String("QListWidget#pagesList { border: none")));
    QVERIFY(sheet.contains(
        QStringLiteral(
            "QListWidget#pagesList::item:selected { background: %1")
            .arg(highlight)));
    QVERIFY(sheet.contains(hover));
}

// THEM03 §c: menus get a flat palette hairline edge — the left
// border column is the separator color top to bottom.
void tst_Arorastyle::flatMenuChrome()
{
    QMenu menu;
    menu.addAction(QLatin1String("Alpha action"));
    menu.addSeparator();
    menu.addAction(QLatin1String("Beta action"));
    menu.popup(QPoint(60, 60));
    QApplication::processEvents();
    QVERIFY(menu.isVisible());
    const QImage image = menu.grab().toImage();
    const QColor sep =
        AroraStyle::separatorColor(menu.palette());
    QVERIFY(nearColor(image.pixelColor(0, image.height() / 2), sep));
    QVERIFY(nearColor(
        image.pixelColor(0, image.height() - 2), sep));
    menu.hide();
}

void tst_Arorastyle::polishSurvivesSchemeFlip()
{
    // A forced dark flip can replace the app style underneath the
    // proxy; the chromeSchemeChanged hook must re-wrap and re-resolve
    // the palette-derived sheet colors.
    const QString before = qApp->styleSheet();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();

    QToolBar bar;
    QCOMPARE(QApplication::style()->pixelMetric(
                 QStyle::PM_ToolBarFrameWidth, nullptr, &bar),
             AroraStyle::Spacing::Small);
    QVERIFY(qApp->styleSheet().contains(
        AroraStyle::separatorColor(QApplication::palette()).name()));
    if (BrowserTheme::isDarkPalette(QApplication::palette()))
        QVERIFY(qApp->styleSheet() != before);

    BrowserTheme::applyColorScheme();
    QApplication::processEvents();
    QCOMPARE(QApplication::style()->pixelMetric(
                 QStyle::PM_ToolBarFrameWidth, nullptr, &bar),
             AroraStyle::Spacing::Small);
    QVERIFY(qApp->styleSheet().contains(
        QLatin1String("QToolBar { border: none; padding: 4px; }")));
}

void tst_Arorastyle::torSheetsShareOneSource()
{
    const QString chrome = AroraStyle::torChromeColor().name();
    QCOMPARE(chrome, QLatin1String("#4a3372"));
    const QStringList sheets = {
        AroraStyle::torBarStyleSheet(),
        AroraStyle::torMenuBarStyleSheet(),
        AroraStyle::torStatusBarStyleSheet(),
        AroraStyle::torTabBarStyleSheet(),
        AroraStyle::torTabWidgetStyleSheet(),
        AroraStyle::torWindowStyleSheet(),
    };
    for (const QString &sheet : sheets)
        QVERIFY2(sheet.contains(chrome), qPrintable(sheet));

    // The selected tor tab borrows the field colors from the live
    // palette so it tracks the location bar in both schemes.
    QVERIFY(AroraStyle::torTabBarStyleSheet().contains(
        QApplication::palette().color(QPalette::Base).name()));
    // Hover/pressed/text steps come from the same family.
    QVERIFY(AroraStyle::torBarStyleSheet().contains(
        AroraStyle::torChromeHoverColor().name()));
    QVERIFY(AroraStyle::torStatusBarStyleSheet().contains(
        AroraStyle::torDimTextColor().name()));
    QVERIFY(AroraStyle::torToolButtonStyleSheet().contains(
        AroraStyle::torChromePressedColor().name()));
}

// THEM02: the proxy paints chrome tabs as flat/rounded cards — a
// resting tab shows nothing but a hairline to its resting neighbor,
// hover and press step up a washed card, and the active tab lifts to
// the field color and merges into the page edge.  A vertical strip's
// rows paint the same card fully rounded instead.
void tst_Arorastyle::tabCardShape()
{
    QTabBar bar;
    bar.setShape(QTabBar::RoundedNorth);
    bar.resize(480, 28);
    bar.addTab(QLatin1String("one"));
    bar.addTab(QLatin1String("two"));
    bar.addTab(QLatin1String("three"));
    bar.setCurrentIndex(2);
    const QRect restRect = bar.tabRect(0);  // forces Qt's layout
    const QRect activeRect = bar.tabRect(2);
    QVERIFY(restRect.isValid());
    QVERIFY(activeRect.isValid());

    const QPalette pal = bar.palette();
    QStyleOptionTab opt;
    opt.initFrom(&bar);
    opt.rect = restRect;
    opt.shape = QTabBar::RoundedNorth;
    opt.fontMetrics = QFontMetrics(bar.font());
    opt.text = QLatin1String("one");

    const auto render = [&](QStyle::ControlElement element,
                            QStyle::State state, const QRect &rect) {
        QStyleOptionTab copy(opt);
        copy.state = state;
        copy.rect = rect;
        QImage image(bar.width(), 40, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        QApplication::style()->drawControl(element, &copy, &painter,
                                           &bar);
        return image;
    };

    // Resting — flat; the only ink is the hairline at the boundary
    // to the next resting tab (index 1 is not the current tab).
    const QImage resting = render(QStyle::CE_TabBarTabShape,
                                  QStyle::State_Enabled, restRect);
    QCOMPARE(resting.pixelColor(restRect.center()).alpha(), 0);
    QVERIFY(nearColor(
        resting.pixelColor(restRect.right(), restRect.top() + 8),
        AroraStyle::separatorColor(pal)));

    // Hover and press — a washed card, press the deeper step.
    const QImage hovered = render(
        QStyle::CE_TabBarTabShape,
        QStyle::State_Enabled | QStyle::State_MouseOver, restRect);
    QCOMPARE(hovered.pixelColor(restRect.topLeft()).alpha(), 0);
    QVERIFY(nearColor(hovered.pixelColor(restRect.center()),
                      AroraStyle::hoverColor(pal)));
    const QImage pressed = render(
        QStyle::CE_TabBarTabShape,
        QStyle::State_Enabled | QStyle::State_MouseOver
            | QStyle::State_Sunken,
        restRect);
    QVERIFY(nearColor(pressed.pixelColor(restRect.center()),
                      AroraStyle::pressedColor(pal)));

    // Active — lifted to the field color, rounded at the outer
    // corners, and the fill reaches past the rect's bottom edge so
    // it merges with the page instead of ending in a border.
    const QImage active = render(
        QStyle::CE_TabBarTabShape,
        QStyle::State_Enabled | QStyle::State_Selected, activeRect);
    QCOMPARE(active.pixelColor(activeRect.topLeft()).alpha(), 0);
    QCOMPARE(active.pixelColor(activeRect.topRight()).alpha(), 0);
    const QColor lifted = AroraStyle::tabActiveColor(pal);
    QVERIFY(nearColor(active.pixelColor(activeRect.center().x(),
                                        activeRect.top() + 2),
                      lifted));
    QVERIFY(nearColor(
        active.pixelColor(activeRect.center().x(),
                          activeRect.bottom() + 1),
        lifted));

    // A vertical strip's rows get the same card fully rounded — the
    // forced-North option shape on a West bar is the TABS02 row.
    TabBar rail;
    rail.setShape(QTabBar::RoundedWest);
    rail.setExpanding(false);
    rail.addTab(QLatin1String("one"));
    rail.addTab(QLatin1String("two"));
    rail.resize(200, 300);
    const QRect row = rail.tabRect(0);
    QVERIFY(row.isValid());
    QStyleOptionTab ropt = opt;
    ropt.initFrom(&rail);
    ropt.rect = row;
    ropt.shape = QTabBar::RoundedNorth;
    ropt.state = QStyle::State_Enabled | QStyle::State_Selected;
    ropt.fontMetrics = QFontMetrics(rail.font());
    QImage rowImage(220, 300, QImage::Format_ARGB32);
    rowImage.fill(Qt::transparent);
    QPainter rowPainter(&rowImage);
    QApplication::style()->drawControl(
        QStyle::CE_TabBarTabShape, &ropt, &rowPainter, &rail);
    QCOMPARE(rowImage.pixelColor(row.topLeft()).alpha(), 0);
    QCOMPARE(rowImage.pixelColor(row.bottomLeft()).alpha(), 0);
    QCOMPARE(rowImage.pixelColor(row.topRight()).alpha(), 0);
    QCOMPARE(rowImage.pixelColor(row.bottomRight()).alpha(), 0);
    QVERIFY(nearColor(rowImage.pixelColor(row.center().x(),
                                          row.top() + 2),
                      AroraStyle::tabActiveColor(rail.palette())));

    // Resting titles dim — the inactive card recedes one step
    // further than its flat fill.  The selected tab's ink keeps
    // full contrast: compare each render's darkest pixel.
    const auto darkestInk = [](const QImage &image) {
        int darkest = 255;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.alpha() > 150)
                    darkest = qMin(darkest, c.lightness());
            }
        return darkest;
    };
    opt.rect = QRect(4, 4, 160, 24);
    const QImage restingLabel = render(QStyle::CE_TabBarTabLabel,
                                       QStyle::State_Enabled,
                                       opt.rect);
    const QImage activeLabel = render(
        QStyle::CE_TabBarTabLabel,
        QStyle::State_Enabled | QStyle::State_Selected, opt.rect);
    QVERIFY(darkestInk(restingLabel) > darkestInk(activeLabel) + 10);
}

// THEM02 §c: the strip's per-tab controls share one flat rounded
// hover — an auto-raise tool button inside a tab bar paints the
// wash instead of the base style's bevelled frame.
void tst_Arorastyle::tabControlHover()
{
    QTabBar bar;
    QToolButton button(&bar);
    button.setAutoRaise(true);
    button.resize(16, 16);

    QStyleOptionButton opt;
    opt.initFrom(&button);
    opt.rect = QRect(0, 0, 16, 16);

    const auto render = [&](QStyle::State state) {
        QStyleOptionButton copy(opt);
        copy.state = state;
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        QApplication::style()->drawPrimitive(
            QStyle::PE_PanelButtonTool, &copy, &painter, &button);
        return image;
    };

    // Idle — nothing paints; hover — a rounded wash whose corner
    // falls back to ~nothing (anti-aliased feather aside).
    const QImage idle = render(QStyle::State_Enabled);
    QCOMPARE(idle.pixelColor(8, 8).alpha(), 0);
    const QImage hover = render(QStyle::State_Enabled
                                | QStyle::State_MouseOver);
    QVERIFY(hover.pixelColor(0, 0).alpha() < 32);
    QVERIFY(nearColor(hover.pixelColor(8, 8),
                      AroraStyle::hoverColor(button.palette())));
    const QImage pressed = render(
        QStyle::State_Enabled | QStyle::State_MouseOver
            | QStyle::State_Sunken);
    QVERIFY(nearColor(pressed.pixelColor(8, 8),
                      AroraStyle::pressedColor(button.palette())));

    // Outside the strip the auto-raise panel is untouched — a
    // toolbar child still renders the base style's frame (opaque).
    QToolBar toolbar;
    QToolButton toolbarButton(&toolbar);
    toolbarButton.setAutoRaise(true);
    QStyleOptionButton topt;
    topt.initFrom(&toolbarButton);
    topt.rect = QRect(0, 0, 24, 24);
    topt.state = QStyle::State_Enabled | QStyle::State_MouseOver;
    QImage outside(24, 24, QImage::Format_ARGB32);
    outside.fill(Qt::transparent);
    QPainter outsidePainter(&outside);
    QApplication::style()->drawPrimitive(
        QStyle::PE_PanelButtonTool, &topt, &outsidePainter,
        &toolbarButton);
    QVERIFY(outside.pixelColor(12, 12).alpha() > 0);
}

// THEM02: the same card on real strips, both orientations — the
// active tab reads lifted, resting tabs flat with hairlines, and
// the corners actually come off the squared look.
void tst_Arorastyle::roundedTabChrome()
{
    TabBar bar;
    bar.setShape(QTabBar::RoundedNorth);
    bar.setExpanding(false);
    bar.show();
    for (const char *title : { "first", "second", "third", "fourth" })
        bar.addTab(QLatin1String(title));
    bar.setCurrentIndex(1);
    bar.resize(560, bar.sizeHint().height());
    QApplication::processEvents();

    const QPalette pal = bar.palette();
    const QColor strip = pal.color(QPalette::Window);
    const QImage image = bar.grab().toImage();
    const QRect active = bar.tabRect(1);
    const QRect resting = bar.tabRect(2);
    QVERIFY(active.isValid());
    QVERIFY(resting.isValid());

    // The active card: outer corners stay on the strip color, the
    // top band fills with the lifted color.
    QVERIFY(nearColor(image.pixelColor(active.topLeft()), strip));
    QVERIFY(nearColor(image.pixelColor(active.topRight()), strip));
    QVERIFY(nearColor(
        image.pixelColor(active.center().x(), active.top() + 2),
        AroraStyle::tabActiveColor(pal)));
    // Resting interior is flat strip color; the boundary to the
    // next resting tab is a hairline, while the gap to the active
    // tab needs none — its card edge separates already.
    QVERIFY(nearColor(
        image.pixelColor(resting.center().x(), resting.bottom() - 2),
        strip));
    QVERIFY(nearColor(
        image.pixelColor(resting.right(), resting.center().y()),
        AroraStyle::separatorColor(pal)));
    const QRect first = bar.tabRect(0);
    // No resting hairline toward the active tab — its edge column
    // carries only the card outline's antialiased fringe, and the
    // pixel inside tab 0 stays on the strip color.
    QVERIFY(!nearColor(
        image.pixelColor(first.right(), first.center().y()),
        AroraStyle::separatorColor(pal)));
    QVERIFY(nearColor(
        image.pixelColor(first.right() - 1, first.center().y()),
        strip));

    // Vertical rail: every row card is rounded on all four corners.
    TabBar rail;
    rail.setShape(QTabBar::RoundedWest);
    rail.setExpanding(false);
    rail.show();
    for (const char *title : { "first", "second", "third", "fourth" })
        rail.addTab(QLatin1String(title));
    rail.setCurrentIndex(1);
    rail.resize(220, 400);
    QApplication::processEvents();

    const QImage railImage = rail.grab().toImage();
    const QPalette rpal = rail.palette();
    const QRect row = rail.tabRect(1);
    const QRect restRow = rail.tabRect(2);
    QVERIFY(nearColor(railImage.pixelColor(row.topLeft()),
                      rpal.color(QPalette::Window)));
    QVERIFY(nearColor(railImage.pixelColor(row.bottomRight()),
                      rpal.color(QPalette::Window)));
    QVERIFY(nearColor(
        railImage.pixelColor(row.center().x(), row.top() + 2),
        AroraStyle::tabActiveColor(rpal)));
    // The resting row separates from its resting neighbor below
    // with a hairline; the row before the active one needs none.
    QVERIFY(nearColor(
        railImage.pixelColor(restRow.center().x(), restRow.bottom()),
        AroraStyle::separatorColor(rpal)));
    const QRect rowBefore = rail.tabRect(0);
    QVERIFY(nearColor(
        railImage.pixelColor(rowBefore.center().x(),
                             rowBefore.bottom()),
        rpal.color(QPalette::Window)));

    // Offscreen grabs for visual review — ARORA_CHROME_GRAB_DIR or
    // ARORA_TABBAR_GRAB_DIR both work here.
    const QByteArray grabDir = qgetenv("ARORA_TABBAR_GRAB_DIR").isEmpty()
        ? qgetenv("ARORA_CHROME_GRAB_DIR")
        : qgetenv("ARORA_TABBAR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        const QString base = QString::fromLocal8Bit(grabDir);
        QDir().mkpath(base);
        image.save(base + QLatin1String("/rounded-tabs-top.png"));
        railImage.save(base + QLatin1String("/rounded-tabs-left.png"));
    }
}

// THEM04 §a — the fixed families sit in the one source and the
// callers quote them by name: the container accents, the
// no-container grey, the warning banner's amber, the location bar's
// cues, the mac toolbar greys, and the forced dark variant.
void tst_Arorastyle::fixedFamilies()
{
    QCOMPARE(AroraStyle::containerIdentityColors(),
             ContainerManager::defaultColors());
    QCOMPARE(AroraStyle::containerIdentityColors().count(), 8);
    QCOMPARE(AroraStyle::noContainerColor(),
             QColor(0x8a, 0x8a, 0x8a));

    // The injected warning banner keeps its amber set.
    QCOMPARE(AroraStyle::warningBannerColor().name(),
             QLatin1String("#fff4ce"));
    QCOMPARE(AroraStyle::warningBannerTextColor().name(),
             QLatin1String("#3b3105"));
    QCOMPARE(AroraStyle::warningBannerBorderColor().name(),
             QLatin1String("#e0c45f"));

    // The location bar's cues — the https tint, and the loading
    // accent: the fixed blue over a light field, the app Base's
    // deeper step where light text sits on the field.
    QCOMPARE(AroraStyle::secureFieldColor(), QColor(248, 248, 210));
    QCOMPARE(AroraStyle::loadingProgressColor(QApplication::palette()),
             QColor(116, 192, 250));
    QPalette violetField;
    violetField.setColor(QPalette::Text, AroraStyle::torTextColor());
    QCOMPARE(AroraStyle::loadingProgressColor(violetField),
             QApplication::palette().color(QPalette::Base)
                 .darker(200));

    // The mac greys patch the two palettes the unified toolbar wears.
    QPalette frame;
    AroraStyle::macToolbarFramePalette(frame);
    QVERIFY(frame.color(QPalette::Active, QPalette::Light)
            != frame.color(QPalette::Inactive, QPalette::Light));
    QPalette bar;
    AroraStyle::macToolbarSurfacePalette(bar);
    QVERIFY(bar.color(QPalette::Active, QPalette::Window)
            != bar.color(QPalette::Inactive, QPalette::Window));

    // The forced dark variant delegates — one definition, two names.
    QCOMPARE(BrowserTheme::darkPalette().color(QPalette::Window),
             AroraStyle::darkPalette().color(QPalette::Window));
}

// THEM04 §b — the contrast audit.  4.5:1 is WCAG AA for body text;
// accent links keep the 3:1 floor used for large text and UI
// affordances.
void tst_Arorastyle::contrastAudit()
{
    // The pick is correct at the extremes and lands on the readable
    // side of mid-tone accents — the old lightness() > 140 threshold
    // chose white on the turquoise container accent (~2.2:1).
    QCOMPARE(AroraStyle::contrastTextColor(Qt::white),
             QColor(Qt::black));
    QCOMPARE(AroraStyle::contrastTextColor(Qt::black),
             QColor(Qt::white));
    QCOMPARE(AroraStyle::contrastTextColor(
                 AroraStyle::containerIdentityColors().at(1)),
             QColor(Qt::black));
    QCOMPARE(AroraStyle::contrastTextColor(AroraStyle::torChromeColor()),
             QColor(Qt::white));

    // Every container accent carries its label at AA or better.
    for (const QColor &accent : AroraStyle::containerIdentityColors()) {
        const QColor text = AroraStyle::contrastTextColor(accent);
        QVERIFY2(AroraStyle::contrastRatio(text, accent) >= 4.5,
                 qPrintable(accent.name()));
    }

    // The tor chrome family — light text on the violet surface and
    // on every step the sheets layer over it.
    const QColor chrome = AroraStyle::torChromeColor();
    QVERIFY(AroraStyle::contrastRatio(AroraStyle::torTextColor(),
                                    chrome) >= 4.5);
    QVERIFY(AroraStyle::contrastRatio(AroraStyle::torDimTextColor(),
                                    chrome) >= 4.5);
    const QList<QColor> steps = { AroraStyle::torChromeHoverColor(),
                                  AroraStyle::torChromePressedColor(),
                                  AroraStyle::torChromeRaisedColor() };
    for (const QColor &step : steps) {
        QVERIFY2(AroraStyle::contrastRatio(AroraStyle::torTextColor(),
                                         step) >= 4.5,
                 qPrintable(step.name()));
    }

    // The injected warning banner — dark ink on amber.
    QVERIFY(AroraStyle::contrastRatio(AroraStyle::warningBannerTextColor(),
                                    AroraStyle::warningBannerColor())
            >= 4.5);

    // The forced dark variant — every text role on its surface.
    const QPalette dark = AroraStyle::darkPalette();
    QVERIFY(AroraStyle::contrastRatio(dark.color(QPalette::Text),
                                    dark.color(QPalette::Base)) >= 4.5);
    QVERIFY(AroraStyle::contrastRatio(dark.color(QPalette::WindowText),
                                    dark.color(QPalette::Window)) >= 4.5);
    QVERIFY(AroraStyle::contrastRatio(dark.color(QPalette::ButtonText),
                                    dark.color(QPalette::Button)) >= 4.5);
    QVERIFY(AroraStyle::contrastRatio(
                dark.color(QPalette::HighlightedText),
                dark.color(QPalette::Highlight)) >= 4.5);
    QCOMPARE(AroraStyle::selectionTextColor(dark),
             dark.color(QPalette::HighlightedText));
    QVERIFY(AroraStyle::contrastRatio(dark.color(QPalette::Link),
                                    dark.color(QPalette::Base)) >= 3.0);
    QVERIFY(AroraStyle::contrastRatio(dark.color(QPalette::PlaceholderText),
                                    dark.color(QPalette::Base)) >= 4.5);

    // The live (normal) palette gets the same audit.  The platform
    // owns its own roles — floor them at the large-text level; the
    // surfaces we control pair selection through selectionTextColor,
    // which upgrades the stock sub-AA pairing (Fusion's light
    // Highlight sits at ~3.7) to a guaranteed AA pick.
    const QPalette live = QApplication::palette();
    QVERIFY(AroraStyle::contrastRatio(live.color(QPalette::Text),
                                    live.color(QPalette::Base)) >= 4.5);
    QVERIFY(AroraStyle::contrastRatio(live.color(QPalette::WindowText),
                                    live.color(QPalette::Window)) >= 4.5);
    QVERIFY(AroraStyle::contrastRatio(
                live.color(QPalette::HighlightedText),
                live.color(QPalette::Highlight)) >= 3.0);
    QVERIFY(AroraStyle::contrastRatio(
                AroraStyle::selectionTextColor(live),
                live.color(QPalette::Highlight)) >= 4.5);
}

void tst_Arorastyle::chromeGrab()
{
    // Offscreen grab for visual review — set ARORA_CHROME_GRAB_DIR to
    // a directory and the main window lands there as mainwindow.png.
    const QByteArray grabDir = qgetenv("ARORA_CHROME_GRAB_DIR");
    if (grabDir.isEmpty())
        QSKIP("ARORA_CHROME_GRAB_DIR not set");

    BrowserMainWindow *window = new BrowserMainWindow;
    window->resize(1000, 640);
    window->show();
    QApplication::processEvents();

    QDir().mkpath(QString::fromLocal8Bit(grabDir));
    const QString base = QString::fromLocal8Bit(grabDir);
    QVERIFY(window->grab().save(base + QLatin1String("/mainwindow.png")));

    delete window;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();

    // THEM02 variants strip — a pinned tab, a named+colored group
    // run, an off-the-record tab and a container tab all ride the
    // new card chrome; grabbed once per orientation.
    TabWidget *variants = new TabWidget;
    variants->resize(860, 220);
    variants->show();
    for (int i = 0; i < 5; ++i)
        variants->newTab();
    variants->setTabPinned(0, true);
    const QString gid = variants->createTabGroup(2);
    variants->renameTabGroup(gid, QLatin1String("Work"));
    variants->setTabGroupColor(gid, QColor(0x2e, 0x7d, 0x32));
    variants->addTabToGroup(3, gid);
    variants->makeNewPrivateTab();
    const ContainerManager::Container box =
        ContainerManager::instance()->createContainer(
            QLatin1String("GrabBox"), QColor(0x8e, 0x24, 0xaa));
    variants->makeNewTabInContainer(box.id);
    variants->setCurrentIndex(1);
    QApplication::processEvents();

    QVERIFY(variants->grab().save(
        base + QLatin1String("/variants-top.png")));
    variants->setTabPosition(QTabWidget::West);
    variants->resize(300, 460);
    QApplication::processEvents();
    QVERIFY(variants->grab().save(
        base + QLatin1String("/variants-left.png")));

    delete variants;
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    ContainerManager::instance()->deleteContainer(box.id);
}

QTEST_MAIN(tst_Arorastyle)
#include "tst_arorastyle.moc"
