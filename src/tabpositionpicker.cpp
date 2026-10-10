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

#include <qboxlayout.h>
#include <qevent.h>
#include <qpainter.h>
#include <qstyle.h>
#include <qstyleoption.h>

// Display slot -> persisted index.  The row shows Top/Left/Right/
// Bottom (Vivaldi's order); the stored value keeps the historical
// combo order Top/Bottom/Left/Right so saved settings don't remap.
static const int s_displayOrder[] = { 0, 2, 3, 1 };

// One tile of the picker: a miniature window sketch with its tab
// strip on the edge this tile represents, plus a label underneath.
// Palette roles only so the previews read correctly under dark
// schemes — Dark for the window body (always the darkest role),
// Window for the strip, Base for the tab cells, Highlight for the
// selection frame + check badge.
class TabPositionTile : public QWidget
{
public:
    TabPositionTile(TabPositionPicker *picker, int positionIndex,
                    const QString &label)
        : QWidget(picker)
        , m_picker(picker)
        , m_positionIndex(positionIndex)
        , m_label(label)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(label);
        setAccessibleDescription(TabPositionPicker::tr("Tab bar position"));
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        setToolTip(label);
    }

    int positionIndex() const { return m_positionIndex; }

    void setSelected(bool selected)
    {
        if (m_selected == selected)
            return;
        m_selected = selected;
        update();
    }

    QSize sizeHint() const override { return QSize(72, 64); }
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const QRect tile = rect().adjusted(2, 2, -2, -2);
        const QPalette pal = palette();

        // Selection wash + frame.
        if (m_selected || underMouse()) {
            QColor wash = pal.color(QPalette::Highlight);
            wash.setAlpha(m_selected ? 46 : 20);
            p.setPen(Qt::NoPen);
            p.setBrush(wash);
            p.drawRoundedRect(tile, 6, 6);
        }
        if (m_selected) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(pal.color(QPalette::Highlight), 2));
            p.drawRoundedRect(tile, 6, 6);
        }

        // Miniature window: dark rounded body + lighter tab strip on
        // the position's edge + a couple of tab cells in the strip.
        const QSize previewSize(52, 36);
        const QRect preview(QPoint(tile.x() + (tile.width() - previewSize.width()) / 2,
                                   tile.y() + 3),
                            previewSize);
        const int bodyInset = 2;

        p.setPen(QPen(pal.color(QPalette::Dark).darker(140), 1));
        p.setBrush(pal.color(QPalette::Dark));
        p.drawRoundedRect(preview, 4, 4);

        const bool vertical = (m_positionIndex == 2 || m_positionIndex == 3);
        QRect strip;
        if (m_positionIndex == 0)          // Top
            strip = QRect(preview.left() + bodyInset, preview.top() + bodyInset,
                          preview.width() - 2 * bodyInset, 9);
        else if (m_positionIndex == 1)     // Bottom
            strip = QRect(preview.left() + bodyInset,
                          preview.bottom() - bodyInset - 8,
                          preview.width() - 2 * bodyInset, 9);
        else if (m_positionIndex == 2)     // Left
            strip = QRect(preview.left() + bodyInset, preview.top() + bodyInset,
                          12, preview.height() - 2 * bodyInset);
        else                               // Right
            strip = QRect(preview.right() - bodyInset - 11,
                          preview.top() + bodyInset,
                          12, preview.height() - 2 * bodyInset);

        p.setPen(Qt::NoPen);
        p.setBrush(pal.color(QPalette::Window));
        p.drawRect(strip);

        p.setBrush(pal.color(QPalette::Base));
        if (vertical) {
            const int cellWidth = strip.width() - 4;
            for (int i = 0; i < 2; ++i)
                p.drawRect(QRect(strip.left() + 2,
                                 strip.top() + 2 + i * 8,
                                 cellWidth, 6));
        } else {
            const int cellHeight = strip.height() - 4;
            for (int i = 0; i < 2; ++i)
                p.drawRect(QRect(strip.left() + 2 + i * 14,
                                 strip.top() + 2,
                                 12, cellHeight));
        }

        // Check badge on the selected tile's top-right corner.
        if (m_selected) {
            const int d = 14;
            const QRect badge(tile.right() - d, tile.top() - 1, d, d);
            p.setPen(Qt::NoPen);
            p.setBrush(pal.color(QPalette::Highlight));
            p.drawEllipse(badge);
            p.setPen(QPen(pal.color(QPalette::HighlightedText), 2));
            const int cx = badge.center().x(), cy = badge.center().y();
            p.drawLine(QPoint(cx - 4, cy), QPoint(cx - 1, cy + 3));
            p.drawLine(QPoint(cx - 1, cy + 3), QPoint(cx + 4, cy - 3));
        }

        // Label under the preview.
        QFont f = p.font();
        f.setBold(m_selected);
        p.setFont(f);
        p.setPen(pal.color(QPalette::Text));
        const QRect labelRect(tile.left(), preview.bottom() + 4,
                              tile.width(), tile.bottom() - preview.bottom() - 4);
        p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop, m_label);

        // Keyboard focus indicator.
        if (hasFocus()) {
            QStyleOptionFocusRect opt;
            opt.initFrom(this);
            opt.rect = tile;
            opt.state |= QStyle::State_KeyboardFocusChange;
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &opt, &p, this);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            setFocus(Qt::MouseFocusReason);
            m_picker->tileActivated(m_positionIndex);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Left:
        case Qt::Key_Up:
            m_picker->stepTileFocus(this, -1);
            event->accept();
            return;
        case Qt::Key_Right:
        case Qt::Key_Down:
            m_picker->stepTileFocus(this, 1);
            event->accept();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            m_picker->tileActivated(m_positionIndex);
            event->accept();
            return;
        default:
            break;
        }
        QWidget::keyPressEvent(event);
    }

    void focusInEvent(QFocusEvent *event) override
    {
        update();
        QWidget::focusInEvent(event);
    }

    void focusOutEvent(QFocusEvent *event) override
    {
        update();
        QWidget::focusOutEvent(event);
    }

