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
#include "downloadgraph.h"

#include <qpainter.h>
#include <qpainterpath.h>

DownloadGraph::DownloadGraph(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QLatin1String("downloadGraph"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(tr("Download speed over time"));
}

void DownloadGraph::setSamples(const QVector<double> &bytesPerSecond)
{
    m_samples = bytesPerSecond;
    // Samples only arrive a few times a second; repainting on every
    // other trigger (resize, expose) is cheap at this size.
    update();
}

QSize DownloadGraph::sizeHint() const
{
    return QSize(160, 48);
}

QSize DownloadGraph::minimumSizeHint() const
{
    return QSize(48, 48);
}

void DownloadGraph::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);

    const QRectF area = QRectF(rect()).adjusted(0, 0, -1, -1);
    const double bottom = area.bottom();

    double peak = 0.0;
    for (double value : m_samples)
        peak = qMax(peak, value);

    const QColor lineColor = palette().color(QPalette::Highlight);
    if (m_samples.isEmpty() || peak <= 0.0) {
        // No data yet — a flat baseline at zero.
        painter.setPen(QPen(palette().color(QPalette::PlaceholderText), 1));
        painter.drawLine(QPointF(area.left(), bottom),
                         QPointF(area.right(), bottom));
        return;
    }

    const int n = m_samples.count();
    const double usableHeight = qMax<double>(area.height() - 2.0, 1.0);
    const double dx = n > 1 ? area.width() / (n - 1) : 0.0;
    QVector<QPointF> points;
    points.reserve(qMax(n, 2));
    for (int i = 0; i < n; ++i) {
        const double x = area.left() + i * dx;
        const double y = bottom - (m_samples.at(i) / peak) * usableHeight;
        points.append(QPointF(x, y));
    }
    if (n == 1) {
        // A single reading reads best as a flat plateau across the
        // whole window rather than a lone dot.
        points.clear();
        const double y = bottom - (m_samples.first() / peak) * usableHeight;
        points.append(QPointF(area.left(), y));
        points.append(QPointF(area.right(), y));
    }

    QPainterPath fill;
    fill.moveTo(QPointF(points.first().x(), bottom));
    for (const QPointF &point : points)
        fill.lineTo(point);
    fill.lineTo(QPointF(points.last().x(), bottom));
    fill.closeSubpath();

    QColor fillColor = lineColor;
    fillColor.setAlphaF(0.35);
    painter.fillPath(fill, fillColor);

    painter.setPen(QPen(lineColor, 1.4));
    painter.drawPolyline(points.constData(), points.count());
}
