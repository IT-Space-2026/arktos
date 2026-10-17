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
#include <qmath.h>
#include <qpainter.h>
#include <qpainterpath.h>
#include <qproxystyle.h>
#include <qstylefactory.h>
#include <qstyleoption.h>
#include <qtabbar.h>
#include <qtoolbutton.h>

namespace {

// THEM02: tab-strip helpers shared by the proxy's drawControl —
// a vertical QTabBar::Shape, and the lifted card's outline path.
// The path is open on the content-facing edge so the active tab
// reads merged into the page instead of boxed inside a border.
bool verticalTabCardShape(QTabBar::Shape shape)
{
    return shape == QTabBar::RoundedWest
        || shape == QTabBar::TriangularWest
        || shape == QTabBar::RoundedEast
        || shape == QTabBar::TriangularEast;
}

QPainterPath tabCardOutline(const QRectF &rect, QTabBar::Shape shape,
                            qreal radius)
{
    QPainterPath path;
    const qreal l = rect.left(), t = rect.top();
    const qreal r = rect.right(), b = rect.bottom();
    switch (shape) {
    case QTabBar::RoundedSouth:
    case QTabBar::TriangularSouth:
        path.moveTo(r, t);
        path.lineTo(r, b - radius);
        path.quadTo(r, b, r - radius, b);
        path.lineTo(l + radius, b);
        path.quadTo(l, b, l, b - radius);
        path.lineTo(l, t);
        break;
    case QTabBar::RoundedWest:
    case QTabBar::TriangularWest:
        path.moveTo(r, t);
        path.lineTo(l + radius, t);
        path.quadTo(l, t, l, t + radius);
        path.lineTo(l, b - radius);
        path.quadTo(l, b, l + radius, b);
        path.lineTo(r, b);
        break;
    case QTabBar::RoundedEast:
    case QTabBar::TriangularEast:
        path.moveTo(l, t);
        path.lineTo(r - radius, t);
        path.quadTo(r, t, r, t + radius);
        path.lineTo(r, b - radius);
        path.quadTo(r, b, r - radius, b);
        path.lineTo(l, b);
        break;
    default:
        path.moveTo(l, b);
        path.lineTo(l, t + radius);
        path.quadTo(l, t, l + radius, t);
        path.lineTo(r - radius, t);
        path.quadTo(r, t, r, t + radius);
        path.lineTo(r, b);
        break;
    }
    return path;
}

} // namespace

