/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2008 Jason A. Donenfeld <Jason@zx2c4.com>
 * Copyright 2008 Ariya Hidayat <ariya.hidayat@gmail.com>
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

#include "browsermainwindow.h"

#include "aboutdialog.h"
#include "addbookmarkdialog.h"
#include "aroraicon.h"
#include "autosaver.h"
#include "bookmarksdialog.h"
#include "bookmarksmanager.h"
#include "bookmarksmenu.h"
#include "bookmarksmodel.h"
#include "bookmarkstoolbar.h"
#include "browserapplication.h"
#include "clearprivatedata.h"
#include "commandpalette.h"
#include "containermanager.h"
#include "devtoolswindow.h"
#include "downloadmanager.h"
#include "engineinterface.h"
#include "history.h"
#include "languagemanager.h"
#include "networkaccessmanager.h"
#include "pictureinpicture.h"
#include "readerbutton.h"
#include "readermode.h"
#include "safetext.h"
#include "securestore.h"
#include "settings.h"
#include "bidipanel.h"
#include "sidebarpanel.h"
#include "sourceviewer.h"
#include "statusbarwidgets.h"
#include "tabbar.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "tormanager.h"
#include "useragentmenu.h"
#include "webview.h"
#include "webviewsearch.h"

#include <qdockwidget.h>
#include <qevent.h>
#include <qfiledialog.h>
#include <qlabel.h>
#include <qprintdialog.h>
#include <qprintpreviewdialog.h>
#include <qprinter.h>
#include <qscreen.h>
#include <qsettings.h>
#include <qstringconverter.h>
#include <qmenubar.h>
#include <qmessagebox.h>
#include <qstatusbar.h>
#include <qtoolbar.h>
#include <qinputdialog.h>
#include <qlineedit.h>
#include <qsplitter.h>

#include <qurl.h>
#include <qwebenginehistory.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

#include <qdebug.h>

BrowserMainWindow::BrowserMainWindow(QWidget *parent, Qt::WindowFlags flags)
    : QMainWindow(parent, flags)
    , m_navigationBar(nullptr)
    , m_navigationSplitter(nullptr)
    , m_readerModeButton(nullptr)
    , m_navReaderAction(nullptr)
    , m_toolbarSearch(nullptr)
#if defined(Q_OS_MACOS)
    , m_bookmarksToolbarFrame(0)
#endif
    , m_bookmarksToolbar(nullptr)
    , m_tabWidget(new TabWidget(this))
    , m_loadingIndicator(new LoadingIndicator(this))
    , m_zoomControl(new ZoomControl(this))
    , m_memIndicator(new MemIndicator(this))
    , m_netIndicator(new NetIndicator(this))
    , m_autoSaver(new AutoSaver(this))
{
    setAttribute(Qt::WA_DeleteOnClose, true);
    statusBar()->setSizeGripEnabled(true);
    // fixes https://bugzilla.mozilla.org/show_bug.cgi?id=219070
    // yes, that's a Firefox bug!
    statusBar()->setLayoutDirection(Qt::LeftToRight);
    setupMenu();
    setupToolBar();

    if (BrowserApplication::isTorMode()) {
        // TOR02: a tor window is private by construction (dedicated
        // OTR profile) — a "New Private Tab" entry would be a no-op
        // there, so it is hidden and disabled.
        m_fileNewPrivateTabAction->setVisible(false);
        m_fileNewPrivateTabAction->setEnabled(false);

        // Unmistakable chrome accent — violet navigation bar + badge,
        // and a "(Tor)" marker in the window title — so a tor window
        // is never mistaken for a normal one at a glance.
        m_navigationBar->setStyleSheet(QLatin1String(
            "QToolBar { background: #4a3372; border: none; }"
            "QToolButton { color: #eee; background: transparent; }"
            "QToolButton:hover { background: #5d4491; }"
            "QToolButton:pressed { background: #3a2a5c; }"));
        QLabel *badge = new QLabel(QLatin1String(" Tor "), m_navigationBar);
        badge->setStyleSheet(QLatin1String(
            "QLabel { color: white; background: #6b4fa3;"
            " border-radius: 6px; padding: 1px 8px; font-weight: bold; }"));
        badge->setToolTip(tr("This window browses through the Tor network"));
        m_navigationBar->addWidget(badge);

        // READ02: the reader button paints its glyph from
        // ButtonText — dark on the violet tor bar.  Lift it to the
        // badge's light text color; the next refresh() picks it up
        // (the button only ever repaints while a page is loaded).
        QPalette readerPalette = m_readerModeButton->palette();
        readerPalette.setColor(QPalette::ButtonText,
                               QColor(238, 238, 238));
        m_readerModeButton->setPalette(readerPalette);

        // TOR04: the live circuit chain — a permanent status-bar
        // label fed by TorManager (GETINFO circuit-status refreshed
        // on 650 CIRC events, the Ready transition, and tab
        // switches).  Added before the load indicator/zoom control so
        // it anchors the permanent group; never created for
        // non-tor windows.
        m_torCircuitLabel = new QLabel(this);
        m_torCircuitLabel->setObjectName(
            QLatin1String("torCircuitLabel"));
        statusBar()->addPermanentWidget(m_torCircuitLabel);
        BrowserApplication *app = BrowserApplication::instance();
        TorManager *tor = app ? app->torManager() : nullptr;
        if (tor) {
            connect(tor, &TorManager::circuitsChanged,
                    this, [this](const QList<TorCircuit> &) {
                updateTorCircuitLabel();
            });
            connect(tor, &TorManager::stateChanged,
                    this, [this](TorManager::State) {
                updateTorCircuitLabel();
            });
            connect(tor, &TorManager::bootstrapProgressChanged,
                    this, [this](int, const QString &) {
                updateTorCircuitLabel();
            });
            connect(m_tabWidget, &QTabWidget::currentChanged,
                    this, [this, tor](int) {
                updateTorCircuitLabel();
                tor->requestCircuitInfo();
            });
            tor->requestCircuitInfo();
        }
        updateTorCircuitLabel();
    }

    QWidget *centralWidget = new QWidget(this);
    BookmarksModel *boomarksModel = BrowserApplication::bookmarksManager()->bookmarksModel();
    m_bookmarksToolbar = new BookmarksToolBar(boomarksModel, this);
    m_bookmarksToolbar->setObjectName(QLatin1String("BookmarksToolbar"));
    // UIP02: bookmark buttons are favicon-sized — pin the bar's icon
    // size so it matches the navigation bar's consistent metrics.
    m_bookmarksToolbar->setIconSize(QSize(16, 16));
    connect(m_bookmarksToolbar,
            QOverload<const QUrl &, const QString &>::of(&BookmarksToolBar::openUrl),
            m_tabWidget, &TabWidget::loadUrlFromUser);
    connect(m_bookmarksToolbar,
            QOverload<const QUrl &, TabWidget::OpenUrlIn, const QString &>::of(&BookmarksToolBar::openUrl),
            m_tabWidget, &TabWidget::loadUrl);

    QVBoxLayout *layout = new QVBoxLayout;
    layout->setSpacing(0);
    layout->setContentsMargins(0, 0, 0, 0);
#if defined(Q_OS_MACOS)
    m_bookmarksToolbarFrame = new QFrame(this);
    m_bookmarksToolbarFrame->setLineWidth(1);
    m_bookmarksToolbarFrame->setMidLineWidth(0);
    m_bookmarksToolbarFrame->setFrameShape(QFrame::HLine);
    m_bookmarksToolbarFrame->setFrameShadow(QFrame::Raised);
    QPalette fp = m_bookmarksToolbarFrame->palette();
    fp.setColor(QPalette::Active, QPalette::Light, QColor(64, 64, 64));
    fp.setColor(QPalette::Active, QPalette::Dark, QColor(192, 192, 192));
    fp.setColor(QPalette::Inactive, QPalette::Light, QColor(135, 135, 135));
    fp.setColor(QPalette::Inactive, QPalette::Dark, QColor(226, 226, 226));
    m_bookmarksToolbarFrame->setAttribute(Qt::WA_MacNoClickThrough, true);
    m_bookmarksToolbarFrame->setPalette(fp);
    layout->addWidget(m_bookmarksToolbarFrame);

    layout->addWidget(m_bookmarksToolbar);
    QPalette p = m_bookmarksToolbar->palette();
    p.setColor(QPalette::Active, QPalette::Window, QColor(150, 150, 150));
    p.setColor(QPalette::Inactive, QPalette::Window, QColor(207, 207, 207));
    m_bookmarksToolbar->setAttribute(Qt::WA_MacNoClickThrough, true);
    m_bookmarksToolbar->setAutoFillBackground(true);
    m_bookmarksToolbar->setPalette(p);
    m_bookmarksToolbar->setBackgroundRole(QPalette::Window);
    m_bookmarksToolbar->setMaximumHeight(19);

    QWidget *w = new QWidget(this);
    w->setMaximumHeight(0);
    layout->addWidget(w); // <- OS X tab widget style bug
#else
    // UIP01: modest padding around the bookmarks row (skipped on macOS
    // where the toolbar is hard-capped to 19px for the unified look).
    m_bookmarksToolbar->setContentsMargins(4, 2, 4, 2);
    addToolBarBreak();
    addToolBar(m_bookmarksToolbar);
#endif
    layout->addWidget(m_tabWidget);
    centralWidget->setLayout(layout);
    setCentralWidget(centralWidget);

    // The dock was created in setupMenu() (its toggle action lives in
    // the View menu); it joins the layout hidden — applySidebarSettings()
    // decides visibility once the persisted state is read.
    addDockWidget(Qt::LeftDockWidgetArea, m_sidebarDock);
    m_sidebarDock->setVisible(false);

    addDockWidget(Qt::RightDockWidgetArea, m_bidiPanelDock);
    m_bidiPanelDock->setVisible(false);

    connect(m_tabWidget, &TabWidget::setCurrentTitle,
            this, &BrowserMainWindow::updateWindowTitle);
    connect(m_tabWidget, &TabWidget::showStatusBarMessage,
            statusBar(), [this](const QString &message) { statusBar()->showMessage(message); });
    connect(m_tabWidget, &TabWidget::linkHovered,
            statusBar(), [this](const QString &link) { statusBar()->showMessage(link); });
    connect(m_tabWidget, &TabWidget::loadProgress,
            this, &BrowserMainWindow::loadProgress);
    connect(m_tabWidget, &TabWidget::tabsChanged,
            m_autoSaver, &AutoSaver::changeOccurred);
    connect(m_tabWidget, &TabWidget::printRequested,
            this, &BrowserMainWindow::printRequested);
    connect(m_tabWidget, &TabWidget::lastTabClosed,
            this, &BrowserMainWindow::lastTabClosed);

    // UIP04: permanent status-bar widgets — page-load timing and a
    // zoom control, both bound to whichever tab is current.
    // SBAR01: the memory/bandwidth pair anchors the group's left edge
    // (Vivaldi-style stats-first ordering); the net indicator hides
    // itself while idle, so the stable widgets sit right of it.
    statusBar()->addPermanentWidget(m_memIndicator);
    statusBar()->addPermanentWidget(m_netIndicator);
    statusBar()->addPermanentWidget(m_loadingIndicator);
    statusBar()->addPermanentWidget(m_zoomControl);
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, [this](int) {
        m_loadingIndicator->setWebView(currentTab());
        m_zoomControl->setWebView(currentTab());
        m_memIndicator->setWebView(currentTab());
        updateReaderState();
    });

    updateWindowTitle();
    loadDefaultState();
    // SIDE01: the settings keys are authoritative over whatever the
    // window-state blob restored for the dock — apply first, then hook
    // persistence so startup writes nothing and a hidden sidebar costs
    // nothing (the panel builds lazily on first show).
    applySidebarSettings();
    connect(m_sidebarDock, &QDockWidget::visibilityChanged,
            this, [this](bool visible) {
        if (visible)
            ensureSidebarPanel();
        QSettings settings;
        settings.setValue(QLatin1String("MainWindow/showSidebar"), visible);
        m_autoSaver->changeOccurred();
    });
    connect(m_sidebarDock, &QDockWidget::dockLocationChanged,
            this, [](Qt::DockWidgetArea area) {
        QSettings settings;
        settings.setValue(QLatin1String("MainWindow/sidebarDockArea"),
                          int(area));
    });
    // DOWN02: a new download no longer pops a separate window — the
    // Downloads section is selected when the dock is already up, and
    // the window flashes so the arrival is still noticeable when the
    // panel is hidden.
    connect(BrowserApplication::downloadManager(),
            &DownloadManager::itemAdded, this, [this]() {
        if (m_sidebarPanel)
            m_sidebarPanel->showDownloads();
        if (isActiveWindow())
            QApplication::alert(this);
    });
    m_tabWidget->newTab();
    m_tabWidget->currentLocationBar()->setFocus();

    // Add each item in the menu bar to the main window so
    // if the menu bar is hidden the shortcuts still work.
    QList<QAction*> actions = menuBar()->actions();
    for (int i = 0; i < actions.count(); ++i) {
        QAction *action = actions.at(i);
        if (action->menu())
            actions += action->menu()->actions();
        addAction(action);
    }
