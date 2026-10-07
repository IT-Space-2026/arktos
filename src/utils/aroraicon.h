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
// palette, so the dark variants are pre-recolored files.
//
// Page-content glyphs are intentionally NOT routed here: the app
// window icon, the private-browsing badge, the default page icon,
// the history-item icon and the throbber keep their fixed bundled
// artwork, and favicons/mime-type icons keep their dynamic sources.
namespace AroraIcon {

// Themed icon for a freedesktop-style name (go-previous,
// view-refresh, process-stop, ...).  Live-updates on theme change.
QIcon get(const QString &name);

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
