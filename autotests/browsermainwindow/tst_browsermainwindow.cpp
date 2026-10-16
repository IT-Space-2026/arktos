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
#include <qinputdialog.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qlistview.h>
#include <qmenu.h>
#include <qmenubar.h>
#include <qmessagebox.h>
#include <qplaintextedit.h>
#include <qprintdialog.h>
#include <qprintpreviewdialog.h>
#include <qprinterinfo.h>
#include <qprogressbar.h>
#include <qpushbutton.h>
#include <qsortfilterproxymodel.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qstatusbar.h>
#include <qtextbrowser.h>
#include <qthreadpool.h>
#include <qtabwidget.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
#include <qtemporarydir.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>
#include <qtreeview.h>
#include <qtreewidget.h>

#include "browsermainwindow.h"
#include "browserapplication.h"
#include "bookmarknode.h"
#include "browsertheme.h"
#include "aroraicon.h"
#include "arorastyle.h"
#include "containermanager.h"
#include "bookmarksdialog.h"
#include "bookmarksmanager.h"
#include "bookmarksmodel.h"
#include "edittreeview.h"
#include "enginetab.h"
#include "engineregistry.h"
#include "fakeengine.h"
#include "history.h"
#include "historymanager.h"
#include "profilecontext.h"
#include "profileidentity.h"
#include "profileregistry.h"
#include "profilemanagerdialog.h"
#include "profilestore.h"
#include "notesmodel.h"
#include "pdfview.h"
#include "printpreviewdialog.h"
#include "sidebargrip.h"
#include "sidebarpanel.h"
#include "statusbarwidgets.h"
#include "tabwidget.h"
#include "tabbar.h"
#include "readermode.h"
#include "readerbutton.h"
#include "readersanitizer.h"
#include "webview.h"
#include "webpage.h"
#include "webviewsearch.h"
#include "locationbar.h"
#include "sitepadlock.h"
#include "sitepanel.h"
#include "locationbarsiteicon.h"
#include "siteshield.h"
#include "toolbarsearch.h"
#include "torcircuitbutton.h"
#include "torcircuitpanel.h"
#include "tormanager.h"
#include "tortitlebar.h"
#include "downloadmanager.h"
#include "containermanager.h"
#include "commandpalette.h"
#include "extensionactionbar.h"
#include "extensionmanager.h"
#include "profilesettings.h"
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
    void cleanupTestCase();

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
    void printPaths();
    void closeConfirm();
    void chromeMetrics();
    void searchBoxVisibility();
    void torSearchBoxHidden();
    void torCircuitStatusLabel();
    void torTabStripLayout();
    void torChromeTheme();
    void torStatusBarTheme();
    void torNavIconsDarkChrome();
    void torChromeGlyphsLegible();
    void torReaderAccent();
    void darkChromeIcons();
    void darkThemeReaderGlyph();
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
    void sidebarResize();
    void sidebarPanelOrder();
    void sidebarPanelOptions();
    void sidebarGripResize();
    void downloadsSidebarPanel();
    void downloadsSidebarFinishedRows();
    void downloadsSidebarMinWidth();
    void downloadsSidebarActions();
    void sidebarHistoryControls();
    void bookmarksSidebarPanel();
    void chromeSlots();
    void downloadsSidebarClearList();
    void widgetTabBookmarksSafety();
    void tabStripCompactHeight();
    void tabStripNoUnderlines();
    void stopReloadState();
    void extensionActionButtons();
    void navShieldPadlock();
    void navBarEditMode();
    void navBarOrderPersistence();
    void siteShieldButton();
    void profileButton();
    void profileMenuEntries();
};

void tst_BrowserMainWindow::initTestCase()
{
    QCoreApplication::setApplicationName("tst_browsermainwindow");
    ProfileSettings settings;
    settings.clear();
    // Keep close() free of the multi-tab confirmation prompt; the
    // prompt's two branches get explicit coverage in closeConfirm().
    settings.setValue(QLatin1String("tabs/confirmClosingMultipleTabs"), false);
    // PRINT01: the preview's PdfDocumentClient must stay in-process
    // here — the bwrap worker spawns "<self> --pdf-worker", and self
    // is a qtest binary: that would rerun the whole suite as a child
    // (and recurse through this leg).
    qputenv("ARORA_PDF_INPROC", "1");
    qRegisterMetaType<QWebEnginePage *>("QWebEnginePage*");
    qRegisterMetaType<QAction *>("QAction*");
}

void tst_BrowserMainWindow::cleanupTestCase()
{
    // Legs that pass early can leave a window, a popup or a detached
    // page standing — drop them all before teardown so the profiles
    // release while the event loop is still around to deliver the
    // WebEngine-side shutdown round-trips.
    const QWidgetList tops = QApplication::topLevelWidgets();
    for (QWidget *top : tops)
        top->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QApplication::processEvents();
    QThreadPool::globalInstance()->waitForDone(5000);
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
    ProfileSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
    window->applySearchBoxVisibility();
    window->toolbarSearch()->setText(QLatin1String("arora"));
    webSearch->trigger();
    QVERIFY(window->toolbarSearch()->hasSelectedText());
    ProfileSettings().remove(QLatin1String("MainWindow/showSearchBox"));
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

    // PRINT02: the File menu carries exactly one print entry —
    // "Print..." keeping Ctrl+P and the print icon.  The separate
    // "Print Preview" row is gone; the preview IS the print dialog.
    QCOMPARE(entries.indexOf(QLatin1String("Print Preview...")), -1);
    QCOMPARE(entries.count(QLatin1String("Print...")), 1);
    QAction *printAction = nullptr;
    for (QAction *action : fileMenu->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Print..."))
            printAction = action;
    }
    QVERIFY(printAction);
    QCOMPARE(printAction->shortcut(),
             QKeySequence(QKeySequence::Print));
    QVERIFY(!printAction->icon().isNull());
    closeWindow(window);
}