#if defined(Q_OS_MACOS)
    setWindowIcon(QIcon());
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    setWindowRole(QLatin1String("browser"));
#endif
    retranslate();
}

BrowserMainWindow::~BrowserMainWindow()
{
    m_autoSaver->changeOccurred();
    m_autoSaver->saveIfNeccessary();
}

void BrowserMainWindow::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_HomePage:
        m_historyHomeAction->trigger();
        event->accept();
        break;
    case Qt::Key_Favorites:
        m_bookmarksShowAllAction->trigger();
        event->accept();
        break;
    case Qt::Key_Search:
        m_toolsWebSearchAction->trigger();
        event->accept();
        break;
    case Qt::Key_OpenUrl:
        m_fileOpenLocationAction->trigger();
        event->accept();
        break;
    default:
        QMainWindow::keyPressEvent(event);
        break;
    }
}

BrowserMainWindow *BrowserMainWindow::parentWindow(QWidget *widget)
{
    while (widget) {
        if (BrowserMainWindow *parent = qobject_cast<BrowserMainWindow*>(widget))
            return parent;

        widget = widget->parentWidget();
    }

    qWarning() << "BrowserMainWindow::" << __FUNCTION__ << " used with a widget none of whose parents is a main window.";
    // instance() is null when qApp is not a BrowserApplication (autotests).
    if (BrowserApplication *application = BrowserApplication::instance())
        return application->mainWindow();
    return nullptr;
}

void BrowserMainWindow::loadDefaultState()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("BrowserMainWindow"));
    QByteArray data = settings.value(QLatin1String("defaultState")).toByteArray();
    restoreState(data);
    settings.endGroup();
}

QSize BrowserMainWindow::sizeHint() const
{
    QScreen *screen = QApplication::primaryScreen();
    if (screen)
        return screen->geometry().size() * 0.9;
    return QSize(1024, 768);
}

void BrowserMainWindow::save()
{
    if (BrowserApplication *application = BrowserApplication::instance())
        application->saveSession();

    QSettings settings;
    settings.beginGroup(QLatin1String("BrowserMainWindow"));
    QByteArray data = saveState(false);
    settings.setValue(QLatin1String("defaultState"), data);
    settings.endGroup();
}

static const qint32 BrowserMainWindowMagic = 0xba;

QByteArray BrowserMainWindow::saveState(bool withTabs) const
{
    int version = 3;
    QByteArray data;
    QDataStream stream(&data, QIODevice::WriteOnly);

    stream << qint32(BrowserMainWindowMagic);
    stream << qint32(version);

    // save the normal size so exiting fullscreen/maximize will work reasonably
    stream << normalGeometry().size();
    stream << !m_navigationBar->isHidden(); // DEAD
    stream << !m_bookmarksToolbar->isHidden(); // DEAD
    stream << !statusBar()->isHidden();
    if (withTabs)
        stream << tabWidget()->saveState();
    else
        stream << QByteArray();
    stream << m_navigationSplitter->saveState();
    stream << m_tabWidget->tabBar()->showTabBarWhenOneTab();

    stream << qint32(toolBarArea(m_navigationBar));
    stream << qint32(toolBarArea(m_bookmarksToolbar));

    // version 3
    stream << isMaximized();
    stream << isFullScreen();
    stream << menuBar()->isVisible();
    stream << m_menuBarVisible; // DEAD
    stream << m_statusBarVisible;

    stream << QMainWindow::saveState();

    return data;
}

bool BrowserMainWindow::restoreState(const QByteArray &state)
{
    QByteArray sd = state;
    QDataStream stream(&sd, QIODevice::ReadOnly);
    if (stream.atEnd())
        return false;

    qint32 marker;
    qint32 version;
    stream >> marker;
    stream >> version;
    if (marker != BrowserMainWindowMagic || !(version == 2 || version == 3))
        return false;

    QSize size;
    bool showToolbarDEAD;
    bool showBookmarksBarDEAD;
    bool showStatusbar;
    QByteArray tabState;
    QByteArray splitterState;
    bool showTabBarWhenOneTab;
    qint32 navigationBarLocation;
    qint32 bookmarkBarLocation;
    bool maximized;
    bool fullScreen;
    bool showMenuBar;
    QByteArray qMainWindowState;

    stream >> size;
    stream >> showToolbarDEAD;
    stream >> showBookmarksBarDEAD;
    stream >> showStatusbar;
    stream >> tabState;
    stream >> splitterState;
    stream >> showTabBarWhenOneTab;
    stream >> navigationBarLocation;
    stream >> bookmarkBarLocation;

    if (version >= 3) {
        stream >> maximized;
        stream >> fullScreen;
        stream >> showMenuBar;
        stream >> showMenuBar; // m_menuBarVisible DEAD
        stream >> m_statusBarVisible;
        stream >> qMainWindowState;
    } else {
        maximized = false;
        fullScreen = false;
        showMenuBar = true;
        m_statusBarVisible = showStatusbar;
    }

    if (stream.status() != QDataStream::Ok)
        return false;

    if (size.isValid())
        resize(size);

    if (maximized)
        setWindowState(windowState() | Qt::WindowMaximized);
    if (fullScreen) {
        setWindowState(windowState() | Qt::WindowFullScreen);
        m_viewFullScreenAction->setChecked(true);
    }

    menuBar()->setVisible(showMenuBar);
    m_menuBarVisible = showMenuBar;

    statusBar()->setVisible(showStatusbar);

    m_navigationSplitter->restoreState(splitterState);

    tabWidget()->restoreState(tabState);

    m_tabWidget->tabBar()->setShowTabBarWhenOneTab(showTabBarWhenOneTab);

    if (qMainWindowState.isEmpty()) {
        m_navigationBar->setVisible(showToolbarDEAD);
        m_bookmarksToolbar->setVisible(showBookmarksBarDEAD);
        Qt::ToolBarArea navigationArea = Qt::ToolBarArea(navigationBarLocation);
        if (navigationArea != Qt::TopToolBarArea && navigationArea != Qt::NoToolBarArea)
            addToolBar(navigationArea, m_navigationBar);
        Qt::ToolBarArea bookmarkArea = Qt::ToolBarArea(bookmarkBarLocation);
        if (bookmarkArea != Qt::TopToolBarArea && bookmarkArea != Qt::NoToolBarArea)
            addToolBar(bookmarkArea, m_bookmarksToolbar);
    } else {
        QMainWindow::restoreState(qMainWindowState);
    }

#if defined(Q_OS_MACOS)
    m_bookmarksToolbarFrame->setVisible(m_bookmarksToolbar->isVisible());
#endif

    return true;
}

void BrowserMainWindow::lastTabClosed()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    bool quit = settings.value(QLatin1String("quitAsLastTabClosed"), true).toBool();

    if (quit)
        close();
    else
        m_tabWidget->makeNewTab(true);
}

QAction *BrowserMainWindow::showMenuBarAction() const
{
    return m_viewShowMenuBarAction;
}

