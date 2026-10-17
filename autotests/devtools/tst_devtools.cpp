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
#include <qmenu.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineview.h>

#include "browsermainwindow.h"
#include "browserprofile.h"
#include "devtoolswindow.h"
#include "webpage.h"
#include "webview.h"
#include "qtest_arora.h"
#include "qtry.h"

#include "arorastyle.h"
#include "bidiclient.h"
#include "bidipanel.h"
#include "engineregistry.h"
#include "enginetab.h"
#include "fakeengine.h"
#include "tabwidget.h"

#include <qabstractitemmodel.h>
#include <qabstractbutton.h>
#include <qbuttongroup.h>
#include <qcheckbox.h>
#include <qcombobox.h>
#include <qheaderview.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qlineedit.h>
#include <qlistwidget.h>
#include <qplaintextedit.h>
#include <qscrollbar.h>
#include <qsortfilterproxymodel.h>
#include <qstackedwidget.h>
#include <qstandarditemmodel.h>
#include <qstyleoption.h>
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
    void bidiElementsRefresh();
    void bidiElementsLiveTree();
    void bidiElementsParity();
    void bidiElementsUx();
    void bidiContextSelector();
    void bidiContextRematch();
    void bidiConsoleParity();
    void bidiWaterfallLegibility();
    void bidiNetworkParity();
    void bidiSourcesParity();
    void bidiSourcesUx();
    void bidiFullTabSet();
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

// DEVT12: the console is a structured tree now — flatten its
// top-level column-0 texts so assertions can stay one-liners.
static QString consoleJoined(const BidiPanel &panel)
{
    QStringList lines;
    for (int i = 0; i < panel.m_console->topLevelItemCount(); ++i)
        lines << panel.m_console->topLevelItem(i)->text(0);
    return lines.join(QLatin1Char('\n'));
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
    QVERIFY(consoleJoined(panel).contains(QStringLiteral("> 1+1")));

    quint64 id = panel.testRegisterTag(1); // console, plain value
    panel.onResponse(id, ok);
    QVERIFY(consoleJoined(panel).contains(QStringLiteral("< [1,2]")));
    id = panel.testRegisterTag(1); // console, undefined collapses
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":"
        "{\"type\":\"undefined\"}}}"));
    QVERIFY(consoleJoined(panel).contains(
        QStringLiteral("< undefined")));
    id = panel.testRegisterTag(1); // console, exceptionDetails text
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"exception\",\"exceptionDetails\":"
        "{\"text\":\"ReferenceError: nope\"}}}"));
    QVERIFY(consoleJoined(panel).contains(
        QStringLiteral("< ReferenceError: nope")));
    id = panel.testRegisterTag(1); // console, top-level error envelope
    panel.onResponse(id, QByteArray(
        "{\"error\":{\"message\":\"denied\"}}"));
    QVERIFY(consoleJoined(panel).contains(
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
    // DEVT22: legacy {label} payloads still paint — the markup is
    // derived from the compact label.
    QCOMPARE(root->child(0)->text(0), QStringLiteral("<div>"));
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
    // DEVT12: the level is the row's wash/icon, not a text prefix.
    QVERIFY(consoleJoined(panel).contains(QStringLiteral("hello")));
    {
        QTreeWidgetItem *row = panel.m_console->topLevelItem(
            panel.m_console->topLevelItemCount() - 1);
        QCOMPARE(row->data(0, Qt::UserRole).toInt(), 2); // Warning
        QVERIFY(!row->icon(0).isNull());
        QVERIFY(row->background(0).color().isValid());
    }
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
                            "· 2.0 kB resources · Finish: 150 ms"));

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
                            "· 2.0 kB resources · Finish: 42 ms"));
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

    // DEVT07: Chrome's status line — the f-rows above already fed it
    // (4 rows, 23.0 kB wire, 100→210 ms recording window).
    QVERIFY(panel.m_networkSummary);
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("4 requests")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("23.0 kB transferred")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("Finish: 110 ms")));

    // The response's own encodedDataLength is the interim Size while
    // a body still loads; the completion overrides it.
    panel.clearNetworkLog();
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("0 requests")));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1000.0,\"request\":{\"request\":\"n1\","
        "\"method\":\"GET\",\"url\":\"http://s/page\","
        "\"type\":\"Document\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":1100.0,\"request\":{\"request\":\"n1\"},"
        "\"response\":{\"status\":200,\"encodedDataLength\":64}}}"));
    QCOMPARE(net->data(net->index(0, 4)).toString(),
             QStringLiteral("64 B"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":1500.0,\"request\":{\"request\":\"n1\"},"
        "\"response\":{\"encodedDataLength\":2048,"
        "\"decodedDataLength\":6000}}}"));
    QCOMPARE(net->data(net->index(0, 4)).toString(),
             QStringLiteral("2.0 kB"));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("1 request")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("2.0 kB transferred")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("5.9 kB resources")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("Finish: 500 ms")));

    // A disk-cached row marks honestly instead of showing "0 B".
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":1600.0,\"request\":{\"request\":\"n2\","
        "\"method\":\"GET\",\"url\":\"http://s/app.js\","
        "\"type\":\"Script\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":1610.0,\"request\":{\"request\":\"n2\"},"
        "\"response\":{\"status\":200,\"fromDiskCache\":true}}}"));
    QCOMPARE(net->data(net->index(1, 4)).toString(),
             QStringLiteral("(disk cache)"));

    // Page-load timings land on the footer for the current context
    // (window start is n1's 1000 ms) and are ignored for others.
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.domContentLoaded\",\"params\":"
        "{\"context\":\"ctx1\",\"timestamp\":1400.0}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.load\",\"params\":"
        "{\"context\":\"ctx1\",\"timestamp\":1800.0}}"));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("DOMContentLoaded: 400 ms")));
    QVERIFY(panel.m_networkSummary->text().contains(
        QStringLiteral("Load: 800 ms")));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.load\",\"params\":"
        "{\"context\":\"elsewhere\",\"timestamp\":9999.0}}"));
    QVERIFY(!panel.m_networkSummary->text().contains(
        QStringLiteral("9999")));

    // Navigation of the current context resets the markers — with
    // Preserve log the rows stay but the times go.
    panel.m_preserveLog->setChecked(true);
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx1\"}}"));
    QCOMPARE(net->rowCount(), 2);
    QVERIFY(!panel.m_networkSummary->text().contains(
        QStringLiteral("DOMContentLoaded")));
    QVERIFY(!panel.m_networkSummary->text().contains(
        QStringLiteral("Load:")));
    panel.m_preserveLog->setChecked(false);

    // Column discipline: the waterfall keeps a real floor at every
    // width; extreme narrowness scrolls horizontally instead of
    // squeezing Size/Time/Waterfall to nothing.
    QHeaderView *nh = panel.m_networkView->horizontalHeader();
    QVERIFY(!nh->stretchLastSection());
    panel.resize(1200, 320);
    QCoreApplication::processEvents();
    QVERIFY(nh->sectionSize(6) >= 140);
    QVERIFY(nh->sectionSize(0) >= 160);
    panel.resize(360, 320);
    QCoreApplication::processEvents();
    QCOMPARE(nh->sectionSize(6), 130); // Waterfall floor
    QCOMPARE(nh->sectionSize(0), 90);  // Name floor
    QVERIFY(nh->sectionSize(4) > 0);
    QVERIFY(nh->sectionSize(5) > 0);
    QTRY_VERIFY(
        panel.m_networkView->horizontalScrollBar()->isVisible());

    // DEVT18: under floor-width the Initiator column yields before
    // the waterfall is pushed off the right edge — the old fit kept
    // every column at its preferred width, which is what hid the
    // graph at a real right-dock width.
    panel.resize(500, 320);
    QCoreApplication::processEvents();
    QVERIFY(nh->isSectionHidden(3)); // Initiator yielded
    const int fallX = nh->sectionViewportPosition(6);
    QVERIFY(fallX >= 0);
    QVERIFY(fallX + nh->sectionSize(6)
            <= panel.m_networkView->viewport()->width());
    panel.resize(760, 320);
    QCoreApplication::processEvents();
    QVERIFY(!nh->isSectionHidden(3)); // room returns it

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

// DEVT10: the context selector mirrors the window's real tab strip —
// tabs with no CDP target (engine tabs, widget tabs) appear disabled
// with the reason instead of being silently absent, and a strip whose
// first slot isn't debuggable never strands the selection on it.
// DEVT17: the empty-Elements report — four holes in the live path
// the canned-JSON legs missed.  (a) Untagged housekeeping answers
// (session.new/subscribe, network.setCacheBehavior) took the
// tag==0/getTree route and wiped the context list + tree because
// their result carries no "contexts".  (b) The only re-fetch
// trigger was navigationStarted, whose evals race the commit and
// land in the torn-down or not-yet-parsed document — the completed
// page never re-fetched.  (c) A rebuild that silently moved the
// effective selection ran no context-switch cascade, so the tree
// stayed empty/stale while the combo pointed at a live context.
// (d) Refused/exception/vanished DOM answers were dropped on the
// floor, hanging branches on "loading…" forever.
void tst_DevTools::bidiElementsRefresh()
{
    const QByteArray domAnswer = QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"div\\\",\\\"children\\\":0}]\"}}}");

    BidiPanel panel;
    panel.onConnectionChanged(BidiClient::Connected);

    // getTree: two contexts, combo claims by order (no strip bound —
    // pending contexts append as enabled rows).
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"},"
        "{\"context\":\"ctx2\",\"url\":\"http://b/\"}]}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));
    QCOMPARE(panel.m_domTree->topLevelItemCount(), 1);
    QTreeWidgetItem *root = panel.m_domTree->topLevelItem(0);

    // (a) Housekeeping answers shaped like session.new /
    // session.subscribe / network.setCacheBehavior must leave the
    // selector and the Elements tree alone.
    panel.onResponse(8001, QByteArray(
        "{\"result\":{\"sessionId\":\"arora-bidi-1\","
        "\"capabilities\":{}}}"));
    panel.onResponse(8002, QByteArray("{\"result\":{}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));
    QCOMPARE(panel.m_contextCombo->count(), 2);
    QCOMPARE(panel.m_domTree->topLevelItemCount(), 1);
    QCOMPARE(panel.m_domTree->topLevelItem(0), root);

    // Materialize real children under the root so a rebuild is
    // observable (the placeholder is the only no-path child).
    quint64 id = panel.testDomExpand(root);
    panel.onResponse(id, domAnswer);
    QCOMPARE(root->childCount(), 1);
    QVERIFY(!root->child(0)->data(0, Qt::UserRole).isNull());

    // (b) domContentLoaded on the current context rebuilds the tree
    // — the live path's missing re-fetch: back to a placeholder.
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.domContentLoaded\",\"params\":{"
        "\"context\":\"ctx1\",\"timestamp\":100.0}}"));
    root = panel.m_domTree->topLevelItem(0);
    QVERIFY(root);
    QCOMPARE(root->childCount(), 1);
    QVERIFY(root->child(0)->data(0, Qt::UserRole).isNull());

    // …and once materialized again, a trailing load event leaves the
    // good tree alone (the stale-only backstop, not a rebuild).
    id = panel.testDomExpand(root);
    panel.onResponse(id, domAnswer);
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.load\",\"params\":{"
        "\"context\":\"ctx1\",\"timestamp\":200.0}}"));
    root = panel.m_domTree->topLevelItem(0);
    QVERIFY(!root->child(0)->data(0, Qt::UserRole).isNull());

    // …but a load arriving while the tree is still a placeholder
    // does re-fetch (covers a domContentLoaded that raced early).
    panel.m_domTree->clear();
    root = new QTreeWidgetItem(QStringList(QStringLiteral("document")));
    root->setData(0, Qt::UserRole, QString());
    root->addChild(new QTreeWidgetItem(QStringList(QStringLiteral("x"))));
    panel.m_domTree->addTopLevelItem(root);
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.load\",\"params\":{"
        "\"context\":\"ctx1\",\"timestamp\":300.0}}"));
    QCOMPARE(panel.m_domTree->topLevelItemCount(), 1);
    root = panel.m_domTree->topLevelItem(0);
    QVERIFY(root->child(0)->data(0, Qt::UserRole).isNull()
            || root->child(0)->text(0) != QStringLiteral("x"));

    // A milestone for another context never touches the tree.
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.domContentLoaded\",\"params\":{"
        "\"context\":\"ctx2\",\"timestamp\":400.0}}"));
    QCOMPARE(panel.m_domTree->topLevelItem(0), root);

    // (d) A refused children eval turns into a verdict row, not a
    // permanent "loading…"; the row keeps a null path so expanding
    // retries the fetch.
    id = panel.testDomExpand(root);
    panel.onResponse(id, QByteArray(
        "{\"error\":{\"message\":\"attach refused\"}}"));
    QCOMPARE(root->childCount(), 1);
    QCOMPARE(root->child(0)->text(0),
             QStringLiteral("attach refused"));
    QVERIFY(root->child(0)->isDisabled());
    QVERIFY(root->child(0)->data(0, Qt::UserRole).isNull());

    // An exception answer (helper gone after a navigation) same.
    id = panel.testDomExpand(root);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"exception\",\"exceptionDetails\":{"
        "\"text\":\"TypeError: gone\"}}}"));
    QCOMPARE(root->child(0)->text(0),
             QStringLiteral("TypeError: gone"));

    // A 'null' payload (element/document gone) gets the honest
    // gone-verdict instead of a silently childless node.
    id = panel.testDomExpand(root);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"null\"}}}"));
    QCOMPARE(root->child(0)->text(0),
             QStringLiteral("element no longer in the document"));

    // (c) A rebuild that silently moves the effective selection runs
    // the switch cascade — the tree repopulates for the new context
    // instead of staying empty.  Selection is ctx1; drop it from the
    // cached contexts so the rebuild falls forward to ctx2.
    panel.m_contextCombo->setCurrentIndex(0); // ctx1
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));
    panel.m_contexts = QJsonArray{QJsonObject{
        {QStringLiteral("context"), QStringLiteral("ctx2")},
        {QStringLiteral("url"), QStringLiteral("http://b/")}}};
    panel.m_domTree->clear();
    panel.rebuildContextCombo();
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx2"));
    QCOMPARE(panel.m_domTree->topLevelItemCount(), 1);
}

