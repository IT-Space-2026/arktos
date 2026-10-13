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

#include "tabbar.h"

#include "aroraicon.h"
#include "browserapplication.h"
#include "containermanager.h"
#include "energysaver.h"
#include "safetext.h"
#include "tabpreview.h"
#include "tabwidget.h"
#include "webview.h"

#include <qaction.h>
#include <qapplication.h>
#include <qclipboard.h>
#include <qcursor.h>
#include <qdrag.h>
#include <qevent.h>
#include <qfontmetrics.h>
#include <qinputdialog.h>
#include <qlineedit.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <qpainter.h>
#include <qsettings.h>
#include <qstyle.h>
#include <qstyleoption.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qurl.h>

#include <qdebug.h>

// TABS16: the Left/Right strip's default width — favicon + close
// button + room for ~18-22 title chars.  TABS10's compact 140
// clipped typical titles at ~10-12 chars, so the default is back
// at the roomier pre-TABS10 180.  A stored past default means the
// grip was never dragged to a pick (a drag persists its own width),
// so reloadVerticalTabWidth() migrates it forward; any other stored
// value is a deliberate pick and stays.  The pre-TABS10 180 needs
// no sentinel — it IS the default again.
static const int defaultVerticalTabWidth = 180;
static const int compactVerticalTabWidth = 140;

// Defined further down — the TABGRP01 drop-zone check in
// mouseReleaseEvent needs it too.
static bool verticalTabShape(QTabBar::Shape shape);
// CONT06: builds a copy of a mouse event whose position() is shifted
// into strip space (below the level-1 band) for the base-class
// handlers.
static QMouseEvent shiftedMouseEvent(const QMouseEvent *event,
                                     const QPointF &delta);

// TABS13: the live in-process tab drag — the bar the drag left and
// the page widget being moved.  The mime token is only trusted when
// it names this pair, so stale or foreign application/x-arora-tab
// data can never reach a tab.
static QPointer<TabBar> s_tabDragSource;
static QPointer<QWidget> s_tabDragPage;
// Set by a dropEvent that consumed the tab — merge into another
// strip, a reorder back on this one, a refused header drop.  After
// exec() returns, an unhandled drop (released outside every tab bar)
// means "tear off into a new window".
static bool s_tabDropHandled = false;

void TabBar::beginTabDragSession(TabBar *source, QWidget *page)
{
    s_tabDragSource = source;
    s_tabDragPage = page;
    s_tabDropHandled = false;
}

void TabBar::endTabDragSession()
{
    s_tabDragSource.clear();
    s_tabDragPage.clear();
    s_tabDropHandled = false;
}

// The mime payload is "<source bar>:<page widget>" — it only yields a
// pair when it matches the live session, which foreign data can never
// name.
bool TabBar::decodeAroraTabDrag(const QMimeData *mimeData,
                                TabBar *&source, QWidget *&page)
{
    source = nullptr;
    page = nullptr;
    if (!mimeData || !mimeData->hasFormat(aroraTabMimeType()))
        return false;
    const QList<QByteArray> parts =
        mimeData->data(aroraTabMimeType()).split(':');
    if (parts.size() != 2)
        return false;
    bool okBar = false, okPage = false;
    TabBar *bar = reinterpret_cast<TabBar *>(
        parts.at(0).toULongLong(&okBar));
    QWidget *candidate = reinterpret_cast<QWidget *>(
        parts.at(1).toULongLong(&okPage));
    if (!okBar || !okPage || !bar || !candidate)
        return false;
    if (s_tabDragSource != bar || s_tabDragPage != candidate)
        return false;
    source = bar;
    page = candidate;
    return true;
}

TabShortcut::TabShortcut(int tab, const QKeySequence &key, QWidget *parent)
    : QShortcut(key, parent)
    , m_tab(tab)
{
}

int TabShortcut::tab()
{
    return m_tab;
}

TabBar::TabBar(QWidget *parent)
    : QTabBar(parent)
    , m_dragTracking(false)
    , m_draggedIndex(-1)
    , m_viewTabBarAction(nullptr)
    , m_iconOnlyTabsAction(nullptr)
    , m_showTabBarWhenOneTab(true)
    , m_perTabCloseButtons(false)
    , m_iconOnlyTabs(false)
    , m_hoveredTab(-1)
    , m_preview(nullptr)
    , m_previewTimer(nullptr)
    , m_tabPreviewsEnabled(true)
    , m_verticalTabWidth(defaultVerticalTabWidth)
    , m_resizing(false)
    , m_resizeStartX(0)
    , m_resizeStartY(0)
    , m_resizeStartWidth(0)
    , m_resizeStartHeight(0)
    , m_tabStripHeight(0)
    , m_gripHovered(false)
    , m_pressedTabIndex(-1)
    , m_moveDragActive(false)
    , m_moveDragVertical(false)
    , m_moveDragMouseY(0)
    , m_moveDragGrabOffset(0)
    , m_tabMinWidth(0)
    , m_tabMaxWidth(0)
    , m_newTabButton(nullptr)
{
    setContextMenuPolicy(Qt::CustomContextMenu);
    setAcceptDrops(true);
    setElideMode(Qt::ElideRight);
    setUsesScrollButtons(true);
    connect(this, &QTabBar::customContextMenuRequested,
            this, &TabBar::contextMenuRequested);

    QString alt = QLatin1String("Ctrl+%1");
    for (int i = 0; i < 9; ++i) {
        int key = i + 1;
        TabShortcut *tabShortCut = new TabShortcut(i, alt.arg(key), this);
        connect(tabShortCut, &QShortcut::activated, this, &TabBar::selectTabAction);
    }

    m_viewTabBarAction = new QAction(this);
    updateViewToolBarAction();
    connect(m_viewTabBarAction, &QAction::triggered,
            this, &TabBar::viewTabBar);

    // TABS09: the icon-only view-mode toggle — one checkable action
    // shared by the tab context menu and the window's View menu.
    m_iconOnlyTabsAction = new QAction(this);
    m_iconOnlyTabsAction->setCheckable(true);
    m_iconOnlyTabsAction->setText(tr("Icon-Only Tabs"));
    m_iconOnlyTabsAction->setToolTip(
        tr("Collapse every tab to its favicon — titles stay on the "
           "tooltips"));
    connect(m_iconOnlyTabsAction, &QAction::triggered,
            this, &TabBar::toggleIconOnlyMode);

    setMovable(true);

    // Per-tab close buttons reveal on hover — without tracking, move
    // events only arrive while a mouse button is held.
    setMouseTracking(true);
    connect(this, &QTabBar::currentChanged,
            this, [this](int) {
        updateCloseButtonVisibility();
        hideTabPreview();
    });

    // POL02: resting the pointer on a tab for a beat pops the hover
    // card.  The card dies when the window goes inactive — a tooltip-
    // style popup must never float over another application's window.
    m_previewTimer = new QTimer(this);
    m_previewTimer->setSingleShot(true);
    m_previewTimer->setInterval(500);
    connect(m_previewTimer, &QTimer::timeout, this, [this]() {
        showTabPreview(m_hoveredTab);
    });
    connect(qApp, &QApplication::applicationStateChanged,
            this, [this](Qt::ApplicationState state) {
        if (state != Qt::ApplicationActive)
            hideTabPreview();
    });
    // PREFUI05: Energy Saver disables the hover card — take down a
    // live popup and stop the dwell clock the moment it engages.
    connect(EnergySaver::instance(), &EnergySaver::activeChanged,
            this, [this](bool saving) {
        if (saving) {
            m_previewTimer->stop();
            hideTabPreview();
        }
    });

    // TABGRP01: while a left-button drag is live, remember where the
    // moved tab lands — on release a middle-of-tab drop stacks it into
    // the target's group instead of staying a plain reorder.
    connect(this, &QTabBar::tabMoved, this, [this](int from, int to) {
        if (m_dragTracking)
            m_draggedIndex = to;
        // TABS02: keep the tracked press index pointing at the same
        // tab while a move drag slides it across the strip.
        if (m_pressedTabIndex == from)
            m_pressedTabIndex = to;
        else if (m_pressedTabIndex > from && m_pressedTabIndex <= to)
            --m_pressedTabIndex;
        else if (m_pressedTabIndex < from && m_pressedTabIndex >= to)
            ++m_pressedTabIndex;
    });

    // TABS15: the Vivaldi-style '+' — a real button docked right
    // after the last tab in every orientation.  The corner widget it
    // replaces never laid out on vertical rails and read as far-edge
    // chrome on horizontal strips; a click emits the same newTab()
    // a double-click on the empty strip fires.
    m_newTabButton = new QToolButton(this);
    m_newTabButton->setAutoRaise(true);
    m_newTabButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    m_newTabButton->setToolTip(tr("New Tab"));
    m_newTabButton->setAccessibleName(tr("New Tab"));
    // The tor strip's violet chrome sits on the app's light palette
    // — the themed dark glyph would be mud, so tint it to the nav
    // cluster's flat #eee the way ICONS06 does, and give the
    // hover/pressed states the violet bar's own shades.
    if (BrowserApplication::isTorMode()) {
        m_newTabButton->setIcon(AroraIcon::getTinted(
            QLatin1String("tab-new"), QColor(0xee, 0xee, 0xee),
            QColor(0xb9, 0xae, 0xcf)));
        m_newTabButton->setStyleSheet(QLatin1String(
            "QToolButton { background: transparent; }"
            "QToolButton:hover { background: #5d4491; }"
            "QToolButton:pressed { background: #3a2a5c; }"));
    } else {
        m_newTabButton->setIcon(AroraIcon::get(QLatin1String("tab-new")));
    }
    connect(m_newTabButton, &QAbstractButton::clicked,
            this, &TabBar::newTab);

    // TABS02: persisted Left/Right strip width.
    reloadVerticalTabWidth();
    // RSZ02: persisted Top/Bottom strip height (0 = Automatic).
    reloadTabStripHeight();
    // TPREV01: the persisted hover-preview opt-out.
    reloadTabHoverPreviews();
    // RSZ01: the persisted horizontal-strip width bounds.
    reloadTabWidths();
    // TABS09: the persisted icon-only view mode.
    reloadIconOnlyTabs();
}

QToolButton *TabBar::newTabButton() const
{
    return m_newTabButton;
}

bool TabBar::showTabBarWhenOneTab() const
{
    return m_showTabBarWhenOneTab;
}

void TabBar::setShowTabBarWhenOneTab(bool enabled)
{
    m_showTabBarWhenOneTab = enabled;
    updateVisibility();
}

QAction *TabBar::viewTabBarAction() const
{
    return m_viewTabBarAction;
}

QTabBar::ButtonPosition TabBar::freeSide()
{
    QTabBar::ButtonPosition side = (QTabBar::ButtonPosition)style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this);
    side = (side == QTabBar::LeftSide) ? QTabBar::RightSide : QTabBar::LeftSide;
    return side;
}

// The side the style reserves for the close control — the opposite of
// freeSide(), which the loading-animation/favicon label occupies.
QTabBar::ButtonPosition TabBar::closeButtonSide() const
{
    return static_cast<QTabBar::ButtonPosition>(
        style()->styleHint(QStyle::SH_TabBar_CloseButtonPosition, nullptr, this));
}

bool TabBar::perTabCloseButtons() const
{
    return m_perTabCloseButtons;
}

void TabBar::setPerTabCloseButtons(bool enabled)
{
    if (m_perTabCloseButtons == enabled)
        return;
    m_perTabCloseButtons = enabled;
    // Qt's built-in indicator draws on every tab unconditionally; we
    // manage real buttons instead so they can appear only where they
    // should (current + hovered tab).
    QTabBar::setTabsClosable(false);
    for (int i = 0; i < count(); ++i) {
        if (enabled)
            installCloseButton(i);
        else
            setTabButton(i, closeButtonSide(), nullptr);
    }
    updateCloseButtonVisibility();
}

void TabBar::installCloseButton(int index)
{
    if (tabButton(index, closeButtonSide()))
        return;
    QToolButton *button = new QToolButton(this);
    button->setAutoRaise(true);
    button->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // Resolves through AroraIcon: the active icon theme first, the
    // bundled sets' dark variants when the palette is dark, and the
    // original closetab.png as the last-resort fallback.
    button->setIcon(AroraIcon::get(QLatin1String("window-close")));
    button->setIconSize(QSize(12, 12));
    // TABS07: an uncapped QToolButton hints ~19px tall, and QTabBar
    // stretches the strip to max(text, widget) + padding — the button
    // alone added ~5px to every tab.  Bound it to the text line so it
    // never contributes height; it stays centered in the tab rect.
    const int buttonExtent = qBound(12, fontMetrics().height(), 16);
    button->setFixedSize(buttonExtent, buttonExtent);
    button->setAccessibleName(tr("Close tab"));
    button->setToolTip(tr("Close Tab"));
    connect(button, &QToolButton::clicked, this, [this, button]() {
        // Resolve the index at click time — it shifts as tabs are
        // added, removed and dragged around.
        const QTabBar::ButtonPosition side = closeButtonSide();
        for (int i = 0; i < count(); ++i) {
            if (tabButton(i, side) == button) {
                emit closeTab(i);
                return;
            }
        }
    });
    setTabButton(index, closeButtonSide(), button);
}