void BrowserMainWindow::setupMenu()
{
    m_menuBarVisible = true;

    new QShortcut(QKeySequence(Qt::Key_F6), this, [this]() { swapFocus(); });

    // File
    m_fileMenu = new QMenu(menuBar());
    menuBar()->addMenu(m_fileMenu);

    m_fileNewWindowAction = new QAction(m_fileMenu);
    m_fileNewWindowAction->setShortcut(QKeySequence::New);
    connect(m_fileNewWindowAction, &QAction::triggered,
            this, &BrowserMainWindow::fileNew);
    m_fileMenu->addAction(m_fileNewWindowAction);
    m_fileMenu->addAction(m_tabWidget->newTabAction());

    // PTAB01: private browsing is per-tab — the off-the-record page
    // lives inside this window alongside normal tabs.
    m_fileNewPrivateTabAction = new QAction(m_fileMenu);
    // Ctrl+Shift+P is the command palette (CMD01); Ctrl+Shift+N is the
    // incognito/private shortcut Chrome popularized.
    m_fileNewPrivateTabAction->setShortcut(
        QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_N));
    connect(m_fileNewPrivateTabAction, &QAction::triggered,
            m_tabWidget, &TabWidget::newPrivateTab);
    m_fileMenu->addAction(m_fileNewPrivateTabAction);

    // TOR02: a Tor window is a separate `arora --tor` process — the
    // application proxy is process-global, so routing cannot be a
    // mode of this window.  Disabled when no tor binary resolves.
    m_fileNewTorWindowAction = new QAction(m_fileMenu);
    connect(m_fileNewTorWindowAction, &QAction::triggered,
            this, []() { BrowserApplication::openTorWindow(); });
    if (TorManager::resolveBinary().isEmpty()) {
        m_fileNewTorWindowAction->setEnabled(false);
        m_fileNewTorWindowAction->setToolTip(
            tr("No tor binary found — install tor or run "
               "BuildProcess/fetch-tor.sh"));
    }
    m_fileMenu->addAction(m_fileNewTorWindowAction);

    // CONT02: container tabs — the submenu lists the registry and is
    // repopulated on open (containers are runtime-editable).  Tor
    // windows hide it — a tor process has no containers.
    m_fileNewContainerTabMenu = new QMenu(m_fileMenu);
    connect(m_fileNewContainerTabMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::populateNewContainerTabMenu);
    m_fileNewContainerTabMenu->menuAction()->setVisible(
        !BrowserApplication::isTorMode());
    m_fileMenu->addMenu(m_fileNewContainerTabMenu);

    m_fileOpenFileAction = new QAction(m_fileMenu);
    m_fileOpenFileAction->setShortcut(QKeySequence::Open);
    connect(m_fileOpenFileAction, &QAction::triggered,
            this, &BrowserMainWindow::fileOpen);
    m_fileMenu->addAction(m_fileOpenFileAction);

    m_fileOpenLocationAction = new QAction(m_fileMenu);
    // Add the location bar shortcuts familiar to users from other browsers
    QList<QKeySequence> openLocationShortcuts;
    openLocationShortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_L));
    openLocationShortcuts.append(QKeySequence(Qt::AltModifier | Qt::Key_O));
    openLocationShortcuts.append(QKeySequence(Qt::AltModifier | Qt::Key_D));
    m_fileOpenLocationAction->setShortcuts(openLocationShortcuts);
    connect(m_fileOpenLocationAction, &QAction::triggered,
            this, &BrowserMainWindow::selectLineEdit);
    m_fileMenu->addAction(m_fileOpenLocationAction);

    m_fileMenu->addSeparator();
    m_fileMenu->addAction(m_tabWidget->closeTabAction());
    m_fileMenu->addSeparator();

    m_fileSaveAsAction = new QAction(m_fileMenu);
    m_fileSaveAsAction->setShortcut(QKeySequence::Save);
    connect(m_fileSaveAsAction, &QAction::triggered,
            this, &BrowserMainWindow::fileSaveAs);
    m_fileMenu->addAction(m_fileSaveAsAction);
    m_fileMenu->addSeparator();

    BookmarksManager *bookmarksManager = BrowserApplication::bookmarksManager();
    m_fileImportBookmarksAction = new QAction(m_fileMenu);
    connect(m_fileImportBookmarksAction, &QAction::triggered,
            bookmarksManager, &BookmarksManager::importBookmarks);
    m_fileMenu->addAction(m_fileImportBookmarksAction);
    m_fileExportBookmarksAction = new QAction(m_fileMenu);
    connect(m_fileExportBookmarksAction, &QAction::triggered,
            bookmarksManager, &BookmarksManager::exportBookmarks);
    m_fileMenu->addAction(m_fileExportBookmarksAction);
    m_fileMenu->addSeparator();

    m_filePrintPreviewAction= new QAction(m_fileMenu);
    connect(m_filePrintPreviewAction, &QAction::triggered,
            this, &BrowserMainWindow::filePrintPreview);
    m_fileMenu->addAction(m_filePrintPreviewAction);

    m_filePrintAction = new QAction(m_fileMenu);
    m_filePrintAction->setShortcut(QKeySequence::Print);
    connect(m_filePrintAction, &QAction::triggered,
            this, &BrowserMainWindow::filePrint);
    m_fileMenu->addAction(m_filePrintAction);
    m_fileMenu->addSeparator();

    // MENU03: Preferences lives in the File menu's bottom group,
    // directly above Close Window (cross-browser convention).
    m_filePreferencesAction = new QAction(m_fileMenu);
    m_filePreferencesAction->setMenuRole(QAction::PreferencesRole);
    connect(m_filePreferencesAction, &QAction::triggered,
            this, &BrowserMainWindow::preferences);
    m_fileMenu->addAction(m_filePreferencesAction);
    m_fileMenu->addSeparator();

    m_fileCloseWindow = new QAction(m_fileMenu);
    connect(m_fileCloseWindow, &QAction::triggered, this, &QWidget::close);
    m_fileCloseWindow->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_W));
    m_fileMenu->addAction(m_fileCloseWindow);

    m_fileQuit = new QAction(m_fileMenu);
    int kdeSessionVersion = QString::fromLocal8Bit(qgetenv("KDE_SESSION_VERSION")).toInt();
    BrowserApplication *application = BrowserApplication::instance();
    if (kdeSessionVersion != 0 || !application)
        connect(m_fileQuit, &QAction::triggered, this, &QWidget::close);
    else
        connect(m_fileQuit, &QAction::triggered, application, &BrowserApplication::quitBrowser);
    m_fileQuit->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_Q));
    m_fileMenu->addAction(m_fileQuit);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_fileNewWindowAction->setIcon(AroraIcon::get(QLatin1String("window-new")));
    m_fileOpenFileAction->setIcon(AroraIcon::get(QLatin1String("document-open")));
    m_filePrintPreviewAction->setIcon(AroraIcon::get(QLatin1String("document-print-preview")));
    m_filePrintAction->setIcon(AroraIcon::get(QLatin1String("document-print")));
    m_fileSaveAsAction->setIcon(AroraIcon::get(QLatin1String("document-save-as")));
    m_fileCloseWindow->setIcon(AroraIcon::get(QLatin1String("window-close")));
    m_fileQuit->setIcon(AroraIcon::get(QLatin1String("application-exit")));
#endif

    // Edit
    m_editMenu = new QMenu(menuBar());
    menuBar()->addMenu(m_editMenu);
    m_editUndoAction = new QAction(m_editMenu);
    m_editUndoAction->setShortcuts(QKeySequence::Undo);
    m_tabWidget->addWebAction(m_editUndoAction, Engine::StandardAction::Undo);
    m_editMenu->addAction(m_editUndoAction);
    m_editRedoAction = new QAction(m_editMenu);
    m_editRedoAction->setShortcuts(QKeySequence::Redo);
    m_tabWidget->addWebAction(m_editRedoAction, Engine::StandardAction::Redo);
    m_editMenu->addAction(m_editRedoAction);
    m_editMenu->addSeparator();
    m_editCutAction = new QAction(m_editMenu);
    m_editCutAction->setShortcuts(QKeySequence::Cut);
    m_tabWidget->addWebAction(m_editCutAction, Engine::StandardAction::Cut);
    m_editMenu->addAction(m_editCutAction);
    m_editCopyAction = new QAction(m_editMenu);
    m_editCopyAction->setShortcuts(QKeySequence::Copy);
    m_tabWidget->addWebAction(m_editCopyAction, Engine::StandardAction::Copy);
    m_editMenu->addAction(m_editCopyAction);
    m_editPasteAction = new QAction(m_editMenu);
    m_editPasteAction->setShortcuts(QKeySequence::Paste);
    m_tabWidget->addWebAction(m_editPasteAction, Engine::StandardAction::Paste);
    m_editMenu->addAction(m_editPasteAction);
    m_editSelectAllAction = new QAction(m_editMenu);
    m_editSelectAllAction->setShortcuts(QKeySequence::SelectAll);
    m_tabWidget->addWebAction(m_editSelectAllAction, Engine::StandardAction::SelectAll);
    m_editMenu->addAction(m_editSelectAllAction);
    m_editMenu->addSeparator();

    m_editFindAction = new QAction(m_editMenu);
    m_editFindAction->setShortcuts(QKeySequence::Find);
    connect(m_editFindAction, &QAction::triggered, this, &BrowserMainWindow::editFind);
    m_editMenu->addAction(m_editFindAction);
    new QShortcut(QKeySequence(Qt::Key_Slash), this, [this]() { editFind(); });

    m_editFindNextAction = new QAction(m_editMenu);
    m_editFindNextAction->setShortcuts(QKeySequence::FindNext);
    connect(m_editFindNextAction, &QAction::triggered, this, &BrowserMainWindow::editFindNext);
    m_editMenu->addAction(m_editFindNextAction);

    m_editFindPreviousAction = new QAction(m_editMenu);
    m_editFindPreviousAction->setShortcuts(QKeySequence::FindPrevious);
    connect(m_editFindPreviousAction, &QAction::triggered, this, &BrowserMainWindow::editFindPrevious);
    m_editMenu->addAction(m_editFindPreviousAction);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_editUndoAction->setIcon(AroraIcon::get(QLatin1String("edit-undo")));
    m_editRedoAction->setIcon(AroraIcon::get(QLatin1String("edit-redo")));
    m_editCutAction->setIcon(AroraIcon::get(QLatin1String("edit-cut")));
    m_editCopyAction->setIcon(AroraIcon::get(QLatin1String("edit-copy")));
    m_editPasteAction->setIcon(AroraIcon::get(QLatin1String("edit-paste")));
    m_editSelectAllAction->setIcon(AroraIcon::get(QLatin1String("edit-select-all")));
    m_editFindAction->setIcon(AroraIcon::get(QLatin1String("edit-find")));
