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
#include "sidebargrip.h"

#include <qdockwidget.h>
#include <qevent.h>
#include <qmainwindow.h>
#include <qpainter.h>

SidebarGrip::SidebarGrip(QDockWidget *dock, QWidget *parent)
    : QWidget(parent)
    , m_dock(dock)
    , m_area(Qt::LeftDockWidgetArea)
    , m_dragging(false)
    , m_startX(0)
    , m_startWidth(0)
    , m_requestedWidth(0)
{
    setFixedWidth(7);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setCursor(Qt::SizeHorCursor);
    setAccessibleName(tr("Sidebar resize handle"));
    setToolTip(tr("Drag to resize the sidebar — double-click to reset"));
}

void SidebarGrip::setDockArea(Qt::DockWidgetArea area)
{
    m_area = area;
}

QSize SidebarGrip::sizeHint() const
{
    return QSize(7, 0);
}

int SidebarGrip::defaultWidth()
{
    return 280;
}

int SidebarGrip::clampedWidth(int width, int windowWidth)
{
    // The floor keeps the panel usable; the ceiling follows the
    // window so a shrunken window still lets the dock shrink and the
    // content area is never squeezed to nothing.
    const int hi = qMax(160, windowWidth - 200);
    return qBound(160, width, hi);
}

int SidebarGrip::applyWidth(int width)
{
    QMainWindow *window = m_dock
        ? qobject_cast<QMainWindow*>(m_dock->parentWidget())
        : nullptr;
    if (!window)
        return width;
    const int clamped = clampedWidth(width, window->width());
    window->resizeDocks({ m_dock }, { clamped }, Qt::Horizontal);
    return clamped;
}

void SidebarGrip::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event);
    QPainter painter(this);
    // A subtle three-dot handle — palette-only so dark schemes read
    // correctly (UIP01).
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette().color(QPalette::PlaceholderText));
    const int cx = rect().center().x();
    const int cy = rect().center().y();
    for (int i = -1; i <= 1; ++i)
        painter.drawEllipse(QPointF(cx, cy + i * 6), 1.6, 1.6);
}

void SidebarGrip::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && m_dock) {
        m_dragging = true;
        m_startX = event->globalPosition().toPoint().x();
        m_startWidth = m_dock->width();
        m_requestedWidth = 0;
    }
    QWidget::mousePressEvent(event);
}

void SidebarGrip::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging && (event->buttons() & Qt::LeftButton)) {
        const int dx = event->globalPosition().toPoint().x() - m_startX;
        // Docked left: the grip is the dock's right edge, so dragging
        // right widens; docked right the grip is the left edge.
        const bool right = (m_area == Qt::RightDockWidgetArea);
        m_requestedWidth = applyWidth(m_startWidth + (right ? -dx : dx));
    }
    QWidget::mouseMoveEvent(event);
}

void SidebarGrip::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        // resizeDocks lands on the next layout pass, so the dock's
        // widget still reports the pre-drag width here — persist the
        // clamped width the drag actually requested.
        if (m_dock)
            emit resizeFinished(m_requestedWidth > 0
                ? m_requestedWidth : m_dock->width());
    }
    QWidget::mouseReleaseEvent(event);
}

void SidebarGrip::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
        emit resizeFinished(applyWidth(defaultWidth()));
    }
    QWidget::mouseDoubleClickEvent(event);
}
