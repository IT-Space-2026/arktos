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

#include "browsertheme.h"

#include <qapplication.h>
#include <qstyle.h>
#include <qstylefactory.h>
#include <qstylehints.h>

// State needed to undo a palette we forced ourselves — on a flip back
// to light we restore exactly what the platform gave us instead of
// inventing a "light" palette.
static bool s_forced = false;
static QPalette s_savedPalette;
static QString s_savedStyleName;

Qt::ColorScheme BrowserTheme::preferredColorScheme()
{
    const QByteArray override_ = qgetenv("ARORA_COLOR_SCHEME").toLower();
    if (override_ == "dark")
        return Qt::ColorScheme::Dark;
    if (override_ == "light")
        return Qt::ColorScheme::Light;
    return QGuiApplication::styleHints()->colorScheme();
}

bool BrowserTheme::isDarkPalette(const QPalette &palette)
{
    return palette.color(QPalette::Active, QPalette::Window).lightness() < 128;
}

QPalette BrowserTheme::darkPalette()
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

void BrowserTheme::applyColorScheme()
{
    const Qt::ColorScheme scheme = preferredColorScheme();
    if (scheme == Qt::ColorScheme::Unknown)
        return;

    const bool darkWanted = scheme == Qt::ColorScheme::Dark;
    const bool darkActive = isDarkPalette(QApplication::palette());

    if (darkWanted == darkActive) {
        // Matches already — either the platform theme handled it or we
        // forced it earlier.  Only clear our flag when the platform's
        // own palette agrees with the (non-dark) preference.
        if (!darkWanted)
            s_forced = false;
        return;
    }

    if (darkWanted) {
        if (!s_forced) {
            s_savedPalette = QApplication::palette();
            s_savedStyleName = QApplication::style()->name();
        }
        // windowsvista/macos ignore palettes; a stylesheet style has no
        // name and already owns the look — leave those alone style-wise
        // and just swap the palette, which still recolors everything
        // the style does not hardcode.
        if (!s_savedStyleName.isEmpty()
                && s_savedStyleName != QLatin1String("fusion")) {
            if (QStyle *fusion = QStyleFactory::create(QLatin1String("fusion")))
                QApplication::setStyle(fusion);
        }
        QApplication::setPalette(darkPalette());
        s_forced = true;
    } else if (s_forced) {
        if (!s_savedStyleName.isEmpty()) {
            if (QStyle *style = QStyleFactory::create(s_savedStyleName))
                QApplication::setStyle(style);
            s_savedStyleName.clear();
        }
        QApplication::setPalette(s_savedPalette);
        s_forced = false;
    }
}

bool BrowserTheme::paletteIsForced()
{
    return s_forced;
}
