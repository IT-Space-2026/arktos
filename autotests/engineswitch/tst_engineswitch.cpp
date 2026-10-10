/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

// ENG05/ENG08 — the per-tab engine switcher driven through a fake
// backend: no real servo-embed artifact required.  Covers the
// registry, the chrome's shared nav-row indicator (ENG08 moved it
// out of the url bar), the swap path, the Tor/private refusal rules
// and the default-engine setting round-trip.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <QtWebEngineWidgets>

#include "qtest_arora.h"

#include <browserapplication.h>
#include <browsermainwindow.h>
#include <containermanager.h>
#include <engineindicator.h>
#include <engineinterface.h>
#include <engineregistry.h>
#include <enginetab.h>
#include <lineedit.h>
#include <tabwidget.h>
#include <toolbarsearch.h>
#include <webview.h>

#include <qsplitter.h>
#include <qwebenginepage.h>

#include "fakeengine.h"

// The fake engine backend lives in autotests/fakeengine.h — shared
// with tst_tabwidget's swap legs.

// --- helpers -----------------------------------------------------------

// ENG08: the indicator is one chrome-owned button in the navigation
// row — reached through a real window, not through per-tab bars.
static EngineIndicator *navIndicator(QWidget *window)
{
    return window
        ? window->findChild<EngineIndicator *>(
            QStringLiteral("navEngineIndicator"))
        : nullptr;
}

static QWidget *navIndicatorHost(QWidget *window)
{
    return window
        ? window->findChild<QWidget *>(
            QStringLiteral("navEngineIndicatorHost"))
        : nullptr;
}

// WA_DeleteOnClose turns close() into deleteLater(); pump the
// deferred deletes so the window is gone before the test returns.
static void closeWindow(QWidget *window)
{
    if (!window)
        return;
    window->close();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// --- test ---------------------------------------------------------------

class tst_EngineSwitch : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void registry();
    void indicatorReflectsEngine();
    void indicatorToolbarSlot();
    void switchViaIndicatorButton();
    void switchReloadsCurrentUrl();
    void perTabEngineState();
    void privateTabHidesAffordance();
    void torLock();
    void missingArtifactHidesOption();
    void settingsRoundTrip();
    void swapKeepsStrip();
    void swapKeepsPinned();
    void swapKeepsGroup();
    void swapCollapsedGroupChip();
    void swapOnContainerLevel();
    void swapProbe();

private:
    FakeEngineBackend *m_fake = nullptr;
};

void tst_EngineSwitch::initTestCase()
{
    QCoreApplication::setApplicationName("tst_engineswitch");
    QSettings settings;
    settings.clear();
    // Keep close() free of the multi-tab confirmation prompt — the
    // window legs open several tabs before closing.
    settings.setValue(QLatin1String("tabs/confirmClosingMultipleTabs"),
                      false);
    m_fake = new FakeEngineBackend(this);
    EngineRegistry::registerBackend(m_fake);
}

void tst_EngineSwitch::cleanupTestCase()
{
    EngineRegistry::unregisterBackend(m_fake);
    delete m_fake;
    m_fake = nullptr;
}

void tst_EngineSwitch::init()
{
    if (EngineRegistry::backendForId(QStringLiteral("fake")) != m_fake)
        EngineRegistry::registerBackend(m_fake);
    EngineRegistry::setDefaultBackendId(QStringLiteral("webengine"));
}

void tst_EngineSwitch::cleanup()
{
    BrowserApplication::setTorMode(false);
    BrowserApplication::setPrivate(false);
    EngineRegistry::setDefaultBackendId(QStringLiteral("webengine"));
}

