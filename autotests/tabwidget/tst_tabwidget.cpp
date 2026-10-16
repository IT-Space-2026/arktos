/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include "qtest_arora.h"

#include <browserapplication.h>
#include <browsermainwindow.h>
#include <browsertheme.h>
#include <engineinterface.h>
#include <engineregistry.h>
#include <enginetab.h>
#include <tabwidget.h>
#include <tabbar.h>
#include <tabpreview.h>
#include <webview.h>

#include "fakeengine.h"

#include <historycompleter.h>
#include <opensearchmanager.h>
#include <opensearchengine.h>
#include <privacyrequestinterceptor.h>
#include <toolbarsearch.h>

#include <containermanager.h>
#include <historymanager.h>

#include <qcompleter.h>
#include <qdir.h>
#include <qfile.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qmenu.h>
#include <qtoolbutton.h>
#include <qurlquery.h>
#include <qwebenginehistory.h>
#include <qsettings.h>

class tst_TabWidget : public QObject
{
    Q_OBJECT

public slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

private slots:
    void tabwidget_data();
    void tabwidget();
    void addWebAction_data();
    void addWebAction();
    void closeTab_data();
    void closeTab();
    void currentLocationBar_data();
    void currentLocationBar();
    void currentWebView_data();
    void currentWebView();
    void locationBarStack_data();
    void locationBarStack();
    void loadUrl_data();
    void loadUrl();
    void newTab_data();
    void newTab();
    void nextTab_data();
    void nextTab();
    void previousTab_data();
    void previousTab();
    void recentlyClosedTabsAction_data();
    void recentlyClosedTabsAction();
    void linkHovered_data();
    void linkHovered(const QString &);
    void loadProgress_data();
    void loadProgress(int);
    void setCurrentTitle_data();
    void setCurrentTitle(const QString &);
    void showStatusBarMessage_data();
    void showStatusBarMessage(const QString &);
    void tabsChanged_data();
    void tabsChanged();

    void saveState();
    void restoreStateCorrupt();
    void loadStringFromUntrustedSource();
    void tabBarPositionSetting();
    void verticalTabStripWidth();
    void verticalTabDefaultWidth();
    void tabHoverPreviews();
    void tabThumbnailStaleness();
    void omnibox();
    void omniboxEnterDispatch();
    void omniboxTabScope();
    void tabGroups();
    void pinnedTabs();
    void iconOnlyTabStrip();
    void reopenClosedTab();
    void engineSwapStripState();
    void sleepingTabs();
    void memoryPurge();
    void freeTabMemory();
    void containerTabs();
    void tabSlotDrive();
    void newTabStartPage();
    void lazySessionRestore();
    void renameTab();
    void tabTransfer();
    void newTabButton();
    void verticalTabRowTextBounds();
    void verticalRailRowOverlap();
    void tabButtonBandAnchor();
};

// MEM08: does any captured emission carry needle — the statusbar
// report assertions keep re-scanning a growing spy.
static bool spyHasText(const QSignalSpy &spy, const QString &needle)
{
    for (const QList<QVariant> &args : spy) {
        if (args.value(0).toString().contains(needle))
            return true;
    }
    return false;
}

// Subclass that exposes the protected functions.
class SubTabWidget : public TabWidget
{
public:
    void call_linkHovered(QString const &link)
        { return SubTabWidget::linkHovered(link); }

    void call_loadProgress(int progress)
        { return SubTabWidget::loadProgress(progress); }

    void call_setCurrentTitle(QString const &url)
        { return SubTabWidget::setCurrentTitle(url); }

    void call_showStatusBarMessage(QString const &message)
        { return SubTabWidget::showStatusBarMessage(message); }

    void call_tabsChanged()
        { return SubTabWidget::tabsChanged(); }

    // tabBar() is protected; tests drive moves through the real bar so
    // the tabMoved -> moveTab -> group normalization path runs.
    TabBar *bar() { return static_cast<TabBar*>(tabBar()); }
};

// This will be called before the first test function is executed.
// It is only called once.
void tst_TabWidget::initTestCase()
{
}

// This will be called after the last test function is executed.
// It is only called once.
void tst_TabWidget::cleanupTestCase()
{
}

// This will be called before each test function is executed.
void tst_TabWidget::init()
{
}

// This will be called after every test function.
void tst_TabWidget::cleanup()
{
    // A mid-test QVERIFY exit would otherwise leak the lazy-restore
    // keys into the shared store and turn every later restoreState()
    // lazy (saveState's restored urls then never load).
    QSettings settings;
    settings.remove(QLatin1String("performance/lazyRestoreTabs"));
    settings.remove(QLatin1String("performance/alwaysLoadPinnedTabs"));
    // Same guard for the icon-only mode (TABS09) — a leaked key would
    // pin every later test's strip to the icon tile width.
    settings.remove(QLatin1String("tabs/iconOnlyTabs"));
    // TABS13: same guard for the process-private flag — a mid-test
    // exit would leave every later transfer refused.
    BrowserApplication::setPrivate(false);
}

void tst_TabWidget::tabwidget_data()
{
}

void tst_TabWidget::tabwidget()
{
    SubTabWidget widget;
    widget.addWebAction((QAction*)0, Engine::StandardAction::Back);
    widget.closeTab();
    QVERIFY(widget.closeTabAction());
    widget.currentWebView();
    widget.locationBarStack();
    widget.loadUrl(QUrl());
    widget.newTab();
    QVERIFY(widget.newTabAction());
    widget.nextTab();
    QVERIFY(widget.nextTabAction());
    widget.previousTab();
    QVERIFY(widget.previousTabAction());
    QVERIFY(widget.recentlyClosedTabsAction());
    QVERIFY(widget.currentLocationBar());
}

void tst_TabWidget::addWebAction_data()
{
    QTest::addColumn<Engine::StandardAction>("webAction");
    QTest::newRow("back") << Engine::StandardAction::Back;
}

// public void addWebAction(QAction *action, Engine::StandardAction webAction)
void tst_TabWidget::addWebAction()
{
    QFETCH(Engine::StandardAction, webAction);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    QAction *action = new QAction(&widget);
    widget.addWebAction(action, webAction);

    widget.newTab();
    QVERIFY(!action->isEnabled());

    // WebEngine navigations are asynchronous; wait for each load to
    // commit before checking the mapped Back action.
    QWebEnginePage *page = widget.currentWebView()->page();
    QSignalSpy loadSpy(page, &QWebEnginePage::loadFinished);
    widget.loadUrl(QUrl("data:text/plain,one"), TabWidget::CurrentTab);
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 15000);
    widget.loadUrl(QUrl("data:text/plain,two"), TabWidget::CurrentTab);
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 2, 15000);

    QTRY_VERIFY(action->isEnabled());
    widget.newTab();
    QVERIFY(!action->isEnabled());

    // WebEngine clears the hovered link on navigation commits, which
    // emits linkHovered("") — only non-empty hovers are meaningful.
    for (int i = 0; i < spy0.count(); ++i)
        QVERIFY(spy0.at(i).at(0).toString().isEmpty());
    QVERIFY(spy3.count() > 0);
    QCOMPARE(spy6.count(), 0);
    QCOMPARE(widget.webView(0)->enginePage()->historyCount(), 2);
}

void tst_TabWidget::closeTab_data()
{
    QTest::addColumn<int>("index");
    QTest::newRow("null") << 0;
}

// public void closeTab(int index = -1)
void tst_TabWidget::closeTab()
{
    QFETCH(int, index);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.closeTab(index);
    widget.newTab();
    widget.newTab();
    widget.loadUrl(QUrl("data:text/plain,closeTab"), TabWidget::CurrentTab);
    widget.newTab();
    return;

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 4);
    QCOMPARE(spy3.count(), 2);
    QCOMPARE(spy4.count(), 4);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
}

void tst_TabWidget::currentLocationBar_data()
{
    /*
    QTest::addColumn<QLineEdit*>("currentLocationBar");
    QTest::newRow("null") << QLineEdit*();
    */
}

// public QLineEdit *currentLocationBar() const
void tst_TabWidget::currentLocationBar()
{
    /*
    QFETCH(QLineEdit*, currentLocationBar);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    QCOMPARE(widget.currentLocationBar(), currentLocationBar);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

Q_DECLARE_METATYPE(WebView*)
void tst_TabWidget::currentWebView_data()
{
    /*
    QTest::addColumn<WebView*>("currentWebView");
    QTest::newRow("null") << WebView*();
    */
}

// public WebView *currentWebView() const
void tst_TabWidget::currentWebView()
{
    /*
    QFETCH(WebView*, currentWebView);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    QCOMPARE(widget.currentWebView(), currentWebView);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

Q_DECLARE_METATYPE(QWidget*)
void tst_TabWidget::locationBarStack_data()
{
    /*
    QTest::addColumn<QWidget*>("locationBarStack");
    QTest::newRow("null") << QWidget*();
    */
}

// public QWidget *locationBarStack() const
void tst_TabWidget::locationBarStack()
{
    /*
    QFETCH(QWidget*, locationBarStack);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    QCOMPARE(widget.locationBarStack(), locationBarStack);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::loadUrl_data()
{
    QTest::addColumn<QUrl>("url");
    QTest::newRow("null") << QUrl();
}

// public void loadUrl(QUrl const &url)
void tst_TabWidget::loadUrl()
{
    /*
    QFETCH(QUrl, url);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.loadUrl(url);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::newTab_data()
{
    QTest::addColumn<int>("foo");
    QTest::newRow("null") << 0;
}

// public void newTab()
void tst_TabWidget::newTab()
{
    /*
    QFETCH(int, foo);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.newTab();

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::nextTab_data()
{
    QTest::addColumn<int>("foo");
    QTest::newRow("null") << 0;
}

// public void nextTab()
void tst_TabWidget::nextTab()
{
    /*
    QFETCH(int, foo);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.nextTab();

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::previousTab_data()
{
    QTest::addColumn<int>("foo");
    QTest::newRow("null") << 0;
}

// public void previousTab()
void tst_TabWidget::previousTab()
{
    /*
    QFETCH(int, foo);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.previousTab();

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::recentlyClosedTabsAction_data()
{
    /*
    QTest::addColumn<QAction*>("recentlyClosedTabsAction");
    QTest::newRow("null") << QAction*();
    */
}

// public QAction *recentlyClosedTabsAction() const
void tst_TabWidget::recentlyClosedTabsAction()
{
    /*
    QFETCH(QAction*, recentlyClosedTabsAction);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    QCOMPARE(widget.recentlyClosedTabsAction(), recentlyClosedTabsAction);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::linkHovered_data()
{
    QTest::addColumn<QString>("link");
    QTest::newRow("null") << QString("foo");
}

// protected void linkHovered(QString const &link)
void tst_TabWidget::linkHovered(const QString &)
{
    /*
    QFETCH(QString, link);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.call_linkHovered(link);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::loadProgress_data()
{
    QTest::addColumn<int>("progress");
    QTest::newRow("null") << 0;
}

// protected void loadProgress(int progress)
void tst_TabWidget::loadProgress(int)
{
    /*
    QFETCH(int, progress);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.call_loadProgress(progress);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::setCurrentTitle_data()
{
    QTest::addColumn<QString>("url");
    QTest::newRow("null") << QString("foo");
}

// protected void setCurrentTitle(QString const &url)
void tst_TabWidget::setCurrentTitle(const QString &)
{
    /*
    QFETCH(QString, url);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.call_setCurrentTitle(url);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::showStatusBarMessage_data()
{
    QTest::addColumn<QString>("message");
    QTest::newRow("null") << QString("foo");
}

// protected void showStatusBarMessage(QString const &message)
void tst_TabWidget::showStatusBarMessage(const QString &)
{
    /*
    QFETCH(QString, message);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.call_showStatusBarMessage(message);

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::tabsChanged_data()
{
    QTest::addColumn<int>("foo");
    QTest::newRow("null") << 0;
}

// protected void tabsChanged()
void tst_TabWidget::tabsChanged()
{
    /*
    QFETCH(int, foo);

    SubTabWidget widget;

    QSignalSpy spy0(&widget, SIGNAL(linkHovered(const QString &)));
    QSignalSpy spy2(&widget, SIGNAL(loadProgress(int)));
    QSignalSpy spy3(&widget, SIGNAL(setCurrentTitle(const QString &)));
    QSignalSpy spy4(&widget, SIGNAL(showStatusBarMessage(const QString &)));
    QSignalSpy spy5(&widget, SIGNAL(tabsChanged()));
    QSignalSpy spy6(&widget, SIGNAL(lastTabClosed()));

    widget.call_tabsChanged();

    QCOMPARE(spy0.count(), 0);
    QCOMPARE(spy2.count(), 0);
    QCOMPARE(spy3.count(), 0);
    QCOMPARE(spy4.count(), 0);
    QCOMPARE(spy5.count(), 0);
    QCOMPARE(spy6.count(), 0);
    */
    QSKIP("Test is not implemented.", SkipAll);
}

void tst_TabWidget::saveState()
{
    SubTabWidget widget;
    widget.newTab();
    QCOMPARE(widget.count(), 1);

    QUrl url = QUrl("data:text/html;base32,Hello%20World");
    widget.loadUrl(url, TabWidget::CurrentTab);
    QCOMPARE(widget.count(), 1);
    // The url lands on the view asynchronously under WebEngine.
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), url, 15000);

    widget.loadUrl(url, TabWidget::NewTab);
    QCOMPARE(widget.count(), 2);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), url, 15000);

    QByteArray state = widget.saveState();

    widget.closeTab();
    QCOMPARE(widget.count(), 1);
    widget.closeTab();
    QCOMPARE(widget.count(), 0);

    widget.newTab();
    widget.restoreState(state);
    QCOMPARE(widget.count(), 2);
    QVERIFY(widget.webView(0));
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), url, 15000);
    QVERIFY(widget.webView(1));
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), url, 15000);

    widget.closeTab();
    widget.closeTab();
}


// SEC04: the serialized session blob is unauthenticated — a bogus
// element count must fail fast rather than being trusted by the
// container deserializer (a 0x7fffffff count used to force a giant
// allocation).
void tst_TabWidget::restoreStateCorrupt()
{
    SubTabWidget widget;
    widget.newTab();
    const int before = widget.count();

    {
        QByteArray blob;
        QDataStream out(&blob, QIODevice::WriteOnly);
        out << qint32(0xaa) << qint32(1) << qint32(0x7fffffff);
        QVERIFY(!widget.restoreState(blob));
        QCOMPARE(widget.count(), before);
    }

    // Truncation inside the tab list is likewise rejected.
    {
        QByteArray blob;
        QDataStream out(&blob, QIODevice::WriteOnly);
        out << qint32(0xaa) << qint32(1)
            << (QStringList() << QLatin1String("http://a/")
                              << QLatin1String("http://b/"));
        blob.chop(3);
        QVERIFY(!widget.restoreState(blob));
        QCOMPARE(widget.count(), before);
    }

    widget.closeTab();
}

// SEC09: argv/IPC urls are untrusted input — a javascript: payload
// must be refused even when it only parses to javascript: after
// normalization, while ordinary urls still resolve and load.
void tst_TabWidget::loadStringFromUntrustedSource()
{
    QVERIFY(WebView::isUrlAllowedOnUntrustedInput(
        QUrl(QLatin1String("https://example.com/"))));
    QVERIFY(WebView::isUrlAllowedOnUntrustedInput(
        QUrl(QLatin1String("data:text/plain,x"))));
    QVERIFY(!WebView::isUrlAllowedOnUntrustedInput(
        QUrl(QLatin1String("javascript:alert(1)"))));
    // QUrl normalizes the scheme to lowercase — no case bypass.
    QVERIFY(!WebView::isUrlAllowedOnUntrustedInput(
        QUrl(QLatin1String("JAVASCRIPT:alert(1)"))));

    SubTabWidget widget;
    widget.newTab();
    WebView *view = widget.currentWebView();
    QVERIFY(view);

    // A real page first so an injected script would have a document to
    // run against — a changed title is the observable side effect.
    widget.loadStringFromUntrustedSource(
        QLatin1String("data:text/html,<title>safe</title>"));
    QTRY_VERIFY_WITH_TIMEOUT(
        view->title() == QLatin1String("safe"), 15000);

    widget.loadStringFromUntrustedSource(
        QLatin1String("javascript:document.title='PWNED'"));
    QTest::qWait(500);
    QCOMPARE(view->title(), QLatin1String("safe"));

    // A payload padded with whitespace would slip through a raw
    // string-scheme check — it is refused after resolution too.
    widget.loadStringFromUntrustedSource(
        QLatin1String("  javascript:document.title='PWNED'"));
    QTest::qWait(500);
    QCOMPARE(view->title(), QLatin1String("safe"));

    // Ordinary untrusted urls still load.
    widget.loadStringFromUntrustedSource(
        QLatin1String("data:text/plain,ok"));
    QTRY_VERIFY_WITH_TIMEOUT(
        view->url() == QUrl(QLatin1String("data:text/plain,ok")), 15000);

    widget.closeTab();
}

// TABS01: the tabs/tabBarPosition QSettings value maps to the
// QTabWidget position (Top/Bottom/Left/Right -> North/South/West/
// East), applies on construction and on live loadSettings() (the call
// SettingsDialog::saveToSettings makes on every open window), and
// keeps movable tabs/context-menu wiring intact in vertical mode.
void tst_TabWidget::tabBarPositionSetting()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));

    settings.remove(QLatin1String("tabBarPosition"));
    {
        SubTabWidget widget;
        QCOMPARE(int(widget.tabPosition()), int(QTabWidget::North));
    }

    settings.setValue(QLatin1String("tabBarPosition"), 2);
    {
        SubTabWidget widget;
        QCOMPARE(int(widget.tabPosition()), int(QTabWidget::West));
        QVERIFY(widget.tabBar()->isMovable());

        // Live re-apply, and an out-of-range value falls back to Top.
        settings.setValue(QLatin1String("tabBarPosition"), 99);
        widget.loadSettings();
        QCOMPARE(int(widget.tabPosition()), int(QTabWidget::North));

        settings.setValue(QLatin1String("tabBarPosition"), 3);
        widget.loadSettings();
        QCOMPARE(int(widget.tabPosition()), int(QTabWidget::East));

        settings.setValue(QLatin1String("tabBarPosition"), 1);
        widget.loadSettings();
        QCOMPARE(int(widget.tabPosition()), int(QTabWidget::South));
    }

    settings.remove(QLatin1String("tabBarPosition"));
    settings.endGroup();
}