// ICONS07: every static leaf action in the shared top-level menus
// carries an icon — the app-menu dropdown reads as one uniform icon
// column.  ICONS08: the STATIC submenu headers that sit among those
// leafs carry icons too (File > New Container Tab, Tools >
// Development Tools, View > Text Encoding, Tools > User Agent,
// History > Recently Closed Tabs).  Documented exceptions, asserted here too:
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
    ProfileSettings iconSettings;
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
    for (QAction *action : menus.value(QLatin1String("Tools"))->actions()) {
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

    // Tools > Development Tools is a static submenu.
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
    // MENU13: icon-only; the text property stays "Arora" for screen
    // readers.
    QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(button->text(), QLatin1String("Arora"));
    QVERIFY(!button->icon().isNull());
    // MENU16: the MENU09 host + fixed 36px extent that rendered the
    // logo oversized are gone; the button is a direct bar child now.
    // MENU17/MENU19: brand accent — its glyph rides ~60% over the
    // bar's shared icon size (18 -> 28.8 -> 29) in a still-square
    // cell, while the toolbar's own action buttons keep the shared
    // size.
    QCOMPARE(button->iconSize().width(), button->iconSize().height());
    QVERIFY(qAbs(button->iconSize().height()
                 - navBar->iconSize().height() * 1.6) <= 1.0);
    QToolButton *sibling = qobject_cast<QToolButton *>(
        navBar->widgetForAction(navBar->actions().at(1)));
    QVERIFY(sibling);
    QCOMPARE(sibling->iconSize(), navBar->iconSize());
    // The bigger glyph fills the face instead of stretching the row:
    // the face caps at a square one pixel over the glyph, and the
    // bar's compacted vertical margins hand back the pixels so the
    // row keeps the height the location field drove.
    QVERIFY(button->sizeHint().height() > sibling->sizeHint().height());
    QCOMPARE(button->sizeHint().width(), button->sizeHint().height());
    QVERIFY(button->sizeHint().height()
            <= button->iconSize().height() + 1);
    QLineEdit *navLocationBar = window->tabWidget()->currentLocationBar();
    QVERIFY(navLocationBar);
    QVERIFY(navBar->sizeHint().height()
            <= navLocationBar->minimumHeight() + 8);
    QVERIFY(!button->icon().pixmap(button->iconSize()).isNull());
    QVERIFY(!button->toolTip().isEmpty());
    QVERIFY(!button->accessibleName().isEmpty());

    // Bar-sized glyph, intact hit-rect — the button is no smaller
    // than the icon it paints.
    QVERIFY(button->sizeHint().height()
            >= button->iconSize().height());
    QVERIFY(button->size().height()
            >= button->iconSize().height());

    // First item on the nav bar, ahead of Back — the action carries
    // the button itself (MENU16 removed the host wrapper).
    QVERIFY(!navBar->actions().isEmpty());
    QCOMPARE(navBar->widgetForAction(navBar->actions().first()),
             button);

    // MENU16: the trailing 'Arora' wordmark is gone — no brand label
    // child anywhere, and every bar action is a real movable item
    // carrying a navBarItemId (the unlisted fixed-tail slot no
    // longer exists).
    QVERIFY(!window->findChild<QLabel *>(
        QLatin1String("navBarBrandLabel")));
    for (QAction *action : navBar->actions())
        QVERIFY2(!action->property("navBarItemId")
                     .toString().isEmpty(),
                 qPrintable(action->text()));
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
    QToolBar *privateBar = privateWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    const bool privateMenuFull = privateButton
        && privateButton->menu()
        && privateButton->menu()->actions().count() >= 8;
    // MENU19: every window type gets the same ~1.6x brand glyph.
    const bool privateBarIcon = privateButton && privateBar
        && qAbs(privateButton->iconSize().height()
                - privateBar->iconSize().height() * 1.6) <= 1.0
        && privateButton->iconSize().width()
            == privateButton->iconSize().height();
    const bool privateBarHidden =
        !privateWindow->menuBar()->isVisible();
    // MENU08: and the same enlarged menu text.
    const bool privateMenuFont = privateButton
        && privateButton->menu()
        && privateButton->menu()->font() == menuFont;
    // MENU16: the private bar carries the same icon-only button and
    // no wordmark label.
    const bool privateNoWordmark = privateButton
        && privateButton->toolButtonStyle() == Qt::ToolButtonIconOnly
        && !privateWindow->findChild<QLabel *>(
            QLatin1String("navBarBrandLabel"));
    closeWindow(privateWindow);
    BrowserApplication::setPrivate(false);
    QVERIFY(privateButton);
    QVERIFY(privateMenuFull);
    QVERIFY(privateBarIcon);
    QVERIFY(privateBarHidden);
    QVERIFY(privateMenuFont);
    QVERIFY(privateNoWordmark);

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
    QToolBar *torNavBar = torWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
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
    // MENU19: the tor button follows the same ~1.6x accent — the
    // violet bar keeps the row height too (the stylesheet's
    // StyleChange rebuilds the layout margins and NavBarToolBar
    // re-compacts them).
    const bool torBarIcon = torButton && torNavBar
        && qAbs(torButton->iconSize().height()
                - torNavBar->iconSize().height() * 1.6) <= 1.0
        && torButton->iconSize().width()
            == torButton->iconSize().height();
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
    // MENU16: no wordmark on tor either — and every bar action is a
    // real item carrying a navBarItemId.
    const bool torNoBrand = torNavBar
        && !torWindow->findChild<QLabel *>(
            QLatin1String("navBarBrandLabel"));
    bool torAllIded = torNavBar != nullptr;
    if (torNavBar)
        for (QAction *action : torNavBar->actions())
            torAllIded = torAllIded
                && !action->property("navBarItemId")
                        .toString().isEmpty();
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
    QVERIFY(torBarIcon);
    QCOMPARE(torMenuFont, menuFont);
    QVERIFY(torRowVisible);
    QVERIFY(torRowBare);
    QCOMPARE(torMenuCount, 13);
    QVERIFY(torPrefsLeaf);
    QVERIFY(torQuitLast);
    QVERIFY(torNoBrand);
    QVERIFY(torAllIded);
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

// DEVT04/DEVT05/DEVT08: Tools > Development Tools holds the engine-
// tool entries — "Chromium Dev Tool" (the working inspector) and,
// under rustcore, "BiDi Dev &Tools".  Both are plain actions —
// devtools rows are open/close commands, so no checkable ticks.
// The View menu keeps Page Source + Text Encoding and carries no
// dev-tools entry.
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

    // The View menu no longer hosts the submenu — Page Source is
    // followed straight by Text Encoding.
    const QList<QAction *> viewActions = viewMenu->actions();
    int sourceIndex = -1;
    for (int i = 0; i < viewActions.size(); ++i) {
        QAction *action = viewActions.at(i);
        if (action->menu())
            QVERIFY2(action->menu()->title()
                         != QLatin1String("Development Tools"),
                     qPrintable(action->menu()->title()));
        QVERIFY2(!action->text().contains(QLatin1String("Dev Tool")),
                 qPrintable(action->text()));
        QVERIFY2(!action->text().contains(QLatin1String("Inspect")),
                 qPrintable(action->text()));
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Page Source"))
            sourceIndex = i;
    }
    QVERIFY(sourceIndex != -1);
    QVERIFY(sourceIndex + 2 < viewActions.size());
    QVERIFY(viewActions.at(sourceIndex + 1)->isSeparator());
    QCOMPARE(viewActions.at(sourceIndex + 2)
                 ->text().remove(QLatin1Char('&')),
             QLatin1String("Text Encoding"));

    // The submenu lives in the Tools menu's utility cluster — after
    // Clear Private Data, before User Agent.
    QMenu *devToolsMenu = nullptr;
    int devToolsIndex = -1;
    int clearIndex = -1;
    int userAgentIndex = -1;
    const QList<QAction *> toolsActions = toolsMenu->actions();
    for (int i = 0; i < toolsActions.size(); ++i) {
        QAction *action = toolsActions.at(i);
        if (action->menu()
            && action->menu()->title()
                   == QLatin1String("Development Tools")) {
            devToolsMenu = action->menu();
            devToolsIndex = i;
        }
        if (action->menu()
            && action->menu()->title() == QLatin1String("User Agent"))
            userAgentIndex = i;
        if (action->text().remove(QLatin1Char('&'))
                == QLatin1String("Clear Private Data"))
            clearIndex = i;
    }
    QVERIFY(devToolsMenu);
    QVERIFY(clearIndex != -1);
    QVERIFY(userAgentIndex != -1);
    QVERIFY(devToolsIndex > clearIndex);
    QVERIFY(devToolsIndex < userAgentIndex);

    // The Tools menu itself carries no BiDi surface — the only BiDi
    // entry lives inside the submenu.
    for (QAction *action : toolsActions) {
        QVERIFY2(!action->text().contains(QLatin1String("BiDi")),
                 qPrintable(action->text()));
    }

    // The submenu holds the live inspector plus the BiDi dock
    // entry — every row a plain action, and no separators.
    QStringList entries;
    QAction *chromium = nullptr;
    QAction *bidi = nullptr;
    for (QAction *action : devToolsMenu->actions()) {
        QVERIFY2(!action->isSeparator(), qPrintable(action->text()));
        QVERIFY2(!action->isCheckable(), qPrintable(action->text()));
        entries << action->text();
        if (action->text() == QLatin1String("Chromium Dev Tool"))
            chromium = action;
        if (action->text() == QLatin1String("BiDi Dev &Tools"))
            bidi = action;
    }
    QVERIFY(chromium);
    QVERIFY(chromium->isEnabled());
    QVERIFY(!toolsMenu->actions().constLast()->isSeparator());

#if defined(ARORA_RUSTCORE)
    QCOMPARE(entries.size(), 2);
    QVERIFY(bidi);
    QVERIFY(bidi->isEnabled());
    // DEVT08: neither devtools entry is checkable — these are
    // open/close actions, not persistent states.
    QVERIFY(!bidi->isCheckable());
    QVERIFY(!chromium->isCheckable());

    // The BiDi entry is a plain command that still toggles the dock:
    // trigger opens it, trigger again closes it — and a non-checkable
    // action can never strand a checked state after close.
    QDockWidget *dock = window->findChild<QDockWidget *>(
        QLatin1String("bidiPanelDock"));
    QVERIFY(dock);
    QVERIFY(!dock->isVisible());
    bidi->trigger();
    QTRY_VERIFY(dock->isVisible());
    QVERIFY(!bidi->isChecked());

    // SIDE05: exactly one title bar — the lazily-built panel inside
    // is plain dock content, never a nested QDockWidget drawing a
    // second frame of its own.
    QVERIFY(dock->widget());
    QVERIFY(!qobject_cast<QDockWidget *>(dock->widget()));
    QVERIFY(dock->findChildren<QDockWidget *>().isEmpty());

    bidi->trigger();
    QTRY_VERIFY(!dock->isVisible());
    closeWindow(window);

    // A tor window keeps the entry visible but disabled, with a
    // tooltip explaining why.  Tor strips the menubar's top-level
    // actions while the bar doubles as the drawn title bar — the
    // same QMenu objects then live under the app-menu button.
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->show();
    QList<QAction *> torTopActions = torWindow->menuBar()->actions();
    if (torTopActions.isEmpty() && torWindow->appMenu())
        torTopActions = torWindow->appMenu()->actions();
    QAction *torBidi = nullptr;
    for (QAction *menuAction : torTopActions) {
        if (!menuAction->menu()
            || !menuAction->text().contains(QLatin1String("Tools")))
            continue;
        for (QAction *sub : menuAction->menu()->actions()) {
            if (!sub->menu()
                || sub->menu()->title()
                       != QLatin1String("Development Tools"))
                continue;
            for (QAction *action : sub->menu()->actions()) {
                if (action->text() == QLatin1String("BiDi Dev &Tools"))
                    torBidi = action;
            }
        }
    }
    BrowserApplication::setTorMode(false);
    QVERIFY(torBidi);
    QVERIFY(!torBidi->isEnabled());
    QVERIFY(!torBidi->isCheckable());
    QVERIFY(!torBidi->toolTip().isEmpty());
    closeWindow(torWindow);
#else
    QCOMPARE(entries.size(), 1);
    closeWindow(window);
#endif
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

    ProfileSettings settings;
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

    // printRequested needs a live page; PRINT02 merged the paths so
    // the preview dialog comes up — rejected like any modal.  A
    // repeating dismisser beats the dialog's variable startup — a
    // one-shot rejectModal() can fire before it becomes the active
    // modal and strand it open forever.
    if (window->currentTab() && window->currentTab()->webPage()) {
        QTimer modal;
        modal.setInterval(50);
        QObject::connect(&modal, &QTimer::timeout, qApp, []() {
            if (QDialog *dialog = qobject_cast<QDialog *>(
                    QApplication::activeModalWidget()))
                dialog->reject();
        });
        modal.start();
        QVERIFY(QMetaObject::invokeMethod(window, "printRequested",
                       Q_ARG(WebView *, window->currentTab())));
        modal.stop();
    }

    // Last-tab-closed closes the window (and, via WA_DeleteOnClose,
    // schedules its deletion) — leave this last.
    QVERIFY(QMetaObject::invokeMethod(window, "lastTabClosed"));
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// CRASH02+PRINT02: the print paths must never kill the app, and
// there is exactly one of them — Ctrl+P, the File-menu entry and JS
// window.print() (printRequested) all open the preview dialog, whose
// own Print button runs the final QPrintDialog -> raster print.  On
// a host with zero printers an ACCEPTED print dialog still describes
// an unprintable target (file-format printer pointed at a
// directory) — the handoff validates the target and warns instead of
// painting into a dead printer.
void tst_BrowserMainWindow::printPaths()
{
    SubWindow *window = new SubWindow;
    window->show();
    WebView *view = window->currentTab();
    QVERIFY(view);

    const bool noPrinters =
        QPrinterInfo::availablePrinterNames().isEmpty();

    // Leg 1 — the merged entry: printRequested (JS window.print(),
    // Ctrl+P, the File-menu item all land here) opens the preview;
    // the generated document must actually arrive — the stock
    // QPrintPreviewDialog starved to "1 / 0" pages, so the regression
    // assertion is real pages (about:blank makes a one-page doc).
    bool sawPreview = false;
    int previewedPages = 0;
    int pumpTicks = 0;
    QString previewFailure;
    QTimer modal;
    modal.setInterval(50);
    QObject::connect(&modal, &QTimer::timeout, qApp, [&]() {
        QWidget *widget = QApplication::activeModalWidget();
        if (QMessageBox *box = qobject_cast<QMessageBox *>(widget)) {
            box->accept();
            return;
        }
        PrintPreviewDialog *dialog =
            qobject_cast<PrintPreviewDialog *>(widget);
        if (!dialog) {
            if (QDialog *other = qobject_cast<QDialog *>(widget))
                other->reject();
            return;
        }
        sawPreview = true;
        ++pumpTicks;
        if (dialog->hasFailed()) {
            previewFailure = dialog->failureMessage();
            dialog->accept();
            return;
        }
        if (dialog->isReady() && dialog->pdfView()
                && dialog->pdfView()->client()
                && dialog->pdfView()->client()->pageCount() > 0) {
            previewedPages = dialog->pdfView()->client()->pageCount();
            dialog->accept();
        }
        if (pumpTicks > 600)    // ~30s — never wedge the suite
            dialog->accept();
    });
    modal.start();
    QVERIFY(QMetaObject::invokeMethod(window, "printRequested",
                                    Q_ARG(WebView *, view)));
    modal.stop();
    QVERIFY(sawPreview);
    QVERIFY2(previewedPages >= 1,
             qPrintable(QStringLiteral("preview never produced pages")
                        + (previewFailure.isEmpty() ? QString()
                           : QLatin1String(": ") + previewFailure)));

    // Leg 2 — the preview's own Print button runs the native print
    // pipeline on the generated PDF.  A nested exec inside the
    // preview's modal loop starves the dismisser, so the preview is
    // shown NON-modally here: pdfView->printDocument() then owns the
    // only exec — the QPrintDialog it raises is accepted, and on a
    // no-printer host the dead target must warn instead of printing.
    PrintPreviewDialog preview;   // no parent: stack object must
                                  // outlive window teardown safely
    preview.setPage(view->page(), view->url());
    preview.show();
    QTRY_VERIFY_WITH_TIMEOUT(preview.isReady(), 30000);
    QVERIFY(preview.pdfView()->client()
            && preview.pdfView()->client()->pageCount() > 0);

    bool sawPrintDialog = false;
    bool sawWarning = false;
    QTimer printPump;
    printPump.setInterval(50);
    QObject::connect(&printPump, &QTimer::timeout, qApp, [&]() {
        QWidget *widget = QApplication::activeModalWidget();
        if (QMessageBox *box = qobject_cast<QMessageBox *>(widget)) {
            sawWarning = true;
            box->accept();
            return;
        }
        if (QPrintDialog *printDialog =
                qobject_cast<QPrintDialog *>(widget)) {
            sawPrintDialog = true;
            printDialog->accept();
            return;
        }
        if (QDialog *other = qobject_cast<QDialog *>(widget))
            other->reject();
    });
    printPump.start();
    QVERIFY(QMetaObject::invokeMethod(preview.pdfView(),
                                    "printDocument"));
    printPump.stop();
    QVERIFY(sawPrintDialog);
    if (noPrinters) {
        // The accepted dialog described a dead target: refusal must
        // have warned instead of printing.
        QVERIFY(sawWarning);
    }

    // The merged pipeline's last mile, host-independent: printPages
    // must produce a real print artifact, honoring the QPrintDialog
    // page range (the only print pipeline left — nothing may be
    // lost with the old straight-to-printer path).
    QTemporaryDir outDir;
    QVERIFY(outDir.isValid());
    const QString outPath = outDir.filePath("print.pdf");
    QPrinter printer(QPrinter::HighResolution);
    printer.setOutputFormat(QPrinter::PdfFormat);
    printer.setOutputFileName(outPath);
    QString printError;
    QVERIFY(preview.pdfView()->canvas()->printPages(&printer,
                                                  &printError));
    QVERIFY2(QFile::exists(outPath), qPrintable(printError));
    QFile out(outPath);
    QVERIFY(out.open(QIODevice::ReadOnly));
    QVERIFY(out.read(5).startsWith("%PDF-"));
    out.close();

    const QString rangedPath = outDir.filePath("print-ranged.pdf");
    printer.setPrintRange(QPrinter::PageRange);
    printer.setFromTo(1, 1);
    printer.setOutputFileName(rangedPath);
    QVERIFY(preview.pdfView()->canvas()->printPages(&printer,
                                                  &printError));
    QVERIFY(QFile::exists(rangedPath));
    preview.close();

    // filePrint — the menu/Ctrl+P slot — opens the same preview; a
    // prompt generic accept lands while a fresh printToPdf may still
    // be in flight — exactly the mid-generation close case.
    QTimer earlyClose;
    earlyClose.setInterval(50);
    QObject::connect(&earlyClose, &QTimer::timeout, qApp, []() {
        if (QDialog *dialog = qobject_cast<QDialog *>(
                QApplication::activeModalWidget()))
            dialog->accept();
    });
    earlyClose.start();
    QVERIFY(QMetaObject::invokeMethod(window, "filePrint"));
    earlyClose.stop();

    // PRINT01/02: an engine tab has no printToPdf — Print must
    // refuse honestly rather than open a dead dialog or do nothing.
    FakeEngineBackend backend;
    EngineRegistry::registerBackend(&backend);
    QVERIFY(window->tabWidget()->reloadTabInEngine(
        0, QStringLiteral("fake")));
    QVERIFY(window->tabWidget()->engineTab(0));
    QVERIFY(!window->currentTab());

    bool sawRefusal = false;
    QTimer refusalPump;
    refusalPump.setInterval(50);
    QObject::connect(&refusalPump, &QTimer::timeout, qApp, [&]() {
        QWidget *widget = QApplication::activeModalWidget();
        if (QMessageBox *box = qobject_cast<QMessageBox *>(widget)) {
            sawRefusal = true;
            box->accept();
        } else if (QDialog *dialog = qobject_cast<QDialog *>(widget)) {
            dialog->reject();
        }
    });
    refusalPump.start();
    QVERIFY(QMetaObject::invokeMethod(window, "filePrint"));
    QVERIFY(sawRefusal);
    refusalPump.stop();

    closeWindow(window);
    EngineRegistry::unregisterBackend(&backend);
}

// With confirmClosingMultipleTabs on, closeEvent() raises a Yes/No
// warning box — cover both answers.
void tst_BrowserMainWindow::closeConfirm()
{
    ProfileSettings().setValue(QLatin1String("tabs/confirmClosingMultipleTabs"), true);

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

    ProfileSettings().setValue(QLatin1String("tabs/confirmClosingMultipleTabs"), false);
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
    // MENU19: the grown app-menu face borrows 2px of vertical chrome
    // margin on each side — the layout's own padding runs 4 -> 2 so
    // the row keeps its height.  The breathing-room contract moves
    // to the row itself: the bar still clears the field by the old
    // chrome depth (asserted below once the field is in hand), and a
    // style reset can never push the margins back out.
    QVERIFY(margins.top() + layoutMargins.top() >= 2);
    QVERIFY(margins.bottom() + layoutMargins.bottom() >= 2);

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
    // MENU19: the row still clears the field by the old chrome depth
    // — the grown app-menu face took the layout margin's room, it
    // did not grow the bar.
    QVERIFY(navBar->sizeHint().height()
            - locationBar->minimumHeight() >= 4);

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
    ProfileSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
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
    ProfileSettings().remove(QLatin1String("MainWindow/showSearchBox"));
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
    ProfileSettings().setValue(QLatin1String("MainWindow/showSearchBox"), true);
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
    ProfileSettings().remove(QLatin1String("MainWindow/showSearchBox"));

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
    ProfileSettings settings;
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
// dark-on-dark while Qt's generated Disabled ghosting made the
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

    ProfileSettings settings;
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
    // TRUST18: the plain-window shield keeps its dark chrome glyph —
    // the palette-driven tint must not change normal surfaces.
    QToolButton *plainShield = plainBar->findChild<QToolButton *>(
        QLatin1String("navSiteShield"));
    QVERIFY(plainShield);
    const qlonglong plainShieldLum = luminance(plainShield->icon());
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
        qlonglong shield = -1;
        qlonglong shieldDisabled = -1;
        qlonglong reader = -1;
        qlonglong circuit = -1;
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
            // TRUST18: the palette-painted and getChrome nav-bar
            // widgets must read light on the violet bar too — the
            // shield regressed to the app's dark ButtonText because
            // getChrome resolves the application palette, not the
            // widget surface's.
            QToolButton *shield = navBar->findChild<QToolButton *>(
                QLatin1String("navSiteShield"));
            if (shield) {
                out.shield = luminance(shield->icon());
                out.shieldDisabled = luminance(shield->icon(),
                                               QIcon::Disabled);
            }
            QToolButton *reader = navBar->findChild<QToolButton *>(
                QLatin1String("navReaderButton"));
            if (reader)
                out.reader = luminance(reader->icon());
            QToolButton *circuit = navBar->findChild<QToolButton *>(
                QLatin1String("torCircuitButton"));
            if (circuit)
                out.circuit = luminance(circuit->icon());
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

    // A second bundled family proves the tint is theme-independent
    // (ICONS12: the old "native" path this leg used to exercise is
    // retired — every selectable set is bundled now).
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("breeze"));
    AroraIcon::applyFromSettings();
    const TorNavLum breezeTheme = measureTorBar("breeze");

    BrowserTheme::applyColorScheme();
    if (savedTheme.isValid())
        settings.setValue(QLatin1String("MainWindow/iconTheme"),
                          savedTheme);
    AroraIcon::applyFromSettings();

    // Light-palette normal window: dark glyphs.  Tor window: light
    // glyphs — readable on the violet bar under any theme.
    QVERIFY(plainBack >= 0 && plainBack < 140);
    QVERIFY(plainReload >= 0 && plainReload < 140);
    QVERIFY(plainShieldLum >= 0 && plainShieldLum < 140);
    for (const TorNavLum &lum : { bundled, breezeTheme }) {
        QVERIFY(lum.barFound);
        QVERIFY(lum.back > 140);
        QVERIFY(lum.forward > 140);
        QVERIFY(lum.reload > 140);
        QVERIFY(lum.home > 140);
        QVERIFY(lum.sidebar > 140);
        // TRUST18: the widget-hosted glyphs — site shield (Normal
        // and Disabled modes), reader toggle, circuit onion — stay
        // light on the violet bar under every bundled theme.
        QVERIFY(lum.shield > 140);
        QVERIFY(lum.shieldDisabled > 140);
        QVERIFY(lum.reader > 140);
        QVERIFY(lum.circuit > 140);
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

// TRUST20: two same-class reports — the reader glyph reverted to
// black on a state change (a stylesheet repolish restores then
// re-applies the widget palette, and a refresh() landing in that
// transient window baked the unthemed tone), and the sidebar rail
// icons painted unthemed dark glyphs.  Both now rebake on palette/
// style settles and the rail resolves its own (violet-seeded)
// palette.  A file:// article bypasses the fail-closed tor proxy, so
// the real probe-verdict flip drives refresh() here.
void tst_BrowserMainWindow::torChromeGlyphsLegible()
{
    const auto luminance = [](const QIcon &icon,
                              QIcon::Mode mode = QIcon::Normal)
            -> qlonglong {
        const QImage image = icon.pixmap(QSize(18, 18), mode).toImage();
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
    const auto isViolet = [](const QColor &c) {
        return c.isValid() && c.blue() > c.red()
            && c.red() > c.green() && c.lightness() < 160;
    };

    ProfileSettings settings;
    const QVariant savedTheme =
        settings.value(QLatin1String("MainWindow/iconTheme"));
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("adwaita"));
    AroraIcon::applyFromSettings();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Light);

    // Normal-window baseline: the rail keeps the app palette's dark
    // glyphs on its light strip — the palette-aware icon path must
    // not change non-tor chrome.
    SubWindow *plain = new SubWindow;
    plain->resize(1000, 600);
    plain->show();
    plain->sidebarDock()->toggleViewAction()->trigger();
    QApplication::processEvents();
    SidebarPanel *plainPanel = plain->sidebarPanel();
    QVERIFY(plainPanel);
    const int plainRailLightness = plainPanel->tabs()->tabBar()
        ->palette().color(QPalette::Window).lightness();
    QList<qlonglong> plainRailLums;
    for (int i = 0; i < plainPanel->tabs()->count(); ++i)
        plainRailLums << luminance(
            plainPanel->tabs()->tabBar()->tabIcon(i));
    closeWindow(plain);

    // An article-shaped file — Readability's probe wants a title plus
    // several dense paragraphs before it reports a verdict.
    const QString articlePath =
        QDir::temp().filePath(QLatin1String("arora-trust20-article.html"));
    {
        QFile article(articlePath);
        QVERIFY(article.open(QIODevice::WriteOnly));
        article.write("<html><head><title>An article</title></head>"
                      "<body><article><h1>An article</h1>");
        for (int i = 0; i < 12; ++i)
            article.write("<p>Lorem ipsum dolor sit amet, consectetur "
                          "adipiscing elit, sed do eiusmod tempor "
                          "incididunt ut labore et dolore magna aliqua. "
                          "Ut enim ad minim veniam, quis nostrud "
                          "exercitation ullamco laboris nisi ut aliquip "
                          "ex ea commodo consequat.</p>");
        article.write("</article></body></html>");
    }

    BrowserApplication::setTorMode(true);
    SubWindow *window = new SubWindow;
    window->resize(1100, 700);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    ReaderButton *reader = navBar->findChild<ReaderButton *>(
        QLatin1String("navReaderButton"));
    QVERIFY(reader);
    WebView *view = window->currentTab();
    QVERIFY(view);

    window->sidebarDock()->toggleViewAction()->trigger();
    QApplication::processEvents();
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QTabBar *rail = panel->tabs()->tabBar();
    QVERIFY(rail->count() > 0);
    QToolButton *fold = rail->findChild<QToolButton *>(
        QLatin1String("sidebarPanelToggle"));
    QVERIFY(fold);

    const auto railLums = [&]() -> QList<qlonglong> {
        QList<qlonglong> lums;
        for (int i = 0; i < rail->count(); ++i)
            lums << luminance(rail->tabIcon(i));
        return lums;
    };

    // THEM06: once the article verdict lands the glyph honestly wears
    // the accent mark — the tor raised violet — which is darker than
    // the chrome text the plain-state assert gates on (and so was the
    // stray app-blue it replaced).  A flat luminance assert read the
    // legitimate accent as a stale dark bake whenever the verdict won
    // the race against the measurement, so the reader checks below
    // are state-aware: a suggested glyph's dominant ink must BE the
    // accent (the button's override when one is set, else the palette
    // accent), a plain glyph must stay light.
    const auto readerInkOk = [&reader]() -> bool {
        const QImage image =
            reader->icon().pixmap(QSize(18, 18)).toImage();
        QHash<QRgb, int> counts;
        int best = 0;
        QColor ink;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 200) {
                    const int n = ++counts[color.rgb()];
                    if (n > best) {
                        best = n;
                        ink = color;
                    }
                }
            }
        if (!ink.isValid())
            return false;
        if (reader->property("suggested").toBool()) {
            const QColor accent = reader->accentOverride().isValid()
                ? reader->accentOverride()
                : reader->palette().color(QPalette::Accent);
            return qAbs(ink.red() - accent.red()) <= 2
                && qAbs(ink.green() - accent.green()) <= 2
                && qAbs(ink.blue() - accent.blue()) <= 2;
        }
        return qGray(ink.rgb()) > 140;
    };

    // At open: reader glyph on-theme, rail strip violet with light
    // glyphs, fold toggle light.
    const bool readerOpen = readerInkOk();
    const QList<qlonglong> railOpen = railLums();
    const qlonglong railOpenSelected =
        luminance(rail->tabIcon(0), QIcon::Selected);
    const qlonglong foldOpen = luminance(fold->icon());
    const QColor railBackground =
        rail->grab().toImage().pixelColor(2, 2);

    // The real verdict path: a file:// page needs no tor circuit —
    // loadFinished probes Readability and availableChanged drives the
    // "suggested" accent on the always-visible button (READ04),
    // exactly the mid-scroll refresh the report caught.
    view->load(QUrl::fromLocalFile(articlePath));
    QTRY_VERIFY_WITH_TIMEOUT(view->readerMode()->isAvailable(), 20000);
    const bool readerVerdict = readerInkOk();

    // Rebind/unbind — the tab-switch refresh path.
    reader->setWebView(nullptr);
    reader->setWebView(view);
    const bool readerRebind = readerInkOk();

    // Nav-bar edit mode re-setStyleSheet()s the bar — the repolish
    // window where the transient dark palette used to bake in.  A
    // refresh inside the settling window must still land on-theme (or
    // be rebaked by the PaletteChange that follows).
    window->navBarEditAction()->trigger();
    QApplication::processEvents();
    reader->setWebView(nullptr);
    reader->setWebView(view);
    const bool readerEditOn = readerInkOk();
    window->navBarEditAction()->trigger();
    QApplication::processEvents();
    reader->setWebView(nullptr);
    reader->setWebView(view);
    const bool readerEditOff = readerInkOk();
    const QList<qlonglong> railAfterEdit = railLums();
    const qlonglong foldAfterEdit = luminance(fold->icon());

    // A scheme flip mid-flight — queued scheme signal plus palette
    // propagation both hit the button and the rail.
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    reader->setWebView(nullptr);
    reader->setWebView(view);
    const bool readerDark = readerInkOk();
    const QList<qlonglong> railDark = railLums();
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Light);
    QApplication::processEvents();
    reader->setWebView(nullptr);
    reader->setWebView(view);
    const bool readerBackLight = readerInkOk();
    const QList<qlonglong> railBackLight = railLums();

    // Offscreen grab for visual review — ARORA_TOR_GRAB_DIR picks up
    // the rail + reader states.
    const QByteArray grabDir = qgetenv("ARORA_TOR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        rail->grab().save(QString::fromLocal8Bit(grabDir)
                          + QLatin1String("/tor-sidebar-rail.png"));
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/tor-nav-reader.png"));
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
    QFile::remove(articlePath);

    // Normal window unchanged: dark glyphs on a light strip.
    QVERIFY(plainRailLightness > 160);
    for (const qlonglong lum : plainRailLums)
        QVERIFY(lum >= 0 && lum < 140);

    // Tor: the rail strip is violet chrome and every glyph on it —
    // tab icons in both modes, the fold toggle — reads light.
    for (const qlonglong lum : railOpen)
        QVERIFY(lum > 140);
    QVERIFY(railOpenSelected > 140);
    QVERIFY(foldOpen > 140);
    QVERIFY(isViolet(railBackground));
    // Reader: on-theme at open, and still on-theme through the probe
    // verdict, tab rebinding, the edit-mode repolish and a live
    // scheme flip — no path may leave a stale dark bake, and the
    // suggested mark must come out of the tor accent, not the app
    // palette's blue.
    QVERIFY(readerOpen);
    QVERIFY(readerVerdict);
    QVERIFY(readerRebind);
    QVERIFY(readerEditOn);
    QVERIFY(readerEditOff);
    QVERIFY(readerDark);
    QVERIFY(readerBackLight);
    for (const qlonglong lum : railAfterEdit)
        QVERIFY(lum > 140);
    QVERIFY(foldAfterEdit > 140);
    for (const qlonglong lum : railDark)
        QVERIFY(lum > 140);
    for (const qlonglong lum : railBackLight)
        QVERIFY(lum > 140);
}