#endif

    // View
    m_viewMenu = new QMenu(menuBar());
    connect(m_viewMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::aboutToShowViewMenu);
    menuBar()->addMenu(m_viewMenu);

    m_viewShowMenuBarAction = new QAction(m_viewMenu);
    m_viewShowMenuBarAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_M));
    connect(m_viewShowMenuBarAction, &QAction::triggered, this, &BrowserMainWindow::viewMenuBar);
    addAction(m_viewShowMenuBarAction);

    m_viewToolbarAction = new QAction(this);
    connect(m_viewToolbarAction, &QAction::triggered, this, &BrowserMainWindow::viewToolbar);
    m_viewMenu->addAction(m_viewToolbarAction);

    m_viewBookmarkBarAction = new QAction(m_viewMenu);
    connect(m_viewBookmarkBarAction, &QAction::triggered, this, &BrowserMainWindow::viewBookmarksBar);
    m_viewMenu->addAction(m_viewBookmarkBarAction);

    QAction *viewTabBarAction = m_tabWidget->tabBar()->viewTabBarAction();
    m_viewMenu->addAction(viewTabBarAction);
    connect(viewTabBarAction, &QAction::toggled,
            m_autoSaver, &AutoSaver::changeOccurred);

    m_viewStatusbarAction = new QAction(m_viewMenu);
    connect(m_viewStatusbarAction, &QAction::triggered, this, &BrowserMainWindow::viewStatusbar);
    m_viewMenu->addAction(m_viewStatusbarAction);

    // SIDE01: optional Vivaldi-style sidebar dock — off by default.
    // The action is the dock's own toggle action so its check state
    // also tracks the dock's close button; applySidebarSettings() is
    // the persisted authority (settings keys, not the window blob).
    m_sidebarDock = new QDockWidget(this);
    m_sidebarDock->setObjectName(QLatin1String("sidebarDock"));
    m_sidebarDock->setAllowedAreas(Qt::LeftDockWidgetArea
                                 | Qt::RightDockWidgetArea);
    m_viewSidebarAction = m_sidebarDock->toggleViewAction();
    m_viewSidebarAction->setShortcut(QKeySequence(Qt::Key_F4));
    m_viewSidebarAction->setIcon(SidebarPanel::icon(this));
    m_viewMenu->addAction(m_viewSidebarAction);

    // DEVT03: dev tools dock — the BiDi panel builds lazily on first
    // show (ensureBidiPanel).  Its toggle action joins the Tools menu
    // next to the Chromium inspector entry further down.
    m_bidiPanelDock = new QDockWidget(this);
    m_bidiPanelDock->setObjectName(QLatin1String("bidiPanelDock"));
    m_bidiPanelDock->setAllowedAreas(Qt::LeftDockWidgetArea
                                   | Qt::RightDockWidgetArea);
    connect(m_bidiPanelDock, &QDockWidget::visibilityChanged,
            this, [this](bool visible) { if (visible) ensureBidiPanel(); });

    m_viewMenu->addSeparator();

    m_viewStopAction = new QAction(m_viewMenu);
    QList<QKeySequence> shortcuts;
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Period));
    shortcuts.append(Qt::Key_Escape);
    m_viewStopAction->setShortcuts(shortcuts);
    m_tabWidget->addWebAction(m_viewStopAction, Engine::StandardAction::Stop);
    m_viewMenu->addAction(m_viewStopAction);

    m_viewReloadAction = new QAction(m_viewMenu);
    shortcuts.clear();
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_R));
    shortcuts.append(QKeySequence(Qt::Key_F5));
    m_viewReloadAction->setShortcuts(shortcuts);
    m_tabWidget->addWebAction(m_viewReloadAction, Engine::StandardAction::Reload);
    m_viewMenu->addAction(m_viewReloadAction);

    m_viewZoomInAction = new QAction(m_viewMenu);
    shortcuts.clear();
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Plus));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Equal));
    m_viewZoomInAction->setShortcuts(shortcuts);
    connect(m_viewZoomInAction, &QAction::triggered,
            this, &BrowserMainWindow::zoomIn);
    m_viewMenu->addAction(m_viewZoomInAction);

    m_viewZoomNormalAction = new QAction(m_viewMenu);
    m_viewZoomNormalAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_0));
    connect(m_viewZoomNormalAction, &QAction::triggered,
            this, &BrowserMainWindow::zoomNormal);
    m_viewMenu->addAction(m_viewZoomNormalAction);

    m_viewZoomOutAction = new QAction(m_viewMenu);
    shortcuts.clear();
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Minus));
    shortcuts.append(QKeySequence(Qt::ControlModifier | Qt::Key_Underscore));
    m_viewZoomOutAction->setShortcuts(shortcuts);
    connect(m_viewZoomOutAction, &QAction::triggered,
            this, &BrowserMainWindow::zoomOut);
    m_viewMenu->addAction(m_viewZoomOutAction);

    m_viewZoomTextOnlyAction = new QAction(m_viewMenu);
    m_viewZoomTextOnlyAction->setCheckable(true);
    connect(m_viewZoomTextOnlyAction, &QAction::toggled,
            this, [](bool checked) { BrowserApplication::setZoomTextOnly(checked); });
    if (BrowserApplication *application = BrowserApplication::instance())
        connect(application, &BrowserApplication::zoomTextOnlyChanged,
                this, &BrowserMainWindow::zoomTextOnlyChanged);
    m_viewMenu->addAction(m_viewZoomTextOnlyAction);

    // READ01: Reader Mode — a clutter-free article overlay.  The
    // action is always shown; updateReaderState() keeps it enabled
    // whenever a tab is present (the extraction itself reports
    // "not available" on non-article pages) and checked while the
    // overlay is up.
    m_viewReaderAction = new QAction(m_viewMenu);
    m_viewReaderAction->setShortcut(
        QKeySequence(Qt::ControlModifier | Qt::AltModifier | Qt::Key_R));
    m_viewReaderAction->setCheckable(true);
    connect(m_viewReaderAction, &QAction::triggered,
            this, [this]() {
        if (currentTab())
            currentTab()->toggleReaderMode();
    });
    m_viewMenu->addAction(m_viewReaderAction);

    // PIP01: Picture-in-Picture pops the page's video into a floating
    // window.  Always enabled with a tab present — popOut() reports
    // "no video" via the status bar when nothing qualifies.
    m_viewPipAction = new QAction(m_viewMenu);
    m_viewPipAction->setCheckable(true);
    connect(m_viewPipAction, &QAction::triggered,
            this, [this]() {
        if (currentTab() && currentTab()->pictureInPicture())
            currentTab()->pictureInPicture()->popOut();
    });
    m_viewMenu->addAction(m_viewPipAction);

    m_viewFullScreenAction = new QAction(m_viewMenu);
    m_viewFullScreenAction->setShortcut(Qt::Key_F11);
    connect(m_viewFullScreenAction, &QAction::triggered,
            this, &BrowserMainWindow::viewFullScreen);
    m_viewFullScreenAction->setCheckable(true);
    m_viewMenu->addAction(m_viewFullScreenAction);


    m_viewMenu->addSeparator();

    m_viewSourceAction = new QAction(m_viewMenu);
    connect(m_viewSourceAction, &QAction::triggered,
            this, &BrowserMainWindow::viewPageSource);
    m_viewMenu->addAction(m_viewSourceAction);

    // DEVT01: engine-facing tools share one submenu next to Page
    // Source.  Qt WebEngine has no DeveloperExtrasEnabled toggle —
    // Chromium DevTools are always available, so the entry opens the
    // inspector on the current page directly.  The bare page action is
    // a no-op until a devToolsPage is bound, so it routes through the
    // shared inspector host (DVT01).
    m_viewDevToolsMenu = new QMenu(m_viewMenu);
    m_viewMenu->addMenu(m_viewDevToolsMenu);

    m_viewChromiumDevToolAction = new QAction(m_viewDevToolsMenu);
    connect(m_viewChromiumDevToolAction, &QAction::triggered,
            this, [this]() {
        if (currentTab())
            DevToolsWindow::inspectElement(currentTab()->page());
    });
    m_viewDevToolsMenu->addAction(m_viewChromiumDevToolAction);

    // Placeholder until DEVT02 wires the BiDi panel — kept visible but
    // disabled so the submenu structure stays stable.
    m_viewBidiDevToolAction = new QAction(m_viewDevToolsMenu);
    m_viewBidiDevToolAction->setEnabled(false);
    m_viewDevToolsMenu->addAction(m_viewBidiDevToolAction);

    m_viewMenu->addSeparator();

    m_viewTextEncodingAction = new QAction(m_viewMenu);
    m_viewMenu->addAction(m_viewTextEncodingAction);
    m_viewTextEncodingMenu = new QMenu(m_viewMenu);
    m_viewTextEncodingAction->setMenu(m_viewTextEncodingMenu);
    connect(m_viewTextEncodingMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::aboutToShowTextEncodingMenu);
    connect(m_viewTextEncodingMenu, &QMenu::triggered,
            this, &BrowserMainWindow::viewTextEncoding);

    m_stopIcon = AroraIcon::get(QLatin1String("process-stop"));
    m_reloadIcon = AroraIcon::get(QLatin1String("view-refresh"));
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_viewStopAction->setIcon(m_stopIcon);
    m_viewReloadAction->setIcon(m_reloadIcon);
    m_viewZoomInAction->setIcon(AroraIcon::get(QLatin1String("zoom-in")));
    m_viewZoomNormalAction->setIcon(AroraIcon::get(QLatin1String("zoom-original")));
    m_viewZoomOutAction->setIcon(AroraIcon::get(QLatin1String("zoom-out")));
    m_viewFullScreenAction->setIcon(AroraIcon::get(QLatin1String("view-fullscreen")));
#endif

    // History
    m_historyMenu = new HistoryMenu(this);
    connect(m_historyMenu, &HistoryMenu::openUrl,
            m_tabWidget, &TabWidget::loadUrlFromUser);
    connect(m_historyMenu, &HistoryMenu::showHistoryPage,
            this, &BrowserMainWindow::showHistoryPage);
    menuBar()->addMenu(m_historyMenu);
    QList<QAction*> historyActions;

    m_historyBackAction = new QAction(this);
    m_tabWidget->addWebAction(m_historyBackAction, Engine::StandardAction::Back);
    m_historyBackAction->setShortcuts(QKeySequence::Back);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_historyBackAction->setIconVisibleInMenu(false);
#endif

    m_historyForwardAction = new QAction(this);
    m_tabWidget->addWebAction(m_historyForwardAction, Engine::StandardAction::Forward);
    m_historyForwardAction->setShortcuts(QKeySequence::Forward);
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_historyForwardAction->setIconVisibleInMenu(false);
#endif

    m_historyHomeAction = new QAction(this);
    connect(m_historyHomeAction, &QAction::triggered, this, &BrowserMainWindow::goHome);
    m_historyHomeAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_H));

    m_historyRestoreLastSessionAction = new QAction(this);
    if (BrowserApplication *app = BrowserApplication::instance()) {
        connect(m_historyRestoreLastSessionAction, &QAction::triggered,
                app, &BrowserApplication::restoreLastSession);
        m_historyRestoreLastSessionAction->setEnabled(app->canRestoreSession());
    } else {
        m_historyRestoreLastSessionAction->setEnabled(false);
    }

    historyActions.append(m_historyBackAction);
    historyActions.append(m_historyForwardAction);
    historyActions.append(m_historyHomeAction);
    historyActions.append(m_tabWidget->recentlyClosedTabsAction());
    historyActions.append(m_historyRestoreLastSessionAction);
    m_historyMenu->setInitialActions(historyActions);
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_historyRestoreLastSessionAction->setIcon(AroraIcon::get(QLatin1String("document-revert")));
    m_historyHomeAction->setIcon(AroraIcon::get(QLatin1String("go-home")));
#endif

    // Bookmarks
    m_bookmarksMenu = new BookmarksMenuBarMenu(this);
    connect(m_bookmarksMenu,
            QOverload<const QUrl &, const QString &>::of(&BookmarksMenu::openUrl),
            m_tabWidget, &TabWidget::loadUrlFromUser);
    connect(m_bookmarksMenu,
            QOverload<const QUrl &, TabWidget::OpenUrlIn, const QString &>::of(&BookmarksMenu::openUrl),
            m_tabWidget, &TabWidget::loadUrl);
    menuBar()->addMenu(m_bookmarksMenu);

    m_bookmarksShowAllAction = new QAction(this);
    m_bookmarksShowAllAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_B));
    connect(m_bookmarksShowAllAction, &QAction::triggered,
            this, &BrowserMainWindow::showBookmarksDialog);

    m_bookmarksAddAction = new QAction(this);
    m_bookmarksAddAction->setIcon(AroraIcon::get(QLatin1String("bookmark-new")));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_bookmarksAddAction->setIconVisibleInMenu(false);