// TABS02: tabs/verticalTabWidth drives the Left/Right strip's actual
// width inside a real QTabWidget — read on construction, re-applied
// live through loadSettings() (the path SettingsDialog::saveToSettings
// calls on every window), and reflected in the tab rects.
void tst_TabWidget::verticalTabStripWidth()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("tabBarPosition"), 2);   // Left
    settings.setValue(QLatin1String("verticalTabWidth"), 200);

    SubTabWidget widget;
    widget.newTab();
    widget.newTab();
    widget.resize(640, 480);
    widget.show();
    QApplication::processEvents();

    TabBar *bar = widget.bar();
    QCOMPARE(bar->verticalTabWidth(), 200);
    // The strip's widget width follows the persisted value (the
    // QTabWidget layout adds a small frame margin around it).
    QVERIFY(bar->width() >= 200 && bar->width() <= 210);
    QCOMPARE(bar->tabRect(0).width(), 200);

    // Live resize — what the edge-drag does per move.
    bar->setVerticalTabWidth(300);
    QApplication::processEvents();   // LayoutRequest -> setUpLayout
    QVERIFY(bar->width() >= 300 && bar->width() <= 310);
    QCOMPARE(bar->tabRect(0).width(), 300);

    // loadSettings() (the settings-save fan-out) re-reads the key.
    settings.setValue(QLatin1String("verticalTabWidth"), 160);
    widget.loadSettings();
    QApplication::processEvents();
    QCOMPARE(bar->verticalTabWidth(), 160);
    QVERIFY(bar->width() >= 160 && bar->width() <= 175);

    // The Right strip picks the same persisted width.
    settings.setValue(QLatin1String("tabBarPosition"), 3);   // Right
    widget.loadSettings();
    QApplication::processEvents();
    QCOMPARE(int(widget.tabPosition()), int(QTabWidget::East));
    QVERIFY(bar->width() >= 160 && bar->width() <= 175);

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.remove(QLatin1String("tabBarPosition"));
    settings.endGroup();
}

// TABS16: a fresh profile gets the roomier 180px Left/Right rail —
// TABS10's compact 140 clipped typical titles at ~10-12 chars — and
// a stored past default migrates forward through the real widget
// (it means the grip was never dragged to a pick); a user-dragged
// width survives the same construction path untouched.
void tst_TabWidget::verticalTabDefaultWidth()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("tabBarPosition"), 2);   // Left

    settings.remove(QLatin1String("verticalTabWidth"));
    {
        SubTabWidget widget;
        widget.newTab();
        widget.resize(640, 480);
        widget.show();
        QApplication::processEvents();
        TabBar *bar = widget.bar();
        QCOMPARE(bar->verticalTabWidth(), 180);
        QVERIFY(bar->width() >= 180 && bar->width() <= 190);
        QCOMPARE(bar->tabRect(0).width(), 180);
        // A plain row is the uniform TABS19 face — content + 17,
        // the same ~33px pad the horizontal strip uses.
        QCOMPARE(bar->tabRect(0).height(),
                 qMax(bar->fontMetrics().height(),
                      bar->iconSize().height()) + 17);
    }

    // Stored past defaults migrate — TABS10's compact 140, and the
    // pre-TABS10 180 (already the new default) both mean the grip
    // was never dragged to a pick.
    settings.setValue(QLatin1String("verticalTabWidth"), 140);
    {
        SubTabWidget widget;
        QCOMPARE(widget.bar()->verticalTabWidth(), 180);
    }
    settings.setValue(QLatin1String("verticalTabWidth"), 180);
    {
        SubTabWidget widget;
        QCOMPARE(widget.bar()->verticalTabWidth(), 180);
    }

    // A user-set width stays.
    settings.setValue(QLatin1String("verticalTabWidth"), 220);
    {
        SubTabWidget widget;
        QCOMPARE(widget.bar()->verticalTabWidth(), 220);
    }

    settings.remove(QLatin1String("verticalTabWidth"));
    settings.remove(QLatin1String("tabBarPosition"));
    settings.endGroup();
}

// TPREV01: tabs/tabHoverPreviews (default on) gates the hover
// preview card — read on construction, re-applied live through
// loadSettings() (the path SettingsDialog::saveToSettings fans out
// to every window), and opting out while a card is up drops it.
void tst_TabWidget::tabHoverPreviews()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("tabHoverPreviews"), false);

    SubTabWidget widget;
    widget.newTab();
    widget.newTab();
    widget.resize(640, 480);
    widget.show();
    QApplication::processEvents();

    TabBar *bar = widget.bar();
    QVERIFY(!bar->tabPreviewsEnabled());

    // Synthetic QTest::mouseMove does not reach widgets under the
    // offscreen QPA — post the move through sendEvent like the
    // tst_tabbar legs drive their handlers.
    const auto hoverTab = [&bar](int index) {
        const QPoint pos = bar->tabRect(index).center();
        QMouseEvent move(QEvent::MouseMove, QPointF(pos),
                         QPointF(bar->mapToGlobal(pos)),
                         Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(bar, &move);
    };

    // Opted out: dwelling on a tab never arms the card.
    hoverTab(1);
    QTest::qWait(700);
    QVERIFY(!bar->tabPreview());

    // Opted back in through the live-apply path the settings dialog
    // uses -> the dwell pops the card on the hovered tab.
    settings.setValue(QLatin1String("tabHoverPreviews"), true);
    widget.loadSettings();
    QVERIFY(bar->tabPreviewsEnabled());
    hoverTab(0);
    QTest::qWait(700);
    QVERIFY(bar->tabPreview());
    QVERIFY(bar->tabPreview()->isVisible());
    QCOMPARE(bar->tabPreview()->previewedIndex(), 0);

    // Opting out while a card is up drops it immediately, and a
    // fresh dwell on another tab never re-arms it.
    settings.setValue(QLatin1String("tabHoverPreviews"), false);
    widget.loadSettings();
    QVERIFY(!bar->tabPreviewsEnabled());
    QVERIFY(!bar->tabPreview()->isVisible());
    hoverTab(1);
    QTest::qWait(700);
    QVERIFY(!bar->tabPreview()->isVisible());

    settings.remove(QLatin1String("tabHoverPreviews"));
    settings.endGroup();
}

// TPREV02: the hover thumbnail must show the tab's OWN current page.
// The current tab re-grabs live; background tabs are served only the
// identity-checked snapshot cached while they last painted (a hidden
// view's compositor frame can be stale or a recycled context's
// pixels — a request-time grab is never trusted); a full navigation
// drops the entry at loadStarted, and a same-document hash change
// disqualifies it on the recorded url.  A miss yields null so the
// preview falls back to the title+url card — never another page's
// pixels.
void tst_TabWidget::tabThumbnailStaleness()
{
    const QString fixturePath = QDir::temp().filePath(
        QLatin1String("arora-tprev02-fixture.html"));
    {
        QFile fixture(fixturePath);
        QVERIFY(fixture.open(QIODevice::WriteOnly));
        fixture.write("<html><head><title>tprev02</title></head>"
                      "<body style='margin:0;background:#c33'>zero</body>"
                      "</html>");
    }
    const QUrl page0 = QUrl::fromLocalFile(fixturePath);
    // Named colours only — a '#'-style colour inside a data: url is a
    // fragment delimiter and would truncate the document.
    const QUrl page1(QStringLiteral(
        "data:text/html,<body style='background:blue;margin:0'>one</body>"));

    SubTabWidget widget;
    widget.newTab();
    widget.newTab();
    widget.resize(640, 480);
    widget.show();
    QApplication::processEvents();

    widget.webView(0)->loadUrl(page0);
    widget.webView(1)->loadUrl(page1);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), page0, 15000);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), page1, 15000);

    // The current tab paints and caches its live frame.
    widget.setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(!widget.tabThumbnail(0).isNull(), 15000);

    // Hidden tab: the identity-matching snapshot is served — this is
    // the only path a background tab may take.
    widget.setCurrentIndex(1);
    QApplication::processEvents();
    QVERIFY(!widget.tabThumbnail(0).isNull());

    // A same-document hash navigation changes the url without any
    // loadStarted — the stored frame can no longer be proven current
    // and must be rejected on identity.
    widget.webView(0)->enginePage()->runJavaScript(
        QStringLiteral("location.hash = '#moved'"));
    QTRY_VERIFY_WITH_TIMEOUT(
        widget.webView(0)->url().fragment()
            == QLatin1String("moved"),
        15000);
    QVERIFY(widget.tabThumbnail(0).isNull());

    // A full background navigation likewise leaves nothing servable:
    // no request-time grab fills in, and loadFinished does not
    // re-cache a tab that never painted (negative control on the
    // stale-frame path — under the old grab-on-request code a
    // painted background frame would have surfaced here).
    const QUrl page0b(QStringLiteral(
        "data:text/html,<body style='background:green;margin:0'>zero-b</body>"));
    QSignalSpy loadedSpy(widget.webView(0)->page(),
                         &QWebEnginePage::loadFinished);
    widget.webView(0)->loadUrl(page0b);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), page0b, 15000);
    QTRY_VERIFY_WITH_TIMEOUT(loadedSpy.count() >= 1, 15000);
    QTest::qWait(400);   // past the 300ms deferred-capture window
    QVERIFY(widget.tabThumbnail(0).isNull());

    // Back on top, the same tab re-grabs live again.
    widget.setCurrentIndex(0);
    QTRY_VERIFY_WITH_TIMEOUT(!widget.tabThumbnail(0).isNull(), 15000);

    // A closed tab's snapshot dies with it — closing the cached tab
    // mid-test must not leave anything servable behind (the entries
    // are removed on both view-death paths, closeTab here).
    widget.closeTab(0);
    QCOMPARE(widget.count(), 1);
    QTest::qWait(50);   // the dying view's deferred deleteLater lands
    widget.closeTab();
    QFile::remove(fixturePath);
}

// SRCH01: the location bar is an omnibox — guessUrlFromString (driven
// through loadString) must search for text that is not address-shaped
// instead of guessing http://, while address-shaped input still
// navigates.  'keyword terms' shortcuts keep first priority and the
// searchInAddressField opt-out restores the historic bare-http guess.
void tst_TabWidget::omnibox()
{
    QSettings settings;
    // SRCH09: this key used to leak — the test disabled it below and
    // never restored it, which broke search for the real profile when
    // the test ran against the live config.  SRCH12 renamed it to
    // searchInAddressField for exactly that reason.
    const QVariant savedSearchFallback =
        settings.value(QLatin1String("urlloading/searchInAddressField"));
    settings.setValue(QLatin1String("urlloading/searchInAddressField"), true);

    // PRIV01/SAFE01: the profile interceptor upgrades http:
    // navigations to https: and HTTPS-Only swaps surviving http:
    // targets for the warning interstitial — this test asserts the
    // URL the address bar resolved to, not the transport, so both
    // are switched off for it.
    const QVariant savedHttpsFirst =
        settings.value(QLatin1String("privacy/httpsFirst"));
    const QVariant savedHttpsOnly =
        settings.value(QLatin1String("privacy/httpsOnly"));
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = new OpenSearchEngine;
    engine->setName(QLatin1String("omnibox-test"));
    engine->setSearchUrlTemplate(
        QLatin1String("http://omnibox-test.invalid/s?q={searchTerms}"));
    // A previous interrupted run may have persisted this engine.
    if (manager->engineExists(engine->name()))
        manager->removeEngine(engine->name());
    QVERIFY(manager->addEngine(engine));
    const QString previousEngine = manager->currentEngineName();
    manager->setCurrentEngineName(engine->name());
    manager->setEngineForKeyword(QLatin1String("ot"), engine);

    SubTabWidget widget;
    widget.newTab();
    WebView *view = widget.currentWebView();
    QVERIFY(view);

    // Chromium may normalize a host-only url to "host/" before
    // urlChanged fires — compare with the trailing slash stripped.
    const auto check = [&widget, view](const QString &input,
                                       const QUrl &expected) {
        QSignalSpy spy(view, &QWebEngineView::urlChanged);
        widget.loadString(input);
        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 10000);
        QCOMPARE(spy.last().first().toUrl().toString(QUrl::StripTrailingSlash),
                 expected.toString(QUrl::StripTrailingSlash));
    };

    // Search terms: multi-word input and bare single words both hit
    // the default engine now — 'word' used to navigate to http://word.
    check(QLatin1String("browser test"), engine->searchUrl(QLatin1String("browser test")));
    check(QLatin1String("word"), engine->searchUrl(QLatin1String("word")));
    check(QLatin1String("a phrase with spaces"),
          engine->searchUrl(QLatin1String("a phrase with spaces")));

    // Address-shaped input still navigates.  (Chromium normalizes
    // host-only urls to "host/" before urlChanged fires.)
    check(QLatin1String("docs.qt.io"), QUrl(QLatin1String("http://docs.qt.io/")));
    check(QLatin1String("a.b"), QUrl(QLatin1String("http://a.b/")));
    check(QLatin1String("localhost"), QUrl(QLatin1String("http://localhost/")));
    check(QLatin1String("localhost:8080"),
          QUrl(QLatin1String("http://localhost:8080/")));
    check(QLatin1String("127.0.0.1"), QUrl(QLatin1String("http://127.0.0.1/")));

    // Explicit schemes load as typed — WebEngine can't navigate ftp,
    // so file: stands in for a non-http explicit scheme.
    check(QLatin1String("http://example.com/x"),
          QUrl(QLatin1String("http://example.com/x")));
    check(QLatin1String("file:///etc/hostname"),
          QUrl(QLatin1String("file:///etc/hostname")));
    check(QLatin1String("about:home"),
          QUrl(QLatin1String("qrc:/startpage.html")));

    // 'keyword terms' wins over everything, even address-shaped terms.
    check(QLatin1String("ot hello"), engine->searchUrl(QLatin1String("hello")));

    // SRCH07/SRCH11: a configured engine that produces no usable
    // search url must not dead-end the bar — the shared rescue
    // degrades to the bundled DuckDuckGo endpoint instead of emitting
    // a bogus http://<term> navigation.
    const QString savedTemplate = engine->searchUrlTemplate();
    engine->setSearchUrlTemplate(QString());
    check(QLatin1String("word"),
          QUrl(QLatin1String("https://duckduckgo.com/?q=word")));
    QUrl lastResort(QLatin1String("https://duckduckgo.com/"));
    QUrlQuery rescueQuery;
    rescueQuery.addQueryItem(QLatin1String("q"),
                             QLatin1String("two words"));
    lastResort.setQuery(rescueQuery);
    check(QLatin1String("two words"), lastResort);
    engine->setSearchUrlTemplate(savedTemplate);

    // Opt-out restores the old bare-http guess for non-address input.
    settings.setValue(QLatin1String("urlloading/searchInAddressField"), false);
    check(QLatin1String("word"), QUrl(QLatin1String("http://word/")));

    // SRCH09/SRCH11: under the opt-out a guess that cannot parse
    // still searches — "http://two words" used to reach loadUrl() as
    // an invalid url and die silently (dead Enter).
    check(QLatin1String("two words"),
          engine->searchUrl(QLatin1String("two words")));
    settings.setValue(QLatin1String("urlloading/searchInAddressField"), true);

    // SRCH12: a stored legacy "searchEngineFallback" is untrusted
    // residue — it must not gate search, and the resolver drops the
    // key on sight.
    settings.setValue(QLatin1String("urlloading/searchEngineFallback"), false);
    check(QLatin1String("word"), engine->searchUrl(QLatin1String("word")));
    QVERIFY(!settings.contains(QLatin1String("urlloading/searchEngineFallback")));

    if (savedSearchFallback.isValid())
        settings.setValue(
            QLatin1String("urlloading/searchInAddressField"),
            savedSearchFallback);
    else
        settings.remove(QLatin1String("urlloading/searchInAddressField"));

    manager->setEngineForKeyword(QLatin1String("ot"), nullptr);
    manager->setCurrentEngineName(previousEngine);
    manager->removeEngine(engine->name());
    widget.closeTab();

    if (savedHttpsFirst.isValid())
        settings.setValue(QLatin1String("privacy/httpsFirst"), savedHttpsFirst);
    else
        settings.remove(QLatin1String("privacy/httpsFirst"));
    if (savedHttpsOnly.isValid())
        settings.setValue(QLatin1String("privacy/httpsOnly"), savedHttpsOnly);
    else
        settings.remove(QLatin1String("privacy/httpsOnly"));
    PrivacyRequestInterceptor::loadSettings();
}

