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
#include <qwebengineview.h>
#include <qcombobox.h>
#include <qdockwidget.h>
#include <qfile.h>
#include <qframe.h>
#include <qhash.h>
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
#include "readersanitizer.h"
#include "webview.h"
#include "webpage.h"
#include "webviewsearch.h"
#include "locationbar.h"
#include "toolbarsearch.h"
#include "torcircuitbutton.h"
#include "torcircuitpanel.h"
#include "tormanager.h"
#include "tortitlebar.h"
#include "downloadmanager.h"
#include "extensionactionbar.h"
#include "extensionmanager.h"
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

// ICONS09: count pixels that survived with a color other than the
// palette tint — the chrome engine flattens all art to ButtonText,
// so a raw-color glyph (or a set's own recolor) shows up as off-tint
// pixels.  A 2-per-channel tolerance covers the premultiplied
// round-trip's ±1 rounding on anti-aliased edges; brand art differs
// by far more.  Returns -1 when the icon rendered nothing at all.
static int offTintPixels(const QIcon &icon, int extent,
                         const QColor &tint)
{
    const QImage image = icon.pixmap(QSize(extent, extent)).toImage();
    if (image.isNull())
        return -1;
    int off = 0;
    int opaque = 0;
    for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x) {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 100) {
                ++opaque;
                if (qAbs(color.red() - tint.red()) > 2
                        || qAbs(color.green() - tint.green()) > 2
                        || qAbs(color.blue() - tint.blue()) > 2)
                    ++off;
            }
        }
    return opaque ? off : -1;
}

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
    void menuIcons();
    void chromeIconsTinted();
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
    void torCircuitPopupFirstShow();
    void torStartPage();
    void torTitleBar();
    void torTitleBarTitle();
    void servoStartPage();
    void privateStartPage();
    void startPageSingleSurface();
    void readerModeButton();
    void readerModeContent_data();
    void readerModeContent();
    void readerSanitizer();
    void homeButton();
    void homePageModes();
    void statusBarWidgets();
    void statusBarIndicators();
    void sidebarPanel();
    void sidebarPanelOrder();
    void sidebarPanelOptions();
    void sidebarGripResize();
    void downloadsSidebarPanel();
    void downloadsSidebarFinishedRows();
    void downloadsSidebarMinWidth();
    void widgetTabBookmarksSafety();
    void tabStripCompactHeight();
    void stopReloadState();
    void extensionActionButtons();
    void navBarEditMode();
    void navBarOrderPersistence();
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
    // loadProgress() calls the private updateStopReloadState()
    // helper — a real mid-flight value takes the "Stop" branch,
    // 100 the "Reload" branch; the current page's isLoading() can
    // still force Stop for a 0/100-only engine.
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

    // MENU12+MENU15: Preferences and Quit both left the File menu —
    // they are direct app-menu leaves now (asserted in
    // appMenuButton) — so Close Window is the last File row.
    QCOMPARE(preferencesIndex, -1);
    QCOMPARE(entries.indexOf(QLatin1String("Quit")), -1);
    QCOMPARE(entries.constLast(), QLatin1String("Close Window"));

    // MENU14: "Open Location" left the File menu too — its action
    // survives on the window as the Ctrl+L/Alt+D/Alt+O carrier so the
    // location-bar focus shortcut keeps working with no menu row.
    QCOMPARE(entries.indexOf(QLatin1String("Open Location...")), -1);
    QAction *openLocation = nullptr;
    for (QAction *action : window->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Open Location..."))
            openLocation = action;
    }
    QVERIFY(openLocation);
    QVERIFY(openLocation->shortcuts().contains(
        QKeySequence(Qt::ControlModifier | Qt::Key_L)));
    openLocation->trigger();
    if (window->isActiveWindow())
        QCOMPARE(QApplication::focusWidget(),
                 static_cast<QWidget *>(
                     window->tabWidget()->currentLocationBar()));

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

// ICONS07: every static leaf action in the shared top-level menus
// carries an icon — the app-menu dropdown reads as one uniform icon
// column.  ICONS08: the STATIC submenu headers that sit among those
// leafs carry icons too (File > New Container Tab, View > Development
// Tools, View > Text Encoding, Tools > User Agent, History >
// Recently Closed Tabs).  Documented exceptions, asserted here too:
// separators and the dynamic entries (history/bookmark rows, window
// list, encoding and user-agent pickers, model-driven flyouts like
// bookmark folders) which legitimately have none.  Context menus and
// the app-menu's own top-level rows read consistently bare — no leaf
// in them carries an icon — so their headers stay clean as well.
void tst_BrowserMainWindow::menuIcons()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    // ICONS10: pin the bundled default — the duplicate-glyph audit
    // below is about what a fresh profile actually renders.
    QSettings iconSettings;
    const QVariant savedIconTheme =
        iconSettings.value(QLatin1String("MainWindow/iconTheme"));
    iconSettings.setValue(QLatin1String("MainWindow/iconTheme"),
                          QLatin1String("tabler"));
    AroraIcon::applyFromSettings();

    SubWindow *window = new SubWindow;
    window->show();

    QHash<QString, QMenu *> menus;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu())
            menus.insert(menuAction->text().remove(QLatin1Char('&')),
                         menuAction->menu());
    }
    QCOMPARE(menus.count(), 8);

    // aboutToShow-built rows only exist once the menu has popped —
    // cycle every top-level menu plus the File > New Container Tab
    // flyout so the audit sees the real layout.
    for (QMenu *menu : std::as_const(menus)) {
        menu->popup(QPoint(60, 60));
        QApplication::processEvents();
        menu->hide();
    }
    QMenu *containerMenu = nullptr;
    QMenu *devToolsMenu = nullptr;
    for (QAction *action : menus.value(QLatin1String("File"))->actions()) {
        if (action->menu()
                && action->menu()->title().remove(QLatin1Char('&'))
                       == QLatin1String("New Container Tab"))
            containerMenu = action->menu();
    }
    if (containerMenu) {
        containerMenu->popup(QPoint(120, 80));
        QApplication::processEvents();
        containerMenu->hide();
    }
    for (QAction *action : menus.value(QLatin1String("View"))->actions()) {
        if (action->menu()
                && action->menu()->title().remove(QLatin1Char('&'))
                       == QLatin1String("Development Tools"))
            devToolsMenu = action->menu();
    }

    // Fully-static menus: no row may be iconless — ICONS08 iconed
    // the submenu headers that stood naked among the leaf rows.
    for (QLatin1String title : { QLatin1String("File"),
                                 QLatin1String("Edit"),
                                 QLatin1String("View"),
                                 QLatin1String("Tools"),
                                 QLatin1String("Help") }) {
        QMenu *menu = menus.value(title);
        QVERIFY(menu);
        for (QAction *action : menu->actions()) {
            if (action->isSeparator())
                continue;
            const QString row = title + QLatin1String(" > ")
                + action->text();
            QVERIFY2(!action->icon().isNull(), qPrintable(row));
            QVERIFY2(!action->icon().pixmap(QSize(16, 16)).isNull(),
                     qPrintable(row));
        }
    }

    // The container flyout mixes colored chips with static tail rows —
    // every leaf carries an icon so no row reads naked next to them.
    QVERIFY(containerMenu);
    for (QAction *action : containerMenu->actions()) {
        if (!action->isSeparator())
            QVERIFY2(!action->icon().isNull(), qPrintable(action->text()));
    }

    // View > Development Tools is a static submenu.
    QVERIFY(devToolsMenu);
    for (QAction *action : devToolsMenu->actions()) {
        if (!action->isSeparator())
            QVERIFY2(!action->icon().isNull(), qPrintable(action->text()));
    }

    // ICONS10: "some icons look the same" — no two rows in one static
    // menu may render an identical glyph.  The mixed menus are exempt:
    // their model-driven rows carry identity art (site favicons,
    // container-color chips) where sharing a glyph is legitimate.
    QList<QMenu *> staticMenus;
    for (QLatin1String title : { QLatin1String("File"),
                                 QLatin1String("Edit"),
                                 QLatin1String("View"),
                                 QLatin1String("Tools"),
                                 QLatin1String("Help") })
        staticMenus.append(menus.value(title));
    staticMenus.append(devToolsMenu);
    for (QMenu *menu : std::as_const(staticMenus)) {
        QVERIFY(menu);
        QList<QPair<QImage, QString>> glyphs;
        for (QAction *action : menu->actions()) {
            if (action->isSeparator() || action->icon().isNull())
                continue;
            const QImage image = action->icon()
                .pixmap(QSize(16, 16)).toImage()
                .convertToFormat(QImage::Format_ARGB32);
            for (const QPair<QImage, QString> &prev : glyphs)
                QVERIFY2(image != prev.first,
                         qPrintable(menu->title() + QLatin1String(" > ")
                             + action->text()
                             + QLatin1String(" shares ")
                             + prev.second
                             + QLatin1String("'s glyph")));
            glyphs.append(qMakePair(image, action->text()));
        }
    }

    // The mixed menus: dynamic entries stay bare, the static rows all
    // carry icons.
    const QHash<QString, QStringList> required = {
        { QLatin1String("History"),
          { QLatin1String("Back"), QLatin1String("Forward"),
            QLatin1String("Home"), QLatin1String("Recently Closed Tabs"),
            QLatin1String("Restore Last Session"),
            QLatin1String("Show All History"),
            QLatin1String("Clear History...") } },
        { QLatin1String("Bookmarks"),
          { QLatin1String("Show All Bookmarks..."),
            QLatin1String("Add Bookmark..."),
            QLatin1String("Bookmark All Tabs"),
            QLatin1String("Add Folder...") } },
        { QLatin1String("Window"),
          { QLatin1String("Show Next Tab"),
            QLatin1String("Show Previous Tab"),
            QLatin1String("Search Tabs...") } },
    };
    for (auto it = required.constBegin(); it != required.constEnd(); ++it) {
        QMenu *menu = menus.value(it.key());
        QVERIFY(menu);
        for (const QString &text : it.value()) {
            QAction *hit = nullptr;
            for (QAction *action : menu->actions()) {
                if (action->text().remove(QLatin1Char('&')) == text)
                    hit = action;
            }
            QVERIFY2(hit, qPrintable(it.key() + QLatin1String(" > ") + text));
            QVERIFY2(!hit->icon().isNull(),
                     qPrintable(it.key() + QLatin1String(" > ") + text));
        }
    }

    // The tor window rides the same menus — icons survive there too.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->show();
    QMenu *torFile = torWindow->menuBar()->actions().isEmpty()
        ? (torWindow->appMenu()
               ? torWindow->appMenu()->actions().first()->menu()
               : nullptr)
        : torWindow->menuBar()->actions().first()->menu();
    QVERIFY(torFile);
    int torMissing = 0;
    for (QAction *action : torFile->actions()) {
        // Headers count too since ICONS08 — the hidden container
        // submenu still carries its icon.
        if (!action->isSeparator()
                && (action->icon().isNull()
                    || action->icon().pixmap(QSize(16, 16)).isNull()))
            ++torMissing;
    }
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);
    QCOMPARE(torMissing, 0);

    closeWindow(window);
    if (savedIconTheme.isValid())
        iconSettings.setValue(QLatin1String("MainWindow/iconTheme"),
                              savedIconTheme);
    else
        iconSettings.remove(QLatin1String("MainWindow/iconTheme"));
    AroraIcon::applyFromSettings();
#endif
}

// ICONS09: the house-icon rule on the real surfaces — every chrome
// glyph renders 100% in the palette's ButtonText (flat monochrome,
// whichever theme served the base art), and the documented identity
// exceptions keep their own colors: the Qt mark on About Qt, the
// Arora mark on About <app>, and the container flyout's color chips.
void tst_BrowserMainWindow::chromeIconsTinted()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    const QColor tint = qApp->palette()
        .color(QPalette::Normal, QPalette::ButtonText);

    // All iconed leaf actions in a menu must be on-tint; brand rows
    // and submenu flyouts with identity art are checked separately.
    const auto checkMenu = [&](QMenu *menu, const QString &title,
                               const QStringList &brandRows) {
        QVERIFY2(menu, qPrintable(title));
        for (QAction *action : menu->actions()) {
            if (action->isSeparator() || action->icon().isNull())
                continue;
            const QString row = title + QLatin1String(" > ")
                + action->text();
            const int off = offTintPixels(action->icon(), 16, tint);
            QVERIFY2(off >= 0, qPrintable(row + " rendered nothing"));
            if (brandRows.contains(
                        action->text().remove(QLatin1Char('&')))) {
                QVERIFY2(off > 0,
                         qPrintable(row + " lost its brand colors"));
            } else {
                QVERIFY2(off == 0, qPrintable(
                    row + QStringLiteral(" (%1 off-tint px)")
                            .arg(off)));
            }
        }
    };

    const QStringList brandRows = {
        QLatin1String("About Qt"),
        // "About <app>" — the window icon, i.e. the Arora mark.
        QCoreApplication::applicationName().isEmpty()
            ? QLatin1String("About")
            : QStringLiteral("About %1")
                  .arg(QCoreApplication::applicationName()),
    };

    const auto menuTitles = [&]() {
        return QList<QLatin1String>({ QLatin1String("File"),
                                      QLatin1String("Edit"),
                                      QLatin1String("View"),
                                      QLatin1String("Tools"),
                                      QLatin1String("Help") });
    };

    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();
    QHash<QString, QMenu *> menus;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu())
            menus.insert(menuAction->text().remove(QLatin1Char('&')),
                         menuAction->menu());
    }
    QCOMPARE(menus.count(), 8);
    // Populate the aboutToShow-built rows the audit must see.
    for (QMenu *menu : std::as_const(menus)) {
        menu->popup(QPoint(60, 60));
        QApplication::processEvents();
        menu->hide();
    }
    for (QLatin1String title : menuTitles())
        checkMenu(menus.value(title), QString::fromLatin1(title),
                  title == QLatin1String("Help") ? brandRows
                                                 : QStringList());

    // The container flyout mixes identity chips with chrome rows:
    // chips stay colored, the static tail rows are tinted.
    for (QAction *action : menus.value(QLatin1String("File"))->actions()) {
        if (!action->menu()
                || action->menu()->title().remove(QLatin1Char('&'))
                       != QLatin1String("New Container Tab"))
            continue;
        action->menu()->popup(QPoint(120, 80));
        QApplication::processEvents();
        action->menu()->hide();
        int chips = 0;
        for (QAction *row : action->menu()->actions()) {
            if (row->isSeparator() || row->icon().isNull())
                continue;
            const QString label = row->text().remove(QLatin1Char('&'));
            if (label == QLatin1String("New Container...")
                    || label == QLatin1String("Manage Containers...")) {
                QCOMPARE(offTintPixels(row->icon(), 16, tint), 0);
            } else {
                QVERIFY2(offTintPixels(row->icon(), 16, tint) > 0,
                         qPrintable(label + " chip lost its color"));
                ++chips;
            }
        }
        QVERIFY(chips > 0);
    }

    // Nav-bar glyph actions ride the same palette tint.
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    int navIcons = 0;
    for (QAction *action : navBar->actions()) {
        if (action->icon().isNull())
            continue;
        QVERIFY2(offTintPixels(action->icon(), 16, tint) == 0,
                 qPrintable(action->text()));
        ++navIcons;
    }
    QVERIFY(navIcons >= 4);

    // An installed menu icon re-tints live across a palette flip —
    // nothing re-sets it.
    const QImage before = menus.value(QLatin1String("File"))
        ->actions().first()->icon().pixmap(QSize(16, 16)).toImage();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    const QColor darkTint = BrowserTheme::darkPalette()
        .color(QPalette::ButtonText);
    QCOMPARE(offTintPixels(menus.value(QLatin1String("File"))
                           ->actions().first()->icon(), 16, darkTint),
             0);
    BrowserTheme::applyColorScheme();
    QApplication::processEvents();
    QCOMPARE(menus.value(QLatin1String("File"))->actions().first()
                 ->icon().pixmap(QSize(16, 16)).toImage(), before);

    // Private window: same surfaces, same rule.
    BrowserApplication::setPrivate(true);
    SubWindow *privateWindow = new SubWindow;
    privateWindow->show();
    QApplication::processEvents();
    int privateIcons = 0;
    for (QAction *action :
         privateWindow->menuBar()->actions()) {
        if (!action->menu()
                || action->text().remove(QLatin1Char('&'))
                       != QLatin1String("File"))
            continue;
        for (QAction *leaf : action->menu()->actions()) {
            if (leaf->isSeparator() || leaf->icon().isNull())
                continue;
            QVERIFY2(offTintPixels(leaf->icon(), 16, tint) == 0,
                     qPrintable(leaf->text()));
            ++privateIcons;
        }
    }
    closeWindow(privateWindow);
    BrowserApplication::setPrivate(false);
    QVERIFY(privateIcons > 0);

    // Tor window: menu popups stay on the platform palette, so the
    // same dark-on-light tint applies — the violet nav cluster's
    // explicit lift is covered by torNavIconsDarkChrome.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->show();
    QApplication::processEvents();
    QMenu *torFile = torWindow->menuBar()->actions().isEmpty()
        ? (torWindow->appMenu()
               ? torWindow->appMenu()->actions().first()->menu()
               : nullptr)
        : torWindow->menuBar()->actions().first()->menu();
    QVERIFY(torFile);
    int torIcons = 0;
    for (QAction *action : torFile->actions()) {
        if (action->isSeparator() || action->icon().isNull())
            continue;
        QVERIFY2(offTintPixels(action->icon(), 16, tint) == 0,
                 qPrintable(action->text()));
        ++torIcons;
    }
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);
    QVERIFY(torIcons > 0);

    closeWindow(window);