// DEVT17: the Elements tab against the REAL debug channel — the
// fixture legs drive onResponse with canned JSON, which proved the
// handlers but not the live connect -> subscribe -> getTree -> eval
// chain.  This leg needs the armed loopback endpoint; it skips when
// the backend is disabled (devtools/bidiBackend=0, no rustcore).
void tst_DevTools::bidiElementsLiveTree()
{
    if (BrowserProfile::bidiDebugPort() == 0)
        QSKIP("debug channel not armed in this build/config");

    TabWidget strip;
    WebView *view = strip.makeNewTab(true);
    QVERIFY(view);
    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->setHtml(QStringLiteral("<html><head><title>elements</title>"
                                 "</head><body><h1 id=\"t\">elem</h1>"
                                 "<p class=\"x\">para</p></body></html>"),
                  QUrl(QStringLiteral("http://elements.test/")));
    QTRY_VERIFY(loaded.count() >= 1);

    BidiPanel panel;
    panel.setTabWidget(&strip);
    panel.show(); // showEvent -> connectToEngine
    QTRY_VERIFY_WITH_TIMEOUT(
        panel.m_client->state() == BidiClient::Connected, 15000);
    // getTree -> rebuild: the tab claims its context.
    QTRY_VERIFY_WITH_TIMEOUT(
        !panel.currentContext().isEmpty(), 15000);

    // The tree root materializes, then its lazy children fetch fills
    // real element rows (the placeholder carries no index path).
    QTRY_VERIFY_WITH_TIMEOUT(
        panel.m_domTree->topLevelItemCount() >= 1, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(
        panel.m_domTree->topLevelItem(0)->childCount() >= 1
            && !panel.m_domTree->topLevelItem(0)->child(0)
                    ->data(0, Qt::UserRole).isNull(),
        15000);
    // document's element children are the real tags — head/body —
    // never the "loading…" placeholder.  DEVT22: rows carry the
    // <tag …> markup lines now.
    QStringList labels;
    QTreeWidgetItem *rootItem = panel.m_domTree->topLevelItem(0);
    for (int i = 0; i < rootItem->childCount(); ++i)
        labels << rootItem->child(i)->text(0);
    bool realTag = false;
    for (const QString &l : labels) {
        if (l.startsWith(QLatin1String("<head"))
            || l.startsWith(QLatin1String("<body")))
            realTag = true;
    }
    QVERIFY(realTag);
}

// DEVT19 — Elements parity: the page-side MutationObserver marker
// triggers a debounced live re-fetch that keeps expanded branches,
// the find bar's result paths drive selection, and the styles pane
// groups Attributes / Computed styles / Box model from real CDP
// answers with DOM.setAttributeValue-backed editing and the
// honest page-side fallbacks.  CdpKind / tag numbers mirror
// bidipanel.cpp (file-local there).
void tst_DevTools::bidiElementsParity()
{
    const QByteArray domAnswer = QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"div\\\",\\\"children\\\":1}"
        ",{\\\"label\\\":\\\"span\\\",\\\"children\\\":0}]\"}}}");

    BidiPanel panel;
    panel.onConnectionChanged(BidiClient::Connected);
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"}]}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));
    QTreeWidgetItem *root = panel.m_domTree->topLevelItem(0);
    QVERIFY(root);

    // Materialize: [div(children:1), span] — div gets its own child.
    quint64 id = panel.testDomExpand(root);
    panel.onResponse(id, domAnswer);
    QCOMPARE(root->childCount(), 2);
    QCOMPARE(root->child(0)->data(0, Qt::UserRole).toString(),
             QStringLiteral("0"));
    QCOMPARE(root->child(1)->data(0, Qt::UserRole).toString(),
             QStringLiteral("1"));
    id = panel.testDomExpand(root->child(0));
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"em\\\",\\\"children\\\":0}]\"}}}"));
    QCOMPARE(root->child(0)->child(0)->data(0, Qt::UserRole).toString(),
             QStringLiteral("0/0"));

    // --- live tree: the mutation marker is control traffic — it
    // never reaches the console, and after the debounce a live
    // refresh re-pulls the tree.  Expanded branches are snapshotted
    // by label and re-opened by the refill.
    const int consoleLines = panel.m_console->topLevelItemCount();
    root->child(0)->setExpanded(true);
    panel.onEvent(QByteArray(
        "{\"method\":\"log.entryAdded\",\"params\":{\"level\":\"log\""
        ",\"text\":\"__arora_dom_mutated\"}}"));
    QCOMPARE(panel.m_console->topLevelItemCount(), consoleLines);
    QTRY_VERIFY_WITH_TIMEOUT(panel.testDomLiveRefreshCount() >= 1,
                             3000);
    // The refresh's own fetch answers with a grown tree — the new
    // sibling lands and the previously-expanded div re-opens.
    id = panel.testDomExpand(root);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"div\\\",\\\"children\\\":1}"
        ",{\\\"label\\\":\\\"span\\\",\\\"children\\\":0}"
        ",{\\\"label\\\":\\\"p\\\",\\\"children\\\":0}]\"}}}"));
    QCOMPARE(root->childCount(), 3);
    QCOMPARE(root->child(2)->text(0), QStringLiteral("<p>"));
    QVERIFY(root->child(0)->isExpanded());
    // The refill replaced div's children with a placeholder —
    // re-materialize the em grandchild so "0/0" resolves again.
    id = panel.testDomExpand(root->child(0));
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[{\\\"label\\\":\\\"em\\\",\\\"children\\\":0}]\"}}}"));
    QCOMPARE(root->child(0)->child(0)->data(0, Qt::UserRole).toString(),
             QStringLiteral("0/0"));

    // --- find bar: the search reply's paths select through the
    // same descend-one-level walk the inspect pick uses.  (The
    // eval itself no-ops — the canned client never reaches
    // Connected — so the reply is fed through the tag seam.)
    panel.m_domFind->setText(QStringLiteral("em"));
    panel.runDomSearch();
    id = panel.testRegisterTag(10); // TagDomSearch
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[\\\"0/0\\\",\\\"1\\\"]\"}}}"));
    QCOMPARE(panel.testSearchPaths(),
             QStringList({QStringLiteral("0/0"),
                          QStringLiteral("1")}));
    QCOMPARE(panel.testSearchIndex(), 0);
    QCOMPARE(panel.m_domFindCount->text(), QStringLiteral("1 of 2"));
    // First hit selected through the lazy walk…
    QTRY_COMPARE(panel.m_domTree->currentItem(),
                 root->child(0)->child(0));
    // …and Enter/arrow cycles the ring.
    panel.domSearchStep(1);
    QCOMPARE(panel.testSearchIndex(), 1);
    QCOMPARE(panel.m_domFindCount->text(), QStringLiteral("2 of 2"));
    panel.domSearchStep(1); // wraps to 0
    QCOMPARE(panel.testSearchIndex(), 0);
    // An empty result reports honestly.
    id = panel.testRegisterTag(10);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[]\"}}}"));
    QCOMPARE(panel.testSearchIndex(), -1);
    QCOMPARE(panel.m_domFindCount->text(), QStringLiteral("0 of 0"));

    // --- styles pane: the CDP chain fills Attributes / Computed
    // styles / Box model as three groups.  Drive the verdicts:
    // CSS up so the CDP path runs.
    const auto probe = [&panel](const char *domain, bool up) {
        const quint64 pid = panel.testRegisterCdp(
            1 /*CdpProbe*/, QString::fromLatin1(domain));
        panel.onResponse(pid, up
            ? QByteArray("{\"result\":{}}")
            : QByteArray("{\"error\":{\"message\":\"down\"}}"));
    };
    probe("CSS", true);
    probe("DOM", true);
    QTreeWidgetItem *emItem = root->child(0)->child(0);
    // requestStylesFor would set these — it early-outs on the
    // disconnected canned client, so stage the selection state the
    // same way and feed the chain's replies through the seam.
    panel.m_propsPath = QStringLiteral("0/0");
    const int serial = ++panel.m_stylesSerial;
    QVERIFY(emItem);
    // Runtime.evaluate -> objectId; DOM.requestNode -> nodeId 42 —
    // which fans out to getAttributes/getComputedStyle/getBoxModel.
    id = panel.testRegisterCdp(4 /*CdpEvalNode*/, QString(), QString(),
                               serial);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"result\":{\"objectId\":\"obj-1\"}}}"));
    id = panel.testRegisterCdp(5 /*CdpRequestNode*/, QString(),
                               QString(), serial);
    panel.onResponse(id, QByteArray("{\"result\":{\"nodeId\":42}}"));
    QCOMPARE(panel.m_propsNodeId, 42);
    id = panel.testRegisterCdp(15 /*CdpAttributes*/, QString(),
                               QString(), serial);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"attributes\":[\"id\",\"main\",\"class\",\"x\"]}}"));
    id = panel.testRegisterCdp(6 /*CdpComputed*/, QString(), QString(),
                               serial);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"computedStyle\":["
        "{\"name\":\"color\",\"value\":\"rgb(9, 9, 9)\"},"
        "{\"name\":\"display\",\"value\":\"inline\"}]}}"));
    id = panel.testRegisterCdp(16 /*CdpBoxModel*/, QString(), QString(),
                               serial);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"model\":{"
        "\"content\":[0,0,100,0,100,50,0,50],"
        "\"padding\":[0,0,110,0,110,60,0,60],"
        "\"border\":[0,0,120,0,120,70,0,70],"
        "\"margin\":[0,0,140,0,140,90,0,90],"
        "\"width\":140,\"height\":90}}}"));

    // Grouped sections in Chrome's order.
    const auto section = [&panel](const QString &name) {
        for (int i = 0; i < panel.m_stylesView->topLevelItemCount();
             ++i) {
            QTreeWidgetItem *s = panel.m_stylesView->topLevelItem(i);
            if (s->data(0, Qt::UserRole + 4).toString() == name)
                return s;
        }
        return static_cast<QTreeWidgetItem *>(nullptr);
    };
    QTreeWidgetItem *attrs = section(QStringLiteral("Attributes"));
    QTreeWidgetItem *computed =
        section(QStringLiteral("Computed styles"));
    QTreeWidgetItem *box = section(QStringLiteral("Box model"));
    QVERIFY(attrs && computed && box);
    QCOMPARE(attrs->childCount(), 2);
    QCOMPARE(attrs->child(0)->text(0), QStringLiteral("id"));
    QCOMPARE(attrs->child(0)->text(1), QStringLiteral("main"));
    QVERIFY(attrs->child(0)->flags() & Qt::ItemIsEditable);
    QCOMPARE(computed->childCount(), 2);
    QCOMPARE(computed->child(0)->text(0), QStringLiteral("color"));
    QCOMPARE(computed->child(0)->text(1),
             QStringLiteral("rgb(9, 9, 9)"));
    // DEVT22: the quads drive a nested-boxes diagram (spanned row
    // under the section), not text rows.  The view's edge values
    // ride on dynamic properties.  Fixture: content 100x50,
    // padding/border rings +10 on right/bottom, margin +20.
    QWidget *boxView = panel.findChild<QWidget *>(
        QStringLiteral("bidiElementsBoxModel"));
    QVERIFY(boxView);
    QCOMPARE(boxView->property("state").toString(),
             QStringLiteral("model"));
    QCOMPARE(boxView->property("cw").toDouble(), 100.0);
    QCOMPARE(boxView->property("ch").toDouble(), 50.0);
    QCOMPARE(boxView->property("mr").toDouble(), 20.0);
    QCOMPARE(boxView->property("mb").toDouble(), 20.0);
    QCOMPARE(boxView->property("br").toDouble(), 10.0);
    QCOMPARE(boxView->property("pr").toDouble(), 10.0);
    QCOMPARE(boxView->property("mt").toDouble(), 0.0);

    // Attribute editing: the resolved node lands a
    // DOM.setAttributeValue whose error surfaces in the console,
    // and a successful answer re-pulls getAttributes.
    QTreeWidgetItem *row = attrs->child(1); // class="x"
    row->setText(1, QStringLiteral("y"));
    panel.editDomAttribute(row, 1); // sends CdpAttrSet on nodeId 42
    id = panel.testRegisterCdp(17 /*CdpAttrSet*/, QStringLiteral("class"),
                               QString(), serial);
    panel.onResponse(id, QByteArray("{\"result\":{}}"));
    id = panel.testRegisterCdp(17, QStringLiteral("class"), QString(),
                               serial);
    panel.onResponse(id, QByteArray(
        "{\"error\":{\"message\":\"node detached\"}}"));
    QVERIFY(consoleJoined(panel).contains(
        QStringLiteral("setAttributeValue failed: node detached")));

    // A stale serial (another selection raced it) drops the reply.
    id = panel.testRegisterCdp(15, QString(), QString(), serial - 1);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"attributes\":[\"stale\",\"yes\"]}}"));
    QCOMPARE(attrs->child(0)->text(1), QStringLiteral("main"));

    // --- fallbacks: CSS down routes through the page-side pair
    // readout (TagDomProps=8) into the same three groups, honestly
    // titled; a refused getComputedStyleForNode fills only the
    // Computed section from the pick list (TagDomStylesPick=11).
    probe("CSS", false);
    panel.requestStylesFor(emItem); // now sends the eval fallback
    id = panel.testRegisterTag(8);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[[\\\"id\\\",\\\"main\\\"],"
        "[\\\"computed: color\\\",\\\"rgb(1, 2, 3)\\\"]]\"}}}"));
    attrs = section(QStringLiteral("Attributes"));
    computed = section(QStringLiteral("Computed styles"));
    QVERIFY(attrs && computed);
    QCOMPARE(attrs->childCount(), 1);
    QCOMPARE(attrs->child(0)->text(0), QStringLiteral("id"));
    QCOMPARE(computed->child(0)->text(0), QStringLiteral("color"));
    QCOMPARE(computed->child(0)->text(1),
             QStringLiteral("rgb(1, 2, 3)"));
    // Fallback attrs stay editable — the write goes page-side.
    QVERIFY(attrs->child(0)->flags() & Qt::ItemIsEditable);
    id = panel.testRegisterTag(11);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"type\":\"success\",\"result\":{\"type\":\"string\""
        ",\"value\":\"[[\\\"display\\\",\\\"block\\\"]]\"}}}"));
    QCOMPARE(computed->child(0)->text(0), QStringLiteral("display"));

    // --- probe watchdog: a probe that never answers becomes an
    // honest verdict instead of stranding the tab defaults; a
    // disconnect drops the verdicts for the next session.
    probe("Runtime", true);
    panel.m_probedContext = QStringLiteral("ctx1");
    panel.probeWatchdog();
    QCOMPARE(panel.m_domainErr.size(), 9);
    // An answered verdict survives; the missing ones read "timed
    // out" rather than silently never-landing.
    QCOMPARE(panel.domainReason(QStringLiteral("CSS")),
             QStringLiteral("down"));
    QCOMPARE(panel.domainReason(QStringLiteral("Page")),
             QStringLiteral("probe timed out"));
    QVERIFY(!panel.domainUp(QStringLiteral("Page")));
    QVERIFY(panel.m_stylesTitle->text().contains(
        QStringLiteral("unavailable")));
    panel.onConnectionChanged(BidiClient::Disconnected);
    QVERIFY(panel.m_domainErr.isEmpty());
    QVERIFY(panel.m_probedContext.isEmpty());
}