// SRCH11: the omnibox contract through the real dispatch — Enter in
// the location bar (with the completer popup up, the real-world shape
// of the report) searches the typed text with the default engine, and
// activating a "Search %engine" suggestion row searches that text.
void tst_TabWidget::omniboxEnterDispatch()
{
    QSettings settings;
    settings.setValue(QLatin1String("urlloading/searchInAddressField"), true);
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
    settings.setValue(QLatin1String("privacy/httpsOnly"), false);
    PrivacyRequestInterceptor::loadSettings();

    OpenSearchManager *manager = ToolbarSearch::openSearchManager();
    OpenSearchEngine *engine = new OpenSearchEngine;
    engine->setName(QLatin1String("enter-repro"));
    engine->setSearchUrlTemplate(
        QLatin1String("http://enter-repro.invalid/s?q={searchTerms}"));
    if (manager->engineExists(engine->name()))
        manager->removeEngine(engine->name());
    QVERIFY(manager->addEngine(engine));
    const QString previousEngine = manager->currentEngineName();
    manager->setCurrentEngineName(engine->name());

    SubTabWidget widget;
    widget.newTab();
    WebView *view = widget.currentWebView();
    QVERIFY(view);
    widget.resize(800, 600);
    widget.show();
    QVERIFY(QTest::qWaitForWindowExposed(&widget));

    QLineEdit *bar = widget.currentLocationBar();
    QVERIFY(bar);
    bar->setFocus();
    QTest::qWait(50);

    const QUrl expected = engine->searchUrl(QLatin1String("test query"));
    QTest::keyClicks(bar, QLatin1String("test query"));
    // Let the 150ms completer debounce fire so the popup is up — the
    // real-world shape of the report.
    QTest::qWait(400);
    QCompleter *completer = bar->completer();
    QVERIFY(completer);
    // Force the real-world shape: suggestion + rows visible in the
    // dropdown (the test profile's history is empty).
    OmniboxCompletionModel *omni =
        qobject_cast<OmniboxCompletionModel*>(completer->model());
    QVERIFY(omni);
    omni->setSuggestions(QStringList()
        << QLatin1String("test query suggestion"));
    completer->complete();
    QTest::qWait(100);
    QTest::keyClick(bar, Qt::Key_Enter);
    QTRY_VERIFY_WITH_TIMEOUT(
        view->url().toString(QUrl::StripTrailingSlash)
            == expected.toString(QUrl::StripTrailingSlash), 10000);

    // Picking a suggestion row searches its text — the UrlStringRole
    // payload routes back through loadString/guessUrlFromString.
    QModelIndex suggestion;
    QAbstractItemModel *proxy = completer->completionModel();
    for (int row = 0; row < proxy->rowCount(); ++row) {
        const QModelIndex at = proxy->index(row, 0);
        if (at.data(Qt::DisplayRole).toString()
                == QLatin1String("test query suggestion")) {
            suggestion = at;
            break;
        }
    }
    QVERIFY(suggestion.isValid());
    emit completer->activated(suggestion);
    const QUrl suggestionExpected = engine->searchUrl(
        QLatin1String("test query suggestion"));
    QTRY_VERIFY_WITH_TIMEOUT(
        view->url().toString(QUrl::StripTrailingSlash)
            == suggestionExpected.toString(QUrl::StripTrailingSlash),
        10000);

    manager->setCurrentEngineName(previousEngine);
    manager->removeEngine(engine->name());
    widget.closeTab();
    settings.remove(QLatin1String("urlloading/searchInAddressField"));
}

// SRCH06: typing the "@tabs" nickname lists this widget's open tabs
// in the completion model, and activating a row switches to that tab
// instead of navigating to its url.
void tst_TabWidget::omniboxTabScope()
{
    ScopeShortcuts::reset();
    SubTabWidget widget;
    widget.newTab();
    widget.newTab();
    QCOMPARE(widget.count(), 2);
    QCOMPARE(widget.currentIndex(), 1);
    widget.setTabText(0, QLatin1String("Scoped Tab One"));
    widget.setTabText(1, QLatin1String("Scoped Tab Two"));

    QLineEdit *bar = widget.currentLocationBar();
    QVERIFY(bar);
    QCompleter *completer = bar->completer();
    QVERIFY(completer);
    OmniboxCompletionModel *model =
        qobject_cast<OmniboxCompletionModel*>(completer->model());
    QVERIFY(model);

    // Drive the completer the way typing does — keyClicks fires
    // textEdited, the filter timer calls setSearchText.
    widget.show();
    bar->setFocus();
    QTest::keyClicks(bar, QLatin1String("@tabs "));
    QTRY_VERIFY_WITH_TIMEOUT(
        model->scope() == ScopeShortcuts::TabsScope, 3000);
    QCOMPARE(model->rowCount(), 2);

    // Rows carry the tab index and the tab's own title/url.
    const QModelIndex first = model->index(0, 0);
    QCOMPARE(first.data(OmniboxCompletionModel::TabIndexRole).toInt(), 0);
    QCOMPARE(first.data(HistoryModel::UrlStringRole).toString(),
             QString::fromUtf8(widget.webView(0)->url().toEncoded()));
    QCOMPARE(model->index(0, 1).data().toString(),
             QLatin1String("Scoped Tab One"));

    // Emitting activated() the way Qt does — with an index from the
    // completer's completion proxy — switches to the tab.
    completer->complete();
    QAbstractItemModel *proxy = completer->completionModel();
    QVERIFY(proxy);
    QCOMPARE(proxy->rowCount(), 2);
    emit completer->activated(proxy->index(0, 0));
    QCOMPARE(widget.currentIndex(), 0);

    // A direct source-model index (the handler's other accepted form)
    // switches back to the second tab.
    emit completer->activated(model->index(1, 0));
    QCOMPARE(widget.currentIndex(), 1);

    // Disabled — the token is ordinary text again, no tab rows.
    ScopeShortcuts::setEnabled(ScopeShortcuts::TabsScope, false);
    QTest::keyClicks(bar, QLatin1String("x"));
    QTRY_VERIFY_WITH_TIMEOUT(
        model->scope() == ScopeShortcuts::NoScope, 3000);
    ScopeShortcuts::setEnabled(ScopeShortcuts::TabsScope, true);

    widget.closeTab();
    widget.closeTab();
}

// TABGRP01: named color-coded tab groups.  Membership is keyed on the
// WebView so it survives drags; collapsing detaches members into the
// group's hidden list (the first stays as the "chip"); the v3 session
// tail carries ids + name + color + collapsed and a v2 blob restores
// ungrouped.
void tst_TabWidget::tabGroups()
{
    SubTabWidget widget;
    for (int i = 0; i < 4; ++i)
        widget.newTab();
    QCOMPARE(widget.count(), 4);
    WebView *v0 = widget.webView(0);
    WebView *v1 = widget.webView(1);
    WebView *v2 = widget.webView(2);
    WebView *v3 = widget.webView(3);
    QVERIFY(v0 && v1 && v2 && v3);

    // create + add — adding a tab far from the run slides it next to
    // the existing member.
    const QString gid = widget.createTabGroup(0);
    QVERIFY(!gid.isEmpty());
    QCOMPARE(widget.tabGroupId(0), gid);
    QCOMPARE(widget.tabGroupSize(gid), 1);
    QCOMPARE(widget.tabGroupIds(), QStringList() << gid);

    widget.addTabToGroup(3, gid);
    // strip: [v0, v3, v1, v2]
    QCOMPARE(widget.webView(1), v3);
    QCOMPARE(widget.tabGroupId(1), gid);
    QCOMPARE(widget.tabGroupSize(gid), 2);

    // rename + color
    widget.renameTabGroup(gid, QStringLiteral("Work"));
    QCOMPARE(widget.tabGroupName(gid), QStringLiteral("Work"));
    widget.setTabGroupColor(gid, QColor(Qt::red));
    QCOMPARE(widget.tabGroupColor(gid), QColor(Qt::red));

    // collapse — only the chip stays on the strip.
    widget.setTabGroupCollapsed(gid, true);
    QVERIFY(widget.tabGroupIsCollapsed(gid));
    QVERIFY(widget.hasCollapsedTabGroup());
    QCOMPARE(widget.count(), 3);
    QCOMPARE(widget.webView(0), v0);
    QVERIFY(widget.isTabGroupChip(0));
    QCOMPARE(widget.tabGroupMembers(gid), QList<int>() << 0);
    QCOMPARE(widget.tabGroupSize(gid), 2);
    QCOMPARE(widget.webView(1), v1);

    // expand restores the members in strip order.
    widget.setTabGroupCollapsed(gid, false);
    QVERIFY(!widget.hasCollapsedTabGroup());
    QCOMPARE(widget.count(), 4);
    QCOMPARE(widget.webView(0), v0);
    QCOMPARE(widget.webView(1), v3);
    QCOMPARE(widget.webView(2), v1);
    QCOMPARE(widget.webView(3), v2);
    QCOMPARE(widget.tabGroupMembers(gid), QList<int>() << 0 << 1);

    // Drag normalization: an ungrouped tab dropped between two members
    // joins; a member dropped with no same-group neighbor leaves.
    widget.bar()->moveTab(2, 1);   // strip [v0, v1, v3, v2]
    QCOMPARE(widget.tabGroupId(1), gid);
    widget.bar()->moveTab(1, 3);   // strip [v0, v3, v2, v1]
    QCOMPARE(widget.tabGroupId(3), QString());
    QCOMPARE(widget.tabGroupSize(gid), 2);

    // Drop-stacking onto a grouped tab joins that group.
    widget.groupTabWith(3, 0);     // v1 -> v0's group, moved into run
    QCOMPARE(widget.tabGroupId(2), gid);
    QCOMPARE(widget.webView(2), v1);

    widget.removeTabFromGroup(2);
    QCOMPARE(widget.tabGroupId(2), QString());
    QCOMPARE(widget.tabGroupSize(gid), 2);

    // Closing a collapsed chip expands the group first so the hidden
    // members are not stranded off-strip; the group lives on.
    widget.setTabGroupCollapsed(gid, true);
    QCOMPARE(widget.count(), 3);
    widget.closeTab(0);
    QCOMPARE(widget.count(), 3);
    QCOMPARE(widget.webView(0), v3);
    QCOMPARE(widget.tabGroupId(0), gid);
    QVERIFY(!widget.tabGroupIsCollapsed(gid));

    // Ungroup leaves the tabs in place.
    widget.ungroupTabs(gid);
    QVERIFY(widget.tabGroupIds().isEmpty());
    QCOMPARE(widget.tabGroupId(0), QString());
    QCOMPARE(widget.count(), 3);

    // Session round-trip: the v3 tail carries group ids + the group
    // table (name/color/collapsed), remapped onto fresh ids.
    {
        SubTabWidget w2;
        for (int i = 0; i < 4; ++i)
            w2.newTab();
        const QString g2 = w2.createTabGroup(0);
        w2.addTabToGroup(1, g2);
        w2.renameTabGroup(g2, QStringLiteral("Persisted"));
        w2.setTabGroupColor(g2, QColor(QLatin1String("magenta")));
        w2.setTabGroupCollapsed(g2, true);
        QCOMPARE(w2.count(), 3);   // chip + two ungrouped tabs

        const QByteArray state = w2.saveState();

        SubTabWidget w3;
        QVERIFY(w3.restoreState(state));
        QCOMPARE(w3.count(), 3);
        const QString rg = w3.tabGroupId(0);
        QVERIFY(!rg.isEmpty());
        QCOMPARE(w3.tabGroupName(rg), QStringLiteral("Persisted"));
        QCOMPARE(w3.tabGroupColor(rg), QColor(QLatin1String("magenta")));
        QVERIFY(w3.tabGroupIsCollapsed(rg));
        QCOMPARE(w3.tabGroupSize(rg), 2);
        QCOMPARE(w3.tabGroupId(1), QString());
        QCOMPARE(w3.tabGroupId(2), QString());

        w3.setTabGroupCollapsed(rg, false);
        QCOMPARE(w3.count(), 4);
        QCOMPARE(w3.tabGroupId(0), rg);
        QCOMPARE(w3.tabGroupId(1), rg);
    }

    // A v2-era session blob (no group tail) restores ungrouped.
    {
        QByteArray v2blob;
        QDataStream out(&v2blob, QIODevice::WriteOnly);
        out << qint32(0xaa) << qint32(2)
            << (QStringList() << QStringLiteral("data:text/plain,v2"))
            << qint32(0)
            << (QList<QByteArray>() << QByteArray())
            << (QStringList() << QString());
        SubTabWidget w4;
        QVERIFY(w4.restoreState(v2blob));
        QCOMPARE(w4.count(), 1);
        QVERIFY(w4.tabGroupIds().isEmpty());
    }
}

// TABS04: pinned tabs — a contiguous leading prefix that keeps its
// titles (TABS14: favicon + title like any tab, never the old
// icon-only tile), survives "Close Other Tabs", ignores middle-click,
// exempts the idle-sleep sweep, converts on a drag across the
// boundary, seats new tabs right after it, and round-trips through
// the v5 session tail.
void tst_TabWidget::pinnedTabs()
{
    SubTabWidget widget;
    for (int i = 0; i < 4; ++i)
        widget.newTab();
    QCOMPARE(widget.count(), 4);
    WebView *v0 = widget.webView(0);
    WebView *v1 = widget.webView(1);
    WebView *v2 = widget.webView(2);
    WebView *v3 = widget.webView(3);
    QVERIFY(v0 && v1 && v2 && v3);

    // Out-of-range queries are safe no-ops.
    QVERIFY(!widget.isTabPinned(-1));
    QVERIFY(!widget.isTabPinned(4));
    QCOMPARE(widget.pinnedTabCount(), 0);

    // Pinning a middle tab slides it to the front of the block.
    widget.setTabPinned(2, true);          // strip [v2, v0, v1, v3]
    QVERIFY(widget.isTabPinned(0));
    QCOMPARE(widget.pinnedTabCount(), 1);
    QCOMPARE(widget.webView(0), v2);
    QCOMPARE(widget.webView(1), v0);
    // A redundant set is a no-op.
    widget.setTabPinned(0, true);
    QCOMPARE(widget.webView(0), v2);

    widget.setTabPinned(3, true);          // strip [v2, v3, v0, v1]
    QCOMPARE(widget.pinnedTabCount(), 2);
    QCOMPARE(widget.webView(1), v3);
    QCOMPARE(widget.webView(2), v0);

    // New tabs insert right after the pinned block, not at the end.
    WebView *fresh = widget.makeNewTab(false);
    QVERIFY(fresh);
    QCOMPARE(widget.webViewIndex(fresh), 2);
    QCOMPARE(widget.count(), 5);

    // TABS14: the pinned tab keeps its title — the same titled
    // geometry as an unpinned tab carrying the same text, never the
    // icon tile.
    TabBar *bar = widget.bar();
    widget.resize(720, 480);
    widget.show();
    QApplication::processEvents();
    QCOMPARE(bar->tabText(0), QLatin1String("Untitled"));
    const int pinnedWidth = bar->tabRect(0).width();
    const int iconTile = bar->iconSize().width() + 20;
    QVERIFY2(pinnedWidth > iconTile + 8,
             qPrintable(QStringLiteral(
                 "pinned tab collapsed to %1px (icon tile %2)")
                 .arg(pinnedWidth).arg(iconTile)));
    QVERIFY(qAbs(pinnedWidth - bar->tabRect(2).width()) <= 4);

    // The title really paints — ink inside the tab's rect, right of
    // the favicon tile, well past what an icon-only tile could hold.
    const auto titleInk = [](TabBar *b, int index) -> int {
        const QImage image = b->grab().toImage();
        const QRect inside = b->tabRect(index)
            .translated(0, b->containerStripHeight())
            .adjusted(28, 4, -8, -4);
        if (inside.isEmpty())
            return -1;
        QHash<QRgb, int> frequency;
        for (int y = 0; y < inside.height(); ++y)
            for (int x = 0; x < inside.width(); ++x)
                ++frequency[image.pixel(inside.topLeft() + QPoint(x, y))];
        QRgb background = 0;
        int most = 0;
        for (auto it = frequency.constBegin();
             it != frequency.constEnd(); ++it) {
            if (it.value() > most) {
                most = it.value();
                background = it.key();
            }
        }
        const int bgGray = qGray(background);
        int ink = 0;
        for (int y = 0; y < inside.height(); ++y)
            for (int x = 0; x < inside.width(); ++x)
                if (qAbs(qGray(image.pixel(inside.topLeft()
                                           + QPoint(x, y)))
                         - bgGray) > 60)
                    ++ink;
        return ink;
    };
    QVERIFY2(titleInk(bar, 0) > 20,
             "no title text painted inside the pinned tab");
    QVERIFY2(titleInk(bar, 2) > 20,
             "no title text painted inside a plain tab (baseline)");

    // A renamed pinned tab wears the override on the strip — the
    // same display title an unpinned tab would show.  TABS19: the
    // title elides inside the shared 225px metric — a longer title
    // no longer stretches the tab.
    widget.setTabTitleOverride(
        0, QLatin1String("A Much Longer Pinned Title"));
    QApplication::processEvents();
    QCOMPARE(bar->tabText(0),
             QLatin1String("A Much Longer Pinned Title"));
    QVERIFY(qAbs(bar->tabRect(0).width() - pinnedWidth) <= 4);
    widget.setTabTitleOverride(0, QString());
    QApplication::processEvents();

    if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
        const QString dir =
            qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
        bar->grab().save(dir + QLatin1String("/strip-pinned.png"));
    }

    // Pinned tabs hide their per-tab close button entirely.
    bar->setPerTabCloseButtons(true);
    QApplication::processEvents();
    const QTabBar::ButtonPosition side =
        bar->freeSide() == QTabBar::LeftSide ? QTabBar::RightSide
                                             : QTabBar::LeftSide;
    QWidget *pinnedClose = bar->tabButton(0, side);
    if (pinnedClose)
        QVERIFY(pinnedClose->isHidden() || !pinnedClose->isVisibleTo(bar));

    // Middle-click on a pinned tab is ignored — Vivaldi semantics.
    QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier,
                      bar->tabRect(0).center());
    QCOMPARE(widget.count(), 5);
    // ...but still closes an ordinary tab.
    QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier,
                      bar->tabRect(2).center());
    QCOMPARE(widget.count(), 4);
    QVERIFY(widget.isTabPinned(0));
    QVERIFY(widget.isTabPinned(1));
    QCOMPARE(widget.webView(0), v2);
    QCOMPARE(widget.webView(1), v3);

    // A pinned background tab is exempt from the idle-sleep sweep.
    widget.setCurrentIndex(0);
    QCOMPARE(widget.sleepBlockReason(1), QLatin1String("pinned"));

    // "Close Other Tabs" skips pinned tabs in both directions.
    widget.closeOtherTabs(1);              // keep v3, drop the rest unpinned
    QCOMPARE(widget.count(), 2);
    QVERIFY(widget.isTabPinned(0));
    QVERIFY(widget.isTabPinned(1));
    QCOMPARE(widget.webView(0), v2);
    QCOMPARE(widget.webView(1), v3);

    // An explicit close still works on a pinned tab.
    widget.closeTab(1);
    QCOMPARE(widget.count(), 1);
    QVERIFY(widget.isTabPinned(0));

    // Drag conversion: an unpinned tab dropped inside the pinned
    // block pins, a pinned tab dragged past the boundary unpins.
    widget.newTab();
    widget.newTab();
    QCOMPARE(widget.count(), 3);
    WebView *u0 = widget.webView(1);
    WebView *u1 = widget.webView(2);
    QVERIFY(u0 && u1);
    bar->moveTab(2, 0);                    // u1 into the pinned block
    QVERIFY(widget.isTabPinned(0));
    QCOMPARE(widget.webView(0), u1);
    QCOMPARE(widget.pinnedTabCount(), 2);
    bar->moveTab(0, 2);                    // u1 back out past the edge
    QVERIFY(!widget.isTabPinned(2));
    QCOMPARE(widget.webView(2), u1);
    QCOMPARE(widget.pinnedTabCount(), 1);
    QCOMPARE(widget.webView(0), v2);
    QCOMPARE(widget.webView(1), u0);

    // Unpin slides the tab to just past the remaining pinned block.
    widget.setTabPinned(1, true);          // [v2, u0] pinned, u1 not
    QCOMPARE(widget.pinnedTabCount(), 2);
    widget.setTabPinned(0, false);         // v2 slides to index 1
    QCOMPARE(widget.pinnedTabCount(), 1);
    QVERIFY(!widget.isTabPinned(1));
    QCOMPARE(widget.webView(0), u0);
    QCOMPARE(widget.webView(1), v2);
    QCOMPARE(widget.webView(2), u1);
    widget.setTabPinned(0, false);         // nothing left pinned
    QCOMPARE(widget.pinnedTabCount(), 0);

    // Session round-trip: the v5 tail carries the per-tab flags and a
    // v4 blob (no tail) restores unpinned.
    {
        SubTabWidget w2;
        for (int i = 0; i < 3; ++i)
            w2.newTab();
        w2.setTabPinned(1, true);          // v[1] leads the strip
        WebView *pv = w2.webView(0);
        QVERIFY(w2.isTabPinned(0));
        const QByteArray state = w2.saveState();

        SubTabWidget w3;
        QVERIFY(w3.restoreState(state));
        QCOMPARE(w3.count(), 3);
        QVERIFY(w3.isTabPinned(0));
        QVERIFY(!w3.isTabPinned(1));
        QVERIFY(!w3.isTabPinned(2));
        QCOMPARE(w3.webView(0)->url(), pv->url());
    }
    {
        QByteArray v4blob;
        QDataStream out(&v4blob, QIODevice::WriteOnly);
        out << qint32(0xaa) << qint32(4)
            << (QStringList() << QStringLiteral("data:text/plain,v4a")
                              << QStringLiteral("data:text/plain,v4b"))
            << qint32(0)
            << (QList<QByteArray>() << QByteArray() << QByteArray())
            << (QStringList() << QString() << QString())
            << (QStringList() << QString() << QString())
            << qint32(0)
            << QString();
        SubTabWidget w4;
        QVERIFY(w4.restoreState(v4blob));
        QCOMPARE(w4.count(), 2);
        QVERIFY(!w4.isTabPinned(0));
        QVERIFY(!w4.isTabPinned(1));
    }

    // Vertical strip: pinned rows stay on top and new tabs still
    // seat right behind them.
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        settings.setValue(QLatin1String("tabBarPosition"), 2);   // Left
        SubTabWidget wv;
        for (int i = 0; i < 3; ++i)
            wv.newTab();
        wv.setTabPinned(2, true);
        QCOMPARE(wv.pinnedTabCount(), 1);
        wv.resize(720, 480);
        wv.show();
        QApplication::processEvents();
        TabBar *vbar = wv.bar();
        QVERIFY(vbar->tabRect(0).top() < vbar->tabRect(1).top());
        WebView *nv = wv.makeNewTab(false);
        QCOMPARE(wv.webViewIndex(nv), 1);
        // TABS14: the pinned vertical row keeps its painted title —
        // same strip-width titled row as any other.
        QApplication::processEvents();
        QCOMPARE(vbar->tabRect(0).width(), vbar->tabRect(1).width());
        QVERIFY2(titleInk(vbar, 0) > 20,
                 "no title text painted inside the pinned row");
        QVERIFY2(titleInk(vbar, 1) > 20,
                 "no title text painted inside a plain row");
        if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
            const QString dir =
                qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
            vbar->grab().save(
                dir + QLatin1String("/strip-pinned-vertical.png"));
        }
        settings.remove(QLatin1String("tabBarPosition"));
        settings.endGroup();
    }

    widget.closeTab();
    widget.closeTab();
    widget.closeTab();
}