#endif
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
    // MENU13: icon-only — the wordmark moved to the bar's trailing
    // edge; the text property stays "Arora" for screen readers.
    QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(button->text(), QLatin1String("Arora"));
    QVERIFY(!button->icon().isNull());
    // MENU07/MENU09: the logo is bigger than the nav bar's shared
    // glyph size — the growth is icon-led, the text font is
    // untouched.
    QCOMPARE(button->iconSize(), QSize(36, 36));
    QVERIFY(button->iconSize().height() > navBar->iconSize().height());
    QVERIFY(!button->icon().pixmap(button->iconSize()).isNull());
    QVERIFY(!button->toolTip().isEmpty());
    QVERIFY(!button->accessibleName().isEmpty());

    // MENU09: iconSize is only a property — Qt paints toolbar-hosted
    // tool buttons at the BAR's iconSize, which is why the MENU07
    // bump never reached the screen.  The button lives in a
    // margin-less host so its own extent governs; prove it reached
    // the layout, not just the getter.
    QVERIFY(button->sizeHint().height()
            >= button->iconSize().height());
    QVERIFY(button->size().height()
            >= button->iconSize().height());

    // First item on the nav bar, ahead of Back — the action carries
    // the host container, whose only child is the button.
    QVERIFY(!navBar->actions().isEmpty());
    QWidget *firstWidget = navBar->widgetForAction(
        navBar->actions().first());
    QVERIFY(firstWidget);
    QCOMPARE(firstWidget, button->parentWidget());

    // MENU13: the 'Arora' wordmark is the bar's fixed trailing
    // element — a plain label riding as the last toolbar action.  It
    // carries no navBarItemId, so the persisted order ignores it and
    // the rebuild's unlisted-tail rule keeps it last; the edit-mode
    // legs below prove it can't be dragged or dropped past.
    QLabel *brandLabel = window->findChild<QLabel *>(
        QLatin1String("navBarBrandLabel"));
    QVERIFY(brandLabel);
    QCOMPARE(brandLabel->text(), QLatin1String("Arora"));
    QCOMPARE(brandLabel->font().letterSpacing(), 1.0);
    QAction *brandItem = navBar->actions().last();
    QCOMPARE(navBar->widgetForAction(brandItem), brandLabel);
    QVERIFY(brandItem->property("navBarItemId").toString().isEmpty());
    QVERIFY(!window->navBarOrder().isEmpty());

    // The dropdown lists the real top-level menus in menubar order —
    // the same QMenu objects the menubar holds, not clones.  MENU11:
    // a direct Preferences... leaf sits bracketed by separators
    // between the Tools and Help entries; MENU15: Quit is the second
    // direct leaf, closing the dropdown after Help; every other row
    // is a submenu.
    QMenu *appMenu = window->appMenu();
    QVERIFY(appMenu);
    QCOMPARE(button->menu(), appMenu);
    QStringList titles;
    QAction *appPrefs = nullptr;
    QAction *appQuit = nullptr;
    for (QAction *menuAction : appMenu->actions()) {
        if (menuAction->isSeparator())
            continue;
        const QString text =
            menuAction->text().remove(QLatin1Char('&'));
        if (!menuAction->menu()) {
            if (text == QLatin1String("Quit")) {
                appQuit = menuAction;
            } else {
                QCOMPARE(text, QLatin1String("Preferences..."));
                appPrefs = menuAction;
            }
        }
        titles << text;
    }
    QCOMPARE(titles, QStringList()
             << QLatin1String("File") << QLatin1String("Edit")
             << QLatin1String("View") << QLatin1String("History")
             << QLatin1String("Bookmarks") << QLatin1String("Window")
             << QLatin1String("Tools")
             << QLatin1String("Preferences...")
             << QLatin1String("Help")
             << QLatin1String("Quit"));
    QVERIFY(appQuit);
    const QList<QAction *> appActions = appMenu->actions();
    int toolsIndex = -1;
    for (int i = 0; i < appActions.count(); ++i) {
        if (appActions.at(i)->text().remove(QLatin1Char('&'))
                == QLatin1String("Tools"))
            toolsIndex = i;
    }
    QVERIFY(toolsIndex >= 0 && toolsIndex + 6 < appActions.count());
    QVERIFY(appActions.at(toolsIndex + 1)->isSeparator());
    QCOMPARE(appActions.at(toolsIndex + 2), appPrefs);
    QVERIFY(appActions.at(toolsIndex + 3)->isSeparator());
    QCOMPARE(appActions.at(toolsIndex + 4)->text()
                 .remove(QLatin1Char('&')),
             QLatin1String("Help"));
    // MENU15: one separator past Help, then Quit is the last action
    // outright.
    QVERIFY(appActions.at(toolsIndex + 5)->isSeparator());
    QCOMPARE(appActions.at(toolsIndex + 6), appQuit);
    QCOMPARE(appActions.constLast(), appQuit);
    const QList<QAction *> barMenus = window->menuBar()->actions();
    QList<QAction *> appSubmenus;
    for (QAction *menuAction : appActions)
        if (menuAction->menu())
            appSubmenus << menuAction;
    QCOMPARE(barMenus.count(), appSubmenus.count());
    for (int i = 0; i < barMenus.count(); ++i)
        QCOMPARE(barMenus.at(i)->menu(), appSubmenus.at(i)->menu());

    // MENU08: the dropdown paints ~120% of the platform menu font,
    // and the growth reaches every shared top-level menu plus the
    // nested flyouts already built at construction (File > New
    // Container Tab, View > Text Encoding, History > Back's flyout).
    // The menubar row itself keeps the default size.
    QMenu plainMenu;
    const QFont menuFont = appMenu->font();
    if (menuFont.pointSizeF() > 0
            && plainMenu.font().pointSizeF() > 0)
        QVERIFY(menuFont.pointSizeF()
                >= plainMenu.font().pointSizeF() * 1.15);
    else
        QVERIFY(menuFont.pixelSize() > plainMenu.font().pixelSize());
    for (QAction *menuAction : appMenu->actions()) {
        // MENU11: the direct Preferences... leaf has no submenu.
        QMenu *sub = menuAction->menu();
        if (!sub)
            continue;
        QCOMPARE(sub->font(), menuFont);
        for (QAction *innerAction : sub->actions())
            if (QMenu *nested = innerAction->menu())
                QCOMPARE(nested->font(), menuFont);
    }
    if (menuFont.pointSizeF() > 0
            && window->menuBar()->font().pointSizeF() > 0)
        QVERIFY(window->menuBar()->font().pointSizeF()
                < menuFont.pointSizeF());

    // Offscreen grabs for visual review — set ARORA_MENU_GRAB_DIR to
    // a directory and the open dropdown + File submenu land there;
    // the tor leg below adds app-menu-tor.png.
    const QByteArray menuGrabDir = qgetenv("ARORA_MENU_GRAB_DIR");
    if (!menuGrabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(menuGrabDir));
        appMenu->popup(button->mapToGlobal(
            QPoint(0, button->height())));
        QApplication::processEvents();
        appMenu->grab().save(QString::fromLocal8Bit(menuGrabDir)
                             + QLatin1String("/app-menu.png"));
        appMenu->hide();
        // MENU09: the button itself too — logo-vs-label balance is
        // the leg this task's reviewers need to see.
        button->grab().save(QString::fromLocal8Bit(menuGrabDir)
                            + QLatin1String("/app-menu-button.png"));
        // MENU10: and the whole nav row — the logo's grown height vs
        // the 18px nav buttons is judged at row level, not in
        // isolation.
        navBar->grab().save(QString::fromLocal8Bit(menuGrabDir)
                            + QLatin1String("/app-menu-navbar.png"));
        QMenu *fileMenu = appMenu->actions().first()->menu();
        fileMenu->popup(QPoint(80, 80));
        QApplication::processEvents();
        fileMenu->grab().save(QString::fromLocal8Bit(menuGrabDir)
                              + QLatin1String("/app-menu-file.png"));
        fileMenu->hide();
        QMenu *viewMenu = appMenu->actions().at(2)->menu();
        viewMenu->popup(QPoint(80, 80));
        QApplication::processEvents();
        viewMenu->grab().save(QString::fromLocal8Bit(menuGrabDir)
                              + QLatin1String("/app-menu-view.png"));
        viewMenu->hide();
    }

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

    // MENU12: the app-menu leaf is the action's only menu home —
    // File carries no copy — and the window registration keeps the
    // Ctrl+, shortcut live with no menubar row.  Triggering it opens
    // the settings tab.
    QVERIFY(!fileMenu->actions().contains(appPrefs));
    QVERIFY(window->actions().contains(appPrefs));
    QCOMPARE(appPrefs->shortcut(),
             QKeySequence(QStringLiteral("Ctrl+,")));
    QCOMPARE(appPrefs->menuRole(), QAction::PreferencesRole);
    appPrefs->trigger();
    int prefsTab = -1;
    for (int i = 0; i < window->tabWidget()->count(); ++i)
        if (window->tabWidget()->widget(i)->inherits("SettingsDialog"))
            prefsTab = i;
    QVERIFY(prefsTab >= 0);
    window->tabWidget()->closeTab(prefsTab);

    // MENU15: Quit mirrors the Preferences carrier exactly — its
    // only menu home is the app-menu leaf, File carries no copy, and
    // the window registration keeps Ctrl+Q live.  Never trigger it:
    // quitBrowser() exits the test process outright.
    QVERIFY(!fileMenu->actions().contains(appQuit));
    QVERIFY(window->actions().contains(appQuit));
    QCOMPARE(appQuit->shortcut(),
             QKeySequence(Qt::ControlModifier | Qt::Key_Q));

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
    // MENU07: every window type gets the same bigger button.
    const bool privateBigIcon = privateButton
        && privateButton->iconSize() == QSize(36, 36);
    const bool privateBarHidden =
        !privateWindow->menuBar()->isVisible();
    // MENU08: and the same enlarged menu text.
    const bool privateMenuFont = privateButton
        && privateButton->menu()
        && privateButton->menu()->font() == menuFont;
    // MENU13: the private bar carries the same icon-only button +
    // trailing wordmark.
    QToolBar *privateBar = privateWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QLabel *privateBrand = privateWindow->findChild<QLabel *>(
        QLatin1String("navBarBrandLabel"));
    const bool privateWordmark = privateButton
        && privateButton->toolButtonStyle() == Qt::ToolButtonIconOnly
        && privateBar && privateBrand
        && privateBar->widgetForAction(privateBar->actions().last())
               == privateBrand;
    closeWindow(privateWindow);
    BrowserApplication::setPrivate(false);
    QVERIFY(privateButton);
    QVERIFY(privateMenuFull);
    QVERIFY(privateBigIcon);
    QVERIFY(privateBarHidden);
    QVERIFY(privateMenuFont);
    QVERIFY(privateWordmark);

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
    // MENU11: the tor dropdown carries the same direct leaf between
    // Tools and Help; MENU15: Quit closes it there too.
    bool torPrefsLeaf = false;
    bool torQuitLast = false;
    if (torWindow->appMenu()) {
        const QList<QAction *> torActions =
            torWindow->appMenu()->actions();
        for (QAction *action : torActions) {
            if (!action->menu() && !action->isSeparator()
                && action->text().remove(QLatin1Char('&'))
                       == QLatin1String("Preferences..."))
                torPrefsLeaf = true;
        }
        torQuitLast = !torActions.isEmpty()
            && torActions.constLast()->text()
                       .remove(QLatin1Char('&'))
                   == QLatin1String("Quit");
    }
    const bool torBigIcon = torButton
        && torButton->iconSize() == QSize(36, 36);
    // MENU08: the violet window's dropdown is enlarged identically —
    // tor only re-colors the bar, menus stay on the palette + font.
    const QFont torMenuFont = torWindow->appMenu()
        ? torWindow->appMenu()->font() : QFont();
    if (!menuGrabDir.isEmpty() && torWindow->appMenu()) {
        torWindow->appMenu()->popup(QPoint(120, 120));
        QApplication::processEvents();
        torWindow->appMenu()->grab().save(
            QString::fromLocal8Bit(menuGrabDir)
            + QLatin1String("/app-menu-tor.png"));
        torWindow->appMenu()->hide();
        if (torButton)
            torButton->grab().save(
                QString::fromLocal8Bit(menuGrabDir)
                + QLatin1String("/app-menu-button-tor.png"));
        // MENU10: the violet nav row as a whole too.
        if (QToolBar *torBar = torWindow->findChild<QToolBar *>(
                QLatin1String("NavigationToolBar")))
            torBar->grab().save(
                QString::fromLocal8Bit(menuGrabDir)
                + QLatin1String("/app-menu-navbar-tor.png"));
    }
    // Grabbed-pixel proof the "Arora" wordmark reads light on
    // violet — MENU13 moved it off the button to the bar's trailing
    // edge, where the tor palette lift keeps it legible.
    QToolBar *torNavBar = torWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QLabel *torBrand = torWindow->findChild<QLabel *>(
        QLatin1String("navBarBrandLabel"));
    int lightPixels = 0;
    if (torBrand) {
        const QImage grab = torBrand->grab().toImage();
        for (int y = 0; y < grab.height(); ++y)
            for (int x = 0; x < grab.width(); ++x) {
                const QColor color = grab.pixelColor(x, y);
                if (color.alpha() > 200 && color.lightness() > 180)
                    ++lightPixels;
            }
    }
    // The wordmark is still the trailing action — the tor badge
    // slots in ahead of it like every other movable item.
    const bool torBrandLast = torNavBar && torBrand
        && torNavBar->widgetForAction(torNavBar->actions().last())
               == torBrand;
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
    QVERIFY(torBigIcon);
    QCOMPARE(torMenuFont, menuFont);
    QVERIFY(torRowVisible);
    QVERIFY(torRowBare);
    QCOMPARE(torMenuCount, 13);
    QVERIFY(torPrefsLeaf);
    QVERIFY(torQuitLast);
    QVERIFY(lightPixels > 0);
    QVERIFY(torBrandLast);
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
    QVERIFY(margins.left() >= 4 && margins.right() >= 4);
    QVERIFY(navBar->layout() && navBar->layout()->spacing() >= 4);
    // MENU10: the widget's vertical margins are zero now — stacking
    // them on the layout's own PM_ToolBar padding is what pinned the
    // row high against the grown app-menu button.  The breathing
    // room contract survives, sourced from the toolbar layout's
    // margins, so assert the effective padding instead.
    const QMargins layoutMargins = navBar->layout()->contentsMargins();
    QVERIFY(margins.top() + layoutMargins.top() >= 4);
    QVERIFY(margins.bottom() + layoutMargins.bottom() >= 4);

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
    // The TABS19 thickness cap — Firefox's ~33px face (content +
    // 17) plus a few px of stylesheet border/frame headroom.
    const int cap = qMax(bar->fontMetrics().height(),
                         bar->iconSize().height()) + 21;
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
    // TABS15: the strip's own '+' button docks right after the last
    // tab — sample past its footprint so the pixel is bare violet
    // chrome, not the tinted glyph.
    const QColor stripColor = image.pixelColor(bar->mapTo(
        window, QPoint(qMin(tabRect.right() + 40, bar->width() - 4),
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

// ICONS04/ICONS06: the tor window's violet chrome sits on the light
// application palette, so the nav cluster's themed icons resolved
// dark-on-dark — and worse, "native" (the default pick) has no -dark
// art to pin at all, while Qt's generated Disabled ghosting made the
// idle arrows read LIGHTER than the enabled reload.  Tor windows now
// tint every nav glyph flat #eee with a seeded dimmer Disabled mode;
// menu actions keep the normal icons because their popups stay on
// the platform palette.
void tst_BrowserMainWindow::torNavIconsDarkChrome()
{
    const auto luminance = [](const QIcon &icon,
                              QIcon::Mode mode = QIcon::Normal)
            -> qlonglong {
        const QImage image =
            icon.pixmap(QSize(18, 18), mode).toImage();
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
    const auto opaqueCount = [](const QIcon &icon, int extent) -> int {
        const QImage image =
            icon.pixmap(QSize(extent, extent)).toImage();
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x)
                if (image.pixelColor(x, y).alpha() > 60)
                    ++count;
        return count;
    };

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
    QToolButton *plainMenuButton = plain->findChild<QToolButton *>(
        QLatin1String("appMenuButton"));
    QVERIFY(plainMenuButton);
    const int plainLogoCoverage =
        opaqueCount(plainMenuButton->icon(), 36);
    closeWindow(plain);

    // A tor window's nav-icon measurements — run under each theme so
    // the tint is proven theme-independent.
    struct TorNavLum {
        qlonglong back = -1;
        qlonglong backDisabled = -1;
        qlonglong forward = -1;
        qlonglong reload = -1;
        qlonglong home = -1;
        qlonglong sidebar = -1;
        qlonglong menuReload = -1;
        int logoCoverage = -1;
        bool barFound = false;
    };
    const auto measureTorBar = [&](const char *grabName) -> TorNavLum {
        TorNavLum out;
        BrowserApplication::setTorMode(true);
        SubWindow *window = new SubWindow;
        window->resize(1000, 600);
        window->show();
        QApplication::processEvents();
        QToolBar *navBar = window->findChild<QToolBar *>(
            QLatin1String("NavigationToolBar"));
        if (navBar && navBar->actions().count() >= 5) {
            out.barFound = true;
            out.back = luminance(navBar->actions().at(1)->icon());
            out.backDisabled = luminance(navBar->actions().at(1)->icon(),
                                         QIcon::Disabled);
            out.forward = luminance(navBar->actions().at(2)->icon());
            out.reload = luminance(navBar->actions().at(3)->icon());
            // HOME01: the Home button sits at index 4.
            out.home = luminance(navBar->actions().at(4)->icon());
            for (QAction *action : navBar->actions())
                if (action->shortcut() == QKeySequence(Qt::Key_F4))
                    out.sidebar = luminance(action->icon());
            QToolButton *menuButton = window->findChild<QToolButton *>(
                QLatin1String("appMenuButton"));
            if (menuButton)
                out.logoCoverage =
                    opaqueCount(menuButton->icon(), 36);
            // MENU06: a tor window's menubar row is the bare title
            // bar — the menus live in the app-menu dropdown (same
            // shared QMenu objects).
            for (QAction *menuAction : window->appMenu()->actions()) {
                if (!menuAction->menu()
                    || !menuAction->text().contains(
                           QLatin1String("View")))
                    continue;
                for (QAction *action : menuAction->menu()->actions())
                    if (action->text().remove(QLatin1Char('&'))
                            == QLatin1String("Reload Page"))
                        out.menuReload = luminance(action->icon());
            }
            // Offscreen grab for visual review — set
            // ARORA_TOR_GRAB_DIR and the tor nav bars land there as
            // tor-nav-icons-<theme>.png.
            const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
            if (grabName && !grabDir.isEmpty()) {
                QDir().mkpath(QString::fromLocal8Bit(grabDir));
                navBar->grab().save(
                    QString::fromLocal8Bit(grabDir)
                    + QLatin1String("/tor-nav-icons-")
                    + QString::fromLatin1(grabName)
                    + QLatin1String(".png"));
            }
        }
        closeWindow(window);
        BrowserApplication::setTorMode(false);
        return out;
    };

    const TorNavLum bundled = measureTorBar("adwaita");

    // ICONS06: the reported path — "native" has no -dark art for
    // getOnDark to pin, so the tint must carry the lift alone.
    // ICONS10: set explicitly — the default pick is now "tabler".
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("native"));
    AroraIcon::applyFromSettings();
    const TorNavLum nativeTheme = measureTorBar("native");

    BrowserTheme::applyColorScheme();
    if (savedTheme.isValid())
        settings.setValue(QLatin1String("MainWindow/iconTheme"),
                          savedTheme);
    AroraIcon::applyFromSettings();

    // Light-palette normal window: dark glyphs.  Tor window: light
    // glyphs — readable on the violet bar under any theme.
    QVERIFY(plainBack >= 0 && plainBack < 140);
    QVERIFY(plainReload >= 0 && plainReload < 140);
    for (const TorNavLum &lum : { bundled, nativeTheme }) {
        QVERIFY(lum.barFound);
        QVERIFY(lum.back > 140);
        QVERIFY(lum.forward > 140);
        QVERIFY(lum.reload > 140);
        QVERIFY(lum.home > 140);
        QVERIFY(lum.sidebar > 140);
        // ICONS06: Disabled is seeded, not generated — deliberately
        // dimmer than Normal yet still clearly light on violet.
        QVERIFY(lum.backDisabled > 140
                && lum.backDisabled < lum.back);
        // The brand mark keeps its colors but gains a light backing
        // disc — strictly more coverage than the plain window's.
        QVERIFY(lum.logoCoverage > plainLogoCoverage);
    }
    QVERIFY(bundled.menuReload >= 0 && bundled.menuReload < 140);
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

    // Force the "native" icon theme — that is the path the bug
    // report hit (a bundled pick already flipped to -dark; ICONS10
    // made bundled the default, so native is pinned explicitly).
    QSettings settings;
    const QVariant savedTheme =
        settings.value(QLatin1String("MainWindow/iconTheme"));
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("native"));
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

    // TOR15: the chain node model — Tor-Browser-style
    // "Country (role)" titles with OR-address and abbreviated
    // fingerprint detail lines.
    QCOMPARE(TorCircuitPanel::roleName(0, 3), QLatin1String("guard"));
    QCOMPARE(TorCircuitPanel::roleName(1, 3), QLatin1String("middle"));
    QCOMPARE(TorCircuitPanel::roleName(2, 3), QLatin1String("exit"));
    QCOMPARE(TorCircuitPanel::roleName(1, 4), QLatin1String("hop 2"));
    QCOMPARE(TorCircuitPanel::shortFingerprint(
                 QStringLiteral("AAAABBBBCCCCDDDDEEEEFFFF0000111122223333")),
             QStringLiteral("AAAABBBB…22223333"));

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

    const QList<TorCircuitPanel::Node> nodes =
        TorCircuitPanel::nodesForCircuit(
            circuit, QLatin1String("example.com"));
    QCOMPARE(nodes.count(), 5);           // browser + 3 hops + site
    QCOMPARE(nodes.at(0).title, QStringLiteral("This browser"));
    QCOMPARE(nodes.at(1).title,
             QStringLiteral("United Kingdom (guard)"));
    QCOMPARE(nodes.at(2).title, QStringLiteral("Germany (middle)"));
    QCOMPARE(nodes.at(3).title, QStringLiteral("Austria (exit)"));
    QCOMPARE(nodes.at(4).title, QStringLiteral("example.com"));
    QCOMPARE(nodes.at(1).kind, TorCircuitPanel::Node::Hop);
    QVERIFY(nodes.at(1).details.contains(
        QStringLiteral("1.2.3.4 — ElToro")));
    QVERIFY(nodes.at(1).details.contains(
        QStringLiteral("AAAABBBB…22223333")));
    QCOMPARE(nodes.at(1).countryCode, QLatin1String("GB"));
    // A hop without geoip/address still renders honestly.
    const TorCircuitPanel::Node bare =
        TorCircuitPanel::hopNode(TorCircuitHop(), 1, 3);
    QCOMPARE(bare.title, QStringLiteral("Unknown (middle)"));
    QVERIFY(bare.details.isEmpty());

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
    // panel reports the missing control connection.
    const bool greyed = button && !button->isEnabled();
    QMenu *dropMenu = button ? button->menu() : nullptr;
    TorCircuitPanel *dropPanel = dropMenu
        ? dropMenu->findChild<TorCircuitPanel *>(
              QLatin1String("torCircuitPanel"))
        : nullptr;
    QStringList stateTexts;
    if (dropPanel
            && QMetaObject::invokeMethod(button, "populateMenu")) {
        stateTexts = dropPanel->contentLines();
    }

    // Offscreen grabs for visual review — set ARORA_TOR_GRAB_DIR to a
    // directory and the nav bar, flag glyphs and a fabricated circuit
    // panel land there.
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
        TorCircuitPanel demo;
        demo.setCircuit(QLatin1String("example.com"), nodes);
        demo.setNewCircuitEnabled(true);
        demo.resize(demo.sizeHint());
        demo.grab().save(dir + QLatin1String("/circuit-menu.png"));
    }

    closeWindow(window);
    BrowserApplication::setTorMode(false);

    QVERIFY(button);
    QVERIFY(dropMenu);
    QVERIFY(dropPanel);
    // MENU06: the app-menu button leads the bar — the nav cluster
    // shifts one slot right.  HOME01 adds Home between reload and the
    // reader button.
    QCOMPARE(readerIndex, 5);             // app menu, back, forward, reload, home, reader
    QCOMPARE(circuitIndex, 6);            // onion right next to it
    QVERIFY(greyed);
    QVERIFY(stateTexts.join(QLatin1Char('\n')).contains(
        QLatin1String("unavailable"), Qt::CaseInsensitive));
}