// pos is a point in the bar's own coordinates (from the mouse event —
// QCursor::pos() does not track synthetic moves and lags real ones).
// A point sitting on a per-tab button (a child widget) still resolves
// to the owning tab, so the button stays shown under the cursor.
void TabBar::updateHoveredTab(const QPoint &pos)
{
    int hovered = -1;
    if (isVisible() && rect().contains(pos))
        hovered = tabAt(pos);
    if (hovered == m_hoveredTab)
        return;
    m_hoveredTab = hovered;
    updateCloseButtonVisibility();
    updatePreviewDwell();
}

bool TabBar::isTabPinned(int index) const
{
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    return tabWidget && tabWidget->isTabPinned(index);
}

void TabBar::updateCloseButtonVisibility()
{
    if (!m_perTabCloseButtons)
        return;
    const int current = currentIndex();
    const QTabBar::ButtonPosition side = closeButtonSide();
    for (int i = 0; i < count(); ++i) {
        // TABS04: a pinned tab has no close affordance at all —
        // unpinning is the way to make it closable again.
        if (QWidget *button = tabButton(i, side))
            button->setVisible(!isTabPinned(i)
                               && (i == current || i == m_hoveredTab));
    }
}

void TabBar::leaveEvent(QEvent *event)
{
    if (cursor().shape() == Qt::SizeHorCursor
        || cursor().shape() == Qt::SizeVerCursor)
        unsetCursor();
    if (m_gripHovered) {
        m_gripHovered = false;
        update();
    }
    if (m_hoveredTab != -1) {
        m_hoveredTab = -1;
        updateCloseButtonVisibility();
    }
    if (m_bandHoverHeader != -1 || m_bandDropHeader != -1) {
        m_bandHoverHeader = -1;
        m_bandDropHeader = -1;
        update();
    }
    m_previewTimer->stop();
    hideTabPreview();
    QTabBar::leaveEvent(event);
}

void TabBar::updateViewToolBarAction()
{
    bool show = showTabBarWhenOneTab();
    if (count() > 1)
        show = true;
    m_viewTabBarAction->setText(!show ? tr("Show Tab Bar") : tr("Hide Tab Bar"));
}

void TabBar::viewTabBar()
{
    setShowTabBarWhenOneTab(!showTabBarWhenOneTab());
    updateViewToolBarAction();
}

void TabBar::selectTabAction()
{
    int index = qobject_cast<TabShortcut*>(sender())->tab();
    setCurrentIndex(index);
}

void TabBar::contextMenuRequested(const QPoint &position)
{
    hideTabPreview();
    // CONT06: the header band has no context menu.
    if (containerStripHeight() > 0
        && position.y() < containerStripHeight())
        return;
    QMenu menu;
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget)
        return;

    menu.addAction(tabWidget->newTabAction());

    // TABS05: undo-close — 'Reopen' pops the newest record, the
    // History-menu submenu lists the whole stack.  The shortcut shown
    // here is display-only; the real binding lives on the TabWidget.
    QAction *reopenAction = menu.addAction(
        tr("Reopen Closed Tab"), tabWidget,
        [tabWidget]() { tabWidget->openLastTab(); });
    reopenAction->setShortcut(QKeySequence(
        Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_T));
    reopenAction->setEnabled(tabWidget->hasRecentlyClosedTabs());
    menu.addAction(tabWidget->recentlyClosedTabsAction());
    menu.addSeparator();

    int index = tabAt(stripPos(position));
    if (-1 != index) {
        QAction *action = menu.addAction(tr("Duplicate Tab"),
                                         this, QOverload<>::of(&TabBar::cloneTab));
        action->setData(index);

        // TABS04: pin/unpin — the toggle carries no default shortcut
        // (Ctrl+P et al. stay untouched).
        menu.addAction(tabWidget->isTabPinned(index)
                       ? tr("Unpin Tab") : tr("Pin Tab"),
                       this, [tabWidget, index]() {
            tabWidget->setTabPinned(index,
                                    !tabWidget->isTabPinned(index));
        });

        // TABS12: rename — the same inline editor double-click opens;
        // committing empty text clears the override and the live page
        // title returns.
        menu.addAction(tr("Re&name Tab..."), this,
                       [this, index]() { editTabTitle(index); });

        // CONT02: "Reopen in Container" moves the tab's page onto
        // another container's profile.  Off-the-record tabs hide the
        // submenu — a private tab (window-global or PTAB01's per-tab
        // kind) can never move to persistent storage, and tor has no
        // containers at all.
        if (!BrowserApplication::isPrivate()
            && !BrowserApplication::isTorMode()
            && !tabWidget->isTabPrivate(index)) {
            QMenu *containersMenu = menu.addMenu(tr("Reopen in Con&tainer"));
            const QString current = tabWidget->containerIdForTab(index);
            ContainerManager *manager = ContainerManager::instance();
            QAction *entry = containersMenu->addAction(tr("&No Container"),
                this, [this, index]() {
                emit reopenInContainer(index, QString());
            });
            entry->setCheckable(true);
            entry->setChecked(current.isEmpty());
            const QList<ContainerManager::Container> containers =
                manager->containers();
            for (const ContainerManager::Container &container : containers) {
                entry = containersMenu->addAction(
                    ContainerManager::colorIcon(container.color),
                    SafeText::menu(container.name));
                entry->setCheckable(true);
                entry->setChecked(container.id == current);
                const QString id = container.id;
                connect(entry, &QAction::triggered, this,
                        [this, index, id]() {
                    emit reopenInContainer(index, id);
                });
            }
            containersMenu->addSeparator();
            containersMenu->addAction(tr("New &Container..."),
                this, [this, index]() {
                const QString id = ContainerManager::instance()
                    ->createContainerInteractive(this);
                if (!id.isEmpty())
                    emit reopenInContainer(index, id);
            });
            containersMenu->addAction(tr("&Manage Containers..."),
                this, [tabWidget]() { tabWidget->manageContainers(); });

            // CONT04: "Always open this site in <container>" — a
            // host->container rule WebPage enforces at navigation
            // time.  Only meaningful for host-bearing web pages; a
            // container tab offers the rule as a checkbox on itself,
            // a default tab picks the container the site belongs to
            // (assigning also moves this tab so it lands in the right
            // context immediately).
            const WebView *tabView = tabWidget->webView(index);
            const QString tabScheme = tabView ? tabView->url().scheme()
                                              : QString();
            const QString host =
                (tabScheme == QLatin1String("http")
                 || tabScheme == QLatin1String("https"))
                    ? tabView->url().host() : QString();
            if (!host.isEmpty()) {
                const QString ruled = manager->containerIdForHost(host);
                if (!current.isEmpty()) {
                    menu.addSeparator();
                    QAction *always = menu.addAction(
                        tr("Always Open This Site in &This Container"));
                    always->setCheckable(true);
                    // Ruled into this container — or anywhere else —
                    // either way checking it claims the site for the
                    // current tab's container.
                    always->setChecked(ruled == current);
                    connect(always, &QAction::triggered, this,
                            [host, current](bool on) {
                        ContainerManager *rules =
                            ContainerManager::instance();
                        if (on)
                            rules->setSiteRule(host, current);
                        else
                            rules->removeSiteRule(host);
                    });
                } else if (!containers.isEmpty()) {
                    menu.addSeparator();
                    QMenu *alwaysMenu = menu.addMenu(
                        tr("Always Open Site in Con&tainer"));
                    for (const ContainerManager::Container &container
                         : containers) {
                        QAction *entry = alwaysMenu->addAction(
                            ContainerManager::colorIcon(container.color),
                            SafeText::menu(container.name));
                        entry->setCheckable(true);
                        entry->setChecked(ruled == container.id);
                        const QString id = container.id;
                        connect(entry, &QAction::triggered, this,
                                [this, index, host, id](bool on) {
                            ContainerManager *rules =
                                ContainerManager::instance();
                            if (on) {
                                rules->setSiteRule(host, id);
                                emit reopenInContainer(index, id);
                            } else {
                                rules->removeSiteRule(host);
                            }
                        });
                    }
                }
            }
        }

        // SLEEP01: Sleep/Wake — the engine discards the page while
        // the slot keeps its title and favicon.  Sleeping the visible
        // tab is impossible, so the entry disables on the current
        // index; Wake shows in its place on a suspended tab.
        const bool sleeping = tabWidget->isTabSleeping(index);
        QAction *sleepAction = menu.addAction(
            sleeping ? tr("Wake Tab") : tr("Sleep Tab"));
        sleepAction->setEnabled(sleeping
                                || index != tabWidget->currentIndex());
        connect(sleepAction, &QAction::triggered, this,
                [this, index, sleeping]() {
            if (sleeping)
                emit wakeTab(index);
            else
                emit sleepTab(index);
        });

        // TABGRP01: tab groups — an ungrouped tab gets "Add Tab to
        // Group" (new group or any existing one); a grouped tab gets
        // the group-level actions plus a way out.
        const QString groupId = tabWidget->tabGroupId(index);
        menu.addSeparator();
        if (groupId.isEmpty()) {
            QMenu *groupMenu = menu.addMenu(tr("Add Tab to &Group"));
            groupMenu->addAction(tr("&New Group"), this,
                                 [tabWidget, index]() {
                tabWidget->createTabGroup(index);
            });
            const QStringList existing = tabWidget->tabGroupIds();
            if (!existing.isEmpty()) {
                groupMenu->addSeparator();
                for (const QString &gid : existing) {
                    const QString name = tabWidget->tabGroupName(gid);
                    groupMenu->addAction(
                        ContainerManager::colorIcon(
                            tabWidget->tabGroupColor(gid)),
                        SafeText::menu(name.isEmpty()
                                       ? tr("Unnamed Group") : name),
                        this, [tabWidget, index, gid]() {
                        tabWidget->addTabToGroup(index, gid);
                    });
                }
            }
        } else {
            const QString name = tabWidget->tabGroupName(groupId);
            menu.addAction(tabWidget->tabGroupIsCollapsed(groupId)
                           ? tr("&Expand Group")
                           : tr("&Collapse Group"),
                           this, [tabWidget, groupId]() {
                const bool collapsed =
                    tabWidget->tabGroupIsCollapsed(groupId);
                tabWidget->setTabGroupCollapsed(groupId, !collapsed);
            });
            menu.addAction(tr("R&ename Group..."), this,
                           [this, tabWidget, groupId, name]() {
                bool ok = false;
                const QString entered = QInputDialog::getText(
                    this, tr("Rename Tab Group"), tr("Group &name:"),
                    QLineEdit::Normal, name, &ok);
                if (ok)
                    tabWidget->renameTabGroup(groupId, entered);
            });
            QMenu *colorMenu = menu.addMenu(tr("Group &Color"));
            const QList<QColor> palette =
                ContainerManager::defaultColors();
            static const char *const colorNames[] = {
                QT_TR_NOOP("Blue"), QT_TR_NOOP("Turquoise"),
                QT_TR_NOOP("Green"), QT_TR_NOOP("Yellow"),
                QT_TR_NOOP("Orange"), QT_TR_NOOP("Red"),
                QT_TR_NOOP("Pink"), QT_TR_NOOP("Purple")
            };
            const QColor current = tabWidget->tabGroupColor(groupId);
            for (int i = 0; i < palette.count(); ++i) {
                QAction *entry = colorMenu->addAction(
                    ContainerManager::colorIcon(palette.at(i)),
                    tr(colorNames[i % 8]));
                entry->setCheckable(true);
                entry->setChecked(palette.at(i) == current);
                const QColor color = palette.at(i);
                connect(entry, &QAction::triggered, this,
                        [tabWidget, groupId, color]() {
                    tabWidget->setTabGroupColor(groupId, color);
                });
            }
            menu.addSeparator();
            menu.addAction(tr("Remove Tab from Group"), this,
                           [tabWidget, index]() {
                tabWidget->removeTabFromGroup(index);
            });
            menu.addAction(tr("U&ngroup"), this,
                           [tabWidget, groupId]() {
                tabWidget->ungroupTabs(groupId);
            });
        }

        menu.addSeparator();

        action = menu.addAction(tr("&Close Tab"), QKeySequence::Close,
                                this, QOverload<>::of(&TabBar::closeTab));
        action->setData(index);

        action = menu.addAction(tr("Close &Other Tabs"),
                                this, QOverload<>::of(&TabBar::closeOtherTabs));
        action->setData(index);

        menu.addSeparator();

        action = menu.addAction(tr("Reload Tab"), QKeySequence::Refresh,
                                this, QOverload<>::of(&TabBar::reloadTab));
        action->setData(index);
    }
    menu.addAction(tr("Reload All Tabs"), this, &TabBar::reloadAllTabs);
    menu.addSeparator();
    menu.addAction(tabWidget->bookmarkTabsAction());
    // TABS09: the icon-only view toggle is a strip-level entry — it
    // is offered whether or not the click landed on a tab.
    menu.addSeparator();
    menu.addAction(m_iconOnlyTabsAction);
    menu.exec(QCursor::pos());
}

void TabBar::cloneTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        int index = action->data().toInt();
        emit cloneTab(index);
    }
}

