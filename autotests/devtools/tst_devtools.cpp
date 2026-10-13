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

#include <qabstractitemmodel.h>
#include <qbuttongroup.h>
#include <qcheckbox.h>
#include <qcombobox.h>
#include <qheaderview.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qplaintextedit.h>
#include <qscrollbar.h>
#include <qsortfilterproxymodel.h>
#include <qstackedwidget.h>
#include <qtableview.h>
#include <qtablewidget.h>
#include <qtabwidget.h>
#include <qtoolbutton.h>
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

    // DEVT06: the Network tab is now a model/proxy/view — drive the
    // Chrome-style lifecycle through onEvent.
    QAbstractTableModel *net = panel.m_networkModel;
    QSortFilterProxyModel *netProxy = panel.m_networkProxy;
    QVERIFY(net);
    QVERIFY(netProxy);
    QVERIFY(panel.m_networkView);
    QVERIFY(panel.m_networkView->model() == netProxy);
    QVERIFY(panel.m_networkView->parentWidget()->findChild<QWidget *>(
                QStringLiteral("bidiNetworkToolbar")));
    QVERIFY(panel.m_networkView->parentWidget()->findChild<QWidget *>(
                QStringLiteral("bidiNetworkChips")));
    // Header order matches Chrome: Name Status Type Initiator Size
    // Time Waterfall.
    QCOMPARE(net->columnCount(), 7);
    QCOMPARE(net->headerData(0, Qt::Horizontal).toString(),
             QStringLiteral("Name"));
    QCOMPARE(net->headerData(6, Qt::Horizontal).toString(),
             QStringLiteral("Waterfall"));

    panel.clearNetworkLog();
    // A document row: name from the URL tail, bucket + initiator from
    // the event extras, "-" Time while pending.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1000.0,\"initiator\":{\"type\":\"other\"},"
        "\"request\":{\"request\":\"r1\",\"method\":\"GET\","
        "\"url\":\"http://site/page.html\",\"type\":\"Document\"}}}"));
    QCOMPARE(net->rowCount(), 1);
    QCOMPARE(net->data(net->index(0, 0)).toString(),
             QStringLiteral("page.html"));
    QCOMPARE(net->data(net->index(0, 2)).toString(),
             QStringLiteral("document"));
    QCOMPARE(net->data(net->index(0, 5)).toString(),
             QStringLiteral("-"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":1020.0,\"type\":\"Document\",\"request\":{"
        "\"request\":\"r1\"},\"response\":{\"status\":200}}}"));
    QCOMPARE(net->data(net->index(0, 1)).toString(),
             QStringLiteral("200"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":1042.0,\"request\":{\"request\":\"r1\"},"
        "\"response\":{\"encodedDataLength\":2048}}}"));
    QCOMPARE(net->data(net->index(0, 4)).toString(),
             QStringLiteral("2.0 kB"));
    QCOMPARE(net->data(net->index(0, 5)).toString(),
             QStringLiteral("42 ms"));
    QVERIFY(!net->data(net->index(0, 0), Qt::ForegroundRole).isValid());

    // A script with a parser initiator carrying url:line.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1010.0,\"initiator\":{\"type\":\"parser\","
        "\"url\":\"http://site/page.html\",\"lineNumber\":7},"
        "\"request\":{\"request\":\"r2\",\"method\":\"GET\","
        "\"url\":\"http://site/app.js\",\"type\":\"Script\"}}}"));
    QCOMPARE(net->data(net->index(1, 0)).toString(),
             QStringLiteral("app.js"));
    QCOMPARE(net->data(net->index(1, 3)).toString(),
             QStringLiteral("page.html:7"));

    // A 404 image row: the status is real but the row still goes red.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1015.0,\"request\":{\"request\":\"r3\","
        "\"method\":\"GET\",\"url\":\"http://cdn/pic.png\","
        "\"type\":\"Image\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":1030.0,\"type\":\"Image\",\"request\":{"
        "\"request\":\"r3\"},\"response\":{\"status\":404}}}"));
    QCOMPARE(net->data(net->index(2, 1)).toString(),
             QStringLiteral("404"));
    QVERIFY(net->data(net->index(2, 0), Qt::ForegroundRole).isValid());

    // A failed fetch: status cell shows the net::ERR text, row red.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1100.0,\"request\":{\"request\":\"r4\","
        "\"method\":\"POST\",\"url\":\"http://site/api/data\","
        "\"type\":\"Fetch\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.fetchError\",\"params\":{\"request\":"
        "{\"request\":\"r4\"},\"errorText\":\"net::ERR_REFUSED\","
        "\"timestamp\":1150.0,\"type\":\"Fetch\"}}"));
    QCOMPARE(net->data(net->index(3, 1)).toString(),
             QStringLiteral("net::ERR_REFUSED"));
    QVERIFY(net->data(net->index(3, 0), Qt::ForegroundRole).isValid());

    // A websocket that stays pending.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1200.0,\"request\":{\"request\":\"r5\","
        "\"method\":\"GET\",\"url\":\"ws://site/socket\","
        "\"type\":\"WebSocket\"}}}"));
    QCOMPARE(net->rowCount(), 5);
    QCOMPARE(netProxy->rowCount(), 5);

    // DEVT07: Chrome's footer line — count, transferred bytes, and
    // the Finish span track the visible (filtered) rows.  Only r1
    // carries a real byte count (2048); r4's end (1150) is the last
    // completion after r1's start (1000).
    QVERIFY(panel.m_networkSummary);
    QCOMPARE(panel.m_networkSummary->objectName(),
             QStringLiteral("bidiNetworkSummary"));
    QCOMPARE(panel.m_networkSummary->text(),
             QStringLiteral("5 requests · 2.0 kB transferred "
                            "· Finish: 150 ms"));

    // Chips are an exclusive filtered view over the same rows.
    const auto chip = [&panel](const QString &label) -> QAbstractButton * {
        for (QAbstractButton *button : panel.m_typeChips->buttons()) {
            if (button->text() == label)
                return button;
        }
        return nullptr;
    };
    QCOMPARE(panel.m_typeChips->buttons().count(), 12);
    chip(QStringLiteral("Doc"))->click();
    QCOMPARE(netProxy->rowCount(), 1);
    // The footer re-slices with the filter — r1 alone finished in 42ms
    // (and gets the singular noun, like Chrome).
    QCOMPARE(panel.m_networkSummary->text(),
             QStringLiteral("1 request · 2.0 kB transferred "
                            "· Finish: 42 ms"));
    chip(QStringLiteral("Fetch/XHR"))->click();
    QCOMPARE(netProxy->rowCount(), 1);
    chip(QStringLiteral("Img"))->click();
    QCOMPARE(netProxy->rowCount(), 1);
    chip(QStringLiteral("JS"))->click();
    QCOMPARE(netProxy->rowCount(), 1);
    chip(QStringLiteral("Socket"))->click();
    QCOMPARE(netProxy->rowCount(), 1);
    chip(QStringLiteral("Other"))->click();
    QCOMPARE(netProxy->rowCount(), 0);
    chip(QStringLiteral("All"))->click();
    QCOMPARE(netProxy->rowCount(), 5);

    // The text filter matches name or URL; Invert flips it.
    panel.m_networkFilter->setText(QStringLiteral("site"));
    QCOMPARE(netProxy->rowCount(), 4); // cdn/pic.png falls out
    panel.m_invertFilter->setChecked(true);
    QCOMPARE(netProxy->rowCount(), 1);
    panel.m_invertFilter->setChecked(false);
    panel.m_networkFilter->clear();
    QCOMPARE(netProxy->rowCount(), 5);

    // Navigation clears the log unless Preserve log is ticked.
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx1\"}}"));
    QCOMPARE(net->rowCount(), 5); // ctx2 is selected — untouched
    panel.m_contextCombo->setCurrentIndex(0);
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx1\"}}"));
    QCOMPARE(net->rowCount(), 0);
    panel.m_preserveLog->setChecked(true);
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":2000.0,\"request\":{\"request\":\"k1\","
        "\"method\":\"GET\",\"url\":\"http://site/keep.html\","
        "\"type\":\"Document\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx1\"}}"));
    QCOMPARE(net->rowCount(), 1);
    panel.m_preserveLog->setChecked(false);

    // The Clear button wipes rows regardless.
    panel.m_clearRequests->click();
    QCOMPARE(net->rowCount(), 0);

    // Sorting runs through the proxy's raw values — numbers order as
    // numbers, not as display text.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":100.0,\"request\":{\"request\":\"s1\","
        "\"method\":\"GET\",\"url\":\"http://a/big\",\"type\":\"Image\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":200.0,\"request\":{\"request\":\"s1\"},"
        "\"response\":{\"encodedDataLength\":4096}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":110.0,\"request\":{\"request\":\"s2\","
        "\"method\":\"GET\",\"url\":\"http://a/small\",\"type\":\"Image\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":150.0,\"request\":{\"request\":\"s2\"},"
        "\"response\":{\"encodedDataLength\":512}}}"));
    netProxy->sort(4, Qt::AscendingOrder);
    QCOMPARE(netProxy->data(netProxy->index(0, 4)).toString(),
             QStringLiteral("512 B"));
    QCOMPARE(netProxy->data(netProxy->index(1, 4)).toString(),
             QStringLiteral("4.0 kB"));

    // DEVT07: cache provenance stays honest — memory/disk hits show
    // Chrome's "(...cache)" label instead of a bogus byte count, and
    // the TTFB split rides along for the two-tone waterfall bar.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":300.0,\"request\":{\"request\":\"c1\","
        "\"method\":\"GET\",\"url\":\"http://a/style.css\","
        "\"type\":\"Stylesheet\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.requestServedFromCache\",\"params\":{"
        "\"request\":{\"request\":\"c1\"}}}"));
    QCOMPARE(net->data(net->index(2, 4)).toString(),
             QStringLiteral("(memory cache)"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":310.0,\"request\":{\"request\":\"c2\","
        "\"method\":\"GET\",\"url\":\"http://a/app.js\","
        "\"type\":\"Script\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":315.0,\"type\":\"Script\",\"request\":{"
        "\"request\":\"c2\"},\"response\":{\"status\":200,"
        "\"fromDiskCache\":true,"
        "\"timing\":{\"receiveHeadersEnd\":2.0}}}}"));
    QCOMPARE(net->data(net->index(3, 4)).toString(),
             QStringLiteral("(disk cache)"));
    // TtfbRole = Qt::UserRole + 9 (mirrors bidipanel.cpp's enum).
    QCOMPARE(net->data(net->index(3, 0), Qt::UserRole + 9).toDouble(),
             2.0);

    // DEVT07: page-load milestones feed the footer relative to the
    // recording window's start (s1 @100 — the earliest start).
    // Milestones for another context are ignored.
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.domContentLoaded\",\"params\":{"
        "\"context\":\"elsewhere\",\"timestamp\":330.0}}"));
    QVERIFY(!panel.m_networkSummary->text().contains(
        QStringLiteral("DOMContentLoaded")));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.domContentLoaded\",\"params\":{"
        "\"context\":\"ctx1\",\"timestamp\":340.0}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.load\",\"params\":{"
        "\"context\":\"ctx1\",\"timestamp\":350.0}}"));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("4 requests")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("DOMContentLoaded: 240 ms")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("Load: 250 ms")));

    // The offscreen grab renders the red failed row — smoke for the
    // Chrome look (palette-safe red pixels present in the view).
    panel.clearNetworkLog();
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":100.0,\"request\":{\"request\":\"f0\","
        "\"method\":\"GET\",\"url\":\"http://a/page.html\","
        "\"type\":\"Document\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":180.0,\"request\":{\"request\":\"f0\"},"
        "\"response\":{\"encodedDataLength\":18432}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":140.0,\"request\":{\"request\":\"f1\","
        "\"method\":\"GET\",\"url\":\"http://a/fail.js\","
        "\"type\":\"Script\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.fetchError\",\"params\":{\"request\":"
        "{\"request\":\"f1\"},\"errorText\":\"net::ERR_X\","
        "\"timestamp\":150.0}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":120.0,\"request\":{\"request\":\"f2\","
        "\"method\":\"GET\",\"url\":\"http://a/logo.png\","
        "\"type\":\"Image\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":160.0,\"type\":\"Image\",\"request\":{"
        "\"request\":\"f2\"},\"response\":{\"status\":200,"
        "\"timing\":{\"receiveHeadersEnd\":25.0}}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":210.0,\"request\":{\"request\":\"f2\"},"
        "\"response\":{\"encodedDataLength\":5120}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":130.0,\"request\":{\"request\":\"f3\","
        "\"method\":\"GET\",\"url\":\"http://a/fonts.css\","
        "\"type\":\"Stylesheet\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.requestServedFromCache\",\"params\":{"
        "\"request\":{\"request\":\"f3\"}}}"));
    panel.resize(700, 300);
    panel.show();
    if (QTabWidget *tabs = panel.findChild<QTabWidget *>())
        tabs->setCurrentWidget(panel.m_networkView->parentWidget());
    QCoreApplication::processEvents();
    // Whole-panel grab exercises the waterfall delegate's paint path.
    panel.grab();
    const QPixmap shot = panel.m_networkView->grab();
    bool sawRed = false;
    const QImage img = shot.toImage();
    for (int y = 0; y < img.height() && !sawRed; ++y) {
        for (int x = 0; x < img.width(); ++x) {
            const QColor c = img.pixelColor(x, y);
            if (c.red() > 150 && c.green() < 110 && c.blue() < 110) {
                sawRed = true;
                break;
            }
        }
    }
    QVERIFY(sawRed);

    // DEVT07: column floors — squeezing the dock pins every column at
    // its minimum instead of losing Size/Time/Waterfall, and the
    // overflow becomes a horizontal scrollbar.
    QHeaderView *netHead = panel.m_networkView->horizontalHeader();
    QVERIFY(netHead->sectionSize(6) >= 130); // Waterfall at 700px
    QVERIFY(netHead->sectionSize(4) >= 64);  // Size
    QVERIFY(netHead->sectionSize(5) >= 56);  // Time
    panel.resize(430, 300);
    QCoreApplication::processEvents();
    QTRY_VERIFY(netHead->sectionSize(0) >= 90);  // Name floor
    QVERIFY(netHead->sectionSize(6) >= 130);
    QVERIFY(netHead->sectionSize(4) >= 64);
    QVERIFY(netHead->sectionSize(1) >= 48);
    QTRY_VERIFY(panel.m_networkView->horizontalScrollBar()
                    ->maximum() > 0);
    panel.resize(700, 300);
    QCoreApplication::processEvents();

    // Unknown request ids and unknown network methods are ignored.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{\"request\":"
        "{\"request\":\"ghost\"},\"response\":{\"status\":404}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.dataReceived\",\"params\":{\"request\":"
        "{\"request\":\"f1\"}}}"));

    // The 1000-row cap trims and rebases the request-id index.
    for (int i = 0; i < 1005; ++i) {
        panel.onEvent(QByteArray(
            "{\"method\":\"network.beforeRequestSent\",\"params\":{\"request\":"
            "{\"request\":\"cap") + QByteArray::number(i)
            + QByteArray("\",\"method\":\"GET\",\"url\":\"http://c/\"}}}"));
    }
    QVERIFY(net->rowCount() <= 1000);
}

QTEST_MAIN(tst_DevTools)
#include "tst_devtools.moc"
