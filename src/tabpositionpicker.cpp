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
#include "tabpositionpicker.h"

#include <qpainter.h>

// Display slot -> persisted index.  The row shows Top/Left/Right/
// Bottom (Vivaldi's order); the stored value keeps the historical
// combo order Top/Bottom/Left/Right so saved settings don't remap.

// The tile's miniature: a lighter tab strip on the position's edge
// plus a couple of tab cells inside it.  Palette roles only — Window
// for the strip, Base for the tab cells.
static void paintTabStripPreview(QPainter *p, const QRect &preview,
                                 const QPalette &pal, int positionIndex)
{
    const int bodyInset = 2;
    const bool vertical = (positionIndex == 2 || positionIndex == 3);
    QRect strip;
    if (positionIndex == 0)          // Top
        strip = QRect(preview.left() + bodyInset, preview.top() + bodyInset,
                      preview.width() - 2 * bodyInset, 9);
    else if (positionIndex == 1)     // Bottom
        strip = QRect(preview.left() + bodyInset,
                      preview.bottom() - bodyInset - 8,
                      preview.width() - 2 * bodyInset, 9);
    else if (positionIndex == 2)     // Left
        strip = QRect(preview.left() + bodyInset, preview.top() + bodyInset,
                      12, preview.height() - 2 * bodyInset);
    else                             // Right
        strip = QRect(preview.right() - bodyInset - 11,
                      preview.top() + bodyInset,
                      12, preview.height() - 2 * bodyInset);

    p->setPen(Qt::NoPen);
    p->setBrush(pal.color(QPalette::Window));
    p->drawRect(strip);

    p->setBrush(pal.color(QPalette::Base));
    if (vertical) {
        const int cellWidth = strip.width() - 4;
        for (int i = 0; i < 2; ++i)
            p->drawRect(QRect(strip.left() + 2,
                              strip.top() + 2 + i * 8,
                              cellWidth, 6));
    } else {
        const int cellHeight = strip.height() - 4;
        for (int i = 0; i < 2; ++i)
            p->drawRect(QRect(strip.left() + 2 + i * 14,
                              strip.top() + 2,
                              12, cellHeight));
    }
}

TabPositionPicker::TabPositionPicker(QWidget *parent)
    : PositionPicker({ tr("Top"), tr("Left"), tr("Right"), tr("Bottom") },
                     { 0, 2, 3, 1 },
                     tr("Tab bar position"),
                     &paintTabStripPreview,
                     parent)
{
}
