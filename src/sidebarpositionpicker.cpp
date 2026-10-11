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
#include "sidebarpositionpicker.h"

#include <qpainter.h>

// The tile's miniature: a dock panel on the position's edge plus a
// thin icon rail on the panel's outer side plus a few list rows
// inside the panel.  Palette roles only — Window for the dock
// surface, Mid for the icon rail, Base for the panel rows.
static void paintDockPanelPreview(QPainter *p, const QRect &preview,
                                  const QPalette &pal, int positionIndex)
{
    const int bodyInset = 2;
    const int panelWidth = 18;
    const QRect panel(positionIndex == 0
                      ? preview.left() + bodyInset
                      : preview.right() - bodyInset - panelWidth + 1,
                      preview.top() + bodyInset,
                      panelWidth,
                      preview.height() - 2 * bodyInset);

    p->setPen(Qt::NoPen);
    p->setBrush(pal.color(QPalette::Window));
    p->drawRect(panel);

    // Icon rail along the panel's outer edge.
    const int railWidth = 5;
    const QRect rail(positionIndex == 0 ? panel.left()
                                        : panel.right() - railWidth + 1,
                     panel.top(), railWidth, panel.height());
    p->setBrush(pal.color(QPalette::Mid));
    p->drawRect(rail);
    p->setBrush(pal.color(QPalette::Base));
    for (int i = 0; i < 3; ++i)
        p->drawRect(QRect(rail.left() + 1, rail.top() + 2 + i * 9,
                          railWidth - 2, 6));

    // Panel list rows beside the rail.
    const int rowsLeft = positionIndex == 0
        ? rail.right() + 2 : panel.left() + 2;
    const int rowsWidth = panelWidth - railWidth - 5;
    for (int i = 0; i < 3; ++i)
        p->drawRect(QRect(rowsLeft, panel.top() + 2 + i * 9,
                          rowsWidth, 6));
}

SidebarPositionPicker::SidebarPositionPicker(QWidget *parent)
    : PositionPicker({ tr("Left"), tr("Right") },
                     { 0, 1 },
                     tr("Sidebar position"),
                     &paintDockPanelPreview,
                     parent)
{
}
