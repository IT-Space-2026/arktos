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
#ifndef TABPOSITIONPICKER_H
#define TABPOSITIONPICKER_H

#include <qvector.h>
#include <qwidget.h>

class TabPositionTile;

// TABS03: Vivaldi-style visual picker for the Tab Settings page's
// tab-bar position option — four clickable tiles that each sketch a
// miniature browser window with the tab strip on its Top/Left/Right/
// Bottom edge (plus a label under each).  The displayed tile order is
// Top/Left/Right/Bottom, but the index semantics stay identical to the
// persisted tabs/tabBarPosition value and the old combo (0=Top,
// 1=Bottom, 2=Left, 3=Right) so existing settings keep their meaning.
class TabPositionPicker : public QWidget
{
    Q_OBJECT

public:
    explicit TabPositionPicker(QWidget *parent = nullptr);

    // QComboBox-shaped surface so the settings load/save lines stay
    // unchanged: index 0..3 == Top/Bottom/Left/Right.
    int currentIndex() const { return m_currentIndex; }
    int count() const { return 4; }

public slots:
    void setCurrentIndex(int index);

signals:
    void positionChanged(int index);

private:
    friend class TabPositionTile;
    void tileActivated(int positionIndex);
    void stepTileFocus(TabPositionTile *tile, int delta);

    int m_currentIndex;
    QVector<TabPositionTile *> m_tiles;
};

#endif // TABPOSITIONPICKER_H
