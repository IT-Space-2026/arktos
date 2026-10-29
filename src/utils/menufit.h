/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef MENUFIT_H
#define MENUFIT_H

#include <qmenu.h>
#include <qsize.h>
#include <qwidget.h>

// CA08: the "fit a popup menu to its embedded panel" resize shared
// by the location-bar popup buttons (site shield, tor circuit).
// Grow only, never shrink — shrinking a popup under the cursor is
// the "creeping popup" class of bug; spare room is harmless.
// QMenu::sizeHint() caches the open-time footprint, so the target is
// computed from the panel's own hint plus the chrome around it.
inline void growMenuToFit(QMenu *menu, QWidget *panel)
{
    if (!menu->isVisible())
        return;
    const QSize chrome = menu->size() - panel->size();
    const QSize grow = (panel->sizeHint() + chrome)
        .expandedTo(menu->size());
    if (grow != menu->size())
        menu->resize(grow);
}

#endif // MENUFIT_H