// THEM06: the reader glyph's accent states resolve QPalette::Accent —
// on the tor window that stayed the app palette's blue, so the
// suggested mark and the active page fill painted a stray blue on the
// violet bar.  The tor palette seed now hands the engine the raised
// violet accent — the family the badge pill and the sidebar rail's
// selection already wear — while a normal window keeps its own
// surface accent.  A file:// article drives the real Readability
// verdict so the suggested state is honest; a click exercises the
// active fill.
void tst_BrowserMainWindow::torReaderAccent()
{
    const auto dominantInk = [](const QIcon &icon, const QSize &size)
            -> QColor {
        const QImage image = icon.pixmap(size).toImage();
        QHash<QRgb, int> counts;
        int best = 0;
        QColor dominant;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 200) {
                    const int n = ++counts[color.rgb()];
                    if (n > best) {
                        best = n;
                        dominant = color;
                    }
                }
            }
        if (!dominant.isValid())
            return dominant;
        return QColor(dominant.red(), dominant.green(), dominant.blue());
    };
    const auto inkMatch = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 2
            && qAbs(a.green() - b.green()) <= 2
            && qAbs(a.blue() - b.blue()) <= 2;
    };

    const QString articlePath =
        QDir::temp().filePath(QLatin1String("arora-them06-article.html"));
    {
        QFile article(articlePath);
        QVERIFY(article.open(QIODevice::WriteOnly));
        article.write("<html><head><title>An article</title></head>"
                      "<body><article><h1>An article</h1>");
        for (int i = 0; i < 12; ++i)
            article.write("<p>Lorem ipsum dolor sit amet, consectetur "
                          "adipiscing elit, sed do eiusmod tempor "
                          "incididunt ut labore et dolore magna aliqua. "
                          "Ut enim ad minim veniam, quis nostrud "
                          "exercitation ullamco laboris nisi ut aliquip "
                          "ex ea commodo consequat.</p>");
        article.write("</article></body></html>");
    }
    const QSize iconSize(18, 18);

    // Normal window: the suggested mark keeps the surface's own
    // accent — whatever the platform palette resolves, unchanged.
    QColor plainSuggested;
    QColor plainAccent;
    {
        SubWindow *plain = new SubWindow;
        plain->resize(1000, 600);
        plain->show();
        QApplication::processEvents();
        ReaderButton *reader = plain->findChild<ReaderButton *>(
            QLatin1String("navReaderButton"));
        QVERIFY(reader);
        WebView *tab = plain->currentTab();
        ReaderMode *mode = tab->readerMode();
        QVERIFY(mode);
        tab->loadUrl(QUrl::fromLocalFile(articlePath));
        QTRY_VERIFY_WITH_TIMEOUT(mode->isAvailable(), 20000);
        QVERIFY(reader->property("suggested").toBool());
        plainSuggested = dominantInk(reader->icon(), iconSize);
        plainAccent = reader->palette().color(QPalette::Accent);
        closeWindow(plain);
    }

    // Tor window: the same mark must come out of the tor accent
    // family — the override the window installs, then the painted
    // pixels of both accent states.
    QColor torSuggested;
    QColor torActive;
    QColor torAccentOverride;
    // A leg that fails mid-test would otherwise leak tor mode into
    // every later test in the process.
    struct TorGuard {
        ~TorGuard() { BrowserApplication::setTorMode(false); }
    } torGuard;
    BrowserApplication::setTorMode(true);
    {
        SubWindow *window = new SubWindow;
        window->resize(1000, 600);
        window->show();
        QApplication::processEvents();
        ReaderButton *reader = window->findChild<ReaderButton *>(
            QLatin1String("navReaderButton"));
        QVERIFY(reader);
        torAccentOverride = reader->accentOverride();
        WebView *tab = window->currentTab();
        ReaderMode *mode = tab->readerMode();
        QVERIFY(mode);
        tab->loadUrl(QUrl::fromLocalFile(articlePath));
        QTRY_VERIFY_WITH_TIMEOUT(mode->isAvailable(), 20000);
        QVERIFY(reader->property("suggested").toBool());
        torSuggested = dominantInk(reader->icon(), iconSize);
        // The active state fills the page with the accent — its
        // dominant ink is the fill color itself.
        reader->click();
        QTRY_VERIFY_WITH_TIMEOUT(mode->isActive(), 20000);
        torActive = dominantInk(reader->icon(), iconSize);
        reader->click();
        QTRY_VERIFY_WITH_TIMEOUT(!mode->isActive(), 20000);
        closeWindow(window);
    }
    BrowserApplication::setTorMode(false);
    QFile::remove(articlePath);

    const QColor torAccent = AroraStyle::torChromeRaisedColor();
    QVERIFY2(inkMatch(plainSuggested, plainAccent),
             qPrintable(QStringLiteral("plain suggested %1 vs accent %2")
                        .arg(plainSuggested.name())
                        .arg(plainAccent.name())));
    QVERIFY2(inkMatch(torAccentOverride, torAccent),
             qPrintable(QStringLiteral("tor accent override %1 vs %2")
                        .arg(torAccentOverride.name())
                        .arg(torAccent.name())));
    QVERIFY2(inkMatch(torSuggested, torAccent),
             qPrintable(QStringLiteral("tor suggested %1 vs %2")
                        .arg(torSuggested.name()).arg(torAccent.name())));
    QVERIFY2(inkMatch(torActive, torAccent),
             qPrintable(QStringLiteral("tor active %1 vs %2")
                        .arg(torActive.name()).arg(torAccent.name())));
    // The regression pin: the platform accent is a blue (green or
    // blue channel leads red) — the tor mark must read violet
    // (red over green) instead.
    QVERIFY2(torSuggested.red() > torSuggested.green(),
             qPrintable(QStringLiteral("tor suggested not violet: %1")
                        .arg(torSuggested.name())));
    QVERIFY2(torActive.red() > torActive.green(),
             qPrintable(QStringLiteral("tor active not violet: %1")
                        .arg(torActive.name())));
}

// ICONS05: the normal window's chrome under a dark palette.  The
// forced dark palette cannot recolor monochrome SVG art (Qt renders
// currentColor black, never palette-recolored), so the active set
// resolves to its bundled -dark variant — either way the nav
// cluster, the status-bar zoom buttons and the palette-painted
// sidebar glyph must all render light.
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

    // Pin the default bundled set explicitly — under the forced dark
    // palette it must resolve arora-tabler-dark (ICONS12: the
    // "native" id the bug report hit is retired).
    ProfileSettings settings;
    const QVariant savedTheme =
        settings.value(QLatin1String("MainWindow/iconTheme"));
    settings.setValue(QLatin1String("MainWindow/iconTheme"),
                      QLatin1String("tabler"));
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

