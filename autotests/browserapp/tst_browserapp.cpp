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

// COV04: BrowserApplication services — the lazy singletons, the
// named-profile wiring, window bookkeeping via newMainWindow(), session
// save/restore round-trip, private-mode flag semantics and the
// modifier-recording API.  newMainWindow() alone exercises the whole
// BrowserMainWindow chrome assembly path (menus, toolbar, tab widget).

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qwebengineprofile.h>
#include <qwebenginepage.h>

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "tabwidget.h"
#include "webview.h"
#include "historymanager.h"
#include "bookmarksmanager.h"
#include "cookiejar.h"
#include "downloadmanager.h"
#include "networkaccessmanager.h"
#include "languagemanager.h"
#include "autofillmanager.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_BrowserApp : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();

    void services();
    void dataPaths();
    void mainWindowLifecycle();
    void sessionRoundTrip();
    void eventModifiers();
    void privateBrowsing();
    void zoomTextOnly();
    void askDesktop();
    void torWindowHandoffGate();
};

void tst_BrowserApp::initTestCase()
{
    QCoreApplication::setApplicationName("tst_browserapp");
    QSettings settings;
    settings.clear();
}

void tst_BrowserApp::cleanupTestCase()
{
    const QList<BrowserMainWindow *> windows =
        BrowserApplication::instance()->mainWindows();
    for (BrowserMainWindow *window : windows)
        delete window;
}

void tst_BrowserApp::services()
{
    BrowserApplication *app = BrowserApplication::instance();
    QVERIFY(app);
    // AUTOTESTS builds skip the standalone/instance-forwarding block,
    // so the flag stays cleared.
    QVERIFY(!app->isStandalone());

    QVERIFY(BrowserApplication::webEngineProfile());
    QVERIFY(!BrowserApplication::webEngineProfile()->isOffTheRecord());
    QVERIFY(BrowserApplication::cookieJar());
    QVERIFY(BrowserApplication::historyManager());
    QVERIFY(BrowserApplication::bookmarksManager());
    QVERIFY(BrowserApplication::downloadManager());
    QVERIFY(BrowserApplication::networkAccessManager());
    QVERIFY(BrowserApplication::languageManager());
    QVERIFY(BrowserApplication::autoFillManager());

    // Static icon delegates to the history manager's icon store.
    QVERIFY(!BrowserApplication::icon(QUrl(QLatin1String("http://a.b/"))).isNull());

    app->loadSettings();
}

void tst_BrowserApp::dataPaths()
{
    QVERIFY(!BrowserApplication::installedDataDirectory().isEmpty());
    const QString path = BrowserApplication::dataFilePath(QLatin1String("test.dat"));
    QVERIFY(path.endsWith(QLatin1String("test.dat")));
}

// newMainWindow() runs the whole chrome assembly: setupMenu, setupToolBar
// and the tab widget wiring inside BrowserMainWindow's ctor.
void tst_BrowserApp::mainWindowLifecycle()
{
    BrowserApplication *app = BrowserApplication::instance();
    QVERIFY(app->mainWindows().isEmpty());

    BrowserMainWindow *window = app->newMainWindow();
    QVERIFY(window);
    QCOMPARE(app->mainWindow(), window);
    QCOMPARE(app->mainWindows().count(), 1);
    QVERIFY(window->tabWidget());
    QVERIFY(window->tabWidget()->count() >= 1);
    QVERIFY(window->currentTab());

    BrowserMainWindow *second = app->newMainWindow();
    QCOMPARE(app->mainWindows().count(), 2);
    QVERIFY(app->allowToCloseWindow(second));

    delete second;
    QTRY_VERIFY(app->mainWindows().count() == 1);
}

// saveSession serializes each window's tab state; restoreLastSession
// rebuilds windows from the blob.
void tst_BrowserApp::sessionRoundTrip()
{
    BrowserApplication *app = BrowserApplication::instance();
    BrowserMainWindow *window = app->mainWindow();
    if (!window)
        window = app->newMainWindow();
    window->tabWidget()->newTab();
    window->tabWidget()->loadUrl(QUrl(QLatin1String("about:blank")));

    app->saveSession();
    QVERIFY(app->canRestoreSession());
    QVERIFY(app->restoreLastSession());
    QVERIFY(app->mainWindows().count() >= 1);
}

