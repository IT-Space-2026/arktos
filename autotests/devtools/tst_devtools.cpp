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
// profile, re-pointing on tab/page switch, the View > Development
// Tools menu action, clean teardown (window close unbinds,
// inspected-page death closes the window), and the devtools frontend
// actually loading.

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

#include "bidiclient.h"
#include "bidipanel.h"

#include <qcombobox.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qplaintextedit.h>
#include <qstackedwidget.h>
#include <qtablewidget.h>
#include <qtreewidget.h>

class tst_DevTools : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanup();

    void inspectBindsHostPage();
    void devToolsFrontendLoads();
    void rebindsToCurrentPage();
    void devToolsMenuOpensInspector();
    void closeReleasesBinding();
    void inspectedPageDestroyedClosesWindow();
    void bidiPanel();
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

void tst_DevTools::devToolsMenuOpensInspector()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    window->setAttribute(Qt::WA_DeleteOnClose);
    window->show();
    WebView *view = window->currentTab();
    QVERIFY(view);

    // DEVT04: the entry lives under Tools > Development Tools as
    // "Chromium Dev Tool" — the View menu no longer carries it.
    QMenu *devToolsMenu = nullptr;
    const QList<QMenu *> menus = window->findChildren<QMenu *>();
    for (QMenu *menu : menus) {
        if (menu->title() == QLatin1String("Development Tools"))
            devToolsMenu = menu;
    }
    QVERIFY(devToolsMenu);

    QAction *inspector = nullptr;
    for (QAction *action : devToolsMenu->actions()) {
        if (action->text() == QLatin1String("Chromium Dev Tool"))
            inspector = action;
    }
    QVERIFY(inspector);
    QVERIFY(inspector->isEnabled());

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