void tst_EngineSwitch::registry()
{
    QVERIFY(EngineRegistry::backends().count() >= 2);
    QVERIFY(EngineRegistry::backendForId(QStringLiteral("webengine")));
    QCOMPARE(EngineRegistry::backendForId(QStringLiteral("fake")), m_fake);
    QVERIFY(!EngineRegistry::backendForId(QStringLiteral("bogus")));
    // An unregistered default resolves back to WebEngine — a stale
    // pref can never strand New Tab.
    EngineRegistry::setDefaultBackendId(QStringLiteral("bogus"));
    QCOMPARE(EngineRegistry::defaultBackend(),
             EngineRegistry::backendForId(QStringLiteral("webengine")));
    EngineRegistry::setDefaultBackendId(QStringLiteral("webengine"));
}

void tst_EngineSwitch::indicatorReflectsEngine()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));

    EngineIndicator *indicator = navIndicator(window);
    QVERIFY(indicator);
    QCOMPARE(indicator->engineId(), QStringLiteral("webengine"));
    // Two registered backends + swappable tab -> shown, named tooltip.
    QVERIFY(!indicator->isHidden());
    QVERIFY(!navIndicatorHost(window)->isHidden());
    QVERIFY(indicator->toolTip().contains(QStringLiteral("Chromium"))
            || indicator->toolTip().contains(
                   QStringLiteral("QtWebEngine")));

    // The shared button tracks the swap through currentChanged.
    QVERIFY(tabs->reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("fake"));
    QVERIFY(tabs->engineTab(0));
    QCOMPARE(indicator->engineId(), QStringLiteral("fake"));
    QVERIFY(indicator->toolTip().contains(
        QStringLiteral("Fake Engine")));
    closeWindow(window);
}

void tst_EngineSwitch::indicatorToolbarSlot()
{
    // ENG08's contract: the button lives in the navigation row as a
    // fixed-width splitter cell immediately right of the url-bar
    // stack — and no indicator survives inside any per-tab bar, so
    // the field's right text margin carries no dead padding.
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    ToolbarSearch *search = window->toolbarSearch();
    QSplitter *splitter = window->findChild<QSplitter *>(
        QStringLiteral("navigationSplitter"));
    QWidget *host = navIndicatorHost(window);
    EngineIndicator *indicator = navIndicator(window);
    QVERIFY(splitter && host && indicator && search);

    QCOMPARE(splitter->widget(0), tabs->locationBarStack());
    QCOMPARE(splitter->indexOf(host), 1);
    QCOMPARE(splitter->indexOf(search), 2);
    QCOMPARE(indicator->parentWidget(), host);
    // fixed size, no stretch — the splitter cannot grow the cell.
    QCOMPARE(host->minimumWidth(), host->maximumWidth());

    QVERIFY(!tabs->locationBar(0)->findChild<EngineIndicator *>());
    QVERIFY(!indicator->accessibleName().isEmpty());
    closeWindow(window);
}

void tst_EngineSwitch::switchViaIndicatorButton()
{
    // The shared button's switch request resolves the CURRENT tab —
    // the single owner path that replaced the per-bar lambdas.
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    EngineIndicator *indicator = navIndicator(window);
    QVERIFY(indicator);

    QVERIFY(QMetaObject::invokeMethod(
        indicator, "switchRequested",
        Q_ARG(QString, QStringLiteral("fake"))));
    QCOMPARE(tabs->tabEngineId(tabs->currentIndex()),
             QStringLiteral("fake"));

    // Swap back through the same path — the affordance that used to
    // live in the engine tab's echo bar.
    QVERIFY(QMetaObject::invokeMethod(
        indicator, "switchRequested",
        Q_ARG(QString, QStringLiteral("webengine"))));
    QCOMPARE(tabs->tabEngineId(tabs->currentIndex()),
             QStringLiteral("webengine"));
    QVERIFY(tabs->webView(tabs->currentIndex()));
    closeWindow(window);
}