// TABS09: the icon-only view mode on a real TabWidget — the shared
// action collapses the strip to favicon tiles, group membership and
// tab data stay untouched, new tabs inherit the mode, and the
// loadSettings fan-out re-reads the persisted key.
void tst_TabWidget::iconOnlyTabStrip()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("iconOnlyTabs"));

    SubTabWidget widget;
    for (int i = 0; i < 3; ++i)
        widget.newTab();
    QCOMPARE(widget.count(), 3);
    const QString gid = widget.createTabGroup(0);
    widget.addTabToGroup(1, gid);
    QCOMPARE(widget.tabGroupSize(gid), 2);

    widget.resize(720, 480);
    widget.show();
    QApplication::processEvents();
    TabBar *bar = widget.bar();

    const int plainWidth = bar->tabRect(2).width();
    QVERIFY(plainWidth > 60);

    QAction *action = bar->iconOnlyTabsAction();
    QVERIFY(action);
    action->trigger();
    QApplication::processEvents();
    QVERIFY(bar->iconOnlyTabs());
    const int tile = bar->iconSize().width() + 20;
    for (int i = 0; i < widget.count(); ++i)
        QVERIFY2(bar->tabRect(i).width() <= tile + 8,
                 qPrintable(QStringLiteral(
                     "tab %1 stayed %2px wide in icon mode")
                     .arg(i).arg(bar->tabRect(i).width())));
    // Grouped members keep their membership — a view mode only.
    QCOMPARE(widget.tabGroupId(0), gid);
    QCOMPARE(widget.tabGroupId(1), gid);
    QCOMPARE(widget.tabGroupSize(gid), 2);
    // New tabs land inside the mode.
    widget.newTab();
    QApplication::processEvents();
    QVERIFY(bar->tabRect(widget.count() - 1).width() <= tile + 8);

    if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
        const QString dir =
            qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
        bar->grab().save(dir + QLatin1String("/strip-icononly.png"));
    }

    // The loadSettings fan-out (the settings-save path) re-reads the
    // key and restores the titled strip.
    settings.setValue(QLatin1String("iconOnlyTabs"), false);
    widget.loadSettings();
    QApplication::processEvents();
    QVERIFY(!bar->iconOnlyTabs());
    QVERIFY(bar->tabRect(2).width() > tile + 8);

    if (qEnvironmentVariableIsSet("ARORA_STRIP_GRAB_DIR")) {
        const QString dir =
            qEnvironmentVariable("ARORA_STRIP_GRAB_DIR");
        bar->grab().save(dir + QLatin1String("/strip-titled.png"));
    }

    settings.remove(QLatin1String("iconOnlyTabs"));
    settings.endGroup();
}

// TABS05: undo closed tab — a capped memory-only stack records each
// closed tab's url, session history blob, strip index, container,
// group, pin and scroll offset.  Ctrl+Shift+T (openLastTab) rebuilds
// the tab at its old slot in LIFO order; off-the-record pages never
// enter and a history wipe or explicit clear empties the stack.
void tst_TabWidget::reopenClosedTab()
{
    // The CONT06 strip (default on) only engages once a second
    // container owns tabs — pin the inline mode anyway so the index
    // assertions below are immune to the setting.
    QSettings settings;
    const QVariant savedDisplay =
        settings.value(QLatin1String("tabs/containerDisplay"));
    settings.setValue(QLatin1String("tabs/containerDisplay"), 0);

    SubTabWidget widget;
    QVERIFY(!widget.hasRecentlyClosedTabs());
    QVERIFY(!widget.recentlyClosedTabsAction()->isEnabled());
    widget.openLastTab();          // empty-stack no-op
    QCOMPARE(widget.count(), 0);

    for (int i = 0; i < 3; ++i)
        widget.newTab();
    QCOMPARE(widget.count(), 3);
    const QUrl u0("data:text/plain,undo-a");
    const QUrl u1("data:text/plain,undo-b");
    const QUrl u2("data:text/plain,undo-c");
    widget.webView(0)->loadUrl(u0);
    widget.webView(1)->loadUrl(u1);
    widget.webView(2)->loadUrl(u2);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), u0, 15000);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), u1, 15000);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(2)->url(), u2, 15000);

    // Close all three; the records stack newest-first.
    widget.closeTab(1);            // u1
    widget.closeTab(0);            // u0
    widget.closeTab(0);            // u2 (shifted down)
    QCOMPARE(widget.count(), 0);
    QVERIFY(widget.hasRecentlyClosedTabs());
    QVERIFY(widget.recentlyClosedTabsAction()->isEnabled());

    // Three undos restore most-recent-first, each at its recorded
    // index — u2@0, then u0@0, then u1@1 -> original order rebuilt.
    widget.openLastTab();
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), u2, 15000);
    widget.openLastTab();
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), u0, 15000);
    QCOMPARE(widget.webView(1)->url(), u2);
    widget.openLastTab();
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), u1, 15000);
    QCOMPARE(widget.webView(0)->url(), u0);
    QCOMPARE(widget.webView(2)->url(), u2);
    QVERIFY(!widget.hasRecentlyClosedTabs());
    QVERIFY(!widget.recentlyClosedTabsAction()->isEnabled());

    // Pin round-trip — a pinned tab comes back into the block.
    widget.setTabPinned(1, true);            // u1 leads, pinned
    QVERIFY(widget.isTabPinned(0));
    widget.closeTab(0);
    QCOMPARE(widget.count(), 2);
    widget.openLastTab();
    QCOMPARE(widget.count(), 3);
    int pinnedIdx = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.webView(i) && widget.webView(i)->url() == u1)
            pinnedIdx = i;
    }
    QCOMPARE(pinnedIdx, 0);
    QVERIFY(widget.isTabPinned(0));
    QCOMPARE(widget.pinnedTabCount(), 1);

    // Group round-trip — close a member, the group lives on, undo
    // rejoins it.
    const QString gid = widget.createTabGroup(1);
    QVERIFY(!gid.isEmpty());
    widget.addTabToGroup(2, gid);
    QCOMPARE(widget.tabGroupId(2), gid);
    widget.closeTab(2);                      // u2 leaves the group
    QCOMPARE(widget.tabGroupSize(gid), 1);
    widget.openLastTab();
    QVERIFY(widget.tabGroupSize(gid) >= 2);
    int groupIdx = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.webView(i) && widget.webView(i)->url() == u2)
            groupIdx = i;
    }
    QVERIFY(groupIdx >= 0);
    QCOMPARE(widget.tabGroupId(groupIdx), gid);

    // A group that died between close and undo restores ungrouped.
    widget.closeTab(1);                      // u0 leaves the group
    QCOMPARE(widget.tabGroupSize(gid), 1);
    widget.ungroupTabs(gid);                 // the group record dies
    QVERIFY(widget.tabGroupIds().isEmpty());
    widget.openLastTab();                    // u0, recorded gid gone
    int ungroupedIdx = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.webView(i) && widget.webView(i)->url() == u0)
            ungroupedIdx = i;
    }
    QVERIFY(ungroupedIdx >= 0);
    QCOMPARE(widget.tabGroupId(ungroupedIdx), QString());

    // Container round-trip — the reopened tab lands back on its
    // container's profile.
    ContainerManager *manager = ContainerManager::instance();
    const QString cid = manager->createContainer(
        QStringLiteral("UndoTest"), QColor(Qt::blue)).id;
    QVERIFY(!cid.isEmpty());
    WebView *containerView = widget.makeNewTabInContainer(cid, true);
    QVERIFY(containerView);
    const QUrl cu("data:text/plain,undo-container");
    containerView->loadUrl(cu);
    QTRY_COMPARE_WITH_TIMEOUT(containerView->url(), cu, 15000);
    const int containerIdx = widget.webViewIndex(containerView);
    QCOMPARE(widget.containerIdForTab(containerIdx), cid);
    widget.closeTab(containerIdx);
    widget.openLastTab();
    int restoredContainerIdx = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.webView(i) && widget.webView(i)->url() == cu)
            restoredContainerIdx = i;
    }
    QVERIFY(restoredContainerIdx >= 0);
    QCOMPARE(widget.containerIdForTab(restoredContainerIdx), cid);
    // deleteContainer() is not idempotent (a second call fails on
    // !isContainerId) and its storage teardown retries internally,
    // so it must run once — wait for the closed view's deferred
    // deletion to land, then call it a single time.
    QSignalSpy containerViewDied(widget.webView(restoredContainerIdx),
                                 &QObject::destroyed);
    widget.closeTab(restoredContainerIdx);
    QTRY_VERIFY_WITH_TIMEOUT(containerViewDied.count() == 1, 5000);
    QVERIFY(manager->deleteContainer(cid));

    // The recently-closed submenu lists records with their titles —
    // a data: page has none, so the url stands in.
    QMenu *closedMenu = widget.recentlyClosedTabsAction()->menu();
    QVERIFY(closedMenu);
    emit closedMenu->aboutToShow();
    const int records = closedMenu->actions().size();
    QVERIFY(records > 0);
    for (QAction *entry : closedMenu->actions())
        QVERIFY(!entry->text().isEmpty());

    // A private tab never enters the stack — closing it queues
    // nothing (SEC07).
    WebView *privateView = widget.makeNewPrivateTab(true);
    QVERIFY(privateView);
    const QUrl pu("data:text/plain,undo-private");
    privateView->loadUrl(pu);
    QTRY_COMPARE_WITH_TIMEOUT(privateView->url(), pu, 15000);
    const int privateIdx = widget.webViewIndex(privateView);
    QVERIFY(widget.isTabPrivate(privateIdx));
    widget.closeTab(privateIdx);
    emit closedMenu->aboutToShow();
    QCOMPARE(closedMenu->actions().size(), records);

    // A history wipe empties the stack (the Clear Private Data path
    // reaches the same slot).
    emit HistoryManager::instance()->historyCleared();
    QVERIFY(!widget.hasRecentlyClosedTabs());
    QVERIFY(!widget.recentlyClosedTabsAction()->isEnabled());

    while (widget.count() > 0)
        widget.closeTab(widget.count() - 1);

    if (savedDisplay.isValid())
        settings.setValue(QLatin1String("tabs/containerDisplay"),
                          savedDisplay);
    else
        settings.remove(QLatin1String("tabs/containerDisplay"));
}

// ENG09 — strip integrity across reloadTabInEngine(): the swap must
// never lose or reorder the other tabs, the replacement takes the
// old tab's slot, and strip state survives per the documented
// contract (pin transfers by position; group membership drops on a
// foreign EngineTab; a collapsed group re-collapses around its next
// member when its chip is swapped).
void tst_TabWidget::engineSwapStripState()
{
    FakeEngineBackend fake;
    EngineRegistry::registerBackend(&fake);

    // Multi-tab strip: the middle tab swaps out and back — every
    // other tab keeps its slot.
    {
        SubTabWidget widget;
        for (int i = 0; i < 3; ++i)
            widget.newTab();
        WebView *v0 = widget.webView(0);
        WebView *v2 = widget.webView(2);
        QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("fake")));
        QCOMPARE(widget.count(), 3);
        QVERIFY(widget.engineTab(1));
        QCOMPARE(widget.webView(0), v0);
        QCOMPARE(widget.webView(2), v2);
        QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("webengine")));
        QCOMPARE(widget.count(), 3);
        QVERIFY(widget.webView(1));
        QCOMPARE(widget.webView(0), v0);
        QCOMPARE(widget.webView(2), v2);
    }

    // Pinned strip: swapping a pinned tab keeps the replacement in
    // the pinned block.
    {
        SubTabWidget widget;
        for (int i = 0; i < 3; ++i)
            widget.newTab();
        widget.setTabPinned(0, true);
        widget.setTabPinned(1, true);
        QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
        QCOMPARE(widget.count(), 3);
        QVERIFY(widget.engineTab(0));
        QCOMPARE(widget.pinnedTabCount(), 2);
        QVERIFY(widget.isTabPinned(0));
    }

    // Collapsed group chip: closing the chip expands the group to
    // free the hidden members — the swap must re-collapse it around
    // the next member rather than leaving the strip expanded.
    {
        SubTabWidget widget;
        for (int i = 0; i < 4; ++i)
            widget.newTab();
        const QString gid = widget.createTabGroup(0);
        widget.addTabToGroup(1, gid);
        widget.addTabToGroup(2, gid);
        widget.setTabGroupCollapsed(gid, true);
        QCOMPARE(widget.count(), 2);            // chip + v3
        const int totalBefore = widget.totalTabCount();
        QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
        // The foreign tab takes the chip's slot, the next member
        // becomes the new chip, the rest stay hidden.
        QCOMPARE(widget.count(), 3);
        QCOMPARE(widget.totalTabCount(), totalBefore);
        QVERIFY(widget.engineTab(0));
        QVERIFY(widget.tabGroupIsCollapsed(gid));
        QVERIFY(widget.isTabGroupChip(1));
        QCOMPARE(widget.tabGroupMembers(gid), QList<int>() << 1);
    }

    EngineRegistry::unregisterBackend(&fake);
}

