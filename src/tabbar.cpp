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

// Defined further down — the TABGRP01 drop-zone check in
// mouseReleaseEvent needs it too.
static bool verticalTabShape(QTabBar::Shape shape);

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
    , m_showTabBarWhenOneTab(true)
    , m_perTabCloseButtons(false)
    , m_hoveredTab(-1)
    , m_preview(nullptr)
    , m_previewTimer(nullptr)
    , m_verticalTabWidth(180)
    , m_resizing(false)
    , m_resizeStartX(0)
    , m_resizeStartWidth(0)
    , m_pressedTabIndex(-1)
    , m_moveDragActive(false)
    , m_moveDragVertical(false)
    , m_moveDragMouseY(0)
    , m_moveDragGrabOffset(0)
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

    // TABS02: persisted Left/Right strip width.
    reloadVerticalTabWidth();
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

void TabBar::updateCloseButtonVisibility()
{
    if (!m_perTabCloseButtons)
        return;
    const int current = currentIndex();
    const QTabBar::ButtonPosition side = closeButtonSide();
    for (int i = 0; i < count(); ++i) {
        if (QWidget *button = tabButton(i, side))
            button->setVisible(i == current || i == m_hoveredTab);
    }
}

void TabBar::leaveEvent(QEvent *event)
{
    if (cursor().shape() == Qt::SizeHorCursor)
        unsetCursor();
    if (m_hoveredTab != -1) {
        m_hoveredTab = -1;
        updateCloseButtonVisibility();
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
    QMenu menu;
    TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
    if (!tabWidget)
        return;

    menu.addAction(tabWidget->newTabAction());
    int index = tabAt(position);
    if (-1 != index) {
        QAction *action = menu.addAction(tr("Duplicate Tab"),
                                         this, QOverload<>::of(&TabBar::cloneTab));
        action->setData(index);

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
    } else {
        menu.addSeparator();
    }
    menu.addAction(tr("Reload All Tabs"), this, &TabBar::reloadAllTabs);
    menu.addSeparator();
    menu.addAction(tabWidget->bookmarkTabsAction());
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
    if (event->button() == Qt::LeftButton
        && tabAt(event->position().toPoint()) == -1) {
        emit newTab();
        return;
    }
    QTabBar::mouseDoubleClickEvent(event);
}

void TabBar::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_pressedTabIndex = -1;
        m_moveDragActive = false;
        // TABS02: end of an inner-edge width drag — persist the width
        // and fan it out to every live window's bar, the same
        // live-apply path TABS01's position uses.
        if (m_resizing) {
            m_resizing = false;
            if (cursor().shape() == Qt::SizeHorCursor)
                unsetCursor();
            QSettings settings;
            settings.beginGroup(QLatin1String("tabs"));
            settings.setValue(QLatin1String("verticalTabWidth"),
                              m_verticalTabWidth);
            const QWidgetList widgets = qApp->allWidgets();
            for (QWidget *widget : widgets) {
                TabBar *bar = qobject_cast<TabBar*>(widget);
                if (bar && bar != this)
                    bar->reloadVerticalTabWidth();
            }
            return;
        }
    }
    if (event->button() == Qt::MiddleButton) {
        int index = tabAt(event->position().toPoint());
        if (index != -1) {
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
            const QPoint pos = event->position().toPoint();
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

    QTabBar::mouseReleaseEvent(event);
}

void TabBar::mousePressEvent(QMouseEvent *event)
{
    m_previewTimer->stop();
    hideTabPreview();
    if (event->button() == Qt::LeftButton) {
        // TABS02: a press in the strip's inner-edge grip zone starts
        // a width drag, not a tab press — QTabBar never sees it, so
        // it can neither select the tab nor arm a move drag.
        if (verticalTabShape(shape())
            && inResizeGrip(event->position().toPoint())) {
            m_resizing = true;
            m_resizeStartX = event->globalPosition().toPoint().x();
            m_resizeStartWidth = m_verticalTabWidth;
            setCursor(Qt::SizeHorCursor);
            return;
        }
        m_dragStartPos = event->position().toPoint();
        // TABS02: track the pressed tab so paintEvent can float it
        // horizontally during a move drag.
        m_pressedTabIndex = tabAt(m_dragStartPos);
        m_moveDragGrabOffset = m_pressedTabIndex >= 0
            ? m_dragStartPos.y() - tabRect(m_pressedTabIndex).top()
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
    const QPoint pos = event->position().toPoint();
    // TABS02: an active inner-edge width drag eats the move — the
    // strip resizes instead of the press acting on a tab.
    if (m_resizing) {
        const int dx = event->globalPosition().toPoint().x()
            - m_resizeStartX;
        // West bars widen rightwards (their grip is the right edge),
        // East bars widen leftwards.
        const bool east = (shape() == QTabBar::RoundedEast
                           || shape() == QTabBar::TriangularEast);
        setVerticalTabWidth(m_resizeStartWidth + (east ? -dx : dx));
        return;
    }
    // TABS02: the grip zone advertises itself with a resize cursor.
    if (!(event->buttons() & Qt::LeftButton)
        && verticalTabShape(shape())) {
        if (inResizeGrip(pos))
            setCursor(Qt::SizeHorCursor);
        else if (cursor().shape() == Qt::SizeHorCursor)
            unsetCursor();
    }

    updateHoveredTab(pos);
    if (event->buttons() == Qt::LeftButton) {
        const QPoint diff = pos - m_dragStartPos;
        // "Tear the tab off" = drag away from the bar, perpendicular
        // to the tab strip; the direction depends on which edge the
        // bar sits on (drag up for North, right for East, ...).
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
            && away) {
            QDrag *drag = new QDrag(this);
            QMimeData *mimeData = new QMimeData;
            QList<QUrl> urls;
            int index = tabAt(event->position().toPoint());
            QUrl url = tabData(index).toUrl();
            urls.append(url);
            mimeData->setUrls(urls);
            mimeData->setText(tabText(index));
            mimeData->setData(QLatin1String("action"), "tab-reordering");
            drag->setMimeData(mimeData);
            // TABGRP01: a tear-off leaves the bar — the in-bar
            // drop-stack tracking stops here.
            m_dragTracking = false;
            // TABS02: and so does the move-drag tracking — the bar is
            // about to spend the drag inside QDrag::exec().
            m_moveDragActive = false;
            m_pressedTabIndex = -1;
            drag->exec();
        }
    }
    QTabBar::mouseMoveEvent(event);
    // TABS02: mirror Qt's move-drag lifecycle — past the drag
    // threshold Qt floats its own rotated tab pixmap; hide that
    // widget and let paintEvent float a horizontal row instead.
    if (m_pressedTabIndex >= 0
        && event->buttons() & Qt::LeftButton) {
        const QPoint diff = pos - m_dragStartPos;
        if (!m_moveDragActive
            && diff.manhattanLength() > QApplication::startDragDistance())
            m_moveDragActive = true;
        if (m_moveDragActive) {
            m_moveDragVertical = verticalTabShape(shape())
                && qAbs(diff.y()) > qAbs(diff.x());
            m_moveDragMouseY = pos.y();
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

void TabBar::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    QUrl url;
    if (mimeData->hasUrls())
        url = mimeData->urls().at(0);
    else if (mimeData->hasText())
        url = QUrl::fromEncoded(mimeData->text().toUtf8());

    if (!url.isEmpty() && url.isValid()) {
        event->acceptProposedAction();
        int index = tabAt(event->position().toPoint());
        if (-1 != index) {
            setCurrentIndex(index);
            emit loadUrl(url, TabWidget::CurrentTab);
        } else {
            emit loadUrl(url, TabWidget::NewSelectedTab);
        }
    }

    QTabBar::dropEvent(event);
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
    if (m_hoveredTab < 0) {
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
        || (QApplication::mouseButtons() & Qt::LeftButton)) {
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

// The extra size a container chip reserves for the tab at index —
// the values match the geometry paintEvent() draws: the 3px outer-edge
// strip plus the name chip's height (vertical bars grow wider for the
// side strip and taller for the chip across the tab's top).
QSize TabBar::containerChipSize(int index) const
{
    const ContainerManager::Container c = containerForTab(index);
    if (c.id.isEmpty())
        return QSize();
    const int chipHeight = QFontMetrics(containerChipFont()).height() + 4;
    if (verticalTabShape(shape()))
        return QSize(3, chipHeight + 2);
    return QSize(0, chipHeight + 3);
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

// CONT02: after the style paints the tab, container tabs get their
// accent — a strip on the tab's outer edge (top for a horizontal bar,
// the free side for a vertical one) plus a name chip just inside it.
void TabBar::paintEvent(QPaintEvent *event)
{
    if (verticalTabShape(shape())) {
        // TABS02: rows, not rotated tabs — each vertical tab paints
        // through the horizontal North shape so its title stays
        // readable.  The side-button widgets (favicon label, close
        // button) are re-anchored to the row ends first; QTabBar's
        // own layout positions them transposed for West/East shapes.
        layoutRowTabButtons();
        QPainter painter(this);
        painter.fillRect(event->rect(),
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
            // initStyleOption already elided the text against the
            // vertical shape's narrow text rect — redo it for the
            // row's width.
            const QRect textRect = style()->subElementRect(
                QStyle::SE_TabBarTabText, &option, this);
            option.text = fontMetrics().elidedText(
                tabText(index), elideMode(), textRect.width(),
                Qt::TextShowMnemonic);
            if (!(option.state & QStyle::State_Enabled))
                option.palette.setCurrentColorGroup(QPalette::Disabled);
            style()->drawControl(QStyle::CE_TabBarTab, &option,
                                 &painter, this);
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
            const QRect textRect = style()->subElementRect(
                QStyle::SE_TabBarTabText, &option, this);
            option.text = fontMetrics().elidedText(
                tabText(m_pressedTabIndex), elideMode(),
                textRect.width(), Qt::TextShowMnemonic);
            style()->drawControl(QStyle::CE_TabBarTab, &option,
                                 &painter, this);
        }
        // A divider along the strip's inner edge doubles as the
        // resize grip's affordance.
        painter.setPen(palette().color(QPalette::Mid));
        if (shape() == QTabBar::RoundedEast
            || shape() == QTabBar::TriangularEast)
            painter.drawLine(0, 0, 0, height());
        else
            painter.drawLine(width() - 1, 0, width() - 1, height());
    } else {
        QTabBar::paintEvent(event);
    }

    const QFont chipFont = containerChipFont();
    const int chipHeight = QFontMetrics(chipFont).height() + 4;
    const int strip = 3;

    QPainter painter(this);
    for (int index = 0; index < count(); ++index) {
        const ContainerManager::Container container = containerForTab(index);
        if (container.id.isEmpty())
            continue;
        const QColor accent = container.color.isValid()
            ? container.color : palette().color(QPalette::Highlight);
        const QRect rect = tabRect(index);

        QRect chip;
        switch (shape()) {
        case QTabBar::RoundedSouth:
        case QTabBar::TriangularSouth:
            painter.fillRect(rect.left(), rect.bottom() - strip + 1,
                             rect.width(), strip, accent);
            chip = QRect(rect.left() + 4, rect.bottom() - chipHeight - strip,
                         rect.width() - 8, chipHeight);
            break;
        case QTabBar::RoundedWest:
        case QTabBar::TriangularWest:
            painter.fillRect(rect.left(), rect.top(),
                             strip, rect.height(), accent);
            chip = QRect(rect.left() + strip + 2, rect.top() + 2,
                         rect.width() - strip - 4, chipHeight);
            break;
        case QTabBar::RoundedEast:
        case QTabBar::TriangularEast:
            painter.fillRect(rect.right() - strip + 1, rect.top(),
                             strip, rect.height(), accent);
            chip = QRect(rect.left() + 2, rect.top() + 2,
                         rect.width() - strip - 4, chipHeight);
            break;
        default:
            painter.fillRect(rect.left(), rect.top(),
                             rect.width(), strip, accent);
            chip = QRect(rect.left() + 4, rect.top() + strip,
                         rect.width() - 8, chipHeight);
            break;
        }

        // The name chip — a rounded pill in the container color.  On a
        // very narrow tab only the strip shows (the accent still
        // identifies the container).
        if (chip.width() >= 24 && !container.name.isEmpty()) {
            painter.save();
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setBrush(accent);
            painter.setPen(accent.darker(130));
            painter.drawRoundedRect(chip, 3, 3);
            painter.setFont(chipFont);
            painter.setPen(accent.lightness() > 140 ? Qt::black : Qt::white);
            const QString elided = painter.fontMetrics().elidedText(
                container.name, Qt::ElideRight, chip.width() - 8);
            painter.drawText(chip.adjusted(4, 0, -4, 0),
                             Qt::AlignCenter | Qt::AlignVCenter, elided);
            painter.restore();
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
            if (name.isEmpty() || !isFirstInGroupRun(index))
                continue;
            const int pillWidth = QFontMetrics(chipFont)
                .horizontalAdvance(name) + 12;
            QRect pill;
            switch (shape()) {
            case QTabBar::RoundedWest:
            case QTabBar::TriangularWest: {
                // A container chip may already own the top band —
                // the group pill slides under it.
                const int top = rect.top() + 2
                    + (containerForTab(index).id.isEmpty()
                       ? 0 : chipHeight + 2);
                pill = QRect(rect.left() + 2, top,
                             rect.width() - strip - 4, chipHeight);
                break;
            }
            case QTabBar::RoundedEast:
            case QTabBar::TriangularEast: {
                const int top = rect.top() + 2
                    + (containerForTab(index).id.isEmpty()
                       ? 0 : chipHeight + 2);
                pill = QRect(rect.left() + strip + 2, top,
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
}

QSize TabBar::tabSizeHint(int index) const
{
    QSize sizeHint = QTabBar::tabSizeHint(index);
    QFontMetrics fm = fontMetrics();
    const int extent = fm.horizontalAdvance(QLatin1Char('M')) * 18;
    // QTabBar::tabSizeHint returns a transposed size for vertical
    // shapes — bound the tab's long axis either way so a long title
    // can't stretch the whole strip, and floor the strip's thickness so
    // tabs keep modern breathing room (UIP01).  CONT02: container tabs
    // reserve their chip's extent on top of that.
    const int thickness = fm.height() + 10;
    const QSize chip = containerChipSize(index);
    const QString gid = groupIdForTab(index);
    // TABS02: vertical strips lay out as horizontal rows — the
    // strip's width is the persisted drag-resizable width and each
    // row is a single text line tall.  Container tabs keep their
    // outer-edge strip + top chip band (containerChipSize already
    // carries them); a group run's first member keeps the band the
    // name pill paints into.
    if (verticalTabShape(shape())) {
        int rowHeight = thickness;
        if (!gid.isEmpty()) {
            TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget());
            if (tabWidget && isFirstInGroupRun(index)
                && !tabWidget->tabGroupName(gid).isEmpty())
                rowHeight += QFontMetrics(containerChipFont()).height() + 6;
        }
        return QSize(m_verticalTabWidth + qMax(0, chip.width()),
                     rowHeight + qMax(0, chip.height()));
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
    return QSize(qMin(sizeHint.width(), extent),
                 qMax(sizeHint.height(), thickness) + chip.height());
}

QSize TabBar::minimumTabSizeHint(int index) const
{
    // TABS02: rows never compress below their line height — when the
    // strip overflows, the scroll buttons take over.
    if (verticalTabShape(shape()))
        return tabSizeHint(index);
    return QTabBar::minimumTabSizeHint(index);
}

void TabBar::tabLayoutChange()
{
    QTabBar::tabLayoutChange();
    if (verticalTabShape(shape()))
        layoutRowTabButtons();
}

// TABS02: ~5px along the strip's content-facing edge is the width
// drag grip — right edge for a Left (West) strip, left edge for a
// Right (East) strip.
bool TabBar::inResizeGrip(const QPoint &pos) const
{
    static const int grip = 5;
    switch (shape()) {
    case QTabBar::RoundedWest:
    case QTabBar::TriangularWest:
        return pos.x() >= width() - grip;
    case QTabBar::RoundedEast:
    case QTabBar::TriangularEast:
        return pos.x() < grip;
    default:
        return false;
    }
}

// TABS02: QTabBar positions per-tab side widgets (favicon label,
// close button) through the style's sub-element rects for the real
// West/East shape — transposed to the tab's top/bottom.  Re-anchor
// them to the row ends the North painting reserves space for.
void TabBar::layoutRowTabButtons()
{
    for (int index = 0; index < count(); ++index) {
        QStyleOptionTab option;
        initStyleOption(&option, index);
        option.shape = QTabBar::RoundedNorth;
        // The dragged tab's buttons ride the floating row.
        if (m_moveDragActive && m_moveDragVertical
            && index == m_pressedTabIndex)
            option.rect.moveTop(m_moveDragMouseY - m_moveDragGrabOffset);
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
            button->move(rect.topLeft());
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
    setVerticalTabWidth(
        settings.value(QLatin1String("verticalTabWidth"), 180).toInt());
}

void TabBar::reloadTab()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        int index = action->data().toInt();
        emit reloadTab(index);
    }
}

void TabBar::tabInserted(int position)
{
    if (m_perTabCloseButtons)
        installCloseButton(position);
    updateCloseButtonVisibility();
    updateVisibility();
}

void TabBar::tabRemoved(int position)
{
    Q_UNUSED(position);
    // Indices shifted — recompute what (if anything) the cursor is
    // over now that the tab under it may be a different one.
    m_hoveredTab = -1;
    updateHoveredTab(mapFromGlobal(QCursor::pos()));
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
    bool collapsedGroups = false;
    if (TabWidget *tabWidget = qobject_cast<TabWidget*>(parentWidget()))
        collapsedGroups = tabWidget->hasCollapsedTabGroup();
    setVisible((count()) > 1 || m_showTabBarWhenOneTab || collapsedGroups);
    bool enabled = (count() == 1);
    if (m_viewTabBarAction->isEnabled() != enabled)
        m_viewTabBarAction->setEnabled(enabled);
    updateViewToolBarAction();
}