void tst_EngineSwitch::switchReloadsCurrentUrl()
{
    TabWidget widget;
    widget.newTab();
    const QUrl url(QStringLiteral("data:text/plain,engine-switch-target"));

    QWebEnginePage *page = widget.currentWebView()->page();
    QSignalSpy loadSpy(page, &QWebEnginePage::loadFinished);
    widget.loadUrl(url, TabWidget::CurrentTab);
    QTRY_VERIFY_WITH_TIMEOUT(loadSpy.count() >= 1, 15000);
    QCOMPARE(widget.currentWebView()->url(), url);

    const int pagesBefore = m_fake->pages.count();
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("fake"));
    QVERIFY(m_fake->pages.count() > pagesBefore);
    FakeEnginePage *fakePage =
        qobject_cast<FakeEnginePage*>(widget.engineTab(0)->page());
    QVERIFY(fakePage);
    QCOMPARE(fakePage->loads, 1);
    QCOMPARE(fakePage->url(), url);

    // And back: the fake tab's url lands on a fresh WebView.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("webengine")));
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("webengine"));
    QVERIFY(widget.webView(0));
    QTRY_COMPARE_WITH_TIMEOUT(widget.webView(0)->url(), url, 15000);
}

void tst_EngineSwitch::perTabEngineState()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    tabs->newTab();
    QCOMPARE(tabs->count(), 2);

    QVERIFY(tabs->reloadTabInEngine(1, QStringLiteral("fake")));
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));
    QCOMPARE(tabs->tabEngineId(1), QStringLiteral("fake"));

    // The shared button surfaces the ACTIVE tab's engine on every
    // switch.
    EngineIndicator *indicator = navIndicator(window);
    tabs->setCurrentIndex(0);
    QCOMPARE(indicator->engineId(), QStringLiteral("webengine"));
    tabs->setCurrentIndex(1);
    QCOMPARE(indicator->engineId(), QStringLiteral("fake"));
    closeWindow(window);
}

void tst_EngineSwitch::privateTabHidesAffordance()
{
    // An off-the-record tab can never swap — the same hidden rule the
    // embedded indicator enforced, now on the shared button, and the
    // cell comes back when a normal tab goes current.
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    EngineIndicator *indicator = navIndicator(window);
    QWidget *host = navIndicatorHost(window);
    QVERIFY(!host->isHidden());

    tabs->newPrivateTab();
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(tabs->isTabPrivate(1));
    QVERIFY(indicator->isHidden());
    QVERIFY(host->isHidden());

    tabs->setCurrentIndex(0);
    QVERIFY(!indicator->isHidden());
    QVERIFY(!host->isHidden());
    closeWindow(window);
}

void tst_EngineSwitch::torLock()
{
    BrowserApplication::setTorMode(true);
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));

    // The swap path refuses outright.
    QVERIFY(!tabs->reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));

    // And the affordance hides even with a second backend registered
    // — the button AND its splitter cell.
    QVERIFY(navIndicator(window)->isHidden());
    QVERIFY(navIndicatorHost(window)->isHidden());
    closeWindow(window);

    // A post-tor window shows it again.
    BrowserApplication::setTorMode(false);
    BrowserMainWindow *window2 = new BrowserMainWindow;
    QVERIFY(!navIndicator(window2)->isHidden());
    QVERIFY(!navIndicatorHost(window2)->isHidden());
    closeWindow(window2);
}

void tst_EngineSwitch::missingArtifactHidesOption()
{
    // "Artifact missing" is an unregistered backend — the indicator
    // must hide rather than offer a dead menu entry.
    EngineRegistry::unregisterBackend(m_fake);
    QCOMPARE(EngineRegistry::backends().count(), 1);

    BrowserMainWindow *window = new BrowserMainWindow;
    QVERIFY(navIndicator(window)->isHidden());
    QVERIFY(navIndicatorHost(window)->isHidden());
    QVERIFY(!window->tabWidget()->reloadTabInEngine(
        0, QStringLiteral("fake")));
    closeWindow(window);

    EngineRegistry::registerBackend(m_fake);
    BrowserMainWindow *window2 = new BrowserMainWindow;
    QVERIFY(!navIndicator(window2)->isHidden());
    QVERIFY(!navIndicatorHost(window2)->isHidden());
    closeWindow(window2);
}

