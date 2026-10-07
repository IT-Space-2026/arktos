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

#include <tabwidget.h>
#include <tabbar.h>
#include <webview.h>

#include <opensearchmanager.h>
#include <opensearchengine.h>
#include <privacyrequestinterceptor.h>
#include <toolbarsearch.h>

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
    void omnibox();
};

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
}

void tst_TabWidget::tabwidget_data()
{
}

void tst_TabWidget::tabwidget()
{
    SubTabWidget widget;
    widget.addWebAction((QAction*)0, QWebEnginePage::Back);
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

Q_DECLARE_METATYPE(QWebEnginePage::WebAction)
void tst_TabWidget::addWebAction_data()
{
    QTest::addColumn<QWebEnginePage::WebAction>("webAction");
    QTest::newRow("back") << QWebEnginePage::Back;
}

// public void addWebAction(QAction *action, QWebEnginePage::WebAction webAction)
void tst_TabWidget::addWebAction()
{
    QFETCH(QWebEnginePage::WebAction, webAction);

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
    QCOMPARE(widget.webView(0)->history()->count(), 2);
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

// SRCH01: the location bar is an omnibox — guessUrlFromString (driven
// through loadString) must search for text that is not address-shaped
// instead of guessing http://, while address-shaped input still
// navigates.  'keyword terms' shortcuts keep first priority and the
// searchEngineFallback opt-out restores the historic bare-http guess.
void tst_TabWidget::omnibox()
{
    QSettings settings;
    settings.setValue(QLatin1String("urlloading/searchEngineFallback"), true);

    // PRIV01: the profile interceptor upgrades http: navigations to
    // https: — this test asserts the URL the address bar resolved to,
    // not the transport, so the upgrade is switched off for it.
    const QVariant savedHttpsFirst =
        settings.value(QLatin1String("privacy/httpsFirst"));
    settings.setValue(QLatin1String("privacy/httpsFirst"), false);
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

    // Opt-out restores the old bare-http guess for non-address input.
    settings.setValue(QLatin1String("urlloading/searchEngineFallback"), false);
    check(QLatin1String("word"), QUrl(QLatin1String("http://word/")));

    manager->setEngineForKeyword(QLatin1String("ot"), nullptr);
    manager->setCurrentEngineName(previousEngine);
    manager->removeEngine(engine->name());
    widget.closeTab();

    if (savedHttpsFirst.isValid())
        settings.setValue(QLatin1String("privacy/httpsFirst"), savedHttpsFirst);
    else
        settings.remove(QLatin1String("privacy/httpsFirst"));
    PrivacyRequestInterceptor::loadSettings();
}

QTEST_MAIN(tst_TabWidget)
#include "tst_tabwidget.moc"