private:
    TabPositionPicker *m_picker;
    int m_positionIndex;
    QString m_label;
    bool m_selected = false;
};

TabPositionPicker::TabPositionPicker(QWidget *parent)
    : QWidget(parent)
    , m_currentIndex(0)
{
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    const QString labels[] = {
        tr("Top"), tr("Left"), tr("Right"), tr("Bottom")
    };
    m_tiles.reserve(4);
    for (int slot = 0; slot < 4; ++slot) {
        TabPositionTile *tile =
            new TabPositionTile(this, s_displayOrder[slot], labels[slot]);
        tile->setSelected(s_displayOrder[slot] == m_currentIndex);
        layout->addWidget(tile);
        m_tiles.append(tile);
    }
    // The .ui tabstop chain lands on the picker — hand focus to the
    // first tile instead of the NoFocus container.
    setFocusProxy(m_tiles.first());
}

void TabPositionPicker::setCurrentIndex(int index)
{
    const int clamped = qBound(0, index, count() - 1);
    if (clamped == m_currentIndex)
        return;
    m_currentIndex = clamped;
    for (TabPositionTile *tile : m_tiles)
        tile->setSelected(tile->positionIndex() == m_currentIndex);
    // Combo parity: the signal fires for user commits and
    // programmatic sets alike (like setCurrentIndex ->
    // currentIndexChanged).
    emit positionChanged(m_currentIndex);
}

void TabPositionPicker::tileActivated(int positionIndex)
{
    setCurrentIndex(positionIndex);
}

void TabPositionPicker::stepTileFocus(TabPositionTile *tile, int delta)
{
    const int current = m_tiles.indexOf(tile);
    if (current < 0)
        return;
    m_tiles.at((current + delta + m_tiles.count()) % m_tiles.count())
        ->setFocus();
}
