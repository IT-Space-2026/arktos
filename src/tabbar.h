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

#include <qpointer.h>

#include "containermanager.h"
#include "tabwidget.h"

class QLineEdit;
class QMimeData;
class QTimer;
class QPainter;
class QToolButton;
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
    // MEM08: the tab context menu's memory flush — see
    // TabWidget::freeTabMemory for the current-tab discard+reload.
    void freeTabMemory(int index);
    void freeBackgroundTabsMemory();

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
    // (tabs/verticalTabWidth, clamped 120-400, default 180; a stored
    // past default — TABS10's compact 140 — migrates since it means
    // the grip was never dragged to a pick — TABS16).
    // reloadVerticalTabWidth() re-reads the persisted value — the
    // settings-save path calls it so a drag on one window (or a
    // later edit of the key) lands on every window.
    int verticalTabWidth() const;
    void setVerticalTabWidth(int width);
    void reloadVerticalTabWidth();

    // RSZ01: horizontal-strip tab width bounds from the Tab Settings
    // page — tabs/tabMinWidth / tabs/tabMaxWidth in px, 0 = automatic
    // (Qt's squeeze floor / the built-in 18-em cap).
    // reloadTabWidths() re-reads both keys on the settings-save
    // live-apply path.
    int tabMinWidth() const;
    int tabMaxWidth() const;
    void setTabWidths(int minimum, int maximum);
    void reloadTabWidths();

    // RSZ02: the drag-resizable height of a Top/Bottom tab strip
    // (tabs/horizontalTabHeight, clamped 20-64, 0 = Automatic —
    // the default, letting TABS07's metrics-bound sizing drive).
    int tabStripHeight() const;
    void setTabStripHeight(int height);
    void reloadTabStripHeight();

    // TPREV01: tabs/tabHoverPreviews (default on) gates the hover
    // card.  reloadTabHoverPreviews() re-reads the key so the
    // settings-save live-apply reaches every window's bar.
    bool tabPreviewsEnabled() const;
    void setTabPreviewsEnabled(bool enabled);
    void reloadTabHoverPreviews();

    // TABS09: tabs/iconOnlyTabs — a user-toggleable icon-only view
    // mode.  Every tab renders at the compact favicon tile geometry
    // (icon only, the title stays on the tooltip); a strip-level view
    // mode, so collapsed group chips keep their label and nothing
    // on the tabs themselves changes.  iconOnlyTabsAction() is the
    // shared checkable toggle (tab context menu + View menu); the
    // write fans out to every window's bar and reloadIconOnlyTabs()
    // is the loadSettings fan-out's re-read.
    bool iconOnlyTabs() const;
    void setIconOnlyTabs(bool enabled);
    void reloadIconOnlyTabs();
    QAction *iconOnlyTabsAction() const;

    // TABS12: the inline rename editor — a QLineEdit laid over the
    // tab's rect.  Double-click on a row or the context menu's
    // "Rename Tab..." opens it; Enter/focus-out commits through
    // TabWidget::setTabTitleOverride (an empty commit clears the
    // override), Esc cancels.
    void editTabTitle(int index);

    // TABS15: the strip's own '+' button — a real widget docked
    // right after the last tab in every orientation (the old corner
    // widget never laid out on vertical rails and read as far-edge
    // chrome on horizontal ones).  Clicking it emits newTab(), the
    // same signal a double-click on the empty strip fires.
    QToolButton *newTabButton() const;

    // TABS13: the live in-process tab-drag session — the (bar, page)
    // pair behind an application/x-arora-tab payload, plus whether a
    // drop already consumed it.  Static because a drag's endpoints
    // are two different bars; the mime token is validated against
    // this pair so a stale or foreign payload can never move a tab.
    // Exposed for the autotests' synthetic drop events.
    static void beginTabDragSession(TabBar *source, QWidget *page);
    static void endTabDragSession();

    // TABGRP02: the tab a live in-bar drag would drop-stack into —
    // the one whose middle band sits under the cursor, or a
    // collapsed group's chip (whole rect).  -1 while no drop-stack
    // target is hovered; paintEvent tints it as the merge preview.
    // Exposed for the autotests' affordance legs.
    int groupDropIndex() const { return m_groupDropIndex; }

#ifdef AUTOTESTS
    // Test seam: drive a synthesized drop through dropEvent() — a
    // plain QApplication::sendEvent never reaches it because QWidget
    // ignores non-spontaneous Drop events.
    void testDropEvent(QDropEvent *event) { dropEvent(event); }
#endif

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
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
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
    void toggleIconOnlyMode();