// THEME01 §c/§d — metrics the stylesheet cannot reach: group-box
// headers drawn flat (DemiBold title + hairline, no sunken frame —
// THEM03) and children keep the base font,
// zero-width toolbar frames so bars are flat on every style, and
// QFrame separator lines redrawn as single palette hairlines instead
// of the sunken/raised bevel pair.
// THEM02 — the same hook owns the tab strip: squared tabs with hard
// borders are repainted as rounded cards (flat resting, washed hover,
// lifted active) and the per-tab controls get a matching rounded
// hover.  Only pixels inside option.rect change — every size hint
// the TABS02/RSZ02 strip metrics computed still stands.
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
        // THEM02: the flat strip carries no base band — the old
        // shaded band used to hide under the squared tabs' opaque
        // bodies and would leak through the rounded corners.  The
        // pane hairline and the active card's open edge mark the
        // seam instead.
        if (element == PE_FrameTabBarBase)
            return;
        // THEM02 §c — the strip's per-tab controls (close buttons,
        // the '+' new-tab button) get one flat rounded hover instead
        // of the base style's bevelled auto-raise frame.  The tor
        // strip's own palette makes the wash read violet there.
        if (element == PE_PanelButtonTool && painter && widget
                && qobject_cast<const QTabBar *>(widget->parentWidget())) {
            const QToolButton *button =
                qobject_cast<const QToolButton *>(widget);
            if (button && button->autoRaise()) {
                if ((option->state & State_Enabled)
                        && (option->state & State_MouseOver)) {
                    const QColor fill = (option->state & State_Sunken)
                        ? AroraStyle::pressedColor(option->palette)
                        : AroraStyle::hoverColor(option->palette);
                    painter->save();
                    painter->setRenderHint(QPainter::Antialiasing);
                    painter->setPen(Qt::NoPen);
                    painter->setBrush(fill);
                    painter->drawRoundedRect(
                        option->rect.adjusted(0, 0, -1, -1), 3, 3);
                    painter->restore();
                }
                return;
            }
        }
        QProxyStyle::drawPrimitive(element, option, painter, widget);
    }

    void drawControl(ControlElement control,
                     const QStyleOption *option, QPainter *painter,
                     const QWidget *widget) const override
    {
        const QStyleOptionTab *tab =
            qstyleoption_cast<const QStyleOptionTab *>(option);
        if (control == CE_TabBarTabShape && tab && painter) {
            drawTabCard(tab, painter, widget);
            return;
        }
        // Resting tabs recede a step further than their flat card —
        // the title drops toward the strip color while the active
        // tab's stays at full contrast.  A hovered resting tab lifts
        // its title back out of the dim along with the card wash.
        if (control == CE_TabBarTabLabel && tab
                && (tab->state & State_Enabled)
                && !(tab->state & State_Selected)
                && !(tab->state & State_MouseOver)) {
            QStyleOptionTab copy(*tab);
            const QColor dim = AroraStyle::blend(
                tab->palette.color(QPalette::WindowText),
                tab->palette.color(QPalette::Window), 0.28);
            copy.palette.setColor(QPalette::WindowText, dim);
            copy.palette.setColor(QPalette::Text, dim);
            copy.palette.setColor(QPalette::ButtonText, dim);
            QProxyStyle::drawControl(control, &copy, painter, widget);
            return;
        }
        QProxyStyle::drawControl(control, option, painter, widget);
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
            // THEM03 §a — a titled group box is a flat section: the
            // sunken rounded frame is replaced by one palette
            // hairline under the header band.  Untitled boxes fall
            // through to the base frame — there is no header for a
            // separator to sit under.
            QStyleOptionGroupBox copy(*box);
            QFont titleFont =
                widget ? widget->font() : QApplication::font();
            titleFont.setWeight(QFont::DemiBold);
            // The label rect follows the option's metrics — widen it
            // so a heavier title cannot clip — while the glyphs pick
            // the painter's font up.
            copy.fontMetrics = QFontMetrics(titleFont);
            const QRect textRect = subControlRect(
                CC_GroupBox, &copy, SC_GroupBoxLabel, widget);
            const QRect checkRect = subControlRect(
                CC_GroupBox, &copy, SC_GroupBoxCheckBox, widget);
            const QRect frameRect = (box->subControls & SC_GroupBoxFrame)
                ? subControlRect(CC_GroupBox, &copy, SC_GroupBoxFrame,
                                 widget)
                : box->rect;
            painter->save();
            painter->setFont(titleFont);
            // The hairline under the header — what the framed
            // panel's top edge used to mark.
            const int lineY =
                qMax(textRect.bottom(), checkRect.bottom()) + 2;
            painter->fillRect(frameRect.left(), lineY,
                              frameRect.width(), 1,
                              AroraStyle::separatorColor(box->palette));
            if (box->subControls & SC_GroupBoxCheckBox) {
                QStyleOptionButton indicator;
                indicator.QStyleOption::operator=(*box);
                indicator.rect = checkRect;
                drawPrimitive(PE_IndicatorCheckBox, &indicator,
                              painter, widget);
            }
            int alignment = int(box->textAlignment);
            if (!styleHint(SH_UnderlineShortcut, &copy, widget))
                alignment |= Qt::TextHideMnemonic;
            if (!(alignment & Qt::AlignVertical_Mask))
                alignment |= Qt::AlignVCenter;
            QPalette textPalette(box->palette);
            if (box->textColor.isValid())
                textPalette.setColor(QPalette::WindowText,
                                     box->textColor);
            drawItemText(painter, textRect,
                         Qt::TextShowMnemonic | alignment, textPalette,
                         box->state & State_Enabled, box->text,
                         QPalette::WindowText);
            painter->restore();
            return;
        }
        QProxyStyle::drawComplexControl(control, option, painter,
                                        widget);
    }

