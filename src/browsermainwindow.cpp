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
#include "bookmarksmanager.h"
#include "bookmarksmenu.h"
#include "bookmarksmodel.h"
#include "bookmarkstoolbar.h"
#include "browserapplication.h"
#include "browsertheme.h"
#include "clearprivatedata.h"
#include "commandpalette.h"
#include "containermanager.h"
#include "devtoolswindow.h"
#include "downloadmanager.h"
#include "engineindicator.h"
#include "engineinterface.h"
#include "enginetab.h"
#include "history.h"
#include "languagemanager.h"
#include "networkaccessmanager.h"
#include "pdfview.h"
#include "pictureinpicture.h"
#include "printpreviewdialog.h"
#include "readerbutton.h"
#include "readermode.h"
#include "safetext.h"
#include "securestore.h"
#include "settings.h"
#include "bidipanel.h"
#include "sidebargrip.h"
#include "sidebarpanel.h"
#include "sourceviewer.h"
#include "statusbarwidgets.h"
#include "tabbar.h"
#include "tabwidget.h"
#include "toolbarsearch.h"
#include "torcircuitbutton.h"
#include "tormanager.h"
#include "torstatusbanner.h"
#include "tortitlebar.h"
#include "useragentmenu.h"
#include "webenginebackend.h"
#include "webview.h"
#include "webviewsearch.h"

#include <qboxlayout.h>
#include <qdockwidget.h>
#include <qevent.h>
#include <qeventloop.h>
#include <qfiledialog.h>
#include <qfileinfo.h>
#include <qlabel.h>
#include <qpointer.h>
#include <qprintdialog.h>
#include <qprinter.h>
#include <qprinterinfo.h>
#include <qscreen.h>
#include <qset.h>
#include <qsettings.h>
#include <qstringconverter.h>
#include <qmenubar.h>
#include <qmessagebox.h>
#include <qstatusbar.h>
#include <qtimer.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>
#include <qinputdialog.h>
#include <qlineedit.h>
#include <qpainter.h>
#include <qsplitter.h>
#include <qtimer.h>

#include <qurl.h>
#include <qwebenginehistory.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

#include <qdebug.h>

// TOR02/TOR07: the tor window's violet chrome — one bar treatment
// shared by the navigation and bookmarks toolbars so the header band
// reads as a single surface.
static QString torBarStyleSheet()
{
    return QLatin1String(
        "QToolBar { background: #4a3372; border: none; }"
        "QToolButton { color: #eee; background: transparent; }"
        "QToolButton:hover { background: #5d4491; }"
        "QToolButton:pressed { background: #3a2a5c; }");
}

// TOR07: the tab strip takes the same treatment as the rest of the
// header: inactive tabs read as chrome (mid violet), the selected tab
// is the light "field" element — the same inside-the-violet treatment
// the location bar carries, resolved from the live palette so it
// tracks the field in day and night schemes alike.  Re-applied on
// chromeSchemeChanged so a live theme preview keeps it in sync.
static QString torTabBarStyleSheet()
{
    const QPalette palette = qApp ? qApp->palette() : QPalette();
    return QString::fromLatin1(
        "QTabBar { background: #4a3372; }"
        // 6px vertical padding pushes the stylesheet's natural tab
        // hint past TabBar::tabSizeHint's TABS07 cap, so the bound
        // clamps to exactly the same thickness a normal strip gets.
        "QTabBar::tab { background: #5d4491; color: #eee;"
        " border: 1px solid #3a2a5c; padding: 6px 8px; }"
        "QTabBar::tab:top { border-bottom: none;"
        " border-top-left-radius: 4px; border-top-right-radius: 4px; }"
        "QTabBar::tab:bottom { border-top: none;"
        " border-bottom-left-radius: 4px; border-bottom-right-radius: 4px; }"
        "QTabBar::tab:left { border-right: none;"
        " border-top-left-radius: 4px; border-bottom-left-radius: 4px; }"
        "QTabBar::tab:right { border-left: none;"
        " border-top-right-radius: 4px; border-bottom-right-radius: 4px; }"
        "QTabBar::tab:selected { background: %1; color: %2; }"
        "QTabBar::tab:!selected:hover { background: #6b4fa3; }")
        .arg(palette.color(QPalette::Base).name(),
             palette.color(QPalette::Text).name());
}

// TOR14: the chrome's bottom edge — the status bar joins the violet
// treatment so the circuit label, the MEM/NET/loading/zoom widgets
// and the transient message line read light-on-violet instead of the
// app palette's dark text.  All fixed colors (unlike the tab strip's
// field element), so a live theme preview needs no re-resolution.
static QString torStatusBarStyleSheet()
{
    return QLatin1String(
        "QStatusBar { background: #4a3372; color: #eee; }"
        "QStatusBar::item { border: none; }"
        "QLabel { color: #eee; background: transparent; }"
        "QToolButton { color: #eee; background: transparent; }"
        "QToolButton:hover { background: #5d4491; }"
        "QToolButton:pressed { background: #3a2a5c; }"
        "QProgressBar { background: #3a2a5c; border: none;"
        " border-radius: 3px; }"
        "QProgressBar::chunk { background: #b9aecf; }");
}

// ICONS06: the app-menu button's bear stays the colored brand mark
// on the violet bar, but its dark-blue body blends into #4a3372 at
// 28px.  A soft light disc behind the mark gives the dark parts a
// surface to read against — deliberately translucent so it reads as
// a subtle backing plate, not a white sticker.
static QIcon torAppMenuIcon()
{
    const int extent = 28;
    const QPixmap bear = QIcon(QLatin1String(":/arora.svg"))
        .pixmap(QSize(extent, extent));
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0xee, 0xee, 0xee, 150));
    painter.drawEllipse(QRectF(1.5, 1.5, extent - 3, extent - 3));
    painter.drawPixmap(0, 0, bear);
    painter.end();
    return QIcon(pixmap);
}

BrowserMainWindow::BrowserMainWindow(QWidget *parent, Qt::WindowFlags flags)
    : QMainWindow(parent, flags)
    , m_sidebarGrip(nullptr)
    , m_sidebarGripLayout(nullptr)
    , m_sidebarWidthTimer(new QTimer(this))
    , m_applyingSidebarWidth(false)
    , m_sidebarWidthApplied(false)
    , m_sidebarAutoClose(false)
    , m_navigationBar(nullptr)
    , m_navigationSplitter(nullptr)
    , m_engineIndicatorHost(nullptr)
    , m_engineIndicator(nullptr)
    , m_readerModeButton(nullptr)
    , m_navReaderAction(nullptr)
    , m_torCircuitButton(nullptr)
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

#if !defined(Q_OS_MACOS)
        // TOR11: the OS title bar cannot be tinted per-window — on
        // GNOME Wayland Qt's own client-side decoration follows only
        // the session-wide portal color-scheme, and _GTK_THEME_VARIANT
        // exists solely on X11 — so a tor window goes frameless and
        // its menu row doubles as a drawn title bar (badge + title +
        // window controls + violet frame ring).  Drag and edge resize
        // still delegate to the WM through startSystemMove/
        // startSystemResize.  macOS keeps the native look: its
        // traffic-light bar is already part of the unified chrome.
        setWindowFlag(Qt::FramelessWindowHint);
        new TorTitleBar(this);