#endif
    connect(m_bookmarksAddAction, &QAction::triggered,
            this, &BrowserMainWindow::addBookmark);
    m_bookmarksAddAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_D));

    m_bookmarksAddFolderAction = new QAction(this);
    connect(m_bookmarksAddFolderAction, &QAction::triggered,
            this, &BrowserMainWindow::addBookmarkFolder);

    QList<QAction*> bookmarksActions;
    bookmarksActions.append(m_bookmarksShowAllAction);
    bookmarksActions.append(m_bookmarksAddAction);
    bookmarksActions.append(tabWidget()->bookmarkTabsAction());
    bookmarksActions.append(m_bookmarksAddFolderAction);
    m_bookmarksMenu->setInitialActions(bookmarksActions);

    m_bookmarksAddFolderAction->setIcon(AroraIcon::get(QLatin1String("folder-new")));
    m_bookmarksShowAllAction->setIcon(AroraIcon::get(QLatin1String("user-bookmarks")));

    // Window
    m_windowMenu = new QMenu(menuBar());
    menuBar()->addMenu(m_windowMenu);

    // POL02: Ctrl+Shift+A opens the palette's tabs-only mode; the
    // menu entry doubles as the shortcut's discoverable home.  The
    // extra addAction keeps the shortcut live while the menu bar is
    // hidden.
    m_windowTabSearchAction = new QAction(m_windowMenu);
    m_windowTabSearchAction->setShortcut(
        QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_A));
    connect(m_windowTabSearchAction, &QAction::triggered,
            this, &BrowserMainWindow::showTabSearch);
    addAction(m_windowTabSearchAction);

    connect(m_windowMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::aboutToShowWindowMenu);
    aboutToShowWindowMenu();

    // Tools
    m_toolsMenu = new QMenu(menuBar());
    menuBar()->addMenu(m_toolsMenu);

    // CMD01: the palette action doubles as the discoverable menu entry
    // for the Ctrl+Shift+P shortcut.
    m_toolsCommandPaletteAction = new QAction(m_toolsMenu);
    m_toolsCommandPaletteAction->setShortcut(
        QKeySequence(Qt::ControlModifier | Qt::ShiftModifier | Qt::Key_P));
    connect(m_toolsCommandPaletteAction, &QAction::triggered,
            this, &BrowserMainWindow::showCommandPalette);
    m_toolsMenu->addAction(m_toolsCommandPaletteAction);
    m_toolsMenu->addSeparator();

    // MENU04: Downloads heads the Tools menu's utility cluster — the
    // Window menu keeps only tab/window-management entries.
    m_toolsDownloadsAction = new QAction(m_toolsMenu);
    m_toolsDownloadsAction->setIcon(
        AroraIcon::get(QLatin1String("emblem-downloads")));
    connect(m_toolsDownloadsAction, &QAction::triggered,
            this, &BrowserMainWindow::downloadManager);
    m_toolsMenu->addAction(m_toolsDownloadsAction);

    // MENU01: no Tools-menu entry — the action lives on the window
    // itself so the Ctrl+K shortcut keeps reaching webSearch()
    // without a menu item to show.
    m_toolsWebSearchAction = new QAction(this);
    connect(m_toolsWebSearchAction, &QAction::triggered,
            this, &BrowserMainWindow::webSearch);
    addAction(m_toolsWebSearchAction);

    m_toolsClearPrivateDataAction = new QAction(m_toolsMenu);
    connect(m_toolsClearPrivateDataAction, &QAction::triggered,
            this, &BrowserMainWindow::clearPrivateData);
    m_toolsMenu->addAction(m_toolsClearPrivateDataAction);

    // SEC13: drops the passphrase-derived key so the saved-password
    // store needs the master passphrase again.  Only meaningful while
    // a passphrase-protected store is unlocked.
    m_toolsLockStoreAction = new QAction(m_toolsMenu);
    connect(m_toolsLockStoreAction, &QAction::triggered, this, [this]() {
        SecureStore::lock();
        statusBar()->showMessage(tr("Credential store locked"), 5000);
    });
    connect(m_toolsMenu, &QMenu::aboutToShow, this, [this]() {
        m_toolsLockStoreAction->setEnabled(
            SecureStore::passphraseProtectionEnabled()
            && SecureStore::isUnlocked());
    });
    m_toolsMenu->addAction(m_toolsLockStoreAction);

    // DEVT01: the inspector action lives under View > Development
    // Tools — the Tools menu no longer carries it.

    // DEVT03: engine-neutral dev tools dock — toggleViewAction keeps
    // its check state synced with the dock's close button.  No-rust
    // builds have no BiDi backend at all — the entry stays out of the
    // menu there; a tor window keeps it visible but disabled since
    // its process never arms the debug channel.
#ifdef ARORA_RUSTCORE
    QAction *bidiAction = m_bidiPanelDock->toggleViewAction();
    bidiAction->setText(tr("BiDi Dev &Tools"));
    if (BrowserApplication::isTorMode()) {
        bidiAction->setEnabled(false);
        bidiAction->setToolTip(
            tr("The debug channel is never armed in Tor windows."));
    }
    m_toolsMenu->addAction(bidiAction);
#endif

    m_toolsUserAgentMenu = new UserAgentMenu(m_toolsMenu);
    m_toolsMenu->addMenu(m_toolsUserAgentMenu);

    // MENU05: filter management lives on the Preferences Privacy
    // page (Content Blocking > Manage...) — the Tools menu no
    // longer carries it.

    // Help
    m_helpMenu = new QMenu(menuBar());
    menuBar()->addMenu(m_helpMenu);

    m_helpChangeLanguageAction = new QAction(m_helpMenu);
    connect(m_helpChangeLanguageAction, &QAction::triggered,
            BrowserApplication::languageManager(), &LanguageManager::chooseNewLanguage);
    m_helpMenu->addAction(m_helpChangeLanguageAction);
    m_helpMenu->addSeparator();

    m_helpAboutQtAction = new QAction(m_helpMenu);
    connect(m_helpAboutQtAction, &QAction::triggered,
            qApp, &QApplication::aboutQt);
    m_helpMenu->addAction(m_helpAboutQtAction);

    m_helpAboutApplicationAction = new QAction(m_helpMenu);
    connect(m_helpAboutApplicationAction, &QAction::triggered,
            this, &BrowserMainWindow::aboutApplication);
    m_helpMenu->addAction(m_helpAboutApplicationAction);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_helpChangeLanguageAction->setIcon(AroraIcon::get(QLatin1String("preferences-desktop-locale")));
    m_helpAboutQtAction->setIcon(QPixmap(QLatin1String(":/qt-project.org/qmessagebox/images/qtlogo-64.png")));
    m_helpAboutApplicationAction->setIcon(windowIcon());
#endif
}

void BrowserMainWindow::aboutToShowViewMenu()
{
    m_viewToolbarAction->setText(m_navigationBar->isVisible() ? tr("Hide Toolbar") : tr("Show Toolbar"));
    m_viewBookmarkBarAction->setText(m_bookmarksToolbar->isVisible() ? tr("Hide Bookmarks Bar") : tr("Show Bookmarks Bar"));
    m_viewStatusbarAction->setText(statusBar()->isVisible() ? tr("Hide Status Bar") : tr("Show Status Bar"));
    updateReaderState();
}

void BrowserMainWindow::updateReaderState()
{
    WebView *view = currentTab();
    // Follow the current tab's ReaderMode so the check mark tracks
    // JS-side exits and probe results, not just our own toggles.
    if (m_readerWatchedView != view) {
        if (m_readerWatchedView && m_readerWatchedView->readerMode())
            disconnect(m_readerWatchedView->readerMode(), nullptr,
                       this, nullptr);
        if (m_readerWatchedView && m_readerWatchedView->pictureInPicture())
            disconnect(m_readerWatchedView->pictureInPicture(), nullptr,
                       this, nullptr);
        m_readerWatchedView = view;
        if (view && view->readerMode()) {
            connect(view->readerMode(), &ReaderMode::activeChanged,
                    this, [this](bool) { updateReaderState(); });
            connect(view->readerMode(), &ReaderMode::availableChanged,
                    this, [this](bool) { updateReaderState(); });
        }
        if (view && view->pictureInPicture()) {
            connect(view->pictureInPicture(),
                    &PictureInPicture::activeChanged,
                    this, [this](bool) { updateReaderState(); });
        }
    }
    m_viewReaderAction->setEnabled(view != nullptr);
    m_viewReaderAction->setChecked(view && view->readerMode()
                                   && view->readerMode()->isActive());
    // READ02: the nav-row button follows the same tab — it manages
    // its own checked state off the view's ReaderMode, while the
    // toolbar item's visibility goes through its widget action.
    m_readerModeButton->setWebView(view);
    const bool readerAvailable = view && view->readerMode()
                                 && view->readerMode()->isAvailable();
    m_navReaderAction->setVisible(readerAvailable);
    m_readerModeButton->setVisible(readerAvailable);
    m_viewPipAction->setEnabled(view != nullptr);
    m_viewPipAction->setChecked(view && view->pictureInPicture()
                                && view->pictureInPicture()->isActive());
}

void BrowserMainWindow::aboutToShowTextEncodingMenu()
{
    m_viewTextEncodingMenu->clear();

    int currentCodec = -1;
    QStringList codecs = QStringConverter::availableCodecs();
    codecs.sort();

    QString defaultTextEncoding = BrowserApplication::webEngineProfile()->settings()->defaultTextEncoding();
    currentCodec = codecs.indexOf(defaultTextEncoding);

    QAction *defaultEncoding = m_viewTextEncodingMenu->addAction(tr("Default"));
    defaultEncoding->setData(QString());
    defaultEncoding->setCheckable(true);
    if (currentCodec == -1)
        defaultEncoding->setChecked(true);
    m_viewTextEncodingMenu->addSeparator();

    for (int i = 0; i < codecs.count(); ++i) {
        const QString &codec = codecs.at(i);
        QAction *action = m_viewTextEncodingMenu->addAction(codec);
        action->setData(codec);
        action->setCheckable(true);
        if (currentCodec == i)
            action->setChecked(true);
    }
}

void BrowserMainWindow::viewTextEncoding(QAction *action)
{
    Q_ASSERT(action);
    QString codec = action->data().toString();
    BrowserApplication::webEngineProfile()->settings()->setDefaultTextEncoding(codec);
    // The default only applies to future page loads, so set the
    // encoding on all currently open pages too.
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        if (WebView *view = m_tabWidget->webView(i))
            view->page()->settings()->setDefaultTextEncoding(codec);
    }
}

void BrowserMainWindow::retranslate()
{
    m_fileMenu->setTitle(tr("&File"));
    m_fileNewWindowAction->setText(tr("&New Window"));
    m_fileNewContainerTabMenu->setTitle(tr("New &Container Tab"));
    m_fileOpenFileAction->setText(tr("&Open File..."));
    m_fileOpenLocationAction->setText(tr("Open &Location..."));
    m_fileSaveAsAction->setText(tr("&Save As..."));
    m_fileImportBookmarksAction->setText(tr("&Import Bookmarks..."));
    m_fileExportBookmarksAction->setText(tr("&Export Bookmarks..."));
    m_filePrintPreviewAction->setText(tr("P&rint Preview..."));
    m_filePrintAction->setText(tr("&Print..."));
    m_fileNewPrivateTabAction->setText(tr("New &Private Tab"));
    m_fileNewTorWindowAction->setText(tr("New &Tor Window"));
    m_filePreferencesAction->setText(tr("Preferences..."));
    m_filePreferencesAction->setShortcut(tr("Ctrl+,"));
    m_fileCloseWindow->setText(tr("Close Window"));
    m_fileQuit->setText(tr("&Quit"));

    m_editMenu->setTitle(tr("&Edit"));
    m_editUndoAction->setText(tr("&Undo"));
    m_editRedoAction->setText(tr("&Redo"));
    m_editCutAction->setText(tr("Cu&t"));
    m_editCopyAction->setText(tr("&Copy"));
    m_editPasteAction->setText(tr("&Paste"));
    m_editSelectAllAction->setText(tr("Select &All"));
    m_editFindAction->setText(tr("&Find"));
    m_editFindNextAction->setText(tr("Find Nex&t"));
    m_editFindPreviousAction->setText(tr("Find P&revious"));

    m_viewMenu->setTitle(tr("&View"));
    m_viewToolbarAction->setShortcut(tr("Ctrl+|"));
    m_viewBookmarkBarAction->setShortcut(tr("Alt+Ctrl+B"));
    m_viewStatusbarAction->setShortcut(tr("Ctrl+/"));
    m_viewShowMenuBarAction->setText(tr("Show Menu Bar"));
    m_viewReloadAction->setText(tr("&Reload Page"));
    m_viewStopAction->setText(tr("&Stop"));
    m_viewZoomInAction->setText(tr("Zoom &In"));
    m_viewZoomNormalAction->setText(tr("Zoom &Normal"));
    m_viewZoomOutAction->setText(tr("Zoom &Out"));
    m_viewZoomTextOnlyAction->setText(tr("Zoom &Text Only"));
    m_viewReaderAction->setText(tr("&Reader Mode"));
    m_viewPipAction->setText(tr("Picture-&in-Picture"));
    m_sidebarDock->setWindowTitle(tr("Sidebar"));
    m_bidiPanelDock->setWindowTitle(tr("Development Tools"));
    m_viewSidebarAction->setText(tr("Sidebar"));
    m_viewSourceAction->setText(tr("Page S&ource"));
    m_viewSourceAction->setShortcut(tr("Ctrl+Alt+U"));
    m_viewFullScreenAction->setText(tr("&Full Screen"));
    m_viewTextEncodingAction->setText(tr("Text Encoding"));
    m_viewDevToolsMenu->setTitle(tr("Development Tools"));
    m_viewChromiumDevToolAction->setText(tr("Chromium Dev Tool"));
    m_viewBidiDevToolAction->setText(tr("BiDi Dev Tool"));
    m_viewBidiDevToolAction->setToolTip(
        tr("Coming with the BiDi backend"));

    m_historyMenu->setTitle(tr("Hi&story"));
    m_historyBackAction->setText(tr("Back"));
    m_historyForwardAction->setText(tr("Forward"));
    m_historyHomeAction->setText(tr("Home"));
    m_historyRestoreLastSessionAction->setText(tr("Restore Last Session"));

    m_bookmarksMenu->setTitle(tr("&Bookmarks"));
    m_bookmarksShowAllAction->setText(tr("Show All Bookmarks..."));
    m_bookmarksAddAction->setText(tr("Add Bookmark..."));
    m_bookmarksAddFolderAction->setText(tr("Add Folder..."));

    m_windowMenu->setTitle(tr("&Window"));
    m_windowTabSearchAction->setText(tr("Search &Tabs..."));

    m_toolsMenu->setTitle(tr("&Tools"));
    m_toolsCommandPaletteAction->setText(tr("Command &Palette..."));
    m_toolsDownloadsAction->setText(tr("Downloads"));
    m_toolsDownloadsAction->setShortcut(
        QKeySequence(tr("Ctrl+Y", "Download Manager")));
    m_toolsWebSearchAction->setText(tr("Web &Search"));
    m_toolsWebSearchAction->setShortcut(QKeySequence(tr("Ctrl+K", "Web Search")));
    m_toolsClearPrivateDataAction->setText(tr("&Clear Private Data"));
    m_toolsClearPrivateDataAction->setShortcut(QKeySequence(tr("Ctrl+Shift+Delete", "Clear Private Data")));
    m_toolsLockStoreAction->setText(tr("&Lock Credential Store"));
    m_toolsUserAgentMenu->setTitle(tr("User Agent"));

    m_helpMenu->setTitle(tr("&Help"));
    m_helpChangeLanguageAction->setText(tr("Switch application language "));
    m_helpAboutQtAction->setText(tr("About &Qt"));
    m_helpAboutApplicationAction->setText(tr("About &%1", "About Browser").arg(QApplication::applicationName()));

    // Toolbar
    m_navigationBar->setWindowTitle(tr("Navigation"));
    m_bookmarksToolbar->setWindowTitle(tr("&Bookmarks"));

    m_stopReloadAction->setText(tr("Reload / Stop"));
    updateStopReloadActionText(false);
}

