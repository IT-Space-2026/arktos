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

#include "arorastyle.h"

#include "browsertheme.h"

#include <qapplication.h>
#include <qfontmetrics.h>
#include <qframe.h>
#include <qpainter.h>
#include <qproxystyle.h>
#include <qstylefactory.h>
#include <qstyleoption.h>

// THEME01 §c/§d — metrics the stylesheet cannot reach: group-box
// titles drawn at section-header weight (children keep the base font),
// zero-width toolbar frames so bars are flat on every style, and
// QFrame separator lines redrawn as single palette hairlines instead
// of the sunken/raised bevel pair.
class ChromeProxyStyle : public QProxyStyle
{
public:
    explicit ChromeProxyStyle(QStyle *baseStyle)
        : QProxyStyle(baseStyle)
    {
    }

    int pixelMetric(PixelMetric metric, const QStyleOption *option,
                    const QWidget *widget) const override
    {
        if (metric == PM_ToolBarFrameWidth)
            return 0;
        return QProxyStyle::pixelMetric(metric, option, widget);
    }

    void drawPrimitive(PrimitiveElement element,
                       const QStyleOption *option, QPainter *painter,
                       const QWidget *widget) const override
    {
        const QStyleOptionFrame *frame =
            qstyleoption_cast<const QStyleOptionFrame *>(option);
        if (element == PE_Frame && frame && painter
                && (frame->frameShape == QFrame::HLine
                    || frame->frameShape == QFrame::VLine)) {
            // One flat hairline from the widget's own palette — the
            // tor window's variant included — instead of the bevel.
            const QColor line =
                AroraStyle::separatorColor(frame->palette);
            const QRect rect = frame->rect;
            if (frame->frameShape == QFrame::HLine)
                painter->fillRect(rect.left(), rect.center().y(),
                                  rect.width(), 1, line);
            else
                painter->fillRect(rect.center().x(), rect.top(),
                                  1, rect.height(), line);
            return;
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawComplexControl(ComplexControl control,
                            const QStyleOptionComplex *option,
                            QPainter *painter,
                            const QWidget *widget) const override
    {
        const QStyleOptionGroupBox *box =
            qstyleoption_cast<const QStyleOptionGroupBox *>(option);
        if (control == CC_GroupBox && box && !box->text.isEmpty()
                && painter) {
            QStyleOptionGroupBox copy(*box);
            QFont titleFont =
                widget ? widget->font() : QApplication::font();
            titleFont.setWeight(QFont::DemiBold);
            // The label rect follows the option's metrics — widen it
            // so a heavier title cannot clip — while the glyphs pick
            // the painter's font up.
            copy.fontMetrics = QFontMetrics(titleFont);
            painter->save();
            painter->setFont(titleFont);
            QProxyStyle::drawComplexControl(control, &copy, painter,
                                            widget);
            painter->restore();
            return;
        }
        QProxyStyle::drawComplexControl(control, option, painter,
                                        widget);
    }
};

QColor AroraStyle::blend(const QColor &base, const QColor &over,
                         qreal amount)
{
    const qreal keep = 1.0 - amount;
    return QColor::fromRgbF(
        base.redF() * keep + over.redF() * amount,
        base.greenF() * keep + over.greenF() * amount,
        base.blueF() * keep + over.blueF() * amount,
        base.alphaF() * keep + over.alphaF() * amount);
}

QColor AroraStyle::separatorColor(const QPalette &palette)
{
    return blend(palette.color(QPalette::Window),
                 palette.color(QPalette::WindowText), 0.14);
}

QColor AroraStyle::hoverColor(const QPalette &palette)
{
    return blend(palette.color(QPalette::Window),
                 palette.color(QPalette::WindowText), 0.08);
}

QColor AroraStyle::torChromeColor()
{
    return QColor(0x4a, 0x33, 0x72);
}

QColor AroraStyle::torChromeHoverColor()
{
    return QColor(0x5d, 0x44, 0x91);
}

QColor AroraStyle::torChromePressedColor()
{
    return QColor(0x3a, 0x2a, 0x5c);
}

QColor AroraStyle::torChromeRaisedColor()
{
    return QColor(0x6b, 0x4f, 0xa3);
}

QColor AroraStyle::torTextColor()
{
    return QColor(0xee, 0xee, 0xee);
}

QColor AroraStyle::torDimTextColor()
{
    return QColor(0xb9, 0xae, 0xcf);
}

QString AroraStyle::chromeStyleSheet()
{
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    const QString separator = separatorColor(palette).name();
    return QString::fromLatin1(
        // Flat bars: no sunken frames anywhere.  The stylesheet
        // style reports PM_ToolBarFrameWidth as border+padding and
        // QToolBarLayout turns that into the bar's content margin,
        // so the Small padding keeps the breathing room the frame
        // used to provide.
        "QToolBar { border: none; padding: %2px; }"
        // The status bar separates from the page with a palette
        // hairline instead of the style's shaded item boxes.
        "QStatusBar { border-top: 1px solid %1; }"
        "QStatusBar::item { border: none; }"
        // Dock chrome keeps a boundary — a flat palette line instead
        // of a styled frame (floating docks keep it too).
        "QDockWidget { border: 1px solid %1; }"
        // The tab pane's seam is the same hairline; the tor window
        // overrides the pane to borderless violet itself.
        "QTabWidget::pane { border: 1px solid %1; }"
        // Menu separators collapse to the hairline convention.
        "QMenu::separator { height: 1px; background: %1;"
        " margin: %2px %2px; }")
        .arg(separator)
        .arg(Spacing::Small);
}

QString AroraStyle::torBarStyleSheet()
{
    // TOR02/TOR07: one bar treatment shared by the navigation and
    // bookmarks toolbars so the header band reads as a single surface.
    return QString::fromLatin1(
        "QToolBar { background: %1; border: none; }"
        "QToolButton { color: %2; background: transparent; }"
        "QToolButton:hover { background: %3; }"
        "QToolButton:pressed { background: %4; }")
        .arg(torChromeColor().name(), torTextColor().name(),
             torChromeHoverColor().name(), torChromePressedColor().name());
}

QString AroraStyle::torMenuBarStyleSheet()
{
    return QString::fromLatin1(
        "QMenuBar { background: %1; border: none; }"
        "QMenuBar::item { color: %2; background: transparent; }"
        "QMenuBar::item:selected { background: %3; }"
        "QMenuBar::item:pressed { background: %4; }")
        .arg(torChromeColor().name(), torTextColor().name(),
             torChromeHoverColor().name(), torChromePressedColor().name());
}

QString AroraStyle::torBadgeStyleSheet()
{
    return QString::fromLatin1(
        "QLabel { color: white; background: %1;"
        " border-radius: 6px; padding: 1px 8px; font-weight: bold; }")
        .arg(torChromeRaisedColor().name());
}

QString AroraStyle::torTabWidgetStyleSheet()
{
    return QString::fromLatin1(
        "QTabWidget#mainTabWidget { background: %1; }"
        "QTabWidget#mainTabWidget::pane { border: none; }")
        .arg(torChromeColor().name());
}

QString AroraStyle::torTabBarStyleSheet()
{
    // TOR07: inactive tabs read as chrome (mid violet), the selected
    // tab is the light "field" element — the same inside-the-violet
    // treatment the location bar carries, resolved from the live
    // palette so it tracks the field in day and night schemes alike.
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    return QString::fromLatin1(
        "QTabBar { background: %1; }"
        // 6px vertical padding pushes the stylesheet's natural tab
        // hint past TabBar::tabSizeHint's TABS07 cap, so the bound
        // clamps to exactly the same thickness a normal strip gets.
        "QTabBar::tab { background: %2; color: %3;"
        " border: 1px solid %4; padding: 6px 8px; }"
        "QTabBar::tab:top { border-bottom: none;"
        " border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:bottom { border-top: none;"
        " border-bottom-left-radius: 4px; border-bottom-right-radius: 4px; }"
        "QTabBar::tab:left { border-right: none;"
        " border-top-left-radius: 4px; border-bottom-left-radius: 4px; }"
        "QTabBar::tab:right { border-left: none;"
        " border-top-right-radius: 4px; border-bottom-right-radius: 4px; }"
        "QTabBar::tab:selected { background: %5; color: %6; }"
        "QTabBar::tab:!selected:hover { background: %7; }")
        .arg(torChromeColor().name(), torChromeHoverColor().name(),
             torTextColor().name(), torChromePressedColor().name(),
             palette.color(QPalette::Base).name(),
             palette.color(QPalette::Text).name(),
             torChromeRaisedColor().name());
}

QString AroraStyle::torStatusBarStyleSheet()
{
    // TOR14: light-on-violet for the circuit label, the MEM/NET/
    // loading/zoom widgets and the transient message line.  The top
    // edge keeps a hairline from the violet family rather than the
    // app sheet's palette separator.
    return QString::fromLatin1(
        "QStatusBar { background: %1; color: %2;"
        " border-top: 1px solid %3; }"
        "QStatusBar::item { border: none; }"
        "QLabel { color: %2; background: transparent; }"
        "QToolButton { color: %2; background: transparent; }"
        "QToolButton:hover { background: %4; }"
        "QToolButton:pressed { background: %3; }"
        "QProgressBar { background: %3; border: none;"
        " border-radius: 3px; }"
        "QProgressBar::chunk { background: %5; }")
        .arg(torChromeColor().name(), torTextColor().name(),
             torChromePressedColor().name(), torChromeHoverColor().name(),
             torDimTextColor().name());
}

QString AroraStyle::torToolButtonStyleSheet()
{
    return QString::fromLatin1(
        "QToolButton { background: transparent; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:pressed { background: %2; }")
        .arg(torChromeHoverColor().name(), torChromePressedColor().name());
}

QString AroraStyle::torBannerStyleSheet()
{
    return QString::fromLatin1(
        "TorStatusBanner { background: %1; color: %2; }"
        "QLabel { color: %2; background: transparent; }"
        "QPushButton { color: %2; background: %3;"
        " border: 1px solid %4; border-radius: 3px;"
        " padding: 3px 12px; }"
        "QPushButton:hover { background: %5; }"
        "QPushButton:pressed { background: %6; }")
        .arg(torChromePressedColor().name(), torTextColor().name(),
             torChromeHoverColor().name(), torDimTextColor().name(),
             torChromeRaisedColor().name(), torChromeColor().name());
}

QString AroraStyle::torTitleBarStyleSheet()
{
    // The title is the headline element on the violet chrome — pure
    // white, the maximum contrast against the surface (~12:1); the
    // badge and glyph family stay at the chrome's light text color.
    return QString::fromLatin1(
        "TorTitleBar { background: transparent; }"
        "QLabel { color: %1; background: transparent; }"
        "QLabel#torTitleText { color: white; }"
        "QLabel#torTitleBadge { color: white; background: %2;"
        " border-radius: 5px; padding: 0px 6px; font-weight: bold; }")
        .arg(torTextColor().name(), torChromeRaisedColor().name());
}

QString AroraStyle::torTitleControlsStyleSheet()
{
    return QString::fromLatin1(
        "QWidget#torTitleControls { background: transparent; }"
        "QToolButton { background: transparent; border-radius: 4px; }"
        "QToolButton:hover { background: %1; }"
        "QToolButton:pressed { background: %2; }")
        .arg(torChromeHoverColor().name(), torChromePressedColor().name());
}

QString AroraStyle::torWindowStyleSheet()
{
    return QString::fromLatin1("#torMainWindow { background: %1; }")
        .arg(torChromeColor().name());
}

// The base style's real factory name, captured when the proxy wrapped
// it.  Once the chrome stylesheet is up, QApplication::style() is a
// QStyleSheetStyle whose name() is empty — the true base cannot be
// recovered from the style stack, so it is remembered here.
static QString s_wrappedBaseName;

QString AroraStyle::baseStyleName()
{
    if (!s_wrappedBaseName.isEmpty())
        return s_wrappedBaseName;
    QStyle *style = QApplication::style();
    if (ChromeProxyStyle *proxy = dynamic_cast<ChromeProxyStyle *>(style))
        return proxy->baseStyle() ? proxy->baseStyle()->name()
                                  : QString();
    return style ? style->name() : QString();
}

static void ensureStyleWrapped()
{
    QStyle *current = QApplication::style();
    if (!current || dynamic_cast<ChromeProxyStyle *>(current))
        return;
    // Never hand the proxy the live app style as its base — a later
    // QApplication::setStyle() (the BrowserTheme forced-palette flip)
    // would delete it out from under us.  Wrap a fresh instance of
    // the same style instead.
    QStyle *base = nullptr;
    const QString name = current->name();
    if (!name.isEmpty())
        base = QStyleFactory::create(name);
    QString baseName = name;
    if (!base) {
        // An unnamed style (e.g. the stylesheet wrapper a scheme
        // flip left on top): fall back to fusion, which palettes
        // always recolor.
        base = QStyleFactory::create(QLatin1String("fusion"));
        baseName = QStringLiteral("fusion");
    }
    if (!base)
        return;
    s_wrappedBaseName = baseName;
    QApplication::setStyle(new ChromeProxyStyle(base));
}

static void refreshChromeSheet()
{
    if (qApp)
        qApp->setStyleSheet(AroraStyle::chromeStyleSheet());
}

void AroraStyle::install()
{
    ensureStyleWrapped();
    refreshChromeSheet();

    static bool hooked = false;
    if (hooked)
        return;
    hooked = true;
    // A light/dark flip re-creates the underlying style (the proxy is
    // replaced) and moves the palette — re-wrap and re-resolve the
    // palette-derived colors so nothing goes stale.
    QObject::connect(BrowserTheme::themeNotifier(),
                     &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
                     qApp, []() {
        ensureStyleWrapped();
        refreshChromeSheet();
    });
}