void TabBar::closeTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        int index = action->data().toInt();
        emit closeTab(index);
    }
}

void TabBar::closeOtherTabs()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        int index = action->data().toInt();
        emit closeOtherTabs(index);
    }
}

void TabBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    m_previewTimer->stop();
    hideTabPreview();
    // RSZ02: double-clicking the strip's resize grip resets the
    // dragged dimension — the vertical width to its default, a
    // horizontal strip's height to Automatic.
    if (event->button() == Qt::LeftButton
        && inResizeGrip(stripPos(event->position().toPoint()))) {
        if (verticalTabShape(shape()))
            setVerticalTabWidth(defaultVerticalTabWidth);
        else
            setTabStripHeight(0);
        persistStripSize();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        const int index = tabAt(stripPos(event->position().toPoint()));
        // TABS12: double-click on a tab row opens the inline rename
        // editor; the empty strip keeps its new-tab shortcut.
        if (index >= 0) {
            editTabTitle(index);
            return;
        }
        // CONT06: double-clicking the header band is not "empty
        // strip" — a click there does not spawn a tab.
        if (containerStripHeight() <= 0
            || event->position().toPoint().y()
               >= containerStripHeight())
            emit newTab();
        return;
    }
    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::mouseReleaseEvent(QMouseEvent *event)
{
    const int band = containerStripHeight();
    const QPoint pos = event->position().toPoint();
    const bool inBand = band > 0 && pos.y() >= 0 && pos.y() < band;
    if (event->button() == Qt::LeftButton) {
        // CONT06: a release in the header band finishes one of two
        // gestures — a tab drag landing on a header rebinds the tab
        // to that container, a header press/drag activates or
        // reorders headers.
        if (inBand) {
            TabWidget *tabWidget =
                qobject_cast<TabWidget*>(parentWidget());
            const int header = containerHeaderAt(pos);
            const int dragged = m_draggedIndex >= 0
                ? m_draggedIndex : m_pressedTabIndex;
            if (m_dragTracking && dragged >= 0 && header >= 0
                && tabWidget) {
                m_dragTracking = false;
                m_draggedIndex = -1;
                m_bandDropHeader = -1;
                tabWidget->reopenTabInContainer(
                    dragged, tabWidget->containerHeaders().value(header));
            } else if (m_bandPressedHeader >= 0 && tabWidget) {
                if (header == m_bandPressedHeader) {
                    tabWidget->setActiveContainerHeader(
                        tabWidget->containerHeaders().value(header));
                } else if (header > 0 && m_bandPressedHeader > 0) {
                    // The default header stays pinned first — only
                    // container headers reorder.
                    tabWidget->moveContainerHeader(
                        m_bandPressedHeader, header);
                }
            }
            m_dragTracking = false;
            m_pressedTabIndex = -1;
            m_moveDragActive = false;
            m_bandPressedHeader = -1;
            m_bandDropHeader = -1;
            update();
            QMouseEvent shifted = shiftedMouseEvent(
                event, QPointF(0, -band));
            QTabBar::mouseReleaseEvent(&shifted);
            return;
        }
        if (m_bandPressedHeader >= 0) {
            m_bandPressedHeader = -1;
            update();
        }
        m_pressedTabIndex = -1;
        m_moveDragActive = false;
        // TABS02: end of an inner-edge width drag — persist the width
        // and fan it out to every live window's bar, the same
        // live-apply path TABS01's position uses.
        if (m_resizing) {
            m_resizing = false;
            if (cursor().shape() == Qt::SizeHorCursor
                || cursor().shape() == Qt::SizeVerCursor)
                unsetCursor();
            update();
            persistStripSize();
            return;
        }
    }
    if (event->button() == Qt::MiddleButton && !inBand) {
        int index = tabAt(stripPos(pos));
        // TABS04: middle-click on a pinned tab is ignored — Vivaldi's
        // semantics — rather than closing it or falling through to
        // the selection-paste path.
        if (index != -1) {
            if (!isTabPinned(index))
                emit closeTab(index);
            return;
        } else {
            // SEC02: a javascript: selection must not turn into script
            // execution in the new tab.
            QUrl url(QApplication::clipboard()->text(QClipboard::Selection));
            if (!url.isEmpty() && url.isValid() && !url.scheme().isEmpty()
                && WebView::isUrlAllowedOnUntrustedInput(url))
                emit loadUrl(url, TabWidget::NewTab);
        }
    }

    // TABGRP01: a dragged tab released over the middle of another tab
    // stacks into its group — Chrome's drop-to-group gesture.  The
    // middle quarter bands at the run's edges still mean "reorder to
    // this slot" so a drop on a tab's edge is never misread.
    if (event->button() == Qt::LeftButton && m_dragTracking) {
        const int dragged = m_draggedIndex;
        m_dragTracking = false;
        if (dragged >= 0) {
            const QPoint pos = stripPos(event->position().toPoint());
            const int over = tabAt(pos);
            if (over != -1 && over != dragged) {
                const QRect r = tabRect(over);
                const bool middle = verticalTabShape(shape())
                    ? (pos.y() > r.top() + r.height() / 4
                       && pos.y() < r.bottom() - r.height() / 4)
                    : (pos.x() > r.left() + r.width() / 4
                       && pos.x() < r.right() - r.width() / 4);
                if (middle) {
                    if (TabWidget *tabWidget =
                            qobject_cast<TabWidget*>(parentWidget()))
                        tabWidget->groupTabWith(dragged, over);
                }
            }
        }
    }

    if (band > 0) {
        QMouseEvent shifted = shiftedMouseEvent(event, QPointF(0, -band));
        QTabBar::mouseReleaseEvent(&shifted);
        return;
    }
    QTabBar::mouseReleaseEvent(event);
}

void TabBar::mousePressEvent(QMouseEvent *event)
{
    m_previewTimer->stop();
    hideTabPreview();
    const int band = containerStripHeight();
    const QPoint pos = event->position().toPoint();
    if (event->button() == Qt::LeftButton) {
        // CONT06: a press in the header band starts a header
        // click/drag — QTabBar never sees it, so it can neither
        // select a tab nor arm a move drag.
        if (band > 0 && pos.y() >= 0 && pos.y() < band) {
            m_bandPressedHeader = containerHeaderAt(pos);
            m_bandFocusHeader = -1;
            update();
            return;
        }
        const QPoint spos = stripPos(pos);
        // TABS02/RSZ02: a press in the strip's content-edge grip zone
        // starts a width (vertical strip) or height (horizontal
        // strip) drag, not a tab press — QTabBar never sees it, so
        // it can neither select the tab nor arm a move drag.
        if (inResizeGrip(spos)) {
            m_resizing = true;
            m_resizeStartX = event->globalPosition().toPoint().x();
            m_resizeStartY = event->globalPosition().toPoint().y();
            m_resizeStartWidth = m_verticalTabWidth;
            // A drag starting from Automatic measures against the
            // strip's current rendered height, so the first pixel
            // of movement continues from what the user sees instead
            // of snapping to the floor.
            m_resizeStartHeight = m_tabStripHeight > 0
                ? m_tabStripHeight
                : height() - containerStripHeight();
            setCursor(verticalTabShape(shape())
                      ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            update();
            return;
        }
        m_dragStartPos = spos;
        // TABS02: track the pressed tab so paintEvent can float it
        // horizontally during a move drag.
        m_pressedTabIndex = tabAt(m_dragStartPos);
        m_moveDragGrabOffset = m_pressedTabIndex >= 0
            ? m_dragStartPos.y() - tabRect(m_pressedTabIndex).top()
            : 0;
        m_moveDragGrabOffsetX = m_pressedTabIndex >= 0
            ? m_dragStartPos.x() - tabRect(m_pressedTabIndex).left()
            : 0;
        // TABGRP01: a press starts a potential drag — arm the
        // tabMoved tracker so the release can see where the tab went.
        m_dragTracking = true;
        m_draggedIndex = -1;
        // TABGRP01: clicking a collapsed group's chip expands it.
        TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
        const int index = tabAt(m_dragStartPos);
        if (tabWidget && index >= 0 && tabWidget->isTabGroupChip(index))
            tabWidget->setTabGroupCollapsed(tabWidget->tabGroupId(index),
                                            false);
    }
    if (band > 0) {
        QMouseEvent shifted = shiftedMouseEvent(event, QPointF(0, -band));
        QTabBar::mousePressEvent(&shifted);
        return;
    }
    QTabBar::mousePressEvent(event);
}

// True when the tab bar's tabs run vertically (West/East tab
// positions on the enclosing QTabWidget pick these shapes).
static bool verticalTabShape(QTabBar::Shape shape)
{
    switch (shape) {
    case QTabBar::RoundedWest:
    case QTabBar::TriangularWest:
    case QTabBar::RoundedEast:
    case QTabBar::TriangularEast:
        return true;
    default:
        return false;
    }
}

void TabBar::mouseMoveEvent(QMouseEvent *event)
{
    const int band = containerStripHeight();
    const QPoint pos = event->position().toPoint();
    const QPoint spos = pos - QPoint(0, band);
    const bool inBand = band > 0 && pos.y() >= 0 && pos.y() < band;
    // TABS02/RSZ02: an active edge drag eats the move — the strip
    // resizes instead of the press acting on a tab.
    if (m_resizing) {
        if (verticalTabShape(shape())) {
            const int dx = event->globalPosition().toPoint().x()
                - m_resizeStartX;
            // West bars widen rightwards (their grip is the right
            // edge), East bars widen leftwards.
            const bool east = (shape() == QTabBar::RoundedEast
                               || shape() == QTabBar::TriangularEast);
            setVerticalTabWidth(m_resizeStartWidth + (east ? -dx : dx));
        } else {
            const int dy = event->globalPosition().toPoint().y()
                - m_resizeStartY;
            // Top bars grow downwards, bottom bars upwards.  The
            // drag clamps inside the 20-64 band — Automatic is only
            // reachable through the double-click reset.
            const bool south = (shape() == QTabBar::RoundedSouth
                                || shape() == QTabBar::TriangularSouth);
            setTabStripHeight(qBound(20,
                m_resizeStartHeight + (south ? -dy : dy), 64));
        }
        return;
    }
    // TABS02/RSZ02: the grip zone advertises itself with a resize
    // cursor and a painted hover band.
    if (!(event->buttons() & Qt::LeftButton)) {
        const bool overGrip = inResizeGrip(spos);
        if (overGrip) {
            setCursor(verticalTabShape(shape())
                      ? Qt::SizeHorCursor : Qt::SizeVerCursor);
        } else if (cursor().shape() == Qt::SizeHorCursor
                   || cursor().shape() == Qt::SizeVerCursor) {
            unsetCursor();
        }
        if (overGrip != m_gripHovered) {
            m_gripHovered = overGrip;
            update();
        }
    }

    // CONT06: track header hover (plain moves and header drags) and
    // the rebind target a dragged tab is hovering over.
    if (band > 0) {
        const int hover = inBand ? containerHeaderAt(pos) : -1;
        const int drop = (inBand && m_pressedTabIndex >= 0
                          && (event->buttons() & Qt::LeftButton))
            ? hover : -1;
        if (hover != m_bandHoverHeader || drop != m_bandDropHeader) {
            m_bandHoverHeader = hover;
            m_bandDropHeader = drop;
            update();
        }
    }

    updateHoveredTab(spos);
    if (event->buttons() == Qt::LeftButton) {
        const QPoint diff = spos - m_dragStartPos;
        // "Tear the tab off" = drag away from the bar, perpendicular
        // to the tab strip; the direction depends on which edge the
        // bar sits on (drag up for North, right for East, ...).  A
        // drag that lands on the header band is a rebind gesture, not
        // a tear-off — the band suppresses it until the pointer
        // actually leaves the widget.
        const bool vertical = verticalTabShape(shape());
        const int along = vertical ? diff.y() : diff.x();
        bool away;
        switch (shape()) {
        case QTabBar::RoundedSouth:
        case QTabBar::TriangularSouth:
            away = diff.y() > 10;
            break;
        case QTabBar::RoundedWest:
        case QTabBar::TriangularWest:
            away = diff.x() < -10;
            break;
        case QTabBar::RoundedEast:
        case QTabBar::TriangularEast:
            away = diff.x() > 10;
            break;
        default:
            away = diff.y() < -10;
            break;
        }
        if (diff.manhattanLength() > QApplication::startDragDistance()
            && along < 3 && along > -3
            && away && !inBand) {
            QDrag *drag = new QDrag(this);
            QMimeData *mimeData = new QMimeData;
            QList<QUrl> urls;
            int index = tabAt(spos);
            QUrl url = tabData(index).toUrl();
            urls.append(url);
            mimeData->setUrls(urls);
            mimeData->setText(tabText(index));
            mimeData->setData(QLatin1String("action"), "tab-reordering");
            // TABS13: tag the drag as a real tab — the page widget is
            // its identity.  The url/text payload stays so dropping a
            // tab into a foreign app still hands it the link.
            TabWidget *tabWidget =
                qobject_cast<TabWidget*>(parentWidget());
            QPointer<QWidget> page = (tabWidget && index >= 0)
                ? tabWidget->widget(index) : nullptr;
            if (page) {
                mimeData->setData(aroraTabMimeType(),
                    QByteArray::number(quintptr(this)) + ':'
                        + QByteArray::number(quintptr(page.data())));
                beginTabDragSession(this, page);
            }
            drag->setMimeData(mimeData);
            // TABGRP01: a tear-off leaves the bar — the in-bar
            // drop-stack tracking stops here.
            m_dragTracking = false;
            // TABS02: and so does the move-drag tracking — the bar is
            // about to spend the drag inside QDrag::exec().
            m_moveDragActive = false;
            m_pressedTabIndex = -1;
            // The drop can merge the window's last tab into another
            // strip — closing this window mid-drag — so guard this
            // bar across exec()'s nested event loop.
            QPointer<TabBar> guard(this);
            const Qt::DropAction action = drag->exec();
            // TABS13: released where nothing accepted the drop —
            // over the desktop, a window's page body, foreign chrome —
            // the tab tears off into a new window.  A consumed drop
            // (a merge, a reorder back here, a foreign url drop) or a
            // dead source bar leaves it put.
            if (guard && page && !s_tabDropHandled
                && action == Qt::IgnoreAction && tabWidget)
                tabWidget->tearTabToNewWindow(tabWidget->indexOf(page));
            endTabDragSession();
        }
    }
    if (band > 0) {
        QMouseEvent shifted = shiftedMouseEvent(event, QPointF(0, -band));
        QTabBar::mouseMoveEvent(&shifted);
    } else {
        QTabBar::mouseMoveEvent(event);
    }
    // TABS02: mirror Qt's move-drag lifecycle — past the drag
    // threshold Qt floats its own rotated tab pixmap; hide that
    // widget and let paintEvent float a horizontal row instead.
    if (m_pressedTabIndex >= 0
        && event->buttons() & Qt::LeftButton) {
        const QPoint diff = spos - m_dragStartPos;
        if (!m_moveDragActive
            && diff.manhattanLength() > QApplication::startDragDistance())
            m_moveDragActive = true;
        if (m_moveDragActive) {
            m_moveDragVertical = verticalTabShape(shape())
                && qAbs(diff.y()) > qAbs(diff.x());
            m_moveDragMouseY = spos.y();
            m_moveDragMouseX = spos.x();
            hideNativeMovingTab();
            update();
        }
    }
}

void TabBar::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (mimeData->hasUrls() || mimeData->hasText())
        event->acceptProposedAction();

    QTabBar::dragEnterEvent(event);
}