// TOR19: the first click on the circuit button must open the popup at
// the populated size, not the stale rect.  QToolButton's InstantPopup
// path queries menu->sizeHint() BEFORE aboutToShow to position the
// popup — that call resolves the action rect from whatever the panel
// last rendered, and QMenu::popup re-runs its layout pass only when
// an item changed since.  Replicated here by calling sizeHint()
// before popup(); the shown panel's geometry must match its live
// sizeHint in both the grow and shrink directions.
void tst_BrowserMainWindow::torCircuitPopupFirstShow()
{
    TorCircuitButton button(nullptr);
    QMenu *menu = button.menu();
    QVERIFY(menu);
    TorCircuitPanel *panel = menu->findChild<TorCircuitPanel *>(
        QLatin1String("torCircuitPanel"));
    QVERIFY(panel);

    // Grow leg: the site-host provider widens the panel at populate
    // time — the stale flush measured the narrower render.
    button.setSiteHostProvider([]() {
        return QStringLiteral(
            "a-very-long-hostname-for-width-check.example.com");
    });
    menu->sizeHint();               // the tool button's pre-popup flush
    menu->popup(QPoint(80, 80));
    QTRY_VERIFY_WITH_TIMEOUT(menu->isVisible(), 5000);
    QCOMPARE(panel->size(), panel->sizeHint());
    QVERIFY(panel->contentLines().first().contains(
        QLatin1String("a-very-long-hostname-for-width-check")));
    const QSize firstShowSize = menu->size();

    // Async content while open: the NEWNYM feedback line lands and
    // the popup grows to fit it (TOR16).
    QVERIFY(QMetaObject::invokeMethod(&button, "onNewCircuit"));
    QTRY_VERIFY_WITH_TIMEOUT(menu->size() != firstShowSize, 5000);
    QCOMPARE(panel->size(), panel->sizeHint());
    menu->hide();

    // Shrink leg: reopening clears the lingering status line — the
    // stale flush measured the taller render.  The re-dirty on
    // populate makes the shown rect match the cleared panel, back to
    // the first-show size: a repeat open is identical.
    menu->sizeHint();
    menu->popup(QPoint(80, 80));
    QTRY_VERIFY_WITH_TIMEOUT(menu->isVisible(), 5000);
    QCOMPARE(panel->size(), panel->sizeHint());
    QCOMPARE(menu->size(), firstShowSize);
    menu->hide();
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

// SPAGE01: every start-page variant keeps its page gradient on a
// viewport-height <html>.  A <body> background propagates to the
// canvas sized to the body box and tiles down the window — the
// horizontal bands the user reported.  Computed style is the pin:
// documentElement carries the gradient at min-height:100% while the
// body paints no background image of its own.
void tst_BrowserMainWindow::startPageSingleSurface()
{
    static const char *pages[] = {
        "qrc:/startpage.html",
        "qrc:/startpage-private.html",
        "qrc:/startpage-tor.html",
        "qrc:/startpage-servo.html",
    };
    const QString probe = QStringLiteral(
        "(function(){"
        "var h=getComputedStyle(document.documentElement);"
        "var b=getComputedStyle(document.body);"
        "return (h.backgroundImage.indexOf('gradient')>=0?'G':'-')+'|'"
        "+(b.backgroundImage==='none'?'b':'-')+'|'"
        "+((h.minHeight==='100%'||"
        "parseFloat(h.minHeight)>=innerHeight)?'m':'-');})()");

    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();
    WebView *tab = window->currentTab();
    QVERIFY(tab);

    QStringList results;
    for (const char *page : pages) {
        tab->loadUrl(QUrl(QString::fromLatin1(page)));
        QString dom;
        QElapsedTimer timer;
        timer.start();
        while (dom != QLatin1String("G|b|m") && timer.elapsed() < 10000) {
            bool jsDone = false;
            tab->enginePage()->runJavaScript(probe,
                [&jsDone, &dom](const QVariant &v) {
                    dom = v.toString();
                    jsDone = true;
                });
            for (int waited = 0; !jsDone && waited < 5000; waited += 50)
                QTest::qWait(50);
        }
        results << QString::fromLatin1("%1=%2")
                       .arg(QLatin1String(page), dom);
    }

    closeWindow(window);
    QCOMPARE(results, QStringList() << QStringLiteral(
        "qrc:/startpage.html=G|b|m")
        << QStringLiteral("qrc:/startpage-private.html=G|b|m")
        << QStringLiteral("qrc:/startpage-tor.html=G|b|m")
        << QStringLiteral("qrc:/startpage-servo.html=G|b|m"));
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
    // button leads, then Back, Forward, Stop/Reload, the HOME01 Home
    // button, the reader button, then the location-bar splitter.
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
    QCOMPARE(readerIndex, 5);
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

// READ03: user report — reader intermittently showed only the page
// title, no body.  Each data row is one corpus leg (one window each —
// the legs stay isolated and independently timed):
//
//   /form    — sanitizer over-strip.  Readability's conditional cleaner
//              keeps a text-heavy <form>, then the old sanitize()
//              deleted the node WITH its children -> title-only
//              overlay.  The wrapper is now unwrapped so the text
//              survives.
//   /thin    — degenerate extraction.  The body text lives in an
//              <aside>, which Readability's _prepArticle cleans
//              unconditionally; parse() still returns an article (its
//              last-ditch longest-attempt fallback), heading-only.
//              The thin-content guard refuses honestly instead of
//              showing a header over an empty column.
//   /danger  — sanitizer regression: real text renders while script,
//              on*-attributes and javascript: URLs stay stripped.
//   /tamper  — READ05: the page main-world-redefines __aroraReader,
//              Readability and isProbablyReaderable to fake a refusal
//              and a "PWNED" article.  Extraction runs in an isolated
//              world, so the real article still renders.
//   /hostile — READ05: mutation-XSS-shaped payloads (entity-encoded
//              and whitespace-split javascript: URLs, on* handlers,
//              script/svg/iframe/srcdoc injection) inside otherwise
//              good article markup — the C++ sanitizer strips them
//              and the sandboxed frame would not run them anyway.
void tst_BrowserMainWindow::readerModeContent_data()
{
    QTest::addColumn<QByteArray>("path");
    QTest::addColumn<bool>("expectActive");
    QTest::addColumn<bool>("expectMessage");
    QTest::addColumn<QStringList>("mustContain");
    QTest::addColumn<QStringList>("mustNotContain");

    QTest::newRow("form")
        << QByteArray("/form") << true << false
        << (QStringList() << QStringLiteral("Reader corpus paragraph"))
        << (QStringList() << QStringLiteral("<form"));
    QTest::newRow("thin")
        << QByteArray("/thin") << false << true << QStringList()
        << QStringList();
    QTest::newRow("danger")
        << QByteArray("/danger") << true << false
        << (QStringList() << QStringLiteral("Reader corpus paragraph"))
        << (QStringList() << QStringLiteral("<script")
            << QStringLiteral("onerror")
            << QStringLiteral("javascript:"));
    QTest::newRow("tamper")
        << QByteArray("/tamper") << true << false
        << (QStringList() << QStringLiteral("Reader corpus paragraph"))
        << (QStringList() << QStringLiteral("PWNED"));
    QTest::newRow("hostile")
        << QByteArray("/hostile") << true << false
        << (QStringList() << QStringLiteral("Reader corpus paragraph"))
        << (QStringList() << QStringLiteral("<script")
            << QStringLiteral("javascript:"));
}

static QHash<QByteArray, QByteArray> readerFixturePages()
{
    const QByteArray paragraph =
        QByteArray("Reader corpus paragraph. The quick brown fox jumps "
                   "over the lazy dog while a sentence of plain article "
                   "copy carries enough weight to look like body text. ")
            .repeated(2);
    QByteArray paragraphs;
    for (int i = 0; i < 6; ++i)
        paragraphs += "<p>" + paragraph + "</p>";

    QHash<QByteArray, QByteArray> pages;
    pages["/form"] = "<html><head><title>Form Article</title></head>"
        "<body><article><form><h1>Form Article</h1>" + paragraphs
        + "</form></article></body></html>";
    pages["/thin"] = "<html><head><title>Thin Article</title></head>"
        "<body><article><h1>Thin Article</h1><aside>" + paragraphs
        + "</aside></article></body></html>";
    // The payloads write a flag rather than alert(): a modal JS alert
    // pauses the renderer's world, and the driver's runJavaScript would
    // never return — the test itself would wedge before asserting.
    pages["/danger"] = "<html><head><title>Danger Article</title>"
        "<script>window.__pwned=1</script>"
        "</head><body><article><h1>Danger Article</h1>" + paragraphs
        + "<p>keep <a href=\"javascript:window.__pwned=2\">link</a></p>"
        "<img src=\"about:blank\" onerror=\"window.__pwned=3\">"
        "</article></body></html>";
    // READ05: main-world forgeries of everything the driver used to
    // define — a refusal __aroraReader, a fake Readability that emits
    // a PWNED article, a readerable helper that lies.  None of it may
    // matter: the pipeline runs in a world the page cannot see.
    pages["/tamper"] = "<html><head><title>Tamper Article</title>"
        "<script>"
        "window.__aroraReader={probe:function(){return false},"
        "enter:function(){return {ok:false}}};"
        "window.isProbablyReaderable=function(){return false};"
        "window.Readability=function(){return{parse:function(){"
        "return {title:'PWNED',content:'<p>PWNED</p>'}}}};"
        "</script>"
        "</head><body><article><h1>Tamper Article</h1>" + paragraphs
        + "</article></body></html>";
    // READ05: hostile markup inside an otherwise-good article.  Each
    // payload is a different bypass shape — raw script, an onerror
    // handler, entity-encoded and whitespace-split javascript: hrefs,
    // an svg script, a srcdoc iframe, a noscript bomb — and the
    // C++-side filter must drop every one of them.  The flags write
    // to the frame's window so a leak is observable from either side.
    pages["/hostile"] = "<html><head><title>Hostile Article</title>"
        "</head><body><article><h1>Hostile Article</h1>" + paragraphs
        + "<script>window.__pwned=1</script>"
        "<p onclick=\"window.__pwned=2\">onclick body</p>"
        "<img src=\"broken-img\" onerror=\"window.__pwned=3\">"
        "<p><a href=\"javascript:window.__pwned=4\">raw js link</a></p>"
        "<p><a href=\"java&#115;cript:window.__pwned=5\">ent js</a></p>"
        "<p><a href=\"&#x6a;avascript:window.__pwned=6\">num js</a></p>"
        "<p><a href=\"java&Tab;script:window.__pwned=7\">tab js</a></p>"
        "<p><a href=\"data:text/html,<script>window.__pwned=8</script>\">"
        "data link</a></p>"
        "<svg><script>window.__pwned=9</script><circle r=\"4\"/></svg>"
        "<iframe srcdoc=\"<script>window.__pwned=10</script>\"></iframe>"
        "<noscript><img src=\"n\" onerror=\"window.__pwned=11\">"
        "</noscript>"
        "<math><mtext><table><mglyph><style><!--</style>"
        "<img title=\"--><img src=1 onerror=window.__pwned=12>\">"
        "</mglyph></table></mtext></math>"
        + paragraphs + "</article></body></html>";
    return pages;
}

void tst_BrowserMainWindow::readerModeContent()
{
    QFETCH(QByteArray, path);
    QFETCH(bool, expectActive);
    QFETCH(bool, expectMessage);
    QFETCH(QStringList, mustContain);
    QFETCH(QStringList, mustNotContain);

    const QHash<QByteArray, QByteArray> pages = readerFixturePages();
    QVERIFY(pages.contains(path));

    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();

    WebView *view = window->currentTab();
    QVERIFY(view);
    ReaderMode *reader = view->readerMode();
    QVERIFY(reader);
    QSignalSpy messageSpy(reader, &ReaderMode::message);

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server, &pages]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        QByteArray *request = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [socket, request, &pages]() {
            request->append(socket->readAll());
            const int lineEnd = request->indexOf("\r\n");
            if (lineEnd < 0)
                return;
            const QByteArray line = request->left(lineEnd);
            const int firstSpace = line.indexOf(' ');
            const QByteArray path = line.mid(
                firstSpace + 1,
                line.indexOf(' ', firstSpace + 1) - firstSpace - 1);
            const QByteArray body =
                pages.value(path, QByteArray("not found"));
            socket->write(QByteArray("HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/html\r\n"
                                     "Connection: close\r\n"
                                     "Content-Length: ")
                          + QByteArray::number(body.size())
                          + "\r\n\r\n" + body);
            socket->disconnectFromHost();
            delete request;
        });
    });

    const auto evalJs = [&](const QString &script) -> QVariant {
        // Heap state — a callback that lands after the wait window must
        // not scribble on this frame.
        auto state = std::make_shared<QPair<QVariant, bool>>(
            QVariant(), false);
        view->enginePage()->runJavaScript(
            script, [state](const QVariant &v) {
                state->first = v;
                state->second = true;
            });
        for (int waited = 0; !state->second && waited < 8000;
             waited += 50)
            QTest::qWait(50);
        return state->first;
    };

    view->loadUrl(QUrl(QStringLiteral("http://127.0.0.1:%1%2")
                       .arg(server.serverPort())
                       .arg(QString::fromLatin1(path))));
    QTRY_VERIFY_WITH_TIMEOUT(!view->page()->isLoading(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(reader->isAvailable(), 20000);

    reader->enter();
    QTRY_VERIFY_WITH_TIMEOUT(
        reader->isActive() || !messageSpy.isEmpty(), 20000);

    QCOMPARE(reader->isActive(), expectActive);
    QCOMPARE(!messageSpy.isEmpty(), expectMessage);

    // READ05: the article lives in the overlay's sandboxed iframe —
    // allow-same-origin keeps it readable here.  The srcdoc parses
    // asynchronously, so the read polls until it lands.
    const auto readerContent = [&]() -> QString {
        return evalJs(QStringLiteral(
            "(function(){"
            "var h=document.getElementById('arora-reader-host');"
            "var f=h&&h.shadowRoot"
            "?h.shadowRoot.querySelector('iframe.frame'):null;"
            "var d=f?f.contentDocument:null;"
            "var c=d?d.querySelector('.content'):null;"
            "return c?c.innerHTML:'';"
            "})()")).toString();
    };
    QString content = readerContent();
    if (expectActive)
        QTRY_VERIFY_WITH_TIMEOUT(
            !(content = readerContent()).isEmpty(), 10000);

    for (const QString &needle : mustContain)
        QVERIFY2(content.contains(needle),
                 qPrintable(QLatin1String("missing: ") + needle));
    for (const QString &needle : mustNotContain)
        QVERIFY2(!content.contains(needle, Qt::CaseInsensitive),
                 qPrintable(QLatin1String("survived: ") + needle));
    if (expectActive)
        QVERIFY(!content.isEmpty());
    else
        QVERIFY(content.isEmpty());

    if (path == QByteArray("/tamper") && expectActive) {
        // The page's forgeries stay live in the main world — they
        // simply never mattered.  This also proves the leg is not
        // vacuous: the fake probe really answers false there.
        QCOMPARE(evalJs(QStringLiteral(
            "window.__aroraReader.probe()")).toBool(), false);
        QCOMPARE(evalJs(QStringLiteral(
            "new Readability(document).parse().title"))
            .toString(), QLatin1String("PWNED"));
    }

    if (path == QByteArray("/hostile") && expectActive) {
        // The fixture's payloads do fire on the served page itself —
        // they are live page markup, that is the point — so the flag
        // that proves reader-content inertness is the SANDBOXED
        // FRAME's window, where the extracted markup renders.
        QCOMPARE(evalJs(QStringLiteral(
            "(function(){"
            "var h=document.getElementById('arora-reader-host');"
            "var f=h.shadowRoot.querySelector('iframe.frame');"
            "return String(f.contentWindow.__pwned);"
            "})()")), QLatin1String("undefined"));

        // The sandbox itself: even markup pushed straight into the
        // frame document — bypassing the sanitizer entirely — cannot
        // run.  allow-same-origin lets the test write it;
        // no-allow-scripts keeps it inert.
        QCOMPARE(evalJs(QStringLiteral(
            "(function(){"
            "var h=document.getElementById('arora-reader-host');"
            "var f=h.shadowRoot.querySelector('iframe.frame');"
            "var d=f.contentDocument;"
            "d.body.innerHTML='<img src=x onerror=\"window.__pwned=42\">'"
            "+'<img src=y onload=\"window.__pwned=43\">';"
            "var s=d.createElement('scr'+'ipt');"
            "s.textContent='window.__pwned=44';"
            "d.body.appendChild(s);"
            "return String(f.contentWindow.__pwned);"
            "})()")), QLatin1String("undefined"));
    }

    closeWindow(window);
}

// READ05: the C++ sanitizer's contract — exercised directly so edge
// cases do not each need a served fixture page.
void tst_BrowserMainWindow::readerSanitizer()
{
    // Transparent wrappers keep their children, with the tag gone.
    {
        const ReaderSanitizer::Result r = ReaderSanitizer::sanitize(
            QStringLiteral("<form><p>kept text</p></form>"
                           "<noscript><b>also kept</b></noscript>"));
        QVERIFY(r.html.contains(QLatin1String("kept text")));
        QVERIFY(r.html.contains(QLatin1String("also kept")));
        QVERIFY(!r.html.contains(QLatin1String("<form")));
        QVERIFY(!r.html.contains(QLatin1String("<noscript")));
        QVERIFY(r.textLength > 0);
    }

    // Dead elements go out with their contents.
    {
        const ReaderSanitizer::Result r = ReaderSanitizer::sanitize(
            QStringLiteral("<p>text</p><script>alert(1)</script>"
                           "<style>a{b:c}</style>"
                           "<iframe src=x><p>inner</p></iframe>"
                           "<object><p>obj</p></object>"
                           "<input value=x><select><option>o</select>"
                           "<textarea>t</textarea><template>t2</template>"));
        QCOMPARE(r.html, QLatin1String("<p>text</p>"));
    }

    // Event handlers and dangerous URL schemes are scrubbed;
    // ordinary links, entities and data images survive.
    {
        const ReaderSanitizer::Result r = ReaderSanitizer::sanitize(
            QStringLiteral(
                "<p onclick=\"f()\" onmouseover='g()'>t</p>"
                "<a href=\"javascript:x()\">j</a>"
                "<a href=\"java&#115;cript:x()\">e</a>"
                "<a href=\"&#x6a;avascript:x()\">n</a>"
                "<a href=\"java&Tab;script:x()\">w</a>"
                "<a href=\" javascript:x()\">s</a>"
                "<a href=\"data:text/html,<b>x\">d</a>"
                "<a href=javascript:x()>u</a>"
                "<a href=\"https://ok.example/p?a=1&amp;b=2\">ok</a>"
                "<img src=\"data:image/png;base64,AAAA\">"
                "<img src=\"x.png\" onerror=\"f()\">"
                "<a ping=\"http://t\" href=\"/y\">p</a>"));
        QVERIFY(!r.html.contains(QLatin1String("onclick")));
        QVERIFY(!r.html.contains(QLatin1String("onmouseover")));
        QVERIFY(!r.html.contains(QLatin1String("onerror")));
        QVERIFY(!r.html.contains(QLatin1String("javascript:"),
                                        Qt::CaseInsensitive));
        QVERIFY(!r.html.contains(QLatin1String("&#115;cript")));
        QVERIFY(!r.html.contains(QLatin1String("&#x6a")));
        QVERIFY(!r.html.contains(QLatin1String("data:text/html")));
        QVERIFY(!r.html.contains(QLatin1String("ping=")));
        // The kept link keeps its attribute and entity.
        QVERIFY(r.html.contains(
            QLatin1String("href=\"https://ok.example/p?a=1&amp;b=2\"")));
        // data: is fine on resource URLs.
        QVERIFY(r.html.contains(
            QLatin1String("src=\"data:image/png;base64,AAAA\"")));
    }

    // Malformed and mutation-shaped input cannot break out.
    {
        const ReaderSanitizer::Result r = ReaderSanitizer::sanitize(
            QStringLiteral(
                "<p>one<svg><script>alert(1)</script><circle r=4/>"
                "</svg><plaintext>gone<script>alert(2)</script>"));
        QVERIFY(r.html.contains(QLatin1String("one")));
        QVERIFY(!r.html.contains(QLatin1String("<script")));
        QVERIFY(!r.html.contains(QLatin1String("alert")));
        QVERIFY(!r.html.contains(QLatin1String("gone")));
    }

    // The thin-content floor inputs: text length is decoded text
    // only; kept media flips hasMedia.
    {
        const ReaderSanitizer::Result text = ReaderSanitizer::sanitize(
            QStringLiteral("<p>hello &amp; bye</p>"));
        QCOMPARE(text.textLength, 11);
        QVERIFY(!text.hasMedia);
        const ReaderSanitizer::Result media =
            ReaderSanitizer::sanitize(QStringLiteral("<p>x</p>"
                                                   "<img src=i.png>"));
        QVERIFY(media.hasMedia);
    }
}

// HOME01: the nav bar carries a Home button immediately after
// Stop/Reload.  It IS the History menu's Home action (Ctrl+Shift+H),
// so button, menu entry and shortcut are one path — click or trigger
// loads the context-appropriate start page in the current tab on
// normal, private and tor windows.
void tst_BrowserMainWindow::homeButton()
{
    const auto homeActionFor = [](QToolBar *bar) -> QAction * {
        const QList<QAction *> actions = bar->actions();
        for (QAction *action : actions) {
            if (action->shortcut()
                == QKeySequence(Qt::ControlModifier | Qt::ShiftModifier
                                | Qt::Key_H))
                return action;
        }
        return nullptr;
    };

    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    QAction *home = homeActionFor(navBar);
    QVERIFY(home);
    // Slot order: app-menu, Back, Forward, Stop/Reload, then Home —
    // directly beside reload, ahead of the reader button + url bar.
    QCOMPARE(navBar->actions().indexOf(home), 4);
    QToolButton *homeButton = qobject_cast<QToolButton *>(
        navBar->widgetForAction(home));
    QVERIFY(homeButton);
    QVERIFY(homeButton->autoRaise());
    QVERIFY(!home->icon().isNull());
    QVERIFY(home->toolTip().contains(QLatin1String("Home")));
    QVERIFY(home->toolTip().contains(QLatin1String("Ctrl+Shift+H")));

    // The History menu entry is the same QAction object — no parallel
    // code path between button, menu and shortcut.
    bool inMenu = false;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
            && menuAction->menu()->actions().contains(home))
            inMenu = true;
    }
    QVERIFY(inMenu);

    // Normal tab: click loads the standard start page in place.
    WebView *view = window->currentTab();
    QVERIFY(view);
    QTest::mouseClick(homeButton, Qt::LeftButton);
    QTRY_VERIFY(view->url()
                == QUrl(QLatin1String("qrc:/startpage.html"))
                && view->page() && !view->page()->isLoading());

    // The action's own trigger (the menu/shortcut path) does the same.
    view->loadUrl(QUrl(QLatin1String("about:blank")));
    QTRY_VERIFY(view->url() == QUrl(QLatin1String("about:blank")));
    home->trigger();
    QTRY_VERIFY(view->url()
                == QUrl(QLatin1String("qrc:/startpage.html"))
                && !view->page()->isLoading());

    // A private tab in the same window resolves the private variant.
    window->tabWidget()->newPrivateTab();
    WebView *privTab = window->currentTab();
    QVERIFY(privTab && privTab != view);
    privTab->loadUrl(QUrl(QLatin1String("about:blank")));
    QTRY_VERIFY(privTab->url() == QUrl(QLatin1String("about:blank")));
    QTest::mouseClick(homeButton, Qt::LeftButton);
    QTRY_VERIFY(privTab->url()
                == QUrl(QLatin1String("qrc:/startpage-private.html"))
                && privTab->page() && !privTab->page()->isLoading());
    closeWindow(window);

    // Tor window: same button, tor start page.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->resize(1000, 600);
    torWindow->show();
    QApplication::processEvents();
    QToolBar *torBar = torWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(torBar);
    QAction *torHome = homeActionFor(torBar);
    QVERIFY(torHome);
    QCOMPARE(torBar->actions().indexOf(torHome), 4);
    QToolButton *torHomeButton = qobject_cast<QToolButton *>(
        torBar->widgetForAction(torHome));
    QVERIFY(torHomeButton);
    WebView *torTab = torWindow->currentTab();
    QVERIFY(torTab);
    // The first tab already shows the tor start page — move it off so
    // the click is observable.
    torTab->loadUrl(QUrl(QLatin1String("about:blank")));
    QTRY_VERIFY(torTab->url() == QUrl(QLatin1String("about:blank")));
    QTest::mouseClick(torHomeButton, Qt::LeftButton);
    QTRY_VERIFY(torTab->url()
                == QUrl(QLatin1String("qrc:/startpage-tor.html"))
                && torTab->page() && !torTab->page()->isLoading());
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);
}

