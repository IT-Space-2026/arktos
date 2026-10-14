/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
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

#ifndef BROWSERMAINWINDOW_H
#define BROWSERMAINWINDOW_H

#include <qmainwindow.h>
#include <qpointer.h>

class AutoSaver;
class BookmarksToolBar;
class CommandPalette;
class ExtensionActionBar;
class QDockWidget;
class QHBoxLayout;
class QLabel;
class QTimer;
class ReaderButton;
class SidebarGrip;
class SidebarPanel;
class TabWidget;
class ToolbarSearch;
class QToolButton;
class TorCircuitButton;
class TorStatusBanner;
class WebView;
class QSplitter;
class QFrame;
class HistoryMenu;
class BookmarksMenuBarMenu;
class UserAgentMenu;
class LoadingIndicator;
class ZoomControl;
class MemIndicator;
class NetIndicator;

/*!
    The MainWindow of the Browser Application.

    Handles the tab widget and all the actions
 */
class BrowserMainWindow : public QMainWindow
{
    Q_OBJECT

public:
    BrowserMainWindow(QWidget *parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    ~BrowserMainWindow();
    QSize sizeHint() const override;

public:
    static BrowserMainWindow *parentWindow(QWidget *widget);

    TabWidget *tabWidget() const;
    WebView *currentTab() const;
    ToolbarSearch *toolbarSearch() const;
    void applySearchBoxVisibility();
    // SIDE01: applies the persisted sidebar visibility + dock side.
    void applySidebarSettings();
    // May be null — the panel is built lazily on first show.
    SidebarPanel *sidebarPanel() const;
    QDockWidget *sidebarDock() const;
    QByteArray saveState(bool withTabs = true) const;
    bool restoreState(const QByteArray &state);
    QAction *showMenuBarAction() const;
    // MENU06: the app-menu button's dropdown — it shares the real
    // top-level menus (same QMenu objects as the menubar).
    QMenu *appMenu() const;
    // NAVB03: nav-bar edit mode state — exposed for tests; the toggle
    // lives in the bar's context menu.
    bool isNavBarEditMode() const;
    QAction *navBarEditAction() const;
    // CMD01: lazily created Ctrl+Shift+P command palette bound to this
    // window.
    CommandPalette *commandPalette();

public slots:
    void goHome();
    void privacyChanged(bool isPrivate);
    void zoomTextOnlyChanged(bool textOnly);
    void preferences();
    // PREFS01: opens the window's single Preferences tab, focusing the
    // existing one on repeat calls.  page is a SettingsDialog::Page
    // index that deep-links a section; -1 keeps the persisted page.
    void showSettingsPage(int page = -1);
    // HIST02: opens the window's single History tab, focusing the
    // existing one on repeat calls — the same pattern as
    // showSettingsPage() above.
    void showHistoryPage();
    void showCommandPalette();
    // POL02: Ctrl+Shift+A tab search — the palette in tabs-only mode.
    void showTabSearch();

protected:
    void closeEvent(QCloseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *object, QEvent *event) override;

private slots:
    void save();

    void lastTabClosed();

    void loadProgress(int);
    // NAVB01: the combined toolbar action's face — Stop while the
    // current page loads, Reload otherwise.  Driven by
    // TabWidget::loadStarted/loadFinished (Qt 6.12 emits no
    // mid-flight loadProgress values) with loadProgress kept as the
    // real-progress input for backends that provide it.
    void updateStopReloadState(bool loading);
    void updateStatusbar(const QString &string);
    void updateWindowTitle(const QString &title = QString());
    // TOR04: re-renders the tor window's status-bar circuit label
    // from TorManager state + circuit snapshot.  No-op off tor.
    void updateTorCircuitLabel();

    void fileNew();
    void fileOpen();
    void filePrintPreview();
    void filePrint();
    void fileSaveAs();
    void editFind();
    void editFindNext();
    void editFindPrevious();
    void showBookmarksDialog();
    void addBookmark();
    void addBookmarkFolder();
    void zoomIn();
    void zoomNormal();
    void zoomOut();
    void viewMenuBar();
    void viewToolbar();
    void viewBookmarksBar();
    void viewStatusbar();
    void viewPageSource();
    void viewFullScreen(bool enable);
    void viewTextEncoding(QAction *action);
    // READ01: keeps the Reader Mode action's enabled/checked state in
    // sync with the current tab's ReaderMode.
    void updateReaderState();

    void webSearch();
    void clearPrivateData();
    void aboutApplication();
    void downloadManager();
    void selectLineEdit();

    void aboutToShowBackMenu();
    void aboutToShowForwardMenu();
    void aboutToShowViewMenu();
    void aboutToShowWindowMenu();
    // CONT02: the File menu's "New Container Tab" submenu repopulates
    // on open because the container registry is runtime-editable.
    void populateNewContainerTabMenu();
    void aboutToShowTextEncodingMenu();
    void openActionUrl(QAction *action);
    void showWindow();
    void swapFocus();
    // NAVB03: the nav bar's context menu — New Tab / New Private Tab
    // on top, then the edit-mode toggle, then the stock QMainWindow
    // toolbar entries.
    void showNavigationBarContextMenu(const QPoint &position);
    void setNavBarEditMode(bool enabled);

    void printRequested(WebView *view);

private:
    void retranslate();
    void loadDefaultState();
    void setupMenu();
    void setupToolBar();
    void ensureSidebarPanel();
    // RSZ02: sidebar width — the persisted MainWindow/sidebarWidth is
    // applied before the dock shows (and once more when it first
    // appears, because a hidden dock ignores resizeDocks); grip
    // drags, double-click resets and native edge resizes persist it.
    void applySidebarWidth();
    void persistSidebarWidth(int width);
    void updateSidebarGripPlacement();
    void updateStopReloadActionText(bool loading);
    // NAVB03: edit-mode machinery — navBarEditEvent runs the qApp
    // event filter while the mode is on (press picks a bar item,
    // drag shows the drop gap, release reorders); the rest are the
    // drop-target math and the drag lifecycle.
    bool navBarEditEvent(QObject *object, QEvent *event);
    QAction *navBarDropTarget(const QPoint &barPos) const;
    void updateNavBarDropIndicator(const QPoint &barPos);
    void commitNavBarDrag();
    void endNavBarDrag();
    // MENU06: applies m_menuBarVisible — a plain visibility flip on
    // normal windows, but on tor windows the menu row doubles as the
    // drawn title bar (TOR11) so it stays put and the top-level menu
    // items are added/removed instead.
    void applyMenuBarVisibility();

private:
    QMenu *m_fileMenu;
    QAction *m_fileNewWindowAction;
    QMenu *m_fileNewContainerTabMenu;
    QAction *m_fileOpenFileAction;
    QAction *m_fileOpenLocationAction;
    QAction *m_fileSaveAsAction;
    QAction *m_fileImportBookmarksAction;
    QAction *m_fileExportBookmarksAction;
    QAction *m_filePrintPreviewAction;
    QAction *m_filePrintAction;
    QAction *m_fileNewPrivateTabAction;
    QAction *m_fileNewTorWindowAction;
    QAction *m_filePreferencesAction;
    QAction *m_fileCloseWindow;
    QAction *m_fileQuit;

    QMenu *m_editMenu;
    QAction *m_editUndoAction;
    QAction *m_editRedoAction;
    QAction *m_editCutAction;
    QAction *m_editCopyAction;
    QAction *m_editPasteAction;
    QAction *m_editSelectAllAction;
    QAction *m_editFindAction;
    QAction *m_editFindNextAction;
    QAction *m_editFindPreviousAction;

    QMenu *m_viewMenu;
    QAction *m_viewShowMenuBarAction;
    QAction *m_viewToolbarAction;
    QAction *m_viewBookmarkBarAction;
    QAction *m_viewStatusbarAction;
    QAction *m_viewStopAction;
    QAction *m_viewReloadAction;
    QAction *m_viewZoomInAction;
    QAction *m_viewZoomNormalAction;
    QAction *m_viewZoomOutAction;
    QAction *m_viewZoomTextOnlyAction;
    QAction *m_viewReaderAction;
    QAction *m_viewPipAction;
    QAction *m_viewSourceAction;
    QAction *m_viewFullScreenAction;
    QAction *m_viewTextEncodingAction;
    QMenu *m_viewTextEncodingMenu;
    // DEVT01: engine-facing tools under View > Development Tools.
    QMenu *m_viewDevToolsMenu;
    QAction *m_viewChromiumDevToolAction;
    QAction *m_viewBidiDevToolAction;
    QPointer<WebView> m_readerWatchedView;
    QAction *m_viewSidebarAction;
    QDockWidget *m_sidebarDock;
    QPointer<SidebarPanel> m_sidebarPanel;
    // RSZ02: the dock's inner-edge resize grip + its persisted-width
    // bookkeeping.  The grip lives inside a container wrapping
    // SidebarPanel; m_sidebarWidthTimer debounces native edge
    // resizes so one drag lands as one QSettings write.
    SidebarGrip *m_sidebarGrip;
    QHBoxLayout *m_sidebarGripLayout;
    QTimer *m_sidebarWidthTimer;
    bool m_applyingSidebarWidth;
    bool m_sidebarWidthApplied;
    // SIDE09: 'Auto-Close Inactive Panel' (sidebar/autoCloseInactive)
    // — the focusChanged hook folds the dock when focus lands outside
    // it in this window.
    bool m_sidebarAutoClose;

    HistoryMenu *m_historyMenu;
    QAction *m_historyBackAction;
    QAction *m_historyForwardAction;
    QAction *m_historyHomeAction;
    QAction *m_historyRestoreLastSessionAction;

    BookmarksMenuBarMenu *m_bookmarksMenu;
    QAction *m_bookmarksShowAllAction;
    QAction *m_bookmarksAddAction;
    QAction *m_bookmarksAddFolderAction;

    QMenu *m_windowMenu;
    QAction *m_windowTabSearchAction;

    QMenu *m_toolsMenu;
    QAction *m_toolsCommandPaletteAction;
    QAction *m_toolsWebSearchAction;
    QAction *m_toolsDownloadsAction;
    QAction *m_toolsClearPrivateDataAction;
    QAction *m_toolsLockStoreAction;
    UserAgentMenu *m_toolsUserAgentMenu;

    QMenu *m_helpMenu;
    QAction *m_helpChangeLanguageAction;
    QAction *m_helpAboutQtAction;
    QAction *m_helpAboutApplicationAction;

    // Toolbar
    QToolBar *m_navigationBar;
    // MENU06: Vivaldi-style application menu — a logo+name button
    // leading the navigation bar; m_appMenu lists the real top-level
    // menus (m_topMenus records their menubar order) so the menubar
    // itself can hide while every menu stays reachable.
    QToolButton *m_appMenuButton = nullptr;
    QMenu *m_appMenu = nullptr;
    QList<QMenu *> m_topMenus;
    QMenu *m_historyBackMenu;
    QMenu *m_historyForwardMenu;
    QAction *m_stopReloadAction;
    QIcon m_reloadIcon;
    QIcon m_stopIcon;
    QSplitter *m_navigationSplitter;
    ReaderButton *m_readerModeButton;
    QAction *m_navReaderAction;
    // TOR08: circuit dropdown next to the reader button — tor windows
    // only, never constructed otherwise.
    TorCircuitButton *m_torCircuitButton;
    // EXT08: extension action strip in the navigation splitter cell
    // right after the location bar — normal windows only; private and
    // tor windows never construct it (no usable profile may load
    // extensions there).
    ExtensionActionBar *m_extensionActionBar = nullptr;
    ToolbarSearch *m_toolbarSearch;
    // NAVB03: in-place nav-bar customization.  The checkable context
    // menu entry toggles edit mode; while on, every bar item — the
    // location-bar splitter included — can be dragged to a new slot
    // (m_navBarDropIndicator marks the gap).  The order is per-window
    // in-memory; persisted order + Reset land in NAVB04.
    QAction *m_navBarEditAction = nullptr;
    bool m_navBarEditMode = false;
    bool m_navBarDragging = false;
    QPointer<QAction> m_navBarDragAction;
    QPointer<QAction> m_navBarDropBefore;
    QPoint m_navBarDragStart;
    QWidget *m_navBarDropIndicator = nullptr;
    QString m_navBarSavedStyleSheet;
#if defined(Q_OS_MACOS)
    QFrame *m_bookmarksToolbarFrame;
#endif
    BookmarksToolBar *m_bookmarksToolbar;

    TabWidget *m_tabWidget;

    // UIP04: permanent status-bar widgets bound to the current tab.
    LoadingIndicator *m_loadingIndicator;
    ZoomControl *m_zoomControl;
    // SBAR01: per-tab renderer memory + live engine bandwidth.
    MemIndicator *m_memIndicator;
    NetIndicator *m_netIndicator;
    // TOR04: permanent status-bar circuit chain — tor windows only.
    QLabel *m_torCircuitLabel = nullptr;
    // TOR17: honest daemon-state strip above the tabs — tor windows
    // only, visible until the SOCKS listener is live or when launch
    // fails (with a Retry action).
    TorStatusBanner *m_torBanner = nullptr;

    AutoSaver *m_autoSaver;
    QPointer<CommandPalette> m_commandPalette;

    // These store if the user requested the menu/status bars visible. They are
    // used to determine if these bars should be reshown when leaving fullscreen.
    // MENU06: the menubar defaults off — the app-menu button is the
    // menu path; the View-menu toggle restores the classic bar.
    bool m_menuBarVisible = false;
    bool m_statusBarVisible = true;

    friend class BrowserApplication;
};

#endif // BROWSERMAINWINDOW_H

