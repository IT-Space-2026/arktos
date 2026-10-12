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
#ifndef SIDEBARPOSITIONPICKER_H
#define SIDEBARPOSITIONPICKER_H

#include "positionpicker.h"

// SIDE03: Vivaldi-style visual picker for the Appearance page's
// Sidebar group — two clickable tiles that each sketch a miniature
// browser window with the dock panel on its Left or Right edge (plus
// a label under each), mirroring TABS03's TabPositionPicker.  Index
// semantics stay identical to the persisted
// MainWindow/sidebarDockArea mapping the old combo used (0 = Left,
// 1 = Right) so existing settings keep their meaning.
// Tile chrome + interaction come from PositionPicker (UIAUDIT03).
class SidebarPositionPicker : public PositionPicker
{
    Q_OBJECT

public:
    explicit SidebarPositionPicker(QWidget *parent = nullptr);
};

#endif // SIDEBARPOSITIONPICKER_H