// PREF04: the Homepage radio mode drives the Home action's target —
// Start Page lands on the branded start page, Specific Page on the
// stored url — and the startup Specific Pages list opens fully.
void tst_BrowserMainWindow::homePageModes()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/homePageMode"));
    settings.remove(QLatin1String("MainWindow/home"));
    settings.remove(QLatin1String("MainWindow/startupPages"));

    const auto writeFixture = [](const QString &name) -> QUrl {
        const QString path = QDir::temp().filePath(name);
        QFile fixture(path);
        if (!fixture.open(QIODevice::WriteOnly))
            return QUrl();
        fixture.write("<html><head><title>pref04</title>"
                      "</head><body>x</body></html>");
        return QUrl::fromLocalFile(path);
    };
    const QUrl homeFixture = writeFixture(
        QLatin1String("arora-pref04-home.html"));
    const QUrl secondFixture = writeFixture(
        QLatin1String("arora-pref04-second.html"));
    QVERIFY(homeFixture.isValid() && secondFixture.isValid());

    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();
    WebView *view = window->currentTab();
    QVERIFY(view);

    // Specific Page mode: the Home slot loads the stored url.
    settings.setValue(QLatin1String("MainWindow/homePageMode"),
                      int(BrowserApplication::SpecificPageHome));
    settings.setValue(QLatin1String("MainWindow/home"),
                      homeFixture.toString());
    window->goHome();
    QTRY_VERIFY(view->url() == homeFixture
                && view->page() && !view->page()->isLoading());

    // Start Page mode: the same slot lands on the branded start page
    // even with a specific url still stored.
    settings.setValue(QLatin1String("MainWindow/homePageMode"),
                      int(BrowserApplication::StartPageHome));
    window->goHome();
    QTRY_VERIFY(view->url()
                == QUrl(QLatin1String("qrc:/startpage.html"))
                && view->page() && !view->page()->isLoading());

    // The startup Specific Pages list opens fully — first entry in
    // the current tab, the rest as background tabs; an empty entry
    // can't wedge the list.
    settings.setValue(QLatin1String("MainWindow/startupPages"),
                      QStringList({homeFixture.toString(), QString(),
                                   secondFixture.toString()}));
    BrowserApplication::openStartupPages(window);
    QTRY_VERIFY(window->tabWidget()->count() == 2);
    QTRY_VERIFY(window->tabWidget()->webView(0)->url() == homeFixture
                && window->tabWidget()->webView(1)->url()
                       == secondFixture);

    // An empty list leaves the window's fresh tab alone.
    SubWindow *window2 = new SubWindow;
    window2->show();
    settings.setValue(QLatin1String("MainWindow/startupPages"),
                      QStringList());
    BrowserApplication::openStartupPages(window2);
    QCOMPARE(window2->tabWidget()->count(), 1);
    closeWindow(window2);

    settings.remove(QLatin1String("MainWindow/homePageMode"));
    settings.remove(QLatin1String("MainWindow/home"));
    settings.remove(QLatin1String("MainWindow/startupPages"));
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
        // THEM03: every panel, not just the current one — the rail
        // tabs swap pages lazily, so select each before grabbing.
        const int current = panel->tabs()->currentIndex();
        for (int i = 0; i < panel->tabs()->count(); ++i) {
            panel->tabs()->setCurrentIndex(i);
            qApp->processEvents();
            panel->grab().save(QString::fromLocal8Bit(sidebarGrabDir)
                + QStringLiteral("/sidebar-panel-%1.png")
                    .arg(panel->tabs()->tabText(i)));
        }
        panel->tabs()->setCurrentIndex(current);
        qApp->processEvents();
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