// TRUST21: the reader glyph is painted (no bundled set ships a
// reader mark) and used to be baked once into a fixed 16px pixmap —
// the nav bar paints buttons at 18px, so the glyph went on screen
// upscaled and blurred: a visibly grey mark next to the crisp
// theme-tinted siblings, starkest on the dark palette.  Its Disabled
// presentation likewise came from Qt's generated ghosting (a
// near-white fade of the white glyph) instead of the palette's
// dimmed tone.  The icon is now an engine that paints at the
// requested size from a palette snapshot with per-mode tints — the
// same contract ChromeIconEngine gives the themed buttons.
//
// The measurement takes the dominant color among the high-alpha
// pixels of the pixmap a request actually returns: that is the
// glyph's ink tone, independent of how thin strokes blend against
// whatever background sits behind the button.
void tst_BrowserMainWindow::darkThemeReaderGlyph()
{
    const auto dominantInk = [](const QIcon &icon, const QSize &size,
                                QIcon::Mode mode = QIcon::Normal)
            -> QColor {
        const QImage image = icon.pixmap(size, mode).toImage();
        QHash<QRgb, int> counts;
        int best = 0;
        QColor dominant;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor color = image.pixelColor(x, y);
                if (color.alpha() > 200) {
                    const int n = ++counts[color.rgb()];
                    if (n > best) {
                        best = n;
                        dominant = color;
                    }
                }
            }
        // The glyph hue is what must match the siblings — anti-alias
        // coverage makes the dominant edge pixel's alpha differ by a
        // point or two between glyph shapes, so it is stripped here.
        if (!dominant.isValid())
            return dominant;
        return QColor(dominant.red(), dominant.green(),
                      dominant.blue());
    };
    const auto inkMatch = [](const QColor &a, const QColor &b) {
        return qAbs(a.red() - b.red()) <= 2
            && qAbs(a.green() - b.green()) <= 2
            && qAbs(a.blue() - b.blue()) <= 2;
    };

    SubWindow *window = new SubWindow;
    window->resize(1000, 600);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    ReaderButton *reader = navBar->findChild<ReaderButton *>(
        QLatin1String("navReaderButton"));
    QVERIFY(reader);
    QAction *backAction = navBar->actions().at(1);
    QVERIFY(backAction);

    // A file:// article — the real Readability verdict flips the
    // button visible, no server needed.
    const QString articlePath = QDir::temp().filePath(
        QLatin1String("arora-trust21-article.html"));
    {
        QFile article(articlePath);
        QVERIFY(article.open(QIODevice::WriteOnly));
        article.write("<html><head><title>An article</title></head>"
                      "<body><article><h1>An article</h1>");
        for (int i = 0; i < 12; ++i)
            article.write("<p>Lorem ipsum dolor sit amet, consectetur "
                          "adipiscing elit, sed do eiusmod tempor "
                          "incididunt ut labore et dolore magna aliqua. "
                          "Ut enim ad minim veniam, quis nostrud "
                          "exercitation ullamco laboris nisi ut aliquip "
                          "ex ea commodo consequat.</p>");
        article.write("</article></body></html>");
    }
    WebView *tab = window->currentTab();
    ReaderMode *mode = tab->readerMode();
    QVERIFY(mode);
    // Settle on a page that can never probe article-positive —
    // whatever the default start page's verdict may be is immaterial
    // once a real navigation bumps the probe generation.
    tab->loadUrl(QUrl(QLatin1String("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(!mode->isAvailable() && !tab->isLoading(),
                             20000);

    const QSize iconSize(18, 18);

    // Light baseline: the glyph's ink is exactly ButtonText — the
    // same flat tint the chrome-tinted siblings carry — and the icon
    // serves the bar's 18px request at full size (the baked 16px
    // pixmap forced an upscale, which is what made it read grey).
    // Measured BEFORE the article loads: once the Readability verdict
    // lands the glyph honestly wears the Accent "suggested" mark, so
    // the ButtonText contract only holds on a non-article page.
    const QColor lightReader = dominantInk(reader->icon(), iconSize);
    const QColor lightSibling = dominantInk(backAction->icon(),
                                            iconSize);
    const QSize lightServed = reader->icon().pixmap(iconSize).size();

    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    // A leg that fails mid-test would otherwise leak the forced dark
    // palette into every later test in the process (the restore below
    // never runs on QFAIL) — re-resolve on scope exit instead.
    struct PaletteGuard {
        ~PaletteGuard() { BrowserTheme::applyColorScheme(); }
    } paletteGuard;
    QApplication::processEvents();
    QApplication::processEvents();

    const QPalette darkPalette = QApplication::palette();
    const QColor darkTint =
        darkPalette.color(QPalette::Normal, QPalette::ButtonText);
    const QColor darkAccent =
        darkPalette.color(QPalette::Normal, QPalette::Accent);
    const QColor darkDim =
        darkPalette.color(QPalette::Disabled, QPalette::ButtonText);

    // Still on the non-article page: the dark glyph must match the
    // siblings' plain ButtonText ink.
    const QColor darkReader = dominantInk(reader->icon(), iconSize);
    const QColor darkSibling = dominantInk(backAction->icon(),
                                           iconSize);
    const QColor darkReaderDisabled = dominantInk(
        reader->icon(), iconSize, QIcon::Disabled);
    const QColor darkSiblingDisabled = dominantInk(
        backAction->icon(), iconSize, QIcon::Disabled);

    // Active (checked) state keeps its deliberate accent mark — it
    // must not collapse to the plain tint, and the engine still
    // serves it at the requested size.  Load the article and gate on
    // the probe verdict first: a click that outruns the commit enters
    // on the empty url and is honestly declined, and READ04 keeps the
    // button present on every engine tab so isHidden is no real gate.
    // Post-verdict the glyph wears the suggested accent — READ04's
    // availability affordance (accent outline + dot), not the plain
    // ButtonText ink.  The button is checkable so click() flips its
    // own flag instantly; the icon only rebakes on
    // ReaderMode::activeChanged — wait on the mode, not the flag.
    tab->loadUrl(QUrl::fromLocalFile(articlePath));
    QTRY_VERIFY_WITH_TIMEOUT(mode->isAvailable(), 20000);
    QVERIFY(!reader->isHidden());
    const QColor darkSuggested = dominantInk(reader->icon(), iconSize);
    reader->click();
    QTRY_VERIFY_WITH_TIMEOUT(mode->isActive(), 20000);
    const QColor activeInk = dominantInk(reader->icon(), iconSize);
    const QSize activeServed =
        reader->icon().pixmap(iconSize).size();
    reader->click();
    QTRY_VERIFY_WITH_TIMEOUT(!mode->isActive(), 20000);

    const QByteArray grabDir = qgetenv("ARORA_DARK_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/dark-reader-glyph.png"));
    }

    // Live flip back — the glyph re-follows the light palette without
    // a restart or a reload.  Return to a non-article page first so
    // the comparison is plain-ink vs plain-ink: on the article the
    // "suggested" state honestly paints Accent, which is not what the
    // baseline measured.
    tab->loadUrl(QUrl(QLatin1String("about:blank")));
    QTRY_VERIFY_WITH_TIMEOUT(!mode->isAvailable(), 20000);
    BrowserTheme::applyColorScheme();
    QApplication::processEvents();
    QApplication::processEvents();
    const QColor lightReaderAgain = dominantInk(reader->icon(),
                                                iconSize);

    // The suggested affordance under the restored light palette:
    // reload the article and check the accent mark there too.
    tab->loadUrl(QUrl::fromLocalFile(articlePath));
    QTRY_VERIFY_WITH_TIMEOUT(mode->isAvailable(), 20000);
    const QColor lightAccent =
        QApplication::palette().color(QPalette::Normal,
                                      QPalette::Accent);
    const QColor lightSuggested = dominantInk(reader->icon(), iconSize);

    closeWindow(window);
    BrowserTheme::applyColorScheme();
    QFile::remove(articlePath);

    QVERIFY2(inkMatch(lightReader, lightSibling),
             qPrintable(QStringLiteral("light ink %1 vs %2")
                        .arg(lightReader.name()).arg(lightSibling.name())));
    QVERIFY2(inkMatch(darkReader, darkSibling),
             qPrintable(QStringLiteral("dark ink %1 vs %2")
                        .arg(darkReader.name()).arg(darkSibling.name())));
    QVERIFY2(inkMatch(darkReader, darkTint),
             qPrintable(QStringLiteral("dark ink %1 vs tint %2")
                        .arg(darkReader.name()).arg(darkTint.name())));
    // The suggested affordance reads as the palette accent — and
    // visibly differs from the plain chrome tint.
    QVERIFY2(inkMatch(lightSuggested, lightAccent),
             qPrintable(QStringLiteral("suggested ink %1 vs accent %2")
                        .arg(lightSuggested.name())
                        .arg(lightAccent.name())));
    QVERIFY2(inkMatch(darkSuggested, darkAccent),
             qPrintable(QStringLiteral("dark suggested %1 vs accent %2")
                        .arg(darkSuggested.name())
                        .arg(darkAccent.name())));
    QVERIFY2(!inkMatch(darkSuggested, darkTint),
             qPrintable(QStringLiteral("dark suggested %1 matches "
                                       "plain tint %2")
                        .arg(darkSuggested.name()).arg(darkTint.name())));
    // Disabled reads the palette's dimmed tone like every sibling —
    // not Qt's generated ghosting of a white glyph (a near-white
    // fade that reads *brighter* than the siblings' dimmed tint).
    QVERIFY2(inkMatch(darkReaderDisabled, darkSiblingDisabled),
             qPrintable(QStringLiteral("disabled ink %1 vs %2")
                        .arg(darkReaderDisabled.name())
                        .arg(darkSiblingDisabled.name())));
    QVERIFY(darkReaderDisabled.isValid());
    QVERIFY(darkReaderDisabled.value() <= darkDim.value() + 4);
    QCOMPARE(lightServed, iconSize);
    QCOMPARE(activeServed, iconSize);
    // Active state: the accent page fill — a colored mark, not the
    // plain chrome tint.
    QVERIFY(activeInk.isValid());
    QVERIFY(activeInk != darkTint);
    QVERIFY(inkMatch(lightReaderAgain, lightReader));
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

    // ENG17: the user-facing New Tab command — Ctrl+T, the strip's
    // "+" button, File > New Tab — lands on the tor start page too,
    // not a blank Untitled.  url() alone only proves the load was
    // issued (m_initialUrl answers early), so the committed title is
    // the real check.
    window->tabWidget()->newTabPage();
    WebView *cmdTab = window->currentTab();
    const QUrl cmdTabUrl = cmdTab ? cmdTab->url() : QUrl();
    QString cmdTitle;
    if (cmdTab) {
        QSignalSpy spy(cmdTab->enginePage(), &Engine::Page::loadFinished);
        const int mark = spy.count();
        for (int i = 0; i < 300 && spy.count() == mark; ++i)
            QTest::qWait(50);
        cmdTitle = cmdTab->title();
    }

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
    QCOMPARE(cmdTabUrl, torStart);
    QCOMPARE(cmdTitle, QStringLiteral("Welcome to Arora!"));
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
    ProfileSettings().setValue(engineKey, QStringLiteral("servo"));
    const QUrl mapped = BrowserApplication::startPageUrl();
    const QUrl guessed =
        TabWidget::guessUrlFromString(QLatin1String("about:home"));

    SubWindow *window = new SubWindow;
    window->show();
    QApplication::processEvents();

    // The engine pick lands on the window's first tab — which KIND
    // depends on the build: a registered servo backend (master's
    // ENG10 wiring) makes it a foreign EngineTab outright, and the
    // branded qrc page — WebEngine content servo cannot render —
    // never loads there.  Without the artifact (or once servo's
    // single-view budget is spent) the pick degrades to a WebView
    // that loads the branded page at once.
    WebView *firstTab = window->currentTab();
    EngineTab *firstEngine = window->tabWidget()->engineTab(0);
    const QUrl firstUrl = firstTab ? firstTab->url() : QUrl();

    // A second fresh tab takes the same branded start page — under a
    // live servo backend the first tab claims the single view, so
    // this one is the WebView fallback that actually loads it.
    window->tabWidget()->newTab();
    WebView *servoTab = window->currentTab();
    const QUrl servoTabUrl = servoTab ? servoTab->url() : QUrl();

    // DOM probe: the logo div sits directly under the search field,
    // its stylesheet resolves the bundled logo asset, the search box
    // exists, and headerTitle only fills once the channel's
    // arora.translate() callback has resolved — the same handshake
    // proof torStartPage uses.  The probe runs on whichever WebView
    // renders the branded page (firstTab when the pick degraded,
    // servoTab under a live backend).
    WebView *domTab = firstTab ? firstTab : servoTab;
    QWebEnginePage *page = domTab ? domTab->page() : nullptr;
    QWebChannel *channel = page ? page->webChannel() : nullptr;
    QString dom;
    if (domTab) {
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
            domTab->enginePage()->runJavaScript(probe,
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
    if (!grabDir.isEmpty() && domTab) {
        const QString dir = QString::fromLocal8Bit(grabDir);
        QDir().mkpath(dir);
        domTab->resize(760, 700);
        domTab->show();
        QApplication::processEvents();
        QTest::qWait(400);
        domTab->grab().save(dir + QLatin1String("/servo-startpage.png"));
    }

    closeWindow(window);
    ProfileSettings().remove(engineKey);

    QCOMPARE(mapped, servoStart);
    QCOMPARE(guessed, servoStart);
    if (firstEngine) {
        // The pick spawned the real engine tab — the branded qrc
        // page is not servo's to render, so a live page on the slot
        // is the check.
        QVERIFY(firstEngine->page());
    } else {
        QCOMPARE(firstUrl, servoStart);
    }
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

// READ02/READ04: the navigation toolbar carries a reader-mode toggle
// between stop/reload and the location bar — always present on engine
// tabs (the article heuristic only sets the "suggested" accent),
// checked while the overlay is up, agreeing with the View-menu
// action, and hidden only where reader mode can never apply
// (widget tabs like History).
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
    // READ04: about:blank is not article-like but the button still
    // shows — the user may always try reader mode.
    QTRY_VERIFY_WITH_TIMEOUT(!reader->isHidden(), 5000);
    QVERIFY(!reader->isChecked());
    QVERIFY(!reader->property("suggested").toBool());
    QVERIFY(reader->toolTip().contains(QLatin1String("Ctrl+Alt+R")));

    // Slot order in the toolbar's action list: the MENU06 app-menu
    // button leads, then Back, Forward, Stop/Reload, the HOME01 Home
    // button, the reader button, the TRUST14/TRUST15 site shield,
    // then the location-bar splitter.
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
    QCOMPARE(splitterIndex, readerIndex + 2);

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
    // availability -> the "suggested" accent lights up (the button
    // was already there); click enters reader mode.
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
    QTRY_VERIFY_WITH_TIMEOUT(reader->property("suggested").toBool(),
                             20000);
    QVERIFY(reader->toolTip().contains(
        QLatin1String("article")));

    reader->click();
    QTRY_VERIFY_WITH_TIMEOUT(reader->isChecked(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(viewAction->isChecked(), 5000);

    // The menu action exits again — the button follows.
    viewAction->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!viewAction->isChecked(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(!reader->isChecked(), 5000);

    // Tab switches re-point the button at the current view.  READ04:
    // the fresh tab stays visible even though it is no article — only
    // the "suggested" accent drops.
    window->tabWidget()->newTab();
    QTRY_VERIFY_WITH_TIMEOUT(!reader->property("suggested").toBool(),
                             10000);
    QVERIFY(!reader->isHidden());
    QVERIFY(!reader->isChecked());
    QVERIFY(!viewAction->isChecked());

    // Clicking on a non-article page is allowed — reader mode just
    // reports it cannot apply rather than hiding the option up front.
    WebView *blankView = window->currentTab();
    QVERIFY(blankView && blankView->readerMode());
    QSignalSpy messageSpy(blankView->readerMode(),
                          &ReaderMode::message);
    reader->click();
    QTRY_VERIFY_WITH_TIMEOUT(messageSpy.count() >= 1, 20000);
    QVERIFY(!reader->isChecked());
    QVERIFY(!blankView->readerMode()->isActive());

    // Back on the article tab the suggested state returns.
    window->tabWidget()->setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(reader->property("suggested").toBool(),
                             10000);
    QVERIFY(!reader->isHidden());

    // Widget tabs are where the control really hides — reader mode
    // can never apply to a History/Preferences page.
    QVERIFY(QMetaObject::invokeMethod(window, "showHistoryPage"));
    QTRY_VERIFY_WITH_TIMEOUT(reader->isHidden(), 10000);
    QVERIFY(!viewAction->isEnabled());
    window->tabWidget()->setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(!reader->isHidden(), 10000);
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

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

#ifdef ARORA_RUSTCORE
    // RDR01: the article lives in the overlay widget's own
    // QWebEngineView — read .content off its page.
    const auto readerContent = [&]() -> QString {
        QWebEngineView *readerView = view->findChild<QWebEngineView *>(
            QLatin1String("aroraReaderView"));
        if (!readerView || !readerView->page())
            return QString();
        auto state = std::make_shared<QPair<QVariant, bool>>(
            QVariant(), false);
        readerView->page()->runJavaScript(QStringLiteral(
            "(function(){var c=document.querySelector('.content');"
            "return c?c.innerHTML:'';})()"), [state](const QVariant &v) {
                state->first = v;
                state->second = true;
            });
        for (int waited = 0; !state->second && waited < 8000;
             waited += 50)
            QTest::qWait(50);
        return state->first.toString();
    };
#else
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
#endif
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
#ifdef ARORA_RUSTCORE
        // The fixture's payloads fire on the served page itself — the
        // observable flag for a leak is the READER VIEW's own window:
        // anything the sanitizer missed would have run there when the
        // article document rendered.
        QWebEngineView *readerView = view->findChild<QWebEngineView *>(
            QLatin1String("aroraReaderView"));
        QVERIFY(readerView);
        auto pwned = std::make_shared<QPair<QVariant, bool>>(
            QVariant(), false);
        readerView->page()->runJavaScript(
            QStringLiteral("String(window.__pwned)"),
            [pwned](const QVariant &v) {
                pwned->first = v;
                pwned->second = true;
            });
        for (int waited = 0; !pwned->second && waited < 8000;
             waited += 50)
            QTest::qWait(50);
        QCOMPARE(pwned->first.toString(), QLatin1String("undefined"));
#else
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
#endif
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
    ProfileSettings settings;
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
    ProfileSettings settings;
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

    // NOTE02: the notes page is a multi-note stack over the shared
    // per-profile NotesModel — empty state -> list -> editor.
    QStackedWidget *notesStack = panel->findChild<QStackedWidget *>(
        QLatin1String("sidebarNotesStack"));
    QListView *notesList = panel->findChild<QListView *>(
        QLatin1String("sidebarNotesList"));
    QToolButton *addNoteButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarNotesAdd"));
    QToolButton *removeNoteButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarNotesRemove"));
    QVERIFY(notesStack && notesList && addNoteButton && removeNoteButton);
    QVERIFY(panel->notesModel());
    QVERIFY(panel->notes());
    // The shared per-profile store can carry leftovers — a migrated
    // scratch note from another leg, or a stale on-disk file — so the
    // empty-state check starts from an actually-empty model.
    if (panel->notesModel()->rowCount() > 0) {
        panel->notesModel()->removeRows(0, panel->notesModel()->rowCount());
        panel->notesModel()->save();
    }
    // Zero notes renders the empty state and disables delete.
    QCOMPARE(notesStack->currentIndex(), 0);
    QVERIFY(!removeNoteButton->isEnabled());

    // + creates a note and opens its editor; typing lands in the
    // model and the autosave slot flushes it to the store.
    addNoteButton->click();
    QCOMPARE(panel->notesModel()->rowCount(), 1);
    QCOMPARE(notesStack->currentIndex(), 2);
    panel->notes()->setPlainText(QLatin1String("remember me"));
    QLineEdit *noteTitle = panel->findChild<QLineEdit *>(
        QLatin1String("sidebarNoteTitle"));
    QVERIFY(noteTitle);
    noteTitle->setText(QLatin1String("First note"));
    QCOMPARE(panel->notesModel()->index(0, 0)
                 .data(Qt::EditRole).toString(),
             QLatin1String("First note"));
    QVERIFY(QMetaObject::invokeMethod(panel, "save"));
    // Persisted through the model's own file, not the retired
    // sidebar/notes scratch key.
    QVERIFY(!settings.contains(QLatin1String("sidebar/notes")));
    QVERIFY(QFileInfo::exists(panel->notesModel()->storageFile()));

    // The editor's back button returns to the (now populated) list.
    QToolButton *notesBack = panel->findChild<QToolButton *>(
        QLatin1String("sidebarNotesBack"));
    QVERIFY(notesBack);
    notesBack->click();
    QCOMPARE(notesStack->currentIndex(), 1);
    QCOMPARE(notesList->model()->rowCount(), 1);

    // − deletes the selected note after confirmation (same
    // arm-the-answer-then-click pattern the history clear leg uses).
    const auto answerQuestion = [](QMessageBox::StandardButton button) {
        QTimer::singleShot(100, qApp, [button]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (box && box->button(button))
                box->button(button)->click();
        });
    };
    notesList->setCurrentIndex(notesList->model()->index(0, 0));
    answerQuestion(QMessageBox::No);
    removeNoteButton->click();
    QCOMPARE(panel->notesModel()->rowCount(), 1);
    answerQuestion(QMessageBox::Yes);
    removeNoteButton->click();
    QCOMPARE(panel->notesModel()->rowCount(), 0);
    QCOMPARE(notesStack->currentIndex(), 0);

    // Migrate the legacy scratch pad in through the public seam —
    // the note lands first and the key is consumed.
    settings.setValue(QLatin1String("sidebar/notes"),
                      QLatin1String("legacy scratch"));
    NotesModel::migrateLegacyScratch(panel->notesModel());
    QCOMPARE(panel->notesModel()->rowCount(), 1);
    QCOMPARE(panel->notesModel()->noteBody(0),
             QLatin1String("legacy scratch"));
    QVERIFY(!settings.contains(QLatin1String("sidebar/notes")));

    // NOTE03: the search row filters the list live on title and body.
    QLineEdit *notesSearch = panel->findChild<QLineEdit *>(
        QLatin1String("sidebarNotesSearch"));
    QVERIFY(notesSearch);
    panel->notesModel()->addNote(QLatin1String("Recipes"),
                                 QLatin1String("**bread** flour water"));
    panel->notesModel()->addNote(QLatin1String("Calls"),
                                 QLatin1String("phone the bank"));
    QCOMPARE(notesList->model()->rowCount(), 3);
    notesSearch->setText(QLatin1String("reci"));
    QCOMPARE(notesList->model()->rowCount(), 1);
    // Body text matches too, and the filtered display stays titled.
    notesSearch->setText(QLatin1String("bank"));
    QCOMPARE(notesList->model()->rowCount(), 1);
    QCOMPARE(notesList->model()->index(0, 0)
                 .data(Qt::DisplayRole).toString(),
             QLatin1String("Calls"));
    notesSearch->clear();
    QCOMPARE(notesList->model()->rowCount(), 3);

    // A filtered row still opens its source note in the editor.
    notesSearch->setText(QLatin1String("reci"));
    emit notesList->activated(notesList->model()->index(0, 0));
    QCOMPARE(notesStack->currentIndex(), 2);
    QCOMPARE(noteTitle->text(), QLatin1String("Recipes"));
    const int recipeRow = 1;   // "legacy scratch" holds row 0

    // The options menu carries only real actions — the per-note
    // markdown flag and the page-URL insert.
    QAction *markdownAction = panel->findChild<QAction *>(
        QLatin1String("sidebarNotesMarkdown"));
    QAction *insertUrlAction = panel->findChild<QAction *>(
        QLatin1String("sidebarNotesInsertUrl"));
    QStackedWidget *bodyStack = panel->findChild<QStackedWidget *>(
        QLatin1String("sidebarNoteBodyStack"));
    QTextBrowser *notesPreview = panel->findChild<QTextBrowser *>(
        QLatin1String("sidebarNotesPreview"));
    QVERIFY(markdownAction && insertUrlAction && bodyStack
            && notesPreview);
    QVERIFY(markdownAction->isCheckable());
    QVERIFY(!markdownAction->isChecked());
    QCOMPARE(bodyStack->currentWidget(),
             static_cast<QWidget *>(panel->notes()));

    // Flagging the note swaps the body to the rendered preview —
    // markdown markup renders instead of showing raw asterisks.
    markdownAction->trigger();
    QVERIFY(markdownAction->isChecked());
    QCOMPARE(panel->notesModel()->index(recipeRow, 0)
                 .data(NotesModel::MarkdownRole).toBool(), true);
    QCOMPARE(bodyStack->currentWidget(),
             static_cast<QWidget *>(notesPreview));
    QVERIFY(notesPreview->toPlainText().contains(
                QLatin1String("bread flour")));
    QVERIFY(!notesPreview->toPlainText().contains(QLatin1String("**")));

    // Unflagging returns to the source editor.
    markdownAction->trigger();
    QVERIFY(!markdownAction->isChecked());
    QCOMPARE(panel->notesModel()->index(recipeRow, 0)
                 .data(NotesModel::MarkdownRole).toBool(), false);
    QCOMPARE(bodyStack->currentWidget(),
             static_cast<QWidget *>(panel->notes()));

    // The link button inserts the current page URL at the cursor —
    // the tab's address lands in the note body.
    WebView *noteTab = window->currentTab();
    QVERIFY(noteTab);
    noteTab->loadUrl(QUrl(QStringLiteral(
        "data:text/html,<title>NoteURL</title><p>x</p>")));
    const QString noteTabUrl = noteTab->url().toString();
    QVERIFY(!noteTabUrl.isEmpty());
    insertUrlAction->trigger();
    QVERIFY(panel->notesModel()->index(recipeRow, 0)
                .data(NotesModel::BodyRole).toString()
                .contains(noteTabUrl));

    // A second window honors the persisted show state + dock side.
    SubWindow *window2 = new SubWindow;
    window2->show();
    QVERIFY(window2->sidebarDock()->isVisible());
    QCOMPARE(window2->dockWidgetArea(window2->sidebarDock()),
             Qt::RightDockWidgetArea);
    QVERIFY(window2->sidebarPanel());
    // The notes store is shared per browsing profile — the second
    // window's panel edits the same model instance.
    QCOMPARE(window2->sidebarPanel()->notesModel(),
             panel->notesModel());
    QCOMPARE(window2->sidebarPanel()->notesModel()->noteBody(0),
             QLatin1String("legacy scratch"));
    closeWindow(window2);

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("sidebar/notes"));
    settings.remove(QLatin1String("sidebar/currentTab"));
    settings.remove(QLatin1String("sidebar/panels"));
    closeWindow(window);
}

// RSZ01: the sidebar's inner edge is a real resize grip — dragging
// it resizes the dock live and persists MainWindow/sidebarWidth on
// release, double-click resets to the default, the grip follows the
// dock area, and the stored width lands on a fresh window's first
// sidebar show.
void tst_BrowserMainWindow::sidebarResize()
{
    ProfileSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("MainWindow/sidebarWidth"));

    SubWindow *window = new SubWindow;
    window->resize(1000, 700);
    window->show();
    QApplication::processEvents();

    QDockWidget *dock = window->sidebarDock();
    dock->toggleViewAction()->trigger();
    QApplication::processEvents();
    QVERIFY(dock->isVisible());
    QVERIFY(window->sidebarPanel());

    SidebarGrip *grip = dock->findChild<SidebarGrip *>();
    QVERIFY(grip);
    QVERIFY(grip->isVisible());
    QCOMPARE(window->dockWidgetArea(dock), Qt::LeftDockWidgetArea);
    // Left dock: the grip trails the panel (the content-facing edge).
    QLayout *layout = grip->parentWidget()->layout();
    QVERIFY(layout);
    QCOMPARE(layout->indexOf(grip), layout->count() - 1);

    const auto dragGrip = [grip](int dx) {
        const QPoint local(3, 20);
        const QPoint start = grip->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(local),
                          QPointF(start), Qt::LeftButton,
                          Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &press);
        const QPoint end = start + QPoint(dx, 0);
        QMouseEvent move(QEvent::MouseMove,
                         QPointF(grip->mapFromGlobal(end)),
                         QPointF(end), Qt::NoButton, Qt::LeftButton,
                         Qt::NoModifier);
        QApplication::sendEvent(grip, &move);
        QMouseEvent release(QEvent::MouseButtonRelease,
                            QPointF(grip->mapFromGlobal(end)),
                            QPointF(end), Qt::LeftButton,
                            Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(grip, &release);
        QApplication::processEvents();
    };

    // Left dock: dragging right widens; release persists the width.
    const int startWidth = dock->width();
    dragGrip(60);
    QVERIFY(dock->width() >= startWidth + 50);
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarWidth"))
                 .toInt(), dock->width());

    // The clamp keeps the panel usable and leaves room for content.
    QCOMPARE(SidebarGrip::clampedWidth(50, 1000), 160);
    QCOMPARE(SidebarGrip::clampedWidth(2000, 1000), 800);
    dragGrip(5000);
    const int maxed = dock->width();
    QVERIFY(maxed <= window->width());
    dragGrip(-5000);
    QVERIFY(dock->width() < maxed);

    // Double-click snaps back to the default and persists it.
    const QPoint local(3, 20);
    QMouseEvent dblClick(QEvent::MouseButtonDblClick, QPointF(local),
                       QPointF(grip->mapToGlobal(local)),
                       Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &dblClick);
    QApplication::processEvents();
    QCOMPARE(dock->width(), SidebarGrip::defaultWidth());
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarWidth"))
                 .toInt(), SidebarGrip::defaultWidth());

    // The grip follows the dock to the right side (leading edge now)
    // and a leftward drag widens from there.
    settings.setValue(QLatin1String("MainWindow/sidebarDockArea"),
                      int(Qt::RightDockWidgetArea));
    window->applySidebarSettings();
    QApplication::processEvents();
    QCOMPARE(window->dockWidgetArea(dock), Qt::RightDockWidgetArea);
    QVERIFY(grip->isVisible());
    QCOMPARE(layout->indexOf(grip), 0);
    const int rightStart = dock->width();
    dragGrip(-60);
    QVERIFY(dock->width() >= rightStart + 50);
    QCOMPARE(settings.value(QLatin1String("MainWindow/sidebarWidth"))
                 .toInt(), dock->width());

    // The stored width lands on a fresh window's first show — the
    // dock auto-opens from the persisted showSidebar setting.
    settings.setValue(QLatin1String("MainWindow/sidebarWidth"), 400);
    SubWindow *window2 = new SubWindow;
    window2->resize(1000, 700);
    window2->show();
    QVERIFY(window2->sidebarDock()->isVisible());
    QTRY_COMPARE(window2->sidebarDock()->width(), 400);
    closeWindow(window2);

    settings.remove(QLatin1String("MainWindow/sidebarWidth"));
    settings.remove(QLatin1String("MainWindow/sidebarDockArea"));
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    closeWindow(window);
}

