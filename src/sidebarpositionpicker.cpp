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

#include <qboxlayout.h>
#include <qevent.h>
#include <qpainter.h>
#include <qstyle.h>
#include <qstyleoption.h>

// One tile of the picker: a miniature window sketch with its dock
// panel on the edge this tile represents, plus a label underneath.
// Palette roles only so the previews read correctly under dark
// schemes — Dark for the window body, Window for the dock surface,
// Base for the panel rows, Mid for the icon rail, Highlight for the
// selection frame + check badge.
class SidebarPositionTile : public QWidget
{
public:
    SidebarPositionTile(SidebarPositionPicker *picker, int positionIndex,
                        const QString &label)
        : QWidget(picker)
        , m_picker(picker)
        , m_positionIndex(positionIndex)
        , m_label(label)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(label);
        setAccessibleDescription(
            SidebarPositionPicker::tr("Sidebar position"));
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

        // Miniature window: dark rounded body + a dock panel on the
        // position's edge + a thin icon rail on the panel's outer
        // side + a few list rows inside the panel.
        const QSize previewSize(52, 36);
        const QRect preview(QPoint(tile.x() + (tile.width() - previewSize.width()) / 2,
                                   tile.y() + 3),
                            previewSize);
        const int bodyInset = 2;

        p.setPen(QPen(pal.color(QPalette::Dark).darker(140), 1));
        p.setBrush(pal.color(QPalette::Dark));
        p.drawRoundedRect(preview, 4, 4);

        const int panelWidth = 18;
        const QRect panel(m_positionIndex == 0
                          ? preview.left() + bodyInset
                          : preview.right() - bodyInset - panelWidth + 1,
                          preview.top() + bodyInset,
                          panelWidth,
                          preview.height() - 2 * bodyInset);

        p.setPen(Qt::NoPen);
        p.setBrush(pal.color(QPalette::Window));
        p.drawRect(panel);

        // Icon rail along the panel's outer edge.
        const int railWidth = 5;
        const QRect rail(m_positionIndex == 0 ? panel.left()
                                              : panel.right() - railWidth + 1,
                         panel.top(), railWidth, panel.height());
        p.setBrush(pal.color(QPalette::Mid));
        p.drawRect(rail);
        p.setBrush(pal.color(QPalette::Base));
        for (int i = 0; i < 3; ++i)
            p.drawRect(QRect(rail.left() + 1, rail.top() + 2 + i * 9,
                             railWidth - 2, 6));

        // Panel list rows beside the rail.
        const int rowsLeft = m_positionIndex == 0
            ? rail.right() + 2 : panel.left() + 2;
        const int rowsWidth = panelWidth - railWidth - 5;
        for (int i = 0; i < 3; ++i)
            p.drawRect(QRect(rowsLeft, panel.top() + 2 + i * 9,
                             rowsWidth, 6));

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
    SidebarPositionPicker *m_picker;
    int m_positionIndex;
    QString m_label;
    bool m_selected = false;
};

SidebarPositionPicker::SidebarPositionPicker(QWidget *parent)
    : QWidget(parent)
    , m_currentIndex(0)
{
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    const QString labels[] = { tr("Left"), tr("Right") };
    m_tiles.reserve(2);
    for (int slot = 0; slot < 2; ++slot) {
        // Display order == index order: Left (0), Right (1).
        SidebarPositionTile *tile =
            new SidebarPositionTile(this, slot, labels[slot]);
        tile->setSelected(slot == m_currentIndex);
        layout->addWidget(tile);
        m_tiles.append(tile);
    }
    // The .ui tabstop chain lands on the picker — hand focus to the
    // first tile instead of the NoFocus container.
    setFocusProxy(m_tiles.first());
}

void SidebarPositionPicker::setCurrentIndex(int index)
{
    const int clamped = qBound(0, index, count() - 1);
    if (clamped == m_currentIndex)
        return;
    m_currentIndex = clamped;
    for (SidebarPositionTile *tile : m_tiles)
        tile->setSelected(tile->positionIndex() == m_currentIndex);
    // Combo parity: the signal fires for user commits and
    // programmatic sets alike (like setCurrentIndex ->
    // currentIndexChanged).
    emit positionChanged(m_currentIndex);
}

void SidebarPositionPicker::tileActivated(int positionIndex)
{
    setCurrentIndex(positionIndex);
}

void SidebarPositionPicker::stepTileFocus(SidebarPositionTile *tile, int delta)
{
    const int current = m_tiles.indexOf(tile);
    if (current < 0)
        return;
    m_tiles.at((current + delta + m_tiles.count()) % m_tiles.count())
        ->setFocus();
}