void tst_EngineSwitch::settingsRoundTrip()
{
    QCOMPARE(EngineRegistry::defaultBackendId(),
             QStringLiteral("webengine"));
    EngineRegistry::setDefaultBackendId(QStringLiteral("fake"));
    QCOMPARE(EngineRegistry::defaultBackendId(), QStringLiteral("fake"));
    QCOMPARE(EngineRegistry::defaultBackend(), m_fake);

    // The pref drives new explicit tabs.
    TabWidget widget;
    widget.newTab();
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("fake"));

    EngineRegistry::setDefaultBackendId(QStringLiteral("webengine"));
    QCOMPARE(EngineRegistry::defaultBackend()->id(),
             QStringLiteral("webengine"));

    TabWidget widget2;
    widget2.newTab();
    QCOMPARE(widget2.tabEngineId(0), QStringLiteral("webengine"));
}

// ENG09 — strip integrity across the engine swap, per strip-state
// variant: the swap must never lose or reorder the OTHER tabs, the
// replacement takes the old tab's slot, and whatever strip state the
// task documents as dropping is asserted rather than assumed.

void tst_EngineSwitch::swapKeepsStrip()
{
    TabWidget widget;
    for (int i = 0; i < 3; ++i)
        widget.newTab();
    QCOMPARE(widget.count(), 3);
    WebView *v0 = widget.webView(0);
    WebView *v2 = widget.webView(2);

    QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 3);
    QCOMPARE(widget.webView(0), v0);
    QCOMPARE(widget.webView(2), v2);
    QVERIFY(widget.engineTab(1));
    QCOMPARE(widget.tabEngineId(1), QStringLiteral("fake"));

    QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("webengine")));
    QCOMPARE(widget.count(), 3);
    QCOMPARE(widget.webView(0), v0);
    QCOMPARE(widget.webView(2), v2);
    QVERIFY(widget.webView(1));
    QCOMPARE(widget.tabEngineId(1), QStringLiteral("webengine"));
}

void tst_EngineSwitch::swapKeepsPinned()
{
    TabWidget widget;
    for (int i = 0; i < 4; ++i)
        widget.newTab();
    widget.setTabPinned(0, true);
    widget.setTabPinned(1, true);
    QCOMPARE(widget.pinnedTabCount(), 2);
    WebView *v3 = widget.webView(3);

    // An unpinned tab swaps to the foreign engine and back — the
    // pinned prefix and the trailing tab are untouched.
    QVERIFY(widget.reloadTabInEngine(2, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 4);
    QCOMPARE(widget.pinnedTabCount(), 2);
    QVERIFY(widget.engineTab(2));
    QCOMPARE(widget.webView(3), v3);

    QVERIFY(widget.reloadTabInEngine(2, QStringLiteral("webengine")));
    QCOMPARE(widget.count(), 4);
    QCOMPARE(widget.pinnedTabCount(), 2);
    QVERIFY(widget.webView(2));
    QCOMPARE(widget.webView(3), v3);

    // A pinned tab swaps too — the replacement inherits the pin
    // (position-based pin boundary) and the block stays a prefix.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 4);
    QCOMPARE(widget.pinnedTabCount(), 2);
    QVERIFY(widget.engineTab(0));
    QVERIFY(widget.isTabPinned(0));

    // And back — still pinned, still first.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("webengine")));
    QCOMPARE(widget.count(), 4);
    QCOMPARE(widget.pinnedTabCount(), 2);
    QVERIFY(widget.isTabPinned(0));
    QVERIFY(widget.webView(0));
}

void tst_EngineSwitch::swapKeepsGroup()
{
    TabWidget widget;
    for (int i = 0; i < 3; ++i)
        widget.newTab();
    const QString gid = widget.createTabGroup(0);
    widget.addTabToGroup(1, gid);      // strip [v0 G, v1 G, v2]
    QCOMPARE(widget.tabGroupMembers(gid), QList<int>() << 0 << 1);
    WebView *v2 = widget.webView(2);

    // Swapping the second member out: the group keeps v0, the
    // EngineTab cannot carry membership (documented drop — the
    // m_tabGroups map keys on WebView*).
    QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 3);
    QCOMPARE(widget.tabGroupId(0), gid);
    QCOMPARE(widget.tabGroupId(1), QString());
    QCOMPARE(widget.webView(2), v2);
    QVERIFY(widget.engineTab(1));

    // Swapping back to WebEngine restores the WebView in place but
    // does not resurrect the dropped membership — the strip shape
    // stays honest either way.
    QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("webengine")));
    QCOMPARE(widget.count(), 3);
    QCOMPARE(widget.webView(2), v2);
    QCOMPARE(widget.tabGroupId(0), gid);
    QCOMPARE(widget.tabGroupId(1), QString());
}