// COV08: the BiDi panel's JSON handlers — driven through the
// AUTOTESTS seams so no live debug channel is needed.  Tag constants
// mirror bidipanel.cpp: 1 console, 2 dom-inject, 3 dom-children,
// 4 localStorage, 5 cookies; any unregistered id lands on the
// getTree bookkeeping path (tag 0).
void tst_DevTools::bidiPanel()
{
    const QByteArray ok = QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"object\""
        ",\"value\":[1,2]}}}");
    const QByteArray domAnswer = QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"div\\\",\\\"children\\\":2}"
        ",{\\\"label\\\":\\\"span\\\",\\\"children\\\":0}]\"}}}");

    BidiPanel panel;
    panel.show(); // showEvent -> connectToEngine (no endpoint to find)

    // Disconnected/unavailable states park on the readable page.
    panel.onConnectionChanged(BidiClient::Disconnected);
    QCOMPARE(panel.m_stack->currentIndex(), 0);
    QVERIFY(!panel.m_unavailableLabel->text().isEmpty());
    panel.onConnectionChanged(BidiClient::Unavailable);
    QCOMPARE(panel.m_stack->currentIndex(), 0);

    // Connected flips to the tabs page and kicks a getTree refresh.
    panel.onConnectionChanged(BidiClient::Connected);
    QCOMPARE(panel.m_stack->currentIndex(), 1);

    // No context yet: the refresh early-outs.
    panel.refreshDom();
    QCOMPARE(panel.m_domTree->topLevelItemCount(), 0);

    // getTree answer on an unregistered id: tag 0 fills the combo and
    // cascades refreshDom + refreshStorage.
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"title\":\"One\",\"url\":\"http://a/\"},"
        "{\"context\":\"ctx2\",\"url\":\"http://b/\"}]}}"));
    QCOMPARE(panel.m_contextCombo->count(), 2);
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));
    QCOMPARE(panel.m_domTree->topLevelItemCount(), 1);
    QTreeWidgetItem *root = panel.m_domTree->topLevelItem(0);
    QVERIFY(root);
    QCOMPARE(root->childCount(), 1); // the loading placeholder

    // A second getTree keeps the current selection when the context
    // still exists — and rebuilds the DOM tree (the old items are
    // deleted; re-fetch the root).
    panel.m_contextCombo->setCurrentIndex(1);
    panel.onResponse(7778, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"title\":\"One\",\"url\":\"http://a/\"},"
        "{\"context\":\"ctx2\",\"url\":\"http://b/\"}]}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx2"));
    root = panel.m_domTree->topLevelItem(0);
    QVERIFY(root);

    // Console eval: "> expr" echo then the tagged answers.
    panel.m_evalLine->setText(QStringLiteral("1+1"));
    emit panel.m_evalLine->returnPressed();
    QVERIFY(panel.m_evalLine->text().isEmpty());
    QVERIFY(panel.m_console->toPlainText().contains(
        QStringLiteral("> 1+1")));

    quint64 id = panel.testRegisterTag(1); // console, plain value
    panel.onResponse(id, ok);
    QVERIFY(panel.m_console->toPlainText().contains(
        QStringLiteral("< [1,2]")));
    id = panel.testRegisterTag(1); // console, undefined collapses
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":"
        "{\"type\":\"undefined\"}}}"));
    QVERIFY(panel.m_console->toPlainText().contains(
        QStringLiteral("< undefined")));
    id = panel.testRegisterTag(1); // console, exceptionDetails text
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"exception\",\"exceptionDetails\":"
        "{\"text\":\"ReferenceError: nope\"}}}"));
    QVERIFY(panel.m_console->toPlainText().contains(
        QStringLiteral("< ReferenceError: nope")));
    id = panel.testRegisterTag(1); // console, top-level error envelope
    panel.onResponse(id, QByteArray(
        "{\"error\":{\"message\":\"denied\"}}"));
    QVERIFY(panel.m_console->toPlainText().contains(
        QStringLiteral("< error: denied")));
    id = panel.testRegisterTag(4); // error envelope, non-console tag
    panel.onResponse(id, QByteArray(
        "{\"error\":{\"message\":\"denied\"}}"));
    id = panel.testRegisterTag(2); // exception envelope, non-console tag
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"exception\",\"exceptionDetails\":"
        "{\"text\":\"quiet\"}}}"));

    // DOM: the helper-inject answer re-expands the root placeholder,
    // then the children answer materializes real items with index
    // paths and placeholders of their own.
    id = panel.testRegisterTag(2); // TagDomInject
    panel.onResponse(id, ok);
    id = panel.testDomExpand(root);
    panel.onResponse(id, domAnswer);
    QCOMPARE(root->childCount(), 2);
    QCOMPARE(root->child(0)->text(0), QStringLiteral("div"));
    QCOMPARE(root->child(0)->data(0, Qt::UserRole).toString(),
             QStringLiteral("0"));
    QCOMPARE(root->child(0)->childCount(), 1); // nested placeholder
    QCOMPARE(root->child(1)->childCount(), 0);
    // Deeper level: the path grows by one index segment.
    id = panel.testDomExpand(root->child(0));
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"em\\\",\\\"children\\\":0}]\"}}}"));
    QCOMPARE(root->child(0)->child(0)->data(0, Qt::UserRole).toString(),
             QStringLiteral("0/0"));
    // A tagged children answer whose item already resolved is a no-op.
    id = panel.testRegisterTag(3);
    panel.onResponse(id, domAnswer);

    // domItemExpanded's own guards: null, fully materialized, and a
    // live placeholder (the eval goes out but nobody answers).
    panel.domItemExpanded(nullptr);
    panel.domItemExpanded(root); // real children already
    QTreeWidgetItem *fresh = new QTreeWidgetItem(
        QStringList(QStringLiteral("article")));
    fresh->setData(0, Qt::UserRole, QStringLiteral("1"));
    fresh->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("loading…"))));
    panel.m_domTree->addTopLevelItem(fresh);
    panel.domItemExpanded(fresh);

    // Storage tables.
    id = panel.testRegisterTag(4); // localStorage pairs
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[[\\\"token\\\",\\\"abc\\\"]]\"}}}"));
    QCOMPARE(panel.m_localStorageTable->rowCount(), 1);
    QCOMPARE(panel.m_localStorageTable->item(0, 0)->text(),
             QStringLiteral("token"));
    QCOMPARE(panel.m_localStorageTable->item(0, 1)->text(),
             QStringLiteral("abc"));
    id = panel.testRegisterTag(5); // cookies
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"cookies\":[{\"name\":\"n\",\"value\":\"v\""
        ",\"domain\":\".site.test\",\"path\":\"/\"}]}}"));
    QCOMPARE(panel.m_cookieTable->rowCount(), 1);
    QCOMPARE(panel.m_cookieTable->item(0, 2)->text(),
             QStringLiteral(".site.test"));
    panel.refreshStorage(); // both requests go out under a live context

    // Events: console log line, context lifecycle, navigation of the
    // selected context vs another, and the network row lifecycle.
    panel.onEvent(QByteArray(
        "{\"method\":\"log.entryAdded\",\"params\":{\"level\":\"warn\""
        ",\"text\":\"hello\"}}"));
    QVERIFY(panel.m_console->toPlainText().contains(
        QStringLiteral("[warn] hello")));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.contextCreated\",\"params\":{}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.contextDestroyed\",\"params\":{}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx2\"}}")); // matches current -> refreshDom
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"elsewhere\"}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"unrelated.method\",\"params\":{}}"));
    panel.onEvent(QByteArray("{not json"));

    panel.m_networkTable->setRowCount(0);
    panel.m_requestRows.clear();
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{\"request\":"
        "{\"request\":\"r1\",\"method\":\"GET\",\"url\":\"http://x/\"}}}"));
    QCOMPARE(panel.m_networkTable->rowCount(), 1);
    QCOMPARE(panel.m_networkTable->item(0, 2)->text(),
             QStringLiteral("pending"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{\"request\":"
        "{\"request\":\"r1\"},\"response\":{\"status\":200}}}"));
    QCOMPARE(panel.m_networkTable->item(0, 1)->text(),
             QStringLiteral("200"));
    QCOMPARE(panel.m_networkTable->item(0, 2)->text(),
             QStringLiteral("receiving"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{\"request\":"
        "{\"request\":\"r1\"}}}"));
    QCOMPARE(panel.m_networkTable->item(0, 2)->text(),
             QStringLiteral("finished"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.fetchError\",\"params\":{\"request\":"
        "{\"request\":\"r1\"},\"errorText\":\"dns\"}}"));
    QVERIFY(panel.m_networkTable->item(0, 2)->text().contains(
        QStringLiteral("dns")));
    // Unknown request ids and unknown network methods are ignored.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{\"request\":"
        "{\"request\":\"ghost\"},\"response\":{\"status\":404}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.dataReceived\",\"params\":{\"request\":"
        "{\"request\":\"r1\"}}}"));

    // The 1000-row cap trims and rebases the request-id index.
    for (int i = 0; i < 1005; ++i) {
        panel.onEvent(QByteArray(
            "{\"method\":\"network.beforeRequestSent\",\"params\":{\"request\":"
            "{\"request\":\"cap") + QByteArray::number(i)
            + QByteArray("\",\"method\":\"GET\",\"url\":\"http://c/\"}}}"));
    }
    QVERIFY(panel.m_networkTable->rowCount() <= 1000);
}

QTEST_MAIN(tst_DevTools)
#include "tst_devtools.moc"
