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

// DVT01: the DevTools host window — InspectElement is a no-op without
// a bound devToolsPage, so every entry point routes through
// DevToolsWindow::inspectElement().  Covered: binding on the right
// profile, re-pointing on tab/page switch, the Tools-menu action,
// clean teardown (window close unbinds, inspected-page death closes
// the window), and the devtools frontend actually loading.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qmenubar.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>

#include "browsermainwindow.h"
#include "devtoolswindow.h"
#include "webpage.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_DevTools : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void inspectBindsHostPage();
    void devToolsFrontendLoads();
    void rebindsToCurrentPage();
    void toolsMenuOpensInspector();
    void closeReleasesBinding();
    void inspectedPageDestroyedClosesWindow();
};

void tst_DevTools::initTestCase()
{
    QCoreApplication::setApplicationName("tst_devtools");
    QSettings settings;
    settings.clear();
}

void tst_DevTools::cleanup()
{
    if (DevToolsWindow *window = DevToolsWindow::instance()) {
        window->close();
        QTRY_VERIFY(!DevToolsWindow::instance());
    }
}

static void loadTestPage(WebView *view)
{
    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->setHtml(QStringLiteral("<html><head><title>target</title></head>"
                                 "<body><p>inspect me</p></body></html>"),
                  QUrl(QStringLiteral("http://localhost/")));
    QTRY_VERIFY(loaded.count() >= 1);
}

void tst_DevTools::inspectBindsHostPage()
{
    WebView view;
    view.show();
    loadTestPage(&view);
    QVERIFY(!view.page()->devToolsPage());

    DevToolsWindow::inspectElement(view.page());

    DevToolsWindow *window = DevToolsWindow::instance();
    QVERIFY(window);
    QVERIFY(window->isVisible());
    QCOMPARE(window->inspectedPage(), static_cast<QWebEnginePage *>(view.page()));
    QCOMPARE(view.page()->devToolsPage(), window->view()->page());
    // The host page must ride on the inspected page's own profile.
    QCOMPARE(window->view()->page()->profile(), view.page()->profile());
    // DevTools needs JS even if the Safest tier disabled it globally.
    QVERIFY(window->view()->page()->settings()->testAttribute(
            QWebEngineSettings::JavascriptEnabled));
}

void tst_DevTools::devToolsFrontendLoads()
{
    WebView view;
    view.show();
    loadTestPage(&view);

    DevToolsWindow::inspectElement(view.page());
    QWebEnginePage *host = view.page()->devToolsPage();
    QVERIFY(host);

    // InspectElement drives the devtools://devtools/ frontend into the
    // host page — the binding is real, not just assigned.
    QTRY_COMPARE(host->url().scheme(), QStringLiteral("devtools"));
    QSignalSpy hostLoaded(host, &QWebEnginePage::loadFinished);
    QTRY_VERIFY_WITH_TIMEOUT(hostLoaded.count() >= 1, 10000);
}

void tst_DevTools::rebindsToCurrentPage()
{
    WebView viewA;
    WebView viewB;
    viewA.show();
    viewB.show();
    loadTestPage(&viewA);
    loadTestPage(&viewB);

    DevToolsWindow::inspectElement(viewA.page());
    DevToolsWindow *window = DevToolsWindow::instance();
    QWebEnginePage *host = viewA.page()->devToolsPage();
    QVERIFY(host);

    // Inspecting another page (a tab switch) moves the same host view.
    DevToolsWindow::inspectElement(viewB.page());
    QCOMPARE(DevToolsWindow::instance(), window);
    QCOMPARE(window->inspectedPage(), static_cast<QWebEnginePage *>(viewB.page()));
    QCOMPARE(viewB.page()->devToolsPage(), host);
    QVERIFY(!viewA.page()->devToolsPage());
}

void tst_DevTools::toolsMenuOpensInspector()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    WebView *view = window->currentTab();
    QVERIFY(view);

    QAction *inspector = nullptr;
    const QList<QAction *> actions = window->findChildren<QAction *>();
    for (QAction *action : actions) {
        if (action->text().contains(QLatin1String("Inspector")))
            inspector = action;
    }
    QVERIFY(inspector);

    inspector->trigger();
    DevToolsWindow *host = DevToolsWindow::instance();
    QVERIFY(host);
    QVERIFY(host->isVisible());
    QCOMPARE(host->inspectedPage(), static_cast<QWebEnginePage *>(view->page()));
    QCOMPARE(view->page()->devToolsPage(), host->view()->page());

    // Closing the inspected window drops the binding.
    QPointer<QWebEnginePage> page = view->page();
    window->close();
    delete window;
    QTRY_VERIFY(!DevToolsWindow::instance() || !DevToolsWindow::instance()->isVisible());
    QTRY_VERIFY(!page || !page->devToolsPage());
}

void tst_DevTools::closeReleasesBinding()
{
    WebView view;
    view.show();
    DevToolsWindow::inspectElement(view.page());
    DevToolsWindow *window = DevToolsWindow::instance();
    QVERIFY(window);
    QCOMPARE(view.page()->devToolsPage(), window->view()->page());

    window->close();
    QVERIFY(!view.page()->devToolsPage());
    // WA_DeleteOnClose — the next inspect builds a fresh host.
    QTRY_VERIFY(!DevToolsWindow::instance());
    DevToolsWindow::inspectElement(view.page());
    QVERIFY(DevToolsWindow::instance());
    QVERIFY(DevToolsWindow::instance() != window);
    QCOMPARE(view.page()->devToolsPage(), DevToolsWindow::instance()->view()->page());
}

void tst_DevTools::inspectedPageDestroyedClosesWindow()
{
    WebView *view = new WebView;
    view->show();
    DevToolsWindow::inspectElement(view->page());
    QPointer<DevToolsWindow> window = DevToolsWindow::instance();
    QVERIFY(window);

    delete view;
    QTRY_VERIFY(!window);
}

QTEST_MAIN(tst_DevTools)
#include "tst_devtools.moc"
