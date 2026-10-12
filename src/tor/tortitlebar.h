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

#ifndef TORTITLEBAR_H
#define TORTITLEBAR_H

#include <qwidget.h>

class QMenuBar;
class QToolButton;
class SqueezeLabel;

// TOR11: the drawn title bar for a frameless tor window.  A native
// title bar cannot be tinted per-window — on GNOME Wayland Qt's own
// client-side decoration only follows the session-wide portal
// color-scheme, and _GTK_THEME_VARIANT exists solely on X11 — so the
// tor window goes Qt::FramelessWindowHint and its menu row doubles as
// the title bar (headerbar style): a Tor badge + live window title in
// the left corner, minimize/maximize/close in the right corner, and
// empty menubar space acting as the drag handle.  The real QMenuBar
// stays in the QMainWindow menu slot — putting a wrapper widget there
// does not survive the next menuBar() call, which lazily replaces a
// non-QMenuBar menu widget and loses the populated menus.
//
// Move and edge resize still delegate to the window manager through
// QWindow::startSystemMove/startSystemResize, so snapping, tiling and
// edge resize keep working; the window's violet frame ring
// (FrameMargin) doubles as the resize grab zone.
class TorTitleBar : public QWidget
{
    Q_OBJECT

public:
    // Decorates window's menubar and frame ring.  window must be a
    // QMainWindow-shaped widget with a populated menubar; the bar
    // reparents into menuBar() via setCornerWidget.
    explicit TorTitleBar(QWidget *window);

    // Width of the violet frame ring drawn around the window content —
    // also the edge-resize grab zone.  0 while maximized/fullscreen.
    static constexpr int FrameMargin = 4;

    // The window edges a point inside the frame ring maps to (a
    // position can hit two edges at a corner).  Exposed for tests.
    static Qt::Edges frameEdges(const QRect &windowRect,
                                const QPoint &pos);

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void beginDrag(const QPoint &globalPos);
    void endDrag();
    void toggleMaximized();
    bool resizable() const;
    void showWindowMenu(const QPoint &globalPos);
    void syncWindowButtons();
    void syncFrameMargins();
    void syncFrameCursor(const QPoint &pos);

    QWidget *m_window;
    QMenuBar *m_menuBar;
    QWidget *m_controls;
    SqueezeLabel *m_titleLabel;
    QToolButton *m_minimizeButton;
    QToolButton *m_maximizeButton;
    QToolButton *m_closeButton;
    QPoint m_dragOffset;
};

#endif // TORTITLEBAR_H