// External URL/text drags get the same header hover highlight the
// internal tab-drag paints — the drop itself is dropEvent's.
void TabBar::dragMoveEvent(QDragMoveEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    const int header =
        (mimeData->hasUrls() || mimeData->hasText())
        ? containerHeaderAt(event->position().toPoint()) : -1;
    if (header != m_bandDropHeader) {
        m_bandDropHeader = header;
        update();
    }
    QTabBar::dragMoveEvent(event);
}

void TabBar::dragLeaveEvent(QDragLeaveEvent *event)
{
    if (m_bandDropHeader >= 0) {
        m_bandDropHeader = -1;
        update();
    }
    QTabBar::dragLeaveEvent(event);
}

void TabBar::dropEvent(QDropEvent *event)
{
    if (m_bandDropHeader >= 0) {
        m_bandDropHeader = -1;
        update();
    }
    const QMimeData *mimeData = event->mimeData();

    // TABS13: a real in-process tab drag takes precedence over the
    // legacy url payload — the page itself moves (or reorders back on
    // its own strip), never loads onto the tab under the cursor.
    // Only a payload matching the live drag session gets here; stale
    // or foreign x-arora-tab data falls through to the url path.
    TabBar *sourceBar = nullptr;
    QWidget *page = nullptr;
    if (decodeAroraTabDrag(mimeData, sourceBar, page)) {
        TabWidget *source = sourceBar
            ? qobject_cast<TabWidget*>(sourceBar->parentWidget())
            : nullptr;
        TabWidget *target = qobject_cast<TabWidget*>(parentWidget());
        if (!source || !target) {
            event->ignore();
            return;
        }
        // Consumed either way — an unhandled drop would tear the tab
        // off into a new window once exec() returns.
        s_tabDropHandled = true;
        event->acceptProposedAction();
        const int band = containerStripHeight();
        const QPoint pos = event->position().toPoint();
        const bool onBand = band > 0 && pos.y() >= 0 && pos.y() < band;
        const int from = source->indexOf(page);
        if (from < 0)
            return;   // the tab vanished mid-drag — nothing to move
        if (source == target) {
            // Back on its own strip: today's reorder (over the header
            // band the tab simply stays put).
            if (!onBand) {
                int to = insertIndexForDrop(stripPos(pos));
                if (to > from)
                    --to;
                if (to != from)
                    moveTab(from, to);
            }
            return;
        }
        // A different window's strip — the real tab moves at the drop
        // slot.  Over the header band it lands at the end of the
        // strip; a live page's container binding can't be rebound the
        // way a bare url can.
        const int to = onBand ? target->count()
                              : insertIndexForDrop(stripPos(pos));
        source->moveTabTo(from, target, to);
        return;
    }

    QUrl url;
    if (mimeData->hasUrls())
        url = mimeData->urls().at(0);
    else if (mimeData->hasText())
        url = QUrl::fromEncoded(mimeData->text().toUtf8());

    if (!url.isEmpty() && url.isValid()) {
        event->acceptProposedAction();
        const int band = containerStripHeight();
        const QPoint pos = event->position().toPoint();
        // CONT06: a drop on a level-1 header opens the url in that
        // container; a drop on the row below keeps today's
        // load-onto-tab / new-tab behavior.
        if (band > 0 && pos.y() >= 0 && pos.y() < band) {
            if (TabWidget *tabWidget =
                    qobject_cast<TabWidget*>(parentWidget())) {
                const int header = containerHeaderAt(pos);
                if (header >= 0) {
                    tabWidget->loadUrlInContainer(
                        url, tabWidget->containerHeaders().value(header));
                    return;
                }
            }
        }
        int index = tabAt(stripPos(pos));
        if (-1 != index) {
            setCurrentIndex(index);
            emit loadUrl(url, TabWidget::CurrentTab);
        } else {
            emit loadUrl(url, TabWidget::NewSelectedTab);
        }
    }

    QTabBar::dropEvent(event);
}

// TABS13: the strip slot a drop maps to — insert before the hovered
// tab, or behind it when the pointer sits past the midpoint; over the
// void after the last tab it appends.
int TabBar::insertIndexForDrop(const QPoint &pos) const
{
    const int over = tabAt(pos);
    if (over < 0)
        return count();
    const QRect rect = tabRect(over);
    const bool vertical = verticalTabShape(shape());
    const int mid = vertical ? rect.center().y() : rect.center().x();
    const int at = vertical ? pos.y() : pos.x();
    return at > mid ? over + 1 : over;
}

void TabBar::wheelEvent(QWheelEvent *event)
{
    // The strip may scroll under the resting pointer — the card's
    // anchor is stale the moment the tabs move.
    m_previewTimer->stop();
    hideTabPreview();
    QTabBar::wheelEvent(event);
}

// POL02: hovering a tab dwells for 500ms before the card appears;
// once a card is up, pointer walks retarget it instantly.  Presses and
// drags keep it down.
void TabBar::updatePreviewDwell()
{
    // PREFUI05: no hover card while Energy Saver is on.
    // TPREV01: or when the user opted the card out in Preferences.
    if (!m_tabPreviewsEnabled || m_hoveredTab < 0
        || EnergySaver::instance()->active()) {
        m_previewTimer->stop();
        hideTabPreview();
        return;
    }
    if (m_preview && m_preview->isVisible()) {
        showTabPreview(m_hoveredTab);
        return;
    }
    if (QApplication::mouseButtons() & Qt::LeftButton)
        return;
    m_previewTimer->start();
}

void TabBar::showTabPreview(int index)
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || index < 0 || index >= count() || !isVisible()
        || (QApplication::mouseButtons() & Qt::LeftButton)
        || !m_tabPreviewsEnabled
        || EnergySaver::instance()->active()) {
        hideTabPreview();
        return;
    }
    if (!m_preview)
        m_preview = new TabPreview(this);
    m_preview->previewTab(index);
}

void TabBar::hideTabPreview()
{
    if (m_preview)
        m_preview->hide();
}

TabPreview *TabBar::tabPreview() const
{
    return m_preview;
}

// CONT02: the container a tab belongs to, resolved through the page's
// profile — the registry entry for a container tab, an empty struct
// for the default container / off-the-record pages / when the bar is
// not hosted by a TabWidget.
ContainerManager::Container TabBar::containerForTab(int index) const
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || index < 0 || index >= count())
        return ContainerManager::Container();
    return ContainerManager::instance()->containerForId(
        tabWidget->containerIdForTab(index));
}

// A compact bold face for the chip text — the chip band is ~12px.
QFont TabBar::containerChipFont() const
{
    QFont chipFont = font();
    chipFont.setBold(true);
    if (chipFont.pointSizeF() > 0)
        chipFont.setPointSizeF(qMax(6.5, chipFont.pointSizeF() * 0.7));
    else if (chipFont.pixelSize() > 0)
        chipFont.setPixelSize(qMax(8, chipFont.pixelSize() * 2 / 3));
    return chipFont;
}

// The extra size a container chip reserves for the tab at index.
// TABS06: the chip lives INSIDE the strip's thickness — the accent
// line paints inside the tab rect and the name pill borrows title
// width at the trailing edge — so the reserve is width-only and a
// container tab is exactly as tall as a plain one (the old top band
// added ~17px and nearly doubled the strip).
QSize TabBar::containerChipSize(int index) const
{
    // CONT06: no chips inside a filtered level — the header already
    // names the container.  QSize() is an invalid (-1,-1) size and
    // silently subtracted a pixel at every + chip.height() site —
    // return a valid empty size instead.
    if (containerStripHeight() > 0)
        return QSize(0, 0);
    if (containerForTab(index).id.isEmpty())
        return QSize(0, 0);
    // Vertical strips: the accent line on the row's outer edge asks
    // for 3px of strip width; the name pill borrows row width, never
    // row height.
    if (verticalTabShape(shape()))
        return QSize(3, 0);
    return QSize(containerPillReserve(index), 0);
}

// The width the name pill borrows from the title for the tab at
// index — zero for pinned tabs and collapsed-group chips (they carry
// no title to shave), nameless containers, and when the two-level
// band already names the container.
int TabBar::containerPillReserve(int index) const
{
    if (containerStripHeight() > 0)
        return 0;
    const ContainerManager::Container c = containerForTab(index);
    if (c.id.isEmpty() || c.name.isEmpty())
        return 0;
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (tabWidget && (tabWidget->isTabPinned(index)
            || tabWidget->isTabGroupChip(index)))
        return 0;
    const int pillWidth = qMin(
        120, QFontMetrics(containerChipFont())
                 .horizontalAdvance(c.name) + 10);
    return pillWidth + 6;
}

// CONT06 — level-1 container-header band ------------------------------
//
// The band occupies the top containerStripHeight() pixels of the bar
// when the TabWidget's two-level mode has more than one container
// owning tabs.  Tabs keep their natural layout — tabRect()/tabAt()
// and Qt's internals all live in the "strip space" starting at
// logical y=0 — so every event handler maps a physical point down by
// the band height first (stripPos()) and the paint paths translate
// up by the same amount.

int TabBar::containerStripHeight() const
{
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || !tabWidget->containerStripActive())
        return 0;
    return fontMetrics().height() + 9;
}

QPoint TabBar::stripPos(const QPoint &physicalPos) const
{
    return physicalPos - QPoint(0, containerStripHeight());
}

QString TabBar::containerHeaderId(int headerIndex) const
{
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget)
        return QString();
    return tabWidget->containerHeaders().value(headerIndex);
}

// Preferred pixel width of one header cell — icon + name + count,
// clamped so a long container name can't starve the other headers.
static int containerHeaderCellWidth(const QString &text,
                                    const QFontMetrics &fm)
{
    const int preferred = 8 + 16 + 4 + fm.horizontalAdvance(text) + 8;
    return qBound(40, preferred, 170);
}

QRect TabBar::containerHeaderRect(int headerIndex) const
{
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget)
        return QRect();
    const QStringList headers = tabWidget->containerHeaders();
    if (headerIndex < 0 || headerIndex >= headers.count())
        return QRect();
    const QFontMetrics fm(font());
    QVector<int> widths(headers.count());
    int total = 0;
    for (int i = 0; i < headers.count(); ++i) {
        const ContainerManager::Container container =
            ContainerManager::instance()->containerForId(headers.at(i));
        const QString name = container.id.isEmpty()
            ? tr("Tabs") : container.name;
        widths[i] = containerHeaderCellWidth(
            name + QLatin1Char(' ')
                + QString::number(tabWidget->containerTabCount(
                    headers.at(i))),
            fm);
        total += widths[i];
    }
    // Overflow shrinks every cell proportionally, never below an
    // icon-and-count sliver.
    const int avail = width() - 8 - qMax(0, headers.count() - 1) * 2;
    if (total > avail && avail > 0) {
        for (int i = 0; i < widths.count(); ++i)
            widths[i] = qMax(24, qMin(widths[i],
                widths[i] * avail / qMax(1, total)));
    }
    int x = 4;
    for (int i = 0; i < headerIndex; ++i)
        x += widths[i] + 2;
    const int band = containerStripHeight();
    return QRect(x, 1, widths[headerIndex], band - 2);
}

