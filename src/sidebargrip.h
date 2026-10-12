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
#ifndef SIDEBARGRIP_H
#define SIDEBARGRIP_H

#include <qpointer.h>
#include <qwidget.h>

class QDockWidget;

// RSZ01: the visible resize handle along the sidebar dock's inner
// edge — a slim grip strip the user drags to resize the dock and
// double-clicks to snap back to the default width.  The widget only
// performs live QMainWindow::resizeDocks calls; the owning window
// persists the result through the resizeFinished signal.
class SidebarGrip : public QWidget
{
    Q_OBJECT

signals:
    // A drag or a double-click reset finished — the width to persist
    // (the dock's real width after a drag, the default after a
    // reset).
    void resizeFinished(int width);

public:
    explicit SidebarGrip(QDockWidget *dock, QWidget *parent = nullptr);

    // Which side of the window the dock sits on — it decides which
    // drag direction widens.  The host re-points this when the dock
    // moves between the left and right areas.
    void setDockArea(Qt::DockWidgetArea area);

    QSize sizeHint() const override;

    // The width a double-click restores, and the bounds every width
    // (drag or restore) is clamped into — the dock can neither
    // collapse to zero nor swallow the window's content area.
    static int defaultWidth();
    static int clampedWidth(int width, int windowWidth);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;

private:
    int applyWidth(int width);

    QPointer<QDockWidget> m_dock;
    Qt::DockWidgetArea m_area;
    bool m_dragging;
    int m_startX;
    int m_startWidth;
    int m_requestedWidth;
};

#endif // SIDEBARGRIP_H