// SIDE09: the rail follows sidebar/panelOrder — written by the prefs
// ▲▼ buttons — applied live by applySidebarSettings (no rebuild),
// composed with per-panel visibility, and carried into a fresh
// window.  Ids the key doesn't mention append in registry order.
void tst_BrowserMainWindow::sidebarPanelOrder()
{
    ProfileSettings settings;
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
    ProfileSettings settings;
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

    // 'Show Close Button' + SIDE11 — docked, the switch drives
    // exactly one close control: the Downloads header X.  The dock
    // title bar's own X (Qt's private qt_dockwidget_closebutton) was
    // the duplicate sitting beside it, so that button is hidden while
    // docked.  DockWidgetClosable itself stays on — stripping it
    // would also break the toggle action's hide path.
    QVERIFY(dock->features() & QDockWidget::DockWidgetClosable);
    QVERIFY(dock->toggleViewAction()->isEnabled());
    QWidget *titleX = dock->findChild<QWidget *>(
        QLatin1String("qt_dockwidget_closebutton"));
    QVERIFY(titleX);
    QVERIFY(titleX->isHidden());
    QWidget *headerX = panel->findChild<QWidget *>(
        QLatin1String("sidebarCloseButton"));
    QVERIFY(headerX);
    // The Downloads page isn't current, so assert the flag the option
    // drives (isHidden), not isVisible() which also needs a shown tab.
    QVERIFY(!headerX->isHidden());
    settings.setValue(QLatin1String("sidebar/showCloseButton"), false);
    window->applySidebarSettings();
    QVERIFY(titleX->isHidden());
    QVERIFY(headerX->isHidden());
    settings.setValue(QLatin1String("sidebar/showCloseButton"), true);
    window->applySidebarSettings();
    QVERIFY(titleX->isHidden());
    QVERIFY(!headerX->isHidden());

    // SIDE11 float/undock: a torn-off dock re-shows the title-bar X
    // (a floating window needs its own close affordance), still
    // gated by the option; re-docking hides it again.
    dock->setFloating(true);
    QTRY_VERIFY(dock->isFloating());
    QVERIFY(!titleX->isHidden());
    dock->setFloating(false);
    QTRY_VERIFY(!dock->isFloating());
    QVERIFY(titleX->isHidden());
    settings.setValue(QLatin1String("sidebar/showCloseButton"), false);
    window->applySidebarSettings();
    dock->setFloating(true);
    QTRY_VERIFY(dock->isFloating());
    QVERIFY(titleX->isHidden());
    dock->setFloating(false);
    QTRY_VERIFY(!dock->isFloating());
    settings.setValue(QLatin1String("sidebar/showCloseButton"), true);
    window->applySidebarSettings();
    QVERIFY(!headerX->isHidden());
    QVERIFY(dock->isVisible());

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
    ProfileSettings settings;
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
    ProfileSettings settings;
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

// HIST03: the History page's header row carries a period combo
// (Today/This Week/This Month/All — a date filter between the
// day-group tree model and the search proxy, persisted under
// sidebar/historyPeriod) and a clear button on the shared
// HistoryManager::confirmClear path.
void tst_BrowserMainWindow::sidebarHistoryControls()
{
    ProfileSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("sidebar/historyPeriod"));

    // Seed the shared store with entries on distinct days — the day
    // groups are what the period filter keeps or drops.
    HistoryManager *manager = HistoryManager::instance();
    manager->setDaysToExpire(-1);   // keep the backdated row
    const QDate today = QDate::currentDate();
    manager->setHistory(QList<HistoryEntry>()
        << HistoryEntry(QStringLiteral("http://today.example"),
                        QDateTime(today, QTime(12, 0)))
        << HistoryEntry(QStringLiteral("http://old.example"),
                        QDateTime(today.addDays(-45), QTime(12, 0))));

    SubWindow *window = new SubWindow;
    window->show();
    QDockWidget *dock = window->sidebarDock();
    dock->toggleViewAction()->trigger();
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);

    QComboBox *period = panel->findChild<QComboBox *>(
        QLatin1String("sidebarHistoryPeriod"));
    QToolButton *clearButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarHistoryClear"));
    QTreeView *view = panel->findChild<QTreeView *>(
        QLatin1String("sidebarHistoryView"));
    QVERIFY(period);
    QVERIFY(clearButton);
    QVERIFY(view);
    QCOMPARE(period->count(), 4);
    QAbstractItemModel *model = view->model();
    QVERIFY(model);

    // Default All — both day groups show, nothing hidden.
    QCOMPARE(period->currentData().toInt(),
             int(HistoryPeriodFilterModel::All));
    QCOMPARE(model->rowCount(), 2);

    // Today — the old group drops entirely; the pick persists.
    emit period->activated(0);
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->index(0, 0).data(HistoryModel::DateRole).toDate(),
             today);
    QCOMPARE(settings.value(QLatin1String("sidebar/historyPeriod"))
                 .toInt(), int(HistoryPeriodFilterModel::Today));

    // Text search composes with the range.  Day groups with children
    // always pass the search proxy, so under Today + "old" the one
    // surviving group shows zero matching children; back on All the
    // old group reappears with its match.
    QLineEdit *search = view->parentWidget()->findChild<QLineEdit *>();
    QVERIFY(search);
    search->setText(QLatin1String("old"));
    QCOMPARE(model->rowCount(), 1);
    QCOMPARE(model->rowCount(model->index(0, 0)), 0);
    emit period->activated(3);
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->rowCount(model->index(0, 0)), 0);
    QCOMPARE(model->rowCount(model->index(1, 0)), 1);
    search->clear();
    QCOMPARE(model->rowCount(), 2);
    QCOMPARE(model->rowCount(model->index(0, 0)), 1);

    // Offscreen grab of the History page (combo + clear button +
    // grouped tree) for visual review — same ARORA_SIDEBAR_GRAB_DIR
    // convention as sidebarPanel, lands as sidebar-history.png.
    const QByteArray sidebarGrabDir = qgetenv("ARORA_SIDEBAR_GRAB_DIR");
    if (!sidebarGrabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(sidebarGrabDir));
        panel->tabs()->setCurrentWidget(view->parentWidget());
        qApp->processEvents();
        view->parentWidget()->grab().save(
            QString::fromLocal8Bit(sidebarGrabDir)
            + QStringLiteral("/sidebar-history.png"));
    }

    // The clear button runs the menu's shared confirmation — No
    // keeps the store, Yes empties it.
    const auto answerQuestion = [](QMessageBox::StandardButton button) {
        QTimer::singleShot(100, qApp, [button]() {
            QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget());
            if (box && box->button(button))
                box->button(button)->click();
        });
    };
    answerQuestion(QMessageBox::No);
    clearButton->click();
    QCOMPARE(manager->history().count(), 2);
    QCOMPARE(model->rowCount(), 2);

    answerQuestion(QMessageBox::Yes);
    clearButton->click();
    QVERIFY(manager->history().isEmpty());
    QCOMPARE(model->rowCount(), 0);

    manager->setDaysToExpire(30);
    settings.remove(QLatin1String("sidebar/historyPeriod"));
    closeWindow(window);
}