// SLEEP01: the suspend cycle end to end — eligibility reasons, the
// async capture/discard, wake-on-activate, the dirty-form latch and
// the idle sweep all observable from the public API.
void tst_TabWidget::sleepingTabs()
{
    SubTabWidget widget;
    widget.newTab();                       // index 0, current
    widget.newTab();                       // index 1, background target
    QCOMPARE(widget.count(), 2);
    widget.setCurrentIndex(0);

    QCOMPARE(widget.sleepBlockReason(-1), QLatin1String("invalid"));
    QCOMPARE(widget.sleepBlockReason(99), QLatin1String("invalid"));
    QCOMPARE(widget.sleepBlockReason(0), QLatin1String("current"));
    QCOMPARE(widget.sleepBlockReason(1), QLatin1String("empty"));

    // Once the page finishes loading the background tab is eligible.
    WebView *view1 = widget.webView(1);
    QSignalSpy loaded(view1->page(), &QWebEnginePage::loadFinished);
    view1->load(QUrl("data:text/html,<title>t</title>body"));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
    QTRY_COMPARE(widget.sleepBlockReason(1), QString());

    // Manual sleep discards the page; the strip slot stays.
    widget.sleepTab(1);
    QTRY_VERIFY_WITH_TIMEOUT(widget.isTabSleeping(1), 10000);
    QCOMPARE(widget.sleepBlockReason(1), QLatin1String("sleeping"));
    QCOMPARE(widget.sleepingTabCount(), 1);
    QVERIFY(!widget.isTabSleeping(0));

    // Selecting a slept tab wakes it.
    QSignalSpy waked(view1->page(), &QWebEnginePage::loadFinished);
    widget.setCurrentIndex(1);
    QVERIFY(!widget.isTabSleeping(1));
    // The wake fires a fresh navigation — a load() issued while it is
    // still in-flight is merged into it by WebEngine and commits
    // without its own loadStarted.  Wait for the restore to finish so
    // the clean-navigation leg below exercises a real navigation.
    QTRY_VERIFY_WITH_TIMEOUT(!waked.isEmpty(), 15000);
    QCOMPARE(widget.sleepingTabCount(), 0);

    // Waking an awake tab and sleeping the current tab are no-ops.
    widget.wakeTab(1);
    widget.setCurrentIndex(0);
    widget.sleepTab();                     // -1 -> current — refused
    QVERIFY(!widget.isTabSleeping(0));

    // Unsaved form input latches the refusal and posts a status text.
    view1->page()->runJavaScript(QLatin1String(
        "document.body.innerHTML='<input id=i>';"
        "document.getElementById('i').value='x';"));
    QSignalSpy statusSpy(&widget, &TabWidget::showStatusBarMessage);
    widget.sleepTab(1);
    QTRY_COMPARE_WITH_TIMEOUT(widget.sleepBlockReason(1),
                              QLatin1String("form"), 10000);
    QVERIFY(!widget.isTabSleeping(1));
    QTRY_VERIFY(statusSpy.count() > 0);

    // A navigation clears the latch — the tab is eligible again.
    QSignalSpy reloaded(view1->page(), &QWebEnginePage::loadFinished);
    view1->load(QUrl("data:text/html,<p>clean</p>"));
    QTRY_VERIFY_WITH_TIMEOUT(!reloaded.isEmpty(), 15000);
    QTRY_COMPARE(widget.sleepBlockReason(1), QString());

    // The idle sweep sleeps only eligible background tabs — the
    // current tab and an unloaded tab are skipped.
    widget.newTab();                       // index 2, never loaded
    widget.setCurrentIndex(0);
    widget.suspendIdleTabs(0);
    QTRY_VERIFY_WITH_TIMEOUT(widget.isTabSleeping(1), 10000);
    QVERIFY(!widget.isTabSleeping(0));
    QVERIFY(!widget.isTabSleeping(2));
    QCOMPARE(widget.sleepingTabCount(), 1);
    widget.wakeTab(1);

    while (widget.count())
        widget.closeTab(0);
}

// MEM06: purgeInactiveTabs freeze-pulses eligible background tabs —
// the page goes Frozen then restores Active a fraction of a second
// later — while the current tab and never-loaded tabs are skipped.
void tst_TabWidget::memoryPurge()
{
    SubTabWidget widget;
    widget.newTab();                       // index 0, current
    widget.newTab();                       // index 1, background
    widget.newTab();                       // index 2, never loaded
    widget.setCurrentIndex(0);

    WebView *view0 = widget.webView(0);
    WebView *view1 = widget.webView(1);
    QVERIFY(view0 && view1);
    QSignalSpy loaded(view1->page(), &QWebEnginePage::loadFinished);
    view1->load(QUrl("data:text/html,<title>t</title>body"));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
    QTRY_VERIFY(!view1->enginePage()->isLoading());

    using LS = Engine::Page::LifecycleState;
    QCOMPARE(view1->enginePage()->lifecycleState(), LS::Active);

    widget.purgeInactiveTabs();
    QTRY_COMPARE_WITH_TIMEOUT(
        view1->enginePage()->lifecycleState(), LS::Frozen, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(
        view1->enginePage()->lifecycleState(), LS::Active, 10000);

    // The current tab is never frozen by the sweep; an empty tab has
    // no page to purge.
    QCOMPARE(view0->enginePage()->lifecycleState(), LS::Active);
    QVERIFY(widget.webView(2));
    QCOMPARE(widget.webView(2)->enginePage()->lifecycleState(), LS::Active);

    // A sleeping (discarded) tab is skipped by the sweep and the
    // deferred unfreeze never revives it.
    widget.sleepTab(1);
    QTRY_VERIFY_WITH_TIMEOUT(widget.isTabSleeping(1), 10000);
    widget.purgeInactiveTabs();
    QTest::qWait(800);                     // past the ~600ms pulse window
    QCOMPARE(view1->enginePage()->lifecycleState(), LS::Discarded);

    while (widget.count())
        widget.closeTab(0);
}

// MEM08: the manual flush — a background tab discards outright via
// the sleep path (its renderer association drops to nothing); the
// current tab takes a discard+reactivate pulse that reloads the
// entry; and the deferred statusbar report measures the real RSS
// delta instead of claiming a figure.
void tst_TabWidget::freeTabMemory()
{
    SubTabWidget widget;
    widget.newTab();                        // 0 — current, cycled
    widget.newTab();                        // 1 — background, cycled
    widget.newTab();                        // 2 — never loaded
    widget.setCurrentIndex(0);
    WebView *view0 = widget.webView(0);
    WebView *view1 = widget.webView(1);
    QVERIFY(view0 && view1 && widget.webView(2));

    // A few unique real documents per tab — the retained session
    // documents are what the flush releases.
    const QString pad(96 * 1024, QLatin1Char('x'));
    for (int i = 0; i < 6; ++i) {
        for (WebView *view : {view0, view1}) {
            QSignalSpy loaded(view->page(),
                              &QWebEnginePage::loadFinished);
            view->load(QUrl(QLatin1String("data:text/html,<p>")
                            + QString::number(i) + pad));
            QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
        }
    }

    using LS = Engine::Page::LifecycleState;
    const qint64 beforeBgKb =
        view1->enginePage()->residentMemoryKb();
    const qint64 beforeFgKb =
        view0->enginePage()->residentMemoryKb();
    qInfo() << "freeTabMemory: rendererKb before" << beforeFgKb
            << beforeBgKb;

    QSignalSpy statusSpy(&widget, &TabWidget::showStatusBarMessage);

    // Background leg — the flush IS the discard; the page's renderer
    // association drops to nothing.
    widget.freeTabMemory(1);
    QTRY_VERIFY_WITH_TIMEOUT(widget.isTabSleeping(1), 10000);
    QCOMPARE(view1->enginePage()->lifecycleState(), LS::Discarded);
    if (beforeBgKb > 0) {
        QTRY_VERIFY_WITH_TIMEOUT(
            view1->enginePage()->residentMemoryKb() <= 0, 10000);
    }

    // A second click on the slept tab reports the state rather than
    // re-discarding.
    widget.freeTabMemory(1);
    QTRY_VERIFY_WITH_TIMEOUT(
        spyHasText(statusSpy, QLatin1String("asleep")), 5000);

    // ~2s out the first flush's measured report lands — the renderer
    // is gone, so it reads as a real freed figure.
    if (beforeBgKb > 0) {
        QTRY_VERIFY_WITH_TIMEOUT(
            spyHasText(statusSpy, QLatin1String("Freed")), 6000);
    }

    // Current leg — discard+reactivate reloads the entry in place.
    QSignalSpy reloaded(view0->page(), &QWebEnginePage::loadFinished);
    widget.freeTabMemory(0);
    // The discard lands synchronously; reactivation follows ~300ms
    // later and drives the reload.
    QCOMPARE(view0->enginePage()->lifecycleState(), LS::Discarded);
    QTRY_VERIFY_WITH_TIMEOUT(!reloaded.isEmpty(), 15000);
    QTRY_COMPARE_WITH_TIMEOUT(view0->enginePage()->lifecycleState(),
                              LS::Active, 10000);
    QVERIFY(!widget.isTabSleeping(0));
    if (beforeFgKb > 0) {
        // The fresh WebContents' renderer reads below the cycled
        // figure — retained session documents are gone.
        QTRY_VERIFY_WITH_TIMEOUT(
            view0->enginePage()->residentMemoryKb() > 0
                && view0->enginePage()->residentMemoryKb()
                       < beforeFgKb,
            15000);
    }

    // Window-level leg — every eligible background tab at once; the
    // empty tab is skipped by sleepBlockReason.
    widget.wakeTab(1);
    QTRY_VERIFY_WITH_TIMEOUT(!widget.isTabSleeping(1), 10000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view1->enginePage()->isLoading(), 15000);
    widget.freeBackgroundTabsMemory();
    QTRY_VERIFY_WITH_TIMEOUT(widget.isTabSleeping(1), 10000);
    QVERIFY(!widget.isTabSleeping(0));
    QVERIFY(!widget.isTabSleeping(2));
    // Nothing left to take — the aggregate reports it honestly.
    widget.freeBackgroundTabsMemory();
    QTRY_VERIFY_WITH_TIMEOUT(
        spyHasText(statusSpy, QLatin1String("No background tabs")),
        5000);

    while (widget.count())
        widget.closeTab(0);
}

// CONT02/CONT04: tabs bind to container profiles, children inherit
// the current tab's container, reopen swaps the profile in place, and
// the diversion entry point lands the url on the right profile.
void tst_TabWidget::containerTabs()
{
    // The CONT06 strip (default on) filters tabs to the active
    // container level once a second container owns tabs — pin inline
    // mode so the index assertions below see every tab.
    QSettings settings;
    const QVariant savedDisplay =
        settings.value(QLatin1String("tabs/containerDisplay"));
    settings.setValue(QLatin1String("tabs/containerDisplay"), 0);

    ContainerManager *manager = ContainerManager::instance();
    const ContainerManager::Container container =
        manager->createContainer(QLatin1String("CovCtx"), QColor(Qt::blue));
    QVERIFY(!container.id.isEmpty());

    SubTabWidget widget;
    widget.newTab();
    widget.webView(0)->load(QUrl("data:text/plain,ctx-base"));
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(),
        QUrl("data:text/plain,ctx-base"), 15000);
    QCOMPARE(widget.containerIdForTab(0),
             ContainerManager::defaultContainerId());

    WebView *ctxView = widget.makeNewTabInContainer(container.id, true);
    QVERIFY(ctxView);
    QCOMPARE(widget.containerIdForTab(1), container.id);

    // A plain new tab opened from a container tab stays inside it.
    widget.makeNewTab(false);
    QCOMPARE(widget.containerIdForTab(2), container.id);

    // Unknown ids degrade to the default container.
    WebView *unknown = widget.makeNewTabInContainer(
        QLatin1String("cov-no-such"), false);
    QVERIFY(unknown);
    QCOMPARE(widget.containerIdForTab(widget.webViewIndex(unknown)),
             ContainerManager::defaultContainerId());

    // Reopen swaps the profile in place — slot and url survive.
    widget.setCurrentIndex(0);
    widget.reopenTabInContainer(0, container.id);
    QCOMPARE(widget.containerIdForTab(0), container.id);
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(),
        QUrl("data:text/plain,ctx-base"), 15000);

    // A bogus id is refused; out-of-range indices are ignored; an
    // empty id returns the tab to the default container.
    widget.reopenTabInContainer(0, QLatin1String("cov-no-such"));
    QCOMPARE(widget.containerIdForTab(0), container.id);
    widget.reopenTabInContainer(-1, container.id);
    widget.reopenTabInContainer(0, QString());
    QCOMPARE(widget.containerIdForTab(0),
             ContainerManager::defaultContainerId());

    // The diversion entry point lands the url on the container.
    const int before = widget.count();
    widget.loadUrlInContainer(QUrl("data:text/plain,diverted"),
                              container.id);
    QCOMPARE(widget.count(), before + 1);
    QCOMPARE(widget.containerIdForTab(widget.currentIndex()), container.id);
    QTRY_COMPARE_WITH_TIMEOUT(widget.currentWebView()->url(),
        QUrl("data:text/plain,diverted"), 15000);
    widget.loadUrlInContainer(QUrl(), container.id);   // invalid — no-op
    QCOMPARE(widget.count(), before + 1);

    // The management entry point opens the settings dialog.
    acceptModal();
    widget.manageContainers();

    // Live container tabs block deletion — close them first.
    while (widget.count())
        widget.closeTab(0);
    QVERIFY(manager->deleteContainer(container.id));

    if (savedDisplay.isValid())
        settings.setValue(QLatin1String("tabs/containerDisplay"),
                          savedDisplay);
    else
        settings.remove(QLatin1String("tabs/containerDisplay"));
}

// The tab-strip slots that drive whole actions: clone/reload/close-
// others/bookmark, the recently-closed stack + menu rebuild, the
// location bar's return-press navigation, page-initiated window
// close, the hover thumbnail, clear() and the modifier mapper.
void tst_TabWidget::tabSlotDrive()
{
    SubTabWidget widget;
    widget.newTab();
    widget.webView(0)->load(QUrl("data:text/plain,one"));

    // cloneTab: a valid index duplicates, -1 takes current, a bad
    // index is ignored.
    widget.cloneTab(0);
    QCOMPARE(widget.count(), 2);
    widget.cloneTab(-1);
    QCOMPARE(widget.count(), 3);
    widget.cloneTab(99);
    QCOMPARE(widget.count(), 3);

    // reloadTab / reloadAllTabs run the views' reload paths.
    widget.reloadTab(0);
    widget.reloadTab(-1);
    widget.reloadTab(99);
    widget.reloadAllTabs();

    // closeOtherTabs keeps only the requested index.
    widget.closeOtherTabs(-1);               // early return
    QCOMPARE(widget.count(), 3);
    widget.closeOtherTabs(1);
    QCOMPARE(widget.count(), 1);

    // Closing a loaded tab pushes it onto the recently-closed stack;
    // the menu rebuilds from it on aboutToShow, and triggering an
    // entry reopens the tab.  (data: urls never enter WebEngine
    // history — the serialized entry would be empty — so a real
    // file: page stands in.)
    WebView *closing = widget.webView(0);
    QSignalSpy loaded(closing->page(), &QWebEnginePage::loadFinished);
    closing->load(QUrl("file:///etc/hostname"));
    QTRY_VERIFY_WITH_TIMEOUT(!loaded.isEmpty(), 15000);
    widget.closeTab(0);
    QCOMPARE(widget.count(), 0);
    QVERIFY(widget.recentlyClosedTabsAction()->isEnabled());

    QMenu *menu = widget.recentlyClosedTabsAction()->menu();
    QVERIFY(menu);
    emit menu->aboutToShow();
    QVERIFY(!menu->actions().isEmpty());
    menu->actions().first()->trigger();
    QCOMPARE(widget.count(), 1);
    // url() reports the requested url before the commit lands; the
    // history entry is what the next closeTab needs to serialize.
    QTRY_VERIFY_WITH_TIMEOUT(
        widget.webView(0)->history()->count() >= 1, 15000);

    // openLastTab consumes the (still-queued) entry via the shortcut
    // path — it's a private slot, driven the way the QShortcut does.
    widget.closeTab(0);
    QVERIFY(widget.recentlyClosedTabsAction()->isEnabled());
    QVERIFY(QMetaObject::invokeMethod(&widget, "openLastTab"));
    QCOMPARE(widget.count(), 1);

    // historyCleared empties the stack and disarms the action.
    QMetaObject::invokeMethod(&widget, "historyCleared");
    QVERIFY(!widget.recentlyClosedTabsAction()->isEnabled());

    // Enter in the per-tab location bar routes through loadString on
    // the sender.  The signal is what wires sender(), so emit it.
    QLineEdit *edit = widget.locationBar(0);
    QVERIFY(edit);
    edit->setText(QLatin1String("data:text/plain,typed"));
    emit edit->returnPressed();
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(),
        QUrl("data:text/plain,typed"), 15000);

    // window.close() from a page in a multi-tab strip closes that tab.
    widget.newTab();
    QCOMPARE(widget.count(), 2);
    emit widget.webView(1)->page()->windowCloseRequested();
    QCOMPARE(widget.count(), 1);

    // bookmarkTabs drives the save-all dialog then bails cleanly.
    rejectModal();
    widget.bookmarkTabs();

    // The hover thumbnail grabs (or fails to grab) the compositor
    // frame — offscreen it may legitimately come back blank.
    widget.tabThumbnail(0);
    widget.tabThumbnail(99);

    // clear() empties the closed-tab stack and refreshes the bars.
    widget.clear();

    // The modifier mapper: no keys leaves the request alone, Ctrl
    // asks for a new (unselected) tab and resets the latched keys.
    BrowserApplication *app = BrowserApplication::instance();
    QCOMPARE(TabWidget::modifyWithUserBehavior(TabWidget::CurrentTab),
             TabWidget::CurrentTab);
    app->setEventKeyboardModifiers(Qt::ControlModifier);
    QCOMPARE(TabWidget::modifyWithUserBehavior(TabWidget::CurrentTab),
             TabWidget::NewNotSelectedTab);
    QCOMPARE(app->eventKeyboardModifiers(), Qt::NoModifier);
    app->setEventKeyboardModifiers(Qt::ControlModifier | Qt::AltModifier);
    QCOMPARE(TabWidget::modifyWithUserBehavior(TabWidget::CurrentTab),
             TabWidget::NewWindow);

    while (widget.count())
        widget.closeTab(0);
}