// DEVT22 — the Elements tab's Chrome-UX shape: markup tree lines
// (<tag attr="v"> with the compact selector kept for crumbs and
// expansion matching), a compact bounded toolbar with the live
// element count, a spanned box-model diagram row, and the find
// dock's results list that selects hits in the tree.
void tst_DevTools::bidiElementsUx()
{
    BidiPanel panel;
    panel.onConnectionChanged(BidiClient::Connected);
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"}]}}"));
    QTreeWidgetItem *root = panel.m_domTree->topLevelItem(0);
    QVERIFY(root);

    // Build the eval envelope programmatically — the inner JSON is
    // the page helper's reply string (real payload shapes, no
    // triple-escaped literals).
    const auto reply = [](const QByteArray &inner) {
        return QJsonDocument(QJsonObject{
            {QStringLiteral("result"), QJsonObject{
                {QStringLiteral("type"), QStringLiteral("success")},
                {QStringLiteral("result"), QJsonObject{
                    {QStringLiteral("type"),
                     QStringLiteral("string")},
                    {QStringLiteral("value"),
                     QString::fromUtf8(inner)}}}}}
        }).toJson(QJsonDocument::Compact);
    };

    // The object reply's own pair rewrites the row to real markup
    // (this is how the root "document" line becomes <html …>);
    // children land as markup lines + compact crumb labels.
    quint64 id = panel.testDomExpand(root);
    const QByteArray treeReply = QJsonDocument(QJsonObject{
        {QStringLiteral("label"), QStringLiteral("html")},
        {QStringLiteral("markup"),
         QStringLiteral("<html lang=\"en\">")},
        {QStringLiteral("children"), QJsonArray{
            QJsonObject{
                {QStringLiteral("label"), QStringLiteral("body.main")},
                {QStringLiteral("markup"),
                 QStringLiteral("<body class=\"main\">")},
                {QStringLiteral("children"), 1}},
            QJsonObject{
                {QStringLiteral("label"), QStringLiteral("div#a.x.y")},
                {QStringLiteral("markup"),
                 QStringLiteral("<div id=\"a\" class=\"x y\" "
                                "data-note=\"z\">")},
                {QStringLiteral("children"), 0}}}}
    }).toJson(QJsonDocument::Compact);
    panel.onResponse(id, reply(treeReply));
    QCOMPARE(root->text(0), QStringLiteral("<html lang=\"en\">"));
    QCOMPARE(root->data(0, Qt::UserRole + 6).toString(),
             QStringLiteral("html"));
    QCOMPARE(root->childCount(), 2);
    QCOMPARE(root->child(0)->text(0),
             QStringLiteral("<body class=\"main\">"));
    QCOMPARE(root->child(0)->data(0, Qt::UserRole + 6).toString(),
             QStringLiteral("body.main"));
    // The attr text the user missed — a real <tag attr> line, not
    // the flat "div#a.x.y" selector string.
    QVERIFY(root->child(1)->text(0).contains(
        QStringLiteral("class=\"x y\"")));
    QVERIFY(root->child(1)->text(0).contains(
        QStringLiteral("data-note=\"z\"")));

    // (a) the toolbar is a compact bounded bar — Fixed vertical
    // policy, no floating buttons in dead space — and its right
    // edge reports the materialized element count (html + 2).
    QWidget *bar = panel.findChild<QWidget *>(
        QStringLiteral("bidiElementsToolbar"));
    QVERIFY(bar);
    QCOMPARE(bar->sizePolicy().verticalPolicy(), QSizePolicy::Fixed);
    QCOMPARE(panel.m_domStats->text(), QStringLiteral("3 elements"));

    // (b) the markup delegate is installed (rows paint colored
    // <tag attr> spans); the crumbs keep the compact selector form.
    QVERIFY(panel.m_domTree->itemDelegate());
    QVERIFY(panel.m_domTree->itemDelegate()
                ->property("aroraDomMarkup").toBool());
    panel.m_domTree->setCurrentItem(root->child(1));
    QStringList crumbTexts;
    QLayout *cl = panel.m_crumbBar->layout();
    for (int i = 0; i < cl->count(); ++i) {
        if (QAbstractButton *b = qobject_cast<QAbstractButton *>(
                cl->itemAt(i)->widget()))
            crumbTexts << b->text();
    }
    QCOMPARE(crumbTexts,
             QStringList({QStringLiteral("html"),
                          QStringLiteral("div#a.x.y")}));

    // The painted row really is multi-colored: render it through
    // the delegate and count the distinct quantized ink hues.
    {
        const QModelIndex midx = panel.m_domTree->indexFromItem(
            root->child(1), 0);
        QVERIFY(midx.isValid());
        QImage img(360, 24, QImage::Format_ARGB32);
        img.fill(Qt::white);
        QPainter p(&img);
        QStyleOptionViewItem opt;
        opt.initFrom(panel.m_domTree);
        opt.rect = img.rect();
        opt.font = panel.m_domTree->font();
        opt.features = QStyleOptionViewItem::HasDisplay;
        opt.text = midx.data(Qt::DisplayRole).toString();
        panel.m_domTree->itemDelegate()->paint(&p, opt, midx);
        p.end();
        QSet<quint32> hues;
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = img.pixel(x, y);
                const int dr = 255 - qRed(px), dg = 255 - qGreen(px),
                          db = 255 - qBlue(px);
                if (dr + dg + db > 90)
                    hues.insert((qRed(px) / 40 << 8)
                                | (qGreen(px) / 40 << 4)
                                | (qBlue(px) / 40));
            }
        }
        // accent tag + muted name + link value = 2+ ink families.
        QVERIFY(hues.size() >= 2);
    }

    // (c) Box model: a spanned diagram row under the section
    // header — per-edge numbers from the quads, honest error state.
    panel.fillBoxModelSection(QJsonObject{
        {QStringLiteral("content"),
         QJsonArray{0, 0, 40, 0, 40, 20, 0, 20}},
        {QStringLiteral("padding"),
         QJsonArray{0, 0, 50, 0, 50, 30, 0, 30}},
        {QStringLiteral("border"),
         QJsonArray{0, 0, 60, 0, 60, 40, 0, 40}},
        {QStringLiteral("margin"),
         QJsonArray{0, 0, 80, 0, 80, 60, 0, 60}},
        {QStringLiteral("width"), 80},
        {QStringLiteral("height"), 60}},
        QString());
    QWidget *boxView = panel.findChild<QWidget *>(
        QStringLiteral("bidiElementsBoxModel"));
    QVERIFY(boxView);
    QCOMPARE(boxView->property("state").toString(),
             QStringLiteral("model"));
    QCOMPARE(boxView->property("cw").toDouble(), 40.0);
    QCOMPARE(boxView->property("ch").toDouble(), 20.0);
    QCOMPARE(boxView->property("mr").toDouble(), 20.0);
    QCOMPARE(boxView->property("mb").toDouble(), 20.0);
    QCOMPARE(boxView->property("br").toDouble(), 10.0);
    QCOMPARE(boxView->property("pl").toDouble(), 0.0);
    panel.fillBoxModelSection(QJsonObject(), QStringLiteral("down"));
    QCOMPARE(boxView->property("state").toString(),
             QStringLiteral("error"));
    QCOMPARE(boxView->property("error").toString(),
             QStringLiteral("down"));

    // (d) the find dock lists every hit's markup; the clicked row
    // selects the node in the tree like Enter/arrow cycling.
    panel.m_domFind->setText(QStringLiteral("div"));
    id = panel.testRegisterTag(10); // TagDomSearch
    panel.onResponse(id, reply(QJsonDocument(QJsonArray{
        QJsonObject{{QStringLiteral("path"), QStringLiteral("1")},
                    {QStringLiteral("label"),
                     QStringLiteral("div#a.x.y")},
                    {QStringLiteral("markup"),
                     QStringLiteral("<div id=\"a\" "
                                    "class=\"x y\">")}},
        QJsonObject{{QStringLiteral("path"), QStringLiteral("0")},
                    {QStringLiteral("label"),
                     QStringLiteral("body.main")},
                    {QStringLiteral("markup"),
                     QStringLiteral("<body class=\"main\">")}}})
        .toJson(QJsonDocument::Compact)));
    QCOMPARE(panel.testSearchPaths(),
             QStringList({QStringLiteral("1"), QStringLiteral("0")}));
    QCOMPARE(panel.m_domFindCount->text(), QStringLiteral("1 of 2"));
    QVERIFY(!panel.m_domFindResults->isHidden());
    QCOMPARE(panel.m_domFindResults->count(), 2);
    QCOMPARE(panel.m_domFindResults->item(0)->text(),
             QStringLiteral("<div id=\"a\" class=\"x y\">"));
    QTRY_COMPARE(panel.m_domTree->currentItem(), root->child(1));
    // Clicking a listed hit jumps the ring to it.
    emit panel.m_domFindResults->itemClicked(
        panel.m_domFindResults->item(1));
    QCOMPARE(panel.testSearchIndex(), 1);
    QCOMPARE(panel.m_domFindCount->text(), QStringLiteral("2 of 2"));
    QTRY_COMPARE(panel.m_domTree->currentItem(), root->child(0));
    // …and cycling moves the list's current row with the ring.
    panel.domSearchStep(1); // wraps to 0
    QCOMPARE(panel.m_domFindResults->currentRow(), 0);
    // An empty result hides the list again.
    id = panel.testRegisterTag(10);
    panel.onResponse(id, reply(QByteArray("[]")));
    QCOMPARE(panel.m_domFindCount->text(), QStringLiteral("0 of 0"));
    QVERIFY(panel.m_domFindResults->isHidden());
}

void tst_DevTools::bidiContextSelector()
{
    const auto flagsOf = [](QComboBox *combo, int row) {
        QStandardItemModel *model =
            qobject_cast<QStandardItemModel *>(combo->model());
        QStandardItem *item = model ? model->item(row) : nullptr;
        return item ? item->flags() : Qt::ItemFlags();
    };
    const auto disabled = [&flagsOf](QComboBox *combo, int row) {
        return !(flagsOf(combo, row) & Qt::ItemIsEnabled);
    };

    // Strip order: [engine tab] [webengine tab] [widget tab] — the
    // user's report had a non-CDP tab in slot 0.
    FakeEngineBackend fake;
    TabWidget strip;
    strip.makeNewTab(true);
    EngineRegistry::registerBackend(&fake);
    QVERIFY(strip.reloadTabInEngine(0, QStringLiteral("fake")));
    QVERIFY(strip.engineTab(0));
    WebView *view = strip.makeNewTab(true);
    QVERIFY(view);
    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->setHtml(QStringLiteral("<html><head><title>web tab</title>"
                                 "</head><body>x</body></html>"),
                  QUrl(QStringLiteral("http://devtools.test/")));
    QTRY_VERIFY(loaded.count() >= 1);
    strip.addWidgetTab(new QLabel(QStringLiteral("prefs")),
                       QStringLiteral("Preferences"), QIcon(), false);
    QCOMPARE(strip.count(), 3);

    BidiPanel panel;
    panel.setTabWidget(&strip);
    // Before any getTree answer the strip rows already list — every
    // one disabled: engine tab, no-target web tab, widget tab.
    QCOMPARE(panel.m_contextCombo->count(), 3);
    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(disabled(panel.m_contextCombo, 1));
    QVERIFY(disabled(panel.m_contextCombo, 2));
    QVERIFY(panel.currentContext().isEmpty());

    // getTree: the web tab claims its context by committed url; a
    // foreign-window page stays appended+enabled as before.
    const QByteArray tree = QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx-web\",\"url\":\"")
        + view->url().toString().toUtf8()
        + QByteArray("\",\"title\":\"web tab\"},"
        "{\"context\":\"ctx-other\",\"url\":\"http://elsewhere/\","
        "\"title\":\"Elsewhere\"}]}}");
    panel.onResponse(4242, tree);
    QCOMPARE(panel.m_contextCombo->count(), 4);

    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(panel.m_contextCombo->itemData(0, Qt::ToolTipRole)
                .toString().contains(
                    QStringLiteral("no devtools backend")));
    QVERIFY(panel.m_contextCombo->itemData(0, Qt::ToolTipRole)
                .toString().contains(
                    QStringLiteral("Fake Engine")));
    QVERIFY(!disabled(panel.m_contextCombo, 1));
    QCOMPARE(panel.m_contextCombo->itemData(1).toString(),
             QStringLiteral("ctx-web"));
    QVERIFY(disabled(panel.m_contextCombo, 2));
    QVERIFY(panel.m_contextCombo->itemData(2, Qt::ToolTipRole)
                .toString().contains(QStringLiteral("Not a web page")));
    QVERIFY(!disabled(panel.m_contextCombo, 3));
    QCOMPARE(panel.m_contextCombo->itemData(3).toString(),
             QStringLiteral("ctx-other"));

    // Slot 0 is the engine tab — the selection must have landed on
    // the first ENABLED row, not index 0.
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx-web"));

    // Keep-restore still honors a live selection across rebuilds.
    panel.m_contextCombo->setCurrentIndex(3);
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx-other"));
    panel.onResponse(4243, tree);
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx-other"));

    // A vanished selection falls forward to the first enabled row.
    panel.onResponse(4244, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx-web\",\"url\":\"")
        + view->url().toString().toUtf8()
        + QByteArray("\",\"title\":\"web tab\"}]}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx-web"));

    // Strip churn with no CDP traffic still re-merges the selector
    // against the cached tree — a new widget tab appears instantly.
    // (4244's tree carried only ctx-web, so 4 strip rows + 0
    // leftovers = 4.)
    strip.addWidgetTab(new QLabel(QStringLiteral("dl")),
                       QStringLiteral("Downloads"), QIcon(), false);
    QCOMPARE(panel.m_contextCombo->count(), 4);
    QVERIFY(disabled(panel.m_contextCombo, 3));
    QVERIFY(panel.m_contextCombo->itemData(3, Qt::ToolTipRole)
                .toString().contains(QStringLiteral("Not a web page")));

    // Index-0 webengine tab: the first row is selectable and evals
    // route to its context.
    TabWidget plain;
    WebView *first = plain.makeNewTab(true);
    QVERIFY(first);
    QSignalSpy loaded2(first->page(), &QWebEnginePage::loadFinished);
    first->setHtml(QStringLiteral("<html><body>first</body></html>"),
                   QUrl(QStringLiteral("http://first.test/")));
    QTRY_VERIFY(loaded2.count() >= 1);
    panel.setTabWidget(&plain);
    panel.onResponse(4245, QByteArray(
        "{\"result\":{\"contexts\":[{\"context\":\"ctx-first\",\"url\":\"")
        + first->url().toString().toUtf8()
        + QByteArray("\",\"title\":\"first\"}]}}"));
    QCOMPARE(panel.m_contextCombo->count(), 1);
    QVERIFY(!disabled(panel.m_contextCombo, 0));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx-first"));
    panel.m_evalLine->setText(QStringLiteral("1+1"));
    emit panel.m_evalLine->returnPressed();
    QVERIFY(consoleJoined(panel).contains(QStringLiteral("> 1+1")));

    // All-disabled strip: the combo shows rows but no context.
    TabWidget widgetsOnly;
    widgetsOnly.addWidgetTab(new QLabel(QStringLiteral("p")),
                             QStringLiteral("Preferences"), QIcon(),
                             false);
    panel.setTabWidget(&widgetsOnly);
    panel.onResponse(4246, QByteArray(
        "{\"result\":{\"contexts\":[]}}"));
    QCOMPARE(panel.m_contextCombo->count(), 1);
    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(panel.currentContext().isEmpty());

    EngineRegistry::unregisterBackend(&fake);
}

