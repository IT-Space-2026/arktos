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

#ifndef BROWSERTHEME_H
#define BROWSERTHEME_H

#include <qpalette.h>

// UIP01: follow-the-desktop light/dark support.  Qt 6.5+ reports the
// desktop preference through QStyleHints::colorScheme(); the platform
// theme normally reacts by swapping the application palette itself
// (GNOME/GTK portal does, KDE does), but styles like windowsvista and
// macos have a fixed look that ignores palettes entirely — on those
// the app must install a palette itself.  applyColorScheme() does
// that, switching to Fusion first since it is the one style that
// honors a custom palette on every platform.
//
// ARORA_COLOR_SCHEME=dark|light overrides the desktop preference —
// mainly useful on desktops that do not report a scheme and for
// headless testing (the offscreen QPA reports Unknown).
namespace BrowserTheme {

// What the environment asks for: env override, else the platform's
// reported scheme (may be Unknown — treat as "leave theme alone").
Qt::ColorScheme preferredColorScheme();

bool isDarkPalette(const QPalette &palette);

// Palette for forced dark mode (Fusion-style dark chrome).
QPalette darkPalette();

// Apply preferredColorScheme() to the application: installs the dark
// palette (+Fusion if the current style cannot take palettes) when the
// desktop wants dark but the palette is still light; restores the
// palette/style captured before we forced anything when it flips back.
// Idempotent; safe to call repeatedly.
void applyColorScheme();

// True when the current palette was installed by applyColorScheme()
// rather than the platform theme.
bool paletteIsForced();

}

#endif // BROWSERTHEME_H