// SIDE04: Bookmarks > "Show All Bookmarks" (Ctrl+Shift+B) raises the
// sidebar dock on the Bookmarks rail — the standalone manager window
// is gone from this path.  The rail carries the old dialog's
// Add/Remove over the shared model: Add runs the same
// AddBookmarkDialog the toolbar star uses and lands the current
// page in the rail-selected folder; Remove deletes the selected
// bookmark (disabled on folders/empty selection), and the context
// menu's "Remove Entry" is the same delete.
void tst_BrowserMainWindow::bookmarksSidebarPanel()
{
    ProfileSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));

    BookmarksManager *manager = BookmarksManager::instance();
    BookmarkNode *seed = new BookmarkNode(BookmarkNode::Bookmark);
    seed->title = QLatin1String("SidebarSeed");
    seed->url = QLatin1String("http://sidebar-seed.example.com/");
    manager->addBookmark(manager->menu(), seed);

    SubWindow *window = new SubWindow;
    window->show();
    WebView *webView = window->currentTab();
    QVERIFY(webView);
    QSignalSpy loaded(webView->page(), &QWebEnginePage::loadFinished);
    webView->load(QUrl(QLatin1String(
        "data:text/html,<title>Sidebar Add</title><p>page</p>")));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);

    // Menu path — same entry as before, but it opens the dock now.
    QMenu *bookmarksMenu = nullptr;
    for (QAction *menuAction : window->menuBar()->actions()) {
        if (menuAction->menu()
            && menuAction->text().remove(QLatin1Char('&'))
                   == QLatin1String("Bookmarks"))
            bookmarksMenu = menuAction->menu();
    }
    QVERIFY(bookmarksMenu);
    QAction *showAll = nullptr;
    for (QAction *action : bookmarksMenu->actions()) {
        if (action->text().remove(QLatin1Char('&'))
                .contains(QLatin1String("Show All Bookmarks")))
            showAll = action;
    }
    QVERIFY(showAll);
    QCOMPARE(showAll->shortcut(),
             QKeySequence(Qt::ControlModifier | Qt::ShiftModifier
                          | Qt::Key_B));

    showAll->trigger();
    QDockWidget *dock = window->sidebarDock();
    QVERIFY(dock->isVisible());
    SidebarPanel *panel = window->sidebarPanel();
    QVERIFY(panel);
    QCOMPARE(panel->tabs()->currentWidget(), panel->bookmarksPage());
    // No second toplevel — the old BookmarksDialog path is gone.
    for (QWidget *widget : QApplication::topLevelWidgets())
        QVERIFY(!qobject_cast<BookmarksDialog *>(widget));

    EditTreeView *view = panel->findChild<EditTreeView *>(
        QLatin1String("sidebarBookmarksView"));
    QPushButton *addButton = panel->findChild<QPushButton *>(
        QLatin1String("sidebarAddBookmark"));
    QPushButton *removeButton = panel->findChild<QPushButton *>(
        QLatin1String("sidebarRemoveBookmark"));
    QVERIFY(view);
    QVERIFY(addButton);
    QVERIFY(addButton->isEnabled());
    QVERIFY(removeButton);
    QSortFilterProxyModel *proxy =
        qobject_cast<QSortFilterProxyModel *>(view->model());
    QVERIFY(proxy);

    // Remove is gated on a bookmark selection — nothing/folder = off.
    view->setCurrentIndex(QModelIndex());
    QVERIFY(!removeButton->isEnabled());
    QModelIndex seedSource = manager->bookmarksModel()->index(seed);
    QVERIFY(seedSource.isValid());
    QModelIndex seedRow = proxy->mapFromSource(seedSource);
    view->setCurrentIndex(seedRow);
    QVERIFY(removeButton->isEnabled());

    // Add opens the AddBookmarkDialog prefilled with the current
    // page; accepting lands the node in the seed's folder (menu).
    const int menuRows = manager->menu()->children().count();
    acceptModal(300);
    addButton->click();
    QCOMPARE(manager->menu()->children().count(), menuRows + 1);
    BookmarkNode *added = manager->menu()->children().constLast();
    QVERIFY(added->url.contains(QLatin1String("data:text/html")));
    QCOMPARE(added->title, QLatin1String("Sidebar Add"));

    // The button deletes the selection straight out of the shared
    // model — the toolbar/menu views see it through the same model.
    view->setCurrentIndex(
        proxy->mapFromSource(manager->bookmarksModel()->index(added)));
    removeButton->click();
    QCOMPARE(manager->menu()->children().count(), menuRows);
    QVERIFY(!manager->menu()->children().contains(added));

    // The context menu's "Remove Entry" deletes the seed the same
    // way — build + trigger through the nested exec loop.
    view->setCurrentIndex(seedRow = proxy->mapFromSource(
            manager->bookmarksModel()->index(seed)));
    bool removed = false;
    int tries = 0;
    QTimer *timer = new QTimer(qApp);
    timer->setInterval(80);
    QObject::connect(timer, &QTimer::timeout, qApp, [&]() {
        QMenu *menu = qobject_cast<QMenu *>(
            QApplication::activePopupWidget());
        if (!menu) {
            const QWidgetList tops = QApplication::topLevelWidgets();
            for (QWidget *widget : tops) {
                menu = qobject_cast<QMenu *>(widget);
                if (menu && menu->isVisible())
                    break;
                menu = nullptr;
            }
        }
        if (!menu) {
            if (++tries > 25)
                timer->stop();
            return;
        }
        timer->stop();
        for (QAction *action : menu->actions()) {
            if (action->text().remove(QLatin1Char('&'))
                    .contains(QLatin1String("Remove Entry"))
                && action->isEnabled()) {
                action->trigger();
                removed = true;
            }
        }
        // A programmatic trigger does not dismiss an exec'd menu.
        menu->close();
    });
    timer->start();
    const QPoint pos = view->visualRect(seedRow).center();
    QVERIFY(QMetaObject::invokeMethod(
        panel, "bookmarksContextMenu", Q_ARG(QPoint, pos)));
    timer->stop();
    timer->deleteLater();
    QVERIFY(removed);
    QVERIFY(!manager->menu()->children().contains(seed));

    // Hiding the dock then re-invoking keeps it docked, on bookmarks.
    dock->hide();
    showAll->trigger();
    QVERIFY(dock->isVisible());
    QVERIFY(!dock->isFloating());
    QCOMPARE(panel->tabs()->currentWidget(), panel->bookmarksPage());

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    closeWindow(window);
}

// COV05: the remaining chrome slots — the merged print dialog,
// the page-source viewer, home navigation, the command palette
// overlays, the New-Container-Tab menu build (private + populated),
// the privacy-mode mirror, and lastTabClosed's keep-open branch.
void tst_BrowserMainWindow::chromeSlots()
{
    // The CONT06 strip (default on) filters tabs to the active
    // container level — pin inline mode so the count assertions
    // below see every tab.
    ProfileSettings settings;
    const QVariant savedDisplay =
        settings.value(QLatin1String("tabs/containerDisplay"));
    settings.setValue(QLatin1String("tabs/containerDisplay"), 0);

    SubWindow *window = new SubWindow;
    window->show();

    // A committed page so toHtml and history menus have content.
    WebView *view = window->currentTab();
    QVERIFY(view);
    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->load(QUrl("about:blank"));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
    // Print path — the machinery stands up even when the dialog is
    // rejected.  PRINT02: the single Print entry opens the preview,
    // which renders the page's printToPdf output in an in-process
    // PdfView — no borrowed printer and no printFinished.  Poll the
    // modal dialog until the generated document is ready (or
    // generation reports failure), then close.
    QString previewState;
    QTimer previewPoll;
    previewPoll.setInterval(50);
    QObject::connect(&previewPoll, &QTimer::timeout, qApp,
            [&previewState]() {
        PrintPreviewDialog *modal =
            qobject_cast<PrintPreviewDialog *>(
                QApplication::activeModalWidget());
        if (!modal || !previewState.isEmpty())
            return;
        if (modal->isReady())
            previewState = QLatin1String("ready");
        else if (modal->hasFailed())
            previewState = modal->failureMessage();
        if (!previewState.isEmpty())
            modal->close();
    });
    previewPoll.start();
    rejectModal(20000); // backstop if the print pipeline never answers
    QVERIFY(QMetaObject::invokeMethod(window, "filePrint"));
    previewPoll.stop();
    QCOMPARE(previewState, QLatin1String("ready"));

    // viewPageSource is intentionally not driven: the SourceViewer's
    // application-side NAM refetch is unstable under the offscreen
    // test environment.

    // Navigation + session persistence entry points.
    QVERIFY(QMetaObject::invokeMethod(window, "goHome"));
    QVERIFY(QMetaObject::invokeMethod(window, "save"));

    // The palette overlays open non-modally over the window.
    QVERIFY(QMetaObject::invokeMethod(window, "showCommandPalette"));
    QVERIFY(window->commandPalette());
    window->commandPalette()->close();
    QVERIFY(QMetaObject::invokeMethod(window, "showTabSearch"));
    window->commandPalette()->close();

    // The container submenu rebuilds on every show: registry entries
    // trigger container tabs; private windows get a dead placeholder.
    ContainerManager *containers = ContainerManager::instance();
    const QString cid = containers->createContainer(
        QLatin1String("CovNav"), QColor(Qt::green)).id;
    QVERIFY(QMetaObject::invokeMethod(window, "populateNewContainerTabMenu"));

    QMenu *containerMenu = nullptr;
    for (QMenu *menu : window->findChildren<QMenu *>()) {
        if (menu->title().contains(QLatin1String("Container Tab")))
            containerMenu = menu;
    }
    QVERIFY(containerMenu);
    QVERIFY(containerMenu->actions().count() >= 4); // none + entry + sep + 2
    const int tabsBefore = window->tabWidget()->count();
    containerMenu->actions().first()->trigger(); // "&No Container"
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 1);
    // The registered container entry opens a container-bound tab.
    QAction *containerEntry = nullptr;
    for (QAction *action : containerMenu->actions()) {
        if (action->text().contains(QLatin1String("CovNav")))
            containerEntry = action;
    }
    QVERIFY(containerEntry);
    containerEntry->trigger();
    QCOMPARE(window->tabWidget()->count(), tabsBefore + 2);
    QCOMPARE(window->tabWidget()->containerIdForTab(
                 window->tabWidget()->count() - 1), cid);

    BrowserApplication::setPrivate(true);
    QVERIFY(QMetaObject::invokeMethod(window, "populateNewContainerTabMenu"));
    BrowserApplication::setPrivate(false);

    // The privacy mirror updates the check state; leaving private
    // mode clears the closed-tab stack.
    QVERIFY(QMetaObject::invokeMethod(window, "privacyChanged",
                                      Q_ARG(bool, true)));
    QVERIFY(QMetaObject::invokeMethod(window, "privacyChanged",
                                      Q_ARG(bool, false)));

    // quitAsLastTabClosed=off: the last close opens a fresh tab
    // instead of closing the window.
    ProfileSettings().setValue(QLatin1String("tabs/quitAsLastTabClosed"), false);
    while (window->tabWidget()->count() > 1)
        window->tabWidget()->closeTab(0);
    QVERIFY(QMetaObject::invokeMethod(window, "lastTabClosed"));
    QVERIFY(window->tabWidget()->count() >= 1);
    ProfileSettings().setValue(QLatin1String("tabs/quitAsLastTabClosed"), true);

    // updateReaderState runs on every view-menu show — here with a
    // real tab so both reader + PiP wiring execute.
    QVERIFY(QMetaObject::invokeMethod(window, "updateReaderState"));

    // Close container tabs before dropping the registry entry.
    for (int i = window->tabWidget()->count() - 1; i >= 0; --i) {
        if (window->tabWidget()->containerIdForTab(i) == cid)
            window->tabWidget()->closeTab(i);
    }
    QVERIFY(containers->deleteContainer(cid));
    closeWindow(window);

    if (savedDisplay.isValid())
        settings.setValue(QLatin1String("tabs/containerDisplay"),
                          savedDisplay);
    else
        settings.remove(QLatin1String("tabs/containerDisplay"));
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
    ProfileSettings settings;
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
    ProfileSettings settings;
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