// SIDE09: the rail follows sidebar/panelOrder — written by the prefs
// ▲▼ buttons — applied live by applySidebarSettings (no rebuild),
// composed with per-panel visibility, and carried into a fresh
// window.  Ids the key doesn't mention append in registry order.
void tst_BrowserMainWindow::sidebarPanelOrder()
{
    QSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("sidebar/panelOrder"));
    settings.remove(QLatin1String("sidebar/panels"));

    settings.setValue(QLatin1String("MainWindow/showSidebar"), true);
    SubWindow *window = new SubWindow;
    window->show();
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QCOMPARE(panel->tabs()->count(), 4);
    QCOMPARE(panel->tabs()->tabText(0), QLatin1String("Bookmarks"));

    // A full reorder re-sequences the rail live.
    SidebarPanel::setPanelOrder(
        QStringList{ QLatin1String("notes"), QLatin1String("bookmarks"),
                     QLatin1String("history"),
                     QLatin1String("downloads") });
    window->applySidebarSettings();
    QCOMPARE(panel->tabs()->count(), 4);
    QCOMPARE(panel->tabs()->tabText(0), QLatin1String("Notes"));
    QCOMPARE(panel->tabs()->tabText(1), QLatin1String("Bookmarks"));
    QCOMPARE(panel->tabs()->tabText(3), QLatin1String("Downloads"));

    // A partial order keeps the named ids up front and appends the
    // rest in registry order.
    SidebarPanel::setPanelOrder(
        QStringList{ QLatin1String("downloads"),
                     QLatin1String("bookmarks") });
    window->applySidebarSettings();
    QCOMPARE(panel->tabs()->tabText(0), QLatin1String("Downloads"));
    QCOMPARE(panel->tabs()->tabText(1), QLatin1String("Bookmarks"));
    QCOMPARE(panel->tabs()->tabText(2), QLatin1String("History"));
    QCOMPARE(panel->tabs()->tabText(3), QLatin1String("Notes"));

    // Order and visibility compose — hiding the front entry pulls the
    // next one up, restoring lands it back in its ordered slot.
    SidebarPanel::setPanelVisible("downloads", false);
    window->applySidebarSettings();
    QCOMPARE(panel->tabs()->count(), 3);
    QCOMPARE(panel->tabs()->tabText(0), QLatin1String("Bookmarks"));
    SidebarPanel::setPanelVisible("downloads", true);
    window->applySidebarSettings();
    QCOMPARE(panel->tabs()->count(), 4);
    QCOMPARE(panel->tabs()->tabText(0), QLatin1String("Downloads"));

    // The order survives a fresh window's construction.
    SubWindow *window2 = new SubWindow;
    window2->show();
    QVERIFY(window2->sidebarPanel());
    QCOMPARE(window2->sidebarPanel()->tabs()->tabText(0),
             QLatin1String("Downloads"));
    QCOMPARE(window2->sidebarPanel()->tabs()->tabText(1),
             QLatin1String("Bookmarks"));
    closeWindow(window2);

    settings.remove(QLatin1String("sidebar/panelOrder"));
    settings.remove(QLatin1String("sidebar/panels"));
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    closeWindow(window);
}

