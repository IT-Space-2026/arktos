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
//     stylesheets cannot reach (zero-width toolbar frames, semibold
//     group-box titles that do not leak into a group's children);
//   * an application stylesheet built from the LIVE palette — flat
//     toolbar/dock/status-bar chrome with hairline separators.  It is
//     rebuilt on every chromeSchemeChanged so a light/dark flip (or a
//     Preferences theme preview) re-derives the colors instead of
//     going stale.
//
// Nothing here hardcodes a color outside the tor family below — the
// violet set is the fixed brand accent (and the single legitimate
// exception every tor rule quotes by name).
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

// --- tor chrome ---------------------------------------------------
// The fixed violet family.  These are the only hardcoded colors in
// chrome code, and they live only here.

QColor torChromeColor();        // the violet surface
QColor torChromeHoverColor();   // hover step on the surface
QColor torChromePressedColor(); // pressed / deepest step
QColor torChromeRaisedColor();  // raised elements (badges)
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
