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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#ifndef FLOWLAYOUT_H
#define FLOWLAYOUT_H

#include <qlayout.h>
#include <qlist.h>
#include <qstyle.h>

/*
    A layout that places its items left-to-right and wraps onto a new
    row when the available width runs out — the classic flow layout.
    Used where a fixed horizontal row would clip or crush its children
    at narrow widths (the downloads sidebar detail card, DOWN04).
*/
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent, int margin = -1,
                        int hSpacing = -1, int vSpacing = -1);
    explicit FlowLayout(int margin = -1, int hSpacing = -1,
                        int vSpacing = -1);
    ~FlowLayout() override;

    void addItem(QLayoutItem *item) override;
    int horizontalSpacing() const;
    int verticalSpacing() const;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    void setGeometry(const QRect &rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    int smartSpacing(QStyle::PixelMetric pm) const;

    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};

#endif // FLOWLAYOUT_H
