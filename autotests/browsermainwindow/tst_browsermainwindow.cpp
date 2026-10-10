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
#include <qwebengineprofile.h>
#include <qwebenginepage.h>
#include <qcombobox.h>
#include <qdockwidget.h>
#include <qframe.h>
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

#include "browsermainwindow.h"
#include "browserapplication.h"
#include "history.h"
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
    void downloadsMenuHome();
    void devToolsSubmenu();
    void stateSerialization();
    void events();
    void closeConfirm();
    void chromeMetrics();
    void searchBoxVisibility();
    void torSearchBoxHidden();
    void torCircuitStatusLabel();
    void readerModeButton();
    void statusBarWidgets();
    void statusBarIndicators();
    void sidebarPanel();
    void downloadsSidebarPanel();
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
                       Q_ARG(QWebEnginePage *, static_cast<QWebEnginePage *>(window->currentTab()->webPage()))));
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

    // Slot order in the toolbar's action list: Back, Forward,
    // Stop/Reload, the reader button, then the location-bar splitter.
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
    QCOMPARE(readerIndex, 3);
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
    QWebEnginePage *page = DownloadManager::instance()->retryPage(false);
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

QTEST_MAIN(tst_BrowserMainWindow)
#include "tst_browsermainwindow.moc"