void tst_EngineSwitch::swapCollapsedGroupChip()
{
    TabWidget widget;
    for (int i = 0; i < 4; ++i)
        widget.newTab();
    const QString gid = widget.createTabGroup(0);
    widget.addTabToGroup(1, gid);
    widget.addTabToGroup(2, gid);      // [v0,v1,v2 G, v3]
    widget.setTabGroupCollapsed(gid, true);
    QVERIFY(widget.tabGroupIsCollapsed(gid));
    QVERIFY(widget.isTabGroupChip(0));
    QCOMPARE(widget.count(), 2);       // chip + v3
    WebView *v3 = widget.webView(1);

    // Swapping the chip: the foreign tab takes the chip's slot but
    // cannot hold membership, so the group re-collapses around its
    // next member — the hidden tabs stay hidden.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 3);       // T + new chip + v3
    QVERIFY(widget.engineTab(0));
    QCOMPARE(widget.tabGroupId(0), QString());
    QVERIFY(widget.tabGroupIsCollapsed(gid));
    QVERIFY(widget.isTabGroupChip(1));
    QCOMPARE(widget.tabGroupMembers(gid), QList<int>() << 1);
    QCOMPARE(widget.webView(2), v3);
}

void tst_EngineSwitch::swapOnContainerLevel()
{
    // CONT06 two-level strip: on the default level a plain swap
    // proceeds; on a non-default level the swap refuses (documented
    // guard) rather than re-filtering mid-swap.
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    TabWidget widget;
    widget.loadSettings();
    QVERIFY(widget.twoLevelStrip());

    for (int i = 0; i < 2; ++i)
        widget.newTab();
    WebView *containerTab = widget.makeNewTabInContainer(
        ContainerManager::instance()->createContainer(
            QStringLiteral("Work"), QColor(Qt::red)).id,
        true);
    QVERIFY(containerTab);
    QVERIFY(widget.containerStripActive());
    // makeCurrent pulled the strip to the Work level — the two
    // default tabs detached into their level's hidden store.
    QCOMPARE(widget.count(), 1);

    // The container tab is on the default level only if its header is
    // active — the swap guard keys on the ACTIVE header.
    const QString workId = containerTab->containerId();
    QVERIFY(!workId.isEmpty());
    // makeCurrent pulled the strip to the Work level.
    QCOMPARE(widget.activeContainerHeader(), workId);
    QVERIFY(!widget.reloadTabInEngine(
        widget.currentIndex(), QStringLiteral("fake")));

    // Back on the default level the swap works and the hidden level
    // keeps its tab.
    widget.setActiveContainerHeader(ContainerManager::defaultContainerId());
    QCOMPARE(widget.count(), 2);
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 2);
    QVERIFY(widget.engineTab(0));
    QCOMPARE(widget.containerTabCount(workId), 1);
    QCOMPARE(widget.totalTabCount(), 3);

    ContainerManager::instance()->deleteContainer(workId);
    QSettings().remove(QLatin1String("tabs/containerDisplay"));
}