// DEVT16: a web tab that outruns its CDP target must not pin "no
// debug target" — while the channel is live the row reads
// "attaching…" and a bounded getTree retry runs; a navigationStarted
// whose committed url reaches the tab re-merges it into a real row;
// typed-vs-committed url forms match through normalization; a lone
// chrome-error target pairs with the lone open tab; and the honest
// verdict returns once the retry budget is spent.
void tst_DevTools::bidiContextRematch()
{
    const auto flagsOf = [](QComboBox *combo, int row) {
        QStandardItemModel *model =
            qobject_cast<QStandardItemModel *>(combo->model());
        QStandardItem *item = model ? model->item(row) : nullptr;
        return item ? item->flags() : Qt::ItemFlags();
    };
    const auto disabled = [&flagsOf](QComboBox *combo, int row) {
        return !(flagsOf(combo, row) & Qt::ItemIsEnabled);
    };

    TabWidget strip;
    WebView *view = strip.makeNewTab(true);
    QVERIFY(view);
    QSignalSpy loaded(view->page(), &QWebEnginePage::loadFinished);
    view->setHtml(QStringLiteral("<html><body>rematch</body></html>"),
                  QUrl(QStringLiteral("http://rematch.test/")));
    QTRY_VERIFY(loaded.count() >= 1);

    BidiPanel panel;
    panel.setTabWidget(&strip);
    // Channel down: the old permanent verdict still applies.
    QCOMPARE(panel.m_contextCombo->count(), 1);
    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(panel.m_contextCombo->itemText(0).contains(
                QStringLiteral("no debug target")));

    // Channel live with no target in the snapshot: the row reads
    // "attaching…" and a deferred getTree retry is armed.
    panel.onConnectionChanged(BidiClient::Connected);
    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(panel.m_contextCombo->itemText(0).contains(
                QStringLiteral("attaching")));
    QTRY_VERIFY_WITH_TIMEOUT(panel.m_attachRetries >= 1, 3000);

    // The snapshot carried the target with its pre-commit url — the
    // tab's committed url reaches it via navigationStarted (the
    // bridge's Target.targetInfoChanged forward) and re-merges.
    panel.onResponse(9100, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx-r\",\"url\":\"about:blank\"}]}}"));
    QVERIFY(disabled(panel.m_contextCombo, 0));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":{"
        "\"context\":\"ctx-r\",\"url\":\"http://rematch.test/\"}}"));
    QVERIFY(!disabled(panel.m_contextCombo, 0));
    QCOMPARE(panel.m_contextCombo->itemData(0).toString(),
             QStringLiteral("ctx-r"));

    // Typed-vs-committed url forms match: the target reports the
    // slash-less typed spelling, the tab holds the normalized one.
    TabWidget strip2;
    WebView *view2 = strip2.makeNewTab(true);
    QVERIFY(view2);
    QSignalSpy loaded2(view2->page(), &QWebEnginePage::loadFinished);
    view2->setHtml(QStringLiteral("<html><body>slash</body></html>"),
                   QUrl(QStringLiteral("http://slash.test/")));
    QTRY_VERIFY(loaded2.count() >= 1);
    panel.setTabWidget(&strip2);
    // The target reports the slash-less typed spelling, the tab
    // holds the normalized one.
    QByteArray typed = view2->url().toString().toUtf8();
    if (typed.endsWith('/'))
        typed.chop(1);
    QVERIFY(typed != view2->url().toString().toUtf8());
    panel.onResponse(9101, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx-s\",\"url\":\"") + typed
        + QByteArray("\"}]}}"));
    QVERIFY(!disabled(panel.m_contextCombo, 0));
    QCOMPARE(panel.m_contextCombo->itemData(0).toString(),
             QStringLiteral("ctx-s"));

    // A failed navigation commits the target on a chrome-error url
    // the tab never shows — the claim is positional and safe only
    // when exactly one open tab and one error target remain.
    panel.onResponse(9102, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx-e\",\"url\":\"chrome-error://"
        "chromewebdata/\"}]}}"));
    QVERIFY(!disabled(panel.m_contextCombo, 0));
    QCOMPARE(panel.m_contextCombo->itemData(0).toString(),
             QStringLiteral("ctx-e"));

    // Two open tabs plus one error target is ambiguous — nobody
    // claims it; both stay attaching, and the target appends as a
    // plain unclaimed row like any foreign-window page.
    WebView *view3 = strip2.makeNewTab(true);
    QVERIFY(view3);
    QSignalSpy loaded3(view3->page(), &QWebEnginePage::loadFinished);
    view3->setHtml(QStringLiteral("<html><body>other</body></html>"),
                   QUrl(QStringLiteral("http://other.test/")));
    QTRY_VERIFY(loaded3.count() >= 1);
    QCOMPARE(panel.m_contextCombo->count(), 3);
    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(disabled(panel.m_contextCombo, 1));
    QVERIFY(!disabled(panel.m_contextCombo, 2));
    QCOMPARE(panel.m_contextCombo->itemData(2).toString(),
             QStringLiteral("ctx-e"));

    // Once the retry budget is spent the row settles back on the
    // honest verdict instead of promising an attach forever.
    panel.m_attachRetries = 100;
    panel.rebuildContextCombo();
    QVERIFY(disabled(panel.m_contextCombo, 0));
    QVERIFY(panel.m_contextCombo->itemText(0).contains(
                QStringLiteral("no debug target")));
}

// DEVT12: the Console's Chromium-parity pass — per-level washes and
// icons, the checkable "Default levels" dropdown + live text filter,
// the right-edge source link, Chrome's repeat-count badge on
// identical consecutive entries, expandable preview children, a
// console-scoped Preserve log, and Up/Down history on the REPL
// prompt.
void tst_DevTools::bidiConsoleParity()
{
    BidiPanel panel;
    panel.onConnectionChanged(BidiClient::Connected);
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"}]}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));

    // Toolbar controls exist — clear, filter, levels dropdown,
    // console-scoped Preserve log — and the REPL sits on a '>'
    // prompt row under the view.
    QVERIFY(panel.findChild<QWidget *>(
                QStringLiteral("bidiConsoleToolbar")));
    QVERIFY(panel.m_clearConsole);
    QVERIFY(panel.m_consoleFilter);
    QVERIFY(panel.m_consoleLevels);
    QVERIFY(panel.m_consolePreserve);
    QVERIFY(panel.findChild<QWidget *>(
                QStringLiteral("bidiConsolePrompt"))
            ->findChild<QLabel *>(
                QStringLiteral("bidiConsolePromptGlyph")));
    QCOMPARE(panel.m_console->columnCount(), 3);

    const auto feed = [&panel](const QString &level,
                               const QString &text,
                               const QByteArray &extra = QByteArray()) {
        panel.onEvent(QByteArray(
            "{\"method\":\"log.entryAdded\",\"params\":{\"level\":\"")
            + level.toUtf8() + QByteArray("\",\"text\":\"")
            + text.toUtf8() + QByteArray("\"") + extra
            + QByteArray("}}"));
    };

    // Levels — each class gets its own chrome: error row washes red
    // + critical icon, warning washes amber + warning icon, debug
    // dims to the disabled-text color, info stays plain.
    feed(QStringLiteral("error"), QStringLiteral("boom"));
    feed(QStringLiteral("warn"), QStringLiteral("careful"));
    feed(QStringLiteral("debug"), QStringLiteral("chatty"));
    feed(QStringLiteral("info"), QStringLiteral("note"));
    QCOMPARE(panel.m_console->topLevelItemCount(), 4);
    QTreeWidgetItem *err = panel.m_console->topLevelItem(0);
    QTreeWidgetItem *wrn = panel.m_console->topLevelItem(1);
    QTreeWidgetItem *dbg = panel.m_console->topLevelItem(2);
    QTreeWidgetItem *inf = panel.m_console->topLevelItem(3);
    QCOMPARE(err->data(0, Qt::UserRole).toInt(), 3);   // LevelError
    QCOMPARE(wrn->data(0, Qt::UserRole).toInt(), 2);   // LevelWarning
    QCOMPARE(dbg->data(0, Qt::UserRole).toInt(), 0);   // LevelVerbose
    QCOMPARE(inf->data(0, Qt::UserRole).toInt(), 1);   // LevelInfo
    QVERIFY(!err->icon(0).isNull());
    QVERIFY(!wrn->icon(0).isNull());
    QVERIFY(err->data(0, Qt::BackgroundRole).isValid());
    QVERIFY(wrn->data(0, Qt::BackgroundRole).isValid());
    QVERIFY(!inf->data(0, Qt::BackgroundRole).isValid());
    QVERIFY(dbg->data(0, Qt::ForegroundRole).isValid());
    // No textual "[level]" prefix survived the structure.
    QVERIFY(!err->text(0).startsWith(QLatin1String("[")));

    // "Default levels" hides Verbose out of the box (Chrome's
    // default set); the debug row is hidden, the rest visible.
    QCOMPARE(panel.m_consoleLevels->text(),
             QStringLiteral("Default levels"));
    QVERIFY(dbg->isHidden());
    QVERIFY(!err->isHidden());

    // The dropdown's checkable actions re-slice live: Errors off
    // hides the error row and renames the button like Chrome.
    QMenu *menu = panel.m_consoleLevels->menu();
    QAction *verbose = nullptr, *errors = nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text() == QLatin1String("Verbose"))
            verbose = action;
        if (action->text() == QLatin1String("Errors"))
            errors = action;
    }
    QVERIFY(verbose && errors);
    errors->setChecked(false);
    QVERIFY(err->isHidden());
    QCOMPARE(panel.m_consoleLevels->text(),
             QStringLiteral("Custom levels"));
    errors->setChecked(true);
    QVERIFY(!err->isHidden());
    verbose->setChecked(true);
    QVERIFY(!dbg->isHidden());
    QCOMPARE(panel.m_consoleLevels->text(),
             QStringLiteral("All levels"));
    verbose->setChecked(false);
    QCOMPARE(panel.m_consoleLevels->text(),
             QStringLiteral("Default levels"));

    // The text filter matches the message and the source column.
    feed(QStringLiteral("info"), QStringLiteral("needle entry"),
         QByteArray(",\"source\":{\"url\":\"http://a/app.js\","
                    "\"line\":6}"));
    QTreeWidgetItem *src = panel.m_console->topLevelItem(
        panel.m_console->topLevelItemCount() - 1);
    QCOMPARE(src->text(2), QStringLiteral("app.js:6"));
    QCOMPARE(src->toolTip(2),
             QStringLiteral("http://a/app.js:6"));
    panel.m_consoleFilter->setText(QStringLiteral("needle"));
    QVERIFY(!src->isHidden());
    QVERIFY(err->isHidden());
    panel.m_consoleFilter->setText(QStringLiteral("app.js"));
    QVERIFY(!src->isHidden()); // source column matches too
    panel.m_consoleFilter->clear();
    QVERIFY(!err->isHidden());

    // Repeat marker: three identical consecutive entries collapse
    // into one row with a count badge; a different entry in between
    // restarts the run.
    feed(QStringLiteral("error"), QStringLiteral("integrity"));
    feed(QStringLiteral("error"), QStringLiteral("integrity"));
    feed(QStringLiteral("error"), QStringLiteral("integrity"));
    QTreeWidgetItem *spam = panel.m_console->topLevelItem(
        panel.m_console->topLevelItemCount() - 1);
    QCOMPARE(spam->text(0), QStringLiteral("integrity"));
    QCOMPARE(spam->text(1), QStringLiteral("3"));
    feed(QStringLiteral("error"), QStringLiteral("other"));
    feed(QStringLiteral("error"), QStringLiteral("integrity"));
    QTreeWidgetItem *tail = panel.m_console->topLevelItem(
        panel.m_console->topLevelItemCount() - 1);
    QCOMPARE(tail->text(0), QStringLiteral("integrity"));
    QCOMPARE(tail->data(0, Qt::UserRole + 2).toInt(), 1);
    QVERIFY(tail->text(1).isEmpty());

    // Expandable previews: args carrying a CDP preview grow
    // "name: value" children (nested previews recurse; overflow gets
    // the "…" row); args without a preview stay flat.
    panel.onEvent(QByteArray(
        "{\"method\":\"log.entryAdded\",\"params\":{\"level\":\"log\""
        ",\"text\":\"Array(2)\",\"args\":[{\"type\":\"object\","
        "\"description\":\"Array(2)\",\"preview\":{\"type\":\"object\""
        ",\"subtype\":\"array\",\"overflow\":true,\"properties\":["
        "{\"name\":\"0\",\"type\":\"number\",\"value\":\"7\"},"
        "{\"name\":\"meta\",\"type\":\"object\",\"valuePreview\":{"
        "\"description\":\"Object\",\"properties\":["
        "{\"name\":\"id\",\"type\":\"number\",\"value\":\"9\"}]}}]}"
        "}]}}"));
    QTreeWidgetItem *arr = panel.m_console->topLevelItem(
        panel.m_console->topLevelItemCount() - 1);
    QVERIFY(arr->childCount() >= 3);
    QCOMPARE(arr->child(0)->text(0), QStringLiteral("0: 7"));
    QCOMPARE(arr->child(1)->text(0), QStringLiteral("meta: Object"));
    QCOMPARE(arr->child(1)->child(0)->text(0),
             QStringLiteral("id: 9"));
    QCOMPARE(arr->child(arr->childCount() - 1)->text(0),
             QStringLiteral("…"));
    feed(QStringLiteral("log"), QStringLiteral("flat"));
    QTreeWidgetItem *flat = panel.m_console->topLevelItem(
        panel.m_console->topLevelItemCount() - 1);
    QCOMPARE(flat->childCount(), 0);

    // Context scoping: entries for another page are dropped; a
    // contextless entry still lands.
    const int before = panel.m_console->topLevelItemCount();
    feed(QStringLiteral("info"), QStringLiteral("foreign"),
         QByteArray(",\"context\":\"other-ctx\""));
    QCOMPARE(panel.m_console->topLevelItemCount(), before);
    feed(QStringLiteral("info"), QStringLiteral("ours"),
         QByteArray(",\"context\":\"ctx1\""));
    QCOMPARE(panel.m_console->topLevelItemCount(), before + 1);

    // Navigation clears the console unless its own Preserve log is
    // ticked — independent of the Network tab's checkbox.
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx1\"}}"));
    QCOMPARE(panel.m_console->topLevelItemCount(), 0);
    panel.m_consolePreserve->setChecked(true);
    feed(QStringLiteral("info"), QStringLiteral("kept"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.navigationStarted\",\"params\":"
        "{\"context\":\"ctx1\"}}"));
    QCOMPARE(panel.m_console->topLevelItemCount(), 1);
    panel.m_consolePreserve->setChecked(false);

    // The REPL prompt recalls history on Up/Down — first Up stashes
    // the draft, Down past the newest restores it.
    panel.m_evalLine->setText(QStringLiteral("1+1"));
    emit panel.m_evalLine->returnPressed();
    panel.m_evalLine->setText(QStringLiteral("2+2"));
    emit panel.m_evalLine->returnPressed();
    panel.m_evalLine->setText(QStringLiteral("draft"));
    QKeyEvent up(QEvent::KeyPress, Qt::Key_Up, Qt::NoModifier);
    QCoreApplication::sendEvent(panel.m_evalLine, &up);
    QCOMPARE(panel.m_evalLine->text(), QStringLiteral("2+2"));
    QCoreApplication::sendEvent(panel.m_evalLine, &up);
    QCOMPARE(panel.m_evalLine->text(), QStringLiteral("1+1"));
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Down, Qt::NoModifier);
    QCoreApplication::sendEvent(panel.m_evalLine, &down);
    QCOMPARE(panel.m_evalLine->text(), QStringLiteral("2+2"));
    QCoreApplication::sendEvent(panel.m_evalLine, &down);
    QCOMPARE(panel.m_evalLine->text(), QStringLiteral("draft"));

    // The Clear button wipes the console regardless of preserve.
    feed(QStringLiteral("info"), QStringLiteral("wipe"));
    panel.m_clearConsole->click();
    QCOMPARE(panel.m_console->topLevelItemCount(), 0);
}

