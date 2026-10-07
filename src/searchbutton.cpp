/*
 * Copyright 2009 Benjamin C. Meyer <ben@meyerhome.net>
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

#include "searchbutton.h"

#include <qcompleter.h>
#include <qevent.h>
#include <qlineedit.h>
#include <qpainter.h>
#include <qpainterpath.h>

SearchButton::SearchButton(QWidget *parent)
    : QAbstractButton(parent)
    , m_showMenuTriangle(false)
{
    setFocusPolicy(Qt::NoFocus);
    setCursor(Qt::ArrowCursor);
    setAccessibleName(tr("Search"));
    setMinimumSize(sizeHint());
}

QSize SearchButton::sizeHint() const
{
    if (!m_cache.isNull())
        return (QSizeF(m_cache.size()) / m_cache.devicePixelRatio()).toSize();
    if (m_showMenuTriangle)
        return QSize(16, 16);
    return QSize(12, 16);
}

QImage SearchButton::generateSearchImage(bool dropDown)
{
    // UIP01: render at the device pixel ratio so the glyph stays crisp
    // under fractional scaling — a fixed 12x16 image blurs on 1.25x/1.5x
    // screens.  Painter runs in logical coordinates (pen widths scale
    // with the transform).
    const qreal dpr = devicePixelRatioF();
    const int logicalWidth = dropDown ? 16 : 12;
    const int logicalHeight = 16;
    QImage image(qRound(logicalWidth * dpr), qRound(logicalHeight * dpr),
                 QImage::Format_ARGB32);
    image.setDevicePixelRatio(dpr);
    image.fill(qRgba(0, 0, 0, 0));
    QPainterPath path;

    // draw magnify glass circle
    int radius = logicalHeight / 2;
    QRect circle(1, 1, radius, radius);
    path.addEllipse(circle);

    // draw handle
    path.arcMoveTo(circle, 300);
    QPointF currentPosition = path.currentPosition();
    path.moveTo(currentPosition.x() + 1, currentPosition.y() + 1);
    if (dropDown)
        path.lineTo(logicalWidth-6, logicalHeight-4);
    else
        path.lineTo(logicalWidth-2, logicalHeight-4);

    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.scale(dpr, dpr);
    // Track the palette instead of a fixed darkGray so the glyph stays
    // legible under a dark application theme.
    const QColor glyphColor =
        palette().color(QPalette::Disabled, QPalette::Text);
    painter.setPen(QPen(glyphColor, 2));
    painter.drawPath(path);

    if (dropDown) {
        // draw the dropdown triangle
        QPainterPath dropPath;
        dropPath.arcMoveTo(circle, 320);
        QPointF currentPosition = dropPath.currentPosition();
        currentPosition = QPointF(currentPosition.x() + 2, currentPosition.y() + 0.5);
        dropPath.moveTo(currentPosition);
        dropPath.lineTo(currentPosition.x() + 4, currentPosition.y());
        dropPath.lineTo(currentPosition.x() + 2, currentPosition.y() + 2);
        dropPath.closeSubpath();
        painter.setPen(glyphColor);
        painter.setBrush(glyphColor);
        painter.setRenderHint(QPainter::Antialiasing, false);
        painter.drawPath(dropPath);
    }
    painter.end();
    m_cacheColor = glyphColor;
    return image;
}

void SearchButton::setImage(const QImage &image)
{
    m_cache = image;
    setMinimumSize(sizeHint());
    update();
}

void SearchButton::setShowMenuTriangle(bool show)
{
    m_showMenuTriangle = show;
    setMinimumSize(sizeHint());
}

bool SearchButton::showMenuTriangle() const
{
    return m_showMenuTriangle;
}

void SearchButton::changeEvent(QEvent *event)
{
    // Palette swaps (light/dark theme change) must regenerate the
    // glyph — the cache stores the color it was rendered with.
    if (event->type() == QEvent::PaletteChange
            || event->type() == QEvent::ApplicationPaletteChange)
        m_cache = QImage();
    QAbstractButton::changeEvent(event);
}

void SearchButton::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    if (m_cache.isNull()
            || m_cache.devicePixelRatio() != devicePixelRatioF()
            || m_cacheColor != palette().color(QPalette::Disabled, QPalette::Text))
        m_cache = generateSearchImage(m_showMenuTriangle);
    QPainter painter(this);
    painter.drawImage(QPoint(0, 0), m_cache);
}

