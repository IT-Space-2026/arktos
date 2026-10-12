/*
 * Copyright (c) 2026, The Arora Authors
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

// COV04: BrowserMainWindow chrome — the private slots are reached
// through QMetaObject::invokeMethod; dialog-producing slots run under
// acceptModal()/rejectModal() timers.
//
// BrowserMainWindow sets WA_DeleteOnClose, so every window in this file
// is heap-allocated and disposed through closeWindow() (close() +
// flush of the DeferredDelete events).

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qwebchannel.h>
#include <qwebengineprofile.h>
#include <qwebenginepage.h>
#include <qcombobox.h>
#include <qdockwidget.h>
#include <qframe.h>
#include <qheaderview.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qmenubar.h>
#include <qmessagebox.h>
#include <qplaintextedit.h>
#include <qprogressbar.h>
#include <qpushbutton.h>
#include <qsortfilterproxymodel.h>
#include <qsplitter.h>
#include <qstatusbar.h>
#include <qtabwidget.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporarydir.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>
#include <qtreeview.h>

#include "browsermainwindow.h"
#include "browserapplication.h"
#include "browsertheme.h"
#include "aroraicon.h"
#include "containermanager.h"
#include "bookmarksdialog.h"
#include "bookmarksmanager.h"
#include "bookmarknode.h"
#include "history.h"
#include "sidebargrip.h"
#include "sidebarpanel.h"
#include "statusbarwidgets.h"
#include "tabwidget.h"
#include "tabbar.h"
#include "readermode.h"
#include "webview.h"
#include "webpage.h"
#include "webviewsearch.h"
#include "locationbar.h"
#include "toolbarsearch.h"
#include "torcircuitbutton.h"
#include "tormanager.h"
#include "tortitlebar.h"
#include "downloadmanager.h"
#include "qtest_arora.h"
#include "qtry.h"

// Exposes the protected event handlers for direct dispatch.
class SubWindow : public BrowserMainWindow
{
public:
    SubWindow(QWidget *parent = nullptr)
        : BrowserMainWindow(parent)
    {
    }

    void sendKeyPress(QKeyEvent *event)
        { keyPressEvent(event); }
    void sendMousePress(QMouseEvent *event)
        { mousePressEvent(event); }
};

// WA_DeleteOnClose turns close() into deleteLater(); pump the deferred
// deletes so the window is gone before the test function returns.
static void closeWindow(QWidget *window)
{
    if (!window)
        return;
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class tst_BrowserMainWindow : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();

    void chromeBasics();
    void statusAndTitle();
    void zoomAndFind();
    void viewToggles();
    void menuPopulation();
    void dialogSlots();
    void historyTab();
    void toolsMenuDedup();
    void fileMenuOrder();
    void appMenuButton();
    void downloadsMenuHome();
    void devToolsSubmenu();
    void iconOnlyTabsMenu();
    void stateSerialization();
    void events();
    void closeConfirm();
    void chromeMetrics();
    void searchBoxVisibility();
    void torSearchBoxHidden();
    void torCircuitStatusLabel();
    void torTabStripLayout();
    void torChromeTheme();
    void torStatusBarTheme();
    void torNavIconsDarkChrome();
    void darkChromeIcons();
    void torCircuitDropdown();
    void torStartPage();
    void torTitleBar();
    void torTitleBarTitle();
    void servoStartPage();
    void privateStartPage();
    void readerModeButton();
    void statusBarWidgets();
    void statusBarIndicators();
    void sidebarPanel();
    void sidebarGripResize();
    void downloadsSidebarPanel();
    void downloadsSidebarFinishedRows();
    void downloadsSidebarMinWidth();
    void widgetTabBookmarksSafety();
    void tabStripCompactHeight();
};

void tst_BrowserMainWindow::initTestCase()
{
    QCoreApplication::setApplicationName("tst_browsermainwindow");
    QSettings settings;
    settings.clear();
    // Keep close() free of the multi-tab confirmation prompt; the
    // prompt's two branches get explicit coverage in closeConfirm().
    settings.setValue(QLatin1String("tabs/confirmClosingMultipleTabs"), false);
    qRegisterMetaType<QWebEnginePage *>("QWebEnginePage*");
    qRegisterMetaType<QAction *>("QAction*");
}


void tst_BrowserMainWindow::chromeBasics()
{
    SubWindow *window = new SubWindow;
    QVERIFY(window->tabWidget());
    QVERIFY(window->currentTab());
    QVERIFY(window->toolbarSearch());
    QVERIFY(window->showMenuBarAction());
    QVERIFY(window->tabWidget()->count() >= 1);
    QVERIFY(!window->sizeHint().isEmpty());
    closeWindow(window);
}

void tst_BrowserMainWindow::statusAndTitle()
{
    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(QMetaObject::invokeMethod(window, "updateStatusbar", Q_ARG(QString, QLatin1String("hovering"))));
    QVERIFY(QMetaObject::invokeMethod(window, "updateWindowTitle", Q_ARG(QString, QLatin1String("A Title"))));
    QVERIFY(window->windowTitle().contains(QLatin1String("A Title")));
    // loadProgress() calls the private updateStopReloadActionText()
    // helper — progress < 100 takes the "Stop" branch, >= 100 the
    // "Reload" branch.
    QVERIFY(QMetaObject::invokeMethod(window, "loadProgress", Q_ARG(int, 42)));
    QVERIFY(QMetaObject::invokeMethod(window, "loadProgress", Q_ARG(int, 100)));
    closeWindow(window);
}

void tst_BrowserMainWindow::zoomAndFind()
{
    SubWindow *window = new SubWindow;
    window->show();
    WebView *view = window->currentTab();
    const qreal start = view->zoomFactor();

    QVERIFY(QMetaObject::invokeMethod(window, "zoomIn"));
    QVERIFY(view->zoomFactor() > start);
    QVERIFY(QMetaObject::invokeMethod(window, "zoomOut"));
    QVERIFY(QMetaObject::invokeMethod(window, "zoomNormal"));
    QCOMPARE(view->zoomFactor(), 1.0);

    // The find bar lives inside the tab widget's stacked search object.
    QVERIFY(QMetaObject::invokeMethod(window, "editFind"));
    QVERIFY(QMetaObject::invokeMethod(window, "editFindNext"));
    QVERIFY(QMetaObject::invokeMethod(window, "editFindPrevious"));

    // zoomTextOnly is a stored preference; toggling re-applies zoom.
    window->zoomTextOnlyChanged(true);
    window->zoomTextOnlyChanged(false);
    closeWindow(window);
}

void tst_BrowserMainWindow::viewToggles()
{
    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(QMetaObject::invokeMethod(window, "viewToolbar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewToolbar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewBookmarksBar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewBookmarksBar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewStatusbar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewStatusbar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewMenuBar"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewMenuBar"));
    QVERIFY(QMetaObject::invokeMethod(window, "selectLineEdit"));
    QVERIFY(QMetaObject::invokeMethod(window, "swapFocus"));
    QVERIFY(QMetaObject::invokeMethod(window, "webSearch"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewFullScreen", Q_ARG(bool, true)));
    QVERIFY(QMetaObject::invokeMethod(window, "viewFullScreen", Q_ARG(bool, false)));
    QVERIFY(QMetaObject::invokeMethod(window, "showWindow"));
    closeWindow(window);
}

// The aboutToShow* slots rebuild menu contents from live state.
void tst_BrowserMainWindow::menuPopulation()
{
    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(QMetaObject::invokeMethod(window, "aboutToShowViewMenu"));
    QVERIFY(QMetaObject::invokeMethod(window, "aboutToShowBackMenu"));
    QVERIFY(QMetaObject::invokeMethod(window, "aboutToShowForwardMenu"));
    QVERIFY(QMetaObject::invokeMethod(window, "aboutToShowWindowMenu"));
    QVERIFY(QMetaObject::invokeMethod(window, "aboutToShowTextEncodingMenu"));

    // openActionUrl navigates the current tab to the action's url.
    // Unparented stack actions — a heap parent would try to delete them
    // when the window is destroyed.
    QAction action;
    action.setData(QUrl(QLatin1String("about:blank")));
    QVERIFY(QMetaObject::invokeMethod(window, "openActionUrl", Q_ARG(QAction *, &action)));

    // viewTextEncoding pulls the codec name out of the action data.
    QAction codec;
    codec.setData(QLatin1String("UTF-8"));
    QVERIFY(QMetaObject::invokeMethod(window, "viewTextEncoding", Q_ARG(QAction *, &codec)));
    closeWindow(window);
}

// Slots that raise dialogs; each runs with a scheduled dismissal.
void tst_BrowserMainWindow::dialogSlots()
{
    SubWindow *window = new SubWindow;
    window->show();

    rejectModal(); // QFileDialog::getOpenFileName
    QVERIFY(QMetaObject::invokeMethod(window, "fileOpen"));
    rejectModal(); // QFileDialog::getSaveFileName
    QVERIFY(QMetaObject::invokeMethod(window, "fileSaveAs"));

    acceptModal(); // AddBookmarkDialog
    QVERIFY(QMetaObject::invokeMethod(window, "addBookmark"));
    acceptModal();
    QVERIFY(QMetaObject::invokeMethod(window, "addBookmarkFolder"));

    acceptModal(); // AboutDialog
    QVERIFY(QMetaObject::invokeMethod(window, "aboutApplication"));

    acceptModal(); // ClearPrivateData
    QVERIFY(QMetaObject::invokeMethod(window, "clearPrivateData"));

    // PREFS01: preferences() opens the settings page as a tab, not a
    // modal — single-instance: a second call only refocuses it.
    const int tabsBeforePrefs = window->tabWidget()->count();
    QVERIFY(QMetaObject::invokeMethod(window, "preferences"));
    int prefsIndex = -1;
    for (int i = 0; i < window->tabWidget()->count(); ++i) {
        if (window->tabWidget()->widget(i)->inherits("SettingsDialog"))
            prefsIndex = i;
    }
    QVERIFY(prefsIndex >= 0);
    QCOMPARE(window->tabWidget()->currentIndex(), prefsIndex);
    QVERIFY(QMetaObject::invokeMethod(window, "preferences"));
    QCOMPARE(window->tabWidget()->count(), tabsBeforePrefs + 1);
    window->tabWidget()->closeTab(prefsIndex);

    // PTAB01: "New Private Tab" opens an off-the-record tab in the
    // same window — no prompt, and the process-global flag stays off.
    QVERIFY(!BrowserApplication::isPrivate());
    const int tabCount = window->tabWidget()->count();
    QVERIFY(QMetaObject::invokeMethod(window->tabWidget(), "newPrivateTab"));
    QCOMPARE(window->tabWidget()->count(), tabCount + 1);
    QVERIFY(window->tabWidget()->isTabPrivate(
        window->tabWidget()->currentIndex()));
    QVERIFY(!BrowserApplication::isPrivate());
    // PTAB02: the private start page loads asynchronously — let the
    // load finish before teardown, or the offscreen WebEngine
    // teardown races an in-flight renderer navigation.
    WebView *privateView = window->currentTab();
    QTRY_VERIFY(privateView->url()
                == QUrl(QLatin1String("qrc:/startpage-private.html"))
                && privateView->page()
                && !privateView->page()->isLoading());

    // fileNew spins up a second window; downloadManager raises the
    // sidebar dock on the Downloads section (DOWN02 — the standalone
    // dialog is gone).
    QVERIFY(QMetaObject::invokeMethod(window, "fileNew"));
    QVERIFY(QMetaObject::invokeMethod(window, "downloadManager"));
    QVERIFY(window->sidebarDock()->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QCOMPARE(panel->tabs()->currentWidget(), panel->downloadsPage());

    const QWidgetList topLevel = QApplication::topLevelWidgets();
    for (QWidget *widget : topLevel) {
        if (widget != window && widget->inherits("QMainWindow"))
            widget->close();
    }
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    closeWindow(window);
}

// HIST02: "Show All History" hosts the history manager in a tab of the
// current window — repeat calls refocus the one tab, entries open into
// a fresh tab beside it, and the page's Close button closes the tab.
void tst_BrowserMainWindow::historyTab()
{
    SubWindow *window = new SubWindow;
    window->show();

    const int tabsBefore = window->tabWidget()->count();
    QVERIFY(QMetaObject::invokeMethod(window, "showHistoryPage"));

    HistoryDialog *page = nullptr;
    int historyIndex = -1;
    for (int i = 0; i < window->tabWidget()->count(); ++i) {
        if (HistoryDialog *candidate = qobject_cast<HistoryDialog *>(
                    window->tabWidget()->widget(i))) {
            page = candidate;
            historyIndex = i;
        }
    }
    QVERIFY(page);
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 1);
    QCOMPARE(window->tabWidget()->currentIndex(), historyIndex);
    QCOMPARE(window->tabWidget()->tabText(historyIndex),
             QLatin1String("History"));

    // It is a tab page, not a floating window.
    for (QWidget *topLevel : QApplication::topLevelWidgets())
        QVERIFY(!qobject_cast<HistoryDialog *>(topLevel));

    // Single-instance: invoking again only refocuses the same tab.
    window->tabWidget()->setCurrentIndex(0);
    QVERIFY(QMetaObject::invokeMethod(window, "showHistoryPage"));
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 1);
    QCOMPARE(window->tabWidget()->currentIndex(), historyIndex);

    // An activated entry opens in a NEW tab; the history tab survives.
    emit page->openUrl(QUrl(QLatin1String("about:blank")),
                       QLatin1String("Blank"));
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 2);
    QCOMPARE(window->tabWidget()->indexOf(page), historyIndex);

    // The page's Close button asks the host to close its tab.
    QAbstractButton *close =
        page->buttonBox->button(QDialogButtonBox::Close);
    QVERIFY(close);
    close->click();
    QCOMPARE(window->tabWidget()->indexOf(page), -1);
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 1);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    closeWindow(window);
}

// MENU01: the Tools menu dropped the redundant "Web Search" and
// "Configure Search Engines" entries — engine management lives on the
// Settings > Search page — but the Ctrl+K shortcut survives as a
// window-level action that still routes to webSearch().
void tst_BrowserMainWindow::toolsMenuDedup()
{
    SubWindow *window = new SubWindow;
    window->show();

    QMenu *toolsMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
            && menuAction->text().contains(QLatin1String("Tools")))
            toolsMenu = menuAction->menu();
    }
    QVERIFY(toolsMenu);
    for (QAction *action : toolsMenu->actions()) {
        QVERIFY2(!action->text().contains(QLatin1String("Search")),
                 qPrintable(action->text()));
        // MENU03: Preferences moved to the File menu — nothing named
        // Options/Preferences may linger in Tools.
        QVERIFY2(!action->text().contains(QLatin1String("Options")),
                 qPrintable(action->text()));
        QVERIFY2(!action->text().contains(QLatin1String("Preferences")),
                 qPrintable(action->text()));
        // MENU05: filter management moved into Preferences > Privacy
        // (Content Blocking > Manage...) — no Ad Block entry in Tools.
        QVERIFY2(!action->text().contains(QLatin1String("Ad Block")),
                 qPrintable(action->text()));
    }
    // The separator that used to precede Options... went with it.
    QVERIFY(!toolsMenu->actions().isEmpty());
    QVERIFY(!toolsMenu->actions().constLast()->isSeparator());

    // The Ctrl+K carrier detached from the menu onto the window.
    QAction *webSearch = nullptr;
    for (QAction *action : window->actions()) {
        if (action->shortcut() == QKeySequence(QStringLiteral("Ctrl+K"))) {
            webSearch = action;
            break;
        }
    }
    QVERIFY(webSearch);
    QVERIFY(!toolsMenu->actions().contains(webSearch));

    // Triggering it still runs webSearch() — SRCH08 hides the box
    // outright on a fresh profile, so the shortcut falls back to the
    // omnibox location bar rather than opening the engines popup.
    webSearch->trigger();
    QVERIFY(!QApplication::activePopupWidget());
    QVERIFY(QApplication::focusWidget() != window->toolbarSearch());
    if (window->isActiveWindow())
        QCOMPARE(QApplication::focusWidget(),
                 static_cast<QWidget *>(
                     window->tabWidget()->currentLocationBar()));

    // Field mode goes back to selecting the box text.
    QSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
    window->applySearchBoxVisibility();
    window->toolbarSearch()->setText(QLatin1String("arora"));
    webSearch->trigger();
    QVERIFY(window->toolbarSearch()->hasSelectedText());
    QSettings().remove(QLatin1String("MainWindow/showSearchBox"));
    closeWindow(window);
}

// MENU02+PTAB01: the New-X group rides at the top of the File menu —
// New Window, New Tab, New Private Tab, New Tor Window — and the old
// app-global "Private Browsing" checkbox is gone (private browsing is
// per-tab now).
void tst_BrowserMainWindow::fileMenuOrder()
{
    SubWindow *window = new SubWindow;
    window->show();

    QMenu *fileMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
            && menuAction->text().contains(QLatin1String("File")))
            fileMenu = menuAction->menu();
    }
    QVERIFY(fileMenu);

    QStringList entries;
    for (QAction *action : fileMenu->actions()) {
        if (action->isSeparator())
            continue;
        QString title = action->menu() ? action->menu()->title()
                                       : action->text();
        entries << title.remove(QLatin1Char('&'));
    }

    const int newWindow = entries.indexOf(QLatin1String("New Window"));
    const int newTab = entries.indexOf(QLatin1String("New Tab"));
    const int newPrivateTab =
        entries.indexOf(QLatin1String("New Private Tab"));
    const int newTor = entries.indexOf(QLatin1String("New Tor Window"));
    const int privateBrowsing =
        entries.indexOf(QLatin1String("Private Browsing..."));
    const int closeWindowIndex =
        entries.indexOf(QLatin1String("Close Window"));
    const int preferencesIndex =
        entries.indexOf(QLatin1String("Preferences..."));
    QVERIFY(newWindow != -1 && newTab != -1 && newPrivateTab != -1
            && newTor != -1);
    QVERIFY(newWindow < newTab);
    QVERIFY(newTab < newPrivateTab);
    QVERIFY(newPrivateTab < newTor);
    QCOMPARE(privateBrowsing, -1);
    QVERIFY(newTor < closeWindowIndex);

    // MENU03: Preferences sits directly above Close Window in the
    // File menu's bottom group, keeping the Ctrl+, shortcut and the
    // PreferencesRole platform hint.
    QCOMPARE(preferencesIndex, closeWindowIndex - 1);
    QAction *prefsAction = nullptr;
    for (QAction *action : fileMenu->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Preferences..."))
            prefsAction = action;
    }
    QVERIFY(prefsAction);
    QCOMPARE(prefsAction->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+,")));
    QCOMPARE(prefsAction->menuRole(), QAction::PreferencesRole);

    // The action fires the tab widget's private-tab slot and carries
    // the incognito-style shortcut (Ctrl+Shift+P is the palette).
    QAction *privateAction = nullptr;
    for (QAction *action : fileMenu->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("New Private Tab"))
            privateAction = action;
    }
    QVERIFY(privateAction);
    QCOMPARE(privateAction->shortcut(),
             QKeySequence(Qt::ControlModifier | Qt::ShiftModifier
                          | Qt::Key_N));
    closeWindow(window);
}

// MENU06: Vivaldi-style application menu — an Arora logo + name
// button leads the navigation bar and drops down every real
// top-level menu (shared QMenu objects in menubar order, so dynamic
// menus stay live).  The menubar hides by default and View > Show
// Menu Bar restores it; the same button is on private and tor
// windows — on tor the menu row is the drawn frameless title bar
// (TOR11), so it stays up and sheds its items instead of hiding.
void tst_BrowserMainWindow::appMenuButton()
{
    SubWindow *window = new SubWindow;
    window->show();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    QToolButton *button = window->findChild<QToolButton *>(
        QLatin1String("appMenuButton"));
    QVERIFY(button);
    QVERIFY(button->autoRaise());
    QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonTextBesideIcon);
    QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(button->text(), QLatin1String("Arora"));
    QVERIFY(!button->icon().isNull());
    QVERIFY(!button->toolTip().isEmpty());
    QVERIFY(!button->accessibleName().isEmpty());

    // First item on the nav bar, ahead of Back.
    QVERIFY(!navBar->actions().isEmpty());
    QCOMPARE(navBar->widgetForAction(navBar->actions().first()),
             static_cast<QWidget *>(button));

    // The dropdown lists the real top-level menus in menubar order —
    // the same QMenu objects the menubar holds, not clones.
    QMenu *appMenu = window->appMenu();
    QVERIFY(appMenu);
    QCOMPARE(button->menu(), appMenu);
    QStringList titles;
    for (QAction *menuAction : appMenu->actions()) {
        QVERIFY(menuAction->menu());
        titles << menuAction->text().remove(QLatin1Char('&'));
    }
    QCOMPARE(titles, QStringList()
             << QLatin1String("File") << QLatin1String("Edit")
             << QLatin1String("View") << QLatin1String("History")
             << QLatin1String("Bookmarks") << QLatin1String("Window")
             << QLatin1String("Tools") << QLatin1String("Help"));
    const QList<QAction *> barMenus = window->menuBar()->actions();
    QCOMPARE(barMenus.count(), appMenu->actions().count());
    for (int i = 0; i < barMenus.count(); ++i)
        QCOMPARE(barMenus.at(i)->menu(),
                 appMenu->actions().at(i)->menu());

    // The menubar is hidden by default; the toggle rides in the
    // dropdown's View submenu and restores the classic bar.
    QVERIFY(!window->menuBar()->isVisible());
    QAction *showBar = window->showMenuBarAction();
    QVERIFY(showBar);
    QMenu *viewMenu = appMenu->actions().at(2)->menu();
    QVERIFY(viewMenu->actions().contains(showBar));
    showBar->trigger();
    QVERIFY(window->menuBar()->isVisible());
    QCOMPARE(showBar->text(), QLatin1String("Hide Menu Bar"));
    showBar->trigger();
    QVERIFY(!window->menuBar()->isVisible());
    QCOMPARE(showBar->text(), QLatin1String("Show Menu Bar"));

    // A File-menu action fires through the shared menu — New Tab is
    // the same QAction the menubar and Ctrl+T use.
    QMenu *fileMenu = appMenu->actions().first()->menu();
    QAction *newTab = window->tabWidget()->newTabAction();
    QVERIFY(fileMenu->actions().contains(newTab));
    const int tabs = window->tabWidget()->count();
    newTab->trigger();
    QCOMPARE(window->tabWidget()->count(), tabs + 1);

    // The window-state blob round-trips the flag both ways — hidden
    // stays hidden, an explicit show survives restore too.
    const QByteArray hiddenState = window->saveState();
    showBar->trigger();
    const QByteArray shownState = window->saveState();
    SubWindow *restoredHidden = new SubWindow;
    QVERIFY(restoredHidden->restoreState(hiddenState));
    restoredHidden->show();
    SubWindow *restoredShown = new SubWindow;
    QVERIFY(restoredShown->restoreState(shownState));
    restoredShown->show();
    const bool hiddenStayed = !restoredHidden->menuBar()->isVisible();
    const bool shownRestored = restoredShown->menuBar()->isVisible();
    // A closing window writes its chrome state as the next window's
    // default — close the hidden one last so the private window below
    // inherits the hidden menubar, not the shown one.
    closeWindow(restoredShown);
    closeWindow(restoredHidden);
    QVERIFY(hiddenStayed);
    QVERIFY(shownRestored);
    showBar->trigger(); // back to the hidden default before closing

    // Private window — same button, same dropdown, hidden menubar.
    BrowserApplication::setPrivate(true);
    SubWindow *privateWindow = new SubWindow;
    privateWindow->show();
    QToolButton *privateButton =
        privateWindow->findChild<QToolButton *>(
            QLatin1String("appMenuButton"));
    const bool privateMenuFull = privateButton
        && privateButton->menu()
        && privateButton->menu()->actions().count() >= 8;
    const bool privateBarHidden =
        !privateWindow->menuBar()->isVisible();
    closeWindow(privateWindow);
    BrowserApplication::setPrivate(false);
    QVERIFY(privateButton);
    QVERIFY(privateMenuFull);
    QVERIFY(privateBarHidden);

    // Tor window — collect state first so a mid-leg failure cannot
    // leave tor mode armed.  The violet menu row is the drawn
    // frameless title bar: it stays visible but carries no items —
    // the menus are in the dropdown, and the toggle re-adds them.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->resize(1000, 600);
    torWindow->show();
    QApplication::processEvents();
    QToolButton *torButton = torWindow->findChild<QToolButton *>(
        QLatin1String("appMenuButton"));
    const bool torRowVisible = torWindow->menuBar()->isVisible();
    const bool torRowBare = torWindow->menuBar()->actions().isEmpty();
    const int torMenuCount = torWindow->appMenu()
        ? torWindow->appMenu()->actions().count() : -1;
    // Grabbed-pixel proof the "Arora" label reads light on violet.
    int lightPixels = 0;
    if (torButton) {
        const QImage grab = torButton->grab().toImage();
        for (int y = 0; y < grab.height(); ++y)
            for (int x = 0; x < grab.width(); ++x) {
                const QColor color = grab.pixelColor(x, y);
                if (color.alpha() > 200 && color.lightness() > 180)
                    ++lightPixels;
            }
    }
    torWindow->showMenuBarAction()->trigger();
    QApplication::processEvents();
    const bool torRowRepopulated =
        !torWindow->menuBar()->actions().isEmpty();
    torWindow->showMenuBarAction()->trigger();
    QApplication::processEvents();
    const bool torRowBareAgain =
        torWindow->menuBar()->actions().isEmpty()
        && torWindow->menuBar()->isVisible();
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);
    QVERIFY(torButton);
    QVERIFY(torRowVisible);
    QVERIFY(torRowBare);
    QCOMPARE(torMenuCount, 8);
    QVERIFY(lightPixels > 0);
    QVERIFY(torRowRepopulated);
    QVERIFY(torRowBareAgain);

    closeWindow(window);
}

// MENU04: Downloads moved out of the Window menu into the Tools
// menu's utility cluster — same text, icon, and Ctrl+Y shortcut, and
// neither menu keeps a doubled/orphaned separator.
void tst_BrowserMainWindow::downloadsMenuHome()
{
    SubWindow *window = new SubWindow;
    window->show();

    QMenu *toolsMenu = nullptr;
    QMenu *windowMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (!menuAction->menu())
            continue;
        if (menuAction->text().contains(QLatin1String("Tools")))
            toolsMenu = menuAction->menu();
        if (menuAction->text().contains(QLatin1String("Window")))
            windowMenu = menuAction->menu();
    }
    QVERIFY(toolsMenu);
    QVERIFY(windowMenu);

    // The Tools copy is the only Downloads entry.
    QAction *downloads = nullptr;
    for (QAction *action : toolsMenu->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Downloads"))
            downloads = action;
    }
    QVERIFY(downloads);
    QCOMPARE(downloads->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+Y")));
    QVERIFY(!downloads->icon().isNull());
    for (QAction *action : windowMenu->actions())
        QVERIFY(!action->text().contains(QLatin1String("Downloads")));

    // No doubled or dangling separators in either menu after the move.
    for (QMenu *menu : {toolsMenu, windowMenu}) {
        bool previousWasSeparator = true;
        for (QAction *action : menu->actions()) {
            QVERIFY2(!(previousWasSeparator && action->isSeparator()),
                     qPrintable(menu->title()));
            previousWasSeparator = action->isSeparator();
        }
        QVERIFY(!menu->actions().constLast()->isSeparator());
    }

    // Triggering it reveals the sidebar focused on Downloads (DOWN02);
    // the manager object is a hidden controller, never a window.
    downloads->trigger();
    QVERIFY(window->sidebarDock()->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QCOMPARE(panel->tabs()->currentWidget(), panel->downloadsPage());
    QVERIFY(!BrowserApplication::downloadManager()->isVisible());

    closeWindow(window);
}

// DEVT01: View > Development Tools holds the engine-tool entries —
// "Chromium Dev Tool" (the working inspector) and "BiDi Dev Tool"
// (disabled placeholder until DEVT02 lands).  The Tools menu drops
// its Web Inspector copy and keeps clean separators.
void tst_BrowserMainWindow::devToolsSubmenu()
{
    SubWindow *window = new SubWindow;
    window->show();

    QMenu *viewMenu = nullptr;
    QMenu *toolsMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (!menuAction->menu())
            continue;
        if (menuAction->text().contains(QLatin1String("View")))
            viewMenu = menuAction->menu();
        if (menuAction->text().contains(QLatin1String("Tools")))
            toolsMenu = menuAction->menu();
    }
    QVERIFY(viewMenu);
    QVERIFY(toolsMenu);

    // The submenu sits in the View menu's developer group, right
    // after Page Source.
    QMenu *devToolsMenu = nullptr;
    int sourceIndex = -1;
    int devToolsIndex = -1;
    const QList<QAction *> viewActions = viewMenu->actions();
    for (int i = 0; i < viewActions.size(); ++i) {
        QAction *action = viewActions.at(i);
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Page Source"))
            sourceIndex = i;
        if (action->menu()
            && action->menu()->title()
                   == QLatin1String("Development Tools")) {
            devToolsMenu = action->menu();
            devToolsIndex = i;
        }
    }
    QVERIFY(devToolsMenu);
    QVERIFY(sourceIndex != -1);
    QCOMPARE(devToolsIndex, sourceIndex + 1);

    // Exactly two entries: the live inspector and the BiDi
    // placeholder (disabled, with a tooltip explaining why).
    QStringList entries;
    QAction *chromium = nullptr;
    QAction *bidi = nullptr;
    for (QAction *action : devToolsMenu->actions()) {
        if (action->isSeparator())
            continue;
        entries << action->text();
        if (action->text() == QLatin1String("Chromium Dev Tool"))
            chromium = action;
        if (action->text() == QLatin1String("BiDi Dev Tool"))
            bidi = action;
    }
    QCOMPARE(entries.size(), 2);
    QVERIFY(chromium);
    QVERIFY(chromium->isEnabled());
    QVERIFY(bidi);
    QVERIFY(!bidi->isEnabled());
    QVERIFY(!bidi->toolTip().isEmpty());

    // One source of truth: Tools has no inspector entry left.
    for (QAction *action : toolsMenu->actions()) {
        QVERIFY2(!action->text().contains(QLatin1String("Inspector")),
                 qPrintable(action->text()));
        QVERIFY2(!action->text().contains(QLatin1String("Dev Tool")),
                 qPrintable(action->text()));
    }
    QVERIFY(!toolsMenu->actions().constLast()->isSeparator());

    closeWindow(window);
}

// TABS09: the icon-only view mode is a checkable entry in the View
// menu — the same QAction the tab context menu offers, toggling the
// bar's mode live.
void tst_BrowserMainWindow::iconOnlyTabsMenu()
{
    SubWindow *window = new SubWindow;
    window->show();

    QMenu *viewMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
            && menuAction->text().contains(QLatin1String("View")))
            viewMenu = menuAction->menu();
    }
    QVERIFY(viewMenu);
    QAction *iconOnly = nullptr;
    for (QAction *action : viewMenu->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Icon-Only Tabs"))
            iconOnly = action;
    }
    QVERIFY(iconOnly);
    QVERIFY(iconOnly->isCheckable());

    TabBar *bar = window->tabWidget()->tabBar();
    QVERIFY(!bar->iconOnlyTabs());
    iconOnly->trigger();
    QVERIFY(bar->iconOnlyTabs());
    QVERIFY(iconOnly->isChecked());
    iconOnly->trigger();
    QVERIFY(!bar->iconOnlyTabs());
    QVERIFY(!iconOnly->isChecked());

    QSettings settings;
    settings.remove(QLatin1String("tabs/iconOnlyTabs"));
    closeWindow(window);
}

// saveState/restoreState serialize the window+tab layout.
void tst_BrowserMainWindow::stateSerialization()
{
    SubWindow *window = new SubWindow;
    window->tabWidget()->newTab();
    const QByteArray state = window->saveState();
    QVERIFY(!state.isEmpty());

    SubWindow *restored = new SubWindow;
    QVERIFY(restored->restoreState(state));
    QVERIFY(restored->tabWidget()->count() >= window->tabWidget()->count() - 1);
    closeWindow(window);
    closeWindow(restored);
}

void tst_BrowserMainWindow::events()
{
    SubWindow *window = new SubWindow;
    window->show();

    QKeyEvent keyEvent(QEvent::KeyPress, Qt::Key_6, Qt::AltModifier);
    window->sendKeyPress(&keyEvent);

    QMouseEvent mouseEvent(QEvent::MouseButtonPress, QPointF(5, 5),
                           QPointF(5, 5), Qt::XButton1, Qt::XButton1,
                           Qt::NoModifier);
    window->sendMousePress(&mouseEvent);

    // printRequested needs a live page; the dialog is rejected.
    if (window->currentTab() && window->currentTab()->webPage()) {
        rejectModal();
        QVERIFY(QMetaObject::invokeMethod(window, "printRequested",
                       Q_ARG(WebView *, window->currentTab())));
    }

    // Last-tab-closed closes the window (and, via WA_DeleteOnClose,
    // schedules its deletion) — leave this last.
    QVERIFY(QMetaObject::invokeMethod(window, "lastTabClosed"));
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// With confirmClosingMultipleTabs on, closeEvent() raises a Yes/No
// warning box — cover both answers.
void tst_BrowserMainWindow::closeConfirm()
{
    QSettings().setValue(QLatin1String("tabs/confirmClosingMultipleTabs"), true);

    QPointer<SubWindow> window = new SubWindow;
    window->tabWidget()->newTab();
    window->show();

    // "No" ignores the event — the window survives.
    QTimer::singleShot(100, qApp, []() {
        if (QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget()))
            box->button(QMessageBox::No)->animateClick();
    });
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!window.isNull());

    // "Yes" accepts — WA_DeleteOnClose schedules the delete.
    QTimer::singleShot(100, qApp, []() {
        if (QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->animateClick();
    });
    window->close();
    QTest::qWait(200);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(window.isNull());

    QSettings().setValue(QLatin1String("tabs/confirmClosingMultipleTabs"), false);
}

// UIP01: the modernized chrome metrics — padded navigation toolbar,
// shared omnibox height on the location bar + toolbar search, and the
// tab strip's minimum thickness.
void tst_BrowserMainWindow::chromeMetrics()
{
    SubWindow *window = new SubWindow;
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    const QMargins margins = navBar->contentsMargins();
    QVERIFY(margins.top() >= 4 && margins.bottom() >= 4);
    QVERIFY(margins.left() >= 4 && margins.right() >= 4);
    QVERIFY(navBar->layout() && navBar->layout()->spacing() >= 4);

    // UIP02: consistent chrome metrics — the nav bar pins one icon
    // size on every platform, the bookmarks bar follows at favicon
    // size, and toolbar buttons stay flat-with-hover (autoRaise)
    // rather than raised Qt4-era frames.
    QCOMPARE(navBar->iconSize(), QSize(18, 18));
    QToolBar *bookmarksBar = window->findChild<QToolBar *>(
        QLatin1String("BookmarksToolbar"));
    QVERIFY(bookmarksBar);
    QCOMPARE(bookmarksBar->iconSize(), QSize(16, 16));
    const QList<QToolButton *> navButtons =
        navBar->findChildren<QToolButton *>();
    QVERIFY(!navButtons.isEmpty());
    for (QToolButton *button : navButtons)
        QVERIFY2(button->autoRaise(), qPrintable(button->text()));

    const int fontHeight = window->fontMetrics().height();
    QLineEdit *locationBar = window->tabWidget()->currentLocationBar();
    QVERIFY(locationBar);
    QVERIFY(locationBar->minimumHeight() >= fontHeight + 12);
    QVERIFY(window->toolbarSearch()->minimumHeight() >= fontHeight + 12);

    // TabBar::tabSizeHint() is protected — the bar's aggregate
    // sizeHint height is >= the floored per-tab thickness.
    TabBar *bar = window->tabWidget()->tabBar();
    QVERIFY(bar->sizeHint().height() >= bar->fontMetrics().height() + 10);
    closeWindow(window);
}

// SRCH03+SRCH08: the dedicated search box is opt-in — showSearchBox
// unset/false hides the widget outright (the SRCH04 collapsed-button
// mode is retired), true gives the full text field.  The preference
// applies live without restart.
void tst_BrowserMainWindow::searchBoxVisibility()
{
    // initTestCase cleared settings; no showSearchBox key = hidden.
    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(!window->toolbarSearch()->isButtonMode());
    QVERIFY(window->toolbarSearch()->isHidden());
    QVERIFY(!window->toolbarSearch()->isVisible());

    // A hidden box must not strand focus — the shortcut falls back to
    // the omnibox location bar.
    window->activateWindow();
    QApplication::processEvents();
    QVERIFY(QMetaObject::invokeMethod(window, "webSearch"));
    QVERIFY(QApplication::focusWidget() != window->toolbarSearch());

    // Field mode applies live and the shortcut selects the box text.
    QSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
    window->applySearchBoxVisibility();
    QVERIFY(!window->toolbarSearch()->isButtonMode());
    QVERIFY(!window->toolbarSearch()->isHidden());
    QVERIFY(!window->toolbarSearch()->isReadOnly());
    window->toolbarSearch()->setText(QLatin1String("arora"));
    QVERIFY(QMetaObject::invokeMethod(window, "webSearch"));
    QVERIFY(window->toolbarSearch()->hasSelectedText());
    closeWindow(window);

    // New windows read the persisted key at construction.
    SubWindow *shown = new SubWindow;
    shown->show();
    QVERIFY(!shown->toolbarSearch()->isButtonMode());
    QVERIFY(!shown->toolbarSearch()->isHidden());
    closeWindow(shown);

    // Back to hidden live — the toggle works in both directions.
    SubWindow *off = new SubWindow;
    off->show();
    QSettings().remove(QLatin1String("MainWindow/showSearchBox"));
    off->applySearchBoxVisibility();
    QVERIFY(off->toolbarSearch()->isHidden());
    closeWindow(off);
}

// TOR03: a tor window never carries the dedicated search box — even
// collapsed to a button it is a divergent-engine leakage surface.
// The widget stays constructed but hidden, the showSearchBox
// preference cannot resurrect it, and the webSearch shortcut falls
// back to the omnibox location bar instead of stranding focus on an
// invisible widget.
void tst_BrowserMainWindow::torSearchBoxHidden()
{
    // Collect state first and assert after cleanup — a mid-test
    // QVERIFY abort would otherwise leave tor mode armed for the
    // remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->show();
    window->activateWindow();
    QApplication::processEvents();

    ToolbarSearch *search = window->toolbarSearch();
    QLineEdit *bar = window->tabWidget()->currentLocationBar();
    if (!search || !bar) {
        closeWindow(window);
        BrowserApplication::setTorMode(false);
        QFAIL("tor window missing toolbarSearch/locationBar");
    }
    const bool hiddenAtConstruction = search->isHidden();

    // The opt-in field preference must not resurrect it in tor mode.
    QSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
    window->applySearchBoxVisibility();
    const bool hiddenAfterPref = search->isHidden();

    const bool invoked =
        QMetaObject::invokeMethod(window, "webSearch");
    // Focus must not land on the invisible widget; when the platform
    // grants the window activation it falls through to the omnibox.
    const QWidget *focus = QApplication::focusWidget();
    const bool focusOnSearch = focus == search;
    const bool focusOnBar = focus == bar;
    const bool wasActive = window->isActiveWindow();

    closeWindow(window);
    BrowserApplication::setTorMode(false);
    QSettings().remove(QLatin1String("MainWindow/showSearchBox"));

    QVERIFY(hiddenAtConstruction);
    QVERIFY(hiddenAfterPref);
    QVERIFY(invoked);
    QVERIFY(!focusOnSearch);
    if (wasActive)
        QVERIFY(focusOnBar);
}

// TOR04: a tor window carries a permanent status-bar label that shows
// the live circuit chain; a normal window must not get one.  (No
// daemon is armed in the autotest build — the label renders its
// daemon-off state; chain rendering itself is covered by
// tst_tormanager::circuitInfo + --tor-window-smoke.)
void tst_BrowserMainWindow::torCircuitStatusLabel()
{
    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(!window->findChild<QLabel *>(
                QLatin1String("torCircuitLabel")));
    closeWindow(window);

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->show();
    QLabel *label = torWindow->findChild<QLabel *>(
        QLatin1String("torCircuitLabel"));
    const bool parented = label
        && label->parentWidget()
               == static_cast<QWidget *>(torWindow->statusBar());
    const QString text = label ? label->text() : QString();
    const QString tooltip = label ? label->toolTip() : QString();
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);

    QVERIFY(label);
    QVERIFY(parented);
    QVERIFY(text.startsWith(QLatin1String("Tor:")));
    QVERIFY(!tooltip.isEmpty());
}

// TOR06: the tor window's tab strip keeps the shared TABS07-normalized
// height, and the private-badged favicon at the strip's left edge is
// fully visible — it used to latch at the empty-label size (7x14)
// because the label is installed before its pixmap exists, painting
// the 16px badge center-clipped.  The same path marks PTAB01's private
// tabs, so a normal window's off-the-record tab is checked too.
void tst_BrowserMainWindow::torTabStripLayout()
{
    // A normal window's private tab carries the same masked favicon
    // widget — its strip height is the parity baseline.
    SubWindow *normalWindow = new SubWindow;
    normalWindow->resize(1000, 600);
    normalWindow->show();
    QApplication::processEvents();
    normalWindow->tabWidget()->newPrivateTab();
    QApplication::processEvents();
    const int normalStripHeight =
        normalWindow->tabWidget()->tabBar()->height();
    closeWindow(normalWindow);

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    TabBar *bar = window->tabWidget()->tabBar();
    const int count = bar->count();
    QWidget *icon = count > 0
        ? bar->tabButton(0, bar->freeSide()) : nullptr;
    const QSize iconSize = icon ? icon->size() : QSize();
    const QString iconName = icon ? icon->accessibleName() : QString();
    const bool iconVisible = icon && icon->isVisible();
    const bool iconInsideTab = icon
        && bar->tabRect(0).contains(icon->geometry());
    const int torStripHeight = bar->height();
    // The TABS07 thickness cap — icon+text line plus modest padding.
    const int cap = qMax(bar->fontMetrics().height(),
                         bar->iconSize().height()) + 12;
    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QVERIFY(count >= 1);
    QVERIFY(icon);
    QCOMPARE(iconName, QLatin1String("Page Icon"));
    QVERIFY(iconVisible);
    QCOMPARE(iconSize, QSize(16, 16));
    QVERIFY(iconInsideTab);
    QVERIFY(torStripHeight <= cap);
    QCOMPARE(torStripHeight, normalStripHeight);
}

// TOR07: the tor header block is one coherent violet scheme — the
// menu bar and tab strip used to stay on the default palette under
// the violet navigation bar.  Now the whole chrome block shares the
// treatment: violet surfaces, light text, and the selected tab
// rendered as the light "field" element (the same inside-the-violet
// look the location bar carries).  Normal windows keep the default
// palette everywhere.
void tst_BrowserMainWindow::torChromeTheme()
{
    const auto isViolet = [](const QColor &c) {
        // #4a3372/#5d4491/#3a2a5c family — blue > red > green, dark.
        return c.isValid() && c.blue() > c.red()
            && c.red() > c.green() && c.lightness() < 160;
    };

    // Normal-window baseline: no tor stylesheet, no violet chrome.
    SubWindow *plain = new SubWindow;
    plain->resize(1000, 600);
    plain->show();
    QApplication::processEvents();
    const bool plainMenuSheet = plain->menuBar()->styleSheet()
        .contains(QLatin1String("4a3372"));
    const bool plainTabSheet = plain->tabWidget()->tabBar()
        ->styleSheet().contains(QLatin1String("4a3372"));
    const QImage plainImage = plain->grab().toImage();
    const QColor plainMenuColor = plainImage.pixelColor(
        plain->menuBar()->mapTo(plain, QPoint(
            plain->menuBar()->width() - 10,
            plain->menuBar()->height() / 2)));
    closeWindow(plain);

    QVERIFY(!plainMenuSheet);
    QVERIFY(!plainTabSheet);
    QVERIFY(!isViolet(plainMenuColor));

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    const QString menuSheet = window->menuBar()->styleSheet();
    const QToolBar *bookmarksBar = window->findChild<QToolBar *>(
        QLatin1String("BookmarksToolbar"));
    const QString bookmarksSheet =
        bookmarksBar ? bookmarksBar->styleSheet() : QString();
    TabBar *bar = window->tabWidget()->tabBar();
    const QString tabSheet = bar->styleSheet();
    QLineEdit *field = window->tabWidget()->currentLocationBar();

    const QImage image = window->grab().toImage();
    const QColor menuColor = image.pixelColor(window->menuBar()->mapTo(
        window, QPoint(window->menuBar()->width() - 10,
                       window->menuBar()->height() / 2)));
    const QRect tabRect = bar->tabRect(0);
    const QColor stripColor = image.pixelColor(bar->mapTo(
        window, QPoint(qMin(tabRect.right() + 8, bar->width() - 4),
                       tabRect.center().y())));
    // Bottom-of-tab pixels dodge the favicon label and the title.
    const QColor selectedTabColor = image.pixelColor(bar->mapTo(
        window, QPoint(tabRect.center().x(), tabRect.bottom() - 3)));
    const QColor fieldColor = field ? image.pixelColor(field->mapTo(
        window, QPoint(field->width() - 30, field->height() / 2)))
        : QColor();

    // A second tab leaves tab 0 inactive — it must read as violet
    // chrome while the new current tab stays the light field color.
    window->tabWidget()->newTab();
    QApplication::processEvents();
    const QImage twoTabImage = window->grab().toImage();
    const QRect inactiveRect = bar->tabRect(0);
    const QColor inactiveTabColor = twoTabImage.pixelColor(bar->mapTo(
        window, QPoint(inactiveRect.center().x(),
                       inactiveRect.bottom() - 3)));

    // Offscreen grab for visual review — set ARORA_TOR_GRAB_DIR to a
    // directory and the tor window lands there as tor-window.png.
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        window->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/tor-window.png"));
    }

    // The selected tab borrows the palette's field colors, so a live
    // dark-scheme flip re-tints it to the dark field while the chrome
    // stays violet.
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    const QImage darkImage = window->grab().toImage();
    const QRect darkRect = bar->tabRect(bar->currentIndex());
    const QColor darkTabColor = darkImage.pixelColor(bar->mapTo(
        window, QPoint(darkRect.center().x(), darkRect.bottom() - 3)));
    const QColor darkMenuColor = darkImage.pixelColor(
        window->menuBar()->mapTo(window, QPoint(
            window->menuBar()->width() - 10,
            window->menuBar()->height() / 2)));
    BrowserTheme::applyColorScheme();
    QApplication::processEvents();

    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QVERIFY(menuSheet.contains(QLatin1String("4a3372")));
    QVERIFY(menuSheet.contains(QLatin1String("QMenuBar::item")));
    QVERIFY(bookmarksSheet.contains(QLatin1String("4a3372")));
    QVERIFY(tabSheet.contains(QLatin1String("4a3372")));
    QVERIFY(tabSheet.contains(QLatin1String("QTabBar::tab")));
    QVERIFY(field);
    QVERIFY(isViolet(menuColor));
    QVERIFY(isViolet(stripColor));
    // The selected tab is the light "field" element — matching the
    // location bar, which keeps its normal light treatment.
    QVERIFY(selectedTabColor.lightness() > 180);
    QVERIFY(fieldColor.lightness() > 170);
    QVERIFY(isViolet(inactiveTabColor));
    // Dark scheme: the field element tracks the dark palette while
    // the chrome stays violet.
    QVERIFY(darkTabColor.lightness() < 100);
    QVERIFY(isViolet(darkMenuColor));
}

// TOR14: the tor window's status bar joins the violet chrome — the
// bug report showed the circuit label, the MEM/loading/zoom widgets
// and the transient message line all rendered in the app palette's
// dark text on the violet bar.  The message line is painted from
// palette().windowText() directly (it bypasses the stylesheet), so
// both the sheet and the lifted palette role are asserted, plus
// grabbed pixels proving the text actually renders light.  Normal
// windows keep the platform status bar.
void tst_BrowserMainWindow::torStatusBarTheme()
{
    const auto isViolet = [](const QColor &c) {
        // #4a3372/#5d4491/#3a2a5c family — blue > red > green, dark.
        return c.isValid() && c.blue() > c.red()
            && c.red() > c.green() && c.lightness() < 160;
    };
    const auto iconLuminance = [](const QIcon &icon) -> qlonglong {
        const QImage image = icon.pixmap(QSize(18, 18)).toImage();
        qlonglong lum = 0;
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 100) {
                    lum += qGray(color.rgb());
                    ++opaque;
                }
            }
        return opaque ? lum / opaque : -1;
    };
    // Opaque near-white pixels in a widget grab — the "is text
    // visible" probe.  Transparent background regions don't count.
    const auto lightTextPixels = [](const QImage &image) -> int {
        int light = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 200 && color.lightness() > 180)
                    ++light;
            }
        return light;
    };

    // Deterministic icon art: getOnDark only differs from get()
    // under a bundled theme, and a light palette keeps the normal
    // window on the dark-glyph variants.
    QSettings settings;
    const QVariant savedTheme =
        settings.value(QLatin1String("MainWindow/iconTheme"));
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("adwaita"));
    AroraIcon::applyFromSettings();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Light);

    // Normal-window baseline: no tor sheet, platform palette.
    SubWindow *plain = new SubWindow;
    plain->resize(1000, 600);
    plain->show();
    QApplication::processEvents();
    const bool plainSheet = plain->statusBar()->styleSheet()
        .contains(QLatin1String("4a3372"));
    const QImage plainBar = plain->statusBar()->grab().toImage();
    const QColor plainBackground = plainBar.pixelColor(
        2, plainBar.height() / 2);
    closeWindow(plain);

    QVERIFY(!plainSheet);
    QVERIFY(!isViolet(plainBackground));

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    QStatusBar *bar = window->statusBar();
    const QString sheet = bar->styleSheet();
    const int textLightness = bar->palette()
        .color(QPalette::WindowText).lightness();
    QLabel *circuitLabel = window->findChild<QLabel *>(
        QLatin1String("torCircuitLabel"));
    QLabel *memLabel = window->findChild<QLabel *>(
        QLatin1String("memLabel"));
    QToolButton *zoomOut = window->findChild<QToolButton *>(
        QLatin1String("zoomOutButton"));
    QToolButton *zoomIn = window->findChild<QToolButton *>(
        QLatin1String("zoomInButton"));
    QToolButton *zoomValue = window->findChild<QToolButton *>(
        QLatin1String("zoomValueButton"));

    const QImage barImage = bar->grab().toImage();
    const QColor barBackground = barImage.pixelColor(
        2, barImage.height() / 2);
    const int circuitLight = circuitLabel
        ? lightTextPixels(circuitLabel->grab().toImage()) : -1;
    const int memLight = memLabel
        ? lightTextPixels(memLabel->grab().toImage()) : -1;
    const int zoomValueLight = zoomValue
        ? lightTextPixels(zoomValue->grab().toImage()) : -1;
    const qlonglong zoomOutLum = zoomOut ? iconLuminance(zoomOut->icon())
                                         : -1;
    const qlonglong zoomInLum = zoomIn ? iconLuminance(zoomIn->icon())
                                       : -1;

    // The transient message line — painted by QStatusBar itself from
    // palette().windowText(); the grab proves it lands light.
    bar->showMessage(QLatin1String("https://example.test/hover-link"));
    QApplication::processEvents();
    const QImage messageImage = bar->grab().toImage();
    // The message starts at x=6; sample the left stretch only so a
    // permanent widget can't supply the light pixels.
    const int messageLight = lightTextPixels(messageImage.copy(
        0, 0, qMin(200, messageImage.width()), messageImage.height()));
    bar->clearMessage();
    QApplication::processEvents();

    // A live dark-scheme flip must not take the bar off violet or
    // drop the lifted text role back to dark.
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    const QImage darkBarImage = bar->grab().toImage();
    const QColor darkBarBackground = darkBarImage.pixelColor(
        2, darkBarImage.height() / 2);
    const int darkTextLightness = bar->palette()
        .color(QPalette::WindowText).lightness();
    BrowserTheme::applyColorScheme();
    QApplication::processEvents();

    // Offscreen grab for visual review — set ARORA_TOR_GRAB_DIR to a
    // directory and the tor status bar lands there.
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        bar->grab().save(QString::fromLocal8Bit(grabDir)
                         + QLatin1String("/tor-statusbar.png"));
    }

    closeWindow(window);
    BrowserApplication::setTorMode(false);
    BrowserTheme::applyColorScheme();
    if (savedTheme.isValid())
        settings.setValue(QLatin1String("MainWindow/iconTheme"),
                          savedTheme);
    else
        settings.remove(QLatin1String("MainWindow/iconTheme"));
    AroraIcon::applyFromSettings();

    QVERIFY(sheet.contains(QLatin1String("4a3372")));
    QVERIFY(textLightness > 150);
    QVERIFY(isViolet(barBackground));
    QVERIFY(circuitLabel);
    QVERIFY(circuitLight > 0);
    QVERIFY(memLabel);
    QVERIFY(memLight > 0);
    QVERIFY(zoomValue);
    QVERIFY(zoomValueLight > 0);
    // The +/- glyphs are pinned to the light -dark art.
    QVERIFY(zoomOutLum > 140);
    QVERIFY(zoomInLum > 140);
    // Transient message text lands light on the violet bar.
    QVERIFY(messageLight > 0);
    // Dark scheme: chrome stays violet, text role stays light.
    QVERIFY(isViolet(darkBarBackground));
    QVERIFY(darkTextLightness > 150);
}

// ICONS04: the tor window's violet chrome sits on the light
// application palette, so the nav cluster's themed icons resolved
// dark-on-dark — and worse, the disabled ghosting made the arrows read
// LIGHTER than the enabled reload.  Tor windows pin the toolbar icons
// to the bundled set's -dark (light-glyph) art; menu actions keep the
// normal icons because their popups stay on the platform palette.
void tst_BrowserMainWindow::torNavIconsDarkChrome()
{
    const auto luminance = [](const QIcon &icon) -> qlonglong {
        const QImage image = icon.pixmap(QSize(18, 18)).toImage();
        qlonglong lum = 0;
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 100) {
                    lum += qGray(color.rgb());
                    ++opaque;
                }
            }
        return opaque ? lum / opaque : -1;
    };

    // getOnDark only differs from get() under a bundled icon theme —
    // "native" has no -dark art to pin.  Force one; a light palette
    // keeps the normal-window leg on the dark-glyph variants.
    QSettings settings;
    const QVariant savedTheme =
        settings.value(QLatin1String("MainWindow/iconTheme"));
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("adwaita"));
    AroraIcon::applyFromSettings();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Light);

    SubWindow *plain = new SubWindow;
    QToolBar *plainBar = plain->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    // MENU06: the app-menu button leads the bar — Back sits at 1.
    QVERIFY(plainBar && plainBar->actions().count() >= 4);
    const qlonglong plainBack =
        luminance(plainBar->actions().at(1)->icon());
    const qlonglong plainReload =
        luminance(plainBar->actions().at(3)->icon());
    closeWindow(plain);

    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar && navBar->actions().count() >= 4);
    const qlonglong torBack =
        luminance(navBar->actions().at(1)->icon());
    const qlonglong torForward =
        luminance(navBar->actions().at(2)->icon());
    const qlonglong torReload =
        luminance(navBar->actions().at(3)->icon());
    qlonglong menuReload = -1;
    // MENU06: a tor window's menubar row is the bare title bar — the
    // menus live in the app-menu dropdown (same shared QMenu objects).
    for (QAction *menuAction : window->appMenu()->actions()) {
        if (!menuAction->menu()
            || !menuAction->text().contains(QLatin1String("View")))
            continue;
        for (QAction *action : menuAction->menu()->actions())
            if (action->text().remove(QLatin1Char('&'))
                    == QLatin1String("Reload Page"))
                menuReload = luminance(action->icon());
    }

    // Offscreen grab for visual review — set ARORA_TOR_GRAB_DIR to a
    // directory and the tor nav bar lands there as tor-nav-icons.png.
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/tor-nav-icons.png"));
    }

    closeWindow(window);
    BrowserApplication::setTorMode(false);
    BrowserTheme::applyColorScheme();
    if (savedTheme.isValid())
        settings.setValue(QLatin1String("MainWindow/iconTheme"),
                          savedTheme);
    else
        settings.remove(QLatin1String("MainWindow/iconTheme"));
    AroraIcon::applyFromSettings();

    // Light-palette normal window: dark glyphs.  Tor window: light
    // glyphs — same family, readable on the violet bar.
    QVERIFY(plainBack >= 0 && plainBack < 140);
    QVERIFY(plainReload >= 0 && plainReload < 140);
    QVERIFY(torBack > 140);
    QVERIFY(torForward > 140);
    QVERIFY(torReload > 140);
    QVERIFY(menuReload >= 0 && menuReload < 140);
}

// ICONS05: the normal window's chrome under a dark palette.  The
// forced dark palette is invisible to the OS icon theme, so "native"
// resolves to bundled -dark art unless the system theme is provably
// dark-aware — either way the nav cluster, the status-bar zoom
// buttons and the palette-painted sidebar glyph must all render
// light.  Assertions are outcome-only so a genuinely dark-aware
// native theme keeps passing.
void tst_BrowserMainWindow::darkChromeIcons()
{
    const auto luminance = [](const QIcon &icon) -> qlonglong {
        const QImage image = icon.pixmap(QSize(18, 18)).toImage();
        qlonglong lum = 0;
        int opaque = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 100) {
                    lum += qGray(color.rgb());
                    ++opaque;
                }
            }
        return opaque ? lum / opaque : -1;
    };

    // Force the default "native" icon theme — that is the path the
    // bug report hit (a bundled pick already flipped to -dark).
    QSettings settings;
    const QVariant savedTheme =
        settings.value(QLatin1String("MainWindow/iconTheme"));
    settings.remove(QLatin1String("MainWindow/iconTheme"));
    AroraIcon::applyFromSettings();

    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    // MENU06: the app-menu button leads the bar — Back sits at 1.
    QVERIFY(navBar && navBar->actions().count() >= 4);
    QToolButton *zoomOut = window->findChild<QToolButton *>(
        QLatin1String("zoomOutButton"));
    QVERIFY(zoomOut);
    QAction *sidebarAction = nullptr;
    for (QAction *action : navBar->actions()) {
        if (action->shortcut() == QKeySequence(Qt::Key_F4))
            sidebarAction = action;
    }
    QVERIFY(sidebarAction);

    const QImage lightBackImage = navBar->actions().at(1)->icon()
        .pixmap(QSize(18, 18)).toImage();

    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();

    const QImage darkBackImage = navBar->actions().at(1)->icon()
        .pixmap(QSize(18, 18)).toImage();
    const qlonglong darkBack =
        luminance(navBar->actions().at(1)->icon());
    const qlonglong darkForward =
        luminance(navBar->actions().at(2)->icon());
    const qlonglong darkReload =
        luminance(navBar->actions().at(3)->icon());
    const qlonglong darkZoomOut = luminance(zoomOut->icon());
    const qlonglong darkSidebar = luminance(sidebarAction->icon());

    // Offscreen grabs for visual review — set ARORA_DARK_GRAB_DIR to
    // a directory and the dark nav bar + full window land there.
    const QByteArray grabDir = qgetenv("ARORA_DARK_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/dark-nav-icons.png"));
        window->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/dark-window.png"));
    }

    // The flip back must re-resolve the original art — a
    // Day->Night->Day cycle must not leave light glyphs on the light
    // chrome, and live theme icons must actually swap pixels.
    BrowserTheme::applyColorScheme();
    QApplication::processEvents();
    const QImage backImage = navBar->actions().at(1)->icon()
        .pixmap(QSize(18, 18)).toImage();

    closeWindow(window);
    if (savedTheme.isValid())
        settings.setValue(QLatin1String("MainWindow/iconTheme"),
                          savedTheme);
    AroraIcon::applyFromSettings();

    QVERIFY(darkBack > 140);
    QVERIFY(darkForward > 140);
    QVERIFY(darkReload > 140);
    QVERIFY(darkZoomOut > 140);
    QVERIFY(darkSidebar > 140);
    QCOMPARE(backImage, lightBackImage);
    QVERIFY(backImage != darkBackImage);
}

// TOR08: the tor window's nav bar carries a circuit dropdown right
// after the reader button — a painted onion whose menu lists the
// displayed circuit's hops as flag rows with country names.  Normal
// windows never construct it (no circuit surface at all); without a
// daemon the button greys out and the menu reports the state.
void tst_BrowserMainWindow::torCircuitDropdown()
{
    // Helper surface: ISO-name table, flag icons, fabricated-circuit
    // rows — all static so no daemon is needed.
    QCOMPARE(TorCircuitButton::countryName(QLatin1String("de")),
             QLatin1String("Germany"));
    QCOMPARE(TorCircuitButton::countryName(QLatin1String(" US ")),
             QLatin1String("United States"));
    QVERIFY(TorCircuitButton::countryName(QLatin1String("ZZ")).isEmpty());
    QVERIFY(TorCircuitButton::countryName(QString()).isEmpty());
    QVERIFY(!TorCircuitButton::flagIcon(QLatin1String("de")).isNull());
    QVERIFY(!TorCircuitButton::flagIcon(QString()).isNull());
    QCOMPARE(TorCircuitButton::hopText(TorCircuitHop(), 1, 3),
             QStringLiteral("Middle:  — Unknown"));

    TorCircuit circuit;
    circuit.id = 7;
    circuit.status = QLatin1String("BUILT");
    circuit.purpose = QLatin1String("GENERAL");
    TorCircuitHop guard;
    guard.fingerprint = QStringLiteral("AAAABBBBCCCCDDDDEEEEFFFF0000111122223333");
    guard.nickname = QLatin1String("ElToro");
    guard.country = QLatin1String("GB");
    guard.address = QLatin1String("1.2.3.4");
    TorCircuitHop middle;
    middle.fingerprint = QStringLiteral("BBBBCCCCDDDDEEEEFFFF00001111222233334444");
    middle.nickname = QLatin1String("Nightingali");
    middle.country = QLatin1String("DE");
    TorCircuitHop exit;
    exit.fingerprint = QStringLiteral("CCCCDDDDEEEEFFFF000011112222333344445555");
    exit.nickname = QLatin1String("elefant");
    exit.country = QLatin1String("AT");
    circuit.hops << guard << middle << exit;

    QMenu menu;
    QVERIFY(TorCircuitButton::addCircuitRows(&menu,
                                             QList<TorCircuit>() << circuit,
                                             7));
    const QList<QAction *> rows = menu.actions();
    QCOMPARE(rows.count(), 4);            // header section + 3 hops
    QCOMPARE(rows.at(1)->text(),
             QStringLiteral("Guard: ElToro — United Kingdom"));
    QCOMPARE(rows.at(2)->text(),
             QStringLiteral("Middle: Nightingali — Germany"));
    QCOMPARE(rows.at(3)->text(),
             QStringLiteral("Exit: elefant — Austria"));
    for (int i = 1; i < rows.count(); ++i) {
        QVERIFY(!rows.at(i)->icon().isNull());
        QVERIFY(rows.at(i)->isEnabled());
    }
    QVERIFY(rows.at(1)->toolTip().contains(QLatin1String("1.2.3.4")));
    QVERIFY(rows.at(1)->toolTip().contains(guard.fingerprint));
    // A display id with no matching circuit yields no rows.
    QMenu emptyMenu;
    QVERIFY(!TorCircuitButton::addCircuitRows(&emptyMenu,
                                            QList<TorCircuit>() << circuit,
                                            99));

    // Normal window: no button at all.
    SubWindow *plain = new SubWindow;
    plain->show();
    QVERIFY(!plain->findChild<QToolButton *>(
                QLatin1String("torCircuitButton")));
    closeWindow(plain);

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();

    QToolButton *button = window->findChild<QToolButton *>(
        QLatin1String("torCircuitButton"));
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    int readerIndex = -1;
    int circuitIndex = -1;
    if (navBar) {
        const QList<QAction *> actions = navBar->actions();
        for (int i = 0; i < actions.count(); ++i) {
            QWidget *widget = navBar->widgetForAction(actions.at(i));
            if (widget
                    && widget->objectName()
                           == QLatin1String("navReaderButton"))
                readerIndex = i;
            if (widget == button)
                circuitIndex = i;
        }
    }
    // No TorManager in the autotest — the button greys out and its
    // menu reports the missing control connection.
    const bool greyed = button && !button->isEnabled();
    QMenu *dropMenu = button ? button->menu() : nullptr;
    QStringList stateTexts;
    if (dropMenu
            && QMetaObject::invokeMethod(button, "populateMenu")) {
        for (QAction *action : dropMenu->actions())
            stateTexts << action->text();
    }

    // Offscreen grabs for visual review — set ARORA_TOR_GRAB_DIR to a
    // directory and the nav bar, flag glyphs and a fabricated circuit
    // menu land there.
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        const QString dir = QString::fromLocal8Bit(grabDir);
        QDir().mkpath(dir);
        if (navBar)
            navBar->grab().save(dir + QLatin1String(
                "/tor-circuit-button.png"));
        TorCircuitButton::flagIcon(QLatin1String("de"))
            .pixmap(QSize(64, 64)).save(dir + QLatin1String(
                "/flag-de.png"));
        TorCircuitButton::flagIcon(QLatin1String("gb"))
            .pixmap(QSize(64, 64)).save(dir + QLatin1String(
                "/flag-gb.png"));
        TorCircuitButton::flagIcon(QString())
            .pixmap(QSize(64, 64)).save(dir + QLatin1String(
                "/flag-unknown.png"));
        menu.popup(window->geometry().center());
        QApplication::processEvents();
        menu.grab().save(dir + QLatin1String("/circuit-menu.png"));
        menu.hide();
    }

    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QVERIFY(button);
    QVERIFY(dropMenu);
    // MENU06: the app-menu button leads the bar — the nav cluster
    // shifts one slot right.
    QCOMPARE(readerIndex, 4);             // app menu, back, forward, reload, reader
    QCOMPARE(circuitIndex, 5);            // onion right next to it
    QVERIFY(greyed);
    QCOMPARE(stateTexts.count(), 1);
    QVERIFY(stateTexts.at(0).contains(
        QLatin1String("unavailable"), Qt::CaseInsensitive));
}

// TOR09: the tor window's bundled start page — a violet-branded qrc
// variant on the same arora-channel contract.  about:home resolves to
// it, the window's first tab and every fresh/private tab load it, and
// the channel arms only "arora" on qrc commits — the privileged
// bridges stay unregistered in tor.
void tst_BrowserMainWindow::torStartPage()
{
    const QUrl standard(QLatin1String("qrc:/startpage.html"));
    const QUrl torStart(QLatin1String("qrc:/startpage-tor.html"));

    // Normal mode: the standard page answers about:home and fresh
    // tabs stay blank.
    QCOMPARE(BrowserApplication::startPageUrl(), standard);
    QCOMPARE(TabWidget::guessUrlFromString(QLatin1String("about:home")),
             standard);
    SubWindow *plain = new SubWindow;
    plain->show();
    WebView *plainTab = plain->currentTab();
    const QUrl plainUrl =
        plainTab ? plainTab->url() : QUrl(QLatin1String("x:"));
    closeWindow(plain);
    QVERIFY(plainUrl.isEmpty()
            || plainUrl == QUrl(QLatin1String("about:blank")));

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    const QUrl mapped = BrowserApplication::startPageUrl();
    const QUrl guessed =
        TabWidget::guessUrlFromString(QLatin1String("about:home"));

    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();

    // The window's first tab is itself newTab() — it lands on the
    // tor page at once, before any startup navigation fires.
    WebView *firstTab = window->currentTab();
    const QUrl firstUrl = firstTab ? firstTab->url() : QUrl();
    QWebEnginePage *page = firstTab ? firstTab->page() : nullptr;
    QWebChannel *channel = page ? page->webChannel() : nullptr;
    const bool bareChannel = channel
        && !channel->registeredObjects().contains(
            QLatin1String("external"))
        && !channel->registeredObjects().contains(
            QLatin1String("aroraAutofill"))
        && !channel->registeredObjects().contains(
            QLatin1String("aroraPip"))
        && !channel->registeredObjects().contains(
            QLatin1String("aroraReader"));

    // A second fresh tab and a private tab take the same start page.
    window->tabWidget()->newTab();
    WebView *torTab = window->currentTab();
    const QUrl torTabUrl = torTab ? torTab->url() : QUrl();
    window->tabWidget()->newPrivateTab();
    WebView *privTab = window->currentTab();
    const QUrl privTabUrl = privTab ? privTab->url() : QUrl();

    // The qrc commit registers "arora" on the channel and the page's
    // own translate() run fills the header text — the DOM probe below
    // is the proof the whole handshake completed in tor mode.
    // runJavaScript runs in ApplicationWorld, so the page-global arora
    // var (MainWorld) is out of reach, but the DOM is shared:
    // headerTitle only fills once the channel's arora.translate()
    // callback has resolved.  The probe races the still-committing
    // load, so it re-issues until the post-load document answers.
    bool aroraRegistered = false;
    QString dom;
    if (firstTab && channel) {
        const QString probe = QStringLiteral(
            "(function(){var t=document.getElementById('torlogo');"
            "var s=document.getElementById('lineEdit');"
            "var h=document.getElementById('headerTitle');"
            "return (t&&t.querySelector('img')?'T':'-')+'|'"
            "+(s?'S':'-')+'|'"
            "+(h&&h.textContent.length?'H':'-');})()");
        QElapsedTimer timer;
        timer.start();
        while (dom != QLatin1String("T|S|H") && timer.elapsed() < 15000) {
            bool jsDone = false;
            firstTab->enginePage()->runJavaScript(probe,
                [&jsDone, &dom](const QVariant &v) {
                    dom = v.toString();
                    jsDone = true;
                });
            for (int waited = 0; !jsDone && waited < 5000; waited += 50)
                QTest::qWait(50);
        }
        aroraRegistered = channel->registeredObjects().contains(
            QLatin1String("arora"));
    }

    // Offscreen grab for visual review — set ARORA_TOR_GRAB_DIR to a
    // directory and the rendered tor start page lands there.
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty() && firstTab) {
        const QString dir = QString::fromLocal8Bit(grabDir);
        QDir().mkpath(dir);
        firstTab->resize(760, 700);
        firstTab->show();
        QApplication::processEvents();
        QTest::qWait(400);
        firstTab->grab().save(dir + QLatin1String("/tor-startpage.png"));
    }

    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QCOMPARE(mapped, torStart);
    QCOMPARE(guessed, torStart);
    QCOMPARE(firstUrl, torStart);
    QCOMPARE(torTabUrl, torStart);
    QCOMPARE(privTabUrl, torStart);
    QVERIFY(channel);
    QVERIFY(bareChannel);
    QVERIFY(aroraRegistered);
    QCOMPARE(dom, QStringLiteral("T|S|H"));
}

// TOR11: a native title bar cannot be tinted per-window (GNOME
// Wayland's Qt CSD only follows the session-wide portal color-scheme,
// _GTK_THEME_VARIANT is X11-only), so a tor window is frameless and
// carries a drawn TorTitleBar — violet chrome, a live window-title
// label, working min/max/close, drag-to-move and a resize ring —
// while a normal window keeps its native frame.
void tst_BrowserMainWindow::torTitleBar()
{
    // Frame-edge mapping: the margin ring hits one edge per side and
    // two at the corners, nothing in the middle.
    const QRect frame(0, 0, 800, 600);
    QCOMPARE(TorTitleBar::frameEdges(frame, QPoint(2, 300)),
             Qt::Edges(Qt::LeftEdge));
    QCOMPARE(TorTitleBar::frameEdges(frame, QPoint(400, 2)),
             Qt::Edges(Qt::TopEdge));
    QCOMPARE(TorTitleBar::frameEdges(frame, QPoint(797, 2)),
             Qt::Edges(Qt::RightEdge | Qt::TopEdge));
    QCOMPARE(TorTitleBar::frameEdges(frame, QPoint(797, 597)),
             Qt::Edges(Qt::RightEdge | Qt::BottomEdge));
    QCOMPARE(TorTitleBar::frameEdges(frame, QPoint(400, 300)),
             Qt::Edges());

    // Normal-window baseline: native frame, no menu widget, no bar.
    SubWindow *plain = new SubWindow;
    plain->resize(1000, 600);
    plain->show();
    QApplication::processEvents();
    const bool plainFrameless =
        plain->windowFlags().testFlag(Qt::FramelessWindowHint);
    const bool plainHasBar = plain->findChild<QWidget *>(
        QLatin1String("torTitleBar")) != nullptr;
    const bool plainMenuDirect = plain->menuBar()
        && plain->menuBar()->parentWidget()
               == static_cast<QWidget *>(plain);
    closeWindow(plain);
    QVERIFY(!plainFrameless);
    QVERIFY(!plainHasBar);
    QVERIFY(plainMenuDirect);

    // Collect state first — a mid-test QVERIFY abort must not leave
    // tor mode armed for the remaining functions.
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    const bool frameless =
        window->windowFlags().testFlag(Qt::FramelessWindowHint);
    QWidget *bar =
        window->findChild<QWidget *>(QLatin1String("torTitleBar"));
    QLabel *titleText =
        window->findChild<QLabel *>(QLatin1String("torTitleText"));
    QToolButton *minimize = window->findChild<QToolButton *>(
        QLatin1String("torTitleMinimize"));
    QToolButton *maximize = window->findChild<QToolButton *>(
        QLatin1String("torTitleMaximize"));
    QToolButton *close = window->findChild<QToolButton *>(
        QLatin1String("torTitleClose"));
    const bool barVisible = bar && bar->isVisible();
    const QString shownTitle = titleText ? titleText->text() : QString();
    const QString realTitle = window->windowTitle();
    const QMargins ring = window->contentsMargins();

    // The drawn title stays live on windowTitle changes.
    window->setWindowTitle(QLatin1String("Relay page - Arora (Tor)"));
    const QString syncedTitle =
        titleText ? titleText->text() : QString();

    // The frame ring and the bar both paint the tor violet.
    const auto isViolet = [](const QColor &c) {
        return c.isValid() && c.blue() > c.red()
            && c.red() > c.green() && c.lightness() < 160;
    };
    const QImage frameImage = window->grab().toImage();
    const QColor ringColor =
        frameImage.pixelColor(QPoint(2, window->height() - 2));
    const QColor barColor = bar
        ? frameImage.pixelColor(bar->mapTo(window,
                QPoint(bar->width() / 2, 2)))
        : QColor();

    // Maximize via the drawn button: state flips and the frame ring
    // collapses; a double-click on the bar restores both.
    bool maximized = false;
    QMargins maximizedRing;
    if (maximize) {
        maximize->click();
        QApplication::processEvents();
        maximized = window->isMaximized();
        maximizedRing = window->contentsMargins();
    }
    bool restored = false;
    QMargins restoredRing;
    if (bar && maximized) {
        const QPointF grip(bar->width() / 2.0, 3.0);
        QMouseEvent dblclick(QEvent::MouseButtonDblClick, grip,
                             bar->mapToGlobal(grip.toPoint()),
                             Qt::LeftButton, Qt::LeftButton,
                             Qt::NoModifier);
        QApplication::sendEvent(bar, &dblclick);
        QApplication::processEvents();
        restored = !window->isMaximized();
        restoredRing = window->contentsMargins();
    }

    // Offscreen grab for visual review — set ARORA_TOR_GRAB_DIR
    // (captured in the restored-normal state before the minimize leg).
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        window->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/tor-titlebar.png"));
    }

    // Offscreen startSystemMove is a no-op, so the manual fallback
    // drag path moves the window itself.  The events are sent to the
    // bar directly rather than through QTest — a leftover window from
    // an earlier test could otherwise sit over the point and swallow
    // the press (QTest resolves the receiver via widgetAt).
    bool dragged = false;
    if (bar && restored) {
        const QPoint before = window->pos();
        const QPoint grip(bar->width() / 2, 3);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(grip),
                          bar->mapToGlobal(grip), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &press);
        const QPoint target = grip + QPoint(23, 11);
        QMouseEvent move(QEvent::MouseMove, QPointF(target),
                         bar->mapToGlobal(target), Qt::NoButton,
                         Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &move);
        QApplication::processEvents();
        dragged = window->pos() != before;
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(target), bar->mapToGlobal(target),
                            Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QApplication::sendEvent(bar, &release);
    }

    // Minimize routes to the window state too.
    bool minimized = false;
    if (minimize) {
        minimize->click();
        QApplication::processEvents();
        minimized = window->isMinimized();
    }

    // The menus outlive the menubar — under MENU06 the tor row is a
    // pure title bar (no items) and the same menus ride the app-menu
    // dropdown instead.  The bar's pieces stay corner widgets on it.
    const bool menuAlive = window->appMenu()
        && !window->appMenu()->actions().isEmpty();
    const bool cornerHosted = bar && window->menuBar()
        && bar->parentWidget()
               == static_cast<QWidget *>(window->menuBar());

    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QVERIFY(frameless);
    QVERIFY(bar);
    QVERIFY(barVisible);
    QVERIFY(titleText);
    QVERIFY(minimize);
    QVERIFY(maximize);
    QVERIFY(close);
    QCOMPARE(shownTitle, realTitle);
    QCOMPARE(syncedTitle, QStringLiteral("Relay page - Arora (Tor)"));
    QVERIFY(ring.left() >= TorTitleBar::FrameMargin);
    QVERIFY(isViolet(ringColor));
    QVERIFY(isViolet(barColor));
    QVERIFY(maximized);
    QCOMPARE(maximizedRing, QMargins(0, 0, 0, 0));
    QVERIFY(restored);
    QVERIFY(restoredRing.left() >= TorTitleBar::FrameMargin);
    QVERIFY(dragged);
    QVERIFY(minimized);
    QVERIFY(menuAlive);
    QVERIFY(cornerHosted);
}

// TOR12: the drawn window title must be readable on the violet
// chrome — near-white ink, and it must keep the room its sizeHint
// earns from the menu items.  The old SqueezeLabel elided by
// rewriting text(), which shrank the sizeHint the menubar's
// corner-widget negotiation reads: one narrow pass collapsed the
// title into a permanent sliver.  The TorTitleLabel keeps the full
// title and elides at display time only, so a long title elides at
// the cap (never destroys the string) and a shorter one regrows.
void tst_BrowserMainWindow::torTitleBarTitle()
{
    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    QLabel *title =
        window->findChild<QLabel *>(QLatin1String("torTitleText"));
    QWidget *bar =
        window->findChild<QWidget *>(QLatin1String("torTitleBar"));
    QMenuBar *menuBar = window->menuBar();
    QVERIFY(title);
    QVERIFY(bar);
    QVERIFY(menuBar);

    const QString shortTitle =
        QLatin1String("Welcome to Arora! - Arora (Tor)");
    const QString longTitle = QLatin1String(
        "A very long page title that keeps going and going until it "
        "would never fit on any reasonable window - Arora (Tor)");

    // A normal-length title keeps its full text and the corner claims
    // its full hint — the menu items are what move right.
    window->setWindowTitle(shortTitle);
    QApplication::processEvents();
    QCOMPARE(title->text(), shortTitle);
    QCOMPARE(title->toolTip(), shortTitle);
    const int shortAdvance =
        title->fontMetrics().horizontalAdvance(shortTitle);
    QVERIFY(title->width() >= shortAdvance);
    QVERIFY(bar->width() > shortAdvance);

    // A title past the cap elides at the cap — the corner still gets
    // the capped room instead of collapsing to a sliver, the menus
    // still sit after it, and the full title survives on the tooltip.
    // MENU06: the tor title row carries no menu items by default —
    // the View-menu toggle brings them back, which is what this leg
    // measures against.
    window->showMenuBarAction()->trigger();
    QApplication::processEvents();
    window->setWindowTitle(longTitle);
    QApplication::processEvents();
    const int longCornerWidth = bar->width();
    const QRect fileRect =
        menuBar->actionGeometry(menuBar->actions().value(0));
    QVERIFY(title->text() != longTitle);
    QVERIFY(title->text().length() < longTitle.length());
    QCOMPARE(title->toolTip(), longTitle);
    QVERIFY2(title->width() >= 300,
             qPrintable(QString::number(title->width())));
    QVERIFY(title->width() <= 420);
    QVERIFY(longCornerWidth > 400);
    QVERIFY(fileRect.isValid());
    QVERIFY(fileRect.left() >= bar->geometry().right() - 1);

    // Shrinking back recovers the full text — the destroyed-string
    // squeeze could never regrow.
    window->setWindowTitle(shortTitle);
    QApplication::processEvents();
    QCOMPARE(title->text(), shortTitle);
    QVERIFY(bar->width() < longCornerWidth);

    // The ink is near-white on the violet chrome — a dark glyph would
    // be the unreadable report again.  Transparent background: every
    // opaque pixel in the grab is painted text.
    const auto inkColors = [](QLabel *label) {
        const QImage image = label->grab().toImage();
        int light = 0, dark = 0;
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alphaF() < 0.5)
                    continue;
                if (color.lightness() > 200)
                    ++light;
                else if (color.lightness() < 100)
                    ++dark;
            }
        }
        return qMakePair(light, dark);
    };
    const QPair<int, int> lightInk = inkColors(title);
    QVERIFY(lightInk.first > 20);
    QCOMPARE(lightInk.second, 0);

    // The chrome stays violet under a dark app palette, so the light
    // title must survive the flip — no palette-driven dark text.
    const QPalette savedPalette = QApplication::palette();
    QPalette darkPalette = savedPalette;
    darkPalette.setColor(QPalette::Window, QColor(0x2b, 0x2b, 0x2b));
    darkPalette.setColor(QPalette::Base, QColor(0x1e, 0x1e, 0x1e));
    darkPalette.setColor(QPalette::Text, Qt::white);
    darkPalette.setColor(QPalette::WindowText, Qt::white);
    QApplication::setPalette(darkPalette);
    QApplication::processEvents();
    const QPair<int, int> darkInk = inkColors(title);
    QApplication::setPalette(savedPalette);
    QApplication::processEvents();

    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QVERIFY(darkInk.first > 20);
    QCOMPARE(darkInk.second, 0);
}

// ENG13: the servo-branded bundled start page — same layout and
// default coloring as the standard page plus the Servo logo under the
// search field.  The pick keys on ENG05's MainWindow/defaultEngine
// setting (the engine a new tab would run); tor precedence holds —
// it is locked to Chromium.  On this branch there is no servo
// backend yet, so the key exercises the branding seam the merged
// engine registry drives for real.
void tst_BrowserMainWindow::servoStartPage()
{
    const QUrl standard(QLatin1String("qrc:/startpage.html"));
    const QUrl torStart(QLatin1String("qrc:/startpage-tor.html"));
    const QUrl servoStart(QLatin1String("qrc:/startpage-servo.html"));
    const QLatin1String engineKey("MainWindow/defaultEngine");

    // The engine-keyed pick: only "servo" re-brands; unknown ids keep
    // the standard page, and tor mode outranks every engine pick.
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("webengine")), standard);
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("servo")), servoStart);
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("bogus")), standard);
    QCOMPARE(BrowserApplication::defaultEngineId(),
             QStringLiteral("webengine"));
    QCOMPARE(BrowserApplication::startPageUrl(), standard);

    BrowserApplication::setTorMode(true);
    const QUrl torWins = BrowserApplication::startPageUrl(
        QLatin1String("servo"));
    BrowserApplication::setTorMode(false);
    QCOMPARE(torWins, torStart);

    // Collect state first — a mid-test QVERIFY abort must not leave
    // the servo engine pick armed for the remaining functions.
    QSettings().setValue(engineKey, QStringLiteral("servo"));
    const QUrl mapped = BrowserApplication::startPageUrl();
    const QUrl guessed =
        TabWidget::guessUrlFromString(QLatin1String("about:home"));

    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();

    // The window's first tab is itself newTab() — with the servo
    // engine pick it lands on the servo start page at once.
    WebView *firstTab = window->currentTab();
    const QUrl firstUrl = firstTab ? firstTab->url() : QUrl();
    QWebEnginePage *page = firstTab ? firstTab->page() : nullptr;
    QWebChannel *channel = page ? page->webChannel() : nullptr;

    // A second fresh tab takes the same branded start page.
    window->tabWidget()->newTab();
    WebView *servoTab = window->currentTab();
    const QUrl servoTabUrl = servoTab ? servoTab->url() : QUrl();

    // DOM probe: the logo div sits directly under the search field,
    // its stylesheet resolves the bundled logo asset, the search box
    // exists, and headerTitle only fills once the channel's
    // arora.translate() callback has resolved — the same handshake
    // proof torStartPage uses.
    QString dom;
    if (firstTab) {
        const QString probe = QStringLiteral(
            "(function(){var l=document.getElementById('servologo');"
            "var s=document.getElementById('search');"
            "var e=document.getElementById('lineEdit');"
            "var h=document.getElementById('headerTitle');"
            "var v=l&&s&&s.nextElementSibling===l&&"
            "getComputedStyle(l).backgroundImage.indexOf("
            "'servo-logo')>=0?"
            "'V':'-';"
            "return v+'|'+(e?'S':'-')+'|'"
            "+(h&&h.textContent.length?'H':'-');})()");
        QElapsedTimer timer;
        timer.start();
        while (dom != QLatin1String("V|S|H") && timer.elapsed() < 15000) {
            bool jsDone = false;
            firstTab->enginePage()->runJavaScript(probe,
                [&jsDone, &dom](const QVariant &v) {
                    dom = v.toString();
                    jsDone = true;
                });
            for (int waited = 0; !jsDone && waited < 5000; waited += 50)
                QTest::qWait(50);
        }
    }

    // Offscreen grab for visual review — set ARORA_SERVO_GRAB_DIR to
    // a directory and the rendered servo start page lands there.
    const QByteArray grabDir = qgetenv("ARORA_SERVO_GRAB_DIR");
    if (!grabDir.isEmpty() && firstTab) {
        const QString dir = QString::fromLocal8Bit(grabDir);
        QDir().mkpath(dir);
        firstTab->resize(760, 700);
        firstTab->show();
        QApplication::processEvents();
        QTest::qWait(400);
        firstTab->grab().save(dir + QLatin1String("/servo-startpage.png"));
    }

    closeWindow(window);
    QSettings().remove(engineKey);

    QCOMPARE(mapped, servoStart);
    QCOMPARE(guessed, servoStart);
    QCOMPARE(firstUrl, servoStart);
    QCOMPARE(servoTabUrl, servoStart);
    QVERIFY(channel);
    QCOMPARE(dom, QStringLiteral("V|S|H"));
}

// PTAB02: private tabs get their own branded start page — a deep
// navy variant of the tor page carrying the Arora logo, the search
// box and an original sunglasses glyph.  The pick is context-keyed:
// precedence tor > private > servo > standard, so a private tab
// shows something branded even on the servo engine pick (which
// cannot render qrc yet — ENG18), while a tor context never leaves
// the tor brand.
void tst_BrowserMainWindow::privateStartPage()
{
    const QUrl standard(QLatin1String("qrc:/startpage.html"));
    const QUrl torStart(QLatin1String("qrc:/startpage-tor.html"));
    const QUrl servoStart(QLatin1String("qrc:/startpage-servo.html"));
    const QUrl privateStart(QLatin1String("qrc:/startpage-private.html"));

    // The context-keyed pick: private outranks every engine id
    // (including servo — documented precedence), non-private keeps
    // the engine pick, and tor mode outranks private itself.
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("webengine"), true), privateStart);
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("servo"), true), privateStart);
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("bogus"), true), privateStart);
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("webengine"), false), standard);
    QCOMPARE(BrowserApplication::startPageUrl(
                 QLatin1String("servo"), false), servoStart);
    QCOMPARE(BrowserApplication::startPageUrl(), standard);

    BrowserApplication::setTorMode(true);
    const QUrl torWins = BrowserApplication::startPageUrl(
        QLatin1String("webengine"), true);
    BrowserApplication::setTorMode(false);
    QCOMPARE(torWins, torStart);

    // about:home is page-context aware: a private tab's typed
    // about:home resolves to the private variant, a normal tab's to
    // the standard one.
    QCOMPARE(TabWidget::guessUrlFromString(QLatin1String("about:home")),
             standard);
    QCOMPARE(TabWidget::guessUrlFromString(QLatin1String("about:home"),
                                           true),
             privateStart);

    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();

    // The first (non-private) tab stays blank in normal mode.
    WebView *firstTab = window->currentTab();
    const QUrl firstUrl = firstTab ? firstTab->url() : QUrl();

    // An explicit private tab lands on the private start page at
    // once.
    window->tabWidget()->newPrivateTab();
    WebView *privTab = window->currentTab();
    const QUrl privTabUrl = privTab ? privTab->url() : QUrl();
    const bool privIsOtr = privTab && privTab->enginePage()
        && privTab->enginePage()->isOffTheRecord();
    QWebEnginePage *privPage = privTab ? privTab->page() : nullptr;
    QWebChannel *privChannel =
        privPage ? privPage->webChannel() : nullptr;

    // A plain newTab spawned while the private tab is current
    // inherits the off-the-record context and gets the private page
    // too — PTAB01's child-tab inheritance feeding the pick.
    window->tabWidget()->newTab();
    WebView *childTab = window->currentTab();
    const QUrl childTabUrl = childTab ? childTab->url() : QUrl();

    // DOM probe on the private tab: the sunglasses mark sits under
    // the search field, the search box exists, and headerTitle only
    // fills once the channel's arora.translate() callback has
    // resolved — the same handshake proof torStartPage uses.
    QString dom;
    if (privTab) {
        const QString probe = QStringLiteral(
            "(function(){var l=document.getElementById('privatelogo');"
            "var s=document.getElementById('lineEdit');"
            "var h=document.getElementById('headerTitle');"
            "return (l&&l.querySelector('img')?'P':'-')+'|'"
            "+(s?'S':'-')+'|'"
            "+(h&&h.textContent.length?'H':'-');})()");
        QElapsedTimer timer;
        timer.start();
        while (dom != QLatin1String("P|S|H") && timer.elapsed() < 15000) {
            bool jsDone = false;
            privTab->enginePage()->runJavaScript(probe,
                [&jsDone, &dom](const QVariant &v) {
                    dom = v.toString();
                    jsDone = true;
                });
            for (int waited = 0; !jsDone && waited < 5000; waited += 50)
                QTest::qWait(50);
        }
    }
    const bool aroraRegistered = privChannel
        && privChannel->registeredObjects().contains(
            QLatin1String("arora"));

    // Offscreen grab for visual review — set ARORA_PRIVATE_GRAB_DIR
    // to a directory and the rendered private start page lands
    // there.
    const QByteArray grabDir = qgetenv("ARORA_PRIVATE_GRAB_DIR");
    if (!grabDir.isEmpty() && privTab) {
        window->tabWidget()->setCurrentWidget(privTab);
        const QString dir = QString::fromLocal8Bit(grabDir);
        QDir().mkpath(dir);
        privTab->resize(760, 700);
        privTab->show();
        QApplication::processEvents();
        QTest::qWait(400);
        privTab->grab().save(
            dir + QLatin1String("/private-startpage.png"));
        // QWidget::grab shows no WebEngine content offscreen —
        // printToPdf is the rendered-page capture under this
        // platform (same convention as ENG13's pdfgrab review).
        if (privPage) {
            QByteArray pdf;
            privPage->printToPdf(
                [&pdf](const QByteArray &data) { pdf = data; });
            for (int waited = 0; pdf.isEmpty() && waited < 5000;
                 waited += 50)
                QTest::qWait(50);
            QFile pdfFile(dir + QLatin1String("/private-startpage.pdf"));
            if (pdfFile.open(QIODevice::WriteOnly))
                pdfFile.write(pdf);
        }
    }

    closeWindow(window);

    QVERIFY(firstUrl.isEmpty()
            || firstUrl == QUrl(QLatin1String("about:blank")));
    QVERIFY(privIsOtr);
    QCOMPARE(privTabUrl, privateStart);
    QCOMPARE(childTabUrl, privateStart);
    QVERIFY(privChannel);
    QVERIFY(aroraRegistered);
    QCOMPARE(dom, QStringLiteral("P|S|H"));
}

// READ02: the navigation toolbar carries a reader-mode toggle between
// stop/reload and the location bar — hidden until the page probes
// article-like, checked while the overlay is up, and always agreeing
// with the View-menu action.
void tst_BrowserMainWindow::readerModeButton()
{
    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    QToolButton *reader = navBar->findChild<QToolButton *>(
        QLatin1String("navReaderButton"));
    QVERIFY(reader);
    QVERIFY(reader->autoRaise());
    QVERIFY(reader->isCheckable());
    // about:blank is not article-like — nothing to read.
    QVERIFY(reader->isHidden());
    QVERIFY(!reader->isChecked());
    QVERIFY(reader->toolTip().contains(QLatin1String("Ctrl+Alt+R")));

    // Slot order in the toolbar's action list: the MENU06 app-menu
    // button leads, then Back, Forward, Stop/Reload, the reader
    // button, then the location-bar splitter.
    const QList<QAction *> barActions = navBar->actions();
    int readerIndex = -1;
    int splitterIndex = -1;
    for (int i = 0; i < barActions.size(); ++i) {
        QWidget *widget = navBar->widgetForAction(barActions.at(i));
        if (widget == reader)
            readerIndex = i;
        else if (widget && qobject_cast<QSplitter *>(widget))
            splitterIndex = i;
    }
    QCOMPARE(readerIndex, 4);
    QCOMPARE(splitterIndex, readerIndex + 1);

    // The View-menu action shares the same ReaderMode state.
    QAction *viewAction = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (!menuAction->menu()
            || !menuAction->text().contains(QLatin1String("View")))
            continue;
        for (QAction *action : menuAction->menu()->actions()) {
            if (action->text().remove(QLatin1Char('&'))
                    == QLatin1String("Reader Mode"))
                viewAction = action;
        }
    }
    QVERIFY(viewAction);
    QCOMPARE(reader->isChecked(), viewAction->isChecked());

    // A page the Readability heuristic calls readerable flips
    // availability -> the button appears; click enters reader mode.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const QByteArray paragraph =
        QByteArray("The quick brown fox jumps over the lazy dog. ")
        .repeated(30);
    const QByteArray body = QByteArray(
        "<html><head><title>Article</title></head><body><article>"
        "<h1>Heading</h1><p>") + paragraph + "</p><p>" + paragraph
        + "</p><p>" + paragraph + "</p></article></body></html>";
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server, &body]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        socket->readAll();
        socket->write(QByteArray("HTTP/1.1 200 OK\r\n"
                                 "Content-Type: text/html\r\n"
                                 "Content-Length: ")
                      + QByteArray::number(body.size())
                      + "\r\n\r\n" + body);
        socket->disconnectFromHost();
    });
    window->currentTab()->loadUrl(
        QUrl(QStringLiteral("http://127.0.0.1:%1/article")
                 .arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(!reader->isHidden(), 20000);

    reader->click();
    QTRY_VERIFY_WITH_TIMEOUT(reader->isChecked(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(viewAction->isChecked(), 5000);

    // The menu action exits again — the button follows.
    viewAction->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!viewAction->isChecked(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!reader->isChecked(), 5000);

    // Tab switches re-point the button at the current view: the fresh
    // tab is no article, switching back shows the toggle again.
    window->tabWidget()->newTab();
    QTRY_VERIFY_WITH_TIMEOUT(reader->isHidden(), 10000);
    window->tabWidget()->setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(!reader->isHidden(), 10000);

    closeWindow(window);
}

// UIP04: permanent status-bar widgets — a load-time indicator and a
// -[100%]+ zoom control, both bound to the current tab and re-pointed
// on tab switches.
void tst_BrowserMainWindow::statusBarWidgets()
{
    // Deterministic widget pass — display state without engine timing.
    {
        LoadingIndicator indicator;
        QLabel *label = indicator.findChild<QLabel *>(
            QLatin1String("loadingLabel"));
        QProgressBar *bar = indicator.findChild<QProgressBar *>(
            QLatin1String("loadingBar"));
        QVERIFY(label && bar);
        QVERIFY(!indicator.isVisible());
        QVERIFY(QMetaObject::invokeMethod(&indicator, "pageLoadStarted"));
        QVERIFY(indicator.loading());
        QVERIFY(indicator.isVisible());
        QVERIFY(QMetaObject::invokeMethod(&indicator, "pageLoadProgress",
                                          Q_ARG(int, 42)));
        QCOMPARE(bar->value(), 42);
        QVERIFY(label->text().endsWith(QLatin1String(" s")));
        QTest::qWait(50);
        QVERIFY(QMetaObject::invokeMethod(&indicator, "pageLoadFinished",
                                          Q_ARG(bool, true)));
        QVERIFY(!indicator.loading());
        QVERIFY(!bar->isVisible());
        QVERIFY(label->text().startsWith(QLatin1String("Loaded in")));
        QVERIFY(QMetaObject::invokeMethod(&indicator, "clearStatus"));
        QVERIFY(!indicator.isVisible());
    }

    SubWindow *window = new SubWindow;
    window->show();

    LoadingIndicator *indicator = window->findChild<LoadingIndicator *>(
        QLatin1String("loadingIndicator"));
    ZoomControl *zoom = window->findChild<ZoomControl *>(
        QLatin1String("zoomControl"));
    QVERIFY(indicator);
    QVERIFY(zoom);
    // Permanent status-bar citizens — they die with the bar, not the
    // temporary-message area.
    QCOMPARE(indicator->parentWidget(),
             static_cast<QWidget *>(window->statusBar()));
    QCOMPARE(zoom->parentWidget(),
             static_cast<QWidget *>(window->statusBar()));
    QCOMPARE(zoom->webView(), window->currentTab());

    QToolButton *zoomOutButton = zoom->findChild<QToolButton *>(
        QLatin1String("zoomOutButton"));
    QToolButton *valueButton = zoom->findChild<QToolButton *>(
        QLatin1String("zoomValueButton"));
    QToolButton *zoomInButton = zoom->findChild<QToolButton *>(
        QLatin1String("zoomInButton"));
    QVERIFY(zoomOutButton && valueButton && zoomInButton);
    QCOMPARE(valueButton->text(), QLatin1String("100%"));

    // The buttons drive the page zoom through the WebView ladder…
    zoomInButton->click();
    QCOMPARE(window->currentTab()->currentZoom(), 110);
    QCOMPARE(valueButton->text(), QLatin1String("110%"));

    // …and the display follows the menu/keyboard path too (both
    // funnel through WebView::applyZoom -> zoomChanged).
    QVERIFY(QMetaObject::invokeMethod(window, "zoomIn"));
    QCOMPARE(valueButton->text(), QLatin1String("120%"));
    zoomOutButton->click();
    QCOMPARE(valueButton->text(), QLatin1String("110%"));

    // Percent label is the reset affordance.
    valueButton->click();
    QCOMPARE(valueButton->text(), QLatin1String("100%"));
    QCOMPARE(window->currentTab()->zoomFactor(), qreal(1.0));

    // Zoom is per-tab: a new tab reads 100%, switching back shows the
    // first tab's level again.
    WebView *firstTab = window->currentTab();
    firstTab->zoomIn();
    QCOMPARE(firstTab->currentZoom(), 110);
    window->tabWidget()->newTab();
    QVERIFY(window->currentTab() != firstTab);
    QCOMPARE(zoom->webView(), window->currentTab());
    QCOMPARE(valueButton->text(), QLatin1String("100%"));
    window->tabWidget()->setCurrentIndex(0);
    QCOMPARE(zoom->webView(), firstTab);
    QCOMPARE(valueButton->text(), QLatin1String("110%"));

    // Load indicator follows a real page load end-to-end.  A local
    // responder holds the body back for a beat — a data:/file: load
    // can finish between QTRY polls, so the loading state would not
    // be reliably observable.
    QLabel *loadingLabel = indicator->findChild<QLabel *>(
        QLatin1String("loadingLabel"));
    QProgressBar *loadingBar = indicator->findChild<QProgressBar *>(
        QLatin1String("loadingBar"));
    QVERIFY(loadingLabel && loadingBar);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        socket->readAll();
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/html\r\n"
                      "Content-Length: 5\r\n\r\n");
        QTimer::singleShot(800, socket, [socket]() {
            socket->write("hello");
            socket->disconnectFromHost();
        });
    });
    window->currentTab()->loadUrl(
        QUrl(QStringLiteral("http://127.0.0.1:%1/slow")
                 .arg(server.serverPort())));
    QTRY_VERIFY_WITH_TIMEOUT(indicator->loading(), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(!indicator->loading()
                             && loadingLabel->text().startsWith(
                                 QLatin1String("Loaded in")), 15000);
    QVERIFY(indicator->lastElapsedMs() > 0);

    // The finished notice clears itself; idle stays hidden.
    QTRY_VERIFY_WITH_TIMEOUT(!indicator->isVisible(), 15000);

    closeWindow(window);
}

// SBAR01: per-tab renderer memory + live bandwidth — the /proc
// helpers are deterministic, the widgets' bind/auto-hide behavior is
// driven through their slots like statusBarWidgets() does.
void tst_BrowserMainWindow::statusBarIndicators()
{
    // VmRSS reader: bogus/dead pids report -1, a live one reads > 0.
    QCOMPARE(MemIndicator::residentMemoryKb(-1), qint64(-1));
    QCOMPARE(MemIndicator::residentMemoryKb(0), qint64(-1));
    QCOMPARE(MemIndicator::residentMemoryKb(99999999), qint64(-1));
    QVERIFY(MemIndicator::residentMemoryKb(
                QCoreApplication::applicationPid()) > 0);

    // Display ladder: one decimal under 100MB, integer from there,
    // GB past a gigabyte.
    QCOMPARE(MemIndicator::formatRss(87 * 1024),
             QLatin1String("87.0 MB"));
    QCOMPARE(MemIndicator::formatRss(238 * 1024),
             QLatin1String("238 MB"));
    QCOMPARE(MemIndicator::formatRss(1536 * 1024),
             QLatin1String("1.5 GB"));

    // Rate formatter shares the DownloadManager unit ladder + "/s".
    QCOMPARE(NetIndicator::formatRate(84 * 1024),
             QLatin1String("84.0 kB/s"));
    QCOMPARE(NetIndicator::formatRate(1200 * 1024),
             QLatin1String("1.2 MB/s"));

    // The standalone io scan is a soft check — a test run may have
    // engine children (earlier cases spawn renderers) or none.
    qint64 ioRead = -1, ioWrite = -1;
    if (NetIndicator::engineIoTotals(&ioRead, &ioWrite)) {
        QVERIFY(ioRead >= 0);
        QVERIFY(ioWrite >= 0);
    }

    {
        MemIndicator mem;
        QLabel *label = mem.findChild<QLabel *>(
            QLatin1String("memLabel"));
        QVERIFY(label);
        // No bound view -> "MEM —", and refresh keeps it there.
        QVERIFY(label->text().startsWith(QLatin1String("MEM")));
        QVERIFY(QMetaObject::invokeMethod(&mem, "refresh"));
        QVERIFY(label->text().contains(
            QString::fromUtf8("\xe2\x80\x94")));

        NetIndicator net;
        QVERIFY(!net.isVisible());
        // First sample only builds the baseline — no burst, stays
        // hidden.
        QVERIFY(QMetaObject::invokeMethod(&net, "sample"));
        QVERIFY(!net.isVisible());
        net.setVisible(true);
        QVERIFY(QMetaObject::invokeMethod(&net, "hideWhenIdle"));
        QVERIFY(!net.isVisible());
    }

    SubWindow *window = new SubWindow;
    window->show();

    MemIndicator *mem = window->findChild<MemIndicator *>(
        QLatin1String("memIndicator"));
    NetIndicator *net = window->findChild<NetIndicator *>(
        QLatin1String("netIndicator"));
    QVERIFY(mem && net);
    QCOMPARE(mem->parentWidget(),
             static_cast<QWidget *>(window->statusBar()));
    QCOMPARE(net->parentWidget(),
             static_cast<QWidget *>(window->statusBar()));
    QCOMPARE(mem->webView(), window->currentTab());

    // The memory indicator follows tab switches like the zoom
    // control does.
    WebView *firstTab = window->currentTab();
    window->tabWidget()->newTab();
    QVERIFY(window->currentTab() != firstTab);
    QCOMPARE(mem->webView(), window->currentTab());

    closeWindow(window);
}

// SIDE01: the optional sidebar dock — off by default, lazily built on
// first show, persisted through MainWindow/showSidebar +
// sidebarDockArea, all four sections functional.
void tst_BrowserMainWindow::sidebarPanel()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("sidebar/notes"));
    settings.remove(QLatin1String("sidebar/currentTab"));
    settings.remove(QLatin1String("sidebar/panels"));

    SubWindow *window = new SubWindow;
    window->show();
    QDockWidget *dock = window->sidebarDock();
    QVERIFY(dock);
    // Default profile starts with the sidebar off — and the panel is
    // not even built, so a hidden sidebar costs nothing.
    QVERIFY(!dock->isVisible());
    QVERIFY(!window->sidebarPanel());

    // Menu action = the dock's toggle action (View > Sidebar, F4).
    QAction *toggle = dock->toggleViewAction();
    QVERIFY(toggle->isCheckable());
    toggle->trigger();
    QVERIFY(dock->isVisible());
    QVERIFY(toggle->isChecked());
    // First show builds the panel: four sections over the shared
    // bookmarks/history/downloads models plus the notes editor.
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QVERIFY(panel->tabs());
    QCOMPARE(panel->tabs()->count(), 4);
    QVERIFY(panel->findChild<QWidget *>(
                QLatin1String("sidebarBookmarksView")));
    QVERIFY(panel->findChild<QWidget *>(
                QLatin1String("sidebarHistoryView")));
    QVERIFY(panel->findChild<QWidget *>(
                QLatin1String("sidebarDownloadsView")));
    QVERIFY(panel->notes());

    // SIDE02: the switcher is a vertical icon rail — entries carry
    // their title as text (a11y) + tooltip plus a themed glyph, but
    // the rail only paints the icon.
    QCOMPARE(panel->tabs()->tabPosition(), QTabWidget::West);
    for (int i = 0; i < panel->tabs()->count(); ++i) {
        QVERIFY(!panel->tabs()->tabIcon(i).isNull());
        QVERIFY(!panel->tabs()->tabText(i).isEmpty());
        QCOMPARE(panel->tabs()->tabToolTip(i),
                 panel->tabs()->tabText(i));
    }

    // SIDE06: name-only trees — the Address column used to soak the
    // slack (stretchLastSection) while titles elided at a fixed
    // ~100px.  Column 0 now stretches across the panel and column 1
    // is hidden (the URL still rides the tooltip).
    QTreeView *bookmarksView = panel->findChild<QTreeView *>(
        QLatin1String("sidebarBookmarksView"));
    QTreeView *historyView = panel->findChild<QTreeView *>(
        QLatin1String("sidebarHistoryView"));
    QVERIFY(bookmarksView);
    QVERIFY(historyView);
    for (QTreeView *view : { bookmarksView, historyView }) {
        QVERIFY(view->isColumnHidden(1));
        QCOMPARE(view->header()->sectionResizeMode(0),
                 QHeaderView::Stretch);
    }
    // The stretched column tracks the viewport — names only elide at
    // the true panel edge.
    QTRY_VERIFY(bookmarksView->header()->sectionSize(0)
                >= bookmarksView->viewport()->width() - 4);
    QTRY_VERIFY(historyView->header()->sectionSize(0)
                >= historyView->viewport()->width() - 4);

    // The context menu's "Edit Address" unhides the column to host
    // the editor; the delegate's closeEditor hook folds it back.
    panel->showBookmarks();
    BookmarksManager *bookmarksManager = BookmarksManager::instance();
    BookmarkNode *entry = new BookmarkNode(BookmarkNode::Bookmark);
    entry->title = QLatin1String("Sidebar Elide Check");
    entry->url = QLatin1String("https://example.com/sidebar-elide");
    bookmarksManager->addBookmark(bookmarksManager->menu(), entry);
    QAbstractItemModel *bmModel = bookmarksView->model();
    QModelIndex menuIndex;
    for (int row = 0; row < bmModel->rowCount(); ++row) {
        const QModelIndex candidate = bmModel->index(row, 0);
        if (candidate.data(Qt::DisplayRole).toString()
            == bookmarksManager->menu()->title) {
            menuIndex = candidate;
            break;
        }
    }
    QVERIFY(menuIndex.isValid());
    bookmarksView->setExpanded(menuIndex, true);
    const int entryRow = bmModel->rowCount(menuIndex) - 1;
    const QModelIndex urlIndex = bmModel->index(entryRow, 1, menuIndex);
    QVERIFY(urlIndex.isValid());
    QCOMPARE(urlIndex.data(Qt::DisplayRole).toString(),
             QLatin1String("https://example.com/sidebar-elide"));
    bookmarksView->setColumnHidden(1, false);
    bookmarksView->edit(urlIndex);
    QApplication::processEvents();
    QVERIFY(bookmarksView->viewport()->findChild<QLineEdit *>());
    QTest::keyClick(bookmarksView->viewport()
                        ->findChild<QLineEdit *>(), Qt::Key_Escape);
    QTRY_VERIFY(bookmarksView->isColumnHidden(1));
    bmModel->removeRow(entryRow, menuIndex);

    // Unchecking a section in settings drops its tab on apply; the
    // rail keeps registry order when it comes back.
    SidebarPanel::setPanelVisible("history", false);
    window->applySidebarSettings();
    QCOMPARE(panel->tabs()->count(), 3);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!panel->findChild<QWidget *>(
                QLatin1String("sidebarHistoryView")));
    SidebarPanel::setPanelVisible("history", true);
    window->applySidebarSettings();
    QCOMPARE(panel->tabs()->count(), 4);
    QCOMPARE(panel->tabs()->tabText(1), QLatin1String("History"));

    // Offscreen panel grab for visual review — set
    // ARORA_SIDEBAR_GRAB_DIR to a directory and the panel lands there
    // as sidebar-panel.png (same convention as ARORA_SETTINGS_GRAB_DIR).
    const QByteArray sidebarGrabDir = qgetenv("ARORA_SIDEBAR_GRAB_DIR");
    if (!sidebarGrabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(sidebarGrabDir));
        qApp->processEvents();
        panel->grab().save(QString::fromLocal8Bit(sidebarGrabDir)
            + QStringLiteral("/sidebar-panel.png"));
    }

    // Toggling writes the persisted key; hiding is remembered.
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar"))
                 .toBool(), true);
    toggle->trigger();
    QVERIFY(!dock->isVisible());
    QVERIFY(!toggle->isChecked());
    QCOMPARE(settings.value(QLatin1String("MainWindow/showSidebar"))
                 .toBool(), false);

    // Settings-driven show + right-side docking via applySidebarSettings.
    settings.setValue(QLatin1String("MainWindow/showSidebar"), true);
    settings.setValue(QLatin1String("MainWindow/sidebarDockArea"),
                      int(Qt::RightDockWidgetArea));
    window->applySidebarSettings();
    QVERIFY(dock->isVisible());
    QCOMPARE(window->dockWidgetArea(dock), Qt::RightDockWidgetArea);

    // SIDE06: the rail hugs the outer edge — it flips East under a
    // right dock and follows live moves back and forth.
    QCOMPARE(panel->tabs()->tabPosition(), QTabWidget::East);
    window->addDockWidget(Qt::LeftDockWidgetArea, dock);
    QCOMPARE(panel->tabs()->tabPosition(), QTabWidget::West);
    window->addDockWidget(Qt::RightDockWidgetArea, dock);
    QCOMPARE(panel->tabs()->tabPosition(), QTabWidget::East);

    // Notes persist through the panel's autosave slot.
    panel->notes()->setPlainText(QLatin1String("remember me"));
    QVERIFY(QMetaObject::invokeMethod(panel, "save"));
    QCOMPARE(settings.value(QLatin1String("sidebar/notes")).toString(),
             QLatin1String("remember me"));

    // A second window honors the persisted show state + dock side.
    SubWindow *window2 = new SubWindow;
    window2->show();
    QVERIFY(window2->sidebarDock()->isVisible());
    QCOMPARE(window2->dockWidgetArea(window2->sidebarDock()),
             Qt::RightDockWidgetArea);
    QVERIFY(window2->sidebarPanel());
    QCOMPARE(window2->sidebarPanel()->notes()->toPlainText(),
             QLatin1String("remember me"));
    closeWindow(window2);

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("sidebar/notes"));
    settings.remove(QLatin1String("sidebar/currentTab"));
    settings.remove(QLatin1String("sidebar/panels"));
    closeWindow(window);
}

// RSZ02: the dock's inner edge hosts a real resize grip — a visible,
// hover-highlighted strip whose drag resizes the dock live through
// QMainWindow::resizeDocks, persists MainWindow/sidebarWidth, mirrors
// its edge + direction on right docks, resets on double-click, and
// hides while the dock floats.
void tst_BrowserMainWindow::sidebarGripResize()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("MainWindow/sidebarWidth"));

    SubWindow *window = new SubWindow;
    window->resize(900, 600);
    window->show();
    QDockWidget *dock = window->sidebarDock();
    dock->toggleViewAction()->trigger();
    QVERIFY(dock->isVisible());
    SidebarGrip *grip = dock->findChild<SidebarGrip *>();
    QVERIFY(grip);
    QVERIFY(grip->isVisible());
    QCOMPARE(grip->cursor().shape(), Qt::SizeHorCursor);
    // Left-docked: the grip trails the panel inside the dock.
    QCOMPARE(grip->parentWidget()->layout()->indexOf(grip), 1);
    // The default width lands on the first show.
    QTRY_VERIFY(dock->width() >= 250);
    const int before = dock->width();

    // Dragging the grip right (into the content) widens the dock and
    // persists the requested width.
    QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier,
                      QPoint(5, 50));
    QTest::mouseMove(grip, QPoint(65, 50));
    QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier,
                        QPoint(65, 50));
    QTRY_VERIFY(dock->width() > before + 30);
    QVERIFY(settings.value(QLatin1String("MainWindow/sidebarWidth"))
                .toInt() > before + 30);

    // Double-click snaps back to the default width.
    QTest::mouseDClick(grip, Qt::LeftButton, Qt::NoModifier,
                       QPoint(5, 50));
    QTRY_VERIFY(qAbs(dock->width() - SidebarGrip::defaultWidth()) <= 4);
    QVERIFY(qAbs(settings
                 .value(QLatin1String("MainWindow/sidebarWidth")).toInt()
                 - SidebarGrip::defaultWidth()) <= 4);

    // Right-docked: the grip re-anchors before the panel and the
    // drag direction flips — pulling left widens.
    window->addDockWidget(Qt::RightDockWidgetArea, dock);
    QCOMPARE(grip->parentWidget()->layout()->indexOf(grip), 0);
    const int rightBefore = dock->width();
    QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier,
                      QPoint(5, 50));
    QTest::mouseMove(grip, QPoint(-55, 50));
    QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier,
                        QPoint(-55, 50));
    QTRY_VERIFY(dock->width() > rightBefore + 30);

    // A floating dock has no window edge — the grip hides.
    dock->setFloating(true);
    QVERIFY(!grip->isVisible());
    dock->setFloating(false);

    settings.remove(QLatin1String("MainWindow/sidebarWidth"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    closeWindow(window);
}

// DOWN02: the sidebar's Downloads page is the only download surface —
// Ctrl+Y / Tools > Downloads focuses it, the filter narrows rows, the
// sort combo reorders, a selected row hosts the item's expanded detail
// card, and the header X folds the dock away.
void tst_BrowserMainWindow::downloadsSidebarPanel()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));

    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    DownloadManager::instance()->setDownloadDirectory(
        downloadDir.path() + QLatin1Char('/'));

    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(QMetaObject::invokeMethod(window, "downloadManager"));

    QDockWidget *dock = window->sidebarDock();
    QVERIFY(dock->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QCOMPARE(panel->tabs()->currentWidget(), panel->downloadsPage());
    // The manager is a hidden controller — never a window.
    QVERIFY(!DownloadManager::instance()->isVisible());

    QListView *view = panel->findChild<QListView *>(
        QLatin1String("sidebarDownloadsView"));
    QVERIFY(view);
    QAbstractItemModel *model = view->model();
    QVERIFY(model);

    // Seed a real download so there is a row to filter and select.
    const QUrl url(QString::fromLatin1("data:text/plain;base64,")
        + QString::fromLatin1(QByteArray("hello world").toBase64()));
    Engine::Page *page = DownloadManager::instance()->retryPage(false);
    QVERIFY(page);
    DownloadManager::instance()->download(page, url);
    QTRY_COMPARE(model->rowCount(), 1);
    QVERIFY(DownloadManager::instance()->itemAt(0));
    QVERIFY(!model->index(0, 0).data(Qt::DisplayRole)
                 .toString().isEmpty());

    // Search filters on file name / source url; clearing restores.
    QLineEdit *search = panel->findChild<QLineEdit *>(
        QLatin1String("sidebarDownloadsSearch"));
    QVERIFY(search);
    search->setText(QLatin1String("zzz-no-match"));
    QTRY_COMPARE(model->rowCount(), 0);
    search->clear();
    QTRY_COMPARE(model->rowCount(), 1);

    // The sort combo drives the proxy's sort role (Date default).
    QComboBox *sort = panel->findChild<QComboBox *>(
        QLatin1String("sidebarDownloadsSort"));
    QVERIFY(sort);
    QCOMPARE(sort->count(), 3);
    QSortFilterProxyModel *proxy = qobject_cast<QSortFilterProxyModel *>(model);
    QVERIFY(proxy);
    QCOMPARE(proxy->sortRole(), int(DownloadModel::StartedTimeRole));
    for (int i = 0; i < sort->count(); ++i) {
        sort->setCurrentIndex(i);
        emit sort->activated(i);
        QCOMPARE(proxy->sortRole(), sort->itemData(i).toInt());
    }

    // Selecting a row hosts the item's expanded detail card in the
    // bottom pane.
    view->setCurrentIndex(model->index(0, 0));
    QFrame *detail = panel->findChild<QFrame *>(
        QLatin1String("sidebarDownloadDetail"));
    QVERIFY(detail);
    QVERIFY(detail->isVisibleTo(detail->parentWidget()));
    DownloadItem *card = detail->findChild<DownloadItem *>();
    QVERIFY(card);
    QVERIFY(card->isExpanded());
    QCOMPARE(card, DownloadManager::instance()->itemAt(
                proxy->mapToSource(view->currentIndex()).row()));

    // Clearing the selection folds the pane and parks the card back on
    // the hidden manager.
    view->setCurrentIndex(QModelIndex());
    QVERIFY(!detail->isVisibleTo(detail->parentWidget()));
    QCOMPARE(card->parentWidget(),
             static_cast<QWidget *>(DownloadManager::instance()));

    // The header's X folds the whole dock.
    QToolButton *closeButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarCloseButton"));
    QVERIFY(closeButton);
    closeButton->click();
    QVERIFY(!dock->isVisible());

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    closeWindow(window);

    // The seeded download left a retry page on the singleton manager;
    // drop it before process teardown or the profile dies with a live
    // page (and segfaults).
    DownloadManager::instance()->cleanup();
    DownloadManager::instance()->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// DOWN04(a): which path removes a finished row — the report said a
// completed download vanished from the sidebar list with the
// removeDownloadsPolicy already at Never.  The candidates are the
// off-the-record rule in DownloadManager::updateRow (a finished
// private download leaves no record, by design since SEC07) and the
// proxy/sort layer.  This reproduces both legs end to end through the
// real sidebar: a normal-profile download must stay in the model and
// the proxy until the user clears it, while an off-the-record one is
// still dropped on finish — the documented rule, re-confirmed.
void tst_BrowserMainWindow::downloadsSidebarFinishedRows()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));

    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    DownloadManager::instance()->setDownloadDirectory(
        downloadDir.path() + QLatin1Char('/'));

    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(QMetaObject::invokeMethod(window, "downloadManager"));
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QListView *view = panel->findChild<QListView *>(
        QLatin1String("sidebarDownloadsView"));
    QVERIFY(view);
    QAbstractItemModel *proxy = view->model();
    QVERIFY(proxy);
    QAbstractItemModel *source =
        DownloadManager::instance()->model();
    QVERIFY(source);
    // The singleton may have carried rows over from an earlier test.
    DownloadManager::instance()->cleanup();
    QCOMPARE(source->rowCount(), 0);

    // A normal-profile download: the row appears and must still be
    // there — in BOTH the source model and the sidebar's proxy —
    // after the transfer finishes (removeDownloadsPolicy is Never).
    const QUrl url(QString::fromLatin1("data:text/plain;base64,")
        + QString::fromLatin1(QByteArray("hello world").toBase64()));
    Engine::Page *normalPage = DownloadManager::instance()->retryPage(false);
    QVERIFY(normalPage);
    QVERIFY(!normalPage->isOffTheRecord());
    DownloadManager::instance()->download(normalPage, url);
    QTRY_COMPARE(source->rowCount(), 1);
    QTRY_COMPARE(proxy->rowCount(), 1);
    DownloadItem *item = DownloadManager::instance()->itemAt(0);
    QVERIFY(item);
    QTRY_VERIFY(item->downloadedSuccessfully());
    QTest::qWait(300);
    QApplication::processEvents();
    QCOMPARE(source->rowCount(), 1);
    QCOMPARE(proxy->rowCount(), 1);
    // The proxy row maps back to the item and stays selectable.
    const QModelIndex proxyIndex = proxy->index(0, 0);
    QVERIFY(proxyIndex.isValid());
    QCOMPARE(proxyIndex.data(DownloadModel::CompletedRole).toBool(), true);
    view->setCurrentIndex(proxyIndex);
    QCOMPARE(DownloadManager::instance()->itemAt(0), item);

    // The off-the-record rule: a private download is dropped from the
    // model when it finishes — SEC07 persistence semantics, kept by
    // design (the finished row may not linger as a recoverable record
    // of a private transfer).  A stalled HTTP response keeps the row
    // observable: an instant data: transfer can add and drop inside a
    // single event-loop turn, so rowCount() would never read 2.
    QTcpServer stalled;
    QVERIFY(stalled.listen(QHostAddress::LocalHost));
    QObject::connect(&stalled, &QTcpServer::newConnection, &stalled,
                     [&stalled]() {
        QTcpSocket *socket = stalled.nextPendingConnection();
        socket->setParent(&stalled);
        socket->readAll();
        socket->write("HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/octet-stream\r\n"
                      "Content-Disposition: attachment; "
                          "filename=\"otr.bin\"\r\n"
                      "Content-Length: 104857600\r\n\r\n");
        QTimer *trickle = new QTimer(socket);
        QObject::connect(trickle, &QTimer::timeout, socket, [socket]() {
            socket->write("x");
        });
        trickle->start(50);
    });
    const QUrl stalledUrl(QString::fromLatin1(
        "http://127.0.0.1:%1/otr.bin").arg(stalled.serverPort()));
    Engine::Page *otrPage = DownloadManager::instance()->retryPage(true);
    QVERIFY(otrPage);
    QVERIFY(otrPage->isOffTheRecord());
    DownloadManager::instance()->download(otrPage, stalledUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source->rowCount(), 2, 30000);
    QTRY_COMPARE_WITH_TIMEOUT(proxy->rowCount(), 2, 30000);
    DownloadItem *otrItem = DownloadManager::instance()->itemAt(1);
    QVERIFY(otrItem);
    QVERIFY(otrItem != item);
    QTRY_VERIFY_WITH_TIMEOUT(otrItem->downloading(), 30000);
    // Once it leaves the in-flight state the OTR row must vanish;
    // stop() lands it in a terminal state like a finish would.
    QMetaObject::invokeMethod(otrItem, "stop");
    QTRY_COMPARE_WITH_TIMEOUT(source->rowCount(), 1, 30000);
    QCOMPARE(proxy->rowCount(), 1);
    // The survivor is the normal download.
    QCOMPARE(DownloadManager::instance()->itemAt(0), item);

    // The row only leaves the list when the user clears it.
    QVERIFY(source->removeRow(0));
    QCOMPARE(source->rowCount(), 0);
    QCOMPARE(proxy->rowCount(), 0);

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    closeWindow(window);

    // Same teardown as downloadsSidebarPanel: the retry pages live on
    // the singleton manager — drop it before process teardown or a
    // profile dies under a live page (and segfaults).
    DownloadManager::instance()->cleanup();
    DownloadManager::instance()->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// DOWN04(b): 'I can not see the main action button' — the expanded
// detail card's fixed-width button row demanded ~440px and got
// crushed to slivers (32px buttons) whenever the dock narrowed, which
// also held the dock at ~280px instead of its 160px floor.  Now the
// buttons reflow in a wrapping row and every page control may
// compress, so at the floor all controls stay fully inside the page.
void tst_BrowserMainWindow::downloadsSidebarMinWidth()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));

    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    DownloadManager::instance()->setDownloadDirectory(
        downloadDir.path() + QLatin1Char('/'));

    SubWindow *window = new SubWindow;
    window->resize(900, 600);
    window->show();
    QVERIFY(QMetaObject::invokeMethod(window, "downloadManager"));
    QDockWidget *dock = window->sidebarDock();
    QVERIFY(dock && dock->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QListView *view = panel->findChild<QListView *>(
        QLatin1String("sidebarDownloadsView"));
    QVERIFY(view);

    // Seed + select a finished download so the expanded card is
    // hosted in the detail pane.
    const QUrl url(QString::fromLatin1("data:text/plain;base64,")
        + QString::fromLatin1(QByteArray("hello world").toBase64()));
    DownloadManager::instance()->download(
        DownloadManager::instance()->retryPage(false), url);
    QTRY_COMPARE(view->model()->rowCount(), 1);
    DownloadItem *item = DownloadManager::instance()->itemAt(0);
    QVERIFY(item);
    QTRY_VERIFY(item->downloadedSuccessfully());
    view->setCurrentIndex(view->model()->index(0, 0));
    QFrame *detail = panel->findChild<QFrame *>(
        QLatin1String("sidebarDownloadDetail"));
    QVERIFY(detail);
    QVERIFY(detail->isVisibleTo(detail->parentWidget()));
    DownloadItem *card = detail->findChild<DownloadItem *>();
    QVERIFY(card);

    // Drag the dock to its 160px floor — before the fix the card's
    // 442px minimum refused to let it below ~280px.
    window->resizeDocks({ dock }, { 160 }, Qt::Horizontal);
    QTRY_VERIFY_WITH_TIMEOUT(dock->width() < 200, 15000);

    QWidget *page = panel->downloadsPage();
    QVERIFY(page);
    const int pageRight = page->rect().right();
    // Header row: the close button must remain fully inside the page
    // — it is the header's only action and the report's 'main action
    // button' must never be clipped again.
    QToolButton *closeButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarCloseButton"));
    QVERIFY(closeButton);
    QVERIFY2(closeButton->isVisibleTo(page), "close button hidden");
    QVERIFY2(closeButton->geometry().right() <= pageRight,
             qPrintable(QStringLiteral("close button clipped: %1 > %2")
                 .arg(closeButton->geometry().right()).arg(pageRight)));
    // Page-level controls keep a usable width instead of slivers.
    for (const QString name :
         { QLatin1String("sidebarDownloadsSearch"),
           QLatin1String("sidebarDownloadsSort"),
           QLatin1String("sidebarDownloadSettings") }) {
        QWidget *control = panel->findChild<QWidget *>(name);
        QVERIFY2(control, qPrintable(name));
        QVERIFY2(control->geometry().right() <= pageRight,
                 qPrintable(name));
        QVERIFY2(control->width() >= 60,
                 qPrintable(name + QStringLiteral(" crushed to %1px")
                     .arg(control->width())));
    }
    // The hosted card fits inside the detail pane and every one of
    // its action buttons keeps its natural size — the flow row wraps
    // them instead of squeezing.
    QVERIFY2(card->geometry().right() <= detail->rect().right(),
             qPrintable(QStringLiteral("card clipped: %1 > %2")
                 .arg(card->geometry().right())
                 .arg(detail->rect().right())));
    const QList<QPushButton *> buttons =
        card->findChildren<QPushButton *>();
    QVERIFY(!buttons.isEmpty());
    for (QPushButton *button : buttons) {
        if (button->isHidden())
            continue;
        QVERIFY2(button->geometry().right() <= card->rect().right(),
                 qPrintable(QStringLiteral("%1 clipped at %2 (card %3)")
                     .arg(button->objectName())
                     .arg(button->geometry().right())
                     .arg(card->rect().right())));
        QVERIFY2(button->width() >= 40,
                 qPrintable(QStringLiteral("%1 squeezed to %2px")
                     .arg(button->objectName()).arg(button->width())));
    }

    // Widen back — the buttons reflow onto fewer rows but none of
    // them is crushed in the process.
    window->resizeDocks({ dock }, { 320 }, Qt::Horizontal);
    QTRY_VERIFY_WITH_TIMEOUT(dock->width() >= 250, 15000);
    for (QPushButton *button : buttons) {
        if (button->isHidden())
            continue;
        QVERIFY2(button->geometry().right() <= card->rect().right(),
                 qPrintable(button->objectName()));
    }

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    view->setCurrentIndex(QModelIndex());
    closeWindow(window);

    DownloadManager::instance()->cleanup();
    DownloadManager::instance()->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// SIDE07: a widget tab (Preferences, History) has no WebView, so
// currentTab() is null — every bookmarks/sidebar entry point reachable
// from that state must survive it: "Show All Bookmarks" still opens,
// bookmark/history opens land in a fresh real tab instead of dying,
// "Add Bookmark" opens its dialog with empty fields rather than
// dereferencing the missing page.
void tst_BrowserMainWindow::widgetTabBookmarksSafety()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("sidebar/panels/bookmarks"));

    SubWindow *window = new SubWindow;
    window->show();
    const int tabsBefore = window->tabWidget()->count();

    QVERIFY(QMetaObject::invokeMethod(window, "preferences"));
    QVERIFY(!window->currentTab());

    // "Show All Bookmarks" raises the sidebar on the Bookmarks page —
    // the reported crash path.  No dialog, no second window.
    QVERIFY(QMetaObject::invokeMethod(window, "showBookmarksDialog"));
    QVERIFY(window->sidebarDock()->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QCOMPARE(panel->tabs()->currentWidget(), panel->bookmarksPage());
    for (QWidget *widget : QApplication::topLevelWidgets())
        QVERIFY(!qobject_cast<BookmarksDialog *>(widget));

    rejectModal(); // AddBookmarkDialog opens empty, never crashes.
    QVERIFY(QMetaObject::invokeMethod(window, "addBookmark"));

    // A bookmark's open-in-current-tab cannot navigate a widget tab —
    // it degrades to a new tab instead of doing nothing (or crashing).
    emit panel->openUrl(QUrl(QLatin1String("about:blank")),
                        TabWidget::CurrentTab, QString());
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 2);
    QVERIFY(window->currentTab());

    // Back on the widget tab, every menu path that used to dereference
    // currentTab() unchecked now no-ops or degrades gracefully.
    QVERIFY(QMetaObject::invokeMethod(window, "preferences"));

    QAction action;
    action.setData(1); // forward-navigation offset
    QVERIFY(QMetaObject::invokeMethod(window, "openActionUrl",
                                      Q_ARG(QAction *, &action)));

    // Window-menu switching to a window whose current tab is a widget
    // tab used to deref the null currentTab() for the focus call —
    // only BrowserApplication::newMainWindow() registers, so this leg
    // needs a window born there.
    BrowserApplication *app = BrowserApplication::instance();
    QVERIFY(app);
    BrowserMainWindow *registered = app->newMainWindow();
    QVERIFY(QMetaObject::invokeMethod(registered, "preferences"));
    QVERIFY(!registered->currentTab());
    const int windowIndex = app->mainWindows().indexOf(registered);
    QVERIFY(windowIndex >= 0);
    QVERIFY(QMetaObject::invokeMethod(window, "aboutToShowWindowMenu"));
    QMenu *windowMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
            && menuAction->text().contains(QLatin1String("Window")))
            windowMenu = menuAction->menu();
    }
    QVERIFY(windowMenu);
    QAction *registeredAction = nullptr;
    for (QAction *menuAction : windowMenu->actions()) {
        if (menuAction->data().isValid()
            && menuAction->data().toInt() == windowIndex) {
            registeredAction = menuAction;
            break;
        }
    }
    QVERIFY(registeredAction);
    registeredAction->trigger();

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("sidebar/panels/bookmarks"));
    closeWindow(registered);
    closeWindow(window);
}

// TABS08: the rendered tab strip stays in the compact ~26-28px
// browser band even at desktop-size fonts, and a container tab no
// longer doubles the row — the regression this fixes: the CONT02
// name chip reserved a second row (~18px) per container tab and
// QTabBar sizes every tab to the tallest hint.
void tst_BrowserMainWindow::tabStripCompactHeight()
{
    // The offscreen fallback 'Sans Serif 9' understates fm.height() —
    // pin a desktop-size font so the exercised bound is the real one.
    QFont font = QApplication::font();
    font.setPointSize(11);
    QApplication::setFont(font);
    SubWindow *window = new SubWindow;
    window->resize(1000, 700);
    window->show();
    for (int i = 0; i < 20; ++i)
        QApplication::processEvents();

    TabBar *bar = window->tabWidget()->findChild<TabBar *>();
    QVERIFY(bar);
    const int cap = qMax(bar->fontMetrics().height(),
                         bar->iconSize().height()) + 9;
    QVERIFY2(bar->height() <= cap,
             qPrintable(QStringLiteral(
                 "strip height %1 exceeds the compact cap %2")
                .arg(bar->height()).arg(cap)));
    QCOMPARE(bar->tabRect(0).height(), bar->height());

    // Inline-chips mode (tabs/containerDisplay=0, what the reporter
    // runs): a container tab must not stretch the strip.
    QSettings settings;
    settings.setValue(QLatin1String("tabs/containerDisplay"), 0);
    window->tabWidget()->loadSettings();
    const ContainerManager::Container container =
        ContainerManager::instance()->createContainer(
            QLatin1String("StripBox"), QColor(Qt::blue));
    window->tabWidget()->makeNewTabInContainer(container.id, true);
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();
    for (int i = 0; i < bar->count(); ++i)
        QCOMPARE(bar->tabRect(i).height(), bar->tabRect(0).height());
    QVERIFY(bar->height() <= cap);
    QCOMPARE(bar->tabRect(0).height(), bar->height());

    // Close + favicon widgets stay vertically centered in the row.
    QWidget *close = bar->tabButton(0, QTabBar::RightSide);
    if (!close)
        close = bar->tabButton(0, QTabBar::LeftSide);
    QVERIFY(close);
    QVERIFY(qAbs(close->geometry().center().y()
                 - bar->tabRect(0).center().y()) <= 2);

    if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
        const QString dir =
            qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
        bar->grab().save(dir + QLatin1String("/strip.png"));
    }

    // TABS10: the Left/Right strip keeps single-line rows on the
    // real window — the reporter saw ~2x a text line.
    settings.setValue(QLatin1String("tabs/tabBarPosition"), 2);
    window->tabWidget()->loadSettings();
    for (int i = 0; i < 10; ++i)
        QApplication::processEvents();
    QVERIFY2(bar->tabRect(0).height()
                 <= bar->fontMetrics().height() + 12,
             qPrintable(QStringLiteral(
                 "vertical row height %1 exceeds one text line (%2)")
                 .arg(bar->tabRect(0).height())
                 .arg(bar->fontMetrics().height() + 12)));
    QVERIFY(bar->width() >= 140 && bar->width() <= 150);
    if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
        const QString dir =
            qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
        bar->grab().save(dir + QLatin1String("/strip-vertical.png"));
    }
    settings.remove(QLatin1String("tabs/tabBarPosition"));

    settings.remove(QLatin1String("tabs/containerDisplay"));
    closeWindow(window);
}

QTEST_MAIN(tst_BrowserMainWindow)
#include "tst_browsermainwindow.moc"