void tst_BrowserApp::eventModifiers()
{
    BrowserApplication *app = BrowserApplication::instance();
    app->setEventMouseButtons(Qt::MiddleButton);
    app->setEventKeyboardModifiers(Qt::ControlModifier);
    QCOMPARE(app->eventMouseButtons(), Qt::MiddleButton);
    QCOMPARE(app->eventKeyboardModifiers(), Qt::KeyboardModifiers(Qt::ControlModifier));
    app->setEventMouseButtons(Qt::NoButton);
    app->setEventKeyboardModifiers(Qt::NoModifier);
}

// setPrivate flips the flag and retargets webEngineProfile() at an
// off-the-record profile for subsequently created pages.
void tst_BrowserApp::privateBrowsing()
{
    QVERIFY(!BrowserApplication::isPrivate());
    BrowserApplication::setPrivate(true);
    QVERIFY(BrowserApplication::isPrivate());
    QVERIFY(BrowserApplication::webEngineProfile()->isOffTheRecord());
    BrowserApplication::setPrivate(false);
    QVERIFY(!BrowserApplication::isPrivate());
    QVERIFY(!BrowserApplication::webEngineProfile()->isOffTheRecord());
}

void tst_BrowserApp::zoomTextOnly()
{
    const bool before = BrowserApplication::zoomTextOnly();
    BrowserApplication::setZoomTextOnly(!before);
    QCOMPARE(BrowserApplication::zoomTextOnly(), !before);
    BrowserApplication::setZoomTextOnly(before);
}

// Routing non-http(s) urls through QDesktopServices is a no-op
// offscreen but must not recurse or crash.
void tst_BrowserApp::askDesktop()
{
    BrowserApplication::instance()->askDesktopToOpenUrl(
        QUrl(QLatin1String("mailto:nobody@example.com")));
}

// CONT08: 'Open in New Tor Window' hands the link url to a fresh
// `arora --tor` process.  The hand-off must carry it as its own argv
// element — a QStringList element is passed verbatim to execvp and no
// shell ever re-parses it, so shell metacharacters in the url are
// inert by construction — and refused schemes must not leave this
// process at all.  The receiving side applies the same page-link
// gate to whatever operand actually arrives.
void tst_BrowserApp::torWindowHandoffGate()
{
    // No url: just the mode switch.
    QCOMPARE(BrowserApplication::torWindowArguments(QUrl()),
             (QStringList{QStringLiteral("--tor")}));

    // A good url is exactly one verbatim element — including content
    // a shell would otherwise reinterpret.
    const QUrl good(QStringLiteral(
        "https://example.com/a?x=$(id)&y=`whoami` z'q"));
    const QStringList arguments =
        BrowserApplication::torWindowArguments(good);
    QCOMPARE(arguments.count(), 2);
    QCOMPARE(arguments.at(0), QStringLiteral("--tor"));
    QCOMPARE(arguments.at(1),
             QString::fromUtf8(good.toEncoded()));
    QCOMPARE(QUrl::fromEncoded(arguments.at(1).toUtf8()), good);

    // The dangerous schemes are dropped on this side — the spawned
    // process sees a bare --tor.
    const char *badUrls[] = {
        "javascript:alert(1)",
        "data:text/html,<h1>x</h1>",
        "blob:https://example.com/uuid",
    };
    for (const char *bad : badUrls) {
        QCOMPARE(BrowserApplication::torWindowArguments(QUrl(bad)),
                 (QStringList{QStringLiteral("--tor")}));
    }

    // The receiving process re-gates the raw operand: an argv url
    // that somehow still arrives with a refused scheme loads
    // nothing.
    QVERIFY(BrowserApplication::isUrlAllowedOnTorArgv(
        QStringLiteral("https://example.com/")));
    QVERIFY(BrowserApplication::isUrlAllowedOnTorArgv(
        QStringLiteral("file:///tmp/x")));
    QVERIFY(!BrowserApplication::isUrlAllowedOnTorArgv(
        QStringLiteral("javascript:alert(1)")));
    QVERIFY(!BrowserApplication::isUrlAllowedOnTorArgv(
        QStringLiteral("data:text/html,<h1>x</h1>")));
    QVERIFY(!BrowserApplication::isUrlAllowedOnTorArgv(
        QStringLiteral("blob:https://example.com/uuid")));
}

QTEST_MAIN(tst_BrowserApp)
#include "tst_browserapp.moc"