private:
    // THEM02 — the tab card: resting tabs stay flat (a hairline
    // separates resting neighbors), hover and press paint a washed
    // card, and the active tab lifts to the field/raised color with
    // an outline that opens into the page edge.  A vertical strip's
    // forced-horizontal rows read as rounded list cards instead of
    // merge-tabs.
    void drawTabCard(const QStyleOptionTab *tab, QPainter *painter,
                     const QWidget *widget) const
    {
        const QRect rect = tab->rect;
        const bool selected = tab->state & State_Selected;
        const bool pressed = tab->state & State_Sunken;
        const bool hovered = tab->state & State_MouseOver;
        const QTabBar *bar = qobject_cast<const QTabBar *>(widget);
        const bool railRow = bar
            && verticalTabCardShape(bar->shape())
            && !verticalTabCardShape(tab->shape);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);

        if (!selected && !hovered && !pressed) {
            // Flat resting tab — only a hairline marks the boundary
            // with a resting neighbor (a card edge needs no help).
            // A horizontal strip's separator is the right-edge line;
            // rail rows paint nothing — a line under each row read as
            // a per-tab underline, which users asked to remove.
            if (bar) {
                const int index = bar->tabAt(rect.center());
                if (index >= 0 && index + 1 < bar->count()
                        && bar->currentIndex() != index + 1
                        && !railRow
                        && !verticalTabCardShape(tab->shape)) {
                    const QColor line =
                        AroraStyle::separatorColor(tab->palette);
                    painter->fillRect(rect.right(), rect.top() + 5,
                                      1, rect.height() - 10, line);
                }
            }
            painter->restore();
            return;
        }

        // The active card reaches past its rect into the page edge;
        // hover/press and rail cards float a pixel inside it.
        QRectF card(rect);
        if (selected && !railRow) {
            switch (tab->shape) {
            case QTabBar::RoundedSouth:
            case QTabBar::TriangularSouth:
                card.adjust(0, -2, 0, 0);
                break;
            case QTabBar::RoundedWest:
            case QTabBar::TriangularWest:
                card.adjust(0, 0, 2, 0);
                break;
            case QTabBar::RoundedEast:
            case QTabBar::TriangularEast:
                card.adjust(-2, 0, 0, 0);
                break;
            default:
                card.adjust(0, 0, 0, 2);
                break;
            }
        } else {
            card = card.adjusted(1, 1, -1, -1);
        }

        const qreal radius = railRow ? 5 : 6;
        QPainterPath outline = railRow
            ? QPainterPath()
            : tabCardOutline(card, tab->shape, radius);
        if (railRow)
            outline.addRoundedRect(card, radius, radius);
        QPainterPath fill(outline);
        fill.closeSubpath();

        const QColor color = selected
            ? AroraStyle::tabActiveColor(tab->palette)
            : pressed ? AroraStyle::pressedColor(tab->palette)
                      : AroraStyle::hoverColor(tab->palette);
        painter->setPen(Qt::NoPen);
        painter->setBrush(color);
        painter->drawPath(fill);
        if (selected) {
            painter->setPen(AroraStyle::separatorColor(tab->palette));
            painter->setBrush(Qt::NoBrush);
            painter->drawPath(outline);
        }
        painter->restore();
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

QColor AroraStyle::pressedColor(const QPalette &palette)
{
    return blend(palette.color(QPalette::Window),
                 palette.color(QPalette::WindowText), 0.13);
}