// START02: the user-facing New Tab command (action / tab-bar
// double-click) opens the fresh tab on the start page — the
// MainWindow/home target goHome() resolves, default about:home ->
// qrc:/startpage.html — while the programmatic newTab() stays bare
// so startup/session-restore paths keep their own say.
void tst_TabWidget::newTabStartPage()
{
    QSettings settings;
    const QString savedHome =
        settings.value(QLatin1String("MainWindow/home")).toString();
    const auto restoreHome = [&settings, &savedHome]() {
        if (savedHome.isEmpty())
            settings.remove(QLatin1String("MainWindow/home"));
        else
            settings.setValue(QLatin1String("MainWindow/home"), savedHome);
    };
    settings.remove(QLatin1String("MainWindow/home"));

    SubTabWidget widget;

    // The real user surface: the New Tab action, not the bare
    // programmatic slot.
    widget.newTabAction()->trigger();
    QCOMPARE(widget.count(), 1);
    WebView *view = widget.currentWebView();
    QVERIFY(view);
    QTRY_COMPARE_WITH_TIMEOUT(view->url(),
        QUrl(QLatin1String("qrc:/startpage.html")), 15000);
    // document.title is written by the page's own update() through
    // the 'arora' channel — its arrival proves the handshake ran,
    // not just that the qrc bytes committed.
    QTRY_COMPARE_WITH_TIMEOUT(view->title(),
        QStringLiteral("Welcome to Arora!"), 15000);

    // A custom homepage redirects the same command.
    settings.setValue(QLatin1String("MainWindow/home"),
                      QLatin1String("data:text/plain,custom-home"));
    widget.newTabAction()->trigger();
    QCOMPARE(widget.count(), 2);
    WebView *custom = widget.currentWebView();
    QVERIFY(custom);
    QVERIFY(custom != view);
    QTRY_COMPARE_WITH_TIMEOUT(custom->url(),
        QUrl(QLatin1String("data:text/plain,custom-home")), 15000);

    // The programmatic slot still produces a bare tab — the window
    // ctor, close-refusal refill and session restore all depend on
    // startupBehavior deciding what the tab shows.
    widget.newTab();
    QCOMPARE(widget.count(), 3);
    QVERIFY(widget.currentWebView());
    QVERIFY(widget.currentWebView()->url().isEmpty());

    while (widget.count())
        widget.closeTab(0);
    restoreHome();
}

// PREFUI04: performance/lazyRestoreTabs defers a restored tab's first
// load until activation — the strip already shows the saved url — the
// selected tab flushes immediately, "Always Load Pinned Tabs" exempts
// pinned tabs, and a still-deferred tab re-serializes its stashed
// url + history blob so saveState() loses nothing.
void tst_TabWidget::lazySessionRestore()
{
    QSettings settings;
    const QUrl url1(QLatin1String("data:text/html,one"));
    const QUrl url2(QLatin1String("data:text/html,two"));
    const QUrl url3(QLatin1String("data:text/html,three"));

    // Saved strip: [url1 pinned] [url2 current] [url3 background].
    QByteArray state;
    {
        SubTabWidget widget;
        widget.newTab();
        widget.loadUrl(url1, TabWidget::CurrentTab);
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), url1, 15000);
        widget.loadUrl(url2, TabWidget::NewSelectedTab);
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), url2, 15000);
        widget.loadUrl(url3, TabWidget::NewTab);
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(2)->url(), url3, 15000);
        widget.setTabPinned(0, true);
        QVERIFY(widget.isTabPinned(0));
        QCOMPARE(widget.currentIndex(), 1);
        state = widget.saveState();
    }

    settings.setValue(QLatin1String("performance/lazyRestoreTabs"), true);
    settings.setValue(QLatin1String("performance/alwaysLoadPinnedTabs"), true);
    {
        SubTabWidget widget;
        widget.newTab();
        QVERIFY(widget.restoreState(state));
        QCOMPARE(widget.count(), 3);

        // The pinned exemption loads eagerly; the saved-current tab
        // flushes its deferral as soon as restore settles.
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), url1, 15000);
        QVERIFY(!widget.isTabPendingRestore(0));
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(1)->url(), url2, 15000);
        QVERIFY(!widget.isTabPendingRestore(1));

        // The background tab holds only its stashed session entry —
        // no load, but the strip + omnibox already read restored.
        QVERIFY(widget.isTabPendingRestore(2));
        QCOMPARE(widget.pendingRestoreUrl(2), url3);
        QVERIFY(widget.webView(2)->url().isEmpty());
        QCOMPARE(widget.locationBar(2)->text(),
                 QString::fromUtf8(url3.toEncoded()));
        QVERIFY(widget.tabText(2).contains(QLatin1String("three")));

        // First activation performs the deferred load.
        widget.setCurrentIndex(2);
        QVERIFY(!widget.isTabPendingRestore(2));
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(2)->url(), url3, 15000);

        while (widget.count() > 0)
            widget.closeTab(widget.count() - 1);
    }

    // A deferred tab re-serializes its stash — restoring the re-saved
    // session eagerly yields every url again.
    {
        SubTabWidget lazy;
        lazy.newTab();
        QVERIFY(lazy.restoreState(state));
        QCOMPARE(lazy.count(), 3);
        QVERIFY(lazy.isTabPendingRestore(2));
        const QByteArray resaved = lazy.saveState();

        settings.setValue(QLatin1String("performance/lazyRestoreTabs"), false);
        SubTabWidget eager;
        eager.newTab();
        QVERIFY(eager.restoreState(resaved));
        QCOMPARE(eager.count(), 3);
        QTRY_COMPARE_WITH_TIMEOUT(eager.webView(0)->url(), url1, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(eager.webView(1)->url(), url2, 15000);
        QTRY_COMPARE_WITH_TIMEOUT(eager.webView(2)->url(), url3, 15000);
        while (eager.count() > 0)
            eager.closeTab(eager.count() - 1);
        while (lazy.count() > 0)
            lazy.closeTab(lazy.count() - 1);
        settings.setValue(QLatin1String("performance/lazyRestoreTabs"), true);
    }

    // Opting out of the pinned exemption defers pinned tabs too.
    settings.setValue(QLatin1String("performance/alwaysLoadPinnedTabs"), false);
    {
        SubTabWidget widget;
        widget.newTab();
        QVERIFY(widget.restoreState(state));
        QCOMPARE(widget.count(), 3);
        QVERIFY(widget.isTabPendingRestore(0));
        QCOMPARE(widget.pendingRestoreUrl(0), url1);
        widget.setCurrentIndex(0);
        QVERIFY(!widget.isTabPendingRestore(0));
        QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), url1, 15000);
        while (widget.count() > 0)
            widget.closeTab(widget.count() - 1);
    }

    settings.remove(QLatin1String("performance/lazyRestoreTabs"));
    settings.remove(QLatin1String("performance/alwaysLoadPinnedTabs"));
}