// DEVT09 helpers — pixel-level checks on the painted waterfall.
static bool colorClose(const QColor &a, const QColor &b, int tol = 10)
{
    return qAbs(a.red() - b.red()) <= tol
        && qAbs(a.green() - b.green()) <= tol
        && qAbs(a.blue() - b.blue()) <= tol;
}

// bidipanel.cpp's failColor() — palette-lightness dependent.
static QColor waterfallFailColor()
{
    return qApp->palette().color(QPalette::Base).lightness() < 128
        ? QColor(0xff, 0x8a, 0x80) : QColor(0xc6, 0x28, 0x28);
}

// The delegate paints per-type swatches (DEVT11) with a darker(140)
// edge; failed rows use the same shape in the fail color.  Anything
// close to a swatch, its edge, or the fail pair counts as bar ink.
static bool isBarPixel(const QColor &c)
{
    static const QStringList types = {
        QStringLiteral("document"), QStringLiteral("stylesheet"),
        QStringLiteral("script"), QStringLiteral("font"),
        QStringLiteral("image"), QStringLiteral("media"),
        QStringLiteral("manifest"), QStringLiteral("websocket"),
        QStringLiteral("wasm"), QStringLiteral("xhr"),
        QStringLiteral("other")
    };
    for (const QString &type : types) {
        const QColor swatch =
            AroraStyle::networkTypeSwatch(type, qApp->palette());
        if (colorClose(c, swatch) || colorClose(c, swatch.darker(140)))
            return true;
    }
    const QColor fail = waterfallFailColor();
    return colorClose(c, fail) || colorClose(c, fail.darker(140));
}

// visualRect is in viewport coordinates; a widget grab() paints the
// whole widget, so cells land viewport()->pos() into the image.
static QRect cellInGrab(QTableView *view,
                        QSortFilterProxyModel *proxy, int proxyRow)
{
    return view->visualRect(proxy->index(proxyRow, 6))
        .adjusted(0, 0, -1, -1)
        .translated(view->viewport()->pos());
}

// Leftmost/rightmost bar-ink pixel on the proxy row's waterfall cell
// center line; {-1,-1} when the row paints no bar.
static QPair<int, int> barRunAt(QTableView *view,
                                QSortFilterProxyModel *proxy,
                                int proxyRow, const QImage &img)
{
    const QRect cell = cellInGrab(view, proxy, proxyRow);
    const int y = cell.center().y();
    int minX = -1, maxX = -1;
    if (y < 0 || y >= img.height())
        return qMakePair(-1, -1);
    for (int x = cell.left(); x <= cell.right(); ++x) {
        if (x < 0 || x >= img.width())
            continue;
        const QColor c = img.pixelColor(x, y);
        if (isBarPixel(c)) {
            if (minX < 0)
                minX = x;
            maxX = x;
        }
    }
    return qMakePair(minX, maxX);
}

// DEVT09: the waterfall must read like Chrome's — a dense-zoomed
// axis, a real minimum bar width, an overflow notch on clipped bars,
// and DCL/Load marker lines.
void tst_DevTools::bidiWaterfallLegibility()
{
    BidiPanel panel;
    panel.onConnectionChanged(BidiClient::Connected);
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"}]}}"));
    QTabWidget *tabs = panel.findChild<QTabWidget *>();
    QVERIFY(tabs);
    tabs->setCurrentWidget(panel.m_networkView->parentWidget());
    panel.resize(1100, 460);
    panel.show();
    QCoreApplication::processEvents();

    // Empty log: no window, nothing to paint — no phantom bars.
    QCOMPARE(panel.testWaterfallFullEnd(), -1.0);
    QCOMPARE(panel.testWaterfallDisplayEnd(), -1.0);

    const auto feed = [&panel](const QByteArray &id, double start,
                               double end) {
        panel.onEvent(QByteArray(
            "{\"method\":\"network.beforeRequestSent\",\"params\":{"
            "\"timestamp\":") + QByteArray::number(start)
            + QByteArray(",\"request\":{\"request\":\"") + id
            + QByteArray("\",\"method\":\"GET\",\"url\":\"http://a/")
            + id + QByteArray("\",\"type\":\"Image\"}}}"));
        if (end >= 0.0)
            panel.onEvent(QByteArray(
                "{\"method\":\"network.responseCompleted\",\"params\":{"
                "\"timestamp\":") + QByteArray::number(end)
                + QByteArray(",\"request\":{\"request\":\"") + id
                + QByteArray("\"},\"response\":{\"encodedDataLength\":128}}}"));
    };
    QSortFilterProxyModel *proxy = panel.m_networkProxy;
    // Proxy row for a request id (name = url tail).  Row order runs
    // through the proxy, so the suite finds rows by name.
    const auto rowFor = [proxy](const QByteArray &id) -> int {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->index(r, 0).data().toString()
                == QString::fromLatin1(id))
                return r;
        }
        return -1;
    };
    QImage shot;
    const auto regrab = [&]() {
        QCoreApplication::processEvents();
        shot = panel.m_networkView->grab().toImage();
    };

    // --- dense-window zoom: ten fast requests + one 18 s outlier.
    for (int i = 0; i < 10; ++i)
        feed("z" + QByteArray::number(i), 0.0, 100.0 + 10 * i);
    feed("zs", 0.0, 18000.0);
    // The true window keeps the outlier (the footer's Finish uses
    // it); the paint window caps at the p90 end (190 ms) because the
    // full span is way past 2x the dense span.
    QCOMPARE(panel.testWaterfallFullEnd(), 18000.0);
    QCOMPARE(panel.testWaterfallDisplayEnd(), 190.0);
    regrab();
    QVERIFY(rowFor("z0") >= 0);
    QVERIFY(rowFor("zs") >= 0);
    const QRect fallCell = cellInGrab(panel.m_networkView, proxy, 0);
    // A 100 ms bar is ~53% of the zoomed 190 ms axis — dozens of
    // pixels, not the 1 px hairline the raw 18 s window gave.
    const QPair<int, int> fast = barRunAt(
        panel.m_networkView, proxy, rowFor("z0"), shot);
    QVERIFY(fast.first >= 0);
    QVERIFY(fast.second - fast.first + 1 > 40);
    // The outlier bar is clipped at the cell's right edge (its
    // overflow notch rides the same x).
    const QPair<int, int> slow = barRunAt(
        panel.m_networkView, proxy, rowFor("zs"), shot);
    QVERIFY(slow.first >= 0);
    QVERIFY(slow.second >= fallCell.right() - 5);

    // Offscreen grab for visual review — set ARORA_DEVTOOLS_GRAB_DIR
    // to a directory; the zoomed waterfall is saved there.
    const QByteArray grabDir = qgetenv("ARORA_DEVTOOLS_GRAB_DIR");
    if (!grabDir.isEmpty())
        panel.m_networkView->grab().save(
            QString::fromLocal8Bit(grabDir)
            + QLatin1String("/bidi-waterfall-zoom.png"));

    // --- minimum readable bar: a 1 ms request in a 400 ms window
    // maps to 0 px unscaled — the 5 px floor keeps it a swatch.
    // (400 == exactly 2x the 200 ms dense end: NOT capped — the zoom
    // only bites when the tail is past twice the dense span.)
    panel.clearNetworkLog();
    feed("m1", 0.0, 1.0);
    feed("m2", 0.0, 200.0);
    feed("m3", 0.0, 400.0);
    QCOMPARE(panel.testWaterfallDisplayEnd(), 400.0);
    regrab();
    const QPair<int, int> tiny = barRunAt(
        panel.m_networkView, proxy, rowFor("m1"), shot);
    QVERIFY(tiny.first >= 0);
    QVERIFY(tiny.second - tiny.first + 1 >= 5);
    QVERIFY(tiny.second - tiny.first + 1 <= 14); // floor, not real width

    // --- even spread keeps the honest full axis (no zoom): ten ends
    // spaced 100..1000 ms leave display == full.
    panel.clearNetworkLog();
    for (int i = 1; i <= 10; ++i)
        feed("e" + QByteArray::number(i), 0.0, 100.0 * i);
    QCOMPARE(panel.testWaterfallFullEnd(), 1000.0);
    QCOMPARE(panel.testWaterfallDisplayEnd(), 1000.0);

    // --- a window with no completed requests paints no bars at all.
    panel.clearNetworkLog();
    feed("p1", 50.0, -1.0);
    QCOMPARE(panel.testWaterfallDisplayEnd(), 50.0); // w1 == w0
    regrab();
    const QPair<int, int> none = barRunAt(
        panel.m_networkView, proxy, rowFor("p1"), shot);
    QCOMPARE(none.first, -1);

    // --- pending beside a completed row still paints, translucent:
    // its ink is the type swatch alpha-120 composited over Base.
    feed("p2", 100.0, -1.0);
    feed("c1", 0.0, 300.0);
    QVERIFY(rowFor("p2") >= 0);
    regrab();
    const QRect pCell = cellInGrab(
        panel.m_networkView, proxy, rowFor("p2"));
    const QColor swatch = AroraStyle::networkTypeSwatch(
        QStringLiteral("image"), qApp->palette());
    const QColor base = qApp->palette().color(QPalette::Base);
    const QColor blended(
        (120 * swatch.red() + 135 * base.red()) / 255,
        (120 * swatch.green() + 135 * base.green()) / 255,
        (120 * swatch.blue() + 135 * base.blue()) / 255);
    bool sawPending = false;
    for (int x = pCell.left() + 2; x <= pCell.right() - 2 && !sawPending;
         ++x) {
        if (colorClose(shot.pixelColor(x, pCell.center().y()),
                       blended, 14))
            sawPending = true;
    }
    QVERIFY(sawPending);

    // --- DCL/Load markers land as Link/fail-colored vertical lines
    // inside the column; a marker outside the window does not paint.
    feed("c2", 0.0, 300.0);
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.domContentLoaded\",\"params\":"
        "{\"context\":\"ctx1\",\"timestamp\":100.0}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"browsingContext.load\",\"params\":"
        "{\"context\":\"ctx1\",\"timestamp\":250.0}}"));
    regrab();
    if (!grabDir.isEmpty())
        panel.m_networkView->grab().save(
            QString::fromLocal8Bit(grabDir)
            + QLatin1String("/bidi-waterfall-markers.png"));
    const QColor link = qApp->palette().color(QPalette::Link);
    bool sawDcl = false, sawLoad = false;
    const QRect anyCell = cellInGrab(panel.m_networkView, proxy, 0);
    const QColor fail = waterfallFailColor();
    for (int x = anyCell.left(); x <= anyCell.right(); ++x) {
        for (int y = anyCell.top(); y <= anyCell.bottom(); ++y) {
            const QColor c = shot.pixelColor(x, y);
            if (!sawDcl && colorClose(c, link, 8))
                sawDcl = true;
            // Only the marker paints the fail color here — no failed
            // rows in this log.
            if (!sawLoad && colorClose(c, fail, 8))
                sawLoad = true;
        }
    }
    QVERIFY(sawDcl);
    QVERIFY(sawLoad);
}

