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
#include "positionpicker.h"

#include "arorastyle.h"

#include <qboxlayout.h>
#include <qevent.h>
#include <qpainter.h>
#include <qstyle.h>
#include <qstyleoption.h>

// One tile of the picker: a miniature window sketch (the rounded
// Dark body is shared, the interior comes from the picker's
// PreviewPainter) plus a label underneath.  Palette roles only so
// the previews read correctly under dark schemes — Dark for the
// window body (always the darkest role), Highlight for the
// selection frame + check badge.
class PositionPickerTile : public QWidget
{
public:
    PositionPickerTile(PositionPicker *picker, int positionIndex,
                       const QString &label,
                       const QString &accessibleDescription,
                       PositionPicker::PreviewPainter paintPreview)
        : QWidget(picker)
        , m_picker(picker)
        , m_positionIndex(positionIndex)
        , m_label(label)
        , m_paintPreview(paintPreview)
    {
        setFocusPolicy(Qt::StrongFocus);
        setAccessibleName(label);
        setAccessibleDescription(accessibleDescription);
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

        // Miniature window: dark rounded body; the picker's painter
        // fills in the strip/dock interior.
        const QSize previewSize(52, 36);
        const QRect preview(QPoint(tile.x() + (tile.width() - previewSize.width()) / 2,
                                   tile.y() + 3),
                            previewSize);

        p.setPen(QPen(pal.color(QPalette::Dark).darker(140), 1));
        p.setBrush(pal.color(QPalette::Dark));
        p.drawRoundedRect(preview, 4, 4);

        if (m_paintPreview)
            m_paintPreview(&p, preview, pal, m_positionIndex);

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
    PositionPicker *m_picker;
    int m_positionIndex;
    QString m_label;
    bool m_selected = false;
    PositionPicker::PreviewPainter m_paintPreview;
};

PositionPicker::PositionPicker(const QStringList &labels,
                               const QVector<int> &displayOrder,
                               const QString &tileAccessibleDescription,
                               PreviewPainter paintPreview,
                               QWidget *parent)
    : QWidget(parent)
    , m_currentIndex(0)
{
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(AroraStyle::Spacing::Large);

    m_tiles.reserve(labels.count());
    for (int slot = 0; slot < labels.count(); ++slot) {
        const int positionIndex = slot < displayOrder.count()
            ? displayOrder.at(slot) : slot;
        PositionPickerTile *tile =
            new PositionPickerTile(this, positionIndex, labels.at(slot),
                                   tileAccessibleDescription,
                                   paintPreview);
        tile->setSelected(positionIndex == m_currentIndex);
        layout->addWidget(tile);
        m_tiles.append(tile);
    }
    // The .ui tabstop chain lands on the picker — hand focus to the
    // first tile instead of the NoFocus container.
    if (!m_tiles.isEmpty())
        setFocusProxy(m_tiles.first());
}

void PositionPicker::setCurrentIndex(int index)
{
    const int clamped = qBound(0, index, count() - 1);
    if (clamped == m_currentIndex)
        return;
    m_currentIndex = clamped;
    for (PositionPickerTile *tile : m_tiles)
        tile->setSelected(tile->positionIndex() == m_currentIndex);
    // Combo parity: the signal fires for user commits and
    // programmatic sets alike (like setCurrentIndex ->
    // currentIndexChanged).
    emit positionChanged(m_currentIndex);
}

void PositionPicker::tileActivated(int positionIndex)
{
    setCurrentIndex(positionIndex);
}

void PositionPicker::stepTileFocus(PositionPickerTile *tile, int delta)
{
    const int current = m_tiles.indexOf(tile);
    if (current < 0)
        return;
    m_tiles.at((current + delta + m_tiles.count()) % m_tiles.count())
        ->setFocus();
}