// SIDE09: every option rendered in the prefs Sidebar group does
// something real — the rail's fold button, the close-button pair,
// focus-loss auto-close, lazy panel construction and bookmark folder
// counters all flip live through applySidebarSettings.
void tst_BrowserMainWindow::sidebarPanelOptions()
{
    QSettings settings;
    const char *const optionKeys[] = {
        "sidebar/showPanelToggle", "sidebar/showCloseButton",
        "sidebar/autoCloseInactive", "sidebar/folderCounters",
        "sidebar/lazyLoad"
    };
    for (const char *key : optionKeys)
        settings.remove(QLatin1String(key));
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("sidebar/panels"));
    settings.remove(QLatin1String("sidebar/panelOrder"));
    settings.setValue(QLatin1String("MainWindow/showSidebar"), true);

    SubWindow *window = new SubWindow;
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));
    window->activateWindow();
    QDockWidget *dock = window->sidebarDock();
    QTRY_VERIFY(dock->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);

    // 'Show Panel Toggle' — the fold button at the rail's foot hides
    // the dock like the page-header X.
    QToolButton *toggle = panel->findChild<QToolButton *>(
        QLatin1String("sidebarPanelToggle"));
    QVERIFY(toggle);
    QVERIFY(toggle->isVisible());
    toggle->click();
    QTRY_VERIFY(!dock->isVisible());
    dock->setVisible(true);
    QTRY_VERIFY(dock->isVisible());
    settings.setValue(QLatin1String("sidebar/showPanelToggle"), false);
    window->applySidebarSettings();
    QVERIFY(toggle->isHidden());
    settings.setValue(QLatin1String("sidebar/showPanelToggle"), true);
    window->applySidebarSettings();
    QVERIFY(toggle->isVisible());

    // 'Show Close Button' — the dock's title-bar X and the Downloads
    // header X share the switch.
    QVERIFY(dock->features() & QDockWidget::DockWidgetClosable);
    QWidget *headerX = panel->findChild<QWidget *>(
        QLatin1String("sidebarCloseButton"));
    QVERIFY(headerX);
    // The Downloads page isn't current, so assert the flag the option
    // drives (isHidden), not isVisible() which also needs a shown tab.
    QVERIFY(!headerX->isHidden());
    settings.setValue(QLatin1String("sidebar/showCloseButton"), false);
    window->applySidebarSettings();
    QVERIFY(!(dock->features() & QDockWidget::DockWidgetClosable));
    QVERIFY(headerX->isHidden());
    settings.setValue(QLatin1String("sidebar/showCloseButton"), true);
    window->applySidebarSettings();
    QVERIFY(dock->features() & QDockWidget::DockWidgetClosable);
    QVERIFY(!headerX->isHidden());

    // 'Auto-Close Inactive Panel' — focus inside the dock keeps it
    // up; focus landing on a window widget outside it folds it.
    settings.setValue(QLatin1String("sidebar/autoCloseInactive"), true);
    window->applySidebarSettings();
    QVERIFY(dock->isVisible());
    QVERIFY(QTest::qWaitForWindowActive(window));
    panel->tabs()->tabBar()->setFocus();
    QVERIFY(QApplication::focusWidget()
            && dock->isAncestorOf(QApplication::focusWidget()));
    QVERIFY(dock->isVisible());
    window->tabWidget()->currentLocationBar()->setFocus();
    QTRY_VERIFY(!dock->isVisible());
    settings.setValue(QLatin1String("sidebar/autoCloseInactive"), false);
    window->applySidebarSettings();
    dock->setVisible(true);
    QTRY_VERIFY(dock->isVisible());

    // 'Lazy Load' — the rail keeps its entries but the unopened pages
    // stay placeholders until selected; showDownloads materializes
    // its page on the way through.
    settings.setValue(QLatin1String("sidebar/lazyLoad"), true);
    SubWindow *lazyWindow = new SubWindow;
    lazyWindow->show();
    QVERIFY(lazyWindow->sidebarPanel());
    SidebarPanel *lazyPanel = lazyWindow->sidebarPanel();
    QCOMPARE(lazyPanel->tabs()->count(), 4);
    QVERIFY(!lazyPanel->findChild<QWidget *>(
                QLatin1String("sidebarDownloadsView")));
    lazyPanel->showDownloads();
    QVERIFY(lazyPanel->findChild<QWidget *>(
                QLatin1String("sidebarDownloadsView")));
    QCOMPARE(lazyPanel->tabs()->currentWidget(),
             static_cast<QWidget *>(lazyPanel->downloadsPage()));
    // Turning the option back off materializes every pending page.
    settings.setValue(QLatin1String("sidebar/lazyLoad"), false);
    lazyWindow->applySidebarSettings();
    QVERIFY(lazyPanel->findChild<QWidget *>(
                QLatin1String("sidebarHistoryView")));
    closeWindow(lazyWindow);

    // 'Show Folder Counters' — bookmark folders carry a " (n)"
    // suffix on the display text while the switch is on.
    QTreeView *bookmarksView = panel->findChild<QTreeView *>(
        QLatin1String("sidebarBookmarksView"));
    QVERIFY(bookmarksView);
    BookmarksManager *bookmarksManager = BookmarksManager::instance();
    BookmarkNode *folder = new BookmarkNode(BookmarkNode::Folder);
    folder->title = QLatin1String("Counted Folder");
    bookmarksManager->addBookmark(bookmarksManager->menu(), folder);
    BookmarkNode *childA = new BookmarkNode(BookmarkNode::Bookmark);
    childA->title = QLatin1String("child-a");
    childA->url = QLatin1String("https://example.com/a");
    bookmarksManager->addBookmark(folder, childA);
    BookmarkNode *childB = new BookmarkNode(BookmarkNode::Bookmark);
    childB->title = QLatin1String("child-b");
    childB->url = QLatin1String("https://example.com/b");
    bookmarksManager->addBookmark(folder, childB);
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
    const QModelIndex folderIndex =
        bmModel->index(bmModel->rowCount(menuIndex) - 1, 0, menuIndex);
    QVERIFY(folderIndex.isValid());
    QCOMPARE(folderIndex.data(Qt::DisplayRole).toString(),
             QLatin1String("Counted Folder (2)"));
    settings.setValue(QLatin1String("sidebar/folderCounters"), false);
    window->applySidebarSettings();
    QCOMPARE(folderIndex.data(Qt::DisplayRole).toString(),
             QLatin1String("Counted Folder"));
    bmModel->removeRow(folderIndex.row(), menuIndex);

    for (const char *key : optionKeys)
        settings.remove(QLatin1String(key));
    settings.remove(QLatin1String("sidebar/panels"));
    settings.remove(QLatin1String("sidebar/panelOrder"));
    settings.remove(QLatin1String("MainWindow/showSidebar"));
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

// TABS19: the rendered tab strip sits at Firefox's normal-density
// metrics — a ~33px face (content + 17) even at desktop-size fonts —
// and a container tab no longer doubles the row — the regression
// this fixes: the CONT02 name chip reserved a second row (~18px)
// per container tab and QTabBar sizes every tab to the tallest hint.
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
                         bar->iconSize().height()) + 17;
    QVERIFY2(bar->height() <= cap,
             qPrintable(QStringLiteral(
                 "strip height %1 exceeds the Firefox face %2")
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
                 <= qMax(bar->fontMetrics().height(),
                         bar->iconSize().height()) + 17,
             qPrintable(QStringLiteral(
                 "vertical row height %1 exceeds the ~33px face (%2)")
                 .arg(bar->tabRect(0).height())
                 .arg(qMax(bar->fontMetrics().height(),
                           bar->iconSize().height()) + 17)));
    QVERIFY(bar->width() >= 180 && bar->width() <= 190);

    // TABS11: the selected row must not paint below its rect —
    // Fusion's selected-tab fillRect is sized by the rect's bottom
    // coordinate, so on a tall rail a ghost band (bottom-1)px deep
    // appeared under the selected row and read as "the row is too
    // tall".  Needs the real TabWidget parent — the tab-frame color
    // (vs plain window color) only applies to HasFrame tabs.
    bar->setCurrentIndex(bar->count() - 1);
    QApplication::processEvents();
    {
        const QImage image = bar->grab().toImage();
        const QRect last = bar->tabRect(bar->count() - 1);
        const int probeY = qMin(last.bottom() + 6, bar->height() - 1);
        const QRgb below = image.pixel(bar->width() / 2, probeY);
        const QRgb tail = image.pixel(bar->width() / 2,
                                      bar->height() - 4);
        QVERIFY2(qAbs(qGray(below) - qGray(tail)) <= 2,
                 qPrintable(QStringLiteral(
                     "ghost fill below the selected row: pixel %1 vs "
                     "strip tail %2").arg(below, 0, 16).arg(tail, 0, 16)));
    }
    if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
        const QString dir =
            qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
        bar->grab().save(dir + QLatin1String("/strip-vertical.png"));
    }
    settings.remove(QLatin1String("tabs/tabBarPosition"));

    settings.remove(QLatin1String("tabs/containerDisplay"));
    closeWindow(window);
}

