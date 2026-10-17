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

#ifndef ARORASTYLE_H
#define ARORASTYLE_H

#include <qcolor.h>
#include <qlist.h>
#include <qpalette.h>
#include <qstring.h>

// THEME01: the one central style source for the browser chrome.
// Everything that decides what the chrome looks like resolves here —
// the spacing scale, the palette-derived separator/hover colors, the
// fixed tor violet family, and every chrome stylesheet.  Chrome code
// routes through these helpers instead of inventing per-widget
// literals and scattered setStyleSheet() strings.
//
// Two runtime surfaces come out of install():
//   * a QProxyStyle layered on the app style — metrics polish that
//     stylesheets cannot reach (zero-width toolbar frames, flat
//     group-box section headers — semibold title + hairline — that
//     do not leak a heavier font into a group's children);
//   * an application stylesheet built from the LIVE palette — flat
//     toolbar/dock/status-bar chrome with hairline separators.  It is
//     rebuilt on every chromeSchemeChanged so a light/dark flip (or a
//     Preferences theme preview) re-derives the colors instead of
//     going stale.
//
// Every hardcoded color in chrome code lives in this file — the tor
// violet family, the container identity accents, the fixed cue and
// notice colors, the macOS toolbar greys, and the forced dark
// palette.  Everything else resolves from the live QPalette.
namespace AroraStyle {

// The chrome spacing scale.  Layout margins and spacings on toolbars,
// panels and popovers snap to these steps — no ad-hoc literals.
namespace Spacing {
constexpr int Small = 4;   // row gaps, compact padding
constexpr int Medium = 8;  // default layout spacing/margins
constexpr int Large = 12;  // section spacing, panel margins
}

// alpha-weighted mix: amount == 0 -> base, amount == 1 -> over.
QColor blend(const QColor &base, const QColor &over, qreal amount);

// A hairline between chrome surfaces — Window nudged toward the text
// color, so it reads on light, dark and tinted (tor) palettes alike.
QColor separatorColor(const QPalette &palette);

// A gentle wash for hovered flat chrome (tool buttons, dock titles) —
// Highlight at low alpha over the surface color.
QColor hoverColor(const QPalette &palette);

// The deeper step on the hover wash — pressed chrome and held tabs.
QColor pressedColor(const QPalette &palette);

// The lifted active tab card — the field color where that is the
// lighter surface (every light scheme), a raised wash where Base
// sinks below the chrome (dark palettes), so the current tab reads
// raised rather than recessed in both.
QColor tabActiveColor(const QPalette &palette);

// WCAG 2.x contrast ratio (1.0 – 21.0) between two colors — the
// audit yardstick, and the pick rule behind contrastTextColor.
qreal contrastRatio(const QColor &a, const QColor &b);

// Black or white — whichever scores the higher contrastRatio()
// against `on`.  For text and icons painted on accent fills the
// palette cannot name (container chips, group bands, badges).
QColor contrastTextColor(const QColor &on);

// Text on a palette Highlight fill.  The platform's own
// HighlightedText wins where it already meets the 4.5:1 AA floor;
// below it (Fusion's stock light palette pairs white on #308cc6 at
// ~3.7) the black/white contrast pick upgrades the surface.  Chrome
// sheets quote this instead of the raw role.
QColor selectionTextColor(const QPalette &palette);

// The accent fill badges and toggles paint — the palette's Accent,
// falling back to Highlight where the palette never set one — and
// the text/glyph color that reads on it: the platform's
// HighlightedText where it meets AA, the black/white pick below it.
QColor accentFillColor(const QPalette &palette);
QColor onAccentColor(const QPalette &palette);

// --- palette variants -----------------------------------------------
// One home for the chrome variants: the platform palette is the
// "normal" one, darkPalette() is the variant BrowserTheme forces
// when the platform has no dark scheme of its own, and the tor
// family below is the tor variant.

QPalette darkPalette();

// --- fixed color families -------------------------------------------
// Fixed by design, like the tor set: identity and cue colors that
// must read identically on every palette.

// The container identity accents — Firefox's eight: blue, turquoise,
// green, yellow, orange, red, pink, purple.  The neutral grey the
// "No Container" entry wears sits next to them.
QList<QColor> containerIdentityColors();
QColor noContainerColor();

// The location bar's loading accent — a fixed blue over a light
// field; where light text sits on the field (dark palettes, the tor
// bar) the app Base color's deeper step keeps the cue visible.
QColor loadingProgressColor(const QPalette &fieldPalette);

// The https field tint — the classic light-yellow secure cue.  The
// caller applies it only where dark text sits on the field.
QColor secureFieldColor();

// DEVT11: the BiDi network waterfall colors each bar by resource
// type — approximately Chrome's swatch set (document blue,
// stylesheet green, script amber, image purple, xhr/fetch light
// blue, ...).  `type` is the CDP resource type
// ("Document"/"Stylesheet"/"XHR"/...); unmapped types share the
// neutral swatch.  Dark palettes get the same hues lifted a step so
// the bars keep their contrast on the dark field.
QColor networkTypeSwatch(const QString &type,
                         const QPalette &palette);

// The record-dot red the BiDi network toolbar's capture toggle
// paints — the classic "recording" cue, fixed like the secure-field
// cue above.
QColor recordDotColor();

// The rate-limit notice WebPage injects into arbitrary page DOM —
// fixed amber because all:initial cuts it off from the page's own
// colors and the widget palette alike.
QColor warningBannerColor();
QColor warningBannerTextColor();
QColor warningBannerBorderColor();

// The macOS unified-toolbar greys (referenced only under Q_OS_MACOS)
// — the fixed Qt4-era Mac look: the bookmarks bar's grey surface and
// the bevel pair its separator frame derives from.
void macToolbarSurfacePalette(QPalette &palette);
void macToolbarFramePalette(QPalette &palette);

// --- tor chrome ---------------------------------------------------
// The fixed violet family — the tor variant's brand accent, quoted
// by name from every tor rule.

QColor torChromeColor();        // the violet surface
QColor torChromeHoverColor();   // hover step on the surface
QColor torChromePressedColor(); // pressed / deepest step
QColor torChromeRaisedColor();  // raised elements (badges)
// A lighter in-family violet for glyph-scale marks (thin outlines,
// icon dots) — the raised step is only ~1.6:1 against the surface,
// fine at fill scale but muddy as a pen stroke; this keeps >=3:1.
QColor torChromeAccentColor();
QColor torTextColor();          // light text and painted glyphs
QColor torDimTextColor();       // receded text / disabled glyphs

// --- chrome stylesheets -------------------------------------------
// Builders so a live theme flip can re-resolve palette-borrowed
// colors; call sites re-apply their sheet on chromeSchemeChanged
// where they quote the palette.

QString chromeStyleSheet();           // app-wide flat chrome
QString torBarStyleSheet();           // navigation + bookmarks bars
QString torMenuBarStyleSheet();
QString torBadgeStyleSheet();         // the "Tor" badge pill
QString torTabWidgetStyleSheet();     // the tab pane's violet band
QString torTabBarStyleSheet();        // resolves Base/Text live
QString torStatusBarStyleSheet();
QString torToolButtonStyleSheet();    // flat buttons on violet chrome
QString torBannerStyleSheet();        // TorStatusBanner
QString torTitleBarStyleSheet();      // drawn title bar (frameless)
QString torTitleControlsStyleSheet(); // min/max/close buttons
QString torWindowStyleSheet();        // the violet frame ring

// The real name of the style under the polish proxy — what
// QApplication::style()->name() reported before install() wrapped it,
// or the proxy's baseStyle().  BrowserTheme's saved-style capture
// consults this so a forced-palette flip restores the platform style
// rather than capturing the proxy's own name.
QString baseStyleName();

// Installs the chrome polish — the metrics proxy plus the
// palette-derived chrome stylesheet — and re-resolves on every
// chromeSchemeChanged.  Idempotent; called once by BrowserApplication.
void install();

}

#endif // ARORASTYLE_H
