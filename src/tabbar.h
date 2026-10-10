/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#ifndef TABBAR_H
#define TABBAR_H

#include <qtabbar.h>

#include "containermanager.h"
#include "tabwidget.h"

class QTimer;
class QPainter;
class TabPreview;

/*
    Tab bar with a few more features such as a context menu and shortcuts
 */
class TabBar : public QTabBar
{
    Q_OBJECT

signals:
    void newTab();
    void cloneTab(int index);
    void closeTab(int index);
    void closeOtherTabs(int index);
    void reloadTab(int index);
    void reloadAllTabs();
    void loadUrl(const QUrl &url, TabWidget::OpenUrlIn tab);
    // CONT02: reopen the tab at index bound to another container —
    // TabWidget::reopenTabInContainer performs the swap.
    void reopenInContainer(int index, const QString &containerId);
    // SLEEP01: the tab context menu's Sleep/Wake entries.
    void sleepTab(int index);
    void wakeTab(int index);

public:
    TabBar(QWidget *parent = nullptr);

    bool showTabBarWhenOneTab() const;
    void setShowTabBarWhenOneTab(bool enabled);
    QAction *viewTabBarAction() const;
    QTabBar::ButtonPosition freeSide();

    // UIP02: per-tab close buttons drawn by the bar itself — visible
    // only on the current tab and the hovered tab, like every modern
    // browser, instead of Qt's always-visible close indicator.
    bool perTabCloseButtons() const;
    void setPerTabCloseButtons(bool enabled);

    // POL02: the hover-preview card, lazily created on the first
    // dwell — exposed for the smoke test.
    TabPreview *tabPreview() const;

    // TABS02: the drag-resizable width of a Left/Right tab strip
    // (tabs/verticalTabWidth, clamped 120-400, default 180).
    // reloadVerticalTabWidth() re-reads the persisted value — the
    // settings-save path calls it so a drag on one window (or a
    // later edit of the key) lands on every window.
    int verticalTabWidth() const;
    void setVerticalTabWidth(int width);
    void reloadVerticalTabWidth();

    // CONT06: the level-1 container-header band painted along the
    // strip's top edge when the TabWidget's two-level mode has more
    // than one container owning tabs.  The band is real height on the
    // widget (sizeHint grows); tabs stay laid out below it in "strip
    // coordinates" — every mouse handler maps physical event pos to
    // stripPos() before tabAt()/tabRect() math.
    int containerStripHeight() const;
    int containerHeaderAt(const QPoint &physicalPos) const;
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void leaveEvent(QEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void initStyleOption(QStyleOptionTab *option, int tabIndex) const override;
    QSize tabSizeHint(int index) const override;
    QSize minimumTabSizeHint(int index) const override;
    void tabLayoutChange() override;
    void tabInserted(int position) override;
    void tabRemoved(int position) override;

private slots:
    void selectTabAction();
    void cloneTab();
    void closeTab();
    void closeOtherTabs();
    void reloadTab();
    void contextMenuRequested(const QPoint &position);
    void updateViewToolBarAction();
    void viewTabBar();

private:
    void updateVisibility();
    QTabBar::ButtonPosition closeButtonSide() const;
    void installCloseButton(int index);
    void updateHoveredTab(const QPoint &pos);
    void updateCloseButtonVisibility();
    // TABS04: the pin flag lives on the TabWidget — the bar only reads
    // it for rendering and the close protections.
    bool isTabPinned(int index) const;
    // POL02: hover-preview plumbing — the dwell timer arms on a fresh
    // hover, retargets instantly once a card is up, and the card drops
    // whenever the pointer leaves, presses, or the strip mutates.
    void updatePreviewDwell();
    void showTabPreview(int index);
    void hideTabPreview();
    // CONT02: the container chip — resolves the registry entry behind
    // the tab at index (empty id when the tab is in the default
    // container), the font the chip text uses, and the extra size the
    // chip needs in this bar's shape.
    ContainerManager::Container containerForTab(int index) const;
    QFont containerChipFont() const;
    QSize containerChipSize(int index) const;
    // TABGRP01: group chip helpers — the tab's group id, whether the
    // tab opens a contiguous run of its group (the pill anchors there),
    // and the group label shown on a collapsed group's chip.
    QString groupIdForTab(int index) const;
    bool isFirstInGroupRun(int index) const;
    QString groupChipLabel(int index) const;
    // TABS02: vertical-strip helpers — the resize grip on the
    // strip's inner edge, the row-anchored side-button layout (Qt
    // positions them transposed for West/East shapes), and hiding
    // Qt's private rotated moving-tab pixmap during a move drag.
    bool inResizeGrip(const QPoint &pos) const;
    void layoutRowTabButtons();
    void hideNativeMovingTab();
    // CONT06: level-1 band helpers — header geometry and painting
    // live in physical widget space (the top containerStripHeight()
    // pixels); stripPos() maps a physical event point into the
    // tabRect() space below the band.
    QPoint stripPos(const QPoint &physicalPos) const;
    QRect containerHeaderRect(int headerIndex) const;
    QString containerHeaderId(int headerIndex) const;
    void paintContainerStrip(QPainter *painter);
    void shiftChildrenBelowStrip();
    void updateAccessibleStrip();
    friend class TabWidget;

    QPoint m_dragStartPos;
    // TABGRP01 drop-stacking state: while the left button is held,
    // tabMoved tracks the dragged tab's index so mouseReleaseEvent can
    // tell a middle-of-tab drop (stack into a group) from a reorder.
    bool m_dragTracking;
    int m_draggedIndex;
    QAction *m_viewTabBarAction;
    bool m_showTabBarWhenOneTab;
    bool m_perTabCloseButtons;
    int m_hoveredTab;
    // POL02: lazily-created hover card + its dwell timer.
    TabPreview *m_preview;
    QTimer *m_previewTimer;
    // TABS02: vertical-strip state — the persisted strip width, the
    // active edge-drag resize, and the tracked press/move-drag used
    // to float the dragged row horizontally in paintEvent.
    int m_verticalTabWidth;
    bool m_resizing;
    int m_resizeStartX;
    int m_resizeStartWidth;
    int m_pressedTabIndex;
    bool m_moveDragActive;
    bool m_moveDragVertical;
    int m_moveDragMouseY;
    int m_moveDragGrabOffset;
    // CONT06: level-1 band state — the hovered header, the header a
    // press started on (click selects, drag to another header
    // reorders), the header a dragged tab is hovering (rebind
    // target), the keyboard-focused header, and the horizontal
    // move-drag float used when the band is up.
    int m_bandHoverHeader = -1;
    int m_bandPressedHeader = -1;
    int m_bandDropHeader = -1;
    int m_bandFocusHeader = -1;
    int m_moveDragMouseX = 0;
    int m_moveDragGrabOffsetX = 0;
};


#include <qshortcut.h>

/*
     Shortcut to switch directly to a tab by index
 */
class TabShortcut : public QShortcut
{
    Q_OBJECT

public:
    int tab();
    TabShortcut(int tab, const QKeySequence &key, QWidget *parent);

private:
    int m_tab;
};

#endif // TABBAR_H