int TabBar::containerHeaderAt(const QPoint &physicalPos) const
{
    const int band = containerStripHeight();
    if (band <= 0 || physicalPos.y() < 0 || physicalPos.y() >= band)
        return -1;
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    const int headers = tabWidget ? tabWidget->containerHeaders().count()
                                  : 0;
    for (int i = 0; i < headers; ++i) {
        if (containerHeaderRect(i).contains(physicalPos))
            return i;
    }
    return -1;
}

void TabBar::paintContainerStrip(QPainter *painter)
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget)
        return;
    const int band = containerStripHeight();
    const QStringList headers = tabWidget->containerHeaders();
    const QString active = tabWidget->activeContainerHeader();
    ContainerManager *manager = ContainerManager::instance();

    painter->fillRect(0, 0, width(), band,
                      palette().color(QPalette::Window).darker(103));
    for (int i = 0; i < headers.count(); ++i) {
        const QString id = headers.at(i);
        const QRect rect = containerHeaderRect(i);
        const bool isActive = (id == active);
        const ContainerManager::Container container =
            manager->containerForId(id);
        const QColor accent = container.color.isValid()
            ? container.color : palette().color(QPalette::Mid);

        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        QColor fill = palette().color(QPalette::Window);
        if (isActive)
            fill = palette().color(QPalette::Base);
        else if (i == m_bandHoverHeader || i == m_bandPressedHeader)
            fill = fill.lighter(112);
        painter->setPen(Qt::NoPen);
        painter->setBrush(fill);
        painter->drawRoundedRect(rect.adjusted(0, 0, 0, 1), 4, 4);
        // The container color rides the cell's bottom edge — same
        // accent language as the CONT02 tab strip.
        painter->fillRect(rect.left() + 4, rect.bottom() - 2,
                          rect.width() - 8, isActive ? 3 : 2, accent);
        if (i == m_bandDropHeader) {
            painter->setPen(QPen(palette().color(QPalette::Highlight),
                                 2));
            painter->setBrush(Qt::NoBrush);
            painter->drawRoundedRect(rect.adjusted(1, 1, -1, -1), 4, 4);
        }
        if (i == m_bandFocusHeader && hasFocus()) {
            QStyleOptionFocusRect option;
            option.initFrom(this);
            option.rect = rect.adjusted(2, 2, -2, -2);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option,
                                   painter, this);
        }

        // Swatch + name + count badge.
        const int iconSize = 14;
        QRect iconRect(rect.left() + 6,
                       rect.top() + (rect.height() - iconSize) / 2 - 2,
                       iconSize, iconSize);
        ContainerManager::colorIcon(accent).paint(
            painter, iconRect, Qt::AlignCenter);
        const QString name = container.id.isEmpty()
            ? tr("Tabs") : container.name;
        const QString text = tr("%1 (%2)")
            .arg(name)
            .arg(tabWidget->containerTabCount(id));
        const QRect textRect(rect.left() + iconSize + 10, rect.top(),
                             rect.width() - iconSize - 14,
                             rect.height() - 3);
        painter->setFont(font());
        painter->setPen(palette().color(
            isActive ? QPalette::Text : QPalette::WindowText));
        painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
                          fontMetrics().elidedText(text, Qt::ElideRight,
                                                   textRect.width()));
        painter->restore();
    }
}

// QTabBar lays per-tab side buttons and its scroll buttons out in
// strip space — with the band up everything sits containerStripHeight()
// pixels too high.  Qt repositions them on every layout, so any child
// still inside the band zone shifts down by the band height.
void TabBar::shiftChildrenBelowStrip()
{
    const int band = containerStripHeight();
    if (band <= 0)
        return;
    const bool vertical = verticalTabShape(shape());
    const QTabBar::ButtonPosition sides[2] = { QTabBar::LeftSide,
                                               QTabBar::RightSide };
    const QObjectList kids = children();
    for (QObject *kid : kids) {
        QWidget *child = qobject_cast<QWidget*>(kid);
        if (!child || child == m_preview || child->y() >= band)
            continue;
        bool isTabButton = false;
        for (int i = 0; i < count() && !isTabButton; ++i) {
            for (const QTabBar::ButtonPosition side : sides) {
                if (tabButton(i, side) == child) {
                    isTabButton = true;
                    break;
                }
            }
        }
        // Vertical tab buttons are placed by layoutRowTabButtons(),
        // which already knows about the band.
        if (isTabButton && vertical)
            continue;
        child->move(child->x(), child->y() + band);
    }
}

// Screen readers can't see the painted headers — carry them on the
// bar's description instead.
void TabBar::updateAccessibleStrip()
{
    const TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || !tabWidget->containerStripActive()) {
        if (!accessibleDescription().isEmpty())
            setAccessibleDescription(QString());
        return;
    }
    ContainerManager *manager = ContainerManager::instance();
    QStringList parts;
    const QStringList headers = tabWidget->containerHeaders();
    const QString active = tabWidget->activeContainerHeader();
    for (const QString &id : headers) {
        const ContainerManager::Container container =
            manager->containerForId(id);
        parts.append(tr("%1 (%2 tabs%3)")
            .arg(container.id.isEmpty() ? tr("Tabs") : container.name)
            .arg(tabWidget->containerTabCount(id))
            .arg(id == active ? tr(", active") : QString()));
    }
    setAccessibleDescription(tr("Container strip: %1")
                             .arg(parts.join(QLatin1String(", "))));
}

// The band is real widget height — the strip below keeps its natural
// tab layout.
QSize TabBar::sizeHint() const
{
    QSize size = QTabBar::sizeHint();
    size.rheight() += containerStripHeight();
    return size;
}

QSize TabBar::minimumSizeHint() const
{
    QSize size = QTabBar::minimumSizeHint();
    size.rheight() += containerStripHeight();
    return size;
}

// Arrow-key header nav: Up leaves the tab row into the band,
// Left/Right walk the headers, Return/Space activates, Down/Escape
// drops back to the row.
void TabBar::keyPressEvent(QKeyEvent *event)
{
    const int band = containerStripHeight();
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (band <= 0 || !tabWidget) {
        QTabBar::keyPressEvent(event);
        return;
    }
    const QStringList headers = tabWidget->containerHeaders();
    if (m_bandFocusHeader >= 0) {
        switch (event->key()) {
        case Qt::Key_Left:
            m_bandFocusHeader = qMax(0, m_bandFocusHeader - 1);
            break;
        case Qt::Key_Right:
            m_bandFocusHeader = qMin(headers.count() - 1,
                                     m_bandFocusHeader + 1);
            break;
        case Qt::Key_Home:
            m_bandFocusHeader = 0;
            break;
        case Qt::Key_End:
            m_bandFocusHeader = headers.count() - 1;
            break;
        case Qt::Key_Return:
        case Qt::Key_Enter:
        case Qt::Key_Space:
            tabWidget->setActiveContainerHeader(
                containerHeaderId(m_bandFocusHeader));
            break;
        case Qt::Key_Down:
        case Qt::Key_Escape:
            m_bandFocusHeader = -1;
            break;
        default:
            QTabBar::keyPressEvent(event);
            return;
        }
        update();
        return;
    }
    if (event->key() == Qt::Key_Up
        && (!verticalTabShape(shape()) || currentIndex() <= 0)) {
        m_bandFocusHeader = qMax(
            0, headers.indexOf(tabWidget->activeContainerHeader()));
        update();
        return;
    }
    QTabBar::keyPressEvent(event);
}

// A mouse event whose position() is shifted into strip space — Qt's
// own handlers (press-to-select, move drags, hover) all resolve tabs
// through it.
static QMouseEvent shiftedMouseEvent(const QMouseEvent *event,
                                     const QPointF &delta)
{
    return QMouseEvent(event->type(), event->position() + delta,
                       event->scenePosition() + delta,
                       event->globalPosition(), event->button(),
                       event->buttons(), event->modifiers());
}

// TABGRP01: the tab's group id, or empty when ungrouped / the bar is
// not hosted by a TabWidget.
QString TabBar::groupIdForTab(int index) const
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || index < 0 || index >= count())
        return QString();
    return tabWidget->tabGroupId(index);
}

// The tab opens a contiguous run of its group — the name pill anchors
// on the run's first member only.
bool TabBar::isFirstInGroupRun(int index) const
{
    const QString gid = groupIdForTab(index);
    return !gid.isEmpty() && groupIdForTab(index - 1) != gid;
}

// The label a collapsed group's chip shows — the group name plus the
// member count, or just the count for an unnamed group.
QString TabBar::groupChipLabel(int index) const
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    const QString gid = groupIdForTab(index);
    if (!tabWidget || gid.isEmpty())
        return QString();
    const QString name = tabWidget->tabGroupName(gid);
    const int size = tabWidget->tabGroupSize(gid);
    return name.isEmpty()
        ? tr("%1 tabs").arg(size)
        : tr("%1 (%2)").arg(name).arg(size);
}

// TABS17: the readable band a vertical row's label and side widgets
// share — the row's rect minus the name pill band a group run's
// first member reserves on top (tabSizeHint grew the row by the
// same extent).  Centering the title on the whole rect would push
// its baseline against the row's clipped bottom edge and slide it
// under the pill.
QRect TabBar::rowLabelRect(const QRect &rowRect, int index) const
{
    const QString gid = groupIdForTab(index);
    if (gid.isEmpty() || m_iconOnlyTabs || !isFirstInGroupRun(index))
        return rowRect;
    const TabWidget *tabWidget =
        qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || tabWidget->tabGroupName(gid).isEmpty())
        return rowRect;
    QRect rect = rowRect;
    rect.setTop(rect.top()
                + QFontMetrics(containerChipFont()).height() + 6);
    return rect;
}

