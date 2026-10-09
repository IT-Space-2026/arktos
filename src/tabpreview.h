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

#ifndef TABPREVIEW_H
#define TABPREVIEW_H

#include <qframe.h>

class QLabel;
class TabBar;

/*!
    POL02: the tab-strip hover card — a passive, non-focusable popup
    showing the tab's title, address and a thumbnail of the page's last
    live render (favicon + title fallback for sleeping tabs and tabs
    with no captured frame, matching SLEEP01's dimmed presentation).

    The widget is a Qt::ToolTip window: it never takes focus and is
    fully transparent to mouse events, so it can sit under the cursor
    without disturbing the tab strip's hover tracking.  The owning
    TabBar drives show/hide timing; previewTab() repopulates the card
    so a visible popup retargets instantly while the pointer walks the
    strip.
*/
class TabPreview : public QFrame
{
    Q_OBJECT

public:
    explicit TabPreview(TabBar *bar);

    // Populates the card for the tab at index, positions it next to
    // the tab and shows it.  Hides instead when index is invalid.
    void previewTab(int index);

    // Introspection for the smoke run.
    int previewedIndex() const;
    QString titleText() const;
    QString detailText() const;
    bool hasThumbnail() const;

private:
    void positionFor(int index);

    TabBar *m_bar;
    QLabel *m_thumbnail;
    QLabel *m_title;
    QLabel *m_detail;
    int m_index;
    bool m_hasThumbnail;
};

#endif // TABPREVIEW_H
