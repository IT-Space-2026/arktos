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

// ENG05/ENG08/ENG19 — the per-tab engine switcher driven through a
// fake backend: no real servo-embed artifact required.  Covers the
// registry, the chrome's shared nav-row indicator (ENG08 moved it
// out of the url bar), the swap path, the Tor/private/container
// lock rules (ENG19: locked shows disabled+explained, not hidden),
// spent-slot menu entries and the default-engine setting round-trip.

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
#include <tabbar.h>
#include <tabwidget.h>
#include <toolbarsearch.h>
#include <webview.h>

#include <qlabel.h>
#include <qmenu.h>
#include <qset.h>
#include <qsplitter.h>
#include <qstackedwidget.h>
#include <qtcpserver.h>
#include <qtcpsocket.h>
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

// closeTab()/the swap deleteLater() the retired widget AND its echo
// bar — pump them before asserting on guards or the bar stack.
static void pumpDeletes()
{
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// The QStackedWidget's own count is the parallel-stack contract:
// insert/remove update it synchronously, while findChildren still
// sees bars whose deleteLater has not been delivered yet.
static int barStackCount(TabWidget *widget)
{
    auto *stack =
        qobject_cast<QStackedWidget*>(widget->locationBarStack());
    return stack ? stack->count() : -1;
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
    void privateTabLocksIndicator();
    void torLock();
    void containerLevelLocksIndicator();
    void widgetTabHidesIndicator();
    void unavailableBackendMenuEntry();
    void missingArtifactHidesOption();
    void settingsRoundTrip();
    void swapKeepsStrip();
    void swapKeepsPinned();
    void swapKeepsGroup();
    void swapCollapsedGroupChip();
    void swapOnContainerLevel();
    void swapProbe();
    void swapMidLoad();
    void closeForeignTab();
    void engineTabLocationBar();
    void engineTabSingleIcon();
    void engineTabReload();

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

    // ENG19: pixel check — the button really paints its palette
    // glyph, not merely reports visible.  A missing/invisible icon
    // (the "present-but-unseeable" failure shape) still passes the
    // flag checks; grab() rasterizes the real paint path.
    const QImage paint = indicator->grab().toImage();
    QVERIFY(!paint.isNull());
    QSet<QRgb> colors;
    for (int y = 0; y < paint.height(); ++y)
        for (int x = 0; x < paint.width(); ++x)
            colors.insert(paint.pixel(x, y));
    QVERIFY2(colors.count() > 2,
             "engine indicator painted fewer than 3 colors — "
             "the glyph is not actually rendered");

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

// ENG19 — an off-the-record tab can never swap (no OTR profile on
// foreign backends), but a vanished button reads as a bug: the lock
// now shows the button DISABLED with the reason in its tooltip, and
// a normal tab restores the live button.
void tst_EngineSwitch::privateTabLocksIndicator()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    EngineIndicator *indicator = navIndicator(window);
    QWidget *host = navIndicatorHost(window);
    QVERIFY(!host->isHidden());
    QVERIFY(indicator->isEnabled());
    QVERIFY(!indicator->isLocked());

    tabs->newPrivateTab();
    QCOMPARE(tabs->currentIndex(), 1);
    QVERIFY(tabs->isTabPrivate(1));
    QVERIFY(!indicator->isHidden());
    QVERIFY(!host->isHidden());
    QVERIFY(!indicator->isEnabled());
    QVERIFY(indicator->isLocked());
    QVERIFY(indicator->toolTip().contains(
        QStringLiteral("off-the-record")));

    tabs->setCurrentIndex(0);
    QVERIFY(!indicator->isHidden());
    QVERIFY(indicator->isEnabled());
    QVERIFY(!indicator->isLocked());
    QVERIFY(!host->isHidden());
    closeWindow(window);
}

void tst_EngineSwitch::torLock()
{
    BrowserApplication::setTorMode(true);
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));

    // The swap path refuses outright — servo has no SOCKS5, a swapped
    // tab would leak traffic off the proxy.
    QVERIFY(!tabs->reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));

    // ENG19: the affordance no longer vanishes on tor — it shows
    // DISABLED with the lock reason, answering the missing-switcher
    // report in place while the swap stays unreachable.
    EngineIndicator *indicator = navIndicator(window);
    QVERIFY(!indicator->isHidden());
    QVERIFY(!indicator->isEnabled());
    QVERIFY(indicator->isLocked());
    QVERIFY(indicator->toolTip().contains(QStringLiteral("Tor")));
    QVERIFY(!navIndicatorHost(window)->isHidden());
    // The disabled button still paints a (greyed) glyph — the lock
    // is visible, not merely un-hidden.
    QVERIFY(!indicator->grab().toImage().isNull());
    closeWindow(window);

    // A post-tor window shows it live again.
    BrowserApplication::setTorMode(false);
    BrowserMainWindow *window2 = new BrowserMainWindow;
    QVERIFY(!navIndicator(window2)->isHidden());
    QVERIFY(navIndicator(window2)->isEnabled());
    QVERIFY(!navIndicatorHost(window2)->isHidden());
    closeWindow(window2);
}