// CONT02: after the style paints the tab, container tabs get their
// accent — a strip on the tab's outer edge (top for a horizontal bar,
// the free side for a vertical one) plus a name chip just inside it.
// CONT06: with the two-level band up, every tab paints containerStripHeight()
// pixels lower and the band paints above it in physical space.
void TabBar::paintEvent(QPaintEvent *event)
{
    // TABS15: tab rects can slide without a tabLayoutChange (wheel
    // scrolls, live move drags) — keep the '+' anchored on the last
    // tab.
    layoutNewTabButton();
    const int band = containerStripHeight();
    if (band > 0) {
        QPainter bandPainter(this);
        paintContainerStrip(&bandPainter);
    }
    if (verticalTabShape(shape())) {
        // TABS02: rows, not rotated tabs — each vertical tab paints
        // through the horizontal North shape so its title stays
        // readable.  The side-button widgets (favicon label, close
        // button) are re-anchored to the row ends first; QTabBar's
        // own layout positions them transposed for West/East shapes.
        layoutRowTabButtons();
        QPainter painter(this);
        painter.translate(0, band);
        painter.fillRect(event->rect().translated(0, -band),
                         palette().color(QPalette::Window));
        for (int index = 0; index < count(); ++index) {
            if (m_moveDragActive && m_moveDragVertical
                && index == m_pressedTabIndex)
                continue;   // painted floating below
            QStyleOptionTab option;
            initStyleOption(&option, index);
            if (option.rect.bottom() < 0 || option.rect.top() > height())
                continue;   // scrolled out of the strip
            option.shape = QTabBar::RoundedNorth;
            // TABS11: clip the row draw to its own rect — Fusion's
            // selected-tab paint issues a fillRect whose height is the
            // rect's bottom coordinate, so on a tall strip the
            // selected row grows a ghost band reaching (bottom-1) px
            // below it.  Harmless inside a ~26px horizontal strip
            // (the widget bounds clip it), catastrophic on a 600px
            // vertical rail.
            painter.setClipRect(option.rect);
            // initStyleOption already elided the text against the
            // vertical shape's narrow text rect — redo it for the
            // row's width.  Every tab under the icon-only view mode
            // stays icon-only (group chips keep their pill label);
            // TABS14: a pinned row keeps its title like any tab.
            const TabWidget *tabWidget =
                qobject_cast<TabWidget*>(parentWidget());
            const bool iconOnly = m_iconOnlyTabs
                && !(tabWidget && tabWidget->isTabGroupChip(index));
            if (!iconOnly) {
                const QRect textRect = style()->subElementRect(
                    QStyle::SE_TabBarTabText, &option, this);
                // The container name pill borrows the title's
                // trailing width — only while the row still leaves a
                // readable title stub, or the pill is dropped and the
                // title keeps the full width.
                const int reserve = containerPillReserve(index);
                const int pillWidth =
                    textRect.width() - reserve > 24 ? reserve : 0;
                option.text = fontMetrics().elidedText(
                    tabText(index), elideMode(),
                    textRect.width() - pillWidth,
                    Qt::TextShowMnemonic);
            }
            if (!(option.state & QStyle::State_Enabled))
                option.palette.setCurrentColorGroup(QPalette::Disabled);
            // TABS17: the title belongs on the readable band — the
            // row's rect minus the reserved group-pill top — not the
            // full row.  Shrinking the option's rect cannot move the
            // text: the styles anchor the label layout to it (the
            // sheet style even re-queries tabRect()), so the painter
            // is translated instead — every style's centered text
            // then lands centered on the band.  The clip was set in
            // device space above, so the shifted draw still cannot
            // leave the row.
            const int labelShift =
                rowLabelRect(option.rect, index).center().y()
                - option.rect.center().y();
            style()->drawControl(QStyle::CE_TabBarTabShape, &option,
                                 &painter, this);
            painter.save();
            if (labelShift)
                painter.translate(0, labelShift);
            style()->drawControl(QStyle::CE_TabBarTabLabel, &option,
                                 &painter, this);
            painter.restore();
            painter.setClipping(false);
        }
        // The tab under a move drag floats with the cursor; Qt's own
        // moving-tab pixmap (painted rotated) is hidden.
        if (m_moveDragActive && m_moveDragVertical
            && m_pressedTabIndex >= 0 && m_pressedTabIndex < count()) {
            QStyleOptionTab option;
            initStyleOption(&option, m_pressedTabIndex);
            option.shape = QTabBar::RoundedNorth;
            option.position = QStyleOptionTab::Moving;
            option.rect.moveTop(m_moveDragMouseY - m_moveDragGrabOffset);
            const TabWidget *tabWidget =
                qobject_cast<TabWidget*>(parentWidget());
            const bool iconOnly = m_iconOnlyTabs
                && !(tabWidget
                     && tabWidget->isTabGroupChip(m_pressedTabIndex));
            if (!iconOnly) {
                const QRect textRect = style()->subElementRect(
                    QStyle::SE_TabBarTabText, &option, this);
                const int reserve =
                    containerPillReserve(m_pressedTabIndex);
                const int pillWidth =
                    textRect.width() - reserve > 24 ? reserve : 0;
                option.text = fontMetrics().elidedText(
                    tabText(m_pressedTabIndex), elideMode(),
                    textRect.width() - pillWidth,
                    Qt::TextShowMnemonic);
            }
            painter.setClipRect(option.rect);
            // TABS17: same painter-shifted band label as the docked
            // rows above.
            const int labelShift =
                rowLabelRect(option.rect, m_pressedTabIndex)
                    .center().y() - option.rect.center().y();
            style()->drawControl(QStyle::CE_TabBarTabShape, &option,
                                 &painter, this);
            painter.save();
            if (labelShift)
                painter.translate(0, labelShift);
            style()->drawControl(QStyle::CE_TabBarTabLabel, &option,
                                 &painter, this);
            painter.restore();
            painter.setClipping(false);
        }
    } else if (band > 0) {
        // CONT06: horizontal shapes hand-paint each tab under the
        // band — QTabBar::paintEvent has no way to offset the row.
        QPainter painter(this);
        painter.translate(0, band);
        for (int index = 0; index < count(); ++index) {
            QStyleOptionTab option;
            initStyleOption(&option, index);
            style()->drawControl(QStyle::CE_TabBarTab, &option,
                                 &painter, this);
        }
    } else {
        QTabBar::paintEvent(event);
    }

    const QFont chipFont = containerChipFont();
    const int chipHeight = QFontMetrics(chipFont).height() + 4;
    const int strip = 3;

    QPainter painter(this);
    // Every overlay below the band still measures against tabRect()'s
    // strip coordinates — translate once instead of touching each
    // chip/badge/pill rect.
    painter.translate(0, band);
    // CONT06: inside a filtered level the container chips are
    // redundant — every tab in the row shares the header's container.
    // They stay on in single-row (inline chips) mode.
    // TABS08: the indicator is the accent edge stripe alone — the name
    // pill used to sit in a second reserved row that doubled the
    // strip's height; the container name still rides the tab tooltip.
    for (int index = 0; band <= 0 && index < count(); ++index) {
        const ContainerManager::Container container = containerForTab(index);
        if (container.id.isEmpty())
            continue;
        const QColor accent = container.color.isValid()
            ? container.color : palette().color(QPalette::Highlight);
        const QRect rect = tabRect(index);

        QRect accentRect;
        switch (shape()) {
        case QTabBar::RoundedSouth:
        case QTabBar::TriangularSouth:
            accentRect = QRect(rect.left(), rect.bottom() - strip + 1,
                               rect.width(), strip);
            break;
        case QTabBar::RoundedWest:
        case QTabBar::TriangularWest:
            accentRect = QRect(rect.left(), rect.top(),
                               strip, rect.height());
            break;
        case QTabBar::RoundedEast:
        case QTabBar::TriangularEast:
            accentRect = QRect(rect.right() - strip + 1, rect.top(),
                               strip, rect.height());
            break;
        default:
            accentRect = QRect(rect.left(), rect.top(),
                               rect.width(), strip);
            break;
        }
        painter.fillRect(accentRect, accent);

        // TABS06: the name pill is a rounded chip in the container
        // color anchored at the title's trailing edge — its width was
        // shaved off the title's elision so the two never overlap and
        // the tab keeps a plain tab's height.  On a very narrow tab
        // only the accent line shows (it still identifies the
        // container).
        const int reserve = containerPillReserve(index);
        if (reserve > 0) {
            QStyleOptionTab option;
            initStyleOption(&option, index);
            if (verticalTabShape(shape()))
                option.shape = QTabBar::RoundedNorth;
            const QRect textRect = style()->subElementRect(
                QStyle::SE_TabBarTabText, &option, this);
            const QRect rightButton = style()->subElementRect(
                QStyle::SE_TabBarTabRightButton, &option, this);
            int right = textRect.right();
            if (rightButton.isValid())
                right = qMin(right, rightButton.left() - 2);
            const QRect chip(right - reserve + 4,
                             rect.top()
                                 + (rect.height() - chipHeight) / 2,
                             reserve - 6, chipHeight);
            // The pill only draws where initStyleOption()/the row's
            // own re-elide actually shaved the title — narrower tabs
            // keep their title and fall back to the accent line.
            if (textRect.width() - reserve > 24
                    && chip.width() >= 24) {
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(accent);
                painter.setPen(accent.darker(130));
                painter.drawRoundedRect(chip, 3, 3);
                painter.setFont(chipFont);
                painter.setPen(accent.lightness() > 140
                               ? Qt::black : Qt::white);
                const QString elided = painter.fontMetrics().elidedText(
                    container.name, Qt::ElideRight, chip.width() - 8);
                painter.drawText(chip.adjusted(4, 0, -4, 0),
                                 Qt::AlignCenter | Qt::AlignVCenter,
                                 elided);
                painter.restore();
            }
        }
    }

    // SLEEP01: suspended tabs carry a small "zZ" badge at the tab's
    // inner edge (the title itself is dimmed by the TabWidget).  The
    // badge sits against the close-button side on horizontal bars and
    // at the bottom on vertical ones.
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (tabWidget) {
        QFont badgeFont = containerChipFont();
        badgeFont.setItalic(true);
        painter.setFont(badgeFont);
        painter.setPen(palette().color(QPalette::Disabled,
                                       QPalette::WindowText));
        const QString badge = QLatin1String("zZ");
        for (int index = 0; index < count(); ++index) {
            if (!tabWidget->isTabSleeping(index))
                continue;
            const QRect rect = tabRect(index);
            QRect badgeRect;
            if (verticalTabShape(shape()))
                badgeRect = QRect(rect.left() + 2, rect.bottom() - chipHeight,
                                  rect.width() - 4, chipHeight);
            else
                badgeRect = QRect(rect.left() + 4, rect.top() + 2,
                                  rect.width() - 8, chipHeight);
            painter.drawText(badgeRect,
                             Qt::AlignRight | Qt::AlignVCenter, badge);
        }
    }

    // TABGRP01: group accents — every member carries a rail on the
    // content-facing edge (bottom for a top bar, the inner side for a
    // vertical one), a run's first member carries the name pill, and a
    // collapsed group's chip is painted over as one full pill.
    if (tabWidget) {
        for (int index = 0; index < count(); ++index) {
            const QString gid = tabWidget->tabGroupId(index);
            if (gid.isEmpty())
                continue;
            const QColor accent = tabWidget->tabGroupColor(gid).isValid()
                ? tabWidget->tabGroupColor(gid)
                : palette().color(QPalette::Highlight);
            const QColor textColor = accent.lightness() > 140
                ? Qt::black : Qt::white;
            const QRect rect = tabRect(index);
            const QString name = tabWidget->tabGroupName(gid);

            if (tabWidget->isTabGroupChip(index)) {
                // Collapsed: the whole tab reads as the group chip.
                const QRect pill = rect.adjusted(2, 2, -2, -2);
                painter.save();
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setBrush(accent);
                painter.setPen(accent.darker(130));
                painter.drawRoundedRect(pill, 5, 5);
                painter.setFont(chipFont);
                painter.setPen(textColor);
                const QString elided = painter.fontMetrics().elidedText(
                    groupChipLabel(index), Qt::ElideRight,
                    pill.width() - 8);
                painter.drawText(pill.adjusted(4, 0, -4, 0),
                                 Qt::AlignCenter | Qt::AlignVCenter,
                                 elided);
                painter.restore();
                continue;
            }

            switch (shape()) {
            case QTabBar::RoundedSouth:
            case QTabBar::TriangularSouth:
                painter.fillRect(rect.left(), rect.top(),
                                 rect.width(), strip, accent);
                break;
            case QTabBar::RoundedWest:
            case QTabBar::TriangularWest:
                painter.fillRect(rect.right() - strip + 1, rect.top(),
                                 strip, rect.height(), accent);
                break;
            case QTabBar::RoundedEast:
            case QTabBar::TriangularEast:
                painter.fillRect(rect.left(), rect.top(),
                                 strip, rect.height(), accent);
                break;
            default:
                painter.fillRect(rect.left(), rect.bottom() - strip + 1,
                                 rect.width(), strip, accent);
                break;
            }

            // The name pill sits just inside the rail on the run's
            // first member — tabSizeHint() reserved its extent.
            // TABS09: icon-only mode keeps the strip uniformly
            // icon-sized — the accent rail still marks the group but
            // the pill would only paint clipped over the favicon.
            if (name.isEmpty() || !isFirstInGroupRun(index)
                || m_iconOnlyTabs)
                continue;
            const int pillWidth = QFontMetrics(chipFont)
                .horizontalAdvance(name) + 12;
            QRect pill;
            switch (shape()) {
            case QTabBar::RoundedWest:
            case QTabBar::TriangularWest: {
                // TABS08: container tabs no longer own a top band —
                // the pill anchors at the row's own reserved space.
                pill = QRect(rect.left() + 2, rect.top() + 2,
                             rect.width() - strip - 4, chipHeight);
                break;
            }
            case QTabBar::RoundedEast:
            case QTabBar::TriangularEast: {
                pill = QRect(rect.left() + strip + 2, rect.top() + 2,
                             rect.width() - strip - 4, chipHeight);
                break;
            }
            default:
                // Horizontal bars: the pill anchors at the leading
                // edge, vertically centered — the reserved width
                // pushes the tab's label right of it.
                pill = QRect(rect.left() + 4,
                             rect.top() + (rect.height() - chipHeight) / 2,
                             qMin(pillWidth, rect.width() - 8),
                             chipHeight);
                break;
            }
            if (pill.width() < 16)
                continue;
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(accent);
            painter.setPen(accent.darker(130));
            painter.drawRoundedRect(pill, 3, 3);
            painter.setFont(chipFont);
            painter.setPen(textColor);
            const QString elided = painter.fontMetrics().elidedText(
                name, Qt::ElideRight, pill.width() - 8);
            painter.drawText(pill.adjusted(4, 0, -4, 0),
                             Qt::AlignCenter | Qt::AlignVCenter, elided);
            painter.restore();
        }
    }

    // RSZ02: the grip affordance paints in physical space — the
    // overlay painter above is still translated into strip
    // coordinates.
    painter.resetTransform();
    paintResizeGrip(&painter);
}

// TABS09: the icon-only view mode paints every tab icon-only — the
// favicon label is a tab-button widget and stays, the style just
// never sees the title.
// TABS14: a pinned tab keeps its title — leading-block position and
// the missing close button mark the pin, not a textless tile.
// A collapsed group's chip keeps its text either way: the pill IS
// the label and a 36px tab could not carry it.
void TabBar::initStyleOption(QStyleOptionTab *option, int tabIndex) const
{
    QTabBar::initStyleOption(option, tabIndex);
    const TabWidget *tabWidget =
        qobject_cast<TabWidget*>(parentWidget());
    if (m_iconOnlyTabs
            && !(tabWidget && tabWidget->isTabGroupChip(tabIndex)))
        option->text.clear();
    // TABS06: on horizontal bars the container name pill borrows the
    // title's trailing width — elide the title short of the pill so
    // the two never paint over each other.  Vertical rows re-elide
    // themselves in paintEvent() against the row's real text rect.
    if (!verticalTabShape(shape()) && !option->text.isEmpty()) {
        const int reserve = containerPillReserve(tabIndex);
        if (reserve > 0) {
            const QRect textRect = style()->subElementRect(
                QStyle::SE_TabBarTabText, option, this);
            if (textRect.width() - reserve > 24) {
                option->text = fontMetrics().elidedText(
                    tabText(tabIndex), elideMode(),
                    textRect.width() - reserve,
                    Qt::TextShowMnemonic);
            }
        }
    }
}