private:
    void updateVisibility();
    // TABS12: one editor at a time — finishTabTitleEdit() commits or
    // drops it; m_editPage carries the tab's page widget so a mid-edit
    // strip shuffle still lands the rename on the right tab.
    void finishTabTitleEdit(bool commit);
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
    // chip needs in this bar's shape.  TABS06: the chip lives inside
    // the strip's thickness — the reserve is width-only (accent edge
    // + name pill borrowed from the title's trailing width), never
    // height.
    ContainerManager::Container containerForTab(int index) const;
    QFont containerChipFont() const;
    QSize containerChipSize(int index) const;
    int containerPillReserve(int index) const;
    // TABGRP01: group chip helpers — the tab's group id, whether the
    // tab opens a contiguous run of its group (the pill anchors there),
    // and the group label shown on a collapsed group's chip.
    QString groupIdForTab(int index) const;
    bool isFirstInGroupRun(int index) const;
    QString groupChipLabel(int index) const;
    // TABS02/RSZ02: strip helpers — the resize grip on the strip's
    // content-facing edge (inner side for vertical strips, top/bottom
    // edge for horizontal ones), its painted affordance, and the
    // row-anchored side-button layout (Qt positions them transposed
    // for West/East shapes), plus hiding Qt's private rotated
    // moving-tab pixmap during a move drag.
    bool inResizeGrip(const QPoint &pos) const;
    void paintResizeGrip(QPainter *painter);
    void persistStripSize();
    QRect rowLabelRect(const QRect &rowRect, int index) const;
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
    // TABS15: places the '+' widget right after the last tab —
    // trailing the row on horizontal strips, a full-width appended
    // row on vertical ones, parked inside Qt's scroll buttons when
    // the strip overflows so it never disappears.
    void layoutNewTabButton();
    // TABS13: the insert slot a strip drop maps to (past the hovered
    // tab's midpoint targets the far side; over the void it appends),
    // and the payload decode — it only yields a bar+page that match
    // the live drag session.
    int insertIndexForDrop(const QPoint &stripPos) const;
    static bool decodeAroraTabDrag(const QMimeData *mimeData,
                                   TabBar *&source, QWidget *&page);
    friend class TabWidget;

    QPoint m_dragStartPos;
    // TABGRP01 drop-stacking state: while the left button is held,
    // tabMoved tracks the dragged tab's index so mouseReleaseEvent can
    // tell a middle-of-tab drop (stack into a group) from a reorder.
    bool m_dragTracking;
    int m_draggedIndex;
    // TABGRP02: the drop-to-group hover target — see groupDropIndex().
    int m_groupDropIndex = -1;
    QAction *m_viewTabBarAction;
    QAction *m_iconOnlyTabsAction;
    bool m_showTabBarWhenOneTab;
    bool m_perTabCloseButtons;
    // TABS09: the persisted icon-only view mode — see the accessors.
    bool m_iconOnlyTabs;
    int m_hoveredTab;
    // POL02: lazily-created hover card + its dwell timer.
    TabPreview *m_preview;
    QTimer *m_previewTimer;
    // TPREV01: the Appearance-page opt-out — off means the dwell
    // timer never arms and an up card drops immediately.
    bool m_tabPreviewsEnabled;
    // TABS02: vertical-strip state — the persisted strip width, the
    // active edge-drag resize, and the tracked press/move-drag used
    // to float the dragged row horizontally in paintEvent.
    int m_verticalTabWidth;
    bool m_resizing;
    int m_resizeStartX;
    int m_resizeStartY;
    int m_resizeStartWidth;
    // RSZ02: horizontal-strip resize state — the persisted height
    // (0 = Automatic) and the value the drag started from; plus the
    // hover flag that arms the grip's painted affordance for both
    // orientations.
    int m_resizeStartHeight;
    int m_tabStripHeight;
    bool m_gripHovered;
    int m_pressedTabIndex;
    bool m_moveDragActive;
    bool m_moveDragVertical;
    int m_moveDragMouseY;
    int m_moveDragGrabOffset;
    // RSZ01: horizontal-strip width bounds (0 = automatic).
    int m_tabMinWidth;
    int m_tabMaxWidth;
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
    // TABS12: the live rename editor + the page widget it renames.
    QPointer<QLineEdit> m_titleEditor;
    QPointer<QWidget> m_editPage;
    // TABS15: the in-strip new-tab button — see layoutNewTabButton().
    QToolButton *m_newTabButton;
    // TABS18: the scroll chrome's band while the '+' is hosted in it
    // — the re-laid buttons no longer span it, so the rect is
    // remembered to keep the reflow stable.
    QRect m_newTabChromeBand;
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