// DEVT11: the Chromium parity pass — the toolbar row gains the
// record/stop toggle plus honestly-disabled throttling and
// response-search controls, waterfall bars take per-type swatches,
// an ms ruler rides the column header, the waterfall hover breaks
// the raw CDP ResourceTiming into phases, and cache-served rows
// gray out.
void tst_DevTools::bidiNetworkParity()
{
    BidiPanel panel;
    panel.onConnectionChanged(BidiClient::Connected);
    panel.onResponse(7777, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"}]}}"));

    QAbstractTableModel *net = panel.m_networkModel;

    // Toolbar: Chrome's row — the record toggle starts recording,
    // the clear button sits next to it, throttling + response search
    // occupy their slots honestly disabled with the reason stated.
    QVERIFY(panel.m_recordToggle);
    QVERIFY(panel.m_recordToggle->isCheckable());
    QVERIFY(panel.m_recordToggle->isChecked());
    QVERIFY(!panel.m_recordToggle->icon().isNull());
    QVERIFY(panel.m_clearRequests);
    QVERIFY(panel.m_throttling);
    QVERIFY(!panel.m_throttling->isEnabled());
    QVERIFY(panel.m_throttling->toolTip().contains(
        QStringLiteral("not exposed")));
    QVERIFY(panel.m_responseSearch);
    QVERIFY(!panel.m_responseSearch->isEnabled());
    QVERIFY(panel.m_responseSearch->toolTip().contains(
        QStringLiteral("response bodies")));

    // Record off is Chrome's stop: network.* events still arrive
    // but nothing is logged; recording picks up where it left off.
    panel.m_recordToggle->setChecked(false);
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":100.0,\"request\":{\"request\":\"p1\","
        "\"method\":\"GET\",\"url\":\"http://a/paused\","
        "\"type\":\"Document\"}}}"));
    QCOMPARE(net->rowCount(), 0);
    panel.m_recordToggle->setChecked(true);
    QVERIFY(panel.m_recordToggle->toolTip().contains(
        QStringLiteral("Stop recording")));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":100.0,\"request\":{\"request\":\"p1\","
        "\"method\":\"GET\",\"url\":\"http://a/paused\","
        "\"type\":\"Document\"}}}"));
    QCOMPARE(net->rowCount(), 1);

    // The raw ResourceTiming block becomes the waterfall hover's
    // per-segment breakdown — every phase CDP reported.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseStarted\",\"params\":{"
        "\"timestamp\":140.0,\"type\":\"Document\",\"request\":{"
        "\"request\":\"p1\"},\"response\":{\"status\":200,"
        "\"timing\":{\"dnsStart\":1.0,\"dnsEnd\":5.0,"
        "\"connectStart\":5.0,\"connectEnd\":25.0,"
        "\"sslStart\":8.0,\"sslEnd\":24.0,"
        "\"sendStart\":25.0,\"sendEnd\":26.0,"
        "\"receiveHeadersEnd\":60.0}}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":200.0,\"request\":{\"request\":\"p1\"},"
        "\"response\":{\"encodedDataLength\":1024,"
        "\"decodedDataLength\":4096}}}"));
    const QString tip =
        net->data(net->index(0, 6), Qt::ToolTipRole).toString();
    QVERIFY(tip.contains(QStringLiteral("DNS Lookup: 4 ms")));
    QVERIFY(tip.contains(
        QStringLiteral("Initial connection: 20 ms")));
    QVERIFY(tip.contains(QStringLiteral("SSL: 16 ms")));
    QVERIFY(tip.contains(QStringLiteral("Request sent: 1 ms")));
    QVERIFY(tip.contains(QStringLiteral("Waiting (TTFB): 34 ms")));
    QVERIFY(tip.contains(QStringLiteral("Content Download: 40 ms")));

    // Size hover: wire bytes + the decoded resource size, like
    // Chrome's "transferred over network, resource size".
    const QString sizeTip =
        net->data(net->index(0, 4), Qt::ToolTipRole).toString();
    QVERIFY(sizeTip.contains(
        QStringLiteral("1.0 kB transferred over network")));
    QVERIFY(sizeTip.contains(
        QStringLiteral("resource size 4.0 kB")));

    // A cache-served row grays out (the request hit no wire) while
    // the live row keeps the default foreground.
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":300.0,\"request\":{\"request\":\"p2\","
        "\"method\":\"GET\",\"url\":\"http://a/cached.css\","
        "\"type\":\"Stylesheet\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.requestServedFromCache\",\"params\":{"
        "\"request\":{\"request\":\"p2\"}}}"));
    const QVariant grayed =
        net->data(net->index(1, 0), Qt::ForegroundRole);
    QVERIFY(grayed.isValid());
    QVERIFY(grayed.value<QColor>() != waterfallFailColor());
    QVERIFY(!net->data(net->index(0, 0), Qt::ForegroundRole)
                .isValid());

    // The ruler's ladder covers the recording window on nice,
    // evenly-spaced steps (p1 spans 100->300 ms).
    const QVector<double> ticks = panel.testWaterfallTicks();
    QVERIFY(ticks.size() >= 3);
    QVERIFY(qAbs(ticks.first() - 100.0) < 1.0);
    QVERIFY(ticks.last() <= panel.testWaterfallDisplayEnd() + 0.001);
    const double step = ticks.at(1) - ticks.at(0);
    for (int i = 2; i < ticks.size(); ++i)
        QVERIFY(qAbs(ticks.at(i) - ticks.at(i - 1) - step) < 0.001);

    // Per-type swatches: the Script row's cell paints amber ink the
    // Image row's does not — Chrome's type-colored waterfall.
    panel.clearNetworkLog();
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":0.0,\"request\":{\"request\":\"t1\","
        "\"method\":\"GET\",\"url\":\"http://a/app.js\","
        "\"type\":\"Script\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":100.0,\"request\":{\"request\":\"t1\"},"
        "\"response\":{\"encodedDataLength\":128}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.beforeRequestSent\",\"params\":{"
        "\"timestamp\":0.0,\"request\":{\"request\":\"t2\","
        "\"method\":\"GET\",\"url\":\"http://a/pic.png\","
        "\"type\":\"Image\"}}}"));
    panel.onEvent(QByteArray(
        "{\"method\":\"network.responseCompleted\",\"params\":{"
        "\"timestamp\":150.0,\"request\":{\"request\":\"t2\"},"
        "\"response\":{\"encodedDataLength\":128}}}"));
    QTabWidget *tabs = panel.findChild<QTabWidget *>();
    QVERIFY(tabs);
    tabs->setCurrentWidget(panel.m_networkView->parentWidget());
    panel.resize(900, 320);
    panel.show();
    QCoreApplication::processEvents();
    const QImage shot = panel.m_networkView->grab().toImage();
    QSortFilterProxyModel *proxy = panel.m_networkProxy;
    const auto rowFor = [proxy](const QString &name) -> int {
        for (int r = 0; r < proxy->rowCount(); ++r) {
            if (proxy->index(r, 0).data().toString() == name)
                return r;
        }
        return -1;
    };
    const auto cellHas = [&](const QString &name,
                             const QColor &want) {
        const int row = rowFor(name);
        if (row < 0)
            return false;
        const QRect cell = cellInGrab(panel.m_networkView, proxy,
                                      row);
        const int y = cell.center().y();
        for (int x = cell.left(); x <= cell.right(); ++x) {
            if (colorClose(shot.pixelColor(x, y), want))
                return true;
        }
        return false;
    };
    const QColor amber = AroraStyle::networkTypeSwatch(
        QStringLiteral("script"), qApp->palette());
    const QColor purple = AroraStyle::networkTypeSwatch(
        QStringLiteral("image"), qApp->palette());
    QVERIFY(cellHas(QStringLiteral("app.js"), amber));
    QVERIFY(cellHas(QStringLiteral("pic.png"), purple));
    QVERIFY(!cellHas(QStringLiteral("app.js"), purple));
    QVERIFY(!cellHas(QStringLiteral("pic.png"), amber));

    // The ruler section paints too — grab the header once so a
    // paint-time crash/regression is caught; the ladder math itself
    // is pinned by testWaterfallTicks above.
    const QByteArray grabDir = qgetenv("ARORA_DEVTOOLS_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        panel.m_networkView->horizontalHeader()->grab().save(
            QString::fromLocal8Bit(grabDir)
            + QLatin1String("/bidi-network-ruler.png"));
        panel.m_networkView->grab().save(
            QString::fromLocal8Bit(grabDir)
            + QLatin1String("/bidi-network-parity.png"));
    }
}