QSize TabBar::tabSizeHint(int index) const
{
    QSize sizeHint = QTabBar::tabSizeHint(index);
    QFontMetrics fm = fontMetrics();
    const int extent = fm.horizontalAdvance(QLatin1Char('M')) * 18;
    // QTabBar::tabSizeHint returns a transposed size for vertical
    // shapes — bound the tab's long axis either way so a long title
    // can't stretch the whole strip.  UIP01: floor the strip's
    // thickness so tabs keep modern breathing room.  TABS07: cap it
    // too — per-tab button widgets and styles with generous padding
    // pushed a plain tab ~5px past icon+text height; keep the strip
    // at browser-normal content+padding.  TABS08: tightened — the
    // user-visible target is the compact ~26-28px band real browsers
    // sit in, so padding is trimmed to text/icon + 9 (fm+8 floor).
    // TABS06: a container chip only borrows long-axis extent (accent
    // line + name pill inside the title's zone) — it never makes the
    // strip thicker.  RSZ02: a dragged-in strip height
    // (horizontalTabHeight > 0) overrides the Automatic metrics
    // bound; 0 keeps the normalized sizing.
    const int minThickness = fm.height() + 8;
    const int maxThickness = qMax(fm.height(), iconSize().height()) + 9;
    const int thickness = m_tabStripHeight > 0
        ? m_tabStripHeight
        : qBound(minThickness, sizeHint.height(), maxThickness);
    const QSize chip = containerChipSize(index);
    const QString gid = groupIdForTab(index);
    // TABS02: vertical strips lay out as horizontal rows — the
    // strip's width is the persisted drag-resizable width and each
    // row is a single text line tall.  Container tabs widen the strip
    // by the accent line (containerChipSize carries it); a group
    // run's first member keeps the band the name pill paints into.
    if (verticalTabShape(shape())) {
        // Rows stay one text line tall — the bounded thickness above
        // is meaningless here since sizeHint.height() is the
        // transposed row width for vertical shapes.  TABS09: the
        // icon-only mode drops the group pill's extra band — the pill
        // is suppressed while the mode is on.  TABS17: a dragged-in
        // strip height (horizontalTabHeight > 0) feeds the rows too,
        // floored at the text line — a too-small persisted value
        // can't crop the title under the row's clip rect.
        int rowHeight = m_tabStripHeight > 0
            ? qMax(minThickness, m_tabStripHeight)
            : minThickness;
        if (!gid.isEmpty() && !m_iconOnlyTabs) {
            TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
            if (tabWidget && isFirstInGroupRun(index)
                && !tabWidget->tabGroupName(gid).isEmpty())
                rowHeight += QFontMetrics(containerChipFont()).height() + 6;
        }
        // TABS14: a pinned row is an ordinary titled row — the
        // strip's shared width and row height, title included.
        // TABS09: the icon-only mode collapses the strip to an icon
        // rail — a collapsed group's chip keeps room for its label,
        // every other row shrinks to the favicon tile.
        int rowWidth = m_verticalTabWidth;
        if (m_iconOnlyTabs) {
            rowWidth = iconSize().width() + 20;
            TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
            if (tabWidget && tabWidget->isTabGroupChip(index)) {
                const int labelWidth = QFontMetrics(containerChipFont())
                    .horizontalAdvance(groupChipLabel(index)) + 28;
                rowWidth = qMin(m_verticalTabWidth,
                                qMax(rowWidth, labelWidth));
            }
        }
        return QSize(rowWidth + qMax(0, chip.width()),
                     rowHeight + qMax(0, chip.height()));
    }
    // TABS09: under the icon-only view mode every tab shrinks to the
    // favicon and its margins; a collapsed group's chip keeps its
    // label-driven size below.  TABS14: a pinned tab keeps the normal
    // titled sizing — pinned no longer means icon-only.
    const TabWidget *tabOwner =
        qobject_cast<TabWidget*>(parentWidget());
    if (m_iconOnlyTabs
            && !(tabOwner && tabOwner->isTabGroupChip(index))) {
        return QSize(iconSize().width() + 20,
                     thickness + chip.height());
    }
    // TABGRP01: a collapsed group's chip shrinks to its label instead
    // of the member's title; the first member of a run widens to carry
    // the group name pill paintEvent() draws.
    if (!gid.isEmpty()) {
        TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
        if (tabWidget && tabWidget->isTabGroupChip(index)) {
            const int labelWidth = QFontMetrics(containerChipFont())
                .horizontalAdvance(groupChipLabel(index)) + 20;
            sizeHint.setWidth(qMin(sizeHint.width(), labelWidth));
        } else if (isFirstInGroupRun(index) && tabWidget) {
            const QString name = tabWidget->tabGroupName(gid);
            if (!name.isEmpty()) {
                const int pillWidth = QFontMetrics(containerChipFont())
                    .horizontalAdvance(name) + 20;
                sizeHint.setWidth(sizeHint.width() + pillWidth);
            }
        }
    }
    // RSZ01: the horizontal width bounds — a configured cap replaces
    // the built-in 18-em extent so tabs cannot balloon; a configured
    // floor keeps each tab at least that wide (below the collective
    // fit the scroll buttons take over).  The floor clamps to an
    // explicit cap but not to the Automatic one — a min wider than
    // the built-in extent is an explicit request for wider tabs.
    const int cap = m_tabMaxWidth > 0 ? m_tabMaxWidth : extent;
    const int floor = m_tabMinWidth > 0
        ? (m_tabMaxWidth > 0 ? qMin(m_tabMinWidth, m_tabMaxWidth)
                           : m_tabMinWidth)
        : 0;
    return QSize(qBound(floor, sizeHint.width() + chip.width(),
                        qMax(cap, floor)),
                 thickness + chip.height());
}

QSize TabBar::minimumTabSizeHint(int index) const
{
    // TABS02: rows never compress below their line height — when the
    // strip overflows, the scroll buttons take over.
    // TABS09: neither does an icon-sized tab — icon-only IS its
    // minimum.
    if (verticalTabShape(shape()) || m_iconOnlyTabs)
        return tabSizeHint(index);
    // RSZ01: a configured floor overrides the style's squeeze
    // minimum — tabs never lay out narrower than tabs/tabMinWidth.
    if (m_tabMinWidth > 0) {
        const QSize hint = tabSizeHint(index);
        return QSize(qMin(hint.width(), m_tabMinWidth), hint.height());
    }
    // TABS14: a pinned tab's floor keeps the favicon plus a stub of
    // title — under pressure it elides toward that minimum but never
    // returns to the icon-only tile.
    if (isTabPinned(index)) {
        const QSize hint = tabSizeHint(index);
        const int floorWidth = iconSize().width() + 8
            + fontMetrics().horizontalAdvance(QLatin1Char('M')) * 5;
        return QSize(qMin(hint.width(), floorWidth), hint.height());
    }
    // RSZ02: a user-set strip height is also the floor — the layout
    // never squeezes a horizontal tab shorter than the drag chose.
    if (m_tabStripHeight > 0) {
        const QSize hint = QTabBar::minimumTabSizeHint(index);
        return QSize(hint.width(),
                     m_tabStripHeight + containerChipSize(index).height());
    }
    return QTabBar::minimumTabSizeHint(index);
}

void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    if (verticalTabShape(shape()))
        layoutRowTabButtons();
    // CONT06: Qt just re-placed every child in strip space — move
    // them below the header band.
    shiftChildrenBelowStrip();
    // TABS15: the '+' trails the last tab — re-anchor it to the new
    // layout (and back below the band shiftChildren just applied).
    layoutNewTabButton();
}

void TabBar::resizeEvent(QResizeEvent *event)
{
    QTabBar::resizeEvent(event);
    // TABS15: a strip resize does not always dirty Qt's tab layout —
    // re-anchor the '+' defensively.
    layoutNewTabButton();
}

// TABS02/RSZ02: ~8px along the strip's content-facing edge is the
// drag grip — the inner side of a Left/Right strip resizes its
// width, the bottom edge of a Top strip (top edge of a Bottom
// strip) resizes its height.  pos is in strip coordinates (the
// band-shifted space tabRect() lives in).
bool TabBar::inResizeGrip(const QPoint &pos) const
{
    static const int grip = 8;
    const int stripHeight = height() - containerStripHeight();
    switch (shape()) {
    case QTabBar::RoundedWest:
    case QTabBar::TriangularWest:
        return pos.x() >= width() - grip;
    case QTabBar::RoundedEast:
    case QTabBar::TriangularEast:
        return pos.x() < grip;
    case QTabBar::RoundedNorth:
    case QTabBar::TriangularNorth:
        return pos.y() >= stripHeight - grip;
    case QTabBar::RoundedSouth:
    case QTabBar::TriangularSouth:
        // Strip-space y is negative inside the container header
        // band — the grip only covers the tab row's own top edge.
        return pos.y() >= 0 && pos.y() < grip;
    default:
        return false;
    }
}

// RSZ02: the grip's painted affordance — a resting edge line plus a
// three-dot handle; hovering or dragging fills the zone with the
// palette's Mid so the strip reads as grabbable, the same feedback
// the sidebar's SidebarGrip gives.  Physical widget coordinates.
void TabBar::paintResizeGrip(QPainter *painter)
{
    static const int grip = 8;
    const int band = containerStripHeight();
    const int stripHeight = height() - band;
    QRect zone;
    switch (shape()) {
    case QTabBar::RoundedWest:
    case QTabBar::TriangularWest:
        zone = QRect(width() - grip, band, grip, stripHeight);
        break;
    case QTabBar::RoundedEast:
    case QTabBar::TriangularEast:
        zone = QRect(0, band, grip, stripHeight);
        break;
    case QTabBar::RoundedNorth:
    case QTabBar::TriangularNorth:
        zone = QRect(0, height() - grip, width(), grip);
        break;
    case QTabBar::RoundedSouth:
    case QTabBar::TriangularSouth:
        zone = QRect(0, band, width(), grip);
        break;
    }
    if (zone.isEmpty())
        return;
    const bool active = m_gripHovered || m_resizing;
    if (active)
        painter->fillRect(zone, palette().color(QPalette::Mid));
    painter->setPen(palette().color(
        active ? QPalette::Dark : QPalette::Mid));
    if (verticalTabShape(shape())) {
        const int x = (shape() == QTabBar::RoundedEast
                       || shape() == QTabBar::TriangularEast)
            ? zone.left() : zone.right();
        painter->drawLine(x, zone.top(), x, zone.bottom());
    } else {
        const int y = (shape() == QTabBar::RoundedSouth
                       || shape() == QTabBar::TriangularSouth)
            ? zone.top() : zone.bottom();
        painter->drawLine(zone.left(), y, zone.right(), y);
    }
    // The handle rides the zone's center — a vertical stack for
    // vertical strips, a horizontal run for horizontal ones.
    painter->setPen(Qt::NoPen);
    painter->setBrush(palette().color(
        active ? QPalette::Dark : QPalette::PlaceholderText));
    const QPointF center = zone.center();
    for (int i = -1; i <= 1; ++i) {
        const QPointF dot = verticalTabShape(shape())
            ? center + QPointF(0, i * 6)
            : center + QPointF(i * 6, 0);
        painter->drawEllipse(dot, 1.6, 1.6);
    }
}

// RSZ02: one write covers both strip dimensions — the drag release
// and the double-click reset share it, and the live-apply fan-out
// re-reads the keys on every other window's bar (reloading a key a
// differently-shaped bar ignores is harmless).
void TabBar::persistStripSize()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("verticalTabWidth"),
                      m_verticalTabWidth);
    settings.setValue(QLatin1String("horizontalTabHeight"),
                      m_tabStripHeight);
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        TabBar *bar = qobject_cast<TabBar*>(widget);
        if (bar && bar != this) {
            bar->reloadVerticalTabWidth();
            bar->reloadTabStripHeight();
        }
    }
}

// TABS02: QTabBar positions per-tab side widgets (favicon label,
// close button) through the style's sub-element rects for the real
// West/East shape — transposed to the tab's top/bottom.  Re-anchor
// them to the row ends the North painting reserves space for.
void TabBar::layoutRowTabButtons()
{
    // CONT06: the option rects are strip coordinates — the widgets
    // they position are physical, so the band height lands here.
    const int band = containerStripHeight();
    for (int index = 0; index < count(); ++index) {
        QStyleOptionTab option;
        initStyleOption(&option, index);
        option.shape = QTabBar::RoundedNorth;
        // The dragged tab's buttons ride the floating row.
        if (m_moveDragActive && m_moveDragVertical
            && index == m_pressedTabIndex)
            option.rect.moveTop(m_moveDragMouseY - m_moveDragGrabOffset);
        // TABS17: the widgets center on the row's label band — a
        // group pill's reserved top is not theirs to sit under.
        option.rect = rowLabelRect(option.rect, index);
        const QTabBar::ButtonPosition sides[2] = {
            QTabBar::LeftSide, QTabBar::RightSide };
        for (const QTabBar::ButtonPosition side : sides) {
            QWidget *button = tabButton(index, side);
            if (!button)
                continue;
            const QStyle::SubElement element =
                (side == QTabBar::LeftSide)
                    ? QStyle::SE_TabBarTabLeftButton
                    : QStyle::SE_TabBarTabRightButton;
            const QRect rect =
                style()->subElementRect(element, &option, this);
            if (button->size().isEmpty())
                button->resize(button->sizeHint());
            button->move(rect.topLeft() + QPoint(0, band));
        }
    }
}