void BrowserMainWindow::setupToolBar()
{
    setUnifiedTitleAndToolBarOnMac(true);
    m_navigationBar = new QToolBar(this);
    m_navigationBar->setObjectName(QLatin1String("NavigationToolBar"));
    // UIP01: breathing room on the navigation row — the Qt4-era
    // default item density reads cramped on modern displays.  Widget
    // contents margins forward to the toolbar's internal layout.
    m_navigationBar->setContentsMargins(6, 4, 6, 4);
    m_navigationBar->layout()->setSpacing(6);
    // UIP02: one icon size for the nav buttons on every platform —
    // was macOS-only, so Linux/Windows fell back to the style's
    // larger default and the row looked uneven.
    m_navigationBar->setIconSize(QSize(18, 18));
    addToolBar(m_navigationBar);

    m_historyBackAction->setIcon(AroraIcon::get(QLatin1String("go-previous")));
    m_historyBackMenu = new QMenu(this);
    m_historyBackAction->setMenu(m_historyBackMenu);
    connect(m_historyBackMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::aboutToShowBackMenu);
    connect(m_historyBackMenu, &QMenu::triggered,
            this, &BrowserMainWindow::openActionUrl);
    m_navigationBar->addAction(m_historyBackAction);

    m_historyForwardAction->setIcon(AroraIcon::get(QLatin1String("go-next")));
    m_historyForwardMenu = new QMenu(this);
    connect(m_historyForwardMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::aboutToShowForwardMenu);
    connect(m_historyForwardMenu, &QMenu::triggered,
            this, &BrowserMainWindow::openActionUrl);
    m_historyForwardAction->setMenu(m_historyForwardMenu);
    m_navigationBar->addAction(m_historyForwardAction);

    m_stopReloadAction = new QAction(this);
    m_stopReloadAction->setIcon(m_reloadIcon);
    m_navigationBar->addAction(m_stopReloadAction);

    // READ02: reader-mode toggle in the nav row — after stop/reload,
    // before the location bar starts.  Same ReaderButton the location
    // bar uses for its in-field page action: palette-painted glyph
    // (no icon theme ships a reader glyph), hidden until the current
    // page reports itself article-like, checked while the overlay is
    // up.  updateReaderState() re-points it at the current tab along
    // with the View-menu action, so the two can never disagree.
    m_readerModeButton = new ReaderButton(m_navigationBar);
    m_readerModeButton->setObjectName(QLatin1String("navReaderButton"));
    // addWidget() wraps the button in a QWidgetAction — a toolbar
    // item's presence is governed by the ACTION's visibility, so
    // updateReaderState() toggles that (the widget's own hidden flag
    // alone leaves the item latched off).
    m_navReaderAction = m_navigationBar->addWidget(m_readerModeButton);
    m_navReaderAction->setVisible(false);
    m_navReaderAction->setEnabled(true);

    m_navigationSplitter = new QSplitter(m_navigationBar);
    m_navigationSplitter->addWidget(m_tabWidget->locationBarStack());

    m_toolbarSearch = new ToolbarSearch(m_navigationBar);
    m_navigationSplitter->addWidget(m_toolbarSearch);
    connect(m_toolbarSearch, &ToolbarSearch::search,
            m_tabWidget, [this](const QUrl &url, TabWidget::OpenUrlIn tab) {
        m_tabWidget->loadUrl(url, tab);
    });
    // The search box records recent searches only when the page the
    // search lands in is not private (SEC07), so it has to track the
    // current tab's view — private browsing is a per-page profile
    // property under WebEngine.
    connect(m_tabWidget, &QTabWidget::currentChanged, this,
            [this](int) { m_toolbarSearch->setWebView(currentTab()); });
    m_toolbarSearch->setWebView(currentTab());
    m_navigationSplitter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    m_tabWidget->locationBarStack()->setMinimumWidth(120);
    m_navigationSplitter->setCollapsible(0, false);
    m_navigationBar->addWidget(m_navigationSplitter);
    // SIDE01: sidebar toggle at the far right of the navigation bar —
    // same checkable action as View > Sidebar (F4).
    m_navigationBar->addAction(m_viewSidebarAction);
    int splitterWidth = m_navigationSplitter->width();
    QList<int> sizes;
    sizes << (int)((double)splitterWidth * .80) << (int)((double)splitterWidth * .20);
    m_navigationSplitter->setSizes(sizes);

    applySearchBoxVisibility();
}

// SRCH03: the dedicated search box is opt-in — the omnibox location
// bar (SRCH01) covers the same job.  SRCH08 retires the SRCH04
// collapsed-button mode entirely: showSearchBox off hides the widget
// outright so the splitter gives the location bar the full width,
// on restores the full text field.  The widget stays constructed
// either way: the splitter layout, the search signal wiring, and
// setWebView() tracking are all unchanged.
void BrowserMainWindow::applySearchBoxVisibility()
{
    // TOR03: tor windows never carry the dedicated search box — a
    // divergent-engine field is a deanonymization wart and even the
    // collapsed button sliver wastes splitter width; the location
    // bar's omnibox search covers the same job.  The widget stays
    // constructed (search wiring, setWebView() tracking, and the
    // clearPrivateData widget sweep all still reach it) but is
    // hidden outright, so the location bar gets the full width.
    if (BrowserApplication::isTorMode()) {
        m_toolbarSearch->setVisible(false);
        return;
    }
    QSettings settings;
    const bool field = settings.value(
        QLatin1String("MainWindow/showSearchBox"), false).toBool();
    m_toolbarSearch->setButtonMode(false);
    m_toolbarSearch->setVisible(field);
}

// SIDE01: the panel is built on first show so a hidden sidebar is
// zero-footprint — constructing it eagerly would pull the bookmarks,
// history and download models into every window's startup path.
void BrowserMainWindow::ensureSidebarPanel()
{
    if (m_sidebarPanel)
        return;
    m_sidebarPanel = new SidebarPanel(m_sidebarDock);
    connect(m_sidebarPanel, &SidebarPanel::openUrl,
            m_tabWidget, [this](const QUrl &url,
                                TabWidget::OpenUrlIn tab,
                                const QString &title) {
        m_tabWidget->loadUrl(url, tab, title);
    });
    // DOWN02: the Downloads page header's X folds the dock away (the
    // toggle action + persistence follow via visibilityChanged).
    connect(m_sidebarPanel, &SidebarPanel::closeRequested,
            m_sidebarDock, &QWidget::hide);
    m_sidebarDock->setWidget(m_sidebarPanel);
}

// DEVT03: built on first show for the same zero-footprint reason as
// the sidebar — the BiDi client inside connects on show too.
void BrowserMainWindow::ensureBidiPanel()
{
    if (m_bidiPanel)
        return;
    m_bidiPanel = new BidiPanel(m_bidiPanelDock);
    m_bidiPanelDock->setWidget(m_bidiPanel);
}

void BrowserMainWindow::applySidebarSettings()
{
    QSettings settings;
    const int storedArea =
        settings.value(QLatin1String("MainWindow/sidebarDockArea"),
                       int(Qt::LeftDockWidgetArea)).toInt();
    const Qt::DockWidgetArea area =
        storedArea == int(Qt::RightDockWidgetArea)
        ? Qt::RightDockWidgetArea : Qt::LeftDockWidgetArea;
    if (dockWidgetArea(m_sidebarDock) != area)
        addDockWidget(area, m_sidebarDock);
    const bool show =
        settings.value(QLatin1String("MainWindow/showSidebar"), false)
            .toBool();
    if (show)
        ensureSidebarPanel();
    // SIDE02: an already-built panel picks up per-panel visibility
    // changes here too (a hidden dock re-reads on next show anyway).
    if (m_sidebarPanel)
        m_sidebarPanel->applyPanelVisibility();
    m_sidebarDock->setVisible(show);
}

SidebarPanel *BrowserMainWindow::sidebarPanel() const
{
    return m_sidebarPanel;
}

QDockWidget *BrowserMainWindow::sidebarDock() const
{
    return m_sidebarDock;
}

void BrowserMainWindow::showBookmarksDialog()
{
    BookmarksDialog *dialog = new BookmarksDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(dialog, &BookmarksDialog::openUrl,
            m_tabWidget, &TabWidget::loadUrl);
    dialog->show();
}

void BrowserMainWindow::addBookmark()
{
    WebView *webView = currentTab();
    QString url = QLatin1String(webView->url().toEncoded());
    QString title = webView->title();

    AddBookmarkDialog dialog;
    dialog.setUrl(url);
    dialog.setTitle(title);
    BookmarkNode *menu = BrowserApplication::bookmarksManager()->menu();
    QModelIndex index = BrowserApplication::bookmarksManager()->bookmarksModel()->index(menu);
    dialog.setCurrentIndex(index);
    dialog.exec();
}