// DEVT13 — the full tab set: Chrome's row order, every new page's
// widgets present, the DOM/Storage tabs retired into Elements/
// Application, and the arora.cdp verdicts driving the honest-
// disabled states.  CdpKind numbers mirror bidipanel.cpp's enum
// (it is file-local there).
// DEVT20: Sources parity — the raw CDP debugger events the bridge
// forwards under the "debugger" subscription (scriptParsed, paused,
// resumed, breakpointResolved, Page.frameNavigated) drive the pause
// chrome, the call stack, the script-source preference and the
// paused-line highlight; getScriptSource replies fill the view.
void tst_DevTools::bidiSourcesParity()
{
    BidiPanel panel;
    panel.show();

    // The debugger surface exists and starts cold: unprobed means
    // every button down and no pause chrome.
    QVERIFY(panel.m_dbgBar);
    QVERIFY(panel.m_dbgPause && panel.m_dbgResume);
    QVERIFY(panel.m_dbgStepOver && panel.m_dbgStepInto
            && panel.m_dbgStepOut);
    QVERIFY(panel.m_callFrames && panel.m_pauseStatus);
    QVERIFY(panel.m_srcRefreshDebounce);
    QVERIFY(!panel.m_debuggerPaused);
    QVERIFY(panel.m_pauseStatus->isHidden());
    QVERIFY(!panel.m_dbgPause->isEnabled());

    // A context so event scoping has something to match.
    panel.onResponse(41, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://a/\"}]}}"));
    QCOMPARE(panel.currentContext(), QStringLiteral("ctx1"));

    auto probe = [&panel](const char *domain, bool up) {
        const quint64 id = panel.testRegisterCdp(
            1 /*CdpProbe*/, QString::fromLatin1(domain));
        panel.onResponse(id, up
            ? QByteArray("{\"result\":{}}")
            : QByteArray("{\"error\":{\"message\":\"'")
                  + QByteArray(domain)
                  + QByteArray("' wasn't found\"}}"));
    };
    probe("Runtime", true);
    probe("DOM", true);
    probe("CSS", true);
    probe("Page", true);
    probe("Debugger", true);
    probe("Security", true);
    probe("Performance", true);
    probe("Profiler", true);
    probe("HeapProfiler", true);
    QVERIFY(panel.domainUp(QStringLiteral("Debugger")));

    // Chrome's enable rule: Pause arms on demand; Resume/steps wait
    // for a real paused state.
    QVERIFY(panel.m_dbgPause->isEnabled());
    QVERIFY(!panel.m_dbgResume->isEnabled());
    QVERIFY(!panel.m_dbgStepOver->isEnabled());
    QVERIFY(!panel.m_dbgStepInto->isEnabled());
    QVERIFY(!panel.m_dbgStepOut->isEnabled());

    // scriptParsed fills the scriptId <-> url maps, context-scoped.
    panel.onEvent(QByteArray(
        "{\"method\":\"Debugger.scriptParsed\",\"params\":{"
        "\"context\":\"ctx1\",\"scriptId\":\"s1\","
        "\"url\":\"http://a/app.js\"}}"));
    QCOMPARE(panel.m_scriptIds.value(
                 QStringLiteral("http://a/app.js")),
             QStringLiteral("s1"));
    QCOMPARE(panel.m_scriptUrls.value(QStringLiteral("s1")),
             QStringLiteral("http://a/app.js"));
    panel.onEvent(QByteArray(
        "{\"method\":\"Debugger.scriptParsed\",\"params\":{"
        "\"context\":\"other\",\"scriptId\":\"sX\","
        "\"url\":\"http://x/foreign.js\"}}"));
    QVERIFY(!panel.m_scriptUrls.contains(QStringLiteral("sX")));
    // A foreign-context pause doesn't move the chrome either.
    panel.onEvent(QByteArray(
        "{\"method\":\"Debugger.paused\",\"params\":{"
        "\"context\":\"other\",\"reason\":\"other\",\"callFrames\":["
        "{\"functionName\":\"f\",\"url\":\"http://x/f.js\","
        "\"location\":{\"scriptId\":\"sX\",\"lineNumber\":0}}]}}"));
    QVERIFY(!panel.m_debuggerPaused);

    // paused: call frames fill (0-based CDP lines display 1-based),
    // the status line names the reason, the buttons flip.
    panel.onEvent(QByteArray(
        "{\"method\":\"Debugger.paused\",\"params\":{"
        "\"context\":\"ctx1\",\"reason\":\"other\","
        "\"hitBreakpoints\":[\"b1\"],\"callFrames\":["
        "{\"functionName\":\"tick\",\"url\":\"http://a/app.js\","
        "\"location\":{\"scriptId\":\"s1\",\"lineNumber\":3}},"
        "{\"functionName\":\"\",\"url\":\"\","
        "\"location\":{\"scriptId\":\"s2\",\"lineNumber\":0}}]}}"));
    QVERIFY(panel.m_debuggerPaused);
    QVERIFY(!panel.m_pauseStatus->isHidden());
    QCOMPARE(panel.m_pauseStatus->text(),
             QStringLiteral("Paused on breakpoint"));
    QCOMPARE(panel.m_callFrames->count(), 2);
    QVERIFY(panel.m_callFrames->item(0)->text().contains(
        QStringLiteral("tick")));
    QVERIFY(panel.m_callFrames->item(0)->text().contains(
        QStringLiteral("app.js:4")));
    QVERIFY(panel.m_callFrames->item(1)->text().contains(
        QStringLiteral("anonymous")));
    QVERIFY(panel.m_dbgResume->isEnabled());
    QVERIFY(panel.m_dbgStepOver->isEnabled());
    QVERIFY(panel.m_dbgStepInto->isEnabled());
    QVERIFY(panel.m_dbgStepOut->isEnabled());
    QVERIFY(!panel.m_dbgPause->isEnabled());

    // The paused location's getScriptSource reply fills the view and
    // highlights the 1-based line the request carried in n.
    const quint64 sid = panel.testRegisterCdp(
        18 /*CdpScriptSource*/, QStringLiteral("s1"),
        QStringLiteral("http://a/app.js"), 4);
    panel.onResponse(sid, QByteArray(
        "{\"result\":{\"scriptSource\":\"a\\nb\\nc\\nd\\n\"}}"));
    QCOMPARE(panel.m_sourceView->toPlainText(),
             QStringLiteral("a\nb\nc\nd\n"));
    QCOMPARE(panel.m_sourceView->extraSelections().count(), 1);
    // A getScriptSource error on an unrelated url lands as honest
    // readable text.
    const quint64 bad = panel.testRegisterCdp(
        18, QStringLiteral("sX"),
        QStringLiteral("http://x/foreign.js"), 0);
    panel.onResponse(bad, QByteArray(
        "{\"error\":{\"message\":\"no script\"}}"));
    QVERIFY(panel.m_sourceView->toPlainText().contains(
        QStringLiteral("no script")));

    // breakpointResolved re-points a pending row at the resolved
    // script's url:line (0-based -> 1-based).
    panel.m_breakpoints.insert(
        QStringLiteral("b1"),
        {QStringLiteral("http://a/app.js"), 0});
    panel.rebuildBreakpointList();
    panel.onEvent(QByteArray(
        "{\"method\":\"Debugger.breakpointResolved\",\"params\":{"
        "\"context\":\"ctx1\",\"breakpointId\":\"b1\",\"location\":{"
        "\"scriptId\":\"s1\",\"lineNumber\":3}}}"));
    QCOMPARE(panel.m_breakpoints.value(
                 QStringLiteral("b1")).second, 4);
    QVERIFY(panel.m_bpList->item(0)->text().contains(
        QStringLiteral("app.js:4")));

    // frameNavigated arms the debounced resource refresh.
    QVERIFY(!panel.m_srcRefreshDebounce->isActive());
    panel.onEvent(QByteArray(
        "{\"method\":\"Page.frameNavigated\",\"params\":{"
        "\"context\":\"ctx1\",\"frame\":{\"id\":\"f1\","
        "\"url\":\"http://a/page.html\"}}}"));
    QVERIFY(panel.m_srcRefreshDebounce->isActive());
    panel.m_srcRefreshDebounce->stop();

    // resumed unwinds the whole paused chrome.
    panel.onEvent(QByteArray(
        "{\"method\":\"Debugger.resumed\",\"params\":{"
        "\"context\":\"ctx1\"}}"));
    QVERIFY(!panel.m_debuggerPaused);
    QCOMPARE(panel.m_callFrames->count(), 0);
    QVERIFY(panel.m_pauseStatus->isHidden());
    QVERIFY(panel.m_dbgPause->isEnabled());
    QVERIFY(!panel.m_dbgResume->isEnabled());
}

// DEVT23: the Sources navigator is Chrome's Page tree — frames
// ("top" + subframe hosts) group resources by origin, an origin's
// files sit under folders mirroring the url path with single-
// child chains folded into one "a/b" row; the right debugger rail
// keeps the live controls + Breakpoints/Call Stack sections while
// the endpoint-less groups stay honestly disabled.
void tst_DevTools::bidiSourcesUx()
{
    BidiPanel panel;
    panel.show();

    panel.onResponse(41, QByteArray(
        "{\"result\":{\"contexts\":["
        "{\"context\":\"ctx1\",\"url\":\"http://hup.hu/\"}]}}"));

    auto probe = [&panel](const char *domain, bool up) {
        const quint64 id = panel.testRegisterCdp(
            1 /*CdpProbe*/, QString::fromLatin1(domain));
        panel.onResponse(id, up
            ? QByteArray("{\"result\":{}}")
            : QByteArray("{\"error\":{\"message\":\"'")
                  + QByteArray(domain)
                  + QByteArray("' wasn't found\"}}"));
    };
    probe("Runtime", true);
    probe("DOM", true);
    probe("CSS", true);
    probe("Page", true);
    probe("Debugger", true);
    probe("Security", true);
    probe("Performance", true);
    probe("Profiler", true);
    probe("HeapProfiler", true);

    // Nested paths across two origins plus a subframe.
    const quint64 id = panel.testRegisterCdp(2 /*CdpResourceTree*/);
    panel.onResponse(id, QByteArray(
        "{\"result\":{\"frameTree\":{\"frame\":{\"id\":\"f1\","
        "\"url\":\"http://hup.hu/index.html\"},\"resources\":["
        "{\"url\":\"http://hup.hu/app.js\",\"type\":\"Script\","
        "\"mimeType\":\"text/javascript\"},"
        "{\"url\":\"http://hup.hu/core/images/modules/deep/util.js\","
        "\"type\":\"Script\",\"mimeType\":\"text/javascript\"},"
        "{\"url\":\"http://hup.hu/core/images/modules/icons.png\","
        "\"type\":\"Image\",\"mimeType\":\"image/png\"},"
        "{\"url\":\"http://cdn.example.com/lib/util.js\","
        "\"type\":\"Script\",\"mimeType\":\"text/javascript\"}],"
        "\"childFrames\":[{\"frame\":{\"id\":\"f2\","
        "\"url\":\"http://ads.example.net/banner.html\"},"
        "\"resources\":[{\"url\":\"http://ads.example.net/b.js\","
        "\"type\":\"Script\",\"mimeType\":\"text/javascript\"}]}]}}}"));

    QTreeWidgetItem *page = panel.m_sourcesTree->topLevelItem(0);
    QVERIFY(page);
    QCOMPARE(page->text(0), QStringLiteral("Page"));
    QCOMPARE(page->childCount(), 2); // top + the subframe
    QTreeWidgetItem *top = page->child(0);
    QCOMPARE(top->text(0), QStringLiteral("top"));
    // Origins sort by label — cdn.example.com before hup.hu.
    QCOMPARE(top->childCount(), 2);
    QCOMPARE(top->child(0)->text(0),
             QStringLiteral("cdn.example.com"));
    QCOMPARE(top->child(0)->child(0)->text(0),
             QStringLiteral("lib"));
    QCOMPARE(top->child(0)->child(0)->child(0)->text(0),
             QStringLiteral("util.js"));
    QTreeWidgetItem *hup = top->child(1);
    QCOMPARE(hup->text(0), QStringLiteral("hup.hu"));
    QCOMPARE(hup->childCount(), 3);
    QCOMPARE(hup->child(0)->text(0), QStringLiteral("app.js"));
    // core/images is a chain of single-child folders — Chrome
    // folds it into one row that stops at "modules", which has a
    // real mixed child set (the deep/ folder and icons.png).
    QTreeWidgetItem *merged = hup->child(1);
    QCOMPARE(merged->text(0),
             QStringLiteral("core/images/modules"));
    QCOMPARE(merged->childCount(), 2);
    QCOMPARE(merged->child(0)->text(0), QStringLiteral("deep"));
    QCOMPARE(merged->child(1)->text(0),
             QStringLiteral("icons.png"));
    QTreeWidgetItem *deep = merged->child(0);
    QCOMPARE(deep->childCount(), 1);
    QCOMPARE(deep->child(0)->text(0), QStringLiteral("util.js"));
    QCOMPARE(merged->child(1)->data(0, Qt::UserRole).toString(),
             QStringLiteral(
                 "http://hup.hu/core/images/modules/icons.png"));
    // The frame document is a leaf too — UserRole+2 keeps "Document"
    // so breakpoints still arm on it.
    QCOMPARE(hup->child(2)->text(0), QStringLiteral("index.html"));
    QCOMPARE(hup->child(2)->data(0, Qt::UserRole + 2).toString(),
             QStringLiteral("Document"));
    // The subframe groups under Page as a sibling of "top".
    QTreeWidgetItem *sub = page->child(1);
    QCOMPARE(sub->text(0), QStringLiteral("ads.example.net"));
    QCOMPARE(sub->child(0)->text(0),
             QStringLiteral("ads.example.net"));
    QCOMPARE(sub->child(0)->childCount(), 2);
    QCOMPARE(sub->child(0)->child(0)->text(0),
             QStringLiteral("b.js"));
    QCOMPARE(sub->child(0)->child(1)->text(0),
             QStringLiteral("banner.html"));
    QCOMPARE(sub->child(0)->child(0)
                 ->data(0, Qt::UserRole + 1).toString(),
             QStringLiteral("f2"));

    // Selecting a file leaf fills the center view; a group row is
    // inert (no fetch, the view keeps its content).
    panel.sourceSelected(deep->child(0));
    const quint64 cid = panel.testRegisterCdp(
        3 /*CdpResourceContent*/, QStringLiteral("f1"),
        QStringLiteral("http://hup.hu/core/images/modules/deep/util.js"));
    panel.onResponse(cid, QByteArray(
        "{\"result\":{\"content\":\"function u(){}\","
        "\"base64Encoded\":false}}"));
    QCOMPARE(panel.m_sourceView->toPlainText(),
             QStringLiteral("function u(){}"));
    // The Script leaf armed the breakpoint row (Debugger is up).
    QVERIFY(panel.m_bpBar->isEnabled());
    panel.sourceSelected(hup);
    QCOMPARE(panel.m_sourceView->toPlainText(),
             QStringLiteral("function u(){}"));

    // The right debugger rail holds the controls + live sections;
    // Watch/Scope and the XHR/DOM/listener groups are present but
    // honestly disabled with a reason.
    QWidget *rail = panel.findChild<QWidget *>(
        QStringLiteral("bidiSourcesDbgRail"));
    QVERIFY(rail);
    QVERIFY(rail->isAncestorOf(panel.m_dbgBar));
    QVERIFY(rail->isAncestorOf(panel.m_bpList));
    QVERIFY(rail->isAncestorOf(panel.m_callFrames));
    QVERIFY(!rail->isAncestorOf(panel.m_sourceView));
    const char *offSections[] = {
        "bidiSourcesSecWatch", "bidiSourcesSecScope",
        "bidiSourcesSecXhr", "bidiSourcesSecDom",
        "bidiSourcesSecListeners", "bidiSourcesSecEventBp",
    };
    for (const char *name : offSections) {
        QAbstractButton *head = panel.findChild<QAbstractButton *>(
            QString::fromLatin1(name));
        QVERIFY(head);
        QVERIFY(!head->isEnabled());
        QVERIFY(!head->toolTip().isEmpty()); // the honest reason
    }
    // The live section headers are real collapsible accordions.
    QAbstractButton *bpSec = panel.findChild<QAbstractButton *>(
        QStringLiteral("bidiSourcesSecBp"));
    QVERIFY(bpSec && bpSec->isCheckable());
    QWidget *bpBody = panel.m_bpList->parentWidget();
    QVERIFY(!bpBody->isHidden());
    bpSec->setChecked(false);
    QVERIFY(bpBody->isHidden());
    bpSec->setChecked(true);
    QVERIFY(!bpBody->isHidden());
}

void tst_DevTools::bidiFullTabSet()
{
    BidiPanel panel;
    panel.show();

    // Tab order: Elements Console Sources Network Performance
    // Memory Application Security — DOM and Storage are gone.
    const char *want[] = {
        "Elements", "Console", "Sources", "Network",
        "Performance", "Memory", "Application", "Security",
    };
    QCOMPARE(panel.m_tabs->count(), 8);
    for (int i = 0; i < 8; ++i)
        QCOMPARE(panel.m_tabs->tabText(i),
                 QString::fromLatin1(want[i]));

    // Elements: tree + toolbar + crumbs + styles pane.  DEVT22:
    // markup delegate, stats label, find results list.
    QVERIFY(panel.findChild<QWidget *>(
                QStringLiteral("bidiElementsToolbar")));
    QVERIFY(panel.findChild<QWidget *>(
                QStringLiteral("bidiElementsCrumbs")));
    QVERIFY(panel.findChild<QWidget *>(
                QStringLiteral("bidiElementsFindResults")));
    QVERIFY(panel.m_inspectToggle && panel.m_inspectToggle->isCheckable());
    QVERIFY(panel.m_stylesView);
    QVERIFY(panel.m_stylesTitle);
    QVERIFY(panel.m_domStats && panel.m_domFindResults);
    QVERIFY(panel.m_domTree->itemDelegate()
                ->property("aroraDomMarkup").toBool());

    // Sources: resource tree, read-only view, breakpoint row + list.
    QVERIFY(panel.m_sourcesTree);
    QVERIFY(panel.m_sourceView->isReadOnly());
    QVERIFY(panel.m_bpBar && panel.m_bpLine && panel.m_bpList);
    // DEVT23: the right debugger rail — controls + collapsible
    // sections live here, not stacked under the view.
    QVERIFY(panel.findChild<QWidget *>(
                QStringLiteral("bidiSourcesDbgRail")));
    QVERIFY(panel.findChild<QAbstractButton *>(
                QStringLiteral("bidiSourcesSecBp")));
    QVERIFY(panel.findChild<QAbstractButton *>(
                QStringLiteral("bidiSourcesSecStack")));

    // Performance / Memory controls exist.
    QVERIFY(panel.m_cpuToggle && panel.m_cpuToggle->isCheckable());
    QVERIFY(panel.m_metricsTable && panel.m_cpuTable);
    QVERIFY(panel.m_heapButton && panel.m_heapTable);
    QVERIFY(panel.m_snapButton);
    QVERIFY(panel.m_perfOffline && panel.m_memOffline);
    QVERIFY(panel.m_perfOffline->isHidden());
    QVERIFY(panel.m_memOffline->isHidden());

    // Application: nav tree + four stacked value pages.
    QVERIFY(panel.m_appTree && panel.m_appStack);
    QCOMPARE(panel.m_appStack->count(), 4);
    QVERIFY(panel.m_cookieTable && panel.m_localStorageTable);
    QVERIFY(panel.m_sessionStorageTable && panel.m_idbTable);

    // Security page labels.
    QVERIFY(panel.m_secHeadline && panel.m_secDetail);
    QVERIFY(panel.m_secMixed);

    // Unprobed = not up.
    QVERIFY(!panel.domainUp(QStringLiteral("CSS")));
    QCOMPARE(panel.domainReason(QStringLiteral("CSS")),
             QStringLiteral("not probed"));

    // Drive the probe verdicts: everything up except Profiler and
    // HeapProfiler — Performance/Memory must disable WITH a reason,
    // the rest stays up.
    auto probe = [&panel](const char *domain, bool up) {
        const quint64 id = panel.testRegisterCdp(
            1 /*CdpProbe*/, QString::fromLatin1(domain));
        panel.onResponse(id, up
            ? QByteArray("{\"result\":{}}")
            : QByteArray("{\"error\":{\"message\":\"'")
                  + QByteArray(domain)
                  + QByteArray("' wasn't found\"}}"));
    };
    probe("Runtime", true);
    probe("DOM", true);
    probe("CSS", true);
    probe("Page", true);
    probe("Debugger", true);
    probe("Security", true);
    probe("Performance", true);
    probe("Profiler", false);
    probe("HeapProfiler", false);
    QVERIFY(panel.domainUp(QStringLiteral("CSS")));
    QVERIFY(panel.domainUp(QStringLiteral("Debugger")));
    QVERIFY(!panel.domainUp(QStringLiteral("Profiler")));
    QCOMPARE(panel.domainReason(QStringLiteral("Profiler")),
             QStringLiteral("'Profiler' wasn't found"));

    // Perf needs Performance+Profiler: Profiler's miss disables it
    // and explains itself; Memory's HeapProfiler miss does the same.
    QVERIFY(!panel.m_perfBody->isEnabled());
    QVERIFY(!panel.m_perfOffline->isHidden());
    QVERIFY(panel.m_perfOffline->text().contains(
        QStringLiteral("Profiler")));
    QVERIFY(!panel.m_memBody->isEnabled());
    QVERIFY(!panel.m_memOffline->isHidden());
    QVERIFY(panel.m_memOffline->text().contains(
        QStringLiteral("HeapProfiler")));
    // Sources is up (Page answered) and the bp bar is live
    // (Debugger answered).
    QVERIFY(panel.m_sourcesTree->isEnabled());
    QVERIFY(panel.m_bpBar->isEnabled());
    QCOMPARE(panel.m_stylesTitle->text(),
             QStringLiteral("Computed styles"));

    // Flip both verdicts up — the bodies re-enable, no fake UI.
    probe("Profiler", true);
    probe("HeapProfiler", true);
    QVERIFY(panel.m_perfBody->isEnabled());
    QVERIFY(panel.m_memBody->isEnabled());
    QVERIFY(panel.m_perfOffline->isHidden());
    QVERIFY(panel.m_memOffline->isHidden());

    // Sources: a canned resource tree builds the grouped nav.
    {
        const quint64 id = panel.testRegisterCdp(2 /*CdpResourceTree*/);
        panel.onResponse(id, QByteArray(
            "{\"result\":{\"frameTree\":{\"frame\":{\"id\":\"f1\","
            "\"url\":\"http://a/page.html\"},\"resources\":["
            "{\"url\":\"http://a/app.js\",\"type\":\"Script\","
            "\"mimeType\":\"text/javascript\"},"
            "{\"url\":\"http://a/site.css\",\"type\":\"Stylesheet\","
            "\"mimeType\":\"text/css\"}]}}}"));
        // DEVT23: Page > top > origin > file leaves — the document
        // itself is a leaf, no flat type groups.
        QTreeWidgetItem *page =
            panel.m_sourcesTree->topLevelItem(0);
        QVERIFY(page);
        QCOMPARE(page->text(0), QStringLiteral("Page"));
        QTreeWidgetItem *top = page->child(0);
        QVERIFY(top);
        QCOMPARE(top->text(0), QStringLiteral("top"));
        QTreeWidgetItem *origin = top->child(0);
        QVERIFY(origin);
        QCOMPARE(origin->text(0), QStringLiteral("a"));
        QCOMPARE(origin->childCount(), 3);
        QTreeWidgetItem *js = origin->child(0);
        QCOMPARE(js->text(0), QStringLiteral("app.js"));
        QCOMPARE(js->data(0, Qt::UserRole).toString(),
                 QStringLiteral("http://a/app.js"));
        QCOMPARE(js->data(0, Qt::UserRole + 1).toString(),
                 QStringLiteral("f1"));
        QCOMPARE(js->data(0, Qt::UserRole + 2).toString(),
                 QStringLiteral("Script"));
        QCOMPARE(origin->child(1)->text(0),
                 QStringLiteral("page.html"));
        QCOMPARE(origin->child(2)->text(0),
                 QStringLiteral("site.css"));

        // Content: raw and base64 bodies both decode.
        const quint64 cid = panel.testRegisterCdp(
            3 /*CdpResourceContent*/, QStringLiteral("f1"),
            QStringLiteral("http://a/app.js"));
        panel.onResponse(cid, QByteArray(
            "{\"result\":{\"content\":\"var x=1;\","
            "\"base64Encoded\":false}}"));
        QCOMPARE(panel.m_sourceView->toPlainText(),
                 QStringLiteral("var x=1;"));
        const quint64 cid2 = panel.testRegisterCdp(
            3, QStringLiteral("f1"), QStringLiteral("http://a/x"));
        panel.onResponse(cid2, QByteArray(
            "{\"result\":{\"content\":\"PGI+bGk8L2I+\","
            "\"base64Encoded\":true}}"));
        QCOMPARE(panel.m_sourceView->toPlainText(),
                 QStringLiteral("<b>li</b>"));
        // A CDP error lands as readable text, not a crash.
        const quint64 cid3 = panel.testRegisterCdp(
            3, QStringLiteral("f1"), QStringLiteral("http://a/gone"));
        panel.onResponse(cid3, QByteArray(
            "{\"error\":{\"message\":\"no resource\"}}"));
        QVERIFY(panel.m_sourceView->toPlainText().contains(
            QStringLiteral("no resource")));
    }

    // Breakpoints: a set answer registers a list row keyed by the
    // real breakpointId; remove clears it.
    {
        const quint64 bid = panel.testRegisterCdp(
            12 /*CdpBpSet*/, QString(), QStringLiteral("http://a/app.js"),
            7);
        panel.onResponse(bid, QByteArray(
            "{\"result\":{\"breakpointId\":\"b1\",\"locations\":[]}}"));
        QCOMPARE(panel.m_bpList->count(), 1);
        QVERIFY(panel.m_bpList->item(0)->text().contains(
            QStringLiteral("app.js:7")));
        QVERIFY(panel.m_breakpoints.contains(
            QStringLiteral("b1")));
        const quint64 rid = panel.testRegisterCdp(
            13 /*CdpBpRemove*/, QStringLiteral("b1"));
        panel.onResponse(rid, QByteArray("{\"result\":{}}"));
        QCOMPARE(panel.m_bpList->count(), 0);
        QVERIFY(panel.m_breakpoints.isEmpty());
    }

    // Metrics: the table fills and JSHeapUsedSize feeds the Memory
    // status line.
    {
        const quint64 id = panel.testRegisterCdp(7 /*CdpMetrics*/);
        panel.onResponse(id, QByteArray(
            "{\"result\":{\"metrics\":["
            "{\"name\":\"TaskDuration\",\"value\":0.5},"
            "{\"name\":\"JSHeapUsedSize\",\"value\":2097152}]}}"));
        QCOMPARE(panel.m_metricsTable->rowCount(), 2);
        QCOMPARE(panel.m_metricsTable->item(1, 1)->text(),
                 QStringLiteral("2.0 MB"));
        QVERIFY(panel.m_memStatus->text().contains(
            QStringLiteral("2.0 MB")));
    }

    // CPU profile: nodes + samples + timeDeltas aggregate into the
    // top-self table.
    {
        const quint64 id = panel.testRegisterCdp(9 /*CdpProfStop*/);
        panel.onResponse(id, QByteArray(
            "{\"result\":{\"profile\":{\"nodes\":["
            "{\"id\":1,\"callFrame\":{\"functionName\":\"root\","
            "\"url\":\"http://a/app.js\",\"lineNumber\":0},"
            "\"children\":[2]},"
            "{\"id\":2,\"callFrame\":{\"functionName\":\"hot\","
            "\"url\":\"http://a/app.js\",\"lineNumber\":40}}],"
            "\"samples\":[2,2,1],"
            "\"timeDeltas\":[1000,2000,500]}}}"));
        // hot: 3ms self; root: 0.5ms self / 3.5ms total.
        QCOMPARE(panel.m_cpuTable->rowCount(), 2);
        QCOMPARE(panel.m_cpuTable->item(0, 0)->text(),
                 QStringLiteral("hot"));
        QCOMPARE(panel.m_cpuTable->item(0, 1)->text(),
                 QStringLiteral("3.0 ms"));
        QCOMPARE(panel.m_cpuTable->item(1, 0)->text(),
                 QStringLiteral("root"));
        QCOMPARE(panel.m_cpuTable->item(1, 2)->text(),
                 QStringLiteral("3.5 ms"));
        QVERIFY(panel.m_perfStatus->text().contains(
            QStringLiteral("2 functions")));
    }

    // Heap sampling: profile.head flattens to self size per fn.
    {
        const quint64 id = panel.testRegisterCdp(11 /*CdpHeapStop*/);
        panel.onResponse(id, QByteArray(
            "{\"result\":{\"profile\":{\"head\":{\"callFrame\":"
            "{\"functionName\":\"\",\"url\":\"\",\"lineNumber\":-1},"
            "\"selfSize\":0,\"children\":["
            "{\"callFrame\":{\"functionName\":\"make\","
            "\"url\":\"http://a/app.js\",\"lineNumber\":3},"
            "\"selfSize\":4096,\"children\":[]}]}}}}"));
        QCOMPARE(panel.m_heapTable->rowCount(), 2);
        QCOMPARE(panel.m_heapTable->item(0, 0)->text(),
                 QStringLiteral("make"));
        QCOMPARE(panel.m_heapTable->item(0, 1)->text(),
                 QStringLiteral("4.0 kB"));
    }

    // DEVT21 — the heap summary: the getHeapUsage answer stashes
    // used/total mid-sample, the stop reply composes one line with
    // totals + site count.
    {
        panel.m_snapshotRun = true;
        panel.m_snapStopSeen = false;
        panel.m_snapUsed = panel.m_snapTotal = -1;
        panel.m_snapErr.clear();
        panel.m_heapSampling = true; // usage lands mid-sample
        const quint64 uid = panel.testRegisterCdp(19 /*CdpHeapUsage*/);
        panel.onResponse(uid, QByteArray(
            "{\"result\":{\"usedSize\":2097152,"
            "\"totalSize\":8388608}}"));
        QCOMPARE(panel.m_snapUsed, qint64(2097152));
        QCOMPARE(panel.m_snapTotal, qint64(8388608));
        QVERIFY(panel.m_snapshotRun); // waits on the sampling leg
        const quint64 sid = panel.testRegisterCdp(11 /*CdpHeapStop*/);
        panel.onResponse(sid, QByteArray(
            "{\"result\":{\"profile\":{\"head\":{\"callFrame\":"
            "{\"functionName\":\"\",\"url\":\"\",\"lineNumber\":-1},"
            "\"selfSize\":0,\"children\":["
            "{\"callFrame\":{\"functionName\":\"alloc\","
            "\"url\":\"http://a/app.js\",\"lineNumber\":9},"
            "\"selfSize\":8192,\"children\":[]}]}}}}"));
        QVERIFY(panel.m_memStatus->text().contains(
            QStringLiteral("2.0 MB used of 8.0 MB total")));
        QVERIFY(panel.m_memStatus->text().contains(
            QStringLiteral("allocation sites")));
        QVERIFY(!panel.m_snapshotRun);

        // Reverse order: stop lands first, the usage reply finishes
        // the line (site count comes from the filled table).
        panel.m_snapshotRun = true;
        panel.m_snapStopSeen = false;
        panel.m_snapUsed = panel.m_snapTotal = -1;
        panel.m_snapErr.clear();
        const quint64 sid2 = panel.testRegisterCdp(11);
        panel.onResponse(sid2, QByteArray(
            "{\"result\":{\"profile\":{\"head\":{\"callFrame\":"
            "{\"functionName\":\"\",\"url\":\"\",\"lineNumber\":-1},"
            "\"selfSize\":0,\"children\":[]}}}}"));
        QVERIFY(panel.m_snapshotRun);
        QVERIFY(panel.m_memStatus->text().contains(
            QStringLiteral("pending")));
        const quint64 uid2 = panel.testRegisterCdp(19);
        panel.onResponse(uid2, QByteArray(
            "{\"result\":{\"usedSize\":1048576,"
            "\"totalSize\":4194304}}"));
        QVERIFY(!panel.m_snapshotRun);
        QVERIFY(panel.m_memStatus->text().contains(
            QStringLiteral("1.0 MB used of 4.0 MB total")));

        // A failed usage call keeps the sites and says why.
        panel.m_snapshotRun = true;
        panel.m_snapStopSeen = false;
        panel.m_snapUsed = panel.m_snapTotal = -1;
        panel.m_snapErr.clear();
        const quint64 uid3 = panel.testRegisterCdp(19);
        panel.m_heapSampling = true;
        panel.onResponse(uid3, QByteArray(
            "{\"error\":{\"message\":\"usage dead\"}}"));
        QCOMPARE(panel.m_snapErr, QStringLiteral("usage dead"));
        const quint64 sid3 = panel.testRegisterCdp(11);
        panel.onResponse(sid3, QByteArray(
            "{\"result\":{\"profile\":{\"head\":{\"callFrame\":"
            "{\"functionName\":\"\",\"url\":\"\",\"lineNumber\":-1},"
            "\"selfSize\":0,\"children\":[]}}}}"));
        QVERIFY(panel.m_memStatus->text().contains(
            QStringLiteral("heap usage unavailable: usage dead")));
        QVERIFY(!panel.m_snapshotRun);
    }

    // Security: http page reads "Not secure", https picks up the
    // TlsVerifier text path (no probe data recorded for the fake
    // host, so it lands on the "no probe" branch).
    panel.m_contexts = QJsonArray{QJsonObject{
        {QStringLiteral("context"), QStringLiteral("ctx1")},
        {QStringLiteral("url"),
         QStringLiteral("http://site/page.html")}}};
    panel.m_contextCombo->clear();
    panel.m_contextCombo->addItem(QStringLiteral("one"),
                                  QStringLiteral("ctx1"));
    panel.refreshSecurity();
    QVERIFY(panel.m_secHeadline->text().contains(
        QStringLiteral("Not secure")));
    QVERIFY(panel.m_secDetail->text().contains(
        QStringLiteral("http://site/page.html")));
    panel.m_contexts = QJsonArray{QJsonObject{
        {QStringLiteral("context"), QStringLiteral("ctx1")},
        {QStringLiteral("url"),
         QStringLiteral("https://site/page.html")}}};
    panel.refreshSecurity();
    QVERIFY(panel.m_secHeadline->text().contains(
        QStringLiteral("Encrypted")));
    QVERIFY(panel.m_secMixed->text().contains(
        QStringLiteral("No insecure")));

    // Breadcrumbs: pick the deepest element, the bar gets the
    // root..leaf chain as buttons.
    panel.m_domTree->clear();
    QTreeWidgetItem *root = new QTreeWidgetItem(
        QStringList(QStringLiteral("html")));
    root->setData(0, Qt::UserRole, QString());
    QTreeWidgetItem *body = new QTreeWidgetItem(
        QStringList(QStringLiteral("body")));
    body->setData(0, Qt::UserRole, QStringLiteral("0"));
    QTreeWidgetItem *div = new QTreeWidgetItem(
        QStringList(QStringLiteral("div#main")));
    div->setData(0, Qt::UserRole, QStringLiteral("0/0"));
    body->addChild(div);
    root->addChild(body);
    panel.m_domTree->addTopLevelItem(root);
    panel.updateCrumbs(div);
    const QList<QWidget *> crumbs =
        panel.m_crumbBar->findChildren<QWidget *>(
            QString(), Qt::FindDirectChildrenOnly);
    QCOMPARE(crumbs.size(), 5); // html › body › div#main
    // domItemForPath / domSelectPath: selecting a path lands the
    // current item on the deepest materialized hop.
    QCOMPARE(panel.domItemForPath(QStringLiteral("0/0")), div);
    panel.domSelectPath(QStringLiteral("0/0"));
    QCOMPARE(panel.m_domTree->currentItem(), div);
    QVERIFY(!panel.m_selectActive);
}

QTEST_MAIN(tst_DevTools)
#include "tst_devtools.moc"