// TABS02: once Qt's move drag starts it floats a private pixmap
// widget painted with the real (rotated) tab shape — hide it; the
// horizontal floating row paintEvent draws takes its place.
void TabBar::hideNativeMovingTab()
{
    const QObjectList kids = children();
    for (QObject *kid : kids) {
        if (qstrcmp(kid->metaObject()->className(),
                    "QMovableTabWidget") == 0) {
            static_cast<QWidget*>(kid)->hide();
            return;
        }
    }
}

// TABS15: the '+' rides inside the strip, right after the last tab
// in every orientation — trailing the row on Top/Bottom bars, a
// full-width appended row under the last tab on Left/Right rails.
// When the tabs overflow and Qt's scroll buttons appear the button
// parks just inside them so it never disappears.  Runs on every
// layout change / resize / paint so tab moves, scrolls and strip
// resizes all re-anchor it.  Physical widget coordinates — the
// CONT06 band offset lands in the vertical cases.
void TabBar::layoutNewTabButton()
{
    if (!m_newTabButton)
        return;
    const int band = containerStripHeight();
    const bool vertical = verticalTabShape(shape());
    // The strip's free run along the tab axis — Qt's scroll buttons
    // are the only non-tab QAbstractButton children, so whatever
    // ends they take clip the space the '+' may use.
    int leading = vertical ? band : 0;
    int trailing = vertical ? height() : width();
    const QTabBar::ButtonPosition sides[2] = { QTabBar::LeftSide,
                                               QTabBar::RightSide };
    const QObjectList kids = children();
    for (QObject *kid : kids) {
        QAbstractButton *child = qobject_cast<QAbstractButton*>(kid);
        if (!child || !child->isVisible() || child == m_newTabButton)
            continue;
        bool isTabButton = false;
        for (int i = 0; i < count() && !isTabButton; ++i) {
            for (const QTabBar::ButtonPosition side : sides) {
                if (tabButton(i, side) == child) {
                    isTabButton = true;
                    break;
                }
            }
        }
        if (isTabButton)
            continue;
        const QRect geo = child->geometry();
        if (vertical) {
            const int mid = (band + height()) / 2;
            if (geo.center().y() > mid)
                trailing = qMin(trailing, geo.top() - 2);
            else
                leading = qMax(leading, geo.bottom() + 3);
        } else {
            const int mid = width() / 2;
            if (geo.center().x() > mid)
                trailing = qMin(trailing, geo.left() - 2);
            else
                leading = qMax(leading, geo.right() + 3);
        }
    }
    const QRect last = count() ? tabRect(count() - 1) : QRect();
    QRect rect;
    if (vertical) {
        const int extent =
            qBound(20, m_newTabButton->sizeHint().height(), 30);
        const int anchor = count() ? band + last.bottom() + 3
                                   : band + 3;
        rect = QRect(2, qBound(leading, anchor,
                               qMax(leading, trailing - extent)),
                     qMax(16, width() - 4), extent);
    } else {
        const int strip = height() - band;
        const int extent = qBound(18, strip - 6, 30);
        const bool rtl = isRightToLeft();
        const int anchor = count()
            ? (rtl ? last.left() - 4 - extent : last.right() + 4)
            : (rtl ? trailing - extent - 3 : leading + 3);
        rect = QRect(qBound(leading, anchor,
                            qMax(leading, trailing - extent)),
                     band + qMax(0, (strip - extent) / 2),
                     extent, extent);
    }
    if (m_newTabButton->geometry() != rect)
        m_newTabButton->setGeometry(rect);
    // Overlap is legal geometry (an overflowing strip leaves no
    // sliver) — keep the '+' above the tab buttons it may cover.
    m_newTabButton->raise();
    m_newTabButton->show();
}

int TabBar::verticalTabWidth() const
{
    return m_verticalTabWidth;
}

void TabBar::setVerticalTabWidth(int width)
{
    width = qBound(120, width, 400);
    if (m_verticalTabWidth == width)
        return;
    m_verticalTabWidth = width;
    // QTabBar caches the laid-out tab rects (what sizeHint() unions)
    // until its private layoutDirty flag is set — a no-op setIconSize
    // is the only public setter that re-arms it without a visual
    // change, so the next sizeHint() sees the new strip width. It
    // also posts this bar's updateGeometry().
    setIconSize(iconSize());
    // The enclosing QTabWidget sizes the strip from the bar's
    // sizeHint inside its private layout — a LayoutRequest on it is
    // what re-runs that layout (QTabWidget::event -> setUpLayout).
    if (QWidget *tabWidget = parentWidget())
        tabWidget->updateGeometry();
    update();
}

void TabBar::reloadVerticalTabWidth()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    const QVariant stored =
        settings.value(QLatin1String("verticalTabWidth"));
    // TABS16: an absent key or a stored past default (TABS10's
    // compact 140 — the pre-TABS10 180 IS the default again) means
    // the grip was never dragged to a pick — migrate forward; a
    // user-dragged width stays.
    setVerticalTabWidth(!stored.isValid()
                            || stored.toInt() == compactVerticalTabWidth
                        ? defaultVerticalTabWidth
                        : stored.toInt());
}

int TabBar::tabMinWidth() const
{
    return m_tabMinWidth;
}

int TabBar::tabMaxWidth() const
{
    return m_tabMaxWidth;
}

void TabBar::setTabWidths(int minimum, int maximum)
{
    minimum = qBound(0, minimum, 400);
    maximum = qBound(0, maximum, 800);
    if (m_tabMinWidth == minimum && m_tabMaxWidth == maximum)
        return;
    m_tabMinWidth = minimum;
    m_tabMaxWidth = maximum;
    // Same layout nudge setVerticalTabWidth() uses — QTabBar only
    // re-plans the strip when a public setter trips its dirty flag.
    setIconSize(iconSize());
    if (QWidget *tabWidget = parentWidget())
        tabWidget->updateGeometry();
    update();
}

int TabBar::tabStripHeight() const
{
    return m_tabStripHeight;
}

void TabBar::setTabStripHeight(int height)
{
    // 0 = Automatic — the TABS07 metrics-bound height drives again.
    height = height <= 0 ? 0 : qBound(20, height, 64);
    if (m_tabStripHeight == height)
        return;
    m_tabStripHeight = height;
    // Same layout nudge setVerticalTabWidth() uses — QTabBar only
    // re-plans the strip when a public setter trips its dirty flag.
    setIconSize(iconSize());
    if (QWidget *tabWidget = parentWidget())
        tabWidget->updateGeometry();
    update();
}

void TabBar::reloadTabWidths()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    setTabWidths(
        settings.value(QLatin1String("tabMinWidth"), 0).toInt(),
        settings.value(QLatin1String("tabMaxWidth"), 0).toInt());
}

void TabBar::reloadTabStripHeight()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    setTabStripHeight(
        settings.value(QLatin1String("horizontalTabHeight"), 0).toInt());
}

bool TabBar::tabPreviewsEnabled() const
{
    return m_tabPreviewsEnabled;
}

void TabBar::setTabPreviewsEnabled(bool enabled)
{
    if (m_tabPreviewsEnabled == enabled)
        return;
    m_tabPreviewsEnabled = enabled;
    // Opting out takes down a live card and disarms the dwell clock
    // immediately — no trailing popup after the pref flips.
    if (!enabled) {
        m_previewTimer->stop();
        hideTabPreview();
    }
}

void TabBar::reloadTabHoverPreviews()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    setTabPreviewsEnabled(
        settings.value(QLatin1String("tabHoverPreviews"), true).toBool());
}

bool TabBar::iconOnlyTabs() const
{
    return m_iconOnlyTabs;
}

QAction *TabBar::iconOnlyTabsAction() const
{
    return m_iconOnlyTabsAction;
}

// The action's trigger is the only writer — the bar applies it, the
// persisted key fans out to every window's strip (persistStripSize's
// all-widgets pattern), so a flip on one window reaches them all.
void TabBar::toggleIconOnlyMode()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("iconOnlyTabs"), !m_iconOnlyTabs);
    const QWidgetList widgets = qApp->allWidgets();
    for (QWidget *widget : widgets) {
        TabBar *bar = qobject_cast<TabBar*>(widget);
        if (bar)
            bar->reloadIconOnlyTabs();
    }
}

void TabBar::setIconOnlyTabs(bool enabled)
{
    if (m_iconOnlyTabs == enabled)
        return;
    m_iconOnlyTabs = enabled;
    m_iconOnlyTabsAction->setChecked(enabled);
    // Same layout re-arm setVerticalTabWidth()/setTabStripHeight()
    // use — every tab's width hint flipped, so the strip re-plans.
    setIconSize(iconSize());
    if (QWidget *tabWidget = parentWidget())
        tabWidget->updateGeometry();
    update();
}

void TabBar::reloadIconOnlyTabs()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    setIconOnlyTabs(
        settings.value(QLatin1String("iconOnlyTabs"), false).toBool());
}

void TabBar::reloadTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        int index = action->data().toInt();
        emit reloadTab(index);
    }
}

// TABS12: the inline rename editor — a QLineEdit parented to the bar
// and laid over the tab's rect.  tabRect() is strip space: when the
// CONT06 level-1 band is up the tab rows sit containerStripHeight()
// pixels lower in the widget, so the editor shifts by the same amount.
void TabBar::editTabTitle(int index)
{
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || index < 0 || index >= count())
        return;
    finishTabTitleEdit(false);   // one editor at a time
    QLineEdit *editor = new QLineEdit(this);
    editor->setObjectName(QLatin1String("tabTitleEditor"));
    editor->setText(tabWidget->tabDisplayTitle(index));
    editor->selectAll();
    QRect rect = tabRect(index).adjusted(2, 0, -2, 0);
    rect.translate(0, containerStripHeight());
    editor->setGeometry(rect);
    m_titleEditor = editor;
    m_editPage = tabWidget->widget(index);
    editor->installEventFilter(this);
    // editingFinished fires on Enter AND on focus-out — both commit.
    connect(editor, &QLineEdit::editingFinished,
            this, [this]() { finishTabTitleEdit(true); });
    editor->show();
    editor->setFocus(Qt::OtherFocusReason);
}

void TabBar::finishTabTitleEdit(bool commit)
{
    QLineEdit *editor = m_titleEditor;
    if (!editor)
        return;
    m_titleEditor = nullptr;
    QWidget *page = m_editPage;
    m_editPage = nullptr;
    const QString text = editor->text();
    editor->removeEventFilter(this);
    editor->deleteLater();
    if (!commit)
        return;
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget || !page)
        return;
    // Resolve the slot at commit time — a mid-edit drag or reorder
    // may have moved the tab.
    const int index = tabWidget->indexOf(page);
    if (index >= 0)
        tabWidget->setTabTitleOverride(index, text);
}

bool TabBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleEditor
        && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            finishTabTitleEdit(false);
            return true;
        }
    }
    return QTabBar::eventFilter(watched, event);
}

void TabBar::tabInserted(int position)
{
    if (m_perTabCloseButtons)
        installCloseButton(position);
    updateCloseButtonVisibility();
    updateVisibility();
    updateAccessibleStrip();
}

void TabBar::tabRemoved(int position)
{
    Q_UNUSED(position);
    // Indices shifted — recompute what (if anything) the cursor is
    // over now that the tab under it may be a different one.
    m_hoveredTab = -1;
    updateHoveredTab(stripPos(mapFromGlobal(QCursor::pos())));
    // TABS12: the strip re-lays out after a removal — drop a live
    // rename editor rather than leave it floating over the wrong row.
    finishTabTitleEdit(false);
    updateAccessibleStrip();
    updateCloseButtonVisibility();
    updateVisibility();
}

void TabBar::updateVisibility()
{
    // Any strip mutation (insert/remove/visibility toggle lands here)
    // invalidates the card's anchor — drop it and re-arm on the next
    // hover change.
    m_previewTimer->stop();
    hideTabPreview();
    // TABGRP01: a collapsed group can leave a single chip on the strip
    // — the bar must stay reachable so the chip can be re-expanded.
    // CONT06: a live header band keeps the bar up at any tab count —
    // the headers are the only way back to the filtered-out levels.
    bool collapsedGroups = false;
    if (TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget()))
        collapsedGroups = tabWidget->hasCollapsedTabGroup();
    setVisible((count()) > 1 || m_showTabBarWhenOneTab || collapsedGroups
               || containerStripHeight() > 0);
    bool enabled = (count() == 1);
    if (m_viewTabBarAction->isEnabled() != enabled)
        m_viewTabBarAction->setEnabled(enabled);
    updateViewToolBarAction();
}