// ENG19 — a non-default container level refuses the swap (the
// append+move+close would re-filter the strip mid-swap, same guard
// reloadTabInEngine enforces): the button shows disabled with the
// reason, live again on the default level.
void tst_EngineSwitch::containerLevelLocksIndicator()
{
    QSettings().setValue(QLatin1String("tabs/containerDisplay"), 1);
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    QVERIFY(tabs->twoLevelStrip());
    EngineIndicator *indicator = navIndicator(window);
    QVERIFY(indicator->isEnabled());
    QVERIFY(!indicator->isLocked());

    WebView *containerTab = tabs->makeNewTabInContainer(
        ContainerManager::instance()->createContainer(
            QStringLiteral("Work"), QColor(Qt::red)).id,
        true);
    QVERIFY(containerTab);
    QVERIFY(tabs->containerStripActive());
    QVERIFY(!tabs->activeContainerHeader().isEmpty());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(!indicator->isHidden());
    QVERIFY(!indicator->isEnabled());
    QVERIFY(indicator->isLocked());
    QVERIFY(indicator->toolTip().contains(
        QStringLiteral("container")));

    // Back on the default level the button goes live again — the
    // tabsChanged hook re-evaluates the lock even though the current
    // index never moved.
    tabs->setActiveContainerHeader(
        ContainerManager::defaultContainerId());
    QCOMPARE(tabs->count(), 1);
    QVERIFY(indicator->isEnabled());
    QVERIFY(!indicator->isLocked());
    QVERIFY(!navIndicatorHost(window)->isHidden());

    ContainerManager::instance()->deleteContainer(
        containerTab->containerId());
    closeWindow(window);
    QSettings().remove(QLatin1String("tabs/containerDisplay"));
}

// ENG19 — a widget tab (Preferences, History, ...) has no engine:
// the whole splitter cell still hides — there is nothing to explain.
void tst_EngineSwitch::widgetTabHidesIndicator()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    EngineIndicator *indicator = navIndicator(window);
    QWidget *host = navIndicatorHost(window);
    QVERIFY(!host->isHidden());

    const int index = tabs->addWidgetTab(
        new QWidget, QStringLiteral("Settings"), QIcon(), true);
    QCOMPARE(tabs->currentIndex(), index);
    QCOMPARE(tabs->tabEngineId(index), QString());
    QVERIFY(host->isHidden());

    tabs->setCurrentIndex(0);
    QVERIFY(!host->isHidden());
    QVERIFY(indicator->isEnabled());
    closeWindow(window);
}

// ENG19 — a spent backend (Servo's single view claimed/exhausted)
// keeps the BUTTON live — swap-back off a live foreign tab still
// needs it — while the dead target reads as a disabled menu entry
// with the reason, and the swap itself refuses.
void tst_EngineSwitch::unavailableBackendMenuEntry()
{
    BrowserMainWindow *window = new BrowserMainWindow;
    TabWidget *tabs = window->tabWidget();
    EngineIndicator *indicator = navIndicator(window);
    QMenu *menu = indicator->menu();
    QVERIFY(menu);

    // Land on a foreign tab first — the unavailable state is staged
    // afterwards, mirroring a slot that is spent while in use.
    QVERIFY(tabs->reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("fake"));

    m_fake->m_unavailableReason =
        QStringLiteral("already in use — one tab per session");

    // The button stays live: its menu offers "Reload in Chromium" —
    // the swap-BACK path a spent slot must not strand.
    QVERIFY(!indicator->isHidden());
    QVERIFY(indicator->isEnabled());
    emit menu->aboutToShow();
    QAction *back = nullptr;
    QAction *dead = nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text().contains(QStringLiteral("Chromium"))
            || action->text().contains(QStringLiteral("QtWebEngine")))
            back = action;
    }
    QVERIFY(back && back->isEnabled());

    // Swap back works — the foreign tab is gone, the slot reads spent.
    QVERIFY(tabs->reloadTabInEngine(0, QStringLiteral("webengine")));
    QCOMPARE(tabs->tabEngineId(0), QStringLiteral("webengine"));

    // Now the fake entry reads disabled with the reason — and the
    // swap itself refuses through createPage, exactly like Servo's
    // exhausted slot.
    QVERIFY(indicator->isEnabled());
    emit menu->aboutToShow();
    for (QAction *action : menu->actions()) {
        if (action->text().contains(QStringLiteral("Fake Engine")))
            dead = action;
    }
    QVERIFY(dead);
    QVERIFY(!dead->isEnabled());
    QVERIFY(dead->toolTip().contains(QStringLiteral("one tab")));
    QVERIFY(!tabs->reloadTabInEngine(0, QStringLiteral("fake")));

    // Clearing the reason restores a live entry.
    m_fake->m_unavailableReason.clear();
    emit menu->aboutToShow();
    QAction *live = nullptr;
    for (QAction *action : menu->actions()) {
        if (action->text().contains(QStringLiteral("Fake Engine")))
            live = action;
    }
    QVERIFY(live && live->isEnabled());
    closeWindow(window);
}

