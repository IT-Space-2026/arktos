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
#ifndef POSITIONPICKER_H
#define POSITIONPICKER_H

#include <qpalette.h>
#include <qvector.h>
#include <qwidget.h>

class PositionPickerTile;

// UIAUDIT03: shared base of the Vivaldi-style visual pickers
// (TabPositionPicker for the tab strip, SidebarPositionPicker for the
// dock) — a row of clickable tiles, each sketching a miniature window
// plus a label underneath.  The base owns the tile chrome (selection
// wash/frame, check badge, label, focus rect) and the keyboard/mouse
// interaction; each concrete picker supplies a PreviewPainter that
// draws its strip/dock preview inside the miniature window rect.
class PositionPicker : public QWidget
{
    Q_OBJECT

public:
    // Draws the position-specific interior of the miniature window
    // (the rounded Dark body is already painted by the tile).
    typedef void (*PreviewPainter)(QPainter *painter,
                                   const QRect &preview,
                                   const QPalette &palette,
                                   int positionIndex);

    // labels.size() tiles are laid out left to right; displayOrder
    // maps display slot -> persisted index (pass an identity mapping
    // when the row order already matches the stored values).
    PositionPicker(const QStringList &labels,
                   const QVector<int> &displayOrder,
                   const QString &tileAccessibleDescription,
                   PreviewPainter paintPreview,
                   QWidget *parent = nullptr);

    // QComboBox-shaped surface so the settings load/save lines stay
    // unchanged.
    int currentIndex() const { return m_currentIndex; }
    int count() const { return m_tiles.count(); }

public slots:
    void setCurrentIndex(int index);

signals:
    void positionChanged(int index);

private:
    friend class PositionPickerTile;
    void tileActivated(int positionIndex);
    void stepTileFocus(PositionPickerTile *tile, int delta);

    int m_currentIndex;
    QVector<PositionPickerTile *> m_tiles;
};

#endif // POSITIONPICKER_H