void BrowserMainWindow::addBookmarkFolder()
{
    AddBookmarkDialog dialog;
    BookmarksManager *bookmarksManager = BrowserApplication::bookmarksManager();
    BookmarkNode *menu = bookmarksManager->menu();
    QModelIndex index = bookmarksManager->bookmarksModel()->index(menu);
    dialog.setCurrentIndex(index);
    dialog.setFolder(true);
    dialog.exec();
}

void BrowserMainWindow::viewMenuBar()
{
    menuBar()->setVisible(!menuBar()->isVisible());

    m_menuBarVisible = menuBar()->isVisible();
    m_autoSaver->changeOccurred();
}

void BrowserMainWindow::viewToolbar()
{
    if (m_navigationBar->isVisible()) {
        m_navigationBar->close();
    } else {
        m_navigationBar->show();
    }
    m_autoSaver->changeOccurred();
}

void BrowserMainWindow::viewBookmarksBar()
{
    if (m_bookmarksToolbar->isVisible()) {
        m_bookmarksToolbar->hide();
#if defined(Q_OS_MACOS)
        m_bookmarksToolbarFrame->hide();
#endif
    } else {
        m_bookmarksToolbar->show();
#if defined(Q_OS_MACOS)
        m_bookmarksToolbarFrame->show();
#endif
    }
    m_autoSaver->changeOccurred();
}

void BrowserMainWindow::viewStatusbar()
{
    if (statusBar()->isVisible()) {
        statusBar()->close();
    } else {
        statusBar()->show();
    }

    m_statusBarVisible = statusBar()->isVisible();

    m_autoSaver->changeOccurred();
}

void BrowserMainWindow::downloadManager()
{
    // DOWN02: downloads live in the sidebar — Ctrl+Y / Tools >
    // Downloads raises the dock on the Downloads section instead of
    // opening the (removed) standalone window.
    ensureSidebarPanel();
    m_sidebarPanel->showDownloads();
    m_sidebarDock->setVisible(true);
    m_sidebarDock->raise();
}

void BrowserMainWindow::selectLineEdit()
{
    if (m_navigationBar->isHidden())
        m_navigationBar->show();

    m_tabWidget->currentLocationBar()->selectAll();
    m_tabWidget->currentLocationBar()->setFocus();
}

void BrowserMainWindow::fileSaveAs()
{
    if (!currentTab())
        return;
    BrowserApplication::downloadManager()->download(currentTab()->page(), currentTab()->url(), true);
}

void BrowserMainWindow::preferences()
{
    showSettingsPage();
}

void BrowserMainWindow::showSettingsPage(int page)
{
    // PREFS01: Preferences is a tab, not a modal dialog — one per
    // window.  A second invocation focuses the existing tab instead
    // of stacking another.
    SettingsDialog *settings = nullptr;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        settings = qobject_cast<SettingsDialog*>(m_tabWidget->widget(i));
        if (settings)
            break;
    }
    if (!settings) {
        settings = new SettingsDialog(m_tabWidget);
        // The page's OK applies + closes; its Cancel — and the tab's
        // own close — discard, exactly the dialog's old semantics.
        connect(settings, &SettingsDialog::closeRequested,
                this, [this, settings]() {
            const int index = m_tabWidget->indexOf(settings);
            if (index >= 0)
                m_tabWidget->closeTab(index);
        });
        m_tabWidget->addWidgetTab(settings, tr("Preferences"),
                                  AroraIcon::get(QLatin1String("preferences-system")));
    }
    if (page >= 0)
        settings->openAtPage(SettingsDialog::Page(page));
    m_tabWidget->setCurrentWidget(settings);
    settings->setFocus();
}

void BrowserMainWindow::showHistoryPage()
{
    // HIST02: "Show All History" is a tab, not a floating dialog —
    // one per window.  A second invocation focuses the existing tab
    // instead of stacking another.
    HistoryDialog *history = nullptr;
    for (int i = 0; i < m_tabWidget->count(); ++i) {
        history = qobject_cast<HistoryDialog*>(m_tabWidget->widget(i));
        if (history)
            break;
    }
    if (!history) {
        history = new HistoryDialog(m_tabWidget);
        connect(history, &HistoryDialog::closeRequested,
                this, [this, history]() {
            const int index = m_tabWidget->indexOf(history);
            if (index >= 0)
                m_tabWidget->closeTab(index);
        });
        // Entries open in a fresh tab so the history tab stays open;
        // the dialog records the activating modifiers so Ctrl/Shift
        // clicks still route through modifyWithUserBehavior.
        connect(history, &HistoryDialog::openUrl,
                this, [this](const QUrl &url, const QString &title) {
            m_tabWidget->loadUrl(url,
                TabWidget::modifyWithUserBehavior(TabWidget::NewSelectedTab),
                title);
        });
        m_tabWidget->addWidgetTab(history, tr("History"),
                                  QIcon(QLatin1String(":graphics/history.png")));
    }
    m_tabWidget->setCurrentWidget(history);
    history->setFocus();
}

CommandPalette *BrowserMainWindow::commandPalette()
{
    if (!m_commandPalette)
        m_commandPalette = new CommandPalette(this);
    return m_commandPalette;
}

void BrowserMainWindow::showCommandPalette()
{
    commandPalette()->openPalette();
}

void BrowserMainWindow::showTabSearch()
{
    commandPalette()->openTabSearch();
}

void BrowserMainWindow::updateStatusbar(const QString &string)
{
    statusBar()->showMessage(string, 2000);
}

// TOR04: renders the circuit chain ("Tor: guard -> middle -> exit")
// on the tor window's status-bar label.  Hop labels prefer the relay
// nickname and fall back to a short fingerprint; TOR05 appends each
// hop's country code ("(DE)", "--" while unknown) resolved by
// TorManager via the control port; the tooltip carries the full
// chain, circuit id, status and purpose.
void BrowserMainWindow::updateTorCircuitLabel()
{
    if (!m_torCircuitLabel)
        return;
    BrowserApplication *app = BrowserApplication::instance();
    TorManager *tor = app ? app->torManager() : nullptr;
    if (!tor) {
        m_torCircuitLabel->setText(tr("Tor: daemon off"));
        m_torCircuitLabel->setToolTip(
            tr("The managed Tor daemon is not running."));
        return;
    }
    switch (tor->state()) {
    case TorManager::Failed:
        m_torCircuitLabel->setText(tr("Tor: failed"));
        m_torCircuitLabel->setToolTip(tor->errorString());
        return;
    case TorManager::Ready:
        break;
    default: {
        const int progress = tor->bootstrapProgress();
        m_torCircuitLabel->setText(progress >= 0
            ? tr("Tor: connecting %1%").arg(progress)
            : tr("Tor: connecting..."));
        m_torCircuitLabel->setToolTip(tor->bootstrapSummary());
        return;
    }
    }

    const TorCircuit *circuit = nullptr;
    const QList<TorCircuit> circuits = tor->circuits();
    for (const TorCircuit &candidate : circuits) {
        if (candidate.id == tor->displayCircuitId()) {
            circuit = &candidate;
            break;
        }
    }
    if (!circuit || circuit->hops.isEmpty()) {
        m_torCircuitLabel->setText(tr("Tor: no circuit"));
        m_torCircuitLabel->setToolTip(
            tr("No traffic-bearing Tor circuit yet."));
        return;
    }
    QStringList names;
    QStringList detail;
    for (const TorCircuitHop &hop : circuit->hops) {
        const QString country = hop.country.isEmpty()
            ? QStringLiteral("--") : hop.country;
        names << QStringLiteral("%1 (%2)")
                     .arg(hop.nickname.isEmpty()
                              ? hop.fingerprint.left(8)
                              : hop.nickname,
                          country);
        detail << (hop.nickname.isEmpty()
                       ? QStringLiteral("%1 [%2]")
                             .arg(hop.fingerprint, country)
                       : QStringLiteral("%1 (%2) [%3]")
                             .arg(hop.nickname, hop.fingerprint,
                                  country));
    }
    m_torCircuitLabel->setText(
        tr("Tor: %1").arg(names.join(QLatin1String(" -> "))));
    QString tooltip = tr("Circuit %1 (%2): %3")
        .arg(circuit->id)
        .arg(circuit->status, detail.join(QLatin1String(" -> ")));
    if (!circuit->purpose.isEmpty())
        tooltip += tr(" — purpose %1").arg(circuit->purpose);
    m_torCircuitLabel->setToolTip(tooltip);
}

void BrowserMainWindow::updateWindowTitle(const QString &title)
{
    // TOR02: tor windows are marked in the title — a window whose
    // traffic exits through tor must never look like a normal one.
    const QString marker = BrowserApplication::isTorMode()
        ? QStringLiteral(" (Tor)") : QString();
    if (title.isEmpty()) {
        setWindowTitle(QApplication::applicationName() + marker);
    } else {
#if defined(Q_OS_MACOS)
        setWindowTitle(title + marker);
#else
        setWindowTitle(tr("%1 - Arora", "Page title and Browser name").arg(title) + marker);
#endif
    }
}

void BrowserMainWindow::aboutApplication()
{
    AboutDialog *aboutDialog = new AboutDialog(this);
    aboutDialog->setAttribute(Qt::WA_DeleteOnClose);
    aboutDialog->show();
}

void BrowserMainWindow::fileNew()
{
    BrowserApplication *application = BrowserApplication::instance();
    if (!application)
        return;
    BrowserMainWindow *window = application->newMainWindow();

    QSettings settings;
    settings.beginGroup(QLatin1String("MainWindow"));
    int startup = settings.value(QLatin1String("startupBehavior")).toInt();

    if (startup == 0)
        window->goHome();
}

void BrowserMainWindow::fileOpen()
{
    QString file = QFileDialog::getOpenFileName(this, tr("Open Web Resource"), QString(),
                   tr("Web Resources (*.html *.htm *.svg *.png *.gif *.svgz);;All files (*.*)"));

    if (file.isEmpty())
        return;

    tabWidget()->loadUrl(QUrl::fromLocalFile(file));
}

void BrowserMainWindow::filePrintPreview()
{
    if (!currentTab())
        return;
    QPrintPreviewDialog dialog(this);
    // Qt6: QWebEngineView::print runs asynchronously; the preview
    // dialog owns the printer for the duration of exec().
    connect(&dialog, &QPrintPreviewDialog::paintRequested,
            currentTab(), &QWebEngineView::print);
    dialog.exec();
}

void BrowserMainWindow::filePrint()
{
    if (!currentTab())
        return;
    printRequested(currentTab()->page());
}

void BrowserMainWindow::printRequested(QWebEnginePage *page)
{
    if (!page)
        return;
    QWebEngineView *view = QWebEngineView::forPage(page);
    if (!view)
        return;
    // QWebEngineView::print is asynchronous — the printer must stay
    // alive until printFinished fires.
    QPrinter *printer = new QPrinter(QPrinter::HighResolution);
    QPrintDialog dialog(printer, this);
    dialog.setWindowTitle(tr("Print Document"));
    if (dialog.exec() != QDialog::Accepted) {
        delete printer;
        return;
    }
    connect(view, &QWebEngineView::printFinished, view,
            [printer](bool) { delete printer; });
    view->print(printer);
}

void BrowserMainWindow::zoomTextOnlyChanged(bool textOnly)
{
    m_viewZoomTextOnlyAction->setChecked(textOnly);
}

void BrowserMainWindow::privacyChanged(bool isPrivate)
{
    // PTAB01: private browsing is per-tab now — the global flag still
    // exists internally (full-private windows, tests), and leaving it
    // drops the off-the-record per-tab state kept here.
    if (!isPrivate)
        tabWidget()->clear();
}