void tst_EngineSwitch::missingArtifactHidesOption()
{
    // "Artifact missing" is an unregistered backend — the indicator
    // must hide rather than offer a dead menu entry.
    EngineRegistry::unregisterBackend(m_fake);
    // ENG10: when the build auto-enables servo the real backend
    // self-registers whenever its cdylib resolves — detach every
    // non-default backend so the single-backend case is exercised.
    QList<Engine::Backend *> detached;
    for (Engine::Backend *backend : EngineRegistry::backends()) {
        if (backend->id() != QLatin1String("webengine"))
            detached.append(backend);
    }
    for (Engine::Backend *backend : detached)
        EngineRegistry::unregisterBackend(backend);
    QCOMPARE(EngineRegistry::backends().count(), 1);

    BrowserMainWindow *window = new BrowserMainWindow;
    QVERIFY(navIndicator(window)->isHidden());
    QVERIFY(navIndicatorHost(window)->isHidden());
    QVERIFY(!window->tabWidget()->reloadTabInEngine(
        0, QStringLiteral("fake")));

    // ENG19: even a hard lock stays hidden with one backend — there
    // is no engine choice to explain.
    window->tabWidget()->newPrivateTab();
    QVERIFY(window->tabWidget()->isTabPrivate(
        window->tabWidget()->currentIndex()));
    QVERIFY(navIndicator(window)->isHidden());
    QVERIFY(navIndicatorHost(window)->isHidden());
    closeWindow(window);

    for (Engine::Backend *backend : detached)
        EngineRegistry::registerBackend(backend);
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

// ENG06(c) — the swap lands while the SOURCE load is still in
// flight: the dying page's pending signals must not corrupt the
// replacement, the strip keeps its shape, and the fake receives the
// in-flight url.
void tst_EngineSwitch::swapMidLoad()
{
    // A server that accepts and never answers keeps the WebEngine
    // load committed-but-unfinished for the whole leg.
    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    connect(&server, &QTcpServer::newConnection, &server, [&server]() {
        QTcpSocket *socket = server.nextPendingConnection();
        socket->setParent(&server);
    });
    const QUrl url(QStringLiteral("http://127.0.0.1:%1/never")
        .arg(server.serverPort()));

    TabWidget widget;
    widget.newTab();
    WebView *view = widget.webView(0);
    QPointer<WebView> viewGuard(view);
    QSignalSpy startedSpy(view->page(), &QWebEnginePage::loadStarted);
    view->load(url);
    QTRY_VERIFY_WITH_TIMEOUT(startedSpy.count() >= 1, 15000);

    // Swap while the source page is mid-load — the swap is
    // synchronous, so any late signal from the torn-down page lands
    // on dead slots, not on the new EngineTab.
    const int pagesBefore = m_fake->pages.count();
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    pumpDeletes();
    QCOMPARE(widget.count(), 1);
    QVERIFY(!viewGuard);
    QVERIFY(m_fake->pages.count() > pagesBefore);
    FakeEnginePage *fakePage =
        qobject_cast<FakeEnginePage*>(widget.engineTab(0)->page());
    QVERIFY(fakePage);
    QCOMPARE(fakePage->loads, 1);
    QCOMPARE(fakePage->url(), url);
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("fake"));
    // The parallel bar stack stayed aligned through the swap.
    QCOMPARE(barStackCount(&widget),
             widget.count());

    // And back mid-flight again — the fake tab's synchronous load
    // finished, but the swap-back still re-loads the same url on a
    // fresh WebView while the original connection is still pending.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("webengine")));
    QCOMPARE(widget.count(), 1);
    QVERIFY(widget.webView(0));
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("webengine"));
    QCOMPARE(barStackCount(&widget),
             widget.count());
}

