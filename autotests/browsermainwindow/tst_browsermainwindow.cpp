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
#include <qdockwidget.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qmenu.h>
#include <qmenubar.h>
#include <qmessagebox.h>
#include <qplaintextedit.h>
#include <qprogressbar.h>
#include <qpushbutton.h>
#include <qstatusbar.h>
#include <qtabwidget.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>

#include "browsermainwindow.h"
#include "browserapplication.h"
#include "sidebarpanel.h"
#include "statusbarwidgets.h"
#include "tabwidget.h"
#include "tabbar.h"
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
    void toolsMenuDedup();
    void fileMenuOrder();
    void stateSerialization();
    void events();
    void closeConfirm();
    void chromeMetrics();
    void searchBoxVisibility();
    void torSearchBoxHidden();
    void torCircuitStatusLabel();
    void statusBarWidgets();
    void sidebarPanel();
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

    // fileNew spins up a second window; downloadManager shows the
    // non-modal download dialog.
    QVERIFY(QMetaObject::invokeMethod(window, "fileNew"));
    QVERIFY(QMetaObject::invokeMethod(window, "downloadManager"));

    const QWidgetList topLevel = QApplication::topLevelWidgets();
    for (QWidget *widget : topLevel) {
        if (widget != window && widget->inherits("QMainWindow"))
            widget->close();
    }
    if (QDialog *downloads = qobject_cast<QDialog *>(
            BrowserApplication::downloadManager()))
        downloads->close();
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
    }

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

    // Triggering it still runs webSearch() — SRCH04's button mode
    // (fresh profile) answers with the non-modal engines menu, whose
    // "Search..." prompt is the search entry point.
    webSearch->trigger();
    QWidget *popup = QApplication::activePopupWidget();
    QVERIFY(popup);
    popup->close();

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
    QVERIFY(newWindow != -1 && newTab != -1 && newPrivateTab != -1
            && newTor != -1);
    QVERIFY(newWindow < newTab);
    QVERIFY(newTab < newPrivateTab);
    QVERIFY(newPrivateTab < newTor);
    QCOMPARE(privateBrowsing, -1);
    QVERIFY(newTor < closeWindowIndex);

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

// SRCH03+SRCH04: the dedicated search box has two display modes —
// showSearchBox unset/false collapses it to the engine button
// (Vivaldi's "Show as a Button"), true gives the full text field.
// The preference applies live without restart.
void tst_BrowserMainWindow::searchBoxVisibility()
{
    // initTestCase cleared settings; no showSearchBox key = button mode.
    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(window->toolbarSearch()->isButtonMode());
    QVERIFY(window->toolbarSearch()->isVisible());
    QVERIFY(window->toolbarSearch()->isReadOnly());

    // Field mode applies live and the shortcut selects the box text.
    QSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
    window->applySearchBoxVisibility();
    QVERIFY(!window->toolbarSearch()->isButtonMode());
    QVERIFY(!window->toolbarSearch()->isReadOnly());
    window->toolbarSearch()->setText(QLatin1String("arora"));
    QVERIFY(QMetaObject::invokeMethod(window, "webSearch"));
    QVERIFY(window->toolbarSearch()->hasSelectedText());
    closeWindow(window);

    // New windows read the persisted key at construction.
    SubWindow *shown = new SubWindow;
    shown->show();
    QVERIFY(!shown->toolbarSearch()->isButtonMode());
    closeWindow(shown);

    QSettings().remove(QLatin1String("MainWindow/showSearchBox"));
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
    closeWindow(window);
}

QTEST_MAIN(tst_BrowserMainWindow)
#include "tst_browsermainwindow.moc"