#endif

        // Unmistakable chrome accent — violet navigation bar + badge,
        // and a "(Tor)" marker in the window title — so a tor window
        // is never mistaken for a normal one at a glance.
        m_navigationBar->setStyleSheet(torBarStyleSheet());

        // TOR07: the violet used to stop at the navigation bar, so
        // the menu row stayed on the default palette — three
        // unrelated shades in the header block.  The menu bar takes
        // the same treatment; its popup menus are intentionally left
        // on the platform palette.
        menuBar()->setStyleSheet(QLatin1String(
            "QMenuBar { background: #4a3372; }"
            "QMenuBar::item { color: #eee; background: transparent; }"
            "QMenuBar::item:selected { background: #5d4491; }"
            "QMenuBar::item:pressed { background: #3a2a5c; }"));
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
        // TOR08: same lift for the circuit button's painted onion —
        // its PaletteChange handler repaints the glyph.
        if (m_torCircuitButton) {
            QPalette circuitPalette = m_torCircuitButton->palette();
            circuitPalette.setColor(QPalette::ButtonText,
                                    QColor(238, 238, 238));
            m_torCircuitButton->setPalette(circuitPalette);
        }

        // ICONS04/ICONS06: the violet bar sits on the application's
        // light palette, so themed icons resolve their dark-glyph
        // variants — mud on purple — and under "native" (the default
        // pick) there is no -dark art to pin at all.  Qt's generated
        // Disabled ghosting then rendered the idle back/forward
        // arrows paler than the enabled reload.  Tint every nav
        // glyph flat #eee — the same value the palette-painted
        // reader/onion buttons take — and seed the Disabled mode
        // with the tab strip's dimmed #b9aecf so the state stays
        // deliberate instead of washed.  The menu actions keep the
        // normal icons — their popups stay on the platform palette,
        // not the violet bar.
        const QColor torGlyph(0xee, 0xee, 0xee);
        const QColor torGlyphDisabled(0xb9, 0xae, 0xcf);
        m_reloadIcon = AroraIcon::getTinted(
            QLatin1String("view-refresh"), torGlyph, torGlyphDisabled);
        m_stopIcon = AroraIcon::getTinted(
            QLatin1String("process-stop"), torGlyph, torGlyphDisabled);
        m_stopReloadAction->setIcon(m_reloadIcon);
        m_historyBackAction->setIcon(AroraIcon::getTinted(
            QLatin1String("go-previous"), torGlyph, torGlyphDisabled));
        m_historyForwardAction->setIcon(AroraIcon::getTinted(
            QLatin1String("go-next"), torGlyph, torGlyphDisabled));
        // HOME01: same lift for the nav-bar Home button — its History
        // menu entry shows the light glyph too, matching Back/Forward.
        m_historyHomeAction->setIcon(AroraIcon::getTinted(
            QLatin1String("go-home"), torGlyph, torGlyphDisabled));
        m_viewSidebarAction->setIcon(AroraIcon::tinted(
            SidebarPanel::icon(m_readerModeButton),
            torGlyph, torGlyphDisabled));

        // ICONS06: the brand mark keeps its colors but gains a soft
        // light backing so the dark-blue body does not fade into the
        // violet; the ButtonText lift reaches the menu-indicator
        // arrow, which follows the palette, not the stylesheet.
        m_appMenuButton->setIcon(torAppMenuIcon());
        QPalette appMenuPalette = m_appMenuButton->palette();
        appMenuPalette.setColor(QPalette::ButtonText, torGlyph);
        m_appMenuButton->setPalette(appMenuPalette);

        // TOR14: the status bar joins the violet chrome.  The
        // transient message line is painted straight from
        // palette().windowText() (it bypasses the stylesheet), so
        // the role is lifted explicitly — children inherit it.
        statusBar()->setStyleSheet(torStatusBarStyleSheet());
        QPalette statusPalette = statusBar()->palette();
        statusPalette.setColor(QPalette::WindowText,
                               QColor(0xee, 0xee, 0xee));
        statusBar()->setPalette(statusPalette);
        // The zoom +/- glyphs are themed icons — dark art on the
        // violet bar.  Tint them to the nav cluster's flat #eee.
        m_zoomControl->useOnDarkIcons();

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

    if (BrowserApplication::isTorMode()) {
        // TOR07: the rest of the header block joins the violet chrome
        // — the bookmarks toolbar shares the navigation bar's sheet,
        // and the tab strip gets its own rules (selected tab = the
        // light "field" element, inactive tabs = violet chrome).
        // The objectName scopes the pane rule so a QTabWidget hosted
        // inside a widget tab never picks the chrome colors up.
        m_bookmarksToolbar->setStyleSheet(torBarStyleSheet());
        m_tabWidget->setObjectName(QLatin1String("mainTabWidget"));
        m_tabWidget->setStyleSheet(QLatin1String(
            "QTabWidget#mainTabWidget { background: #4a3372; }"
            "QTabWidget#mainTabWidget::pane { border: none; }"));
        // Manual strip paints (the vertical-row background fill, the
        // edge divider, the sleeping-tab badge) take palette() colors
        // — pull them into the same violet family.
        QPalette stripPalette = m_tabWidget->tabBar()->palette();
        stripPalette.setColor(QPalette::Window, QColor(0x4a, 0x33, 0x72));
        stripPalette.setColor(QPalette::Button, QColor(0x4a, 0x33, 0x72));
        stripPalette.setColor(QPalette::WindowText,
                              QColor(0xee, 0xee, 0xee));
        stripPalette.setColor(QPalette::ButtonText,
                              QColor(0xee, 0xee, 0xee));
        stripPalette.setColor(QPalette::Disabled, QPalette::WindowText,
                              QColor(0xb9, 0xae, 0xcf));
        stripPalette.setColor(QPalette::Disabled, QPalette::ButtonText,
                              QColor(0xb9, 0xae, 0xcf));
        stripPalette.setColor(QPalette::Mid, QColor(0x3a, 0x2a, 0x5c));
        TabBar *tabBar = m_tabWidget->tabBar();
        tabBar->setPalette(stripPalette);
        tabBar->setStyleSheet(torTabBarStyleSheet());
        // The selected tab borrows the palette's field colors — a
        // live theme preview re-resolves them so the tab keeps
        // matching the location bar.
        connect(BrowserTheme::themeNotifier(),
                &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
                tabBar, [tabBar]() {
            tabBar->setStyleSheet(torTabBarStyleSheet());
        });

        // TOR17: until the daemon's SOCKS listener is live every page
        // would just die at the fail-closed proxy — the banner says
        // what is happening (connecting % / failure + Retry) instead
        // of leaving a raw ERR_PROXY_CONNECTION_FAILED on every tab.
        m_torBanner = new TorStatusBanner(this);
        layout->insertWidget(layout->indexOf(m_tabWidget), m_torBanner);
        BrowserApplication *app = BrowserApplication::instance();
        m_torBanner->bind(app ? app->torManager() : nullptr);
    }

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
    connect(m_tabWidget, &TabWidget::loadStarted,
            this, [this]() { updateStopReloadState(true); });
    connect(m_tabWidget, &TabWidget::loadFinished,
            this, [this](bool) { updateStopReloadState(false); });
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
            this, [this](Qt::DockWidgetArea area) {
        QSettings settings;
        settings.setValue(QLatin1String("MainWindow/sidebarDockArea"),
                          int(area));
        // SIDE06: the rail follows the dock to the other edge.
        if (m_sidebarPanel)
            m_sidebarPanel->setDockSide(area);
        // RSZ02: the grip re-anchors to the new inner edge.
        updateSidebarGripPlacement();
    });
    // RSZ02: a floating dock has no window edge to resize against —
    // the grip hides while it floats.
    connect(m_sidebarDock, &QDockWidget::topLevelChanged,
            this, [this](bool) { updateSidebarGripPlacement(); });
    // SIDE09: 'Auto-Close Inactive Panel' — fold the dock when focus
    // lands on a widget in this window outside the sidebar (the page,
    // the location bar, a menu's parent...).  Menus/popups raised
    // from the panel itself don't count, focus leaving the window
    // entirely keeps the dock up, and a floating dock never folds.
    connect(qApp, &QApplication::focusChanged,
            this, [this](QWidget *, QWidget *now) {
        if (!m_sidebarAutoClose || !m_sidebarDock->isVisible()
            || m_sidebarDock->isFloating() || !now
            || now->window() != this || m_sidebarDock->isAncestorOf(now)
            || QApplication::activePopupWidget()
            || QApplication::activeModalWidget())
            return;
        m_sidebarDock->hide();
    });
    // RSZ02: debounce for dock width persistence — a native edge
    // drag emits a Resize per pixel; the write lands once the drag
    // ends.
    m_sidebarWidthTimer->setSingleShot(true);
    m_sidebarWidthTimer->setInterval(150);
    connect(m_sidebarWidthTimer, &QTimer::timeout, this, [this]() {
        if (m_sidebarDock)
            persistSidebarWidth(m_sidebarDock->width());
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

    // Add each top-level menu's actions to the main window so
    // shortcuts keep working while the menubar is hidden (MENU06 —
    // the app menu is the reliable source on every window type; a
    // tor window's menubar carries no items at all).
    QList<QAction*> actions = m_appMenu->actions();
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
    // Version 4: MENU06 — the menubar defaults hidden under the
    // app-menu button, so pre-v4 blobs cannot tell "user chose the
    // menubar" from "it was always on"; restoreState treats their
    // saved flag as unset and opens Vivaldi-style.
    int version = 4;
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
    if (marker != BrowserMainWindowMagic || version < 2 || version > 4)
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

    // MENU06: pre-v4 blobs saved "was the menubar up" — always true
    // under the old default — not a real choice.  Migrate to the new
    // hidden-by-default; v4+ carries the explicit toggle.
    if (version < 4)
        showMenuBar = false;

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

    m_menuBarVisible = showMenuBar;
    applyMenuBarVisibility();

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
        // START02: closing the last tab into a fresh one is a
        // user-facing new tab — it lands on the start page.
        m_tabWidget->newTabPage();
}

QAction *BrowserMainWindow::showMenuBarAction() const
{
    return m_viewShowMenuBarAction;
}

void BrowserMainWindow::setupMenu()
{
    // MENU06: the menubar defaults hidden — the app-menu button on
    // the navigation bar carries the same menus.  The toggle in the
    // View menu (or Ctrl+M) still restores the classic bar.
    m_menuBarVisible = false;

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
    m_fileNewPrivateTabAction->setIcon(AroraIcon::get(QLatin1String("view-private")));
    m_fileNewTorWindowAction->setIcon(AroraIcon::get(QLatin1String("tor-onion")));
    m_fileOpenFileAction->setIcon(AroraIcon::get(QLatin1String("document-open")));
    m_fileOpenLocationAction->setIcon(AroraIcon::get(QLatin1String("go-jump")));
    m_fileSaveAsAction->setIcon(AroraIcon::get(QLatin1String("document-save-as")));
    m_fileImportBookmarksAction->setIcon(AroraIcon::get(QLatin1String("document-import")));
    m_fileExportBookmarksAction->setIcon(AroraIcon::get(QLatin1String("document-export")));
    m_filePrintPreviewAction->setIcon(AroraIcon::get(QLatin1String("document-print-preview")));
    m_filePrintAction->setIcon(AroraIcon::get(QLatin1String("document-print")));
    m_filePreferencesAction->setIcon(AroraIcon::get(QLatin1String("preferences-system")));
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
    m_editFindNextAction->setIcon(AroraIcon::get(QLatin1String("go-down")));
    m_editFindPreviousAction->setIcon(AroraIcon::get(QLatin1String("go-up")));
#endif

    // View
    m_viewMenu = new QMenu(menuBar());
    connect(m_viewMenu, &QMenu::aboutToShow,
            this, &BrowserMainWindow::aboutToShowViewMenu);
    menuBar()->addMenu(m_viewMenu);

    m_viewShowMenuBarAction = new QAction(m_viewMenu);
    m_viewShowMenuBarAction->setShortcut(QKeySequence(Qt::ControlModifier | Qt::Key_M));
    connect(m_viewShowMenuBarAction, &QAction::triggered, this, &BrowserMainWindow::viewMenuBar);
    // MENU06: reachable from the app-menu dropdown's View submenu now
    // that the menubar is hidden by default; the window-level
    // addAction keeps the Ctrl+M shortcut and the page context
    // menu's hidden-menubar entry working.
    addAction(m_viewShowMenuBarAction);
    m_viewMenu->addAction(m_viewShowMenuBarAction);

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

    // TABS09: the icon-only view mode is also reachable from the
    // tab strip's context menu — the bar owns the shared action.
    m_viewMenu->addAction(m_tabWidget->tabBar()->iconOnlyTabsAction());

    m_viewStatusbarAction = new QAction(m_viewMenu);
    connect(m_viewStatusbarAction, &QAction::triggered, this, &BrowserMainWindow::viewStatusbar);
    m_viewMenu->addAction(m_viewStatusbarAction);

    // SIDE01: optional Vivaldi-style sidebar dock — off by default.
    // The action is the dock's own toggle action so its check state
    // also tracks the dock's close button; applySidebarSettings() is
    // the persisted authority (settings keys, not the window blob).
    m_sidebarDock = new QDockWidget(this);
    m_sidebarDock->setObjectName(QLatin1String("sidebarDock"));
    // RSZ02: the filter watches for native edge resizes (persisted,
    // debounced) and the first show (applies the stored width — a
    // hidden dock ignores resizeDocks).
    m_sidebarDock->installEventFilter(this);
    m_sidebarDock->setAllowedAreas(Qt::LeftDockWidgetArea
                                 | Qt::RightDockWidgetArea);
    m_viewSidebarAction = m_sidebarDock->toggleViewAction();
    m_viewSidebarAction->setShortcut(QKeySequence(Qt::Key_F4));
    m_viewSidebarAction->setIcon(SidebarPanel::icon(this));
    // ICONS05: the sidebar glyph is painted from ButtonText once —
    // repaint it on a light<->dark flip.  Tor chrome pins its own
    // lifted glyph in the ctor, so the repaint must not reach it.
    // Queued: the signal fires inside applyScheme() before widget
    // palette propagation finishes — painting must see the new one.
    if (!BrowserApplication::isTorMode()) {
        connect(BrowserTheme::themeNotifier(),
                &BrowserTheme::BrowserThemeNotifier::chromeSchemeChanged,
                this, [this]() {
            m_viewSidebarAction->setIcon(SidebarPanel::icon(this));
        }, Qt::QueuedConnection);
    }
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
    // ICONS07: the bar-visibility toggles and engine tools get icons
    // too — names with no bundled-set artwork resolve to the mid-grey
    // legacy glyphs (SHLD01 convention).
    m_viewShowMenuBarAction->setIcon(AroraIcon::get(QLatin1String("view-menubar")));
    m_viewToolbarAction->setIcon(AroraIcon::get(QLatin1String("view-toolbar")));
    m_viewBookmarkBarAction->setIcon(AroraIcon::get(QLatin1String("user-bookmarks")));
    viewTabBarAction->setIcon(AroraIcon::get(QLatin1String("view-tabbar")));
    m_tabWidget->tabBar()->iconOnlyTabsAction()->setIcon(
        AroraIcon::get(QLatin1String("view-icon")));
    m_viewStatusbarAction->setIcon(AroraIcon::get(QLatin1String("view-statusbar")));
    m_viewZoomTextOnlyAction->setIcon(AroraIcon::get(QLatin1String("zoom-text")));
    m_viewReaderAction->setIcon(AroraIcon::get(QLatin1String("text-x-generic")));
    m_viewPipAction->setIcon(AroraIcon::get(QLatin1String("view-pip")));
    m_viewSourceAction->setIcon(AroraIcon::get(QLatin1String("text-html")));
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
    // HOME01: the nav-bar Home button reuses this action, so the icon
    // is unconditional — platform convention only hides it in menus.
    m_historyHomeAction->setIcon(AroraIcon::get(QLatin1String("go-home")));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    m_historyHomeAction->setIconVisibleInMenu(false);
#endif

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
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    tabWidget()->bookmarkTabsAction()->setIcon(
        AroraIcon::get(QLatin1String("bookmark-new")));
#endif

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
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_windowTabSearchAction->setIcon(AroraIcon::get(QLatin1String("edit-find")));
#endif

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

    // DEVT01/DEVT04: engine-facing tools share one submenu under
    // Tools (moved out of View; the context menu's click-to-inspect
    // entry is gone too — this action is the way in).  Qt WebEngine
    // has no DeveloperExtrasEnabled toggle — Chromium DevTools are
    // always available, so the entry opens the inspector on the
    // current page directly.  The bare page action is a no-op until
    // a devToolsPage is bound, so it routes through the shared
    // inspector host (DVT01).
    m_toolsDevToolsMenu = new QMenu(m_toolsMenu);
    m_toolsMenu->addMenu(m_toolsDevToolsMenu);

    m_toolsChromiumDevToolAction = new QAction(m_toolsDevToolsMenu);
    connect(m_toolsChromiumDevToolAction, &QAction::triggered,
            this, [this]() {
        if (!currentTab())
            return;
        // DevTools is engine-bound (capabilities().devTools) — reach
        // the wrapped page through the adapter's escape hatch.
        if (WebEnginePageAdapter *adapter =
                WebEnginePageAdapter::of(currentTab()->enginePage()))
            DevToolsWindow::inspectElement(adapter->webEnginePage());
    });
    m_toolsDevToolsMenu->addAction(m_toolsChromiumDevToolAction);

    // DEVT03/DEVT05/DEVT08: engine-neutral dev tools dock — the BiDi
    // entry is a plain action like the Chromium inspector one, not
    // the dock's checkable toggleViewAction: devtools rows are
    // open/close commands, not persistent states like sidebar
    // visibility, so nothing in this submenu carries a tick.  The
    // dock's own visibility is the only truth — triggering flips it,
    // its close button needs no menu state to stay honest.  No-rust
    // builds have no BiDi backend at all — the entry stays out of
    // the menu there; a tor window keeps it visible but disabled
    // since its process never arms the debug channel.
#ifdef ARORA_RUSTCORE
    m_toolsBidiDevToolAction = new QAction(m_toolsDevToolsMenu);
    connect(m_toolsBidiDevToolAction, &QAction::triggered,
            this, [this]() {
        m_bidiPanelDock->setVisible(!m_bidiPanelDock->isVisible());
    });
    m_toolsBidiDevToolAction->setText(tr("BiDi Dev &Tools"));
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    m_toolsBidiDevToolAction->setIcon(
        AroraIcon::get(QLatin1String("system-run")));
#endif
    if (BrowserApplication::isTorMode()) {
        m_toolsBidiDevToolAction->setEnabled(false);
        m_toolsBidiDevToolAction->setToolTip(
            tr("The debug channel is never armed in Tor windows."));
    }
    m_toolsDevToolsMenu->addAction(m_toolsBidiDevToolAction);
#endif

    m_toolsUserAgentMenu = new UserAgentMenu(m_toolsMenu);
    m_toolsMenu->addMenu(m_toolsUserAgentMenu);

#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // ICONS07: the dev-tools rows get icons too.
    m_toolsChromiumDevToolAction->setIcon(
        AroraIcon::get(QLatin1String("utilities-terminal")));
    m_toolsCommandPaletteAction->setIcon(
        AroraIcon::get(QLatin1String("system-run")));
    m_toolsClearPrivateDataAction->setIcon(
        AroraIcon::get(QLatin1String("edit-clear")));
    m_toolsLockStoreAction->setIcon(
        AroraIcon::get(QLatin1String("security-high")));
#endif

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

    // MENU06: record the top-level menus in menubar order — the
    // app-menu dropdown shares these QMenu objects, and on tor
    // windows applyMenuBarVisibility() strips/re-adds them on the
    // title-bar row.
    for (QAction *menuAction : menuBar()->actions()) {
        if (menuAction->menu())
            m_topMenus.append(menuAction->menu());
    }
    applyMenuBarVisibility();
}

void BrowserMainWindow::aboutToShowViewMenu()
{
    m_viewShowMenuBarAction->setText(m_menuBarVisible ? tr("Hide Menu Bar") : tr("Show Menu Bar"));
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
    // READ02/READ04: the nav-row button follows the same tab — it
    // manages its own checked/suggested state off the view's
    // ReaderMode, and it stays visible on every engine tab.  The
    // article heuristic no longer gates presence; it only drives the
    // "suggested" accent.  Only tabs where reader mode can never
    // apply (widget pages like History/Preferences, non-WebView
    // engine tabs) hide the control.
    m_readerModeButton->setWebView(view);
    m_navReaderAction->setVisible(view != nullptr);
    m_readerModeButton->setVisible(view != nullptr);
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
        WebView *view = m_tabWidget->webView(i);
        if (!view)
            continue;
        // The encoding override is a per-page engine setting — no
        // engine-neutral surface, so the escape hatch applies.
        if (WebEnginePageAdapter *adapter =
                WebEnginePageAdapter::of(view->enginePage()))
            adapter->webEnginePage()->settings()
                ->setDefaultTextEncoding(codec);
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
    m_viewShowMenuBarAction->setText(m_menuBarVisible ? tr("Hide Menu Bar") : tr("Show Menu Bar"));
    m_viewReloadAction->setText(tr("&Reload Page"));
    m_viewStopAction->setText(tr("&Stop"));
    m_viewZoomInAction->setText(tr("Zoom &In"));
    m_viewZoomNormalAction->setText(tr("Zoom &Normal"));
    m_viewZoomOutAction->setText(tr("Zoom &Out"));
    m_viewZoomTextOnlyAction->setText(tr("Zoom &Text Only"));
    m_viewReaderAction->setText(tr("&Reader Mode"));
    m_viewPipAction->setText(tr("Picture-&in-Picture"));
    m_sidebarDock->setWindowTitle(tr("Sidebar"));
    // SIDE05: the dock's single title bar keeps the generic name —
    // same as the submenu it lives under and the engine-neutral BiDi
    // panel inside; the menu action stays "BiDi Dev &Tools" so it
    // reads distinctly next to "Chromium Dev Tool".
    m_bidiPanelDock->setWindowTitle(tr("Development Tools"));
#ifdef ARORA_RUSTCORE
    m_toolsBidiDevToolAction->setText(tr("BiDi Dev &Tools"));
#endif
    m_viewSidebarAction->setText(tr("Sidebar"));
    m_viewSourceAction->setText(tr("Page S&ource"));
    m_viewSourceAction->setShortcut(tr("Ctrl+Alt+U"));
    m_viewFullScreenAction->setText(tr("&Full Screen"));
    m_viewTextEncodingAction->setText(tr("Text Encoding"));

    m_historyMenu->setTitle(tr("Hi&story"));
    m_historyBackAction->setText(tr("Back"));
    m_historyForwardAction->setText(tr("Forward"));
    m_historyHomeAction->setText(tr("Home"));
    m_historyHomeAction->setToolTip(
        tr("Home (%1)").arg(m_historyHomeAction->shortcut().toString(
            QKeySequence::NativeText)));
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
    m_toolsDevToolsMenu->setTitle(tr("Development Tools"));
    m_toolsChromiumDevToolAction->setText(tr("Chromium Dev Tool"));
    m_toolsUserAgentMenu->setTitle(tr("User Agent"));

    m_helpMenu->setTitle(tr("&Help"));
    m_helpChangeLanguageAction->setText(tr("Switch application language "));
    m_helpAboutQtAction->setText(tr("About &Qt"));
    m_helpAboutApplicationAction->setText(tr("About &%1", "About Browser").arg(QApplication::applicationName()));

    // Toolbar
    m_navigationBar->setWindowTitle(tr("Navigation"));
    m_bookmarksToolbar->setWindowTitle(tr("&Bookmarks"));

    m_appMenuButton->setText(tr("Arora"));
    m_appMenuButton->setToolTip(tr("Arora menu"));
    m_appMenuButton->setAccessibleName(tr("Arora menu"));

    m_stopReloadAction->setText(tr("Reload / Stop"));
    updateStopReloadActionText(false);
}

// MENU08: sets `font` on `menu` and every submenu reachable through
// its actions.  Popup windows never inherit a parent widget's font,
// so styling a top-level menu leaves its flyouts on the default size
// without the walk; `seen` keeps the shared QMenu objects (MENU06)
// from being revisited.
static void growMenuTreeFont(QMenu *menu, const QFont &font,
                             QSet<QMenu *> *seen)
{
    if (!menu || seen->contains(menu))
        return;
    seen->insert(menu);
    menu->setFont(font);
    const QList<QAction *> actions = menu->actions();
    for (QAction *action : actions)
        growMenuTreeFont(action->menu(), font, seen);
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

    // MENU06: Vivaldi-style application menu — an Arora logo + name
    // button leading the navigation bar whose dropdown lists every
    // real top-level menu.  The QMenu objects are shared, not cloned,
    // so dynamic menus (History/Bookmarks/Window) stay live and the
    // menubar can hide without losing anything.  First widget added,
    // so it anchors the bar ahead of Back on every window type.
    m_appMenu = new QMenu(this);
    for (QMenu *menu : m_topMenus)
        m_appMenu->addMenu(menu);

    m_appMenuButton = new QToolButton(m_navigationBar);
    m_appMenuButton->setObjectName(QLatin1String("appMenuButton"));
    m_appMenuButton->setAutoRaise(true);
    // MENU07: the logo is the brand mark, not a nav glyph — render it
    // bigger than the bar's shared 18px so the button reads as the app
    // menu anchor on every window type.  The bundled vector art stays
    // crisp at 28px and fractional scale factors where a resized png
    // smudges; the text keeps the toolbar font so the growth is
    // logo-led.
    m_appMenuButton->setIcon(QIcon(QLatin1String(":/arora.svg")));
    m_appMenuButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    m_appMenuButton->setPopupMode(QToolButton::InstantPopup);
    m_appMenuButton->setMenu(m_appMenu);
    m_navigationBar->addWidget(m_appMenuButton);
    // Set after addWidget so nothing on the bar's side can clamp it
    // back to the shared nav-glyph size.
    m_appMenuButton->setIconSize(QSize(28, 28));

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

    // HOME01: a Home button immediately after Stop/Reload — the same
    // QAction the History menu and Ctrl+Shift+H drive, so the button
    // can never diverge from the goHome() path.  Shared setup covers
    // normal/private/tor/container windows alike.
    m_navigationBar->addAction(m_historyHomeAction);

    // READ02: reader-mode toggle in the nav row — after stop/reload,
    // before the location bar starts.  READ03: the only reader
    // control — the in-bar duplicate is gone.  Palette-painted glyph
    // (no icon theme ships a reader glyph), hidden until the first
    // updateReaderState() — READ04 keeps it present on every engine
    // tab, accent-marked on article-like pages, checked while the
    // overlay is up.  updateReaderState() re-points it at the
    // current tab along with the View-menu action, so the two can
    // never disagree.
    m_readerModeButton = new ReaderButton(m_navigationBar);
    m_readerModeButton->setObjectName(QLatin1String("navReaderButton"));
    // addWidget() wraps the button in a QWidgetAction — a toolbar
    // item's presence is governed by the ACTION's visibility, so
    // updateReaderState() toggles that (the widget's own hidden flag
    // alone leaves the item latched off).
    m_navReaderAction = m_navigationBar->addWidget(m_readerModeButton);
    m_navReaderAction->setVisible(false);
    m_navReaderAction->setEnabled(true);

    // TOR08: the tor circuit dropdown — a small onion button next to
    // the reader-mode button whose menu lists the live circuit's hops
    // with country flags and names.  Tor windows only; a normal
    // window never constructs it, so no circuit surface exists there
    // at all.
    if (BrowserApplication::isTorMode()) {
        BrowserApplication *app = BrowserApplication::instance();
        m_torCircuitButton = new TorCircuitButton(
            app ? app->torManager() : nullptr, m_navigationBar);
        m_torCircuitButton->setObjectName(
            QLatin1String("torCircuitButton"));
        // TOR15: the popup's "Circuit for <host>" header and the site
        // node at the chain's end read the current tab's host.
        m_torCircuitButton->setSiteHostProvider([this]() {
            WebView *tab = currentTab();
            return tab ? tab->url().host() : QString();
        });
        m_navigationBar->addWidget(m_torCircuitButton);
    }

    m_navigationSplitter = new QSplitter(m_navigationBar);
    m_navigationSplitter->setObjectName(QLatin1String("navigationSplitter"));
    m_navigationSplitter->addWidget(m_tabWidget->locationBarStack());

    // ENG08: the engine switcher is chrome, not part of the field —
    // one shared button hugging the url bar's right edge, replacing
    // the per-bar embedded indicators.  A fixed-width host keeps the
    // cell from stretching inside the splitter and vertically centers
    // the button at nav-row height; hiding the cell (single backend,
    // tor, private or widget tab — see updateEngineIndicator) hands
    // the width back to the location bar with no dead padding.
    m_engineIndicator = new EngineIndicator(m_navigationBar);
    m_engineIndicator->setObjectName(QLatin1String("navEngineIndicator"));
    m_engineIndicator->setIconSize(m_navigationBar->iconSize());
    m_engineIndicatorHost = new QWidget(m_navigationBar);
    m_engineIndicatorHost->setObjectName(
        QLatin1String("navEngineIndicatorHost"));
    QHBoxLayout *indicatorLayout = new QHBoxLayout(m_engineIndicatorHost);
    indicatorLayout->setContentsMargins(2, 0, 2, 0);
    indicatorLayout->addWidget(m_engineIndicator);
    m_engineIndicatorHost->setFixedWidth(
        m_engineIndicator->sizeHint().width() + 4);
    m_navigationSplitter->insertWidget(1, m_engineIndicatorHost);
    m_navigationSplitter->setCollapsible(1, false);
    m_navigationSplitter->setStretchFactor(1, 0);
    // One owner, one path: the request always resolves against the
    // current tab — the button's state mirrors it (below), so the
    // choice can never land on a background tab.
    connect(m_engineIndicator, &EngineIndicator::switchRequested,
            this, [this](const QString &engineId) {
        m_tabWidget->reloadTabInEngine(
            m_tabWidget->currentIndex(), engineId);
    });
    connect(m_tabWidget, &QTabWidget::currentChanged,
            this, [this](int) { updateEngineIndicator(); });
    // The lock state also tracks the strip, not just the current
    // tab: a CONT06 container-level switch refilters the strip and
    // can land on the same index — tabsChanged is the only signal
    // that always follows a level change.
    connect(m_tabWidget, &TabWidget::tabsChanged,
            this, [this]() { updateEngineIndicator(); });
    updateEngineIndicator();

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
    sizes << (int)((double)splitterWidth * .80)
          << m_engineIndicatorHost->sizeHint().width()
          << (int)((double)splitterWidth * .20);
    m_navigationSplitter->setSizes(sizes);

    // MENU08: bigger text under the Arora button — the dropdown and
    // its whole submenu tree paint at ~120% of the platform menu font
    // (MENU07's larger button pairs with a proportionally larger
    // menu).  The QMenus are shared with the menubar, so an unhidden
    // classic bar shows the same enlarged items — consistent either
    // way — while the menubar row itself keeps the default size.
    // Applied this late so the back/forward history flyouts created
    // above are covered too; the flyouts ModelMenu builds lazily at
    // aboutToShow copy their parent menu's font at creation.
    QFont menuFont = m_appMenu->font();
    if (menuFont.pointSizeF() > 0)
        menuFont.setPointSizeF(menuFont.pointSizeF() * 1.2);
    else if (menuFont.pixelSize() > 0)
        menuFont.setPixelSize(qRound(menuFont.pixelSize() * 1.2));
    QSet<QMenu *> menuTreeSeen;
    growMenuTreeFont(m_appMenu, menuFont, &menuTreeSeen);

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

// ENG08: one toolbar-level indicator mirrors the ACTIVE tab.  The
// rules are the same ones the embedded indicators enforced, now
// computed per tab switch instead of baked per bar — with the ENG19
// amendment that a hard lock no longer hides the button: it shows
// disabled with the reason in its tooltip, because an unexplained
// disappearance reads as a bug (the user report was a tor window).
//   - a private tab refuses the swap (no OTR profile on foreign
//     backends) -> locked, disabled, tooltip explains;
//   - a tor window is Chromium-locked (servo has no SOCKS5 — a swap
//     would leak traffic off the proxy) -> locked, disabled;
//   - a non-default container tab level refuses the swap (the
//     append+move+close would re-filter the strip mid-swap — the
//     same guard reloadTabInEngine enforces) -> locked, disabled;
//   - a widget tab (Preferences, History, ...) has no engine -> the
//     whole cell hides — there is nothing to explain;
//   - fewer than two registered backends -> hidden inside
//     EngineIndicator::refresh(), locked or not;
//   - an engine tab reports its own backend id and stays swappable
//     — "Reload in Chromium" on the menu is the swap-back path.
void BrowserMainWindow::updateEngineIndicator()
{
    const int index = m_tabWidget->currentIndex();
    const QString engineId = index >= 0
        ? m_tabWidget->tabEngineId(index) : QString();
    QString lockReason;
    if (BrowserApplication::isTorMode()) {
        lockReason = tr("Tor windows are locked to Chromium — "
                        "switching engines would bypass the Tor proxy");
    } else if (index >= 0 && m_tabWidget->isTabPrivate(index)) {
        lockReason = tr("private tabs stay on the built-in engine — "
                        "other engines have no off-the-record profile");
    } else if (m_tabWidget->containerStripActive()
               && !m_tabWidget->activeContainerHeader().isEmpty()) {
        lockReason = tr("engine switching is unavailable while a "
                        "container tab level is active");
    }
    m_engineIndicator->setLockReason(lockReason);
    m_engineIndicator->setSwappable(index >= 0);
    m_engineIndicator->setEngineId(engineId);
    // The setters refresh() the button's own availability (2+
    // backends, lock state); the host cell gates the one case the
    // button cannot see — a tab with no engine at all.
    m_engineIndicatorHost->setVisible(
        !engineId.isEmpty() && !m_engineIndicator->isHidden());
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
    // SIDE04: the rail's Add button adds the current tab's page —
    // the window owns the tab, the panel supplies the folder.
    connect(m_sidebarPanel, &SidebarPanel::addBookmarkRequested,
            this, &BrowserMainWindow::addSidebarBookmark);
    // DOWN02: the Downloads page header's X folds the dock away (the
    // toggle action + persistence follow via visibilityChanged).
    connect(m_sidebarPanel, &SidebarPanel::closeRequested,
            m_sidebarDock, &QWidget::hide);
    // RSZ02: the dock's content is the panel plus a visible grip
    // along its inner edge — which side the grip sits on follows
    // the dock area (updateSidebarGripPlacement reorders on area
    // changes).
    QWidget *container = new QWidget(m_sidebarDock);
    m_sidebarGripLayout = new QHBoxLayout(container);
    m_sidebarGripLayout->setContentsMargins(0, 0, 0, 0);
    m_sidebarGripLayout->setSpacing(0);
    m_sidebarGrip = new SidebarGrip(m_sidebarDock, container);
    connect(m_sidebarGrip, &SidebarGrip::resizeFinished,
            this, &BrowserMainWindow::persistSidebarWidth);
    m_sidebarGripLayout->addWidget(m_sidebarPanel);
    m_sidebarGripLayout->addWidget(m_sidebarGrip);
    m_sidebarDock->setWidget(container);
    updateSidebarGripPlacement();
    // SIDE06: the rail hugs the dock's outer edge.
    m_sidebarPanel->setDockSide(dockWidgetArea(m_sidebarDock));
    updateSidebarGripPlacement();
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
    if (m_sidebarPanel) {
        m_sidebarPanel->applyPanelVisibility();
        // SIDE06: keep the rail on the outer edge after a
        // settings-driven dock move.
        m_sidebarPanel->setDockSide(area);
        // SIDE09: the working panel options (rail toggle, page-header
        // X, folder counters, lazy load) re-read their keys here.
        m_sidebarPanel->applyOptions();
    }
    // SIDE09: 'Auto-Close Inactive Panel' — the focusChanged hook.
    m_sidebarAutoClose = settings.value(
        QLatin1String("sidebar/autoCloseInactive"), false).toBool();
    // SIDE09: 'Show Close Button' — the dock's title-bar X joins the
    // per-panel header X the panel itself gates.
    QDockWidget::DockWidgetFeatures features = m_sidebarDock->features();
    if (settings.value(QLatin1String("sidebar/showCloseButton"), true)
            .toBool())
        features |= QDockWidget::DockWidgetClosable;
    else
        features &= ~QDockWidget::DockWidgetClosable;
    if (features != m_sidebarDock->features())
        m_sidebarDock->setFeatures(features);
    // RSZ02: the stored width lands before the dock shows; the
    // dock's first Show re-applies it via the event filter.
    applySidebarWidth();
    m_sidebarDock->setVisible(show);
    updateSidebarGripPlacement();
}

void BrowserMainWindow::applySidebarWidth()
{
    if (!m_sidebarDock)
        return;
    QSettings settings;
    const int stored =
        settings.value(QLatin1String("MainWindow/sidebarWidth"),
                       SidebarGrip::defaultWidth()).toInt();
    m_applyingSidebarWidth = true;
    resizeDocks({ m_sidebarDock },
                { SidebarGrip::clampedWidth(stored, width()) },
                Qt::Horizontal);
    m_applyingSidebarWidth = false;
}

void BrowserMainWindow::persistSidebarWidth(int width)
{
    // Same convention as the dock-area write above — an immediate
    // QSettings key, no autosaver churn.
    QSettings settings;
    settings.setValue(QLatin1String("MainWindow/sidebarWidth"), width);
}

void BrowserMainWindow::updateSidebarGripPlacement()
{
    if (!m_sidebarGrip || !m_sidebarGripLayout)
        return;
    const Qt::DockWidgetArea area = dockWidgetArea(m_sidebarDock);
    m_sidebarGrip->setDockArea(area);
    // The grip hugs the content-facing edge — after the panel when
    // docked left, before it when docked right.
    m_sidebarGrip->setVisible(!m_sidebarDock->isFloating()
        && (area == Qt::LeftDockWidgetArea
            || area == Qt::RightDockWidgetArea));
    const int gripIndex = m_sidebarGripLayout->indexOf(m_sidebarGrip);
    if (area == Qt::RightDockWidgetArea) {
        if (gripIndex != 0) {
            m_sidebarGripLayout->removeWidget(m_sidebarGrip);
            m_sidebarGripLayout->insertWidget(0, m_sidebarGrip);
        }
    } else if (gripIndex != m_sidebarGripLayout->count() - 1) {
        m_sidebarGripLayout->removeWidget(m_sidebarGrip);
        m_sidebarGripLayout->addWidget(m_sidebarGrip);
    }
}

bool BrowserMainWindow::eventFilter(QObject *object, QEvent *event)
{
    if (object == m_sidebarDock) {
        // RSZ02: a hidden dock ignores resizeDocks, so the stored
        // width lands again on the first show.  Afterwards every
        // resize we didn't perform ourselves (the native edge drag)
        // is persisted, debounced to one write per gesture.
        if (event->type() == QEvent::Show && !m_sidebarWidthApplied) {
            m_sidebarWidthApplied = true;
            QTimer::singleShot(0, this,
                               &BrowserMainWindow::applySidebarWidth);
        } else if (event->type() == QEvent::Resize
                   && m_sidebarWidthApplied
                   && !m_applyingSidebarWidth
                   && m_sidebarDock->isVisible()
                   && !m_sidebarDock->isFloating()) {
            m_sidebarWidthTimer->start();
        }
    }
    return QMainWindow::eventFilter(object, event);
}

SidebarPanel *BrowserMainWindow::sidebarPanel() const
{
    return m_sidebarPanel;
}

QDockWidget *BrowserMainWindow::sidebarDock() const
{
    return m_sidebarDock;
}

// SIDE04: "Show All Bookmarks" raises the sidebar dock on the
// Bookmarks rail instead of spawning a separate window — the same
// single-instance pattern downloadManager() uses above.  The
// BookmarksDialog class itself stays (tests still exercise it); this
// path simply stops instantiating it.
void BrowserMainWindow::showBookmarksDialog()
{
    // SIDE02/SIDE07: "Show All Bookmarks" raises the sidebar on its
    // Bookmarks section — the dock is the bookmark surface, the same
    // pattern Tools > Downloads follows (DOWN02).
    ensureSidebarPanel();
    m_sidebarDock->setVisible(true);
    m_sidebarDock->raise();
    m_sidebarPanel->showBookmarks();
}

// The rail's Add button — the same AddBookmarkDialog the toolbar
// star uses, pre-filled with the current tab's page and opened on
// the folder the rail selection resolves to.
void BrowserMainWindow::addSidebarBookmark(const QModelIndex &folder)
{
    AddBookmarkDialog dialog;
    if (WebView *webView = currentTab()) {
        dialog.setUrl(QLatin1String(webView->url().toEncoded()));
        dialog.setTitle(webView->title());
    }
    BookmarksManager *bookmarksManager =
        BrowserApplication::bookmarksManager();
    QModelIndex index = folder;
    if (!index.isValid())
        index = bookmarksManager->bookmarksModel()
                    ->index(bookmarksManager->menu());
    dialog.setCurrentIndex(index);
    dialog.exec();
}

void BrowserMainWindow::addBookmark()
{
    WebView *webView = currentTab();
    // PREFS01: a widget tab (Preferences, History) has no page — the
    // dialog opens with blank fields instead of dereferencing the
    // missing view.
    QString url = webView ? QLatin1String(webView->url().toEncoded())
                          : QString();
    QString title = webView ? webView->title() : QString();

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

QMenu *BrowserMainWindow::appMenu() const
{
    return m_appMenu;
}

void BrowserMainWindow::viewMenuBar()
{
    m_menuBarVisible = !m_menuBarVisible;
    applyMenuBarVisibility();
    m_autoSaver->changeOccurred();
}

// MENU06: the menubar flag applied.  On a tor window the menu row is
// the frameless drawn title bar (TOR11) — hiding it would take the
// window controls with it, so the row stays up and the toggle adds
// or strips the top-level menu items instead.  Everywhere else it
// is a plain visibility flip.
void BrowserMainWindow::applyMenuBarVisibility()
{
    if (BrowserApplication::isTorMode()) {
        for (QMenu *menu : m_topMenus)
            menuBar()->removeAction(menu->menuAction());
        if (m_menuBarVisible) {
            for (QMenu *menu : m_topMenus)
                menuBar()->addMenu(menu);
        }
        menuBar()->setVisible(true);
    } else {
        menuBar()->setVisible(m_menuBarVisible);
    }
    m_viewShowMenuBarAction->setText(
        m_menuBarVisible ? tr("Hide Menu Bar") : tr("Show Menu Bar"));
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
    BrowserApplication::downloadManager()->download(
        currentTab()->enginePage(), currentTab()->url(), true);
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

// PRINT01: a tab showing the shell document view previews/prints the
// staged PDF itself; other engine tabs have no printToPdf so they get
// the honest refusal instead of a silent dead menu entry.
bool BrowserMainWindow::previewEngineTabDocument()
{
    EngineTab *foreign =
        m_tabWidget->engineTab(m_tabWidget->currentIndex());
    if (!foreign)
        return false;
    if (PdfView *pdfView = foreign->pdfView();
            pdfView && pdfView->isReady()) {
        PrintPreviewDialog dialog(this);
        dialog.setDocumentFile(pdfView->localPath(),
                               pdfView->documentTitle());
        dialog.exec();
        return true;
    }
    const QString engine = foreign->backend()
        ? foreign->backend()->displayName()
        : tr("This engine");
    QMessageBox::information(this, tr("Print Preview"),
        tr("%1 cannot produce print output — there is nothing to "
           "preview for this tab.").arg(engine));
    return true;
}

void BrowserMainWindow::filePrintPreview()
{
    QPointer<WebView> view = currentTab();
    if (!view) {
        previewEngineTabDocument();
        return;
    }
    // A tab hosting the shell document view previews the staged
    // document — the engine page under it has nothing to print.
    if (PdfView *pdfView = view->pdfView();
            pdfView && pdfView->isReady()) {
        PrintPreviewDialog dialog(this);
        dialog.setDocumentFile(pdfView->localPath(),
                               pdfView->documentTitle());
        dialog.exec();
        return;
    }
    // The stock QPrintPreviewDialog's paintRequested expects a
    // synchronous paint into a QPrinter while engine printing is
    // async — the preview rendered zero pages.  PrintPreviewDialog
    // renders the page's printToPdf output instead: real pagination,
    // real margins, no borrowed printer to outlive.
    PrintPreviewDialog dialog(this);
    dialog.setPage(view->page(), view->url());
    dialog.exec();
}

void BrowserMainWindow::filePrint()
{
    WebView *view = currentTab();
    if (view) {
        printRequested(view);
        return;
    }
    // Engine tabs can't printToPdf, but one hosting the shell
    // document view still prints its staged PDF; anything else gets
    // the honest refusal rather than a dead Ctrl+P.
    if (EngineTab *foreign =
            m_tabWidget->engineTab(m_tabWidget->currentIndex())) {
        if (PdfView *pdfView = foreign->pdfView();
                pdfView && pdfView->isReady()) {
            pdfView->printDocument();
            return;
        }
        const QString engine = foreign->backend()
            ? foreign->backend()->displayName()
            : tr("This engine");
        QMessageBox::information(this, tr("Print Document"),
            tr("%1 cannot produce print output — there is nothing to "
               "print for this tab.").arg(engine));
    }
}

void BrowserMainWindow::printRequested(WebView *view)
{
    if (!view)
        return;
    // PDF02: a tab showing the shell document view prints through
    // the pdf raster path — engine printing has nothing to paint
    // (the page underneath holds the previous document).
    if (PdfView *pdfView = view->pdfView()) {
        if (pdfView->isReady()) {
            pdfView->printDocument();
            return;
        }
    }
    // Engine printing is asynchronous — the printer must stay alive
    // until printFinished fires. The guard is a child of the view, so
    // the printer is also freed if the view is destroyed mid-print
    // (the connection alone would leak it).
    QPrinter *printer = new QPrinter(QPrinter::HighResolution);
    QPrintDialog dialog(printer, this);
    dialog.setWindowTitle(tr("Print Document"));
    if (dialog.exec() != QDialog::Accepted) {
        delete printer;
        return;
    }
    // CRASH02: an accepted dialog can still describe an unprintable
    // target — on a CUPS host with zero destinations Qt leaves a
    // file-format printer pointed at a *directory* (the user's home
    // dir), and the engine's print path dies trying to paint into it
    // ("QPainter::begin(): Returned false" then a silent exit).  Fail
    // visibly instead of handing the engine a dead printer.
    QString refuseReason;
    if (!printer->isValid()) {
        refuseReason = tr("The selected printer is not valid.");
    } else {
        const bool fileTarget =
            printer->outputFormat() != QPrinter::NativeFormat
            || printer->printerName().isEmpty();
        if (fileTarget) {
            const QFileInfo outFile(printer->outputFileName());
            const QFileInfo outDir(outFile.dir().absolutePath());
            if (printer->outputFileName().isEmpty() || outFile.isDir()
                    || (outFile.exists() && !outFile.isWritable())
                    || (!outFile.exists() && !outDir.isWritable()))
                refuseReason =
                    tr("The output file '%1' cannot be written.")
                        .arg(printer->outputFileName());
        } else if (QPrinterInfo::printerInfo(printer->printerName())
                       .isNull()) {
            refuseReason = tr("The selected printer is not available.");
        }
    }
    if (!refuseReason.isEmpty()) {
        QMessageBox::warning(this, tr("Print Document"),
                             tr("Cannot print: %1").arg(refuseReason));
        delete printer;
        return;
    }
    QObject *printerGuard = new QObject(view);
    connect(printerGuard, &QObject::destroyed,
            [printer] { delete printer; });
    connect(view, &WebView::printFinished, printerGuard,
            [printerGuard](bool) { printerGuard->deleteLater(); });
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
            m_tabWidget->newTabPage();
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
        applyMenuBarVisibility();
        statusBar()->setVisible(m_statusBarVisible);
    }
}

void BrowserMainWindow::viewPageSource()
{
    if (!currentTab())
        return;

    QString title = currentTab()->title();
    QUrl url = currentTab()->url();
    // toHtml answers asynchronously — the viewer opens once the
    // serialized DOM arrives from the render process.
    currentTab()->enginePage()->toHtml([title, url](const QString &markup) {
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
    if (dialog.exec() == QDialog::Accepted) {
        // TABS05: the undo-close stacks hold urls, titles and history
        // blobs — a private-data wipe drops them in every window, the
        // same trace removal the dialog performs on the stores.
        if (BrowserApplication *application =
                BrowserApplication::instance()) {
            const QList<BrowserMainWindow*> windows =
                application->mainWindows();
            for (BrowserMainWindow *window : windows)
                window->tabWidget()->clearRecentlyClosedTabs();
        } else {
            m_tabWidget->clearRecentlyClosedTabs();
        }
    }
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
    // NAVB01: Qt 6.12 emits page progress only as 0 and 100, so the
    // value alone can never select the stop face — consult the
    // page's real loading state too (mid-flight backends still get
    // their interim values honored).
    WebView *view = currentTab();
    updateStopReloadState(
        (progress > 0 && progress < 100)
        || (view && view->enginePage() && view->enginePage()->isLoading()));
}

void BrowserMainWindow::updateStopReloadState(bool loading)
{
    if (loading) {
        disconnect(m_stopReloadAction, &QAction::triggered, m_viewReloadAction, &QAction::trigger);
        m_stopReloadAction->setIcon(m_stopIcon);
        connect(m_stopReloadAction, &QAction::triggered, m_viewStopAction, &QAction::trigger,
                Qt::UniqueConnection);
        updateStopReloadActionText(true);
    } else {
        disconnect(m_stopReloadAction, &QAction::triggered, m_viewStopAction, &QAction::trigger);
        m_stopReloadAction->setIcon(m_reloadIcon);
        connect(m_stopReloadAction, &QAction::triggered, m_viewReloadAction, &QAction::trigger,
                Qt::UniqueConnection);
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
    // ICONS07: the container rows all carry color chips, so the static
    // entries get icons too — a grey chip keeps "No Container" in
    // column with them.
    QAction *noContainer = m_fileNewContainerTabMenu->addAction(
        tr("&No Container"), this, [this]() {
        m_tabWidget->newTabPageInContainer(
            ContainerManager::defaultContainerId());
    });
    const QList<ContainerManager::Container> containers =
        manager->containers();
    for (const ContainerManager::Container &container : containers) {
        QAction *entry = m_fileNewContainerTabMenu->addAction(
            ContainerManager::colorIcon(container.color),
            SafeText::menu(container.name));
        const QString id = container.id;
        connect(entry, &QAction::triggered, this, [this, id]() {
            m_tabWidget->newTabPageInContainer(id);
        });
    }
    m_fileNewContainerTabMenu->addSeparator();
    QAction *newContainer = m_fileNewContainerTabMenu->addAction(
        tr("New &Container..."), this, [this]() {
        const QString id = ContainerManager::instance()
            ->createContainerInteractive(this);
        if (!id.isEmpty())
            m_tabWidget->newTabPageInContainer(id);
    });
    QAction *manageContainers = m_fileNewContainerTabMenu->addAction(
        tr("&Manage Containers..."),
        m_tabWidget, &TabWidget::manageContainers);
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    noContainer->setIcon(ContainerManager::colorIcon(QColor(0x8a, 0x8a, 0x8a)));
    newContainer->setIcon(AroraIcon::get(QLatin1String("list-add")));
    manageContainers->setIcon(AroraIcon::get(QLatin1String("preferences-system")));
#endif
}

void BrowserMainWindow::aboutToShowBackMenu()
{
    m_historyBackMenu->clear();
    if (!currentTab())
        return;
    Engine::Page *page = currentTab()->enginePage();
    int historyCount = page->historyCount();
    const QList<Engine::HistoryEntry> backItems = page->backItems(historyCount);
    for (int i = backItems.count() - 1; i >= 0; --i) {
        const Engine::HistoryEntry item = backItems.at(i);
        QAction *action = new QAction(this);
        action->setData(-1 * (historyCount - i - 1));
        QIcon icon = BrowserApplication::icon(item.url);
        action->setIcon(icon);
        action->setText(SafeText::menu(item.title));
        m_historyBackMenu->addAction(action);
    }
}

void BrowserMainWindow::aboutToShowForwardMenu()
{
    m_historyForwardMenu->clear();
    if (!currentTab())
        return;
    Engine::Page *page = currentTab()->enginePage();
    int historyCount = page->historyCount();
    const QList<Engine::HistoryEntry> forwardItems = page->forwardItems(historyCount);
    for (int i = 0; i < forwardItems.count(); ++i) {
        const Engine::HistoryEntry item = forwardItems.at(i);
        QAction *action = new QAction(this);
        action->setData(historyCount - i);
        QIcon icon = BrowserApplication::icon(item.url);
        action->setIcon(icon);
        action->setText(SafeText::menu(item.title));
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
            // A widget tab (Preferences, History) has no view to focus.
            if (WebView *view = windows.at(offset)->currentTab())
                view->setFocus();
        }
    }
}

void BrowserMainWindow::openActionUrl(QAction *action)
{
    int offset = action->data().toInt();
    if (!currentTab())
        return;
    Engine::Page *page = currentTab()->enginePage();
    if (offset < 0) {
        const QList<Engine::HistoryEntry> items = page->backItems(-1 * offset);
        if (!items.isEmpty())
            page->goToHistoryEntry(items.first()); // back
    } else if (offset > 0) {
        const QList<Engine::HistoryEntry> items = page->forwardItems(page->historyCount() - offset + 1);
        if (!items.isEmpty())
            page->goToHistoryEntry(items.back()); // forward
    }
}