// DOWN04: the compact per-download action row — pause/resume, retry,
// remove, show-in-folder and the details (speed graph) toggle all act
// on the selected row, with enablement mirroring the card's own gates:
// no pausing a finished download, no retrying a running one, no
// removing an in-flight row.
void tst_BrowserMainWindow::downloadsSidebarActions()
{
    ProfileSettings settings;
    settings.remove(QLatin1String("MainWindow/showSidebar"));
    // The fake requests below carry http urls — keep the rustdl
    // takeover and the external handler from claiming them.
    settings.setValue(QLatin1String("downloadmanager/engine"),
                      QLatin1String("builtin"));
    settings.setValue(QLatin1String("downloadmanager/external"), false);

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
    QSortFilterProxyModel *proxy =
        qobject_cast<QSortFilterProxyModel *>(view->model());
    QVERIFY(proxy);
    DownloadManager::instance()->cleanup();
    QCOMPARE(proxy->rowCount(), 0);

    QToolButton *pauseButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarDownloadPause"));
    QToolButton *retryButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarDownloadRetry"));
    QToolButton *removeButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarDownloadRemove"));
    QToolButton *folderButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarDownloadFolder"));
    QToolButton *detailsButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarDownloadDetails"));
    QVERIFY(pauseButton);
    QVERIFY(retryButton);
    QVERIFY(removeButton);
    QVERIFY(folderButton);
    QVERIFY(detailsButton);
    // Icon-sized row — no dialog-era text buttons.
    QVERIFY(pauseButton->text().isEmpty());
    QVERIFY(!pauseButton->icon().isNull());

    // Nothing selected -> the whole row sits disabled.
    const QList<QToolButton *> row = { pauseButton, retryButton,
        removeButton, folderButton, detailsButton };
    for (QToolButton *button : row)
        QVERIFY(!button->isEnabled());

    // A deterministic in-flight row through the fake engine — no
    // network, no engine-claim gate, and the pause/resume counters are
    // directly observable.
    FakeEngineDownloadRequest *live = new FakeEngineDownloadRequest(this);
    live->m_url = QUrl(QLatin1String("http://example.com/live.bin"));
    live->m_name = QLatin1String("live.bin");
    DownloadManager::instance()->handleDownloadRequested(live);
    QTRY_COMPARE(proxy->rowCount(), 1);
    DownloadItem *liveItem = DownloadManager::instance()->itemAt(0);
    QVERIFY(liveItem);
    QVERIFY(liveItem->downloading());
    // itemAdded auto-selects the fresh row (DLUX01) and hosts the card.
    QCOMPARE(proxy->mapToSource(view->currentIndex()).row(), 0);
    QFrame *detail = panel->findChild<QFrame *>(
        QLatin1String("sidebarDownloadDetail"));
    QVERIFY(detail);
    QVERIFY(detail->isVisibleTo(detail->parentWidget()));

    // In-flight: pause on, retry/remove off (a running row is neither
    // restartable nor removable), folder on once the name is picked.
    QTRY_VERIFY(pauseButton->isEnabled());
    QVERIFY(!retryButton->isEnabled());
    QVERIFY(!removeButton->isEnabled());
    QVERIFY(folderButton->isEnabled());
    QVERIFY(detailsButton->isEnabled());
    QVERIFY(detailsButton->isChecked());

    // Pause flips the request's paused flag and the toggle's wording.
    pauseButton->click();
    QCOMPARE(live->pauses, 1);
    QVERIFY(liveItem->isPaused());
    QVERIFY(pauseButton->isEnabled());
    QVERIFY(pauseButton->toolTip().contains(QLatin1String("Resume")));
    pauseButton->click();
    QCOMPARE(live->resumes, 1);
    QVERIFY(!liveItem->isPaused());
    QVERIFY(pauseButton->toolTip().contains(QLatin1String("Pause")));

    // The details toggle folds the hosted card's speed graph away and
    // back — the row stays in sync with the card's own chevron state.
    detailsButton->click();
    QVERIFY(!liveItem->isExpanded());
    QVERIFY(!detailsButton->isChecked());
    detailsButton->click();
    QVERIFY(liveItem->isExpanded());
    QVERIFY(detailsButton->isChecked());
    // The card's chevron drives the toggle's check state in return.
    QToolButton *chevron = liveItem->findChild<QToolButton *>(
        QLatin1String("expandButton"));
    QVERIFY(chevron);
    chevron->click();
    QVERIFY(!liveItem->isExpanded());
    QVERIFY(!detailsButton->isChecked());
    detailsButton->click();
    QVERIFY(liveItem->isExpanded());

    // Interrupting the row turns pause off and retry + remove on —
    // the hosted card must NOT fold shut on the terminal transition
    // (pinExpanded: the pane's only content is the card).
    live->setState(Engine::DownloadRequest::State::Interrupted);
    QTRY_VERIFY(!pauseButton->isEnabled());
    QVERIFY(retryButton->isEnabled());
    QVERIFY(removeButton->isEnabled());
    QVERIFY(folderButton->isEnabled());
    QVERIFY(liveItem->isExpanded());
    QVERIFY(detailsButton->isChecked());

    // Remove deletes the row and folds every action back off.
    removeButton->click();
    QTRY_COMPARE(proxy->rowCount(), 0);
    for (QToolButton *button : row)
        QVERIFY(!button->isEnabled());

    // A completed row: retry + remove + folder valid, pause off — and
    // the hosted card stays expanded through the finish transition.
    FakeEngineDownloadRequest *done = new FakeEngineDownloadRequest(this);
    done->m_url = QUrl(QLatin1String("http://example.com/done.txt"));
    done->m_name = QLatin1String("done.txt");
    DownloadManager::instance()->handleDownloadRequested(done);
    QTRY_COMPARE(proxy->rowCount(), 1);
    DownloadItem *doneItem = DownloadManager::instance()->itemAt(0);
    QVERIFY(doneItem);
    done->setState(Engine::DownloadRequest::State::Completed);
    QTRY_VERIFY(doneItem->downloadedSuccessfully());
    QTRY_VERIFY(retryButton->isEnabled());
    QVERIFY(!pauseButton->isEnabled());
    QVERIFY(removeButton->isEnabled());
    QVERIFY(folderButton->isEnabled());
    QVERIFY(doneItem->isExpanded());
    QVERIFY(detailsButton->isChecked());

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    settings.remove(QLatin1String("downloadmanager/engine"));
    settings.remove(QLatin1String("downloadmanager/external"));
    closeWindow(window);

    DownloadManager::instance()->cleanup();
    DownloadManager::instance()->deleteLater();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// DOWN05: the downloads page header gains a clear-list button left of
// the close X.  It stays disabled while nothing is removable (empty
// list, or every remaining row in flight), asks the same Yes/No
// confirm the History menu's Clear uses, and drops only
// finished/failed entries — an in-flight transfer and the completed
// file on disk survive.
void tst_BrowserMainWindow::downloadsSidebarClearList()
{
    ProfileSettings settings;
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
    QAbstractItemModel *source = DownloadManager::instance()->model();
    QVERIFY(source);
    // The singleton may have carried rows over from an earlier test.
    DownloadManager::instance()->cleanup();
    QCOMPARE(source->rowCount(), 0);

    QToolButton *clearButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarClearDownloads"));
    QVERIFY(clearButton);
    QCOMPARE(clearButton->toolTip(), QLatin1String("Clear download list"));
    // Header order: the clear button sits immediately left of the X.
    QToolButton *closeButton = panel->findChild<QToolButton *>(
        QLatin1String("sidebarCloseButton"));
    QVERIFY(closeButton);
    QVERIFY(clearButton->x() < closeButton->x());
    // Empty list — nothing cleanup() could drop, honestly disabled.
    QVERIFY(!clearButton->isEnabled());

    // A finished download enables the button.
    const QUrl url(QString::fromLatin1("data:text/plain;base64,")
        + QString::fromLatin1(QByteArray("hello world").toBase64()));
    Engine::Page *page = DownloadManager::instance()->retryPage(false);
    QVERIFY(page);
    DownloadManager::instance()->download(page, url);
    QTRY_COMPARE(source->rowCount(), 1);
    DownloadItem *item = DownloadManager::instance()->itemAt(0);
    QVERIFY(item);
    QTRY_VERIFY(item->downloadedSuccessfully());
    QTRY_VERIFY(clearButton->isEnabled());
    const QString savedPath = item->m_outputFileName;
    QVERIFY(QFileInfo::exists(savedPath));

    // A stalled HTTP transfer joins as a second, in-flight row — the
    // button stays enabled because a removable row exists.
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
                          "filename=\"slow.bin\"\r\n"
                      "Content-Length: 104857600\r\n\r\n");
        QTimer *trickle = new QTimer(socket);
        QObject::connect(trickle, &QTimer::timeout, socket, [socket]() {
            socket->write("x");
        });
        trickle->start(50);
    });
    const QUrl stalledUrl(QString::fromLatin1(
        "http://127.0.0.1:%1/slow.bin").arg(stalled.serverPort()));
    DownloadManager::instance()->download(page, stalledUrl);
    QTRY_COMPARE_WITH_TIMEOUT(source->rowCount(), 2, 30000);
    DownloadItem *activeItem = DownloadManager::instance()->itemAt(1);
    QVERIFY(activeItem);
    QVERIFY(activeItem != item);
    QTRY_VERIFY_WITH_TIMEOUT(activeItem->downloading(), 30000);
    QVERIFY(clearButton->isEnabled());

    // Offscreen grab for visual review — ARORA_SIDEBAR_GRAB_DIR gets
    // downloads-clear.png: populated list + enabled header button.
    const QByteArray grabDir = qgetenv("ARORA_SIDEBAR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        qApp->processEvents();
        panel->grab().save(QString::fromLocal8Bit(grabDir)
            + QStringLiteral("/downloads-clear.png"));
    }

    // "No" on the confirm keeps the whole list.
    QTimer::singleShot(100, qApp, []() {
        if (QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget()))
            box->button(QMessageBox::No)->animateClick();
    });
    clearButton->click();
    QApplication::processEvents();
    QCOMPARE(source->rowCount(), 2);

    // "Yes" drops the finished row only — the in-flight transfer is
    // untouched and the completed file stays on disk.
    QTimer::singleShot(100, qApp, []() {
        if (QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->animateClick();
    });
    clearButton->click();
    QTRY_COMPARE(source->rowCount(), 1);
    QCOMPARE(DownloadManager::instance()->itemAt(0), activeItem);
    QVERIFY(QFileInfo::exists(savedPath));
    // Only an in-flight row remains — disabled again.
    QVERIFY(!clearButton->isEnabled());

    // Cancelling the straggler makes it removable; a second clear
    // empties the list (and removes just the partial file).
    QMetaObject::invokeMethod(activeItem, "stop");
    QTRY_VERIFY_WITH_TIMEOUT(clearButton->isEnabled(), 30000);
    QTimer::singleShot(100, qApp, []() {
        if (QMessageBox *box = qobject_cast<QMessageBox *>(
                QApplication::activeModalWidget()))
            box->button(QMessageBox::Yes)->animateClick();
    });
    clearButton->click();
    QTRY_COMPARE(source->rowCount(), 0);
    QVERIFY(!clearButton->isEnabled());

    settings.remove(QLatin1String("MainWindow/showSidebar"));
    closeWindow(window);

    // Same teardown as downloadsSidebarPanel — the retry pages live
    // on the singleton manager.
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
    ProfileSettings settings;
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
    ProfileSettings settings;
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

// TABS10: a resting tab must not read as underlined — the strip's
// resting edge seam is gone, and the pane's top border (the same
// seam continued under the page) no longer boxes the browser's tab
// widget.  Probe each resting tab's bottom-edge row for a run of
// pixels darker than the strip background; a painted hairline is
// tens of px while text/dots never reach that row.
void tst_BrowserMainWindow::tabStripNoUnderlines()
{
    SubWindow *window = new SubWindow;
    window->resize(900, 600);
    window->show();
    TabWidget *tabWidget = window->tabWidget();
    tabWidget->newTabAction()->trigger();
    tabWidget->newTabAction()->trigger();
    for (int i = 0; i < 20; ++i)
        QApplication::processEvents();

    TabBar *bar = tabWidget->findChild<TabBar *>();
    QVERIFY(bar);
    QVERIFY(bar->count() >= 3);
    bar->setCurrentIndex(0);
    QApplication::processEvents();

    const int bgGray = qGray(bar->palette().color(QPalette::Window).rgb());
    const QImage strip = bar->grab().toImage();
    for (int i = 1; i < bar->count(); ++i) {
        const QRect rect = bar->tabRect(i);
        int run = 0;
        int best = 0;
        for (int x = rect.left() + 6; x <= rect.right() - 6; ++x) {
            if (qGray(strip.pixel(x, rect.bottom())) < bgGray - 12) {
                ++run;
                best = qMax(best, run);
            } else {
                run = 0;
            }
        }
        QVERIFY2(best < 30, qPrintable(QStringLiteral(
            "resting tab %1 carries a %2px underline").arg(i).arg(best)));
    }

    // The pane's 1px border is gone on the browser's tab widget: the
    // seam band between the strip and the view carries no line —
    // every row there is plain background (the pane border used to
    // paint one separator-colored row across the full width).
    QWidget *view = tabWidget->currentWidget();
    QVERIFY(view);
    const int viewTop = view->mapTo(tabWidget, QPoint(0, 0)).y();
    const int barBottom = bar->geometry().bottom();
    const QImage pane = tabWidget->grab().toImage();
    const int paneGray = qGray(tabWidget->palette()
                               .color(QPalette::Window).rgb());
    for (int y = barBottom + 1; y < viewTop && y < pane.height(); ++y) {
        int run = 0;
        int best = 0;
        for (int x = 0; x < pane.width(); ++x) {
            if (qGray(pane.pixel(x, y)) < paneGray - 12) {
                ++run;
                best = qMax(best, run);
            } else {
                run = 0;
            }
        }
        QVERIFY2(best < 30, qPrintable(QStringLiteral(
            "pane seam row %1 still paints a %2px line")
            .arg(y).arg(best)));
    }

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

    // The strip lives in the navigation splitter right after the
    // engine indicator cell — location bar stack, engine indicator,
    // then this.
    QSplitter *splitter = window
        ->findChild<QToolBar *>(QLatin1String("NavigationToolBar"))
        ->findChild<QSplitter *>();
    QVERIFY(splitter);
    QCOMPARE(splitter->indexOf(bar), 2);

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

    extensions->grantConsent(fixture.path());
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

// TRUST14: the site shield moved out of the url bar onto the
// navigation toolbar — the shared button sits immediately left of the
// location-bar stack and rebinds to the ACTIVE tab's WebView on every
// switch (the user's explicit rule: the shield always shows the
// settings of the active tab).  The padlock took the in-bar slot.
void tst_BrowserMainWindow::navShieldPadlock()
{
    SubWindow *window = new SubWindow;
    window->resize(1100, 700);
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window));

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    SiteShieldButton *shield = window->findChild<SiteShieldButton *>(
        QLatin1String("navSiteShield"));
    QVERIFY(shield);
    QCOMPARE(shield->parentWidget(), navBar);

    // Immediately left of the splitter = immediately left of the url
    // bar itself (the splitter's first cell is the location-bar stack).
    QWidget *splitter = window->findChild<QWidget *>(
        QLatin1String("navigationSplitter"));
    QVERIFY(splitter);
    int shieldIndex = -1;
    int splitterIndex = -1;
    const QList<QAction *> actions = navBar->actions();
    for (int i = 0; i < actions.size(); ++i) {
        QWidget *widget = navBar->widgetForAction(actions.at(i));
        if (widget == shield)
            shieldIndex = i;
        if (widget == splitter)
            splitterIndex = i;
    }
    QVERIFY(shieldIndex >= 0);
    QCOMPARE(splitterIndex, shieldIndex + 1);

    // Bound to the front tab from the start — its privacy panel shows
    // the active tab's site settings.
    WebView *first = window->currentTab();
    QVERIFY(first);
    QVERIFY(shield->panel());
    QCOMPARE(shield->panel()->webView(), first);

    // The bar itself carries a padlock (SitePadlockButton), no shield.
    LocationBar *bar = qobject_cast<LocationBar *>(
        window->tabWidget()->currentLocationBar());
    QVERIFY(bar);
    QVERIFY(!bar->findChild<SiteShieldButton *>());
    SitePadlockButton *padlock =
        bar->findChild<SitePadlockButton *>();
    QVERIFY(padlock);

    // Tab switch rebinds the shared shield; the padlock belongs to the
    // tab's own bar, so each page keeps its own verdict.
    WebView *second = window->tabWidget()->makeNewTab(true);
    QVERIFY(second && second != first);
    QCOMPARE(shield->panel()->webView(), second);
    window->tabWidget()->setCurrentIndex(0);
    QCOMPARE(shield->panel()->webView(), first);
    window->tabWidget()->setCurrentIndex(1);
    QCOMPARE(shield->panel()->webView(), second);

    // Offscreen grabs for visual review — set ARORA_TRUST_GRAB_DIR to a
    // directory; normal + tor + private nav rows land there.
    const QByteArray grabDir = qgetenv("ARORA_TRUST_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/nav-normal.png"));
        window->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/window-normal.png"));
    }
    closeWindow(window);

    if (!grabDir.isEmpty()) {
        BrowserApplication::setTorMode(true);
        SubWindow *torWindow = new SubWindow;
        torWindow->resize(1100, 700);
        torWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(torWindow));
        if (QToolBar *torBar = torWindow->findChild<QToolBar *>(
                QLatin1String("NavigationToolBar")))
            torBar->grab().save(QString::fromLocal8Bit(grabDir)
                                + QLatin1String("/nav-tor.png"));
        torWindow->grab().save(QString::fromLocal8Bit(grabDir)
                               + QLatin1String("/window-tor.png"));
        closeWindow(torWindow);
        BrowserApplication::setTorMode(false);

        BrowserApplication::setPrivate(true);
        SubWindow *privateWindow = new SubWindow;
        privateWindow->resize(1100, 700);
        privateWindow->show();
        QVERIFY(QTest::qWaitForWindowExposed(privateWindow));
        if (QToolBar *privateBar = privateWindow->findChild<QToolBar *>(
                QLatin1String("NavigationToolBar")))
            privateBar->grab().save(QString::fromLocal8Bit(grabDir)
                                    + QLatin1String("/nav-private.png"));
        privateWindow->grab().save(QString::fromLocal8Bit(grabDir)
                                   + QLatin1String("/window-private.png"));
        closeWindow(privateWindow);
        BrowserApplication::setPrivate(false);
    }
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
        ProfileSettings settings;
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
    // lock: app-menu button, Back, Forward, Stop/Reload, Home, hidden
    // reader, splitter, sidebar toggle (the stock tail — MENU16
    // removed the pinned wordmark).  The sidebar toggle resolves by
    // its stable item id, not by position.
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

    // Second leg: the tail item (sidebar toggle, resolved by id) can
    // move into the middle — drops are not boundary special cases.
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

    // MENU16: no pinned wordmark anymore — a drop past the last item
    // appends at the tail instead of clamping before fixed chrome.
    // Re-fetch the sidebar button: the earlier drags' remove/insert
    // rebuilt its widget, so the cached pointer's geometry is the
    // pre-drag position.
    QWidget *freshSidebarButton = navBar->widgetForAction(sidebarToggle);
    QVERIFY(freshSidebarButton);
    QTest::mousePress(freshSidebarButton, Qt::LeftButton, Qt::NoModifier,
                      freshSidebarButton->rect().center());
    const QPoint endDrop = freshSidebarButton->mapFrom(
        navBar, QPoint(navBar->width() - 2,
                       navBar->rect().center().y()));
    QTest::mouseMove(freshSidebarButton, endDrop);
    QVERIFY(indicator->isVisible());
    QTest::mouseRelease(freshSidebarButton, Qt::LeftButton, Qt::NoModifier,
                        endDrop);
    QCOMPARE(navBar->actions().last(), sidebarToggle);

    // Esc exits — the entry unlatches, the sheet restores, the order
    // the user left stays (sidebar toggle is the tail now).
    QTest::keyClick(window, Qt::Key_Escape);
    QVERIFY(!window->isNavBarEditMode());
    QVERIFY(!editAction->isChecked());
    QVERIFY(!navBar->styleSheet().contains(QLatin1String("dashed")));
    QCOMPARE(navBar->actions().last(), sidebarToggle);

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
        ProfileSettings settings;
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
        ProfileSettings settings;
        settings.remove(key);
        settings.sync();
    }
    const QStringList stockOrder{
        QStringLiteral("appMenu"), QStringLiteral("back"),
        QStringLiteral("forward"), QStringLiteral("stopReload"),
        QStringLiteral("home"), QStringLiteral("reader"),
        QStringLiteral("siteShield"), QStringLiteral("locationBar"),
        QStringLiteral("sidebar"), QStringLiteral("profileButton")};

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
    // ProfileSettings on release, not on edit-mode exit or window close.
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
        QStringLiteral("siteShield"), QStringLiteral("locationBar"),
        QStringLiteral("sidebar"), QStringLiteral("profileButton")};
    QCOMPARE(window->navBarOrder(), draggedOrder);
    {
        ProfileSettings settings;
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
        QStringLiteral("siteShield"), QStringLiteral("locationBar"),
        QStringLiteral("sidebar"), QStringLiteral("profileButton")};
    QCOMPARE(window->navBarOrder(), resyncedOrder);
    QCOMPARE(second->navBarOrder(), resyncedOrder);
    {
        ProfileSettings settings;
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
        ProfileSettings settings;
        QVERIFY(!settings.value(key).isValid());
    }

    // Stale/partial stored orders degrade gracefully: unknown ids
    // (including ones only another window type carries, like
    // torBadge) drop, and items the list doesn't name keep their
    // stock sequence at the end — the bar can never corrupt.
    {
        ProfileSettings settings;
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
                          QStringLiteral("siteShield"),
                          QStringLiteral("locationBar"),
                          QStringLiteral("profileButton")}));
    closeWindow(third);

    // A corrupt non-list value falls back to stock entirely.
    {
        ProfileSettings settings;
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
        ProfileSettings settings;
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
                          QStringLiteral("siteShield"),
                          QStringLiteral("locationBar"),
                          QStringLiteral("profileButton")}));
    QVERIFY(torBar->styleSheet().contains(QLatin1String("#4a3372")));
    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);

    closeWindow(second);
    closeWindow(window);
    // Leave the store clean for whatever runs next.
    {
        ProfileSettings settings;
        settings.remove(key);
        settings.sync();
    }
}