// ENG09 reproduction probe — permutes strip shapes and dumps any
// invariant break: lost/duplicated widgets, bar-stack desync, dropped
// pins, vanished group members.
void tst_EngineSwitch::swapProbe()
{
    int failures = 0;
    const auto fail = [&failures](const QString &tag, const char *what) {
        ++failures;
        qWarning() << "FAIL" << tag << what;
    };
    for (int n = 1; n <= 4; ++n) {
        for (int pins = 0; pins <= qMin(n, 2); ++pins) {
            for (int grp = 0; grp < 3; ++grp) {  // 0 none, 1 expanded, 2 collapsed
                for (int victim = 0; victim < n; ++victim) {
                    TabWidget widget;
                    for (int i = 0; i < n; ++i)
                        widget.newTab();
                    for (int i = 0; i < pins; ++i)
                        widget.setTabPinned(i, true);
                    QString gid;
                    if (grp) {
                        gid = widget.createTabGroup(qMin(n - 1, 1));
                        if (n > 2)
                            widget.addTabToGroup(n - 1, gid);
                    }
                    if (grp == 2)
                        widget.setTabGroupCollapsed(gid, true);
                    if (victim >= widget.count())
                        continue;
                    for (int dir = 0; dir < 2; ++dir) { // 0 -> fake, 1 -> back
                        const int visibleBefore = widget.count();
                        const int totalBefore = widget.totalTabCount();
                        QList<QWidget*> before;
                        for (int i = 0; i < widget.count(); ++i)
                            before << widget.widget(i);
                        QWidget *victimW = widget.widget(victim);
                        // A collapsed chip's swap legitimately adds
                        // one visible slot — the foreign tab takes
                        // the slot and the next member becomes the
                        // new chip — but only when the group has
                        // hidden members to re-collapse around.
                        const bool chip = widget.isTabGroupChip(victim)
                            && widget.tabGroupSize(
                                widget.tabGroupId(victim)) > 1;
                        const QString target = dir
                            ? QStringLiteral("webengine")
                            : QStringLiteral("fake");
                        const QString tag = QStringLiteral(
                            "n=%1 pins=%2 grp=%3 victim=%4 dir=%5")
                            .arg(n).arg(pins).arg(grp).arg(victim).arg(dir);
                        if (!widget.reloadTabInEngine(victim, target)) {
                            // Refusal must leave the strip untouched.
                            if (widget.count() != visibleBefore)
                                fail(tag, "refuse-mutated");
                            else {
                                for (int i = 0; i < widget.count(); ++i)
                                    if (widget.widget(i) != before.value(i)) {
                                        fail(tag, "refuse-reorder");
                                        break;
                                    }
                            }
                            break;   // a refused swap-back ends the variant
                        }
                        // Invariants: no page lost anywhere (visible
                        // or hidden), the victim swapped out, every
                        // other pre-swap widget still present.
                        if (widget.totalTabCount() != totalBefore)
                            fail(tag, "total");
                        if (widget.count() != visibleBefore + (chip ? 1 : 0))
                            fail(tag, "count");
                        QList<QWidget*> survivors;
                        for (int i = 0; i < widget.count(); ++i)
                            survivors << widget.widget(i);
                        if (survivors.contains(victimW))
                            fail(tag, "victim-lived");
                        QList<QWidget*> expected = before;
                        expected.removeAll(victimW);
                        for (QWidget *w : expected)
                            if (!survivors.removeOne(w))
                                fail(tag, "lost");
                        if (survivors.count() != 1 + (chip ? 1 : 0))
                            fail(tag, "surplus");
                    }
                }
            }
        }
    }
    QCOMPARE(failures, 0);
}

QTEST_MAIN(tst_EngineSwitch)
#include "tst_engineswitch.moc"
