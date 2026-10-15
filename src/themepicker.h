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
#ifndef THEMEPICKER_H
#define THEMEPICKER_H

#include "positionpicker.h"

// THEM05: Vivaldi-style visual picker for the Appearance page's
// Window theme row — three clickable tiles that each sketch a small
// page swatch in the scheme they commit: a light document, the
// browser's own dark palette, and a half/half split for Auto
// (follows the desktop).  Tiles display left to right as
// Light / Dark / Auto but commit the persisted combo indices
// system=0 / light=1 / dark=2 so the browser/colorScheme values
// keep their meaning.  Tile chrome + interaction come from
// PositionPicker (UIAUDIT03).
class ThemePicker : public PositionPicker
{
    Q_OBJECT

public:
    explicit ThemePicker(QWidget *parent = nullptr);
};

#endif // THEMEPICKER_H