// TRUST15: the site shield is a permanent nav-row item — one shared
// button immediately right of the reader button (left of the url
// field's splitter slot), bound to the ACTIVE tab.  It is never
// hidden: a widget/engine tab shows it disabled with the reason in
// its tooltip.  The leg exists because a ctor-hidden widget inserted
// via QToolBar::addWidget latches its QWidgetAction invisible and no
// later widget-side setVisible(true) unlatches it — the shield was
// silently absent from the live bar.
void tst_BrowserMainWindow::siteShieldButton()
{
    const QLatin1String key("navbar/order");
    {
        ProfileSettings settings;
        settings.remove(key);
        settings.sync();
    }

    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();

    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    SiteShieldButton *shield = navBar->findChild<SiteShieldButton *>(
        QLatin1String("navSiteShield"));
    QVERIFY(shield);

    // Presence — the widget AND its toolbar action are both visible;
    // the action's flag is what actually controls the slot.
    QAction *shieldAction = nullptr;
    for (QAction *action : navBar->actions()) {
        if (navBar->widgetForAction(action) == shield)
            shieldAction = action;
    }
    QVERIFY(shieldAction);
    QVERIFY(shieldAction->isVisible());
    QVERIFY(!shield->isHidden());
    QVERIFY(!shield->icon().isNull());
    QVERIFY(!shield->icon().pixmap(shield->iconSize()).isNull());

    // The themed glyph resolves in the dark set too — a light<->dark
    // flip re-bakes the icon through the queued scheme-change hook.
    BrowserTheme::previewColorScheme(Qt::ColorScheme::Dark);
    QApplication::processEvents();
    QVERIFY(!shield->icon().isNull());
    QVERIFY(!shield->icon().pixmap(shield->iconSize()).isNull());
    BrowserTheme::applyColorScheme();
    QApplication::processEvents();

    // Position: stock order parks it immediately after "reader" and
    // immediately before "locationBar" — directly left of the field.
    const QStringList order = window->navBarOrder();
    QCOMPARE(order.indexOf(QLatin1String("siteShield")),
             order.indexOf(QLatin1String("reader")) + 1);
    QCOMPARE(order.indexOf(QLatin1String("locationBar")),
             order.indexOf(QLatin1String("siteShield")) + 1);

    // TRUST16 lock: exactly one shield exists in the whole window and
    // it lives on the bar — never inside the url field (a per-tab
    // LocationBar copy must not resurrect at merge).  The field keeps
    // its own left-side surface — the site icon here; the padlock's
    // slot on the merged tree.
    QCOMPARE(window->findChildren<SiteShieldButton *>().size(), 1);
    LocationBar *field = qobject_cast<LocationBar *>(
        window->tabWidget()->currentLocationBar());
    QVERIFY(field);
    QVERIFY(field->findChild<LocationBarSiteIcon *>());
    QVERIFY(!field->findChild<SiteShieldButton *>());

    // Physical adjacency, not just list order: the shield's slot sits
    // flush against the field's — no other surface may slip between.
    QAction *fieldAction = nullptr;
    for (QAction *action : navBar->actions()) {
        if (action->property("navBarItemId").toString()
                == QLatin1String("locationBar"))
            fieldAction = action;
    }
    QVERIFY(fieldAction);
    const QRect shieldRect = navBar->actionGeometry(shieldAction);
    const QRect fieldRect = navBar->actionGeometry(fieldAction);
    QVERIFY(shieldRect.isValid() && fieldRect.isValid());
    QVERIFY(shieldRect.right() <= fieldRect.left());
    QVERIFY(fieldRect.left() - shieldRect.right() <= 20);

    // Offscreen grab for visual review — [reader][shield][field].
    const QByteArray grabDir = qgetenv("ARORA_NAVBAR_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        QDir().mkpath(QString::fromLocal8Bit(grabDir));
        navBar->grab().save(QString::fromLocal8Bit(grabDir)
                            + QLatin1String("/navbar-shield-slot.png"));
    }

    // Web tab: bound and live.
    QVERIFY(window->currentTab());
    QVERIFY(shield->isEnabled());
    QVERIFY(!shield->toolTip().contains(QLatin1String("not available"),
                                        Qt::CaseInsensitive));

    // Widget tab: the shield stays put — disabled with the honest
    // reason — rather than disappearing from the row.
    QVERIFY(QMetaObject::invokeMethod(window, "preferences"));
    QVERIFY(!window->currentTab());
    QVERIFY(!shield->isHidden());
    QVERIFY(shieldAction->isVisible());
    QVERIFY(!shield->isEnabled());
    QVERIFY(shield->toolTip().contains(QLatin1String("not available"),
                                       Qt::CaseInsensitive));

    // Back on a web tab the same button rebinds live.
    window->tabWidget()->setCurrentIndex(0);
    QVERIFY(window->currentTab());
    QVERIFY(shield->isEnabled());

    // TRUST16/NAVB03: the shield stays a normal movable item in edit
    // mode — a drag can park it ahead of Back — but Reset restores
    // the locked stock slot directly left of the url field.
    window->navBarEditAction()->setChecked(true);
    QVERIFY(window->isNavBarEditMode());
    QAction *backAction = navBar->actions().at(1);
    QCOMPARE(backAction->property("navBarItemId").toString(),
             QLatin1String("back"));
    QTest::mousePress(shield, Qt::LeftButton, Qt::NoModifier,
                      shield->rect().center());
    const QPoint shieldDrop = shield->mapFrom(
        navBar, QPoint(navBar->actionGeometry(backAction).left()
                       + navBar->actionGeometry(backAction).width() / 4,
                       navBar->actionGeometry(backAction).center().y()));
    QTest::mouseMove(shield, shieldDrop);
    QTest::mouseRelease(shield, Qt::LeftButton, Qt::NoModifier,
                        shieldDrop);
    QCOMPARE(window->navBarOrder().indexOf(QLatin1String("siteShield")),
             1);
    window->navBarResetAction()->trigger();
    const QStringList resetOrder = window->navBarOrder();
    QCOMPARE(resetOrder.indexOf(QLatin1String("siteShield")),
             resetOrder.indexOf(QLatin1String("reader")) + 1);
    QCOMPARE(resetOrder.indexOf(QLatin1String("locationBar")),
             resetOrder.indexOf(QLatin1String("siteShield")) + 1);
    window->navBarEditAction()->setChecked(false);
    QVERIFY(!window->isNavBarEditMode());

    // Rebuild leg — a stored order performs the remove/add round-trip
    // on every bar action (the QWidgetAction unembed/re-embed path);
    // a fresh window on that order is also the session-restore shape.
    {
        ProfileSettings settings;
        settings.setValue(key, QStringList{
            QStringLiteral("locationBar"), QStringLiteral("back"),
            QStringLiteral("appMenu"), QStringLiteral("sidebar")});
        settings.sync();
    }
    SubWindow *restored = new SubWindow;
    restored->resize(1200, 700);
    restored->show();
    QApplication::processEvents();
    QToolBar *restoredBar = restored->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(restoredBar);
    SiteShieldButton *restoredShield =
        restoredBar->findChild<SiteShieldButton *>(
            QLatin1String("navSiteShield"));
    QVERIFY(restoredShield);
    QVERIFY(!restoredShield->isHidden());
    QVERIFY(restoredShield->isEnabled());
    QVERIFY(restored->navBarOrder().contains(
        QLatin1String("siteShield")));
    closeWindow(restored);
    {
        ProfileSettings settings;
        settings.remove(key);
        settings.sync();
    }

    closeWindow(window);
}

// PROF01: the profile button is the rightmost nav-bar widget — its
// face matched to the app-menu logo on the left — dropping the
// single-profile identity menu (circular avatar + editable name).
// The name persists in the profile registry and repaints live on
// every window through the shared ProfileIdentity; the default
// avatar is the bundled silhouette until PROF02 lets the user pick a
// picture.
void tst_BrowserMainWindow::profileButton()
{
    {
        QSettings settings;
        settings.remove(QLatin1String("navbar/order"));
        settings.sync();
        // MPROF01: the display name lives in the profile registry —
        // reset it there, not in QSettings.
        ProfileContext context =
            ProfileRegistry::instance()->activeProfile();
        context.name = QStringLiteral("Profile");
        ProfileRegistry::instance()->updateProfile(context);
    }

    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();
    QToolBar *navBar = window->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(navBar);
    QToolButton *button = window->findChild<QToolButton *>(
        QLatin1String("profileButton"));
    QVERIFY(button);
    QVERIFY(button->autoRaise());
    QCOMPARE(button->toolButtonStyle(), Qt::ToolButtonIconOnly);
    QCOMPARE(button->popupMode(), QToolButton::InstantPopup);
    QCOMPARE(button->text(), QLatin1String("Profile"));
    QVERIFY(!button->toolTip().isEmpty());
    QVERIFY(!button->accessibleName().isEmpty());
    QVERIFY(!button->icon().isNull());

    // Rightmost: the button's own action is the bar's last item and
    // carries the stable NAVB04 id so edit mode covers it.
    QAction *profileAction = nullptr;
    for (QAction *action : navBar->actions()) {
        if (navBar->widgetForAction(action) == button)
            profileAction = action;
    }
    QVERIFY(profileAction);
    QCOMPARE(navBar->actions().last(), profileAction);
    QCOMPARE(profileAction->property("navBarItemId").toString(),
             QLatin1String("profileButton"));

    // Face-matched to the app-menu logo on the left — same oversized
    // iconSize (~1.6x the bar's 18px) and the same capped square
    // face one pixel over the glyph.
    QToolButton *appButton = window->findChild<QToolButton *>(
        QLatin1String("appMenuButton"));
    QVERIFY(appButton);
    QCOMPARE(button->iconSize(), appButton->iconSize());
    QCOMPARE(button->sizeHint().width(), button->sizeHint().height());
    QVERIFY(button->sizeHint().height()
            >= button->iconSize().height());
    QVERIFY(button->sizeHint().height()
            <= button->iconSize().height() + 1);

    // The dropdown scaffolds the identity menu: a header row with a
    // 32px circular avatar + the persisted profile name, then the
    // rename entry — PROF02/PROF03 append below.
    QMenu *menu = button->menu();
    QVERIFY(menu);
    QLabel *nameLabel = menu->findChild<QLabel *>(
        QLatin1String("profileMenuName"));
    QVERIFY(nameLabel);
    QCOMPARE(nameLabel->text(), QLatin1String("Profile"));
    QLabel *avatarLabel = menu->findChild<QLabel *>(
        QLatin1String("profileMenuAvatar"));
    QVERIFY(avatarLabel);
    QVERIFY(!avatarLabel->pixmap().isNull());
    QAction *rename = menu->findChild<QAction *>(
        QLatin1String("profileRenameAction"));
    QVERIFY(rename);

    // No custom avatar under the isolated test profile — the face is
    // the bundled silhouette, a real mostly-painted disc.
    QVERIFY(!ProfileIdentity::instance()->hasCustomAvatar());
    const QImage face =
        button->icon().pixmap(button->iconSize()).toImage();
    int opaque = 0;
    for (int y = 0; y < face.height(); ++y)
        for (int x = 0; x < face.width(); ++x)
            if (face.pixelColor(x, y).alpha() > 100)
                ++opaque;
    QVERIFY(opaque > face.width() * face.height() / 2);

    // Rename leg: the dialog persists the registry name and the shared
    // ProfileIdentity repaints every window's header live.
    SubWindow *second = new SubWindow;
    second->show();
    QApplication::processEvents();
    QToolButton *secondButton = second->findChild<QToolButton *>(
        QLatin1String("profileButton"));
    QVERIFY(secondButton);
    QMenu *secondMenu = secondButton->menu();
    QVERIFY(secondMenu);
    QLabel *secondName = secondMenu->findChild<QLabel *>(
        QLatin1String("profileMenuName"));
    QVERIFY(secondName);
    QCOMPARE(secondName->text(), QLatin1String("Profile"));

    QTimer::singleShot(150, qApp, []() {
        if (QInputDialog *dialog = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
            dialog->setTextValue(QLatin1String("Ada"));
            dialog->accept();
        }
    });
    rename->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        ProfileIdentity::instance()->name(), QLatin1String("Ada"),
        5000);
    QCOMPARE(ProfileRegistry::instance()->activeProfile().name,
             QLatin1String("Ada"));
    QCOMPARE(nameLabel->text(), QLatin1String("Ada"));
    QCOMPARE(secondName->text(), QLatin1String("Ada"));

    // A blank rename falls back to the default — no empty header.
    QTimer::singleShot(150, qApp, []() {
        if (QInputDialog *dialog = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
            dialog->setTextValue(QLatin1String("   "));
            dialog->accept();
        }
    });
    rename->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        ProfileIdentity::instance()->name(), QLatin1String("Profile"),
        5000);
    QCOMPARE(nameLabel->text(), QLatin1String("Profile"));

    closeWindow(second);
    closeWindow(window);

    // Tor window: the button keeps the bar's right edge — the "Tor"
    // badge parks one slot left of it — and the neutral silhouette
    // stays legible on the violet chrome (the disc's ~150 luminance
    // reads far above the ~55 violet).
    BrowserApplication::setTorMode(true);
    SubWindow *torWindow = new SubWindow;
    torWindow->resize(1200, 700);
    torWindow->show();
    QApplication::processEvents();
    QToolBar *torBar = torWindow->findChild<QToolBar *>(
        QLatin1String("NavigationToolBar"));
    QVERIFY(torBar);
    QToolButton *torButton = torWindow->findChild<QToolButton *>(
        QLatin1String("profileButton"));
    QVERIFY(torButton);
    QAction *torProfile = nullptr;
    QAction *torBadge = nullptr;
    for (QAction *action : torBar->actions()) {
        if (torBar->widgetForAction(action) == torButton)
            torProfile = action;
        if (action->property("navBarItemId").toString()
                == QLatin1String("torBadge"))
            torBadge = action;
    }
    QVERIFY(torProfile);
    QCOMPARE(torBar->actions().last(), torProfile);
    QVERIFY(torBadge);
    QCOMPARE(torBar->actions().indexOf(torBadge),
             torBar->actions().indexOf(torProfile) - 1);
    const QImage torFace =
        torButton->icon().pixmap(QSize(29, 29)).toImage();
    qlonglong lum = 0;
    int torOpaque = 0;
    for (int y = 0; y < torFace.height(); ++y)
        for (int x = 0; x < torFace.width(); ++x) {
            const QColor color = torFace.pixelColor(x, y);
            if (color.alpha() > 100) {
                lum += qGray(color.rgb());
                ++torOpaque;
            }
        }
    QVERIFY(torOpaque > 0);
    QVERIFY(lum / torOpaque > 100);

    closeWindow(torWindow);
    BrowserApplication::setTorMode(false);

    {
        ProfileContext context =
            ProfileRegistry::instance()->activeProfile();
        context.name = QStringLiteral("Profile");
        ProfileRegistry::instance()->updateProfile(context);
    }
}

// MPROF05: the profile menu carries the switcher submenu and the
// Profile Manager entry; the manager is also reachable from Tools so
// it is discoverable without the nav button (the app menu embeds the
// shared Tools menu).  Since MPROF03's window binding, every 'New
// Window' row is enabled and opens a window bound to that profile.
void tst_BrowserMainWindow::profileMenuEntries()
{
    SubWindow *window = new SubWindow;
    window->resize(1400, 700);
    window->show();
    QApplication::processEvents();
    QToolButton *button = window->findChild<QToolButton *>(
        QLatin1String("profileButton"));
    QVERIFY(button);
    QMenu *menu = button->menu();
    QVERIFY(menu);

    QAction *managerAction = menu->findChild<QAction *>(
        QLatin1String("profileManagerAction"));
    QVERIFY(managerAction);
    QVERIFY(!managerAction->text().isEmpty());

    QAction *submenuAction = menu->findChild<QAction *>(
        QLatin1String("profileNewWindowMenu"));
    QVERIFY(submenuAction);
    QMenu *newWindowMenu = submenuAction->menu();
    QVERIFY(newWindowMenu);

    // The submenu populates on open.  Offscreen, show() fires
    // aboutToShow the same way a real popup does.
    newWindowMenu->show();
    QApplication::processEvents();
    newWindowMenu->hide();
    QVERIFY(!newWindowMenu->actions().isEmpty());
    QAction *defaultEntry = nullptr;
    for (QAction *action : newWindowMenu->actions()) {
        if (action->data().toString() == QLatin1String("default"))
            defaultEntry = action;
    }
    QVERIFY(defaultEntry);
    QVERIFY(defaultEntry->isEnabled());
    QVERIFY(defaultEntry->isChecked());

    // A second profile lists as an enabled row — triggering it opens
    // a real window bound to that persona (MPROF03).
    ProfileStore *store = ProfileStore::instance();
    QVERIFY(store->createProfile(QLatin1String("Suite")).isValid());
    newWindowMenu->show();
    QApplication::processEvents();
    newWindowMenu->hide();
    QAction *suiteEntry = nullptr;
    for (QAction *action : newWindowMenu->actions()) {
        if (action->data().toString() == QLatin1String("suite"))
            suiteEntry = action;
    }
    QVERIFY(suiteEntry);
    QVERIFY(suiteEntry->isEnabled());
    const int windowsBefore =
        BrowserApplication::instance()->mainWindows().count();
    suiteEntry->trigger();
    QApplication::processEvents();
    BrowserMainWindow *suiteWindow = nullptr;
    for (BrowserMainWindow *candidate
         : BrowserApplication::instance()->mainWindows()) {
        if (candidate && candidate->profileId()
                == QLatin1String("suite"))
            suiteWindow = candidate;
    }
    QVERIFY(suiteWindow);
    QCOMPARE(BrowserApplication::instance()->mainWindows().count(),
             windowsBefore + 1);
    closeWindow(suiteWindow);
    store->deleteProfile(QLatin1String("suite"));

    // Triggering the manager action opens the shared dialog; closing
    // drops the singleton so the next open rereads the registry.
    managerAction->trigger();
    QTRY_VERIFY(ProfileManagerDialog::instance());
    ProfileManagerDialog::instance()->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QVERIFY(!ProfileManagerDialog::instance());

    // Tools > Profiles... reaches the same manager.
    QAction *toolsAction = window->findChild<QAction *>(
        QLatin1String("toolsProfilesAction"));
    QVERIFY(toolsAction);
    QVERIFY(toolsAction->isVisible());
    toolsAction->trigger();
    QTRY_VERIFY(ProfileManagerDialog::instance());
    QCOMPARE(ProfileManagerDialog::instance()->profileTree
                 ->topLevelItemCount() >= 1, true);
    ProfileManagerDialog::instance()->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    closeWindow(window);
}

QTEST_MAIN(tst_BrowserMainWindow)
#include "tst_browsermainwindow.moc"