// TABS12: rename — a per-tab custom title that replaces the page
// title on the strip, tooltip, window-title emit and hover preview;
// survives pin/group/container/undo-close churn; round-trips through
// the v6 session tail; the dblclick inline editor + context-menu
// entry commit on Enter (focus-out shares the same signal) and cancel
// on Esc; an empty commit clears the override.
void tst_TabWidget::renameTab()
{
    SubTabWidget widget;
    widget.newTab();
    widget.newTab();
    QCOMPARE(widget.count(), 2);
    WebView *v0 = widget.webView(0);
    WebView *v1 = widget.webView(1);
    QVERIFY(v0 && v1);

    const QUrl u1("data:text/html,<title>Page One</title>");
    widget.setCurrentIndex(0);          // newTab() selects each tab
    widget.loadUrl(u1, TabWidget::CurrentTab);
    QTRY_VERIFY_WITH_TIMEOUT(
        v0->title() == QLatin1String("Page One"), 15000);
    QCOMPARE(widget.tabText(0), QLatin1String("Page One"));
    QCOMPARE(widget.tabTitleOverride(0), QString());
    QCOMPARE(widget.tabDisplayTitle(0), QLatin1String("Page One"));

    // The override replaces the page title; the stored value is raw
    // while the strip text is mnemonic-escaped like any page title.
    widget.setTabTitleOverride(0, QLatin1String("My & Tab"));
    QCOMPARE(widget.tabTitleOverride(0), QLatin1String("My & Tab"));
    QCOMPARE(widget.tabDisplayTitle(0), QLatin1String("My & Tab"));
    QCOMPARE(widget.tabText(0), QLatin1String("My && Tab"));
    QVERIFY(widget.tabToolTip(0).contains(QLatin1String("&amp;")));

    // A later page title must not clobber the override — the page's
    // titleChanged lands on the same applyTabTitle path the rename
    // used, and the override wins there.
    const QUrl u2("data:text/html,<title>Page Two</title>");
    widget.loadUrl(u2, TabWidget::CurrentTab);
    QTRY_VERIFY_WITH_TIMEOUT(
        v0->title() == QLatin1String("Page Two"), 15000);
    QCOMPARE(widget.tabText(0), QLatin1String("My && Tab"));

    // Whitespace/empty clears the override and the live title returns.
    widget.setTabTitleOverride(0, QLatin1String("   "));
    QCOMPARE(widget.tabTitleOverride(0), QString());
    QCOMPARE(widget.tabText(0), QLatin1String("Page Two"));

    // Group collapse detaches the tab's page widget; expand reinserts
    // it — the keyed override rides both ways.
    widget.setTabTitleOverride(1, QLatin1String("Hide Me"));
    const QString gid = widget.createTabGroup(0);
    widget.addTabToGroup(1, gid);
    widget.setTabGroupCollapsed(gid, true);
    QCOMPARE(widget.count(), 1);
    widget.setTabGroupCollapsed(gid, false);
    QCOMPARE(widget.count(), 2);
    QCOMPARE(widget.webView(1), v1);
    QCOMPARE(widget.tabTitleOverride(1), QLatin1String("Hide Me"));
    QCOMPARE(widget.tabText(1), QLatin1String("Hide Me"));
    widget.ungroupTabs(gid);

    // Pin/unpin keeps it too (the override is page-widget keyed, the
    // same identity the pin flag uses).
    widget.setTabTitleOverride(0, QLatin1String("My & Tab"));
    widget.setTabPinned(0, true);
    QCOMPARE(widget.tabTitleOverride(0), QLatin1String("My & Tab"));
    widget.setTabPinned(0, false);

    // Undo-close carries the rename — the record snapshots it and the
    // restore re-keys it onto the replacement page.
    const QUrl undoUrl("data:text/plain,renamed-second");
    v1->loadUrl(undoUrl);
    QTRY_COMPARE_WITH_TIMEOUT(v1->url(), undoUrl, 15000);
    widget.closeTab(1);
    widget.openLastTab();
    int reopened = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.tabTitleOverride(i) == QLatin1String("Hide Me"))
            reopened = i;
    }
    QVERIFY(reopened >= 0);
    QCOMPARE(widget.tabText(reopened), QLatin1String("Hide Me"));

    // Reopen-in-container swaps the page widget — the override
    // re-keys onto the replacement.
    ContainerManager *manager = ContainerManager::instance();
    const QString cid = manager->createContainer(
        QLatin1String("RenameTest"), QColor(Qt::green)).id;
    QVERIFY(!cid.isEmpty());
    widget.reopenTabInContainer(reopened, cid);
    int containerIdx = -1;
    for (int i = 0; i < widget.count(); ++i) {
        if (widget.containerIdForTab(i) == cid)
            containerIdx = i;
    }
    QVERIFY(containerIdx >= 0);
    QCOMPARE(widget.tabTitleOverride(containerIdx),
             QLatin1String("Hide Me"));
    QCOMPARE(widget.tabText(containerIdx), QLatin1String("Hide Me"));

    // Close the container tab and delete the container (the deferred
    // view teardown must land before the one-shot deleteContainer).
    {
        QSignalSpy viewDied(widget.webView(containerIdx),
                            &QObject::destroyed);
        widget.closeTab(containerIdx);
        QTRY_VERIFY_WITH_TIMEOUT(viewDied.count() == 1, 5000);
    }
    QVERIFY(manager->deleteContainer(cid));

    // Session v6: the title tail round-trips the override, and the
    // restored page's own title never clobbers it.
    widget.setTabTitleOverride(0, QLatin1String("Saved Name"));
    const QByteArray state = widget.saveState();
    {
        SubTabWidget w2;
        QVERIFY(w2.restoreState(state));
        int saved = -1;
        for (int i = 0; i < w2.count(); ++i) {
            if (w2.tabTitleOverride(i) == QLatin1String("Saved Name"))
                saved = i;
        }
        QVERIFY(saved >= 0);
        QCOMPARE(w2.tabText(saved), QLatin1String("Saved Name"));
        QTRY_VERIFY_WITH_TIMEOUT(
            w2.webView(saved)->title() == QLatin1String("Page Two"),
            15000);
        QCOMPARE(w2.tabText(saved), QLatin1String("Saved Name"));
        while (w2.count() > 0)
            w2.closeTab(w2.count() - 1);
    }
    // A v5-era blob (no title tail) restores without overrides.
    {
        QByteArray v5blob;
        QDataStream out(&v5blob, QIODevice::WriteOnly);
        out << qint32(0xaa) << qint32(5)
            << (QStringList() << QStringLiteral("data:text/plain,v5"))
            << qint32(0)
            << (QList<QByteArray>() << QByteArray())
            << (QStringList() << QString())
            << (QStringList() << QString())
            << qint32(0)
            << QString()
            << (QList<qint32>() << 0);
        SubTabWidget w3;
        QVERIFY(w3.restoreState(v5blob));
        QCOMPARE(w3.count(), 1);
        QCOMPARE(w3.tabTitleOverride(0), QString());
        while (w3.count() > 0)
            w3.closeTab(w3.count() - 1);
    }

    // Inline editor: double-click on a row opens a QLineEdit over the
    // tab rect — Enter commits, Esc cancels.  (Focus-out commits too:
    // it shares QLineEdit::editingFinished with Enter.)  The prefill
    // is the current display title — set one up front so it is not
    // racing the start page's own title load.
    widget.newTab();
    QCOMPARE(widget.count(), 2);
    widget.setTabTitleOverride(1, QLatin1String("Before Edit"));
    widget.resize(720, 480);
    widget.show();
    QApplication::processEvents();
    TabBar *bar = widget.bar();
    const QPoint rowPos = bar->tabRect(1).center()
        + QPoint(0, bar->containerStripHeight());
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, rowPos);
    QLineEdit *editor =
        bar->findChild<QLineEdit*>(QLatin1String("tabTitleEditor"));
    QVERIFY(editor);
    QVERIFY(editor->isVisible());
    QCOMPARE(editor->text(), QLatin1String("Before Edit"));
    // The editor overlays the tab row (band-shifted in strip space).
    QVERIFY(editor->geometry().intersects(
        bar->tabRect(1).translated(0, bar->containerStripHeight())));
    QTest::keyClicks(editor, QLatin1String("Typed Name"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(widget.tabTitleOverride(1), QLatin1String("Typed Name"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !bar->findChild<QLineEdit*>(QLatin1String("tabTitleEditor")),
        5000);

    // Esc drops the edit — the committed override stays.
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, rowPos);
    editor = bar->findChild<QLineEdit*>(QLatin1String("tabTitleEditor"));
    QVERIFY(editor);
    QCOMPARE(editor->text(), QLatin1String("Typed Name"));
    editor->setText(QLatin1String("Discard Me"));
    QTest::keyClick(editor, Qt::Key_Escape);
    QTRY_VERIFY_WITH_TIMEOUT(
        !bar->findChild<QLineEdit*>(QLatin1String("tabTitleEditor")),
        5000);
    QCOMPARE(widget.tabTitleOverride(1), QLatin1String("Typed Name"));

    // The context menu's "Rename Tab..." opens the same editor.  The
    // menu is a popup exec()'d on the stack — it shows up via
    // activePopupWidget(), not the modal stack.
    QAction *renameAction = nullptr;
    QTimer::singleShot(50, &widget, [&renameAction]() {
        QMenu *menu =
            qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (!menu) {
            const auto tops = QApplication::topLevelWidgets();
            for (QWidget *w : tops) {
                if (QMenu *m = qobject_cast<QMenu*>(w);
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
            if (action->text().contains(QLatin1String("name Tab")))
                renameAction = action;
        }
        if (renameAction)
            renameAction->trigger();
        else
            menu->close();
    });
    // Belt & braces: never let the exec() outlive this leg.
    QTimer::singleShot(5000, &widget, []() {
        const auto tops = QApplication::topLevelWidgets();
        for (QWidget *w : tops) {
            if (QMenu *m = qobject_cast<QMenu*>(w); m && m->isVisible())
                m->close();
        }
    });
    QMetaObject::invokeMethod(bar, "contextMenuRequested",
                              Q_ARG(QPoint, rowPos));
    QVERIFY(renameAction);
    editor = bar->findChild<QLineEdit*>(QLatin1String("tabTitleEditor"));
    QVERIFY(editor);
    editor->setText(QLatin1String("Menu Named"));
    QTest::keyClick(editor, Qt::Key_Return);
    QCOMPARE(widget.tabTitleOverride(1), QLatin1String("Menu Named"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !bar->findChild<QLineEdit*>(QLatin1String("tabTitleEditor")),
        5000);

    // Same gesture on a vertical (West) strip.
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        settings.setValue(QLatin1String("tabBarPosition"), 2);   // Left
        SubTabWidget wv;
        wv.newTab();
        wv.newTab();
        wv.resize(400, 480);
        wv.show();
        QApplication::processEvents();
        TabBar *vbar = wv.bar();
        const QPoint vpos = vbar->tabRect(0).center()
            + QPoint(0, vbar->containerStripHeight());
        QTest::mouseDClick(vbar, Qt::LeftButton, Qt::NoModifier, vpos);
        QLineEdit *veditor = vbar->findChild<QLineEdit*>(
            QLatin1String("tabTitleEditor"));
        QVERIFY(veditor);
        veditor->setText(QLatin1String("Vertical Name"));
        QTest::keyClick(veditor, Qt::Key_Return);
        QCOMPARE(wv.tabTitleOverride(0),
                 QLatin1String("Vertical Name"));
        QCOMPARE(wv.tabText(0), QLatin1String("Vertical Name"));
        while (wv.count() > 0)
            wv.closeTab(wv.count() - 1);
        settings.remove(QLatin1String("tabBarPosition"));
        settings.endGroup();
    }

    while (widget.count() > 0)
        widget.closeTab(widget.count() - 1);
}

// TABS13: cross-window tab transfer — moveTabTo() carries the live
// page widget (history/scroll/engine state ride along), the carried
// pin/rename/group metadata re-keys in the target, the privacy
// boundary refuses a persistent page into a private-mode window, and
// a synthesized drop on the target bar drives the full merge path
// including the mime/session handshake.
void tst_TabWidget::tabTransfer()
{
    SubTabWidget source;
    for (int i = 0; i < 3; ++i)
        source.newTab();
    SubTabWidget target;
    for (int i = 0; i < 2; ++i)
        target.newTab();
    QCOMPARE(source.count(), 3);
    QCOMPARE(target.count(), 2);
    WebView *s0 = source.webView(0);
    WebView *s1 = source.webView(1);
    WebView *s2 = source.webView(2);
    WebView *t0 = target.webView(0);
    WebView *t1 = target.webView(1);
    QVERIFY(s0 && s1 && s2 && t0 && t1);

    // The drop slot is honored — the same page widget lands at the
    // drop index (move, not clone) and raises current.
    QWidget *movedPage = source.widget(1);
    QVERIFY(source.moveTabTo(1, &target, 1));
    QCOMPARE(source.count(), 2);
    QCOMPARE(source.webView(0), s0);
    QCOMPARE(source.webView(1), s2);
    QCOMPARE(target.count(), 3);
    QCOMPARE(target.webView(0), t0);
    QCOMPARE(target.webView(1), s1);
    QCOMPARE(target.webView(2), t1);
    QCOMPARE(target.widget(1), movedPage);
    QCOMPARE(target.currentIndex(), 1);
    // The strip/location-bar stacks stay paired.
    QVERIFY(target.locationBar(1));
    QCOMPARE(target.count(), 3);

    // The moved view's signals land on the new owner — the override
    // path repaints the target strip.
    target.setTabTitleOverride(1, QLatin1String("Moved"));
    QCOMPARE(target.tabTitleOverride(1), QLatin1String("Moved"));

    // Metadata rides the carrier: pin + rename + group binding.
    source.setTabPinned(0, true);
    source.setTabTitleOverride(0, QLatin1String("Carried"));
    const QString gid = source.createTabGroup(0);
    source.renameTabGroup(gid, QLatin1String("Convoy"));
    source.setTabGroupColor(gid, QColor(Qt::blue));
    QCOMPARE(source.tabGroupId(0), gid);
    const QUrl markerUrl(QStringLiteral("data:text/plain,carried"));
    s0->loadUrl(markerUrl);
    QTRY_COMPARE_WITH_TIMEOUT(s0->url(), markerUrl, 15000);
    QVERIFY(source.moveTabTo(0, &target, 0));
    QCOMPARE(source.count(), 1);
    QCOMPARE(source.webView(0), s2);
    QCOMPARE(target.count(), 4);
    QCOMPARE(target.webView(0), s0);
    QVERIFY(target.isTabPinned(0));
    QCOMPARE(target.tabTitleOverride(0), QLatin1String("Carried"));
    QCOMPARE(target.tabDisplayTitle(0), QLatin1String("Carried"));
    QCOMPARE(target.webView(0)->url(), markerUrl);
    // Group ids are per-window: the carried binding lands under a
    // fresh id but keeps the name and color.
    const QString newGid = target.tabGroupId(0);
    QVERIFY(!newGid.isEmpty());
    QCOMPARE(target.tabGroupName(newGid), QLatin1String("Convoy"));
    QCOMPARE(target.tabGroupColor(newGid), QColor(Qt::blue));
    // The emptied source group is gone.
    QVERIFY(!source.tabGroupIds().contains(gid));

    // Refusals: a marked no-transfer page (a future engine backend's
    // host widget) stays put.
    QWidget *marked = source.widget(0);
    marked->setProperty("aroraNoTabTransfer", true);
    QVERIFY(!source.moveTabTo(0, &target));
    QCOMPARE(source.count(), 1);
    QCOMPARE(target.count(), 4);
    marked->setProperty("aroraNoTabTransfer", QVariant());

    // OTR boundary: a private page keeps its off-the-record profile
    // through the move; a persistent page is refused while the
    // process runs private (the window it would land in is treated
    // as throwaway).
    WebView *priv = source.makeNewPrivateTab(true);
    QVERIFY(priv);
    QVERIFY(priv->enginePage()->isOffTheRecord());
    const int privIndex = source.webViewIndex(priv);
    QVERIFY(source.moveTabTo(privIndex, &target, -1));
    QCOMPARE(target.webView(target.count() - 1), priv);
    QVERIFY(target.webView(target.count() - 1)->enginePage()
            ->isOffTheRecord());

    BrowserApplication::setPrivate(true);
    QVERIFY(!source.moveTabTo(0, &target));
    QCOMPARE(source.count(), 1);
    BrowserApplication::setPrivate(false);

    // Same-widget move is a no-op — reorder is the bar's job.
    QVERIFY(!source.moveTabTo(0, &source));
    QVERIFY(!source.moveTabTo(0, nullptr));
    QVERIFY(!source.moveTabTo(-1, &target));
    QVERIFY(!source.moveTabTo(9, &target));

    // The full drop path: a live drag session + arora-tab mime over
    // the target bar inserts at the drop slot instead of loading the
    // url into the hovered tab.
    source.newTab();
    QCOMPARE(source.count(), 2);
    WebView *d0 = source.webView(0);
    WebView *d1 = source.webView(1);
    target.resize(720, 480);
    target.show();
    QApplication::processEvents();
    TabBar *sourceBar = source.bar();
    TabBar *targetBar = target.bar();
    QWidget *dragged = source.widget(0);
    TabBar::beginTabDragSession(sourceBar, dragged);
    QMimeData mime;
    mime.setUrls(QList<QUrl>() << QUrl(QStringLiteral("https://a.example/")));
    mime.setText(QStringLiteral("https://a.example/"));
    mime.setData(QLatin1String(aroraTabMimeType()),
                 QByteArray::number(quintptr(sourceBar)) + ':'
                 + QByteArray::number(quintptr(dragged)));
    // A drop on the tab-2 midpoint inserts there.
    const QPoint dropPos = targetBar->tabRect(2).center()
        + QPoint(0, targetBar->containerStripHeight());
    QSignalSpy mergeLoadSpy(targetBar,
        QOverload<const QUrl &, TabWidget::OpenUrlIn>::of(&TabBar::loadUrl));
    QDropEvent drop(QPointF(dropPos), Qt::MoveAction, &mime,
                    Qt::LeftButton, Qt::NoModifier);
    targetBar->testDropEvent(&drop);
    QCOMPARE(drop.isAccepted(), true);
    QCOMPARE(mergeLoadSpy.count(), 0);
    TabBar::endTabDragSession();
    // The tab landed at the drop slot — the hovered tab's url is
    // untouched, the source lost the page, and the url payload was
    // not turned into a navigation.
    QCOMPARE(target.count(), 6);
    QCOMPARE(source.count(), 1);
    QCOMPARE(source.webView(0), d1);
    QCOMPARE(target.widget(2), dragged);
    QCOMPARE(target.webView(2), d0);
    QCOMPARE(target.webView(3), s1);
    QVERIFY(target.webView(3)->url()
            != QUrl(QStringLiteral("https://a.example/")));

    // A stale payload (no live session) falls back to the legacy url
    // drop — foreign drops still open the link.
    QSignalSpy loadSpy(targetBar,
        QOverload<const QUrl &, TabWidget::OpenUrlIn>::of(&TabBar::loadUrl));
    QMimeData foreign;
    foreign.setUrls(QList<QUrl>() << QUrl(QStringLiteral("https://b.example/")));
    foreign.setData(QLatin1String(aroraTabMimeType()),
                    QByteArrayLiteral("1234:5678"));
    QDropEvent staleDrop(QPointF(dropPos), Qt::MoveAction, &foreign,
                         Qt::LeftButton, Qt::NoModifier);
    targetBar->testDropEvent(&staleDrop);
    QCOMPARE(loadSpy.count(), 1);
    QCOMPARE(target.count(), 6);          // nothing moved

    // Tear-off: the driver behind an unhandled release — the page
    // lands live in a fresh BrowserMainWindow, and the ctor's blank
    // placeholder tab is closed behind it.
    source.newTab();
    QCOMPARE(source.count(), 2);
    WebView *torn = source.webView(1);
    source.tearTabToNewWindow(1);
    QCOMPARE(source.count(), 1);
    // The leg's earlier NewWindow drops leave other browser windows
    // on the top-level list — pick the one that actually holds the
    // torn view, not just the last BrowserMainWindow.
    BrowserMainWindow *tornWindow = nullptr;
    for (QWidget *top : QApplication::topLevelWidgets()) {
        BrowserMainWindow *mw = qobject_cast<BrowserMainWindow*>(top);
        if (!mw || !mw->tabWidget())
            continue;
        for (int i = 0; i < mw->tabWidget()->count(); ++i) {
            if (mw->tabWidget()->webView(i) == torn)
                tornWindow = mw;
        }
    }
    QVERIFY(tornWindow);
    QCOMPARE(tornWindow->tabWidget()->count(), 1);
    QCOMPARE(tornWindow->tabWidget()->webView(0), torn);
    delete tornWindow;

    while (source.count() > 0)
        source.closeTab(source.count() - 1);
    while (target.count() > 0)
        target.closeTab(target.count() - 1);
}

// TABS15: the strip's own '+' opens a real tab — the bar's newTab
// signal routes through the same slot as the Ctrl+T action — and
// it stays docked under the last row when the bar goes vertical
// (the corner widget it replaced never rendered there at all).
void tst_TabWidget::newTabButton()
{
    SubTabWidget widget;
    widget.newTab();
    widget.newTab();
    widget.resize(640, 480);
    widget.show();
    QApplication::processEvents();

    TabBar *bar = widget.bar();
    QToolButton *button = bar->newTabButton();
    QVERIFY(button);
    QVERIFY(button->isVisible());
    // The horizontal anchor: right after the last tab, not at the
    // strip's far edge.
    const QRect last = bar->tabRect(bar->count() - 1);
    QVERIFY(button->x() >= last.right());
    QVERIFY(button->x() <= last.right() + 8);

    QCOMPARE(widget.count(), 2);
    button->click();
    QApplication::processEvents();
    QCOMPARE(widget.count(), 3);

    // Left rail — an appended row under the last tab.
    QSettings settings;
    settings.beginGroup(QLatin1String("tabs"));
    settings.setValue(QLatin1String("tabBarPosition"), 2);
    widget.loadSettings();
    QApplication::processEvents();
    QCOMPARE(int(widget.tabPosition()), int(QTabWidget::West));
    QVERIFY(button->isVisible());
    const QRect vlast = bar->tabRect(bar->count() - 1);
    QVERIFY(button->y() >= vlast.bottom());

    // The click still opens a tab from the rail too.
    const int count = widget.count();
    button->click();
    QApplication::processEvents();
    QCOMPARE(widget.count(), count + 1);

    // Offscreen grabs of the real chrome for visual review — set
    // ARORA_TABWIDGET_GRAB_DIR.
    const QByteArray grabDir = qgetenv("ARORA_TABWIDGET_GRAB_DIR");
    if (!grabDir.isEmpty()) {
        const QString base = QString::fromLocal8Bit(grabDir);
        bar->grab().save(base + QLatin1String("/plus-rail.png"));
        settings.setValue(QLatin1String("tabBarPosition"), 0);
        widget.loadSettings();
        QApplication::processEvents();
        bar->grab().save(base + QLatin1String("/plus-strip.png"));
    }

    settings.remove(QLatin1String("tabBarPosition"));
    settings.endGroup();
}

// TABS17: on a Left/Right rail every row's title paints fully inside
// the row's readable band — the rect minus the name-pill top a group
// run's first member reserves.  A production-mix rail (favicon side
// widgets, close buttons, a real icon on the History widget tab, a
// pinned tab, a renamed tab, a named group) is the repro that used
// to center the grouped title on the whole rect — its baseline fell
// against the row's clipped bottom edge while the pill floated over
// dead space.  Legs run on both rails, on the dark palette, and on
// the tor window's violet stylesheet chrome.
void tst_TabWidget::verticalTabRowTextBounds()
{
    // Build the production-mix rail and return its bar.
    const auto buildRail = [](SubTabWidget &widget) -> TabBar * {
        for (int i = 0; i < 3; ++i)
            widget.newTab();
        TabBar *bar = widget.bar();
        // Production favicon labels on freeSide.
        for (int i = 0; i < widget.count(); ++i) {
            QLabel *label = new QLabel(&widget);
            label->setFixedSize(16, 16);
            QPixmap pm(16, 16);
            pm.fill(QColor(40 + i * 60, 90, 200 - i * 50));
            label->setPixmap(pm);
            bar->setTabButton(i, bar->freeSide(), label);
        }
        // The History widget tab — real strip icon, no favicon label.
        QWidget *historyPage = new QWidget(&widget);
        widget.addWidgetTab(
            historyPage, tr("History"),
            QIcon(QLatin1String(":graphics/history.png")));
        widget.setTabPinned(0, true);
        widget.setTabTitleOverride(
            1, tr("Renamed tab — gypq descenders"));
        const QString gid = widget.createTabGroup(2);
        widget.renameTabGroup(gid, tr("Group"));
        widget.addTabToGroup(3, gid);
        widget.resize(720, 480);
        widget.show();
        QApplication::processEvents();
        widget.loadSettings();
        QApplication::processEvents();
        return bar;
    };

    // Ink = pixels whose luminance sits far from the row's dominant
    // background — one detector serves light text on violet, dark
    // text on light, and the dark palette's light-on-dark titles.
    const auto inkBounds = [](const QImage &image, const QRect &lane,
                              int bandTop, int bandBottom,
                              int &inkTop, int &inkBottom) {
        inkTop = inkBottom = -1;
        QHash<QRgb, int> frequency;
        for (int y = bandTop; y <= bandBottom && y < image.height(); ++y)
            for (int x = lane.left();
                 x <= lane.right() && x < image.width(); ++x)
                ++frequency[image.pixel(x, y)];
        int bgLum = 0, most = 0;
        for (auto it = frequency.constBegin();
             it != frequency.constEnd(); ++it) {
            if (it.value() > most) {
                most = it.value();
                bgLum = qGray(it.key());
            }
        }
        // A scanline where almost every lane pixel carries ink is a
        // border/separator, not text — glyph strokes never fill the
        // lane.  Skip those so the bounds measure the title alone.
        const int laneWidth = lane.width();
        for (int y = bandTop; y <= bandBottom && y < image.height(); ++y) {
            int ink = 0;
            for (int x = lane.left();
                 x <= lane.right() && x < image.width(); ++x) {
                if (qAbs(qGray(image.pixel(x, y)) - bgLum) > 60)
                    ++ink;
            }
            if (ink >= 2 && ink < laneWidth * 7 / 10) {
                if (inkTop < 0)
                    inkTop = y;
                inkBottom = y;
            }
        }
    };

    const auto verifyRail = [&inkBounds](TabBar *bar, const QString &leg,
                                         QImage &image) {
        image = bar->grab().toImage();
        // TABS19: the uniform row face is content + 17 (~33px) — the
        // reserve above a group run's pill is whatever exceeds it.
        const int bandFloor = qMax(bar->fontMetrics().height(),
                                   bar->iconSize().height()) + 17;
        for (int i = 0; i < bar->count(); ++i) {
            const QRect row = bar->tabRect(i);
            QVERIFY2(row.height() >= bandFloor,
                     qPrintable(QStringLiteral(
                         "%1: tab %2 row %3 shorter than the %4px "
                         "floor")
                         .arg(leg).arg(i).arg(row.height())
                         .arg(bandFloor)));
            // A group run's first member reserves its top for the
            // name pill; the label band sits below it.
            const int reserve = qMax(0, row.height() - bandFloor);
            const int bandTop = row.top() + reserve;
            // Title ink: inside the text lane — clear of the favicon's
            // leading zone and the close button's trailing one.
            const QRect lane(40, 0, qMax(1, row.width() - 76), 0);
            int inkTop, inkBottom;
            inkBounds(image, lane, bandTop, row.bottom(),
                      inkTop, inkBottom);
            QVERIFY2(inkTop >= 0,
                     qPrintable(QStringLiteral(
                         "%1: tab %2 (%3) painted no title ink")
                         .arg(leg).arg(i).arg(bar->tabText(i))));
            QVERIFY2(inkTop > bandTop,
                     qPrintable(QStringLiteral(
                         "%1: tab %2 title ink starts on the band's "
                         "top edge — the reserve overlaps or the "
                         "baseline is high (top %3, band %4)")
                         .arg(leg).arg(i).arg(inkTop).arg(bandTop)));
            QVERIFY2(inkBottom < row.bottom(),
                     qPrintable(QStringLiteral(
                         "%1: tab %2 title ink reaches the row's "
                         "bottom edge — clipped descenders "
                         "(bottom %3, row %4)")
                         .arg(leg).arg(i).arg(inkBottom)
                         .arg(row.bottom())));
            // Centered in the band, within the style's selected-tab
            // nudge — symmetric padding is the readable outcome.
            const int bandCenter = (bandTop + row.bottom()) / 2;
            const int inkCenter = (inkTop + inkBottom) / 2;
            QVERIFY2(qAbs(inkCenter - bandCenter) <= 4,
                     qPrintable(QStringLiteral(
                         "%1: tab %2 title center %3 drifts from "
                         "the band's center %4")
                         .arg(leg).arg(i).arg(inkCenter)
                         .arg(bandCenter)));

            // Side widgets ride the same band — a group pill's
            // reserve is not theirs to sit under.
            QWidget *favicon = bar->tabButton(i, bar->freeSide());
            if (favicon) {
                QVERIFY2(favicon->geometry().center().y() > bandTop
                         && favicon->geometry().center().y()
                                < row.bottom(),
                         qPrintable(QStringLiteral(
                             "%1: tab %2 side widget centers outside "
                             "the label band").arg(leg).arg(i)));
            }
        }
    };

    for (const int position : { 2, 3 }) {   // Left rail, Right rail
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        settings.setValue(QLatin1String("tabBarPosition"), position);
        settings.remove(QLatin1String("verticalTabWidth"));

        SubTabWidget widget;
        TabBar *bar = buildRail(widget);
        QImage light;
        verifyRail(bar, QStringLiteral("rail %1").arg(position),
                   light);

        // The tor window's violet chrome is a per-bar stylesheet —
        // QStyleSheetStyle's own box model lays the label out, so the
        // band contract gets a second, independent exercise.  The CSS
        // mirrors torTabBarStyleSheet() in browsermainwindow.cpp.
        bar->setStyleSheet(QLatin1String(
            "QTabBar { background: #4a3372; }"
            "QTabBar::tab { background: #5d4491; color: #eee;"
            " border: 1px solid #3a2a5c; padding: 6px 8px; }"
            "QTabBar::tab:left { border-right: none;"
            " border-top-left-radius: 4px;"
            " border-bottom-left-radius: 4px; }"
            "QTabBar::tab:right { border-left: none;"
            " border-top-right-radius: 4px;"
            " border-bottom-right-radius: 4px; }"
            "QTabBar::tab:selected { background: #ffffff;"
            " color: #202020; }"
            "QTabBar::tab:!selected:hover { background: #6b4fa3; }"));
        QApplication::processEvents();
        QImage violet;
        verifyRail(bar, QStringLiteral("violet %1").arg(position),
                   violet);

        const QByteArray grabDir = qgetenv("ARORA_TABWIDGET_GRAB_DIR");
        if (!grabDir.isEmpty()) {
            const QString base = QString::fromLocal8Bit(grabDir)
                + (position == 2 ? QLatin1String("/rows-left")
                                 : QLatin1String("/rows-right"));
            light.save(base + QLatin1String(".png"));
            violet.save(base + QLatin1String("-violet.png"));
        }

        settings.remove(QLatin1String("tabBarPosition"));
        settings.endGroup();
    }

    // The forced-dark palette — the same text-on-dark contract the
    // violet leg exercises through a different paint path.
    const QPalette appPalette = QApplication::palette();
    QApplication::setPalette(BrowserTheme::darkPalette());
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        settings.setValue(QLatin1String("tabBarPosition"), 2);
        SubTabWidget widget;
        TabBar *bar = buildRail(widget);
        QImage dark;
        verifyRail(bar, QStringLiteral("dark"), dark);
        const QByteArray grabDir = qgetenv("ARORA_TABWIDGET_GRAB_DIR");
        if (!grabDir.isEmpty())
            dark.save(QString::fromLocal8Bit(grabDir)
                      + QLatin1String("/rows-dark.png"));
        settings.remove(QLatin1String("tabBarPosition"));
        settings.endGroup();
    }
    QApplication::setPalette(appPalette);
}

// TABS18: a vertical rail whose rows fill the strip left the '+'
// no free run — it clamped onto the row the rail was scrolled to
// and read as a truncated tab (the user's clipped "History" row).
// The button now lives inside the trailing scroll chrome instead:
// stacked arrows share the band's first row and the '+' takes the
// row below.  Every row keeps its full painted height on both
// rails, no bar chrome covers a row, and the '+' still opens tabs.
void tst_TabWidget::verticalRailRowOverlap()
{
    const auto buildOverflowedRail =
        [](SubTabWidget &widget) -> TabBar * {
        for (int i = 0; i < 40; ++i)
            widget.newTab();
        TabBar *bar = widget.bar();
        bar->setPerTabCloseButtons(true);
        // Favicon labels like production installs.
        for (int i = 0; i < widget.count(); ++i) {
            QLabel *label = new QLabel(&widget);
            label->setFixedSize(16, 16);
            QPixmap pm(16, 16);
            pm.fill(QColor(40 + i * 60 % 220, 90,
                           200 - i * 40 % 150));
            label->setPixmap(pm);
            bar->setTabButton(i, bar->freeSide(), label);
        }
        widget.setTabPinned(0, true);
        widget.setTabTitleOverride(1, tr("Renamed tab — gypq"));
        const QString gid = widget.createTabGroup(2);
        widget.renameTabGroup(gid, tr("Group"));
        widget.addTabToGroup(3, gid);
        // The user's clipped row — a History widget tab appended
        // last: real strip icon, no favicon label.
        QWidget *historyPage = new QWidget(&widget);
        widget.addWidgetTab(
            historyPage, tr("History"),
            QIcon(QLatin1String(":graphics/history.png")));
        widget.resize(720, 480);
        widget.show();
        QApplication::processEvents();
        widget.loadSettings();
        QApplication::processEvents();
        return bar;
    };

    for (const int position : { 2, 3 }) {   // Left rail, Right rail
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        settings.setValue(QLatin1String("tabBarPosition"), position);
        settings.remove(QLatin1String("verticalTabWidth"));

        SubTabWidget widget;
        TabBar *bar = buildOverflowedRail(widget);
        const int band = bar->containerStripHeight();
        // Overflowed — Qt's scroll chrome is up.
        QList<QAbstractButton*> chrome;
        const QObjectList kids = bar->children();
        for (QObject *kid : kids) {
            QAbstractButton *child =
                qobject_cast<QAbstractButton*>(kid);
            if (!child || !child->isVisible()
                || child == bar->newTabButton())
                continue;
            bool isTabButton = false;
            for (int i = 0; i < bar->count() && !isTabButton; ++i) {
                if (bar->tabButton(i, QTabBar::LeftSide) == child
                    || bar->tabButton(i, QTabBar::RightSide) == child)
                    isTabButton = true;
            }
            if (!isTabButton)
                chrome.append(child);
        }
        QVERIFY2(!chrome.isEmpty(),
                 "overflowed rail did not raise the scroll chrome");

        // Every visible row shares one height — group runs carry
        // their pill reserve only on the first member.
        const int bandFloor = bar->fontMetrics().height() + 8;
        int plainHeight = -1;
        for (int i = 0; i < bar->count(); ++i) {
            const QRect row = bar->tabRect(i);
            if (row.bottom() < band || row.top() > bar->height())
                continue;   // scrolled out
            QVERIFY2(row.height() >= bandFloor,
                     qPrintable(QStringLiteral(
                         "rail %1: tab %2 row %3px under the %4px "
                         "floor")
                         .arg(position).arg(i).arg(row.height())
                         .arg(bandFloor)));
            const QString gid = widget.tabGroupId(i);
            const bool reserves =
                !gid.isEmpty()
                && !widget.tabGroupName(gid).isEmpty()
                && widget.tabGroupId(i - 1) != gid;
            if (!reserves) {
                if (plainHeight < 0)
                    plainHeight = row.height();
                QVERIFY2(row.height() == plainHeight,
                         qPrintable(QStringLiteral(
                             "rail %1: tab %2 row %3px vs rail "
                             "height %4px")
                             .arg(position).arg(i).arg(row.height())
                             .arg(plainHeight)));
            }
        }

        // No strip chrome covers the visible part of a row — the
        // '+' and the scroll buttons are the TABS18 offenders.  Qt
        // lets a scrolled row's tail flow under the chrome band, so
        // only the part above the band's top must stay uncovered.
        QToolButton *plus = bar->newTabButton();
        QVERIFY(plus->isVisible());
        QVERIFY(bar->rect().contains(plus->geometry()));
        int chromeTop = bar->height();
        for (QAbstractButton *child : std::as_const(chrome))
            chromeTop = qMin(chromeTop, child->geometry().top());
        const QRect visibleArea(0, band, bar->width(),
                                chromeTop - band);
        QList<QWidget*> overlays;
        for (QAbstractButton *child : std::as_const(chrome))
            overlays.append(child);
        overlays.append(plus);
        for (int i = 0; i < bar->count(); ++i) {
            const QRect row = bar->tabRect(i)
                .translated(0, band).intersected(visibleArea);
            if (row.isEmpty())
                continue;
            for (QWidget *overlay : std::as_const(overlays)) {
                QVERIFY2(!overlay->geometry().intersects(row),
                         qPrintable(QStringLiteral(
                             "rail %1: %2 %3 covers tab %4's row %5")
                             .arg(position)
                             .arg(QLatin1String(
                                 overlay->metaObject()->className()))
                             .arg(QLatin1String(QTest::toString(
                                 overlay->geometry())))
                             .arg(i).arg(QLatin1String(
                                 QTest::toString(row)))));
            }
        }
        // The '+' lives inside the chrome band it borrowed — never
        // floating over the rows' painted area.
        QVERIFY(plus->geometry().top() >= chromeTop);

        // The reflowed chrome still scrolls — the down arrow drives
        // the strip — and the '+' still opens a real tab.
        const int count = widget.count();
        plus->click();
        QApplication::processEvents();
        QCOMPARE(widget.count(), count + 1);

        if (qEnvironmentVariableIsSet("ARORA_TABWIDGET_GRAB_DIR")) {
            const QString dir =
                qEnvironmentVariable("ARORA_TABWIDGET_GRAB_DIR");
            bar->grab().save(dir + (position == 2
                ? QLatin1String("/rail-overlap-left.png")
                : QLatin1String("/rail-overlap-right.png")));
        }

        settings.remove(QLatin1String("tabBarPosition"));
        settings.endGroup();
    }

    // A rail exactly full — the rows reach the strip's end but no
    // scroll chrome ever engaged: the '+' hides rather than cover
    // the last row.
    {
        QSettings settings;
        settings.beginGroup(QLatin1String("tabs"));
        settings.setValue(QLatin1String("tabBarPosition"), 2);
        settings.remove(QLatin1String("verticalTabWidth"));

        SubTabWidget widget;
        widget.newTab();
        TabBar *bar = widget.bar();
        widget.resize(720, 480);
        widget.show();
        QApplication::processEvents();
        widget.loadSettings();
        QApplication::processEvents();
        const int rowHeight = bar->tabRect(0).height();
        const int rows = bar->height() / rowHeight;
        while (widget.count() < rows)
            widget.newTab();
        QApplication::processEvents();
        QTRY_VERIFY_WITH_TIMEOUT(
            bar->tabRect(widget.count() - 1).bottom()
                >= bar->height() - rowHeight,
            5000);
        for (int i = 0; i < bar->count(); ++i) {
            const QRect row = bar->tabRect(i)
                .translated(0, bar->containerStripHeight());
            if (!row.isValid())
                continue;
            QVERIFY2(!bar->newTabButton()->isVisible()
                         || !bar->newTabButton()->geometry()
                                .intersects(row),
                     qPrintable(QStringLiteral(
                         "exactly-full rail: '+' %1 covers tab %2 "
                         "row %3")
                         .arg(QLatin1String(QTest::toString(
                             bar->newTabButton()->geometry())))
                         .arg(i)
                         .arg(QLatin1String(QTest::toString(row)))));
        }
        settings.remove(QLatin1String("tabBarPosition"));
        settings.endGroup();
    }
}

// TABS20: with the CONT06 level-1 band up, the favicon label floated
// up into the header band — its top edge sat above the tab face's top
// border.  Qt repositions per-tab side buttons in strip space on
// several paths that never reach tabLayoutChange() (setCurrentIndex,
// move drags, scroll reflows), so the band shift has to re-derive the
// anchor, not incrementally offset: the button's strip-space y comes
// from the style's sub-element rect and the band is added on top.
// Asserts the favicon sits inside the painted face with symmetric
// margins — at Automatic height, at a dragged height, and after a
// tab switch.
void tst_TabWidget::tabButtonBandAnchor()
{
    QSettings settings;
    settings.setValue(QLatin1String("tabs/containerDisplay"), 1);
    settings.beginGroup(QLatin1String("tabs"));
    settings.remove(QLatin1String("tabBarPosition"));
    settings.remove(QLatin1String("horizontalTabHeight"));
    settings.endGroup();

    ContainerManager *manager = ContainerManager::instance();
    const QString cid = manager->createContainer(
        tr("Work"), QColor(0x37, 0xad, 0xff)).id;

    SubTabWidget widget;
    for (int i = 0; i < 3; ++i)
        widget.newTab();
    // A second level's tab raises the header band; it stays filtered
    // off the visible row.
    widget.makeNewTabInContainer(cid, false);
    TabBar *bar = widget.bar();
    bar->setPerTabCloseButtons(true);
    for (int i = 0; i < widget.count(); ++i) {
        QLabel *label = new QLabel(&widget);
        label->setFixedSize(16, 16);
        QPixmap pm(16, 16);
        pm.fill(QColor(200, 40 + i * 60, 90));
        label->setPixmap(pm);
        bar->setTabButton(i, bar->freeSide(), label);
    }
    widget.resize(720, 480);
    widget.show();
    QApplication::processEvents();

    const int band = bar->containerStripHeight();
    QVERIFY(band > 0);

    const auto verifyAnchors = [&widget, &bar,
                                band](const QString &leg) {
        for (int i = 0; i < bar->count(); ++i) {
            // The face paints the strip-space rect translated down by
            // the band — the physical rect the buttons must sit in.
            const QRect face = bar->tabRect(i).translated(0, band);
            QWidget *favicon = bar->tabButton(i, bar->freeSide());
            QVERIFY2(favicon,
                     qPrintable(QStringLiteral("%1: tab %2 has no "
                                "favicon widget").arg(leg).arg(i)));
            QVERIFY2(face.contains(favicon->geometry()),
                     qPrintable(QStringLiteral(
                         "%1: tab %2 favicon %3 floats outside the "
                         "face %4")
                         .arg(leg).arg(i)
                         .arg(QLatin1String(QTest::toString(
                             favicon->geometry())))
                         .arg(QLatin1String(QTest::toString(face)))));
            // Optically centered: top and bottom margins differ by
            // the style's selected-tab nudge at most.
            const int topMargin = favicon->geometry().top() - face.top();
            const int bottomMargin =
                face.bottom() - favicon->geometry().bottom();
            QVERIFY2(qAbs(topMargin - bottomMargin) <= 4,
                     qPrintable(QStringLiteral(
                         "%1: tab %2 favicon margins %3/%4 not "
                         "symmetric")
                         .arg(leg).arg(i).arg(topMargin)
                         .arg(bottomMargin)));
        }
    };

    verifyAnchors(QStringLiteral("initial"));

    // setCurrentIndex re-lays only the two touched tabs' buttons in
    // strip space — the path that parked the favicon in the band.
    widget.setCurrentIndex(0);
    widget.setCurrentIndex(1);
    QApplication::processEvents();
    verifyAnchors(QStringLiteral("after tab switch"));

    // A dragged-in strip height grew past the band: the strip-space
    // button y could land >= band and the old incremental shift would
    // skip it entirely.
    bar->setTabStripHeight(56);
    widget.resize(720, 480);
    QApplication::processEvents();
    verifyAnchors(QStringLiteral("dragged height 56"));

    // Back to Automatic.
    bar->setTabStripHeight(0);
    QApplication::processEvents();
    verifyAnchors(QStringLiteral("automatic again"));

    const QByteArray grabDir = qgetenv("ARORA_TABWIDGET_GRAB_DIR");
    if (!grabDir.isEmpty())
        bar->grab().save(QString::fromLocal8Bit(grabDir)
                         + QLatin1String("/band-anchor.png"));

    settings.remove(QLatin1String("tabs/containerDisplay"));
}

QTEST_MAIN(tst_TabWidget)
#include "tst_tabwidget.moc"