// NAVB01: the toolbar's combined Stop/Reload action must follow the
// engine page's load lifecycle — a held-open response keeps progress
// at 0 forever under Qt 6.12, yet the face must still flip to Stop on
// loadStarted and back to Reload on finish or abort.  The chain —
// Engine::Page::loadStarted/loadFinished -> TabWidget ->
// BrowserMainWindow::updateStopReloadState — is exercised end-to-end,
// including across a tab switch away from and back to a loading tab.
// NAVB02: the url bar's duplicate stop/reload button is gone — its
// only remaining load surface is the painted strip, asserted here
// alongside the toolbar face.  The same page-action binding keeps
// Back enabled and live after navigations (the "sometimes dead"
// report).
void tst_BrowserMainWindow::stopReloadState()
{
    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    // Action order: app menu, Back, Forward, Stop/Reload, Home — the
    // HOME01 leg locks Home at index 4.
    QAction *back = navBar->actions().value(1);
    QAction *stopReload = navBar->actions().value(3);
    QVERIFY(back);
    QVERIFY(stopReload);
    QVERIFY2(stopReload->toolTip().contains(QLatin1String("Reload")),
             qPrintable(stopReload->toolTip()));

    // "/held" answers headers + an unterminated body and stays open —
    // the navigation commits but progress never completes; "/abort"
    // dies mid-request so the load fails; everything else finishes.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    QObject::connect(&server, &QTcpServer::newConnection, &server,
                     [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
        QByteArray *request = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket,
                         [socket, request]() {
            request->append(socket->readAll());
            if (!request->contains("\r\n\r\n"))
                return;
            if (request->contains("GET /held")) {
                socket->write(QByteArray(
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "\r\n"
                    "<html><body><p>loading"));
                socket->flush();
                return;
            }
            if (request->contains("GET /abort")) {
                socket->abort();
                return;
            }
            const QByteArray body =
                "<html><head><title>Done</title></head>"
                "<body><p>done</p></body></html>";
            socket->write(QByteArray("HTTP/1.1 200 OK\r\n"
                                     "Content-Type: text/html\r\n"
                                     "Content-Length: ")
                          + QByteArray::number(body.size())
                          + "\r\n\r\n" + body);
            socket->flush();
            socket->disconnectFromHost();
        });
    });

    WebView *view = window->currentTab();
    QVERIFY(view);
    // NAVB02: the toolbar action is the single stop/reload control —
    // the url bar carries no second button, only the painted loading
    // strip on its bottom edge while WebView::isLoading().
    LocationBar *bar = nullptr;
    const QList<LocationBar *> bars =
        window->findChildren<LocationBar *>();
    for (LocationBar *candidate : bars) {
        if (candidate->webView() == view)
            bar = candidate;
    }
    QVERIFY2(bar, "no url bar bound to the current view");
    QVERIFY(!bar->findChild<QToolButton *>(
        QLatin1String("stopReloadButton")));

    // Strip detection: LocationBar::paintEvent fills the field's
    // bottom edge with the loading color (116,192,250) — light
    // palettes — while a load is in flight.
    auto stripPainted = [bar]() {
        const QImage grab = bar->grab().toImage();
        const int y = grab.height() - 2;
        for (int x = 2; x < grab.width() - 2; ++x) {
            const QColor pixel = grab.pixelColor(x, y);
            if (qAbs(pixel.red() - 116) < 24
                && qAbs(pixel.green() - 192) < 24
                && qAbs(pixel.blue() - 250) < 24)
                return true;
        }
        return false;
    };

    QSignalSpy progressSpy(view->enginePage(), &Engine::Page::loadProgress);
    QVERIFY2(progressSpy.isValid(), "Engine::Page::loadProgress not a signal");
    // isLoading alone can pass in the gap before loadStarted lands —
    // every completed leg waits on a loadFinished edge instead.
    QSignalSpy finishSpy(view->page(), &QWebEnginePage::loadFinished);

    const QUrl base(QStringLiteral("http://127.0.0.1:%1")
                        .arg(server.serverPort()));

    // Load in flight -> Stop face on the toolbar action and the
    // loading strip on the url bar.  Qt 6.12 emits progress only as 0
    // then 100, so this must hold while every progress value stays at
    // 0 — the whole point of keying on loadStarted/loadFinished.
    view->loadUrl(base.resolved(QUrl(QLatin1String("/held"))));
    QTRY_VERIFY_WITH_TIMEOUT(
        stopReload->toolTip().contains(QLatin1String("Stop")), 15000);
    QTRY_VERIFY_WITH_TIMEOUT(stripPainted(), 5000);
    QVERIFY(view->page()->isLoading());
    for (const QList<QVariant> &args : progressSpy) {
        const int p = args.value(0).toInt();
        QVERIFY2(p == 0 || p == 100, qPrintable(
            QStringLiteral("unexpected mid-flight progress %1").arg(p)));
    }

    // The stop face must really stop: the toolbar action dispatches
    // through m_viewStopAction's mapper to the page's Stop action.
    stopReload->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!view->page()->isLoading(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        stopReload->toolTip().contains(QLatin1String("Reload")), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(!stripPainted(), 5000);

    // A finished load leaves the reload face, and the reload face
    // really reloads — a fresh load begins on trigger.
    int done = finishSpy.count();
    view->loadUrl(base.resolved(QUrl(QLatin1String("/one"))));
    QTRY_VERIFY_WITH_TIMEOUT(finishSpy.count() > done, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        stopReload->toolTip().contains(QLatin1String("Reload")), 5000);
    QSignalSpy startedSpy(view->enginePage(), &Engine::Page::loadStarted);
    done = finishSpy.count();
    stopReload->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() > 0, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(finishSpy.count() > done, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!view->page()->isLoading(), 20000);
    QVERIFY(!stripPainted());

    // A failed load restores the reload face the same way.
    done = finishSpy.count();
    view->loadUrl(base.resolved(QUrl(QLatin1String("/abort"))));
    QTRY_VERIFY_WITH_TIMEOUT(finishSpy.count() > done, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view->page()->isLoading()
        && stopReload->toolTip().contains(QLatin1String("Reload")),
        20000);
    QVERIFY(!stripPainted());

    // Back binding: two clean navigations leave the toolbar Back
    // enabled and live — a trigger walks exactly one history step.
    done = finishSpy.count();
    view->loadUrl(base.resolved(QUrl(QLatin1String("/first"))));
    QTRY_VERIFY_WITH_TIMEOUT(finishSpy.count() > done, 20000);
    done = finishSpy.count();
    view->loadUrl(base.resolved(QUrl(QLatin1String("/second"))));
    QTRY_VERIFY_WITH_TIMEOUT(finishSpy.count() > done, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(back->isEnabled(), 10000);
    done = finishSpy.count();
    back->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(finishSpy.count() > done, 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        view->url().path().endsWith(QLatin1String("/first")), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!view->page()->isLoading(), 20000);

    // Tab switching: the shared chrome tracks the CURRENT tab's load
    // state — a background tab mid-load must not pin the stop face,
    // and switching back must restore it even though its progress
    // value stayed 0 the whole time.
    WebView *second = window->tabWidget()->makeNewTab(true);
    QVERIFY(second);
    second->loadUrl(base.resolved(QUrl(QLatin1String("/held"))));
    QTRY_VERIFY_WITH_TIMEOUT(
        stopReload->toolTip().contains(QLatin1String("Stop")), 15000);

    int firstIndex = -1;
    int secondIndex = -1;
    for (int i = 0; i < window->tabWidget()->count(); ++i) {
        if (window->tabWidget()->webView(i) == view)
            firstIndex = i;
        else if (window->tabWidget()->webView(i) == second)
            secondIndex = i;
    }
    QVERIFY(firstIndex >= 0 && secondIndex >= 0);
    window->tabWidget()->setCurrentIndex(firstIndex);
    // Switching to the first tab may kick off an engine reload (a
    // discarded background WebContents reloads when shown again) —
    // while that load runs the Stop face is the CORRECT state for the
    // current tab, so wait for it to settle before pinning Reload.
    QTRY_VERIFY_WITH_TIMEOUT(!view->page()->isLoading(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(
        stopReload->toolTip().contains(QLatin1String("Reload")), 5000);
    // The background tab's held load is untouched either way.
    QVERIFY(second->page()->isLoading());

    window->tabWidget()->setCurrentIndex(secondIndex);
    QTRY_VERIFY_WITH_TIMEOUT(
        stopReload->toolTip().contains(QLatin1String("Stop")), 5000);

    // Stop the background load and put the selection back so nothing
    // is in flight during teardown.
    second->page()->triggerAction(QWebEnginePage::Stop);
    QTRY_VERIFY_WITH_TIMEOUT(!second->page()->isLoading(), 10000);
    window->tabWidget()->setCurrentIndex(firstIndex);

    closeWindow(window);

    // Private window — the same chrome path on the off-the-record
    // profile: load -> stop face -> finish -> reload face.
    BrowserApplication::setPrivate(true);
    SubWindow *privateWindow = new SubWindow;
    privateWindow->resize(1000, 600);
    privateWindow->show();
    QApplication::processEvents();
    WebView *privateView = privateWindow->currentTab();
    QVERIFY(privateView);
    QVERIFY(privateView->enginePage()->isOffTheRecord());
    QAction *privateStopReload = privateWindow
        ->findChild<QToolBar *>(QLatin1String("NavigationToolBar"))
        ->actions().value(3);
    QVERIFY(privateStopReload);
    privateView->loadUrl(base.resolved(QUrl(QLatin1String("/held"))));
    QTRY_VERIFY_WITH_TIMEOUT(
        privateStopReload->toolTip().contains(QLatin1String("Stop")),
        15000);
    privateStopReload->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!privateView->page()->isLoading(), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        privateStopReload->toolTip().contains(QLatin1String("Reload")),
        5000);
    closeWindow(privateWindow);
    BrowserApplication::setPrivate(false);

    // Tor window — no tor daemon is up, so the navigation fails
    // (slowly, on the SOCKS connect timeout); the faces still
    // bracket the attempt through the same path.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->resize(1000, 600);
    torWindow->show();
    QApplication::processEvents();
    WebView *torView = torWindow->currentTab();
    QVERIFY(torView);
    QAction *torStopReload = torWindow
        ->findChild<QToolBar *>(QLatin1String("NavigationToolBar"))
        ->actions().value(3);
    QVERIFY(torStopReload);
    QSignalSpy torFinish(torView->page(), &QWebEnginePage::loadFinished);
    torView->loadUrl(base.resolved(QUrl(QLatin1String("/done"))));
    QTRY_VERIFY_WITH_TIMEOUT(torFinish.count() > 0, 60000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !torView->page()->isLoading()
        && torStopReload->toolTip().contains(QLatin1String("Reload")),
        20000);
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);
}

// EXT08: enabled extensions that declare an action popup surface one
// nav-bar button each in the splitter cell right after the location
// bar; the button's click opens the chrome-extension:// popup in a
// Qt::Popup-framed view on the canonical profile.  Nothing shows on
// private/tor windows — their profiles may never load extensions.
void tst_BrowserMainWindow::extensionActionButtons()
{
    if (!ExtensionManager::isSupported())
        QSKIP("Qt built without webengine_extensions");

    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    ExtensionActionBar *bar = window->findChild<ExtensionActionBar *>(
        QLatin1String("extensionActionBar"));
    QVERIFY(bar);

    // The strip lives in the navigation splitter immediately after
    // the location bar stack — index 1, where the engine indicator
    // cell sits on master.
    QSplitter *splitter = window
        ->findChild<QToolBar *>(QLatin1String("NavigationToolBar"))
        ->findChild<QSplitter *>();
    QVERIFY(splitter);
    QCOMPARE(splitter->indexOf(bar), 1);

    // The bundled component extensions carry no action popup, so a
    // fresh window's strip is empty and hidden — no dead gap.
    const int baseline = bar->actionCount();
    if (baseline == 0)
        QVERIFY(bar->isHidden());

    // Popup-bearing MV3 fixture: action.default_popup +
    // action.default_icon + a top-level icons map.
    QTemporaryDir fixture;
    QVERIFY(fixture.isValid());
    auto writeFile = [&fixture](const QString &name,
                                const QByteArray &contents) {
        QFile file(fixture.path() + QLatin1Char('/') + name);
        return file.open(QIODevice::WriteOnly)
            && file.write(contents) == contents.size();
    };
    QVERIFY(writeFile(QLatin1String("manifest.json"),
        "{\"manifest_version\":3,"
        "\"name\":\"ext-action-fixture\","
        "\"version\":\"0.1\","
        "\"action\":{\"default_popup\":\"popup.html\","
        "\"default_icon\":{\"16\":\"icon16.png\",\"32\":\"icon32.png\"}},"
        "\"icons\":{\"16\":\"icon16.png\",\"48\":\"icon48.png\"}}"));
    QVERIFY(writeFile(QLatin1String("popup.html"),
        "<html><body style=\"width:300px;height:200px\">popup</body></html>"));
    for (const char *icon : {"icon16.png", "icon32.png", "icon48.png"}) {
        QImage image(16, 16, QImage::Format_ARGB32);
        image.fill(QColor(30, 144, 60));
        QVERIFY(image.save(fixture.path() + QLatin1Char('/')
                           + QLatin1String(icon)));
    }

    ExtensionManager *extensions = ExtensionManager::instance();
    const auto findId = [extensions]() {
        for (const ExtensionManager::ExtensionInfo &info
             : extensions->extensions()) {
            if (info.name == QLatin1String("ext-action-fixture"))
                return info.id;
        }
        return QString();
    };

    extensions->loadExtension(fixture.path());
    QString extensionId;
    QTRY_VERIFY_WITH_TIMEOUT(!(extensionId = findId()).isEmpty(),
                             15000);

    // Loaded but still disabled => no button yet.
    QTRY_VERIFY_WITH_TIMEOUT(bar->actionCount() == baseline, 5000);

    extensions->setExtensionEnabled(extensionId, true);
    QTRY_VERIFY_WITH_TIMEOUT(bar->actionCount() == baseline + 1,
                             15000);
    QVERIFY(bar->isVisibleTo(window));

    QToolButton *button = bar->findChild<QToolButton *>(
        QLatin1String("extensionActionButton_") + extensionId);
    QVERIFY(button);
    QCOMPARE(button->toolTip(), QLatin1String("ext-action-fixture"));
    QVERIFY(!button->icon().isNull());
    QVERIFY(!button->icon()
        .pixmap(button->iconSize()).isNull());

    // Click -> Qt::Popup-framed view on the canonical profile loads
    // the extension's action popup url.
    const auto popup = []() -> QFrame * {
        for (QWidget *top : QApplication::topLevelWidgets()) {
            if (top->objectName()
                == QLatin1String("extensionActionPopup"))
                return qobject_cast<QFrame *>(top);
        }
        return nullptr;
    };
    QVERIFY(!popup());
    button->click();
    QFrame *actionPopup = nullptr;
    QTRY_VERIFY_WITH_TIMEOUT((actionPopup = popup()) != nullptr, 10000);
    QVERIFY(actionPopup->isVisible());
    QVERIFY(actionPopup->windowFlags() & Qt::Popup);
    QWebEngineView *popupView =
        actionPopup->findChild<QWebEngineView *>();
    QVERIFY(popupView);
    QTRY_COMPARE_WITH_TIMEOUT(
        popupView->url().scheme(), QLatin1String("chrome-extension"),
        10000);
    QCOMPARE(popupView->page()->profile(),
             BrowserApplication::instance()->webEngineProfile());
    // The popup is chrome, never a tab.
    for (int i = 0; i < window->tabWidget()->count(); ++i) {
        QVERIFY(!window->tabWidget()->webView(i)->url().scheme()
            .startsWith(QLatin1String("chrome-extension")));
    }

    // Optional visual review: ARORA_EXT_GRAB_DIR=<dir> grabs the
    // window (button strip visible right of the url bar) and the
    // open action popup.
    const QByteArray grabDir = qgetenv("ARORA_EXT_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QTRY_VERIFY_WITH_TIMEOUT(
            popupView->page()->isLoading() == false, 15000);
        window->grab().save(
            QLatin1String(grabDir) + QLatin1String("/ext-bar.png"));
        actionPopup->grab().save(
            QLatin1String(grabDir) + QLatin1String("/ext-popup.png"));
    }

    // Re-clicking the same button toggles the popup closed.
    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(!popup() || !popup()->isVisible(), 5000);

    // A second window mirrors the same extension set on its own host.
    SubWindow *second = new SubWindow;
    second->show();
    ExtensionActionBar *secondBar =
        second->findChild<ExtensionActionBar *>(
            QLatin1String("extensionActionBar"));
    QVERIFY(secondBar);
    QVERIFY(secondBar != bar);
    QTRY_VERIFY_WITH_TIMEOUT(
        secondBar->actionCount() == baseline + 1, 10000);
    closeWindow(second);

    // Disable -> the button goes away in every window.
    extensions->setExtensionEnabled(extensionId, false);
    QTRY_VERIFY_WITH_TIMEOUT(bar->actionCount() == baseline, 15000);
    if (baseline == 0)
        QVERIFY(bar->isHidden());

    // Private + tor windows never get the host at all, whatever the
    // extension set.
    BrowserApplication::setPrivate(true);
    SubWindow *privateWindow = new SubWindow;
    privateWindow->show();
    QVERIFY(!privateWindow->findChild<ExtensionActionBar *>(
        QLatin1String("extensionActionBar")));
    closeWindow(privateWindow);
    BrowserApplication::setPrivate(false);

    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->show();
    QVERIFY(!torWindow->findChild<ExtensionActionBar *>(
        QLatin1String("extensionActionBar")));
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);

    closeWindow(window);

    // Session-scoped fixture — unload so nothing leaks into later
    // tests or the real profile's extension list.
    extensions->removeExtension(extensionId);
    QTRY_VERIFY_WITH_TIMEOUT(findId().isEmpty(), 15000);
}