QColor AroraStyle::tabActiveColor(const QPalette &palette)
{
    const QColor window = palette.color(QPalette::Window);
    const QColor base = palette.color(QPalette::Base);
    if (base.value() >= window.value())
        return base;
    return blend(window, palette.color(QPalette::WindowText), 0.14);
}

// WCAG 2.x relative luminance — the sRGB channels linearized, then
// weighted for how the eye reads them.
static qreal relativeLuminance(const QColor &color)
{
    const auto channel = [](qreal c) {
        return c <= 0.04045 ? c / 12.92
                            : qPow((c + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(color.redF())
        + 0.7152 * channel(color.greenF())
        + 0.0722 * channel(color.blueF());
}

qreal AroraStyle::contrastRatio(const QColor &a, const QColor &b)
{
    qreal high = relativeLuminance(a);
    qreal low = relativeLuminance(b);
    if (high < low)
        qSwap(high, low);
    return (high + 0.05) / (low + 0.05);
}

QColor AroraStyle::contrastTextColor(const QColor &on)
{
    return contrastRatio(QColor(Qt::black), on)
            >= contrastRatio(QColor(Qt::white), on)
        ? QColor(Qt::black) : QColor(Qt::white);
}

QColor AroraStyle::selectionTextColor(const QPalette &palette)
{
    const QColor highlight = palette.color(QPalette::Highlight);
    const QColor onHighlight =
        palette.color(QPalette::HighlightedText);
    if (contrastRatio(onHighlight, highlight) >= 4.5)
        return onHighlight;
    return contrastTextColor(highlight);
}

QColor AroraStyle::accentFillColor(const QPalette &palette)
{
    const QColor accent = palette.color(QPalette::Accent);
    return accent.isValid() ? accent
                            : palette.color(QPalette::Highlight);
}

QColor AroraStyle::onAccentColor(const QPalette &palette)
{
    const QColor fill = accentFillColor(palette);
    const QColor on = palette.color(QPalette::HighlightedText);
    if (contrastRatio(on, fill) >= 4.5)
        return on;
    return contrastTextColor(fill);
}

// THEME01's dark variant moved here (THEM04) — the palette
// BrowserTheme forces when the platform has no dark scheme.  It is
// the classic Fusion dark set; every color is a variant definition,
// not a stray literal.
QPalette AroraStyle::darkPalette()
{
    QPalette palette;
    palette.setColor(QPalette::Window, QColor(53, 53, 53));
    palette.setColor(QPalette::WindowText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::WindowText,
                     QColor(127, 127, 127));
    palette.setColor(QPalette::Base, QColor(35, 35, 35));
    palette.setColor(QPalette::AlternateBase, QColor(53, 53, 53));
    palette.setColor(QPalette::ToolTipBase, QColor(25, 25, 25));
    palette.setColor(QPalette::ToolTipText, Qt::white);
    palette.setColor(QPalette::Text, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::Text,
                     QColor(127, 127, 127));
    palette.setColor(QPalette::Dark, QColor(35, 35, 35));
    palette.setColor(QPalette::Light, QColor(75, 75, 75));
    palette.setColor(QPalette::Midlight, QColor(64, 64, 64));
    palette.setColor(QPalette::Mid, QColor(45, 45, 45));
    palette.setColor(QPalette::Shadow, QColor(20, 20, 20));
    palette.setColor(QPalette::Button, QColor(53, 53, 53));
    palette.setColor(QPalette::ButtonText, Qt::white);
    palette.setColor(QPalette::Disabled, QPalette::ButtonText,
                     QColor(127, 127, 127));
    palette.setColor(QPalette::BrightText, Qt::red);
    palette.setColor(QPalette::Link, QColor(42, 130, 218));
    palette.setColor(QPalette::LinkVisited, QColor(127, 84, 183));
    palette.setColor(QPalette::Highlight, QColor(42, 130, 218));
    palette.setColor(QPalette::Disabled, QPalette::Highlight,
                     QColor(80, 80, 80));
    palette.setColor(QPalette::HighlightedText, Qt::black);
    palette.setColor(QPalette::Disabled, QPalette::HighlightedText,
                     QColor(127, 127, 127));
    palette.setColor(QPalette::PlaceholderText, QColor(160, 160, 160));
    return palette;
}

QList<QColor> AroraStyle::containerIdentityColors()
{
    // Firefox's container accent palette — blue, turquoise, green,
    // yellow, orange, red, pink, purple.
    return { QColor(0x37, 0xad, 0xff), QColor(0x00, 0xc7, 0x9a),
             QColor(0x51, 0xcd, 0x00), QColor(0xff, 0xcb, 0x00),
             QColor(0xff, 0x9f, 0x00), QColor(0xff, 0x61, 0x3d),
             QColor(0xff, 0x4b, 0xda), QColor(0xaf, 0x51, 0xf5) };
}

QColor AroraStyle::noContainerColor()
{
    return QColor(0x8a, 0x8a, 0x8a);
}

QColor AroraStyle::loadingProgressColor(const QPalette &fieldPalette)
{
    // Light text on the field means a dark or violet scheme — step
    // the app palette's Base down for the cue instead.  The app
    // palette, not the widget's own: the progress gradient may
    // already have overwritten the widget's Base role.
    if (fieldPalette.color(QPalette::Text).value() >= 128) {
        const QPalette appPalette =
            qApp ? qApp->palette() : QPalette();
        return appPalette.color(QPalette::Base).darker(200);
    }
    return QColor(116, 192, 250);
}

QColor AroraStyle::secureFieldColor()
{
    return QColor(248, 248, 210);
}

QColor AroraStyle::networkTypeSwatch(const QString &type,
                                     const QPalette &palette)
{
    // Chrome DevTools' per-type waterfall hues, approximately —
    // fixed identity colors like the container accents.  Keyed on
    // the CDP resource type (lowercased by the caller or here).
    const QString t = type.toLower();
    QColor swatch;
    if (t == QLatin1String("document"))
        swatch = QColor(0x1a, 0x73, 0xe8);          // blue
    else if (t == QLatin1String("stylesheet"))
        swatch = QColor(0x18, 0x80, 0x38);          // green
    else if (t == QLatin1String("script"))
        swatch = QColor(0xf9, 0xab, 0x00);          // amber
    else if (t == QLatin1String("font"))
        swatch = QColor(0xd0, 0x18, 0x84);          // magenta
    else if (t == QLatin1String("image"))
        swatch = QColor(0x93, 0x34, 0xe6);          // purple
    else if (t == QLatin1String("media"))
        swatch = QColor(0x00, 0x83, 0x8f);          // teal
    else if (t == QLatin1String("manifest"))
        swatch = QColor(0x5f, 0x63, 0x68);          // gray
    else if (t == QLatin1String("websocket"))
        swatch = QColor(0x8e, 0x24, 0xaa);          // deep violet
    else if (t == QLatin1String("wasm"))
        swatch = QColor(0x54, 0x6e, 0x7a);          // blue-gray
    else if (t == QLatin1String("xhr")
             || t == QLatin1String("fetch"))
        swatch = QColor(0x03, 0x9b, 0xe5);          // light blue
    else
        swatch = QColor(0x9a, 0xa0, 0xa6);          // neutral gray
    // On a dark field the same hues lift toward white so the bars do
    // not sink into the background — Chrome keeps the saturated set
    // in dark mode too, just brighter.
    if (palette.color(QPalette::Base).lightness() < 128)
        swatch = blend(swatch, Qt::white, 0.18);
    return swatch;
}

QColor AroraStyle::recordDotColor()
{
    return QColor(0xd9, 0x30, 0x25);
}

QColor AroraStyle::warningBannerColor()
{
    return QColor(0xff, 0xf4, 0xce);
}

QColor AroraStyle::warningBannerTextColor()
{
    return QColor(0x3b, 0x31, 0x05);
}

QColor AroraStyle::warningBannerBorderColor()
{
    return QColor(0xe0, 0xc4, 0x5f);
}

void AroraStyle::macToolbarSurfacePalette(QPalette &palette)
{
    palette.setColor(QPalette::Active, QPalette::Window,
                     QColor(150, 150, 150));
    palette.setColor(QPalette::Inactive, QPalette::Window,
                     QColor(207, 207, 207));
}

void AroraStyle::macToolbarFramePalette(QPalette &palette)
{
    palette.setColor(QPalette::Active, QPalette::Light,
                     QColor(64, 64, 64));
    palette.setColor(QPalette::Active, QPalette::Dark,
                     QColor(192, 192, 192));
    palette.setColor(QPalette::Inactive, QPalette::Light,
                     QColor(135, 135, 135));
    palette.setColor(QPalette::Inactive, QPalette::Dark,
                     QColor(226, 226, 226));
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

QColor AroraStyle::torChromeAccentColor()
{
    // THEM07: the raised violet (~1.6:1 over the surface) reads as a
    // muddy raised box when a glyph-scale mark paints with it — a
    // lighter in-family step keeps ~3.7:1 against the surface.
    return QColor(0xa9, 0x8b, 0xd9);
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
    const QString highlight = palette.color(QPalette::Highlight).name();
    const QString hover = hoverColor(palette).name();
    const QString onHighlight = selectionTextColor(palette).name();
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
        // The dock title reads as a header row on the spacing scale.
        "QDockWidget::title { padding: %2px %4px; }"
        // The tab pane's seam is the same hairline — except on the
        // browser's own tab widget: a border at the pane's top edge
        // lands as a line under every resting tab (the lifted card
        // gaps it), which users read as per-tab underlines.  The tor
        // window already ships the pane borderless.
        "QTabWidget::pane { border: 1px solid %1; }"
        "QTabWidget#mainTabWidget::pane { border: none; }"
        // THEM03 §c — menus and dropdown popovers share one flat
        // treatment app-wide (the tor window's menus included — the
        // rules resolve from the palette, not the chrome color):
        // hairline edge, Small pad around the list, a softly rounded
        // Highlight selection, and the same hairline separator.
        "QMenu { border: 1px solid %1; padding: %2px; }"
        "QMenu::item { padding: %2px 20px; border-radius: 3px; }"
        "QMenu::item:selected { background: %3; color: %6; }"
        "QMenu::separator { height: 1px; background: %1;"
        " margin: %2px %2px; }"
        "QComboBox QAbstractItemView { border: 1px solid %1;"
        " padding: %2px; }"
        // THEM03 §b — sidebar panel surfaces keep a flat hairline
        // instead of the styled field frame.
        "#sidebarTabs QAbstractItemView { border: 1px solid %1; }"
        "#sidebarTabs QPlainTextEdit { border: 1px solid %1; }"
        "QFrame#sidebarDownloadDetail { border: 1px solid %1; }"
        // THEM03 §a — the Preferences nav list is a frameless rail:
        // rows ride a rounded Highlight card on selection and the
        // hover wash in between, matching the tab-strip language.
        "QListWidget#pagesList { border: none;"
        " background: transparent; }"
        "QListWidget#pagesList::item { border-radius: 4px;"
        " padding: 2px %2px; }"
        "QListWidget#pagesList::item:hover:!selected {"
        " background: %5; }"
        "QListWidget#pagesList::item:selected { background: %3;"
        " color: %6; }")
        .arg(separator)
        .arg(Spacing::Small)
        .arg(highlight)
        .arg(Spacing::Medium)
        .arg(hover)
        .arg(onHighlight);
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