// ENG06(c) — closed-tab edges around the swap: a foreign EngineTab
// tears down through the same closeTab path, by identity rather than
// a stale index, and the parallel bar stack stays aligned.
void tst_EngineSwitch::closeForeignTab()
{
    TabWidget widget;
    for (int i = 0; i < 3; ++i)
        widget.newTab();
    QCOMPARE(widget.count(), 3);
    WebView *v0 = widget.webView(0);
    WebView *v2 = widget.webView(2);

    // Swap the middle tab out, then close IT — the surviving tabs
    // shift down by identity, not by a stale captured index.
    QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("fake")));
    EngineTab *foreign = widget.engineTab(1);
    QVERIFY(foreign);
    QPointer<EngineTab> foreignGuard(foreign);
    const int fakePagesBefore = m_fake->pages.count();

    QSignalSpy changedSpy(&widget, &TabWidget::tabsChanged);
    widget.closeTab(1);
    pumpDeletes();
    QCOMPARE(widget.count(), 2);
    QVERIFY(changedSpy.count() >= 1);
    QVERIFY(!foreignGuard);          // really deleted, not reparented
    QCOMPARE(widget.webView(0), v0);
    QCOMPARE(widget.webView(1), v2); // v2 shifted, same identity
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("webengine"));
    QCOMPARE(widget.tabEngineId(1), QStringLiteral("webengine"));
    QCOMPARE(barStackCount(&widget),
             widget.count());

    // Stale-index edges: closing/swapping past the end refuses
    // cleanly and leaves the strip untouched.
    widget.closeTab(7);
    QCOMPARE(widget.count(), 2);
    QVERIFY(!widget.reloadTabInEngine(7, QStringLiteral("fake")));
    QCOMPARE(widget.count(), 2);
    QCOMPARE(widget.webView(1), v2);

    // Swap the CURRENT tab to fake and close it — the hasFocus path
    // sees a null WebView and skips the focus handoff, and the last
    // teardown still reports a clean strip.
    QVERIFY(widget.reloadTabInEngine(1, QStringLiteral("fake")));
    widget.setCurrentIndex(1);
    widget.closeTab();               // current -> the foreign tab
    pumpDeletes();
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.webView(0), v0);
    QCOMPARE(barStackCount(&widget),
             widget.count());
    QCOMPARE(m_fake->pages.count(), fakePagesBefore + 1);

    // The remaining tab's swap still works after all the churn —
    // state followed widget identity, never the shifting index.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(widget.tabEngineId(0), QStringLiteral("fake"));
    QSignalSpy lastSpy(&widget, &TabWidget::lastTabClosed);
    widget.closeTab(0);
    pumpDeletes();
    QCOMPARE(widget.count(), 0);
    QCOMPARE(lastSpy.count(), 1);
    QCOMPARE(barStackCount(&widget), 0);
}

// ENG14 — the foreign tab's location bar is a real omnibox: Enter
// dispatches to the tab's own page, a bare phrase resolves through
// the shared search fallback, and neither spawns a fresh WebEngine
// tab (the pre-fix bar was a read-only echo and loadUrl() resolved
// through currentWebView(), which is null on an engine tab).
void tst_EngineSwitch::engineTabLocationBar()
{
    TabWidget widget;
    widget.newTab();
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    EngineTab *foreign = widget.engineTab(0);
    QVERIFY(foreign);
    FakeEnginePage *page =
        qobject_cast<FakeEnginePage*>(foreign->page());
    QVERIFY(page);

    QLineEdit *bar = widget.locationBar(0);
    QVERIFY(bar);
    QVERIFY(!bar->isReadOnly());

    // A typed url navigates the SAME engine tab.
    const QUrl url(QStringLiteral("file:///tmp/eng14-typed-url.html"));
    bar->setText(url.toString());
    emit bar->returnPressed();
    QCOMPARE(page->loads, 1);
    QCOMPARE(page->url(), url);
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.engineTab(0), foreign);
    QCOMPARE(bar->text(), QString::fromUtf8(url.toEncoded()));

    // Plain text resolves through the identical search fallback a
    // WebEngine tab would use — no forked behavior.
    const QUrl search = TabWidget::guessUrlFromString(
        QStringLiteral("hello engine search"));
    QVERIFY(search.isValid());
    bar->setText(QStringLiteral("hello engine search"));
    emit bar->returnPressed();
    QCOMPARE(page->loads, 2);
    QCOMPARE(page->url(), search);
    QCOMPARE(widget.count(), 1);
    QCOMPARE(widget.engineTab(0), foreign);

    // The swap-back path is untouched: the restored WebEngine tab
    // still loads through its own WebView.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("webengine")));
    QVERIFY(widget.webView(0));
}