// NAVB03: nav-bar context menu + in-place edit mode.  Right-click
// offers New Tab / New Private Tab / Edit Navigation Bar ahead of
// the stock toolbar entries; edit mode drag-reorders items behind a
// drop-gap line and exits via the toggle, Esc or an outside click.
void tst_BrowserMainWindow::navBarEditMode()
{
    // NAVB04: drags persist through the shared settings store —
    // start from a clean key so both legs see the stock order.
    {
        QSettings settings;
        settings.remove(QLatin1String("navbar/order"));
        settings.sync();
    }

    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    QCOMPARE(navBar->contextMenuPolicy(), Qt::CustomContextMenu);
    QAction *editAction = window->navBarEditAction();
    QVERIFY(editAction);
    QVERIFY(editAction->isCheckable());
    QVERIFY(!window->isNavBarEditMode());

    // The File menu's shared private action — the context menu must
    // carry the same object, not a re-wired clone.
    QMenu *fileMenu = window->appMenu()->actions().first()->menu();
    QAction *filePrivate = nullptr;
    for (QAction *action : fileMenu->actions()) {
        if (action->text().contains(QLatin1String("Private")))
            filePrivate = action;
    }
    QVERIFY(filePrivate);

    // Menu leg — the exec()'d popup is inspected + closed from a
    // timer (same pattern as the tab-rename context menu test).
    QStringList labels;
    QList<bool> visibleFlags;
    QList<QAction *> menuActions;
    const QByteArray grabDir = qgetenv("ARORA_NAVBAR_GRAB_DIR");
    const auto inspectPopup = [&labels, &visibleFlags,
                               &menuActions,
                               &grabDir](const char *grabName) {
        QMenu *menu = qobject_cast<QMenu *>(
            QApplication::activePopupWidget());
        if (!menu) {
            const auto tops = QApplication::topLevelWidgets();
            for (QWidget *top : tops) {
                if (QMenu *m = qobject_cast<QMenu *>(top);
                    m && m->isVisible()) {
                    menu = m;
                    break;
                }
            }
        }
        if (!menu)
            return;
        const auto acts = menu->actions();
        for (QAction *action : acts) {
            labels << (action->isSeparator()
                           ? QStringLiteral("|")
                           : action->text().remove(QLatin1Char('&')));
            visibleFlags << action->isVisible();
            menuActions << action;
        }
        if (!grabDir.isEmpty()) {
            QDir().mkpath(QString::fromLocal8Bit(grabDir));
            menu->grab().save(QString::fromLocal8Bit(grabDir)
                              + QLatin1String(grabName));
        }
        menu->close();
    };
    QTimer::singleShot(50, navBar, [&inspectPopup]() {
        inspectPopup("/navbar-context.png");
    });
    emit navBar->customContextMenuRequested(QPoint(6, 6));
    QTRY_VERIFY_WITH_TIMEOUT(labels.size() >= 5, 5000);
    QCOMPARE(labels.at(0), QLatin1String("New Tab"));
    QCOMPARE(labels.at(1), QLatin1String("New Private Tab"));
    QCOMPARE(labels.at(2), QLatin1String("|"));
    QCOMPARE(labels.at(3), QLatin1String("Edit Navigation Bar"));
    // NAVB04: the reset entry rides with the toggle — greyed while
    // the bar already sits at stock.
    QCOMPARE(labels.at(4), QLatin1String("Reset Navigation Bar"));
    QCOMPARE(menuActions.at(4), window->navBarResetAction());
    QVERIFY(!menuActions.at(4)->isEnabled());
    QCOMPARE(labels.at(5), QLatin1String("|"));
    // Same QAction objects the File menu drives.
    QCOMPARE(menuActions.at(0), window->tabWidget()->newTabAction());
    QCOMPARE(menuActions.at(1), filePrivate);
    // The stock QMainWindow toolbar toggles still ride below.
    QVERIFY(labels.contains(QLatin1String("Navigation")));
    QVERIFY(labels.contains(QLatin1String("Bookmarks")));

    // Slot identities for the drag legs — the order READ02/HOME01
    // lock: app-menu host, Back, Forward, Stop/Reload, Home, hidden
    // reader, splitter, sidebar toggle.  MENU13: the wordmark label
    // owns the last slot now, so the sidebar toggle resolves by its
    // stable item id, not by position.
    const QList<QAction *> stock = navBar->actions();
    QAction *back = stock.at(1);
    QAction *stopReload = stock.at(3);
    QAction *sidebarToggle = nullptr;
    for (QAction *action : stock) {
        if (action->property("navBarItemId").toString()
                == QLatin1String("sidebar"))
            sidebarToggle = action;
    }
    QVERIFY(sidebarToggle);
    QWidget *stopButton = navBar->widgetForAction(stopReload);
    QWidget *backButton = navBar->widgetForAction(back);
    QVERIFY(stopButton && backButton);

    // Normal mode: a click on a bar button still fires it.
    QSignalSpy reloadSpy(stopReload, &QAction::triggered);
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(reloadSpy.count() == 1, 5000);

    // Toggle on — the checkable entry latches, the frame is drawn.
    editAction->setChecked(true);
    QVERIFY(window->isNavBarEditMode());
    QVERIFY(editAction->isChecked());
    QVERIFY(navBar->styleSheet().contains(QLatin1String("dashed")));
    QWidget *indicator = navBar->findChild<QWidget *>(
        QLatin1String("navBarDropIndicator"));
    QVERIFY(indicator);
    QVERIFY(indicator->isHidden());

    // In edit mode a plain click picks the item — it doesn't fire,
    // and it doesn't drop out of the mode either.
    reloadSpy.clear();
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTest::qWait(200);
    QCOMPARE(reloadSpy.count(), 0);
    QVERIFY(window->isNavBarEditMode());

    // Drag leg: pick up Stop/Reload and drop it ahead of Back — the
    // gap line shows while the drag is live, then the item moves.
    // (Sibling-widget drops are aimed through the bar's own
    // coordinates — QWidget::mapTo only walks up the parent chain.)
    const QPoint start = stopButton->rect().center();
    QTest::mousePress(stopButton, Qt::LeftButton, Qt::NoModifier,
                      start);
    const QPoint dropPos = stopButton->mapFrom(
        navBar, QPoint(navBar->actionGeometry(back).left()
                           + navBar->actionGeometry(back).width() / 4,
                       navBar->actionGeometry(back).center().y()));
    QTest::mouseMove(stopButton, dropPos);
    QVERIFY(indicator->isVisible());
    // Offscreen grab for visual review — framed bar + live gap line.
    if (!grabDir.isEmpty()) {
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/navbar-edit-drag.png"));
    }
    QTest::mouseRelease(stopButton, Qt::LeftButton, Qt::NoModifier,
                        dropPos);
    QCOMPARE(navBar->actions().indexOf(stopReload), 1);
    QCOMPARE(navBar->actions().indexOf(back), 2);
    QVERIFY(indicator->isHidden());
    QCOMPARE(reloadSpy.count(), 0);

    // Second leg: the last item (sidebar toggle) can move into the
    // middle — drops are not tail/boundary special cases.
    QWidget *sidebarButton = navBar->widgetForAction(sidebarToggle);
    QVERIFY(sidebarButton);
    QTest::mousePress(sidebarButton, Qt::LeftButton, Qt::NoModifier,
                      sidebarButton->rect().center());
    const QPoint midDrop = sidebarButton->mapFrom(
        navBar, QPoint(navBar->actionGeometry(stopReload).left()
                           + navBar->actionGeometry(stopReload).width()
                                 / 4,
                       navBar->actionGeometry(stopReload).center()
                           .y()));
    QTest::mouseMove(sidebarButton, midDrop);
    QTest::mouseRelease(sidebarButton, Qt::LeftButton, Qt::NoModifier,
                        midDrop);
    QCOMPARE(navBar->actions().indexOf(sidebarToggle), 1);
    QCOMPARE(navBar->actions().indexOf(stopReload), 2);

    // MENU13: the trailing 'Arora' wordmark is pinned chrome — a
    // press on it picks nothing (no drag, no gap line), and a drop
    // past every movable item still lands before it, so it can never
    // be pushed off the bar's tail.
    QAction *brandItem = navBar->actions().last();
    QLabel *brandLabel = qobject_cast<QLabel *>(
        navBar->widgetForAction(brandItem));
    QVERIFY(brandLabel);
    QCOMPARE(brandLabel->objectName(),
             QLatin1String("navBarBrandLabel"));
    QTest::mousePress(brandLabel, Qt::LeftButton, Qt::NoModifier,
                      brandLabel->rect().center());
    QTest::mouseMove(brandLabel, QPoint(0, brandLabel->rect().center().y()));
    QVERIFY(indicator->isHidden());
    QTest::mouseRelease(brandLabel, Qt::LeftButton, Qt::NoModifier,
                        QPoint(0, brandLabel->rect().center().y()));
    QCOMPARE(navBar->actions().last(), brandItem);
    QVERIFY(window->isNavBarEditMode());

    // Drag the sidebar toggle to the bar's right edge — it lands
    // before the pinned wordmark, which keeps the tail.  Re-fetch the
    // button: the earlier drags' remove/insert rebuilt its widget, so
    // the cached pointer's geometry is the pre-drag position.
    QWidget *freshSidebarButton = navBar->widgetForAction(sidebarToggle);
    QVERIFY(freshSidebarButton);
    QTest::mousePress(freshSidebarButton, Qt::LeftButton, Qt::NoModifier,
                      freshSidebarButton->rect().center());
    const QPoint endDrop = freshSidebarButton->mapFrom(
        navBar, QPoint(navBar->width() - 2,
                       navBar->actionGeometry(brandItem).center().y()));
    QTest::mouseMove(freshSidebarButton, endDrop);
    QVERIFY(indicator->isVisible());
    QTest::mouseRelease(freshSidebarButton, Qt::LeftButton, Qt::NoModifier,
                        endDrop);
    QCOMPARE(navBar->actions().last(), brandItem);
    QCOMPARE(navBar->actions().indexOf(sidebarToggle),
             navBar->actions().count() - 2);

    // Esc exits — the entry unlatches, the sheet restores, the order
    // the user left stays (sidebar toggle still just ahead of the
    // pinned wordmark).
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(!window->isNavBarEditMode());
    QVERIFY(!editAction->isChecked());
    QVERIFY(!navBar->styleSheet().contains(QLatin1String("dashed")));
    QCOMPARE(navBar->actions().indexOf(sidebarToggle),
             navBar->actions().indexOf(brandItem) - 1);
    QCOMPARE(navBar->actions().last(), brandItem);

    // Re-enter — a click anywhere outside the bar locks the order
    // and exits (the click itself is passed through).
    editAction->setChecked(true);
    QVERIFY(window->isNavBarEditMode());
    QTest::mouseClick(window->tabWidget(), Qt::LeftButton,
                      Qt::NoModifier, QPoint(30, 30));
    QVERIFY(!window->isNavBarEditMode());
    QVERIFY(!editAction->isChecked());

    // And normal mode fires bar buttons again.
    reloadSpy.clear();
    QTest::mouseClick(stopButton, Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(reloadSpy.count() >= 1, 5000);
    closeWindow(window);

    // Tor window: the violet sheet survives entering/leaving edit
    // mode byte-identically, the private entry stays hidden like in
    // the File menu, and reorder works under the tor chrome.  The
    // first window's drags wrote the shared order — clear it so this
    // leg exercises stock positions like the assertions assume.
    {
        QSettings settings;
        settings.remove(QLatin1String("navbar/order"));
        settings.sync();
    }
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->resize(1200, 700);
    torWindow->show();
    QApplication::processEvents();
    QToolBar *torBar = torWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(torBar);
    const QString torSheet = torBar->styleSheet();
    QVERIFY(torSheet.contains(QLatin1String("#4a3372")));
    QAction *torEdit = torWindow->navBarEditAction();
    QVERIFY(torEdit);
    torEdit->setChecked(true);
    QVERIFY(torWindow->isNavBarEditMode());
    QVERIFY(torBar->styleSheet().contains(QLatin1String("#4a3372")));
    QVERIFY(torBar->styleSheet().contains(QLatin1String("dashed")));
    if (!grabDir.isEmpty()) {
        torBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/navbar-edit-tor.png"));
    }

    labels.clear();
    visibleFlags.clear();
    menuActions.clear();
    QTimer::singleShot(50, torBar, [&inspectPopup]() {
        inspectPopup("/navbar-context-tor.png");
    });
    emit torBar->customContextMenuRequested(QPoint(6, 6));
    QTRY_VERIFY_WITH_TIMEOUT(labels.size() >= 3, 5000);
    QCOMPARE(labels.at(0), QLatin1String("New Tab"));
    // The shared private action is hidden on tor — the row exists in
    // the action list but never paints, matching the File menu.
    QCOMPARE(labels.at(1), QLatin1String("New Private Tab"));
    QVERIFY(!visibleFlags.at(1));

    const QList<QAction *> torStock = torBar->actions();
    QAction *torHome = torStock.at(4);
    QAction *torBack = torStock.at(1);
    QWidget *torHomeButton = torBar->widgetForAction(torHome);
    QWidget *torBackButton = torBar->widgetForAction(torBack);
    QVERIFY(torHomeButton && torBackButton);
    QTest::mousePress(torHomeButton, Qt::LeftButton, Qt::NoModifier,
                      torHomeButton->rect().center());
    const QPoint torDrop = torHomeButton->mapFrom(
        torBar, QPoint(torBar->actionGeometry(torBack).left()
                           + torBar->actionGeometry(torBack).width()
                                 / 4,
                       torBar->actionGeometry(torBack).center().y()));
    QTest::mouseMove(torHomeButton, torDrop);
    QTest::mouseRelease(torHomeButton, Qt::LeftButton, Qt::NoModifier,
                        torDrop);
    QCOMPARE(torBar->actions().indexOf(torHome), 1);

    torEdit->setChecked(false);
    QVERIFY(!torWindow->isNavBarEditMode());
    QCOMPARE(torBar->styleSheet(), torSheet);
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);
}

// NAVB04: the dragged order is one shared persisted sequence —
// written on every committed drop, applied by every new window (the
// session-restore path too: restored windows are freshly
// constructed), synced to already-open windows, resettable to stock
// through the context menu, and tolerant of stale or corrupt stored
// values.  Item ids ride as a QAction property; navBarOrder() reads
// the live bar back.
void tst_BrowserMainWindow::navBarOrderPersistence()
{
    const QLatin1String key("navbar/order");
    {
        QSettings settings;
        settings.remove(key);
        settings.sync();
    }
    const QStringList stockOrder{
        QStringLiteral("appMenu"), QStringLiteral("back"),
        QStringLiteral("forward"), QStringLiteral("stopReload"),
        QStringLiteral("home"), QStringLiteral("reader"),
        QStringLiteral("locationBar"), QStringLiteral("sidebar")};

    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    QVERIFY(window->navBarResetAction());
    QCOMPARE(window->navBarOrder(), stockOrder);

    // A real edit-mode drag — Stop/Reload ahead of Back — lands in
    // QSettings on release, not on edit-mode exit or window close.
    window->navBarEditAction()->setChecked(true);
    QVERIFY(window->isNavBarEditMode());
    const QList<QAction *> actions = navBar->actions();
    QAction *back = actions.at(1);
    QAction *stopReload = actions.at(3);
    QWidget *stopButton = navBar->widgetForAction(stopReload);
    QVERIFY(stopButton);
    QTest::mousePress(stopButton, Qt::LeftButton, Qt::NoModifier,
                      stopButton->rect().center());
    const QPoint dropPos = stopButton->mapFrom(
        navBar, QPoint(navBar->actionGeometry(back).left()
                       + navBar->actionGeometry(back).width() / 4,
                       navBar->actionGeometry(back).center().y()));
    QTest::mouseMove(stopButton, dropPos);
    QTest::mouseRelease(stopButton, Qt::LeftButton, Qt::NoModifier,
                        dropPos);
    const QStringList draggedOrder{
        QStringLiteral("appMenu"), QStringLiteral("stopReload"),
        QStringLiteral("back"), QStringLiteral("forward"),
        QStringLiteral("home"), QStringLiteral("reader"),
        QStringLiteral("locationBar"), QStringLiteral("sidebar")};
    QCOMPARE(window->navBarOrder(), draggedOrder);
    {
        QSettings settings;
        QCOMPARE(settings.value(key).toStringList(), draggedOrder);
    }
    window->navBarEditAction()->setChecked(false);
    QVERIFY(!window->isNavBarEditMode());

    // New windows pick the shared order up at construction — this is
    // also the session-restore path.
    SubWindow *second = new SubWindow;
    second->resize(1200, 700);
    second->show();
    QApplication::processEvents();
    QToolBar *secondBar = second->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(secondBar);
    QCOMPARE(second->navBarOrder(), draggedOrder);

    // Live sync — a drag in the first window lands on the open
    // second window without a restart: pull Back to the very front.
    window->navBarEditAction()->setChecked(true);
    const QList<QAction *> dragged = navBar->actions();
    QAction *appMenuItem = dragged.first();
    QWidget *backButton = navBar->widgetForAction(back);
    QVERIFY(backButton);
    QTest::mousePress(backButton, Qt::LeftButton, Qt::NoModifier,
                      backButton->rect().center());
    const QPoint frontDrop = backButton->mapFrom(
        navBar, QPoint(navBar->actionGeometry(appMenuItem).left()
                       + navBar->actionGeometry(appMenuItem).width() / 4,
                       navBar->actionGeometry(appMenuItem).center().y()));
    QTest::mouseMove(backButton, frontDrop);
    QTest::mouseRelease(backButton, Qt::LeftButton, Qt::NoModifier,
                        frontDrop);
    const QStringList resyncedOrder{
        QStringLiteral("back"), QStringLiteral("appMenu"),
        QStringLiteral("stopReload"), QStringLiteral("forward"),
        QStringLiteral("home"), QStringLiteral("reader"),
        QStringLiteral("locationBar"), QStringLiteral("sidebar")};
    QCOMPARE(window->navBarOrder(), resyncedOrder);
    QCOMPARE(second->navBarOrder(), resyncedOrder);
    {
        QSettings settings;
        QCOMPARE(settings.value(key).toStringList(), resyncedOrder);
    }
    window->navBarEditAction()->setChecked(false);
    // Exiting edit mode resyncs against the store — already equal,
    // so the order stands.
    QCOMPARE(window->navBarOrder(), resyncedOrder);

    // Reset restores stock on every open window and drops the key.
    window->navBarResetAction()->trigger();
    QCOMPARE(window->navBarOrder(), stockOrder);
    QCOMPARE(second->navBarOrder(), stockOrder);
    {
        QSettings settings;
        QVERIFY(!settings.value(key).isValid());
    }

    // Stale/partial stored orders degrade gracefully: unknown ids
    // (including ones only another window type carries, like
    // torBadge) drop, and items the list doesn't name keep their
    // stock sequence at the end — the bar can never corrupt.
    {
        QSettings settings;
        settings.setValue(key, QStringList{
            QStringLiteral("bogus"), QStringLiteral("torBadge"),
            QStringLiteral("back"), QStringLiteral("sidebar")});
        settings.sync();
    }
    SubWindow *third = new SubWindow;
    third->show();
    QApplication::processEvents();
    QCOMPARE(third->navBarOrder(),
             (QStringList{QStringLiteral("back"),
                          QStringLiteral("sidebar"),
                          QStringLiteral("appMenu"),
                          QStringLiteral("forward"),
                          QStringLiteral("stopReload"),
                          QStringLiteral("home"),
                          QStringLiteral("reader"),
                          QStringLiteral("locationBar")}));
    closeWindow(third);

    // A corrupt non-list value falls back to stock entirely.
    {
        QSettings settings;
        settings.setValue(key, 42);
        settings.sync();
    }
    SubWindow *fourth = new SubWindow;
    fourth->show();
    QApplication::processEvents();
    QCOMPARE(fourth->navBarOrder(), stockOrder);
    closeWindow(fourth);

    // Tor window: the same shared order drives the violet bar —
    // tor-only ids resolve only there — and the chrome sheet is
    // untouched by the rebuild.
    {
        QSettings settings;
        settings.setValue(key, QStringList{
            QStringLiteral("torBadge"), QStringLiteral("appMenu"),
            QStringLiteral("sidebar"), QStringLiteral("back")});
        settings.sync();
    }
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->resize(1200, 700);
    torWindow->show();
    QApplication::processEvents();
    QToolBar *torBar = torWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(torBar);
    QCOMPARE(torWindow->navBarOrder(),
             (QStringList{QStringLiteral("torBadge"),
                          QStringLiteral("appMenu"),
                          QStringLiteral("sidebar"),
                          QStringLiteral("back"),
                          QStringLiteral("forward"),
                          QStringLiteral("stopReload"),
                          QStringLiteral("home"),
                          QStringLiteral("reader"),
                          QStringLiteral("torCircuit"),
                          QStringLiteral("locationBar")}));
    QVERIFY(torBar->styleSheet().contains(QLatin1String("#4a3372")));
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);

    closeWindow(second);
    closeWindow(window);
    // Leave the store clean for whatever runs next.
    {
        QSettings settings;
        settings.remove(key);
        settings.sync();
    }
}

QTEST_MAIN(tst_BrowserMainWindow)
#include "tst_browsermainwindow.moc"