void BrowserMainWindow::closeEvent(QCloseEvent *event)
{
    BrowserApplication *application = BrowserApplication::instance();
    if (application && !application->allowToCloseWindow(this)) {
        event->ignore();
        if (m_tabWidget->count() == 0)
            m_tabWidget->newTab();
        return;
    }

    // CONT06: count() only sees the active container level — tabs in
    // filtered levels still die with the window, so they must count.
    const int openTabs = m_tabWidget->totalTabCount();
    if (openTabs > 1) {
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        bool confirm = settings.value(QLatin1String("confirmClosingMultipleTabs"), true).toBool();
        if (confirm) {
            QApplication::alert(this);
            int ret = QMessageBox::warning(this, QString(),
                                           tr("Are you sure you want to close the window?"
                                              "  There are %1 tabs open").arg(openTabs),
                                           QMessageBox::Yes | QMessageBox::No,
                                           QMessageBox::No);
            if (ret == QMessageBox::No) {
                event->ignore();
                return;
            }
        }
    }

    event->accept();
}

void BrowserMainWindow::mousePressEvent(QMouseEvent *event)
{
    switch (event->button()) {
    case Qt::XButton1:
        m_historyBackAction->trigger();
        break;
    case Qt::XButton2:
        m_historyForwardAction->trigger();
        break;
    default:
        QMainWindow::mousePressEvent(event);
        break;
    }
}

void BrowserMainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslate();
    QMainWindow::changeEvent(event);
}

void BrowserMainWindow::editFind()
{
    if (WebViewSearch *search = tabWidget()->webViewSearch(m_tabWidget->currentIndex()))
        search->showFind();
}

void BrowserMainWindow::editFindNext()
{
    if (WebViewSearch *search = tabWidget()->webViewSearch(m_tabWidget->currentIndex()))
        search->findNext();
}

void BrowserMainWindow::editFindPrevious()
{
    if (WebViewSearch *search = tabWidget()->webViewSearch(m_tabWidget->currentIndex()))
        search->findPrevious();
}

void BrowserMainWindow::zoomIn()
{
    if (!currentTab())
        return;
    currentTab()->zoomIn();
}

void BrowserMainWindow::zoomNormal()
{
    if (!currentTab())
        return;
    currentTab()->resetZoom();
}

void BrowserMainWindow::zoomOut()
{
    if (!currentTab())
        return;
    currentTab()->zoomOut();
}

void BrowserMainWindow::viewFullScreen(bool makeFullScreen)
{
    if (makeFullScreen) {
        setUnifiedTitleAndToolBarOnMac(false);
        setWindowState(windowState() | Qt::WindowFullScreen);

        menuBar()->hide();
        statusBar()->hide();
    } else {
        setWindowState(windowState() & ~Qt::WindowFullScreen);

        setUnifiedTitleAndToolBarOnMac(true);
        menuBar()->setVisible(m_menuBarVisible);
        statusBar()->setVisible(m_statusBarVisible);
    }
}

void BrowserMainWindow::viewPageSource()
{
    if (!currentTab())
        return;

    QString title = currentTab()->title();
    QUrl url = currentTab()->url();
    // Qt6: QWebEnginePage::toHtml answers asynchronously — the viewer
    // opens once the serialized DOM arrives from the render process.
    currentTab()->page()->toHtml([title, url](const QString &markup) {
        SourceViewer *viewer = new SourceViewer(markup, title, url);
        viewer->setAttribute(Qt::WA_DeleteOnClose);
        viewer->show();
    });
}

void BrowserMainWindow::goHome()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("MainWindow"));
    QString home = settings.value(QLatin1String("home"), QLatin1String("about:home")).toString();
    tabWidget()->loadString(home);
}

void BrowserMainWindow::webSearch()
{
    // TOR03/SRCH08: a hidden search box has nothing to focus — the
    // shortcut falls back to the omnibox, which runs the same
    // engines.
    if (m_toolbarSearch->isHidden()) {
        if (QLineEdit *bar = m_tabWidget->currentLocationBar()) {
            bar->selectAll();
            bar->setFocus();
        }
        return;
    }
    // SRCH04: in button mode there is no field to focus — the
    // shortcut opens the engines menu, which leads with a "Search..."
    // prompt.  applySearchBoxVisibility() no longer arms it, but the
    // ToolbarSearch API still supports it, so keep the path live.
    if (m_toolbarSearch->isButtonMode()) {
        m_toolbarSearch->showEnginesMenu();
        return;
    }
    m_toolbarSearch->selectAll();
    m_toolbarSearch->setFocus();
}

void BrowserMainWindow::clearPrivateData()
{
    ClearPrivateData dialog;
    dialog.exec();
}

void BrowserMainWindow::swapFocus()
{
    if (!currentTab())
        return;
    if (currentTab()->hasFocus()) {
        m_tabWidget->currentLocationBar()->setFocus();
        m_tabWidget->currentLocationBar()->selectAll();
    } else {
        currentTab()->setFocus();
    }
}

TabWidget *BrowserMainWindow::tabWidget() const
{
    return m_tabWidget;
}

WebView *BrowserMainWindow::currentTab() const
{
    return m_tabWidget->currentWebView();
}

ToolbarSearch *BrowserMainWindow::toolbarSearch() const
{
    return m_toolbarSearch;
}

void BrowserMainWindow::updateStopReloadActionText(bool loading)
{
    if (loading) {
        m_stopReloadAction->setToolTip(tr("Stop loading the current page"));
        m_stopReloadAction->setIconText(tr("Stop"));
    } else {
        m_stopReloadAction->setToolTip(tr("Reload the current page"));
        m_stopReloadAction->setIconText(tr("Reload"));
    }
}

void BrowserMainWindow::loadProgress(int progress)
{
    if (progress < 100 && progress > 0) {
        disconnect(m_stopReloadAction, &QAction::triggered, m_viewReloadAction, &QAction::trigger);
        m_stopReloadAction->setIcon(m_stopIcon);
        connect(m_stopReloadAction, &QAction::triggered, m_viewStopAction, &QAction::trigger);
        updateStopReloadActionText(true);
    } else {
        disconnect(m_stopReloadAction, &QAction::triggered, m_viewStopAction, &QAction::trigger);
        m_stopReloadAction->setIcon(m_reloadIcon);
        connect(m_stopReloadAction, &QAction::triggered, m_viewReloadAction, &QAction::trigger);
        updateStopReloadActionText(false);
    }
}

// CONT02: File ▸ New Container Tab — the registry (plus the explicit
// "No Container" escape, useful when the current tab is already in a
// container) and the management entries.  Private windows can't make
// container tabs: a container is persistent storage, the antithesis
// of off-the-record.
void BrowserMainWindow::populateNewContainerTabMenu()
{
    m_fileNewContainerTabMenu->clear();
    if (BrowserApplication::isPrivate()) {
        QAction *unavailable = m_fileNewContainerTabMenu->addAction(
            tr("Unavailable in private windows"));
        unavailable->setEnabled(false);
        return;
    }
    ContainerManager *manager = ContainerManager::instance();
    m_fileNewContainerTabMenu->addAction(tr("&No Container"), this, [this]() {
        m_tabWidget->makeNewTabInContainer(
            ContainerManager::defaultContainerId(), true);
    });
    const QList<ContainerManager::Container> containers =
        manager->containers();
    for (const ContainerManager::Container &container : containers) {
        QAction *entry = m_fileNewContainerTabMenu->addAction(
            ContainerManager::colorIcon(container.color),
            SafeText::menu(container.name));
        const QString id = container.id;
        connect(entry, &QAction::triggered, this, [this, id]() {
            m_tabWidget->makeNewTabInContainer(id, true);
        });
    }
    m_fileNewContainerTabMenu->addSeparator();
    m_fileNewContainerTabMenu->addAction(tr("New &Container..."), this, [this]() {
        const QString id = ContainerManager::instance()
            ->createContainerInteractive(this);
        if (!id.isEmpty())
            m_tabWidget->makeNewTabInContainer(id, true);
    });
    m_fileNewContainerTabMenu->addAction(tr("&Manage Containers..."),
        m_tabWidget, &TabWidget::manageContainers);
}

void BrowserMainWindow::aboutToShowBackMenu()
{
    m_historyBackMenu->clear();
    if (!currentTab())
        return;
    QWebEngineHistory *history = currentTab()->history();
    int historyCount = history->count();
    const QList<QWebEngineHistoryItem> backItems = history->backItems(historyCount);
    for (int i = backItems.count() - 1; i >= 0; --i) {
        const QWebEngineHistoryItem item = backItems.at(i);
        QAction *action = new QAction(this);
        action->setData(-1 * (historyCount - i - 1));
        QIcon icon = BrowserApplication::icon(item.url());
        action->setIcon(icon);
        action->setText(SafeText::menu(item.title()));
        m_historyBackMenu->addAction(action);
    }
}

void BrowserMainWindow::aboutToShowForwardMenu()
{
    m_historyForwardMenu->clear();
    if (!currentTab())
        return;
    QWebEngineHistory *history = currentTab()->history();
    int historyCount = history->count();
    const QList<QWebEngineHistoryItem> forwardItems = history->forwardItems(historyCount);
    for (int i = 0; i < forwardItems.count(); ++i) {
        const QWebEngineHistoryItem item = forwardItems.at(i);
        QAction *action = new QAction(this);
        action->setData(historyCount - i);
        QIcon icon = BrowserApplication::icon(item.url());
        action->setIcon(icon);
        action->setText(SafeText::menu(item.title()));
        m_historyForwardMenu->addAction(action);
    }
}

void BrowserMainWindow::aboutToShowWindowMenu()
{
    m_windowMenu->clear();
    m_windowMenu->addAction(m_tabWidget->nextTabAction());
    m_windowMenu->addAction(m_tabWidget->previousTabAction());
    m_windowMenu->addAction(m_windowTabSearchAction);
    BrowserApplication *application = BrowserApplication::instance();
    if (!application)
        return;
    QList<BrowserMainWindow*> windows = application->mainWindows();
    if (windows.isEmpty())
        return;
    m_windowMenu->addSeparator();
    for (int i = 0; i < windows.count(); ++i) {
        BrowserMainWindow *window = windows.at(i);
        QAction *action = m_windowMenu->addAction(SafeText::menu(window->windowTitle()), this, &BrowserMainWindow::showWindow);
        action->setData(i);
        action->setCheckable(true);
        if (window == this)
            action->setChecked(true);
    }
}

void BrowserMainWindow::showWindow()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        QVariant v = action->data();
        if (v.canConvert<int>()) {
            int offset = qvariant_cast<int>(v);
            BrowserApplication *application = BrowserApplication::instance();
            if (!application)
                return;
            QList<BrowserMainWindow*> windows = application->mainWindows();
            windows.at(offset)->activateWindow();
            windows.at(offset)->raise();
            windows.at(offset)->currentTab()->setFocus();
        }
    }
}

void BrowserMainWindow::openActionUrl(QAction *action)
{
    int offset = action->data().toInt();
    QWebEngineHistory *history = currentTab()->history();
    if (offset < 0) {
        const QList<QWebEngineHistoryItem> items = history->backItems(-1 * offset);
        if (!items.isEmpty())
            history->goToItem(items.first()); // back
    } else if (offset > 0) {
        const QList<QWebEngineHistoryItem> items = history->forwardItems(history->count() - offset + 1);
        if (!items.isEmpty())
            history->goToItem(items.back()); // forward
    }
}

