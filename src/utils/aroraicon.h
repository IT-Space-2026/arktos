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

#include <qcolor.h>
#include <qicon.h>
#include <qstringlist.h>

// ICONS01: single choke point for chrome icons (toolbar/menu/dialog
// actions).  Everything resolves through QIcon::fromTheme so the
// desktop icon theme is honored in "native" mode; the bundled sets
// are real icon themes embedded under :/icons and selected by
// switching QIcon::setThemeName() — existing QIcon objects pick the
// new theme up automatically, which is what makes the Settings combo
// apply live.
//
// Resolution order for every mode:
//   active theme -> inherited themes -> arora-legacy (the original
//   2009 artwork via QIcon::setFallbackThemeName) -> hicolor.
// "native" maps to whatever theme the platform reported at startup;
// names the desktop theme lacks fall through to the bundled art, so
// theme-less systems (Windows/macOS defaults, bare containers) still
// render a full toolbar.
//
// Monochrome bundled sets also ship an "-dark" sibling theme that is
// picked automatically when the application palette is dark — Qt
// renders SVG currentColor as black rather than recoloring to the
// palette, so the dark variants are pre-recolored files.  "native"
// under a dark palette probes the captured system theme for a
// light-on-dark glyph and falls back to the bundled -dark art when
// it cannot serve one — a forced dark palette (THEME01) is invisible
// to the OS icon theme, and some "dark" desktops still ship
// dark-glyph sets.
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
// "native" and names with no bundled -dark file fall back to get().
QIcon getOnDark(const QString &name);

// ICONS06: a flat `color` glyph for dark chrome — the shape comes
// from `icon` (getTinted resolves `name` through getOnDark's light
// art first), then every opaque pixel is re-tinted with the alpha
// preserved, so the result matches the palette-painted reader/onion
// buttons exactly whatever theme served the base art — including
// "native", which has no -dark variant for getOnDark to pin.  The
// Disabled mode is seeded with `disabledColor` rather than left to
// Qt's generated ghosting, which is what washed the idle
// back/forward arrows out on the violet bar.  The default pair is
// the tor chrome's glyph colors (enabled #eee, disabled #b9aecf —
// the tab strip's dimmed text tone).
QIcon tinted(const QIcon &icon,
             const QColor &color = QColor(0xee, 0xee, 0xee),
             const QColor &disabledColor = QColor(0xb9, 0xae, 0xcf));
QIcon getTinted(const QString &name,
                const QColor &color = QColor(0xee, 0xee, 0xee),
                const QColor &disabledColor = QColor(0xb9, 0xae, 0xcf));

// ICONS09: the house icon for chrome surfaces — the themed shape
// re-tinted to the palette's ButtonText, resolved lazily so an
// icon-theme swap or a live light<->dark flip restyles icons that
// are already installed on actions.  Menus, toolbars, tabs, panels
// and dialogs all take this path so no surface mixes a theme's raw
// colors with monochrome peers; the identity exceptions are listed
// in the file comment above.
QIcon getChrome(const QString &name);

// Selectable sets: "native" first, then the bundled ids.
QStringList themeIds();
QString themeDisplayName(const QString &id);

// The persisted choice ("MainWindow/iconTheme", default "native").
QString theme();

// Applies an id immediately — existing icons refresh.  Persisting
// is the caller's job (SettingsDialog writes the QSettings key).
void setTheme(const QString &id);

// setTheme(theme()); called at startup and after palette/scheme
// changes that may flip a bundled set to its "-dark" variant.
void applyFromSettings();

// The icon theme name an id resolves to right now — the captured
// system theme for "native", "arora-<id>" or "arora-<id>-dark" for
// the bundled sets.  Exposed for tests/smokes.
QString effectiveThemeName(const QString &id);

// The freedesktop names covered by the bundled sets (read from the
// qrc so coverage checks test the shipped files, not a list that
// can drift).
QStringList names();

// An icon from a specific set without switching the active theme —
// the Settings combo uses it for per-option previews.  "native" maps
// to the plain QIcon::fromTheme lookup.
QIcon iconForTheme(const QString &id, const QString &name);

} // namespace AroraIcon

#endif // ARORAICON_H
