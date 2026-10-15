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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */
#ifndef ARORAICON_H
#define ARORAICON_H

#include "arorastyle.h"

#include <qcolor.h>
#include <qicon.h>
#include <qstringlist.h>

// ICONS01: single choke point for chrome icons (toolbar/menu/dialog
// actions).  Everything resolves through QIcon::fromTheme; the
// bundled sets are real icon themes embedded under :/icons and
// selected by switching QIcon::setThemeName() — existing QIcon
// objects pick the new theme up automatically, which is what makes
// the Settings combo apply live.
//
// Resolution order:
//   active theme -> inherited themes (the sparse sets inherit
//   arora-tabler; the -dark variants inherit arora-tabler-dark) ->
//   arora-tabler via QIcon::setFallbackThemeName -> hicolor (the
//   desktop's own search paths — kept as the last resort so a name
//   no bundled set ships still has a chance to resolve rather than
//   painting a blank hole).
//
// ICONS12: the "native" mode (QIcon::fromTheme straight onto the OS
// theme) is retired — an incomplete desktop theme served missing
// names as duplicate lookalikes, which is the report the bundled
// sets fixed.  Every selectable set is now bundled art; a stored
// "native" id migrates to the default set like any unknown id.
//
// ICONS13: the bundled arora-legacy set (the original 2009 artwork)
// is retired entirely — the Tabler set covers its whole name list,
// so the sparse sets now inherit arora-tabler{,-dark} and Tabler is
// the fallback theme.  A stored "arora-legacy"/"legacy" pick is an
// unknown id and migrates to the default set like any other.
//
// Monochrome bundled sets also ship an "-dark" sibling theme that is
// picked automatically when the application palette is dark — Qt
// renders SVG currentColor as black rather than recoloring to the
// palette, so the dark variants are pre-recolored files.  A forced
// dark palette (THEME01) is invisible to the OS icon theme, so the
// bundled -dark art is what keeps chrome legible there.
//
// Page-content glyphs are intentionally NOT routed here: the app
// window icon, the private-browsing badge, the default page icon,
// the history-item icon and the throbber keep their fixed bundled
// artwork, and favicons/mime-type icons keep their dynamic sources.
//
// ICONS09 — the house-icon rule.  Chrome surfaces (menus, the nav
// bar, tab-strip controls, sidebar rail/panels, location-bar
// indicators, status-bar buttons, dialog chrome, the Preferences
// nav/panels lists) paint in one style: monochrome, tinted with the
// palette's ButtonText at paint time — getChrome().  Everything that
// is identity rather than chrome keeps its own colors and does NOT
// go through getChrome; the exceptions list, deliberately:
//   - the Arora mark (app-menu button, window icon, About, error
//     pages) — brand;
//   - the Qt logo on Help > About Qt — Qt's brand;
//   - container color chips — the color IS the container's identity;
//   - favicons, site/search-engine icons, the default page icon —
//     page content;
//   - extension action icons — manifest artwork;
//   - the private-browsing badge — state art, not a button glyph;
//   - the tor window's nav cluster — sits on a violet bar that is
//     not in the palette, so it keeps its explicit #eee getTinted
//     lift instead (the File > New Tor Window onion IS menu chrome
//     and follows getChrome like every other menu row).
// If a new icon is chrome, use getChrome; if it is identity, leave
// it raw — do not "unify" brand art into monochrome.
namespace AroraIcon {

// Themed icon for a freedesktop-style name (go-previous,
// view-refresh, process-stop, ...).  Live-updates on theme change.
QIcon get(const QString &name);

// ICONS04: same name resolved to the selected set's -dark (light
// glyph) variant — for icons painted on dark chrome while the
// application palette is light, e.g. the tor window's violet toolbar.
// Names with no bundled -dark file fall back to get().
QIcon getOnDark(const QString &name);

// ICONS06: a flat `color` glyph for dark chrome — the shape comes
// from `icon` (getTinted resolves `name` through getOnDark's light
// art first), then every opaque pixel is re-tinted with the alpha
// preserved, so the result matches the palette-painted reader/onion
// buttons exactly whatever bundled set served the base art.  The
// Disabled mode is seeded with `disabledColor` rather than left to
// Qt's generated ghosting, which is what washed the idle
// back/forward arrows out on the violet bar.  The default pair is
// the tor chrome's glyph colors from AroraStyle (the light text and
// the tab strip's dimmed tone) — the single tor color source.
QIcon tinted(const QIcon &icon,
             const QColor &color = AroraStyle::torTextColor(),
             const QColor &disabledColor = AroraStyle::torDimTextColor());
QIcon getTinted(const QString &name,
                const QColor &color = AroraStyle::torTextColor(),
                const QColor &disabledColor = AroraStyle::torDimTextColor());

// ICONS09: the house icon for chrome surfaces — the themed shape
// re-tinted to the palette's ButtonText (the Highlight card's own
// foreground via AroraStyle::selectionTextColor when the pixmap is
// requested Selected, Disabled to the disabled tone),
// resolved lazily so an
// icon-theme swap or a live light<->dark flip restyles icons that
// are already installed on actions.  Menus, toolbars, tabs, panels
// and dialogs all take this path so no surface mixes a theme's raw
// colors with monochrome peers; the identity exceptions are listed
// in the file comment above.
QIcon getChrome(const QString &name);

// Selectable sets — the bundled ids only (ICONS12: "native" is
// retired).
QStringList themeIds();
QString themeDisplayName(const QString &id);

// The persisted choice ("MainWindow/iconTheme", default "tabler" —
// ICONS10: bundled so every chrome name resolves to distinct art).
// ICONS12: a stored "native" (or any unknown id) reads back as the
// default set — old profiles migrate instead of erroring.
QString theme();

// Applies an id immediately — existing icons refresh.  Persisting
// is the caller's job (SettingsDialog writes the QSettings key).
void setTheme(const QString &id);

// setTheme(theme()); called at startup and after palette/scheme
// changes that may flip a bundled set to its "-dark" variant.
void applyFromSettings();

// The icon theme name an id resolves to right now — "arora-<id>" or
// "arora-<id>-dark" when a dark palette is active and the set ships
// the recolor.  Exposed for tests/smokes.
QString effectiveThemeName(const QString &id);

// The freedesktop names covered by the bundled sets (read from the
// qrc so coverage checks test the shipped files, not a list that
// can drift).
QStringList names();

// An icon from a specific set without switching the active theme —
// the Settings combo uses it for per-option previews.
QIcon iconForTheme(const QString &id, const QString &name);

} // namespace AroraIcon

#endif // ARORAICON_H
