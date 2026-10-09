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
#include <qobject.h>

class QDialog;

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
//
// THEME01: the Appearance page's Theme combo persists
// "browser/colorScheme" = system|light|dark (default system).  An
// explicit light/dark pick wins outright; "system" falls through to
// the env override, then the platform's reported scheme.
namespace BrowserTheme {

// What the effective preference is: an explicit settings pick, else
// the env override, else the platform's reported scheme (may be
// Unknown — treat as "leave theme alone").
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

// Singleton notifier (process lifetime): emits chromeSchemeChanged()
// whenever applyColorScheme() actually flips the application palette.
// For observers that cannot see the palette change themselves — e.g.
// the qrc start page's channel object or future chrome pieces.
class BrowserThemeNotifier : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

signals:
    void chromeSchemeChanged();
};

BrowserThemeNotifier *themeNotifier();

// THEME01: tags the <html> element of a generated internal page
// (htmls/*.html) so its dark-scheme CSS rules — the html.arora-dark
// selectors — engage while the chrome palette is dark.  The pages are
// produced as QStrings at generation time, so prefers-color-scheme
// (which follows the OS, not the app palette) is not an option.
// No-op on a light palette or markup without an <html> tag.
void decorateInternalPage(QString &html);

// UIP02: uniform, modern metrics for dialog push buttons — a minimum
// width so OK/Cancel-style rows stop shrinking to their caption text,
// and a taller minimum height so the buttons don't read as cramped
// Qt4-era controls.  Applied through size constraints only, never a
// stylesheet, so the active style (native, Fusion-forced dark, or
// platform theme) keeps rendering the button itself.
//
// Buttons inside a widget carrying the dynamic property
// "aroraNoButtonPolish"=true (e.g. the compact per-download row
// widget) are left alone — composite list rows need compact buttons.
void polishDialogButtons(QDialog *dialog);

// Installs the application event filter that runs
// polishDialogButtons() on each QDialog once, as it is polished
// before its first show.  Called once by BrowserApplication.
void installDialogButtonPolish();

}

#endif // BROWSERTHEME_H