// ENG15 — a foreign tab must draw ONE strip icon.  The regression
// stacked two copies of the default glyph side by side: the addTab()
// tab icon AND the free-side favicon label's pixmap.  WebView tabs
// pass no tab icon at all — the label is the only surface — and the
// engine tab now follows the same shape, before AND after a load,
// and across repeated swaps.
void tst_EngineSwitch::engineTabSingleIcon()
{
#if !defined(Q_OS_MACOS)
    TabWidget widget;
    widget.newTab();
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QVERIFY(widget.engineTab(0));

    const QTabBar::ButtonPosition iconSide =
        widget.tabBar()->freeSide();
    const auto visibleIcons = [&widget, iconSide]() {
        int icons = widget.tabBar()->tabIcon(0).isNull() ? 0 : 1;
        if (QLabel *label = qobject_cast<QLabel*>(
                widget.tabBar()->tabButton(0, iconSide))) {
            if (label->movie() || !label->pixmap().isNull())
                ++icons;
        }
        return icons;
    };

    // An empty swapped tab never loads — the label's default pixmap
    // is installed eagerly and the QTabBar icon stays null.
    QCOMPARE(visibleIcons(), 1);
    QLabel *label = qobject_cast<QLabel*>(
        widget.tabBar()->tabButton(0, iconSide));
    QVERIFY(label);
    QVERIFY(!label->pixmap().isNull());

    // After a real load the same single surface remains — the
    // loadFinished pixmap swap never reintroduces a second icon.
    widget.engineTab(0)->load(
        QUrl(QStringLiteral("file:///tmp/eng15-icon.html")));
    QCOMPARE(visibleIcons(), 1);
    QVERIFY(!label->pixmap().isNull());

    // Round-trip through webengine and swap forward again — still
    // exactly one icon, no stranded second surface.
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("webengine")));
    QVERIFY(widget.webView(0));
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    QCOMPARE(visibleIcons(), 1);
#else
    QSKIP("favicon label is non-macOS only");
#endif
}

// ENG16 — reload on a foreign tab used to be dead on every
// user-facing path: the strip slot (context menu, reload-all)
// resolved through webView() which is nullptr on engine tabs, and
// the chrome actions unbound entirely instead of tracking the
// foreign page's standard actions.
void tst_EngineSwitch::engineTabReload()
{
    TabWidget widget;
    widget.newTab();
    QVERIFY(widget.reloadTabInEngine(0, QStringLiteral("fake")));
    EngineTab *foreign = widget.engineTab(0);
    QVERIFY(foreign);
    FakeEnginePage *page =
        qobject_cast<FakeEnginePage*>(foreign->page());
    QVERIFY(page);

    // Strip dispatch — reloadTab (context menu) and reloadAllTabs.
    widget.reloadTab(0);
    QCOMPARE(page->reloads, 1);
    widget.reloadAllTabs();
    QCOMPARE(page->reloads, 2);

    // Chrome-action dispatch — the WebActionMapper picks up the
    // standard action the page maps and disables the ones it does
    // not (nullptr reads as "disabled", never a stale enable).
    QAction *reloadRoot = new QAction(&widget);
    widget.addWebAction(reloadRoot, Engine::StandardAction::Reload);
    QAction *unmappedRoot = new QAction(&widget);
    widget.addWebAction(unmappedRoot, Engine::StandardAction::Forward);
    QAction *child = new QAction(page);
    connect(child, &QAction::triggered,
            page, &FakeEnginePage::reload);
    page->actions.insert(Engine::StandardAction::Reload, child);

    // Re-bind through a tab switch — activation is what syncs the
    // mappers with the current page.
    widget.newTab();
    widget.setCurrentIndex(0);
    QVERIFY(reloadRoot->isEnabled());
    QVERIFY(!unmappedRoot->isEnabled());
    reloadRoot->trigger();
    QCOMPARE(page->reloads, 3);

    // Switching back to the WebView tab re-binds the real page —
    // the foreign disabled state must not leak across engines.
    widget.setCurrentIndex(1);
    QCOMPARE(unmappedRoot->isEnabled(),
             widget.webView(1)->enginePage()
                 ->action(Engine::StandardAction::Forward)->isEnabled());
}

QTEST_MAIN(tst_EngineSwitch)
#include "tst_engineswitch.moc"
